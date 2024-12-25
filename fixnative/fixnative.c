/*
 * FixScript Native v0.4 - https://www.fixscript.org/
 * Copyright (c) 2020-2024 Martin Dvorak <jezek2@advel.cz>
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
#include <stdint.h>
#include <string.h>
#include <limits.h>
#if defined(_WIN32)
#include <windows.h>
#elif defined(__wasm__)
#include <wasm-support.h>
#else
#include <dlfcn.h>
#include <sys/mman.h>
#endif
#include "fixnative.h"

#undef DEF_ARCH_X86
#undef DEF_ARCH_X86_64
#undef DEF_ARCH_WIN64
#undef DEF_ARCH_ARM32

#if defined(__i386__) || defined(_M_IX86)
   #define DEF_ARCH_X86
   #define STACK_TYPE uint32_t
   #define NUM_CALLBACKS 404
#elif defined(_WIN64)
   #define DEF_ARCH_WIN64
   #define STACK_TYPE uint64_t
   #define NUM_CALLBACKS 334
#elif defined(__LP64__)
   #define DEF_ARCH_X86_64
   #define STACK_TYPE uint64_t
   #define NUM_CALLBACKS 333
#elif defined(__arm__)
   #define DEF_ARCH_ARM32
   #ifndef __SYMBIAN32__
      #define USE_HARD_FLOAT
   #endif
   #define STACK_TYPE uint32_t
   #define NUM_CALLBACKS 336
   // TODO
#elif defined(__wasm__)
#else
   #error "unknown architecture type"
#endif

#ifdef WASM_BROWSER
extern int js_create(const char *src, int len) IMPORT("env", "js_create");
extern int js_call(int id, void *ptr) IMPORT("env", "js_call");
#endif

enum {
   SYSTEM_WINDOWS,
   SYSTEM_LINUX,
   SYSTEM_MACOS,
   SYSTEM_HAIKU,
   SYSTEM_SYMBIAN,
   SYSTEM_WEB,
   SYSTEM_WASI
};

enum {
   ARCH_X86,
   ARCH_X86_64,
   ARCH_ARM32,
   ARCH_WASM
};

#ifndef __wasm__
typedef struct {
   char *sig;
   Value func;
   Value data;
} Callback;

typedef struct CallbackBlock {
   uint8_t *ptr;
   int entry_start;
   int entry_size;
   int cnt;
   Heap *heap;
   Callback callbacks[NUM_CALLBACKS];
   struct CallbackBlock *next;
   int ignore_dealloc;
} CallbackBlock;
#endif

typedef struct {
   void *ptr;
   int size;
   int refcnt;
   int owned;
   SharedArrayHandle *sah;
} Memory;

typedef struct {
   Value func;
   Value data;
} Destructor;

enum {
   SYSTEM_GET_TYPE,
   SYSTEM_GET_ARCH,
   SYSTEM_GET_POINTER_SIZE,
   SYSTEM_GET_NLONG_SIZE,
   SYSTEM_GET_POINTER_ALIGN,
   SYSTEM_GET_LONG_ALIGN,
   SYSTEM_GET_NLONG_ALIGN,
   SYSTEM_GET_DOUBLE_ALIGN
};

enum {
   MEM_SET_BYTE,
   MEM_SET_SHORT,
   MEM_SET_INT,
   MEM_SET_LONG,
   MEM_SET_NLONG,
   MEM_SET_FLOAT,
   MEM_SET_DOUBLE,
   MEM_SET_POINTER,

   MEM_GET_BYTE,
   MEM_GET_SHORT,
   MEM_GET_INT,
   MEM_GET_LONG,
   MEM_GET_NLONG,
   MEM_GET_FLOAT,
   MEM_GET_DOUBLE,
   MEM_GET_POINTER,
};

enum {
   FIND_BYTE,
   FIND_SHORT,
   FIND_INT
};

#define NUM_HANDLE_TYPES 4
#define HANDLE_TYPE_POINTER    (handles_offset+0)
#define HANDLE_TYPE_MEMORY     (handles_offset+1)
#define HANDLE_TYPE_VIEW       (handles_offset+2)
#define HANDLE_TYPE_DESTRUCTOR (handles_offset+3)

static volatile int handles_offset = 0;
static volatile int callback_block_key = 0;


#ifdef __SYMBIAN32__
#define __sync_add_and_fetch x__sync_add_and_fetch
static inline int x__sync_add_and_fetch(volatile int *ptr, int amount)
{
   *ptr = (*ptr) + amount;
   return *ptr;
}

#define __sync_sub_and_fetch x__sync_sub_and_fetch
static inline int x__sync_sub_and_fetch(volatile int *ptr, int amount)
{
   *ptr = (*ptr) - amount;
   return *ptr;
}
#endif


static void *pointer_handle_func(Heap *heap, int op, void *p1, void *p2)
{
   char buf[64];

   switch (op) {
      case HANDLE_OP_FREE:
         break;

      case HANDLE_OP_COPY:
         return p1;

      case HANDLE_OP_COMPARE:
         return (void *)(intptr_t)(p1 == p2);

      case HANDLE_OP_HASH:
         return p1;
         
      case HANDLE_OP_TO_STRING:
         snprintf(buf, sizeof(buf), "pointer(%p)", p1);
         return strdup(buf);
   }

   return NULL;
}


static void *memory_handle_func(Heap *heap, int op, void *p1, void *p2)
{
   Memory *mem1 = p1;
   Memory *mem2 = p2;
   char buf[64];

   switch (op) {
      case HANDLE_OP_FREE:
         if (__sync_sub_and_fetch(&mem1->refcnt, 1) == 0) {
            if (mem1->owned) {
               free(mem1->ptr);
            }
            if (mem1->sah) {
               fixscript_unref_shared_array(mem1->sah);
            }
            free(mem1);
         }
         break;

      case HANDLE_OP_COPY:
         (void)__sync_add_and_fetch(&mem1->refcnt, 1);
         return mem1;

      case HANDLE_OP_COMPARE:
         return (void *)(intptr_t)(mem1->ptr == mem2->ptr);

      case HANDLE_OP_HASH:
         return mem1->ptr;
         
      case HANDLE_OP_TO_STRING:
         snprintf(buf, sizeof(buf), "memory(%p,%d%s)", mem1->ptr, mem1->size, mem1->owned? ",owned":"");
         return strdup(buf);
   }

   return NULL;
}


static void *destructor_handle_func(Heap *heap, int op, void *p1, void *p2)
{
   Destructor *destr = p1;
   Value error;
   char *func_name, *s;

   switch (op) {
      case HANDLE_OP_FREE:
         if (destr->func.value) {
            #ifdef __wasm__
               fixscript_allow_sync_call(heap);
            #endif
            fixscript_call(heap, destr->func, 1, &error, destr->data);
            if (error.value) {
               fixscript_dump_value(heap, error, 1);
            }
         }
         fixscript_unref(heap, destr->data);
         free(destr);
         break;
         
      case HANDLE_OP_TO_STRING:
         if (!destr->func.value) {
            return strdup("destructor()");
         }
         if (fixscript_to_string(heap, destr->func, 0, &func_name, NULL) == 0) {
            s = malloc(11+strlen(func_name)+1+1);
            if (!s) {
               free(func_name);
               break;
            }
            strcpy(s, "destructor(");
            strcat(s, func_name);
            strcat(s, ")");
            free(func_name);
            return s;
         }
         break;
   }

   return NULL;
}


static Value create_pointer(Heap *heap, void *ptr)
{
   if (!ptr) {
      return fixscript_int(0);
   }
   return fixscript_create_value_handle(heap, HANDLE_TYPE_POINTER, ptr, pointer_handle_func);
}


static inline void *get_pointer(Heap *heap, Value value, int *size)
{
   Memory *mem;
   void *ptr;
   int type;

   if (!value.value) {
      if (size) {
         *size = -1;
      }
      return NULL;
   }

   ptr = fixscript_get_handle(heap, value, -1, &type);
   if (type == HANDLE_TYPE_POINTER) {
      if (size) {
         *size = -1;
      }
      return ptr;
   }
   if (type == HANDLE_TYPE_MEMORY) {
      mem = ptr;
      if (size) {
         *size = mem->size;
      }
      return mem->ptr;
   }
   return NULL;
}


static Value pointer_create(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   uint64_t ptr = (uint32_t)params[0].value | (((uint64_t)(uint32_t)params[1].value) << 32);
   return create_pointer(heap, (void *)(intptr_t)ptr);
}


static Value pointer_from_shared_array(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   void *ptr;

   ptr = fixscript_get_shared_array_data(heap, params[0], NULL, NULL, NULL, -1, NULL);
   if (!ptr) {
      *error = fixscript_create_error_string(heap, "invalid shared array");
      return fixscript_int(0);
   }
   return create_pointer(heap, ptr);
}


static Value pointer_get_value(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   uint64_t ptr;

   ptr = (intptr_t)get_pointer(heap, params[0], NULL);
   *error = fixscript_int(ptr >> 32);
   return fixscript_int(ptr);
}


static Value pointer_with_offset(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   int64_t ptr;

   ptr = (intptr_t)get_pointer(heap, params[0], NULL);
   ptr += params[1].value;
   return create_pointer(heap, (void *)(intptr_t)ptr);
}


static Value system_get(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   int type = (int)(intptr_t)data;

   switch (type) {
      case SYSTEM_GET_TYPE:
         #if defined(_WIN32)
            return fixscript_int(SYSTEM_WINDOWS);
         #elif defined(__APPLE__)
            return fixscript_int(SYSTEM_MACOS);
         #elif defined(__linux__)
            return fixscript_int(SYSTEM_LINUX);
         #elif defined(__HAIKU__)
            return fixscript_int(SYSTEM_HAIKU);
         #elif defined(__SYMBIAN32__)
            return fixscript_int(SYSTEM_SYMBIAN);
         #elif defined(WASM_BROWSER)
            return fixscript_int(SYSTEM_WEB);
         #elif defined(__wasi__)
            return fixscript_int(SYSTEM_WASI);
         #else
            #error "unknown system type"
         #endif

      case SYSTEM_GET_ARCH:
         #if defined(DEF_ARCH_X86)
            return fixscript_int(ARCH_X86);
         #elif defined(DEF_ARCH_X86_64)
            return fixscript_int(ARCH_X86_64);
         #elif defined(DEF_ARCH_WIN64)
            return fixscript_int(ARCH_X86_64);
         #elif defined(DEF_ARCH_ARM32)
            return fixscript_int(ARCH_ARM32);
         #elif defined(__wasm__)
            return fixscript_int(ARCH_WASM);
         #else
            #error "unknown architecture type"
         #endif

      case SYSTEM_GET_POINTER_SIZE:
         return fixscript_int(sizeof(void *));

      case SYSTEM_GET_NLONG_SIZE:
         return fixscript_int(sizeof(long));
         
      case SYSTEM_GET_POINTER_ALIGN:
         #if defined(DEF_ARCH_X86)
            return fixscript_int(4);
         #elif defined(DEF_ARCH_X86_64)
            return fixscript_int(8);
         #elif defined(DEF_ARCH_WIN64)
            return fixscript_int(8);
         #elif defined(DEF_ARCH_ARM32)
            return fixscript_int(4);
         #elif defined(__wasm__)
            return fixscript_int(4);
         #else
            #error "unknown architecture type"
         #endif

      case SYSTEM_GET_LONG_ALIGN:
         #if defined(DEF_ARCH_X86)
            return fixscript_int(4);
         #elif defined(DEF_ARCH_X86_64)
            return fixscript_int(8);
         #elif defined(DEF_ARCH_WIN64)
            return fixscript_int(8);
         #elif defined(DEF_ARCH_ARM32)
            return fixscript_int(4);
         #elif defined(__wasm__)
            return fixscript_int(8);
         #else
            #error "unknown architecture type"
         #endif

      case SYSTEM_GET_NLONG_ALIGN:
         #if defined(DEF_ARCH_X86)
            return fixscript_int(4);
         #elif defined(DEF_ARCH_X86_64)
            return fixscript_int(8);
         #elif defined(DEF_ARCH_WIN64)
            return fixscript_int(4);
         #elif defined(DEF_ARCH_ARM32)
            return fixscript_int(4);
         #elif defined(__wasm__)
            return fixscript_int(4);
         #else
            #error "unknown architecture type"
         #endif

      case SYSTEM_GET_DOUBLE_ALIGN:
         #if defined(DEF_ARCH_X86)
            return fixscript_int(4);
         #elif defined(DEF_ARCH_X86_64)
            return fixscript_int(8);
         #elif defined(DEF_ARCH_WIN64)
            return fixscript_int(8);
         #elif defined(DEF_ARCH_ARM32)
            return fixscript_int(4);
         #elif defined(__wasm__)
            return fixscript_int(8);
         #else
            #error "unknown architecture type"
         #endif
   }

   return fixscript_int(0);
}


static Value memory_create(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Memory *mem;
   uint64_t ptr;
   
   ptr = (uint32_t)params[0].value | (((uint64_t)(uint32_t)params[1].value) << 32);
   if (params[2].value < 0) {
      *error = fixscript_create_error_string(heap, "negative length");
      return fixscript_int(0);
   }

   mem = calloc(1, sizeof(Memory));
   mem->ptr = (void *)(intptr_t)ptr;
   mem->size = params[2].value;
   mem->refcnt = 1;
   mem->owned = params[3].value != 0;
   return fixscript_create_value_handle(heap, HANDLE_TYPE_MEMORY, mem, memory_handle_func);
}


static Value memory_from_shared_array(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   SharedArrayHandle *sah;
   Memory *mem;
   void *ptr;
   int64_t total_size;
   int off, size, len, elem_size;
   
   sah = fixscript_get_shared_array_handle(heap, params[0], -1, NULL);
   if (!sah) {
      *error = fixscript_create_error_string(heap, "invalid shared array");
      return fixscript_int(0);
   }

   ptr = fixscript_get_shared_array_handle_data(sah, &len, &elem_size, NULL, -1, NULL);
   if (!ptr) {
      *error = fixscript_create_error_string(heap, "invalid shared array");
      return fixscript_int(0);
   }

   total_size = (int64_t)len * (int64_t)elem_size;

   if (num_params == 1) {
      if (total_size > INT_MAX) {
         *error = fixscript_create_error_string(heap, "shared array is too big");
         return fixscript_int(0);
      }
      off = 0;
      size = total_size;
   }
   else {
      off = params[1].value;
      size = params[2].value;
      if (off < 0) {
         *error = fixscript_create_error_string(heap, "negative offset");
         return fixscript_int(0);
      }
      if (size < 0) {
         *error = fixscript_create_error_string(heap, "negative size");
         return fixscript_int(0);
      }
      if ((int64_t)off + (int64_t)size > total_size) {
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_BOUNDS);
      }
   }

   mem = calloc(1, sizeof(Memory));
   mem->ptr = ptr + off;
   mem->size = size;
   mem->refcnt = 1;
   mem->owned = 0;
   mem->sah = sah;
   fixscript_ref_shared_array(sah);
   return fixscript_create_value_handle(heap, HANDLE_TYPE_MEMORY, mem, memory_handle_func);
}


static Value memory_alloc(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Memory *mem;

   mem = calloc(1, sizeof(Memory));
   if (data == (void *)0) {
      mem->ptr = calloc(1, params[0].value);
   }
   else {
      mem->ptr = malloc(params[0].value);
   }
   if (!mem->ptr) {
      free(mem);
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   mem->size = params[0].value;
   mem->refcnt = 1;
   mem->owned = 1;
   return fixscript_create_value_handle(heap, HANDLE_TYPE_MEMORY, mem, memory_handle_func);
}


static Value memory_realloc(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Memory *mem;
   void *new_ptr;

   mem = fixscript_get_handle(heap, params[0], HANDLE_TYPE_MEMORY, NULL);
   if (!mem) {
      *error = fixscript_create_error_string(heap, "invalid memory handle");
      return fixscript_int(0);
   }
   if (!mem->owned) {
      *error = fixscript_create_error_string(heap, "memory not owned");
      return fixscript_int(0);
   }

   new_ptr = realloc(mem->ptr, params[1].value);
   if (!new_ptr) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   mem->ptr = NULL;

   mem = calloc(1, sizeof(Memory));
   mem->ptr = new_ptr;
   mem->size = params[1].value;
   mem->refcnt = 1;
   mem->owned = 1;
   return fixscript_create_value_handle(heap, HANDLE_TYPE_MEMORY, mem, memory_handle_func);
}


static Value memory_free(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Memory *mem;

   mem = fixscript_get_handle(heap, params[0], HANDLE_TYPE_MEMORY, NULL);
   if (!mem) {
      *error = fixscript_create_error_string(heap, "invalid memory handle");
      return fixscript_int(0);
   }

   if (mem->owned) {
      free(mem->ptr);
   }
   mem->ptr = NULL;
   return fixscript_int(0);
}


static Value memory_get_size(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Memory *mem;

   mem = fixscript_get_handle(heap, params[0], HANDLE_TYPE_MEMORY, NULL);
   if (!mem) {
      *error = fixscript_create_error_string(heap, "invalid memory handle");
      return fixscript_int(0);
   }
   return fixscript_int(mem->size);
}


static Value memory_is_owned(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Memory *mem;

   mem = fixscript_get_handle(heap, params[0], HANDLE_TYPE_MEMORY, NULL);
   if (!mem) {
      *error = fixscript_create_error_string(heap, "invalid memory handle");
      return fixscript_int(0);
   }
   return fixscript_int(mem->owned != 0);
}


static void view_free(void *user_data)
{
   Memory *mem = user_data;

   if (mem) {
      memory_handle_func(NULL, HANDLE_OP_FREE, mem, NULL);
   }
}


static Value pointer_get_view(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value ret;
   Memory *mem;
   void *ptr;
   int ptr_size, elem_size;
   int off, len, type;
   int created;

   if (!params[0].value) {
      *error = fixscript_create_error_string(heap, "invalid pointer handle");
      return fixscript_int(0);
   }

   off = params[1].value;
   len = params[2].value;
   elem_size = params[3].value;

   ptr = fixscript_get_handle(heap, params[0], -1, &type);
   if (type == HANDLE_TYPE_POINTER) {
      mem = NULL;
      ptr_size = -1;
   }
   else if (type == HANDLE_TYPE_MEMORY) {
      mem = ptr;
      ptr_size = mem->size;
      ptr = mem->ptr;
   }
   else {
      *error = fixscript_create_error_string(heap, "invalid pointer handle");
      return fixscript_int(0);
   }

   if (((intptr_t)ptr & (elem_size-1)) != 0) {
      *error = fixscript_create_error_string(heap, "unaligned access");
      return fixscript_int(0);
   }

   if (ptr_size >= 0) {
      if (off < 0 || ((int64_t)off + (int64_t)len) * elem_size > ptr_size) {
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_BOUNDS);
      }
   }

   if (mem) {
      (void)__sync_add_and_fetch(&mem->refcnt, 1);
   }

   ret = fixscript_create_or_get_shared_array(heap, HANDLE_TYPE_VIEW, ptr + off * elem_size, len, elem_size, view_free, mem, &created);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   if (!created && mem) {
      memory_handle_func(NULL, HANDLE_OP_FREE, mem, NULL);
   }
   return ret;
}


static Value pointer_memory_op(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   union {
      int i[2];
      float f;
      double d;
      int64_t l;
   } u;
   void *ptr;
   int ptr_size, elem_size;

   ptr = get_pointer(heap, params[0], &ptr_size);
   if (!ptr) {
      *error = fixscript_create_error_string(heap, "invalid pointer handle");
      return fixscript_int(0);
   }

   switch ((int)(intptr_t)data) {
      default:
      case MEM_SET_BYTE:
      case MEM_GET_BYTE:
         elem_size = 1;
         break;

      case MEM_SET_SHORT:
      case MEM_GET_SHORT:
         elem_size = 2;
         break;

      case MEM_SET_INT:
      case MEM_GET_INT:
      case MEM_SET_FLOAT:
      case MEM_GET_FLOAT:
         elem_size = 4;
         break;

      case MEM_SET_LONG:
      case MEM_GET_LONG:
      case MEM_SET_DOUBLE:
      case MEM_GET_DOUBLE:
         elem_size = 8;
         break;

      case MEM_SET_NLONG:
      case MEM_GET_NLONG:
         elem_size = sizeof(long int);
         break;

      case MEM_SET_POINTER:
      case MEM_GET_POINTER:
         elem_size = sizeof(void *);
         break;
   }

   if (ptr_size >= 0) {
      if (params[1].value < 0 || ((int64_t)params[1].value + (int64_t)elem_size) > ptr_size) {
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_BOUNDS);
      }
   }

   ptr += params[1].value;

   switch ((int)(intptr_t)data) {
      case MEM_SET_BYTE:    *((int8_t *)ptr) = params[2].value; break;
      case MEM_SET_SHORT:   *((int16_t *)ptr) = params[2].value; break;
      case MEM_SET_INT:     *((int32_t *)ptr) = params[2].value; break;
      case MEM_SET_LONG:    *((int64_t *)ptr) = ((uint64_t)params[2].value) | (((uint64_t)params[3].value) << 32); break;
      case MEM_SET_NLONG:   *((long int *)ptr) = ((uint64_t)params[2].value) | (((uint64_t)params[3].value) << 32); break;
      case MEM_SET_FLOAT:   u.i[0] = params[2].value; *((float *)ptr) = u.f; break;
      case MEM_SET_DOUBLE:  u.i[0] = params[2].value; u.i[1] = params[3].value; *((double *)ptr) = u.d; break;
      case MEM_SET_POINTER: *((void **)ptr) = get_pointer(heap, params[2], NULL); break;

      case MEM_GET_BYTE:    return fixscript_int(*((uint8_t *)ptr));
      case MEM_GET_SHORT:   return fixscript_int(*((uint16_t *)ptr));
      case MEM_GET_INT:     return fixscript_int(*((int32_t *)ptr));
      case MEM_GET_LONG:    u.l = *((int64_t *)ptr); *error = fixscript_int(u.i[1]); return fixscript_int(u.i[0]);
      case MEM_GET_NLONG:   u.l = *((long int *)ptr); *error = fixscript_int(u.i[1]); return fixscript_int(u.i[0]);
      case MEM_GET_FLOAT:   return fixscript_float(*((float *)ptr));
      case MEM_GET_DOUBLE:  u.d = *((double *)ptr); *error = fixscript_int(u.i[1]); return fixscript_int(u.i[0]);
      case MEM_GET_POINTER: return create_pointer(heap, *((void **)ptr));
         break;
   }

   return fixscript_int(0);
}


static Value pointer_set_get_bytes(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   int get_mode = (data != NULL);
   void *ptr;
   int ptr_size, ptr_off;
   int off, len;
   int err;

   ptr = get_pointer(heap, params[0], &ptr_size);
   if (!ptr) {
      *error = fixscript_create_error_string(heap, "invalid pointer handle");
      return fixscript_int(0);
   }

   ptr_off = params[1].value;

   if (num_params == 3) {
      off = 0;
      err = fixscript_get_array_length(heap, params[2], &len);
      if (err) {
         return fixscript_error(heap, error, err);
      }
   }
   else {
      off = params[3].value;
      len = params[4].value;
   }

   if (ptr_size >= 0) {
      if (ptr_off < 0) {
         *error = fixscript_create_error_string(heap, "negative pointer offset");
         return fixscript_int(0);
      }
      if (((int64_t)ptr_off) + ((int64_t)len) > (int64_t)ptr_size) {
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_BOUNDS);
      }
   }

   if (get_mode) {
      err = fixscript_set_array_bytes(heap, params[2], off, len, ptr + ptr_off);
   }
   else {
      err = fixscript_get_array_bytes(heap, params[2], off, len, ptr + ptr_off);
   }
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return fixscript_int(0);
}


static Value pointer_find(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   int mode = (int)(intptr_t)data;
   void *ptr;
   int ptr_size, ptr_off, ptr_end = -1, extra=0;
   int i, search_val;

   ptr = get_pointer(heap, params[0], &ptr_size);
   if (!ptr) {
      *error = fixscript_create_error_string(heap, "invalid pointer handle");
      return fixscript_int(0);
   }

   ptr_off = params[1].value;
   if (num_params == 4) {
      ptr_end = params[2].value;
      search_val = params[3].value;
   }
   else {
      search_val = params[2].value;
   }

   if (ptr_off < 0) {
      *error = fixscript_create_error_string(heap, "negative pointer offset");
      return fixscript_int(0);
   }

   if (ptr_size >= 0) {
      switch (mode) {
         case FIND_BYTE:  extra = 0; break;
         case FIND_SHORT: extra = 1; break;
         case FIND_INT:   extra = 3; break;
      }
      if (ptr_end < 0 || ((int64_t)ptr_end)+extra > ptr_size) {
         ptr_end = ptr_size-extra;
      }
   }

   if (ptr_end < 0) {
      ptr_end = INT_MAX;
   }

   switch (mode) {
      case FIND_BYTE:
         for (i=ptr_off; i<ptr_end; i++) {
            if (*((char *)(ptr + i)) == (char)search_val) {
               return fixscript_int(i);
            }
         }
         break;

      case FIND_SHORT:
         for (i=ptr_off; i<ptr_end; i+=2) {
            if (*((short *)(ptr + i)) == (short)search_val) {
               return fixscript_int(i);
            }
         }
         break;

      case FIND_INT:
         for (i=ptr_off; i<ptr_end; i+=4) {
            if (*((int *)(ptr + i)) == search_val) {
               return fixscript_int(i);
            }
         }
         break;
   }
   return fixscript_int(-1);
}


static Value native_open_library(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#ifdef __wasm__
   *error = fixscript_create_error_string(heap, "not supported on this platform");
   return fixscript_int(0);
#else
   void *lib;
   char *s = NULL;
   int err;

   if (params[0].value) {
      err = fixscript_get_string(heap, params[0], 0, -1, &s, NULL);
      if (err) {
         return fixscript_error(heap, error, err);
      }
   }
#ifdef _WIN32
   lib = LoadLibrary(s);
#else
   lib = dlopen(s, RTLD_LAZY);
#endif
   free(s);

   return create_pointer(heap, lib);
#endif
}


static Value native_close_library(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#ifdef __wasm__
   *error = fixscript_create_error_string(heap, "not supported on this platform");
   return fixscript_int(0);
#else
   void *lib;
   
   lib = get_pointer(heap, params[0], NULL);
   if (!lib) {
      *error = fixscript_create_error_string(heap, "invalid pointer handle");
      return fixscript_int(0);
   }

#ifdef _WIN32
   FreeLibrary(lib);
#else
   dlclose(lib);
#endif
   return fixscript_int(0);
#endif
}


static Value native_get_symbol(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#ifdef __wasm__
   *error = fixscript_create_error_string(heap, "not supported on this platform");
   return fixscript_int(0);
#else
   void *lib, *sym;
   char *s;
   int err;

   err = fixscript_get_string(heap, params[1], 0, -1, &s, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   if (params[0].value) {
      lib = get_pointer(heap, params[0], NULL);
      if (!lib) {
         *error = fixscript_create_error_string(heap, "invalid pointer handle");
         return fixscript_int(0);
      }
#ifdef _WIN32
      sym = GetProcAddress(lib, s);
#else
      sym = dlsym(lib, s);
#endif
   }
   else {
#if defined(_WIN32)
      sym = GetProcAddress(GetModuleHandle(NULL), s);
#elif defined(__SYMBIAN32__)
      sym = dlsym(RTLD_DEFAULT, s);
#else
      sym = dlsym(NULL, s);
#endif
   }

   free(s);
   return create_pointer(heap, sym);
#endif
}


static Value destructor_create(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Destructor *destr;
   Value ret;

   destr = calloc(1, sizeof(Destructor));
   destr->func = params[0];
   destr->data = params[1];
   fixscript_ref(heap, destr->data);

   ret = fixscript_create_value_handle(heap, HANDLE_TYPE_DESTRUCTOR, destr, destructor_handle_func);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return ret;
}


static Value destructor_disarm(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Destructor *destr;

   destr = fixscript_get_handle(heap, params[0], HANDLE_TYPE_DESTRUCTOR, NULL);
   if (!destr) {
      *error = fixscript_create_error_string(heap, "invalid destructor handle");
      return fixscript_int(0);
   }

   fixscript_unref(heap, destr->data);
   destr->func = fixscript_int(0);
   destr->data = fixscript_int(0);
   return fixscript_int(0);
}


static Value native_call_func(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#ifdef __wasm__
   *error = fixscript_create_error_string(heap, "not supported on this platform");
   return fixscript_int(0);
#else
   Value *args, *a, ret = fixscript_int(0);
   void *func_ptr;
   char *sig, *s;
   int err, num_args;
   int stdcall = 0;
   int variadic = 0;
   STACK_TYPE buf[32+1], *stack, *st;
#if defined(DEF_ARCH_X86)
   uint32_t ret_lo, ret_hi;
   double ret_double;
#elif defined(DEF_ARCH_X86_64)
   int num_int_regs = 0;
   int num_float_regs = 0;
   register uint64_t stack_cnt asm("r8");
   register uint64_t stack_end asm("r10");
   register uint64_t stack_restore asm("r12");
   register uint64_t func_reg asm("r11");
   register uint64_t num_floats asm("rax");
   register uint64_t ret_reg asm("rax");
   register uint64_t ret_reg2 asm("rdx");
#elif defined(DEF_ARCH_WIN64)
   register uint64_t stack_cnt asm("rcx");
   register uint64_t stack_end asm("r10");
   register uint64_t stack_restore asm("r12");
   register uint64_t func_reg asm("r13");
   register uint64_t ret_reg asm("rax");
   register uint64_t ret_reg2 asm("rdx");
#elif defined(DEF_ARCH_ARM32)
   uint32_t regs[4];
   int num_regs = 0;
   int num_stack = 0;
   register uint32_t reg0 asm("r0");
   register uint32_t reg1 asm("r1");
   register uint32_t reg2 asm("r2");
   register uint32_t reg3 asm("r3");
   register uint32_t stack_end asm("r4");
   register uint32_t stack_cnt asm("r5");
   register uint32_t stack_restore asm("r6");
   register uint32_t func_reg asm("r7");
   #ifdef USE_HARD_FLOAT
   uint32_t float_regs[16];
   int float_mask = 0;
   register void *float_regs_ptr asm("r8");
   int i;
   #endif
#endif
   union {
      int i[2];
      float f;
      double d;
      uint64_t l;
      void *p;
   } u;

   func_ptr = get_pointer(heap, params[0], NULL);
   if (!func_ptr) {
      *error = fixscript_create_error_string(heap, "invalid pointer handle");
      return fixscript_int(0);
   }

   err = fixscript_get_string(heap, params[1], 0, -1, &sig, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   if (data == (void *)1) {
      err = fixscript_get_array_length(heap, params[2], &num_args);
      if (err) {
         free(sig);
         return fixscript_error(heap, error, err);
      }
      args = malloc(num_args * sizeof(Value));
      if (!args) {
         free(sig);
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
      err = fixscript_get_array_range(heap, params[2], 0, num_args, args);
      if (err) {
         free(sig);
         free(args);
         return fixscript_error(heap, error, err);
      }
      stack = malloc((num_args*2+1)*sizeof(STACK_TYPE));
      if (!stack) {
         free(sig);
         free(args);
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
   }
   else {
      args = params+2;
      stack = buf;
   }

   s = sig;
   for (;;) {
      if (*s == '$') {
         stdcall = 1;
         s++;
         continue;
      }
      if (*s == '+') {
         variadic = 1;
         s++;
         continue;
      }
      break;
   }
   s++;

#if defined(DEF_ARCH_X86)
   for (st=stack, a=args; *s; s++) {
      switch (*s) {
         case 'i':
         case 'f':
            *st++ = (a++)->value;
            break;

         case 'n':
            *st++ = (a++)->value;
            a++;
            break;

         case 'p':
            *st++ = (intptr_t)get_pointer(heap, *(a++), NULL);
            break;

         case 'l':
         case 'd':
            *st++ = (a++)->value;
            *st++ = (a++)->value;
            break;
      }
   }
   
   asm volatile (
      "again: dec %%ecx\n"
      "       jz end\n"
      "       sub $4, %%esi\n"
      "       push (%%esi)\n"
      "       jmp again\n"

      "end:   call *%%eax\n"
      "       add %%ebx, %%esp\n"

      //"       test %%edi, %%edi\n"
      //"       jz skip\n"
      "       fstpl (%%edi)\n"
      //"skip:  \n"

      : "=a" (ret_lo), "=d" (ret_hi)
      : "S" (st), "c" ((st-stack)+1), "b" (stdcall? 0 : (st-stack)*4), "a" (func_ptr), "D" (&ret_double)
      : "cc", "memory"
   );

   switch (sig[stdcall+variadic]) {
      case 'i':
         ret = fixscript_int(ret_lo);
         break;

      case 'p':
         ret = create_pointer(heap, (void *)ret_lo);
         break;

      case 'n':
         ret = fixscript_int(ret_lo);
         *error = fixscript_int(((int32_t)ret_lo) >> 31);
         break;

      case 'l':
         ret = fixscript_int(ret_lo);
         *error = fixscript_int(ret_hi);
         break;

      case 'f':
         ret = fixscript_float(ret_double);
         break;

      case 'd':
         u.d = ret_double;
         ret = fixscript_int(u.i[0]);
         *error = fixscript_int(u.i[1]);
         break;
   }
#elif defined(DEF_ARCH_X86_64)
   for (st=stack+14, a=args; *s; s++) {
      switch (*s) {
         case 'i':
            if (num_int_regs < 6) {
               stack[num_int_regs++] = (a++)->value;
            }
            else {
               *st++ = (a++)->value;
            }
            break;

         case 'f':
            if (num_float_regs < 8) {
               stack[6 + num_float_regs++] = (a++)->value;
            }
            else {
               *st++ = (a++)->value;
            }
            break;

         case 'n':
            if (num_int_regs < 6) {
               stack[num_int_regs++] = (a++)->value;
            }
            else {
               *st++ = (a++)->value;
            }
            a++;
            break;

         case 'p':
            if (num_int_regs < 6) {
               stack[num_int_regs++] = (intptr_t)get_pointer(heap, *(a++), NULL);
            }
            else {
               *st++ = (intptr_t)get_pointer(heap, *(a++), NULL);
            }
            break;

         case 'l':
            u.i[0] = (a++)->value;
            u.i[1] = (a++)->value;
            if (num_int_regs < 6) {
               stack[num_int_regs++] = u.l;
            }
            else {
               *st++ = u.l;
            }
            break;

         case 'd':
            u.i[0] = (a++)->value;
            u.i[1] = (a++)->value;
            if (num_float_regs < 8) {
               stack[6 + num_float_regs++] = u.l;
            }
            else {
               *st++ = u.l;
            }
            break;
      }
   }

   if ((st - stack) & 1) {
      st++;
   }

   stack_cnt = (st-stack-14)+1;
   stack_end = (intptr_t)st;
   stack_restore = (st-stack-14)*8;
   func_reg = (intptr_t)func_ptr;
   num_floats = num_float_regs; 

   asm volatile (
      "again: dec %%r8\n"
      "       jz end\n"
      "       sub $8, %%r10\n"
      "       push (%%r10)\n"
      "       jmp again\n"
      "end:   movq -8(%%r10), %%xmm7\n"
      "       movq -16(%%r10), %%xmm6\n"
      "       movq -24(%%r10), %%xmm5\n"
      "       movq -32(%%r10), %%xmm4\n"
      "       movq -40(%%r10), %%xmm3\n"
      "       movq -48(%%r10), %%xmm2\n"
      "       movq -56(%%r10), %%xmm1\n"
      "       movq -64(%%r10), %%xmm0\n"
      "       movq -72(%%r10), %%r9\n"
      "       movq -80(%%r10), %%r8\n"
      "       movq -88(%%r10), %%rcx\n"
      "       movq -96(%%r10), %%rdx\n"
      "       movq -104(%%r10), %%rsi\n"
      "       movq -112(%%r10), %%rdi\n"
      "       call *%%r11\n"
      "       add %%r12, %%rsp\n"
      "       movq %%xmm0, %%rdx\n"

      : "=r" (ret_reg), "=r" (ret_reg2)
      : "r" (stack_end), "r" (stack_cnt), "r" (stack_restore), "r" (func_reg), "r" (num_floats)
      : "rdi", "rsi", "rcx", "r9", "memory", "cc",
        "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
        "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"
   );

   switch (sig[stdcall+variadic]) {
      case 'i':
         u.l = ret_reg;
         ret = fixscript_int(u.i[0]);
         break;

      case 'f':
         u.l = ret_reg2;
         ret = fixscript_float(u.f);
         break;

      case 'p':
         u.l = ret_reg;
         ret = create_pointer(heap, u.p);
         break;

      case 'n':
      case 'l':
         u.l = ret_reg;
         ret = fixscript_int(u.i[0]);
         *error = fixscript_int(u.i[1]);
         break;

      case 'd':
         u.l = ret_reg2;
         ret = fixscript_int(u.i[0]);
         *error = fixscript_int(u.i[1]);
         break;
   }
#elif defined(DEF_ARCH_WIN64)
   for (st=stack, a=args; *s; s++) {
      switch (*s) {
         case 'i':
         case 'f':
            *st++ = (a++)->value;
            break;

         case 'n':
            *st++ = (a++)->value;
            a++;
            break;

         case 'p':
            *st++ = (intptr_t)get_pointer(heap, *(a++), NULL);
            break;

         case 'l':
         case 'd':
            u.i[0] = (a++)->value;
            u.i[1] = (a++)->value;
            *st++ = u.l;
            break;
      }
   }

   while (st - stack < 4) {
      st++;
   }

   if ((st - stack) & 1) {
      st++;
   }
   
   stack_cnt = (st-stack-4)+1;
   stack_end = (intptr_t)st;
   stack_restore = (st-stack-4)*8;
   func_reg = (intptr_t)func_ptr;

   asm volatile (
      "again: dec %%rcx\n"
      "       jz end\n"
      "       sub $8, %%r10\n"
      "       push (%%r10)\n"
      "       jmp again\n"
      "end:   movq -8(%%r10), %%r9\n"
      "       movq -16(%%r10), %%r8\n"
      "       movq -24(%%r10), %%rdx\n"
      "       movq -32(%%r10), %%rcx\n"
      "       movq %%r9, %%xmm3\n"
      "       movq %%r8, %%xmm2\n"
      "       movq %%rdx, %%xmm1\n"
      "       movq %%rcx, %%xmm0\n"
      "       subq $32, %%rsp\n"
      "       call *%%r13\n"
      "       addq $32, %%rsp\n"
      "       add %%r12, %%rsp\n"
      "       movq %%xmm0, %%rdx\n"

      : "=r" (ret_reg), "=r" (ret_reg2)
      : "r" (stack_end), "r" (stack_cnt), "r" (stack_restore), "r" (func_reg)
      : "r8", "r9", "r11", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "cc", "memory"
   );

   switch (sig[stdcall+variadic]) {
      case 'i':
         u.l = ret_reg;
         ret = fixscript_int(u.i[0]);
         break;

      case 'f':
         u.l = ret_reg2;
         ret = fixscript_float(u.f);
         break;

      case 'p':
         u.l = ret_reg;
         ret = create_pointer(heap, u.p);
         break;

      case 'n':
         ret = fixscript_int(ret_reg);
         *error = fixscript_int(((int32_t)ret_reg) >> 31);
         break;

      case 'l':
         u.l = ret_reg;
         ret = fixscript_int(u.i[0]);
         *error = fixscript_int(u.i[1]);
         break;

      case 'd':
         u.l = ret_reg2;
         ret = fixscript_int(u.i[0]);
         *error = fixscript_int(u.i[1]);
         break;
   }
#elif defined(DEF_ARCH_ARM32)
   for (st=stack, a=args; *s; s++) {
      switch (*s) {
         case 'f':
            #ifdef USE_HARD_FLOAT
            if (!variadic) {
               int found = 0;
               for (i=0; i<16; i++) {
                  if ((float_mask & (1 << i)) == 0) {
                     float_mask |= 1 << i;
                     float_regs[i] = (a++)->value;
                     found = 1;
                     break;
                  }
               }
               if (!found) {
                  num_stack++;
                  *st++ = (a++)->value;
                  float_mask = -1;
               }
               break;
            }
            #endif
            // fallthrough

         case 'i':
            if (num_regs < 4) {
               regs[num_regs++] = (a++)->value;
            }
            else {
               num_stack++;
               *st++ = (a++)->value;
            }
            break;

         case 'n':
            if (num_regs < 4) {
               regs[num_regs++] = (a++)->value;
            }
            else {
               num_stack++;
               *st++ = (a++)->value;
            }
            a++;
            break;

         case 'p':
            if (num_regs < 4) {
               regs[num_regs++] = (intptr_t)get_pointer(heap, *(a++), NULL);
            }
            else {
               num_stack++;
               *st++ = (intptr_t)get_pointer(heap, *(a++), NULL);
            }
            break;

         case 'd':
            #ifdef USE_HARD_FLOAT
            if (!variadic) {
               int found = 0;
               for (i=0; i<16; i+=2) {
                  if ((float_mask & (3 << i)) == 0) {
                     float_mask |= 3 << i;
                     float_regs[i+0] = (a++)->value;
                     float_regs[i+1] = (a++)->value;
                     found = 1;
                     break;
                  }
               }
               if (!found) {
                  if (num_stack & 1) {
                     *st++ = 0;
                     num_stack++;
                  }
                  num_stack += 2;
                  *st++ = (a++)->value;
                  *st++ = (a++)->value;
                  float_mask = -1;
               }
               break;
            }
            #endif
            // fallthrough

         case 'l':
            if (num_regs < 3) {
               num_regs = (num_regs+1) & ~1;
               regs[num_regs++] = (a++)->value;
               regs[num_regs++] = (a++)->value;
            }
            else {
               num_regs = 4;
               if (num_stack & 1) {
                  *st++ = 0;
                  num_stack++;
               }
               num_stack += 2;
               *st++ = (a++)->value;
               *st++ = (a++)->value;
            }
            break;
      }
   }

   if ((st - stack) & 1) {
      st++;
   }

   reg0 = regs[0];
   reg1 = regs[1];
   reg2 = regs[2];
   reg3 = regs[3];
   stack_cnt = (st-stack)+1;
   stack_end = (intptr_t)st;
   stack_restore = (st-stack)*4;
   func_reg = (intptr_t)func_ptr;
   #ifdef USE_HARD_FLOAT
   float_regs_ptr = float_regs;
   #endif

   asm volatile (
      "       mov r10, r0\n"

      "again: sub r5, #1\n"
      "       cmp r5, #0\n"
      "       beq end\n"
      "       sub r4, #4\n"
      "       ldr r0, [r4]\n"
      "       sub sp, #4\n"
      "       str r0, [sp]\n"
      "       b again\n"
      "end:\n"

      #ifdef USE_HARD_FLOAT
      "       vldm r8, {d0,d1,d2,d3,d4,d5,d6,d7}\n"
      #endif

      "       mov r0, r10\n"
      "       blx r7\n"
      "       add sp, r6\n"

      #ifdef USE_HARD_FLOAT
      "       vstr d0, [r8]\n"
      #endif

      : "+l" (reg0), "+l" (reg1)
      : "l" (reg2), "l" (reg3), "l" (stack_end), "l" (stack_cnt), "l" (stack_restore), "l" (func_reg)
      #ifdef USE_HARD_FLOAT
      , "l" (float_regs_ptr)
      : "r9", "r10", "r11", "r12", "cc", "memory", "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7"
      #else
      : "r8", "r9", "r10", "r11", "r12", "cc", "memory"
      #endif
   );

   switch (sig[stdcall+variadic]) {
      case 'i':
         ret = fixscript_int(reg0);
         break;

      case 'p':
         ret = create_pointer(heap, (void *)reg0);
         break;

      case 'n':
         ret = fixscript_int(reg0);
         *error = fixscript_int(((int32_t)reg0) >> 31);
         break;

      case 'l':
         ret = fixscript_int(reg0);
         *error = fixscript_int(reg1);
         break;

      case 'f':
         #ifdef USE_HARD_FLOAT
         if (!variadic) {
            u.i[0] = float_regs[0];
            ret = fixscript_float(u.f);
            break;
         }
         #endif
         u.i[0] = reg0;
         ret = fixscript_float(u.f);
         break;

      case 'd':
         #ifdef USE_HARD_FLOAT
         if (!variadic) {
            ret = fixscript_int(float_regs[0]);
            *error = fixscript_int(float_regs[1]);
            break;
         }
         #endif
         ret = fixscript_int(reg0);
         *error = fixscript_int(reg1);
         break;
   }
#else
   #error "assembler part not provided for architecture"
#endif

   free(sig);
   if (data == (void *)1) {
      free(args);
      free(stack);
   }
   return ret;
#endif
}


#ifndef __wasm__

typedef struct {
#if defined(DEF_ARCH_X86)
   int type;
   int stack_inc;
   union {
      float f;
      double d;
   };
#elif defined(DEF_ARCH_X86_64)
   union {
      float f;
      double d;
   };
#elif defined(DEF_ARCH_WIN64)
   union {
      float f;
      double d;
   };
#endif
} RetData;

#if defined(DEF_ARCH_X86_64)
static uint64_t callback(uint64_t reg0, uint64_t reg1, uint64_t reg2, uint64_t reg3, uint64_t reg4, uint64_t reg5, uint64_t freg0, uint64_t freg1, uint64_t freg2, uint64_t freg3, uint64_t freg4, uint64_t freg5, uint64_t freg6, uint64_t freg7, RetData *ret, CallbackBlock *block, int idx, uint64_t *stack)
#elif defined(DEF_ARCH_WIN64)
static uint64_t callback(uint64_t reg0, uint64_t reg1, uint64_t reg2, uint64_t reg3, uint64_t freg0, uint64_t freg1, uint64_t freg2, uint64_t freg3, RetData *ret, CallbackBlock *block, int64_t idx, uint64_t *stack)
#elif defined(DEF_ARCH_ARM32)
static uint64_t callback(CallbackBlock *block, int32_t idx, uint32_t *stack)
#else
static uint64_t callback(RetData *ret, CallbackBlock *block, int idx, uint32_t *stack)
#endif
{
#ifdef DEF_ARCH_ARM32
   Callback *callback = &block->callbacks[idx >= 256? 255+(idx>>8) : idx];
#else
   Callback *callback = &block->callbacks[idx];
#endif
   Value buf[16*2+1], *params, *p;
   Value error, ret_val;
   const char *s;
   int stdcall = 0;
   int variadic = 0;
   int num_params=0, params_allocated=0;
   uint64_t ret_int=0;
   STACK_TYPE *st;
   union {
      int i[2];
      float f;
      double d;
      uint64_t l;
   } u;
#if defined(DEF_ARCH_X86_64)
   uint64_t regs[6];
   uint64_t fregs[8];
   int int_regs_idx = 0;
   int float_regs_idx = 0;
#elif defined(DEF_ARCH_WIN64)
   uint64_t regs[4];
   uint64_t fregs[4];
   int regs_idx = 0;
#elif defined(DEF_ARCH_ARM32)
   uint32_t *int_regs;
   uint32_t *float_regs;
   int regs_idx = 0;
   int float_mask = 0;
   int i;
#endif

   s = callback->sig;
   for (;;) {
      if (*s == '$') {
         stdcall = 1;
         s++;
         continue;
      }
      if (*s == '+') {
         variadic = 1;
         s++;
         continue;
      }
      break;
   }
   s++;
   for (; *s; s++) {
      num_params++;
   }
   if (num_params > 16) {
      params = malloc((num_params*2+1)*sizeof(Value));
      params_allocated = 1;
   }
   else {
      params = buf;
   }

   s = &callback->sig[stdcall+variadic+1];
   p = params;
   *p++ = callback->data;

#if defined(DEF_ARCH_X86)
   for (st=&stack[1]; *s; s++) {
      switch (*s) {
         case 'i':
            *p++ = fixscript_int(*st++);
            break;

         case 'n':
            *p++ = fixscript_int(*st);
            *p++ = fixscript_int(((int32_t)(*st++)) >> 31);
            num_params++;
            break;

         case 'f':
            u.i[0] = *st++;
            *p++ = fixscript_float(u.f);
            break;

         case 'p':
            *p++ = create_pointer(block->heap, (void *)(intptr_t)(*st++));
            break;

         case 'l':
         case 'd':
            *p++ = fixscript_int(*st++);
            *p++ = fixscript_int(*st++);
            num_params++;
            break;
      }
   }
#elif defined(DEF_ARCH_X86_64)
   regs[0] = reg0;
   regs[1] = reg1;
   regs[2] = reg2;
   regs[3] = reg3;
   regs[4] = reg4;
   regs[5] = reg5;
   fregs[0] = freg0;
   fregs[1] = freg1;
   fregs[2] = freg2;
   fregs[3] = freg3;
   fregs[4] = freg4;
   fregs[5] = freg5;
   fregs[6] = freg6;
   fregs[7] = freg7;

   for (st=&stack[5]; *s; s++) {
      switch (*s) {
         case 'i':
            if (int_regs_idx < 6) {
               *p++ = fixscript_int(regs[int_regs_idx++]);
            }
            else {
               *p++ = fixscript_int(*st++);
            }
            break;

         case 'f':
            if (float_regs_idx < 8) {
               u.i[0] = fregs[float_regs_idx++];
            }
            else {
               u.i[0] = *st++;
            }
            *p++ = fixscript_float(u.f);
            break;

         case 'p':
            if (int_regs_idx < 6) {
               *p++ = create_pointer(block->heap, (void *)(intptr_t)regs[int_regs_idx++]);
            }
            else {
               *p++ = create_pointer(block->heap, (void *)(intptr_t)(*st++));
            }
            break;

         case 'n':
         case 'l':
            if (int_regs_idx < 6) {
               u.l = regs[int_regs_idx++];
            }
            else {
               u.l = *st++;
            }
            *p++ = fixscript_int(u.i[0]);
            *p++ = fixscript_int(u.i[1]);
            num_params++;
            break;

         case 'd':
            if (float_regs_idx < 8) {
               u.l = fregs[float_regs_idx++];
            }
            else {
               u.l = *st++;
            }
            *p++ = fixscript_int(u.i[0]);
            *p++ = fixscript_int(u.i[1]);
            num_params++;
            break;
      }
   }
#elif defined(DEF_ARCH_WIN64)
   regs[0] = reg0;
   regs[1] = reg1;
   regs[2] = reg2;
   regs[3] = reg3;
   fregs[0] = freg0;
   fregs[1] = freg1;
   fregs[2] = freg2;
   fregs[3] = freg3;

   for (st=&stack[5]; *s; s++) {
      switch (*s) {
         case 'i':
            if (regs_idx < 4) {
               *p++ = fixscript_int(regs[regs_idx++]);
            }
            else {
               *p++ = fixscript_int(*st++);
            }
            break;

         case 'n':
            if (regs_idx < 4) {
               *p++ = fixscript_int(regs[regs_idx]);
               *p++ = fixscript_int(((int32_t)regs[regs_idx++]) >> 31);
            }
            else {
               *p++ = fixscript_int(*st);
               *p++ = fixscript_int(((int32_t)(*st++)) >> 31);
            }
            num_params++;
            break;

         case 'f':
            if (regs_idx < 4) {
               u.i[0] = fregs[regs_idx++];
            }
            else {
               u.i[0] = *st++;
            }
            *p++ = fixscript_float(u.f);
            break;

         case 'p':
            if (regs_idx < 4) {
               *p++ = create_pointer(block->heap, (void *)(intptr_t)regs[regs_idx++]);
            }
            else {
               *p++ = create_pointer(block->heap, (void *)(intptr_t)(*st++));
            }
            break;

         case 'l':
            if (regs_idx < 4) {
               u.l = regs[regs_idx++];
            }
            else {
               u.l = *st++;
            }
            *p++ = fixscript_int(u.i[0]);
            *p++ = fixscript_int(u.i[1]);
            num_params++;
            break;

         case 'd':
            if (regs_idx < 4) {
               u.l = fregs[regs_idx++];
            }
            else {
               u.l = *st++;
            }
            *p++ = fixscript_int(u.i[0]);
            *p++ = fixscript_int(u.i[1]);
            num_params++;
            break;
      }
   }
#elif defined(DEF_ARCH_ARM32)
   int_regs = stack;
   float_regs = stack+4;
   stack += 4+16+2;

   for (st=stack; *s; s++) {
      switch (*s) {
         case 'i':
            if (regs_idx < 4) {
               *p++ = fixscript_int(int_regs[regs_idx++]);
            }
            else {
               *p++ = fixscript_int(*st++);
            }
            break;

         case 'n':
            if (regs_idx < 4) {
               *p++ = fixscript_int(int_regs[regs_idx]);
               *p++ = fixscript_int(((int32_t)(int_regs[regs_idx++])) >> 31);
            }
            else {
               *p++ = fixscript_int(*st);
               *p++ = fixscript_int(((int32_t)(*st++)) >> 31);
            }
            num_params++;
            break;

         case 'f':
            #ifdef USE_HARD_FLOAT
            if (!variadic) {
               int found = 0;
               for (i=0; i<16; i++) {
                  if ((float_mask & (1 << i)) == 0) {
                     float_mask |= 1 << i;
                     u.i[0] = float_regs[i];
                     found = 1;
                     break;
                  }
               }
               if (!found) {
                  u.i[0] = *st++;
                  float_mask = -1;
               }
            }
            else
            #endif
            {
               if (regs_idx < 4) {
                  u.i[0] = int_regs[regs_idx++];
               }
               else {
                  u.i[0] = *st++;
               }
            }
            *p++ = fixscript_float(u.f);
            break;

         case 'p':
            if (regs_idx < 4) {
               *p++ = create_pointer(block->heap, (void *)(intptr_t)int_regs[regs_idx++]);
            }
            else {
               *p++ = create_pointer(block->heap, (void *)(intptr_t)(*st++));
            }
            break;

         case 'd':
            #ifdef USE_HARD_FLOAT
            if (!variadic) {
               int found = 0;
               for (i=0; i<16; i+=2) {
                  if ((float_mask & (3 << i)) == 0) {
                     float_mask |= 3 << i;
                     *p++ = fixscript_int(float_regs[i+0]);
                     *p++ = fixscript_int(float_regs[i+1]);
                     found = 1;
                     break;
                  }
               }
               if (!found) {
                  if ((st - stack) & 1) {
                     st++;
                  }
                  *p++ = fixscript_int(*st++);
                  *p++ = fixscript_int(*st++);
                  float_mask = -1;
               }
               num_params++;
               break;
            }
            #endif
            // fallthrough

         case 'l':
            if (regs_idx < 3) {
               if (regs_idx & 1) regs_idx++;
               *p++ = fixscript_int(int_regs[regs_idx++]);
               *p++ = fixscript_int(int_regs[regs_idx++]);
            }
            else {
               regs_idx = 4;
               if ((st - stack) & 1) {
                  st++;
               }
               *p++ = fixscript_int(*st++);
               *p++ = fixscript_int(*st++);
            }
            num_params++;
            break;
      }
   }

#else
   #error "code not provided for architecture"
#endif

   ret_val = fixscript_call_args(block->heap, callback->func, num_params+1, &error, params);
   if (error.is_array && fixscript_is_array(block->heap, error)) {
      fixscript_dump_value(block->heap, error, 1);
   }
   
#if defined(DEF_ARCH_X86)
   ret->type = 0;
   ret->stack_inc = stdcall? (st - &stack[1])*4 : 0;

   switch (callback->sig[stdcall+variadic]) {
      case 'i':
      case 'n':
         ret_int = (uint32_t)ret_val.value;
         break;

      case 'p':
         ret_int = (uint32_t)(intptr_t)get_pointer(block->heap, ret_val, NULL);
         break;

      case 'l':
         u.i[0] = ret_val.value;
         u.i[1] = error.value;
         ret_int = u.l;
         break;

      case 'f':
         ret->type = 1;
         ret->f = fixscript_get_float(ret_val);
         break;

      case 'd':
         u.i[0] = ret_val.value;
         u.i[1] = error.value;
         ret->type = 2;
         ret->d = u.d;
         break;
   }
#elif defined(DEF_ARCH_X86_64)
   switch (callback->sig[stdcall+variadic]) {
      case 'i':
         ret_int = ret_val.value;
         break;

      case 'p':
         ret_int = (intptr_t)get_pointer(block->heap, ret_val, NULL);
         break;

      case 'n':
      case 'l':
         u.i[0] = ret_val.value;
         u.i[1] = error.value;
         ret_int = u.l;
         break;

      case 'f':
         ret->f = fixscript_get_float(ret_val);
         break;

      case 'd':
         u.i[0] = ret_val.value;
         u.i[1] = error.value;
         ret->d = u.d;
         break;
   }
#elif defined(DEF_ARCH_WIN64)
   switch (callback->sig[stdcall+variadic]) {
      case 'i':
      case 'n':
         ret_int = ret_val.value;
         break;

      case 'p':
         ret_int = (intptr_t)get_pointer(block->heap, ret_val, NULL);
         break;

      case 'l':
         u.i[0] = ret_val.value;
         u.i[1] = error.value;
         ret_int = u.l;
         break;

      case 'f':
         ret->f = fixscript_get_float(ret_val);
         break;

      case 'd':
         u.i[0] = ret_val.value;
         u.i[1] = error.value;
         ret->d = u.d;
         break;
   }
#elif defined(DEF_ARCH_ARM32)
   switch (callback->sig[stdcall+variadic]) {
      case 'i':
      case 'n':
         ret_int = (uint32_t)ret_val.value;
         break;

      case 'p':
         ret_int = (uint32_t)(intptr_t)get_pointer(block->heap, ret_val, NULL);
         break;

      case 'l':
         u.i[0] = ret_val.value;
         u.i[1] = error.value;
         ret_int = u.l;
         break;

      case 'f':
         u.f = fixscript_get_float(ret_val);
         ret_int = u.i[0];
         break;

      case 'd':
         u.i[0] = ret_val.value;
         u.i[1] = error.value;
         ret_int = u.l;
         break;
   }
#else
   #error "code not provided for architecture"
#endif
   
   if (params_allocated) {
      free(params);
   }
   return ret_int;
}


static CallbackBlock *alloc_callback_block(Heap *heap)
{
   #define PUT1(v) *p++ = v
   #define PUT2(v0, v1) PUT1(v0); PUT1(v1)
   #define PUT3(v0, v1, v2) PUT2(v0, v1); PUT1(v2)
   #define PUT4(v0, v1, v2, v3) PUT2(v0, v1); PUT2(v2, v3)
   #define PUT5(v0, v1, v2, v3, v4) PUT4(v0, v1, v2, v3); PUT1(v4)
   #define PUT_INT(v) PUT4((v) & 0xFF, ((v)>>8) & 0xFF, ((v)>>16) & 0xFF, ((v)>>24) & 0xFF)
   #define PUT_LONG(v) PUT_INT(v); PUT_INT((v)>>32)

   CallbackBlock *block;
   uint8_t *p, *end;
   intptr_t target;
   int cnt;
#ifdef _WIN32
   DWORD old_protect;
#endif
#ifdef DEF_ARCH_ARM32
   int cnt2;
#endif

   block = calloc(1, sizeof(CallbackBlock));
   if (!block) return NULL;
   block->heap = heap;

#ifdef _WIN32
   block->ptr = VirtualAlloc(NULL, 4096, MEM_COMMIT, PAGE_READWRITE);
#else
   block->ptr = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, 0, 0);
#endif
   if (!block->ptr) {
      free(block);
      return NULL;
   }

   p = block->ptr;
   end = p + 4096;

#if defined(DEF_ARCH_X86)
   PUT2(0x89,0xE2);                // movl %esp, %edx
   PUT3(0x83,0xEC,0x10);           // subl $16, %esp
   PUT1(0x52);                     // pushl %edx
   PUT1(0x50);                     // pushl %eax
   PUT1(0x68);                     // pushl <block>
   PUT_INT((intptr_t)block);
   PUT3(0x83,0xEA,0x10);           // subl $16, %edx
   PUT1(0x52);                     // pushl %edx
   target = (intptr_t)callback - (intptr_t)p - 5;
   PUT1(0xE8);                     // call <callback>
   PUT_INT(target);
   PUT5(0x83,0x7C,0x24,0x10,0x00); // cmpl $0, 16(%esp)
   PUT2(0x74,0x11);                // jz skip
   PUT5(0x83,0x7C,0x24,0x10,0x01); // cmpl $1, 16(%esp)
   PUT2(0x75,0x06);                // jne double
   PUT4(0xD9,0x44,0x24,0x18);      // flds 24(%esp)
   PUT2(0xEB,0x04);                // jmp skip
   // double:
   PUT4(0xDD,0x44,0x24,0x18);      // fldl 24(%esp)
   // skip:
   PUT4(0x03,0x64,0x24,0x14);      // addl 20(%esp), %esp
   PUT3(0x83,0xC4,0x20);           // addl $32, %esp
   PUT1(0xC3);                     // ret
#elif defined(DEF_ARCH_X86_64)
   PUT3(0x49,0x89,0xE2);           // movq %rsp, %r10
   PUT4(0x48,0x83,0xEC,0x18);      // subq $24, %rsp
   PUT2(0x41,0x52);                // pushq %r10
   PUT1(0x50);                     // pushq %rax
   PUT2(0x49,0xBB);                // movq <block>, %r11
   PUT_LONG((intptr_t)block);
   PUT2(0x41,0x53);                // pushq %r11
   PUT4(0x49,0x83,0xEA,0x18);      // subq $24, %r10
   PUT2(0x41,0x52);                // pushq %r10
   PUT5(0x66,0x48,0x0F,0x7E,0xF8); // movq %xmm7, %rax
   PUT1(0x50);                     // pushq %rax
   PUT5(0x66,0x48,0x0F,0x7E,0xF0); // movq %xmm6, %rax
   PUT1(0x50);                     // pushq %rax
   PUT5(0x66,0x48,0x0F,0x7E,0xE8); // movq %xmm5, %rax
   PUT1(0x50);                     // pushq %rax
   PUT5(0x66,0x48,0x0F,0x7E,0xE0); // movq %xmm4, %rax
   PUT1(0x50);                     // pushq %rax
   PUT5(0x66,0x48,0x0F,0x7E,0xD8); // movq %xmm3, %rax
   PUT1(0x50);                     // pushq %rax
   PUT5(0x66,0x48,0x0F,0x7E,0xD0); // movq %xmm2, %rax
   PUT1(0x50);                     // pushq %rax
   PUT5(0x66,0x48,0x0F,0x7E,0xC8); // movq %xmm1, %rax
   PUT1(0x50);                     // pushq %rax
   PUT5(0x66,0x48,0x0F,0x7E,0xC0); // movq %xmm0, %rax
   PUT1(0x50);                     // pushq %rax
   PUT2(0x48,0xB8);                // movq <callback>, %rax
   PUT_LONG((intptr_t)callback);
   PUT2(0xFF,0xD0);                // call *%rax
   PUT4(0xF3,0x0F,0x7E,0x44);      // movq 96(%rsp), %xmm0
   PUT2(0x24,0x60);
   PUT4(0x48,0x83,0xC4,0x78);      // addq $120, %rsp
   PUT1(0xC3);                     // ret
#elif defined(DEF_ARCH_WIN64)
   PUT3(0x49,0x89,0xE2);           // movq %rsp, %r10
   PUT4(0x48,0x83,0xEC,0x18);      // subq $24, %rsp
   PUT2(0x41,0x52);                // pushq %r10
   PUT1(0x50);                     // pushq %rax
   PUT2(0x49,0xBB);                // movq <block>, %r11
   PUT_LONG((intptr_t)block);
   PUT2(0x41,0x53);                // pushq %r11
   PUT4(0x49,0x83,0xEA,0x18);      // subq $24, %r10
   PUT2(0x41,0x52);                // pushq %r10
   PUT5(0x66,0x48,0x0F,0x7E,0xD8); // movq %xmm3, %rax
   PUT1(0x50);                     // pushq %rax
   PUT5(0x66,0x48,0x0F,0x7E,0xD0); // movq %xmm2, %rax
   PUT1(0x50);                     // pushq %rax
   PUT5(0x66,0x48,0x0F,0x7E,0xC8); // movq %xmm1, %rax
   PUT1(0x50);                     // pushq %rax
   PUT5(0x66,0x48,0x0F,0x7E,0xC0); // movq %xmm0, %rax
   PUT1(0x50);                     // pushq %rax
   PUT4(0x48,0x83,0xEC,0x20);      // subq $32, %rsp
   PUT2(0x48,0xB8);                // movq <callback>, %rax
   PUT_LONG((intptr_t)callback);
   PUT2(0xFF,0xD0);                // call *%rax
   PUT4(0xF3,0x0F,0x7E,0x44);      // movq 96(%rsp), %xmm0
   PUT2(0x24,0x60);
   PUT4(0x48,0x83,0xC4,0x78);      // addq $120, %rsp
   PUT1(0xC3);                     // ret
#elif defined(DEF_ARCH_ARM32)
   PUT_INT((intptr_t)block);
   PUT_INT((intptr_t)callback);
   PUT4(0x10,0x0B,0x2D,0xED);      // vpush {d0,d1,d2,d3,d4,d5,d6,d7}
   PUT4(0x0F,0x00,0x2D,0xE9);      // push {r0,r1,r2,r3}
   PUT4(0x18,0x00,0x1F,0xE5);      // ldr r0, const_block
   PUT4(0x04,0x10,0xA0,0xE1);      // mov r1, r4
   PUT4(0x0D,0x20,0xA0,0xE1);      // mov r2, sp
   PUT4(0x20,0x30,0x1F,0xE5);      // ldr r3, const_func
   PUT4(0x33,0xFF,0x2F,0xE1);      // blx r3
   PUT4(0x50,0xD0,0x8D,0xE2);      // add sp, #80
   PUT4(0x10,0x0A,0x00,0xEE);      // vmov s0, r0
   PUT4(0x90,0x1A,0x00,0xEE);      // vmov s1, r1
   PUT4(0x10,0x40,0xBD,0xE8);      // pop {r4,lr}
   PUT4(0x1E,0xFF,0x2F,0xE1);      // bx lr
#else
   #error "assembler part not provided for architecture"
#endif

   block->entry_start = (intptr_t)p - (intptr_t)block->ptr;

#if defined(DEF_ARCH_X86)
   block->entry_size = 10;
   cnt = 0;
   while (p <= end-10) {
      PUT1(0xB8); // movl <id>,%eax
      PUT_INT(cnt);
      target = (intptr_t)block->ptr - (intptr_t)p - 5;
      PUT1(0xE9); // jmp <main>
      PUT_INT(target);
      cnt++;
   }
#elif defined(DEF_ARCH_X86_64) || defined(DEF_ARCH_WIN64)
   block->entry_size = 12;
   cnt = 0;
   while (p <= end-12) {
      PUT3(0x48,0xC7,0xC0); // movq <id>,%rax
      PUT_INT(cnt);
      target = (intptr_t)block->ptr - (intptr_t)p - 5;
      PUT1(0xE9); // jmp <main>
      PUT_INT(target);
      cnt++;
   }
#elif defined(DEF_ARCH_ARM32)
   block->entry_size = 12;
   cnt = 0;
   while (p <= end-12) {
      PUT4(0x10,0x40,0x2D,0xE9); // push {r4,lr}
      cnt2 = cnt + (cnt >> 8);
      PUT4(cnt2&0xFF,0x40|((16-(cnt2>>8)*4)&0x0F),0xA0,0xE3); // mov r4, <id>
      target = ((((intptr_t)block->ptr - (intptr_t)p) >> 2)) - 2 + 2*1;
      PUT4(target&0xFF,(target>>8)&0xFF,(target>>16)&0xFF,0xEA); // b main
      cnt++;
   }
#else
   #error "assembler part not provided for architecture"
#endif
   #if 0
   {
      FILE *f;
      f = fopen("__block.bin", "wb");
      fwrite(block->ptr, 4096, 1, f);
      fclose(f);
   }
   #endif
   if (cnt != NUM_CALLBACKS) {
      fprintf(stderr, "invalid number of callbacks (%d)\n", cnt);
      abort();
   }

#ifdef _WIN32
   VirtualProtect(block->ptr, 4096, PAGE_EXECUTE_READ, &old_protect);
#else
   if (mprotect(block->ptr, 4096, PROT_READ | PROT_EXEC) != 0) {
      munmap(block->ptr, 4096);
      free(block);
      return NULL;
   }
#endif
   return block;

   #undef PUT1
   #undef PUT2
   #undef PUT3
   #undef PUT4
   #undef PUT5
   #undef PUT_INT
}


static void free_callback_blocks(void *data)
{
   CallbackBlock *block = data, *next;
   int i;

   if (block->ignore_dealloc) return;

   while (block) {
      next = block->next;
#ifdef _WIN32
      VirtualFree(block->ptr, 0, MEM_RELEASE);
#else
      munmap(block->ptr, 4096);
#endif
      for (i=0; i<NUM_CALLBACKS; i++) {
         free(block->callbacks[i].sig);
      }
      free(block);
      block = next;
   }
}

#endif /* __wasm__ */


