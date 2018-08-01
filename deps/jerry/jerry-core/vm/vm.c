/* Copyright JS Foundation and other contributors, http://js.foundation
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

#include "common.h"

#include "ecma-alloc.h"
#include "ecma-array-object.h"
#include "ecma-builtins.h"
#include "ecma-comparison.h"
#include "ecma-conversion.h"
#include "ecma-exceptions.h"
#include "ecma-function-object.h"
#include "ecma-gc.h"
#include "ecma-helpers.h"
#include "ecma-lcache.h"
#include "ecma-lex-env.h"
#include "ecma-objects.h"
#include "ecma-objects-general.h"
#include "ecma-regexp-object.h"
#include "ecma-try-catch-macro.h"
#include "jcontext.h"
#include "opcodes.h"
#include "vm.h"
#include "vm-stack.h"

/** \addtogroup vm Virtual machine
 * @{
 *
 * \addtogroup vm_executor Executor
 * @{
 */

/**
 * Get the value of object[property].
 *
 * @return ecma value
 */
static ecma_value_t
vm_op_get_value (ecma_value_t object, /**< base object */
                 ecma_value_t property) /**< property name */
{
  if (ecma_is_value_object (object))
  {
    ecma_object_t *object_p = ecma_get_object_from_value (object);
    ecma_string_t *property_name_p = NULL;

    if (ecma_is_value_integer_number (property))
    {
      ecma_integer_value_t int_value = ecma_get_integer_from_value (property);

      if (int_value >= 0 && int_value <= ECMA_DIRECT_STRING_MAX_IMM)
      {
        property_name_p = (ecma_string_t *) ECMA_CREATE_DIRECT_STRING (ECMA_DIRECT_STRING_UINT,
                                                                       (uintptr_t) int_value);
      }
    }
    else if (ecma_is_value_string (property))
    {
      property_name_p = ecma_get_string_from_value (property);
    }

    if (property_name_p != NULL)
    {
      ecma_property_t *property_p = ecma_lcache_lookup (object_p, property_name_p);

      if (property_p != NULL &&
          ECMA_PROPERTY_GET_TYPE (*property_p) == ECMA_PROPERTY_TYPE_NAMEDDATA)
      {
        return ecma_fast_copy_value (ECMA_PROPERTY_VALUE_PTR (property_p)->value);
      }

      /* There is no need to free the name. */
      return ecma_op_object_get (ecma_get_object_from_value (object), property_name_p);
    }
  }

  if (unlikely (ecma_is_value_undefined (object) || ecma_is_value_null (object)))
  {
#ifdef JERRY_ENABLE_ERROR_MESSAGES
    ecma_value_t error_value = ecma_raise_standard_error_with_format (ECMA_ERROR_TYPE,
                                                                      "Cannot read property '%' of %",
                                                                      property,
                                                                      object);
#else /* !JERRY_ENABLE_ERROR_MESSAGES */
    ecma_value_t error_value = ecma_raise_type_error (NULL);
#endif /* JERRY_ENABLE_ERROR_MESSAGES */
    return error_value;
  }

  ecma_value_t prop_to_string_result = ecma_op_to_string (property);

  if (ECMA_IS_VALUE_ERROR (prop_to_string_result))
  {
    return prop_to_string_result;
  }

  ecma_string_t *property_name_p = ecma_get_string_from_value (prop_to_string_result);

  ecma_value_t get_value_result = ecma_op_get_value_object_base (object, property_name_p);

  ecma_deref_ecma_string (property_name_p);
  return get_value_result;
} /* vm_op_get_value */

/**
 * Set the value of object[property].
 *
 * Note:
 *  this function frees its object and property arguments
 *
 * @return an ecma value which contains an error
 *         if the property setting is unsuccessful
 */
static ecma_value_t
vm_op_set_value (ecma_value_t object, /**< base object */
                 ecma_value_t property, /**< property name */
                 ecma_value_t value, /**< ecma value */
                 bool is_strict) /**< strict mode */
{
  if (unlikely (!ecma_is_value_object (object)))
  {
    ecma_value_t to_object = ecma_op_to_object (object);
    ecma_free_value (object);

    if (ECMA_IS_VALUE_ERROR (to_object))
    {
#ifdef JERRY_ENABLE_ERROR_MESSAGES
      ecma_free_value (to_object);
      ecma_free_value (JERRY_CONTEXT (error_value));

      ecma_value_t error_value = ecma_raise_standard_error_with_format (ECMA_ERROR_TYPE,
                                                                        "Cannot set property '%' of %",
                                                                        property,
                                                                        object);
      ecma_free_value (property);

      return error_value;
#else /* !JERRY_ENABLE_ERROR_MESSAGES */
      ecma_free_value (property);
      return to_object;
#endif /* JERRY_ENABLE_ERROR_MESSAGES */
    }

    object = to_object;
  }

  if (!ecma_is_value_string (property))
  {
    ecma_value_t to_string = ecma_op_to_string (property);
    ecma_fast_free_value (property);

    if (ECMA_IS_VALUE_ERROR (to_string))
    {
      ecma_free_value (object);
      return to_string;
    }

    property = to_string;
  }

  ecma_object_t *object_p = ecma_get_object_from_value (object);
  ecma_string_t *property_p = ecma_get_string_from_value (property);
  ecma_value_t completion_value = ECMA_VALUE_EMPTY;

  if (!ecma_is_lexical_environment (object_p))
  {
    completion_value = ecma_op_object_put (object_p,
                                           property_p,
                                           value,
                                           is_strict);
  }
  else
  {
    completion_value = ecma_op_set_mutable_binding (object_p,
                                                    property_p,
                                                    value,
                                                    is_strict);
  }

  ecma_free_value (object);
  ecma_free_value (property);

  return completion_value;
} /* vm_op_set_value */

#define CBC_OPCODE(arg1, arg2, arg3, arg4) arg4,

/**
 * Decode table for both opcodes and extended opcodes.
 */
static const uint16_t vm_decode_table[] JERRY_CONST_DATA =
{
  CBC_OPCODE_LIST
  CBC_EXT_OPCODE_LIST
};

#undef CBC_OPCODE

/**
 * Run global code
 *
 * Note:
 *      returned value must be freed with ecma_free_value, when it is no longer needed.
 *
 * @return ecma value
 */
ecma_value_t
vm_run_global (const ecma_compiled_code_t *bytecode_p) /**< pointer to bytecode to run */
{
  ecma_object_t *glob_obj_p = ecma_builtin_get (ECMA_BUILTIN_ID_GLOBAL);

  ecma_value_t ret_value = vm_run (bytecode_p,
                                   ecma_make_object_value (glob_obj_p),
                                   ecma_get_global_environment (),
                                   false,
                                   NULL,
                                   0);

  ecma_deref_object (glob_obj_p);
  return ret_value;
} /* vm_run_global */

/**
 * Run specified eval-mode bytecode
 *
 * @return ecma value
 */
ecma_value_t
vm_run_eval (ecma_compiled_code_t *bytecode_data_p, /**< byte-code data */
             bool is_direct) /**< is eval called in direct mode? */
{
  ecma_value_t this_binding;
  ecma_object_t *lex_env_p;

  /* ECMA-262 v5, 10.4.2 */
  if (is_direct)
  {
    this_binding = ecma_copy_value (JERRY_CONTEXT (vm_top_context_p)->this_binding);
    lex_env_p = JERRY_CONTEXT (vm_top_context_p)->lex_env_p;
  }
  else
  {
    this_binding = ecma_make_object_value (ecma_builtin_get (ECMA_BUILTIN_ID_GLOBAL));
    lex_env_p = ecma_get_global_environment ();
  }

  ecma_ref_object (lex_env_p);

  if ((bytecode_data_p->status_flags & CBC_CODE_FLAGS_STRICT_MODE) != 0)
  {
    ecma_object_t *strict_lex_env_p = ecma_create_decl_lex_env (lex_env_p);
    ecma_deref_object (lex_env_p);

    lex_env_p = strict_lex_env_p;
  }

  ecma_value_t completion_value = vm_run (bytecode_data_p,
                                          this_binding,
                                          lex_env_p,
                                          true,
                                          NULL,
                                          0);

  ecma_deref_object (lex_env_p);
  ecma_free_value (this_binding);

#ifdef JERRY_ENABLE_SNAPSHOT_EXEC
  if (!(bytecode_data_p->status_flags & CBC_CODE_FLAGS_STATIC_FUNCTION))
  {
    ecma_bytecode_deref (bytecode_data_p);
  }
#else /* !JERRY_ENABLE_SNAPSHOT_EXEC */
  ecma_bytecode_deref (bytecode_data_p);
#endif /* JERRY_ENABLE_SNAPSHOT_EXEC */

  return completion_value;
} /* vm_run_eval */

/**
 * Construct object
 *
 * @return object value
 */
static ecma_value_t
vm_construct_literal_object (vm_frame_ctx_t *frame_ctx_p, /**< frame context */
                             ecma_value_t lit_value) /**< literal */
{
#ifdef JERRY_ENABLE_SNAPSHOT_EXEC
  ecma_compiled_code_t *bytecode_p;

  if (likely (!(frame_ctx_p->bytecode_header_p->status_flags & CBC_CODE_FLAGS_STATIC_FUNCTION)))
  {
    bytecode_p = ECMA_GET_INTERNAL_VALUE_POINTER (ecma_compiled_code_t,
                                                  lit_value);
  }
  else
  {
    uint8_t *byte_p = ((uint8_t *) frame_ctx_p->bytecode_header_p) + lit_value;
    bytecode_p = (ecma_compiled_code_t *) byte_p;
  }
#else /* !JERRY_ENABLE_SNAPSHOT_EXEC */
  ecma_compiled_code_t *bytecode_p = ECMA_GET_INTERNAL_VALUE_POINTER (ecma_compiled_code_t,
                                                                      lit_value);
#endif /* JERRY_ENABLE_SNAPSHOT_EXEC */

  bool is_function = ((bytecode_p->status_flags & CBC_CODE_FLAGS_FUNCTION) != 0);

  if (is_function)
  {
    ecma_object_t *func_obj_p;

#ifndef CONFIG_DISABLE_ES2015_ARROW_FUNCTION
    if (!(bytecode_p->status_flags & CBC_CODE_FLAGS_ARROW_FUNCTION))
    {
      func_obj_p = ecma_op_create_function_object (frame_ctx_p->lex_env_p,
                                                   bytecode_p);
    }
    else
    {
      func_obj_p = ecma_op_create_arrow_function_object (frame_ctx_p->lex_env_p,
                                                         bytecode_p,
                                                         frame_ctx_p->this_binding);
    }
#else /* CONFIG_DISABLE_ES2015_ARROW_FUNCTION */
    func_obj_p = ecma_op_create_function_object (frame_ctx_p->lex_env_p,
                                                 bytecode_p);
#endif /* !CONFIG_DISABLE_ES2015_ARROW_FUNCTION */

    return ecma_make_object_value (func_obj_p);
  }
  else
  {
#ifndef CONFIG_DISABLE_REGEXP_BUILTIN
    ecma_value_t ret_value;
    ret_value = ecma_op_create_regexp_object_from_bytecode ((re_compiled_code_t *) bytecode_p);

    if (ECMA_IS_VALUE_ERROR (ret_value))
    {
      /* TODO: throw exception instead of define an 'undefined' value. */
      return ECMA_VALUE_UNDEFINED;
    }

    return ret_value;
#else /* CONFIG_DISABLE_REGEXP_BUILTIN */
    JERRY_UNREACHABLE (); /* Regular Expressions are not supported in the selected profile! */
#endif /* !CONFIG_DISABLE_REGEXP_BUILTIN */
  }
} /* vm_construct_literal_object */

/**
 * Get implicit this value
 *
 * @return true - if the implicit 'this' value is updated,
 *         false - otherwise
 */
static inline bool __attr_always_inline___
vm_get_implicit_this_value (ecma_value_t *this_value_p) /**< [in,out] this value */
{
  if (ecma_is_value_object (*this_value_p))
  {
    ecma_object_t *this_obj_p = ecma_get_object_from_value (*this_value_p);

    if (ecma_is_lexical_environment (this_obj_p))
    {
      ecma_value_t completion_value = ecma_op_implicit_this_value (this_obj_p);

      JERRY_ASSERT (!ECMA_IS_VALUE_ERROR (completion_value));

      *this_value_p = completion_value;
      return true;
    }
  }
  return false;
} /* vm_get_implicit_this_value */

/**
 * 'Function call' opcode handler.
 *
 * See also: ECMA-262 v5, 11.2.3
 */
