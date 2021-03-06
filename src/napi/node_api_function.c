/* Copyright 2018-present Rokid Co., Ltd. and other contributors
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

#include "jerryscript-ext/handle-scope.h"
#include "jerryscript.h"
#include <stdlib.h>
#include "internal/node_api_internal.h"
#include "node_api.h"

static const jerry_object_native_info_t native_obj_type_info = { .free_cb =
                                                                     free };

static jerry_value_t iotjs_napi_function_handler(
    const jerry_value_t function_obj, const jerry_value_t this_val,
    const jerry_value_t args_p[], const jerry_length_t args_cnt) {
  iotjs_function_info_t *function_info;
  jerry_get_object_native_pointer(function_obj, (void *)&function_info, NULL);
  iotjs_callback_info_t *callback_info = IOTJS_ALLOC(iotjs_callback_info_t);
  callback_info->argc = args_cnt;
  callback_info->argv = (jerry_value_t *)args_p;
  callback_info->jval_this = this_val;
  callback_info->function_info = function_info;

  napi_env env = function_info->env;
  jerry_value_t jval_ret;

  jerryx_handle_scope scope;
  jerryx_open_handle_scope(&scope);

  napi_value nvalue_ret =
      function_info->cb(env, (napi_callback_info)callback_info);
  free(callback_info);

  iotjs_napi_env_t *iotjs_napi_env = (iotjs_napi_env_t *)env;
  if (iotjs_napi_is_exception_pending(iotjs_napi_env)) {
    if (iotjs_napi_env->pending_exception != NULL) {
      jval_ret = AS_JERRY_VALUE(iotjs_napi_env->pending_exception);
    } else {
      // TODO: fatal error support, trigger `uncaughtException`
      jval_ret = AS_JERRY_VALUE(iotjs_napi_env->pending_fatal_exception);
    }

    goto cleanup;
  }

  // TODO: check if nvalue_ret is escaped
  if (nvalue_ret == NULL) {
    jval_ret = jerry_create_undefined();
  } else {
    /** jval returned from N-API functions is scoped */
    jval_ret = AS_JERRY_VALUE(nvalue_ret);
  }
  jerryx_remove_handle(scope, jval_ret, &jval_ret);


cleanup:
  jerryx_close_handle_scope(scope);
  return jval_ret;
}

napi_status napi_create_function(napi_env env, const char *utf8name,
                                 size_t length, napi_callback cb, void *data,
                                 napi_value *result) {
  jerry_value_t jval_func =
      jerry_create_external_function(iotjs_napi_function_handler);
  jerryx_create_handle(jval_func);

  iotjs_function_info_t *function_info = IOTJS_ALLOC(iotjs_function_info_t);
  function_info->env = env;
  function_info->cb = cb;
  function_info->data = data;
  jerry_set_object_native_pointer(jval_func, function_info,
                                  &native_obj_type_info);

  *result = AS_NAPI_VALUE(jval_func);
  return napi_ok;
}
