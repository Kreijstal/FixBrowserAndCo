/*
 * FixScript v0.9 - https://www.fixscript.org/
 * Copyright (c) 2018-2024 Martin Dvorak <jezek2@advel.cz>
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * In no event will the authors be held liable for any damages arising from
 * the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose, 
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */

#ifndef FIXSCRIPT_H
#define FIXSCRIPT_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__wasm__) && !defined(FIXSCRIPT_NO_ASYNC)
   #undef FIXSCRIPT_ASYNC
   #define FIXSCRIPT_ASYNC
#endif

typedef struct Heap Heap;
typedef struct Script Script;
typedef struct { int value; int is_array; } Value;
typedef struct SharedArrayHandle SharedArrayHandle;
typedef void (*HandleFreeFunc)(void *p);
typedef void *(*HandleFunc)(Heap *heap, int op, void *p1, void *p2);
typedef Script *(*LoadScriptFunc)(Heap *heap, const char *fname, Value *error, void *data);
typedef Value (*NativeFunc)(Heap *heap, Value *error, int num_params, Value *params, void *data);

#ifdef FIXSCRIPT_ASYNC
typedef void (*ContinuationFunc)(void *data);
typedef void (*ContinuationResultFunc)(Heap *heap, Value result, Value error, void *data);
typedef void (*ContinuationSuspendFunc)(ContinuationFunc resume_func, void *resume_data, void *data);
#endif

enum {
   FIXSCRIPT_SUCCESS                            = 0,
   FIXSCRIPT_ERR_INVALID_ACCESS                 = -1,
   FIXSCRIPT_ERR_INVALID_BYTE_ARRAY             = -2,
   FIXSCRIPT_ERR_INVALID_SHORT_ARRAY            = -3,
   FIXSCRIPT_ERR_INVALID_NULL_STRING            = -4,
   FIXSCRIPT_ERR_CONST_WRITE                    = -5,
   FIXSCRIPT_ERR_OUT_OF_BOUNDS                  = -6,
   FIXSCRIPT_ERR_OUT_OF_MEMORY                  = -7,
   FIXSCRIPT_ERR_INVALID_SHARED_ARRAY_OPERATION = -8,
   FIXSCRIPT_ERR_KEY_NOT_FOUND                  = -9,
   FIXSCRIPT_ERR_RECURSION_LIMIT                = -10,
   FIXSCRIPT_ERR_UNSERIALIZABLE_REF             = -11,
   FIXSCRIPT_ERR_BAD_FORMAT                     = -12,
   FIXSCRIPT_ERR_FUNC_REF_LOAD_ERROR            = -13,
   FIXSCRIPT_ERR_NESTED_WEAKREF                 = -14
};

enum {
   HANDLE_OP_FREE,
   HANDLE_OP_COPY,
   HANDLE_OP_COMPARE,
   HANDLE_OP_HASH,
   HANDLE_OP_TO_STRING,
   HANDLE_OP_MARK_REFS,
   HANDLE_OP_COPY_REFS
};

enum {
   ACCESS_READ_ONLY  = 0x01,
   ACCESS_WRITE_ONLY = 0x02,
   ACCESS_READ_WRITE = 0x03
};

static inline Value fixscript_int(int value);
static inline Value fixscript_float(float value);
static inline int fixscript_is_int(Value value);
static inline int fixscript_is_float(Value value);
static inline int fixscript_get_int(Value value);
static inline float fixscript_get_float(Value value);

Heap *fixscript_create_heap();
void fixscript_free_heap(Heap *heap);
void fixscript_collect_heap(Heap *heap);
long long fixscript_heap_size(Heap *heap);
void fixscript_adjust_heap_size(Heap *heap, long long relative_change);
void fixscript_set_max_stack_size(Heap *heap, int size);
int fixscript_get_max_stack_size(Heap *heap);
int fixscript_get_stack_size(Heap *heap);
void fixscript_ref(Heap *heap, Value value);
void fixscript_unref(Heap *heap, Value value);
void fixscript_set_protected(Heap *heap, Value value, int is_protected);
int fixscript_is_protected(Heap *heap, Value value);
void fixscript_register_cleanup(Heap *heap, HandleFreeFunc free_func, void *data);
void fixscript_register_heap_key(volatile int *key);
int fixscript_set_heap_data(Heap *heap, int key, void *data, HandleFreeFunc free_func);
void *fixscript_get_heap_data(Heap *heap, int key);
void fixscript_set_time_limit(Heap *heap, int limit);
int fixscript_get_remaining_time(Heap *heap);
void fixscript_stop_execution(Heap *heap);

void fixscript_mark_ref(Heap *heap, Value value);
Value fixscript_copy_ref(void *ctx, Value value);