static void
opfunc_call (vm_frame_ctx_t *frame_ctx_p) /**< frame context */
{
  uint8_t opcode = frame_ctx_p->byte_code_p[0];
  uint32_t arguments_list_len;

  if (opcode >= CBC_CALL0)
  {
    arguments_list_len = (unsigned int) ((opcode - CBC_CALL0) / 6);
  }
  else
  {
    arguments_list_len = frame_ctx_p->byte_code_p[1];
  }

  bool is_call_prop = ((opcode - CBC_CALL) % 6) >= 3;

  ecma_value_t this_value = ECMA_VALUE_UNDEFINED;
  ecma_value_t *stack_top_p = frame_ctx_p->stack_top_p - arguments_list_len;

  if (is_call_prop)
  {
    this_value = stack_top_p[-3];

    if (this_value == ECMA_VALUE_REGISTER_REF)
    {
      /* Lexical environment cannot be 'this' value. */
      stack_top_p[-2] = ECMA_VALUE_UNDEFINED;
      this_value = ECMA_VALUE_UNDEFINED;
    }
    else if (vm_get_implicit_this_value (&this_value))
    {
      ecma_free_value (stack_top_p[-3]);
      stack_top_p[-3] = this_value;
    }
  }

  ecma_value_t func_value = stack_top_p[-1];
  ecma_value_t completion_value;

  if (!ecma_op_is_callable (func_value))
  {
    completion_value = ecma_raise_type_error (ECMA_ERR_MSG ("Expected a function."));
  }
  else
  {
    ecma_object_t *func_obj_p = ecma_get_object_from_value (func_value);

    completion_value = ecma_op_function_call (func_obj_p,
                                              this_value,
                                              stack_top_p,
                                              arguments_list_len);
  }

  JERRY_CONTEXT (status_flags) &= (uint32_t) ~ECMA_STATUS_DIRECT_EVAL;

  /* Free registers. */
  for (uint32_t i = 0; i < arguments_list_len; i++)
  {
    ecma_fast_free_value (stack_top_p[i]);
  }

  if (is_call_prop)
  {
    ecma_free_value (*(--stack_top_p));
    ecma_free_value (*(--stack_top_p));
  }

  ecma_free_value (stack_top_p[-1]);
  stack_top_p[-1] = completion_value;

  frame_ctx_p->stack_top_p = stack_top_p;
} /* opfunc_call */

/**
 * 'Constructor call' opcode handler.
 *
 * See also: ECMA-262 v5, 11.2.2
 */
static void
opfunc_construct (vm_frame_ctx_t *frame_ctx_p) /**< frame context */
{
  uint8_t opcode = frame_ctx_p->byte_code_p[0];
  unsigned int arguments_list_len;

  if (opcode >= CBC_NEW0)
  {
    arguments_list_len = (unsigned int) (opcode - CBC_NEW0);
  }
  else
  {
    arguments_list_len = frame_ctx_p->byte_code_p[1];
  }

  ecma_value_t *stack_top_p = frame_ctx_p->stack_top_p - arguments_list_len;
  ecma_value_t constructor_value = stack_top_p[-1];
  ecma_value_t completion_value;

  if (!ecma_is_constructor (constructor_value))
  {
    completion_value = ecma_raise_type_error (ECMA_ERR_MSG ("Expected a constructor."));
  }
  else
  {
    ecma_object_t *constructor_obj_p = ecma_get_object_from_value (constructor_value);

    completion_value = ecma_op_function_construct (constructor_obj_p,
                                                   stack_top_p,
                                                   arguments_list_len);
  }

  /* Free registers. */
  for (uint32_t i = 0; i < arguments_list_len; i++)
  {
    ecma_fast_free_value (stack_top_p[i]);
  }

  ecma_free_value (stack_top_p[-1]);
  stack_top_p[-1] = completion_value;

  frame_ctx_p->stack_top_p = stack_top_p;
} /* opfunc_construct */

#define READ_LITERAL_INDEX(destination) \
  do \
  { \
    (destination) = *byte_code_p++; \
    if ((destination) >= encoding_limit) \
    { \
      (destination) = (uint16_t) ((((destination) << 8) | *byte_code_p++) - encoding_delta); \
    } \
  } \
  while (0)

/* TODO: For performance reasons, we define this as a macro.
 * When we are able to construct a function with similar speed,
 * we can remove this macro. */
#define READ_LITERAL(literal_index, target_value) \
  do \
  { \
    if ((literal_index) < ident_end) \
    { \
      if ((literal_index) < register_end) \
      { \
        /* Note: There should be no specialization for arguments. */ \
        (target_value) = ecma_fast_copy_value (frame_ctx_p->registers_p[literal_index]); \
      } \
      else \
      { \
        ecma_string_t *name_p = ecma_get_string_from_value (literal_start_p[literal_index]); \
        \
        result = ecma_op_resolve_reference_value (frame_ctx_p->lex_env_p, \
                                                  name_p); \
        \
        if (ECMA_IS_VALUE_ERROR (result)) \
        { \
          goto error; \
        } \
        (target_value) = result; \
      } \
    } \
    else if (literal_index < const_literal_end) \
    { \
      (target_value) = ecma_fast_copy_value (literal_start_p[literal_index]); \
    } \
    else \
    { \
      /* Object construction. */ \
      (target_value) = vm_construct_literal_object (frame_ctx_p, \
                                                    literal_start_p[literal_index]); \
    } \
  } \
  while (0)

/**
 * Run initializer byte codes.
 *
 * @return ecma value
 */
static void
vm_init_loop (vm_frame_ctx_t *frame_ctx_p) /**< frame context */
{
  const ecma_compiled_code_t *bytecode_header_p = frame_ctx_p->bytecode_header_p;
  uint8_t *byte_code_p = frame_ctx_p->byte_code_p;
  uint16_t encoding_limit;
  uint16_t encoding_delta;
  uint16_t register_end;
  ecma_value_t *literal_start_p = frame_ctx_p->literal_start_p;
  bool is_strict = ((frame_ctx_p->bytecode_header_p->status_flags & CBC_CODE_FLAGS_STRICT_MODE) != 0);
  ecma_value_t self_reference;

#ifdef JERRY_ENABLE_SNAPSHOT_EXEC
  self_reference = 0;
  if (!(bytecode_header_p->status_flags & CBC_CODE_FLAGS_STATIC_FUNCTION))
  {
    ECMA_SET_INTERNAL_VALUE_POINTER (self_reference, bytecode_header_p);
  }
#else /* !JERRY_ENABLE_SNAPSHOT_EXEC */
  ECMA_SET_INTERNAL_VALUE_POINTER (self_reference, bytecode_header_p);
#endif

  /* Prepare. */
  if (!(bytecode_header_p->status_flags & CBC_CODE_FLAGS_FULL_LITERAL_ENCODING))
  {
    encoding_limit = 255;
    encoding_delta = 0xfe01;
  }
  else
  {
    encoding_limit = 128;
    encoding_delta = 0x8000;
  }

  if (frame_ctx_p->bytecode_header_p->status_flags & CBC_CODE_FLAGS_UINT16_ARGUMENTS)
  {
    cbc_uint16_arguments_t *args_p = (cbc_uint16_arguments_t *) (frame_ctx_p->bytecode_header_p);
    register_end = args_p->register_end;
  }
  else
  {
    cbc_uint8_arguments_t *args_p = (cbc_uint8_arguments_t *) (frame_ctx_p->bytecode_header_p);
    register_end = args_p->register_end;
  }

  while (true)
  {
    switch (*byte_code_p)
    {
      case CBC_DEFINE_VARS:
      {
        uint32_t literal_index_end;
        uint32_t literal_index = register_end;

        byte_code_p++;
        READ_LITERAL_INDEX (literal_index_end);

        while (literal_index <= literal_index_end)
        {
          ecma_string_t *name_p = ecma_get_string_from_value (literal_start_p[literal_index]);
          vm_var_decl (frame_ctx_p, name_p);
          literal_index++;
        }
        break;
      }

      case CBC_INITIALIZE_VAR:
      case CBC_INITIALIZE_VARS:
      {
        uint8_t type = *byte_code_p;
        uint32_t literal_index;
        uint32_t literal_index_end;

        byte_code_p++;
        READ_LITERAL_INDEX (literal_index);

        if (type == CBC_INITIALIZE_VAR)
        {
          literal_index_end = literal_index;
        }
        else
        {
          READ_LITERAL_INDEX (literal_index_end);
        }

        while (literal_index <= literal_index_end)
        {
          uint32_t value_index;
          ecma_value_t lit_value;
          bool is_immutable_binding = false;

          READ_LITERAL_INDEX (value_index);

          if (value_index < register_end)
          {
            lit_value = frame_ctx_p->registers_p[value_index];
          }
          else
          {
            is_immutable_binding = (self_reference == literal_start_p[value_index]);
            lit_value = vm_construct_literal_object (frame_ctx_p,
                                                     literal_start_p[value_index]);
          }

          if (literal_index < register_end)
          {
            frame_ctx_p->registers_p[literal_index] = lit_value;
          }
          else
          {
            ecma_string_t *name_p = ecma_get_string_from_value (literal_start_p[literal_index]);

            if (likely (!is_immutable_binding))
            {
              vm_var_decl (frame_ctx_p, name_p);

              ecma_object_t *ref_base_lex_env_p = ecma_op_resolve_reference_base (frame_ctx_p->lex_env_p, name_p);

              ecma_value_t put_value_result = ecma_op_put_value_lex_env_base (ref_base_lex_env_p,
                                                                              name_p,
                                                                              is_strict,
                                                                              lit_value);

              JERRY_ASSERT (ecma_is_value_boolean (put_value_result)
                            || ecma_is_value_empty (put_value_result)
                            || ECMA_IS_VALUE_ERROR (put_value_result));

              if (ECMA_IS_VALUE_ERROR (put_value_result))
              {
                ecma_free_value (JERRY_CONTEXT (error_value));
              }
            }
            else
            {
              ecma_op_create_immutable_binding (frame_ctx_p->lex_env_p, name_p, lit_value);
            }

            if (value_index >= register_end)
            {
              ecma_free_value (lit_value);
            }
          }

          literal_index++;
        }
        break;
      }

#ifdef JERRY_ENABLE_SNAPSHOT_EXEC
      case CBC_SET_BYTECODE_PTR:
      {
        memcpy (&byte_code_p, byte_code_p + 1, sizeof (uint8_t *));
        frame_ctx_p->byte_code_start_p = byte_code_p;
        break;
      }
#endif /* JERRY_ENABLE_SNAPSHOT_EXEC */

      default:
      {
        frame_ctx_p->byte_code_p = byte_code_p;
        return;
      }
    }
  }
} /* vm_init_loop */

/**
 * Run generic byte code.
 *
 * @return ecma value
 */
