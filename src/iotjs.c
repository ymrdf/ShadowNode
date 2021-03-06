/* Copyright 2015-present Samsung Electronics Co., Ltd. and other contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "iotjs_def.h"

#include "iotjs.h"
#include "iotjs_handlewrap.h"
#include "iotjs_js.h"
#include "iotjs_string_ext.h"

#include "jerryscript-debugger.h"
#ifndef __NUTTX__
#include "jerryscript-port-default.h"
#endif
#ifdef ENABLE_JERRYX
#include "jerryscript-ext/handle-scope.h"
#endif
#include "jerryscript-port.h"
#include "jerryscript.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/**
 * Initialize JerryScript.
 */
static bool iotjs_jerry_initialize(iotjs_environment_t* env) {
  // Set jerry run flags.
  jerry_init_flag_t jerry_flags = JERRY_INIT_EMPTY;

  if (iotjs_environment_config(env)->memstat) {
    jerry_flags |= JERRY_INIT_MEM_STATS;
#if !defined(__NUTTX__) && !defined(__TIZENRT__)
    jerry_port_default_set_log_level(JERRY_LOG_LEVEL_DEBUG);
#endif
  }

  if (iotjs_environment_config(env)->show_opcode) {
    jerry_flags |= JERRY_INIT_SHOW_OPCODES;
#if !defined(__NUTTX__) && !defined(__TIZENRT__)
    jerry_port_default_set_log_level(JERRY_LOG_LEVEL_DEBUG);
#endif
  }
  // Initialize jerry.
  jerry_init(jerry_flags);

  if (iotjs_environment_config(env)->debugger != NULL) {
    jerry_debugger_init(iotjs_environment_config(env)->debugger->port);
    jerry_debugger_continue();
  }

  // Set magic strings.
  iotjs_register_jerry_magic_string();

  // Register VM execution stop callback.
  IOTJS_VALIDATED_STRUCT_METHOD(iotjs_environment_t, env);
  jerry_set_vm_exec_stop_callback(vm_exec_stop_callback, &(_this->state), 2);

  // Do parse and run to generate initial javascript environment.
  jerry_value_t parsed_code = jerry_parse((jerry_char_t*)"", 0, false);
  if (jerry_value_has_error_flag(parsed_code)) {
    DLOG("jerry_parse() failed");
    jerry_release_value(parsed_code);
    return false;
  }

  jerry_value_t ret_val = jerry_run(parsed_code);
  if (jerry_value_has_error_flag(ret_val)) {
    DLOG("jerry_run() failed");
    jerry_release_value(parsed_code);
    jerry_release_value(ret_val);
    return false;
  }

  jerry_release_value(parsed_code);
  jerry_release_value(ret_val);
  return true;
}


static void iotjs_jerry_release(iotjs_environment_t* env) {
  jerry_cleanup();
}


static bool iotjs_run(iotjs_environment_t* env) {
  // Evaluating 'iotjs.js' returns a function.
  bool throws = false;
#ifndef ENABLE_SNAPSHOT
  jerry_value_t jmain = iotjs_jhelper_eval("iotjs.js", strlen("iotjs.js"),
                                           iotjs_s, iotjs_l, false, &throws);
#else
  jerry_value_t jmain =
      jerry_exec_snapshot_at((const void*)iotjs_js_modules_s,
                             iotjs_js_modules_l, module_iotjs_idx, false);
  if (jerry_value_has_error_flag(jmain)) {
    jerry_value_clear_error_flag(&jmain);
    throws = true;
  }
#endif

  if (throws) {
    iotjs_uncaught_exception(jmain);
  }

  jerry_release_value(jmain);

  return !throws;
}