Value fixscript_create_array(Heap *heap, int len);
Value fixscript_create_byte_array(Heap *heap, const char *buf, int len);
int fixscript_set_array_length(Heap *heap, Value arr_val, int len);
int fixscript_get_array_length(Heap *heap, Value arr_val, int *len);
int fixscript_get_array_element_size(Heap *heap, Value arr_val, int *elem_size);
int fixscript_is_array(Heap *heap, Value arr_val);
int fixscript_set_array_elem(Heap *heap, Value arr_val, int idx, Value value);
int fixscript_get_array_elem(Heap *heap, Value arr_val, int idx, Value *value);
int fixscript_append_array_elem(Heap *heap, Value arr_val, Value value);
int fixscript_get_array_range(Heap *heap, Value arr_val, int off, int len, Value *values);
int fixscript_set_array_range(Heap *heap, Value arr_val, int off, int len, Value *values);
int fixscript_get_array_bytes(Heap *heap, Value arr_val, int off, int len, char *bytes);
int fixscript_set_array_bytes(Heap *heap, Value arr_val, int off, int len, char *bytes);
int fixscript_has_array_references(Heap *heap, Value arr_val, int off, int len, int float_as_ref, int *result);
int fixscript_copy_array(Heap *heap, Value dest, int dest_off, Value src, int src_off, int count);
int fixscript_lock_array(Heap *heap, Value arr_val, int off, int len, void **data, int elem_size, int access);
void fixscript_unlock_array(Heap *heap, Value arr_val, int off, int len, void **data, int elem_size, int access);

Value fixscript_create_shared_array(Heap *heap, int len, int elem_size);
Value fixscript_create_or_get_shared_array(Heap *heap, int type, void *ptr, int len, int elem_size, HandleFreeFunc free_func, void *data, int *created);
void fixscript_ref_shared_array(SharedArrayHandle *sah);
void fixscript_unref_shared_array(SharedArrayHandle *sah);
int fixscript_get_shared_array_reference_count(SharedArrayHandle *sah);
SharedArrayHandle *fixscript_get_shared_array_handle(Heap *heap, Value arr_val, int expected_type, int *actual_type);
void *fixscript_get_shared_array_handle_data(SharedArrayHandle *sah, int *len, int *elem_size, void **data, int expected_type, int *actual_type);
Value fixscript_get_shared_array_value(Heap *heap, SharedArrayHandle *sah);
Value fixscript_get_shared_array(Heap *heap, int type, void *ptr, int len, int elem_size, void *data);
void *fixscript_get_shared_array_data(Heap *heap, Value arr_val, int *len, int *elem_size, void **data, int expected_type, int *actual_type);
int fixscript_is_shared_array(Heap *heap, Value arr_val);

Value fixscript_create_string(Heap *heap, const char *s, int len);
Value fixscript_create_string_utf16(Heap *heap, const unsigned short *s, int len);
int fixscript_get_string(Heap *heap, Value str_val, int str_off, int str_len, char **str, int *len_out);
int fixscript_get_string_utf16(Heap *heap, Value str_val, int str_off, int str_len, unsigned short **str, int *len_out);
int fixscript_is_string(Heap *heap, Value str_val);
int fixscript_get_const_string(Heap *heap, Value str_val, int off, int len, Value *ret);
int fixscript_get_const_string_between(Heap *dest, Heap *src, Value str_val, int off, int len, Value *ret);
int fixscript_is_const_string(Heap *heap, Value str_val);

Value fixscript_create_hash(Heap *heap);
int fixscript_is_hash(Heap *heap, Value hash_val);
int fixscript_set_hash_elem(Heap *heap, Value hash_val, Value key_val, Value value_val);
int fixscript_get_hash_elem(Heap *heap, Value hash_val, Value key_val, Value *value_val);
int fixscript_get_hash_elem_between(Heap *heap, Value hash_val, Heap *key_heap, Value key_val, Value *value_val);
int fixscript_remove_hash_elem(Heap *heap, Value hash_val, Value key_val, Value *value_val);
int fixscript_clear_hash(Heap *heap, Value hash_val);
int fixscript_iter_hash(Heap *heap, Value hash_val, Value *key_val, Value *value_val, int *pos);

Value fixscript_create_handle(Heap *heap, int type, void *handle, HandleFreeFunc free_func);
Value fixscript_create_value_handle(Heap *heap, int type, void *handle, HandleFunc handle_func);
void *fixscript_get_handle(Heap *heap, Value handle_val, int expected_type, int *actual_type);
void fixscript_register_handle_types(volatile int *offset, int count);
int fixscript_is_handle(Heap *heap, Value handle_val);

int fixscript_create_weak_ref(Heap *heap, Value value, Value *container, Value *key, Value *weak_ref);
int fixscript_get_weak_ref(Heap *heap, Value weak_ref, Value *value);
int fixscript_is_weak_ref(Heap *heap, Value weak_ref);