static ecma_value_t __attr_noinline___
vm_loop (vm_frame_ctx_t *frame_ctx_p) /**< frame context */
{
  const ecma_compiled_code_t *bytecode_header_p = frame_ctx_p->bytecode_header_p;
  uint8_t *byte_code_p = frame_ctx_p->byte_code_p;
  ecma_value_t *literal_start_p = frame_ctx_p->literal_start_p;

  ecma_value_t *stack_top_p;
  uint16_t encoding_limit;
  uint16_t encoding_delta;
  uint16_t register_end;
  uint16_t ident_end;
  uint16_t const_literal_end;
  int32_t branch_offset = 0;
  uint8_t branch_offset_length = 0;
  ecma_value_t left_value;
  ecma_value_t right_value;
  ecma_value_t result = ECMA_VALUE_EMPTY;
  ecma_value_t block_result = ECMA_VALUE_UNDEFINED;
  bool is_strict = ((frame_ctx_p->bytecode_header_p->status_flags & CBC_CODE_FLAGS_STRICT_MODE) != 0);

  /* Prepare for byte code execution. */
  if (!(bytecode_header_p->status_flags & CBC_CODE_FLAGS_FULL_LITERAL_ENCODING))
  {
    encoding_limit = 255;
    encoding_delta = 0xfe01;
  }
  else
  {
    encoding_limit = 128;
    encoding_delta = 0x8000;
  }

  if (bytecode_header_p->status_flags & CBC_CODE_FLAGS_UINT16_ARGUMENTS)
  {
    cbc_uint16_arguments_t *args_p = (cbc_uint16_arguments_t *) (bytecode_header_p);
    register_end = args_p->register_end;
    ident_end = args_p->ident_end;
    const_literal_end = args_p->const_literal_end;
  }
  else
  {
    cbc_uint8_arguments_t *args_p = (cbc_uint8_arguments_t *) (bytecode_header_p);
    register_end = args_p->register_end;
    ident_end = args_p->ident_end;
    const_literal_end = args_p->const_literal_end;
  }

  stack_top_p = frame_ctx_p->stack_top_p;

  /* Outer loop for exception handling. */
  while (true)
  {
    /* Internal loop for byte code execution. */
    while (true)
    {
      uint8_t *byte_code_start_p = byte_code_p;
      uint8_t opcode = *byte_code_p++;
      uint32_t opcode_data = opcode;

      if (opcode == CBC_EXT_OPCODE)
      {
        opcode = *byte_code_p++;
        opcode_data = (uint32_t) ((CBC_END + 1) + opcode);
      }

      opcode_data = vm_decode_table[opcode_data];

      left_value = ECMA_VALUE_UNDEFINED;
      right_value = ECMA_VALUE_UNDEFINED;

      uint32_t operands = VM_OC_GET_ARGS_INDEX (opcode_data);

      if (operands >= VM_OC_GET_LITERAL)
      {
        uint16_t literal_index;
        READ_LITERAL_INDEX (literal_index);
        READ_LITERAL (literal_index, left_value);

        if (operands != VM_OC_GET_LITERAL)
        {
          switch (operands)
          {
            case VM_OC_GET_LITERAL_LITERAL:
            {
              uint16_t second_literal_index;
              READ_LITERAL_INDEX (second_literal_index);
              READ_LITERAL (second_literal_index, right_value);
              break;
            }
            case VM_OC_GET_STACK_LITERAL:
            {
              JERRY_ASSERT (stack_top_p > frame_ctx_p->registers_p + register_end);
              right_value = left_value;
              left_value = *(--stack_top_p);
              break;
            }
            default:
            {
              JERRY_ASSERT (operands == VM_OC_GET_THIS_LITERAL);
              right_value = left_value;
              left_value = ecma_copy_value (frame_ctx_p->this_binding);
              break;
            }
          }
        }
      }
      else if (operands >= VM_OC_GET_STACK)
      {
        JERRY_ASSERT (operands == VM_OC_GET_STACK
                      || operands == VM_OC_GET_STACK_STACK);

        JERRY_ASSERT (stack_top_p > frame_ctx_p->registers_p + register_end);
        left_value = *(--stack_top_p);

        if (operands == VM_OC_GET_STACK_STACK)
        {
          JERRY_ASSERT (stack_top_p > frame_ctx_p->registers_p + register_end);
          right_value = left_value;
          left_value = *(--stack_top_p);
        }
      }
      else if (operands == VM_OC_GET_BRANCH)
      {
        branch_offset_length = CBC_BRANCH_OFFSET_LENGTH (opcode);
        JERRY_ASSERT (branch_offset_length >= 1 && branch_offset_length <= 3);

        branch_offset = *(byte_code_p++);

        if (unlikely (branch_offset_length != 1))
        {
          branch_offset <<= 8;
          branch_offset |= *(byte_code_p++);

          if (unlikely (branch_offset_length == 3))
          {
            branch_offset <<= 8;
            branch_offset |= *(byte_code_p++);
          }
        }

        if (opcode_data & VM_OC_BACKWARD_BRANCH)
        {
#ifdef JERRY_VM_EXEC_STOP
          if (JERRY_CONTEXT (vm_exec_stop_cb) != NULL
              && --JERRY_CONTEXT (vm_exec_stop_counter) == 0)
          {
            result = JERRY_CONTEXT (vm_exec_stop_cb) (JERRY_CONTEXT (vm_exec_stop_user_p));

            if (ecma_is_value_undefined (result))
            {
              JERRY_CONTEXT (vm_exec_stop_counter) = JERRY_CONTEXT (vm_exec_stop_frequency);
            }
            else
            {
              JERRY_CONTEXT (vm_exec_stop_counter) = 1;

              if (!ecma_is_value_error_reference (result))
              {
                JERRY_CONTEXT (error_value) = result;
              }
              else
              {
                JERRY_CONTEXT (error_value) = ecma_clear_error_reference (result, false);
              }

              JERRY_CONTEXT (status_flags) &= (uint32_t) ~ECMA_STATUS_EXCEPTION;
              result = ECMA_VALUE_ERROR;
              goto error;
            }
          }
#endif /* JERRY_VM_EXEC_STOP */

          branch_offset = -branch_offset;
        }
      }

      switch (VM_OC_GROUP_GET_INDEX (opcode_data))
      {
        case VM_OC_NONE:
        {
          JERRY_ASSERT (opcode == CBC_EXT_DEBUGGER);
          continue;
        }
        case VM_OC_POP:
        {
          JERRY_ASSERT (stack_top_p > frame_ctx_p->registers_p + register_end);
          ecma_free_value (*(--stack_top_p));
          continue;
        }
        case VM_OC_POP_BLOCK:
        {
          ecma_fast_free_value (block_result);
          block_result = *(--stack_top_p);
          continue;
        }
        case VM_OC_PUSH:
        {
          *stack_top_p++ = left_value;
          continue;
        }
        case VM_OC_PUSH_TWO:
        {
          *stack_top_p++ = left_value;
          *stack_top_p++ = right_value;
          continue;
        }
        case VM_OC_PUSH_THREE:
        {
          uint16_t literal_index;

          *stack_top_p++ = left_value;
          left_value = ECMA_VALUE_UNDEFINED;

          READ_LITERAL_INDEX (literal_index);
          READ_LITERAL (literal_index, left_value);

          *stack_top_p++ = right_value;
          *stack_top_p++ = left_value;
          continue;
        }
        case VM_OC_PUSH_UNDEFINED:
        {
          *stack_top_p++ = ECMA_VALUE_UNDEFINED;
          continue;
        }
        case VM_OC_PUSH_TRUE:
        {
          *stack_top_p++ = ECMA_VALUE_TRUE;
          continue;
        }
        case VM_OC_PUSH_FALSE:
        {
          *stack_top_p++ = ECMA_VALUE_FALSE;
          continue;
        }
        case VM_OC_PUSH_NULL:
        {
          *stack_top_p++ = ECMA_VALUE_NULL;
          continue;
        }
        case VM_OC_PUSH_THIS:
        {
          *stack_top_p++ = ecma_copy_value (frame_ctx_p->this_binding);
          continue;
        }
        case VM_OC_PUSH_NUMBER_0:
        {
          *stack_top_p++ = ecma_make_integer_value (0);
          continue;
        }
        case VM_OC_PUSH_NUMBER_POS_BYTE:
        {
          ecma_integer_value_t number = *byte_code_p++;
          *stack_top_p++ = ecma_make_integer_value (number + 1);
          continue;
        }
        case VM_OC_PUSH_NUMBER_NEG_BYTE:
        {
          ecma_integer_value_t number = *byte_code_p++;
          *stack_top_p++ = ecma_make_integer_value (-(number + 1));
          continue;
        }
        case VM_OC_PUSH_OBJECT:
        {
          ecma_object_t *prototype_p = ecma_builtin_get (ECMA_BUILTIN_ID_OBJECT_PROTOTYPE);
          ecma_object_t *obj_p = ecma_create_object (prototype_p,
                                                     0,
                                                     ECMA_OBJECT_TYPE_GENERAL);

          ecma_deref_object (prototype_p);
          *stack_top_p++ = ecma_make_object_value (obj_p);
          continue;
        }
        case VM_OC_SET_PROPERTY:
        {
          ecma_object_t *object_p = ecma_get_object_from_value (stack_top_p[-1]);
          ecma_string_t *prop_name_p;
          ecma_property_t *property_p;

          if (ecma_is_value_string (right_value))
          {
            prop_name_p = ecma_get_string_from_value (right_value);
          }
          else
          {
            result = ecma_op_to_string (right_value);

            if (ECMA_IS_VALUE_ERROR (result))
            {
              goto error;
            }

            prop_name_p = ecma_get_string_from_value (result);
          }

          property_p = ecma_find_named_property (object_p, prop_name_p);

          if (property_p != NULL
              && ECMA_PROPERTY_GET_TYPE (*property_p) != ECMA_PROPERTY_TYPE_NAMEDDATA)
          {
            ecma_delete_property (object_p, ECMA_PROPERTY_VALUE_PTR (property_p));
            property_p = NULL;
          }

          ecma_property_value_t *prop_value_p;

          if (property_p == NULL)
          {
            prop_value_p = ecma_create_named_data_property (object_p,
                                                            prop_name_p,
                                                            ECMA_PROPERTY_CONFIGURABLE_ENUMERABLE_WRITABLE,
                                                            NULL);
          }
          else
          {
            prop_value_p = ECMA_PROPERTY_VALUE_PTR (property_p);
          }

          ecma_named_data_property_assign_value (object_p, prop_value_p, left_value);

          if (!ecma_is_value_string (right_value))
          {
            ecma_deref_ecma_string (prop_name_p);
          }

          goto free_both_values;
        }
        case VM_OC_SET_GETTER:
        case VM_OC_SET_SETTER:
        {
          opfunc_set_accessor (VM_OC_GROUP_GET_INDEX (opcode_data) == VM_OC_SET_GETTER,
                               stack_top_p[-1],
                               left_value,
                               right_value);

          goto free_both_values;
        }
        case VM_OC_PUSH_ARRAY:
        {
          result = ecma_op_create_array_object (NULL, 0, false);

          if (ECMA_IS_VALUE_ERROR (result))
          {
            goto error;
          }

          *stack_top_p++ = result;
          continue;
        }
        case VM_OC_PUSH_ELISON:
        {
          *stack_top_p++ = ECMA_VALUE_ARRAY_HOLE;
          continue;
        }
        case VM_OC_APPEND_ARRAY:
        {
          ecma_object_t *array_obj_p;
          uint32_t length_num;
          uint32_t values_length = *byte_code_p++;

          stack_top_p -= values_length;

          array_obj_p = ecma_get_object_from_value (stack_top_p[-1]);
          ecma_extended_object_t *ext_array_obj_p = (ecma_extended_object_t *) array_obj_p;

          length_num = ext_array_obj_p->u.array.length;

          for (uint32_t i = 0; i < values_length; i++)
          {
            if (!ecma_is_value_array_hole (stack_top_p[i]))
            {
              ecma_string_t *index_str_p = ecma_new_ecma_string_from_uint32 (length_num);

              ecma_property_value_t *prop_value_p;
              prop_value_p = ecma_create_named_data_property (array_obj_p,
                                                              index_str_p,
                                                              ECMA_PROPERTY_CONFIGURABLE_ENUMERABLE_WRITABLE,
                                                              NULL);

              JERRY_ASSERT (ecma_is_value_undefined (prop_value_p->value));
              prop_value_p->value = stack_top_p[i];

              /* The reference is moved so no need to free stack_top_p[i] except for objects. */
              if (ecma_is_value_object (stack_top_p[i]))
              {
                ecma_free_value (stack_top_p[i]);
              }

              ecma_deref_ecma_string (index_str_p);
            }

            length_num++;
          }

          ext_array_obj_p->u.array.length = length_num;
          continue;
        }
        case VM_OC_PUSH_UNDEFINED_BASE:
        {
          stack_top_p[0] = stack_top_p[-1];
          stack_top_p[-1] = ECMA_VALUE_UNDEFINED;
          stack_top_p++;
          continue;
        }
        case VM_OC_IDENT_REFERENCE:
        {
          uint16_t literal_index;

          READ_LITERAL_INDEX (literal_index);

          JERRY_ASSERT (literal_index < ident_end);

          if (literal_index < register_end)
          {
            *stack_top_p++ = ECMA_VALUE_REGISTER_REF;
            *stack_top_p++ = literal_index;
            *stack_top_p++ = ecma_fast_copy_value (frame_ctx_p->registers_p[literal_index]);
          }
          else
          {
            ecma_string_t *name_p = ecma_get_string_from_value (literal_start_p[literal_index]);

            ecma_object_t *ref_base_lex_env_p;
            ref_base_lex_env_p = ecma_op_resolve_reference_base (frame_ctx_p->lex_env_p,
                                                                 name_p);

            result = ecma_op_get_value_lex_env_base (ref_base_lex_env_p,
                                                     name_p,
                                                     is_strict);

            if (ECMA_IS_VALUE_ERROR (result))
            {
              goto error;
            }

            ecma_ref_object (ref_base_lex_env_p);
            ecma_ref_ecma_string (name_p);
            *stack_top_p++ = ecma_make_object_value (ref_base_lex_env_p);
            *stack_top_p++ = ecma_make_string_value (name_p);
            *stack_top_p++ = result;
          }
          continue;
        }
        case VM_OC_PROP_REFERENCE:
        {
          /* Forms with reference requires preserving the base and offset. */

          if (opcode == CBC_PUSH_PROP_REFERENCE)
          {
            left_value = stack_top_p[-2];
            right_value = stack_top_p[-1];
          }
          else if (opcode == CBC_PUSH_PROP_LITERAL_REFERENCE)
          {
            *stack_top_p++ = left_value;
            right_value = left_value;
            left_value = stack_top_p[-2];
          }
          else
          {
            JERRY_ASSERT (opcode == CBC_PUSH_PROP_LITERAL_LITERAL_REFERENCE
                          || opcode == CBC_PUSH_PROP_THIS_LITERAL_REFERENCE);
            *stack_top_p++ = left_value;
            *stack_top_p++ = right_value;
          }
          /* FALLTHRU */
        }
        case VM_OC_PROP_GET:
        case VM_OC_PROP_PRE_INCR:
        case VM_OC_PROP_PRE_DECR:
        case VM_OC_PROP_POST_INCR:
        case VM_OC_PROP_POST_DECR:
        {
          result = vm_op_get_value (left_value,
                                    right_value);

          if (ECMA_IS_VALUE_ERROR (result))
          {
            if (opcode >= CBC_PUSH_PROP_REFERENCE && opcode < CBC_PRE_INCR)
            {
              left_value = ECMA_VALUE_UNDEFINED;
              right_value = ECMA_VALUE_UNDEFINED;
            }
            goto error;
          }

          if (opcode < CBC_PRE_INCR)
          {
            if (opcode >= CBC_PUSH_PROP_REFERENCE)
            {
              left_value = ECMA_VALUE_UNDEFINED;
              right_value = ECMA_VALUE_UNDEFINED;
            }
            break;
          }

          stack_top_p += 2;
          left_value = result;
          right_value = ECMA_VALUE_UNDEFINED;
          /* FALLTHRU */
        }
        case VM_OC_PRE_INCR:
        case VM_OC_PRE_DECR:
        case VM_OC_POST_INCR:
        case VM_OC_POST_DECR:
        {
          uint32_t opcode_flags = VM_OC_GROUP_GET_INDEX (opcode_data) - VM_OC_PROP_PRE_INCR;

          byte_code_p = byte_code_start_p + 1;

          if (ecma_is_value_integer_number (left_value))
          {
            result = left_value;
            left_value = ECMA_VALUE_UNDEFINED;

            ecma_integer_value_t int_value = (ecma_integer_value_t) result;
            ecma_integer_value_t int_increase = 0;

            if (opcode_flags & VM_OC_DECREMENT_OPERATOR_FLAG)
            {
              if (int_value > ECMA_INTEGER_NUMBER_MIN_SHIFTED)
              {
                int_increase = -(1 << ECMA_DIRECT_SHIFT);
              }
            }
            else if (int_value < ECMA_INTEGER_NUMBER_MAX_SHIFTED)
            {
              int_increase = 1 << ECMA_DIRECT_SHIFT;
            }

            if (likely (int_increase != 0))
            {
              /* Postfix operators require the unmodifed number value. */
              if (opcode_flags & VM_OC_POST_INCR_DECR_OPERATOR_FLAG)
              {
                if (opcode_data & VM_OC_PUT_STACK)
                {
                  if (opcode_flags & VM_OC_IDENT_INCR_DECR_OPERATOR_FLAG)
                  {
                    JERRY_ASSERT (opcode == CBC_POST_INCR_IDENT_PUSH_RESULT
                                  || opcode == CBC_POST_DECR_IDENT_PUSH_RESULT);

                    *stack_top_p++ = result;
                  }
                  else
                  {
                    /* The parser ensures there is enough space for the
                     * extra value on the stack. See js-parser-expr.c. */

                    JERRY_ASSERT (opcode == CBC_POST_INCR_PUSH_RESULT
                                  || opcode == CBC_POST_DECR_PUSH_RESULT);

                    stack_top_p++;
                    stack_top_p[-1] = stack_top_p[-2];
                    stack_top_p[-2] = stack_top_p[-3];
                    stack_top_p[-3] = result;
                  }
                  opcode_data &= (uint32_t)~VM_OC_PUT_STACK;
                }
                else if (opcode_data & VM_OC_PUT_BLOCK)
                {
                  ecma_free_value (block_result);
                  block_result = result;
                  opcode_data &= (uint32_t) ~VM_OC_PUT_BLOCK;
                }
              }

              result = (ecma_value_t) (int_value + int_increase);
              break;
            }
          }
          else if (ecma_is_value_float_number (left_value))
          {
            result = left_value;
            left_value = ECMA_VALUE_UNDEFINED;
          }
          else
          {
            result = ecma_op_to_number (left_value);

            if (ECMA_IS_VALUE_ERROR (result))
            {
              goto error;
            }
          }

          ecma_number_t increase = ECMA_NUMBER_ONE;
          ecma_number_t result_number = ecma_get_number_from_value (result);

          if (opcode_flags & VM_OC_DECREMENT_OPERATOR_FLAG)
          {
            /* For decrement operators */
            increase = ECMA_NUMBER_MINUS_ONE;
          }

          /* Post operators require the unmodifed number value. */
          if (opcode_flags & VM_OC_POST_INCR_DECR_OPERATOR_FLAG)
          {
            if (opcode_data & VM_OC_PUT_STACK)
            {
              if (opcode_flags & VM_OC_IDENT_INCR_DECR_OPERATOR_FLAG)
              {
                JERRY_ASSERT (opcode == CBC_POST_INCR_IDENT_PUSH_RESULT
                              || opcode == CBC_POST_DECR_IDENT_PUSH_RESULT);

                *stack_top_p++ = ecma_copy_value (result);
              }
              else
              {
                /* The parser ensures there is enough space for the
                 * extra value on the stack. See js-parser-expr.c. */

                JERRY_ASSERT (opcode == CBC_POST_INCR_PUSH_RESULT
                              || opcode == CBC_POST_DECR_PUSH_RESULT);

                stack_top_p++;
                stack_top_p[-1] = stack_top_p[-2];
                stack_top_p[-2] = stack_top_p[-3];
                stack_top_p[-3] = ecma_copy_value (result);
              }
              opcode_data &= (uint32_t)~VM_OC_PUT_STACK;
            }
            else if (opcode_data & VM_OC_PUT_BLOCK)
            {
              ecma_free_value (block_result);
              block_result = ecma_copy_value (result);
              opcode_data &= (uint32_t) ~VM_OC_PUT_BLOCK;
            }
          }

          if (ecma_is_value_integer_number (result))
          {
            result = ecma_make_number_value (result_number + increase);
          }
          else
          {
            result = ecma_update_float_number (result, result_number + increase);
          }
          break;
        }
        case VM_OC_ASSIGN:
        {
          result = left_value;
          left_value = ECMA_VALUE_UNDEFINED;
          break;
        }
        case VM_OC_ASSIGN_PROP:
        {
          result = stack_top_p[-1];
          stack_top_p[-1] = left_value;
          left_value = ECMA_VALUE_UNDEFINED;
          break;
        }
        case VM_OC_ASSIGN_PROP_THIS:
        {
          result = stack_top_p[-1];
          stack_top_p[-1] = ecma_copy_value (frame_ctx_p->this_binding);
          *stack_top_p++ = left_value;
          left_value = ECMA_VALUE_UNDEFINED;
          break;
        }
        case VM_OC_RET:
        {
          JERRY_ASSERT (opcode == CBC_RETURN
                        || opcode == CBC_RETURN_WITH_BLOCK
                        || opcode == CBC_RETURN_WITH_LITERAL);

          if (opcode == CBC_RETURN_WITH_BLOCK)
          {
            left_value = block_result;
            block_result = ECMA_VALUE_UNDEFINED;
          }

          result = left_value;
          left_value = ECMA_VALUE_UNDEFINED;
          goto error;
        }
        case VM_OC_THROW:
        {
          JERRY_CONTEXT (error_value) = left_value;
          JERRY_CONTEXT (status_flags) |= ECMA_STATUS_EXCEPTION;
          JERRY_CONTEXT (stack_index) = 0;
          memset( JERRY_CONTEXT(stack_frames), 0, 10 );

          result = ECMA_VALUE_ERROR;
          left_value = ECMA_VALUE_UNDEFINED;
          goto error;
        }
        case VM_OC_THROW_REFERENCE_ERROR:
        {
          result = ecma_raise_reference_error (ECMA_ERR_MSG ("Undefined reference."));
          goto error;
        }
        case VM_OC_EVAL:
        {
          JERRY_CONTEXT (status_flags) |= ECMA_STATUS_DIRECT_EVAL;
          JERRY_ASSERT (*byte_code_p >= CBC_CALL && *byte_code_p <= CBC_CALL2_PROP_BLOCK);
          continue;
        }
        case VM_OC_CALL:
        {
          if (frame_ctx_p->call_operation == VM_NO_EXEC_OP)
          {
            frame_ctx_p->call_operation = VM_EXEC_CALL;
            frame_ctx_p->byte_code_p = byte_code_start_p;
            frame_ctx_p->stack_top_p = stack_top_p;
            frame_ctx_p->call_block_result = block_result;
            return ECMA_VALUE_UNDEFINED;
          }

          if (opcode < CBC_CALL0)
          {
            byte_code_p++;
          }

          frame_ctx_p->call_operation = VM_NO_EXEC_OP;

          result = *(--stack_top_p);
          block_result = frame_ctx_p->call_block_result;

          if (ECMA_IS_VALUE_ERROR (result))
          {
            goto error;
          }

          if (!(opcode_data & (VM_OC_PUT_STACK | VM_OC_PUT_BLOCK)))
          {
            ecma_fast_free_value (result);
          }
          else if (opcode_data & VM_OC_PUT_STACK)
          {
            *stack_top_p++ = result;
          }
          else
          {
            ecma_fast_free_value (block_result);
            block_result = result;
          }
          continue;
        }
        case VM_OC_NEW:
        {
          if (frame_ctx_p->call_operation == VM_NO_EXEC_OP)
          {
            frame_ctx_p->call_operation = VM_EXEC_CONSTRUCT;
            frame_ctx_p->byte_code_p = byte_code_start_p;
            frame_ctx_p->stack_top_p = stack_top_p;
            frame_ctx_p->call_block_result = block_result;
            return ECMA_VALUE_UNDEFINED;
          }

          if (opcode < CBC_NEW0)
          {
            byte_code_p++;
          }

          frame_ctx_p->call_operation = VM_NO_EXEC_OP;

          result = *(--stack_top_p);
          block_result = frame_ctx_p->call_block_result;

          if (ECMA_IS_VALUE_ERROR (result))
          {
            goto error;
          }

          *stack_top_p++ = result;
          continue;
        }
        case VM_OC_PROP_DELETE:
        {
          result = vm_op_delete_prop (left_value, right_value, is_strict);

          if (ECMA_IS_VALUE_ERROR (result))
          {
            goto error;
          }

          JERRY_ASSERT (ecma_is_value_boolean (result));

          *stack_top_p++ = result;
          goto free_both_values;
        }
        case VM_OC_DELETE:
        {
          uint16_t literal_index;

          READ_LITERAL_INDEX (literal_index);

          if (literal_index < register_end)
          {
            *stack_top_p++ = ECMA_VALUE_FALSE;
            continue;
          }

          result = vm_op_delete_var (literal_start_p[literal_index],
                                     frame_ctx_p->lex_env_p);

          if (ECMA_IS_VALUE_ERROR (result))
          {
            goto error;
          }

          JERRY_ASSERT (ecma_is_value_boolean (result));

          *stack_top_p++ = result;
          continue;
        }
        case VM_OC_JUMP:
        {
          byte_code_p = byte_code_start_p + branch_offset;
          continue;
        }
        case VM_OC_BRANCH_IF_STRICT_EQUAL:
        {
          ecma_value_t value = *(--stack_top_p);

          JERRY_ASSERT (stack_top_p > frame_ctx_p->registers_p + register_end);

          if (ecma_op_strict_equality_compare (value, stack_top_p[-1]))
          {
            byte_code_p = byte_code_start_p + branch_offset;
            ecma_free_value (*--stack_top_p);
          }
          ecma_free_value (value);
          continue;
        }
        case VM_OC_BRANCH_IF_TRUE:
        case VM_OC_BRANCH_IF_FALSE:
        case VM_OC_BRANCH_IF_LOGICAL_TRUE:
        case VM_OC_BRANCH_IF_LOGICAL_FALSE:
        {
          uint32_t opcode_flags = VM_OC_GROUP_GET_INDEX (opcode_data) - VM_OC_BRANCH_IF_TRUE;
          ecma_value_t value = *(--stack_top_p);

          bool boolean_value = ecma_op_to_boolean (value);

          if (opcode_flags & VM_OC_BRANCH_IF_FALSE_FLAG)
          {
            boolean_value = !boolean_value;
          }

          if (boolean_value)
          {
            byte_code_p = byte_code_start_p + branch_offset;
            if (opcode_flags & VM_OC_LOGICAL_BRANCH_FLAG)
            {
              /* "Push" the value back to the stack. */
              ++stack_top_p;
              continue;
            }
          }

          ecma_fast_free_value (value);
          continue;
        }
        case VM_OC_PLUS:
        case VM_OC_MINUS:
        {
          result = opfunc_unary_operation (left_value, VM_OC_GROUP_GET_INDEX (opcode_data) == VM_OC_PLUS);

          if (ECMA_IS_VALUE_ERROR (result))
          {
            goto error;
          }

          *stack_top_p++ = result;
          goto free_left_value;
        }
        case VM_OC_NOT:
        {
          result = opfunc_logical_not (left_value);

          if (ECMA_IS_VALUE_ERROR (result))
          {
            goto error;
          }

          *stack_top_p++ = result;
          goto free_left_value;
        }
        case VM_OC_BIT_NOT:
        {
          JERRY_STATIC_ASSERT (ECMA_DIRECT_TYPE_MASK == ((1 << ECMA_DIRECT_SHIFT) - 1),
                               direct_type_mask_must_fill_all_bits_before_the_value_starts);

          if (ecma_is_value_integer_number (left_value))
          {
            *stack_top_p++ = (~left_value) & (ecma_value_t) (~ECMA_DIRECT_TYPE_MASK);
            goto free_left_value;
          }

          result = do_number_bitwise_logic (NUMBER_BITWISE_NOT,
                                            left_value,
                                            left_value);

          if (ECMA_IS_VALUE_ERROR (result))
          {
            goto error;
          }

          *stack_top_p++ = result;
          goto free_left_value;
        }
        case VM_OC_VOID:
        {
          *stack_top_p++ = ECMA_VALUE_UNDEFINED;
          goto free_left_value;
        }
        case VM_OC_TYPEOF_IDENT:
        {
          uint16_t literal_index;

          READ_LITERAL_INDEX (literal_index);

          JERRY_ASSERT (literal_index < ident_end);

          if (literal_index < register_end)
          {
            left_value = ecma_copy_value (frame_ctx_p->registers_p[literal_index]);
          }
          else
          {
            ecma_string_t *name_p = ecma_get_string_from_value (literal_start_p[literal_index]);

            ecma_object_t *ref_base_lex_env_p = ecma_op_resolve_reference_base (frame_ctx_p->lex_env_p,
                                                                                name_p);

            if (ref_base_lex_env_p == NULL)
            {
              result = ECMA_VALUE_UNDEFINED;
            }
            else
            {
              result = ecma_op_get_value_lex_env_base (ref_base_lex_env_p,
                                                       name_p,
                                                       is_strict);

            }

            if (ECMA_IS_VALUE_ERROR (result))
            {
              goto error;
            }

            left_value = result;
          }
          /* FALLTHRU */
        }
        case VM_OC_TYPEOF:
        {
          result = opfunc_typeof (left_value);

          if (ECMA_IS_VALUE_ERROR (result))
          {
            goto error;
          }

          *stack_top_p++ = result;
          goto free_left_value;
        }
        case VM_OC_ADD:
        {
          if (ecma_are_values_integer_numbers (left_value, right_value))
          {
            ecma_integer_value_t left_integer = ecma_get_integer_from_value (left_value);
            ecma_integer_value_t right_integer = ecma_get_integer_from_value (right_value);
            result = ecma_make_int32_value ((int32_t) (left_integer + right_integer));
            break;
          }

          if (ecma_is_value_float_number (left_value)
              && ecma_is_value_number (right_value))
          {
            ecma_number_t new_value = (ecma_get_float_from_value (left_value) +
                                       ecma_get_number_from_value (right_value));

            result = ecma_update_float_number (left_value, new_value);
            left_value = ECMA_VALUE_UNDEFINED;
            break;
          }

          if (ecma_is_value_float_number (right_value)
              && ecma_is_value_integer_number (left_value))
          {
            ecma_number_t new_value = ((ecma_number_t) ecma_get_integer_from_value (left_value) +
                                       ecma_get_float_from_value (right_value));

            result = ecma_update_float_number (right_value, new_value);
            right_value = ECMA_VALUE_UNDEFINED;
            break;
          }

          result = opfunc_addition (left_value, right_value);

          if (ECMA_IS_VALUE_ERROR (result))
          {
            goto error;
          }
          break;
        }
        case VM_OC_SUB:
        {
          JERRY_STATIC_ASSERT (ECMA_INTEGER_NUMBER_MAX * 2 <= INT32_MAX
                               && ECMA_INTEGER_NUMBER_MIN * 2 >= INT32_MIN,
                               doubled_ecma_numbers_must_fit_into_int32_range);

          JERRY_ASSERT (!ECMA_IS_VALUE_ERROR (left_value)
                        && !ECMA_IS_VALUE_ERROR (right_value));

          if (ecma_are_values_integer_numbers (left_value, right_value))
          {
            ecma_integer_value_t left_integer = ecma_get_integer_from_value (left_value);
            ecma_integer_value_t right_integer = ecma_get_integer_from_value (right_value);
            result = ecma_make_int32_value ((int32_t) (left_integer - right_integer));
            break;
          }

          if (ecma_is_value_float_number (left_value)
              && ecma_is_value_number (right_value))
          {
            ecma_number_t new_value = (ecma_get_float_from_value (left_value) -
                                       ecma_get_number_from_value (right_value));

            result = ecma_update_float_number (left_value, new_value);
            left_value = ECMA_VALUE_UNDEFINED;
            break;
          }

          if (ecma_is_value_float_number (right_value)
              && ecma_is_value_integer_number (left_value))
          {
            ecma_number_t new_value = ((ecma_number_t) ecma_get_integer_from_value (left_value) -
                                       ecma_get_float_from_value (right_value));

            result = ecma_update_float_number (right_value, new_value);
            right_value = ECMA_VALUE_UNDEFINED;
            break;
          }

          result = do_number_arithmetic (NUMBER_ARITHMETIC_SUBSTRACTION,
                                         left_value,
                                         right_value);

          if (ECMA_IS_VALUE_ERROR (result))
          {
            goto error;
          }
          break;
        }
        case VM_OC_MUL:
        {
          JERRY_ASSERT (!ECMA_IS_VALUE_ERROR (left_value)
                        && !ECMA_IS_VALUE_ERROR (right_value));

          JERRY_STATIC_ASSERT (ECMA_INTEGER_MULTIPLY_MAX * ECMA_INTEGER_MULTIPLY_MAX <= ECMA_INTEGER_NUMBER_MAX
                               && -(ECMA_INTEGER_MULTIPLY_MAX * ECMA_INTEGER_MULTIPLY_MAX) >= ECMA_INTEGER_NUMBER_MIN,
                               square_of_integer_multiply_max_must_fit_into_integer_value_range);

          if (ecma_are_values_integer_numbers (left_value, right_value))
          {
            ecma_integer_value_t left_integer = ecma_get_integer_from_value (left_value);
            ecma_integer_value_t right_integer = ecma_get_integer_from_value (right_value);

            if (-ECMA_INTEGER_MULTIPLY_MAX <= left_integer
                && left_integer <= ECMA_INTEGER_MULTIPLY_MAX
                && -ECMA_INTEGER_MULTIPLY_MAX <= right_integer
                && right_integer <= ECMA_INTEGER_MULTIPLY_MAX
                && left_value != 0
                && right_value != 0)
            {
              result = ecma_integer_multiply (left_integer, right_integer);
              break;
            }

            ecma_number_t multiply = (ecma_number_t) left_integer * (ecma_number_t) right_integer;
            result = ecma_make_number_value (multiply);
            break;
          }

          if (ecma_is_value_float_number (left_value)
              && ecma_is_value_number (right_value))
          {
            ecma_number_t new_value = (ecma_get_float_from_value (left_value) *
                                       ecma_get_number_from_value (right_value));

            result = ecma_update_float_number (left_value, new_value);
            left_value = ECMA_VALUE_UNDEFINED;
            break;
          }

          if (ecma_is_value_float_number (right_value)
              && ecma_is_value_integer_number (left_value))
          {
            ecma_number_t new_value = ((ecma_number_t) ecma_get_integer_from_value (left_value) *
                                       ecma_get_float_from_value (right_value));

            result = ecma_update_float_number (right_value, new_value);
            right_value = ECMA_VALUE_UNDEFINED;
            break;
          }

          result = do_number_arithmetic (NUMBER_ARITHMETIC_MULTIPLICATION,
                                         left_value,
                                         right_value);

          if (ECMA_IS_VALUE_ERROR (result))
          {
            goto error;
          }
          break;
        }
        case VM_OC_DIV:
        {
          JERRY_ASSERT (!ECMA_IS_VALUE_ERROR (left_value)
                        && !ECMA_IS_VALUE_ERROR (right_value));

          result = do_number_arithmetic (NUMBER_ARITHMETIC_DIVISION,
                                         left_value,
                                         right_value);

          if (ECMA_IS_VALUE_ERROR (result))
          {
            goto error;
          }
          break;
        }
        case VM_OC_MOD:
        {
          JERRY_ASSERT (!ECMA_IS_VALUE_ERROR (left_value)
                        && !ECMA_IS_VALUE_ERROR (right_value));

          if (ecma_are_values_integer_numbers (left_value, right_value))
          {
            ecma_integer_value_t left_integer = ecma_get_integer_from_value (left_value);
            ecma_integer_value_t right_integer = ecma_get_integer_from_value (right_value);

            if (right_integer != 0)
            {
              ecma_integer_value_t mod_result = left_integer % right_integer;

              if (mod_result != 0 || left_integer >= 0)
              {
                result = ecma_make_integer_value (mod_result);
                break;
              }
            }
          }

          result = do_number_arithmetic (NUMBER_ARITHMETIC_REMAINDER,
                                         left_value,
                                         right_value);

          if (ECMA_IS_VALUE_ERROR (result))
          {
            goto error;
          }
          break;
        }
        case VM_OC_EQUAL:
        {
          result = opfunc_equality (left_value, right_value);

          if (ECMA_IS_VALUE_ERROR (result))
          {
            goto error;
          }

          *stack_top_p++ = result;
          goto free_both_values;
        }
        case VM_OC_NOT_EQUAL:
        {
          result = opfunc_equality (left_value, right_value);

          if (ECMA_IS_VALUE_ERROR (result))
          {
            goto error;
          }

          *stack_top_p++ = ecma_invert_boolean_value (result);
          goto free_both_values;
        }
        case VM_OC_STRICT_EQUAL:
        {
          bool is_equal = ecma_op_strict_equality_compare (left_value, right_value);

          result = ecma_make_boolean_value (is_equal);

          *stack_top_p++ = result;
          goto free_both_values;
        }
        case VM_OC_STRICT_NOT_EQUAL:
        {
          bool is_equal = ecma_op_strict_equality_compare (left_value, right_value);

          result = ecma_make_boolean_value (!is_equal);

          *stack_top_p++ = result;
          goto free_both_values;
        }
        case VM_OC_BIT_OR:
        {
          JERRY_STATIC_ASSERT (ECMA_DIRECT_TYPE_MASK == ((1 << ECMA_DIRECT_SHIFT) - 1),
                               direct_type_mask_must_fill_all_bits_before_the_value_starts);

          if (ecma_are_values_integer_numbers (left_value, right_value))
          {
            result = left_value | right_value;
            break;
          }

          result = do_number_bitwise_logic (NUMBER_BITWISE_LOGIC_OR,
                                            left_value,
                                            right_value);

          if (ECMA_IS_VALUE_ERROR (result))
          {
            goto error;
          }
          break;
        }
        case VM_OC_BIT_XOR:
        {
          JERRY_STATIC_ASSERT (ECMA_DIRECT_TYPE_MASK == ((1 << ECMA_DIRECT_SHIFT) - 1),
                               direct_type_mask_must_fill_all_bits_before_the_value_starts);

          if (ecma_are_values_integer_numbers (left_value, right_value))
          {
            result = (left_value ^ right_value) & (ecma_value_t) (~ECMA_DIRECT_TYPE_MASK);
            break;
          }

          result = do_number_bitwise_logic (NUMBER_BITWISE_LOGIC_XOR,
                                            left_value,
                                            right_value);

          if (ECMA_IS_VALUE_ERROR (result))
          {
            goto error;
          }
          break;
        }
        case VM_OC_BIT_AND:
        {
          JERRY_STATIC_ASSERT (ECMA_DIRECT_TYPE_MASK == ((1 << ECMA_DIRECT_SHIFT) - 1),
                               direct_type_mask_must_fill_all_bits_before_the_value_starts);

          if (ecma_are_values_integer_numbers (left_value, right_value))
          {
            result = left_value & right_value;
            break;
          }

          result = do_number_bitwise_logic (NUMBER_BITWISE_LOGIC_AND,
                                            left_value,
                                            right_value);

          if (ECMA_IS_VALUE_ERROR (result))
          {
            goto error;
          }
          break;
        }
        case VM_OC_LEFT_SHIFT:
        {
          JERRY_STATIC_ASSERT (ECMA_DIRECT_TYPE_MASK == ((1 << ECMA_DIRECT_SHIFT) - 1),
                               direct_type_mask_must_fill_all_bits_before_the_value_starts);

          if (ecma_are_values_integer_numbers (left_value, right_value))
          {
            ecma_integer_value_t left_integer = ecma_get_integer_from_value (left_value);
            ecma_integer_value_t right_integer = ecma_get_integer_from_value (right_value);
            result = ecma_make_int32_value ((int32_t) (left_integer << (right_integer & 0x1f)));
            break;
          }

          result = do_number_bitwise_logic (NUMBER_BITWISE_SHIFT_LEFT,
                                            left_value,
                                            right_value);

          if (ECMA_IS_VALUE_ERROR (result))
          {
            goto error;
          }
          break;
        }
        case VM_OC_RIGHT_SHIFT:
        {
          JERRY_STATIC_ASSERT (ECMA_DIRECT_TYPE_MASK == ((1 << ECMA_DIRECT_SHIFT) - 1),
                               direct_type_mask_must_fill_all_bits_before_the_value_starts);

          if (ecma_are_values_integer_numbers (left_value, right_value))
          {
            ecma_integer_value_t left_integer = ecma_get_integer_from_value (left_value);
            ecma_integer_value_t right_integer = ecma_get_integer_from_value (right_value);
            result = ecma_make_integer_value (left_integer >> (right_integer & 0x1f));
            break;
          }

          result = do_number_bitwise_logic (NUMBER_BITWISE_SHIFT_RIGHT,
                                            left_value,
                                            right_value);

          if (ECMA_IS_VALUE_ERROR (result))
          {
            goto error;
          }
          break;
        }
        case VM_OC_UNS_RIGHT_SHIFT:
        {
          JERRY_STATIC_ASSERT (ECMA_DIRECT_TYPE_MASK == ((1 << ECMA_DIRECT_SHIFT) - 1),
                               direct_type_mask_must_fill_all_bits_before_the_value_starts);

          if (ecma_are_values_integer_numbers (left_value, right_value))
          {
            uint32_t left_uint32 = (uint32_t) ecma_get_integer_from_value (left_value);
            ecma_integer_value_t right_integer = ecma_get_integer_from_value (right_value);
            result = ecma_make_uint32_value (left_uint32 >> (right_integer & 0x1f));
            break;
          }

          result = do_number_bitwise_logic (NUMBER_BITWISE_SHIFT_URIGHT,
                                            left_value,
                                            right_value);

          if (ECMA_IS_VALUE_ERROR (result))
          {
            goto error;
          }
          break;
        }
        case VM_OC_LESS:
        {
          if (ecma_are_values_integer_numbers (left_value, right_value))
          {
            bool is_less = (ecma_integer_value_t) left_value < (ecma_integer_value_t) right_value;

            /* This is a lookahead to the next opcode to improve performance.
             * If it is CBC_BRANCH_IF_TRUE_BACKWARD, execute it. */
            if (*byte_code_p <= CBC_BRANCH_IF_TRUE_BACKWARD_3 && *byte_code_p >= CBC_BRANCH_IF_TRUE_BACKWARD)
            {
              byte_code_start_p = byte_code_p++;
              branch_offset_length = CBC_BRANCH_OFFSET_LENGTH (*byte_code_start_p);
              JERRY_ASSERT (branch_offset_length >= 1 && branch_offset_length <= 3);

              if (is_less)
              {
                branch_offset = *(byte_code_p++);

                if (unlikely (branch_offset_length != 1))
                {
                  branch_offset <<= 8;
                  branch_offset |= *(byte_code_p++);
                  if (unlikely (branch_offset_length == 3))
                  {
                    branch_offset <<= 8;
                    branch_offset |= *(byte_code_p++);
                  }
                }

                /* Note: The opcode is a backward branch. */
                byte_code_p = byte_code_start_p - branch_offset;
              }
              else
              {
                byte_code_p += branch_offset_length;
              }

              continue;
            }

            *stack_top_p++ = ecma_make_boolean_value (is_less);
            continue;
          }

          if (ecma_is_value_number (left_value) && ecma_is_value_number (right_value))
          {
            ecma_number_t left_number = ecma_get_number_from_value (left_value);
            ecma_number_t right_number = ecma_get_number_from_value (right_value);

            *stack_top_p++ = ecma_make_boolean_value (left_number < right_number);
            goto free_both_values;
          }

          result = opfunc_relation (left_value, right_value, true, false);

          if (ECMA_IS_VALUE_ERROR (result))
          {
            goto error;
          }

          *stack_top_p++ = result;
          goto free_both_values;
        }
        case VM_OC_GREATER:
        {
          if (ecma_are_values_integer_numbers (left_value, right_value))
          {
            ecma_integer_value_t left_integer = (ecma_integer_value_t) left_value;
            ecma_integer_value_t right_integer = (ecma_integer_value_t) right_value;

            *stack_top_p++ = ecma_make_boolean_value (left_integer > right_integer);
            continue;
          }

          if (ecma_is_value_number (left_value) && ecma_is_value_number (right_value))
          {
            ecma_number_t left_number = ecma_get_number_from_value (left_value);
            ecma_number_t right_number = ecma_get_number_from_value (right_value);

            *stack_top_p++ = ecma_make_boolean_value (left_number > right_number);
            goto free_both_values;
          }

          result = opfunc_relation (left_value, right_value, false, false);

          if (ECMA_IS_VALUE_ERROR (result))
          {
            goto error;
          }

          *stack_top_p++ = result;
          goto free_both_values;
        }
        case VM_OC_LESS_EQUAL:
        {
          if (ecma_are_values_integer_numbers (left_value, right_value))
          {
            ecma_integer_value_t left_integer = (ecma_integer_value_t) left_value;
            ecma_integer_value_t right_integer = (ecma_integer_value_t) right_value;

            *stack_top_p++ = ecma_make_boolean_value (left_integer <= right_integer);
            continue;
          }

          if (ecma_is_value_number (left_value) && ecma_is_value_number (right_value))
          {
            ecma_number_t left_number = ecma_get_number_from_value (left_value);
            ecma_number_t right_number = ecma_get_number_from_value (right_value);

            *stack_top_p++ = ecma_make_boolean_value (left_number <= right_number);
            goto free_both_values;
          }

          result = opfunc_relation (left_value, right_value, false, true);

          if (ECMA_IS_VALUE_ERROR (result))
          {
            goto error;
          }

          *stack_top_p++ = result;
          goto free_both_values;
        }
        case VM_OC_GREATER_EQUAL:
        {
          if (ecma_are_values_integer_numbers (left_value, right_value))
          {
            ecma_integer_value_t left_integer = (ecma_integer_value_t) left_value;
            ecma_integer_value_t right_integer = (ecma_integer_value_t) right_value;

            *stack_top_p++ = ecma_make_boolean_value (left_integer >= right_integer);
            continue;
          }

          if (ecma_is_value_number (left_value) && ecma_is_value_number (right_value))
          {
            ecma_number_t left_number = ecma_get_number_from_value (left_value);
            ecma_number_t right_number = ecma_get_number_from_value (right_value);

            *stack_top_p++ = ecma_make_boolean_value (left_number >= right_number);
            goto free_both_values;
          }

          result = opfunc_relation (left_value, right_value, true, true);

          if (ECMA_IS_VALUE_ERROR (result))
          {
            goto error;
          }

          *stack_top_p++ = result;
          goto free_both_values;
        }
        case VM_OC_IN:
        {
          result = opfunc_in (left_value, right_value);

          if (ECMA_IS_VALUE_ERROR (result))
          {
            goto error;
          }

          *stack_top_p++ = result;
          goto free_both_values;
        }
        case VM_OC_INSTANCEOF:
        {
          result = opfunc_instanceof (left_value, right_value);

          if (ECMA_IS_VALUE_ERROR (result))
          {
            goto error;
          }

          *stack_top_p++ = result;
          goto free_both_values;
        }
        case VM_OC_WITH:
        {
          ecma_value_t value = *(--stack_top_p);
          ecma_object_t *object_p;
          ecma_object_t *with_env_p;

          branch_offset += (int32_t) (byte_code_start_p - frame_ctx_p->byte_code_start_p);

          JERRY_ASSERT (frame_ctx_p->registers_p + register_end + frame_ctx_p->context_depth == stack_top_p);

          result = ecma_op_to_object (value);
          ecma_free_value (value);

          if (ECMA_IS_VALUE_ERROR (result))
          {
            goto error;
          }

          object_p = ecma_get_object_from_value (result);

          with_env_p = ecma_create_object_lex_env (frame_ctx_p->lex_env_p,
                                                   object_p,
                                                   true);

          ecma_deref_object (object_p);

          VM_PLUS_EQUAL_U16 (frame_ctx_p->context_depth, PARSER_WITH_CONTEXT_STACK_ALLOCATION);
          stack_top_p += PARSER_WITH_CONTEXT_STACK_ALLOCATION;

          stack_top_p[-1] = VM_CREATE_CONTEXT (VM_CONTEXT_WITH, branch_offset);
          stack_top_p[-2] = ecma_make_object_value (frame_ctx_p->lex_env_p);

          frame_ctx_p->lex_env_p = with_env_p;
          continue;
        }
        case VM_OC_FOR_IN_CREATE_CONTEXT:
        {
          ecma_value_t value = *(--stack_top_p);

          JERRY_ASSERT (frame_ctx_p->registers_p + register_end + frame_ctx_p->context_depth == stack_top_p);

          ecma_value_t expr_obj_value = ECMA_VALUE_UNDEFINED;
          ecma_collection_chunk_t *prop_names_p = opfunc_for_in (value, &expr_obj_value);
          ecma_free_value (value);

          if (prop_names_p == NULL)
          {
            byte_code_p = byte_code_start_p + branch_offset;
            continue;
          }

          branch_offset += (int32_t) (byte_code_start_p - frame_ctx_p->byte_code_start_p);

          VM_PLUS_EQUAL_U16 (frame_ctx_p->context_depth, PARSER_FOR_IN_CONTEXT_STACK_ALLOCATION);
          stack_top_p += PARSER_FOR_IN_CONTEXT_STACK_ALLOCATION;
          stack_top_p[-1] = (ecma_value_t) VM_CREATE_CONTEXT (VM_CONTEXT_FOR_IN, branch_offset);
          ECMA_SET_INTERNAL_VALUE_ANY_POINTER (stack_top_p[-2], prop_names_p);
          stack_top_p[-3] = 0;
          stack_top_p[-4] = expr_obj_value;

          continue;
        }
        case VM_OC_FOR_IN_GET_NEXT:
        {
          ecma_value_t *context_top_p = frame_ctx_p->registers_p + register_end + frame_ctx_p->context_depth;

          ecma_collection_chunk_t *chunk_p;
          chunk_p = ECMA_GET_INTERNAL_VALUE_ANY_POINTER (ecma_collection_chunk_t, context_top_p[-2]);

          JERRY_ASSERT (VM_GET_CONTEXT_TYPE (context_top_p[-1]) == VM_CONTEXT_FOR_IN);

          uint32_t index = context_top_p[-3];

          JERRY_ASSERT (!ecma_is_value_collection_chunk (chunk_p->items[index]));

          *stack_top_p++ = chunk_p->items[index];
          index++;

          if (likely (!ecma_is_value_collection_chunk (chunk_p->items[index])))
          {
            context_top_p[-3] = index;
            continue;
          }

          context_top_p[-3] = 0;

          ecma_collection_chunk_t *next_chunk_p = ecma_get_collection_chunk_from_value (chunk_p->items[index]);
          ECMA_SET_INTERNAL_VALUE_ANY_POINTER (context_top_p[-2], next_chunk_p);

          jmem_heap_free_block (chunk_p, sizeof (ecma_collection_chunk_t));
          continue;
        }
        case VM_OC_FOR_IN_HAS_NEXT:
        {
          JERRY_ASSERT (frame_ctx_p->registers_p + register_end + frame_ctx_p->context_depth == stack_top_p);

          ecma_collection_chunk_t *chunk_p;
          chunk_p = ECMA_GET_INTERNAL_VALUE_ANY_POINTER (ecma_collection_chunk_t, stack_top_p[-2]);

          uint32_t index = stack_top_p[-3];
          ecma_object_t *object_p = ecma_get_object_from_value (stack_top_p[-4]);

          while (true)
          {
            if (chunk_p == NULL)
            {
              ecma_deref_object (object_p);

              VM_MINUS_EQUAL_U16 (frame_ctx_p->context_depth, PARSER_FOR_IN_CONTEXT_STACK_ALLOCATION);
              stack_top_p -= PARSER_FOR_IN_CONTEXT_STACK_ALLOCATION;
              break;
            }

            ecma_string_t *prop_name_p = ecma_get_string_from_value (chunk_p->items[index]);

            if (likely (ecma_op_object_has_property (object_p, prop_name_p)))
            {
              byte_code_p = byte_code_start_p + branch_offset;
              break;
            }

            index++;
            ecma_value_t value = chunk_p->items[index];

            if (likely (!ecma_is_value_collection_chunk (value)))
            {
              stack_top_p[-3] = index;
            }
            else
            {
              index = 0;
              stack_top_p[-3] = 0;

              ecma_collection_chunk_t *next_chunk_p = ecma_get_collection_chunk_from_value (value);
              ECMA_SET_INTERNAL_VALUE_ANY_POINTER (stack_top_p[-2], next_chunk_p);

              jmem_heap_free_block (chunk_p, sizeof (ecma_collection_chunk_t));
              chunk_p = next_chunk_p;
            }

            ecma_deref_ecma_string (prop_name_p);
          }
          continue;
        }
        case VM_OC_TRY:
        {
          /* Try opcode simply creates the try context. */
          branch_offset += (int32_t) (byte_code_start_p - frame_ctx_p->byte_code_start_p);

          JERRY_ASSERT (frame_ctx_p->registers_p + register_end + frame_ctx_p->context_depth == stack_top_p);

          VM_PLUS_EQUAL_U16 (frame_ctx_p->context_depth, PARSER_TRY_CONTEXT_STACK_ALLOCATION);
          stack_top_p += PARSER_TRY_CONTEXT_STACK_ALLOCATION;

          stack_top_p[-1] = (ecma_value_t) VM_CREATE_CONTEXT (VM_CONTEXT_TRY, branch_offset);
          continue;
        }
        case VM_OC_CATCH:
        {
          /* Catches are ignored and turned to jumps. */
          JERRY_ASSERT (frame_ctx_p->registers_p + register_end + frame_ctx_p->context_depth == stack_top_p);
          JERRY_ASSERT (VM_GET_CONTEXT_TYPE (stack_top_p[-1]) == VM_CONTEXT_TRY);

          byte_code_p = byte_code_start_p + branch_offset;
          continue;
        }
        case VM_OC_FINALLY:
        {
          branch_offset += (int32_t) (byte_code_start_p - frame_ctx_p->byte_code_start_p);

          JERRY_ASSERT (frame_ctx_p->registers_p + register_end + frame_ctx_p->context_depth == stack_top_p);

          JERRY_ASSERT (VM_GET_CONTEXT_TYPE (stack_top_p[-1]) == VM_CONTEXT_TRY
                        || VM_GET_CONTEXT_TYPE (stack_top_p[-1]) == VM_CONTEXT_CATCH);

          if (VM_GET_CONTEXT_TYPE (stack_top_p[-1]) == VM_CONTEXT_CATCH)
          {
            ecma_deref_object (frame_ctx_p->lex_env_p);
            frame_ctx_p->lex_env_p = ecma_get_object_from_value (stack_top_p[-2]);
          }

          stack_top_p[-1] = (ecma_value_t) VM_CREATE_CONTEXT (VM_CONTEXT_FINALLY_JUMP, branch_offset);
          stack_top_p[-2] = (ecma_value_t) branch_offset;
          continue;
        }
        case VM_OC_CONTEXT_END:
        {
          JERRY_ASSERT (frame_ctx_p->registers_p + register_end + frame_ctx_p->context_depth == stack_top_p);

          switch (VM_GET_CONTEXT_TYPE (stack_top_p[-1]))
          {
            case VM_CONTEXT_FINALLY_JUMP:
            {
              uint32_t jump_target = stack_top_p[-2];

              VM_MINUS_EQUAL_U16 (frame_ctx_p->context_depth,
                                  PARSER_TRY_CONTEXT_STACK_ALLOCATION);
              stack_top_p -= PARSER_TRY_CONTEXT_STACK_ALLOCATION;

              if (vm_stack_find_finally (frame_ctx_p,
                                         &stack_top_p,
                                         VM_CONTEXT_FINALLY_JUMP,
                                         jump_target))
              {
                JERRY_ASSERT (VM_GET_CONTEXT_TYPE (stack_top_p[-1]) == VM_CONTEXT_FINALLY_JUMP);
                byte_code_p = frame_ctx_p->byte_code_p;
                stack_top_p[-2] = jump_target;
              }
              else
              {
                byte_code_p = frame_ctx_p->byte_code_start_p + jump_target;
              }
              break;
            }
            case VM_CONTEXT_FINALLY_THROW:
            {
              JERRY_CONTEXT (error_value) = stack_top_p[-2];
              JERRY_CONTEXT (status_flags) |= ECMA_STATUS_EXCEPTION;

              VM_MINUS_EQUAL_U16 (frame_ctx_p->context_depth,
                                  PARSER_TRY_CONTEXT_STACK_ALLOCATION);
              stack_top_p -= PARSER_TRY_CONTEXT_STACK_ALLOCATION;
              result = ECMA_VALUE_ERROR;
              goto error;
            }
            case VM_CONTEXT_FINALLY_RETURN:
            {
              result = stack_top_p[-2];

              VM_MINUS_EQUAL_U16 (frame_ctx_p->context_depth,
                                  PARSER_TRY_CONTEXT_STACK_ALLOCATION);
              stack_top_p -= PARSER_TRY_CONTEXT_STACK_ALLOCATION;
              goto error;
            }
            default:
            {
              stack_top_p = vm_stack_context_abort (frame_ctx_p, stack_top_p);
            }
          }

          JERRY_ASSERT (frame_ctx_p->registers_p + register_end + frame_ctx_p->context_depth == stack_top_p);
          continue;
        }
        case VM_OC_JUMP_AND_EXIT_CONTEXT:
        {
          JERRY_ASSERT (frame_ctx_p->registers_p + register_end + frame_ctx_p->context_depth == stack_top_p);

          branch_offset += (int32_t) (byte_code_start_p - frame_ctx_p->byte_code_start_p);

          if (vm_stack_find_finally (frame_ctx_p,
                                     &stack_top_p,
                                     VM_CONTEXT_FINALLY_JUMP,
                                     (uint32_t) branch_offset))
          {
            JERRY_ASSERT (VM_GET_CONTEXT_TYPE (stack_top_p[-1]) == VM_CONTEXT_FINALLY_JUMP);
            byte_code_p = frame_ctx_p->byte_code_p;
            stack_top_p[-2] = (uint32_t) branch_offset;
          }
          else
          {
            byte_code_p = frame_ctx_p->byte_code_start_p + branch_offset;
          }

          JERRY_ASSERT (frame_ctx_p->registers_p + register_end + frame_ctx_p->context_depth == stack_top_p);
          continue;
        }
        case VM_OC_BREAKPOINT_ENABLED:
        {
#ifdef JERRY_DEBUGGER
          if (JERRY_CONTEXT (debugger_flags) & JERRY_DEBUGGER_VM_IGNORE)
          {
            continue;
          }

          JERRY_ASSERT (JERRY_CONTEXT (debugger_flags) & JERRY_DEBUGGER_CONNECTED);

          JERRY_ASSERT (!(frame_ctx_p->bytecode_header_p->status_flags & CBC_CODE_FLAGS_DEBUGGER_IGNORE));

          frame_ctx_p->byte_code_p = byte_code_start_p;

          jerry_debugger_breakpoint_hit (JERRY_DEBUGGER_BREAKPOINT_HIT);
#endif /* JERRY_DEBUGGER */
          continue;
        }
        case VM_OC_BREAKPOINT_DISABLED:
        {
#ifdef JERRY_DEBUGGER
          if (JERRY_CONTEXT (debugger_flags) & JERRY_DEBUGGER_VM_IGNORE)
          {
            continue;
          }

          JERRY_ASSERT (JERRY_CONTEXT (debugger_flags) & JERRY_DEBUGGER_CONNECTED);

          JERRY_ASSERT (!(frame_ctx_p->bytecode_header_p->status_flags & CBC_CODE_FLAGS_DEBUGGER_IGNORE));

          frame_ctx_p->byte_code_p = byte_code_start_p;

          if ((JERRY_CONTEXT (debugger_flags) & JERRY_DEBUGGER_VM_STOP)
              && (JERRY_CONTEXT (debugger_stop_context) == NULL
                  || JERRY_CONTEXT (debugger_stop_context) == JERRY_CONTEXT (vm_top_context_p)))
          {
            jerry_debugger_breakpoint_hit (JERRY_DEBUGGER_BREAKPOINT_HIT);
            continue;
          }

          if (JERRY_CONTEXT (debugger_message_delay) > 0)
          {
            JERRY_CONTEXT (debugger_message_delay)--;
            continue;
          }

          JERRY_CONTEXT (debugger_message_delay) = JERRY_DEBUGGER_MESSAGE_FREQUENCY;

          if (jerry_debugger_receive (NULL))
          {
            continue;
          }

          if ((JERRY_CONTEXT (debugger_flags) & JERRY_DEBUGGER_VM_STOP)
              && (JERRY_CONTEXT (debugger_stop_context) == NULL
                  || JERRY_CONTEXT (debugger_stop_context) == JERRY_CONTEXT (vm_top_context_p)))
          {
            jerry_debugger_breakpoint_hit (JERRY_DEBUGGER_BREAKPOINT_HIT);
          }
#endif /* JERRY_DEBUGGER */
          continue;
        }
        default:
        {
          JERRY_UNREACHABLE ();
          continue;
        }
      }

      JERRY_ASSERT (VM_OC_HAS_PUT_RESULT (opcode_data));

      if (opcode_data & VM_OC_PUT_IDENT)
      {
        uint16_t literal_index;

        READ_LITERAL_INDEX (literal_index);

        if (literal_index < register_end)
        {
          ecma_fast_free_value (frame_ctx_p->registers_p[literal_index]);

          frame_ctx_p->registers_p[literal_index] = result;

          if (opcode_data & (VM_OC_PUT_STACK | VM_OC_PUT_BLOCK))
          {
            result = ecma_fast_copy_value (result);
          }
        }
        else
        {
          ecma_string_t *var_name_str_p = ecma_get_string_from_value (literal_start_p[literal_index]);

          ecma_object_t *ref_base_lex_env_p = ecma_op_resolve_reference_base (frame_ctx_p->lex_env_p,
                                                                              var_name_str_p);

          ecma_value_t put_value_result = ecma_op_put_value_lex_env_base (ref_base_lex_env_p,
                                                                          var_name_str_p,
                                                                          is_strict,
                                                                          result);

          if (ECMA_IS_VALUE_ERROR (put_value_result))
          {
            ecma_free_value (result);
            result = put_value_result;
            goto error;
          }

          if (!(opcode_data & (VM_OC_PUT_STACK | VM_OC_PUT_BLOCK)))
          {
            ecma_fast_free_value (result);
          }
        }
      }
      else if (opcode_data & VM_OC_PUT_REFERENCE)
      {
        ecma_value_t property = *(--stack_top_p);
        ecma_value_t object = *(--stack_top_p);
        if (object == ECMA_VALUE_REGISTER_REF)
        {
          ecma_fast_free_value (frame_ctx_p->registers_p[property]);

          frame_ctx_p->registers_p[property] = result;

          if (!(opcode_data & (VM_OC_PUT_STACK | VM_OC_PUT_BLOCK)))
          {
            goto free_both_values;
          }
          result = ecma_fast_copy_value (result);
        }
        else
        {
          ecma_value_t set_value_result = vm_op_set_value (object,
                                                           property,
                                                           result,
                                                           is_strict);

          if (ECMA_IS_VALUE_ERROR (set_value_result))
          {
            ecma_free_value (result);
            result = set_value_result;
            goto error;
          }

          if (!(opcode_data & (VM_OC_PUT_STACK | VM_OC_PUT_BLOCK)))
          {
            ecma_fast_free_value (result);
            goto free_both_values;
          }
        }
      }

      if (opcode_data & VM_OC_PUT_STACK)
      {
        *stack_top_p++ = result;
      }
      else if (opcode_data & VM_OC_PUT_BLOCK)
      {
        ecma_fast_free_value (block_result);
        block_result = result;
      }

free_both_values:
      ecma_fast_free_value (right_value);
free_left_value:
      ecma_fast_free_value (left_value);
    }
error:

    ecma_fast_free_value (left_value);
    ecma_fast_free_value (right_value);

    if (ECMA_IS_VALUE_ERROR (result))
    {
      ecma_value_t *vm_stack_p = stack_top_p;

      for (vm_stack_p = frame_ctx_p->registers_p + register_end + frame_ctx_p->context_depth;
           vm_stack_p < stack_top_p;
           vm_stack_p++)
      {
        if (*vm_stack_p == ECMA_VALUE_REGISTER_REF)
        {
          JERRY_ASSERT (vm_stack_p < stack_top_p);
          vm_stack_p++;
        }
        else
        {
          ecma_free_value (*vm_stack_p);
        }
      }

      stack_top_p = frame_ctx_p->registers_p + register_end + frame_ctx_p->context_depth;
      {
        jmem_cpointer_t byte_code_cp;
        JMEM_CP_SET_NON_NULL_POINTER (byte_code_cp, frame_ctx_p->bytecode_header_p);
        uint32_t idx = JERRY_CONTEXT (stack_index);
        if (idx < 10) {
          JERRY_CONTEXT (stack_frames) [idx] = (uint32_t) byte_code_cp;
          JERRY_CONTEXT (stack_index) += 1;
        }
      }

#ifdef JERRY_DEBUGGER
      if ((JERRY_CONTEXT (debugger_flags) & JERRY_DEBUGGER_CONNECTED)
          && !(frame_ctx_p->bytecode_header_p->status_flags & CBC_CODE_FLAGS_DEBUGGER_IGNORE)
          && !(JERRY_CONTEXT (debugger_flags) & (JERRY_DEBUGGER_VM_IGNORE_EXCEPTION | JERRY_DEBUGGER_VM_IGNORE)))
      {
        if (jerry_debugger_send_exception_string ())
        {
          jerry_debugger_breakpoint_hit (JERRY_DEBUGGER_EXCEPTION_HIT);
        }
      }
#endif /* JERRY_DEBUGGER */
    }

    JERRY_ASSERT (frame_ctx_p->registers_p + register_end + frame_ctx_p->context_depth == stack_top_p);

    if (frame_ctx_p->context_depth == 0)
    {
      /* In most cases there is no context. */

      ecma_fast_free_value (block_result);
      return result;
    }

    if (!ECMA_IS_VALUE_ERROR (result))
    {
      if (vm_stack_find_finally (frame_ctx_p,
                                 &stack_top_p,
                                 VM_CONTEXT_FINALLY_RETURN,
                                 0))
      {
        JERRY_ASSERT (VM_GET_CONTEXT_TYPE (stack_top_p[-1]) == VM_CONTEXT_FINALLY_RETURN);
        JERRY_ASSERT (frame_ctx_p->registers_p + register_end + frame_ctx_p->context_depth == stack_top_p);

        byte_code_p = frame_ctx_p->byte_code_p;
        stack_top_p[-2] = result;
        continue;
      }
    }
    else if (JERRY_CONTEXT (status_flags) & ECMA_STATUS_EXCEPTION)
    {
      if (vm_stack_find_finally (frame_ctx_p,
                                 &stack_top_p,
                                 VM_CONTEXT_FINALLY_THROW,
                                 0))
      {
        JERRY_ASSERT (frame_ctx_p->registers_p + register_end + frame_ctx_p->context_depth == stack_top_p);

        byte_code_p = frame_ctx_p->byte_code_p;

        if (VM_GET_CONTEXT_TYPE (stack_top_p[-1]) == VM_CONTEXT_CATCH)
        {
          *stack_top_p++ = JERRY_CONTEXT (error_value);

          JERRY_ASSERT (byte_code_p[0] == CBC_ASSIGN_SET_IDENT);

          uint32_t literal_index = byte_code_p[1];

          if (literal_index >= encoding_limit)
          {
            literal_index = ((literal_index << 8) | byte_code_p[2]) - encoding_delta;
          }

          ecma_object_t *catch_env_p = ecma_create_decl_lex_env (frame_ctx_p->lex_env_p);

          ecma_string_t *catch_name_p = ecma_get_string_from_value (literal_start_p[literal_index]);
          ecma_op_create_mutable_binding (catch_env_p, catch_name_p, false);

          stack_top_p[-2 - 1] = ecma_make_object_value (frame_ctx_p->lex_env_p);
          frame_ctx_p->lex_env_p = catch_env_p;
        }
        else
        {
          JERRY_ASSERT (VM_GET_CONTEXT_TYPE (stack_top_p[-1]) == VM_CONTEXT_FINALLY_THROW);
          stack_top_p[-2] = JERRY_CONTEXT (error_value);
        }

        continue;
      }
    }
    else
    {
      do
      {
        JERRY_ASSERT (frame_ctx_p->registers_p + register_end + frame_ctx_p->context_depth == stack_top_p);

        stack_top_p = vm_stack_context_abort (frame_ctx_p, stack_top_p);
      }
      while (frame_ctx_p->context_depth > 0);
    }

    ecma_free_value (block_result);
    return result;
  }
} /* vm_loop */