static int iotjs_start(iotjs_environment_t* env) {
  // Bind environment to global object.
  const jerry_value_t global = jerry_get_global_object();
  jerry_set_object_native_pointer(global, env, NULL);

  // Initialize builtin process module.
  const jerry_value_t process = iotjs_module_get("process");
  iotjs_jval_set_property_jval(global, "process", process);

  // Release the global object
  jerry_release_value(global);

  // Set running state.
  iotjs_environment_go_state_running_main(env);

  // Load and call iotjs.js.
  iotjs_run(env);

  int exit_code = 0;
  if (!iotjs_environment_is_exiting(env)) {
    // Run event loop.
    iotjs_environment_go_state_running_loop(env);

    bool more;
    do {
      more = uv_run(iotjs_environment_loop(env), UV_RUN_ONCE);
      more |= iotjs_process_next_tick();

      jerry_value_t ret_val = jerry_run_all_enqueued_jobs();
      if (jerry_value_has_error_flag(ret_val)) {
        DLOG("jerry_run_all_enqueued_jobs() failed");
      }

      if (more == false) {
        more = uv_loop_alive(iotjs_environment_loop(env));
      }
    } while (more && !iotjs_environment_is_exiting(env));

    exit_code = iotjs_process_exitcode();
    if (!iotjs_environment_is_exiting(env)) {
      // Emit 'exit' event.
      iotjs_process_emit_exit(exit_code);
    }
    if (!iotjs_environment_is_exiting(env)) {
      iotjs_environment_go_state_exiting(env);
    }
  }

  exit_code = iotjs_process_exitcode();

  // Release builtin modules.
  iotjs_module_list_cleanup();

  return exit_code;
}

static void iotjs_uv_walk_to_close_callback(uv_handle_t* handle, void* arg) {
  if (!uv_is_closing(handle))
    uv_close(handle, NULL);
}

static jerry_value_t dummy_wait_for_client_source_cb() {
  return jerry_create_undefined();
}

int iotjs_entry(int argc, char** argv) {
  // Disable stdio buffering, it interacts poorly with printf()
  // calls elsewhere in the program
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);

  // Initialize debug print.
  init_debug_settings();

  // Initialize seed of random
  srand((unsigned)jerry_port_get_current_time());

  // Create environment.
  iotjs_environment_t* env = (iotjs_environment_t*)iotjs_environment_get();

  int ret_code = 0;

  // Parse command line arguments.
  if (!iotjs_environment_parse_command_line_arguments(env, (uint32_t)argc,
                                                      argv)) {
    ret_code = 1;
    goto terminate;
  }

  // Initialize JerryScript engine.
  if (!iotjs_jerry_initialize(env)) {
    DLOG("iotjs_jerry_initialize failed");
    ret_code = 1;
    goto terminate;
  }

  // Set event loop.
  if (!uv_default_loop()) {
    DLOG("iotjs uvloop init failed");
    return false;
  }
  iotjs_environment_set_loop(env, uv_default_loop());

  // set parser dump file
  jerry_open_parser_dump();

  // Start iot.js.
  ret_code = iotjs_start(env);

  // Cleanup handlewraps by handlewrap_queue
  iotjs_environment_cleanup_handlewrap();

  // Close uv loop.
  uv_walk(iotjs_environment_loop(env), iotjs_uv_walk_to_close_callback, NULL);
  uv_run(iotjs_environment_loop(env), UV_RUN_DEFAULT);

  int res = uv_loop_close(iotjs_environment_loop(env));
  IOTJS_ASSERT(res == 0);

  // Check whether context reset was sent or not.
  if (iotjs_environment_config(env)->debugger != NULL) {
    jerry_value_t res;
    jerry_debugger_wait_for_source_status_t receive_status;
    receive_status =
        jerry_debugger_wait_for_client_source(dummy_wait_for_client_source_cb,
                                              NULL, &res);

    if (receive_status == JERRY_DEBUGGER_CONTEXT_RESET_RECEIVED) {
      iotjs_environment_config(env)->debugger->context_reset = true;
    }
    jerry_release_value(res);
  }

#ifdef ENABLE_JERRYX
  // Release handles in root handle scope
  jerryx_close_handle_scope(jerryx_handle_scope_get_root());
#endif
  // Release JerryScript engine.
  iotjs_jerry_release(env);

terminate:;
  bool context_reset = false;
  if (iotjs_environment_config(env)->debugger != NULL) {
    context_reset = iotjs_environment_config(env)->debugger->context_reset;
  }
  // Release environment.
  iotjs_environment_release();

  // Release debug print setting.
  release_debug_settings();

  if (context_reset) {
    return iotjs_entry(argc, argv);
  }
  return ret_code;
}