const char *fixscript_get_error_msg(int error_code);
Value fixscript_create_error(Heap *heap, Value msg);
Value fixscript_create_error_string(Heap *heap, const char *s);
Value fixscript_error(Heap *heap, Value *error, int code);
const char *fixscript_get_compiler_error(Heap *heap, Value error);

int fixscript_dump_value(Heap *heap, Value value, int newlines);
int fixscript_to_string(Heap *heap, Value value, int newlines, char **str, int *len);

int fixscript_compare(Heap *heap, Value value1, Value value2);
int fixscript_compare_between(Heap *heap1, Value value1, Heap *heap2, Value value2);
int fixscript_clone(Heap *heap, Value value, int deep, Value *clone);
int fixscript_clone_between(Heap *dest, Heap *src, Value value, Value *clone, LoadScriptFunc load_func, void *load_data, Value *error);
int fixscript_serialize(Heap *heap, Value *buf_val, Value value);
int fixscript_unserialize(Heap *heap, Value buf_val, int *off, int len, Value *value);
int fixscript_serialize_to_array(Heap *heap, char **buf, int *len_out, Value value);
int fixscript_unserialize_from_array(Heap *heap, const char *buf, int *off_out, int len, Value *value);

Script *fixscript_load(Heap *heap, const char *src, const char *fname, Value *error, LoadScriptFunc load_func, void *load_data);
Script *fixscript_load_file(Heap *heap, const char *name, Value *error, const char *dirname);
Script *fixscript_load_embed(Heap *heap, const char *name, Value *error, const char * const * const embed_files);
Script *fixscript_reload(Heap *heap, const char *src, const char *fname, Value *error, LoadScriptFunc load_func, void *load_data);
Script *fixscript_resolve_existing(Heap *heap, const char *name, Value *error, void *data);
Script *fixscript_get(Heap *heap, const char *fname);
char *fixscript_get_script_name(Heap *heap, Script *script);
Value fixscript_get_function(Heap *heap, Script *script, const char *func_name);
int fixscript_get_function_list(Heap *heap, Script *script, char ***functions_out, int *count_out);
int fixscript_get_function_name(Heap *heap, Value func_val, char **script_name_out, char **func_name_out, int *num_params_out);
int fixscript_is_func_ref(Heap *heap, Value func_ref);
Value fixscript_run(Heap *heap, Script *script, const char *func_name, Value *error, ...);
Value fixscript_run_args(Heap *heap, Script *script, const char *func_name, Value *error, Value *args);
Value fixscript_call(Heap *heap, Value func, int num_params, Value *error, ...);
Value fixscript_call_args(Heap *heap, Value func, int num_params, Value *error, Value *args);
void fixscript_register_native_func(Heap *heap, const char *name, NativeFunc func, void *data);
NativeFunc fixscript_get_native_func(Heap *heap, const char *name, void **data);

char *fixscript_dump_code(Heap *heap, Script *script, const char *func_name);
char *fixscript_dump_heap(Heap *heap);

#ifdef FIXSCRIPT_ASYNC
void fixscript_set_auto_suspend_handler(Heap *heap, int num_instructions, ContinuationSuspendFunc func, void *data);
void fixscript_get_auto_suspend_handler(Heap *heap, int *num_instructions, ContinuationSuspendFunc *func, void **data);
void fixscript_suspend(Heap *heap, ContinuationResultFunc *func, void **data);
void fixscript_suspend_void(Heap *heap, ContinuationFunc *func, void **data);
void fixscript_run_async(Heap *heap, Script *script, const char *func_name, Value *args, ContinuationResultFunc cont_func, void *cont_data);
void fixscript_call_async(Heap *heap, Value func, int num_params, Value *args, ContinuationResultFunc cont_func, void *cont_data);
void fixscript_allow_sync_call(Heap *heap);
int fixscript_in_async_call(Heap *heap);
#endif


// inline functions:

static inline Value fixscript_int(int value)
{
   return (Value) { value, 0 };
}

static inline Value fixscript_float(float value)
{
   union {
      float f;
      unsigned int i;
   } u;
   u.f = value;
   // flush denormals to zero:
   if ((u.i & (0xFF << 23)) == 0) {
      u.i &= ~((1<<23)-1);
   }
   return (Value) { (int)u.i, 1 };
}

static inline int fixscript_is_int(Value value)
{
   return !value.is_array;
}

static inline int fixscript_is_float(Value value)
{
   return value.is_array && (value.value == 0 || ((unsigned int)value.value) >= (1 << 23));
}

static inline int fixscript_get_int(Value value)
{
   return value.value;
}

static inline float fixscript_get_float(Value value)
{
   union {
      float f;
      unsigned int i;
   } u;
   u.i = value.value;
   return u.f;
}

#ifdef __cplusplus
}
#endif

#endif /* FIXSCRIPT_H */