#undef READ_LITERAL
#undef READ_LITERAL_INDEX

/**
 * Execute code block.
 *
 * @return ecma value
 */
static ecma_value_t __attr_noinline___
vm_execute (vm_frame_ctx_t *frame_ctx_p, /**< frame context */
            const ecma_value_t *arg_p, /**< arguments list */
            ecma_length_t arg_list_len) /**< length of arguments list */
{
  const ecma_compiled_code_t *bytecode_header_p = frame_ctx_p->bytecode_header_p;
  ecma_value_t completion_value;
  uint16_t argument_end;
  uint16_t register_end;

  if (bytecode_header_p->status_flags & CBC_CODE_FLAGS_UINT16_ARGUMENTS)
  {
    cbc_uint16_arguments_t *args_p = (cbc_uint16_arguments_t *) bytecode_header_p;

    argument_end = args_p->argument_end;
    register_end = args_p->register_end;
  }
  else
  {
    cbc_uint8_arguments_t *args_p = (cbc_uint8_arguments_t *) bytecode_header_p;

    argument_end = args_p->argument_end;
    register_end = args_p->register_end;
  }

  frame_ctx_p->stack_top_p = frame_ctx_p->registers_p + register_end;

  if (arg_list_len > argument_end)
  {
    arg_list_len = argument_end;
  }

  for (uint32_t i = 0; i < arg_list_len; i++)
  {
    frame_ctx_p->registers_p[i] = ecma_fast_copy_value (arg_p[i]);
  }

  /* The arg_list_len contains the end of the copied arguments.
   * Fill everything else with undefined. */
  if (register_end > arg_list_len)
  {
    ecma_value_t *stack_p = frame_ctx_p->registers_p + arg_list_len;

    for (uint32_t i = arg_list_len; i < register_end; i++)
    {
      *stack_p++ = ECMA_VALUE_UNDEFINED;
    }
  }

  JERRY_CONTEXT (status_flags) &= (uint32_t) ~ECMA_STATUS_DIRECT_EVAL;

  JERRY_CONTEXT (vm_top_context_p) = frame_ctx_p;

  vm_init_loop (frame_ctx_p);

#ifdef JERRY_CPU_PROFILER
  double begin_time = jerry_port_get_current_time();
#endif /* JERRY_CPU_PROFILER */

  while (true)
  {
    completion_value = vm_loop (frame_ctx_p);

    if (frame_ctx_p->call_operation == VM_NO_EXEC_OP)
    {
      break;
    }

    if (frame_ctx_p->call_operation == VM_EXEC_CALL)
    {
      opfunc_call (frame_ctx_p);
    }
    else
    {
      JERRY_ASSERT (frame_ctx_p->call_operation == VM_EXEC_CONSTRUCT);
      opfunc_construct (frame_ctx_p);
    }
  }

  /* Free arguments and registers */
  for (uint32_t i = 0; i < register_end; i++)
  {
    ecma_fast_free_value (frame_ctx_p->registers_p[i]);
  }

#ifdef JERRY_DEBUGGER
  if (JERRY_CONTEXT (debugger_stop_context) == JERRY_CONTEXT (vm_top_context_p))
  {
    /* The engine will stop when the next breakpoint is reached. */
    JERRY_ASSERT (JERRY_CONTEXT (debugger_flags) & JERRY_DEBUGGER_VM_STOP);
    JERRY_CONTEXT (debugger_stop_context) = NULL;
  }
#endif /* JERRY_DEBUGGER */

#ifdef JERRY_CPU_PROFILER
  double end_time = jerry_port_get_current_time ();
  FILE *fp = JERRY_CONTEXT (cpu_profiling_fp);
  if (fp && (JERRY_CONTEXT (cpu_profiler_type) == JS_CPU_PROFILER))
  {
    fprintf (fp, "%g,", end_time - begin_time);
    jcontext_print_backtrace (fp);
    fprintf (fp, "\n");
    if (JERRY_CONTEXT (cpu_profiling_duration) > 0 &&
        end_time > JERRY_CONTEXT (cpu_profiling_start_time) + JERRY_CONTEXT (cpu_profiling_duration))
    {
      jerry_stop_cpu_profiling ();
    }
  }
#endif /* JERRY_CPU_PROFILER */

  JERRY_CONTEXT (vm_top_context_p) = frame_ctx_p->prev_context_p;
  return completion_value;
} /* vm_execute */

