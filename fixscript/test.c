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

#include <stdio.h>
#include <stdlib.h>
#include "fixscript.h"

#ifdef __wasm__
#include <wasm-support.h>
#include "test_scripts.h"
#endif

typedef struct {
   Value value;
} NativeRef;

#ifdef __wasm__
typedef struct {
   Heap *heap;
   Value func;
   ContinuationResultFunc cont_func;
   void *cont_data;
} RunLaterData;
#endif


static void *native_ref_handle_func(Heap *heap, int op, void *p1, void *p2)
{
   NativeRef *ref = p1, *copy;

   switch (op) {
      case HANDLE_OP_FREE:
         free(ref);
         break;

      case HANDLE_OP_COPY:
         copy = calloc(1, sizeof(NativeRef));
         if (!copy) return NULL;
         copy->value = ref->value;
         return copy;

      case HANDLE_OP_MARK_REFS:
         fixscript_mark_ref(heap, ref->value);
         break;
         
      case HANDLE_OP_COPY_REFS:
         ref->value = fixscript_copy_ref(p2, ref->value);
         break;
   }
   return NULL;
}


static Value get_func_name_func(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   char *script_name = NULL;
   char *func_name = NULL;
   int func_num_params;
   int err;
   Value ret = fixscript_int(0);
   Value arr, val;
   
   err = fixscript_get_function_name(heap, params[0], &script_name, &func_name, &func_num_params);
   if (err) {
      ret = fixscript_error(heap, error, err);
      goto error;
   }

   arr = fixscript_create_array(heap, 0);
   if (!arr.value) {
      ret = fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   val = fixscript_create_string(heap, script_name, -1);
   if (!val.value) {
      ret = fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   err = fixscript_append_array_elem(heap, arr, val);
   if (err) {
      ret = fixscript_error(heap, error, err);
      goto error;
   }

   val = fixscript_create_string(heap, func_name, -1);
   if (!val.value) {
      ret = fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   err = fixscript_append_array_elem(heap, arr, val);
   if (err) {
      ret = fixscript_error(heap, error, err);
      goto error;
   }

   err = fixscript_append_array_elem(heap, arr, fixscript_int(func_num_params));
   if (err) {
      ret = fixscript_error(heap, error, err);
      goto error;
   }

   ret = arr;

error:
   free(script_name);
   free(func_name);
   return ret;
}


static Value create_handle_func(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   return fixscript_create_handle(heap, 0, (void *)1, NULL);
}


static Value overrided_native_func(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   return fixscript_int(123);
}


static Value test_alt_heap(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Heap *alt_heap = data;
   Value val;
   static Value alt_val;
   int err;

   if (!alt_val.value) {
      alt_val = fixscript_create_array(alt_heap, 2);
      if (!alt_val.value) {
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
      fixscript_ref(alt_heap, alt_val);
   }

   err = fixscript_clone_between(alt_heap, heap, params[0], &val, NULL, NULL, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   err = fixscript_set_array_elem(alt_heap, alt_val, params[1].value, val);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   fixscript_collect_heap(alt_heap);
   return fixscript_int(val.value);
}


static Value test_alt_heap_func_ref0(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Heap *alt_heap = data;
   Value val;
   int err;

   err = fixscript_clone_between(alt_heap, heap, params[0], &val, NULL, NULL, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   err = fixscript_clone_between(heap, alt_heap, val, &val, NULL, NULL, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   return val;
}


static Value test_alt_heap_func_ref1(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Heap *alt_heap = data;
   Value val;
   char *s;
   int err;

   err = fixscript_clone_between(alt_heap, heap, params[0], &val, NULL, NULL, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }

#ifdef __wasm__
   err = fixscript_clone_between(heap, alt_heap, val, &val, (LoadScriptFunc)fixscript_load_embed, (void *)test_scripts, error);
#else
   err = fixscript_clone_between(heap, alt_heap, val, &val, (LoadScriptFunc)fixscript_load_file, ".", error);
#endif
   if (err) {
      if (error->value) {
         return fixscript_int(0);
      }
      return fixscript_error(heap, error, err);
   }

   err = fixscript_to_string(heap, val, 0, &s, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   val = fixscript_create_string(heap, s, -1);
   free(s);
   if (!val.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   return val;
}


static Value test_alt_heap_func_ref2(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Heap *alt_heap = data;
   Value val;
   char *s;
   int err;

   if (params[1].value) {
#ifdef __wasm__
      err = fixscript_clone_between(alt_heap, heap, params[0], &val, (LoadScriptFunc)fixscript_load_embed, (void *)test_scripts, error);
#else
      err = fixscript_clone_between(alt_heap, heap, params[0], &val, (LoadScriptFunc)fixscript_load_file, ".", error);
#endif
   }
   else {
      err = fixscript_clone_between(alt_heap, heap, params[0], &val, NULL, NULL, NULL);
   }
   if (err) {
      if (error->value) {
         err = fixscript_clone_between(heap, alt_heap, *error, error, NULL, NULL, NULL);
         if (err) {
            return fixscript_error(heap, error, err);
         }
         return fixscript_int(0);
      }
      return fixscript_error(heap, error, err);
   }

   err = fixscript_to_string(alt_heap, val, 0, &s, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   val = fixscript_create_string(heap, s, -1);
   free(s);
   if (!val.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   return val;
}


static Value dummy_func(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   *error = fixscript_create_error_string(heap, "dummy function");
   return fixscript_int(0);
}


static Value create_native_ref(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   NativeRef *ref;
   Value ret;

   ref = calloc(1, sizeof(NativeRef));
   if (!ref) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   ref->value = params[0];

   ret = fixscript_create_value_handle(heap, 1, ref, native_ref_handle_func);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return ret;
}


static Value create_heap(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Heap *heap2;
   Value ret;

   heap2 = fixscript_create_heap();
   if (!heap2) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   ret = fixscript_create_handle(heap, 2, heap2, (HandleFreeFunc)fixscript_free_heap);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return ret;
}


static Value heap_reload_script(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Heap *heap2;
   Script *script;
   char *fname = NULL;
   char *src = NULL;
   int err;

   heap2 = fixscript_get_handle(heap, params[0], 2, NULL);
   if (!heap2) {
      *error = fixscript_create_error_string(heap, "invalid heap handle");
      return fixscript_int(0);
   }

   err = fixscript_get_string(heap, params[1], 0, -1, &fname, NULL);
   if (!err) {
      err = fixscript_get_string(heap, params[2], 0, -1, &src, NULL);
   }
   if (err) {
      free(fname);
      free(src);
      return fixscript_error(heap, error, err);
   }

   script = fixscript_reload(heap2, src, fname, error, fixscript_resolve_existing, NULL);
   free(fname);
   free(src);
   if (!script) {
      err = fixscript_clone_between(heap, heap2, *error, error, NULL, NULL, NULL);
      if (err) {
         return fixscript_error(heap, error, err);
      }
      return fixscript_int(0);
   }
   return fixscript_int(0);
}


static Value heap_run_func(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Heap *heap2;
   Script *script;
   char *fname = NULL;
   char *func_name = NULL;
   Value ret, func = fixscript_int(0);
   int err;

   heap2 = fixscript_get_handle(heap, params[0], 2, NULL);
   if (!heap2) {
      *error = fixscript_create_error_string(heap, "invalid heap handle");
      return fixscript_int(0);
   }

   err = fixscript_get_string(heap, params[1], 0, -1, &fname, NULL);
   if (!err) {
      err = fixscript_get_string(heap, params[2], 0, -1, &func_name, NULL);
   }
   if (err) {
      free(fname);
      free(func_name);
      return fixscript_error(heap, error, err);
   }

   script = fixscript_get(heap2, fname);
   if (script) {
      func = fixscript_get_function(heap2, script, func_name);
   }
   free(fname);
   free(func_name);
   if (!func.value) {
      *error = fixscript_create_error_string(heap, "function not found");
      return fixscript_int(0);
   }

   ret = fixscript_call(heap2, func, 0, error);
   if (error->value) {
      err = fixscript_clone_between(heap, heap2, *error, error, NULL, NULL, NULL);
      if (err) {
         return fixscript_error(heap, error, err);
      }
      return fixscript_int(0);
   }

   err = fixscript_clone_between(heap, heap2, ret, &ret, NULL, NULL, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return ret;
}



#ifdef __wasm__
static void run_later_cont2(Heap *heap, Value result, Value error, void *data)
{
   RunLaterData *rld = data;
   ContinuationResultFunc cont_func;
   void *cont_data;

   cont_func = rld->cont_func;
   cont_data = rld->cont_data;
   free(rld);

   cont_func(heap, result, error, cont_data);
}

static void run_later_cont(void *data)
{
   RunLaterData *rld = data;

   fixscript_call_async(rld->heap, rld->func, 0, NULL, run_later_cont2, rld);
}
#endif


static Value run_later(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#ifdef __wasm__
   RunLaterData *rld;

   if (fixscript_in_async_call(heap)) {
      rld = malloc(sizeof(RunLaterData));
      if (!rld) {
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
      rld->heap = heap;
      rld->func = params[0];

      fixscript_suspend(heap, &rld->cont_func, &rld->cont_data);
      wasm_sleep(0, run_later_cont, rld);
      return fixscript_int(0);
   }
#endif

   return fixscript_call_args(heap, params[0], 0, error, NULL);
}


#ifdef __wasm__
static void auto_suspend_func(ContinuationFunc resume_func, void *resume_data, void *data);
static void main_run_cont1(Heap *heap, Value result, Value error, void *data);
static void main_run_cont2(Heap *heap, Value result, Value error, void *data);
#endif

int main(int argc, char **argv)
{
   Heap *heap, *alt_heap;
   Script *script;
#ifndef __wasm__
   Value val;
#endif
   Value error;

   heap = fixscript_create_heap();
   fixscript_register_native_func(heap, "get_func_name#1", get_func_name_func, NULL);
   fixscript_register_native_func(heap, "create_handle#0", create_handle_func, NULL);
   fixscript_register_native_func(heap, "overrided_native_func1#0", overrided_native_func, NULL);
   fixscript_register_native_func(heap, "overrided_native_func2#0", overrided_native_func, NULL);
   fixscript_register_native_func(heap, "create_native_ref#1", create_native_ref, NULL);
   fixscript_register_native_func(heap, "create_heap#0", create_heap, NULL);
   fixscript_register_native_func(heap, "heap_reload_script#3", heap_reload_script, NULL);
   fixscript_register_native_func(heap, "heap_run_func#3", heap_run_func, NULL);
   fixscript_register_native_func(heap, "run_later#1", run_later, NULL);

   alt_heap = fixscript_create_heap();
   fixscript_register_native_func(heap, "test_alt_heap#2", test_alt_heap, alt_heap);
   fixscript_register_native_func(heap, "test_alt_heap_func_ref0#1", test_alt_heap_func_ref0, alt_heap);
   fixscript_register_native_func(heap, "test_alt_heap_func_ref1#1", test_alt_heap_func_ref1, alt_heap);
   fixscript_register_native_func(heap, "test_alt_heap_func_ref2#2", test_alt_heap_func_ref2, alt_heap);
   fixscript_register_native_func(heap, "dummy_native_func#0", dummy_func, NULL);

   fixscript_register_native_func(alt_heap, "get_func_name#1", dummy_func, NULL);
   fixscript_register_native_func(alt_heap, "create_handle#0", dummy_func, NULL);
   fixscript_register_native_func(alt_heap, "overrided_native_func1#0", dummy_func, NULL);
   fixscript_register_native_func(alt_heap, "overrided_native_func2#0", dummy_func, NULL);
   fixscript_register_native_func(alt_heap, "test_alt_heap#2", dummy_func, NULL);
   fixscript_register_native_func(alt_heap, "test_alt_heap_func_ref0#1", dummy_func, NULL);
   fixscript_register_native_func(alt_heap, "test_alt_heap_func_ref1#1", dummy_func, NULL);
   fixscript_register_native_func(alt_heap, "test_alt_heap_func_ref2#2", dummy_func, NULL);
   fixscript_register_native_func(alt_heap, "dummy_native_func#0", dummy_func, NULL);
   fixscript_register_native_func(alt_heap, "create_native_ref#1", dummy_func, NULL);
   fixscript_register_native_func(alt_heap, "create_heap#0", dummy_func, NULL);
   fixscript_register_native_func(alt_heap, "heap_reload_script#3", dummy_func, NULL);
   fixscript_register_native_func(alt_heap, "heap_run_func#3", dummy_func, NULL);
   fixscript_register_native_func(alt_heap, "run_later#1", dummy_func, NULL);

#ifdef __wasm__
   fixscript_set_auto_suspend_handler(heap, 10000, auto_suspend_func, NULL);
#endif

#ifdef __wasm__
   script = fixscript_load_embed(heap, "test", &error, test_scripts);
#else
   script = fixscript_load_file(heap, "test", &error, ".");
#endif
   if (!script) {
      fprintf(stderr, "%s\n", fixscript_get_compiler_error(heap, error));
      fflush(stderr);
      return 0;
   }
#ifdef __wasm__
   fixscript_run_async(heap, script, "test#3", (Value[]) { fixscript_int(1), fixscript_int(2), fixscript_int(3) }, main_run_cont1, NULL);
   #ifdef __wasi__
      wasi_run_loop();
   #endif
#else
   val = fixscript_run(heap, script, "test#3", &error, fixscript_int(1), fixscript_int(2), fixscript_int(3));
   fixscript_dump_value(heap, val, 1);
   fixscript_dump_value(heap, error, 1);
   fprintf(stderr, "calling function dynamically:\n");
   fflush(stderr);
   val = fixscript_call(heap, val, 1, &error, fixscript_create_string(heap, "hello!", -1));
   fixscript_dump_value(heap, val, 1);
   fixscript_dump_value(heap, error, 1);
   fprintf(stderr, "Done!\n");
   fflush(stderr);
#endif
   return 0;
}


#ifdef __wasm__
static void auto_suspend_func(ContinuationFunc resume_func, void *resume_data, void *data)
{
   wasm_sleep(0, resume_func, resume_data);
}

static void main_run_cont1(Heap *heap, Value result, Value error, void *data)
{
   fixscript_dump_value(heap, result, 1);
   fixscript_dump_value(heap, error, 1);
   fprintf(stderr, "calling function dynamically:\n");
   fflush(stderr);
   fixscript_call_async(heap, result, 1, (Value[]) { fixscript_create_string(heap, "hello!", -1) }, main_run_cont2, NULL);
}

static void main_run_cont2(Heap *heap, Value result, Value error, void *data)
{
   fixscript_dump_value(heap, result, 1);
   fixscript_dump_value(heap, error, 1);
   fprintf(stderr, "Done!\n");
   fflush(stderr);
   #ifdef __wasi__
      wasi_exit_loop();
   #endif
}
#endif
