/*
 * FixBrowser v0.1 - https://www.fixbrowser.org/
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "browser.h"

typedef struct {
   Heap *heap;
   Value heap_val;
   Value func;
   Value data;
} ScriptFunc;

typedef struct {
   Heap *heap;
   ScriptFunc **funcs;
   int num_funcs;
} ScriptHeap;

typedef struct {
   Heap *heap;
   int type;
   Value value;
} ScriptHandle;


static void script_heap_free(void *data)
{
   ScriptHeap *heap = data;
   int i;

   fixscript_free_heap(heap->heap);
   for (i=0; i<heap->num_funcs; i++) {
      free(heap->funcs[i]);
   }
   free(heap->funcs);
   free(heap);
}


static void *script_handle_func(Heap *heap, int op, void *p1, void *p2)
{
   ScriptHandle *handle = p1;
   ScriptHandle *handle2 = p2;

   switch (op) {
      case HANDLE_OP_FREE:
         fixscript_unref(handle->heap, handle->value);
         free(handle);
         return NULL;

      case HANDLE_OP_COPY:
         handle2 = calloc(1, sizeof(ScriptHandle));
         handle2->heap = handle->heap;
         handle2->value = handle->value;
         handle2->type = handle->type;
         fixscript_ref(handle->heap, handle->value);
         return handle2;

      case HANDLE_OP_COMPARE:
         if (handle->type == handle2->type && handle->value.value == handle2->value.value && handle->value.is_array == handle2->value.is_array) {
            return (void *)1;
         }
         return (void *)0;

      case HANDLE_OP_HASH:
         return (void *)handle->value.value;
   }

   return NULL;
}


static Value script_log(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   char *str, *s, *n;
   int err;

   if (fixscript_is_string(heap, params[0])) {
      err = fixscript_get_string(heap, params[0], 0, -1, &str, NULL);
   }
   else {
      err = fixscript_to_string(heap, params[0], 0, &str, NULL);
   }
   if (err != FIXSCRIPT_SUCCESS) {
      return fixscript_error(heap, error, err);
   }

   s = str;

   while ((n = strchr(s, '\n'))) {
      fprintf(stderr, "script: %.*s\n", n-s, s);
      s = n+1;
   }

   if (*s) {
      fprintf(stderr, "script: %s\n", s);
   }

   free(str);
   return fixscript_int(0);
}


static Value script_create_heap(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value handle, arr;
   int err;

   script_heap = calloc(1, sizeof(ScriptHeap));
   if (!script_heap) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   script_heap->heap = fixscript_create_heap();
   if (!script_heap->heap) {
      free(script_heap);
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   fixscript_register_native_func(script_heap->heap, "log#1", script_log, NULL);
   
   handle = fixscript_create_handle(heap, HANDLE_TYPE_SCRIPT_HEAP, script_heap, script_heap_free);
   if (!handle.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   arr = fixscript_create_array(heap, 1);
   if (!arr.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   err = fixscript_set_array_elem(heap, arr, 0, handle);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   return arr;
}


static ScriptHeap *get_script_heap(Heap *heap, Value value)
{
   Value handle;
   int err;

   err = fixscript_get_array_elem(heap, value, 0, &handle);
   if (err) return NULL;

   return fixscript_get_handle(heap, handle, HANDLE_TYPE_SCRIPT_HEAP, NULL);
}


static Value script_run_func(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptFunc *func = data;
   Value arr, result, func_result, func_error;
   int err;

   arr = fixscript_create_array(heap, num_params);
   if (!arr.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   err = fixscript_set_array_range(heap, arr, 0, num_params, params);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   err = fixscript_clone_between(func->heap, heap, arr, &arr, NULL, NULL, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   
   func_result = fixscript_call(func->heap, func->func, 2, &func_error, arr, func->data);

   if (func_error.value) {
      err = fixscript_clone_between(heap, func->heap, func_error, error, NULL, NULL, NULL);
      if (err) {
         return fixscript_error(heap, error, err);
      }

      *error = fixscript_create_error(heap, *error);
      return fixscript_int(0);
   }

   err = fixscript_clone_between(heap, func->heap, func_result, &result, NULL, NULL, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return result;
}


static Value script_run_native_func(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptFunc *func = data;
   Value arr, result, func_result, func_error;
   int i, err;

   arr = fixscript_create_array(func->heap, num_params);
   if (!arr.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   for (i=0; i<num_params; i++) {
      err = fixscript_set_array_elem(func->heap, arr, i, fixscript_is_float(params[i])? params[i] : fixscript_int(params[i].value));
      if (err) {
         return fixscript_error(heap, error, err);
      }
   }
   
   func_result = fixscript_call(func->heap, func->func, 3, &func_error, func->heap_val, arr, func->data);

   if (func_error.value) {
      err = fixscript_clone_between(heap, func->heap, func_error, error, NULL, NULL, NULL);
      if (err) {
         return fixscript_error(heap, error, err);
      }

      *error = fixscript_create_error(heap, *error);
      return fixscript_int(0);
   }

   err = fixscript_clone_between(heap, func->heap, func_result, &result, NULL, NULL, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return result;
}


static Value script_collect_heap(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;

   script_heap = get_script_heap(heap, params[0]);
   if (!script_heap) {
      *error = fixscript_create_error_string(heap, "invalid heap");
      return fixscript_int(0);
   }

   fixscript_collect_heap(script_heap->heap);

   return fixscript_int(0);
}


static Value script_register_function(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   ScriptFunc *func, **new_funcs;
   char *name;
   int err;

   script_heap = get_script_heap(heap, params[0]);
   if (!script_heap) {
      *error = fixscript_create_error_string(heap, "invalid heap");
      return fixscript_int(0);
   }

   err = fixscript_append_array_elem(heap, params[0], params[2]);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   err = fixscript_append_array_elem(heap, params[0], params[3]);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   err = fixscript_get_string(heap, params[1], 0, -1, &name, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   func = calloc(1, sizeof(ScriptFunc));
   if (!func) {
      free(name);
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   func->heap = heap;
   func->heap_val = params[0];
   func->func = params[2];
   func->data = params[3];

   new_funcs = realloc(script_heap->funcs, ++script_heap->num_funcs * sizeof(ScriptFunc *));
   if (!new_funcs) {
      script_heap->num_funcs--;
      free(func);
      free(name);
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   script_heap->funcs = new_funcs;
   script_heap->funcs[script_heap->num_funcs-1] = func;

   fixscript_register_native_func(script_heap->heap, name, data? script_run_native_func : script_run_func, func);
   free(name);

   return fixscript_int(0);
}


static Value script_get_length(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   int err, len;

   script_heap = get_script_heap(heap, params[0]);
   if (!script_heap) {
      *error = fixscript_create_error_string(heap, "invalid heap");
      return fixscript_int(0);
   }

   err = fixscript_get_array_length(script_heap->heap, (Value) { params[1].value, 1 }, &len);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   return fixscript_int(len);
}


static Value script_get_value(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value value;
   int err;

   script_heap = get_script_heap(heap, params[0]);
   if (!script_heap) {
      *error = fixscript_create_error_string(heap, "invalid heap");
      return fixscript_int(0);
   }

   err = fixscript_clone_between(heap, script_heap->heap, (Value) { params[1].value, 1 }, &value, NULL, NULL, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   return value;
}


static Value script_set_byte_array(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   char *buf;
   int err, len;

   script_heap = get_script_heap(heap, params[0]);
   if (!script_heap) {
      *error = fixscript_create_error_string(heap, "invalid heap");
      return fixscript_int(0);
   }

   len = params[5].value;
   buf = malloc(len);
   if (!buf) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   err = fixscript_get_array_bytes(heap, params[3], params[4].value, len, buf);
   if (err) {
      free(buf);
      return fixscript_error(heap, error, err);
   }

   err = fixscript_set_array_bytes(script_heap->heap, (Value) { params[1].value, 1 }, params[2].value, len, buf);
   free(buf);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   return fixscript_int(0);
}


static Value script_get_byte_array(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   char *buf;
   int err, len;

   script_heap = get_script_heap(heap, params[0]);
   if (!script_heap) {
      *error = fixscript_create_error_string(heap, "invalid heap");
      return fixscript_int(0);
   }

   len = params[5].value;
   buf = malloc(len);
   if (!buf) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   err = fixscript_get_array_bytes(script_heap->heap, (Value) { params[1].value, 1 }, params[2].value, len, buf);
   if (err) {
      free(buf);
      return fixscript_error(heap, error, err);
   }

   err = fixscript_set_array_bytes(heap, params[3], params[4].value, len, buf);
   free(buf);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   return fixscript_int(0);
}


static Value script_create_handle(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHandle *handle;

   if (params[1].value == 0 && !params[1].is_array) {
      return fixscript_int(0);
   }
   
   handle = calloc(1, sizeof(ScriptHandle));
   handle->heap = heap;
   handle->type = params[0].value;
   handle->value = params[1];
   fixscript_ref(heap, handle->value);

   return fixscript_create_value_handle(heap, HANDLE_TYPE_SCRIPT_VALUE, handle, script_handle_func);
}


static Value script_get_handle_type(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHandle *handle;

   if (params[0].value == 0 && !params[0].is_array) {
      return fixscript_int(-1);
   }

   handle = fixscript_get_handle(heap, params[0], HANDLE_TYPE_SCRIPT_VALUE, NULL);
   if (!handle) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_INVALID_ACCESS);
   }

   return fixscript_int(handle->type);
}


static Value script_get_handle(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHandle *handle;

   if (params[0].value == 0 && !params[0].is_array) {
      return fixscript_int(0);
   }

   handle = fixscript_get_handle(heap, params[0], HANDLE_TYPE_SCRIPT_VALUE, NULL);
   if (!handle) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_INVALID_ACCESS);
   }

   if (handle->heap != heap) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_INVALID_ACCESS);
   }

   if (handle->type != params[1].value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_INVALID_ACCESS);
   }

   return handle->value;
}


static Value script_has_function(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Script *script;
   char *script_name, *error_msg, *func_name;
   Value script_error, result;
   int err, len;

   script_heap = get_script_heap(heap, params[0]);
   if (!script_heap) {
      *error = fixscript_create_error_string(heap, "invalid heap");
      return fixscript_int(0);
   }

   err = fixscript_get_string(heap, params[1], 0, -1, &script_name, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   script = fixscript_load_file(script_heap->heap, script_name, &script_error, "scripts");
   if (!script) {
      error_msg = strdup(fixscript_get_compiler_error(script_heap->heap, script_error));
      len = strlen(error_msg);
      if (len > 0 && error_msg[len-1] == '\n') {
         error_msg[--len] = 0;
      }
      if (len == 7+strlen(script_name)+10 && strncmp(error_msg, "script ", 7) == 0 && strncmp(error_msg+7, script_name, strlen(script_name)) == 0 && strcmp(error_msg+len-10, " not found") == 0) {
         free(error_msg);
         return fixscript_int(0);
      }
      free(script_name);
      err = fixscript_clone_between(heap, script_heap->heap, script_error, error, NULL, NULL, NULL);
      if (err) {
         *error = fixscript_create_error_string(heap, error_msg);
      }
      free(error_msg);
      return fixscript_int(0);
   }
   free(script_name);

   err = fixscript_get_string(heap, params[2], 0, -1, &func_name, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   result = fixscript_int(fixscript_get_function(heap, script, func_name).value != 0);
   free(func_name);
   return result;
}


static Value script_run(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Script *script;
   char *script_name, *error_msg, *func_name;
   Value script_result, script_error, script_args, *args_arr, result;
   int err, num_args, len;

   script_heap = get_script_heap(heap, params[0]);
   if (!script_heap) {
      *error = fixscript_create_error_string(heap, "invalid heap");
      return fixscript_int(0);
   }

   err = fixscript_get_string(heap, params[1], 0, -1, &script_name, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   script = fixscript_load_file(script_heap->heap, script_name, &script_error, "scripts");
   free(script_name);
   if (!script) {
      error_msg = strdup(fixscript_get_compiler_error(script_heap->heap, script_error));
      len = strlen(error_msg);
      if (len > 0 && error_msg[len-1] == '\n') {
         error_msg[--len] = 0;
      }
      err = fixscript_clone_between(heap, script_heap->heap, script_error, error, NULL, NULL, NULL);
      if (err) {
         *error = fixscript_create_error_string(heap, error_msg);
      }
      free(error_msg);
      return fixscript_int(0);
   }

   err = fixscript_clone_between(script_heap->heap, heap, params[3], &script_args, NULL, NULL, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   err = fixscript_get_array_length(script_heap->heap, script_args, &num_args);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   args_arr = malloc(num_args * sizeof(Value));
   if (!args_arr) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   err = fixscript_get_array_range(script_heap->heap, script_args, 0, num_args, args_arr);
   if (err) {
      free(args_arr);
      return fixscript_error(heap, error, err);
   }

   err = fixscript_get_string(heap, params[2], 0, -1, &func_name, NULL);
   if (err) {
      free(args_arr);
      return fixscript_error(heap, error, err);
   }
   script_result = fixscript_run_args(script_heap->heap, script, func_name, &script_error, args_arr);
   free(func_name);
   free(args_arr);

   if (script_error.value) {
      err = fixscript_clone_between(heap, script_heap->heap, script_error, error, NULL, NULL, NULL);
      if (err) {
         return fixscript_error(heap, error, err);
      }

      *error = fixscript_create_error(heap, *error);
      return fixscript_int(0);
   }

   err = fixscript_clone_between(heap, script_heap->heap, script_result, &result, NULL, NULL, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   return result;
}


void register_script_functions(Heap *heap)
{
   fixscript_register_native_func(heap, "script_create_heap#0", script_create_heap, NULL);
   fixscript_register_native_func(heap, "script_collect_heap#1", script_collect_heap, NULL);
   fixscript_register_native_func(heap, "script_register_function#4", script_register_function, (void *)0);
   fixscript_register_native_func(heap, "script_register_native_function#4", script_register_function, (void *)1);
   fixscript_register_native_func(heap, "script_get_length#2", script_get_length, NULL);
   fixscript_register_native_func(heap, "script_get_value#2", script_get_value, NULL);
   fixscript_register_native_func(heap, "script_set_byte_array#6", script_set_byte_array, NULL);
   fixscript_register_native_func(heap, "script_get_byte_array#6", script_get_byte_array, NULL);
   fixscript_register_native_func(heap, "script_create_handle#2", script_create_handle, NULL);
   fixscript_register_native_func(heap, "script_get_handle_type#1", script_get_handle_type, NULL);
   fixscript_register_native_func(heap, "script_get_handle#2", script_get_handle, NULL);
   fixscript_register_native_func(heap, "script_has_function#3", script_has_function, NULL);
   fixscript_register_native_func(heap, "script_run#4", script_run, NULL);
}