/**
 * Run the code.
 *
 * @return ecma value
 */
ecma_value_t
vm_run (const ecma_compiled_code_t *bytecode_header_p, /**< byte-code data header */
        ecma_value_t this_binding_value, /**< value of 'ThisBinding' */
        ecma_object_t *lex_env_p, /**< lexical environment to use */
        bool is_eval_code, /**< is the code is eval code (ECMA-262 v5, 10.1) */
        const ecma_value_t *arg_list_p, /**< arguments list */
        ecma_length_t arg_list_len) /**< length of arguments list */
{
  ecma_value_t *literal_p;
  vm_frame_ctx_t frame_ctx;
  uint32_t call_stack_size;

  if (bytecode_header_p->status_flags & CBC_CODE_FLAGS_UINT16_ARGUMENTS)
  {
    cbc_uint16_arguments_t *args_p = (cbc_uint16_arguments_t *) bytecode_header_p;
    call_stack_size = (uint32_t) (args_p->register_end + args_p->stack_limit);

    literal_p = (ecma_value_t *) ((uint8_t *) bytecode_header_p + sizeof (cbc_uint16_arguments_t));
    literal_p -= args_p->register_end;
    frame_ctx.literal_start_p = literal_p;
    literal_p += args_p->literal_end;
  }
  else
  {
    cbc_uint8_arguments_t *args_p = (cbc_uint8_arguments_t *) bytecode_header_p;
    call_stack_size = (uint32_t) (args_p->register_end + args_p->stack_limit);

    literal_p = (ecma_value_t *) ((uint8_t *) bytecode_header_p + sizeof (cbc_uint8_arguments_t));
    literal_p -= args_p->register_end;
    frame_ctx.literal_start_p = literal_p;
    literal_p += args_p->literal_end;
  }

  frame_ctx.bytecode_header_p = bytecode_header_p;
  frame_ctx.byte_code_p = (uint8_t *) literal_p;
  frame_ctx.byte_code_start_p = (uint8_t *) literal_p;
  frame_ctx.lex_env_p = lex_env_p;
  frame_ctx.prev_context_p = JERRY_CONTEXT (vm_top_context_p);
  frame_ctx.this_binding = this_binding_value;
  frame_ctx.context_depth = 0;
  frame_ctx.is_eval_code = is_eval_code;
  frame_ctx.call_operation = VM_NO_EXEC_OP;

  /* Use JERRY_MAX() to avoid array declaration with size 0. */
  ecma_value_t stack[JERRY_MAX (call_stack_size, 1)];
  frame_ctx.registers_p = stack;

  return vm_execute (&frame_ctx, arg_list_p, arg_list_len);
} /* vm_run */

/**
 * Check whether currently executed code is strict mode code
 *
 * @return true - current code is executed in strict mode,
 *         false - otherwise
 */
bool
vm_is_strict_mode (void)
{
  JERRY_ASSERT (JERRY_CONTEXT (vm_top_context_p) != NULL);

  return JERRY_CONTEXT (vm_top_context_p)->bytecode_header_p->status_flags & CBC_CODE_FLAGS_STRICT_MODE;
} /* vm_is_strict_mode */

/**
 * Check whether currently performed call (on top of call-stack) is performed in form,
 * meeting conditions of 'Direct Call to Eval' (see also: ECMA-262 v5, 15.1.2.1.1)
 *
 * Warning:
 *         the function should only be called from implementation
 *         of built-in 'eval' routine of Global object
 *
 * @return true - currently performed call is performed through 'eval' identifier,
 *                without 'this' argument,
 *         false - otherwise
 */
inline bool __attr_always_inline___
vm_is_direct_eval_form_call (void)
{
  return (JERRY_CONTEXT (status_flags) & ECMA_STATUS_DIRECT_EVAL) != 0;
} /* vm_is_direct_eval_form_call */

/**
 * @}
 * @}
 */