static Value callback_create(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#ifdef __wasm__
   *error = fixscript_create_error_string(heap, "not supported on this platform");
   return fixscript_int(0);
#else
   CallbackBlock *block, *new_block;
   Callback *cb;
   char *sig;
   int err;
   
   block = fixscript_get_heap_data(heap, callback_block_key);
   if (!block || block->cnt == NUM_CALLBACKS) {
      new_block = alloc_callback_block(heap);
      if (!new_block) {
         *error = fixscript_create_error_string(heap, "can't create callback block");
         return fixscript_int(0);
      }
      new_block->next = block;
      if (block) {
         block->ignore_dealloc = 1;
      }
      fixscript_set_heap_data(heap, callback_block_key, new_block, free_callback_blocks);
      if (block) {
         block->ignore_dealloc = 0;
      }
      block = new_block;
   }

   err = fixscript_get_string(heap, params[0], 0, -1, &sig, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   cb = &block->callbacks[block->cnt];
   cb->sig = sig;
   cb->func = params[1];
   cb->data = params[2];
   fixscript_ref(heap, cb->data);

   return create_pointer(heap, block->ptr + block->entry_start + (block->cnt++)*block->entry_size);
#endif
}


#ifdef WASM_BROWSER

typedef struct JSCont {
   int used;
   Heap *heap;
   ContinuationResultFunc func;
   void *data;
} JSCont;

static Heap *js_cur_heap = NULL;
static JSCont *js_conts = NULL;
static int js_conts_cap = 0;
static int js_suspended = 0;

static Value native_js_create(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   char *src, *msg;
   int err, len=0, ret;

   err = fixscript_get_string(heap, params[0], 0, -1, &src, &len);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   ret = js_create(src, len);
   free(src);
   if (ret < 0) {
      msg = (char *)(intptr_t)(-ret);
      *error = fixscript_create_error_string(heap, msg);
      free(msg);
      return fixscript_int(0);
   }
   return fixscript_int(ret);
}

static Value native_js_call(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Heap *prev_js_cur_heap;
   int prev_js_suspended;
   char *msg;
   int ret;
   
   prev_js_cur_heap = js_cur_heap;
   prev_js_suspended = js_suspended;
   js_cur_heap = heap;
   js_suspended = 0;
   ret = js_call(params[0].value, get_pointer(heap, params[1], NULL));
   js_cur_heap = prev_js_cur_heap;
   js_suspended = prev_js_suspended;
   if (ret < 0) {
      msg = (char *)(intptr_t)(-ret);
      *error = fixscript_create_error_string(heap, msg);
      free(msg);
      return fixscript_int(0);
   }
   return fixscript_int(0);
}

int native_js_suspend() EXPORT("js_suspend")
{
   JSCont *new_conts;
   int i, idx=-1, new_cap;

   if (!js_cur_heap || !fixscript_in_async_call(js_cur_heap)) {
      return 0;
   }

   if (js_suspended) {
      return 0;
   }

   for (i=0; i<js_conts_cap; i++) {
      if (!js_conts[i].used) {
         idx = i;
         break;
      }
   }
   if (idx == -1) {
      if (js_conts_cap > INT_MAX/sizeof(JSCont)/2) {
         return 0;
      }
      new_cap = js_conts_cap == 0? 4 : js_conts_cap*2;
      new_conts = realloc(js_conts, new_cap * sizeof(JSCont));
      if (!new_conts) {
         return 0;
      }
      idx = js_conts_cap;
      for (i=js_conts_cap; i<new_cap; i++) {
         new_conts[i].used = 0;
      }
      js_conts = new_conts;
      js_conts_cap = new_cap;
   }

   js_conts[idx].used = 1;
   js_conts[idx].heap = js_cur_heap;
   fixscript_suspend(js_cur_heap, &js_conts[idx].func, &js_conts[idx].data);
   js_suspended = 1;
   return idx+1;
}

int native_js_resume(int idx) EXPORT("js_resume")
{
   idx--;
   if (idx < 0 || idx >= js_conts_cap) {
      return 0;
   }
   if (!js_conts[idx].used) {
      return 0;
   }
   js_conts[idx].used = 0;
   js_conts[idx].func(js_conts[idx].heap, fixscript_int(0), fixscript_int(0), js_conts[idx].data);
   return 1;
}

#endif /* WASM_BROWSER */


void fixnative_register_functions(Heap *heap)
{
   char buf[32];
   int i;

   fixscript_register_handle_types(&handles_offset, NUM_HANDLE_TYPES);
   fixscript_register_heap_key(&callback_block_key);

   fixscript_register_native_func(heap, "system_get_type#0", system_get, (void *)SYSTEM_GET_TYPE);
   fixscript_register_native_func(heap, "system_get_arch#0", system_get, (void *)SYSTEM_GET_ARCH);
   fixscript_register_native_func(heap, "system_get_pointer_size#0", system_get, (void *)SYSTEM_GET_POINTER_SIZE);
   fixscript_register_native_func(heap, "system_get_nlong_size#0", system_get, (void *)SYSTEM_GET_NLONG_SIZE);
   fixscript_register_native_func(heap, "system_get_pointer_align#0", system_get, (void *)SYSTEM_GET_POINTER_ALIGN);
   fixscript_register_native_func(heap, "system_get_long_align#0", system_get, (void *)SYSTEM_GET_LONG_ALIGN);
   fixscript_register_native_func(heap, "system_get_nlong_align#0", system_get, (void *)SYSTEM_GET_NLONG_ALIGN);
   fixscript_register_native_func(heap, "system_get_double_align#0", system_get, (void *)SYSTEM_GET_DOUBLE_ALIGN);
   fixscript_register_native_func(heap, "pointer_create#2", pointer_create, NULL);
   fixscript_register_native_func(heap, "pointer_from_shared_array#1", pointer_from_shared_array, NULL);
   fixscript_register_native_func(heap, "pointer_get_value#1", pointer_get_value, NULL);
   fixscript_register_native_func(heap, "pointer_with_offset#2", pointer_with_offset, NULL);
   fixscript_register_native_func(heap, "memory_create#4", memory_create, NULL);
   fixscript_register_native_func(heap, "memory_from_shared_array#1", memory_from_shared_array, NULL);
   fixscript_register_native_func(heap, "memory_from_shared_array#3", memory_from_shared_array, NULL);
   fixscript_register_native_func(heap, "memory_alloc#1", memory_alloc, (void *)0);
   fixscript_register_native_func(heap, "memory_alloc_fast#1", memory_alloc, (void *)1);
   fixscript_register_native_func(heap, "memory_realloc#2", memory_realloc, NULL);
   fixscript_register_native_func(heap, "memory_free#1", memory_free, NULL);
   fixscript_register_native_func(heap, "memory_get_size#1", memory_get_size, NULL);
   fixscript_register_native_func(heap, "memory_is_owned#1", memory_is_owned, NULL);
   fixscript_register_native_func(heap, "pointer_get_view#4", pointer_get_view, NULL);
   fixscript_register_native_func(heap, "pointer_set_byte#3", pointer_memory_op, (void *)MEM_SET_BYTE);
   fixscript_register_native_func(heap, "pointer_set_short#3", pointer_memory_op, (void *)MEM_SET_SHORT);
   fixscript_register_native_func(heap, "pointer_set_int#3", pointer_memory_op, (void *)MEM_SET_INT);
   fixscript_register_native_func(heap, "pointer_set_long#4", pointer_memory_op, (void *)MEM_SET_LONG);
   fixscript_register_native_func(heap, "pointer_set_nlong#4", pointer_memory_op, (void *)MEM_SET_NLONG);
   fixscript_register_native_func(heap, "pointer_set_float#3", pointer_memory_op, (void *)MEM_SET_FLOAT);
   fixscript_register_native_func(heap, "pointer_set_double#4", pointer_memory_op, (void *)MEM_SET_DOUBLE);
   fixscript_register_native_func(heap, "pointer_set_pointer#3", pointer_memory_op, (void *)MEM_SET_POINTER);
   fixscript_register_native_func(heap, "pointer_get_byte#2", pointer_memory_op, (void *)MEM_GET_BYTE);
   fixscript_register_native_func(heap, "pointer_get_short#2", pointer_memory_op, (void *)MEM_GET_SHORT);
   fixscript_register_native_func(heap, "pointer_get_int#2", pointer_memory_op, (void *)MEM_GET_INT);
   fixscript_register_native_func(heap, "pointer_get_long#2", pointer_memory_op, (void *)MEM_GET_LONG);
   fixscript_register_native_func(heap, "pointer_get_nlong#2", pointer_memory_op, (void *)MEM_GET_NLONG);
   fixscript_register_native_func(heap, "pointer_get_float#2", pointer_memory_op, (void *)MEM_GET_FLOAT);
   fixscript_register_native_func(heap, "pointer_get_double#2", pointer_memory_op, (void *)MEM_GET_DOUBLE);
   fixscript_register_native_func(heap, "pointer_get_pointer#2", pointer_memory_op, (void *)MEM_GET_POINTER);
   fixscript_register_native_func(heap, "pointer_set_bytes#3", pointer_set_get_bytes, (void *)0);
   fixscript_register_native_func(heap, "pointer_set_bytes#5", pointer_set_get_bytes, (void *)0);
   fixscript_register_native_func(heap, "pointer_get_bytes#3", pointer_set_get_bytes, (void *)1);
   fixscript_register_native_func(heap, "pointer_get_bytes#5", pointer_set_get_bytes, (void *)1);
   fixscript_register_native_func(heap, "pointer_find_byte#3", pointer_find, (void *)FIND_BYTE);
   fixscript_register_native_func(heap, "pointer_find_byte#4", pointer_find, (void *)FIND_BYTE);
   fixscript_register_native_func(heap, "pointer_find_short#3", pointer_find, (void *)FIND_SHORT);
   fixscript_register_native_func(heap, "pointer_find_short#4", pointer_find, (void *)FIND_SHORT);
   fixscript_register_native_func(heap, "pointer_find_int#3", pointer_find, (void *)FIND_INT);
   fixscript_register_native_func(heap, "pointer_find_int#4", pointer_find, (void *)FIND_INT);
   fixscript_register_native_func(heap, "native_open_library#1", native_open_library, NULL);
   fixscript_register_native_func(heap, "native_close_library#1", native_close_library, NULL);
   fixscript_register_native_func(heap, "native_get_symbol#2", native_get_symbol, NULL);
   fixscript_register_native_func(heap, "destructor_create#2", destructor_create, NULL);
   fixscript_register_native_func(heap, "destructor_disarm#1", destructor_disarm, NULL);
   fixscript_register_native_func(heap, "callback_create#3", callback_create, NULL);
 
   for (i=0; i<=16; i++) {
      snprintf(buf, sizeof(buf), "native_call#%d", 2+i);
      fixscript_register_native_func(heap, buf, native_call_func, (void *)0);
   }
   fixscript_register_native_func(heap, "native_call_args#3", native_call_func, (void *)1);

   #ifdef WASM_BROWSER
      fixscript_register_native_func(heap, "native_js_create#1", native_js_create, NULL);
      fixscript_register_native_func(heap, "native_js_call#2", native_js_call, NULL);
   #endif
}
