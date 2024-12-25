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

#ifdef FIXSCRIPT_ASYNC
   #undef FIXSCRIPT_NO_JIT
   #define FIXSCRIPT_NO_JIT
#endif

#ifndef FIXSCRIPT_NO_JIT
   #define JIT_RUN_CODE
   #if defined(__i386__) || defined(_M_IX86)
      #define JIT_X86
   #elif defined(__LP64__) || defined(_WIN64)
      #define JIT_X86
      #define JIT_X86_64
      #ifdef _WIN64
         #define JIT_WIN64
      #endif
   #else
      #define FIXSCRIPT_NO_JIT
      #undef JIT_RUN_CODE
   #endif
#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#if defined(_WIN32)
#define UNICODE
#define _UNICODE
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach_time.h>
#include <xlocale.h>
#else
#include <time.h>
#include <locale.h>
#endif
#ifndef FIXSCRIPT_NO_JIT
#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#endif
#endif
#include "fixscript.h"

#ifdef _WIN32
#undef ERROR
#endif

#define MAX_IMPORT_RECURSION    100
#define DEFAULT_MAX_STACK_SIZE  8192 // 40KB (native JIT stack up to 256KB)
#define MAX_IMMEDIATE_STACK     256
#define MAX_COMPARE_RECURSION   50
#define MAX_DUMP_RECURSION      50
#define ARRAYS_GROW_CUTOFF      4096
#define MARK_RECURSION_CUTOFF   1000
#define CLONE_RECURSION_CUTOFF  200
#define FUNC_REF_OFFSET         ((1<<23)-256*1024)

#define PARAMS_ON_STACK 16

#define MIN(a, b) ((a)<(b)? (a):(b))
#define MAX(a, b) ((a)>(b)? (a):(b))
#define OFFSETOF(type, field) ((char *)&((type *)NULL)->field - (char *)(type *)NULL)

typedef struct {
   void **data;
   int size, len, slots;
} StringHash;

typedef struct {
   void **data;
   int size, len;
} DynArray;

typedef struct {
   char *data;
   int size, len;
} String;

typedef struct {
   union {
      int *flags;
      union {
         HandleFreeFunc handle_free;
         HandleFunc handle_func;
      };
   };
   union {
      int *data;
      unsigned char *byte_data;
      unsigned short *short_data;
      void *handle_ptr;
   };
   int size, len;
   union {
      int hash_slots;
      int type;
   };
   unsigned int ext_refcnt : 24;
   unsigned int is_string : 1;
   unsigned int is_handle : 2;
   unsigned int is_static : 1;
   unsigned int is_const : 1;
   unsigned int is_shared : 1;
   unsigned int has_weak_refs : 1;
   unsigned int is_protected : 1;
} Array;

struct SharedArrayHandle {
   volatile unsigned int refcnt;
   int type;
   void *ptr;
   int len, elem_size;
   HandleFreeFunc free_func;
   void *free_data;
};

typedef struct {
   int pc;
   int line;
} LineEntry;

typedef struct {
   int *data;
   int size, len, slots;
} ConstStringSet;

#ifdef FIXSCRIPT_ASYNC
typedef struct {
   int continue_pc;
   int set_stack_len;
   int stack_overflow;
   int stack_base;
   int error_stack_base;
   ContinuationResultFunc cont_func;
   void *cont_data;
   Heap *auto_suspend_heap;
} ResumeContinuation;
#endif

#ifndef FIXSCRIPT_NO_JIT
typedef struct StackBlock {
   void **ret;
   void **fp;
   struct StackBlock *next;
} StackBlock;
#endif

struct Heap {
   Array *data;
   int *reachable;
   int size;
   int next_idx;
   int64_t total_size, total_cap;

   int max_stack_size;
   int stack_len, stack_cap;
   int *stack_data;
   char *stack_flags;

   int locals_len, locals_cap;
   int *locals_data;
   char *locals_flags;

#ifndef FIXSCRIPT_NO_JIT
   void *jit_error_sp;  // native stack
   int jit_error_pc;    // jit pc relative to jit_code
   int jit_error_stack; // stack base for function
   int jit_error_base;  // stack base for return values

   uint8_t *jit_array_get_funcs;
   uint8_t *jit_array_set_funcs;
   uint8_t *jit_array_append_funcs;
#endif

   DynArray roots;
   DynArray ext_roots;
   int marking_limit;
   int collecting;

   unsigned char *bytecode;
   int bytecode_size;

   LineEntry *lines;
   int lines_size;

   StringHash scripts;
   int cur_import_recursion;

   DynArray functions;

   DynArray native_functions;
   StringHash native_functions_hash;

   DynArray error_stack;

   uint64_t perf_start_time;
   uint64_t perf_last_time;

   int handle_created;

   LoadScriptFunc cur_load_func;
   void *cur_load_data;
   void *cur_parser;
   DynArray *cur_postprocess_funcs;

   StringHash weak_refs;
   uint64_t weak_id_cnt;

   StringHash shared_arrays;
   DynArray user_data;

   uint64_t time_limit;
   int time_counter;
   volatile int stop_execution;

   char *compiler_error;
   int reload_counter;
   int compile_counter;

   ConstStringSet const_string_set;

#ifdef FIXEMBED_TOKEN_DUMP
   int token_dump_mode;
   Heap *token_heap;
   Heap *script_heap;
#endif

#ifdef FIXSCRIPT_ASYNC
   DynArray async_continuations;
   int async_ret;
   Value async_ret_result;
   Value async_ret_error;
   uint32_t instruction_counter, instruction_limit;
   int auto_suspend_num_instructions;
   ContinuationSuspendFunc auto_suspend_func;
   void *auto_suspend_data;
   int async_active;
   int allow_sync_call;
#endif

#ifndef FIXSCRIPT_NO_JIT
   unsigned char *jit_code;
   int jit_code_len, jit_code_cap;
   int jit_exec;
   int jit_entry_func_end;
#ifdef JIT_X86_64
   int jit_reinit_regs_func;
#endif
   int jit_error_code;
   int jit_invalid_array_stack_error_code;
   int jit_out_of_bounds_stack_error_code;
   int jit_invalid_shared_stack_error_code;
   int jit_out_of_memory_stack_error_code;
   int jit_upgrade_code[6];
   DynArray jit_pc_mappings;
   DynArray jit_heap_data_refs;
   DynArray jit_array_get_refs;
   DynArray jit_array_set_refs;
   DynArray jit_array_append_refs;
   DynArray jit_length_refs;
   DynArray jit_adjustments;
   int jit_array_get_func_base;
   uint8_t jit_array_get_byte_func;
   uint8_t jit_array_get_short_func;
   uint8_t jit_array_get_int_func;
   int jit_array_set_func_base;
   uint8_t jit_array_set_const_string;
   uint8_t jit_array_set_byte_func[2];
   uint8_t jit_array_set_short_func[2];
   uint8_t jit_array_set_int_func[2];
   uint8_t jit_shared_set_byte_func[2];
   uint8_t jit_shared_set_short_func[2];
   uint8_t jit_shared_set_int_func[2];
   int jit_array_append_func_base;
   uint8_t jit_array_append_const_string;
   uint8_t jit_array_append_shared;
   uint8_t jit_array_append_byte_func[2];
   uint8_t jit_array_append_short_func[2];
   uint8_t jit_array_append_int_func[2];
   StackBlock *jit_stack_block;
#endif
};

#define EXT_REFCNT_LIMIT ((1<<24)-1)
#define SAH_REFCNT_LIMIT ((1<<30)-1)

enum {
   ARR_HASH  = 0,
   ARR_INT   = -1,    /* 0x00000000 - 1 */
   ARR_BYTE  = -257,  /* 0xFFFFFF00 - 1 */
   ARR_SHORT = -65537 /* 0xFFFF0000 - 1 */
};

#define ARRAY_NEEDS_UPGRADE(arr, value) ((value) & (((unsigned int)(arr)->type) + 1U))
#define ARRAY_SHARED_HEADER(arr) ((SharedArrayHandle *)(((char *)(arr)->flags) - sizeof(SharedArrayHandle)))

enum {
   SER_ZERO         = 0,
   SER_BYTE         = 1,
   SER_SHORT        = 2,
   SER_INT          = 3,
   SER_FLOAT        = 4,
   SER_FLOAT_ZERO   = 5,
   SER_REF          = 6,
   SER_REF_SHORT    = 7,
   SER_ARRAY        = 8,
   SER_ARRAY_BYTE   = 9,
   SER_ARRAY_SHORT  = 10,
   SER_ARRAY_INT    = 11,
   SER_STRING_BYTE  = 12,
   SER_STRING_SHORT = 13,
   SER_STRING_INT   = 14,
   SER_HASH         = 15
};

typedef struct Constant {
   Value value;
   int local;
   Script *ref_script;
   struct Constant *ref_constant;
   int idx;
} Constant;

typedef struct {
   int id;
   int addr;
   int num_params;
   int local;
   Script *script;
   int lines_start, lines_end;
   int max_stack;
#ifndef FIXSCRIPT_NO_JIT
   int jit_addr;
#endif
} Function;

typedef struct {
   NativeFunc func;
   void *data;
   int id;
   int num_params;
   int bytecode_ident_pc;
} NativeFunction;

struct Script {
   DynArray imports;
   StringHash constants;
   StringHash locals;
   StringHash functions;
   struct Script *old_script;
};

enum {
   TOK_IDENT,
   TOK_FUNC_REF,
   TOK_NUMBER,
   TOK_HEX_NUMBER,
   TOK_FLOAT_NUMBER,
   TOK_CHAR,
   TOK_STRING,
   TOK_UNKNOWN,

   KW_DO,
   KW_IF,
   KW_FOR,
   KW_USE,
   KW_VAR,
   KW_CASE,
   KW_ELSE,
   KW_BREAK,
   KW_CONST,
   KW_WHILE,
   KW_IMPORT,
   KW_RETURN,
   KW_SWITCH,
   KW_DEFAULT,
   KW_CONTINUE,
   KW_FUNCTION
};

typedef struct {
   const char *cur, *start;
   int line;
   int type;
   const char *value;
   int len;
   int num_chars, num_utf8_bytes, max_num_value;
   const char *error;
   int again;
   const char *tokens_src;
   Value *cur_token, *tokens_end;
   int ignore_errors;
} Tokenizer;

enum {
   TOK_type,
   TOK_off,
   TOK_len,
   TOK_line,
   TOK_SIZE
};

typedef struct {
   Tokenizer tok;
   unsigned char *buf;
   int buf_size, buf_len;
   int last_buf_pos;
   DynArray lines;
   Heap *heap;
   Script *script;
   int stack_pos, max_stack;
   StringHash variables;
   int has_vars;
   int long_jumps, long_func_refs;
   StringHash const_strings;
   StringHash import_aliases;
   LoadScriptFunc load_func;
   void *load_data;
   const char *fname;
   int use_fast_error;
   int max_immediate_stack;

   int has_break, has_continue;
   int continue_pc;
   int break_stack_pos, continue_stack_pos;
   DynArray break_jumps;
   DynArray continue_jumps;

   DynArray func_refs;
   
   char *tokens_src;
   Value *tokens_arr;
   Value *tokens_end;
   Value tokens_src_val;
   Value tokens_arr_val;
   int semicolon_removed;

   Script *old_script;
} Parser;

typedef struct {
   int has_break, has_continue;
   int continue_pc;
   int break_stack_pos, continue_stack_pos;
   int break_jumps_len;
   int continue_jumps_len;
} LoopState;

typedef struct {
   int used;
   int functions_len;
   int locals_len;
} ScriptState;
 
typedef struct {
   char *script_name;
   char *func_name;
} FuncRefHandle;

typedef struct WeakRefHandle {
   uint64_t id;
   int target;
   int value;
   int container;
   Value key;
   struct WeakRefHandle *next;
} WeakRefHandle;

#ifdef _WIN32
#define CopyContext fixscript_CopyContext
#endif

typedef struct {
   Heap *dest, *src;
   Value map;
   int err;
   LoadScriptFunc load_func;
   void *load_data;
   Value *error;
   DynArray *queue;
   int recursion_limit;
} CopyContext;

enum {
   ET_HASH,
   ET_STRING,
   ET_FLOAT,
   ET_BLOCK
};

enum {
   BT_NORMAL,
   BT_FOR,
   BT_EXPR
};

enum {
   BC_POP,
   BC_POPN,
   BC_LOADN,
   BC_STOREN,
   BC_ADD,
   BC_SUB,
   BC_MUL,
   BC_ADD_MOD,
   BC_SUB_MOD,
   BC_MUL_MOD,
   BC_DIV,
   BC_REM,
   BC_SHL,
   BC_SHR,
   BC_USHR,
   BC_AND,
   BC_OR,
   BC_XOR,
   BC_LT,
   BC_LE,
   BC_GT,
   BC_GE,
   BC_EQ,
   BC_NE,
   BC_EQ_VALUE,
   BC_NE_VALUE,
   BC_BITNOT,
   BC_LOGNOT,
   BC_INC,
   BC_DEC,
   BC_FLOAT_ADD,
   BC_FLOAT_SUB,
   BC_FLOAT_MUL,
   BC_FLOAT_DIV,
   BC_FLOAT_LT,
   BC_FLOAT_LE,
   BC_FLOAT_GT,
   BC_FLOAT_GE,
   BC_FLOAT_EQ,
   BC_FLOAT_NE,
   BC_RETURN,
   BC_RETURN2,
   BC_CALL_DIRECT,
   BC_CALL_DYNAMIC,
   BC_CALL_NATIVE,
   BC_CALL2_DIRECT,
   BC_CALL2_DYNAMIC,
   BC_CALL2_NATIVE,
   BC_CLEAN_CALL2,
   BC_CREATE_ARRAY,
   BC_CREATE_HASH,
   BC_ARRAY_GET,
   BC_ARRAY_SET,
   BC_ARRAY_APPEND,
   BC_HASH_GET,
   BC_HASH_SET,

   BC_CONST_P8      = 0x38,
   BC_CONST_N8      = 0x39,
   BC_CONST_P16     = 0x3A,
   BC_CONST_N16     = 0x3B,
   BC_CONST_I32     = 0x3C,
   BC_CONST_F32     = 0x3D,
   BC_CONSTM1       = 0x3E,
   BC_CONST0        = 0x3F,
   BC_BRANCH0       = 0x60,
   BC_JUMP0         = 0x68,
   BC_BRANCH_LONG   = 0x70,
   BC_JUMP_LONG     = 0x71,
   BC_LOOP_I8       = 0x72,
   BC_LOOP_I16      = 0x73,
   BC_LOOP_I32      = 0x74,
   BC_LOAD_LOCAL    = 0x75,
   BC_STORE_LOCAL   = 0x76,
   BC_SWITCH        = 0x77,
   BC_LENGTH        = 0x78,
   BC_CONST_STRING  = 0x79,
   BC_STRING_CONCAT = 0x7A,
   BC_CHECK_STACK   = 0x7C,
   BC_EXTENDED      = 0x7D,
   BC_CONST63       = 0x7E,
   BC_CONST64       = 0x7F,
   BC_STOREM64      = 0x80,
   BC_LOADM64       = 0xC0
};

enum {
   BC_EXT_MIN,
   BC_EXT_MAX,
   BC_EXT_CLAMP,
   BC_EXT_ABS,
   BC_EXT_ADD32,
   BC_EXT_SUB32,
   BC_EXT_ADD64,
   BC_EXT_SUB64,
   BC_EXT_MUL64,
   BC_EXT_UMUL64,
   BC_EXT_MUL64_LONG,
   BC_EXT_DIV64,
   BC_EXT_UDIV64,
   BC_EXT_REM64,
   BC_EXT_UREM64,
   BC_EXT_FLOAT,
   BC_EXT_INT,
   BC_EXT_FABS,
   BC_EXT_FMIN,
   BC_EXT_FMAX,
   BC_EXT_FCLAMP,
   BC_EXT_FLOOR,
   BC_EXT_CEIL,
   BC_EXT_ROUND,
   BC_EXT_POW,
   BC_EXT_SQRT,
   BC_EXT_CBRT,
   BC_EXT_EXP,
   BC_EXT_LN,
   BC_EXT_LOG2,
   BC_EXT_LOG10,
   BC_EXT_SIN,
   BC_EXT_COS,
   BC_EXT_ASIN,
   BC_EXT_ACOS,
   BC_EXT_TAN,
   BC_EXT_ATAN,
   BC_EXT_ATAN2,
   BC_EXT_DBL_FLOAT,
   BC_EXT_DBL_INT,
   BC_EXT_DBL_CONV_DOWN,
   BC_EXT_DBL_CONV_UP,
   BC_EXT_DBL_ADD,
   BC_EXT_DBL_SUB,
   BC_EXT_DBL_MUL,
   BC_EXT_DBL_DIV,
   BC_EXT_DBL_CMP_LT,
   BC_EXT_DBL_CMP_LE,
   BC_EXT_DBL_CMP_GT,
   BC_EXT_DBL_CMP_GE,
   BC_EXT_DBL_CMP_EQ,
   BC_EXT_DBL_CMP_NE,
   BC_EXT_DBL_FABS,
   BC_EXT_DBL_FMIN,
   BC_EXT_DBL_FMAX,
   BC_EXT_DBL_FCLAMP,
   BC_EXT_DBL_FCLAMP_SHORT,
   BC_EXT_DBL_FLOOR,
   BC_EXT_DBL_CEIL,
   BC_EXT_DBL_ROUND,
   BC_EXT_DBL_POW,
   BC_EXT_DBL_SQRT,
   BC_EXT_DBL_CBRT,
   BC_EXT_DBL_EXP,
   BC_EXT_DBL_LN,
   BC_EXT_DBL_LOG2,
   BC_EXT_DBL_LOG10,
   BC_EXT_DBL_SIN,
   BC_EXT_DBL_COS,
   BC_EXT_DBL_ASIN,
   BC_EXT_DBL_ACOS,
   BC_EXT_DBL_TAN,
   BC_EXT_DBL_ATAN,
   BC_EXT_DBL_ATAN2,
   BC_EXT_IS_INT,
   BC_EXT_IS_FLOAT,
   BC_EXT_IS_ARRAY,
   BC_EXT_IS_STRING,
   BC_EXT_IS_HASH,
   BC_EXT_IS_SHARED,
   BC_EXT_IS_CONST,
   BC_EXT_IS_FUNCREF,
   BC_EXT_IS_WEAKREF,
   BC_EXT_IS_HANDLE,
   BC_EXT_CHECK_TIME_LIMIT
};
   
static const Constant zero_const = { { 0, 0 }, 1 };
static const Constant one_const = { { 1, 0 }, 1 };

#define FUNC_REF_HANDLE_TYPE INT_MAX
#define WEAK_REF_HANDLE_TYPE (INT_MAX-1)
#define CLEANUP_HANDLE_TYPE  (INT_MAX-2)
static volatile int native_handles_alloc_cnt = INT_MAX-2;
static volatile int heap_keys_cnt = 0;

#define ASSUME(name,expr) typedef char assume_##name[(expr)? 1 : -1]

ASSUME(short_is_2_bytes, sizeof(short) == 2);
ASSUME(int_is_4_bytes, sizeof(int) == 4);

#define FLAGS_SIZE(size) ((int)(((uint32_t)(size)+31U) >> 5))
#define FLAGS_IDX(idx) ((idx) >> 5)
#define FLAGS_ARR(arr, idx) (arr)->flags[FLAGS_IDX(idx)]
#define FLAGS_BIT(idx) (1 << ((idx) & 31))
#define IS_ARRAY(arr, idx) (FLAGS_ARR(arr, idx) & FLAGS_BIT(idx))
#define SET_IS_ARRAY(arr, idx) FLAGS_ARR(arr, idx) |= FLAGS_BIT(idx)
#define CLEAR_IS_ARRAY(arr, idx) FLAGS_ARR(arr, idx) &= ~FLAGS_BIT(idx)
#define ASSIGN_IS_ARRAY(arr, idx, value) ((value)? (SET_IS_ARRAY(arr, idx)) : (CLEAR_IS_ARRAY(arr, idx)))

#define HAS_DATA(arr, idx) IS_ARRAY(arr, (1<<(arr)->size) + (idx))
#define SET_HAS_DATA(arr, idx) SET_IS_ARRAY(arr, (1<<(arr)->size) + (idx))
#define CLEAR_HAS_DATA(arr, idx) CLEAR_IS_ARRAY(arr, (1<<(arr)->size) + (idx))

#define SYM2(a, b) ((a) | ((b) << 8))
#define SYM3(a, b, c) ((a) | ((b) << 8) | ((c) << 16))
#define SYM4(a, b, c, d) ((a) | ((b) << 8) | ((c) << 16) | ((d) << 24))


#ifdef FIXEMBED_TOKEN_DUMP
void fixembed_native_function_used(const char *name);
void fixembed_dump_tokens(const char *fname, Tokenizer *tok);
#endif

#ifndef FIXSCRIPT_NO_JIT
static void jit_update_exec(Heap *heap, int exec);
static const char *jit_compile(Heap *heap, int func_start);
static void jit_update_heap_refs(Heap *heap);
#endif

#if !defined(_WIN32) && !defined(__SYMBIAN32__)
float fminf(float x, float y);
float fmaxf(float x, float y);
float roundf(float x);
float log2f(float x);

double fmin(double x, double y);
double fmax(double x, double y);
double round(double x);
double log2(double x);
#endif

#if defined(FIXBUILD_BINCOMPAT) && defined(__linux__)
   #if defined(__i386__)
      asm(".symver expf,expf@GLIBC_2.0");
      asm(".symver powf,powf@GLIBC_2.0");
      asm(".symver logf,logf@GLIBC_2.0");
      asm(".symver log2f,log2f@GLIBC_2.1");
   #elif defined(__x86_64__)
      asm(".symver expf,expf@GLIBC_2.2.5");
      asm(".symver powf,powf@GLIBC_2.2.5");
      asm(".symver logf,logf@GLIBC_2.2.5");
      asm(".symver log2f,log2f@GLIBC_2.2.5");
      asm(".symver memcpy,memcpy@GLIBC_2.2.5");
   #elif defined(__arm__)
      asm(".symver expf,expf@GLIBC_2.4");
      asm(".symver powf,powf@GLIBC_2.4");
      asm(".symver logf,logf@GLIBC_2.4");
      asm(".symver log2f,log2f@GLIBC_2.4");
   #endif
#endif


#if !defined(_WIN32) && !defined(__SYMBIAN32__) && !defined(__HAIKU__) && !defined(__wasm__)
#define strtod strtod_default_locale
static double strtod_default_locale(const char *nptr, char **endptr)
{
   static volatile locale_t locale = NULL;
   locale_t new_locale, old_locale;

   old_locale = locale;
   if (!old_locale) {
      new_locale = newlocale(LC_ALL_MASK, "C", NULL);
      old_locale = __sync_val_compare_and_swap(&locale, NULL, new_locale);
      if (old_locale) {
         freelocale(new_locale);
      }
      else {
         old_locale = new_locale;
      }
   }

   return strtod_l(nptr, endptr, old_locale);
}
#endif


#if !defined(__GNUC__) && defined(_WIN32)
static inline int __sync_add_and_fetch(volatile int *ptr, int amount)
{
   if (amount == 1) {
      return InterlockedIncrement((volatile LONG *)ptr);
   }
   else {
      return InterlockedExchangeAdd((volatile LONG *)ptr, amount) + amount;
   }
}

static inline int __sync_sub_and_fetch(volatile int *ptr, int amount)
{
   if (amount == 1) {
      return InterlockedDecrement((volatile LONG *)ptr);
   }
   else {
      return InterlockedExchangeAdd((volatile LONG *)ptr, -amount) - amount;
   }
}

static inline int __sync_val_compare_and_swap(volatile int *ptr, int old_value, int new_value)
{
   return InterlockedCompareExchange((volatile LONG *)ptr, new_value, old_value) == old_value;
}
#endif


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

#define __sync_val_compare_and_swap x__sync_val_compare_and_swap
static inline int x__sync_val_compare_and_swap(volatile int *ptr, int old_value, int new_value)
{
   int prev = *ptr;
   if (prev == old_value) {
      *ptr = new_value;
   }
   return prev;
}

float log2f(float x)
{
   return logf(x) / logf(2.0f);
}

double log2(double x)
{
   return log(x) / log(2.0);
}
#endif


#ifdef _WIN32
static int win32_vsnprintf(char *str, size_t size, const char *format, va_list ap)
{
   va_list aq;
   char *s, buf[128];
   int len, cap;

   va_copy(aq, ap);
   len = vsnprintf(str, size, format, aq);
   va_end(aq);

   if (len < 0 && !str && size == 0) {
      va_copy(aq, ap);
      len = vsnprintf(buf, sizeof(buf), format, aq);
      va_end(aq);
      if (len >= 0 && len < sizeof(buf)) {
         return len;
      }
      cap = sizeof(buf);
      while (cap < (1<<30)) {
         cap *= 2;
         s = malloc(cap);
         if (!s) return -1;
         va_copy(aq, ap);
         len = vsnprintf(s, cap, format, aq);
         va_end(aq);
         free(s);
         if (len >= 0 && len < cap) {
            return len;
         }
      }
      return -1;
   }
   return len;
}
#define vsnprintf win32_vsnprintf
#endif


static void *malloc_array(int nmemb, int size)
{
   int64_t mul = ((int64_t)nmemb) * ((int64_t)size);
   if (mul < 0 || mul > INTPTR_MAX) {
      return NULL;
   }
   return malloc((size_t)mul);
}


static void *realloc_array(void *ptr, int nmemb, int size)
{
   int64_t mul = ((int64_t)nmemb) * ((int64_t)size);
   if (mul < 0 || mul > INTPTR_MAX) {
      return NULL;
   }
   return realloc(ptr, (size_t)mul);
}


static char *string_dup(const char *s, size_t n)
{
   char *d;

   d = malloc(n+1);
   if (!d) return NULL;
   memcpy(d, s, n);
   d[n] = '\0';
   return d;
}


static char *string_dup_with_prefix(const char *s, size_t n, const char *prefix, size_t prefix_n)
{
   int64_t len;
   char *d;

   len = (int64_t)prefix_n + (int64_t)n + 1;
   if (len > INT_MAX) return NULL;
   d = malloc(len);
   if (!d) return NULL;
   memcpy(d, prefix, prefix_n);
   memcpy(d+prefix_n, s, n);
   d[prefix_n+n] = '\0';
   return d;
}


static char *string_format(const char *fmt, ...)
{
   va_list ap;
   int len;
   char *s;

   va_start(ap, fmt);
   len = vsnprintf(NULL, 0, fmt, ap);
   va_end(ap);
   if (len < 0) return NULL;
   
   s = malloc(len+1);
   if (!s) return NULL;

   va_start(ap, fmt);
   vsnprintf(s, len+1, fmt, ap);
   va_end(ap);
   return s;
}


static int string_append(String *str, const char *fmt, ...)
{
   va_list ap;
   int len;
   int new_size;
   char *new_data;

   va_start(ap, fmt);
   len = vsnprintf(NULL, 0, fmt, ap);
   va_end(ap);
   if (len < 0) return 0;

   if (str->len + len + 1 > str->size) {
      new_size = (str->size == 0? 16 : str->size);
      while (str->len + len + 1 > new_size) {
         if (new_size >= (1<<30)) return 0;
         new_size <<= 1;
      }
      new_data = realloc(str->data, new_size);
      if (!new_data) return 0;
      str->data = new_data;
      str->size = new_size;
   }

   va_start(ap, fmt);
   vsnprintf(str->data + str->len, len+1, fmt, ap);
   va_end(ap);

   str->len += len;
   return 1;
}


static void *string_hash_set(StringHash *hash, char *key, void *value)
{
   StringHash new_hash;
   int i, idx;
   unsigned int keyhash = 5381;
   unsigned char *s;
   void *old_val;
   
   if (hash->slots >= (hash->size >> 2)) {
      new_hash.size = hash->size;
      if (hash->len >= (hash->size >> 2)) {
         new_hash.size <<= 1;
      }
      if (new_hash.size == 0) {
         new_hash.size = 4*2;
      }
      new_hash.len = 0;
      new_hash.slots = 0;
      new_hash.data = calloc(new_hash.size, sizeof(void *));
      for (i=0; i<hash->size; i+=2) {
         if (hash->data[i+0]) {
            if (hash->data[i+1]) {
               string_hash_set(&new_hash, hash->data[i+0], hash->data[i+1]);
            }
            else {
               free(hash->data[i+0]);
            }
         }
      }
      free(hash->data);
      *hash = new_hash;
   }

   s = (unsigned char *)key;
   while (*s) {
      keyhash = ((keyhash << 5) + keyhash) + *s++;
   }

   idx = (keyhash << 1) & (hash->size-1);
   for (;;) {
      if (!hash->data[idx+0]) break;
      if (!strcmp(hash->data[idx+0], key)) {
         free(hash->data[idx+0]);
         old_val = hash->data[idx+1];
         hash->data[idx+0] = key;
         hash->data[idx+1] = value;
         if (old_val) hash->len--;
         if (value) hash->len++;
         return old_val;
      }
      idx = (idx + 2) & (hash->size-1);
   }

   if (!value) {
      free(key);
      return NULL;
   }

   hash->len++;
   hash->slots++;
   hash->data[idx+0] = key;
   hash->data[idx+1] = value;
   return NULL;
}


static void *string_hash_get(StringHash *hash, const char *key)
{
   int idx;
   unsigned int keyhash = 5381;
   unsigned char *s;

   if (!hash->data) {
      return NULL;
   }

   s = (unsigned char *)key;
   while (*s) {
      keyhash = ((keyhash << 5) + keyhash) + *s++;
   }

   idx = (keyhash << 1) & (hash->size-1);
   for (;;) {
      if (!hash->data[idx+0]) break;
      if (!strcmp(hash->data[idx+0], key)) {
         return hash->data[idx+1];
      }
      idx = (idx + 2) & (hash->size-1);
   }
   return NULL;
}


static const char *string_hash_find_name(StringHash *hash, void *value)
{
   int i;

   for (i=0; i<hash->size; i+=2) {
      if (hash->data[i+1] == value) {
         return hash->data[i+0];
      }
   }
   return NULL;
}


static void string_filter_control_chars(char *s, int len)
{
   int i;

   for (i=0; i<len; i++) {
      if (s[i] >= 0 && s[i] < 32) {
         switch (s[i]) {
            case '\t':
            case '\n':
               break;

            default:
               s[i] = '?';
         }
      }
   }
}


static int dynarray_add(DynArray *arr, void *value)
{
   void *new_data;
   int new_size;

   if (arr->len == arr->size) {
      new_size = arr->size == 0? 4 : arr->size;
      if (new_size >= (1<<30)) {
         return FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
      new_size <<= 1;
      new_data = realloc_array(arr->data, new_size, sizeof(void *));
      if (!new_data) {
         return FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
      arr->data = new_data;
      arr->size = new_size;
   }

   arr->data[arr->len++] = value;
   return FIXSCRIPT_SUCCESS;
}


static void dynarray_remove_value_fast(DynArray *arr, void *value)
{
   int i;

   for (i=0; i<arr->len; i++) {
      if (arr->data[i] == value) {
         arr->data[i] = arr->data[--arr->len];
         return;
      }
   }
}


static inline int get_low_mask(int num_bits)
{
   return ~(-1 << num_bits);
}


static inline int get_high_mask(int num_bits)
{
   return ~((uint32_t)-1 >> (uint32_t)num_bits);
}


static inline int get_middle_mask(int start, int end)
{
   return get_low_mask(end - start) << start;
}


static void flags_clear_range(Array *arr, int off, int len)
{
   uint32_t start = off;
   uint32_t end = (uint32_t)off+(uint32_t)len;
   uint32_t inner_start = (start+31U) & ~31;
   uint32_t inner_end = end & ~31;

   if (inner_end >= inner_start) {
      if (inner_start - start > 0) {
         arr->flags[start >> 5] &= ~get_high_mask(inner_start - start);
      }
      memset(&arr->flags[inner_start >> 5], 0, (inner_end - inner_start) >> (5-2));
      if (end - inner_end > 0) {
         arr->flags[inner_end >> 5] &= ~get_low_mask(end - inner_end);
      }
   }
   else {
      arr->flags[start >> 5] &= ~get_middle_mask(start & 31, end & 31);
   }
}


static void flags_set_range(Array *arr, int off, int len)
{
   uint32_t start = off;
   uint32_t end = (uint32_t)off+(uint32_t)len;
   uint32_t inner_start = (start+31U) & ~31;
   uint32_t inner_end = end & ~31;

   if (inner_end >= inner_start) {
      if (inner_start - start > 0) {
         arr->flags[start >> 5] |= get_high_mask(inner_start - start);
      }
      memset(&arr->flags[inner_start >> 5], 0xFF, (inner_end - inner_start) >> (5-2));
      if (end - inner_end > 0) {
         arr->flags[inner_end >> 5] |= get_low_mask(end - inner_end);
      }
   }
   else {
      arr->flags[start >> 5] |= get_middle_mask(start & 31, end & 31);
   }
}


static void flags_copy_range(Array *dest_arr, int dest_off, Array *src_arr, int src_off, int count)
{
   int i;

   if (dest_arr == src_arr && dest_off > src_off) {
      for (i=count-1; i>=0; i--) {
         if (IS_ARRAY(src_arr, src_off+i)) {
            SET_IS_ARRAY(dest_arr, dest_off+i);
         }
         else {
            CLEAR_IS_ARRAY(dest_arr, dest_off+i);
         }
      }
   }
   else {
      for (i=0; i<count; i++) {
         if (IS_ARRAY(src_arr, src_off+i)) {
            SET_IS_ARRAY(dest_arr, dest_off+i);
         }
         else {
            CLEAR_IS_ARRAY(dest_arr, dest_off+i);
         }
      }
   }
}


static int flags_is_array_clear_in_range(Array *arr, int off, int len)
{
   uint32_t start = off;
   uint32_t end = (uint32_t)off+(uint32_t)len;
   uint32_t inner_start = (start+31U) & ~31;
   uint32_t inner_end = end & ~31;
   int i, n;

   if (inner_end >= inner_start) {
      if (inner_start - start > 0) {
         if (arr->flags[start >> 5] & get_high_mask(inner_start - start)) {
            return 0;
         }
      }
      for (i=(inner_start >> 5), n=i+((inner_end - inner_start) >> 5); i<n; i++) {
         if (arr->flags[i]) return 0;
      }
      if (end - inner_end > 0) {
         if (arr->flags[inner_end >> 5] & get_low_mask(end - inner_end)) {
            return 0;
         }
      }
   }
   else {
      if (arr->flags[start >> 5] & get_middle_mask(start & 31, end & 31)) {
         return 0;
      }
   }
   return 1;
}


static int bitarray_size(int elem_size, int count)
{
   return ((elem_size * count + 31) >> 5)+1;
}


static void bitarray_set(int *array, int elem_size, int index, int value)
{
   unsigned int *arr = (unsigned int *)array;
   int idx = (elem_size * index) >> 5;
   int off = (elem_size * index) & 31;
   int mask = (1 << elem_size) - 1;
   uint64_t val = arr[idx] | ((uint64_t)arr[idx+1] << 32);

   val &= ~((uint64_t)mask << off);
   val |= (uint64_t)(value & mask) << off;

   arr[idx+0] = (unsigned int)val;
   arr[idx+1] = (unsigned int)(val >> 32);
}


static int bitarray_get(int *array, int elem_size, int index)
{
   unsigned int *arr = (unsigned int *)array;
   int idx = (elem_size * index) >> 5;
   int off = (elem_size * index) & 31;
   int mask = (1 << elem_size) - 1;
   uint64_t val = arr[idx] | ((uint64_t)arr[idx+1] << 32);

   return (val >> off) & mask;
}


static unsigned int rehash(unsigned int a)
{
   a = (a+0x7ed55d16) + (a<<12);
   a = (a^0xc761c23c) ^ (a>>19);
   a = (a+0x165667b1) + (a<<5);
   a = (a+0xd3a2646c) ^ (a<<9);
   a = (a+0xfd7046c5) + (a<<3);
   a = (a^0xb55a4f09) ^ (a>>16);
   return a;
}


static int handle_const_string_set(Heap *heap, ConstStringSet *set, Array *arr, int off, int len, int set_value)
{
   ConstStringSet new_set;
   Array *entry;
   uint32_t hash = 0;
   int i, idx, found;

   if (set->size == 0) {
      if (set_value <= 0) return 0;

      set->size = 64;
      set->data = calloc(set->size, sizeof(int));
      if (!set->data) {
         set->size = 0;
         return -1;
      }
   }

   if (set_value > 0 && set->slots >= (set->size >> 1)) {
      if (set->size >= 1*1024*1024*1024) {
         return -1;
      }
      if (set->len >= (set->size >> 1)) {
         new_set.size = set->size << 1;
      }
      else {
         new_set.size = set->size;
      }
      new_set.len = 0;
      new_set.slots = 0;
      new_set.data = calloc(new_set.size, sizeof(int));
      if (!new_set.data) {
         return -1;
      }

      for (i=0; i<set->size; i++) {
         if (set->data[i] > 0) {
            if (handle_const_string_set(heap, &new_set, &heap->data[set->data[i]], 0, heap->data[set->data[i]].len, set->data[i]) != set->data[i]) {
               free(new_set.data);
               return -1;
            }
         }
      }

      free(set->data);
      *set = new_set;
   }

   if (arr->type == ARR_BYTE) {
      for (i=0; i<len; i++) {
         hash = hash*31 + arr->byte_data[off+i];
      }
   }
   else if (arr->type == ARR_SHORT) {
      for (i=0; i<len; i++) {
         hash = hash*31 + arr->short_data[off+i];
      }
   }
   else {
      for (i=0; i<len; i++) {
         hash = hash*31 + ((uint32_t)arr->data[off+i]);
      }
   }

   idx = rehash(hash) & (set->size-1);
   for (;;) {
      if (set->data[idx] == 0) {
         if (set_value > 0) {
            set->data[idx] = set_value;
            set->len++;
            set->slots++;
            return set_value;
         }
         return 0;
      }

      if (set->data[idx] != -1) {
         entry = &heap->data[set->data[idx]];

         if (entry->len == len) {
            found = 1;
            if (entry->type == ARR_BYTE) {
               if (arr->type == ARR_BYTE) {
                  for (i=0; i<len; i++) {
                     if (entry->byte_data[i] != arr->byte_data[off+i]) {
                        found = 0;
                        break;
                     }
                  }
               }
               else if (arr->type == ARR_SHORT) {
                  for (i=0; i<len; i++) {
                     if (entry->byte_data[i] != arr->short_data[off+i]) {
                        found = 0;
                        break;
                     }
                  }
               }
               else {
                  for (i=0; i<len; i++) {
                     if (entry->byte_data[i] != arr->data[off+i]) {
                        found = 0;
                        break;
                     }
                  }
               }
            }
            else if (entry->type == ARR_SHORT) {
               if (arr->type == ARR_BYTE) {
                  found = 0;
               }
               else if (arr->type == ARR_SHORT) {
                  for (i=0; i<len; i++) {
                     if (entry->short_data[i] != arr->short_data[off+i]) {
                        found = 0;
                        break;
                     }
                  }
               }
               else {
                  for (i=0; i<len; i++) {
                     if (entry->short_data[i] != arr->data[off+i]) {
                        found = 0;
                        break;
                     }
                  }
               }
            }
            else {
               if (arr->type == ARR_BYTE || arr->type == ARR_SHORT) {
                  found = 0;
               }
               else {
                  for (i=0; i<len; i++) {
                     if (entry->data[i] != arr->data[off+i]) {
                        found = 0;
                        break;
                     }
                  }
               }
            }

            if (found) {
               if (set_value == -1) {
                  set->data[idx] = -1;
                  set->len--;
               }
               return set->data[idx];
            }
         }
      }

      idx = (idx+1) & (set->size-1);
   }
}


////////////////////////////////////////////////////////////////////////
// Heap management:
////////////////////////////////////////////////////////////////////////


static void *func_ref_handle_func(Heap *heap, int op, void *p1, void *p2)
{
   FuncRefHandle *handle = p1, *copy, *other;
   uint32_t hash;
   char *s;
   int len;
   
   switch (op) {
      case HANDLE_OP_FREE:
         free(handle->script_name);
         free(handle->func_name);
         free(handle);
         break;

      case HANDLE_OP_COPY:
         copy = calloc(1, sizeof(FuncRefHandle));
         if (!copy) return NULL;
         copy->script_name = strdup(handle->script_name);
         copy->func_name = strdup(handle->func_name);
         if (!copy->script_name || !copy->func_name) {
            func_ref_handle_func(heap, HANDLE_OP_FREE, copy, NULL);
            return NULL;
         }
         return copy;

      case HANDLE_OP_COMPARE:
         other = p2;
         if (strcmp(handle->script_name, other->script_name) != 0) return (void *)0;
         if (strcmp(handle->func_name, other->func_name) != 0) return (void *)0;
         return (void *)1;

      case HANDLE_OP_HASH:
         hash = 0;
         for (s = handle->script_name; *s; s++) {
            hash = hash*31 + *s;
         }
         for (s = handle->func_name; *s; s++) {
            hash = hash*31 + *s;
         }
         return (void *)(intptr_t)hash;

      case HANDLE_OP_TO_STRING:
         len = strlen(handle->script_name) + strlen(handle->func_name) + 32;
         s = malloc(len);
         if (!s) return NULL;
         snprintf(s, len, "<%s:%s> [unresolved]", handle->script_name, handle->func_name);
         return s;
   }

   return NULL;
}


static void *weak_ref_handle_func(Heap *heap, int op, void *p1, void *p2)
{
   WeakRefHandle *handle = p1, *hash_handle, **prev;
   char buf[64];
   
   switch (op) {
      case HANDLE_OP_FREE:
         if (handle->target) {
            snprintf(buf, sizeof(buf), "%d", handle->target);
            hash_handle = string_hash_get(&heap->weak_refs, buf);
            if (hash_handle == handle) {
               string_hash_set(&heap->weak_refs, strdup(buf), handle->next);
               if (!handle->next) {
                  heap->data[handle->target].has_weak_refs = 0;
               }
            }
            else {
               prev = &hash_handle->next;
               for (;;) {
                  hash_handle = hash_handle->next;
                  if (!hash_handle) break;
                  if (hash_handle == handle) {
                     *prev = handle->next;
                     break;
                  }
               }
            }
         }
         free(handle);
         break;

      case HANDLE_OP_COMPARE:
         return (void *)(intptr_t)(handle->id == ((WeakRefHandle *)p2)->id);

      case HANDLE_OP_HASH:
         return (void *)(intptr_t)((int)handle->id ^ (int)(handle->id >> 32));

      case HANDLE_OP_TO_STRING:
         if (handle->target) {
            snprintf(buf, sizeof(buf), "(weak reference to #%d)", handle->target);
         }
         else {
            snprintf(buf, sizeof(buf), "(empty weak reference)");
         }
         return strdup(buf);
   
      case HANDLE_OP_MARK_REFS:
         if (handle->container) {
            fixscript_mark_ref(heap, (Value) { handle->container, 1 });
            if (handle->key.is_array != 2) {
               fixscript_mark_ref(heap, handle->key);
            }
         }
         break;
   }
   return NULL;
}


static void add_root(Heap *heap, Value value)
{
   int old_size = heap->roots.size;
   dynarray_add(&heap->roots, (void *)(intptr_t)value.value);
   heap->total_size += (int64_t)(heap->roots.size - old_size) * sizeof(void *);
}


static void clear_roots(Heap *heap)
{
   if (!heap->collecting) {
      heap->roots.len = 0;
   }
}


static inline int get_array_value(Array *arr, int idx)
{
   if (arr->type == ARR_BYTE) {
      return arr->byte_data[idx];
   }
   else if (arr->type == ARR_SHORT) {
      return arr->short_data[idx];
   }
   else {
      return arr->data[idx];
   }
}


static inline void set_array_value(Array *arr, int idx, int value)
{
   if (arr->type == ARR_BYTE) {
      arr->byte_data[idx] = value;
   }
   else if (arr->type == ARR_SHORT) {
      arr->short_data[idx] = value;
   }
   else {
      arr->data[idx] = value;
   }
}


static int mark_array(Heap *heap, int idx, int recursion_limit)
{
   Array *arr;
   int i, j, val, len, more, flags;

   if (heap->reachable[idx >> 5] & (1 << (idx & 31))) {
      return 0;
   }

   if (recursion_limit <= 0) {
      heap->reachable[(heap->size + idx) >> 5] |= 1 << (idx & 31);
      return 1;
   }

   heap->reachable[idx >> 5] |= 1 << (idx & 31);

   arr = &heap->data[idx];
   if (arr->is_handle || arr->is_shared) {
      if (arr->is_handle == 2) {
         val = heap->marking_limit;
         heap->marking_limit = recursion_limit;
         arr->handle_func(heap, HANDLE_OP_MARK_REFS, arr->handle_ptr, NULL);
         more = heap->marking_limit < 0;
         heap->marking_limit = val;
         return more;
      }
      return 0;
   }
   
   more = 0;
   len = arr->hash_slots >= 0? (1 << arr->size) : arr->len;

   if (arr->type == ARR_BYTE) {
      for (i=0; i<(len >> 5); i++) {
         flags = arr->flags[i];
         if (flags != 0) {
            for (j=0; j<32; j++) {
               if (flags & 1) {
                  val = arr->byte_data[(i<<5) | j];
                  if (val > 0 && val < heap->size) {
                     more |= mark_array(heap, val, recursion_limit-1);
                  }
               }
               flags >>= 1;
            }
         }
      }
      for (i=(len & ~31); i<len; i++) {
         if (IS_ARRAY(arr, i)) {
            val = arr->byte_data[i];
            if (val > 0 && val < heap->size) {
               more |= mark_array(heap, val, recursion_limit-1);
            }
         }
      }
   }
   else if (arr->type == ARR_SHORT) {
      for (i=0; i<(len >> 5); i++) {
         flags = arr->flags[i];
         if (flags != 0) {
            for (j=0; j<32; j++) {
               if (flags & 1) {
                  val = arr->short_data[(i<<5) | j];
                  if (val > 0 && val < heap->size) {
                     more |= mark_array(heap, val, recursion_limit-1);
                  }
               }
               flags >>= 1;
            }
         }
      }
      for (i=(len & ~31); i<len; i++) {
         if (IS_ARRAY(arr, i)) {
            val = arr->short_data[i];
            if (val > 0 && val < heap->size) {
               more |= mark_array(heap, val, recursion_limit-1);
            }
         }
      }
   }
   else {
      for (i=0; i<(len >> 5); i++) {
         flags = arr->flags[i];
         if (flags != 0) {
            for (j=0; j<32; j++) {
               if (flags & 1) {
                  val = arr->data[(i<<5) | j];
                  if (val > 0 && val < heap->size) {
                     more |= mark_array(heap, val, recursion_limit-1);
                  }
               }
               flags >>= 1;
            }
         }
      }
      for (i=(len & ~31); i<len; i++) {
         if (IS_ARRAY(arr, i)) {
            val = arr->data[i];
            if (val > 0 && val < heap->size) {
               more |= mark_array(heap, val, recursion_limit-1);
            }
         }
      }
   }

   return more;
}


static int mark_direct_array(Heap *heap, int *data, char *flags, int len)
{
   int i, value, more=0;

   for (i=0; i<len; i++) {
      value = data[i];
      if (flags[i] && value > 0 && value < heap->size) {
         more |= mark_array(heap, value, MARK_RECURSION_CUTOFF-1);
      }
   }
   return more;
}


static int collect_heap(Heap *heap, int *hash_removal)
{
   SharedArrayHandle *sah;
   WeakRefHandle *wrh, *orig_wrh, *hash_wrh, **prev;
   Array *arr, *new_data;
   int *new_reachable;
   Value container;
   int i, j, err, num_reclaimed=0, elem_size, max_index=0, new_size, num_used=0, more=0, reachable_block, idx;
   char buf[128];
#ifndef FIXSCRIPT_NO_JIT
   uint8_t *new_jit_funcs;
#endif

   if (heap->collecting) {
      return 0;
   }
   heap->collecting = 1;
   
   more |= mark_direct_array(heap, heap->stack_data, heap->stack_flags, heap->stack_len);
   more |= mark_direct_array(heap, heap->locals_data, heap->locals_flags, heap->locals_len);
   for (i=0; i<heap->roots.len; i++) {
      more |= mark_array(heap, (intptr_t)heap->roots.data[i], MARK_RECURSION_CUTOFF);
   }
   for (i=0; i<heap->ext_roots.len; i++) {
      more |= mark_array(heap, (intptr_t)heap->ext_roots.data[i], MARK_RECURSION_CUTOFF);
   }

   while (more) {
      more = 0;

      for (i=0; i<(heap->size >> 5); i++) {
         reachable_block = heap->reachable[(heap->size >> 5)+i];
         if (reachable_block) {
            heap->reachable[(heap->size >> 5)+i] = 0;
            for (j=0; j<32; j++, reachable_block >>= 1) {
               if (reachable_block & 1) {
                  more |= mark_array(heap, (i << 5) | j, MARK_RECURSION_CUTOFF);
               }
            }
         }
      }
   }
   
   for (i=0; i<(heap->size >> 5); i++) {
      reachable_block = heap->reachable[i];
      if (reachable_block == 0xFFFFFFFF) {
         max_index = (i << 5) | 31;
         num_used += 32;
         continue;
      }
      for (j=0; j<32; j++, reachable_block >>= 1) {
         idx = (i << 5) | j;
         if (reachable_block & 1) {
            max_index = idx;
            num_used++;
            continue;
         }
         arr = &heap->data[idx];
         if (arr->len != -1 && !arr->is_static) {
            if (arr->is_handle) {
               if (arr->is_handle == 2) {
                  arr->handle_func(heap, HANDLE_OP_FREE, arr->handle_ptr, NULL);
               }
               else if (arr->handle_free) {
                  arr->handle_free(arr->handle_ptr);
               }
               arr = &heap->data[idx];
            }
            else if (arr->is_shared) {
               if (arr->flags) {
                  sah = ARRAY_SHARED_HEADER(arr);
                  elem_size = arr->type == ARR_BYTE? 1 : arr->type == ARR_SHORT? 2 : 4;
                  snprintf(buf, sizeof(buf), "%d,%p,%d,%d,%p", sah->type, arr->data, arr->len, elem_size, sah->free_data);
                  string_hash_set(&heap->shared_arrays, strdup(buf), NULL);
                  if (sah->refcnt < SAH_REFCNT_LIMIT && __sync_sub_and_fetch(&sah->refcnt, 1) == 0) {
                     if (sah->free_func) {
                        sah->free_func(sah->free_data);
                     }
                     free(sah);
                     arr = &heap->data[idx];
                  }
                  heap->total_size -= (int64_t)FLAGS_SIZE(arr->size) * sizeof(int) + (int64_t)arr->size * elem_size;
               }
            }
            else {
               if (arr->is_const) {
                  handle_const_string_set(heap, &heap->const_string_set, arr, 0, arr->len, -1);
               }
               free(arr->flags);
               if (arr->type == ARR_BYTE) {
                  free(arr->byte_data);
                  heap->total_size -= (int64_t)FLAGS_SIZE(arr->size) * sizeof(int) + (int64_t)arr->size * sizeof(unsigned char);
               }
               else if (arr->type == ARR_SHORT) {
                  free(arr->short_data);
                  heap->total_size -= (int64_t)FLAGS_SIZE(arr->size) * sizeof(int) + (int64_t)arr->size * sizeof(unsigned short);
               }
               else {
                  free(arr->data);
                  if (arr->hash_slots >= 0) {
                     heap->total_size -= ((int64_t)FLAGS_SIZE((1<<arr->size)*2) + (int64_t)bitarray_size(arr->size-1, 1<<arr->size)) * sizeof(int) + (int64_t)(1 << arr->size) * sizeof(int);
                  }
                  else {
                     heap->total_size -= (int64_t)FLAGS_SIZE(arr->size) * sizeof(int) + (int64_t)arr->size * sizeof(int);
                  }
               }
            }
            if (arr->has_weak_refs) {
               snprintf(buf, sizeof(buf), "%d", idx);
               hash_wrh = string_hash_get(&heap->weak_refs, buf);
               orig_wrh = hash_wrh;
               prev = &hash_wrh;
               for (wrh = hash_wrh; wrh; prev = &wrh->next, wrh = wrh->next) {
                  if (wrh->container) {
                     container = (Value) { wrh->container, 1 };
                     if (fixscript_is_hash(heap, container)) {
                        if (wrh->key.is_array == 2) {
                           fixscript_remove_hash_elem(heap, container, (Value) { wrh->value, 1 }, NULL);
                        }
                        else {
                           fixscript_remove_hash_elem(heap, container, wrh->key, NULL);
                           wrh->key.is_array = 2;
                        }
                        wrh->container = 0;
                        wrh->target = 0;
                        *prev = wrh->next;
                        if (hash_removal) {
                           *hash_removal = 1;
                        }
                     }
                     else {
                        if (wrh->key.is_array == 2) {
                           err = fixscript_append_array_elem(heap, container, (Value) { wrh->value, 1 });
                        }
                        else {
                           err = fixscript_append_array_elem(heap, container, wrh->key);
                           if (!err) {
                              wrh->key.is_array = 2;
                           }
                        }
                        if (!err) {
                           wrh->container = 0;
                           wrh->target = 0;
                           *prev = wrh->next;
                        }
                     }
                  }
                  else {
                     wrh->target = 0;
                     *prev = wrh->next;
                  }
               }
               if (hash_wrh != orig_wrh) {
                  string_hash_set(&heap->weak_refs, strdup(buf), hash_wrh);
               }
               else {
                  arr->flags = NULL;
                  arr->data = NULL;
                  arr->size = 0;
                  arr->len = 0;
                  arr->type = ARR_BYTE;
                  #ifndef FIXSCRIPT_NO_JIT
                     heap->jit_array_get_funcs[idx] = heap->jit_array_get_byte_func;
                     heap->jit_array_set_funcs[idx*2+0] = heap->jit_array_set_byte_func[0];
                     heap->jit_array_set_funcs[idx*2+1] = heap->jit_array_set_byte_func[1];
                     heap->jit_array_append_funcs[idx*2+0] = heap->jit_array_append_byte_func[0];
                     heap->jit_array_append_funcs[idx*2+1] = heap->jit_array_append_byte_func[1];
                  #endif
                  max_index = idx;
                  num_used++;
                  continue;
               }
            }
            arr->len = -1;
            #ifndef FIXSCRIPT_NO_JIT
               heap->jit_array_get_funcs[idx] = 0;
               heap->jit_array_set_funcs[idx*2+0] = 0;
               heap->jit_array_set_funcs[idx*2+1] = 0;
               heap->jit_array_append_funcs[idx*2+0] = 0;
               heap->jit_array_append_funcs[idx*2+1] = 0;
            #endif
            if (num_reclaimed++ == 0) {
               heap->next_idx = idx;
            }
         }

         if (arr->len != -1) {
            max_index = idx;
            num_used++;
         }
      }
   }

   memset(heap->reachable, 0, (heap->size >> 4) * sizeof(int));

   if (heap->size > ARRAYS_GROW_CUTOFF) {
      new_size = (max_index + ARRAYS_GROW_CUTOFF) & ~(ARRAYS_GROW_CUTOFF-1);
      if (heap->size - num_used < ARRAYS_GROW_CUTOFF) {
         new_size += ARRAYS_GROW_CUTOFF;
      }
      new_size = (new_size + 31) & ~31;
      
      if (new_size < heap->size) {
         new_data = realloc_array(heap->data, new_size, sizeof(Array));
         if (new_data) {
            heap->total_size -= (int64_t)(heap->size - new_size) * sizeof(Array);
            heap->data = new_data;
            heap->size = new_size;
            if (heap->next_idx >= new_size) {
               heap->next_idx = 1;
            }
            new_reachable = realloc_array(heap->reachable, new_size >> 4, sizeof(int));
            if (new_reachable) {
               heap->reachable = new_reachable;
            }
         }
         #ifndef FIXSCRIPT_NO_JIT
            new_jit_funcs = realloc_array(heap->jit_array_get_funcs, heap->size, sizeof(uint8_t));
            if (new_jit_funcs) {
               heap->jit_array_get_funcs = new_jit_funcs;
            }
            new_jit_funcs = realloc_array(heap->jit_array_set_funcs, heap->size * 2, sizeof(uint8_t));
            if (new_jit_funcs) {
               heap->jit_array_set_funcs = new_jit_funcs;
            }
            new_jit_funcs = realloc_array(heap->jit_array_append_funcs, heap->size * 2, sizeof(uint8_t));
            if (new_jit_funcs) {
               heap->jit_array_append_funcs = new_jit_funcs;
            }
            jit_update_heap_refs(heap);
         #endif
      }
   }

   heap->collecting = 0;
   return num_reclaimed;
}


static void reclaim_array(Heap *heap, int idx, Array *arr)
{
   int i;

   if (!arr) {
      arr = &heap->data[idx];
   }
   free(arr->flags);
   if (arr->type == ARR_BYTE) {
      free(arr->byte_data);
      heap->total_size -= (int64_t)FLAGS_SIZE(arr->size) * sizeof(int) + (int64_t)arr->size * sizeof(unsigned char);
   }
   else if (arr->type == ARR_SHORT) {
      free(arr->short_data);
      heap->total_size -= (int64_t)FLAGS_SIZE(arr->size) * sizeof(int) + (int64_t)arr->size * sizeof(unsigned short);
   }
   else {
      free(arr->data);
      if (arr->hash_slots >= 0) {
         heap->total_size -= ((int64_t)FLAGS_SIZE((1<<arr->size)*2) + (int64_t)bitarray_size(arr->size-1, 1<<arr->size)) * sizeof(int) + (int64_t)(1 << arr->size) * sizeof(int);
      }
      else {
         heap->total_size -= (int64_t)FLAGS_SIZE(arr->size) * sizeof(int) + (int64_t)arr->size * sizeof(int);
      }
   }
   arr->len = -1;
   if (heap->collecting) {
      heap->reachable[idx >> 5] &= ~(1 << (idx & 31));
      heap->reachable[(heap->size+idx) >> 5] &= ~(1 << (idx & 31));
   }

   #ifndef FIXSCRIPT_NO_JIT
      heap->jit_array_get_funcs[idx] = 0;
      heap->jit_array_set_funcs[idx*2+0] = 0;
      heap->jit_array_set_funcs[idx*2+1] = 0;
      heap->jit_array_append_funcs[idx*2+0] = 0;
      heap->jit_array_append_funcs[idx*2+1] = 0;
   #endif

   for (i=0; i<heap->roots.len; i++) {
      if ((intptr_t)heap->roots.data[i] == idx) {
         heap->roots.data[i] = heap->roots.data[--heap->roots.len];
         break;
      }
   }
}


void fixscript_collect_heap(Heap *heap)
{
   int hash_removal;

   clear_roots(heap);

   do {
      hash_removal = 0;
      collect_heap(heap, &hash_removal);
   }
   while (hash_removal);
}


static Value create_array(Heap *heap, int type, int size)
{
   int new_size, alloc_size;
   Array *new_data;
   int *new_reachable;
   int i, idx = -1, collected = -1;
   Array *arr;
#ifndef FIXSCRIPT_NO_JIT
   uint8_t *new_jit_funcs;
#endif

   if (heap->total_size > heap->total_cap) {
      collect_heap(heap, NULL);
      while (heap->total_size + (heap->total_size >> 2) > heap->total_cap) {
         heap->total_cap <<= 1;
      }
      while (heap->total_size < (heap->total_cap >> 2) && heap->total_cap > 1) {
         heap->total_cap >>= 1;
      }
   }
   
   for (i=heap->next_idx; i<heap->size; i++) {
      if (heap->data[i].len == -1) {
         idx = i;
         heap->next_idx = i+1;
         break;
      }
   }

   if (idx == -1 && (collected = collect_heap(heap, NULL)) > 0) {
      idx = heap->next_idx++;
   }

   if (idx == -1 || (collected > 0 && (heap->size - collected) >= heap->size - (heap->size >> 2))) {
      if (heap->size >= FUNC_REF_OFFSET) {
         return fixscript_int(0);
      }
      if (idx == -1) idx = heap->size;
      new_size = heap->size >= ARRAYS_GROW_CUTOFF? heap->size + ARRAYS_GROW_CUTOFF : heap->size << 1;
      new_size = (new_size + 31) & ~31;
      if (new_size > FUNC_REF_OFFSET) {
         new_size = FUNC_REF_OFFSET;
      }
      new_data = realloc_array(heap->data, new_size, sizeof(Array));
      if (!new_data) {
         return fixscript_int(0);
      }
      new_reachable = realloc_array(heap->reachable, new_size >> 4, sizeof(int));
      if (!new_reachable) {
         return fixscript_int(0);
      }
      heap->reachable = new_reachable;
      for (i=(heap->size >> 5)-1; i>=0; i--) {
         heap->reachable[(new_size >> 5)+i] = heap->reachable[(heap->size >> 5)+i];
      }
      for (i=heap->size >> 5; i<(new_size >> 5); i++) {
         heap->reachable[i] = 0;
         heap->reachable[(new_size >> 5)+i] = 0;
      }
      #ifndef FIXSCRIPT_NO_JIT
         new_jit_funcs = realloc_array(heap->jit_array_get_funcs, new_size, sizeof(uint8_t));
         if (!new_jit_funcs) {
            return fixscript_int(0);
         }
         heap->jit_array_get_funcs = new_jit_funcs;
         for (i=heap->size; i<new_size; i++) {
            heap->jit_array_get_funcs[i] = 0;
         }
         new_jit_funcs = realloc_array(heap->jit_array_set_funcs, new_size * 2, sizeof(uint8_t));
         if (!new_jit_funcs) {
            return fixscript_int(0);
         }
         heap->jit_array_set_funcs = new_jit_funcs;
         for (i=heap->size*2; i<new_size*2; i++) {
            heap->jit_array_set_funcs[i] = 0;
         }
         new_jit_funcs = realloc_array(heap->jit_array_append_funcs, new_size * 2, sizeof(uint8_t));
         if (!new_jit_funcs) {
            return fixscript_int(0);
         }
         heap->jit_array_append_funcs = new_jit_funcs;
         for (i=heap->size*2; i<new_size*2; i++) {
            heap->jit_array_append_funcs[i] = 0;
         }
      #endif
      heap->total_size += (int64_t)(new_size - heap->size) * sizeof(Array);
      heap->data = new_data;
      for (i=heap->size; i<new_size; i++) {
         arr = &heap->data[i];
         arr->len = -1;
      }
      heap->size = new_size;
      #ifndef FIXSCRIPT_NO_JIT
         jit_update_heap_refs(heap);
      #endif
   }

   arr = &heap->data[idx];
   if (size > 0) {
      if (type == ARR_HASH && size >= 30) {
         return fixscript_int(0);
      }
      alloc_size = (type == ARR_HASH? (1 << size) : size);

      arr->flags = malloc_array(type == ARR_HASH? FLAGS_SIZE(alloc_size*2) + bitarray_size(size-1, alloc_size) : FLAGS_SIZE(alloc_size), sizeof(int));
      if (!arr->flags) return fixscript_int(0);

      if (type == ARR_BYTE) {
         arr->byte_data = malloc_array(alloc_size, sizeof(unsigned char));
         if (!arr->byte_data) {
            free(arr->flags);
            return fixscript_int(0);
         }
         heap->total_size += (int64_t)FLAGS_SIZE(alloc_size) * sizeof(int) + (int64_t)alloc_size * sizeof(unsigned char);
      }
      else if (type == ARR_SHORT) {
         arr->short_data = malloc_array(alloc_size, sizeof(unsigned short));
         if (!arr->short_data) {
            free(arr->flags);
            return fixscript_int(0);
         }
         heap->total_size += (int64_t)FLAGS_SIZE(alloc_size) * sizeof(int) + (int64_t)alloc_size * sizeof(unsigned short);
      }
      else {
         arr->data = malloc_array(alloc_size, sizeof(int));
         if (!arr->data) {
            free(arr->flags);
            return fixscript_int(0);
         }
         if (type == ARR_HASH) {
            heap->total_size += ((int64_t)FLAGS_SIZE(alloc_size*2) + (int64_t)bitarray_size(size-1, alloc_size)) * sizeof(int) + (int64_t)alloc_size * sizeof(int);
         }
         else {
            heap->total_size += (int64_t)FLAGS_SIZE(alloc_size) * sizeof(int) + (int64_t)alloc_size * sizeof(int);
         }
      }
      arr->size = size;
   }
   else {
      arr->flags = NULL;
      arr->data = NULL;
      arr->size = 0;
   }

   #ifndef FIXSCRIPT_NO_JIT
      if (type == ARR_BYTE) {
         heap->jit_array_get_funcs[idx] = heap->jit_array_get_byte_func;
         heap->jit_array_set_funcs[idx*2+0] = heap->jit_array_set_byte_func[0];
         heap->jit_array_set_funcs[idx*2+1] = heap->jit_array_set_byte_func[1];
         heap->jit_array_append_funcs[idx*2+0] = heap->jit_array_append_byte_func[0];
         heap->jit_array_append_funcs[idx*2+1] = heap->jit_array_append_byte_func[1];
      }
      else if (type == ARR_SHORT) {
         heap->jit_array_get_funcs[idx] = heap->jit_array_get_short_func;
         heap->jit_array_set_funcs[idx*2+0] = heap->jit_array_set_short_func[0];
         heap->jit_array_set_funcs[idx*2+1] = heap->jit_array_set_short_func[1];
         heap->jit_array_append_funcs[idx*2+0] = heap->jit_array_append_short_func[0];
         heap->jit_array_append_funcs[idx*2+1] = heap->jit_array_append_short_func[1];
      }
      else if (type == ARR_INT) {
         heap->jit_array_get_funcs[idx] = heap->jit_array_get_int_func;
         heap->jit_array_set_funcs[idx*2+0] = heap->jit_array_set_int_func[0];
         heap->jit_array_set_funcs[idx*2+1] = heap->jit_array_set_int_func[1];
         heap->jit_array_append_funcs[idx*2+0] = heap->jit_array_append_int_func[0];
         heap->jit_array_append_funcs[idx*2+1] = heap->jit_array_append_int_func[1];
      }
   #endif

   arr->type = type;
   arr->len = 0;
   arr->ext_refcnt = 0;
   if (heap->collecting) {
      heap->reachable[idx >> 5] |= 1 << (idx & 31);
   }
   arr->is_string = 0;
   arr->is_handle = 0;
   arr->is_static = 0;
   arr->is_const = 0;
   arr->is_shared = 0;
   arr->has_weak_refs = 0;
   arr->is_protected = 0;
   return (Value) { idx, 1 };
}


static void set_const_string(Heap *heap, int idx)
{
   heap->data[idx].is_const = 1;

   #ifndef FIXSCRIPT_NO_JIT
      heap->jit_array_set_funcs[idx*2+0] = heap->jit_array_set_const_string;
      heap->jit_array_set_funcs[idx*2+1] = heap->jit_array_set_const_string;
      heap->jit_array_append_funcs[idx*2+0] = heap->jit_array_append_const_string;
      heap->jit_array_append_funcs[idx*2+1] = heap->jit_array_append_const_string;
   #endif
}


static void set_shared_array(Heap *heap, int idx)
{
   Array *arr;

   arr = &heap->data[idx];
   arr->is_shared = 1;

   #ifndef FIXSCRIPT_NO_JIT
      if (arr->type == ARR_BYTE) {
         heap->jit_array_set_funcs[idx*2+0] = heap->jit_shared_set_byte_func[0];
         heap->jit_array_set_funcs[idx*2+1] = heap->jit_shared_set_byte_func[1];
      }
      else if (arr->type == ARR_SHORT) {
         heap->jit_array_set_funcs[idx*2+0] = heap->jit_shared_set_short_func[0];
         heap->jit_array_set_funcs[idx*2+1] = heap->jit_shared_set_short_func[1];
      }
      else if (arr->type == ARR_INT) {
         heap->jit_array_set_funcs[idx*2+0] = heap->jit_shared_set_int_func[0];
         heap->jit_array_set_funcs[idx*2+1] = heap->jit_shared_set_int_func[1];
      }
      heap->jit_array_append_funcs[idx*2+0] = heap->jit_array_append_shared;
      heap->jit_array_append_funcs[idx*2+1] = heap->jit_array_append_shared;
   #endif
}


Value fixscript_create_array(Heap *heap, int len)
{
   Value value;
   Array *arr;
   
   if (len < 0) {
      return fixscript_int(0);
   }
   value = create_array(heap, ARR_BYTE, len);
   if (!value.is_array) return value;
   add_root(heap, value);
   if (len > 0) {
      arr = &heap->data[value.value];
      arr->len = len;
      memset(arr->flags, 0, FLAGS_SIZE(len) * sizeof(int));
      memset(arr->byte_data, 0, len);
   }
   return value;
}


Value fixscript_create_byte_array(Heap *heap, const char *buf, int len)
{
   Value arr_val;
   Array *arr;
   
   arr_val = create_array(heap, ARR_BYTE, len);
   if (!arr_val.is_array) return arr_val;
   add_root(heap, arr_val);
   arr = &heap->data[arr_val.value];

   arr->len = len;
   memset(arr->flags, 0, FLAGS_SIZE(len) * sizeof(int));
   memcpy(arr->byte_data, buf, len);
   return arr_val;
}


Value fixscript_create_shared_array(Heap *heap, int len, int elem_size)
{
   void *ptr = calloc(len, elem_size);
   if (!ptr) {
      return fixscript_int(0);
   }
   return fixscript_create_or_get_shared_array(heap, -1, ptr, len, elem_size, free, ptr, NULL);
}


static Value create_shared_array_from(Heap *heap, int type, void *ptr, int len, int elem_size, HandleFreeFunc free_func, void *data, int *created, SharedArrayHandle *sah)
{
   Value value;
   Array *arr;
   int arr_type;
   char buf[128];

   switch (elem_size) {
      case 1: arr_type = ARR_BYTE; break;
      case 2: arr_type = ARR_SHORT; break;
      case 4: arr_type = ARR_INT; break;

      default:
         if (free_func) {
            free_func(data);
         }
         return fixscript_int(0);
   }

   if (elem_size == 2 && (((intptr_t)ptr) & 1) != 0) {
      if (free_func) {
         free_func(data);
      }
      return fixscript_int(0);
   }

   if (elem_size == 4 && (((intptr_t)ptr) & 3) != 0) {
      if (free_func) {
         free_func(data);
      }
      return fixscript_int(0);
   }

   snprintf(buf, sizeof(buf), "%d,%p,%d,%d,%p", type, ptr, len, elem_size, data);
   value.value = (intptr_t)string_hash_get(&heap->shared_arrays, buf);
   if (value.value) {
      value.is_array = 1;
      add_root(heap, value);
      if (created) {
         *created = 0;
      }
      return value;
   }
   
   value = create_array(heap, arr_type, 0);
   if (!value.value) {
      if (free_func) {
         free_func(data);
      }
      return fixscript_int(0);
   }

   arr = &heap->data[value.value];
   arr->data = ptr;
   arr->flags = sah? sah : calloc(1, sizeof(SharedArrayHandle) + FLAGS_SIZE(len) * sizeof(int));

   if (!arr->flags) {
      if (free_func) {
         free_func(data);
      }
      arr->data = NULL;
      reclaim_array(heap, value.value, arr);
      return fixscript_int(0);
   }

   arr->len = len;
   arr->size = len;
   arr->flags = (int *)(((char *)arr->flags) + sizeof(SharedArrayHandle));
   set_shared_array(heap, value.value);

   if (sah) {
      if (sah->refcnt < SAH_REFCNT_LIMIT) {
         __sync_add_and_fetch(&sah->refcnt, 1);
      }
   }
   else {
      sah = ARRAY_SHARED_HEADER(arr);
      sah->refcnt = 1;
      sah->type = type;
      sah->ptr = ptr;
      sah->len = len;
      sah->elem_size = elem_size;
      sah->free_func = free_func;
      sah->free_data = data;
   }

   string_hash_set(&heap->shared_arrays, strdup(buf), (void *)(intptr_t)value.value);
   
   heap->total_size += (int64_t)FLAGS_SIZE(len) * sizeof(int) + (int64_t)len * elem_size;

   add_root(heap, value);
   heap->handle_created = 1;
   if (created) {
      *created = 1;
   }
   return value;
}


Value fixscript_create_or_get_shared_array(Heap *heap, int type, void *ptr, int len, int elem_size, HandleFreeFunc free_func, void *data, int *created)
{
   return create_shared_array_from(heap, type, ptr, len, elem_size, free_func, data, created, NULL);
}


int fixscript_set_array_length(Heap *heap, Value arr_val, int len)
{
   Array *arr;
   int new_size;
   int *new_flags;
   void *new_data;

   if (!arr_val.is_array || arr_val.value <= 0 || arr_val.value >= heap->size) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   arr = &heap->data[arr_val.value];
   if (arr->len == -1 || arr->hash_slots >= 0) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   if (arr->is_const) {
      return FIXSCRIPT_ERR_CONST_WRITE;
   }

   if (arr->is_shared) {
      return FIXSCRIPT_ERR_INVALID_SHARED_ARRAY_OPERATION;
   }

   if (len < 0) {
      return FIXSCRIPT_ERR_OUT_OF_BOUNDS;
   }

   if (len > arr->size) {
      new_size = arr->size == 0? 2 : arr->size;
      do {
         if (new_size >= (1<<30)) {
            return FIXSCRIPT_ERR_OUT_OF_MEMORY;
         }
         new_size <<= 1;
      }
      while (len > new_size);

      new_flags = realloc_array(arr->flags, FLAGS_SIZE(new_size), sizeof(int));
      if (!new_flags) {
         return FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
      arr->flags = new_flags;
      
      if (arr->type == ARR_BYTE) {
         new_data = realloc_array(arr->byte_data, new_size, sizeof(unsigned char));
         if (!new_data) {
            return FIXSCRIPT_ERR_OUT_OF_MEMORY;
         }
         arr->byte_data = new_data;
         heap->total_size += (int64_t)(FLAGS_SIZE(new_size) - FLAGS_SIZE(arr->size)) * sizeof(int);
         heap->total_size += (int64_t)(new_size - arr->size) * sizeof(unsigned char);
      }
      else if (arr->type == ARR_SHORT) {
         new_data = realloc_array(arr->short_data, new_size, sizeof(unsigned short));
         if (!new_data) {
            return FIXSCRIPT_ERR_OUT_OF_MEMORY;
         }
         arr->short_data = new_data;
         heap->total_size += (int64_t)(FLAGS_SIZE(new_size) - FLAGS_SIZE(arr->size)) * sizeof(int);
         heap->total_size += (int64_t)(new_size - arr->size) * sizeof(unsigned short);
      }
      else {
         new_data = realloc_array(arr->data, new_size, sizeof(int));
         if (!new_data) {
            return FIXSCRIPT_ERR_OUT_OF_MEMORY;
         }
         arr->data = new_data;
         heap->total_size += (int64_t)(FLAGS_SIZE(new_size) - FLAGS_SIZE(arr->size)) * sizeof(int);
         heap->total_size += (int64_t)(new_size - arr->size) * sizeof(int);
      }

      arr->size = new_size;
   }

   if (len > arr->len) {
      // note: integer overflow can't happen because it wouldn't get past the allocation code
      if (arr->type == ARR_BYTE) {
         memset(&arr->byte_data[arr->len], 0, (len - arr->len) * sizeof(unsigned char));
      }
      else if (arr->type == ARR_SHORT) {
         memset(&arr->short_data[arr->len], 0, (len - arr->len) * sizeof(unsigned short));
      }
      else {
         memset(&arr->data[arr->len], 0, (len - arr->len) * sizeof(int));
      }
      flags_clear_range(arr, arr->len, len - arr->len);
   }
   arr->len = len;

   return FIXSCRIPT_SUCCESS;
}


int fixscript_get_array_length(Heap *heap, Value arr_val, int *len)
{
   Array *arr;

   if (!arr_val.is_array || arr_val.value <= 0 || arr_val.value >= heap->size) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   arr = &heap->data[arr_val.value];
   if (arr->len == -1) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   *len = arr->len;
   return FIXSCRIPT_SUCCESS;
}


int fixscript_get_array_element_size(Heap *heap, Value arr_val, int *elem_size)
{
   Array *arr;

   if (!fixscript_is_array(heap, arr_val)) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   arr = &heap->data[arr_val.value];
   switch (arr->type) {
      case ARR_BYTE:  *elem_size = 1; return FIXSCRIPT_SUCCESS;
      case ARR_SHORT: *elem_size = 2; return FIXSCRIPT_SUCCESS;
      case ARR_INT:   *elem_size = 4; return FIXSCRIPT_SUCCESS;
   }

   return FIXSCRIPT_ERR_INVALID_ACCESS;
}


int fixscript_is_array(Heap *heap, Value arr_val)
{
   Array *arr;

   if (!arr_val.is_array || arr_val.value <= 0 || arr_val.value >= heap->size) {
      return 0;
   }

   arr = &heap->data[arr_val.value];
   if (arr->len == -1 || arr->hash_slots >= 0) {
      return 0;
   }

   return 1;
}


static int upgrade_array(Heap *heap, Array *arr, int arr_val, int int_val)
{
   unsigned short *short_data;
   int *data;
   int i;

   if (arr->is_shared) {
      return FIXSCRIPT_ERR_INVALID_SHARED_ARRAY_OPERATION;
   }
   
   if (arr->type == ARR_BYTE) {
      if (int_val >= 0 && int_val <= 0xFFFF) {
         short_data = malloc_array(arr->size, sizeof(unsigned short));
         if (!short_data) {
            return FIXSCRIPT_ERR_OUT_OF_MEMORY;
         }
         for (i=0; i<arr->len; i++) {
            short_data[i] = arr->byte_data[i];
         }
         free(arr->byte_data);
         arr->short_data = short_data;
         arr->type = ARR_SHORT;
         heap->total_size += (int64_t)arr->size * 1;
         #ifndef FIXSCRIPT_NO_JIT
            heap->jit_array_get_funcs[arr_val] = heap->jit_array_get_short_func;
            heap->jit_array_set_funcs[arr_val*2+0] = heap->jit_array_set_short_func[0];
            heap->jit_array_set_funcs[arr_val*2+1] = heap->jit_array_set_short_func[1];
            heap->jit_array_append_funcs[arr_val*2+0] = heap->jit_array_append_short_func[0];
            heap->jit_array_append_funcs[arr_val*2+1] = heap->jit_array_append_short_func[1];
         #endif
      }
      else {
         data = malloc_array(arr->size, sizeof(int));
         if (!data) {
            return FIXSCRIPT_ERR_OUT_OF_MEMORY;
         }
         for (i=0; i<arr->len; i++) {
            data[i] = arr->byte_data[i];
         }
         free(arr->byte_data);
         arr->data = data;
         arr->type = ARR_INT;
         heap->total_size += (int64_t)arr->size * 3;
         #ifndef FIXSCRIPT_NO_JIT
            heap->jit_array_get_funcs[arr_val] = heap->jit_array_get_int_func;
            heap->jit_array_set_funcs[arr_val*2+0] = heap->jit_array_set_int_func[0];
            heap->jit_array_set_funcs[arr_val*2+1] = heap->jit_array_set_int_func[1];
            heap->jit_array_append_funcs[arr_val*2+0] = heap->jit_array_append_int_func[0];
            heap->jit_array_append_funcs[arr_val*2+1] = heap->jit_array_append_int_func[1];
         #endif
      }
   }
   else if (arr->type == ARR_SHORT) {
      data = malloc_array(arr->size, sizeof(int));
      if (!data) {
         return FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
      for (i=0; i<arr->len; i++) {
         data[i] = arr->short_data[i];
      }
      free(arr->short_data);
      arr->data = data;
      arr->type = ARR_INT;
      heap->total_size += (int64_t)arr->size * 2;
      #ifndef FIXSCRIPT_NO_JIT
         heap->jit_array_get_funcs[arr_val] = heap->jit_array_get_int_func;
         heap->jit_array_set_funcs[arr_val*2+0] = heap->jit_array_set_int_func[0];
         heap->jit_array_set_funcs[arr_val*2+1] = heap->jit_array_set_int_func[1];
         heap->jit_array_append_funcs[arr_val*2+0] = heap->jit_array_append_int_func[0];
         heap->jit_array_append_funcs[arr_val*2+1] = heap->jit_array_append_int_func[1];
      #endif
   }

   return FIXSCRIPT_SUCCESS;
}


static int expand_array(Heap *heap, Array *arr, int idx)
{
   int new_size;
   int *new_flags;
   void *new_data;

   if (arr->is_shared) {
      return FIXSCRIPT_ERR_INVALID_SHARED_ARRAY_OPERATION;
   }

   new_size = arr->size == 0? 2 : arr->size;
   do {
      if (new_size >= (1<<30)) {
         return FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
      new_size <<= 1;
   }
   while (idx >= new_size);

   new_flags = realloc_array(arr->flags, FLAGS_SIZE(new_size), sizeof(int));
   if (!new_flags) {
      return FIXSCRIPT_ERR_OUT_OF_MEMORY;
   }
   arr->flags = new_flags;

   if (arr->type == ARR_BYTE) {
      new_data = realloc_array(arr->byte_data, new_size, sizeof(unsigned char));
      if (!new_data) {
         return FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
      arr->byte_data = new_data;
      heap->total_size += (int64_t)(FLAGS_SIZE(new_size) - FLAGS_SIZE(arr->size)) * sizeof(int);
      heap->total_size += (int64_t)(new_size - arr->size) * sizeof(unsigned char);
   }
   else if (arr->type == ARR_SHORT) {
      new_data = realloc_array(arr->short_data, new_size, sizeof(unsigned short));
      if (!new_data) {
         return FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
      arr->short_data = new_data;
      heap->total_size += (int64_t)(FLAGS_SIZE(new_size) - FLAGS_SIZE(arr->size)) * sizeof(int);
      heap->total_size += (int64_t)(new_size - arr->size) * sizeof(unsigned short);
   }
   else {
      new_data = realloc_array(arr->data, new_size, sizeof(int));
      if (!new_data) {
         return FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
      arr->data = new_data;
      heap->total_size += (int64_t)(FLAGS_SIZE(new_size) - FLAGS_SIZE(arr->size)) * sizeof(int);
      heap->total_size += (int64_t)(new_size - arr->size) * sizeof(int);
   }

   arr->size = new_size;

   return FIXSCRIPT_SUCCESS;
}


int fixscript_set_array_elem(Heap *heap, Value arr_val, int idx, Value value)
{
   Array *arr;
   int ret;

   if (!arr_val.is_array || arr_val.value <= 0 || arr_val.value >= heap->size) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   arr = &heap->data[arr_val.value];
   if (arr->len == -1 || arr->hash_slots >= 0) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   if (arr->is_const) {
      return FIXSCRIPT_ERR_CONST_WRITE;
   }

   if (arr->is_shared && !fixscript_is_int(value) && !fixscript_is_float(value)) {
      return FIXSCRIPT_ERR_INVALID_SHARED_ARRAY_OPERATION;
   }

   if (idx < 0 || idx >= arr->len) {
      return FIXSCRIPT_ERR_OUT_OF_BOUNDS;
   }

   if (ARRAY_NEEDS_UPGRADE(arr, value.value)) {
      ret = upgrade_array(heap, arr, arr_val.value, value.value);
      if (ret != FIXSCRIPT_SUCCESS) {
         return ret;
      }
   }

   set_array_value(arr, idx, value.value);
   if (!arr->is_shared) {
      ASSIGN_IS_ARRAY(arr, idx, value.is_array);
   }
   return FIXSCRIPT_SUCCESS;
}


int fixscript_get_array_elem(Heap *heap, Value arr_val, int idx, Value *value)
{
   Array *arr;

   if (!arr_val.is_array || arr_val.value <= 0 || arr_val.value >= heap->size) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   arr = &heap->data[arr_val.value];
   if (arr->len == -1 || arr->hash_slots >= 0) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   if (idx < 0 || idx >= arr->len) {
      return FIXSCRIPT_ERR_OUT_OF_BOUNDS;
   }

   *value = (Value) { get_array_value(arr, idx), IS_ARRAY(arr, idx) != 0 };
   return FIXSCRIPT_SUCCESS;
}


int fixscript_append_array_elem(Heap *heap, Value arr_val, Value value)
{
   int len, err;

   err = fixscript_get_array_length(heap, arr_val, &len);
   if (err) {
      return err;
   }

   err = fixscript_set_array_length(heap, arr_val, len+1);
   if (err) {
      return err;
   }
   
   return fixscript_set_array_elem(heap, arr_val, len, value);
}


int fixscript_get_array_range(Heap *heap, Value arr_val, int off, int len, Value *values)
{
   Array *arr;
   int i, idx;

   if (!arr_val.is_array || arr_val.value <= 0 || arr_val.value >= heap->size) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   arr = &heap->data[arr_val.value];
   if (arr->len == -1 || arr->hash_slots >= 0) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   for (i=0; i<len; i++) {
      idx = off+i;
      if (idx < 0 || idx >= arr->len) {
         return FIXSCRIPT_ERR_OUT_OF_BOUNDS;
      }
      values[i] = (Value) { get_array_value(arr, idx), IS_ARRAY(arr, idx) != 0 };
   }

   return FIXSCRIPT_SUCCESS;
}


int fixscript_set_array_range(Heap *heap, Value arr_val, int off, int len, Value *values)
{
   Array *arr;
   int i, idx, ret;
   unsigned int max_value;

   if (!arr_val.is_array || arr_val.value <= 0 || arr_val.value >= heap->size) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   arr = &heap->data[arr_val.value];
   if (arr->len == -1 || arr->hash_slots >= 0) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   if (arr->is_const) {
      return FIXSCRIPT_ERR_CONST_WRITE;
   }

   if (arr->is_shared) {
      for (i=0; i<len; i++)  {
         if (!fixscript_is_int(values[i]) && !fixscript_is_float(values[i])) {
            return FIXSCRIPT_ERR_INVALID_SHARED_ARRAY_OPERATION;
         }
      }
   }

   if (arr->type == ARR_BYTE || arr->type == ARR_SHORT) {
      max_value = 0;
      for (i=0; i<len; i++) {
         if ((unsigned int)values[i].value > max_value) {
            max_value = values[i].value;
         }
      }
      if (ARRAY_NEEDS_UPGRADE(arr, max_value)) {
         ret = upgrade_array(heap, arr, arr_val.value, max_value);
         if (ret != FIXSCRIPT_SUCCESS) {
            return ret;
         }
      }
   }

   for (i=0; i<len; i++) {
      idx = off+i;
      if (idx < 0 || idx >= arr->len) {
         return FIXSCRIPT_ERR_OUT_OF_BOUNDS;
      }
      set_array_value(arr, idx, values[i].value);
      if (!arr->is_shared) {
         ASSIGN_IS_ARRAY(arr, idx, values[i].is_array);
      }
   }

   return FIXSCRIPT_SUCCESS;
}


int fixscript_get_array_bytes(Heap *heap, Value arr_val, int off, int len, char *bytes)
{
   Array *arr;
   int i, value;

   if (!arr_val.is_array || arr_val.value <= 0 || arr_val.value >= heap->size) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   arr = &heap->data[arr_val.value];
   if (arr->len == -1 || arr->hash_slots >= 0) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   if (off < 0 || len < 0 || ((int64_t)off) + ((int64_t)len) > ((int64_t)arr->len)) {
      return FIXSCRIPT_ERR_OUT_OF_BOUNDS;
   }

   if (!arr->is_shared && !flags_is_array_clear_in_range(arr, off, len)) {
      return FIXSCRIPT_ERR_INVALID_BYTE_ARRAY;
   }

   if (arr->type == ARR_BYTE) {
      memcpy(bytes, arr->byte_data + off, len);
   }
   else {
      for (i=0; i<len; i++) {
         value = get_array_value(arr, off+i);
         if (value < 0 || value > 255) {
            return FIXSCRIPT_ERR_INVALID_BYTE_ARRAY;
         }
         bytes[i] = value;
      }
   }

   return FIXSCRIPT_SUCCESS;
}


int fixscript_set_array_bytes(Heap *heap, Value arr_val, int off, int len, char *bytes)
{
   Array *arr;
   int i;

   if (!arr_val.is_array || arr_val.value <= 0 || arr_val.value >= heap->size) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   arr = &heap->data[arr_val.value];
   if (arr->len == -1 || arr->hash_slots >= 0) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   if (arr->is_const) {
      return FIXSCRIPT_ERR_CONST_WRITE;
   }

   if (off < 0 || len < 0 || ((int64_t)off) + ((int64_t)len) > ((int64_t)arr->len)) {
      return FIXSCRIPT_ERR_OUT_OF_BOUNDS;
   }

   if (arr->type == ARR_BYTE) {
      memcpy(arr->byte_data + off, bytes, len);
   }
   else {
      for (i=0; i<len; i++) {
         set_array_value(arr, off+i, (uint32_t)(uint8_t)bytes[i]);
      }
   }
   if (!arr->is_shared) {
      flags_clear_range(arr, off, len);
   }

   return FIXSCRIPT_SUCCESS;
}


int fixscript_has_array_references(Heap *heap, Value arr_val, int off, int len, int float_as_ref, int *result)
{
   Array *arr;
   int i, value;

   if (!arr_val.is_array || arr_val.value <= 0 || arr_val.value >= heap->size) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   arr = &heap->data[arr_val.value];
   if (arr->len == -1 || arr->hash_slots >= 0) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   if (off < 0 || len < 0 || ((int64_t)off) + ((int64_t)len) > ((int64_t)arr->len)) {
      return FIXSCRIPT_ERR_OUT_OF_BOUNDS;
   }

   if (!float_as_ref) {
      for (i=off; i<len; i++) {
         if (IS_ARRAY(arr, i)) {
            value = get_array_value(arr, i);
            if (value == 0 || ((unsigned int)value) >= (1 << 23)) {
               continue;
            }
            *result = 1;
            return FIXSCRIPT_SUCCESS;
         }
      }
      *result = 0;
      return FIXSCRIPT_SUCCESS;
   }

   *result = !flags_is_array_clear_in_range(arr, off, len);
   return FIXSCRIPT_SUCCESS;
}


int fixscript_copy_array(Heap *heap, Value dest, int dest_off, Value src, int src_off, int count)
{
   int buf_size = 1024;
   Array *dest_arr, *src_arr;
   Value *values;
   int i, num, err = FIXSCRIPT_SUCCESS;

   if (!fixscript_is_array(heap, dest) || !fixscript_is_array(heap, src)) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   dest_arr = &heap->data[dest.value];
   src_arr = &heap->data[src.value];

   if (dest_arr->is_const) {
      return FIXSCRIPT_ERR_CONST_WRITE;
   }

   if (dest_off < 0 || src_off < 0 || count < 0) {
      return FIXSCRIPT_ERR_OUT_OF_BOUNDS;
   }

   if ((int64_t)dest_off + count > dest_arr->len) {
      return FIXSCRIPT_ERR_OUT_OF_BOUNDS;
   }
   if ((int64_t)src_off + count > src_arr->len) {
      return FIXSCRIPT_ERR_OUT_OF_BOUNDS;
   }

   if (!dest_arr->is_shared && dest_arr->type != src_arr->type) {
      switch (src_arr->type) {
         case ARR_SHORT:
            if (dest_arr->type == ARR_BYTE) {
               upgrade_array(heap, dest_arr, dest.value, 0xFFFF);
            }
            break;

         case ARR_INT:
            upgrade_array(heap, dest_arr, dest.value, -1);
            break;
      }
   }

#define SHARED_COPY(data_field) \
   for (i=0; i<count; i++) { \
      uint32_t value = src_arr->data_field[src_off+i]; \
      if (IS_ARRAY(src_arr, src_off+i)) { \
         if (value > 0 && value < (1 << 23)) { \
            return FIXSCRIPT_ERR_INVALID_SHARED_ARRAY_OPERATION; \
         } \
      } \
      dest_arr->data_field[dest_off+i] = value; \
   }

   if (dest_arr->type == src_arr->type) {
      if (!src_arr->is_shared && dest_arr->is_shared && !flags_is_array_clear_in_range(src_arr, src_off, count)) {
         switch (dest_arr->type) {
            case ARR_BYTE:  SHARED_COPY(byte_data); break;
            case ARR_SHORT: SHARED_COPY(short_data); break;
            case ARR_INT:   SHARED_COPY(data); break;
         }
         return err;
      }

      switch (dest_arr->type) {
         case ARR_BYTE:  memmove(dest_arr->byte_data + dest_off, src_arr->byte_data + src_off, count); break;
         case ARR_SHORT: memmove(dest_arr->short_data + dest_off, src_arr->short_data + src_off, count*2); break;
         case ARR_INT:   memmove(dest_arr->data + dest_off, src_arr->data + src_off, count*4); break;
      }
      if (src_arr->is_shared) {
         if (!dest_arr->is_shared) {
            flags_clear_range(dest_arr, dest_off, count);
         }
      }
      else {
         if (!dest_arr->is_shared) {
            if (flags_is_array_clear_in_range(src_arr, src_off, count)) {
               flags_clear_range(dest_arr, dest_off, count);
            }
            else {
               flags_copy_range(dest_arr, dest_off, src_arr, src_off, count);
            }
         }
      }
      return err;
   }

#undef SHARED_COPY

   values = malloc_array(MIN(count, buf_size), sizeof(Value));
   if (!values) {
      return FIXSCRIPT_ERR_OUT_OF_MEMORY;
   }

   if (dest.value == src.value && dest_off > src_off) {
      while (count > 0) {
         num = MIN(count, buf_size);

         err = fixscript_get_array_range(heap, src, src_off + count - num, num, values);
         if (err) break;

         err = fixscript_set_array_range(heap, dest, dest_off + count - num, num, values);
         if (err) break;

         count -= num;
      }
   }
   else {
      while (count > 0) {
         num = MIN(count, buf_size);

         err = fixscript_get_array_range(heap, src, src_off, num, values);
         if (err) break;

         err = fixscript_set_array_range(heap, dest, dest_off, num, values);
         if (err) break;

         src_off += num;
         dest_off += num;
         count -= num;
      }
   }

   free(values);
   return err;
}


int fixscript_lock_array(Heap *heap, Value arr_val, int off, int len, void **data, int elem_size, int access)
{
   Array *arr;
   int i, arr_elem, value;
   char *buf;

   if (!arr_val.is_array || arr_val.value <= 0 || arr_val.value >= heap->size) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   arr = &heap->data[arr_val.value];
   if (arr->len == -1 || arr->hash_slots >= 0) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   if (off < 0 || len < 0 || ((int64_t)off) + ((int64_t)len) > ((int64_t)arr->len)) {
      return FIXSCRIPT_ERR_OUT_OF_BOUNDS;
   }

   if (elem_size != 1 && elem_size != 2 && elem_size != 4) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   add_root(heap, arr_val);

   arr_elem = arr->type == ARR_BYTE? 1 : arr->type == ARR_SHORT? 2 : 4;
   if (arr_elem == elem_size) {
      *data = arr->byte_data + (intptr_t)off*(intptr_t)elem_size;
      return FIXSCRIPT_SUCCESS;
   }

   if ((int64_t)len * (int64_t)elem_size > INT_MAX) {
      return FIXSCRIPT_ERR_OUT_OF_BOUNDS;
   }

   buf = malloc(len * elem_size);
   if (!buf) {
      return FIXSCRIPT_ERR_OUT_OF_MEMORY;
   }

   if (access == ACCESS_WRITE_ONLY) {
      *data = buf;
      return FIXSCRIPT_SUCCESS;
   }

   if (arr_elem == elem_size) {
      memcpy(buf, arr->byte_data + (intptr_t)off*(intptr_t)elem_size, (intptr_t)len*(intptr_t)elem_size);
   }
   else {
      if (elem_size == 1) {
         for (i=0; i<len; i++) {
            value = get_array_value(arr, off+i);
            if (value < 0 || value > 255) {
               free(buf);
               return FIXSCRIPT_ERR_INVALID_BYTE_ARRAY;
            }
            buf[i] = value;
         }
      }
      else if (elem_size == 2) {
         for (i=0; i<len; i++) {
            value = get_array_value(arr, off+i);
            if (value < 0 || value > 65535) {
               free(buf);
               return FIXSCRIPT_ERR_INVALID_SHORT_ARRAY;
            }
            ((uint16_t *)buf)[i] = value;
         }
      }
      else {
         for (i=0; i<len; i++) {
            ((int *)buf)[i] = get_array_value(arr, off+i);
         }
      }
   }

   *data = buf;
   return FIXSCRIPT_SUCCESS;
}


void fixscript_unlock_array(Heap *heap, Value arr_val, int off, int len, void **data, int elem_size, int access)
{
   Array *arr;
   int i, arr_elem;
   char *buf = *data;

   *data = NULL;

   arr = &heap->data[arr_val.value];
   arr_elem = arr->type == ARR_BYTE? 1 : arr->type == ARR_SHORT? 2 : 4;

   if (arr_elem == elem_size) {
      if (!arr->is_shared && access != ACCESS_READ_ONLY) {
         flags_clear_range(arr, off, len);
      }
      return;
   }

   if (access == ACCESS_READ_ONLY) {
      free(buf);
      return;
   }

   if (arr_elem == elem_size) {
      memcpy(arr->byte_data + (intptr_t)off*(intptr_t)elem_size, buf, (intptr_t)len*(intptr_t)elem_size);
   }
   else {
      if (elem_size == 1) {
         for (i=0; i<len; i++) {
            set_array_value(arr, off+i, buf[i]);
         }
      }
      else if (elem_size == 2) {
         for (i=0; i<len; i++) {
            set_array_value(arr, off+i, ((uint16_t *)buf)[i]);
         }
      }
      else {
         for (i=0; i<len; i++) {
            set_array_value(arr, off+i, ((int *)buf)[i]);
         }
      }
      if (!arr->is_shared) {
         flags_clear_range(arr, off, len);
      }
   }
   free(buf);
}


void fixscript_ref_shared_array(SharedArrayHandle *sah)
{
   if (!sah) return;

   if (sah->refcnt < SAH_REFCNT_LIMIT) {
      __sync_add_and_fetch(&sah->refcnt, 1);
   }
}


void fixscript_unref_shared_array(SharedArrayHandle *sah)
{
   if (!sah) return;

   if (sah->refcnt < SAH_REFCNT_LIMIT && __sync_sub_and_fetch(&sah->refcnt, 1) == 0) {
      if (sah->free_func) {
         sah->free_func(sah->free_data);
      }
      free(sah);
   }
}


int fixscript_get_shared_array_reference_count(SharedArrayHandle *sah)
{
   if (!sah) return 0;
   return sah->refcnt;
}


SharedArrayHandle *fixscript_get_shared_array_handle(Heap *heap, Value arr_val, int expected_type, int *actual_type)
{
   SharedArrayHandle *sah;
   Array *arr;

   if (!arr_val.is_array || arr_val.value <= 0 || arr_val.value >= heap->size) {
      return NULL;
   }

   arr = &heap->data[arr_val.value];
   if (arr->len == -1 || arr->hash_slots >= 0 || !arr->is_shared) {
      return NULL;
   }

   sah = ARRAY_SHARED_HEADER(arr);
   if (expected_type >= 0 && sah->type != expected_type) {
      return NULL;
   }
   if (actual_type) *actual_type = sah->type;
   return sah;
}


void *fixscript_get_shared_array_handle_data(SharedArrayHandle *sah, int *len, int *elem_size, void **data, int expected_type, int *actual_type)
{
   if (expected_type >= 0 && sah->type != expected_type) {
      return NULL;
   }
   if (len) *len = sah->len;
   if (elem_size) *elem_size = sah->elem_size;
   if (data) *data = sah->free_data;
   if (actual_type) *actual_type = sah->type;
   return sah->ptr;
}


Value fixscript_get_shared_array_value(Heap *heap, SharedArrayHandle *sah)
{
   return create_shared_array_from(heap, sah->type, sah->ptr, sah->len, sah->elem_size, sah->free_func, sah->free_data, NULL, sah);
}


Value fixscript_get_shared_array(Heap *heap, int type, void *ptr, int len, int elem_size, void *data)
{
   Value value;
   char buf[128];

   snprintf(buf, sizeof(buf), "%d,%p,%d,%d,%p", type, ptr, len, elem_size, data);
   value.value = (intptr_t)string_hash_get(&heap->shared_arrays, buf);
   if (value.value) {
      value.is_array = 1;
      add_root(heap, value);
      return value;
   }
   return fixscript_int(0);
}


void *fixscript_get_shared_array_data(Heap *heap, Value arr_val, int *len, int *elem_size, void **data, int expected_type, int *actual_type)
{
   SharedArrayHandle *sah;
   Array *arr;

   if (!arr_val.is_array || arr_val.value <= 0 || arr_val.value >= heap->size) {
      return NULL;
   }

   arr = &heap->data[arr_val.value];
   if (arr->len == -1 || arr->hash_slots >= 0 || !arr->is_shared) {
      return NULL;
   }

   sah = ARRAY_SHARED_HEADER(arr);
   if (expected_type >= 0 && sah->type != expected_type) {
      return NULL;
   }
   if (len) *len = arr->len;
   if (elem_size) *elem_size = arr->type == ARR_BYTE? 1 : arr->type == ARR_SHORT? 2 : 4;
   if (data) *data = sah->free_data;
   if (actual_type) *actual_type = sah->type;
   return arr->data;
}


int fixscript_is_shared_array(Heap *heap, Value arr_val)
{
   return fixscript_get_shared_array_data(heap, arr_val, NULL, NULL, NULL, -1, NULL) != NULL;
}


Value fixscript_create_string(Heap *heap, const char *s, int len)
{
   Value arr_val;
   Array *arr;
   int i, j, dest_len = 0;
   unsigned int c, max_value = 0;
   unsigned char c2, c3, c4;
   
   if (!s && len != 0) {
      return fixscript_int(0);
   }

   for (i=0; i < len || (len < 0 && s[i]); i++) {
      c = (unsigned char)s[i];
      if ((c & 0x80) == 0) {
         // nothing
      }
      else if ((c & 0xE0) == 0xC0 && (i+1 < len || (len < 0 && s[i+1]))) {
         c2 = s[++i];
         c = ((c & 0x1F) << 6) | (c2 & 0x3F);
         if (c < 0x80) {
            c = 0xFFFD;
         }
      }
      else if ((c & 0xF0) == 0xE0 && (i+2 < len || (len < 0 && s[i+1] && s[i+2]))) {
         c2 = s[++i];
         c3 = s[++i];
         c = ((c & 0x0F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
         if (c < 0x800) {
            c = 0xFFFD;
         }
      }
      else if ((c & 0xF8) == 0xF0 && (i+3 < len || (len < 0 && s[i+1] && s[i+2] && s[i+3]))) {
         c2 = s[++i];
         c3 = s[++i];
         c4 = s[++i];
         c = ((c & 0x07) << 18) | ((c2 & 0x3F) << 12) | ((c3 & 0x3F) << 6) | (c4 & 0x3F);
         if (c < 0x10000 || c > 0x10FFFF) {
            c = 0xFFFD;
         }
      }
      else {
         c = 0xFFFD;
      }

      if (c >= 0xD800 && c <= 0xDFFF) {
         c = 0xFFFD;
      }

      if (c > max_value) {
         max_value = c;
      }
      dest_len++;
   }

   if (len < 0) {
      len = i;
   }

   arr_val = create_array(heap, max_value > 0xFFFF? ARR_INT : max_value > 0xFF? ARR_SHORT : ARR_BYTE, dest_len);
   if (!arr_val.is_array) return arr_val;
   add_root(heap, arr_val);
   arr = &heap->data[arr_val.value];
   arr->len = dest_len;
   arr->is_string = 1;
   memset(arr->flags, 0, FLAGS_SIZE(dest_len) * sizeof(int));

   for (i=0, j=0; i<len; i++, j++) {
      c = s[i];
      if ((c & 0x80) == 0) {
         // nothing
      }
      else if ((c & 0xE0) == 0xC0 && (i+1 < len || (len < 0 && s[i+1]))) {
         c2 = s[++i];
         c = ((c & 0x1F) << 6) | (c2 & 0x3F);
      }
      else if ((c & 0xF0) == 0xE0 && (i+2 < len || (len < 0 && s[i+1] && s[i+2]))) {
         c2 = s[++i];
         c3 = s[++i];
         c = ((c & 0x0F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
      }
      else if ((c & 0xF8) == 0xF0 && (i+3 < len || (len < 0 && s[i+1] && s[i+2] && s[i+3]))) {
         c2 = s[++i];
         c3 = s[++i];
         c4 = s[++i];
         c = ((c & 0x07) << 18) | ((c2 & 0x3F) << 12) | ((c3 & 0x3F) << 6) | (c4 & 0x3F);
      }
      else {
         c = 0xFFFD;
      }

      if (c >= 0xD800 && c <= 0xDFFF) {
         c = 0xFFFD;
      }

      set_array_value(arr, j, c);
   }
   return arr_val;
}


Value fixscript_create_string_utf16(Heap *heap, const unsigned short *s, int len)
{
   Value arr_val;
   Array *arr;
   int i, j, dest_len = 0;
   unsigned int c, c2, max_value = 0;
   
   if (!s && len != 0) {
      return fixscript_int(0);
   }

   for (i=0; i < len || (len < 0 && s[i]); i++) {
      c = s[i];
      if (c >= 0xD800 && c <= 0xDBFF && (i+1 < len || (len < 0 && s[i+1]))) {
         c2 = s[i+1];
         if (c2 >= 0xDC00 && c2 <= 0xDFFF) {
            c = ((c - 0xD800) << 10) | (c2 - 0xDC00);
            i++;
         }
      }
      if (c >= 0xD800 && c <= 0xDFFF) {
         c = 0xFFFD;
      }
      if (c > max_value) {
         max_value = c;
      }
      dest_len++;
   }

   if (len < 0) {
      len = i;
   }

   arr_val = create_array(heap, max_value > 0xFFFF? ARR_INT : max_value > 0xFF? ARR_SHORT : ARR_BYTE, dest_len);
   if (!arr_val.is_array) return arr_val;
   add_root(heap, arr_val);
   arr = &heap->data[arr_val.value];
   arr->len = dest_len;
   arr->is_string = 1;
   memset(arr->flags, 0, FLAGS_SIZE(dest_len) * sizeof(int));

   for (i=0, j=0; i<len; i++, j++) {
      c = s[i];
      if (c >= 0xD800 && c <= 0xDBFF && (i+1 < len)) {
         c2 = s[i+1];
         if (c2 >= 0xDC00 && c2 <= 0xDFFF) {
            c = 0x10000 + (((c - 0xD800) << 10) | (c2 - 0xDC00));
            i++;
         }
      }
      if (c >= 0xD800 && c <= 0xDFFF) {
         c = 0xFFFD;
      }
      set_array_value(arr, j, c);
   }
   return arr_val;
}


int fixscript_get_string(Heap *heap, Value str_val, int str_off, int str_len, char **str_out, int *len_out)
{
   Array *arr;
   char *s;
   int64_t len64;
   int i, len, c;

   if (!str_val.is_array || str_val.value <= 0 || str_val.value >= heap->size) {
      *str_out = NULL;
      if (len_out) *len_out = 0;
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   arr = &heap->data[str_val.value];
   if (arr->len == -1 || arr->hash_slots >= 0) {
      *str_out = NULL;
      if (len_out) *len_out = 0;
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   if (str_len < 0) {
      str_len = arr->len;
   }

   if (str_off < 0 || str_len < 0 || (int64_t)str_off + (int64_t)str_len > arr->len) {
      *str_out = NULL;
      if (len_out) *len_out = 0;
      return FIXSCRIPT_ERR_OUT_OF_BOUNDS;
   }

   for (i=0, len64=0; i<str_len; i++, len64++) {
      c = get_array_value(arr, str_off+i);
      if (c == 0 && !len_out) {
         *str_out = NULL;
         if (len_out) *len_out = 0;
         return FIXSCRIPT_ERR_INVALID_NULL_STRING;
      }
      
      if (c < 0 || c > 0x10FFFF) {
         c = 0xFFFD;
      }
      if (c >= 0xD800 && c <= 0xDFFF) {
         c = 0xFFFD;
      }
      
      if (c >= 0x10000) {
         len64 += 3;
      }
      else if (c >= 0x800) {
         len64 += 2;
      }
      else if (c >= 0x80) {
         len64++;
      }
   }

   if (len64 > INT_MAX-1) {
      *str_out = NULL;
      if (len_out) *len_out = 0;
      return FIXSCRIPT_ERR_OUT_OF_MEMORY;
   }
   len = len64;

   s = malloc(len+1);
   if (!s) {
      *str_out = NULL;
      if (len_out) *len_out = 0;
      return FIXSCRIPT_ERR_OUT_OF_MEMORY;
   }

   for (i=0, len=0; i<str_len; i++) {
      c = get_array_value(arr, str_off+i);
      
      if (c < 0 || c > 0x10FFFF) {
         c = 0xFFFD;
      }
      if (c >= 0xD800 && c <= 0xDFFF) {
         c = 0xFFFD;
      }
      
      if (c >= 0x10000) {
         s[len++] = (c >> 18) | 0xF0;
         s[len++] = ((c >> 12) & 0x3F) | 0x80;
         s[len++] = ((c >> 6) & 0x3F) | 0x80;
         s[len++] = (c & 0x3F) | 0x80;
      }
      else if (c >= 0x800) {
         s[len++] = (c >> 12) | 0xE0;
         s[len++] = ((c >> 6) & 0x3F) | 0x80;
         s[len++] = (c & 0x3F) | 0x80;
      }
      else if (c >= 0x80) {
         s[len++] = (c >> 6) | 0xC0;
         s[len++] = (c & 0x3F) | 0x80;
      }
      else {
         s[len++] = c;
      }
   }

   s[len] = 0;
   *str_out = s;
   if (len_out) *len_out = len;
   return FIXSCRIPT_SUCCESS;
}


int fixscript_get_string_utf16(Heap *heap, Value str_val, int str_off, int str_len, unsigned short **str_out, int *len_out)
{
   Array *arr;
   unsigned short *s;
   int64_t len64;
   int i, len, c;

   if (!str_val.is_array || str_val.value <= 0 || str_val.value >= heap->size) {
      *str_out = NULL;
      if (len_out) *len_out = 0;
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   arr = &heap->data[str_val.value];
   if (arr->len == -1 || arr->hash_slots >= 0) {
      *str_out = NULL;
      if (len_out) *len_out = 0;
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   if (str_len < 0) {
      str_len = arr->len;
   }

   if (str_off < 0 || str_len < 0 || (int64_t)str_off + (int64_t)str_len > arr->len) {
      *str_out = NULL;
      if (len_out) *len_out = 0;
      return FIXSCRIPT_ERR_OUT_OF_BOUNDS;
   }

   for (i=0, len=0; i<str_len; i++, len++) {
      c = get_array_value(arr, str_off+i);
      if (c == 0 && !len_out) {
         *str_out = NULL;
         if (len_out) *len_out = 0;
         return FIXSCRIPT_ERR_INVALID_NULL_STRING;
      }

      if (c < 0 || c > 0x10FFFF) {
         c = 0xFFFD;
      }
      if (c >= 0xD800 && c <= 0xDFFF) {
         c = 0xFFFD;
      }
      if (c > 0xFFFF) {
         len++;
      }
   }

   len64 = (((int64_t)len)+1) * sizeof(unsigned short);
   if (len64 > INT_MAX) {
      *str_out = NULL;
      if (len_out) *len_out = 0;
      return FIXSCRIPT_ERR_OUT_OF_MEMORY;
   }

   s = malloc(len64);
   if (!s) {
      *str_out = NULL;
      if (len_out) *len_out = 0;
      return FIXSCRIPT_ERR_OUT_OF_MEMORY;
   }

   for (i=0, len=0; i<str_len; i++) {
      c = get_array_value(arr, str_off+i);
      if (c < 0 || c > 0x10FFFF) {
         c = 0xFFFD;
      }
      if (c >= 0xD800 && c <= 0xDFFF) {
         c = 0xFFFD;
      }
      if (c > 0xFFFF) {
         c -= 0x10000;
         s[len++] = 0xD800 + (c >> 10);
         s[len++] = 0xDC00 + (c & 0x3FF);
         continue;
      }
      s[len++] = c;
   }
   
   s[len] = 0;
   *str_out = s;
   if (len_out) *len_out = len;
   return FIXSCRIPT_SUCCESS;
}


int fixscript_is_string(Heap *heap, Value str_val)
{
   Array *arr;

   if (!str_val.is_array || str_val.value <= 0 || str_val.value >= heap->size) {
      return 0;
   }

   arr = &heap->data[str_val.value];
   if (arr->len == -1 || arr->hash_slots >= 0) {
      return 0;
   }

   return arr->is_string;
}


int fixscript_get_const_string(Heap *heap, Value str_val, int off, int len, Value *ret)
{
   return fixscript_get_const_string_between(heap, heap, str_val, off, len, ret);
}


int fixscript_get_const_string_between(Heap *dest, Heap *src, Value str_val, int off, int len, Value *ret)
{
   Array *arr, *new_arr;
   Value new_str;
   int i, dest_type;

   if (!str_val.is_array || str_val.value <= 0 || str_val.value >= src->size) {
      *ret = fixscript_int(0);
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   arr = &src->data[str_val.value];
   if (arr->len == -1 || !arr->is_string) {
      *ret = fixscript_int(0);
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   if (len < 0) {
      if (arr->is_const && dest == src) {
         *ret = str_val;
         return FIXSCRIPT_SUCCESS;
      }

      off = 0;
      len = arr->len;
   }
   else {
      if (off < 0 || ((int64_t)off) + ((int64_t)len) > ((int64_t)arr->len)) {
         return FIXSCRIPT_ERR_OUT_OF_BOUNDS;
      }
   }

   if (!flags_is_array_clear_in_range(arr, off, len)) {
      *ret = fixscript_int(0);
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   ret->value = handle_const_string_set(dest, &dest->const_string_set, arr, off, len, 0);
   if (ret->value) {
      if (ret->value == -1) {
         *ret = fixscript_int(0);
         return FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
      ret->is_array = 1;
      return FIXSCRIPT_SUCCESS;
   }

   if (arr->type == ARR_BYTE) {
      dest_type = ARR_BYTE;
   }
   else if (arr->type == ARR_SHORT) {
      dest_type = ARR_BYTE;
      for (i=0; i<len; i++) {
         if (arr->short_data[off+i] > 0xFF) {
            dest_type = ARR_SHORT;
            break;
         }
      }
   }
   else {
      dest_type = ARR_BYTE;
      for (i=0; i<len; i++) {
         if (arr->data[off+i] < 0 || arr->data[off+i] > 0xFFFF) {
            dest_type = ARR_INT;
            break;
         }
         if (arr->data[off+i] > 0xFF) {
            dest_type = ARR_SHORT;
         }
      }
   }

   new_str = create_array(dest, dest_type, len);
   if (!new_str.value) {
      *ret = fixscript_int(0);
      return FIXSCRIPT_ERR_OUT_OF_MEMORY;
   }

   arr = &src->data[str_val.value];
   new_arr = &dest->data[new_str.value];

   new_arr->len = new_arr->size;
   flags_clear_range(new_arr, 0, new_arr->len);

   new_arr->is_string = 1;
   set_const_string(dest, new_str.value);

   if (arr->type == ARR_BYTE) {
      memcpy(new_arr->byte_data, arr->byte_data + off, ((size_t)len) * sizeof(unsigned char));
   }
   else if (arr->type == ARR_SHORT) {
      if (dest_type == ARR_SHORT) {
         memcpy(new_arr->short_data, arr->short_data + off, ((size_t)len) * sizeof(unsigned short));
      }
      else {
         for (i=0; i<len; i++) {
            new_arr->byte_data[i] = arr->short_data[off+i];
         }
      }
   }
   else {
      if (dest_type == ARR_INT) {
         memcpy(new_arr->data, arr->data + off, ((size_t)len) * sizeof(int));
      }
      else if (dest_type == ARR_SHORT) {
         for (i=0; i<len; i++) {
            new_arr->short_data[i] = arr->data[off+i];
         }
      }
      else {
         for (i=0; i<len; i++) {
            new_arr->byte_data[i] = arr->data[off+i];
         }
      }
   }

   if (handle_const_string_set(dest, &dest->const_string_set, new_arr, 0, new_arr->len, new_str.value) != new_str.value) {
      *ret = fixscript_int(0);
      return FIXSCRIPT_ERR_OUT_OF_MEMORY;
   }

   *ret = new_str;
   return FIXSCRIPT_SUCCESS;
}


static Value get_const_string_direct(Heap *heap, Value str)
{
   Array *arr;
   Value ret;

   arr = &heap->data[str.value];
   ret.value = handle_const_string_set(heap, &heap->const_string_set, arr, 0, arr->len, 0);
   if (ret.value) {
      if (ret.value == -1) {
         return fixscript_int(0);
      }
      ret.is_array = 1;
      return ret;
   }

   set_const_string(heap, str.value);
   if (handle_const_string_set(heap, &heap->const_string_set, arr, 0, arr->len, str.value) != str.value) {
      return fixscript_int(0);
   }
   return str;
}


static unsigned int compute_hash(Heap *heap, Value value, int recursion_limit)
{
   Array *arr;
   int i, val;
   unsigned int hash = 0, entry_hash;
   
   if (recursion_limit <= 0) {
      return 0;
   }

   if (value.is_array) {
      if (value.value <= 0 || value.value >= heap->size) {
         return value.value;
      }
      arr = &heap->data[value.value];
      if (arr->len == -1) {
         return value.value;
      }
      
      if (arr->is_handle) {
         if (arr->is_handle == 2) {
            return (unsigned int)(intptr_t)arr->handle_func(heap, HANDLE_OP_HASH, arr->handle_ptr, NULL);
         }
         return value.value;
      }

      if (arr->hash_slots >= 0) {
         for (i=0; i<(1<<arr->size); i+=2) {
            if (HAS_DATA(arr, i+0) && HAS_DATA(arr, i+1)) {
               if (IS_ARRAY(arr, i+0)) {
                  entry_hash = compute_hash(heap, (Value) { arr->data[i+0], 1 }, recursion_limit-1);
               }
               else {
                  entry_hash = arr->data[i+0];
               }

               if (IS_ARRAY(arr, i+1)) {
                  entry_hash = entry_hash * 31 + compute_hash(heap, (Value) { arr->data[i+1], 1 }, recursion_limit-1);
               }
               else {
                  entry_hash = entry_hash * 31 + arr->data[i+1];
               }

               hash ^= entry_hash;
            }
         }
      }
      else {
         for (i=0; i<arr->len; i++) {
            val = get_array_value(arr, i);
            if (IS_ARRAY(arr, i)) {
               val = compute_hash(heap, (Value) { val, 1 }, recursion_limit-1);
            }
            hash = hash*31 + ((unsigned int)val);
         }
      }

      return hash;
   }

   return value.value;
}


int fixscript_is_const_string(Heap *heap, Value str_val)
{
   Array *arr;

   if (!str_val.is_array || str_val.value <= 0 || str_val.value >= heap->size) {
      return 0;
   }

   arr = &heap->data[str_val.value];
   if (arr->len == -1 || arr->hash_slots >= 0) {
      return 0;
   }

   return arr->is_const;
}


static int get_hash_elem(Heap *heap, Array *arr, Heap *key_heap, Value key_val, Value *value_val);

static int compare_values(Heap *heap1, Value value1, Heap *heap2, Value value2, int recursion_limit)
{
   Array *arr1, *arr2;
   Value val1, val2;
   int i;
   
   if (recursion_limit <= 0) {
      return 0;
   }

   if (value1.is_array && !value2.is_array) {
      return 0;
   }

   if (!value1.is_array && value2.is_array) {
      return 0;
   }

   if (value1.is_array) {
      if (value1.value == value2.value && heap1 == heap2) {
         return 1;
      }

      if (fixscript_is_float(value1) && fixscript_is_float(value2)) {
         return fixscript_get_float(value1) == fixscript_get_float(value2);
      }

      if (value1.value <= 0 || value1.value >= heap1->size || value2.value <= 0 || value2.value >= heap2->size) {
         return 0;
      }

      arr1 = &heap1->data[value1.value];
      arr2 = &heap2->data[value2.value];

      if (arr1->len != arr2->len || arr1->len == -1) {
         return 0;
      }

      if ((arr1->hash_slots >= 0 && arr2->hash_slots < 0) || (arr1->hash_slots < 0 && arr2->hash_slots >= 0)) {
         return 0;
      }

      if (arr1->is_handle || arr2->is_handle) {
         if (arr1->is_handle == 2 && arr2->is_handle == 2 && arr1->type == arr2->type) {
            return arr1->handle_func(heap1, HANDLE_OP_COMPARE, arr1->handle_ptr, arr2->handle_ptr) != NULL;
         }
         return 0;
      }

      if (arr1->hash_slots >= 0) {
         for (i=0; i<(1<<arr1->size); i+=2) {
            if (HAS_DATA(arr1, i+0) && HAS_DATA(arr1, i+1)) {
               val1 = (Value) { arr1->data[i+1], IS_ARRAY(arr1, i+1) };
               if (get_hash_elem(heap2, arr2, heap1, (Value) { arr1->data[i+0], IS_ARRAY(arr1, i+0) }, &val2) != FIXSCRIPT_SUCCESS) {
                  return 0;
               }
               
               if (!compare_values(heap1, val1, heap2, val2, recursion_limit-1)) {
                  return 0;
               }
            }
         }
      }
      else {
         for (i=0; i<arr1->len; i++) {
            val1 = (Value) { get_array_value(arr1, i), IS_ARRAY(arr1, i) };
            val2 = (Value) { get_array_value(arr2, i), IS_ARRAY(arr2, i) };
            if (!compare_values(heap1, val1, heap2, val2, recursion_limit-1)) {
               return 0;
            }
         }
      }

      return 1;
   }

   return (value1.value == value2.value);
}


static Value create_hash(Heap *heap)
{
   Value arr_val;
   Array *arr;
   
   arr_val = create_array(heap, ARR_HASH, 3); // 4 entries * 2 = 8 = 1<<3
   if (!arr_val.is_array) return arr_val;
   arr = &heap->data[arr_val.value];
   memset(arr->flags, 0, (FLAGS_SIZE((1<<arr->size) * 2) + bitarray_size(arr->size-1, 1<<arr->size)) * sizeof(int));
   memset(arr->data, 0, (1<<arr->size) * sizeof(int));
   return arr_val;
}


Value fixscript_create_hash(Heap *heap)
{
   Value arr_val;

   arr_val = create_hash(heap);
   add_root(heap, arr_val);
   return arr_val;
}


int fixscript_is_hash(Heap *heap, Value hash_val)
{
   Array *arr;

   if (!hash_val.is_array || hash_val.value <= 0 || hash_val.value >= heap->size) {
      return 0;
   }

   arr = &heap->data[hash_val.value];
   if (arr->len == -1 || arr->hash_slots < 0) {
      return 0;
   }

   return !arr->is_handle;
}


static int expand_hash(Heap *heap, Value hash_val, Array *arr)
{
   Array old;
   int i, err, idx;
   int old_flags_size, new_flags_size;
   int new_size;
   int *new_flags;
   int *new_data;

   old = *arr;

   new_size = arr->size;
   if (arr->len >= ((1<<new_size) >> 2)) {
      if (new_size >= 30) return FIXSCRIPT_ERR_OUT_OF_MEMORY;
      new_size++;
   }

   if (new_size >= 30) return FIXSCRIPT_ERR_OUT_OF_MEMORY;

   old_flags_size = FLAGS_SIZE((1<<arr->size) * 2) + bitarray_size(arr->size-1, 1<<arr->size);
   new_flags_size = FLAGS_SIZE((1<<new_size) * 2) + bitarray_size(new_size-1, 1<<new_size);

   new_flags = calloc(new_flags_size, sizeof(int));
   if (!new_flags) {
      return FIXSCRIPT_ERR_OUT_OF_MEMORY;
   }

   new_data = calloc((1<<new_size), sizeof(int));
   if (!new_data) {
      free(new_flags);
      return FIXSCRIPT_ERR_OUT_OF_MEMORY;
   }

   heap->total_size += (int64_t)(new_flags_size - old_flags_size) * sizeof(int);
   heap->total_size += (int64_t)((1<<new_size) - (1<<arr->size)) * sizeof(int);

   arr->len = 0;
   arr->size = new_size;
   arr->flags = new_flags;
   arr->data = new_data;
   arr->hash_slots = 0;

   for (i=0; i<old.hash_slots; i++) {
      idx = bitarray_get(&old.flags[FLAGS_SIZE((1<<old.size)*2)], old.size-1, i) << 1;

      if (HAS_DATA(&old, idx+0) && HAS_DATA(&old, idx+1)) {
         err = fixscript_set_hash_elem(heap, hash_val, (Value) { old.data[idx+0], IS_ARRAY(&old, idx+0) }, (Value) { old.data[idx+1], IS_ARRAY(&old, idx+1) });
         if (err != FIXSCRIPT_SUCCESS) {
            free(arr->flags);
            free(arr->data);
            *arr = old;
            return err;
         }
      }
   }

   free(old.flags);
   free(old.data);
   return FIXSCRIPT_SUCCESS;
}


static int set_hash_elem(Heap *heap, Value hash_val, Value key_val, Value value_val, int *key_was_present)
{
   Array *arr;
   int idx, err, mask;

   if (!hash_val.is_array || hash_val.value <= 0 || hash_val.value >= heap->size) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   arr = &heap->data[hash_val.value];
   if (arr->len == -1 || arr->hash_slots < 0 || arr->is_handle) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   if (arr->hash_slots >= ((1<<arr->size) >> 2)) {
      err = expand_hash(heap, hash_val, arr);
      if (err != FIXSCRIPT_SUCCESS) return err;
   }

   mask = (1<<arr->size)-1;
   idx = (rehash(compute_hash(heap, key_val, MAX_COMPARE_RECURSION)) << 1) & mask;
   for (;;) {
      if (!HAS_DATA(arr, idx+0)) break;

      if (HAS_DATA(arr, idx+1) && compare_values(heap, (Value) { arr->data[idx+0], IS_ARRAY(arr, idx+0) }, heap, key_val, MAX_COMPARE_RECURSION)) {
         arr->data[idx+1] = value_val.value;
         ASSIGN_IS_ARRAY(arr, idx+1, value_val.is_array);
         if (key_was_present) {
            *key_was_present = 1;
         }
         return FIXSCRIPT_SUCCESS;
      }

      idx = (idx+2) & mask;
   }

   bitarray_set(&arr->flags[FLAGS_SIZE((1<<arr->size)*2)], arr->size-1, arr->hash_slots, idx >> 1);

   arr->len++;
   arr->hash_slots++;
   SET_HAS_DATA(arr, idx+0);
   SET_HAS_DATA(arr, idx+1);

   arr->data[idx+0] = key_val.value;
   ASSIGN_IS_ARRAY(arr, idx+0, key_val.is_array);

   arr->data[idx+1] = value_val.value;
   ASSIGN_IS_ARRAY(arr, idx+1, value_val.is_array);
   return FIXSCRIPT_SUCCESS;
}


int fixscript_set_hash_elem(Heap *heap, Value hash_val, Value key_val, Value value_val)
{
   return set_hash_elem(heap, hash_val, key_val, value_val, NULL);
}


static int get_hash_elem(Heap *heap, Array *arr, Heap *key_heap, Value key_val, Value *value_val)
{
   int idx, mask;

   mask = (1<<arr->size)-1;
   idx = (rehash(compute_hash(key_heap, key_val, MAX_COMPARE_RECURSION)) << 1) & mask;

   for (;;) {
      if (!HAS_DATA(arr, idx+0)) break;

      if (HAS_DATA(arr, idx+1) && compare_values(heap, (Value) { arr->data[idx+0], IS_ARRAY(arr, idx+0) }, key_heap, key_val, MAX_COMPARE_RECURSION)) {
         if (value_val) {
            *value_val = (Value) { arr->data[idx+1], IS_ARRAY(arr, idx+1) != 0 };
         }
         return FIXSCRIPT_SUCCESS;
      }

      idx = (idx+2) & mask;
   }

   if (value_val) {
      *value_val = fixscript_int(0);
   }
   return FIXSCRIPT_ERR_KEY_NOT_FOUND;
}


int fixscript_get_hash_elem(Heap *heap, Value hash_val, Value key_val, Value *value_val)
{
   return fixscript_get_hash_elem_between(heap, hash_val, heap, key_val, value_val);
}


int fixscript_get_hash_elem_between(Heap *heap, Value hash_val, Heap *key_heap, Value key_val, Value *value_val)
{
   Array *arr;

   if (!hash_val.is_array || hash_val.value <= 0 || hash_val.value >= heap->size) {
      if (value_val) {
         *value_val = fixscript_int(0);
      }
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   arr = &heap->data[hash_val.value];
   if (arr->len == -1 || arr->hash_slots < 0 || arr->is_handle) {
      if (value_val) {
         *value_val = fixscript_int(0);
      }
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   return get_hash_elem(heap, arr, key_heap, key_val, value_val);
}


int fixscript_remove_hash_elem(Heap *heap, Value hash_val, Value key_val, Value *value_val)
{
   Array *arr;
   int idx, mask;

   if (!hash_val.is_array || hash_val.value <= 0 || hash_val.value >= heap->size) {
      if (value_val) {
         *value_val = fixscript_int(0);
      }
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   arr = &heap->data[hash_val.value];
   if (arr->len == -1 || arr->hash_slots < 0 || arr->is_handle) {
      if (value_val) {
         *value_val = fixscript_int(0);
      }
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   mask = (1<<arr->size)-1;
   idx = (rehash(compute_hash(heap, key_val, MAX_COMPARE_RECURSION)) << 1) & mask;

   for (;;) {
      if (!HAS_DATA(arr, idx+0)) break;

      if (HAS_DATA(arr, idx+1) && compare_values(heap, (Value) { arr->data[idx+0], IS_ARRAY(arr, idx+0) }, heap, key_val, MAX_COMPARE_RECURSION)) {
         if (value_val) {
            *value_val = (Value) { arr->data[idx+1], IS_ARRAY(arr, idx+1) != 0 };
         }
         CLEAR_HAS_DATA(arr, idx+1);
         CLEAR_IS_ARRAY(arr, idx+0);
         CLEAR_IS_ARRAY(arr, idx+1);
         arr->data[idx+0] = 0;
         arr->data[idx+1] = 0;
         arr->len--;
         return FIXSCRIPT_SUCCESS;
      }

      idx = (idx+2) & mask;
   }

   if (value_val) {
      *value_val = fixscript_int(0);
   }
   return FIXSCRIPT_ERR_KEY_NOT_FOUND;
}


int fixscript_clear_hash(Heap *heap, Value hash_val)
{
   Array *arr;

   if (!hash_val.is_array || hash_val.value <= 0 || hash_val.value >= heap->size) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   arr = &heap->data[hash_val.value];
   if (arr->len == -1 || arr->hash_slots < 0 || arr->is_handle) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   memset(arr->flags, 0, (FLAGS_SIZE((1<<arr->size)*2) + bitarray_size(arr->size-1, 1<<arr->size)) * sizeof(int));
   memset(arr->data, 0, (1<<arr->size) * sizeof(int));
   arr->len = 0;
   arr->hash_slots = 0;
   return FIXSCRIPT_SUCCESS;
}


int fixscript_iter_hash(Heap *heap, Value hash_val, Value *key_val, Value *value_val, int *pos)
{
   Array *arr;
   int size, idx;

   if (!hash_val.is_array || hash_val.value <= 0 || hash_val.value >= heap->size) {
      return 0;
   }
   arr = &heap->data[hash_val.value];
   if (arr->len == -1 || arr->hash_slots < 0 || arr->is_handle) {
      return 0;
   }

   size = arr->hash_slots;

   while (*pos < size) {
      idx = bitarray_get(&arr->flags[FLAGS_SIZE((1<<arr->size)*2)], arr->size-1, *pos) << 1;
      if (HAS_DATA(arr, idx+0) && HAS_DATA(arr, idx+1)) break;
      (*pos)++;
   }

   if (*pos >= size) {
      return 0;
   }

   *key_val = (Value) { arr->data[idx+0], IS_ARRAY(arr, idx+0) != 0 };
   *value_val = (Value) { arr->data[idx+1], IS_ARRAY(arr, idx+1) != 0 };
   (*pos)++;
   return 1;
}


Value fixscript_create_handle(Heap *heap, int type, void *handle, HandleFreeFunc free_func)
{
   Value handle_val;
   Array *arr;

   if (!handle) {
      return fixscript_int(0);
   }

   if (type < 0) {
      if (free_func) {
         free_func(handle);
      }
      return fixscript_int(0);
   }

   handle_val = create_array(heap, type, 0);
   if (!handle_val.is_array) {
      if (free_func) {
         free_func(handle);
      }
      return handle_val;
   }
   add_root(heap, handle_val);
   arr = &heap->data[handle_val.value];
   arr->is_handle = 1;
   arr->handle_free = free_func;
   arr->handle_ptr = handle;
   heap->handle_created = 1;
   return handle_val;
}


Value fixscript_create_value_handle(Heap *heap, int type, void *handle, HandleFunc handle_func)
{
   Value handle_val;
   Array *arr;

   if (!handle) {
      return fixscript_int(0);
   }

   handle_val = fixscript_create_handle(heap, type, handle, NULL);
   if (!handle_val.value) {
      handle_func(heap, HANDLE_OP_FREE, handle, NULL);
      return handle_val;
   }

   arr = &heap->data[handle_val.value];
   arr->is_handle = 2;
   arr->handle_func = handle_func;
   return handle_val;
}


void *fixscript_get_handle(Heap *heap, Value handle_val, int expected_type, int *actual_type)
{
   Array *arr;

   if (!handle_val.is_array || handle_val.value <= 0 || handle_val.value >= heap->size) {
      if (actual_type) {
         *actual_type = -1;
      }
      return NULL;
   }

   arr = &heap->data[handle_val.value];
   if (arr->len == -1 || arr->hash_slots < 0 || !arr->is_handle) {
      if (actual_type) {
         *actual_type = -1;
      }
      return NULL;
   }

   if (actual_type) {
      *actual_type = arr->type;
   }
   if (expected_type >= 0 && arr->type != expected_type) {
      return NULL;
   }
   return arr->handle_ptr;
}


void fixscript_register_handle_types(volatile int *offset, int count)
{
   int new_offset;
   
   if (*offset == 0) {
      new_offset = __sync_sub_and_fetch(&native_handles_alloc_cnt, count);
      (void)__sync_val_compare_and_swap(offset, 0, new_offset);
   }
}


int fixscript_is_handle(Heap *heap, Value handle_val)
{
   return fixscript_get_handle(heap, handle_val, -1, NULL) != NULL;
}


int fixscript_create_weak_ref(Heap *heap, Value value, Value *container, Value *key, Value *weak_ref)
{
   Array *arr;
   WeakRefHandle *handle = NULL, *hash_handle = NULL;
   Value handle_val;
   int is_hash=0, is_array=0, len, err;
   char buf[16];

   if (!value.value) {
      *weak_ref = fixscript_int(0);
      return FIXSCRIPT_SUCCESS;
   }

   if (!value.is_array || value.value <= 0 || value.value >= heap->size) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   if (fixscript_is_weak_ref(heap, value)) {
      return FIXSCRIPT_ERR_NESTED_WEAKREF;
   }

   if (container) {
      is_hash = fixscript_is_hash(heap, *container);
      is_array = fixscript_is_array(heap, *container);
      if (!is_hash && !is_array) {
         return FIXSCRIPT_ERR_INVALID_ACCESS;
      }
      if (key && fixscript_is_weak_ref(heap, *key)) {
         return FIXSCRIPT_ERR_NESTED_WEAKREF;
      }
   }

   if (key && !container) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   arr = &heap->data[value.value];
   if (arr->len == -1) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   snprintf(buf, sizeof(buf), "%d", value.value);

   if (arr->has_weak_refs) {
      hash_handle = string_hash_get(&heap->weak_refs, buf);
      for (handle = hash_handle; handle; handle = handle->next) {
         if (!container && handle->container == 0) break;
         if (container && handle->container == container->value) {
            if (!key && handle->key.is_array == 2) break;
            if (key && handle->key.value == key->value && handle->key.is_array == key->is_array) {
               break;
            }
         }
      }
   }
   
   if (!handle) {
      if (is_array) {
         err = fixscript_append_array_elem(heap, *container, fixscript_int(-1));
         if (err) return err;

         err = fixscript_get_array_length(heap, *container, &len);
         if (err) return err;

         err = fixscript_set_array_length(heap, *container, len-1);
         if (err) return err;
      }

      handle = calloc(1, sizeof(WeakRefHandle));
      if (!handle) {
         return FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
      handle->id = heap->weak_id_cnt++;
      handle->target = value.value;
      handle->container = container? container->value : 0;
      if (key) {
         handle->key = *key;
      }
      else {
         handle->key.is_array = 2;
      }
      handle->next = hash_handle;
      string_hash_set(&heap->weak_refs, strdup(buf), handle);
      arr->has_weak_refs = 1;
      
      handle_val = fixscript_create_value_handle(heap, WEAK_REF_HANDLE_TYPE, handle, weak_ref_handle_func);
      if (!handle_val.value) {
         return FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }

      handle->value = handle_val.value;
   }

   *weak_ref = (Value) { handle->value, 1 };
   return FIXSCRIPT_SUCCESS;
}


int fixscript_get_weak_ref(Heap *heap, Value weak_ref, Value *value)
{
   WeakRefHandle *handle;

   if (!weak_ref.value) {
      *value = fixscript_int(0);
      return FIXSCRIPT_SUCCESS;
   }
   
   handle = fixscript_get_handle(heap, weak_ref, WEAK_REF_HANDLE_TYPE, NULL);
   if (!handle) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   if (handle->target) {
      *value = (Value) { handle->target, 1 };
   }
   else {
      *value = fixscript_int(0);
   }
   return FIXSCRIPT_SUCCESS;
}


int fixscript_is_weak_ref(Heap *heap, Value weak_ref)
{
   return fixscript_get_handle(heap, weak_ref, WEAK_REF_HANDLE_TYPE, NULL) != NULL;
}


static inline int read_byte(unsigned char **ptr, unsigned char *end, int *value)
{
   if (end - (*ptr) < 1) {
      return 0;
   }
   *value = (*ptr)[0];
   (*ptr)++;
   return 1;
}


static inline int read_short(unsigned char **ptr, unsigned char *end, int *value)
{
   if (end - (*ptr) < 2) {
      return 0;
   }
   *value = (*ptr)[0] | ((*ptr)[1] << 8);
   (*ptr) += 2;
   return 1;
}


static inline int read_int(unsigned char **ptr, unsigned char *end, int *value)
{
   if (end - (*ptr) < 4) {
      return 0;
   }
   *value = (*ptr)[0] | ((*ptr)[1] << 8) | ((*ptr)[2] << 16) | ((*ptr)[3] << 24);
   (*ptr) += 4;
   return 1;
}


static int skip_string(unsigned char **ptr, unsigned char *end, int type)
{
   int len;

   len = type >> 4;
   if (len == 0x0D) {
      if (!read_byte(ptr, end, &len)) goto error;
   }
   else if (len == 0x0E) {
      if (!read_short(ptr, end, &len)) goto error;
   }
   else if (len == 0x0F) {
      if (!read_int(ptr, end, &len)) goto error;
   }
   if (len < 0) goto error;
   if (len > 10000) goto error;
   if ((type & 0x0F) == SER_STRING_SHORT) {
      len *= 2;
   }
   else if ((type & 0x0F) == SER_STRING_INT) {
      len *= 4;
   }
   if (end - (*ptr) < len) goto error;
   (*ptr) += len;
   return 1;

error:
   return 0;
}


static int read_int_value(unsigned char **ptr, unsigned char *end, int *result)
{
   int val;

   if (!read_byte(ptr, end, &val)) goto error;
   
   switch (val) {
      case SER_ZERO:
         *result = 0;
         return 1;

      case SER_BYTE:
         if (!read_byte(ptr, end, result)) goto error;
         if (*result == 0) goto error;
         return 1;

      case SER_SHORT:
         if (!read_short(ptr, end, result)) goto error;
         if (((unsigned int)*result) <= 0xFF) goto error;
         return 1;

      case SER_INT:
         if (!read_int(ptr, end, result)) goto error;
         if (((unsigned int)*result) <= 0xFFFF) goto error;
         return 1;
   }

error:
   return 0;
}


static int read_string_ref(unsigned char **ptr, unsigned char *end, unsigned char *base, DynArray *strings, int *result)
{
   int val;

   if (!read_byte(ptr, end, &val)) goto error;
   if (val == SER_ZERO) {
      *result = -1;
      return 1;
   }
   else if ((val & 0x0F) == SER_STRING_BYTE || (val & 0x0F) == SER_STRING_SHORT || (val & 0x0F) == SER_STRING_INT) {
      *result = strings->len;
      if (dynarray_add(strings, (void *)((*ptr) - base - 1))) goto error;
      if (!skip_string(ptr, end, val)) goto error;
      return 1;
   }
   else if (val == SER_REF) {
      if (!read_int(ptr, end, &val)) goto error;
      if (((unsigned int)val) <= 0xFFFF) goto error;
      if (val < 0 || val >= strings->len) goto error;
      *result = val;
      return 1;
   }
   else if (val == SER_REF_SHORT) {
      if (!read_short(ptr, end, &val)) goto error;
      if (val < 0 || val >= strings->len) goto error;
      *result = val;
      return 1;
   }

error:
   return 0;
}


static char *read_string(Heap *heap, Value value, DynArray *strings, int id)
{
   Value str_val;
   char *str;
   int err, off;

   if (id < 0 || id >= strings->len) return NULL;

   off = (int)(intptr_t)strings->data[id];
   err = fixscript_unserialize(heap, value, &off, -1, &str_val);
   if (err) return NULL;

   err = fixscript_get_string(heap, str_val, 0, -1, &str, NULL);
   if (err) return NULL;
   return str;
}


static void process_stack_trace_lines(Heap *heap, Value value, Value trace, char **orig_script_name, int *orig_line)
{
   Array *arr;
   DynArray strings;
   Value elem;
   unsigned char *ptr, *end;
   char *s, *file_str, *func_str;
   int start_line, end_line, file_name, line_num, func_name;
   int new_script_name = -1, new_line=0;
   int i, len, val, trace_pos=0, trace_len;

   memset(&strings, 0, sizeof(DynArray));

   if (!fixscript_is_string(heap, value)) goto error;
   if (trace.value) {
      if (fixscript_get_array_length(heap, trace, &trace_pos)) goto error;
   }

   arr = &heap->data[value.value];
   if (arr->type != ARR_BYTE) goto error;
   ptr = arr->byte_data;
   end = ptr + arr->len;

   if (!read_byte(&ptr, end, &val)) goto error;
   if ((val & 0x0F) != SER_ARRAY) goto error;
   len = val >> 4;
   if (len == 0x0D) {
      if (!read_byte(&ptr, end, &len)) goto error;
   }
   else if (len == 0x0E) {
      if (!read_short(&ptr, end, &len)) goto error;
   }
   else if (len == 0x0F) {
      if (!read_int(&ptr, end, &len)) goto error;
   }
   if (len < 1 || len > 100000) goto error;
   if ((len % 5) != 0) goto error;
   len /= 5;

   for (i=0; i<len; i++) {
      if (!read_int_value(&ptr, end, &start_line)) goto error;
      if (!read_int_value(&ptr, end, &end_line)) goto error;

      if (!read_string_ref(&ptr, end, arr->byte_data, &strings, &file_name)) goto error;
      if (file_name < 0) goto error;

      if (!read_int_value(&ptr, end, &line_num)) goto error;

      if (!read_string_ref(&ptr, end, arr->byte_data, &strings, &func_name)) goto error;

      if (*orig_line >= start_line && *orig_line <= end_line) {
         if (func_name >= 0) {
            if (trace.value) {
               file_str = read_string(heap, value, &strings, file_name);
               func_str = read_string(heap, value, &strings, func_name);
               s = string_format("%s (%s:%d)", func_str, file_str, *orig_line - start_line + line_num);
               free(file_str);
               free(func_str);
               if (!s) goto error;
               elem = fixscript_create_string(heap, s, -1);
               free(s);
               if (!elem.value) goto error;
               if (fixscript_get_array_length(heap, trace, &trace_len)) goto error;
               if (fixscript_set_array_length(heap, trace, trace_len+1)) goto error;
               if (fixscript_copy_array(heap, trace, trace_pos+1, trace, trace_pos, trace_len-trace_pos)) goto error;
               if (fixscript_set_array_elem(heap, trace, trace_pos, elem)) goto error;
            }
         }
         else {
            new_script_name = file_name;
            new_line = *orig_line - start_line + line_num;
         }
      }
   }
   if (ptr != end) {
      goto error;
   }

   if (new_script_name >= 0) {
      *orig_script_name = read_string(heap, value, &strings, new_script_name);
      *orig_line = new_line;
   }

error:
   free(strings.data);
}


static void add_stack_entry(Heap *heap, Value trace, int pc)
{
   Function *func;
   NativeFunction *nfunc;
   Value elem;
   int i, j, line;
   char *s, *custom_func_name, *custom_script_name;
   const char *script_name, *func_name;
   Constant *constant;
   char *buf;

   for (i=0; i<heap->native_functions.len; i++) {
      nfunc = heap->native_functions.data[i];
      if (pc == nfunc->bytecode_ident_pc) {
         func_name = string_hash_find_name(&heap->native_functions_hash, nfunc);
         elem = fixscript_create_string(heap, func_name? func_name : "(replaced native function)", -1);
         fixscript_append_array_elem(heap, trace, elem);
         return;
      }
   }

   for (i=heap->functions.len-1; i > 0; i--) {
      func = heap->functions.data[i];
      if (pc >= func->addr) {
         script_name = string_hash_find_name(&heap->scripts, func->script);
         func_name = string_hash_find_name(&func->script->functions, func);

         custom_func_name = NULL;
         buf = malloc(9+strlen(func_name)+1);
         if (buf) {
            sprintf(buf, "function_%s", func_name);
            *strrchr(buf, '#') = '_';
            constant = string_hash_get(&func->script->constants, buf);
            if (constant && constant->local) {
               if (fixscript_get_string(heap, constant->value, 0, -1, &custom_func_name, NULL) == 0) {
                  func_name = custom_func_name;
               }
            }
            free(buf);
         }

         line = 0;
         for (j=func->lines_start; j<func->lines_end; j++) {
            if (pc == heap->lines[j].pc) {
               line = heap->lines[j].line;
               break;
            }
         }

         custom_script_name = NULL;

         constant = string_hash_get(&func->script->constants, "stack_trace_lines");
         if (constant && constant->local) {
            process_stack_trace_lines(heap, constant->value, trace, &custom_script_name, &line);
            if (custom_script_name) {
               script_name = custom_script_name;
            }
         }

         if (func->script->old_script && !custom_script_name) {
            script_name = string_hash_find_name(&heap->scripts, func->script->old_script);
         }

         s = string_format("%s (%s:%d)", func_name, script_name, line);
         elem = fixscript_create_string(heap, s, -1);
         if (func_name[0]) {
            fixscript_append_array_elem(heap, trace, elem);
         }
         free(s);
         free(custom_func_name);
         free(custom_script_name);
         return;
      }
   }
}


static Value create_error(Heap *heap, Value msg, int skip_last, int extra_pc)
{
   Value error, trace;
   int i, pc;

   error = fixscript_create_array(heap, 2);
   trace = fixscript_create_array(heap, 0);
   if (!error.value || !trace.value) {
      return msg.value? msg : fixscript_int(1);
   }

   fixscript_set_array_elem(heap, error, 0, msg);
   fixscript_set_array_elem(heap, error, 1, trace);

   if (extra_pc) {
      add_stack_entry(heap, trace, extra_pc);
   }
   
   for (i=heap->stack_len-(skip_last? 2:1); i>=0; i--) {
      if (heap->stack_flags[i] && (heap->stack_data[i] & (1<<31))) {
         pc = heap->stack_data[i] & ~(1<<31);
         if (pc > 0 && pc < (1<<23)) {
            add_stack_entry(heap, trace, pc);
         }
      }
   }

   return error;
}


Value fixscript_create_error(Heap *heap, Value msg)
{
   return create_error(heap, msg, 0, 0);
}


Value fixscript_create_error_string(Heap *heap, const char *s)
{
   if (!s) {
      return fixscript_int(1);
   }
   return fixscript_create_error(heap, fixscript_create_string(heap, s, -1));
}


Value fixscript_error(Heap *heap, Value *error, int code)
{
   *error = fixscript_create_error_string(heap, fixscript_get_error_msg(code));
   return fixscript_int(0);
}


const char *fixscript_get_compiler_error(Heap *heap, Value error)
{
   Value value, stack;
   Script *script;
   char *s, *p, *q, *r;
   size_t slen;
   int i, err, len;

   free(heap->compiler_error);
   heap->compiler_error = NULL;

   if (fixscript_is_string(heap, error)) {
      fixscript_get_string(heap, error, 0, -1, &heap->compiler_error, NULL);
   }
   else {
      value = error;
      stack = fixscript_int(0);
      for (;;) {
         if (!fixscript_is_array(heap, value)) break;
         if (fixscript_get_array_length(heap, value, &len) != FIXSCRIPT_SUCCESS) break;
         if (len != 2) break;
         if (fixscript_get_array_elem(heap, value, 1, &stack) != FIXSCRIPT_SUCCESS) break;
         if (fixscript_get_array_elem(heap, value, 0, &value) != FIXSCRIPT_SUCCESS) break;
      }
      if (fixscript_is_string(heap, value)) {
         err = fixscript_get_string(heap, value, 0, -1, &s, NULL);
         if (!err) {
            p = strstr(s, ".fix(");
            if (p) {
               p += 5;
               if (*p >= '0' && *p <= '9') {
                  while (*p >= '0' && *p <= '9') p++;
                  if (strncmp(p, "): ", 3) == 0) {
                     if (fixscript_get_array_length(heap, stack, &len) == FIXSCRIPT_SUCCESS) {
                        for (i=0; i<len; i++) {
                           err = fixscript_get_array_elem(heap, stack, i, &value);
                           if (err) break;
                           err = fixscript_get_string(heap, value, 0, -1, &p, NULL);
                           if (err) break;
                           if (strcmp(p, "script_query#5") == 0) {
                              heap->compiler_error = strdup(s);
                              free(p);
                              break;
                           }
                           q = strrchr(p, ':');
                           if (q) *q = '\0';
                           q = strrchr(p, '(');
                           if (q) {
                              script = fixscript_get(heap, q+1);
                              if (script) {
                                 if (string_hash_get(&script->functions, "process_tokens#3")) {
                                    r = strrchr(q+1, '.');
                                    if (r) *r = '\0';
                                    heap->compiler_error = string_format("%s [%s]", s, q+1);
                                    free(p);
                                    break;
                                 }
                              }
                           }
                           free(p);
                        }
                     }
                  }
               }
            }
            free(s);
         }
      }
      if (!heap->compiler_error) {
         fixscript_to_string(heap, error, 1, &heap->compiler_error, NULL);
      }
   }
   if (heap->compiler_error) {
      slen = strlen(heap->compiler_error);
      if (slen > INT_MAX) {
         free(heap->compiler_error);
         heap->compiler_error = strdup("internal error: error is too long");
         slen = strlen(heap->compiler_error);
      }
      string_filter_control_chars(heap->compiler_error, slen);
   }
   return heap->compiler_error;
}


const char *fixscript_get_error_msg(int error_code)
{
   switch (error_code) {
      case FIXSCRIPT_ERR_INVALID_ACCESS:                 return "invalid array access";
      case FIXSCRIPT_ERR_INVALID_BYTE_ARRAY:             return "invalid byte array";
      case FIXSCRIPT_ERR_INVALID_SHORT_ARRAY:            return "invalid short array";
      case FIXSCRIPT_ERR_INVALID_NULL_STRING:            return "invalid null-terminated string";
      case FIXSCRIPT_ERR_CONST_WRITE:                    return "write access to constant string";
      case FIXSCRIPT_ERR_OUT_OF_BOUNDS:                  return "array out of bounds access";
      case FIXSCRIPT_ERR_OUT_OF_MEMORY:                  return "out of memory";
      case FIXSCRIPT_ERR_INVALID_SHARED_ARRAY_OPERATION: return "invalid shared array operation";
      case FIXSCRIPT_ERR_KEY_NOT_FOUND:                  return "hash key not found";
      case FIXSCRIPT_ERR_RECURSION_LIMIT:                return "recursion limit exceeded";
      case FIXSCRIPT_ERR_UNSERIALIZABLE_REF:             return "unserializable reference occurred";
      case FIXSCRIPT_ERR_BAD_FORMAT:                     return "bad format";
      case FIXSCRIPT_ERR_FUNC_REF_LOAD_ERROR:            return "script load error during resolving of function reference";
      case FIXSCRIPT_ERR_NESTED_WEAKREF:                 return "nested weak reference";
   }
   return NULL;
}


static Value builtin_log(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#ifdef _WIN32
   HANDLE out;
   DWORD console_mode, ret;
   char *s8, *bytes;
   uint16_t *s;
   int i, err, len, bytes_len, reclaim=0;

   if (fixscript_is_string(heap, params[0])) {
      err = fixscript_get_string_utf16(heap, params[0], 0, -1, &s, &len);
   }
   else {
      err = fixscript_to_string(heap, params[0], 0, &s8, &len);
      if (!err) {
         params[0] = fixscript_create_string(heap, s8, len);
         if (!params[0].value) {
            err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
         }
      }
      free(s8);
      if (!err) {
         err = fixscript_get_string_utf16(heap, params[0], 0, -1, &s, &len);
         reclaim = 1;
      }
   }
   if (err) {
      return fixscript_error(heap, error, err);
   }

   for (i=0; i<len; i++) {
      if (s[i] >= 0 && s[i] < 32) {
         switch (s[i]) {
            case '\t':
            case '\n':
               break;

            default:
               s[i] = '?';
         }
      }
   }

   out = GetStdHandle(STD_ERROR_HANDLE);
   if (GetConsoleMode(out, &console_mode)) {
      WriteConsole(out, s, len, NULL, NULL);
      WriteConsole(out, L"\n", 1, NULL, NULL);
   }
   else {
      if (len > 0) {
         bytes_len = WideCharToMultiByte(CP_ACP, 0, s, len, NULL, 0, NULL, NULL);
         if (bytes_len <= 0) {
            free(s);
            return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         }

         if (INT_MAX - bytes_len < 1) {
            free(s);
            return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         }
      }
      else {
         bytes_len = 0;
      }

      bytes = malloc(bytes_len+1);
      if (!bytes) {
         free(s);
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }

      if (len > 0) {
         if (WideCharToMultiByte(CP_ACP, 0, s, len, bytes, bytes_len, NULL, NULL) != bytes_len) {
            free(bytes);
            free(s);
            return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         }
      }

      bytes[bytes_len++] = '\n';
      WriteFile(out, bytes, bytes_len, &ret, NULL);
      free(bytes);
   }

   if (reclaim) {
      reclaim_array(heap, params[0].value, NULL);
   }
   free(s);
#else
   char *s;
   int err, len;

   if (fixscript_is_string(heap, params[0])) {
      err = fixscript_get_string(heap, params[0], 0, -1, &s, &len);
   }
   else {
      err = fixscript_to_string(heap, params[0], 0, &s, &len);
   }
   if (err) {
      return fixscript_error(heap, error, err);
   }

   string_filter_control_chars(s, len);
   fprintf(stderr, "%.*s\n", len, s);
   fflush(stderr);
   free(s);
#endif

   return fixscript_int(0);
}


static Value builtin_dump(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   NativeFunction *log_func;
   Value value;
   char *s;
   int len, err;
   
   err = fixscript_to_string(heap, params[0], 1, &s, &len);
   if (err != FIXSCRIPT_SUCCESS) {
      return fixscript_error(heap, error, err);
   }

   value = fixscript_create_string(heap, s, len);
   free(s);

   log_func = string_hash_get(&heap->native_functions_hash, "log#1");
   return log_func->func(heap, error, 1, &value, log_func->data);
}


static Value builtin_to_string(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   int newlines = 0, len, err;
   char *s;
   Value ret;
   
   if (num_params == 2 && params[1].value) {
      newlines = 1;
   }

   err = fixscript_to_string(heap, params[0], newlines, &s, &len);
   if (err != FIXSCRIPT_SUCCESS) {
      return fixscript_error(heap, error, err);
   }

   ret = fixscript_create_string(heap, s, len);
   free(s);
   return ret;
}


static Value builtin_error(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   return create_error(heap, params[0], 1, 0);
}


static Value builtin_clone(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value clone;
   int deep = (data != NULL);
   int err;

   err = fixscript_clone(heap, params[0], deep, &clone);
   if (err != FIXSCRIPT_SUCCESS) {
      return fixscript_error(heap, error, err);
   }

   return clone;
}


static Value builtin_array_create(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value value;
   int err, type, elem_size = 1;

   if (!fixscript_is_int(params[0]) || params[0].value < 0) {
      *error = fixscript_create_error_string(heap, "length must be positive integer");
      return fixscript_int(0);
   }

   if (num_params == 2) {
      if (!fixscript_is_int(params[1])) {
         *error = fixscript_create_error_string(heap, "element size must be integer");
         return fixscript_int(0);
      }
      elem_size = fixscript_get_int(params[1]);
   }

   switch (elem_size) {
      case 1: type = ARR_BYTE; break;
      case 2: type = ARR_SHORT; break;
      case 4: type = ARR_INT; break;
      default:
         *error = fixscript_create_error_string(heap, "element size must be 1, 2 or 4");
         return fixscript_int(0);
   }

   value = create_array(heap, type, params[0].value);
   if (!value.is_array) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   err = fixscript_set_array_length(heap, value, params[0].value);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   add_root(heap, value);
   return value;
}


static Value builtin_array_create_shared(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value value;
   int elem_size;

   if (!fixscript_is_int(params[0]) || params[0].value < 0) {
      *error = fixscript_create_error_string(heap, "length must be positive integer");
      return fixscript_int(0);
   }

   if (!fixscript_is_int(params[1])) {
      *error = fixscript_create_error_string(heap, "element size must be integer");
      return fixscript_int(0);
   }

   elem_size = params[1].value;
   if (elem_size != 1 && elem_size != 2 && elem_size != 4) {
      *error = fixscript_create_error_string(heap, "element size must be 1, 2 or 4");
      return fixscript_int(0);
   }
   
   value = fixscript_create_shared_array(heap, params[0].value, elem_size);
   if (!value.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return value;
}


static Value builtin_array_get_shared_count(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Array *arr;

   if (!fixscript_is_array(heap, params[0])) {
      *error = fixscript_create_error_string(heap, "invalid value (not a shared array)");
      return fixscript_int(0);
   }

   arr = &heap->data[params[0].value];
   if (!arr->is_shared) {
      *error = fixscript_create_error_string(heap, "invalid value (not a shared array)");
      return fixscript_int(0);
   }

   return fixscript_int(ARRAY_SHARED_HEADER(arr)->refcnt);
}


static Value builtin_array_get_element_size(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Array *arr;

   if (!fixscript_is_array(heap, params[0])) {
      *error = fixscript_create_error_string(heap, "invalid value (not an array)");
      return fixscript_int(0);
   }

   arr = &heap->data[params[0].value];
   switch (arr->type) {
      case ARR_BYTE:  return fixscript_int(1);
      case ARR_SHORT: return fixscript_int(2);
      case ARR_INT:   return fixscript_int(4);
   }

   *error = fixscript_create_error_string(heap, "internal error");
   return fixscript_int(0);
}


static Value builtin_array_set_length(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   int is_object_create = (data != NULL);
   Value arr, len;
   int err, cur_len;

   if (is_object_create && num_params == 1) {
      arr = fixscript_int(0);
      len = params[0];
   }
   else {
      arr = params[0];
      len = num_params == 2? params[1] : fixscript_int(0);
   }

   if (!fixscript_is_int(len)) {
      *error = fixscript_create_error_string(heap, "length must be an integer");
      return fixscript_int(0);
   }

   if (fixscript_get_int(len) < 0) {
      *error = fixscript_create_error_string(heap, "length must not be negative");
      return fixscript_int(0);
   }

   if (is_object_create && num_params == 1) {
      arr = fixscript_create_array(heap, 0);
      if (!arr.value) {
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
   }

   if (is_object_create && num_params == 2) {
      err = fixscript_get_array_length(heap, arr, &cur_len);
      if (err) {
         return fixscript_error(heap, error, err);
      }

      if (len.value < cur_len) {
         *error = fixscript_create_error_string(heap, "new length must not be smaller");
         return fixscript_int(0);
      }
   }

   err = fixscript_set_array_length(heap, arr, len.value);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   return is_object_create? arr : fixscript_int(0);
}


static Value builtin_array_copy(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value dest_val, src_val;
   int dest_off, src_off, count;
   int err = FIXSCRIPT_SUCCESS;

   if (!fixscript_is_int(params[1])) {
      *error = fixscript_create_error_string(heap, "dest_off must be an integer");
   }
   if (!fixscript_is_int(params[3])) {
      *error = fixscript_create_error_string(heap, "src_off must be an integer");
   }
   if (!fixscript_is_int(params[4])) {
      *error = fixscript_create_error_string(heap, "count must be an integer");
   }
   if (error->value) {
      return fixscript_int(0);
   }

   dest_val = params[0];
   dest_off = fixscript_get_int(params[1]);
   src_val = params[2];
   src_off = fixscript_get_int(params[3]);
   count = fixscript_get_int(params[4]);

   if (dest_off < 0) {
      *error = fixscript_create_error_string(heap, "negative dest_off");
      return fixscript_int(0);
   }
   if (src_off < 0) {
      *error = fixscript_create_error_string(heap, "negative src_off");
      return fixscript_int(0);
   }
   if (count < 0) {
      *error = fixscript_create_error_string(heap, "negative count");
      return fixscript_int(0);
   }

   err = fixscript_copy_array(heap, dest_val, dest_off, src_val, src_off, count);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return fixscript_int(0);
}


static Value builtin_array_fill(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Array *arr;
   Value arr_val, value;
   int off=0, count=0;
   int i, err;

   arr_val = params[0];
   if (num_params == 4) {
      if (!fixscript_is_int(params[1])) {
         *error = fixscript_create_error_string(heap, "off must be an integer");
         return fixscript_int(0);
      }
      if (!fixscript_is_int(params[2])) {
         *error = fixscript_create_error_string(heap, "count must be an integer");
         return fixscript_int(0);
      }
      off = fixscript_get_int(params[1]);
      count = fixscript_get_int(params[2]);
      value = params[3];
   }
   else {
      value = params[1];
   }

   if (!arr_val.is_array || arr_val.value <= 0 || arr_val.value >= heap->size) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_INVALID_ACCESS);
   }

   arr = &heap->data[arr_val.value];
   if (arr->len == -1 || arr->hash_slots >= 0) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_INVALID_ACCESS);
   }

   if (num_params == 2) {
      off = 0;
      count = arr->len;
   }

   if (off < 0 || count < 0 || ((int64_t)off) + ((int64_t)count) > ((int64_t)arr->len)) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_BOUNDS);
   }

   if (arr->is_shared && value.is_array && !fixscript_is_float(value)) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_INVALID_SHARED_ARRAY_OPERATION);
   }

   if (ARRAY_NEEDS_UPGRADE(arr, value.value)) {
      err = upgrade_array(heap, arr, arr_val.value, value.value);
      if (err) {
         return fixscript_error(heap, error, err);
      }
   }

   switch (arr->type) {
      case ARR_BYTE:
         memset(arr->byte_data + off, value.value, count);
         break;

      case ARR_SHORT:
         for (i=0; i<count; i++) {
            arr->short_data[off+i] = value.value;
         }
         break;

      case ARR_INT:
         for (i=0; i<count; i++) {
            arr->data[off+i] = value.value;
         }
         break;
   }

   if (!arr->is_shared) {
      if (value.is_array) {
         flags_set_range(arr, off, count);
      }
      else {
         flags_clear_range(arr, off, count);
      }
   }

   return fixscript_int(0);
}


static Value builtin_array_extract(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value array, new_array;
   int off, count;
   int ret;

   if (!fixscript_is_int(params[1])) {
      *error = fixscript_create_error_string(heap, "off must be an integer");
      return fixscript_int(0);
   }
   if (!fixscript_is_int(params[2])) {
      *error = fixscript_create_error_string(heap, "count must be an integer");
      return fixscript_int(0);
   }

   array = params[0];
   off = fixscript_get_int(params[1]);
   count = fixscript_get_int(params[2]);

   if (off < 0) {
      *error = fixscript_create_error_string(heap, "negative off");
      return fixscript_int(0);
   }
   if (count < 0) {
      *error = fixscript_create_error_string(heap, "negative count");
      return fixscript_int(0);
   }

   new_array = fixscript_create_array(heap, count);
   if (!new_array.value) {
      *error = fixscript_create_error_string(heap, "out of memory");
      return fixscript_int(0);
   }
   if (fixscript_is_array(heap, array)) {
      heap->data[new_array.value].is_string = heap->data[array.value].is_string;
   }

   ret = fixscript_copy_array(heap, new_array, 0, array, off, count);
   if (ret != FIXSCRIPT_SUCCESS) {
      return fixscript_error(heap, error, ret);
   }

   return new_array;
}


static Value builtin_array_insert(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value array, value;
   int off;
   int err, len;
   int64_t new_len;

   if (!fixscript_is_int(params[1])) {
      *error = fixscript_create_error_string(heap, "off must be an integer");
      return fixscript_int(0);
   }

   array = params[0];
   off = fixscript_get_int(params[1]);
   value = params[2];

   if (off < 0) {
      *error = fixscript_create_error_string(heap, "negative off");
      return fixscript_int(0);
   }

   err = fixscript_get_array_length(heap, array, &len);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   new_len = ((int64_t)len) + ((int64_t)1);
   if (new_len > INT_MAX) {
      *error = fixscript_create_error_string(heap, "array out of bounds access");
      return fixscript_int(0);
   }

   err = fixscript_set_array_length(heap, array, (int)new_len);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   err = fixscript_copy_array(heap, array, off + 1, array, off, len - off);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   err = fixscript_set_array_elem(heap, array, off, value);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   return fixscript_int(0);
}


static Value builtin_array_append(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   int64_t sum;
   int err, len1, len2, off;

   err = fixscript_get_array_length(heap, params[0], &len1);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   if (num_params == 4) {
      off = fixscript_get_int(params[2]);
      len2 = fixscript_get_int(params[3]);
      if (off < 0 || len2 < 0) {
         *error = fixscript_create_error_string(heap, "negative offset or count");
         return fixscript_int(0);
      }
   }
   else {
      off = 0;
      err = fixscript_get_array_length(heap, params[1], &len2);
      if (err) {
         return fixscript_error(heap, error, err);
      }
   }

   sum = (int64_t)len1 + (int64_t)len2;
   if (sum > 0xFFFFFFFF) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   err = fixscript_set_array_length(heap, params[0], (int)sum);
   if (!err) {
      err = fixscript_copy_array(heap, params[0], len1, params[1], off, len2);
   }
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return fixscript_int(0);
}


static Value builtin_array_replace_range(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   int err, start, end, off, len, remove_len, old_len;

   start = fixscript_get_int(params[1]);
   end = fixscript_get_int(params[2]);
   if (start < 0 || end < 0) {
      *error = fixscript_create_error_string(heap, "negative start or end");
      return fixscript_int(0);
   }
   if (start > end) {
      *error = fixscript_create_error_string(heap, "invalid range");
      return fixscript_int(0);
   }

   if (num_params == 6) {
      off = fixscript_get_int(params[4]);
      len = fixscript_get_int(params[5]);
      if (off < 0 || len < 0) {
         *error = fixscript_create_error_string(heap, "negative offset or length");
         return fixscript_int(0);
      }
   }
   else {
      off = 0;
      err = fixscript_get_array_length(heap, params[3], &len);
      if (err) {
         return fixscript_error(heap, error, err);
      }
   }

   err = fixscript_get_array_length(heap, params[0], &old_len);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   remove_len = end - start;
   if (len >= remove_len) {
      err = fixscript_set_array_length(heap, params[0], old_len + (len - remove_len));
      if (!err) {
         err = fixscript_copy_array(heap, params[0], start + len, params[0], end, old_len - end);
      }
   }
   else {
      err = fixscript_copy_array(heap, params[0], start + len, params[0], end, old_len - end);
      if (!err) {
         err = fixscript_set_array_length(heap, params[0], old_len + (len - remove_len));
      }
   }

   if (!err) {
      err = fixscript_copy_array(heap, params[0], start, params[3], off, len);
   }
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return fixscript_int(0);
}


static Value builtin_array_insert_array(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value p[6];

   if (num_params == 3) {
      p[0] = params[0];
      p[1] = params[1];
      p[2] = params[1];
      p[3] = params[2];
      return builtin_array_replace_range(heap, error, 4, p, NULL);
   }
   else {
      p[0] = params[0];
      p[1] = params[1];
      p[2] = params[1];
      p[3] = params[2];
      p[4] = params[3];
      p[5] = params[4];
      return builtin_array_replace_range(heap, error, 6, p, NULL);
   }
}


static Value builtin_string_const(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value ret;
   int err, off, len;

   if (num_params == 3) {
      off = params[1].value;
      len = params[2].value;
   }
   else {
      off = 0;
      len = -1;
   }
   
   err = fixscript_get_const_string(heap, params[0], off, len, &ret);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return ret;
}


static Value builtin_array_remove(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value array;
   int off, count;
   int err, len;
   int64_t new_len;

   if (!fixscript_is_int(params[1])) {
      *error = fixscript_create_error_string(heap, "off must be an integer");
      return fixscript_int(0);
   }
   if (num_params == 3 && !fixscript_is_int(params[2])) {
      *error = fixscript_create_error_string(heap, "count must be an integer");
      return fixscript_int(0);
   }

   array = params[0];
   off = fixscript_get_int(params[1]);
   count = (num_params == 3? fixscript_get_int(params[2]) : 1);

   if (off < 0) {
      *error = fixscript_create_error_string(heap, "negative off");
      return fixscript_int(0);
   }
   if (count < 0) {
      *error = fixscript_create_error_string(heap, "negative count");
      return fixscript_int(0);
   }

   err = fixscript_get_array_length(heap, array, &len);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   new_len = ((int64_t)len) - ((int64_t)count);
   if (new_len < 0) {
      *error = fixscript_create_error_string(heap, "array out of bounds access");
      return fixscript_int(0);
   }

   err = fixscript_copy_array(heap, array, off, array, off + count, len - off - count);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   err = fixscript_set_array_length(heap, array, (int)new_len);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   return fixscript_int(0);
}


static Value builtin_string_parse_single(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value default_val = fixscript_int(0);
   Value result;
   char *s, *end;
   long int int_val;
   int err, off, len, has_default=0, valid;

   if (num_params == 3 || num_params == 4) {
      off = fixscript_get_int(params[1]);
      len = fixscript_get_int(params[2]);
      if (len < 0) {
         *error = fixscript_create_error_string(heap, "negative length");
         return fixscript_int(0);
      }
   }
   else {
      off = 0;
      err = fixscript_get_array_length(heap, params[0], &len);
      if (err) {
         return fixscript_error(heap, error, err);
      }
   }

   if (num_params == 2 || num_params == 4) {
      has_default = 1;
      default_val = params[num_params-1];
   }

   err = fixscript_get_string(heap, params[0], off, len, &s, NULL);
   if (err) {
      if (err == FIXSCRIPT_ERR_INVALID_NULL_STRING && has_default) {
         return default_val;
      }
      return fixscript_error(heap, error, err);
   }

   if (data == (void *)0) {
      valid = 1;
      errno = 0;
      int_val = strtol(s, &end, 10);
      if (errno != 0 || *end != '\0') {
         valid = 0;
      }
      else if (sizeof(long int) > sizeof(int)) {
         if (int_val < INT_MIN || int_val > INT_MAX) {
            valid = 0;
         }
      }
      result = fixscript_int(int_val);
   }
   else {
      result = fixscript_float((float)strtod(s, &end));
      valid = (*end == '\0');
   }

   free(s);

   if (!valid || len == 0) {
      if (has_default) {
         return default_val;
      }
      *error = fixscript_create_error_string(heap, "parse error");
      return fixscript_int(0);
   }
   
   return result;
}


static Value builtin_string_parse_double(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value default_lo, default_hi, result;
   int has_default=0, valid=0;
   char *s, *end;
   int64_t long_val;
   union {
      double d;
      uint64_t i;
   } u;
   int err, off, len;

   default_lo = fixscript_int(0);
   default_hi = fixscript_int(0);

   if (num_params == 1) {
      off = 0;
      err = fixscript_get_array_length(heap, params[0], &len);
      if (err) {
         return fixscript_error(heap, error, err);
      }
   }
   else {
      off = fixscript_get_int(params[1]);
      len = fixscript_get_int(params[2]);
      if (len < 0) {
         *error = fixscript_create_error_string(heap, "negative length");
         return fixscript_int(0);
      }
      if (num_params == 5) {
         has_default = 1;
         default_lo = params[3];
         default_hi = params[4];
      }
   }

   err = fixscript_get_string(heap, params[0], off, len, &s, NULL);
   if (err || len == 0) {
      if (has_default) {
         *error = default_hi;
         result = default_lo;
      }
      else {
         *error = fixscript_create_error_string(heap, "parse error");
         result = fixscript_int(0);
      }
      return result;
   }

   if (data == (void *)0) {
      errno = 0;
      long_val = strtoll(s, &end, 10);
      if (errno == 0 && *end == '\0') {
         valid = 1;
         *error = fixscript_int((int)(((uint64_t)long_val) >> 32));
         result = fixscript_int((int)long_val);
      }
   }
   else {
      u.d = strtod(s, &end);
      if (*end == '\0') {
         valid = 1;
         *error = fixscript_int((int)((u.i) >> 32));
         result = fixscript_int((int)u.i);
      }
   }

   free(s);

   if (!valid) {
      if (has_default) {
         *error = default_hi;
         result = default_lo;
      }
      else {
         *error = fixscript_create_error_string(heap, "parse error");
         result = fixscript_int(0);
      }
   }

   return result;
}


static Value builtin_string_from_double(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value value_lo, value_hi, result;
   char buf[64], *s;
   union {
      double d;
      uint64_t i;
   } u;
   int err, len, len2;

   if (num_params == 3) {
      value_lo = params[1];
      value_hi = params[2];
   }
   else {
      value_lo = params[0];
      value_hi = params[1];
   }

   u.i = (((uint64_t)(uint32_t)value_hi.value) << 32) | (uint64_t)(uint32_t)value_lo.value;
   if (data == (void *)0) {
      #ifdef _WIN32
         snprintf(buf, sizeof(buf), "%I64d", u.i);
      #else
         snprintf(buf, sizeof(buf), "%lld", (long long)u.i);
      #endif
   }
   else {
      snprintf(buf, sizeof(buf), "%.17g", u.d);
      for (s=buf; *s; s++) {
         if (*s == ',') *s = '.';
      }
      s = strstr(buf, "e+");
      if (s) {
         memmove(s+1, s+2, strlen(s)-1);
      }
      if (!strchr(buf, '.') && !strchr(buf, 'e')) {
         len = strlen(buf);
         if (len+2 < sizeof(buf)) {
            strcpy(buf+len, ".0");
         }
      }
   }

   if (num_params == 3) {
      result = params[0];

      err = fixscript_get_array_length(heap, result, &len);
      if (err) {
         return fixscript_error(heap, error, err);
      }

      len2 = strlen(buf);
      err = fixscript_set_array_length(heap, result, len+len2);
      if (err) {
         return fixscript_error(heap, error, err);
      }

      err = fixscript_set_array_bytes(heap, result, len, len2, buf);
      if (err) {
         return fixscript_error(heap, error, err);
      }
   }
   else {
      result = fixscript_create_string(heap, buf, -1);
      if (!result.value) {
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
   }
   
   return result;
}


static Value builtin_string_from_utf8(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value arr, off, len, result, str, func_params[2];
   char *bytes;
   int err;

   if (num_params == 2 || num_params == 4) {
      result = *params++;
   }
   else {
      result = fixscript_int(0);
   }
   arr = *params++;
   if (num_params == 3 || num_params == 4) {
      off = *params++;
      len = *params++;

      if (off.value < 0) {
         *error = fixscript_create_error_string(heap, "negative offset");
         return fixscript_int(0);
      }
      if (len.value < 0) {
         *error = fixscript_create_error_string(heap, "negative length");
         return fixscript_int(0);
      }
   }
   else {
      off.value = 0;
      err = fixscript_get_array_length(heap, arr, &len.value);
      if (err) {
         return fixscript_error(heap, error, err);
      }
   }

   err = fixscript_lock_array(heap, arr, off.value, len.value, (void **)&bytes, 1, 1);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   str = fixscript_create_string(heap, bytes, len.value);
   if (!str.value) err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
   
   if (!err) {
      if (result.value) {
         func_params[0] = result;
         func_params[1] = str;
         builtin_array_append(heap, error, 2, func_params, NULL);
         reclaim_array(heap, str.value, NULL);
         if (error->value) {
            fixscript_unlock_array(heap, arr, off.value, len.value, (void **)&bytes, 1, 0);
            return fixscript_int(0);
         }
      }
      else {
         result = str;
      }
   }

   fixscript_unlock_array(heap, arr, off.value, len.value, (void **)&bytes, 1, 0);

   if (err) {
      return fixscript_error(heap, error, err);
   }
   return result;
}


static Value builtin_string_to_utf8(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value str, off, len, result, shared, func_params[2];
   char *bytes;
   int err, bytes_len;

   if (num_params == 2 || num_params == 4) {
      result = *params++;
   }
   else {
      result = fixscript_int(0);
   }
   str = *params++;
   if (num_params == 3 || num_params == 4) {
      off = *params++;
      len = *params++;

      if (off.value < 0) {
         *error = fixscript_create_error_string(heap, "negative offset");
         return fixscript_int(0);
      }
      if (len.value < 0) {
         *error = fixscript_create_error_string(heap, "negative length");
         return fixscript_int(0);
      }
   }
   else {
      off.value = 0;
      len.value = -1;
   }

   err = fixscript_get_string(heap, str, off.value, len.value, &bytes, &bytes_len);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   if (result.value) {
      shared = fixscript_create_or_get_shared_array(heap, -1, bytes, bytes_len, 1, NULL, NULL, NULL);
      if (!shared.value) {
         free(bytes);
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
      
      func_params[0] = result;
      func_params[1] = shared;
      builtin_array_append(heap, error, 2, func_params, NULL);
      if (error->value) {
         free(bytes);
         return fixscript_int(0);
      }
   }
   else {
      result = fixscript_create_byte_array(heap, bytes, bytes_len);
      if (!result.value) {
         free(bytes);
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
   }

   free(bytes);

   if (err) {
      return fixscript_error(heap, error, err);
   }
   return result;
}


static Value builtin_weakref_create(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value ret;
   int err;
   
   err = fixscript_create_weak_ref(heap, params[0], num_params >= 2? &params[1] : NULL, num_params >= 3? &params[2] : NULL, &ret);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return ret;
}


static Value builtin_weakref_get(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value ret;
   int err;

   err = fixscript_get_weak_ref(heap, params[0], &ret);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return ret;
}


#ifdef FIXSCRIPT_ASYNC
typedef struct {
   Value *func_params;
   ContinuationResultFunc cont_func;
   void *cont_data;
} FuncRefCallData;

static void funcref_call_cont(Heap *heap, Value result, Value error, void *data)
{
   FuncRefCallData *frc = data;
   ContinuationResultFunc cont_func;
   void *cont_data;

   free(frc->func_params);
   cont_func = frc->cont_func;
   cont_data = frc->cont_data;
   free(frc);

   cont_func(heap, result, error, cont_data);
}
#endif


static Value builtin_funcref_call(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#ifdef FIXSCRIPT_ASYNC
   FuncRefCallData *frc;
#endif
   Value ret, *func_params = NULL;
   int err, func_num_params;

   err = fixscript_get_array_length(heap, params[1], &func_num_params);
   if (!err) {
      func_params = calloc(func_num_params, sizeof(Value));
      if (!func_params) {
         err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
   }
   if (!err) {
      err = fixscript_get_array_range(heap, params[1], 0, func_num_params, func_params);
   }
   if (err) {
      free(func_params);
      return fixscript_error(heap, error, err);
   }

#ifdef FIXSCRIPT_ASYNC
   if (fixscript_in_async_call(heap)) {
      frc = malloc(sizeof(FuncRefCallData));
      if (!frc) {
         free(func_params);
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
      frc->func_params = func_params;

      fixscript_suspend(heap, &frc->cont_func, &frc->cont_data);
      fixscript_call_async(heap, params[0], func_num_params, func_params, funcref_call_cont, frc);
      return fixscript_int(0);
   }
#endif

   ret = fixscript_call_args(heap, params[0], func_num_params, error, func_params);
   free(func_params);
   return ret;
}


static Value builtin_hash_get(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value value;
   int err;

   err = fixscript_get_hash_elem(heap, params[0], params[1], &value);
   if (err == FIXSCRIPT_ERR_KEY_NOT_FOUND) {
      return params[2];
   }
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return value;
}


static Value builtin_hash_entry(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value hash_val = params[0];
   int idx = params[1].value;
   Array *arr;
   int err;

   if (idx < 0) {
      *error = fixscript_int(0);
      return fixscript_int(0);
   }

   if (!hash_val.is_array || hash_val.value <= 0 || hash_val.value >= heap->size) {
      *error = fixscript_int(0);
      return fixscript_int(0);
   }

   arr = &heap->data[hash_val.value];
   if (arr->len == -1 || arr->hash_slots < 0 || arr->is_handle) {
      *error = fixscript_int(0);
      return fixscript_int(0);
   }

   if (idx >= arr->len) {
      *error = fixscript_int(0);
      return fixscript_int(0);
   }

   // TODO: adjust indicies on entry removal instead of rehashing during retrieving of the entry
   if (arr->hash_slots != arr->len) {
      err = expand_hash(heap, hash_val, arr);

      // provide correct results (albeit slower) in case of error (out of memory):
      if (err != FIXSCRIPT_SUCCESS) {
         Value key, value;
         int pos = 0;
         while (fixscript_iter_hash(heap, hash_val, &key, &value, &pos)) {
            if (idx-- == 0) {
               *error = value;
               return key;
            }
         }
         *error = fixscript_int(0);
         return fixscript_int(0);
      }
   }
   
   idx = bitarray_get(&arr->flags[FLAGS_SIZE((1<<arr->size)*2)], arr->size-1, idx) << 1;
   *error = (Value) { arr->data[idx+1], IS_ARRAY(arr, idx+1) != 0 };
   return (Value) { arr->data[idx+0], IS_ARRAY(arr, idx+0) != 0 };
}


static Value builtin_hash_contains(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value hash_val = params[0];
   Value key_val = params[1];
   int err;
   
   err = fixscript_get_hash_elem(heap, hash_val, key_val, NULL);
   if (err == FIXSCRIPT_ERR_KEY_NOT_FOUND) {
      return fixscript_int(0);
   }
   
   if (err != FIXSCRIPT_SUCCESS) {
      return fixscript_error(heap, error, err);
   }

   return fixscript_int(1);
}


static Value builtin_hash_remove(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value hash_val = params[0];
   Value key_val = params[1];
   Value value_val;
   int err;
   
   err = fixscript_remove_hash_elem(heap, hash_val, key_val, &value_val);
   if (err != FIXSCRIPT_SUCCESS) {
      return fixscript_error(heap, error, err);
   }

   return value_val;
}


static Value builtin_hash_get_values(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value hash_val = params[0];
   Value key, value;
   Value arr_val;
   int mode = (intptr_t)data;
   int err, len, pos = 0, idx = 0;

   err = fixscript_get_array_length(heap, hash_val, &len);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   if (mode == 2) {
      if (len >= (1<<30)) {
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
      len <<= 1;
   }
   
   arr_val = fixscript_create_array(heap, len);
   if (!arr_val.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   
   while (fixscript_iter_hash(heap, hash_val, &key, &value, &pos)) {
      if (mode == 0 || mode == 2) {
         err = fixscript_set_array_elem(heap, arr_val, idx++, key);
         if (err != FIXSCRIPT_SUCCESS) {
            return fixscript_error(heap, error, err);
         }
      }
      if (mode == 1 || mode == 2) {
         err = fixscript_set_array_elem(heap, arr_val, idx++, value);
         if (err != FIXSCRIPT_SUCCESS) {
            return fixscript_error(heap, error, err);
         }
      }
   }

   return arr_val;
}


static Value builtin_hash_clear(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   int err;

   err = fixscript_clear_hash(heap, params[0]);
   if (err != FIXSCRIPT_SUCCESS) {
      return fixscript_error(heap, error, err);
   }
   return fixscript_int(0);
}


static Value builtin_heap_collect(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   fixscript_collect_heap(heap);
   return fixscript_int(0);
}


static Value builtin_heap_size(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   long long size = (fixscript_heap_size(heap) + 1023) >> 10;
   if (size > INT_MAX) {
      return fixscript_int(INT_MAX);
   }
   return fixscript_int((int)size);
}


static int get_time(uint64_t *time)
{
#if defined(_WIN32)
   uint64_t freq;
   QueryPerformanceFrequency((LARGE_INTEGER *)&freq);
   QueryPerformanceCounter((LARGE_INTEGER *)time);
   *time = (*time) * 1000000 / freq;
   return 1;
#elif defined(__APPLE__)
   mach_timebase_info_data_t info;
   *time = mach_absolute_time();
   mach_timebase_info(&info);
   *time = *time * info.numer / info.denom / 1000;
   return 1;
#else
   struct timespec ts;
   if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
      return 0;
   }
   *time = ((uint64_t)ts.tv_sec) * 1000000ULL + ((uint64_t)ts.tv_nsec / 1000ULL);
   return 1;
#endif
}


static Value builtin_perf_log(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   NativeFunction *log_func;
   Value value;
   uint64_t cur_time;
   char *s, *s2, *msg, buf[64];
   int err, len, len2;
   
   if (!get_time(&cur_time)) {
      return fixscript_int(0);
   }

   if (cur_time == 0) {
      cur_time++;
   }

   if (num_params == 0) {
      heap->perf_start_time = cur_time;
      heap->perf_last_time = cur_time;
      return fixscript_int(0);
   }

   if (heap->perf_start_time == 0) {
      heap->perf_start_time = cur_time;
      heap->perf_last_time = cur_time;
   }

   if (fixscript_is_string(heap, params[0])) {
      err = fixscript_get_string(heap, params[0], 0, -1, &s, &len);
   }
   else {
      err = fixscript_to_string(heap, params[0], 0, &s, &len);
   }
   if (err != FIXSCRIPT_SUCCESS) {
      return fixscript_error(heap, error, err);
   }
   
   if (heap->perf_last_time == heap->perf_start_time) {
      snprintf(buf, sizeof(buf), " [%.3f ms]", (cur_time - heap->perf_last_time)/1000.0);
   }
   else {
      #ifdef __wasm__
         cur_time++;
         heap->perf_start_time++;
      #endif
      snprintf(buf, sizeof(buf), " [%.3f ms; %.3f ms]", (cur_time - heap->perf_last_time)/1000.0, (cur_time - heap->perf_start_time)/1000.0);
      #ifdef __wasm__
         cur_time--;
         heap->perf_start_time--;
      #endif
   }
   for (s2=buf; *s2; s2++) {
      if (*s2 == ',') *s2 = '.';
      if (*s2 == ';') *s2 = ',';
   }
   len2 = strlen(buf);
   msg = malloc(len+len2+1);
   if (msg) {
      memcpy(msg, s, len);
      memcpy(msg+len, buf, len2+1);
   }
   free(s);
   if (!msg) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   value = fixscript_create_string(heap, msg, len+len2);
   free(msg);
   if (!value.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   #ifdef __wasm__
   if (cur_time <= heap->perf_last_time) {
      heap->perf_last_time = cur_time+1;
   }
   else
   #endif
   {
      heap->perf_last_time = cur_time;
   }

   log_func = string_hash_get(&heap->native_functions_hash, "log#1");
   return log_func->func(heap, error, 1, &value, log_func->data);
}


static Value builtin_serialize(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value buf, value;
   int err;

   if (num_params == 2) {
      buf = params[0];
      value = params[1];
   }
   else {
      buf = fixscript_int(0);
      value = params[0];
   }
   
   err = fixscript_serialize(heap, &buf, value);
   if (err != FIXSCRIPT_SUCCESS) {
      return fixscript_error(heap, error, err);
   }

   return buf;
}


static Value builtin_unserialize(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value buf, value, off_ref = fixscript_int(0), tmp;
   int err, off, len;

   buf = params[0];

   if (num_params == 3) {
      if (!fixscript_is_int(params[1])) {
         *error = fixscript_create_error_string(heap, "off must be an integer");
         return fixscript_int(0);
      }
      if (!fixscript_is_int(params[2])) {
         *error = fixscript_create_error_string(heap, "len must be an integer");
         return fixscript_int(0);
      }
      off = params[1].value;
      len = params[2].value;
   }
   else if (num_params == 2) {
      if (!fixscript_is_array(heap, params[1])) {
         *error = fixscript_create_error_string(heap, "off_ref must be an array");
         return fixscript_int(0);
      }
      off_ref = params[1];
      err = fixscript_get_array_elem(heap, off_ref, 0, &tmp);
      if (err != FIXSCRIPT_SUCCESS) {
         if (err == FIXSCRIPT_ERR_OUT_OF_BOUNDS) {
            *error = fixscript_create_error_string(heap, "off_ref must have at least one integer element");
            return fixscript_int(0);
         }
         return fixscript_error(heap, error, err);
      }
      if (!fixscript_is_int(tmp)) {
         *error = fixscript_create_error_string(heap, "off_ref must have at least one integer element");
         return fixscript_int(0);
      }
      off = tmp.value;
      len = -1;
   }
   else {
      off = 0;
      err = fixscript_get_array_length(heap, buf, &len);
      if (err) return fixscript_error(heap, error, err);
   }
   
   err = fixscript_unserialize(heap, buf, &off, len, &value);
   if (err != FIXSCRIPT_SUCCESS) {
      return fixscript_error(heap, error, err);
   }

   if (num_params == 2) {
      err = fixscript_set_array_elem(heap, off_ref, 0, fixscript_int(off));
      if (err != FIXSCRIPT_SUCCESS) return fixscript_error(heap, error, err);
   }

   return value;
}


static int get_public_funcs(Script *script, int *list_cnt_out, void ***list_out)
{
   Function *func;
   void **list;
   int i, min_value, max_value, list_cnt;

   min_value = INT_MAX;
   max_value = INT_MIN;
   for (i=0; i<script->functions.size; i+=2) {
      if (script->functions.data[i+0]) {
         func = script->functions.data[i+1];
         min_value = MIN(min_value, func->id);
         max_value = MAX(max_value, func->id);
      }
   }
   list_cnt = max_value - min_value + 1;
   list = calloc(list_cnt, sizeof(void *));
   if (!list) {
      return FIXSCRIPT_ERR_OUT_OF_MEMORY;
   }
   for (i=0; i<script->functions.size; i+=2) {
      if (script->functions.data[i+0]) {
         func = script->functions.data[i+1];
         if (!func->local) {
            list[func->id - min_value] = script->functions.data[i+0];
         }
      }
   }
   *list_cnt_out = list_cnt;
   *list_out = list;
   return FIXSCRIPT_SUCCESS;
}


static Value builtin_script_query(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#ifdef FIXEMBED_TOKEN_DUMP
   Heap *script_heap = heap->script_heap? heap->script_heap : heap;
#else
   Heap *script_heap = heap;
#endif
   Script *script;
   Value values[64];
   Constant *constant;
   void **list;
   int64_t clen;
   Value key, value;
   char *name, *s, *s2;
   int i, err, cnt, total_cnt, len, min_value, max_value, list_cnt;

   if (!heap->cur_load_func) {
      *error = fixscript_create_error_string(heap, "cannot be called outside token processing");
      return fixscript_int(0);
   }

   if (heap->cur_import_recursion >= MAX_IMPORT_RECURSION) {
      *error = fixscript_create_error_string(heap, "maximum import recursion limit reached");
      return fixscript_int(0);
   }

   err = fixscript_get_string(heap, params[0], 0, -1, &name, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   script = heap->cur_load_func(script_heap, name, error, heap->cur_load_data);
   free(name);
   if (!script) {
      if (fixscript_is_string(heap, *error)) {
         *error = fixscript_create_error(heap, *error);
      }
      return fixscript_int(0);
   }

   if (params[1].value) {
      value = fixscript_create_string(heap, string_hash_find_name(&script_heap->scripts, script), -1);
      err = fixscript_get_array_length(heap, value, &len);
      if (!err) {
         err = fixscript_set_array_length(heap, params[1], len);
      }
      if (!err) {
         err = fixscript_copy_array(heap, params[1], 0, value, 0, len);
      }
      if (err) {
         return fixscript_error(heap, error, err);
      }
   }

   if (params[2].value) {
      clen = (int64_t)script->constants.len * 2;
      if (clen > INT_MAX) {
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
      list = calloc(clen, sizeof(void *));
      if (!list) {
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
      for (i=0; i<script->constants.size; i+=2) {
         if (script->constants.data[i+0]) {
            constant = script->constants.data[i+1];
            if (constant->idx < 0 || constant->idx >= script->constants.len) {
               free(list);
               *error = fixscript_create_error_string(heap, "internal error: invalid constant index");
               return fixscript_int(0);
            }
            list[constant->idx*2+0] = script->constants.data[i+0];
            list[constant->idx*2+1] = constant;
         }
      }
      for (i=0; i<script->constants.len; i++) {
         s = list[i*2+0];
         constant = list[i*2+1];
         if (!constant) {
            free(list);
            *error = fixscript_create_error_string(heap, "internal error: invalid constant index");
            return fixscript_int(0);
         }
         if (constant->local) {
            s = malloc(1+strlen(s)+1);
            if (!s) {
               free(list);
               return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
            }
            strcpy(s, "@");
            strcat(s, list[i*2+0]);
         }
         key = fixscript_create_string(heap, s, -1);
         if (constant->local) {
            free(s);
         }
         if (!key.value) {
            free(list);
            return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         }
         if (constant->ref_script && constant->ref_constant) {
            value = fixscript_create_array(heap, 3);
            #ifdef FIXEMBED_TOKEN_DUMP
               if (script_heap != heap) {
                  if (fixscript_clone_between(heap, script_heap, constant->value, &values[0], NULL, NULL, NULL) != 0) {
                     free(list);
                     return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
                  }
               }
               else {
                  values[0] = constant->value;
               }
            #else
               values[0] = constant->value;
            #endif
            values[1] = fixscript_create_string(heap, string_hash_find_name(&script_heap->scripts, constant->ref_script), -1);
            s = (char *)string_hash_find_name(&constant->ref_script->constants, constant->ref_constant);
            if (constant->ref_constant->local) {
               s2 = s;
               s = malloc(1+strlen(s)+1);
               if (!s) {
                  free(list);
                  return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
               }
               strcpy(s, "@");
               strcat(s, s2);
            }
            values[2] = fixscript_create_string(heap, s, -1);
            if (constant->ref_constant->local) {
               free(s);
            }
            if (!values[1].value || !values[2].value) {
               free(list);
               return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
            }
            err = fixscript_set_array_range(heap, value, 0, 3, values);
            if (err) {
               free(list);
               return fixscript_error(heap, error, err);
            }
         }
         else {
            #ifdef FIXEMBED_TOKEN_DUMP
               if (script_heap != heap) {
                  if (fixscript_clone_between(heap, script_heap, constant->value, &value, NULL, NULL, NULL) != 0) {
                     free(list);
                     return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
                  }
               }
               else {
                  value = constant->value;
               }
            #else
               value = constant->value;
            #endif
         }
         err = fixscript_set_hash_elem(heap, params[2], key, value);
         if (err) {
            free(list);
            return fixscript_error(heap, error, err);
         }
      }
      free(list);
   }

   if (params[3].value) {
      min_value = INT_MAX;
      max_value = INT_MIN;
      for (i=0; i<script->locals.size; i+=2) {
         if (script->locals.data[i+0] && (intptr_t)script->locals.data[i+1] > 0) {
            min_value = MIN(min_value, (intptr_t)script->locals.data[i+1]);
            max_value = MAX(max_value, (intptr_t)script->locals.data[i+1]);
         }
      }
      cnt = max_value - min_value + 1;
      list = calloc(cnt, sizeof(void *));
      if (!list) {
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
      for (i=0; i<script->locals.size; i+=2) {
         if (script->locals.data[i+0] && (intptr_t)script->locals.data[i+1] > 0) {
            list[(intptr_t)script->locals.data[i+1] - min_value] = script->locals.data[i+0];
         }
      }
      for (i=0; i<cnt; i++) {
         if (!list[i]) continue;
         value = fixscript_create_string(heap, list[i], -1);
         if (!value.value) {
            free(list);
            return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         }
         err = fixscript_append_array_elem(heap, params[3], value);
         if (err) {
            free(list);
            return fixscript_error(heap, error, err);
         }
      }
      free(list);
   }

   if (params[4].value) {
      err = get_public_funcs(script, &list_cnt, &list);
      if (err) {
         return fixscript_error(heap, error, err);
      }

      cnt = 0;
      total_cnt = 0;
      for (i=0; ; i++) {
         if (i >= list_cnt || cnt == 64) {
            err = fixscript_set_array_length(heap, params[4], total_cnt + cnt);
            if (!err) {
               err = fixscript_set_array_range(heap, params[4], total_cnt, cnt, values);
            }
            if (err) {
               free(list);
               return fixscript_error(heap, error, err);
            }
            total_cnt += cnt;
            cnt = 0;
            if (i >= list_cnt) break;
         }
         
         if (list[i]) {
            values[cnt] = fixscript_create_string(heap, list[i], -1);
            if (!values[cnt].value) {
               free(list);
               return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
            }
            cnt++;
         }
      }
      free(list);
   }

   return fixscript_int(0);
}


static int next_token(Tokenizer *tok);
static char *get_token_string(Tokenizer *tok);
static int extract_tokens(Tokenizer *tok, Heap *heap, Value tokens_val, int src_off);

static Value builtin_script_line(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Parser *par;
   Tokenizer tok;
   Value tokens, src, ret, values[64*TOK_SIZE], stack_trace_lines;
   char *fname = NULL, *s, *serialized;
   int i, j, line, pos, err, remaining, num, state;

   if (!heap->cur_load_func) {
      *error = fixscript_create_error_string(heap, "cannot be called outside token processing");
      return fixscript_int(0);
   }

   par = heap->cur_parser;
   if (num_params == 1) {
      tokens = par->tokens_arr_val;
      src = par->tokens_src_val;
      line = params[0].value;
   }
   else {
      tokens = params[1];
      src = params[2];
      line = params[3].value;
   }

   err = fixscript_get_array_length(heap, tokens, &remaining);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   if (remaining % TOK_SIZE != 0) {
      *error = fixscript_create_error_string(heap, "invalid token array length (must be divisible by token size)");
      return fixscript_int(0);
   }

   stack_trace_lines = fixscript_int(0);
   state = 0;
   pos = 0;

   while (remaining > 0) {
      num = MIN(remaining, sizeof(values)/sizeof(Value));
      err = fixscript_get_array_range(heap, tokens, pos, num, values);
      if (err) {
         return fixscript_error(heap, error, err);
      }
      for (i=0; i<num; i+=TOK_SIZE) {
         switch (state) {
            case 0: // searching for 'const'
               if (values[i+TOK_type].value == KW_CONST) {
                  state = 1;
               }
               break;

            case 1: // expecting '@'
               if (values[i+TOK_type].value == '@') {
                  state = 2;
                  break;
               }
               state = 0;
               break;

            case 2: // checking for 'stack_trace_lines'
               if (values[i+TOK_type].value == TOK_IDENT && values[i+TOK_len].value == 17) {
                  err = fixscript_get_string(heap, src, values[i+TOK_off].value, values[i+TOK_len].value, &s, NULL);
                  if (err) {
                     return fixscript_error(heap, error, err);
                  }
                  if (strcmp(s, "stack_trace_lines") == 0) {
                     free(s);
                     state = 3;
                     break;
                  }
                  free(s);
               }
               state = 0;
               break;

            case 3: // expecting '='
               if (values[i+TOK_type].value == '=') {
                  state = 4;
                  break;
               }
               state = 0;
               break;

            case 4: // expecting string literal
               if (values[i+TOK_type].value == TOK_STRING) {
                  s = malloc(values[i+TOK_len].value+1);
                  if (!s) {
                     return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
                  }
                  err = fixscript_get_array_bytes(heap, src, values[i+TOK_off].value, values[i+TOK_len].value, s);
                  if (err) {
                     free(s);
                     return fixscript_error(heap, error, err);
                  }
                  s[values[i+TOK_len].value] = 0;

                  memset(&tok, 0, sizeof(Tokenizer));
                  tok.cur = s;
                  tok.start = s;

                  if (!next_token(&tok) || *tok.cur != 0 || tok.type != TOK_STRING) {
                     free(s);
                     remaining = 0;
                     break;
                  }

                  serialized = get_token_string(&tok);
                  free(s);

                  for (j=0; j<tok.num_utf8_bytes; j++) {
                     if ((unsigned char)serialized[j] == 0xFF) {
                        serialized[j] = 0;
                     }
                  }

                  stack_trace_lines = fixscript_create_string(heap, serialized, tok.num_utf8_bytes);
                  free(serialized);
               }
               goto end;
         }
      }
      pos += num;
      remaining -= num;
   }
   end:;

   if (stack_trace_lines.value) {
      process_stack_trace_lines(heap, stack_trace_lines, fixscript_int(0), &fname, &line);
   }
   
   if (!fname && num_params == 4 && params[0].value) {
      err = fixscript_get_string(heap, params[0], 0, -1, &fname, NULL);
      if (err) {
         return fixscript_error(heap, error, err);
      }
   }

   if (fname) {
      s = malloc(strlen(fname)+11+2+1);
      if (!s) {
         free(fname);
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
      sprintf(s, "%s(%d)", fname, line);
      free(fname);
   }
   else {
      s = malloc(strlen(par->fname)+11+2+1);
      if (!s) {
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
      sprintf(s, "%s(%d)", par->fname, line);
   }

   ret = fixscript_create_string(heap, s, -1);
   free(s);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return ret;
}


static Value builtin_script_postprocess(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   int err, func_num_params, orig_len;

   if (!heap->cur_load_func) {
      *error = fixscript_create_error_string(heap, "cannot be called outside token processing");
      return fixscript_int(0);
   }

   err = fixscript_get_function_name(heap, params[0], NULL, NULL, &func_num_params);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   if (func_num_params != 4) {
      *error = fixscript_create_error_string(heap, "invalid number of parameters in provided callback");
      return fixscript_int(0);
   }

   if (!heap->cur_postprocess_funcs) {
      heap->cur_postprocess_funcs = calloc(1, sizeof(DynArray));
      if (!heap->cur_postprocess_funcs) {
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
   }

   orig_len = heap->cur_postprocess_funcs->len;
   err = dynarray_add(heap->cur_postprocess_funcs, (void *)(intptr_t)params[0].value);
   if (!err) {
      err = dynarray_add(heap->cur_postprocess_funcs, (void *)(intptr_t)params[1].value);
   }
   if (!err) {
      err = dynarray_add(heap->cur_postprocess_funcs, (void *)(intptr_t)params[1].is_array);
   }
   if (err) {
      heap->cur_postprocess_funcs->len = orig_len;
      return fixscript_error(heap, error, err);
   }

   fixscript_ref(heap, params[1]);
   return fixscript_int(0);
}


static const char *use_tokens(Heap *token_heap, Value tokens_val, Value source_val, Parser *par);
static Script *load_script(Heap *heap, const char *src, const char *fname, Value *error, int long_jumps, int long_func_refs, LoadScriptFunc load_func, void *load_data, Parser *reuse_tokens, int reload);

static Value builtin_script_compile(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Script *script;
   Value ret, key, value;
   Parser reuse_tokens;
   char *src = NULL, *s;
   char buf[128];
   const char *err_str;
   void **list;
   int i, err, cnt, len;

   if (!heap->cur_load_func) {
      *error = fixscript_create_error_string(heap, "cannot be called outside token processing");
      return fixscript_int(0);
   }

   if (num_params == 2) {
      reuse_tokens.tokens_src = NULL;
      reuse_tokens.tokens_arr = NULL;
      reuse_tokens.tokens_arr_val = params[0];
      reuse_tokens.tokens_src_val = params[1];
      reuse_tokens.semicolon_removed = 1;

      err_str = use_tokens(heap, params[0], params[1], &reuse_tokens);
      if (err_str) {
         *error = fixscript_create_error_string(heap, err_str);
         return fixscript_int(0);
      }

      fixscript_ref(heap, reuse_tokens.tokens_arr_val);
      fixscript_ref(heap, reuse_tokens.tokens_src_val);
   }
   else {
      err = fixscript_get_array_length(heap, params[0], &len);
      if (err) {
         return fixscript_error(heap, error, err);
      }

      src = malloc(len+1);
      if (!src) {
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }

      err = fixscript_get_array_bytes(heap, params[0], 0, len, src);
      if (err) {
         free(src);
         return fixscript_error(heap, error, err);
      }
      for (i=0; i<len; i++) {
         if (src[i] == 0) {
            free(src);
            return fixscript_error(heap, error, FIXSCRIPT_ERR_INVALID_NULL_STRING);
         }
      }
      src[len] = 0;
   }

   snprintf(buf, sizeof(buf), "fixscript:compile/%d.fix", heap->compile_counter++);

   if (num_params == 2) {
      script = load_script(heap, "", buf, error, 0, 0, heap->cur_load_func, heap->cur_load_data, &reuse_tokens, 0);
   }
   else {
      script = fixscript_load(heap, src, buf, error, heap->cur_load_func, heap->cur_load_data);
      free(src);
   }

   if (!script) {
      if (fixscript_is_string(heap, *error)) {
         *error = fixscript_create_error(heap, *error);
      }
      err = fixscript_get_array_elem(heap, *error, 0, &value);
      if (!err) {
         err = fixscript_get_string(heap, value, 0, -1, &s, NULL);
      }
      if (err) {
         return fixscript_error(heap, error, err);
      }
      if (strncmp(s, buf, strlen(buf)) == 0) {
         value = fixscript_create_string(heap, s+strlen(buf), -1);
         if (!value.value) {
            return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         }
         err = fixscript_set_array_elem(heap, *error, 0, value);
         if (err) {
            return fixscript_error(heap, error, err);
         }
      }
      return fixscript_int(0);
   }

   ret = fixscript_create_hash(heap);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   err = get_public_funcs(script, &cnt, &list);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   for (i=0; i<cnt; i++) {
      if (!list[i]) continue;

      key = fixscript_create_string(heap, list[i], -1);
      if (!key.value) {
         err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
         break;
      }
      value = fixscript_get_function(heap, script, list[i]);
      if (!value.value) {
         err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
         break;
      }
      err = fixscript_set_hash_elem(heap, ret, key, value);
      if (err) break;
   }
   
   free(list);

   if (err) {
      return fixscript_error(heap, error, err);
   }
   return ret;
}


static Value builtin_tokens_parse(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Tokenizer tok;
   char *src;
   int i, err, off, len, src_off, line;

   if (!heap->cur_load_func) {
      *error = fixscript_create_error_string(heap, "cannot be called outside token processing");
      return fixscript_int(0);
   }

   if (!fixscript_is_array(heap, params[0]) || fixscript_is_string(heap, params[0])) {
      *error = fixscript_create_error_string(heap, "tokens must be an array");
      return fixscript_int(0);
   }

   if (num_params == 6) {
      off = fixscript_get_int(params[3]);
      len = fixscript_get_int(params[4]);
      line = fixscript_get_int(params[5]);
   }
   else {
      off = 0;
      err = fixscript_get_array_length(heap, params[2], &len);
      if (err) {
         return fixscript_error(heap, error, err);
      }
      line = fixscript_get_int(params[3]);
   }

   src = malloc(len+1);
   if (!src) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   err = fixscript_get_array_bytes(heap, params[2], off, len, src);
   if (err) {
      free(src);
      return fixscript_error(heap, error, err);
   }
   for (i=0; i<len; i++) {
      if (src[i] == 0) {
         free(src);
         return fixscript_error(heap, error, FIXSCRIPT_ERR_INVALID_NULL_STRING);
      }
   }
   src[len] = 0;

   err = fixscript_get_array_length(heap, params[1], &src_off);
   if (err) {
      free(src);
      return fixscript_error(heap, error, err);
   }
   err = fixscript_set_array_length(heap, params[1], src_off + len);
   if (!err) {
      err = fixscript_set_array_bytes(heap, params[1], src_off, len, src);
   }
   if (err) {
      free(src);
      return fixscript_error(heap, error, err);
   }

   memset(&tok, 0, sizeof(Tokenizer));
   tok.cur = src;
   tok.start = src;
   tok.line = line;

   if (!extract_tokens(&tok, heap, params[0], src_off) || *tok.cur != '\0') {
      free(src);
      *error = fixscript_create_error_string(heap, "syntax error");
      return fixscript_int(0);
   }
   free(src);

   return params[0];
}


static Value builtin_token_parse_string(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Tokenizer tok;
   Value ret;
   char *src, *result;
   int i, err, off, len;

   if (!heap->cur_load_func) {
      *error = fixscript_create_error_string(heap, "cannot be called outside token processing");
      return fixscript_int(0);
   }

   if (num_params == 3) {
      off = fixscript_get_int(params[1]);
      len = fixscript_get_int(params[2]);
   }
   else {
      off = 0;
      err = fixscript_get_array_length(heap, params[0], &len);
      if (err) {
         return fixscript_error(heap, error, err);
      }
   }

   src = malloc(len+1);
   if (!src) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   err = fixscript_get_array_bytes(heap, params[0], off, len, src);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   for (i=0; i<len; i++) {
      if (src[i] == 0) {
         free(src);
         return fixscript_error(heap, error, FIXSCRIPT_ERR_INVALID_NULL_STRING);
      }
   }
   src[len] = 0;

   memset(&tok, 0, sizeof(Tokenizer));
   tok.cur = src;
   tok.start = src;

   if (!next_token(&tok) || *tok.cur != 0 || (tok.type != TOK_STRING && tok.type != TOK_CHAR)) {
      free(src);
      *error = fixscript_create_error_string(heap, "syntax error");
      return fixscript_int(0);
   }

   tok.type = TOK_STRING;
   result = get_token_string(&tok);
   free(src);

   for (i=0; i<tok.num_utf8_bytes; i++) {
      if ((unsigned char)result[i] == 0xFF) {
         result[i] = 0;
      }
   }
   ret = fixscript_create_string(heap, result, tok.num_utf8_bytes);
   free(result);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   return ret;
}


static char get_hex_char(int value)
{
   if (value >= 0 && value <= 9) return '0' + value;
   return 'A'+(value-10);
}


static Value builtin_token_escape_string(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value ret;
   char *str, *dest, *p;
   int i, err, len, dest_len;

   if (!heap->cur_load_func) {
      *error = fixscript_create_error_string(heap, "cannot be called outside token processing");
      return fixscript_int(0);
   }

   err = fixscript_get_string(heap, params[0], 0, -1, &str, &len);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   dest_len = 2;
   for (i=0; i<len; i++) {
      switch (str[i]) {
         case '\r':
         case '\n':
         case '\t':
         case '\\':
         case '\'':
         case '\"':
            dest_len += 2;
            break;
            
         default:
            if (str[i] >= 0 && str[i] < 32) {
               dest_len += 3;
            }
            else {
               dest_len++;
            }
            break;
      }
   }

   dest = malloc(dest_len);
   p = dest;
   *p++ = '\"';
   for (i=0; i<len; i++) {
      switch (str[i]) {
         case '\r': *p++ = '\\'; *p++ = 'r'; break;
         case '\n': *p++ = '\\'; *p++ = 'n'; break;
         case '\t': *p++ = '\\'; *p++ = 't'; break;
         case '\\': *p++ = '\\'; *p++ = '\\'; break;
         case '\'': *p++ = '\\'; *p++ = '\''; break;
         case '\"': *p++ = '\\'; *p++ = '\"'; break;
            
         default:
            if (str[i] >= 0 && str[i] < 32) {
               *p++ = '\\';
               *p++ = get_hex_char(str[i] >> 4);
               *p++ = get_hex_char(str[i] & 0xF);
            }
            else {
               *p++ = str[i];
            }
            break;
      }
   }
   *p++ = '\"';
   free(str);

   ret = fixscript_create_byte_array(heap, dest, dest_len);
   free(dest);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   heap->data[ret.value].is_string = 1;
   return ret;
}


Heap *fixscript_create_heap()
{
   Heap *heap;
   int i;

   heap = calloc(1, sizeof(Heap));
   heap->size = 256;
   heap->data = calloc(heap->size, sizeof(Array));
   heap->reachable = calloc(heap->size >> 4, sizeof(int));
   heap->next_idx = 1;
   for (i=0; i<heap->size; i++) {
      heap->data[i].len = -1;
   }
   heap->total_size = sizeof(Heap) + heap->size*sizeof(Array);
   heap->total_cap = 16384;
   heap->max_stack_size = DEFAULT_MAX_STACK_SIZE;

   heap->stack_cap = 8;
   heap->stack_data = calloc(heap->stack_cap, sizeof(int));
   heap->stack_flags = calloc(heap->stack_cap, sizeof(char));
   heap->total_size += heap->stack_cap * sizeof(int) + heap->stack_cap * sizeof(char);

   // reserve index 0 for PC to return from the interpreter:
   heap->bytecode = calloc(1, 1);
   heap->bytecode_size = 1;

   // reserve index 0 so the allocated indicies can be used as values in string hash:
   heap->locals_len = 1;
   heap->locals_cap = 8;
   heap->locals_data = calloc(heap->locals_cap, sizeof(int));
   heap->locals_flags = calloc(heap->locals_cap, sizeof(char));
   heap->total_size += heap->locals_cap * sizeof(int) + heap->locals_cap * sizeof(char);

   // reserve index 0 so function references are not confused with false:
   dynarray_add(&heap->functions, NULL);

   #ifndef FIXSCRIPT_NO_JIT
      heap->jit_array_get_funcs = calloc(heap->size, sizeof(uint8_t));
      heap->jit_array_set_funcs = calloc(heap->size * 2, sizeof(uint8_t));
      heap->jit_array_append_funcs = calloc(heap->size * 2, sizeof(uint8_t));
   #endif

   fixscript_register_native_func(heap, "log#1", builtin_log, NULL);
   fixscript_register_native_func(heap, "dump#1", builtin_dump, NULL);
   fixscript_register_native_func(heap, "to_string#1", builtin_to_string, NULL);
   fixscript_register_native_func(heap, "to_string#2", builtin_to_string, NULL);
   fixscript_register_native_func(heap, "error#1", builtin_error, NULL);
   fixscript_register_native_func(heap, "clone#1", builtin_clone, (void *)0);
   fixscript_register_native_func(heap, "clone_deep#1", builtin_clone, (void *)1);
   fixscript_register_native_func(heap, "array_create#1", builtin_array_create, NULL);
   fixscript_register_native_func(heap, "array_create#2", builtin_array_create, NULL);
   fixscript_register_native_func(heap, "array_create_shared#2", builtin_array_create_shared, NULL);
   fixscript_register_native_func(heap, "array_get_shared_count#1", builtin_array_get_shared_count, NULL);
   fixscript_register_native_func(heap, "array_get_element_size#1", builtin_array_get_element_size, NULL);
   fixscript_register_native_func(heap, "array_set_length#2", builtin_array_set_length, NULL);
   fixscript_register_native_func(heap, "array_copy#5", builtin_array_copy, NULL);
   fixscript_register_native_func(heap, "array_fill#2", builtin_array_fill, NULL);
   fixscript_register_native_func(heap, "array_fill#4", builtin_array_fill, NULL);
   fixscript_register_native_func(heap, "array_extract#3", builtin_array_extract, NULL);
   fixscript_register_native_func(heap, "array_insert#3", builtin_array_insert, NULL);
   fixscript_register_native_func(heap, "array_insert_array#3", builtin_array_insert_array, NULL);
   fixscript_register_native_func(heap, "array_insert_array#5", builtin_array_insert_array, NULL);
   fixscript_register_native_func(heap, "array_append#2", builtin_array_append, NULL);
   fixscript_register_native_func(heap, "array_append#4", builtin_array_append, NULL);
   fixscript_register_native_func(heap, "array_replace_range#4", builtin_array_replace_range, NULL);
   fixscript_register_native_func(heap, "array_replace_range#6", builtin_array_replace_range, NULL);
   fixscript_register_native_func(heap, "array_remove#2", builtin_array_remove, NULL);
   fixscript_register_native_func(heap, "array_remove#3", builtin_array_remove, NULL);
   fixscript_register_native_func(heap, "array_clear#1", builtin_array_set_length, NULL);
   fixscript_register_native_func(heap, "string_const#1", builtin_string_const, NULL);
   fixscript_register_native_func(heap, "string_const#3", builtin_string_const, NULL);
   fixscript_register_native_func(heap, "string_parse_int#1", builtin_string_parse_single, (void *)0);
   fixscript_register_native_func(heap, "string_parse_int#2", builtin_string_parse_single, (void *)0);
   fixscript_register_native_func(heap, "string_parse_int#3", builtin_string_parse_single, (void *)0);
   fixscript_register_native_func(heap, "string_parse_int#4", builtin_string_parse_single, (void *)0);
   fixscript_register_native_func(heap, "string_parse_float#1", builtin_string_parse_single, (void *)1);
   fixscript_register_native_func(heap, "string_parse_float#2", builtin_string_parse_single, (void *)1);
   fixscript_register_native_func(heap, "string_parse_float#3", builtin_string_parse_single, (void *)1);
   fixscript_register_native_func(heap, "string_parse_float#4", builtin_string_parse_single, (void *)1);
   fixscript_register_native_func(heap, "string_parse_long#1", builtin_string_parse_double, (void *)0);
   fixscript_register_native_func(heap, "string_parse_long#3", builtin_string_parse_double, (void *)0);
   fixscript_register_native_func(heap, "string_parse_long#5", builtin_string_parse_double, (void *)0);
   fixscript_register_native_func(heap, "string_parse_double#1", builtin_string_parse_double, (void *)1);
   fixscript_register_native_func(heap, "string_parse_double#3", builtin_string_parse_double, (void *)1);
   fixscript_register_native_func(heap, "string_parse_double#5", builtin_string_parse_double, (void *)1);
   fixscript_register_native_func(heap, "string_from_long#2", builtin_string_from_double, (void *)0);
   fixscript_register_native_func(heap, "string_from_long#3", builtin_string_from_double, (void *)0);
   fixscript_register_native_func(heap, "string_from_double#2", builtin_string_from_double, (void *)1);
   fixscript_register_native_func(heap, "string_from_double#3", builtin_string_from_double, (void *)1);
   fixscript_register_native_func(heap, "string_from_utf8#1", builtin_string_from_utf8, NULL);
   fixscript_register_native_func(heap, "string_from_utf8#2", builtin_string_from_utf8, NULL);
   fixscript_register_native_func(heap, "string_from_utf8#3", builtin_string_from_utf8, NULL);
   fixscript_register_native_func(heap, "string_from_utf8#4", builtin_string_from_utf8, NULL);
   fixscript_register_native_func(heap, "string_to_utf8#1", builtin_string_to_utf8, NULL);
   fixscript_register_native_func(heap, "string_to_utf8#2", builtin_string_to_utf8, NULL);
   fixscript_register_native_func(heap, "string_to_utf8#3", builtin_string_to_utf8, NULL);
   fixscript_register_native_func(heap, "string_to_utf8#4", builtin_string_to_utf8, NULL);
   fixscript_register_native_func(heap, "object_create#1", builtin_array_set_length, (void *)1);
   fixscript_register_native_func(heap, "object_extend#2", builtin_array_set_length, (void *)1);
   fixscript_register_native_func(heap, "weakref_create#1", builtin_weakref_create, NULL);
   fixscript_register_native_func(heap, "weakref_create#2", builtin_weakref_create, NULL);
   fixscript_register_native_func(heap, "weakref_create#3", builtin_weakref_create, NULL);
   fixscript_register_native_func(heap, "weakref_get#1", builtin_weakref_get, NULL);
   fixscript_register_native_func(heap, "funcref_call#2", builtin_funcref_call, NULL);
   fixscript_register_native_func(heap, "hash_get#3", builtin_hash_get, NULL);
   fixscript_register_native_func(heap, "hash_entry#2", builtin_hash_entry, NULL);
   fixscript_register_native_func(heap, "hash_contains#2", builtin_hash_contains, NULL);
   fixscript_register_native_func(heap, "hash_remove#2", builtin_hash_remove, NULL);
   fixscript_register_native_func(heap, "hash_keys#1", builtin_hash_get_values, (void *)0);
   fixscript_register_native_func(heap, "hash_values#1", builtin_hash_get_values, (void *)1);
   fixscript_register_native_func(heap, "hash_pairs#1", builtin_hash_get_values, (void *)2);
   fixscript_register_native_func(heap, "hash_clear#1", builtin_hash_clear, NULL);
   fixscript_register_native_func(heap, "heap_collect#0", builtin_heap_collect, NULL);
   fixscript_register_native_func(heap, "heap_size#0", builtin_heap_size, NULL);
   fixscript_register_native_func(heap, "perf_reset#0", builtin_perf_log, NULL);
   fixscript_register_native_func(heap, "perf_log#1", builtin_perf_log, NULL);
   fixscript_register_native_func(heap, "serialize#1", builtin_serialize, NULL);
   fixscript_register_native_func(heap, "serialize#2", builtin_serialize, NULL);
   fixscript_register_native_func(heap, "unserialize#1", builtin_unserialize, NULL);
   fixscript_register_native_func(heap, "unserialize#2", builtin_unserialize, NULL);
   fixscript_register_native_func(heap, "unserialize#3", builtin_unserialize, NULL);
   fixscript_register_native_func(heap, "script_query#5", builtin_script_query, NULL);
   fixscript_register_native_func(heap, "script_line#1", builtin_script_line, NULL);
   fixscript_register_native_func(heap, "script_line#4", builtin_script_line, NULL);
   fixscript_register_native_func(heap, "script_postprocess#2", builtin_script_postprocess, NULL);
   fixscript_register_native_func(heap, "script_compile#1", builtin_script_compile, NULL);
   fixscript_register_native_func(heap, "script_compile#2", builtin_script_compile, NULL);
   fixscript_register_native_func(heap, "tokens_parse#4", builtin_tokens_parse, NULL);
   fixscript_register_native_func(heap, "tokens_parse#6", builtin_tokens_parse, NULL);
   fixscript_register_native_func(heap, "token_parse_string#1", builtin_token_parse_string, NULL);
   fixscript_register_native_func(heap, "token_parse_string#3", builtin_token_parse_string, NULL);
   fixscript_register_native_func(heap, "token_escape_string#1", builtin_token_escape_string, NULL);

   #ifdef FIXSCRIPT_ASYNC
      fixscript_set_time_limit(heap, -1);
   #endif
   return heap;
}


static void free_function(Function *func)
{
   free(func);
}


static void free_script(Script *script)
{
   int i;

   free(script->imports.data);

   for (i=0; i<script->constants.size; i+=2) {
      if (script->constants.data[i+0]) {
         free(script->constants.data[i+0]);
         free(script->constants.data[i+1]);
      }
   }
   free(script->constants.data);

   for (i=0; i<script->locals.size; i+=2) {
      if (script->locals.data[i+0]) {
         free(script->locals.data[i+0]);
      }
   }
   free(script->locals.data);

   for (i=0; i<script->functions.size; i+=2) {
      if (script->functions.data[i+0]) {
         free(script->functions.data[i+0]);
         free_function(script->functions.data[i+1]);
      }
   }
   free(script->functions.data);

   free(script);
}


void fixscript_free_heap(Heap *heap)
{
   Array *arr;
   HandleFreeFunc handle_free;
   HandleFunc handle_func;
   SharedArrayHandle *sah;
   void *handle_ptr;
   int i, handle_type;

   while (heap->handle_created) {
      heap->handle_created = 0;

      for (i=0; i<heap->size; i++) {
         arr = &heap->data[i];
         if (arr->len == -1) continue;
         if (arr->is_handle) {
            handle_type = arr->is_handle;
            if (handle_type == 2) {
               handle_func = arr->handle_func;
               arr->handle_func = NULL;
               arr->is_handle = 1;
               arr->handle_free = NULL;
            }
            else {
               handle_free = arr->handle_free;
               arr->handle_free = NULL;
            }
            handle_ptr = arr->handle_ptr;
            arr->handle_ptr = NULL;

            if (handle_type == 2) {
               handle_func(heap, HANDLE_OP_FREE, handle_ptr, NULL);
            }
            else if (handle_free) {
               handle_free(handle_ptr);
            }
         }
         if (arr->is_shared) {
            sah = ARRAY_SHARED_HEADER(arr);
            if (sah->refcnt < SAH_REFCNT_LIMIT && __sync_sub_and_fetch(&sah->refcnt, 1) == 0) {
               if (sah->free_func) {
                  sah->free_func(sah->free_data);
               }
               free(sah);
            }
            arr->data = NULL;
            arr->flags = NULL;
            arr->size = 0;
            arr->len = 0;
         }
      }
   }

   for (i=0; i<heap->size; i++) {
      arr = &heap->data[i];
      if (arr->len != -1 && !arr->is_handle && !arr->is_shared) {
         free(arr->flags);
         free(arr->data);
      }
   }
   free(heap->data);
   free(heap->reachable);

   free(heap->stack_data);
   free(heap->stack_flags);
   free(heap->locals_data);
   free(heap->locals_flags);

   free(heap->roots.data);
   free(heap->ext_roots.data);
   free(heap->bytecode);
   free(heap->lines);

   for (i=0; i<heap->scripts.size; i+=2) {
      if (heap->scripts.data[i+0]) {
         free(heap->scripts.data[i+0]);
         free_script(heap->scripts.data[i+1]);
      }
   }
   free(heap->scripts.data);

   free(heap->functions.data);

   for (i=0; i<heap->native_functions.len; i++) {
      free(heap->native_functions.data[i]);
   }
   free(heap->native_functions.data);

   for (i=0; i<heap->native_functions_hash.size; i+=2) {
      if (heap->native_functions_hash.data[i+0]) {
         free(heap->native_functions_hash.data[i+0]);
      }
   }
   free(heap->native_functions_hash.data);

   free(heap->error_stack.data);

   for (i=0; i<heap->weak_refs.size; i+=2) {
      if (heap->weak_refs.data[i+0]) {
         free(heap->weak_refs.data[i+0]);
      }
   }
   free(heap->weak_refs.data);

   for (i=0; i<heap->shared_arrays.size; i+=2) {
      if (heap->shared_arrays.data[i+0]) {
         free(heap->shared_arrays.data[i+0]);
      }
   }
   free(heap->shared_arrays.data);

   for (i=0; i<heap->user_data.len; i+=2) {
      if (heap->user_data.data[i+0] && heap->user_data.data[i+1]) {
         ((HandleFreeFunc)heap->user_data.data[i+1])(heap->user_data.data[i+0]);
      }
   }
   free(heap->user_data.data);

   free(heap->compiler_error);

   free(heap->const_string_set.data);

   #ifdef FIXSCRIPT_ASYNC
      free(heap->async_continuations.data);
   #endif

   #ifndef FIXSCRIPT_NO_JIT
      if (heap->jit_code) {
         #ifdef _WIN32
            VirtualFree(heap->jit_code, 0, MEM_RELEASE);
         #else
            munmap(heap->jit_code, heap->jit_code_cap);
         #endif
      }
      free(heap->jit_pc_mappings.data);
      free(heap->jit_heap_data_refs.data);
      free(heap->jit_array_get_refs.data);
      free(heap->jit_array_set_refs.data);
      free(heap->jit_array_append_refs.data);
      free(heap->jit_length_refs.data);
      free(heap->jit_adjustments.data);
      free(heap->jit_array_get_funcs);
      free(heap->jit_array_set_funcs);
      free(heap->jit_array_append_funcs);
   #endif

   free(heap);
}


long long fixscript_heap_size(Heap *heap)
{
#if 0
   Array *arr;
   int i, size, alloc_size, used=0, block=0;
   char buf[256];
   
   buf[0] = 0;
   size = sizeof(Heap) + heap->size * sizeof(Array) + heap->roots.size * sizeof(void *) + heap->ext_roots.size * sizeof(void *);
   size += heap->stack_cap * sizeof(int) + heap->stack_cap * sizeof(char);
   size += heap->locals_cap * sizeof(int) + heap->locals_cap * sizeof(char);
   for (i=0; i<heap->size; i++) {
      arr = &heap->data[i];
      if (arr->len != -1 && !arr->is_handle) {
         alloc_size = arr->size;
         if (arr->hash_slots >= 0) {
            alloc_size = 1<<arr->size;
            size += (FLAGS_SIZE(alloc_size*2) + bitarray_size(arr->size-1, alloc_size)) * sizeof(int); // flags
         }
         else {
            size += FLAGS_SIZE(arr->size) * sizeof(int); // flags
         }
         if (arr->type == ARR_BYTE) {
            size += alloc_size * sizeof(unsigned char);
         }
         else if (arr->type == ARR_SHORT) {
            size += alloc_size * sizeof(unsigned short);
         }
         else {
            size += alloc_size * sizeof(int);
         }
      }
      if (arr->len != -1) {
         used++;
         block = 1;
      }
      if ((i % 4096) == 4095) {
         strcat(buf, block? "1": "0");
         block = 0;
      }
   }

   if (size != heap->total_size) {
      #ifdef _WIN32
         fprintf(stderr, "heap size mismatch %I64d != %d (delta: %I64d)\n", heap->total_size, size, heap->total_size - size);
      #else
         fprintf(stderr, "heap size mismatch %lld != %d (delta: %lld)\n", heap->total_size, size, heap->total_size - size);
      #endif
      fflush(stderr);
   }
   else {
      #ifdef _WIN32
         fprintf(stderr, "heap size = %I64d (%d/%d = %I64d, blocks=%s)\n", heap->total_size, used, heap->size, (int64_t)heap->size * sizeof(Array), buf);
      #else
         fprintf(stderr, "heap size = %lld (%d/%d = %lld, blocks=%s)\n", heap->total_size, used, heap->size, (int64_t)heap->size * sizeof(Array), buf);
      #endif
      fflush(stderr);
   }
#endif

   return heap->total_size;
}


void fixscript_adjust_heap_size(Heap *heap, long long relative_change)
{
   heap->total_size += relative_change;
}


void fixscript_set_max_stack_size(Heap *heap, int size)
{
   heap->max_stack_size = size;
}


int fixscript_get_max_stack_size(Heap *heap)
{
   return heap->max_stack_size;
}


int fixscript_get_stack_size(Heap *heap)
{
   return heap->stack_cap;
}


void fixscript_ref(Heap *heap, Value value)
{
   Array *arr;
   int old_size;

   if (!value.is_array || value.value <= 0 || value.value >= heap->size) return;
   arr = &heap->data[value.value];
   if (arr->len == -1) return;

   if (arr->ext_refcnt >= EXT_REFCNT_LIMIT) {
      return;
   }
   if (arr->ext_refcnt++ == 0) {
      old_size = heap->ext_roots.size;
      dynarray_add(&heap->ext_roots, (void *)(intptr_t)value.value);
      heap->total_size += (int64_t)(heap->ext_roots.size - old_size) * sizeof(void *);
   }
}


void fixscript_unref(Heap *heap, Value value)
{
   Array *arr;

   if (!value.is_array || value.value <= 0 || value.value >= heap->size) return;
   arr = &heap->data[value.value];
   if (arr->len == -1) return;

   if (arr->ext_refcnt >= EXT_REFCNT_LIMIT) {
      return;
   }
   if (--arr->ext_refcnt == 0) {
      dynarray_remove_value_fast(&heap->ext_roots, (void *)(intptr_t)value.value);
   }
}


void fixscript_set_protected(Heap *heap, Value value, int is_protected)
{
   Array *arr;

   if (!value.is_array || value.value <= 0 || value.value >= heap->size) return;
   arr = &heap->data[value.value];
   if (arr->len == -1) return;

   arr->is_protected = is_protected != 0;
}


int fixscript_is_protected(Heap *heap, Value value)
{
   Array *arr;

   if (!value.is_array || value.value <= 0 || value.value >= heap->size) return 0;
   arr = &heap->data[value.value];
   if (arr->len == -1) return 0;

   return arr->is_protected;
}


void fixscript_register_cleanup(Heap *heap, HandleFreeFunc free_func, void *data)
{
   fixscript_ref(heap, fixscript_create_handle(heap, CLEANUP_HANDLE_TYPE, data, free_func));
}


void fixscript_register_heap_key(volatile int *key)
{
   int new_key;
   
   if (*key == 0) {
      new_key = __sync_add_and_fetch(&heap_keys_cnt, 1);
      (void)__sync_val_compare_and_swap(key, 0, new_key);
   }
}


int fixscript_set_heap_data(Heap *heap, int key, void *data, HandleFreeFunc free_func)
{
   int err;
   void **p1, **p2;

   if (key <= 0) {
      if (data && free_func) {
         free_func(data);
      }
      return FIXSCRIPT_ERR_KEY_NOT_FOUND;
   }
   
   while (heap->user_data.len <= key*2+1) {
      err = dynarray_add(&heap->user_data, NULL);
      if (err) {
         if (data && free_func) {
            free_func(data);
         }
         return err;
      }
   }

   p1 = &heap->user_data.data[key*2+0];
   p2 = &heap->user_data.data[key*2+1];

   if ((*p1) && (*p2)) {
      ((HandleFreeFunc)(*p2))(*p1);
   }

   *p1 = data;
   *p2 = free_func;
   return FIXSCRIPT_SUCCESS;
}


void *fixscript_get_heap_data(Heap *heap, int key)
{
   if (key*2+0 < heap->user_data.len) {
      return heap->user_data.data[key*2+0];
   }
   return NULL;
}


void fixscript_set_time_limit(Heap *heap, int limit)
{
   if (limit < 0) {
      heap->time_limit = -1;
   }
   else if (limit == 0) {
      heap->time_limit = 0;
   }
   else {
      get_time(&heap->time_limit);
      heap->time_limit += ((uint64_t)limit) * 1000;
      if (heap->time_limit == 0 || heap->time_limit == -1) {
         heap->time_limit = 1;
      }
   }
   heap->stop_execution = 0;
}


int fixscript_get_remaining_time(Heap *heap)
{
   uint64_t time = 0;
   int64_t diff;

   if (heap->time_limit == 0) {
      return -1;
   }
   if (heap->stop_execution) {
      heap->time_counter = 0;
      return 0;
   }
   if (heap->time_limit == -1) {
      return -1;
   }
   get_time(&time);
   diff = (int64_t)(heap->time_limit - time);
   if (diff < 0) diff = 0;
   diff /= 1000;
   if (diff > INT_MAX) diff = INT_MAX;
   if (diff == 0) {
      heap->time_counter = 0;
   }
   return diff;
}


void fixscript_stop_execution(Heap *heap)
{
   heap->stop_execution = 1;
}


void fixscript_mark_ref(Heap *heap, Value value)
{
   Array *arr;

   if (heap->marking_limit == 0) return;

   if (value.is_array && value.value > 0 && value.value < heap->size) {
      arr = &heap->data[value.value];
      if (arr->len != -1) {
         if (mark_array(heap, value.value, abs(heap->marking_limit)-1)) {
            heap->marking_limit = -abs(heap->marking_limit);
         }
      }
   }
}


static int clone_value(Heap *dest, Heap *src, Value value, Value map, Value *clone, LoadScriptFunc load_func, void *load_data, Value *error, DynArray *queue, int recursion_limit);

Value fixscript_copy_ref(void *ctx, Value value)
{
   CopyContext *cc = ctx;
   Value clone;

   if (cc->err) {
      return fixscript_int(0);
   }
   cc->err = clone_value(cc->dest, cc->src, value, cc->map, &clone, cc->load_func, cc->load_data, cc->error, cc->queue, cc->recursion_limit);
   return clone;
}


static int indent(String *str, int level)
{
   int i;
   
   for (i=0; i<level; i++) {
      if (!string_append(str, "  ")) return 0;
   }
   return 1;
}


static int dump_value(Heap *heap, String *str, DynArray *stack, Value value, int newlines, int level)
{
   Value elem_val, key_val;
   Function *func;
   const char *script_name, *func_name;
   int i, len, dest_len, err, ok, hash_pos, type, func_id;
   char *s, *p, *dest;
   char buf[32];
   
   if (level >= MAX_DUMP_RECURSION) {
      if (!string_append(str, "(recursion limit reached)")) return FIXSCRIPT_ERR_OUT_OF_MEMORY;
      return FIXSCRIPT_SUCCESS;
   }
   
   if (value.is_array) {
      if (fixscript_is_float(value)) {
         snprintf(buf, sizeof(buf), "%.9g", fixscript_get_float(value));
         for (s=buf; *s; s++) {
            if (*s == ',') *s = '.';
         }
         s = strstr(buf, "e+");
         if (s) {
            memmove(s+1, s+2, strlen(s)-1);
         }
         if (!string_append(str, "%s", buf)) return FIXSCRIPT_ERR_OUT_OF_MEMORY;
         if (!strchr(buf, '.') && !strchr(buf, 'e')) {
            if (!string_append(str, ".0")) return FIXSCRIPT_ERR_OUT_OF_MEMORY;
         }
      }
      else if (fixscript_is_string(heap, value)) {
         err = fixscript_get_string(heap, value, 0, -1, &s, &len);
         if (err != FIXSCRIPT_SUCCESS) return err;

         dest_len = 2;
         for (i=0; i<len; i++) {
            switch (s[i]) {
               case '\0':
                  dest_len += 3;
                  break;

               case '\r':
               case '\n':
               case '\t':
               case '\\':
               case '\"':
                  dest_len += 2;
                  break;
               
               default:
                  if (s[i] >= 0 && s[i] < 32) {
                     dest_len += 3;
                  }
                  else {
                     dest_len++;
                  }
                  break;
            }
         }

         dest = malloc(dest_len+1);
         if (!dest) {
            free(s);
            return FIXSCRIPT_ERR_OUT_OF_MEMORY;
         }
         p = dest;
         *p++ = '\"';
         for (i=0; i<len; i++) {
            switch (s[i]) {
               case '\0': *p++ = '\\'; *p++ = '0'; *p++ = '0'; break;
               case '\r': *p++ = '\\'; *p++ = 'r'; break;
               case '\n': *p++ = '\\'; *p++ = 'n'; break;
               case '\t': *p++ = '\\'; *p++ = 't'; break;
               case '\\': *p++ = '\\'; *p++ = '\\'; break;
               case '\"': *p++ = '\\'; *p++ = '\"'; break;
                  
               default:
                  if (s[i] >= 0 && s[i] < 32) {
                     *p++ = '\\';
                     *p++ = get_hex_char(s[i] >> 4);
                     *p++ = get_hex_char(s[i] & 0xF);
                  }
                  else {
                     *p++ = s[i];
                  }
                  break;
            }
         }
         *p++ = '\"';
         *p++ = '\0';
         free(s);

         ok = string_append(str, "%s", dest);
         free(dest);
         if (!ok) return FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
      else if (fixscript_is_hash(heap, value)) {
         for (i=0; i<stack->len; i++) {
            if (value.value == (intptr_t)stack->data[i]) {
               if (!string_append(str, "(hash reference -%d)", stack->len-i)) return FIXSCRIPT_ERR_OUT_OF_MEMORY;
               return FIXSCRIPT_SUCCESS;
            }
         }

         err = fixscript_get_array_length(heap, value, &len);
         if (err) return err;
         if (len == 0) {
            if (!string_append(str, "{}")) return FIXSCRIPT_ERR_OUT_OF_MEMORY;
            return FIXSCRIPT_SUCCESS;
         }

         if (!string_append(str, newlines? "{\n" : "{ ")) return FIXSCRIPT_ERR_OUT_OF_MEMORY;

         err = dynarray_add(stack, (void *)(intptr_t)value.value);
         if (err != FIXSCRIPT_SUCCESS) return err;

         hash_pos = 0;
         i = 0;

         while (fixscript_iter_hash(heap, value, &key_val, &elem_val, &hash_pos)) {
            if (i++) {
               if (!string_append(str, newlines? ",\n" : ", ")) return FIXSCRIPT_ERR_OUT_OF_MEMORY;
            }
            if (newlines) {
               if (!indent(str, level+1)) return FIXSCRIPT_ERR_OUT_OF_MEMORY;
            }

            err = dump_value(heap, str, stack, key_val, newlines, level+1);
            if (err != FIXSCRIPT_SUCCESS) return err;

            if (!string_append(str, ": ")) return FIXSCRIPT_ERR_OUT_OF_MEMORY;

            err = dump_value(heap, str, stack, elem_val, newlines, level+1);
            if (err != FIXSCRIPT_SUCCESS) return err;
         }

         stack->len--;

         if (newlines) {
            if (!string_append(str, "\n")) return FIXSCRIPT_ERR_OUT_OF_MEMORY;
         }
         if (newlines) {
            if (!indent(str, level)) return FIXSCRIPT_ERR_OUT_OF_MEMORY;
         }
         if (!string_append(str, newlines? "}" : " }")) return FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
      else if (fixscript_is_array(heap, value)) {
         for (i=0; i<stack->len; i++) {
            if (value.value == (intptr_t)stack->data[i]) {
               if (!string_append(str, "(array reference -%d)", stack->len-i)) return FIXSCRIPT_ERR_OUT_OF_MEMORY;
               return FIXSCRIPT_SUCCESS;
            }
         }

         err = fixscript_get_array_length(heap, value, &len);
         if (err) return err;
         if (len == 0) {
            if (!string_append(str, "[]")) return FIXSCRIPT_ERR_OUT_OF_MEMORY;
            return FIXSCRIPT_SUCCESS;
         }
         
         if (!string_append(str, newlines? "[\n" : "[")) return FIXSCRIPT_ERR_OUT_OF_MEMORY;

         err = dynarray_add(stack, (void *)(intptr_t)value.value);
         if (err != FIXSCRIPT_SUCCESS) return err;

         for (i=0; i<len; i++) {
            err = fixscript_get_array_elem(heap, value, i, &elem_val);
            if (err != FIXSCRIPT_SUCCESS) return err;

            if (newlines) {
               if (!indent(str, level+1)) return FIXSCRIPT_ERR_OUT_OF_MEMORY;
            }

            err = dump_value(heap, str, stack, elem_val, newlines, level+1);
            if (err != FIXSCRIPT_SUCCESS) return err;

            if (newlines) {
               if (!string_append(str, (i < len-1)? ",\n" : "\n")) return FIXSCRIPT_ERR_OUT_OF_MEMORY;
            }
            else if (i < len-1) {
               if (!string_append(str, ", ")) return FIXSCRIPT_ERR_OUT_OF_MEMORY;
            }
         }

         stack->len--;

         if (newlines) {
            if (!indent(str, level)) return FIXSCRIPT_ERR_OUT_OF_MEMORY;
         }
         if (!string_append(str, "]")) return FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
      else {
         fixscript_get_handle(heap, value, -1, &type);
         if (type >= 0) {
            Array *arr = &heap->data[value.value];
            s = NULL;
            if (arr->is_handle == 2) {
               s = arr->handle_func(heap, HANDLE_OP_TO_STRING, arr->handle_ptr, NULL);
            }
            if (s) {
               err = !string_append(str, "%s", s);
               free(s);
               if (err) {
                  return FIXSCRIPT_ERR_OUT_OF_MEMORY;
               }
            }
            else {
               if (!string_append(str, "(native handle #%d)", value.value)) return FIXSCRIPT_ERR_OUT_OF_MEMORY;
            }
         }
         else {
            func_id = value.value - FUNC_REF_OFFSET;
            if (func_id > 0 && func_id < heap->functions.len) {
               func = heap->functions.data[func_id];
               script_name = string_hash_find_name(&heap->scripts, func->script);
               func_name = string_hash_find_name(&func->script->functions, func);
               if (script_name && func_name) {
                  if (!string_append(str, "<%s:%s>", script_name, func_name)) return FIXSCRIPT_ERR_OUT_OF_MEMORY;
               }
               else {
                  if (!string_append(str, "(invalid function reference)")) return FIXSCRIPT_ERR_OUT_OF_MEMORY;
               }
            }
            else {
               if (!string_append(str, "(invalid)")) return FIXSCRIPT_ERR_OUT_OF_MEMORY;
            }
         }
      }
   }
   else {
      if (!string_append(str, "%d", value.value)) return FIXSCRIPT_ERR_OUT_OF_MEMORY;
   }
   return FIXSCRIPT_SUCCESS;
}


int fixscript_dump_value(Heap *heap, Value value, int newlines)
{
   char *s;
   int err, len;
   
   err = fixscript_to_string(heap, value, newlines, &s, &len);
   if (err != FIXSCRIPT_SUCCESS) {
      fprintf(stderr, "error while dumping value (%s)\n", fixscript_get_error_msg(err));
      fflush(stderr);
      return err;
   }
   string_filter_control_chars(s, len);
   fprintf(stderr, "%.*s\n", len, s);
   fflush(stderr);
   free(s);
   return FIXSCRIPT_SUCCESS;
}


int fixscript_to_string(Heap *heap, Value value, int newlines, char **str_out, int *len_out)
{
   String str;
   DynArray stack;
   int err;

   memset(&str, 0, sizeof(String));
   memset(&stack, 0, sizeof(DynArray));

   err = dump_value(heap, &str, &stack, value, newlines, 0);
   if (err != FIXSCRIPT_SUCCESS) goto error;

   *str_out = str.data;
   if (len_out) *len_out = str.len;
   free(stack.data);
   return FIXSCRIPT_SUCCESS;

error:
   free(str.data);
   free(stack.data);
   *str_out = NULL;
   if (len_out) *len_out = 0;
   return err;
}


static inline int byte_array_append(Heap *heap, Array *buf, int *off, int count)
{
   int64_t new_len;
   int err;

   new_len = ((int64_t)*off) + (int64_t)count;
   if (count < 0) {
      return FIXSCRIPT_ERR_OUT_OF_MEMORY;
   }
   if (new_len > INT_MAX) {
      return FIXSCRIPT_ERR_OUT_OF_MEMORY;
   }
   if (new_len > buf->size) {
      err = expand_array(heap, buf, ((int)new_len)-1);
      if (err != FIXSCRIPT_SUCCESS) return err;
   }
   flags_clear_range(buf, *off, count);
   buf->len = (int)new_len;
   return FIXSCRIPT_SUCCESS;
}


static int clone_value(Heap *dest, Heap *src, Value value, Value map, Value *clone, LoadScriptFunc load_func, void *load_data, Value *error, DynArray *queue, int recursion_limit)
{
   SharedArrayHandle *sah;
   WeakRefHandle *wrh;
   int buf_size = 1024;
   Value *values;
   Value arr_val, hash_val, entry_key, entry_value, ref_value, handle_val;
   Array *arr, *new_arr;
   Function *func;
   FuncRefHandle *frh;
   Script *script;
   const char *script_name, *func_name;
   void *new_ptr;
   char *s, *p;
   char buf[128];
   int i, err, len, num, off, count, pos, type, func_id, elem_size;

   if (fixscript_is_int(value) || fixscript_is_float(value)) {
      *clone = value;
      return FIXSCRIPT_SUCCESS;
   }

   if (map.value) {
      err = fixscript_get_hash_elem(dest, map, fixscript_int(value.value), &ref_value);
      if (err == FIXSCRIPT_SUCCESS) {
         *clone = ref_value;
         return FIXSCRIPT_SUCCESS;
      }
      if (err != FIXSCRIPT_ERR_KEY_NOT_FOUND) {
         return err;
      }
   }

   if (fixscript_is_array(src, value)) {
      err = fixscript_get_array_length(src, value, &len);
      if (err) return err;

      arr = &src->data[value.value];
      if (arr->is_const) {
         if (dest == src) {
            *clone = value;
            return FIXSCRIPT_SUCCESS;
         }

         err = fixscript_get_const_string_between(dest, src, value, 0, -1, &arr_val);
         if (!err && map.value) {
            err = fixscript_set_hash_elem(dest, map, fixscript_int(value.value), arr_val);
         }
         if (err) {
            return err;
         }

         add_root(dest, arr_val);
         *clone = arr_val;
         return FIXSCRIPT_SUCCESS;
      }

      if (arr->is_shared) {
         if (dest != src) {
            sah = ARRAY_SHARED_HEADER(arr);
            elem_size = (arr->type == ARR_BYTE? 1 : arr->type == ARR_SHORT? 2 : 4);

            snprintf(buf, sizeof(buf), "%d,%p,%d,%d,%p", sah->type, arr->data, arr->len, elem_size, sah->free_data);
            arr_val.value = (intptr_t)string_hash_get(&dest->shared_arrays, buf);
            if (arr_val.value) {
               arr_val.is_array = 1;
               add_root(dest, arr_val);
               *clone = arr_val;
               return FIXSCRIPT_SUCCESS;
            }

            arr_val = create_array(dest, arr->type, 0);
            if (!arr_val.value) {
               return FIXSCRIPT_ERR_OUT_OF_MEMORY;
            }

            new_arr = &dest->data[arr_val.value];
            new_arr->len = arr->len;
            new_arr->size = arr->size;
            new_arr->data = arr->data;
            new_arr->flags = arr->flags;
            set_shared_array(dest, arr_val.value);
            if (sah->refcnt < SAH_REFCNT_LIMIT) {
               __sync_add_and_fetch(&sah->refcnt, 1);
            }
   
            string_hash_set(&dest->shared_arrays, strdup(buf), (void *)(intptr_t)arr_val.value);

            dest->total_size += (int64_t)FLAGS_SIZE(arr->len) * sizeof(int) + (int64_t)arr->len * elem_size;

            add_root(dest, arr_val);
            *clone = arr_val;
            return FIXSCRIPT_SUCCESS;
         }
         *clone = value;
         return FIXSCRIPT_SUCCESS;
      }

      if (fixscript_is_string(src, value)) {
         arr_val = fixscript_create_string(dest, NULL, 0);
      }
      else {
         arr_val = fixscript_create_array(dest, 0);
      }
      if (!arr_val.value) return FIXSCRIPT_ERR_OUT_OF_MEMORY;

      err = fixscript_set_array_length(dest, arr_val, len);
      if (err) return err;

      if (map.value) {
         err = fixscript_set_hash_elem(dest, map, fixscript_int(value.value), arr_val);
         if (err != FIXSCRIPT_SUCCESS) return err;

         if (recursion_limit <= 0) {
            err = dynarray_add(queue, (void *)(intptr_t)arr_val.value);
            if (err) return err;
            err = dynarray_add(queue, (void *)(intptr_t)value.value);
            if (err) return err;
            *clone = arr_val;
            return FIXSCRIPT_SUCCESS;
         }
      }

      off = 0;
      count = len;

      values = malloc_array(MIN(count, buf_size), sizeof(Value));
      if (!values) {
         return FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }

      err = FIXSCRIPT_SUCCESS;

      while (count > 0) {
         num = MIN(count, buf_size);

         err = fixscript_get_array_range(src, value, off, num, values);
         if (err) break;

         if (map.value) {
            for (i=0; i<num; i++) {
               err = clone_value(dest, src, values[i], map, &values[i], load_func, load_data, error, queue, recursion_limit-1);
               if (err) break;
            }
            if (err) break;
         }

         err = fixscript_set_array_range(dest, arr_val, off, num, values);
         if (err) break;

         off += num;
         count -= num;
      }

      free(values);
      if (err) return err;

      *clone = arr_val;
      return FIXSCRIPT_SUCCESS;
   }

   if (fixscript_is_hash(src, value)) {
      hash_val = fixscript_create_hash(dest);
      if (!hash_val.value) return FIXSCRIPT_ERR_OUT_OF_MEMORY;

      if (map.value) {
         err = fixscript_set_hash_elem(dest, map, fixscript_int(value.value), hash_val);
         if (err != FIXSCRIPT_SUCCESS) return err;

         if (recursion_limit <= 0) {
            err = dynarray_add(queue, (void *)(intptr_t)hash_val.value);
            if (err) return err;
            err = dynarray_add(queue, (void *)(intptr_t)value.value);
            if (err) return err;
            *clone = hash_val;
            return FIXSCRIPT_SUCCESS;
         }
      }

      pos = 0;
      while (fixscript_iter_hash(src, value, &entry_key, &entry_value, &pos)) {
         if (map.value) {
            err = clone_value(dest, src, entry_key, map, &entry_key, load_func, load_data, error, queue, recursion_limit-1);
            if (err) return err;

            err = clone_value(dest, src, entry_value, map, &entry_value, load_func, load_data, error, queue, recursion_limit-1);
            if (err) return err;
         }

         err = fixscript_set_hash_elem(dest, hash_val, entry_key, entry_value);
         if (err) return err;
      }

      *clone = hash_val;
      return FIXSCRIPT_SUCCESS;
   }
   
   fixscript_get_handle(src, value, -1, &type);
   if (type >= 0) {
      arr = &src->data[value.value];
      
      if (type == FUNC_REF_HANDLE_TYPE && load_func) {
         frh = arr->handle_ptr;
         script = fixscript_get(dest, frh->script_name);
         if (!script) {
            s = strdup(frh->script_name);
            if (!s) return FIXSCRIPT_ERR_OUT_OF_MEMORY;
            p = strrchr(s, '.');
            if (p) *p = 0;
            script = load_func(dest, s, error, load_data);
            free(s);
         }
         if (!script) {
            return FIXSCRIPT_ERR_FUNC_REF_LOAD_ERROR;
         }
         *clone = fixscript_get_function(dest, script, frh->func_name);
         if (!clone->value) {
            return FIXSCRIPT_ERR_UNSERIALIZABLE_REF;
         }
         if (map.value) {
            err = fixscript_set_hash_elem(dest, map, fixscript_int(value.value), *clone);
            if (err != FIXSCRIPT_SUCCESS) return err;
         }
         return FIXSCRIPT_SUCCESS;
      }
      else if (type == WEAK_REF_HANDLE_TYPE) {
         wrh = arr->handle_ptr;
         if (wrh->target) {
            entry_value = (Value) { wrh->target, 1 };
            hash_val = (Value) { wrh->container, 1 };
            entry_key = wrh->key;

            if (map.value) {
               err = clone_value(dest, src, entry_value, map, &entry_value, load_func, load_data, error, queue, recursion_limit-1);
               if (err) return err;

               if (hash_val.value) {
                  err = clone_value(dest, src, hash_val, map, &hash_val, load_func, load_data, error, queue, recursion_limit-1);
                  if (err) return err;
               }

               if (entry_key.is_array != 2) {
                  err = clone_value(dest, src, entry_key, map, &entry_key, load_func, load_data, error, queue, recursion_limit-1);
                  if (err) return err;
               }
            }

            err = fixscript_create_weak_ref(dest, entry_value, hash_val.value? &hash_val : NULL, entry_key.is_array != 2? &entry_key : NULL, &handle_val);
            if (err) return err;
         }
         else {
            wrh = calloc(1, sizeof(WeakRefHandle));
            wrh->id = dest->weak_id_cnt++;
      
            handle_val = fixscript_create_value_handle(dest, WEAK_REF_HANDLE_TYPE, wrh, weak_ref_handle_func);
            if (!handle_val.value) {
               return FIXSCRIPT_ERR_OUT_OF_MEMORY;
            }

            wrh->value = handle_val.value;
            wrh->key.is_array = 2;
         }

         if (map.value) {
            err = fixscript_set_hash_elem(dest, map, fixscript_int(value.value), handle_val);
            if (err) return err;
         }

         *clone = handle_val;
         return FIXSCRIPT_SUCCESS;
      }
      
      if (arr->is_handle == 2) {
         new_ptr = arr->handle_func(src, HANDLE_OP_COPY, arr->handle_ptr, dest);
         if (!new_ptr) {
            return FIXSCRIPT_ERR_UNSERIALIZABLE_REF;
         }
         handle_val = fixscript_create_value_handle(dest, type, new_ptr, arr->handle_func);
         if (!handle_val.value) {
            return FIXSCRIPT_ERR_OUT_OF_MEMORY;
         }

         if (map.value) {
            err = fixscript_set_hash_elem(dest, map, fixscript_int(value.value), handle_val);
            if (err != FIXSCRIPT_SUCCESS) return err;

            if (recursion_limit <= 0) {
               err = dynarray_add(queue, (void *)(intptr_t)handle_val.value);
               if (err) return err;
               err = dynarray_add(queue, (void *)(intptr_t)value.value);
               if (err) return err;
               *clone = handle_val;
               return FIXSCRIPT_SUCCESS;
            }
         }

         if (map.value) {
            CopyContext cc;
            cc.dest = dest;
            cc.src = src;
            cc.map = map;
            cc.err = 0;
            cc.load_func = load_func;
            cc.load_data = load_data;
            cc.error = error;
            cc.queue = queue;
            cc.recursion_limit = recursion_limit-1;
            dest->data[handle_val.value].handle_func(dest, HANDLE_OP_COPY_REFS, new_ptr, &cc);
            if (cc.err) {
               return cc.err;
            }
         }
      
         *clone = handle_val;
         return FIXSCRIPT_SUCCESS;
      }
      
      return FIXSCRIPT_ERR_UNSERIALIZABLE_REF;
   }

   func_id = value.value - FUNC_REF_OFFSET;
   if (func_id > 0 && func_id < src->functions.len) {
      if (dest != src) {
         func = src->functions.data[func_id];
         script_name = string_hash_find_name(&src->scripts, func->script);
         func_name = string_hash_find_name(&func->script->functions, func);
   
         if (!script_name || !func_name) {
            return FIXSCRIPT_ERR_UNSERIALIZABLE_REF;
         }

         if (load_func) {
            script = fixscript_get(dest, script_name);
            if (!script) {
               s = strdup(script_name);
               if (!s) return FIXSCRIPT_ERR_OUT_OF_MEMORY;
               p = strrchr(s, '.');
               if (p) *p = 0;
               script = load_func(dest, s, error, load_data);
               free(s);
            }
            if (!script) {
               return FIXSCRIPT_ERR_FUNC_REF_LOAD_ERROR;
            }
            *clone = fixscript_get_function(dest, script, func_name);
            if (!clone->value) {
               return FIXSCRIPT_ERR_UNSERIALIZABLE_REF;
            }
         }
         else {
            frh = calloc(1, sizeof(FuncRefHandle));
            if (!frh) {
               return FIXSCRIPT_ERR_OUT_OF_MEMORY;
            }
            frh->script_name = strdup(script_name);
            frh->func_name = strdup(func_name);
            if (!frh->script_name || !frh->func_name) {
               func_ref_handle_func(dest, HANDLE_OP_FREE, frh, NULL);
               return FIXSCRIPT_ERR_OUT_OF_MEMORY;
            }
            *clone = fixscript_create_value_handle(dest, FUNC_REF_HANDLE_TYPE, frh, func_ref_handle_func);
            if (!clone->value) {
               return FIXSCRIPT_ERR_OUT_OF_MEMORY;
            }
         }
         
         if (map.value) {
            err = fixscript_set_hash_elem(dest, map, fixscript_int(value.value), *clone);
            if (err != FIXSCRIPT_SUCCESS) return err;
         }
         return FIXSCRIPT_SUCCESS;
      }
      else {
         *clone = value;
         return FIXSCRIPT_SUCCESS;
      }
   }

   return FIXSCRIPT_ERR_UNSERIALIZABLE_REF;
}


int fixscript_compare(Heap *heap, Value value1, Value value2)
{
   return compare_values(heap, value1, heap, value2, MAX_COMPARE_RECURSION);
}


int fixscript_compare_between(Heap *heap1, Value value1, Heap *heap2, Value value2)
{
   return compare_values(heap1, value1, heap2, value2, MAX_COMPARE_RECURSION);
}


int fixscript_clone(Heap *heap, Value value, int deep, Value *clone)
{
   if (deep) {
      return fixscript_clone_between(heap, heap, value, clone, NULL, NULL, NULL);
   }
   return clone_value(heap, heap, value, fixscript_int(0), clone, NULL, NULL, NULL, NULL, 1);
}


int fixscript_clone_between(Heap *dest, Heap *src, Value value, Value *clone, LoadScriptFunc load_func, void *load_data, Value *error)
{
   int buf_size = 1024;
   DynArray queue;
   Value map, dest_val, src_val, entry_key, entry_value, *values;
   void *new_ptr;
   int i, err, off, count, num, type;

   if (error) {
      *error = fixscript_int(0);
   }
   memset(&queue, 0, sizeof(DynArray));
   map = fixscript_create_hash(dest);
   if (!map.value) {
      return FIXSCRIPT_ERR_OUT_OF_MEMORY;
   }
   fixscript_ref(dest, map);

   err = clone_value(dest, src, value, map, clone, load_func, load_data, error, &queue, CLONE_RECURSION_CUTOFF);
   if (err) goto error;

   while (queue.len > 0) {
      src_val = (Value) { (intptr_t)queue.data[--queue.len], 1 };
      dest_val = (Value) { (intptr_t)queue.data[--queue.len], 1 };
      if (fixscript_is_array(src, src_val)) {
         off = 0;
         fixscript_get_array_length(src, src_val, &count);

         values = malloc_array(MIN(count, buf_size), sizeof(Value));
         if (!values) {
            err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
            goto error;
         }

         while (count > 0) {
            num = MIN(count, buf_size);

            err = fixscript_get_array_range(src, src_val, off, num, values);
            if (err) break;

            for (i=0; i<num; i++) {
               err = clone_value(dest, src, values[i], map, &values[i], load_func, load_data, error, &queue, CLONE_RECURSION_CUTOFF);
               if (err) break;
            }
            if (err) break;

            err = fixscript_set_array_range(dest, dest_val, off, num, values);
            if (err) break;

            off += num;
            count -= num;
         }

         free(values);
         if (err) goto error;
      }
      else if (fixscript_is_hash(src, src_val)) {
         i = 0;
         while (fixscript_iter_hash(src, src_val, &entry_key, &entry_value, &i)) {
            err = clone_value(dest, src, entry_key, map, &entry_key, load_func, load_data, error, &queue, CLONE_RECURSION_CUTOFF);
            if (err) goto error;

            err = clone_value(dest, src, entry_value, map, &entry_value, load_func, load_data, error, &queue, CLONE_RECURSION_CUTOFF);
            if (err) goto error;

            err = fixscript_set_hash_elem(dest, dest_val, entry_key, entry_value);
            if (err) goto error;
         }
      }
      else {
         new_ptr = fixscript_get_handle(dest, dest_val, -1, &type);
         if (type >= 0) {
            CopyContext cc;
            cc.dest = dest;
            cc.src = src;
            cc.map = map;
            cc.err = 0;
            cc.load_func = load_func;
            cc.load_data = load_data;
            cc.error = error;
            cc.queue = &queue;
            cc.recursion_limit = CLONE_RECURSION_CUTOFF;
            dest->data[dest_val.value].handle_func(dest, HANDLE_OP_COPY_REFS, new_ptr, &cc);
            if (cc.err) {
               err = cc.err;
               goto error;
            }
         }
      }
   }

error:
   fixscript_unref(dest, map);
   reclaim_array(dest, map.value, NULL);
   free(queue.data);
   return err;
}


static inline void serialize_byte(Array *buf, int *off, uint8_t value)
{
   buf->byte_data[(*off)++] = value;
}


static inline void serialize_short(Array *buf, int *off, uint16_t value)
{
   buf->byte_data[(*off)++] = (value) & 0xFF;
   buf->byte_data[(*off)++] = (value >> 8) & 0xFF;
}


static inline void serialize_int(Array *buf, int *off, uint32_t value)
{
   buf->byte_data[(*off)++] = (value) & 0xFF;
   buf->byte_data[(*off)++] = (value >> 8) & 0xFF;
   buf->byte_data[(*off)++] = (value >> 16) & 0xFF;
   buf->byte_data[(*off)++] = (value >> 24);
}


static int serialize_value(Heap *heap, Array *buf, int *off, Value map, Value value)
{
   DynArray stack;
   Value hash_key, hash_value, ref_value, cur_hash = fixscript_int(0);
   Array *arr, *cur_array = NULL;
   int i, len, val, err=0, little_endian_test, max_val, type;
   int cur_idx=0, cur_is_hash=0;
   int64_t sum;

   memset(&stack, 0, sizeof(DynArray));

   for (;;) {
      if (cur_array) {
         if (cur_idx >= cur_array->len) {
            goto pop_stack;
         }
         value = (Value) { get_array_value(cur_array, cur_idx), IS_ARRAY(cur_array, cur_idx) != 0 };
         cur_idx++;
      }
      else if (cur_hash.value) {
         i = cur_idx >> 1;
         if (!fixscript_iter_hash(heap, cur_hash, &hash_key, &hash_value, &i)) {
            goto pop_stack;
         }
         if (cur_idx & 1) {
            value = hash_value;
            cur_idx = i << 1;
         }
         else {
            value = hash_key;
            cur_idx++;
         }
      }

      if (fixscript_is_int(value)) {
         val = value.value;
         if (val == 0) {
            err = byte_array_append(heap, buf, off, 1);
            if (err) goto error;
            serialize_byte(buf, off, SER_ZERO);
         }
         else if (((unsigned int)val) <= 0xFF) {
            err = byte_array_append(heap, buf, off, 2);
            if (err) goto error;
            serialize_byte(buf, off, SER_BYTE);
            serialize_byte(buf, off, val);
         }
         else if (((unsigned int)val) <= 0xFFFF) {
            err = byte_array_append(heap, buf, off, 3);
            if (err) goto error;
            serialize_byte(buf, off, SER_SHORT);
            serialize_short(buf, off, val);
         }
         else {
            err = byte_array_append(heap, buf, off, 5);
            if (err) goto error;
            serialize_byte(buf, off, SER_INT);
            serialize_int(buf, off, val);
         }
         goto next_value;
      }

      if (fixscript_is_float(value)) {
         val = value.value;
         if (val == 0) {
            err = byte_array_append(heap, buf, off, 1);
            if (err) goto error;
            serialize_byte(buf, off, SER_FLOAT_ZERO);
         }
         else {
            // normalize NaNs:
            if (((val >> 23) & 0xFF) == 0xFF && (val & ((1<<23)-1))) {
               val = (val & ~((1<<23)-1)) | (1 << 22);
            }
            err = byte_array_append(heap, buf, off, 5);
            if (err) goto error;
            serialize_byte(buf, off, SER_FLOAT);
            serialize_int(buf, off, val);
         }
         goto next_value;
      }

      err = fixscript_get_hash_elem(heap, map, fixscript_int(value.value), &ref_value);
      if (!err) {
         if (ref_value.value <= 0xFFFF) {
            err = byte_array_append(heap, buf, off, 3);
            if (err) goto error;
            serialize_byte(buf, off, SER_REF_SHORT);
            serialize_short(buf, off, ref_value.value);
         }
         else {
            err = byte_array_append(heap, buf, off, 5);
            if (err) goto error;
            serialize_byte(buf, off, SER_REF);
            serialize_int(buf, off, ref_value.value);
         }
         goto next_value;
      }
      if (err != FIXSCRIPT_ERR_KEY_NOT_FOUND) {
         goto error;
      }

      err = fixscript_get_array_length(heap, map, &len);
      if (err) goto error;

      err = fixscript_set_hash_elem(heap, map, fixscript_int(value.value), fixscript_int(len));
      if (err) goto error;

      if (fixscript_is_hash(heap, value)) {
         err = fixscript_get_array_length(heap, value, &len);
         if (err) goto error;

         err = byte_array_append(heap, buf, off, len <= 12? 1 : len <= 0xFF? 2 : len <= 0xFFFF? 3 : 5);
         if (err) goto error;

         if (len <= 12) {
            serialize_byte(buf, off, SER_HASH | (len << 4));
         }
         else if (len <= 0xFF) {
            serialize_byte(buf, off, SER_HASH | 0xD0);
            serialize_byte(buf, off, len);
         }
         else if (len <= 0xFFFF) {
            serialize_byte(buf, off, SER_HASH | 0xE0);
            serialize_short(buf, off, len);
         }
         else {
            serialize_byte(buf, off, SER_HASH | 0xF0);
            serialize_int(buf, off, len);
         }

         err = fixscript_get_array_length(heap, value, &len);
         if (err) goto error;

         if (cur_array || cur_hash.value) {
            err = dynarray_add(&stack, cur_is_hash? (void *)(intptr_t)cur_hash.value : cur_array);
            if (err) goto error;
            err = dynarray_add(&stack, (void *)(intptr_t)(cur_idx | (cur_is_hash << 31)));
            if (err) goto error;
         }
         cur_is_hash = 1;
         cur_array = NULL;
         cur_hash = value;
         cur_idx = 0;
         continue;
      }

      if (fixscript_is_array(heap, value)) {
         err = fixscript_get_array_length(heap, value, &len);
         if (err) goto error;

         arr = &heap->data[value.value];

         err = byte_array_append(heap, buf, off, len <= 12? 1 : len <= 0xFF? 2 : len <= 0xFFFF? 3 : 5);
         if (err) goto error;

         if (arr->type == ARR_BYTE && flags_is_array_clear_in_range(arr, 0, len)) {
            type = fixscript_is_string(heap, value)? SER_STRING_BYTE : SER_ARRAY_BYTE;
            if (len <= 12) {
               serialize_byte(buf, off, type | (len << 4));
            }
            else if (len <= 0xFF) {
               serialize_byte(buf, off, type | 0xD0);
               serialize_byte(buf, off, len);
            }
            else if (len <= 0xFFFF) {
               serialize_byte(buf, off, type | 0xE0);
               serialize_short(buf, off, len);
            }
            else {
               serialize_byte(buf, off, type | 0xF0);
               serialize_int(buf, off, len);
            }

            err = byte_array_append(heap, buf, off, len);
            if (err) goto error;

            memcpy(buf->byte_data + *off, arr->byte_data, len);
            (*off) += len;
            goto next_value;
         }

         if (arr->type == ARR_SHORT && flags_is_array_clear_in_range(arr, 0, len)) {
            max_val = 0;
            for (i=0; i<len; i++) {
               max_val |= arr->short_data[i];
            }

            if (max_val & ~0xFF) {
               type = fixscript_is_string(heap, value)? SER_STRING_SHORT : SER_ARRAY_SHORT;
               sum = ((int64_t)len) << 1;
            }
            else {
               type = fixscript_is_string(heap, value)? SER_STRING_BYTE : SER_ARRAY_BYTE;
               sum = len;
            }

            if (len <= 12) {
               serialize_byte(buf, off, type | (len << 4));
            }
            else if (len <= 0xFF) {
               serialize_byte(buf, off, type | 0xD0);
               serialize_byte(buf, off, len);
            }
            else if (len <= 0xFFFF) {
               serialize_byte(buf, off, type | 0xE0);
               serialize_short(buf, off, len);
            }
            else {
               serialize_byte(buf, off, type | 0xF0);
               serialize_int(buf, off, len);
            }

            if (sum > INT_MAX) {
               err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
               goto error;
            }

            err = byte_array_append(heap, buf, off, (int)sum);
            if (err) goto error;

            if (max_val & ~0xFF) {
               little_endian_test = 1;
               if (*(char *)&little_endian_test == 1) {
                  memcpy(buf->byte_data + *off, arr->short_data, (int)sum);
                  (*off) += (int)sum;
               }
               else {
                  for (i=0; i<len; i++) {
                     serialize_short(buf, off, arr->short_data[i]);
                  }
               }
            }
            else {
               for (i=0; i<len; i++) {
                  serialize_byte(buf, off, (uint8_t)arr->short_data[i]);
               }
            }
            goto next_value;
         }

         if (flags_is_array_clear_in_range(arr, 0, len)) {
            max_val = 0;
            for (i=0; i<len; i++) {
               max_val |= arr->data[i];
            }

            if (max_val & ~0xFFFF) {
               type = fixscript_is_string(heap, value)? SER_STRING_INT : SER_ARRAY_INT;
               sum = ((int64_t)len) << 2;
            }
            else if (max_val & ~0xFF) {
               type = fixscript_is_string(heap, value)? SER_STRING_SHORT : SER_ARRAY_SHORT;
               sum = ((int64_t)len) << 1;
            }
            else {
               type = fixscript_is_string(heap, value)? SER_STRING_BYTE : SER_ARRAY_BYTE;
               sum = len;
            }

            if (len <= 12) {
               serialize_byte(buf, off, type | (len << 4));
            }
            else if (len <= 0xFF) {
               serialize_byte(buf, off, type | 0xD0);
               serialize_byte(buf, off, len);
            }
            else if (len <= 0xFFFF) {
               serialize_byte(buf, off, type | 0xE0);
               serialize_short(buf, off, len);
            }
            else {
               serialize_byte(buf, off, type | 0xF0);
               serialize_int(buf, off, len);
            }

            if (sum > INT_MAX) {
               err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
               goto error;
            }

            err = byte_array_append(heap, buf, off, (int)sum);
            if (err) goto error;

            if (max_val & ~0xFFFF) {
               little_endian_test = 1;
               if (*(char *)&little_endian_test == 1) {
                  memcpy(buf->byte_data + *off, arr->data, (int)sum);
                  (*off) += (int)sum;
               }
               else {
                  for (i=0; i<len; i++) {
                     serialize_int(buf, off, arr->data[i]);
                  }
               }
            }
            else if (max_val & ~0xFF) {
               for (i=0; i<len; i++) {
                  serialize_short(buf, off, arr->data[i]);
               }
            }
            else {
               for (i=0; i<len; i++) {
                  serialize_byte(buf, off, arr->data[i]);
               }
            }
            goto next_value;
         }

         if (len <= 12) {
            serialize_byte(buf, off, SER_ARRAY | (len << 4));
         }
         else if (len <= 0xFF) {
            serialize_byte(buf, off, SER_ARRAY | 0xD0);
            serialize_byte(buf, off, len);
         }
         else if (len <= 0xFFFF) {
            serialize_byte(buf, off, SER_ARRAY | 0xE0);
            serialize_short(buf, off, len);
         }
         else {
            serialize_byte(buf, off, SER_ARRAY | 0xF0);
            serialize_int(buf, off, len);
         }

         if (fixscript_is_string(heap, value)) {
            err = FIXSCRIPT_ERR_UNSERIALIZABLE_REF;
            goto error;
         }

         if (cur_array || cur_hash.value) {
            err = dynarray_add(&stack, cur_is_hash? (void *)(intptr_t)cur_hash.value : cur_array);
            if (err) goto error;
            err = dynarray_add(&stack, (void *)(intptr_t)(cur_idx | (cur_is_hash << 31)));
            if (err) goto error;
         }
         cur_is_hash = 0;
         cur_array = arr;
         cur_hash = fixscript_int(0);
         cur_idx = 0;
         continue;
      }

      err = FIXSCRIPT_ERR_UNSERIALIZABLE_REF;
      goto error;

pop_stack:
      if (stack.len == 0) break;
      cur_idx = (intptr_t)stack.data[--stack.len];
      cur_is_hash = cur_idx >> 31;
      cur_idx &= 0x7FFFFFFF;
      if (cur_is_hash) {
         cur_array = NULL;
         cur_hash = (Value) { (intptr_t)stack.data[--stack.len], 1 };
      }
      else {
         cur_array = stack.data[--stack.len];
         cur_hash = fixscript_int(0);
      }
      continue;

next_value:
      if (stack.len == 0 && !cur_array && !cur_hash.value) break;
      continue;
   }

error:
   free(stack.data);
   return err;
}


int fixscript_serialize(Heap *heap, Value *buf_val, Value value)
{
   Array *buf;
   Value map;
   int off, orig_len, err;
   
   if (!buf_val->value) {
      *buf_val = fixscript_create_array(heap, 0);
      if (!buf_val->value) {
         return FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
   }

   map = fixscript_create_hash(heap);
   if (!map.value) {
      return FIXSCRIPT_ERR_OUT_OF_MEMORY;
   }

   if (!buf_val->is_array || buf_val->value <= 0 || buf_val->value >= heap->size) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   buf = &heap->data[buf_val->value];
   if (buf->len == -1 || buf->hash_slots >= 0) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }
   if (buf->type != ARR_BYTE) {
      return FIXSCRIPT_ERR_INVALID_BYTE_ARRAY;
   }

   off = buf->len;
   orig_len = buf->len;
   err = serialize_value(heap, buf, &off, map, value);
   if (err != FIXSCRIPT_SUCCESS) {
      buf->len = orig_len;
   }
   reclaim_array(heap, map.value, NULL);
   return err;
}


static inline int unserialize_byte(const unsigned char **buf, int *remaining, int *value)
{
   if (*remaining < 1) {
      return FIXSCRIPT_ERR_BAD_FORMAT;
   }
   *value = (unsigned int)(*buf)[0];
   (*buf)++;
   (*remaining)--;
   return FIXSCRIPT_SUCCESS;
}


static inline int unserialize_short(const unsigned char **buf, int *remaining, int *value)
{
   if (*remaining < 2) {
      return FIXSCRIPT_ERR_BAD_FORMAT;
   }
   *value = (unsigned int)((*buf)[0] | ((*buf)[1] << 8));
   (*buf) += 2;
   (*remaining) -= 2;
   return FIXSCRIPT_SUCCESS;
}


static inline int unserialize_int(const unsigned char **buf, int *remaining, int *value)
{
   if (*remaining < 4) {
      return FIXSCRIPT_ERR_BAD_FORMAT;
   }
   *value = (*buf)[0] | ((*buf)[1] << 8) | ((*buf)[2] << 16) | ((*buf)[3] << 24);
   (*buf) += 4;
   (*remaining) -= 4;
   return FIXSCRIPT_SUCCESS;
}


static int unserialize_value(Heap *heap, const unsigned char **buf, int *remaining, Value list, Value *value)
{
   DynArray stack;
   Array *arr;
   Value array, hash, cur_value = fixscript_int(0);
   int i, err=0, type, flt, ref=0, len, int_val=0, little_endian_test, max_val, key_was_present;
   int cur_idx=0, idx;
   int64_t sum;

   memset(&stack, 0, sizeof(DynArray));

   for (;;) {
      if (cur_value.value) {
         arr = &heap->data[cur_value.value];
         if (arr->hash_slots >= 0) {
            if (cur_idx & 1) {
               idx = bitarray_get(&arr->flags[FLAGS_SIZE((1<<arr->size)*2)], arr->size-1, arr->len-1) << 1;
               arr->data[idx+1] = value->value;
               ASSIGN_IS_ARRAY(arr, idx+1, value->is_array);
            }
            else {
               key_was_present = 0;
               err = set_hash_elem(heap, cur_value, *value, fixscript_int(0), &key_was_present);
               if (err) goto error;
               if (key_was_present) {
                  err = FIXSCRIPT_ERR_BAD_FORMAT;
                  goto error;
               }
            }

            if (--cur_idx <= 0) {
               goto pop_stack;
            }
         }
         else {
            err = fixscript_set_array_elem(heap, cur_value, cur_idx++, *value);
            if (err) goto error;

            if (cur_idx >= arr->len) {
               if (flags_is_array_clear_in_range(arr, 0, arr->len)) {
                  err = FIXSCRIPT_ERR_BAD_FORMAT;
                  goto error;
               }
               goto pop_stack;
            }
         }
      }

fetch_value:
      err = unserialize_byte(buf, remaining, &type);
      if (err) goto error;

      if ((type & 0x0F) < SER_ARRAY && (type & 0xF0) != 0) {
         err = FIXSCRIPT_ERR_BAD_FORMAT;
         goto error;
      }

      switch (type & 0x0F) {
         case SER_ZERO: {
            value->value = 0;
            value->is_array = 0;
            goto got_value;
         }

         case SER_BYTE: {
            err = unserialize_byte(buf, remaining, &int_val);
            if (int_val == 0 && !err) err = FIXSCRIPT_ERR_BAD_FORMAT;
            if (err) goto error;

            value->value = int_val;
            value->is_array = 0;
            goto got_value;
         }

         case SER_SHORT: {
            err = unserialize_short(buf, remaining, &int_val);
            if (int_val <= 0xFF && !err) err = FIXSCRIPT_ERR_BAD_FORMAT;
            if (err) goto error;

            value->value = int_val;
            value->is_array = 0;
            goto got_value;
         }

         case SER_INT: {
            err = unserialize_int(buf, remaining, &int_val);
            if ((int_val & ~0xFFFF) == 0 && !err) err = FIXSCRIPT_ERR_BAD_FORMAT;
            if (err) goto error;

            value->value = int_val;
            value->is_array = 0;
            goto got_value;
         }

         case SER_FLOAT: {
            err = unserialize_int(buf, remaining, &int_val);
            if (int_val == 0 && !err) err = FIXSCRIPT_ERR_BAD_FORMAT;
            if (err) goto error;

            flt = int_val & ~(1<<31);
            if (flt > 0 && flt < (1<<23)) {
               err = FIXSCRIPT_ERR_BAD_FORMAT;
               goto error;
            }
            if ((flt >> 23) == 0xFF) {
               flt &= (1<<23)-1;
               if (flt != 0 && flt != (1<<22)) {
                  err = FIXSCRIPT_ERR_BAD_FORMAT;
                  goto error;
               }
            }
            value->value = int_val;
            value->is_array = 1;
            goto got_value;
         }

         case SER_FLOAT_ZERO: {
            value->value = 0;
            value->is_array = 1;
            goto got_value;
         }

         case SER_REF: {
            err = unserialize_int(buf, remaining, &ref);
            if (((unsigned int)ref) <= 0xFFFF && !err) err = FIXSCRIPT_ERR_BAD_FORMAT;
            if (err) goto error;

            err = fixscript_get_array_elem(heap, list, ref, value);
            if (err) {
               if (err == FIXSCRIPT_ERR_OUT_OF_BOUNDS) {
                  err = FIXSCRIPT_ERR_BAD_FORMAT;
               }
               goto error;
            }

            if (!value->value) {
               err = FIXSCRIPT_ERR_BAD_FORMAT;
               goto error;
            }
            goto got_value;
         }

         case SER_REF_SHORT: {
            err = unserialize_short(buf, remaining, &ref);
            if (err) goto error;

            err = fixscript_get_array_elem(heap, list, ref, value);
            if (err) {
               if (err == FIXSCRIPT_ERR_OUT_OF_BOUNDS) {
                  err = FIXSCRIPT_ERR_BAD_FORMAT;
               }
               goto error;
            }

            if (!value->value) {
               err = FIXSCRIPT_ERR_BAD_FORMAT;
               goto error;
            }
            goto got_value;
         }

         case SER_ARRAY:
         case SER_ARRAY_BYTE:
         case SER_ARRAY_SHORT:
         case SER_ARRAY_INT:
         case SER_STRING_BYTE:
         case SER_STRING_SHORT:
         case SER_STRING_INT: {
            len = type >> 4;
            type &= 0x0F;
            if (len == 0x0D) {
               err = unserialize_byte(buf, remaining, &len);
               if (len <= 12 && !err) err = FIXSCRIPT_ERR_BAD_FORMAT;
               if (err) goto error;
            }
            else if (len == 0x0E) {
               err = unserialize_short(buf, remaining, &len);
               if (len <= 0xFF && !err) err = FIXSCRIPT_ERR_BAD_FORMAT;
               if (err) goto error;
            }
            else if (len == 0x0F) {
               err = unserialize_int(buf, remaining, &len);
               if (len <= 0xFFFF && !err) err = FIXSCRIPT_ERR_BAD_FORMAT;
               if (err) goto error;
            }

            if (len < 0) {
               err = FIXSCRIPT_ERR_BAD_FORMAT;
               goto error;
            }

            if (type == SER_ARRAY && len == 0) {
               err = FIXSCRIPT_ERR_BAD_FORMAT;
               goto error;
            }

            if (type >= SER_ARRAY && type <= SER_ARRAY_INT) {
               array = fixscript_create_array(heap, 0);
            }
            else {
               array = fixscript_create_string(heap, "", 0);
            }
            if (!array.value) {
               err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
               goto error;
            }

            arr = &heap->data[array.value];
            if (type == SER_ARRAY || type == SER_ARRAY_INT || type == SER_STRING_INT) {
               arr->type = ARR_INT;
               #ifndef FIXSCRIPT_NO_JIT
                  heap->jit_array_get_funcs[array.value] = heap->jit_array_get_int_func;
                  heap->jit_array_set_funcs[array.value*2+0] = heap->jit_array_set_int_func[0];
                  heap->jit_array_set_funcs[array.value*2+1] = heap->jit_array_set_int_func[1];
                  heap->jit_array_append_funcs[array.value*2+0] = heap->jit_array_append_int_func[0];
                  heap->jit_array_append_funcs[array.value*2+1] = heap->jit_array_append_int_func[1];
               #endif
            }
            else if (type == SER_ARRAY_SHORT || type == SER_STRING_SHORT) {
               arr->type = ARR_SHORT;
               #ifndef FIXSCRIPT_NO_JIT
                  heap->jit_array_get_funcs[array.value] = heap->jit_array_get_short_func;
                  heap->jit_array_set_funcs[array.value*2+0] = heap->jit_array_set_short_func[0];
                  heap->jit_array_set_funcs[array.value*2+1] = heap->jit_array_set_short_func[1];
                  heap->jit_array_append_funcs[array.value*2+0] = heap->jit_array_append_short_func[0];
                  heap->jit_array_append_funcs[array.value*2+1] = heap->jit_array_append_short_func[1];
               #endif
            }

            err = fixscript_append_array_elem(heap, list, array);
            if (err) goto error;

            err = fixscript_set_array_length(heap, array, len);
            if (err) goto error;

            if (type == SER_ARRAY_BYTE || type == SER_STRING_BYTE) {
               if (*remaining < len) {
                  err = FIXSCRIPT_ERR_BAD_FORMAT;
                  goto error;
               }
               memcpy(arr->byte_data, *buf, len);
               (*buf) += len;
               (*remaining) -= len;
               *value = array;
               goto got_value;
            }

            if (type == SER_ARRAY_SHORT || type == SER_STRING_SHORT) {
               sum = ((int64_t)len) << 1;
               if (sum > INT_MAX) {
                  err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
                  goto error;
               }
               if (*remaining < sum) {
                  err = FIXSCRIPT_ERR_BAD_FORMAT;
                  goto error;
               }
               little_endian_test = 1;
               if (*(char *)&little_endian_test == 1) {
                  memcpy(arr->short_data, *buf, (int)sum);
               }
               else {
                  for (i=0; i<len; i++) {
                     arr->short_data[i] = (*buf)[i*2+0] | ((*buf)[i*2+1] << 8);
                  }
               }
               (*buf) += (int)sum;
               (*remaining) -= (int)sum;
               *value = array;

               max_val = 0;
               for (i=0; i<len; i++) {
                  max_val |= arr->short_data[i];
               }
               if ((max_val & ~0xFF) == 0) {
                  err = FIXSCRIPT_ERR_BAD_FORMAT;
                  goto error;
               }
               goto got_value;
            }

            if (type == SER_ARRAY_INT || type == SER_STRING_INT) {
               sum = ((int64_t)len) << 2;
               if (sum > INT_MAX) {
                  err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
                  goto error;
               }
               if (*remaining < sum) {
                  err = FIXSCRIPT_ERR_BAD_FORMAT;
                  goto error;
               }
               little_endian_test = 1;
               if (*(char *)&little_endian_test == 1) {
                  memcpy(arr->data, *buf, (int)sum);
               }
               else {
                  for (i=0; i<len; i++) {
                     arr->data[i] = (*buf)[i*4+0] | ((*buf)[i*4+1] << 8) | ((*buf)[i*4+2] << 16) | ((*buf)[i*4+3] << 24);
                  }
               }
               (*buf) += (int)sum;
               (*remaining) -= (int)sum;
               *value = array;

               max_val = 0;
               for (i=0; i<len; i++) {
                  max_val |= arr->data[i];
               }
               if ((max_val & ~0xFFFF) == 0) {
                  err = FIXSCRIPT_ERR_BAD_FORMAT;
                  goto error;
               }
               goto got_value;
            }

            err = dynarray_add(&stack, (void *)(intptr_t)cur_value.value);
            if (err) goto error;
            err = dynarray_add(&stack, (void *)(intptr_t)cur_idx);
            if (err) goto error;

            cur_value = array;
            cur_idx = 0;
            goto fetch_value;
         }

         case SER_HASH: {
            len = type >> 4;
            if (len == 0x0D) {
               err = unserialize_byte(buf, remaining, &len);
               if (len <= 12 && !err) err = FIXSCRIPT_ERR_BAD_FORMAT;
               if (err) goto error;
            }
            else if (len == 0x0E) {
               err = unserialize_short(buf, remaining, &len);
               if (len <= 0xFF && !err) err = FIXSCRIPT_ERR_BAD_FORMAT;
               if (err) goto error;
            }
            else if (len == 0x0F) {
               err = unserialize_int(buf, remaining, &len);
               if (len <= 0xFFFF && !err) err = FIXSCRIPT_ERR_BAD_FORMAT;
               if (err) goto error;
            }

            if (len < 0) {
               err = FIXSCRIPT_ERR_BAD_FORMAT;
               goto error;
            }

            hash = fixscript_create_hash(heap);
            if (!hash.value) {
               err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
               goto error;
            }

            err = fixscript_append_array_elem(heap, list, hash);
            if (err) goto error;

            if (len == 0) {
               *value = hash;
               goto got_value;
            }

            err = dynarray_add(&stack, (void *)(intptr_t)cur_value.value);
            if (err) goto error;
            err = dynarray_add(&stack, (void *)(intptr_t)cur_idx);
            if (err) goto error;

            cur_value = hash;
            cur_idx = len << 1;
            goto fetch_value;
         }
      }

      err = FIXSCRIPT_ERR_BAD_FORMAT;
      goto error;

pop_stack:
      *value = cur_value;
      cur_idx = (intptr_t)stack.data[--stack.len];
      cur_value.value = (intptr_t)stack.data[--stack.len];
      cur_value.is_array = 1;
      if (!cur_value.value) break;
      continue;

got_value:
      if (!cur_value.value) break;
      continue;
   }

error:
   free(stack.data);
   return err;
}


int fixscript_unserialize(Heap *heap, Value buf_val, int *off, int len, Value *value)
{
   Value list;
   Array *arr;
   const unsigned char *buf, *byte_data;
   int err, remaining, unspec_len;

   if (!fixscript_is_array(heap, buf_val)) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   arr = &heap->data[buf_val.value];
   if (arr->type != ARR_BYTE) {
      return FIXSCRIPT_ERR_INVALID_BYTE_ARRAY;
   }

   if (*off < 0) {
      return FIXSCRIPT_ERR_OUT_OF_BOUNDS;
   }

   unspec_len = (len < 0);
   if (unspec_len) {
      len = arr->len - *off;
      if (len < 0) {
         return FIXSCRIPT_ERR_OUT_OF_BOUNDS;
      }
   }

   if (((int64_t)*off) + ((int64_t)len) > arr->len) {
      return FIXSCRIPT_ERR_OUT_OF_BOUNDS;
   }

   byte_data = arr->byte_data;
   buf = byte_data + *off;
   remaining = len;
   
   list = fixscript_create_array(heap, 0);
   if (!list.value) {
      return FIXSCRIPT_ERR_OUT_OF_MEMORY;
   }

   err = unserialize_value(heap, &buf, &remaining, list, value);
   *off = buf - byte_data;
   if (err == FIXSCRIPT_SUCCESS && !unspec_len && remaining != 0) {
      err = FIXSCRIPT_ERR_BAD_FORMAT;
   }
   reclaim_array(heap, list.value, NULL);
   return err;
}


int fixscript_serialize_to_array(Heap *heap, char **buf, int *len_out, Value value)
{
   Value buf_val;
   Array *arr;
   int err, len;

   buf_val = fixscript_create_array(heap, 0);
   if (!buf_val.value) return FIXSCRIPT_ERR_OUT_OF_MEMORY;

   if (!len_out) {
      err = fixscript_set_array_length(heap, buf_val, sizeof(int));
      if (err != FIXSCRIPT_SUCCESS) return err;
   }

   err = fixscript_serialize(heap, &buf_val, value);
   if (err != FIXSCRIPT_SUCCESS) return err;

   err = fixscript_get_array_length(heap, buf_val, &len);
   if (err) return err;
   
   arr = &heap->data[buf_val.value];
   if (arr->type != ARR_BYTE) return FIXSCRIPT_ERR_INVALID_BYTE_ARRAY;

   if (!len_out) {
      len -= sizeof(int);
      memcpy(arr->byte_data, &len, sizeof(int));
   }
   else {
      *len_out = len;
   }

   *buf = (char *)arr->byte_data;
   arr->byte_data = NULL;
   reclaim_array(heap, buf_val.value, arr);
   return FIXSCRIPT_SUCCESS;
}


int fixscript_unserialize_from_array(Heap *heap, const char *buf, int *off_out, int len, Value *value)
{
   Value buf_val;
   Array *arr;
   int err, off;

   buf_val = fixscript_create_array(heap, 0);
   if (!buf_val.value) return FIXSCRIPT_ERR_OUT_OF_MEMORY;

   if (len < 0) {
      memcpy(&len, buf, sizeof(int));
      buf += sizeof(int);
   }

   arr = &heap->data[buf_val.value];
   if (arr->type != ARR_BYTE) return FIXSCRIPT_ERR_INVALID_BYTE_ARRAY;

   arr->flags = calloc(FLAGS_SIZE(len), sizeof(int));
   arr->byte_data = (unsigned char *)buf;
   arr->size = len;
   arr->len = len;
   heap->total_size += (int64_t)FLAGS_SIZE(arr->size) * sizeof(int) + (int64_t)arr->size;

   off = 0;
   err = fixscript_unserialize(heap, buf_val, &off, off_out? -1 : len, value);
   if (err == FIXSCRIPT_SUCCESS && off_out) {
      *off_out = off;
   }

   arr = &heap->data[buf_val.value];
   arr->byte_data = NULL;
   reclaim_array(heap, buf_val.value, arr);
   return err;
}


////////////////////////////////////////////////////////////////////////
// Tokenizer:
////////////////////////////////////////////////////////////////////////


static int is_ident(char c)
{
   return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c == '_');
}


static int is_digit(char c)
{
   return (c >= '0' && c <= '9');
}


static int is_hex_digit(char c)
{
   return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}


static int get_hex_digit(char c)
{
   if (c >= '0' && c <= '9') return c - '0';
   if (c >= 'a' && c <= 'f') return c - 'a' + 10;
   if (c >= 'A' && c <= 'F') return c - 'A' + 10;
   return -1;
}


static int is_whitespace(char c)
{
   return (c == ' ' || c == '\r' || c == '\n' || c == '\t');
}


static int is_newline(char c)
{
   return (c == '\r' || c == '\n');
}


static int is_symbol(char c)
{
   switch (c) {
      case '(':
      case ')':
      case '{':
      case '}':
      case '[':
      case ']':
      case ',':
      case ';':
      case '~':
      case ':':
      case '@':
      case '?':
      case '#':
      case '$':
      case '\\':
      case '`':
         return 1;

      case '+':
      case '-':
      case '*':
      case '/':
      case '%':
      case '&':
      case '|':
      case '^':
      case '.':
         return 2;

      case '<':
      case '=':
      case '!':
         return 3;

      case '>':
         return 4;
   }
   return 0;
}


static int is_symbol1(char c)
{
   return 1;
}


static int is_symbol2(char c1, char c2)
{
   switch (c1) {
      case '+': if (c2 == '=' || c2 == '+') return 1; break;
      case '-': if (c2 == '=' || c2 == '-' || c2 == '>') return 1; break;
      case '*': if (c2 == '=') return 1; break;
      case '/': if (c2 == '=') return 1; break;
      case '%': if (c2 == '=') return 1; break;
      case '&': if (c2 == '=' || c2 == '&') return 1; break;
      case '|': if (c2 == '=' || c2 == '|') return 1; break;
      case '^': if (c2 == '=') return 1; break;
      case '<': if (c2 == '=' || c2 == '<') return 1; break;
      case '>': if (c2 == '=' || c2 == '>') return 1; break;
      case '=': if (c2 == '=') return 1; break;
      case '!': if (c2 == '=') return 1; break;
      case '.': if (c2 == '.') return 1; break;
   }
   return 0;
}


static int is_symbol3(char c1, char c2, char c3)
{
   if (c1 == '=' && c2 == '=' && c3 == '=') return 1;
   if (c1 == '!' && c2 == '=' && c3 == '=') return 1;
   if (c1 == '<' && c2 == '<' && c3 == '=') return 1;
   if (c1 == '>' && c2 == '>' && c3 == '=') return 1;
   if (c1 == '>' && c2 == '>' && c3 == '>') return 1;
   return 0;
}


static int is_symbol4(char c1, char c2, char c3, char c4)
{
   if (c1 == '>' && c2 == '>' && c3 == '>' && c4 == '=') return 1;
   return 0;
}


static int is_unknown(char c)
{
   if (is_ident(c)) return 0;
   if (is_digit(c)) return 0;
   if (is_whitespace(c)) return 0;
   if (is_symbol(c)) return 0;
   if (c == '\'' || c == '"' || c == 0) return 0;
   return 1;
}


static int set_value(Tokenizer *tok, const char *start, int type)
{
   tok->value = start;
   tok->len = tok->cur - start;
   tok->type = type;
   
   if (tok->cur_token) {
      if (type != tok->cur_token[TOK_type].value) {
         tok->error = "token type mismatch";
         return 0;
      }
      if (tok->cur != tok->tokens_src + tok->cur_token[TOK_off].value + tok->cur_token[TOK_len].value) {
         tok->error = "token length mismatch";
         return 0;
      }
      tok->cur_token += TOK_SIZE;
   }
   return 1;
}


static void skip_whitespace(Tokenizer *tok)
{
   while (is_whitespace(*tok->cur)) {
      if (*tok->cur == '\n') {
         tok->line++;
      }
      tok->cur++;
   }
}


static int next_token(Tokenizer *tok)
{
   const char *start;
   int cnt, c, len, closed, type;
   char end_char;
   const char *end_ptr = NULL;
   
   if (tok->again) {
      if (tok->again == 2 || tok->error) {
         return 0;
      }
      tok->again = 0;
      return 1;
   }

   if (tok->cur_token) {
      if (tok->cur_token == tok->tokens_end) {
         tok->again = 2;
         return 0;
      }
      tok->cur = tok->tokens_src + tok->cur_token[TOK_off].value;
      end_ptr = tok->cur + tok->cur_token[TOK_len].value;
      tok->line = tok->cur_token[TOK_line].value;
   }
   else {
      for (;;) {
         skip_whitespace(tok);

         if (tok->cur[0] == '/' && tok->cur[1] == '/') {
            tok->cur += 2;
            while (*tok->cur && !is_newline(*tok->cur)) tok->cur++;
            if (*tok->cur == '\r') tok->cur++;
            if (*tok->cur == '\n') {
               tok->cur++;
               tok->line++;
            }
            continue;
         }

         if (tok->cur[0] == '/' && tok->cur[1] == '*') {
            tok->cur += 2;
            while (tok->cur[0] && (tok->cur[0] != '*' || tok->cur[1] != '/')) {
               if (*tok->cur == '\n') {
                  tok->line++;
               }
               tok->cur++;
            }
            if (!tok->cur[0]) {
               continue;
            }
            tok->cur += 2;
            continue;
         }

         break;
      }
   }

   if (*tok->cur == '\0') {
      tok->again = 2;
      return 0;
   }

   start = tok->cur;

   if (is_ident(*tok->cur)) {
      while ((is_ident(*tok->cur) || is_digit(*tok->cur)) && (!end_ptr || tok->cur < end_ptr)) {
         tok->cur++;
      }

      tok->type = TOK_IDENT;
      len = tok->cur - start;
      switch (len) {
         case 2:
            if (!strncmp(start, "do", len)) { tok->type = KW_DO; break; }
            if (!strncmp(start, "if", len)) { tok->type = KW_IF; break; }
            break;

         case 3:
            if (!strncmp(start, "for", len)) { tok->type = KW_FOR; break; }
            if (!strncmp(start, "use", len)) { tok->type = KW_USE; break; }
            if (!strncmp(start, "var", len)) { tok->type = KW_VAR; break; }
            break;

         case 4:
            if (!strncmp(start, "case", len)) { tok->type = KW_CASE; break; }
            if (!strncmp(start, "else", len)) { tok->type = KW_ELSE; break; }
            break;

         case 5:
            if (!strncmp(start, "break", len)) { tok->type = KW_BREAK; break; }
            if (!strncmp(start, "const", len)) { tok->type = KW_CONST; break; }
            if (!strncmp(start, "while", len)) { tok->type = KW_WHILE; break; }
            break;

         case 6:
            if (!strncmp(start, "import", len)) { tok->type = KW_IMPORT; break; }
            if (!strncmp(start, "return", len)) { tok->type = KW_RETURN; break; }
            if (!strncmp(start, "switch", len)) { tok->type = KW_SWITCH; break; }
            break;

         case 7:
            if (!strncmp(start, "default", len)) { tok->type = KW_DEFAULT; break; }
            break;

         case 8:
            if (!strncmp(start, "continue", len)) { tok->type = KW_CONTINUE; break; }
            if (!strncmp(start, "function", len)) { tok->type = KW_FUNCTION; break; }
            break;
      }

      if (*tok->cur == '#' && tok->type == TOK_IDENT && (!end_ptr || tok->cur+1 < end_ptr) && is_digit(tok->cur[1])) {
         tok->cur++;
         while (is_digit(*tok->cur) && (!end_ptr || tok->cur < end_ptr)) {
            tok->cur++;
         }
         tok->type = TOK_FUNC_REF;
      }
      return set_value(tok, start, tok->type);
   }

   if (tok->cur[0] == '0' && tok->cur[1] == 'x' && (!end_ptr || tok->cur+1 < end_ptr)) {
      if (!is_hex_digit(tok->cur[2]) || (end_ptr && tok->cur+2 >= end_ptr)) {
         if (tok->ignore_errors) {
            tok->cur += 2;
            return set_value(tok, start, TOK_UNKNOWN);
         }
         tok->error = "invalid hexadecimal constant";
         return 0;
      }
      tok->cur += 3;
      while (is_hex_digit(*tok->cur) && (!end_ptr || tok->cur < end_ptr)) tok->cur++;
      return set_value(tok, start, TOK_HEX_NUMBER);
   }

   if (is_digit(*tok->cur)) {
      tok->type = TOK_NUMBER;
      tok->cur++;
      while (is_digit(*tok->cur) && (!end_ptr || tok->cur < end_ptr)) tok->cur++;
      if (*tok->cur == '.' && (!end_ptr || tok->cur < end_ptr)) {
         tok->cur++;
         if (*tok->cur == '.' && (!end_ptr || tok->cur < end_ptr)) {
            tok->cur--;
         }
         else {
            tok->type = TOK_FLOAT_NUMBER;
            if (!is_digit(*tok->cur) || (end_ptr && tok->cur >= end_ptr)) {
               if (tok->ignore_errors) {
                  return set_value(tok, start, TOK_UNKNOWN);
               }
               tok->error = "invalid float constant";
               return 0;
            }
            while (is_digit(*tok->cur) && (!end_ptr || tok->cur < end_ptr)) tok->cur++;
         }
      }
      if ((*tok->cur == 'e' || *tok->cur == 'E') && (!end_ptr || tok->cur < end_ptr)) {
         tok->type = TOK_FLOAT_NUMBER;
         tok->cur++;
         if ((*tok->cur == '+' || *tok->cur == '-') && (!end_ptr || tok->cur < end_ptr)) tok->cur++;
         if (!is_digit(*tok->cur) || (end_ptr && tok->cur >= end_ptr)) {
            if (tok->ignore_errors) {
               return set_value(tok, start, TOK_UNKNOWN);
            }
            tok->error = "invalid float constant";
            return 0;
         }
         while (is_digit(*tok->cur) && (!end_ptr || tok->cur < end_ptr)) tok->cur++;
      }
      return set_value(tok, start, tok->type);
   }

   if (*tok->cur == '\'' || *tok->cur == '"') {
      end_char = *tok->cur++;
      tok->num_chars = 0;
      tok->num_utf8_bytes = 0;
      tok->max_num_value = 0xFF;
      closed = 0;

      while (*tok->cur && (!end_ptr || tok->cur < end_ptr)) {
         if (*tok->cur == end_char) {
            tok->cur++;
            closed = 1;
            break;
         }
         if (*tok->cur == '\\') {
            tok->cur++;
            switch ((!end_ptr || tok->cur < end_ptr)? *tok->cur : '\0') {
               case 'r':
               case 'n':
               case 't':
               case '\\':
               case '\'':
               case '"':
                  tok->num_utf8_bytes++;
                  break;

               case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
               case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
               case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
                  if (tok->cur[1] && is_hex_digit(tok->cur[1]) && (!end_ptr || tok->cur+1 < end_ptr)) {
                     c = (get_hex_digit(tok->cur[0]) << 4) |
                         (get_hex_digit(tok->cur[1]));
                     if (c < 0x80) {
                        tok->num_utf8_bytes++;
                     }
                     else {
                        tok->num_utf8_bytes += 2;
                     }
                     tok->cur++;
                  }
                  else {
                     if (tok->ignore_errors) {
                        tok->cur++;
                        return set_value(tok, start, TOK_UNKNOWN);
                     }
                     tok->error = "bad escape sequence";
                     return 0;
                  }
                  break;

               case 'u':
                  if (tok->cur[1] && tok->cur[2] && tok->cur[3] && tok->cur[4] &&
                      is_hex_digit(tok->cur[1]) && is_hex_digit(tok->cur[2]) && is_hex_digit(tok->cur[3]) && is_hex_digit(tok->cur[4]) &&
                      (!end_ptr || tok->cur+4 < end_ptr))
                  {
                     c = (get_hex_digit(tok->cur[1]) << 12) |
                         (get_hex_digit(tok->cur[2]) << 8) |
                         (get_hex_digit(tok->cur[3]) << 4) |
                         (get_hex_digit(tok->cur[4]));
                     if (c >= 0xD800 && c <= 0xDFFF) {
                        if (tok->ignore_errors) {
                           tok->cur += 5;
                           return set_value(tok, start, TOK_UNKNOWN);
                        }
                        tok->error = "illegal code point";
                        return 0;
                     }
                     if (c < 0x80) {
                        tok->num_utf8_bytes++;
                     }
                     else if (c < 0x800) {
                        tok->num_utf8_bytes += 2;
                     }
                     else {
                        tok->num_utf8_bytes += 3;
                     }
                     if (c > tok->max_num_value) {
                        tok->max_num_value = c;
                     }
                     tok->cur += 4;
                  }
                  else {
                     if (tok->ignore_errors) {
                        tok->cur++;
                        return set_value(tok, start, TOK_UNKNOWN);
                     }
                     tok->error = "bad escape sequence";
                     return 0;
                  }
                  break;

               case 'U':
                  if (tok->cur[1] && tok->cur[2] && tok->cur[3] && tok->cur[4] && tok->cur[5] && tok->cur[6] &&
                      is_hex_digit(tok->cur[1]) && is_hex_digit(tok->cur[2]) && is_hex_digit(tok->cur[3]) && is_hex_digit(tok->cur[4]) && is_hex_digit(tok->cur[5]) && is_hex_digit(tok->cur[6]) &&
                      (!end_ptr || tok->cur+6 < end_ptr))
                  {
                     c = (get_hex_digit(tok->cur[1]) << 20) |
                         (get_hex_digit(tok->cur[2]) << 16) |
                         (get_hex_digit(tok->cur[3]) << 12) |
                         (get_hex_digit(tok->cur[4]) << 8) |
                         (get_hex_digit(tok->cur[5]) << 4) |
                         (get_hex_digit(tok->cur[6]));
                     if (c > 0x10FFFF) {
                        if (tok->ignore_errors) {
                           tok->cur++;
                           return set_value(tok, start, TOK_UNKNOWN);
                        }
                        tok->error = "illegal code point";
                        return 0;
                     }
                     if (c >= 0xD800 && c <= 0xDFFF) {
                        if (tok->ignore_errors) {
                           tok->cur++;
                           return set_value(tok, start, TOK_UNKNOWN);
                        }
                        tok->error = "illegal code point";
                        return 0;
                     }
                     if (c < 0x80) {
                        tok->num_utf8_bytes++;
                     }
                     else if (c < 0x800) {
                        tok->num_utf8_bytes += 2;
                     }
                     else if (c < 0x10000) {
                        tok->num_utf8_bytes += 3;
                     }
                     else {
                        tok->num_utf8_bytes += 4;
                     }
                     if (c > tok->max_num_value) {
                        tok->max_num_value = c;
                     }
                     tok->cur += 6;
                  }
                  else {
                     if (tok->ignore_errors) {
                        tok->cur++;
                        return set_value(tok, start, TOK_UNKNOWN);
                     }
                     tok->error = "bad escape sequence";
                     return 0;
                  }
                  break;

               default:
                  if (tok->ignore_errors) {
                     if ((!end_ptr || tok->cur < end_ptr)? *tok->cur : '\0') {
                        tok->cur++;
                     }
                     return set_value(tok, start, TOK_UNKNOWN);
                  }
                  tok->error = "bad escape sequence";
                  return 0;
            }
            tok->cur++;
            tok->num_chars++;
            continue;
         }

         if (*tok->cur == '\r' || *tok->cur == '\n') {
            if (tok->ignore_errors) {
               return set_value(tok, start, TOK_UNKNOWN);
            }
            tok->error = end_char == '\''? "unclosed char literal" : "unclosed string literal";
            return 0;
         }

         if ((tok->cur[0] & 0x80) == 0) {
            c = tok->cur[0];
            tok->cur++;
            tok->num_utf8_bytes++;
         }
         else if ((tok->cur[0] & 0xE0) == 0xC0 && (tok->cur[1] & 0xC0) == 0x80 && (!end_ptr || tok->cur+1 < end_ptr)) {
            c = ((tok->cur[0] & 0x1F) << 6) | (tok->cur[1] & 0x3F);
            if (c < 0x80) {
               tok->error = "illegal UTF-8 sequence";
               return 0;
            }
            tok->cur += 2;
            tok->num_utf8_bytes += 2;
         }
         else if ((tok->cur[0] & 0xF0) == 0xE0 && (tok->cur[1] & 0xC0) == 0x80 && (tok->cur[2] & 0xC0) == 0x80 && (!end_ptr || tok->cur+2 < end_ptr)) {
            c = ((tok->cur[0] & 0x0F) << 12) | ((tok->cur[1] & 0x3F) << 6) | (tok->cur[2] & 0x3F);
            if (c < 0x800) {
               tok->error = "illegal UTF-8 sequence";
               return 0;
            }
            tok->cur += 3;
            tok->num_utf8_bytes += 3;
         }
         else if ((tok->cur[0] & 0xF8) == 0xF0 && (tok->cur[1] & 0xC0) == 0x80 && (tok->cur[2] & 0xC0) == 0x80 && (tok->cur[3] & 0xC0) == 0x80 && (!end_ptr || tok->cur+3 < end_ptr)) {
            c = ((tok->cur[0] & 0x07) << 18) | ((tok->cur[1] & 0x3F) << 12) | ((tok->cur[2] & 0x3F) << 6) | (tok->cur[3] & 0x3F);
            if (c < 0x10000 || c > 0x10FFFF) {
               tok->error = "illegal UTF-8 sequence";
               return 0;
            }
            tok->cur += 4;
            tok->num_utf8_bytes += 4;
         }
         else {
            tok->error = "illegal UTF-8 sequence";
            return 0;
         }

         if (c >= 0xD800 && c <= 0xDFFF) {
            tok->error = "illegal UTF-8 sequence";
            return 0;
         }

         if (c > tok->max_num_value) {
            tok->max_num_value = c;
         }

         tok->num_chars++;
      }

      if (!closed) {
         if (tok->ignore_errors) {
            return set_value(tok, start, TOK_UNKNOWN);
         }
         tok->error = end_char == '\''? "unclosed char literal" : "unclosed string literal";
         return 0;
      }

      if (end_char == '\'') {
         if (tok->num_chars == 0) {
            if (tok->ignore_errors) {
               return set_value(tok, start, TOK_UNKNOWN);
            }
            tok->error = "empty char literal";
            return 0;
         }
         if (tok->max_num_value > 0xFF && tok->num_chars > 1) {
            if (tok->ignore_errors) {
               return set_value(tok, start, TOK_UNKNOWN);
            }
            tok->error = "multiple characters in char literal";
            return 0;
         }
         if (tok->num_chars > 4) {
            if (tok->ignore_errors) {
               return set_value(tok, start, TOK_UNKNOWN);
            }
            tok->error = "more than 4 characters in packed char literal";
            return 0;
         }
      }

      return set_value(tok, start, end_char == '\''? TOK_CHAR : TOK_STRING);
   }

   cnt = is_symbol(*tok->cur);
   if (cnt >= 4 && tok->cur[1] && tok->cur[2] && is_symbol4(tok->cur[0], tok->cur[1], tok->cur[2], tok->cur[3]) && (!end_ptr || tok->cur+3 < end_ptr)) {
      type = SYM4(tok->cur[0], tok->cur[1], tok->cur[2], tok->cur[3]);
      tok->cur += 4;
      return set_value(tok, start, type);
   }
   if (cnt >= 3 && tok->cur[1] && is_symbol3(tok->cur[0], tok->cur[1], tok->cur[2]) && (!end_ptr || tok->cur+2 < end_ptr)) {
      type = SYM3(tok->cur[0], tok->cur[1], tok->cur[2]);
      tok->cur += 3;
      return set_value(tok, start, type);
   }
   if (cnt >= 2 && is_symbol2(tok->cur[0], tok->cur[1]) && (!end_ptr || tok->cur+1 < end_ptr)) {
      type = SYM2(tok->cur[0], tok->cur[1]);
      tok->cur += 2;
      return set_value(tok, start, type);
   }
   if (cnt >= 1 && is_symbol1(tok->cur[0])) {
      type = tok->cur[0];
      tok->cur++;
      return set_value(tok, start, type);
   }

   tok->cur++;
   while (is_unknown(*tok->cur) && (!end_ptr || tok->cur < end_ptr)) {
      tok->cur++;
   }

   return set_value(tok, start, TOK_UNKNOWN);
}


// note: zero character is escaped as 0xFF (beware of signed chars when testing for it)
static char *get_token_string(Tokenizer *tok)
{
   const char *s, *end;
   char *out, *out_buf;
   int c;
   
   if (tok->type != TOK_STRING) {
      return NULL;
   }

   out = out_buf = malloc(tok->num_utf8_bytes+1);
   s = tok->value+1;
   end = tok->value+tok->len-1;
   while (s < end) {
      if (*s == '\\') {
         s++;
         switch (*s++) {
            case 'r': *out++ = '\r'; break;
            case 'n': *out++ = '\n'; break;
            case 't': *out++ = '\t'; break;
            case '\\': *out++ = '\\'; break;
            case '\'': *out++ = '\''; break;
            case '"': *out++ = '\"'; break;

            case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
            case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
            case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
               c = (get_hex_digit(s[-1]) << 4) |
                   (get_hex_digit(s[0]));
               s++;
               if (c >= 0x80) {
                  *out++ = (c >> 6) | 0xC0;
                  *out++ = (c & 0x3F) | 0x80;
               }
               else {
                  *out++ = c == 0? 0xFF : c;
               }
               break;

            case 'u':
               c = (get_hex_digit(s[0]) << 12) |
                   (get_hex_digit(s[1]) << 8) |
                   (get_hex_digit(s[2]) << 4) |
                   (get_hex_digit(s[3]));
               s += 4;
               if (c >= 0x800) {
                  *out++ = (c >> 12) | 0xE0;
                  *out++ = ((c >> 6) & 0x3F) | 0x80;
                  *out++ = (c & 0x3F) | 0x80;
               }
               else if (c >= 0x80) {
                  *out++ = (c >> 6) | 0xC0;
                  *out++ = (c & 0x3F) | 0x80;
               }
               else {
                  *out++ = c == 0? 0xFF : c;
               }
               break;

            case 'U':
               c = (get_hex_digit(s[0]) << 20) |
                   (get_hex_digit(s[1]) << 16) |
                   (get_hex_digit(s[2]) << 12) |
                   (get_hex_digit(s[3]) << 8) |
                   (get_hex_digit(s[4]) << 4) |
                   (get_hex_digit(s[5]));
               s += 6;
               if (c >= 0x10000) {
                  *out++ = (c >> 18) | 0xF0;
                  *out++ = ((c >> 12) & 0x3F) | 0x80;
                  *out++ = ((c >> 6) & 0x3F) | 0x80;
                  *out++ = (c & 0x3F) | 0x80;
               }
               else if (c >= 0x800) {
                  *out++ = (c >> 12) | 0xE0;
                  *out++ = ((c >> 6) & 0x3F) | 0x80;
                  *out++ = (c & 0x3F) | 0x80;
               }
               else if (c >= 0x80) {
                  *out++ = (c >> 6) | 0xC0;
                  *out++ = (c & 0x3F) | 0x80;
               }
               else {
                  *out++ = c == 0? 0xFF : c;
               }
               break;
         }
      }
      else {
         *out++ = *s++;
      }
   }
   out_buf[tok->num_utf8_bytes] = '\0';
   return out_buf;
}


static int get_token_char(Tokenizer *tok, Value *value_out)
{
   const char *s, *end;
   int cur_char, value=0, idx=0;
   
   if (tok->type != TOK_CHAR) {
      return 0;
   }

   s = tok->value+1;
   end = tok->value+tok->len-1;
   while (s < end) {
      if (*s == '\\') {
         s++;
         switch (*s++) {
            case 'r': cur_char = '\r'; break;
            case 'n': cur_char = '\n'; break;
            case 't': cur_char = '\t'; break;
            case '\\': cur_char = '\\'; break;
            case '\'': cur_char = '\''; break;
            case '"': cur_char = '"'; break;

            case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
            case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
            case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
               cur_char = (get_hex_digit(s[-1]) << 4) |
                          (get_hex_digit(s[0]));
               s++;
               break;

            case 'u':
               cur_char = (get_hex_digit(s[0]) << 12) |
                          (get_hex_digit(s[1]) << 8) |
                          (get_hex_digit(s[2]) << 4) |
                          (get_hex_digit(s[3]));
               s += 4;
               break;

            case 'U':
               cur_char = (get_hex_digit(s[0]) << 20) |
                          (get_hex_digit(s[1]) << 16) |
                          (get_hex_digit(s[2]) << 12) |
                          (get_hex_digit(s[3]) << 8) |
                          (get_hex_digit(s[4]) << 4) |
                          (get_hex_digit(s[5]));
               s += 6;
               break;

            default:
               return 0;
         }
      }
      else {
         if ((s[0] & 0x80) == 0) {
            cur_char = s[0];
            s++;
         }
         else if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
            cur_char = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
            s += 2;
         }
         else if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
            cur_char = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
            s += 3;
         }
         else if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
            cur_char = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
            s += 4;
         }
         else {
            return 0;
         }
      }
      if (tok->num_chars == 1) {
         *value_out = fixscript_int(cur_char);
         return 1;
      }
      value |= cur_char << ((idx++)*8);
   }

   *value_out = fixscript_int(value);
   return 1;
}


static void undo_token(Tokenizer *tok)
{
   if (tok->again == 0) {
      tok->again = 1;
   }
}


////////////////////////////////////////////////////////////////////////
// Parser:
////////////////////////////////////////////////////////////////////////


static void inc_stack(Parser *par, int change)
{
   par->stack_pos += change;
   if (par->stack_pos > par->max_stack) {
      par->max_stack = par->stack_pos;
   }
}


static void buf_append(Parser *par, unsigned char bytecode)
{
   if (par->buf_len == par->buf_size) {
      par->buf_size <<= 1;
      par->buf = realloc(par->buf, par->buf_size);
   }
   par->last_buf_pos = par->buf_len;
   par->buf[par->buf_len++] = bytecode;
}


static void buf_append_int(Parser *par, int value)
{
   union {
      int i;
      unsigned char c[4];
   } int_val;
   int i;

   int_val.i = value;
   for (i=0; i<4; i++) {
      buf_append(par, int_val.c[i]);
   }
}


static void buf_append_const(Parser *par, int value)
{
   union {
      unsigned short s;
      unsigned char c[2];
   } short_val;

   int last_buf_pos = par->buf_len;

   if (value >= -1 && (value <= 32 || value == 63 || value == 64)) {
      buf_append(par, BC_CONST0+value);
      return;
   }
   if (value > 0 && value <= 256) {
      buf_append(par, BC_CONST_P8);
      buf_append(par, value - 1);
      par->last_buf_pos = last_buf_pos;
      return;
   }
   if (value < 0 && value >= -256) {
      buf_append(par, BC_CONST_N8);
      buf_append(par, (-value) - 1);
      par->last_buf_pos = last_buf_pos;
      return;
   }
   if (value > 0 && value <= 65536) {
      buf_append(par, BC_CONST_P16);
      short_val.s = value - 1;
      buf_append(par, short_val.c[0]);
      buf_append(par, short_val.c[1]);
      par->last_buf_pos = last_buf_pos;
      return;
   }
   if (value < 0 && value >= -65536) {
      buf_append(par, BC_CONST_N16);
      short_val.s = (-value) - 1;
      buf_append(par, short_val.c[0]);
      buf_append(par, short_val.c[1]);
      par->last_buf_pos = last_buf_pos;
      return;
   }

   buf_append(par, BC_CONST_I32);
   buf_append_int(par, value);
   par->last_buf_pos = last_buf_pos;
}


static void buf_append_const_float(Parser *par, int value)
{
   int last_buf_pos = par->buf_len;

   buf_append(par, BC_CONST_F32);
   buf_append_int(par, value);
   par->last_buf_pos = last_buf_pos;
}


static void buf_append_load(Parser *par, int pos)
{
   int last_buf_pos;

   if (pos >= -64 && pos <= -1) {
      buf_append(par, BC_LOADM64+64+pos);
   }
   else {
      last_buf_pos = par->buf_len;
      buf_append_const(par, pos-1);
      buf_append(par, BC_LOADN);
      par->last_buf_pos = last_buf_pos;
   }
}


static void buf_append_store(Parser *par, int pos)
{
   int last_buf_pos;

   if (pos >= -64 && pos <= -1) {
      buf_append(par, BC_STOREM64+64+pos);
   }
   else {
      last_buf_pos = par->buf_len;
      buf_append_const(par, pos-1);
      buf_append(par, BC_STOREN);
      par->last_buf_pos = last_buf_pos;
   }
}


static void buf_append_load_local_var(Parser *par, int local_var)
{
   int last_buf_pos;

   last_buf_pos = par->buf_len;
   buf_append(par, BC_LOAD_LOCAL);
   buf_append_int(par, local_var);
   par->last_buf_pos = last_buf_pos;
}


static void buf_append_store_local_var(Parser *par, int local_var)
{
   int last_buf_pos;

   last_buf_pos = par->buf_len;
   buf_append(par, BC_STORE_LOCAL);
   buf_append_int(par, local_var);
   par->last_buf_pos = last_buf_pos;
}


static void buf_append_pop(Parser *par, int num)
{
   int last_buf_pos;

   if (num == 1) {
      buf_append(par, BC_POP);
   }
   else if (num == 2) {
      buf_append(par, BC_POP);
      buf_append(par, BC_POP);
      par->last_buf_pos--;
   }
   else if (num > 2) {
      last_buf_pos = par->buf_len;
      buf_append_const(par, num);
      inc_stack(par, 1);
      buf_append(par, BC_POPN);
      par->stack_pos--;
      par->last_buf_pos = last_buf_pos;
   }
}


static int buf_append_branch(Parser *par, int type)
{
   if (par->long_jumps) {
      switch (type) {
         case BC_BRANCH0: type = BC_BRANCH_LONG; break;
         case BC_JUMP0: type = BC_JUMP_LONG; break;
      }
      buf_append(par, type);
      buf_append_int(par, 0);
      par->last_buf_pos -= 4;
      return par->last_buf_pos;
   }
   else {
      buf_append(par, type);
      buf_append(par, 0);
      par->last_buf_pos--;
      return par->last_buf_pos;
   }
}


static int buf_update_branch(Parser *par, int pos)
{
   union {
      int i;
      unsigned char c[4];
   } int_val;
   int value = par->buf_len - (pos+(par->long_jumps? 5 : 2));

   if (value < 0) {
      par->tok.error = "internal error: negative jump target";
      return 0;
   }

   if (par->long_jumps) {
      int_val.i = value;
      par->buf[pos+1] = int_val.c[0];
      par->buf[pos+2] = int_val.c[1];
      par->buf[pos+3] = int_val.c[2];
      par->buf[pos+4] = int_val.c[3];
      return 1;
   }
   else {
      if (value >= 2048) {
         par->long_jumps = 1;
         return 0;
      }
      par->buf[pos+0] += value >> 8;
      par->buf[pos+1] = value & 0xFF;
      return 1;
   }
}


static void add_line_info(Parser *par);

static void buf_append_loop(Parser *par, int pos)
{
   union {
      unsigned short s;
      unsigned char c[2];
   } short_val;
   int value;

   if (par->heap->time_limit != 0) {
      buf_append(par, BC_EXTENDED);
      buf_append(par, BC_EXT_CHECK_TIME_LIMIT);
      add_line_info(par);
   }

   value = par->buf_len - pos + 1;

   if (value <= 0xFF) {
      buf_append(par, BC_LOOP_I8);
      buf_append(par, value);
      par->last_buf_pos--;
   }
   else if (value <= 0xFFFF) {
      buf_append(par, BC_LOOP_I16);
      short_val.s = value;
      buf_append(par, short_val.c[0]);
      buf_append(par, short_val.c[1]);
      par->last_buf_pos -= 2;
   }
   else {
      buf_append(par, BC_LOOP_I32);
      buf_append_int(par, value);
      par->last_buf_pos -= 4;
   }
}


static int buf_is_const(Parser *par, int pos, int *value, int *is_float)
{
   union {
      unsigned short s;
      unsigned char c[2];
   } short_val;

   union {
      int i;
      unsigned char c[4];
   } int_val;

   unsigned char bc = par->buf[pos];

   if (is_float) {
      *is_float = 0;
   }

   if (bc >= BC_CONSTM1 && (bc <= BC_CONST0+32 || bc == BC_CONST0+63 || bc == BC_CONST0+64)) {
      *value = (int)bc - 0x3F;
      return 1;
   }
   else if (bc == BC_CONST_P8) {
      *value = (int)par->buf[pos+1] + 1;
      return 2;
   }
   else if (bc == BC_CONST_N8) {
      *value = -((int)par->buf[pos+1] + 1);
      return 2;
   }
   else if (bc == BC_CONST_P16) {
      short_val.c[0] = par->buf[pos+1];
      short_val.c[1] = par->buf[pos+2];
      *value = (int)short_val.s + 1;
      return 3;
   }
   else if (bc == BC_CONST_N16) {
      short_val.c[0] = par->buf[pos+1];
      short_val.c[1] = par->buf[pos+2];
      *value = -((int)short_val.s + 1);
      return 3;
   }
   else if (bc == BC_CONST_I32 || bc == BC_CONST_F32) {
      int_val.c[0] = par->buf[pos+1];
      int_val.c[1] = par->buf[pos+2];
      int_val.c[2] = par->buf[pos+3];
      int_val.c[3] = par->buf[pos+4];
      *value = int_val.i;
      if (bc == BC_CONST_F32 && is_float) {
         *is_float = 1;
      }
      return 5;
   }

   return 0;
}


static int buf_is_load(Parser *par, int pos, int *value)
{
   unsigned char bc = par->buf[pos];
   int len;

   if (bc >= BC_LOADM64) {
      *value = (signed char)bc;
      return 1;
   }
   else if ((len = buf_is_const(par, pos, value, NULL))) {
      if (par->buf[pos+len] == BC_LOADN) {
         *value += 1;
         return 1;
      }
   }
   return 0;
}


static int buf_is_load_local_var(Parser *par, int pos, int *local_var)
{
   union {
      int i;
      unsigned char c[4];
   } int_val;

   unsigned char bc = par->buf[pos];

   if (bc == BC_LOAD_LOCAL) {
      int_val.c[0] = par->buf[pos+1];
      int_val.c[1] = par->buf[pos+2];
      int_val.c[2] = par->buf[pos+3];
      int_val.c[3] = par->buf[pos+4];
      *local_var = int_val.i;
      return 1;
   }
   return 0;
}


static int buf_set_call2(Parser *par, int *weak_call)
{
   unsigned char *bc = &par->buf[par->last_buf_pos];

   *weak_call = 0;
   if (*bc == BC_CALL_DIRECT) {
      *bc = BC_CALL2_DIRECT;
      return 1;
   }
   if (*bc == BC_CALL_DYNAMIC) {
      *bc = BC_CALL2_DYNAMIC;
      return 1;
   }
   if (*bc == BC_CALL_NATIVE) {
      *bc = BC_CALL2_NATIVE;
      return 1;
   }
   if (*bc == BC_RETURN2) {
      par->buf_len--;
      *weak_call = 1;
      return 1;
   }
   return 0;
}


static int get_const_string(Parser *par, const char *s)
{
   Value str;
   char *tmp;
   int i, len, value;

   value = ((int)(intptr_t)string_hash_get(&par->const_strings, s)) & 0x7FFFFFFF;
   if (!value) {
      tmp = strdup(s);
      if (tmp) {
         len = strlen(tmp);
         for (i=0; i<len; i++) {
            if ((unsigned char)tmp[i] == 0xFF) {
               tmp[i] = 0;
            }
         }
         str = fixscript_create_string(par->heap, tmp, len);
         if (str.value) {
            str = get_const_string_direct(par->heap, str);
            value = str.value;
            if (value) {
               string_hash_set(&par->const_strings, strdup(s), (void *)(intptr_t)(value | (((uint32_t)par->heap->data[value].is_static) << 31)));
               par->heap->data[value].is_static = 1;
            }
         }
         free(tmp);
      }
   }

   if (!value) {
      par->tok.error = "not enough memory for constant string";
   }
   return value;
}


static void enter_loop(Parser *par, LoopState *state, int has_break, int has_continue, int continue_pc)
{
   if (has_break) {
      state->has_break = par->has_break;
      state->break_stack_pos = par->break_stack_pos;
      state->break_jumps_len = par->break_jumps.len;
      par->has_break = 1;
      par->break_stack_pos = par->stack_pos;
   }
   if (has_continue) {
      state->has_continue = par->has_continue;
      state->continue_pc = par->continue_pc;
      state->continue_stack_pos = par->continue_stack_pos;
      state->continue_jumps_len = par->continue_jumps.len;
      par->has_continue = 1;
      par->continue_pc = continue_pc;
      par->continue_stack_pos = par->stack_pos;
   }
}


static int leave_loop_break(Parser *par, LoopState *state)
{
   int i;

   for (i=state->break_jumps_len; i<par->break_jumps.len; i++) {
      if (!buf_update_branch(par, (intptr_t)par->break_jumps.data[i])) return 0;
   }
   par->has_break = state->has_break;
   par->break_stack_pos = state->break_stack_pos;
   par->break_jumps.len = state->break_jumps_len;
   return 1;
}


static int leave_loop_continue(Parser *par, LoopState *state)
{
   int i;

   for (i=state->continue_jumps_len; i<par->continue_jumps.len; i++) {
      if (!buf_update_branch(par, (intptr_t)par->continue_jumps.data[i])) return 0;
   }
   par->has_continue = state->has_continue;
   par->continue_pc = state->continue_pc;
   par->continue_stack_pos = state->continue_stack_pos;
   par->continue_jumps.len = state->continue_jumps_len;
   return 1;
}


static void add_line_info(Parser *par)
{
   dynarray_add(&par->lines, (void *)(intptr_t)(par->heap->bytecode_size + par->buf_len));
   dynarray_add(&par->lines, (void *)(intptr_t)par->tok.line);
}


static void remove_line_info(Parser *par)
{
   if (par->lines.len >= 2) {
      par->lines.len -= 2;
   }
}


static int has_next(Parser *par)
{
   if (next_token(&par->tok)) {
      undo_token(&par->tok);
      return 1;
   }
   return 0;
}


static int expect_type(Parser *par, int type, const char *error)
{
   if (!next_token(&par->tok)) {
      undo_token(&par->tok);
      if (!par->tok.error) {
         par->tok.error = error;
      }
      return 0;
   }
   if (par->tok.type != type) {
      undo_token(&par->tok);
      par->tok.error = error;
      return 0;
   }
   return 1;
}


static int expect_symbol(Parser *par, char sym, const char *error)
{
   return expect_type(par, sym, error);
}


static int expect_symbol2(Parser *par, char sym1, char sym2, const char *error)
{
   return expect_type(par, SYM2(sym1, sym2), error);
}


static int expect_symbol3(Parser *par, char sym1, char sym2, char sym3, const char *error)
{
   return expect_type(par, SYM3(sym1, sym2, sym3), error);
}


static int expect_symbol4(Parser *par, char sym1, char sym2, char sym3, char sym4, const char *error)
{
   return expect_type(par, SYM4(sym1, sym2, sym3, sym4), error);
}


static Constant *find_constant(Script *script, const char *name, int used_import_alias, int *conflict, Script **script_out);
static int parse_primary_expression(Parser *par);
static int parse_expression(Parser *par);
static int parse_block(Parser *par, int type);
static int parse_constant(Parser *par, Value *value, int int_only);
static int parse_statement(Parser *par, const char *error);

static int extract_tokens(Tokenizer *tok, Heap *heap, Value tokens_val, int src_off)
{
   Value values[64 * TOK_SIZE];
   const char *src;
   int err, cnt, total_cnt, has_token;

   err = fixscript_get_array_length(heap, tokens_val, &total_cnt);
   if (err) return 0;
   total_cnt /= TOK_SIZE;

   tok->ignore_errors = 1;
   src = tok->start;
   cnt = 0;
   for (;;) {
      has_token = next_token(tok);
      if (!has_token || cnt == 64) {
         err = fixscript_set_array_length(heap, tokens_val, (total_cnt + cnt) * TOK_SIZE);
         if (!err) {
            err = fixscript_set_array_range(heap, tokens_val, total_cnt * TOK_SIZE, cnt * TOK_SIZE, values);
         }
         if (err) {
            tok->ignore_errors = 0;
            return 0;
         }
         total_cnt += cnt;
         cnt = 0;
         if (!has_token) break;
      }
      values[cnt*TOK_SIZE + TOK_type] = fixscript_int(tok->type);
      values[cnt*TOK_SIZE + TOK_off] = fixscript_int(tok->value - src + src_off);
      values[cnt*TOK_SIZE + TOK_len] = fixscript_int(tok->len);
      values[cnt*TOK_SIZE + TOK_line] = fixscript_int(tok->line);
      cnt++;
   }

   tok->ignore_errors = 0;
   return 1;
}


static const char *use_tokens(Heap *heap, Value tokens_val, Value source_val, Parser *par)
{
   const char *err_str = NULL;
   Value *new_tokens_arr = NULL, *new_tokens_end, *cur_token;
   char *new_tokens_src = NULL;
   int i, err, len;

   err = fixscript_get_array_length(heap, tokens_val, &len);
   if (err) {
      err_str = fixscript_get_error_msg(err);
      goto error;
   }
   if (len % TOK_SIZE != 0) {
      err_str = "invalid token array length (must be divisible by token size)";
      goto error;
   }

   new_tokens_arr = malloc(len * sizeof(Value));
   if (!new_tokens_arr) {
      err_str = "out of memory";
      goto error;
   }

   err = fixscript_get_array_range(heap, tokens_val, 0, len, new_tokens_arr);
   if (err) {
      err_str = fixscript_get_error_msg(err);
      goto error;
   }
   new_tokens_end = new_tokens_arr + len;

   err = fixscript_get_array_length(heap, source_val, &len);
   if (err) {
      err_str = fixscript_get_error_msg(err);
      goto error;
   }

   for (cur_token = new_tokens_arr; cur_token < new_tokens_end; cur_token += TOK_SIZE) {
      if (cur_token[TOK_off].value < 0 || cur_token[TOK_len].value < 1 || (int64_t)cur_token[TOK_off].value + (int64_t)cur_token[TOK_len].value > (int64_t)len) {
         err_str = "invalid token offset or length";
         goto error;
      }
   }

   new_tokens_src = malloc(len + 1);
   if (!new_tokens_src) {
      err_str = "out of memory";
      goto error;
   }

   err = fixscript_get_array_bytes(heap, source_val, 0, len, new_tokens_src);
   if (!err) {
      for (i=0; i<len; i++) {
         if (new_tokens_src[i] == 0) {
            err = FIXSCRIPT_ERR_INVALID_NULL_STRING;
            break;
         }
      }
   }
   if (err) {
      err_str = fixscript_get_error_msg(err);
      goto error;
   }

   new_tokens_src[len] = 0;

   par->tok.tokens_src = new_tokens_src;
   par->tok.cur_token = new_tokens_arr;
   par->tok.tokens_end = new_tokens_end;
   par->tok.again = 0;

   free(par->tokens_src);
   free(par->tokens_arr);
   par->tokens_src = new_tokens_src;
   par->tokens_arr = new_tokens_arr;
   par->tokens_end = new_tokens_end;
   return NULL;
   
error:
   free(new_tokens_src);
   free(new_tokens_arr);
   return err_str;
}


static int parse_use_inner(Parser *par, Script *script, Value *error, Value func_ref, Value func_data)
{
#ifdef FIXEMBED_TOKEN_DUMP
   Heap *token_heap = par->heap->token_dump_mode? par->heap->token_heap : par->heap;
#else
   Heap *token_heap = par->heap;
#endif
   Value fname_val, tokens_val, source_val, error_val;
   const char *src, *err_str;
   int err, len, remaining, old_line;

   if (par->tokens_arr_val.value) {
      source_val = par->tokens_src_val;
      tokens_val = par->tokens_arr_val;

      remaining = par->tok.tokens_end - par->tok.cur_token;

      err = fixscript_get_array_length(token_heap, tokens_val, &len);
      if (err) {
         par->tok.error = fixscript_get_error_msg(err);
         goto error;
      }

      err = fixscript_copy_array(token_heap, tokens_val, 0, tokens_val, len - remaining, remaining);
      if (!err) {
         err = fixscript_set_array_length(token_heap, tokens_val, remaining);
      }
      if (err) {
         par->tok.error = fixscript_get_error_msg(err);
         goto error;
      }
   }
   else {
      src = par->tok.start;

      source_val = fixscript_create_byte_array(token_heap, src, strlen(src));
      tokens_val = fixscript_create_array(token_heap, 0);
      if (!source_val.value || !tokens_val.value) {
         par->tok.error = "out of memory";
         goto error;
      }

      token_heap->data[source_val.value].is_string = 1;

      old_line = par->tok.line;
      if (!extract_tokens(&par->tok, token_heap, tokens_val, 0)) {
         par->tok.error = "out of memory";
         goto error;
      }
      if (*par->tok.cur != '\0') {
         par->tok.error = "syntax error";
         goto error;
      }
      par->tok.line = old_line;

      fixscript_ref(token_heap, source_val);
      fixscript_ref(token_heap, tokens_val);
      par->tokens_src_val = source_val;
      par->tokens_arr_val = tokens_val;
   }

   fname_val = fixscript_create_string(token_heap, par->fname, -1);
   if (!fname_val.value) {
      par->tok.error = "out of memory";
      goto error;
   }

   if (script) {
      fixscript_run(token_heap, script, "process_tokens#3", &error_val, fname_val, tokens_val, source_val);
   }
   else {
      fixscript_call(token_heap, func_ref, 4, &error_val, func_data, fname_val, tokens_val, source_val);
   }
   if (error_val.value) {
      if (error && !error->value) {
         fixscript_ref(token_heap, error_val);
         *error = error_val;
      }
      goto error;
   }

   err_str = use_tokens(token_heap, tokens_val, source_val, par);
   if (err_str) {
      par->tok.error = err_str;
      goto error;
   }
   return 1;

error:
   return 0;
}


static int parse_import(Parser *par, Value *error, int is_use)
{
#ifdef FIXEMBED_TOKEN_DUMP
   Heap *token_heap = par->heap->token_dump_mode? par->heap->token_heap : par->heap;
#else
   Heap *token_heap = par->heap;
#endif
   Value new_msg;
   char *fname, *s, *p;
   Script *script;
   int i, err, has_fname;
   
   if (!expect_type(par, TOK_STRING, "expected script name")) return 0;
   
   if (!par->load_func) {
      par->tok.error = "can't import scripts with no load script callback defined";
      return 0;
   }

   if (par->heap->cur_import_recursion >= MAX_IMPORT_RECURSION) {
      par->tok.error = "maximum import recursion limit reached";
      return 0;
   }
   
   fname = get_token_string(&par->tok);
   for (i=strlen(fname)-1; i>=0; i--) {
      if ((unsigned char)fname[i] == 0xFF) {
         par->tok.error = "invalid import script name";
         return 0;
      }
   }
   script = par->load_func(is_use? token_heap : par->heap, fname, error, par->load_data);
   free(fname);
   if (!script) {
      if (error) {
         if (fixscript_is_string(token_heap, *error)) {
            err = fixscript_get_string(token_heap, *error, 0, -1, &s, NULL);
            if (!err) {
               has_fname = 0;
               p = strstr(s, ".fix(");
               if (p) {
                  p += 5;
                  if (*p >= '0' && *p <= '9') {
                     while (*p >= '0' && *p <= '9') p++;
                     if (strncmp(p, "): ", 3) == 0) {
                        has_fname = 1;
                     }
                  }
               }
               if (!has_fname) {
                  p = string_format("%s(%d): %s", par->fname, par->tok.line, s);
                  if (p) {
                     new_msg = fixscript_create_string(token_heap, p, -1);
                     if (new_msg.value) {
                        *error = new_msg;
                     }
                     free(p);
                  }
               }
               free(s);
            }
         }
         fixscript_ref(token_heap, *error);
      }
      return 0;
   }

   if (is_use) {
      if (!parse_use_inner(par, script, error, fixscript_int(0), fixscript_int(0))) return 0;
   }
   else {
      for (i=0; i<par->script->imports.len; i++) {
         if (par->script->imports.data[i] == script) {
            par->tok.error = "duplicate import";
            return 0;
         }
      }

      dynarray_add(&par->script->imports, script);

      if (expect_symbol(par, ':', NULL)) {
         if (!expect_type(par, TOK_IDENT, "expected identifier")) return 0;

         s = string_dup(par->tok.value, par->tok.len);
         string_hash_set(&par->import_aliases, s, script);
      }
   }

   if (!expect_symbol(par, ';', "expected ';'")) return 0;
   return 1;
}


static int parse_constant_define_inner(Parser *par, int *inc_value)
{
   int local = 0, conflict;
   Value value;
   char *name = NULL, *s;
   Constant *constant, *prev_constant, *ref_constant = NULL;
   Script *script = NULL;

   if (expect_symbol(par, '@', NULL)) {
      local = 1;
   }

   if (!expect_type(par, TOK_IDENT, "expected identifier")) goto error;

   name = string_dup(par->tok.value, par->tok.len);

   if (inc_value) {
      if (expect_symbol(par, '=', NULL)) {
         if (expect_type(par, TOK_IDENT, NULL)) {
            s = string_dup(par->tok.value, par->tok.len);
            script = NULL;

            if (expect_symbol(par, ':', NULL)) {
               script = string_hash_get(&par->import_aliases, s);

               if (script) {
                  free(s);

                  if (!expect_type(par, TOK_IDENT, "expected identifier")) return 0;
                  s = string_dup(par->tok.value, par->tok.len);
               }
               else {
                  undo_token(&par->tok);
               }
            }

            constant = find_constant(script? script : par->script, s, script != NULL, &conflict, &script);
            free(s);
            if (conflict) {
               par->tok.error = "declaration of constant in multiple imports";
               return 0;
            }

            if (!constant) {
               par->tok.error = "unknown constant name";
               return 0;
            }

            if (!fixscript_is_int(constant->value)) {
               if (!par->tok.error) {
                  par->tok.error = "expected integer constant";
               }
               goto error;
            }

            value = constant->value;
            *inc_value = value.value;
            if (constant != &zero_const && constant != &one_const) {
               ref_constant = constant;
            }
         }
         else {
            if (!parse_constant(par, &value, 1)) {
               if (!par->tok.error) {
                  par->tok.error = "expected integer constant";
               }
               goto error;
            }
            *inc_value = value.value;
         }
      }
      else {
         if (*inc_value == INT_MAX) {
            par->tok.error = "integer overflow in autoincrement constant";
            goto error;
         }
         value = fixscript_int(++(*inc_value));
      }
   }
   else {
      if (!expect_symbol(par, '=', "expected '='")) goto error;

      if (expect_type(par, TOK_IDENT, NULL)) {
         s = string_dup(par->tok.value, par->tok.len);
         script = NULL;

         if (expect_symbol(par, ':', NULL)) {
            script = string_hash_get(&par->import_aliases, s);

            if (script) {
               free(s);

               if (!expect_type(par, TOK_IDENT, "expected identifier")) return 0;
               s = string_dup(par->tok.value, par->tok.len);
            }
            else {
               undo_token(&par->tok);
            }
         }

         constant = find_constant(script? script : par->script, s, script != NULL, &conflict, &script);
         free(s);
         if (conflict) {
            par->tok.error = "declaration of constant in multiple imports";
            return 0;
         }

         if (!constant) {
            par->tok.error = "unknown constant name";
            return 0;
         }

         value = constant->value;
         if (constant != &zero_const && constant != &one_const) {
            ref_constant = constant;
         }
      }
      else {
         if (!parse_constant(par, &value, 0)) {
            if (!par->tok.error) {
               par->tok.error = "expected integer, float or string constant";
            }
            goto error;
         }
      }
   }

   constant = malloc(sizeof(Constant));
   constant->value = value;
   constant->local = local;
   constant->ref_script = script;
   constant->ref_constant = ref_constant;
   constant->idx = par->script->constants.len;

   prev_constant = string_hash_set(&par->script->constants, name, constant);
   name = NULL;

   if (prev_constant) {
      free(prev_constant);
      par->tok.error = "duplicate constant";
      goto error;
   }

   return 1;

error:
   free(name);
   return 0;
}


static int parse_constant_define(Parser *par)
{
   int inc_value;

   if (expect_symbol(par, '{', NULL)) {
      inc_value = -1;
      do {
         if (!parse_constant_define_inner(par, &inc_value)) return 0;
      }
      while (expect_symbol(par, ',', NULL));

      if (!expect_symbol(par, '}', "expected ',' or '}'")) return 0;
   }
   else {
      if (!parse_constant_define_inner(par, NULL)) return 0;
   }

   if (!expect_symbol(par, ';', "expected ';'")) return 0;
   return 1;
}


static int expand_locals(Heap *heap)
{
   int *new_data;
   char *new_flags;
   int new_cap;

   if (heap->locals_cap >= (1<<28)) {
      return 0;
   }
   
   new_cap = heap->locals_cap << 1;
   
   new_data = realloc(heap->locals_data, new_cap * sizeof(int));
   if (!new_data) {
      return 0;
   }
   heap->locals_data = new_data;

   new_flags = realloc(heap->locals_flags, new_cap * sizeof(char));
   if (!new_flags) {
      return 0;
   }
   heap->locals_flags = new_flags;

   heap->total_size += (int64_t)(new_cap - heap->locals_cap) * sizeof(char);
   heap->total_size += (int64_t)(new_cap - heap->locals_cap) * sizeof(int);

   heap->locals_cap = new_cap;
   return 1;
}


static int parse_local_var(Parser *par)
{
   char *name;
   int idx, local;

   do {
      local = 0;
      if (expect_symbol(par, '@', NULL)) {
         local = 1;
      }

      if (!expect_type(par, TOK_IDENT, "expected identifier")) return 0;

      name = string_dup(par->tok.value, par->tok.len);

      idx = 0;
      if (par->old_script) {
         idx = (intptr_t)string_hash_get(&par->old_script->locals, name);
         if (idx < 0) idx = -idx;
      }

      if (idx == 0) {
         if (par->heap->locals_len == par->heap->locals_cap) {
            if (!expand_locals(par->heap)) {
               par->tok.error = "internal error: locals index assign";
               return 0;
            }
         }
         idx = par->heap->locals_len++;
         par->heap->locals_data[idx] = 0;
         par->heap->locals_flags[idx] = 0;
      }
      if (local) {
         idx = -idx;
      }

      if (string_hash_set(&par->script->locals, name, (void *)(intptr_t)idx)) {
         par->tok.error = "duplicate local variable";
         return 0;
      }
   }
   while (expect_symbol(par, ',', NULL));

   if (!expect_symbol(par, ';', "expected ';'")) return 0;
   return 1;
}


static Function *find_function(Script *script, const char *name, int used_import_alias, int *conflict)
{
   Function *func, *found_func = NULL;
   Script *s;
   int i;
   
   *conflict = 0;

   func = string_hash_get(&script->functions, name);
   if (func) {
      if (used_import_alias && func->local) {
         return NULL;
      }
      return func;
   }

   if (!used_import_alias) {
      for (i=0; i<script->imports.len; i++) {
         s = script->imports.data[i];
         func = string_hash_get(&s->functions, name);
         if (func && !func->local) {
            if (found_func) {
               *conflict = 1;
               return NULL;
            }
            found_func = func;
         }
      }
   }

   return found_func;
}


static Constant *find_constant(Script *script, const char *name, int used_import_alias, int *conflict, Script **script_out)
{
   Constant *constant, *found_constant = NULL;
   Script *s;
   int i;
   
   *conflict = 0;

   constant = string_hash_get(&script->constants, name);
   if (constant) {
      if (used_import_alias && constant->local) {
         return NULL;
      }
      if (script_out) {
         *script_out = script;
      }
      return constant;
   }

   if (!used_import_alias) {
      for (i=0; i<script->imports.len; i++) {
         s = script->imports.data[i];
         constant = string_hash_get(&s->constants, name);
         if (constant && !constant->local) {
            if (found_constant) {
               *conflict = 1;
               return NULL;
            }
            if (script_out) {
               *script_out = s;
            }
            found_constant = constant;
         }
      }
   }
   
   if (found_constant) {
      return found_constant;
   }

   if (script_out) {
      *script_out = script;
   }

   if (!strcmp(name, "null") || !strcmp(name, "false")) {
      return (Constant *)&zero_const;
   }
   if (!strcmp(name, "true")) {
      return (Constant *)&one_const;
   }

   return NULL;
}


static int find_local_var(Script *script, const char *name, int used_import_alias, int *conflict)
{
   Script *s;
   int i, local_var, found_local_var = 0;
   
   *conflict = 0;

   local_var = (intptr_t)string_hash_get(&script->locals, name);
   if (local_var) {
      if (used_import_alias && local_var < 0) {
         return 0;
      }
      return local_var < 0? -local_var : local_var;
   }

   if (!used_import_alias) {
      for (i=0; i<script->imports.len; i++) {
         s = script->imports.data[i];
         local_var = (intptr_t)string_hash_get(&s->locals, name);
         if (local_var > 0) {
            if (found_local_var) {
               *conflict = 1;
               return 0;
            }
            found_local_var = local_var;
         }
      }
   }

   return found_local_var;
}


static int parse_variable_or_function(Parser *par, Script *script, const char *name, int weak_call)
{
   Constant *constant;
   Function *func = NULL;
   NativeFunction *nfunc;
   char *s, *func_name = NULL;
   int var_stack_pos, local_var, num, value;
   int intrinsic_type, num_args, needs_line_info, is_int_conv, conflict, has_float_shorthand;

   if (!weak_call) {
      if (!script) {
         var_stack_pos = (intptr_t)string_hash_get(&par->variables, name);
         if (var_stack_pos) {
            buf_append_load(par, var_stack_pos - par->stack_pos);
            inc_stack(par, 1);
            return 1;
         }
      }

      local_var = find_local_var(script? script : par->script, name, script != NULL, &conflict);
      if (conflict) {
         par->tok.error = "declaration of local variable in multiple imports";
         return 0;
      }
      if (local_var) {
         buf_append_load_local_var(par, local_var);
         inc_stack(par, 1);
         return 1;
      }

      constant = find_constant(script? script : par->script, name, script != NULL, &conflict, NULL);
      if (conflict) {
         par->tok.error = "declaration of constant in multiple imports";
         return 0;
      }
      if (constant) {
         if (fixscript_is_int(constant->value)) {
            buf_append_const(par, constant->value.value);
         }
         else if (fixscript_is_float(constant->value)) {
            buf_append_const_float(par, constant->value.value);
         }
         else {
            buf_append_const(par, constant->value.value);
            buf_append(par, BC_CONST_STRING);
         }
         inc_stack(par, 1);
         return 1;
      }
   }
   else {
      if (!expect_symbol(par, '(', "expected '('")) return 0;
      undo_token(&par->tok);
   }

   if (!weak_call) {
      intrinsic_type = -1;
      needs_line_info = 0;
      is_int_conv = 0;
      has_float_shorthand = 0;

      switch (strlen(name)) {
         case 2:
            if (!strcmp(name, "ln")) { intrinsic_type = BC_EXT_LN; num_args = 1; break; }
            break;

         case 3:
            if (!strcmp(name, "min")) { intrinsic_type = BC_EXT_MIN; num_args = 2; break; }
            if (!strcmp(name, "max")) { intrinsic_type = BC_EXT_MAX; num_args = 2; break; }
            if (!strcmp(name, "abs")) { intrinsic_type = BC_EXT_ABS; num_args = 1; needs_line_info = 1; break; }
            if (!strcmp(name, "int")) { intrinsic_type = BC_EXT_INT; num_args = 1; break; }
            if (!strcmp(name, "pow")) { intrinsic_type = BC_EXT_POW; num_args = 2; break; }
            if (!strcmp(name, "exp")) { intrinsic_type = BC_EXT_EXP; num_args = 1; break; }
            if (!strcmp(name, "sin")) { intrinsic_type = BC_EXT_SIN; num_args = 1; break; }
            if (!strcmp(name, "cos")) { intrinsic_type = BC_EXT_COS; num_args = 1; break; }
            if (!strcmp(name, "tan")) { intrinsic_type = BC_EXT_TAN; num_args = 1; break; }
            break;

         case 4:
            if (!strcmp(name, "fmin")) { intrinsic_type = BC_EXT_FMIN; num_args = 2; break; }
            if (!strcmp(name, "fmax")) { intrinsic_type = BC_EXT_FMAX; num_args = 2; break; }
            if (!strcmp(name, "fabs")) { intrinsic_type = BC_EXT_FABS; num_args = 1; break; }
            if (!strcmp(name, "ceil")) { intrinsic_type = BC_EXT_CEIL; num_args = 1; break; }
            if (!strcmp(name, "sqrt")) { intrinsic_type = BC_EXT_SQRT; num_args = 1; break; }
            if (!strcmp(name, "cbrt")) { intrinsic_type = BC_EXT_CBRT; num_args = 1; break; }
            if (!strcmp(name, "log2")) { intrinsic_type = BC_EXT_LOG2; num_args = 1; break; }
            if (!strcmp(name, "asin")) { intrinsic_type = BC_EXT_ASIN; num_args = 1; break; }
            if (!strcmp(name, "acos")) { intrinsic_type = BC_EXT_ACOS; num_args = 1; break; }
            if (!strcmp(name, "atan")) { intrinsic_type = BC_EXT_ATAN; num_args = 1; break; }
            break;

         case 5:
            if (!strcmp(name, "clamp")) { intrinsic_type = BC_EXT_CLAMP; num_args = 3; break; }
            if (!strcmp(name, "add32")) { intrinsic_type = 0x100 + BC_ADD_MOD; num_args = 2; break; }
            if (!strcmp(name, "sub32")) { intrinsic_type = 0x100 + BC_SUB_MOD; num_args = 2; break; }
            if (!strcmp(name, "mul32")) { intrinsic_type = 0x100 + BC_MUL_MOD; num_args = 2; break; }
            if (!strcmp(name, "float")) { intrinsic_type = BC_EXT_FLOAT; num_args = 1; break; }
            if (!strcmp(name, "floor")) { intrinsic_type = BC_EXT_FLOOR; num_args = 1; break; }
            if (!strcmp(name, "iceil")) { intrinsic_type = BC_EXT_CEIL; num_args = 1; is_int_conv = 1; break; }
            if (!strcmp(name, "round")) { intrinsic_type = BC_EXT_ROUND; num_args = 1; break; }
            if (!strcmp(name, "log10")) { intrinsic_type = BC_EXT_LOG10; num_args = 1; break; }
            if (!strcmp(name, "atan2")) { intrinsic_type = BC_EXT_ATAN2; num_args = 2; break; }
            if (!strcmp(name, "fconv")) { intrinsic_type = BC_EXT_DBL_CONV_DOWN; num_args = 2; break; }
            break;

         case 6:
            if (!strcmp(name, "fclamp")) { intrinsic_type = BC_EXT_FCLAMP; num_args = 3; break; }
            if (!strcmp(name, "length")) { intrinsic_type = 0x100 + BC_LENGTH; num_args = 1; needs_line_info = 1; break; }
            if (!strcmp(name, "ifloor")) { intrinsic_type = BC_EXT_FLOOR; num_args = 1; is_int_conv = 1; break; }
            if (!strcmp(name, "iround")) { intrinsic_type = BC_EXT_ROUND; num_args = 1; is_int_conv = 1; break; }
            if (!strcmp(name, "is_int")) { intrinsic_type = BC_EXT_IS_INT; num_args = 1; break; }
            break;

         case 7:
            if (!strcmp(name, "is_hash")) { intrinsic_type = BC_EXT_IS_HASH; num_args = 1; break; }
            if (!strcmp(name, "fcmp_lt")) { intrinsic_type = BC_EXT_DBL_CMP_LT; num_args = 4; has_float_shorthand = 1; break; }
            if (!strcmp(name, "fcmp_le")) { intrinsic_type = BC_EXT_DBL_CMP_LE; num_args = 4; has_float_shorthand = 1; break; }
            if (!strcmp(name, "fcmp_gt")) { intrinsic_type = BC_EXT_DBL_CMP_GT; num_args = 4; has_float_shorthand = 1; break; }
            if (!strcmp(name, "fcmp_ge")) { intrinsic_type = BC_EXT_DBL_CMP_GE; num_args = 4; has_float_shorthand = 1; break; }
            if (!strcmp(name, "fcmp_eq")) { intrinsic_type = BC_EXT_DBL_CMP_EQ; num_args = 4; has_float_shorthand = 1; break; }
            if (!strcmp(name, "fcmp_ne")) { intrinsic_type = BC_EXT_DBL_CMP_NE; num_args = 4; has_float_shorthand = 1; break; }
            break;

         case 8:
            if (!strcmp(name, "is_float")) { intrinsic_type = BC_EXT_IS_FLOAT; num_args = 1; break; }
            if (!strcmp(name, "is_array")) { intrinsic_type = BC_EXT_IS_ARRAY; num_args = 1; break; }
            if (!strcmp(name, "is_const")) { intrinsic_type = BC_EXT_IS_CONST; num_args = 1; break; }
            break;

         case 9:
            if (!strcmp(name, "is_string")) { intrinsic_type = BC_EXT_IS_STRING; num_args = 1; break; }
            if (!strcmp(name, "is_shared")) { intrinsic_type = BC_EXT_IS_SHARED; num_args = 1; break; }
            if (!strcmp(name, "is_handle")) { intrinsic_type = BC_EXT_IS_HANDLE; num_args = 1; break; }
            break;

         case 10:
            if (!strcmp(name, "is_funcref")) { intrinsic_type = BC_EXT_IS_FUNCREF; num_args = 1; break; }
            if (!strcmp(name, "is_weakref")) { intrinsic_type = BC_EXT_IS_WEAKREF; num_args = 1; break; }
            break;
      }

      if (intrinsic_type != -1) {
         if (!expect_symbol(par, '(', "expected '('")) return 0;

         num = 0;
         if (!expect_symbol(par, ')', NULL)) {
            do {
               if (!parse_expression(par)) return 0;
               num++;
            }
            while (expect_symbol(par, ',', NULL));

            if (!expect_symbol(par, ')', "expected ')' or ','")) return 0;
         }

         if ((intrinsic_type == 0x100 + BC_ADD_MOD || intrinsic_type == 0x100 + BC_SUB_MOD) && num == 3) {
            buf_append(par, BC_EXTENDED);
            buf_append(par, intrinsic_type == 0x100 + BC_ADD_MOD? BC_EXT_ADD32 : BC_EXT_SUB32);
            buf_append(par, BC_POP);
            par->stack_pos -= num - 1;
            return 1;
         }

         if (has_float_shorthand) {
            if (num != num_args && num != num_args-1) {
               par->tok.error = "improper number of function parameters";
               return 0;
            }
         }
         else if (num != num_args) {
            par->tok.error = "improper number of function parameters";
            return 0;
         }

         if (intrinsic_type >= 0x100) {
            buf_append(par, intrinsic_type - 0x100);
         }
         else {
            if (has_float_shorthand && num == num_args-1) {
               buf_append(par, BC_EXTENDED);
               buf_append(par, BC_EXT_DBL_CONV_UP);
               inc_stack(par, 1);
               par->stack_pos--;
            }
            buf_append(par, BC_EXTENDED);
            buf_append(par, intrinsic_type);
            if (is_int_conv) {
               buf_append(par, BC_EXTENDED);
               buf_append(par, BC_EXT_INT);
            }
            par->last_buf_pos--;
         }
         if (needs_line_info) {
            add_line_info(par);
         }
         par->stack_pos -= num - 1;
         return 1;
      }
   }
   
   if (expect_symbol(par, '(', NULL)) {
      buf_append_const(par, 0);
      inc_stack(par, 1);

      num = 0;
      if (!expect_symbol(par, ')', NULL)) {
         do {
            if (!parse_expression(par)) return 0;
            num++;
         }
         while (expect_symbol(par, ',', NULL));

         if (!expect_symbol(par, ')', "expected ')' or ','")) return 0;
      }

      s = string_format("%s#%d", name, num);
      nfunc = string_hash_get(&par->heap->native_functions_hash, s);
      if (!weak_call) {
         func = find_function(script? script : par->script, s, script != NULL, &conflict);
         if (func) nfunc = NULL;
         if (conflict) {
            free(s);
            par->tok.error = "declaration of function in multiple imports";
            return 0;
         }
      }

      func_name = s;

      if (weak_call && !nfunc) {
         #ifdef FIXEMBED_TOKEN_DUMP
            if (par->heap->token_dump_mode) {
               fixembed_native_function_used(func_name);
            }
         #endif
         free(func_name);

         if (par->use_fast_error) {
            s = string_format("native function %s#%d is not present (%s:%d)", name, num, par->fname, par->tok.line);
         }
         else {
            s = string_format("native function %s#%d is not present", name, num);
         }
         value = get_const_string(par, s);
         free(s);
         if (!value) return 0;

         buf_append_pop(par, 1+num);
         buf_append_const(par, 0);
         par->stack_pos -= num;
         if (!par->use_fast_error) {
            buf_append_const(par, 0);
            inc_stack(par, 1);
         }
         buf_append_const(par, value);
         inc_stack(par, 1);
         buf_append(par, BC_CONST_STRING);
         if (!par->use_fast_error) {
            nfunc = string_hash_get(&par->heap->native_functions_hash, "error#1");
            if (!nfunc) return 0;
            buf_append_const(par, nfunc->id);
            inc_stack(par, 1);
            buf_append(par, BC_CALL_NATIVE);
            add_line_info(par);
            par->stack_pos -= 2;
         }
         buf_append(par, BC_RETURN2);
         par->stack_pos--;
         return 1;
      }

      if (par->heap->time_limit != 0) {
         buf_append(par, BC_EXTENDED);
         buf_append(par, BC_EXT_CHECK_TIME_LIMIT);
         add_line_info(par);
      }

      if (nfunc) {
         #ifdef FIXEMBED_TOKEN_DUMP
            if (par->heap->token_dump_mode) {
               fixembed_native_function_used(func_name);
            }
         #endif

         if (weak_call) {
            free(func_name);
            buf_append_const(par, nfunc->id);
         }
         else {
            if (par->long_func_refs) {
               buf_append(par, BC_CONST_I32);
               dynarray_add(&par->func_refs, func_name);
               dynarray_add(&par->func_refs, (void *)(intptr_t)par->buf_len);
               dynarray_add(&par->func_refs, (void *)(intptr_t)par->tok.line);
               buf_append_int(par, nfunc->id);
            }
            else {
               buf_append_const(par, nfunc->id);
               dynarray_add(&par->func_refs, func_name);
               dynarray_add(&par->func_refs, (void *)0);
               dynarray_add(&par->func_refs, (void *)0);
            }
         }
         buf_append(par, BC_CALL_NATIVE);
      }
      else {
         if (func) {
            if (!script && func->script != par->script) {
               if (par->long_func_refs) {
                  buf_append(par, BC_CONST_I32);
                  dynarray_add(&par->func_refs, func_name);
                  dynarray_add(&par->func_refs, (void *)(intptr_t)par->buf_len);
                  dynarray_add(&par->func_refs, (void *)(intptr_t)par->tok.line);
                  buf_append_int(par, func->id);
               }
               else {
                  buf_append_const(par, func->id);
                  dynarray_add(&par->func_refs, func_name);
                  dynarray_add(&par->func_refs, (void *)0);
                  dynarray_add(&par->func_refs, (void *)0);
               }
            }
            else {
               buf_append_const(par, func->id);
               free(func_name);
            }
         }
         else {
            buf_append(par, BC_CONST_I32);
            dynarray_add(&par->func_refs, func_name);
            dynarray_add(&par->func_refs, (void *)(intptr_t)(par->buf_len | (1<<31)));
            dynarray_add(&par->func_refs, (void *)(intptr_t)par->tok.line);
            buf_append_int(par, 0);
         }
         buf_append(par, BC_CALL_DIRECT);
      }
      inc_stack(par, 1);
      add_line_info(par);
      par->stack_pos -= num+1;
      return 1;
   }

   par->tok.error = "undefined variable name";
   return 0;
}


static int parse_extended_float_operator(Parser *par)
{
   int type = -1;

   if (!parse_primary_expression(par)) return 0;

   for (;;) {
      if (type != -1 && expect_symbol(par, '}', NULL)) {
         break;
      }
      
      if (expect_symbol(par, '+', NULL) || expect_symbol(par, '-', NULL)) {
         if (type != -1 && type != BC_FLOAT_ADD && type != BC_FLOAT_SUB) {
            par->tok.error = "can't mix additive and multiplicative operations in a single extended operator";
            return 0;
         }
         type = par->tok.value[0] == '+'? BC_FLOAT_ADD : BC_FLOAT_SUB;
         if (!parse_primary_expression(par)) return 0;
         buf_append(par, type);
         par->stack_pos--;
         continue;
      }

      if (expect_symbol(par, '*', NULL) || expect_symbol(par, '/', NULL)) {
         if (type != -1 && type != BC_FLOAT_MUL && type != BC_FLOAT_DIV) {
            par->tok.error = "can't mix additive and multiplicative operations in a single extended operator";
            return 0;
         }
         type = par->tok.value[0] == '*'? BC_FLOAT_MUL : BC_FLOAT_DIV;
         if (!parse_primary_expression(par)) return 0;
         buf_append(par, type);
         par->stack_pos--;
         continue;
      }

      if (type == -1 && expect_symbol(par, '<', NULL)) {
         if (!parse_primary_expression(par)) return 0;
         buf_append(par, BC_FLOAT_LT);
         par->stack_pos--;
         if (!expect_symbol(par, '}', NULL)) return 0;
         return 1;
      }

      if (type == -1 && expect_symbol2(par, '<', '=', NULL)) {
         if (!parse_primary_expression(par)) return 0;
         buf_append(par, BC_FLOAT_LE);
         par->stack_pos--;
         if (!expect_symbol(par, '}', NULL)) return 0;
         return 1;
      }

      if (type == -1 && expect_symbol(par, '>', NULL)) {
         if (!parse_primary_expression(par)) return 0;
         buf_append(par, BC_FLOAT_GT);
         par->stack_pos--;
         if (!expect_symbol(par, '}', NULL)) return 0;
         return 1;
      }

      if (type == -1 && expect_symbol2(par, '>', '=', NULL)) {
         if (!parse_primary_expression(par)) return 0;
         buf_append(par, BC_FLOAT_GE);
         par->stack_pos--;
         if (!expect_symbol(par, '}', NULL)) return 0;
         return 1;
      }

      if (type == -1 && expect_symbol2(par, '=', '=', NULL)) {
         if (!parse_primary_expression(par)) return 0;
         buf_append(par, BC_FLOAT_EQ);
         par->stack_pos--;
         if (!expect_symbol(par, '}', NULL)) return 0;
         return 1;
      }

      if (type == -1 && expect_symbol2(par, '!', '=', NULL)) {
         if (!parse_primary_expression(par)) return 0;
         buf_append(par, BC_FLOAT_NE);
         par->stack_pos--;
         if (!expect_symbol(par, '}', NULL)) return 0;
         return 1;
      }

      if (type == BC_FLOAT_ADD || type == BC_FLOAT_SUB) {
         par->tok.error = "expected '+' or '-'";
      }
      else if (type == BC_FLOAT_MUL || type == BC_FLOAT_DIV) {
         par->tok.error = "expected '*' or '/'";
      }
      else {
         par->tok.error = "expected '+', '-', '*', '/', '<', '<=', '>', '>=', '==' or '!='";
      }
      return 0;
   }

   return 1;
}


static int parse_extended_operator(Parser *par)
{
   Tokenizer save_tok;
   int num = 0, type = -1, level = 0, first, cur_immediate_stack, ret;

   save_tok = par->tok;
   if (expect_symbol(par, '}', NULL)) {
      type = ET_HASH;
   }
   else if (expect_symbol(par, '=', NULL)) {
      type = ET_BLOCK;
   }
   else {
      first = 1;
      while (next_token(&par->tok)) {
         switch (par->tok.type) {
            case '(':
            case '{':
            case '[':
               level++;
               break;

            case ')':
            case '}':
            case ']':
               if (--level < 0) goto end;
               if (level == 0 && par->tok.value[0] == '}') {
                  if (expect_symbol(par, '=', NULL)) {
                     type = ET_BLOCK;
                     goto end;
                  }
               }
               break;

            case '+':
            case '-':
               if (first) {
                  break;
               }
               // fallthrough

            case '*':
            case '/':
            case '<':
            case '>':
               if (level == 0 && type == -1) {
                  type = ET_FLOAT;
               }
               break;

            case '?':
               if (level == 0 && type == -1) {
                  type = ET_STRING;
               }
               break;

            case ':':
               if (level == 0 && type == -1) {
                  type = ET_HASH;
               }
               break;

            case ',':
               if (level == 0 && (type == -1 || type == ET_FLOAT)) {
                  type = ET_STRING;
               }
               break;

            case ';':
               if (level == 0 && expect_symbol(par, '=', NULL)) {
                  type = ET_BLOCK;
                  goto end;
               }
               break;

            case SYM2('<', '='):
            case SYM2('>', '='):
            case SYM2('=', '='):
            case SYM2('!', '='):
               if (level == 0 && type == -1) {
                  type = ET_FLOAT;
               }
               break;
         }
         first = 0;
      }
   }
end:
   par->tok = save_tok;
   if (type == -1) {
      type = ET_STRING;
   }

   if (type == ET_FLOAT) {
      return parse_extended_float_operator(par);
   }

   if (type == ET_BLOCK) {
      return parse_block(par, BT_EXPR);
   }

   if (type == ET_HASH) {
      for (;;) {
         if (expect_symbol(par, '}', NULL)) {
            if (num < par->max_immediate_stack) {
               buf_append_const(par, num);
               inc_stack(par, 1);
               buf_append(par, BC_CREATE_HASH);
               add_line_info(par);
               par->stack_pos -= num*2;
            }
            break;
         }

         if (num > par->max_immediate_stack) {
            buf_append_load(par, -1);
            inc_stack(par, 1);
         }

         if (num > 0) {
            if (!expect_symbol(par, ',', "expected ','")) return 0;
         }

         cur_immediate_stack = par->max_immediate_stack;
         par->max_immediate_stack = MAX(1, par->max_immediate_stack >> 1);
         ret = parse_expression(par) && expect_symbol(par, ':', "expected ':'") && parse_expression(par);
         par->max_immediate_stack = cur_immediate_stack;
         if (!ret) return 0;

         if (num < par->max_immediate_stack) {
            num++;
         }

         if (num == par->max_immediate_stack) {
            buf_append_const(par, num);
            inc_stack(par, 1);
            buf_append(par, BC_CREATE_HASH);
            add_line_info(par);
            par->stack_pos -= num*2;
            num++;
         }
         else if (num > par->max_immediate_stack) {
            buf_append(par, BC_HASH_SET);
            add_line_info(par);
            par->stack_pos -= 3;
         }
      }
   }
   else if (type == ET_STRING) {
      first = 1;
      for (;;) {
         if (expect_symbol(par, '}', NULL)) {
            if (first || num > 1) {
               buf_append_const(par, num);
               inc_stack(par, 1);
               buf_append(par, BC_STRING_CONCAT);
               add_line_info(par);
               par->stack_pos -= num;
            }
            break;
         }

         if (num > 0) {
            if (!expect_symbol(par, ',', "expected ','")) return 0;
         }

         cur_immediate_stack = par->max_immediate_stack;
         par->max_immediate_stack = MAX(2, par->max_immediate_stack >> 1);
         ret = parse_expression(par);
         par->max_immediate_stack = cur_immediate_stack;
         if (!ret) return 0;

         num++;
         if (num == par->max_immediate_stack) {
            buf_append_const(par, num);
            inc_stack(par, 1);
            buf_append(par, BC_STRING_CONCAT);
            add_line_info(par);
            par->stack_pos -= num;
            num = 1;
            first = 0;
         }
      }
   }
   else {
      par->tok.error = "internal error: unhandled type of extended operator";
      return 0;
   }

   return 1;
}


static int parse_constant(Parser *par, Value *value, int int_only)
{
   char *s, *end;
   long int val;
   unsigned long int uval;
   float fval;
   int ret, valid, sign=0;

   if (next_token(&par->tok)) {
      switch (par->tok.type) {
         case '+': sign = +1; break;
         case '-': sign = -1; break;
         default: undo_token(&par->tok); break;
      }
   }

   if (expect_type(par, TOK_NUMBER, NULL)) {
      if (sign < 0) {
         s = string_dup_with_prefix(par->tok.value, par->tok.len, "-", 1);
      }
      else {
         s = string_dup(par->tok.value, par->tok.len);
      }
      valid = 1;
      errno = 0;
      val = strtol(s, &end, 10);
      if (errno != 0 || *end != '\0') {
         valid = 0;
      }
      else if (sizeof(long int) > sizeof(int)) {
         if (val < INT_MIN || val > INT_MAX) {
            valid = 0;
         }
      }
      free(s);

      if (!valid) {
         par->tok.error = "integer constant out of range";
         return 0;
      }
      *value = fixscript_int(val);
      return 1;
   }

   if (expect_type(par, TOK_HEX_NUMBER, NULL)) {
      s = string_dup(par->tok.value, par->tok.len);
      valid = 1;
      errno = 0;
      uval = strtoul(s, &end, 16);
      if (errno != 0 || *end != '\0') {
         valid = 0;
      }
      else if (sizeof(unsigned long int) > sizeof(unsigned int)) {
         if (uval > UINT_MAX) {
            valid = 0;
         }
      }
      free(s);

      if (sign < 0 && valid) {
         if (uval == 0x80000000) {
            valid = 0;
         }
         else {
            uval = -(int)uval;
         }
      }

      if (!valid) {
         par->tok.error = "hexadecimal constant out of range";
         return 0;
      }
      *value = fixscript_int(uval);
      return 1;
   }

   if (sign == 0 && expect_type(par, TOK_CHAR, NULL)) {
      if (!get_token_char(&par->tok, value)) {
         par->tok.error = "internal error while parsing char";
         return 0;
      }
      return 1;
   }
   
   if (int_only) {
      return 0;
   }

   if (expect_type(par, TOK_FLOAT_NUMBER, NULL)) {
      if (sign < 0) {
         s = string_dup_with_prefix(par->tok.value, par->tok.len, "-", 1);
      }
      else {
         s = string_dup(par->tok.value, par->tok.len);
      }
      fval = (float)strtod(s, &end);
      valid = (*end == '\0');
      free(s);

      if (!valid) {
         par->tok.error = "invalid float constant";
         return 0;
      }
      *value = fixscript_float(fval);
      return 1;
   }

   if (sign == 0 && expect_type(par, TOK_STRING, NULL)) {
      s = get_token_string(&par->tok);
      ret = get_const_string(par, s);
      free(s);
      if (!ret) return 0;
      
      *value = (Value) { ret, 1 };
      return 1;
   }

   return 0;
}


static int parse_primary_prefix_expression(Parser *par)
{
   char *s;
   int num, ret, weak_call, conflict, cur_immediate_stack;
   Function *func;
   Script *script;
   Value value;
   Tokenizer save_tok;

   if (parse_constant(par, &value, 0)) {
      if (fixscript_is_int(value)) {
         buf_append_const(par, value.value);
      }
      else if (fixscript_is_float(value)) {
         buf_append_const_float(par, value.value);
      }
      else {
         buf_append_const(par, value.value);
         buf_append(par, BC_CONST_STRING);
      }
      inc_stack(par, 1);
      return 1;
   }
   else if (par->tok.error) {
      return 0;
   }

   if (expect_symbol(par, '(', NULL)) {
      if (!parse_expression(par)) return 0;
      if (!expect_symbol(par, ')', "expected ')'")) return 0;
      return 1;
   }

   if (expect_type(par, TOK_IDENT, NULL) || expect_symbol(par, '@', NULL)) {
      weak_call = (par->tok.type == '@');
      if (weak_call) {
         if (!expect_type(par, TOK_IDENT, "expected identifier")) return 0;
      }

      s = string_dup(par->tok.value, par->tok.len);
      script = NULL;

      save_tok = par->tok;
      if (expect_symbol(par, ':', NULL) && (expect_type(par, TOK_IDENT, NULL) || expect_type(par, TOK_FUNC_REF, NULL))) {
         script = string_hash_get(&par->import_aliases, s);
      }
      par->tok = save_tok;

      if (script) {
         free(s);

         if (!expect_symbol(par, ':', "internal error when parsing import alias")) return 0;

         if (expect_type(par, TOK_FUNC_REF, NULL)) {
            s = string_dup(par->tok.value, par->tok.len);
            func = find_function(script, s, 1, &conflict);
            free(s);
            if (conflict) {
               par->tok.error = "declaration of function in multiple imports";
               return 0;
            }

            if (!func) {
               par->tok.error = "undefined function name";
               return 0;
            }

            buf_append_const_float(par, FUNC_REF_OFFSET + func->id);
            inc_stack(par, 1);
            return 1;
         }

         if (!expect_type(par, TOK_IDENT, "expected identifier")) return 0;
         s = string_dup(par->tok.value, par->tok.len);
      }

      ret = parse_variable_or_function(par, script, s, weak_call);
      free(s);
      return ret;
   }

   if (expect_type(par, TOK_FUNC_REF, NULL)) {
      s = string_dup(par->tok.value, par->tok.len);
      func = find_function(par->script, s, 0, &conflict);
      if (conflict) {
         free(s);
         par->tok.error = "declaration of function in multiple imports";
         return 0;
      }

      if (func) {
         if (func->script != par->script) {
            if (par->long_func_refs) {
               buf_append(par, BC_CONST_F32);
               dynarray_add(&par->func_refs, s);
               dynarray_add(&par->func_refs, (void *)(intptr_t)par->buf_len);
               dynarray_add(&par->func_refs, (void *)(intptr_t)(par->tok.line | (1<<31)));
               buf_append_int(par, FUNC_REF_OFFSET + func->id);
               par->last_buf_pos = par->buf_len - 5;
            }
            else {
               buf_append_const_float(par, FUNC_REF_OFFSET + func->id);
               dynarray_add(&par->func_refs, s);
               dynarray_add(&par->func_refs, (void *)0);
               dynarray_add(&par->func_refs, (void *)0);
            }
         }
         else {
            buf_append_const_float(par, FUNC_REF_OFFSET + func->id);
            free(s);
         }
      }
      else {
         buf_append(par, BC_CONST_F32);
         dynarray_add(&par->func_refs, s);
         dynarray_add(&par->func_refs, (void *)(intptr_t)(par->buf_len | (1<<31)));
         dynarray_add(&par->func_refs, (void *)(intptr_t)(par->tok.line | (1<<31)));
         buf_append_int(par, 0);
         par->last_buf_pos = par->buf_len - 5;
      }

      inc_stack(par, 1);
      return 1;
   }

   if (expect_symbol(par, '[', NULL)) {
      num = 0;
      for (;;) {
         if (expect_symbol(par, ']', NULL)) {
            if (num < par->max_immediate_stack) {
               buf_append_const(par, num);
               inc_stack(par, 1);
               buf_append(par, BC_CREATE_ARRAY);
               add_line_info(par);
               par->stack_pos -= num;
            }
            break;
         }

         if (num > par->max_immediate_stack) {
            buf_append_load(par, -1);
            inc_stack(par, 1);
         }

         if (num > 0) {
            if (!expect_symbol(par, ',', "expected ','")) return 0;
         }

         cur_immediate_stack = par->max_immediate_stack;
         par->max_immediate_stack = MAX(1, par->max_immediate_stack >> 1);
         ret = parse_expression(par);
         par->max_immediate_stack = cur_immediate_stack;
         if (!ret) return 0;

         if (num < par->max_immediate_stack) {
            num++;
         }
         
         if (num == par->max_immediate_stack) {
            buf_append_const(par, num);
            inc_stack(par, 1);
            buf_append(par, BC_CREATE_ARRAY);
            add_line_info(par);
            par->stack_pos -= num;
            num++;
         }
         else if (num > par->max_immediate_stack) {
            buf_append(par, BC_ARRAY_APPEND);
            add_line_info(par);
            par->stack_pos -= 2;
         }
      }
      return 1;
   }

   if (expect_symbol(par, '{', NULL)) {
      return parse_extended_operator(par);
   }

   if (!par->tok.error) {
      par->tok.error = "expected value";
   }
   return 0;
}


static int parse_primary_expression(Parser *par)
{
   Constant *constant;
   Script *script;
   Tokenizer save_tok;
   char *s;
   int num, conflict;

   if (!parse_primary_prefix_expression(par)) return 0;
   
   for (;;) {
      if (expect_symbol(par, '(', NULL)) {
         num = 0;
         if (!expect_symbol(par, ')', NULL)) {
            do {
               if (!parse_expression(par)) return 0;
               num++;
            }
            while (expect_symbol(par, ',', NULL));

            if (!expect_symbol(par, ')', "expected ')' or ','")) return 0;
         }

         if (par->heap->time_limit != 0) {
            buf_append(par, BC_EXTENDED);
            buf_append(par, BC_EXT_CHECK_TIME_LIMIT);
            add_line_info(par);
         }
         buf_append_const(par, num);
         inc_stack(par, 1);
         buf_append(par, BC_CALL_DYNAMIC);
         add_line_info(par);
         par->stack_pos -= num+1;
         continue;
      }

      if (expect_symbol(par, '[', NULL)) {
         save_tok = par->tok;
         if (!expect_symbol(par, ']', NULL)) {
            if (!parse_expression(par)) return 0;
            if (!expect_symbol(par, ']', "expected ']'")) return 0;

            buf_append(par, BC_ARRAY_GET);
            add_line_info(par);
            par->stack_pos--;
            continue;
         }
         par->tok = save_tok;
         undo_token(&par->tok);
      }

      if (expect_symbol2(par, '-', '>', NULL)) {
         if (!expect_type(par, TOK_IDENT, "expected named constant")) return 0;

         s = string_dup(par->tok.value, par->tok.len);
         script = NULL;

         if (expect_symbol(par, ':', NULL)) {
            script = string_hash_get(&par->import_aliases, s);

            if (script) {
               free(s);

               if (!expect_type(par, TOK_IDENT, "expected named constant")) return 0;
               s = string_dup(par->tok.value, par->tok.len);
            }
            else {
               undo_token(&par->tok);
            }
         }

         constant = find_constant(script? script : par->script, s, script != NULL, &conflict, NULL);
         free(s);
         if (conflict) {
            par->tok.error = "declaration of constant in multiple imports";
            return 0;
         }

         if (!constant) {
            par->tok.error = "unknown constant name";
            return 0;
         }

         if (!fixscript_is_int(constant->value)) {
            par->tok.error = "constant must be integer";
            return 0;
         }
         
         buf_append_const(par, constant->value.value);
         inc_stack(par, 1);

         buf_append(par, BC_ARRAY_GET);
         add_line_info(par);
         par->stack_pos--;
         continue;
      }

      if (expect_symbol(par, '{', NULL)) {
         if (!parse_expression(par)) return 0;
         if (!expect_symbol(par, '}', "expected '}'")) return 0;

         buf_append(par, BC_HASH_GET);
         add_line_info(par);
         par->stack_pos--;
         continue;
      }
      break;
   }

   return 1;
}


static int parse_unary_expression(Parser *par)
{
   Tokenizer save_tok;
   int inc, value, is_const;

   if (expect_symbol(par, '~', NULL)) {
      if (!parse_unary_expression(par)) return 0;
      buf_append(par, BC_BITNOT);
      return 1;
   }

   if (expect_symbol(par, '!', NULL)) {
      if (!parse_unary_expression(par)) return 0;
      buf_append(par, BC_LOGNOT);
      return 1;
   }

   if (expect_symbol(par, '+', NULL)) {
      return parse_unary_expression(par);
   }

   if (expect_symbol(par, '-', NULL)) {
      is_const = 0;
      save_tok = par->tok;
      if (next_token(&par->tok)) {
         if (par->tok.type == TOK_NUMBER || par->tok.type == TOK_HEX_NUMBER || par->tok.type == TOK_FLOAT_NUMBER) {
            is_const = 1;
         }
      }
      par->tok = save_tok;
      if (is_const) {
         undo_token(&par->tok);
      }
      else {
         buf_append_const(par, 0);
         inc_stack(par, 1);
         if (!parse_unary_expression(par)) return 0;
         buf_append(par, BC_SUB);
         add_line_info(par);
         par->stack_pos--;
         return 1;
      }
   }

   if (expect_symbol2(par, '+', '+', NULL) || expect_symbol2(par, '-', '-', NULL)) {
      inc = (par->tok.value[0] == '+');
      if (!parse_primary_expression(par)) return 0;

      if (buf_is_load(par, par->last_buf_pos, &value)) {
         if (value >= -128 && value < 0) {
            par->buf_len = par->last_buf_pos;
            buf_append(par, inc? BC_INC : BC_DEC);
            buf_append(par, value);
            add_line_info(par);
            buf_append_load(par, value);
         }
         else {
            inc_stack(par, 1);
            buf_append_const(par, 1);
            buf_append(par, inc? BC_ADD : BC_SUB);
            add_line_info(par);
            buf_append_load(par, -1);
            buf_append_store(par, value-2);
            par->stack_pos--;
         }
      }
      else if (buf_is_load_local_var(par, par->last_buf_pos, &value)) {
         inc_stack(par, 1);
         buf_append_const(par, 1);
         buf_append(par, inc? BC_ADD : BC_SUB);
         add_line_info(par);
         buf_append_load(par, -1);
         buf_append_store_local_var(par, value);
         par->stack_pos--;
      }
      else if (par->buf[par->last_buf_pos] == BC_ARRAY_GET) {
         remove_line_info(par);
         par->buf_len = par->last_buf_pos;
         inc_stack(par, 3);
         buf_append_load(par, -2);
         buf_append_load(par, -2);
         buf_append(par, BC_ARRAY_GET);
         add_line_info(par);
         buf_append_const(par, 1);
         buf_append(par, inc? BC_ADD : BC_SUB);
         add_line_info(par);
         buf_append(par, BC_ARRAY_SET);
         add_line_info(par);
         buf_append_load(par, 2);
         par->stack_pos -= 3;
      }
      else if (par->buf[par->last_buf_pos] == BC_HASH_GET) {
         remove_line_info(par);
         par->buf_len = par->last_buf_pos;
         inc_stack(par, 3);
         buf_append_load(par, -2);
         buf_append_load(par, -2);
         buf_append(par, BC_HASH_GET);
         add_line_info(par);
         buf_append_const(par, 1);
         buf_append(par, inc? BC_ADD : BC_SUB);
         add_line_info(par);
         buf_append(par, BC_HASH_SET);
         add_line_info(par);
         buf_append_load(par, 2);
         par->stack_pos -= 3;
      }
      else {
         par->tok.error = "invalid assignment destination";
         return 0;
      }
      return 1;
   }
   
   if (!parse_primary_expression(par)) return 0;

   if (expect_symbol2(par, '+', '+', NULL) || expect_symbol2(par, '-', '-', NULL)) {
      inc = (par->tok.value[0] == '+');

      if (buf_is_load(par, par->last_buf_pos, &value)) {
         if (value-1 >= -128 && value-1 < 0) {
            buf_append(par, inc? BC_INC : BC_DEC);
            buf_append(par, value-1);
            add_line_info(par);
         }
         else {
            inc_stack(par, 2);
            buf_append_load(par, -1);
            buf_append_const(par, 1);
            buf_append(par, inc? BC_ADD : BC_SUB);
            add_line_info(par);
            buf_append_store(par, value-2);
            par->stack_pos -= 2;
         }
      }
      else if (buf_is_load_local_var(par, par->last_buf_pos, &value)) {
         inc_stack(par, 2);
         buf_append_load(par, -1);
         buf_append_const(par, 1);
         buf_append(par, inc? BC_ADD : BC_SUB);
         add_line_info(par);
         buf_append_store_local_var(par, value);
         par->stack_pos -= 2;
      }
      else if (par->buf[par->last_buf_pos] == BC_ARRAY_GET) {
         remove_line_info(par);
         par->buf_len = par->last_buf_pos;
         inc_stack(par, 6);
         buf_append_load(par, -2);
         buf_append_load(par, -2);
         buf_append(par, BC_ARRAY_GET);
         add_line_info(par);
         buf_append_load(par, -3);
         buf_append_load(par, -3);
         buf_append_load(par, -3);
         buf_append_const(par, 1);
         buf_append(par, inc? BC_ADD : BC_SUB);
         add_line_info(par);
         buf_append(par, BC_ARRAY_SET);
         add_line_info(par);
         buf_append_store(par, -3);
         buf_append_pop(par, 1);
         par->stack_pos -= 6;
      }
      else if (par->buf[par->last_buf_pos] == BC_HASH_GET) {
         remove_line_info(par);
         par->buf_len = par->last_buf_pos;
         inc_stack(par, 6);
         buf_append_load(par, -2);
         buf_append_load(par, -2);
         buf_append(par, BC_HASH_GET);
         add_line_info(par);
         buf_append_load(par, -3);
         buf_append_load(par, -3);
         buf_append_load(par, -3);
         buf_append_const(par, 1);
         buf_append(par, inc? BC_ADD : BC_SUB);
         add_line_info(par);
         buf_append(par, BC_HASH_SET);
         add_line_info(par);
         buf_append_store(par, -3);
         buf_append_pop(par, 1);
         par->stack_pos -= 6;
      }
      else {
         par->tok.error = "invalid assignment destination";
         return 0;
      }
      return 1;
   }

   return 1;
}


static int parse_multiplicative_expression(Parser *par)
{
   if (!parse_unary_expression(par)) return 0;

   for (;;) {
      if (expect_symbol(par, '*', NULL)) {
         if (!parse_unary_expression(par)) return 0;
         buf_append(par, BC_MUL);
         add_line_info(par);
         par->stack_pos--;
         continue;
      }
      if (expect_symbol(par, '/', NULL)) {
         if (!parse_unary_expression(par)) return 0;
         buf_append(par, BC_DIV);
         add_line_info(par);
         par->stack_pos--;
         continue;
      }
      if (expect_symbol(par, '%', NULL)) {
         if (!parse_unary_expression(par)) return 0;
         buf_append(par, BC_REM);
         add_line_info(par);
         par->stack_pos--;
         continue;
      }
      break;
   }

   return 1;
}


static int parse_additive_expression(Parser *par)
{
   if (!parse_multiplicative_expression(par)) return 0;
   
   for (;;) {
      if (expect_symbol(par, '+', NULL)) {
         if (!parse_multiplicative_expression(par)) return 0;
         buf_append(par, BC_ADD);
         add_line_info(par);
         par->stack_pos--;
         continue;
      }
      if (expect_symbol(par, '-', NULL)) {
         if (!parse_multiplicative_expression(par)) return 0;
         buf_append(par, BC_SUB);
         add_line_info(par);
         par->stack_pos--;
         continue;
      }
      break;
   }

   return 1;
}


static int parse_bitwise_expression(Parser *par)
{
   if (!parse_additive_expression(par)) return 0;
   
   for (;;) {
      if (expect_symbol2(par, '<', '<', NULL)) {
         if (!parse_additive_expression(par)) return 0;
         buf_append(par, BC_SHL);
         par->stack_pos--;
         continue;
      }
      if (expect_symbol2(par, '>', '>', NULL)) {
         if (!parse_additive_expression(par)) return 0;
         buf_append(par, BC_SHR);
         par->stack_pos--;
         continue;
      }
      if (expect_symbol3(par, '>', '>', '>', NULL)) {
         if (!parse_additive_expression(par)) return 0;
         buf_append(par, BC_USHR);
         par->stack_pos--;
         continue;
      }
      if (expect_symbol(par, '&', NULL)) {
         if (!parse_additive_expression(par)) return 0;
         buf_append(par, BC_AND);
         par->stack_pos--;
         continue;
      }
      if (expect_symbol(par, '|', NULL)) {
         if (!parse_additive_expression(par)) return 0;
         buf_append(par, BC_OR);
         par->stack_pos--;
         continue;
      }
      if (expect_symbol(par, '^', NULL)) {
         if (!parse_additive_expression(par)) return 0;
         buf_append(par, BC_XOR);
         par->stack_pos--;
         continue;
      }
      break;
   }

   return 1;
}


static int parse_comparison_expression(Parser *par)
{
   if (!parse_bitwise_expression(par)) return 0;
   
   for (;;) {
      if (expect_symbol(par, '<', NULL)) {
         if (!parse_bitwise_expression(par)) return 0;
         buf_append(par, BC_LT);
         par->stack_pos--;
         continue;
      }
      if (expect_symbol2(par, '<', '=', NULL)) {
         if (!parse_bitwise_expression(par)) return 0;
         buf_append(par, BC_LE);
         par->stack_pos--;
         continue;
      }
      if (expect_symbol(par, '>', NULL)) {
         if (!parse_bitwise_expression(par)) return 0;
         buf_append(par, BC_GT);
         par->stack_pos--;
         continue;
      }
      if (expect_symbol2(par, '>', '=', NULL)) {
         if (!parse_bitwise_expression(par)) return 0;
         buf_append(par, BC_GE);
         par->stack_pos--;
         continue;
      }
      if (expect_symbol3(par, '=', '=', '=', NULL)) {
         if (!parse_bitwise_expression(par)) return 0;
         buf_append(par, BC_EQ);
         par->stack_pos--;
         continue;
      }
      if (expect_symbol3(par, '!', '=', '=', NULL)) {
         if (!parse_bitwise_expression(par)) return 0;
         buf_append(par, BC_NE);
         par->stack_pos--;
         continue;
      }
      if (expect_symbol2(par, '=', '=', NULL)) {
         if (!parse_bitwise_expression(par)) return 0;
         buf_append(par, BC_EQ_VALUE);
         par->stack_pos--;
         continue;
      }
      if (expect_symbol2(par, '!', '=', NULL)) {
         if (!parse_bitwise_expression(par)) return 0;
         buf_append(par, BC_NE_VALUE);
         par->stack_pos--;
         continue;
      }
      break;
   }

   return 1;
}


static int parse_logical_expression(Parser *par)
{
   int skip_pos;

   if (!parse_comparison_expression(par)) return 0;
   
   for (;;) {
      if (expect_symbol2(par, '&', '&', NULL) || expect_symbol2(par, '|', '|', NULL)) {
         buf_append_load(par, -1);
         inc_stack(par, 1);
         if (par->tok.value[0] == '|') {
            buf_append(par, BC_LOGNOT);
         }
         skip_pos = buf_append_branch(par, BC_BRANCH0);
         buf_append_pop(par, 1);
         par->stack_pos -= 2;
         if (!parse_comparison_expression(par)) return 0;
         if (!buf_update_branch(par, skip_pos)) return 0;
         continue;
      }
      break;
   }

   return 1;
}


static int parse_ternary_expression(Parser *par)
{
   int skip_pos, end_pos;

   if (!parse_logical_expression(par)) return 0;
   
   if (expect_symbol(par, '?', NULL)) {
      skip_pos = buf_append_branch(par, BC_BRANCH0);
      par->stack_pos--;
      
      if (!parse_expression(par)) return 0;
      
      if (!expect_symbol(par, ':', "expected ':'")) return 0;
      
      end_pos = buf_append_branch(par, BC_JUMP0);
      if (!buf_update_branch(par, skip_pos)) return 0;
      par->stack_pos--;
      
      if (!parse_expression(par)) return 0;

      if (!buf_update_branch(par, end_pos)) return 0;
   }

   return 1;
}


static int replace_simple_incdec(Parser *par)
{
   char *name;
   int inc, var_stack_pos, value;
   
   if (expect_symbol2(par, '+', '+', NULL) || expect_symbol2(par, '-', '-', NULL)) {
      inc = (par->tok.value[0] == '+');

      if (!expect_type(par, TOK_IDENT, NULL)) return 0;

      name = string_dup(par->tok.value, par->tok.len);
      var_stack_pos = (intptr_t)string_hash_get(&par->variables, name);
      value = var_stack_pos - par->stack_pos;
      free(name);

      if (!var_stack_pos || value < -128 || value >= 0) return 0;

      if (!expect_symbol(par, ',', NULL) && !expect_symbol(par, ';', NULL) && !expect_symbol(par, ')', NULL)) {
         return 0;
      }
      undo_token(&par->tok);

      buf_append(par, inc? BC_INC : BC_DEC);
      buf_append(par, value);
      add_line_info(par);
      return 1;
   }

   if (!expect_type(par, TOK_IDENT, NULL)) return 0;

   name = string_dup(par->tok.value, par->tok.len);
   var_stack_pos = (intptr_t)string_hash_get(&par->variables, name);
   value = var_stack_pos - par->stack_pos;
   free(name);
      
   if (!var_stack_pos || value < -128 || value >= 0) return 0;

   if (!expect_symbol2(par, '+', '+', NULL) && !expect_symbol2(par, '-', '-', NULL)) return 0;
   inc = (par->tok.value[0] == '+');

   if (!expect_symbol(par, ',', NULL) && !expect_symbol(par, ';', NULL) && !expect_symbol(par, ')', NULL)) {
      return 0;
   }
   undo_token(&par->tok);
   
   buf_append(par, inc? BC_INC : BC_DEC);
   buf_append(par, value);
   add_line_info(par);
   return 1;
}


static int parse_assignment_expression(Parser *par, int statement)
{
   Tokenizer save_tok;
   int value, type, needs_line_info;

   if (statement) {
      save_tok = par->tok;
      if (replace_simple_incdec(par)) return 1;
      par->tok = save_tok;
   }

   if (!parse_ternary_expression(par)) return 0;

   type = -1;
   needs_line_info = 0;

   if (expect_symbol(par, '=', NULL)) {
      type = BC_EQ;
   }
   else if (expect_symbol2(par, '+', '=', NULL)) {
      type = BC_ADD;
      needs_line_info = 1;
   }
   else if (expect_symbol2(par, '-', '=', NULL)) {
      type = BC_SUB;
      needs_line_info = 1;
   }
   else if (expect_symbol2(par, '*', '=', NULL)) {
      type = BC_MUL;
      needs_line_info = 1;
   }
   else if (expect_symbol2(par, '/', '=', NULL)) {
      type = BC_DIV;
      needs_line_info = 1;
   }
   else if (expect_symbol2(par, '%', '=', NULL)) {
      type = BC_REM;
      needs_line_info = 1;
   }
   else if (expect_symbol2(par, '&', '=', NULL)) {
      type = BC_AND;
   }
   else if (expect_symbol2(par, '|', '=', NULL)) {
      type = BC_OR;
   }
   else if (expect_symbol2(par, '^', '=', NULL)) {
      type = BC_XOR;
   }
   else if (expect_symbol3(par, '<', '<', '=', NULL)) {
      type = BC_SHL;
   }
   else if (expect_symbol3(par, '>', '>', '=', NULL)) {
      type = BC_SHR;
   }
   else if (expect_symbol4(par, '>', '>', '>', '=', NULL)) {
      type = BC_USHR;
   }

   if (type != -1) {
      if (buf_is_load(par, par->last_buf_pos, &value)) {
         if (type == BC_EQ) {
            par->buf_len = par->last_buf_pos;
            par->stack_pos--;
            if (!parse_expression(par)) return 0;
         }
         else {
            if (!parse_expression(par)) return 0;
            buf_append(par, type);
            if (needs_line_info) {
               add_line_info(par);
            }
            par->stack_pos--;
         }
         if (statement) {
            buf_append_store(par, value-1);
            par->stack_pos--;
         }
         else {
            inc_stack(par, 1);
            buf_append_load(par, -1);
            buf_append_store(par, value-2);
            par->stack_pos--;
         }
      }
      else if (buf_is_load_local_var(par, par->last_buf_pos, &value)) {
         if (type == BC_EQ) {
            par->buf_len = par->last_buf_pos;
            par->stack_pos--;
            if (!parse_expression(par)) return 0;
         }
         else {
            if (!parse_expression(par)) return 0;
            buf_append(par, type);
            if (needs_line_info) {
               add_line_info(par);
            }
            par->stack_pos--;
         }
         if (statement) {
            buf_append_store_local_var(par, value);
            par->stack_pos--;
         }
         else {
            inc_stack(par, 1);
            buf_append_load(par, -1);
            buf_append_store_local_var(par, value);
            par->stack_pos--;
         }
      }
      else if (par->buf[par->last_buf_pos] == BC_ARRAY_GET) {
         remove_line_info(par);
         par->buf_len = par->last_buf_pos;
         inc_stack(par, 1);
         if (type == BC_EQ) {
            if (!parse_expression(par)) return 0;
         }
         else {
            inc_stack(par, 2);
            buf_append_load(par, -2);
            buf_append_load(par, -2);
            buf_append(par, BC_ARRAY_GET);
            add_line_info(par);
            par->stack_pos--;
            if (!parse_expression(par)) return 0;
            buf_append(par, type);
            if (needs_line_info) {
               add_line_info(par);
            }
            par->stack_pos--;
         }
         if (statement) {
            buf_append(par, BC_ARRAY_SET);
            add_line_info(par);
            par->stack_pos -= 3;
         }
         else {
            buf_append(par, BC_ARRAY_SET);
            add_line_info(par);
            buf_append_load(par, 2);
            par->stack_pos -= 2;
         }
      }
      else if (par->buf[par->last_buf_pos] == BC_HASH_GET) {
         remove_line_info(par);
         par->buf_len = par->last_buf_pos;
         inc_stack(par, 1);
         if (type == BC_EQ) {
            if (!parse_expression(par)) return 0;
         }
         else {
            inc_stack(par, 2);
            buf_append_load(par, -2);
            buf_append_load(par, -2);
            buf_append(par, BC_HASH_GET);
            add_line_info(par);
            par->stack_pos--;
            if (!parse_expression(par)) return 0;
            buf_append(par, type);
            if (needs_line_info) {
               add_line_info(par);
            }
            par->stack_pos--;
         }
         if (statement) {
            buf_append(par, BC_HASH_SET);
            add_line_info(par);
            par->stack_pos -= 3;
         }
         else {
            buf_append(par, BC_HASH_SET);
            add_line_info(par);
            buf_append_load(par, 2);
            par->stack_pos -= 2;
         }
      }
      else {
         par->tok.error = "invalid assignment destination";
         return 0;
      }
   }
   else {
      if (expect_symbol(par, '[', NULL)) {
         save_tok = par->tok;
         if (expect_symbol(par, ']', NULL)) {
            if (!expect_symbol(par, '=', "expected '='")) return 0;
            if (!parse_expression(par)) return 0;
            buf_append(par, BC_ARRAY_APPEND);
            add_line_info(par);
            par->stack_pos -= 2;
            return 1;
         }
         else {
            par->tok = save_tok;
            undo_token(&par->tok);
         }
      }
      if (statement) {
         buf_append_pop(par, 1);
         par->stack_pos--;
         return 1;
      }
   }

   return 1;
}


static int parse_expression(Parser *par)
{
   if (!parse_assignment_expression(par, 0)) return 0;
   return 1;
}


static int put_variable(Parser *par, char *name, int stack_pos)
{
   int old_stack_pos;

   old_stack_pos = (intptr_t)string_hash_set(&par->variables, name, (void *)(intptr_t)stack_pos);
   if (old_stack_pos) {
      par->tok.error = "duplicate variable name in current scope";
      return 0;
   }
   par->has_vars = 1;
   return 1;
}


static int parse_var_init(Parser *par)
{
   char *name;
   
   do {
      if (!expect_type(par, TOK_IDENT, "expected variable name")) return 0;
      name = string_dup(par->tok.value, par->tok.len);

      if (expect_symbol(par, '=', NULL)) {
         if (!parse_expression(par)) {
            free(name);
            return 0;
         }
      }
      else {
         buf_append_const(par, 0);
         inc_stack(par, 1);
      }

      if (!put_variable(par, name, par->stack_pos-1)) return 0;
   }
   while (expect_symbol(par, ',', NULL));

   return 1;
}


static int parse_var_call2(Parser *par, int assign)
{
   char *name1 = NULL, *name2 = NULL;
   int ret, weak_call = 0; // prevents uninitialized warning
   int intrinsic_type, needs_line_info, num, num_args, is_int_conv, var_stack_pos, has_float_shorthand, use_fast_error;

   if (!expect_type(par, TOK_IDENT, "expected variable name")) goto error;
   name1 = string_dup(par->tok.value, par->tok.len);

   if (!expect_symbol(par, ',', "expected ','")) goto error;

   if (!expect_type(par, TOK_IDENT, "expected variable name")) goto error;
   name2 = string_dup(par->tok.value, par->tok.len);

   if (!expect_symbol(par, ')', "expected ')'")) goto error;
   if (!expect_symbol(par, '=', "expected '='")) goto error;

   intrinsic_type = -1;
   needs_line_info = 0;

   if (expect_type(par, TOK_IDENT, NULL)) {
      num_args = 4;
      is_int_conv = 0;
      has_float_shorthand = 0;

      switch (par->tok.len) {
         case 2:
            if (!strncmp(par->tok.value, "ln", par->tok.len)) { intrinsic_type = BC_EXT_DBL_LN; num_args = 2; break; }
            break;

         case 3:
            if (!strncmp(par->tok.value, "int", par->tok.len)) { intrinsic_type = BC_EXT_DBL_INT; num_args = 2; break; }
            if (!strncmp(par->tok.value, "pow", par->tok.len)) { intrinsic_type = BC_EXT_DBL_POW; num_args = 4; has_float_shorthand = 1; break; }
            if (!strncmp(par->tok.value, "exp", par->tok.len)) { intrinsic_type = BC_EXT_DBL_EXP; num_args = 2; break; }
            if (!strncmp(par->tok.value, "sin", par->tok.len)) { intrinsic_type = BC_EXT_DBL_SIN; num_args = 2; break; }
            if (!strncmp(par->tok.value, "cos", par->tok.len)) { intrinsic_type = BC_EXT_DBL_COS; num_args = 2; break; }
            if (!strncmp(par->tok.value, "tan", par->tok.len)) { intrinsic_type = BC_EXT_DBL_TAN; num_args = 2; break; }
            break;

         case 4:
            if (!strncmp(par->tok.value, "fadd", par->tok.len)) { intrinsic_type = BC_EXT_DBL_ADD; num_args = 4; has_float_shorthand = 1; break; }
            if (!strncmp(par->tok.value, "fsub", par->tok.len)) { intrinsic_type = BC_EXT_DBL_SUB; num_args = 4; has_float_shorthand = 1; break; }
            if (!strncmp(par->tok.value, "fmul", par->tok.len)) { intrinsic_type = BC_EXT_DBL_MUL; num_args = 4; has_float_shorthand = 1; break; }
            if (!strncmp(par->tok.value, "fdiv", par->tok.len)) { intrinsic_type = BC_EXT_DBL_DIV; num_args = 4; has_float_shorthand = 1; break; }
            if (!strncmp(par->tok.value, "fabs", par->tok.len)) { intrinsic_type = BC_EXT_DBL_FABS; num_args = 2; break; }
            if (!strncmp(par->tok.value, "fmin", par->tok.len)) { intrinsic_type = BC_EXT_DBL_FMIN; num_args = 4; has_float_shorthand = 1; break; }
            if (!strncmp(par->tok.value, "fmax", par->tok.len)) { intrinsic_type = BC_EXT_DBL_FMAX; num_args = 4; has_float_shorthand = 1; break; }
            if (!strncmp(par->tok.value, "ceil", par->tok.len)) { intrinsic_type = BC_EXT_DBL_CEIL; num_args = 2; break; }
            if (!strncmp(par->tok.value, "sqrt", par->tok.len)) { intrinsic_type = BC_EXT_DBL_SQRT; num_args = 2; break; }
            if (!strncmp(par->tok.value, "cbrt", par->tok.len)) { intrinsic_type = BC_EXT_DBL_CBRT; num_args = 2; break; }
            if (!strncmp(par->tok.value, "log2", par->tok.len)) { intrinsic_type = BC_EXT_DBL_LOG2; num_args = 2; break; }
            if (!strncmp(par->tok.value, "asin", par->tok.len)) { intrinsic_type = BC_EXT_DBL_ASIN; num_args = 2; break; }
            if (!strncmp(par->tok.value, "acos", par->tok.len)) { intrinsic_type = BC_EXT_DBL_ACOS; num_args = 2; break; }
            if (!strncmp(par->tok.value, "atan", par->tok.len)) { intrinsic_type = BC_EXT_DBL_ATAN; num_args = 2; break; }
            break;

         case 5:
            if (!strncmp(par->tok.value, "add32", par->tok.len)) { intrinsic_type = BC_EXT_ADD32; break; }
            if (!strncmp(par->tok.value, "sub32", par->tok.len)) { intrinsic_type = BC_EXT_SUB32; break; }
            if (!strncmp(par->tok.value, "add64", par->tok.len)) { intrinsic_type = BC_EXT_ADD64; break; }
            if (!strncmp(par->tok.value, "sub64", par->tok.len)) { intrinsic_type = BC_EXT_SUB64; break; }
            if (!strncmp(par->tok.value, "mul64", par->tok.len)) { intrinsic_type = BC_EXT_MUL64; break; }
            if (!strncmp(par->tok.value, "div64", par->tok.len)) { intrinsic_type = BC_EXT_DIV64; needs_line_info = 1; break; }
            if (!strncmp(par->tok.value, "rem64", par->tok.len)) { intrinsic_type = BC_EXT_REM64; needs_line_info = 1; break; }
            if (!strncmp(par->tok.value, "float", par->tok.len)) { intrinsic_type = BC_EXT_DBL_FLOAT; num_args = 2; break; }
            if (!strncmp(par->tok.value, "fconv", par->tok.len)) { intrinsic_type = BC_EXT_DBL_CONV_UP; num_args = 1; break; }
            if (!strncmp(par->tok.value, "floor", par->tok.len)) { intrinsic_type = BC_EXT_DBL_FLOOR; num_args = 2; break; }
            if (!strncmp(par->tok.value, "iceil", par->tok.len)) { intrinsic_type = BC_EXT_DBL_CEIL; num_args = 2; is_int_conv = 1; break; }
            if (!strncmp(par->tok.value, "round", par->tok.len)) { intrinsic_type = BC_EXT_DBL_ROUND; num_args = 2; break; }
            if (!strncmp(par->tok.value, "log10", par->tok.len)) { intrinsic_type = BC_EXT_DBL_LOG10; num_args = 2; break; }
            if (!strncmp(par->tok.value, "atan2", par->tok.len)) { intrinsic_type = BC_EXT_DBL_ATAN2; num_args = 4; break; }
            break;

         case 6:
            if (!strncmp(par->tok.value, "umul64", par->tok.len)) { intrinsic_type = BC_EXT_UMUL64; num_args = 2; break; }
            if (!strncmp(par->tok.value, "udiv64", par->tok.len)) { intrinsic_type = BC_EXT_UDIV64; needs_line_info = 1; break; }
            if (!strncmp(par->tok.value, "urem64", par->tok.len)) { intrinsic_type = BC_EXT_UREM64; needs_line_info = 1; break; }
            if (!strncmp(par->tok.value, "fclamp", par->tok.len)) { intrinsic_type = BC_EXT_DBL_FCLAMP; break; }
            if (!strncmp(par->tok.value, "ifloor", par->tok.len)) { intrinsic_type = BC_EXT_DBL_FLOOR; num_args = 2; is_int_conv = 1; break; }
            if (!strncmp(par->tok.value, "iround", par->tok.len)) { intrinsic_type = BC_EXT_DBL_ROUND; num_args = 2; is_int_conv = 1; break; }
            break;
      }

      if (intrinsic_type == -1) {
         undo_token(&par->tok);
      }
   }

   if (intrinsic_type != -1) {
      if (!expect_symbol(par, '(', "expected '('")) goto error;

      num = 0;
      if (!expect_symbol(par, ')', NULL)) {
         do {
            if (!parse_expression(par)) goto error;
            num++;
         }
         while (expect_symbol(par, ',', NULL));

         if (!expect_symbol(par, ')', "expected ')' or ','")) goto error;
      }

      if (intrinsic_type == BC_EXT_DBL_FCLAMP) {
         if (num != 4 && num != 6) {
            par->tok.error = "improper number of function parameters";
            goto error;
         }
         if (num == 4) {
            intrinsic_type = BC_EXT_DBL_FCLAMP_SHORT;
         }
      }
      else if (intrinsic_type == BC_EXT_ADD32 || intrinsic_type == BC_EXT_SUB32) {
         if (num != 2 && num != 3) {
            par->tok.error = "improper number of function parameters";
            goto error;
         }
      }
      else if (intrinsic_type == BC_EXT_MUL64) {
         if (num != 2 && num != 4) {
            par->tok.error = "improper number of function parameters";
            goto error;
         }
         if (num == 4) {
            intrinsic_type = BC_EXT_MUL64_LONG;
         }
      }
      else if (has_float_shorthand) {
         if (num != num_args && num != num_args-1) {
            par->tok.error = "improper number of function parameters";
            goto error;
         }
      }
      else {
         if (num != num_args) {
            par->tok.error = "improper number of function parameters";
            goto error;
         }
      }

      if ((intrinsic_type == BC_EXT_ADD32 || intrinsic_type == BC_EXT_SUB32) && num == 2) {
         buf_append_const(par, 0);
         inc_stack(par, 1);
         par->stack_pos--;
      }
      if (has_float_shorthand && num == num_args-1) {
         buf_append(par, BC_EXTENDED);
         buf_append(par, BC_EXT_DBL_CONV_UP);
         inc_stack(par, 1);
         par->stack_pos--;
      }
      buf_append(par, BC_EXTENDED);
      buf_append(par, intrinsic_type);
      if (is_int_conv) {
         buf_append(par, BC_EXTENDED);
         buf_append(par, BC_EXT_DBL_INT);
      }
      if (needs_line_info) {
         add_line_info(par);
      }
      par->last_buf_pos--;
      par->stack_pos -= num - 1;
   }
   else {
      use_fast_error = par->use_fast_error;

      if (expect_symbol(par, '@', NULL)) {
         par->use_fast_error = 1;
         undo_token(&par->tok);
      }

      if (!parse_primary_expression(par)) goto error;

      par->use_fast_error = use_fast_error;

      if (!buf_set_call2(par, &weak_call)) {
         par->tok.error = "last expression must be function call";
         goto error;
      }
   }

   if (assign) {
      if (intrinsic_type == -1 && !weak_call) {
         buf_append(par, BC_CLEAN_CALL2);
      }
      inc_stack(par, 1);

      var_stack_pos = (intptr_t)string_hash_get(&par->variables, name2);
      free(name2);
      name2 = NULL;
      if (!var_stack_pos) {
         par->tok.error = "undefined variable name";
         goto error;
      }
      buf_append_store(par, var_stack_pos - par->stack_pos);
      par->stack_pos--;

      var_stack_pos = (intptr_t)string_hash_get(&par->variables, name1);
      free(name1);
      name1 = NULL;
      if (!var_stack_pos) {
         par->tok.error = "undefined variable name";
         goto error;
      }
      buf_append_store(par, var_stack_pos - par->stack_pos);
      par->stack_pos--;
   }
   else {
      ret = put_variable(par, name1, par->stack_pos-1);
      name1 = NULL;
      if (!ret) goto error;
   
      if (intrinsic_type == -1 && !weak_call) {
         buf_append(par, BC_CLEAN_CALL2);
      }
      inc_stack(par, 1);
   
      ret = put_variable(par, name2, par->stack_pos-1);
      name2 = NULL;
      if (!ret) goto error;
   }

   return 1;

error:
   free(name1);
   free(name2);
   return 0;
}


static int parse_case_constant(Parser *par, int *int_value)
{
   Script *script;
   Constant *constant;
   Value value;
   char *s;
   int conflict;

   if (expect_type(par, TOK_IDENT, NULL)) {
      s = string_dup(par->tok.value, par->tok.len);
      script = NULL;

      if (expect_symbol(par, ':', NULL)) {
         script = string_hash_get(&par->import_aliases, s);

         if (script) {
            free(s);

            if (!expect_type(par, TOK_IDENT, "expected identifier")) return 0;
            s = string_dup(par->tok.value, par->tok.len);
         }
         else {
            undo_token(&par->tok);
         }
      }

      constant = find_constant(script? script : par->script, s, script != NULL, &conflict, NULL);
      free(s);
      if (conflict) {
         par->tok.error = "declaration of constant in multiple imports";
         return 0;
      }

      if (!constant) {
         par->tok.error = "unknown constant name";
         return 0;
      }

      if (!fixscript_is_int(constant->value)) {
         return 0;
      }

      *int_value = constant->value.value;
      return 1; 
   }

   if (!parse_constant(par, &value, 1)) {
      if (!par->tok.error) {
         par->tok.error = "expected integer constant";
      }
      return 0;
   }

   *int_value = value.value;
   return 1;
}


static int compare_switch_cases(const void *ptr1, const void *ptr2)
{
   void **case1 = (void *)ptr1;
   void **case2 = (void *)ptr2;
   int value1 = (intptr_t)case1[0];
   int value2 = (intptr_t)case2[0];
   return value1 < value2? -1 : value1 > value2? +1 : 0;
}


static int parse_switch(Parser *par)
{
   union {
      int i;
      unsigned char c[4];
   } int_val;

   DynArray cases;
   LoopState loop;
   int i, value, value2, is_range, pc, default_pc = -1, ret = 0, prev_value = 0;
   int switch_pos, default_pc_pos, end_pos, aligned;

   memset(&cases, 0, sizeof(DynArray));

   if (!expect_symbol(par, '(', "expected '('")) goto error;

   if (!parse_expression(par)) goto error;

   buf_append(par, BC_SWITCH);
   switch_pos = par->buf_len;
   buf_append_int(par, 0);
   par->stack_pos--;

   if (!expect_symbol(par, ')', "expected ')'")) goto error;
   if (!expect_symbol(par, '{', "expected '{'")) goto error;

   enter_loop(par, &loop, 1, 0, 0);

   while (!expect_symbol(par, '}', NULL)) {
      pc = par->heap->bytecode_size + par->buf_len;

      if (expect_type(par, KW_CASE, NULL)) {
         for (;;) {
            is_range = 0;
            if (!parse_case_constant(par, &value)) goto error;
            if (expect_symbol2(par, '.', '.', NULL)) {
               if (!parse_case_constant(par, &value2)) goto error;
               is_range = 1;
            }
            
            if (is_range) {
               if (value >= value2) {
                  par->tok.error = "invalid range";
                  goto error;
               }
               dynarray_add(&cases, (void *)(intptr_t)value);
               dynarray_add(&cases, (void *)(intptr_t)(-pc));
               dynarray_add(&cases, (void *)(intptr_t)value2);
               dynarray_add(&cases, (void *)0);
            }
            else {
               dynarray_add(&cases, (void *)(intptr_t)value);
               dynarray_add(&cases, (void *)(intptr_t)pc);
            }

            if (expect_symbol(par, ',', NULL)) continue;
            break;
         }
         if (!expect_symbol(par, ':', "expected ':'")) goto error;
      }
      else if (expect_type(par, KW_DEFAULT, NULL)) {
         if (!expect_symbol(par, ':', "expected ':'")) goto error;
         if (default_pc != -1) {
            par->tok.error = "duplicate default case";
            goto error;
         }
         default_pc = pc;
      }
      else {
         if (expect_symbol(par, '{', NULL)) {
            if (!parse_block(par, BT_NORMAL)) goto error;
         }
         else {
            if (!parse_statement(par, "expected statement, 'case', 'default' or '}'")) goto error;
         }
      }
   }

   if (default_pc == -1 && cases.len == 0) {
      par->tok.error = "empty switch";
      goto error;
   }

   qsort(cases.data, cases.len/2, 2*sizeof(void *), compare_switch_cases);

   for (i=0; i<cases.len; i+=2) {
      value = (intptr_t)cases.data[i+0];
      pc = (intptr_t)cases.data[i+1];
      if (i > 0 && value == prev_value) {
         par->tok.error = "duplicate case value";
         goto error;
      }
      if (pc < 0) {
         if (i+2 >= cases.len || (intptr_t)cases.data[i+2+1] != 0) {
            par->tok.error = "intersection of ranges";
            goto error;
         }
      }
      if (pc == 0) {
         if (i == 0 || (intptr_t)cases.data[i-2+1] > 0) {
            par->tok.error = "intersection of ranges";
            goto error;
         }
      }
      prev_value = value;
   }

   end_pos = buf_append_branch(par, BC_JUMP0);
   
   pc = par->heap->bytecode_size + par->buf_len;
   aligned = (pc + 3) & ~3;
   for (i=0; i<aligned-pc; i++) {
      buf_append(par, 0);
   }
   int_val.i = (aligned >> 2) + 2;
   for (i=0; i<4; i++) {
      par->buf[switch_pos+i] = int_val.c[i];
   }

   buf_append_int(par, cases.len/2);
   default_pc_pos = par->buf_len;
   buf_append_int(par, default_pc);
   for (i=0; i<cases.len; i++) {
      buf_append_int(par, (intptr_t)cases.data[i]);
   }

   if (!buf_update_branch(par, end_pos)) goto error;
   if (!leave_loop_break(par, &loop)) goto error;

   if (default_pc == -1) {
      int_val.i = par->heap->bytecode_size + par->buf_len;
      for (i=0; i<4; i++) {
         par->buf[default_pc_pos+i] = int_val.c[i];
      }
   }
   
   ret = 1;

error:
   free(cases.data);
   return ret;
}


static int parse_statement(Parser *par, const char *error)
{
   LoopState loop;
   int num;
   int skip_pos, end_pos, start_pc;

   if (expect_type(par, KW_RETURN, NULL)) {
      num = 1;
      if (expect_symbol(par, ';', NULL)) {
         buf_append_const(par, 0);
         inc_stack(par, 1);
      }
      else {
         if (!parse_expression(par)) return 0;
         if (expect_symbol(par, ',', NULL)) {
            if (!parse_expression(par)) return 0;
            num = 2;
         }
         if (!expect_symbol(par, ';', "expected ';'")) return 0;
      }
      if (num == 2) {
         buf_append(par, BC_RETURN2);
      }
      else {
         buf_append_const(par, par->stack_pos-1);
         inc_stack(par, 1);
         buf_append(par, BC_RETURN);
         par->stack_pos--;
      }
      par->stack_pos -= num;
      return 1;
   }

   if (expect_type(par, KW_IF, NULL)) {
      if (!expect_symbol(par, '(', "expected '('")) return 0;

      if (!parse_expression(par)) return 0;
      skip_pos = buf_append_branch(par, BC_BRANCH0);
      par->stack_pos--;

      if (!expect_symbol(par, ')', "expected ')'")) return 0;
      
      if (expect_symbol(par, '{', NULL)) {
         if (!parse_block(par, BT_NORMAL)) return 0;
         if (expect_type(par, KW_ELSE, NULL)) {
            end_pos = buf_append_branch(par, BC_JUMP0);
            if (!buf_update_branch(par, skip_pos)) return 0;
            
            if (expect_symbol(par, '{', NULL)) {
               if (!parse_block(par, BT_NORMAL)) return 0;
            }
            else {
               if (!parse_statement(par, "expected statement")) return 0;
            }

            if (!buf_update_branch(par, end_pos)) return 0;
         }
         else {
            if (!buf_update_branch(par, skip_pos)) return 0;
         }
      }
      else {
         if (!parse_statement(par, "expected statement")) return 0;
         if (!buf_update_branch(par, skip_pos)) return 0;
      }
      return 1;
   }

   if (expect_type(par, KW_FOR, NULL)) {
      return parse_block(par, BT_FOR);
   }

   if (expect_type(par, KW_WHILE, NULL)) {
      if (!expect_symbol(par, '(', "expected '('")) return 0;
      
      start_pc = par->buf_len;

      if (!parse_expression(par)) return 0;

      end_pos = buf_append_branch(par, BC_BRANCH0);
      par->stack_pos--;

      if (!expect_symbol(par, ')', "expected ')'")) return 0;

      enter_loop(par, &loop, 1, 1, start_pc);

      if (expect_symbol(par, '{', NULL)) {
         if (!parse_block(par, BT_NORMAL)) return 0;
      }
      else {
         if (!parse_statement(par, "expected statement")) return 0;
      }
      
      if (!leave_loop_continue(par, &loop)) return 0;
      buf_append_loop(par, start_pc);
      if (!leave_loop_break(par, &loop)) return 0;

      if (!buf_update_branch(par, end_pos)) return 0;
      return 1;
   }

   if (expect_type(par, KW_DO, NULL)) {
      start_pc = par->buf_len;
      enter_loop(par, &loop, 1, 1, 0);

      if (!expect_symbol(par, '{', "expected '{'")) return 0;
      if (!parse_block(par, BT_NORMAL)) return 0;

      if (!leave_loop_continue(par, &loop)) return 0;

      if (!expect_type(par, KW_WHILE, "expected 'while'")) return 0;
      if (!expect_symbol(par, '(', "expected '('")) return 0;
      if (!parse_expression(par)) return 0;
      if (!expect_symbol(par, ')', "expected ')'")) return 0;
      if (!expect_symbol(par, ';', "expected ';'")) return 0;

      end_pos = buf_append_branch(par, BC_BRANCH0);
      par->stack_pos--;

      buf_append_loop(par, start_pc);
      if (!leave_loop_break(par, &loop)) return 0;

      if (!buf_update_branch(par, end_pos)) return 0;
      return 1;
   }

   if (expect_type(par, KW_BREAK, NULL)) {
      if (!par->has_break) {
         par->tok.error = "no loop or switch in current scope";
         return 0;
      }
      buf_append_pop(par, par->stack_pos - par->break_stack_pos);
      end_pos = buf_append_branch(par, BC_JUMP0);
      dynarray_add(&par->break_jumps, (void *)(intptr_t)end_pos);
      if (!expect_symbol(par, ';', "expected ';'")) return 0;
      return 1;
   }

   if (expect_type(par, KW_CONTINUE, NULL)) {
      if (!par->has_continue) {
         par->tok.error = "no loop in current scope";
         return 0;
      }
      buf_append_pop(par, par->stack_pos - par->continue_stack_pos);
      if (par->continue_pc != 0) {
         buf_append_loop(par, par->continue_pc);
      }
      else {
         skip_pos = buf_append_branch(par, BC_JUMP0);
         dynarray_add(&par->continue_jumps, (void *)(intptr_t)skip_pos);
      }
      if (!expect_symbol(par, ';', "expected ';'")) return 0;
      return 1;
   }

   if (expect_type(par, KW_SWITCH, NULL)) {
      return parse_switch(par);
   }

   if (expect_symbol(par, ';', NULL)) {
      return 1;
   }

   if (parse_assignment_expression(par, 1)) {
      if (!expect_symbol(par, ';', "expected ';'")) return 0;
      return 1;
   }

   if (!par->tok.error) {
      par->tok.error = error;
   }
   return 0;
}


static int parse_for_update(Parser *par)
{
   do {
      if (!parse_assignment_expression(par, 1)) return 0;
   }
   while (expect_symbol(par, ',', NULL));

   return 1;
}


static int parse_for_inner(Parser *par)
{
   LoopState loop;
   Tokenizer update_tok, end_tok;
   int start_pc, end_pos = -1, has_update, level;

   update_tok = par->tok; // prevents uninitialized warning

   if (!expect_symbol(par, '(', "expected '('")) return 0;

   if (expect_type(par, KW_VAR, NULL)) {
      if (!parse_var_init(par)) return 0;
      if (!expect_symbol(par, ';', "expected ';'")) return 0;
   }
   else if (!expect_symbol(par, ';', NULL)) {
      do {
         if (!parse_assignment_expression(par, 1)) return 0;
      }
      while (expect_symbol(par, ',', NULL));

      if (!expect_symbol(par, ';', "expected ';' or ','")) return 0;
   }

   start_pc = par->buf_len;
   
   if (!expect_symbol(par, ';', NULL)) {
      if (!parse_expression(par)) return 0;
      end_pos = buf_append_branch(par, BC_BRANCH0);
      par->stack_pos--;
      if (!expect_symbol(par, ';', "expected ';'")) return 0;
   }

   has_update = 0;
   if (!expect_symbol(par, ')', NULL)) {
      has_update = 1;

      update_tok = par->tok;
      level = 0;
      while (next_token(&par->tok)) {
         switch (par->tok.type) {
            case '(':
            case '{':
            case '[':
               level++;
               break;

            case ')':
            case '}':
            case ']':
               if (--level < 0) {
                  undo_token(&par->tok);
                  goto end;
               }
               break;
         }
      }
      end:;

      if (!expect_symbol(par, ')', "expected ')'")) return 0;
   }

   enter_loop(par, &loop, 1, 1, !has_update? start_pc : 0);

   if (expect_symbol(par, '{', NULL)) {
      if (!parse_block(par, BT_NORMAL)) return 0;
   }
   else {
      if (!parse_statement(par, "expected statement")) return 0;
   }

   if (!leave_loop_continue(par, &loop)) return 0;

   if (has_update) {
      end_tok = par->tok;
      par->tok = update_tok;
      if (!parse_for_update(par)) return 0;
      if (!expect_symbol(par, ')', "expected ')'")) return 0;
      par->tok = end_tok;
   }

   buf_append_loop(par, start_pc);

   if (!leave_loop_break(par, &loop)) return 0;
   if (end_pos != -1 && !buf_update_branch(par, end_pos)) return 0;
   return 1;
}


static int parse_block_inner(Parser *par, int *expr_has_ret)
{
   Tokenizer save_tok;
   int found;

   while (!expect_symbol(par, '}', NULL)) {
      if (expect_type(par, KW_VAR, NULL)) {
         if (expect_symbol(par, '(', NULL)) {
            if (!parse_var_call2(par, 0)) return 0;
            if (!expect_symbol(par, ';', "expected ';'")) return 0;
            continue;
         }
         if (!parse_var_init(par)) return 0;
         if (!expect_symbol(par, ';', "expected ';'")) return 0;
         continue;
      }

      if (expect_symbol(par, '(', NULL)) {
         found = 0;
         save_tok = par->tok;
         if (expect_type(par, TOK_IDENT, NULL) && expect_symbol(par, ',', NULL)) {
            found = 1;
         }
         par->tok = save_tok;
         if (found) {
            if (!parse_var_call2(par, 1)) return 0;
            if (!expect_symbol(par, ';', "expected ';'")) return 0;
            continue;
         }
         else {
            undo_token(&par->tok);
         }
      }

      if (expect_symbol(par, '{', NULL)) {
         if (!parse_block(par, BT_NORMAL)) return 0;
         continue;
      }

      if (expr_has_ret && expect_symbol(par, '=', NULL)) {
         if (!parse_expression(par)) return 0;
         if (!expect_symbol(par, '}', "expected '}'")) return 0;
         *expr_has_ret = 1;
         return 1;
      }

      if (!parse_statement(par, "expected statement or '}'")) return 0;
   }
   
   return 1;
}


static int parse_block(Parser *par, int type)
{
   int i, ret, old_has_vars, old_stack_pos, var_stack_pos, num;
   int expr_has_ret = 0;

   old_has_vars = par->has_vars;
   old_stack_pos = par->stack_pos;
   par->has_vars = 0;

   if (type == BT_FOR) {
      ret = parse_for_inner(par);
   }
   else {
      ret = parse_block_inner(par, type == BT_EXPR? &expr_has_ret : NULL);
   }

   if (type == BT_EXPR && ret && !expr_has_ret) {
      par->tok.error = "statement expression must provide output value";
      return 0;
   }
   
   if (par->has_vars) {
      num = 0;
      for (i=0; i<par->variables.size; i+=2) {
         if (par->variables.data[i+0] && par->variables.data[i+1]) {
            var_stack_pos = (intptr_t)par->variables.data[i+1];
            if (var_stack_pos >= old_stack_pos) {
               par->variables.data[i+1] = NULL;
               par->variables.len--;
               num++;
            }
         }
      }
      if (expr_has_ret) {
         buf_append_store(par, -num-1);
         buf_append_pop(par, num-1);
      }
      else {
         buf_append_pop(par, num);
      }
      par->stack_pos -= num;
   }

   par->has_vars = old_has_vars;
   return ret;
}


static int parse_function_inner(Parser *par, Function *func, const char *func_name)
{
   Function *old_func;
   NativeFunction *native_func;
   char *name;
   uint16_t max_stack;
   int old_stack_pos, check_stack_pos;

   par->stack_pos = 1;
   par->max_stack = par->stack_pos;

   buf_append(par, BC_CHECK_STACK);
   check_stack_pos = par->buf_len;
   buf_append(par, 0);
   buf_append(par, 0);
   add_line_info(par);

   if (!expect_symbol(par, '(', "expected '('")) return 0;

   while (expect_type(par, TOK_IDENT, NULL)) {
      if (++func->num_params > 255) {
         par->tok.error = "more than 255 parameters";
         return 0;
      }
      name = string_dup(par->tok.value, par->tok.len);
      old_stack_pos = (intptr_t)string_hash_set(&par->variables, name, (void *)(intptr_t)par->stack_pos++);
      if (old_stack_pos) {
         par->tok.error = "duplicate parameter name";
         return 0;
      }
      
      if (expect_symbol(par, ',', NULL)) {
         if (!expect_type(par, TOK_IDENT, "expected parameter name")) return 0;
         undo_token(&par->tok);
         continue;
      }

      if (!expect_symbol(par, ')', "expected ')', ',' or parameter name")) return 0;
      undo_token(&par->tok);
      break;
   }

   if (!expect_symbol(par, ')', "expected ')' or parameter name")) return 0;

   name = string_format("%s#%d", func_name, func->num_params);

   if (expect_symbol(par, ';', NULL)) {
      #ifdef FIXEMBED_TOKEN_DUMP
      if (par->heap->token_dump_mode) {
         fixembed_native_function_used(name);
      }
      else
      #endif
      {
         native_func = string_hash_get(&par->heap->native_functions_hash, name);
         if (!native_func) {
            par->tok.error = "native function not present";
         }
      }
      free(name);
      return 2;
   }

   old_func = string_hash_set(&par->script->functions, name, func);
   if (old_func) {
      free_function(old_func);
      par->tok.error = "duplicate function name";
      return 0;
   }

   if (!expect_symbol(par, '{', "expected '{' or ';'")) return 0;
   if (!parse_block(par, BT_NORMAL)) return 0;
   
   buf_append_const(par, 0);
   inc_stack(par, 1);
   buf_append_const(par, par->stack_pos-1);
   inc_stack(par, 1);
   buf_append(par, BC_RETURN);
   par->stack_pos -= 2;

   if (par->stack_pos != 1 + func->num_params || par->stack_pos > par->max_stack) {
      par->tok.error = "internal error: stack misalignment";
      return 0;
   }

   par->max_stack -= func->num_params + 1;
   if (par->max_stack > 0xFFFF) {
      par->tok.error = "stack usage is too big";
      return 0;
   }

   max_stack = par->max_stack;
   memcpy(par->buf + check_stack_pos, &max_stack, sizeof(uint16_t));
   func->max_stack = max_stack;

   return 1;
}


static int parse_function(Parser *par)
{
   Function *func;
   char *name;
   int i, ret, local = 0;
   
   if (expect_symbol(par, '@', NULL)) {
      local = 1;
   }
   
   if (!expect_type(par, TOK_IDENT, "expected identifier")) return 0;

   func = calloc(1, sizeof(Function));
   func->id = par->heap->functions.len;
   dynarray_add(&par->heap->functions, func);
   func->addr = par->heap->bytecode_size + par->buf_len;
   func->local = local;
   func->script = par->script;
   func->lines_start = par->heap->lines_size + par->lines.len/2;

   name = string_dup(par->tok.value, par->tok.len);
   ret = parse_function_inner(par, func, name);
   free(name);

   if (ret == 2) {
      par->heap->functions.len--;
      free_function(func);
      ret = par->tok.error? 0:1;
   }
   else {
      func->lines_end = par->heap->lines_size + par->lines.len/2;
   }

   for (i=0; i<par->variables.size; i+=2) {
      if (par->variables.data[i+0]) {
         free(par->variables.data[i+0]);
         par->variables.data[i+0] = NULL;
         par->variables.data[i+1] = NULL;
      }
   }

   return ret;
}


static int parse_script_inner(Parser *par)
{
   int first = 1;

   while (expect_type(par, KW_VAR, NULL)) {
      if (!parse_local_var(par)) return 0;
   }

   while (has_next(par)) {
      if (!expect_type(par, KW_FUNCTION, first? "expected 'function' or 'import' keyword" : "expected 'function' keyword")) {
         return 0;
      }
      if (!parse_function(par)) return 0;
      first = 0;
   }

   return 1;
}


static void save_script_state(Parser *par, ScriptState *state)
{
   state->used = 1;
   state->functions_len = par->heap->functions.len;
   state->locals_len = par->heap->locals_len;
}


static void restore_script_state(Parser *par, ScriptState *state)
{
   if (state->used) {
      par->heap->functions.len = state->functions_len;
      par->heap->locals_len = state->locals_len;
   }
}


static int parse_script(Parser *par, Value *error, ScriptState *state)
{
#ifdef FIXEMBED_TOKEN_DUMP
   Heap *token_heap = par->heap->token_dump_mode? par->heap->token_heap : par->heap;
#else
   Heap *token_heap = par->heap;
#endif
   Tokenizer save_tok;
   DynArray *prev_postprocess_funcs;
   Value func, value;
   int i, ok = 1;

   prev_postprocess_funcs = token_heap->cur_postprocess_funcs;
   token_heap->cur_postprocess_funcs = NULL;

   for (;;) {
      save_tok = par->tok;
      if (!expect_type(par, KW_USE, NULL)) {
         par->tok = save_tok;
         break;
      }
      if (!parse_import(par, error, 1)) {
         ok = 0;
         break;
      }
   }

   if (token_heap->cur_postprocess_funcs) {
      if (ok) {
         while (token_heap->cur_postprocess_funcs->len >= 3) {
            i = token_heap->cur_postprocess_funcs->len-3;
            func.value = (int)(intptr_t)token_heap->cur_postprocess_funcs->data[i+0];
            func.is_array = 1;
            value.value = (int)(intptr_t)token_heap->cur_postprocess_funcs->data[i+1];
            value.is_array = (int)(intptr_t)token_heap->cur_postprocess_funcs->data[i+2];
            token_heap->cur_postprocess_funcs->len -= 3;
            if (!parse_use_inner(par, NULL, error, func, value)) {
               ok = 0;
               break;
            }
            fixscript_unref(token_heap, value);
            par->semicolon_removed = 1;
         }
      }

      for (i=0; i<token_heap->cur_postprocess_funcs->len; i+=3) {
         value.value = (int)(intptr_t)token_heap->cur_postprocess_funcs->data[i+1];
         value.is_array = (int)(intptr_t)token_heap->cur_postprocess_funcs->data[i+2];
         fixscript_unref(token_heap, value);
      }
   }

   token_heap->cur_postprocess_funcs = prev_postprocess_funcs;
   if (!ok) {
      return 0;
   }

   #ifdef FIXEMBED_TOKEN_DUMP
      if (par->heap->token_dump_mode) {
         if (!par->long_jumps && !par->long_func_refs) {
            fixembed_dump_tokens(par->fname, &par->tok);
         }
      }
   #endif

   while (expect_type(par, KW_IMPORT, NULL)) {
      if (!parse_import(par, error, 0)) return 0;
   }

   while (expect_type(par, KW_CONST, NULL)) {
      if (!parse_constant_define(par)) return 0;
   }

   save_script_state(par, state);
   return parse_script_inner(par);
}


static Script *load_script(Heap *heap, const char *src, const char *fname, Value *error, int long_jumps, int long_func_refs, LoadScriptFunc load_func, void *load_data, Parser *reuse_tokens, int reload)
{
#ifdef FIXEMBED_TOKEN_DUMP
   Heap *token_heap = heap->token_dump_mode? heap->token_heap : heap;
#else
   Heap *token_heap = heap;
#endif
   Script *script;
   Parser par;
   ScriptState state;
   LoadScriptFunc prev_load_func;
   void *prev_load_data, *prev_parser;
   unsigned char *new_bytecode;
   LineEntry *new_lines;
   char *tmp;
   int i, val;
#ifndef FIXSCRIPT_NO_JIT
   const char *jit_error;
#endif

   if (!reload) {
      script = string_hash_get(&heap->scripts, fname);
      if (script) {
         return script;
      }
   }

   script = calloc(1, sizeof(Script));

   memset(&par, 0, sizeof(Parser));
   par.tok.cur = src;
   par.tok.start = src;
   par.tok.line = 1;
   par.buf_size = 1024;
   par.buf = malloc(par.buf_size);
   par.heap = heap;
   par.script = script;
   par.long_jumps = long_jumps;
   par.long_func_refs = long_func_refs;
   par.load_func = load_func;
   par.load_data = load_data;
   par.fname = fname;
   par.max_immediate_stack = MAX_IMMEDIATE_STACK;
   if (reload) {
      par.old_script = string_hash_get(&heap->scripts, fname);
      script->old_script = par.old_script;
   }

   if (reuse_tokens && reuse_tokens->tokens_src) {
      par.tokens_src = reuse_tokens->tokens_src;
      par.tokens_arr = reuse_tokens->tokens_arr;
      par.tokens_end = reuse_tokens->tokens_end;
      par.tokens_src_val = reuse_tokens->tokens_src_val;
      par.tokens_arr_val = reuse_tokens->tokens_arr_val;
   
      par.tok.tokens_src = par.tokens_src;
      par.tok.cur_token = par.tokens_arr + (reuse_tokens->semicolon_removed? 0 : TOK_SIZE);
      par.tok.tokens_end = par.tokens_end;
      par.tok.again = 0;
   }

   if (error) {
      *error = fixscript_int(0);
   }

   heap->cur_import_recursion++;

   prev_load_func = token_heap->cur_load_func;
   prev_load_data = token_heap->cur_load_data;
   prev_parser = token_heap->cur_parser;
   token_heap->cur_load_func = load_func;
   token_heap->cur_load_data = load_data;
   token_heap->cur_parser = &par;

   memset(&state, 0, sizeof(ScriptState));
   if (!parse_script(&par, error, &state)) {
      if (error && !error->value) {
         Constant *constant = string_hash_get(&par.script->constants, "stack_trace_lines");
         if (constant && constant->local) {
            char *custom_script_name = NULL;
            const char *error_fname = fname;
            int line = par.tok.line;
            process_stack_trace_lines(heap, constant->value, fixscript_int(0), &custom_script_name, &line);
            if (custom_script_name) {
               error_fname = custom_script_name;
            }
            tmp = string_format("%s(%d): %s", error_fname, line, par.tok.error);
            *error = fixscript_create_string(token_heap, tmp, -1);
            fixscript_ref(token_heap, *error);
            free(tmp);
            free(custom_script_name);
         }
         else {
            tmp = string_format("%s(%d): %s", fname, par.tok.line, par.tok.error);
            *error = fixscript_create_string(token_heap, tmp, -1);
            fixscript_ref(token_heap, *error);
            free(tmp);
         }
      }
      restore_script_state(&par, &state);
      free_script(script);
      script = NULL;
   }

   heap->cur_import_recursion--;

   token_heap->cur_load_func = prev_load_func;
   token_heap->cur_load_data = prev_load_data;
   token_heap->cur_parser = prev_parser;

   if (script) {
      for (i=0; i<par.func_refs.len; i+=3) {
         char *func_name = par.func_refs.data[i+0];
         int buf_off = (intptr_t)par.func_refs.data[i+1];
         int line_num = (intptr_t)par.func_refs.data[i+2];
         int empty = buf_off & (1<<31);
         int func_ref = line_num & (1<<31);
         Function *func = string_hash_get(&script->functions, func_name);
         int func_id;
         unsigned char *bc;

         buf_off &= ~(1<<31);
         line_num &= ~(1<<31);

         if (func) {
            if (buf_off == 0) {
               par.long_func_refs = 1;
               restore_script_state(&par, &state);
               free_script(script);
               script = NULL;
               break;
            }
            func_id = func->id;
            if (func_ref) {
               func_id += FUNC_REF_OFFSET;
            }
            memcpy(&par.buf[buf_off], &func_id, sizeof(int));
            if (!func_ref) {
               bc = &par.buf[buf_off+4];
               if (*bc == BC_CALL2_DIRECT || *bc == BC_CALL2_NATIVE) {
                  *bc = BC_CALL2_DIRECT;
               }
               else {
                  *bc = BC_CALL_DIRECT;
               }
            }
         }
         else if (empty) {
            #ifdef FIXEMBED_TOKEN_DUMP
            if (heap->token_dump_mode) {
               fixembed_native_function_used(func_name);
            }
            else
            #endif
            {
               if (error && !error->value) {
                  tmp = string_format("%s(%d): %s", fname, line_num, "undefined function");
                  *error = fixscript_create_string(token_heap, tmp, -1);
                  fixscript_ref(token_heap, *error);
                  free(tmp);
               }
               restore_script_state(&par, &state);
               free_script(script);
               script = NULL;
               break;
            }
         }
      }
   }

   if (script) {
      if (heap->bytecode_size + par.buf_len > (1<<23)) {
         if (error && !error->value) {
            tmp = string_format("%s: maximum bytecode limit reached", fname);
            *error = fixscript_create_string(token_heap, tmp, -1);
            fixscript_ref(token_heap, *error);
            free(tmp);
         }
         restore_script_state(&par, &state);
         free_script(script);
         script = NULL;
      }
      else {
         new_bytecode = realloc(heap->bytecode, heap->bytecode_size + par.buf_len);
         if (!new_bytecode) {
            goto bytecode_out_of_memory;
         }
         heap->bytecode = new_bytecode;
         memcpy(heap->bytecode + heap->bytecode_size, par.buf, par.buf_len);
         heap->bytecode_size += par.buf_len;

         new_lines = realloc(heap->lines, (heap->lines_size + par.lines.len/2) * sizeof(LineEntry));
         if (!new_lines) {
            heap->bytecode_size -= par.buf_len;
            goto bytecode_out_of_memory;
         }
         heap->lines = new_lines;
         for (i=0; i<par.lines.len; i+=2) {
            heap->lines[heap->lines_size++] = (LineEntry) { (intptr_t)par.lines.data[i+0], (intptr_t)par.lines.data[i+1] };
         }

         #ifndef FIXSCRIPT_NO_JIT
            #ifdef FIXEMBED_TOKEN_DUMP
            if (!heap->token_dump_mode)
            #endif
            {
               jit_error = jit_compile(heap, state.functions_len);
               if (jit_error) {
                  heap->bytecode_size -= par.buf_len;
                  heap->lines_size -= par.lines.len/2;
                  if (error && !error->value) {
                     tmp = string_format("%s: %s", fname, jit_error);
                     *error = fixscript_create_string(token_heap, tmp, -1);
                     fixscript_ref(token_heap, *error);
                     free(tmp);
                  }
                  goto bytecode_error;
               }
            }
         #endif

         if (reload) {
            tmp = string_format("fixscript:reload/%d", heap->reload_counter++);
            string_hash_set(&heap->scripts, tmp, script);
         }
         else {
            string_hash_set(&heap->scripts, strdup(fname), script);
         }

         if (0) {
bytecode_out_of_memory:
            if (error && !error->value) {
               tmp = string_format("%s: out of memory", fname);
               *error = fixscript_create_string(token_heap, tmp, -1);
               fixscript_ref(token_heap, *error);
               free(tmp);
            }
#ifndef FIXSCRIPT_NO_JIT
bytecode_error:
#endif
            restore_script_state(&par, &state);
            free_script(script);
            script = NULL;
         }
      }
   }

   free(par.buf);
   free(par.lines.data);

   for (i=0; i<par.variables.size; i+=2) {
      if (par.variables.data[i+0]) {
         free(par.variables.data[i+0]);
      }
   }
   free(par.variables.data);

   for (i=0; i<par.const_strings.size; i+=2) {
      if (par.const_strings.data[i+0]) {
         free(par.const_strings.data[i+0]);
         if (!script) {
            val = (intptr_t)par.const_strings.data[i+1];
            if ((((uint32_t)val) & 0x80000000U) == 0) {
               heap->data[val & 0x7FFFFFFF].is_static = 0;
            }
         }
      }
   }
   free(par.const_strings.data);

   for (i=0; i<par.import_aliases.size; i+=2) {
      if (par.import_aliases.data[i+0]) {
         free(par.import_aliases.data[i+0]);
      }
   }
   free(par.import_aliases.data);

   free(par.break_jumps.data);
   free(par.continue_jumps.data);

   for (i=0; i<par.func_refs.len; i+=3) {
      free(par.func_refs.data[i+0]);
   }
   free(par.func_refs.data);

   if (!script && ((par.long_jumps && !long_jumps) || (par.long_func_refs && !long_func_refs))) {
      if (par.long_jumps) {
         par.long_func_refs = 1;
      }
      if (error) {
         fixscript_unref(token_heap, *error);
         *error = fixscript_int(0);
      }
      return load_script(heap, src, fname, error, par.long_jumps, par.long_func_refs, load_func, load_data, &par, reload);
   }
   
   free(par.tokens_src);
   free(par.tokens_arr);
   fixscript_unref(token_heap, par.tokens_src_val);
   fixscript_unref(token_heap, par.tokens_arr_val);

   if (!script) {
      collect_heap(heap, NULL);
   }

   if (error) {
      fixscript_unref(token_heap, *error);
   }
   return script;
}


Script *fixscript_load(Heap *heap, const char *src, const char *fname, Value *error, LoadScriptFunc load_func, void *load_data)
{
   return load_script(heap, src, fname, error, 0, 0, load_func, load_data, NULL, 0);
}


Script *fixscript_reload(Heap *heap, const char *src, const char *fname, Value *error, LoadScriptFunc load_func, void *load_data)
{
   Script *old_script, *new_script;
   Function *old_func, *new_func;
   int i;

   old_script = fixscript_get(heap, fname);
   if (!old_script) {
      return load_script(heap, src, fname, error, 0, 0, load_func, load_data, NULL, 0);
   }
   
   new_script = load_script(heap, src, fname, error, 0, 0, load_func, load_data, NULL, 1);
   if (!new_script) {
      return NULL;
   }

   for (i=0; i<new_script->locals.size; i+=2) {
      if (new_script->locals.data[i+0]) {
         if (string_hash_get(&old_script->locals, new_script->locals.data[i+0]) == 0) {
            string_hash_set(&old_script->locals, strdup(new_script->locals.data[i+0]), new_script->locals.data[i+1]);
         }
      }
   }

   for (i=0; i<new_script->functions.size; i+=2) {
      if (new_script->functions.data[i+0]) {
         old_func = string_hash_get(&old_script->functions, new_script->functions.data[i+0]);
         new_func = new_script->functions.data[i+1];
         if (old_func) {
            heap->functions.data[old_func->id] = new_func;
         }
      }
   }

   return old_script;
}


static int is_forbidden_name(const char *name, int len)
{
   char buf[4];
   int i;

   for (i=0; i<len; i++) {
      if (name[i] == '.') {
         len = i;
         break;
      }
   }

   if (len < 3 || len > 4) return 0;

   for (i=0; i<len; i++) {
      if (name[i] >= 'a' && name[i] <= 'z') {
         buf[i] = name[i] - 'a' + 'A';
      }
      else {
         buf[i] = name[i];
      }
   }

   if (len == 3 && memcmp(buf, "CON", 3) == 0) return 1;
   if (len == 3 && memcmp(buf, "PRN", 3) == 0) return 1;
   if (len == 3 && memcmp(buf, "AUX", 3) == 0) return 1;
   if (len == 3 && memcmp(buf, "NUL", 3) == 0) return 1;
   if (len == 4 && memcmp(buf, "COM", 3) == 0 && buf[3] >= '0' && buf[3] <= '9') return 1;
   if (len == 4 && memcmp(buf, "LPT", 3) == 0 && buf[3] >= '0' && buf[3] <= '9') return 1;
   return 0;
}


static int is_valid_path(const char *path)
{
   const char *cur = path;
   const char *last = path;

   if (*path == 0) return 0;

   for (cur = path; *cur; cur++) {
      if (*cur >= 'A' && *cur <= 'Z') continue;
      if (*cur >= 'a' && *cur <= 'z') continue;
      if (*cur >= '0' && *cur <= '9') continue;
      if (*cur == '-' || *cur == '_' || *cur == ' ') continue;

      if (*cur == '.') {
         if (cur == last) return 0;
         if (cur[1] == '/' || cur[1] == 0) return 0;
         continue;
      }
      
      if (*cur == '/') {
         if (cur == last) return 0;
         if (cur[1] == '/') return 0;
         if (is_forbidden_name(last, cur-last)) return 0;
         last = cur+1;
         continue;
      }

      return 0;
   }

   if (is_forbidden_name(last, cur-last)) return 0;
   return 1;
}


Script *fixscript_load_file(Heap *heap, const char *name, Value *error, const char *dirname)
{
#ifdef FIXEMBED_TOKEN_DUMP
   Heap *token_heap = heap->token_dump_mode? heap->token_heap : heap;
#else
   Heap *token_heap = heap;
#endif
   FILE *f = NULL;
   char *src = NULL, *new_src;
   int src_size = 0;
   int buf_size = 4096;
   char *buf = NULL, *tmp;
   int read;
   Script *script = NULL;
   char *sname = NULL, *fname = NULL;

   sname = string_format("%s.fix", name);
   script = fixscript_get(heap, sname);
   if (script) {
      free(sname);
      return script;
   }

   if (!is_valid_path(name)) {
      free(sname);
      if (error) {
         tmp = string_format("invalid script file name %s given", name);
         *error = fixscript_create_string(token_heap, tmp, -1);
         free(tmp);
      }
      return NULL;
   }

   fname = string_format("%s/%s.fix", dirname, name);
   f = fopen(fname, "rb");
   if (!f) {
      if (error) {
         tmp = string_format("script %s not found", name);
         *error = fixscript_create_string(token_heap, tmp, -1);
         free(tmp);
      }
      goto error;
   }

   buf = malloc(buf_size);

   while ((read = fread(buf, 1, buf_size, f)) > 0) {
      if (src_size > INT_MAX - (read + 1)) {
         if (error) {
            fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         }
         goto error;
      }
      new_src = realloc(src, src_size + read + 1);
      if (!new_src) {
         if (error) {
            fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         }
         goto error;
      }
      src = new_src;
      memcpy(src + src_size, buf, read);
      src_size += read;
   }

   if (ferror(f)) {
      if (error) {
         tmp = string_format("reading of script %s failed", name);
         *error = fixscript_create_string(token_heap, tmp, -1);
         free(tmp);
      }
      goto error;
   }
   fclose(f);
   f = NULL;

   if (src) {
      src[src_size] = 0;
   }
   else {
      src = strdup("");
   }
   
   script = fixscript_load(heap, src, sname, error, (LoadScriptFunc)fixscript_load_file, (void *)dirname);

error:
   free(sname);
   free(fname);
   free(buf);
   free(src);
   if (f) fclose(f);
   return script;
}


static int uncompress_script(const char *in, char **dest_out)
{
   char *out = NULL;
   int in_size, out_size;
   int in_idx = 0, out_idx = 0;
   int literal_len, match_len, match_off, amount;
   uint8_t token, b;
   uint16_t offset;

   memcpy(&in_size, &in[0], sizeof(int));
   memcpy(&out_size, &in[4], sizeof(int));
   in += 8;

   out = malloc(out_size+1);
   if (!out) goto error;

   while (in_idx < in_size) {
      token = in[in_idx++];

      literal_len = token >> 4;
      if (literal_len == 15) {
         do {
            if (in_idx >= in_size) goto error;
            b = in[in_idx++];
            literal_len += b;
            if (literal_len > out_size) goto error;
         }
         while (b == 255);
      }
      if (literal_len > 0) {
         if (in_idx + literal_len > in_size) goto error;
         if (out_idx + literal_len > out_size) goto error;
         memcpy(out + out_idx, in + in_idx, literal_len);
         in_idx += literal_len;
         out_idx += literal_len;
      }
      
      if (in_idx == in_size) break;

      if (in_idx+2 > in_size) goto error;
      memcpy(&offset, in + in_idx, 2);
      in_idx += 2;
      if (offset == 0) goto error;

      match_off = out_idx - offset;
      if (match_off < 0) goto error;

      match_len = (token & 0xF) + 4;
      if (match_len == 19) {
         do {
            if (in_idx >= in_size) goto error;
            b = in[in_idx++];
            match_len += b;
            if (match_len > out_size) goto error;
         }
         while (b == 255);
      }
      if (out_idx + match_len > out_size) goto error;

      if (match_off + match_len <= out_idx) {
         memcpy(out + out_idx, out + match_off, match_len);
         out_idx += match_len;
      }
      else {
         amount = out_idx - match_off;
         while (match_len > 0) {
            if (amount > match_len) {
               amount = match_len;
            }
            memcpy(out + out_idx, out + match_off, amount);
            out_idx += amount;
            match_len -= amount;
         }
      }
   }

   if (out_idx != out_size) goto error;

   out[out_size] = '\0';
   *dest_out = out;
   return 1;

error:
   free(out);
   return 0;
}


Script *fixscript_load_embed(Heap *heap, const char *name, Value *error, const char * const * const embed_files)
{
   const char * const *p;
   const char *src = NULL;
   Script *script;
   char *fname, *uncompressed = NULL, *tmp;

   fname = string_format("%s.fix", name);
   script = fixscript_get(heap, fname);
   if (script) {
      free(fname);
      return script;
   }

   for (p = embed_files; *p; p += 2) {
      if (!strcmp(*p, fname)) {
         src = *(p+1);
         break;
      }
   }

   if (!src) {
      free(fname);
      if (error) {
         tmp = string_format("script %s not found", name);
         *error = fixscript_create_string(heap, tmp, -1);
         free(tmp);
      }
      return NULL;
   }

   if ((uint8_t)src[0] == 0xFF) {
      if (!uncompress_script(src+1, &uncompressed)) {
         free(fname);
         if (error) {
            tmp = string_format("script %s cannot be uncompressed", name);
            *error = fixscript_create_string(heap, tmp, -1);
            free(tmp);
         }
         return NULL;
      }
      src = uncompressed;
   }

   script = fixscript_load(heap, src, fname, error, (LoadScriptFunc)fixscript_load_embed, (void *)embed_files);
   free(fname);
   free(uncompressed);
   return script;
}


Script *fixscript_resolve_existing(Heap *heap, const char *name, Value *error, void *data)
{
   Script *script;
   char *tmp;

   tmp = string_format("%s.fix", name);
   script = fixscript_get(heap, tmp);
   free(tmp);
   if (script) {
      return script;
   }

   if (error) {
      tmp = string_format("tried to load script %s.fix during resolving of function references with loading disabled", name);
      *error = fixscript_create_string(heap, tmp, -1);
      free(tmp);
   }
   return NULL;
}


Script *fixscript_get(Heap *heap, const char *fname)
{
   return string_hash_get(&heap->scripts, fname);
}


char *fixscript_get_script_name(Heap *heap, Script *script)
{
   const char *name;
   
   if (!script) {
      return NULL;
   }
   name = string_hash_find_name(&heap->scripts, script);
   if (!name) {
      return NULL;
   }
   return strdup(name);
}


Value fixscript_get_function(Heap *heap, Script *script, const char *func_name)
{
   Function *func;
   
   if (!script) {
      return fixscript_int(0);
   }
   func = string_hash_get(&script->functions, func_name);
   return (Value) {func? FUNC_REF_OFFSET + func->id : 0, func? 1:0};
}


int fixscript_get_function_list(Heap *heap, Script *script, char ***functions_out, int *count_out)
{
   StringHash *hash;
   char **functions;
   int i, cnt;
   
   if (!script) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   hash = &script->functions;
   cnt = 0;
   for (i=0; i<hash->size; i+=2) {
      if (hash->data[i+0] && hash->data[i+1]) {
         cnt++;
      }
   }

   functions = calloc(cnt, sizeof(char *));
   if (!functions) {
      return FIXSCRIPT_ERR_OUT_OF_MEMORY;
   }

   cnt = 0;
   for (i=0; i<hash->size; i+=2) {
      if (hash->data[i+0] && hash->data[i+1]) {
         functions[cnt] = strdup(hash->data[i+0]);
         if (!functions[cnt]) {
            for (cnt--; cnt>=0; cnt--) {
               free(functions[cnt]);
            }
            free(functions);
            return FIXSCRIPT_ERR_OUT_OF_MEMORY;
         }
         cnt++;
      }
   }

   *functions_out = functions;
   *count_out = cnt;
   return FIXSCRIPT_SUCCESS;
}


int fixscript_get_function_name(Heap *heap, Value func_val, char **script_name_out, char **func_name_out, int *num_params_out)
{
   int func_id = func_val.value - FUNC_REF_OFFSET;
   Function *func;
   const char *script_name, *func_name;

   if (!func_val.is_array || func_id < 1 || func_id >= heap->functions.len) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   func = heap->functions.data[func_id];
   script_name = string_hash_find_name(&heap->scripts, func->script);
   func_name = string_hash_find_name(&func->script->functions, func);
   if (!script_name || !func_name) {
      return FIXSCRIPT_ERR_INVALID_ACCESS;
   }

   if (script_name_out) {
      *script_name_out = strdup(script_name);
   }
   if (func_name_out) {
      *func_name_out = strdup(func_name);
   }
   if (num_params_out) {
      *num_params_out = func->num_params;
   }
   return FIXSCRIPT_SUCCESS;
}


int fixscript_is_func_ref(Heap *heap, Value func_ref)
{
   int func_id;

   if (!func_ref.is_array) {
      return 0;
   }
   if (func_ref.value < FUNC_REF_OFFSET) {
      return fixscript_get_handle(heap, func_ref, FUNC_REF_HANDLE_TYPE, NULL) != NULL;
   }

   func_id = func_ref.value - FUNC_REF_OFFSET;
   return (func_id > 0 && func_id < heap->functions.len);
}


static int expand_stack(Heap *heap)
{
   int new_cap;
   char *new_flags;
   void *new_data;
   
   if (heap->stack_cap >= heap->max_stack_size) {
      return 0;
   }
   new_cap = heap->stack_cap << 1;
   if (new_cap > heap->max_stack_size) {
      new_cap = heap->max_stack_size;
   }

   new_flags = realloc_array(heap->stack_flags, new_cap, sizeof(char));
   if (!new_flags) {
      return 0;
   }
   heap->stack_flags = new_flags;

   new_data = realloc_array(heap->stack_data, new_cap, sizeof(int));
   if (!new_data) {
      return 0;
   }
   heap->stack_data = new_data;

   heap->total_size += (int64_t)(new_cap - heap->stack_cap) * sizeof(char);
   heap->total_size += (int64_t)(new_cap - heap->stack_cap) * sizeof(int);

   heap->stack_cap = new_cap;
   return 1;
}


#ifndef JIT_RUN_CODE

static int emit_error(Heap *heap, const char *msg, int pc)
{
   Value msg_val, error_val;
   int error_pc, stack_base;

   msg_val = fixscript_create_string(heap, msg, -1);
   if (!msg_val.is_array) return -1;

   error_val = create_error(heap, msg_val, 0, pc);
   if (!error_val.is_array) return -1;
   
   error_pc = (intptr_t)heap->error_stack.data[heap->error_stack.len-2];
   stack_base = (intptr_t)heap->error_stack.data[heap->error_stack.len-1];
   heap->error_stack.len -= 2;

   while (stack_base+2 > heap->stack_cap) {
      if (!expand_stack(heap)) return -2;
   }
   heap->stack_len = stack_base+2;

   heap->stack_data[stack_base+0] = 0;
   heap->stack_flags[stack_base+0] = 0;

   heap->stack_data[stack_base+1] = error_val.value;
   heap->stack_flags[stack_base+1] = 1;

   return error_pc;
}


#ifdef FIXSCRIPT_ASYNC
static void auto_suspend_resume_func(void *data);
#endif


static int run_bytecode(Heap *heap, int pc)
{
   #ifdef FIXSCRIPT_ASYNC
      #define SAVE_DATA() \
         heap->instruction_counter = instruction_counter;
      #define RESTORE_DATA() \
         instruction_counter = heap->instruction_counter;
      #define INC_INSN_COUNT() \
         instruction_counter++;
   #else
      #define SAVE_DATA()
      #define RESTORE_DATA()
      #define INC_INSN_COUNT()
   #endif

#ifdef __GNUC__
   #define DUP2(a) a, a
   #define DUP4(a) DUP2(a), DUP2(a)
   #define DUP7(a) DUP4(a), DUP2(a), a
   #define DUP8(a) DUP4(a), DUP4(a)
   #define DUP16(a) DUP8(a), DUP8(a)
   #define DUP32(a) DUP16(a), DUP16(a)
   #define DUP64(a) DUP32(a), DUP32(a)
   static void *dispatch[256] = {
      &&op_pop,
      &&op_popn,
      &&op_loadn,
      &&op_storen,
      &&op_add,
      &&op_sub,
      &&op_mul,
      &&op_add_mod,
      &&op_sub_mod,
      &&op_mul_mod,
      &&op_div,
      &&op_rem,
      &&op_shl,
      &&op_shr,
      &&op_ushr,
      &&op_and,
      &&op_or,
      &&op_xor,
      &&op_lt,
      &&op_le,
      &&op_gt,
      &&op_ge,
      &&op_eq,
      &&op_ne,
      &&op_eq_value,
      &&op_eq_value,
      &&op_bitnot,
      &&op_lognot,
      &&op_inc,
      &&op_dec,
      &&op_float_add,
      &&op_float_sub,
      &&op_float_mul,
      &&op_float_div,
      &&op_float_lt,
      &&op_float_le,
      &&op_float_gt,
      &&op_float_ge,
      &&op_float_eq,
      &&op_float_ne,
      &&op_return,
      &&op_return2,
      &&op_call_direct,
      &&op_call_dynamic,
      &&op_call_native,
      &&op_call2_direct,
      &&op_call2_dynamic,
      &&op_call_native,
      &&op_clean_call2,
      &&op_create_array,
      &&op_create_hash,
      &&op_array_get,
      &&op_array_set,
      &&op_array_append,
      &&op_hash_get,
      &&op_hash_set,
      
      &&op_const_p8,
      &&op_const_n8,
      &&op_const_p16,
      &&op_const_n16,
      &&op_const_i32,
      &&op_const_f32,
      &&op_const,
      &&op_const,
      
      DUP32(&&op_const),

      &&op_branch0,
      DUP7(&&op_branch),
      &&op_jump0,
      DUP7(&&op_jump),
      &&op_branch_long,
      &&op_jump_long,
      &&op_loop_i8,
      &&op_loop_i16,
      &&op_loop_i32,

      &&op_load_local,
      &&op_store_local,
      &&op_switch,
      &&op_length,
      &&op_const_string,
      &&op_string_concat,
      &&op_unused,

      &&op_check_stack,
      &&op_extended,
      
      DUP2(&&op_const),

      DUP64(&&op_store),
      DUP64(&&op_load)
   };
   #undef DUP2
   #undef DUP4
   #undef DUP7
   #undef DUP8
   #undef DUP16
   #undef DUP32
   #undef DUP64
   static void *ext_dispatch[85] = {
      &&op_ext_min,
      &&op_ext_max,
      &&op_ext_clamp,
      &&op_ext_abs,
      &&op_ext_add32,
      &&op_ext_sub32,
      &&op_ext_add64,
      &&op_ext_sub64,
      &&op_ext_mul64,
      &&op_ext_umul64,
      &&op_ext_mul64_long,
      &&op_ext_div64,
      &&op_ext_udiv64,
      &&op_ext_rem64,
      &&op_ext_urem64,
      &&op_ext_float,
      &&op_ext_int,
      &&op_ext_fabs,
      &&op_ext_fmin,
      &&op_ext_fmax,
      &&op_ext_fclamp,
      &&op_ext_floor,
      &&op_ext_ceil,
      &&op_ext_round,
      &&op_ext_pow,
      &&op_ext_sqrt,
      &&op_ext_cbrt,
      &&op_ext_exp,
      &&op_ext_ln,
      &&op_ext_log2,
      &&op_ext_log10,
      &&op_ext_sin,
      &&op_ext_cos,
      &&op_ext_asin,
      &&op_ext_acos,
      &&op_ext_tan,
      &&op_ext_atan,
      &&op_ext_atan2,
      &&op_ext_dbl_float,
      &&op_ext_dbl_int,
      &&op_ext_dbl_conv_down,
      &&op_ext_dbl_conv_up,
      &&op_ext_dbl_add,
      &&op_ext_dbl_sub,
      &&op_ext_dbl_mul,
      &&op_ext_dbl_div,
      &&op_ext_dbl_cmp_lt,
      &&op_ext_dbl_cmp_le,
      &&op_ext_dbl_cmp_gt,
      &&op_ext_dbl_cmp_ge,
      &&op_ext_dbl_cmp_eq,
      &&op_ext_dbl_cmp_ne,
      &&op_ext_dbl_fabs,
      &&op_ext_dbl_fmin,
      &&op_ext_dbl_fmax,
      &&op_ext_dbl_fclamp,
      &&op_ext_dbl_fclamp_short,
      &&op_ext_dbl_floor,
      &&op_ext_dbl_ceil,
      &&op_ext_dbl_round,
      &&op_ext_dbl_pow,
      &&op_ext_dbl_sqrt,
      &&op_ext_dbl_cbrt,
      &&op_ext_dbl_exp,
      &&op_ext_dbl_ln,
      &&op_ext_dbl_log2,
      &&op_ext_dbl_log10,
      &&op_ext_dbl_sin,
      &&op_ext_dbl_cos,
      &&op_ext_dbl_asin,
      &&op_ext_dbl_acos,
      &&op_ext_dbl_tan,
      &&op_ext_dbl_atan,
      &&op_ext_dbl_atan2,
      &&op_ext_is_int,
      &&op_ext_is_float,
      &&op_ext_is_array,
      &&op_ext_is_string,
      &&op_ext_is_hash,
      &&op_ext_is_shared,
      &&op_ext_is_const,
      &&op_ext_is_funcref,
      &&op_ext_is_weakref,
      &&op_ext_is_handle,
      &&op_ext_check_time_limit
   };
   #define DISPATCH() INC_INSN_COUNT(); goto *dispatch[bc = *bytecode++];
   //#define DISPATCH() INC_INSN_COUNT(); bc = *bytecode++; printf("bc=%02X stack=%d\n", bc, stack_data - heap->stack_data); goto *dispatch[bc];
   #define EXT_DISPATCH() goto *ext_dispatch[*bytecode++];
#else
   #define DISPATCH() \
      INC_INSN_COUNT(); \
      switch (bc = *bytecode++) { \
         case 0x00: goto op_pop; \
         case 0x01: goto op_popn; \
         case 0x02: goto op_loadn; \
         case 0x03: goto op_storen; \
         case 0x04: goto op_add; \
         case 0x05: goto op_sub; \
         case 0x06: goto op_mul; \
         case 0x07: goto op_add_mod; \
         case 0x08: goto op_sub_mod; \
         case 0x09: goto op_mul_mod; \
         case 0x0A: goto op_div; \
         case 0x0B: goto op_rem; \
         case 0x0C: goto op_shl; \
         case 0x0D: goto op_shr; \
         case 0x0E: goto op_ushr; \
         case 0x0F: goto op_and; \
         case 0x10: goto op_or; \
         case 0x11: goto op_xor; \
         case 0x12: goto op_lt; \
         case 0x13: goto op_le; \
         case 0x14: goto op_gt; \
         case 0x15: goto op_ge; \
         case 0x16: goto op_eq; \
         case 0x17: goto op_ne; \
         case 0x18: goto op_eq_value; \
         case 0x19: goto op_eq_value; \
         case 0x1A: goto op_bitnot; \
         case 0x1B: goto op_lognot; \
         case 0x1C: goto op_inc; \
         case 0x1D: goto op_dec; \
         case 0x1E: goto op_float_add; \
         case 0x1F: goto op_float_sub; \
         case 0x20: goto op_float_mul; \
         case 0x21: goto op_float_div; \
         case 0x22: goto op_float_lt; \
         case 0x23: goto op_float_le; \
         case 0x24: goto op_float_gt; \
         case 0x25: goto op_float_ge; \
         case 0x26: goto op_float_eq; \
         case 0x27: goto op_float_ne; \
         case 0x28: goto op_return; \
         case 0x29: goto op_return2; \
         case 0x2A: goto op_call_direct; \
         case 0x2B: goto op_call_dynamic; \
         case 0x2C: goto op_call_native; \
         case 0x2D: goto op_call2_direct; \
         case 0x2E: goto op_call2_dynamic; \
         case 0x2F: goto op_call_native; \
         case 0x30: goto op_clean_call2; \
         case 0x31: goto op_create_array; \
         case 0x32: goto op_create_hash; \
         case 0x33: goto op_array_get; \
         case 0x34: goto op_array_set; \
         case 0x35: goto op_array_append; \
         case 0x36: goto op_hash_get; \
         case 0x37: goto op_hash_set; \
         \
         case 0x38: goto op_const_p8; \
         case 0x39: goto op_const_n8; \
         case 0x3A: goto op_const_p16; \
         case 0x3B: goto op_const_n16; \
         case 0x3C: goto op_const_i32; \
         case 0x3D: goto op_const_f32; \
         case 0x3E: goto op_const; \
         case 0x3F: goto op_const; \
         \
         case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: \
         case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: \
         case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: \
         case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: \
            goto op_const; \
         \
         case 0x60: goto op_branch0; \
         case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: \
            goto op_branch; \
         case 0x68: goto op_jump0; \
         case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F: \
            goto op_jump; \
         case 0x70: goto op_branch_long; \
         case 0x71: goto op_jump_long; \
         case 0x72: goto op_loop_i8; \
         case 0x73: goto op_loop_i16; \
         case 0x74: goto op_loop_i32; \
         \
         case 0x75: goto op_load_local; \
         case 0x76: goto op_store_local; \
         case 0x77: goto op_switch; \
         case 0x78: goto op_length; \
         case 0x79: goto op_const_string; \
         case 0x7A: goto op_string_concat; \
         case 0x7B: \
            goto op_unused; \
         \
         case 0x7C: goto op_check_stack; \
         case 0x7D: goto op_extended; \
         \
         case 0x7E: case 0x7F: \
            goto op_const; \
         \
         case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87: \
         case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F: \
         case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: \
         case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F: \
         case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5: case 0xA6: case 0xA7: \
         case 0xA8: case 0xA9: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF: \
         case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7: \
         case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF: \
            goto op_store; \
         case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5: case 0xC6: case 0xC7: \
         case 0xC8: case 0xC9: case 0xCA: case 0xCB: case 0xCC: case 0xCD: case 0xCE: case 0xCF: \
         case 0xD0: case 0xD1: case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD7: \
         case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF: \
         case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: case 0xE6: case 0xE7: \
         case 0xE8: case 0xE9: case 0xEA: case 0xEB: case 0xEC: case 0xED: case 0xEE: case 0xEF: \
         case 0xF0: case 0xF1: case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0xF7: \
         case 0xF8: case 0xF9: case 0xFA: case 0xFB: case 0xFC: case 0xFD: case 0xFE: case 0xFF: \
            goto op_load; \
      }
   #define EXT_DISPATCH() \
      switch (*bytecode++) { \
         case 0x00: goto op_ext_min; \
         case 0x01: goto op_ext_max; \
         case 0x02: goto op_ext_clamp; \
         case 0x03: goto op_ext_abs; \
         case 0x04: goto op_ext_add32; \
         case 0x05: goto op_ext_sub32; \
         case 0x06: goto op_ext_add64; \
         case 0x07: goto op_ext_sub64; \
         case 0x08: goto op_ext_mul64; \
         case 0x09: goto op_ext_umul64; \
         case 0x0A: goto op_ext_mul64_long; \
         case 0x0B: goto op_ext_div64; \
         case 0x0C: goto op_ext_udiv64; \
         case 0x0D: goto op_ext_rem64; \
         case 0x0E: goto op_ext_urem64; \
         case 0x0F: goto op_ext_float; \
         case 0x10: goto op_ext_int; \
         case 0x11: goto op_ext_fabs; \
         case 0x12: goto op_ext_fmin; \
         case 0x13: goto op_ext_fmax; \
         case 0x14: goto op_ext_fclamp; \
         case 0x15: goto op_ext_floor; \
         case 0x16: goto op_ext_ceil; \
         case 0x17: goto op_ext_round; \
         case 0x18: goto op_ext_pow; \
         case 0x19: goto op_ext_sqrt; \
         case 0x1A: goto op_ext_cbrt; \
         case 0x1B: goto op_ext_exp; \
         case 0x1C: goto op_ext_ln; \
         case 0x1D: goto op_ext_log2; \
         case 0x1E: goto op_ext_log10; \
         case 0x1F: goto op_ext_sin; \
         case 0x20: goto op_ext_cos; \
         case 0x21: goto op_ext_asin; \
         case 0x22: goto op_ext_acos; \
         case 0x23: goto op_ext_tan; \
         case 0x24: goto op_ext_atan; \
         case 0x25: goto op_ext_atan2; \
         case 0x26: goto op_ext_dbl_float; \
         case 0x27: goto op_ext_dbl_int; \
         case 0x28: goto op_ext_dbl_conv_down; \
         case 0x29: goto op_ext_dbl_conv_up; \
         case 0x2A: goto op_ext_dbl_add; \
         case 0x2B: goto op_ext_dbl_sub; \
         case 0x2C: goto op_ext_dbl_mul; \
         case 0x2D: goto op_ext_dbl_div; \
         case 0x2E: goto op_ext_dbl_cmp_lt; \
         case 0x2F: goto op_ext_dbl_cmp_le; \
         case 0x30: goto op_ext_dbl_cmp_gt; \
         case 0x31: goto op_ext_dbl_cmp_ge; \
         case 0x32: goto op_ext_dbl_cmp_eq; \
         case 0x33: goto op_ext_dbl_cmp_ne; \
         case 0x34: goto op_ext_dbl_fabs; \
         case 0x35: goto op_ext_dbl_fmin; \
         case 0x36: goto op_ext_dbl_fmax; \
         case 0x37: goto op_ext_dbl_fclamp; \
         case 0x38: goto op_ext_dbl_fclamp_short; \
         case 0x39: goto op_ext_dbl_floor; \
         case 0x3A: goto op_ext_dbl_ceil; \
         case 0x3B: goto op_ext_dbl_round; \
         case 0x3C: goto op_ext_dbl_pow; \
         case 0x3D: goto op_ext_dbl_sqrt; \
         case 0x3E: goto op_ext_dbl_cbrt; \
         case 0x3F: goto op_ext_dbl_exp; \
         case 0x40: goto op_ext_dbl_ln; \
         case 0x41: goto op_ext_dbl_log2; \
         case 0x42: goto op_ext_dbl_log10; \
         case 0x43: goto op_ext_dbl_sin; \
         case 0x44: goto op_ext_dbl_cos; \
         case 0x45: goto op_ext_dbl_asin; \
         case 0x46: goto op_ext_dbl_acos; \
         case 0x47: goto op_ext_dbl_tan; \
         case 0x48: goto op_ext_dbl_atan; \
         case 0x49: goto op_ext_dbl_atan2; \
         case 0x4A: goto op_ext_is_int; \
         case 0x4B: goto op_ext_is_float; \
         case 0x4C: goto op_ext_is_array; \
         case 0x4D: goto op_ext_is_string; \
         case 0x4E: goto op_ext_is_hash; \
         case 0x4F: goto op_ext_is_shared; \
         case 0x50: goto op_ext_is_const; \
         case 0x51: goto op_ext_is_funcref; \
         case 0x52: goto op_ext_is_weakref; \
         case 0x53: goto op_ext_is_handle; \
         case 0x54: goto op_ext_check_time_limit; \
      }
#endif

   unsigned char *bytecode;
   unsigned char bc;
   int *stack_data, *stack_end;
   char *stack_flags;
   Value params_on_stack[PARAMS_ON_STACK];
#ifdef FIXSCRIPT_ASYNC
   uint32_t instruction_counter = heap->instruction_counter;
#endif

   #define ENTER() \
      bytecode = &heap->bytecode[pc]; \
      stack_data = &heap->stack_data[heap->stack_len]; \
      stack_end = &heap->stack_data[heap->stack_cap]; \
      stack_flags = &heap->stack_flags[heap->stack_len]; \
      RESTORE_DATA();

   #define LEAVE() \
      pc = bytecode - heap->bytecode; \
      heap->stack_len = stack_data - heap->stack_data; \
      SAVE_DATA();

   #define ERROR(msg) \
      LEAVE(); \
      pc = emit_error(heap, msg, pc); \
      if (pc <= 0) { \
         return (pc < 0? 0 : 1); \
      } \
      ENTER(); \
      DISPATCH();
      
   ENTER();
   DISPATCH();
   for (;;) {
      op_pop: {
         stack_data--;
         stack_flags--;
         DISPATCH();
      }

      op_popn: {
         int val = stack_data[-1] + 1;
         stack_data -= val;
         stack_flags -= val;
         DISPATCH();
      }

      op_loadn: {
         int val = stack_data[-1];
         stack_data[-1] = stack_data[val];
         stack_flags[-1] = stack_flags[val];
         DISPATCH();
      }

      op_storen: {
         int val = stack_data[-1];
         stack_data[val] = stack_data[-2];
         stack_flags[val] = stack_flags[-2];
         stack_data -= 2;
         stack_flags -= 2;
         DISPATCH();
      }

      #define INT_OP(label, expr) \
      label: { \
         int val1 = stack_data[-2]; \
         int val2 = stack_data[-1]; \
         stack_data--; \
         stack_flags--; \
         stack_data[-1] = expr; \
         stack_flags[-1] = 0; \
         DISPATCH(); \
      }

      #define INT_CHECKED_OP(label, expr) \
      label: { \
         int64_t val1 = stack_data[-2]; \
         int64_t val2 = stack_data[-1]; \
         int64_t result = expr; \
         if (result < INT_MIN || result > INT_MAX) { \
            ERROR("integer overflow"); \
         } \
         stack_data--; \
         stack_flags--; \
         stack_data[-1] = (int)result; \
         stack_flags[-1] = 0; \
         DISPATCH(); \
      }

      INT_CHECKED_OP(op_add, val1 + val2)
      INT_CHECKED_OP(op_sub, val1 - val2)
      INT_CHECKED_OP(op_mul, val1 * val2)

      INT_OP(op_add_mod, ((unsigned int)val1) + ((unsigned int)val2))
      INT_OP(op_sub_mod, ((unsigned int)val1) - ((unsigned int)val2))
      INT_OP(op_mul_mod, ((unsigned int)val1) * ((unsigned int)val2))
      
      op_div: {
         int val1 = stack_data[-2];
         int val2 = stack_data[-1];
         if (val2 == 0) {
            ERROR("division by zero");
         }
         if (val2 == -1 && val1 == INT_MIN) {
            ERROR("integer overflow");
         }
         stack_data--;
         stack_flags--;
         stack_data[-1] = val1 / val2;
         stack_flags[-1] = 0;
         DISPATCH();
      }
      
      op_rem: {
         int val1 = stack_data[-2];
         int val2 = stack_data[-1];
         if (val2 == 0) {
            ERROR("division by zero");
         }
         if (val2 == -1 && val1 == INT_MIN) {
            ERROR("integer overflow");
         }
         stack_data--;
         stack_flags--;
         stack_data[-1] = val1 % val2;
         stack_flags[-1] = 0;
         DISPATCH();
      }

      INT_OP(op_shl,  val1 << (val2 & 31))
      INT_OP(op_shr,  val1 >> (val2 & 31))
      INT_OP(op_ushr, ((unsigned int)val1) >> (((unsigned int)val2) & 31))
      INT_OP(op_and,  val1 & val2)
      INT_OP(op_or,   val1 | val2)
      INT_OP(op_xor,  val1 ^ val2)

      INT_OP(op_lt, val1 < val2)
      INT_OP(op_le, val1 <= val2)
      INT_OP(op_gt, val1 > val2)
      INT_OP(op_ge, val1 >= val2)

      #define INT_CMP_OP(label, expr) \
      label: { \
         int val1 = stack_data[-2]; \
         int val2 = stack_data[-1]; \
         int is_array1 = stack_flags[-2]; \
         int is_array2 = stack_flags[-1]; \
         stack_data--; \
         stack_flags--; \
         stack_data[-1] = expr; \
         stack_flags[-1] = 0; \
         DISPATCH(); \
      }

      INT_CMP_OP(op_eq, val1 == val2 && is_array1 == is_array2)
      INT_CMP_OP(op_ne, val1 != val2 || is_array1 != is_array2)

      op_eq_value: {
         int val1 = stack_data[-2];
         int val2 = stack_data[-1];
         int is_array1 = stack_flags[-2];
         int is_array2 = stack_flags[-1];
         int ret = 1;
         stack_data--;
         stack_flags--;

         if ((is_array1 && !is_array2) || (!is_array1 && is_array2)) {
            ret = 0;
         }
         else if (val1 != val2) {
            LEAVE();
            if (!compare_values(heap, (Value) { val1, is_array1 }, heap, (Value) { val2, is_array2 }, MAX_COMPARE_RECURSION)) {
               ret = 0;
            }
            ENTER();
         }

         if (bc == BC_NE_VALUE) {
            ret = !ret;
         }
         
         stack_data[-1] = ret;
         stack_flags[-1] = 0;
         DISPATCH();
      }

      #define INT_UNARY_OP(label, expr) \
      label: { \
         int val = stack_data[-1]; \
         stack_data[-1] = expr; \
         stack_flags[-1] = 0; \
         DISPATCH(); \
      }
      
      INT_UNARY_OP(op_bitnot, ~val)
      INT_UNARY_OP(op_lognot, !val)

      op_inc: {
         int pos = (signed char)(*bytecode++);
         int val = stack_data[pos];
         if (val == INT_MAX) {
            ERROR("integer overflow");
         }
         stack_data[pos] = ((unsigned int)val) + 1U;
         stack_flags[pos] = 0;
         DISPATCH();
      }

      op_dec: {
         int pos = (signed char)(*bytecode++);
         int val = stack_data[pos];
         if (val == INT_MIN) {
            ERROR("integer overflow");
         }
         stack_data[pos] = ((unsigned int)val) - 1U;
         stack_flags[pos] = 0;
         DISPATCH();
      }

      #define FLOAT_OP(label, expr) \
      label: { \
         union { \
            unsigned int i; \
            float f; \
         } u; \
         float val1, val2; \
         u.i = stack_data[-2]; \
         val1 = u.f; \
         u.i = stack_data[-1]; \
         val2 = u.f; \
         stack_data--; \
         stack_flags--; \
         u.f = expr; \
         /* flush denormals to zero: */ \
         if ((u.i & (0xFF << 23)) == 0) { \
            u.i &= ~((1<<23)-1); \
         } \
         stack_data[-1] = u.i; \
         stack_flags[-1] = 1; \
         DISPATCH(); \
      }

      #define FLOAT_UNARY_OP(label, expr) \
      label: { \
         union { \
            unsigned int i; \
            float f; \
         } u; \
         float val; \
         u.i = stack_data[-1]; \
         val = u.f; \
         u.f = expr; \
         /* flush denormals to zero: */ \
         if ((u.i & (0xFF << 23)) == 0) { \
            u.i &= ~((1<<23)-1); \
         } \
         stack_data[-1] = u.i; \
         stack_flags[-1] = 1; \
         DISPATCH(); \
      }

      #define FLOAT_CMP_OP(label, expr) \
      label: { \
         union { \
            unsigned int i; \
            float f; \
         } u; \
         float val1, val2; \
         u.i = stack_data[-2]; \
         val1 = u.f; \
         u.i = stack_data[-1]; \
         val2 = u.f; \
         stack_data--; \
         stack_flags--; \
         stack_data[-1] = expr; \
         stack_flags[-1] = 0; \
         DISPATCH(); \
      }

      FLOAT_OP(op_float_add, val1 + val2)
      FLOAT_OP(op_float_sub, val1 - val2)
      FLOAT_OP(op_float_mul, val1 * val2)
      FLOAT_OP(op_float_div, val1 / val2)
      FLOAT_CMP_OP(op_float_lt, val1 < val2)
      FLOAT_CMP_OP(op_float_le, val1 <= val2)
      FLOAT_CMP_OP(op_float_gt, val1 > val2)
      FLOAT_CMP_OP(op_float_ge, val1 >= val2)
      FLOAT_CMP_OP(op_float_eq, val1 == val2)
      FLOAT_CMP_OP(op_float_ne, val1 != val2)
      
      op_return: {
         int num = stack_data[-1];
         int ret = stack_data[-2];
         int is_array = stack_flags[-2];
         int ret_pc;
         stack_data -= num+1;
         stack_flags -= num+1;
         ret_pc = stack_data[-1] & ~(1<<31);
         stack_data[-1] = ret;
         stack_flags[-1] = is_array;
         if (ret_pc == 0) {
            LEAVE();
            return 1;
         }
         bytecode = &heap->bytecode[ret_pc];
         DISPATCH();
      }
      
      op_return2: {
         int ret1 = stack_data[-2];
         int ret2 = stack_data[-1];
         int is_array1 = stack_flags[-2];
         int is_array2 = stack_flags[-1];
         int error_pc, stack_base;

         error_pc = (intptr_t)heap->error_stack.data[heap->error_stack.len-2];
         stack_base = (intptr_t)heap->error_stack.data[heap->error_stack.len-1];
         heap->error_stack.len -= 2;

         heap->stack_data[stack_base+0] = ret1;
         heap->stack_flags[stack_base+0] = is_array1;

         heap->stack_data[stack_base+1] = ret2;
         heap->stack_flags[stack_base+1] = is_array2;

         stack_data = &heap->stack_data[stack_base+2];
         stack_flags = &heap->stack_flags[stack_base+2];

         if (error_pc == 0) {
            LEAVE();
            return 1;
         }
         bytecode = &heap->bytecode[error_pc];
         DISPATCH();
      }

      op_call_direct: {
         int func_id = stack_data[-1];
         Function *func;
         func = heap->functions.data[func_id];
         pc = bytecode - heap->bytecode;
         stack_data[-func->num_params-2] = pc | (1<<31);
         stack_flags[-func->num_params-2] = 1;
         stack_data--;
         stack_flags--;
         bytecode = &heap->bytecode[func->addr];
         DISPATCH();
      }

      op_call_dynamic: {
         int num_params = stack_data[-1];
         int func_id = stack_data[-num_params-2] - FUNC_REF_OFFSET;
         int is_array = stack_flags[-num_params-2];
         Function *func;
         if (!is_array || func_id < 1 || func_id >= heap->functions.len) {
            ERROR("invalid function reference");
         }
         func = heap->functions.data[func_id];
         if (num_params != func->num_params) {
            ERROR("improper number of function parameters");
         }
         pc = bytecode - heap->bytecode;
         stack_data[-func->num_params-2] = pc | (1<<31);
         stack_flags[-func->num_params-2] = 1;
         stack_data--;
         stack_flags--;
         bytecode = &heap->bytecode[func->addr];
         DISPATCH();
      }

      op_call_native: {
         Value ret, error, *params;
         NativeFunction *nfunc;
         int i, base, nfunc_id;
         int error_pc, stack_base;
         int stack_len = stack_data - heap->stack_data;
         
         pc = bytecode - heap->bytecode;
         nfunc_id = stack_data[-1];
         nfunc = heap->native_functions.data[nfunc_id];
         if (bc == BC_CALL2_NATIVE) {
            dynarray_add(&heap->error_stack, (void *)(intptr_t)(pc+1));
            dynarray_add(&heap->error_stack, (void *)(intptr_t)(stack_len - nfunc->num_params-2));
         }
         base = stack_len - nfunc->num_params - 1;
         heap->stack_data[base-1] = pc | (1<<31);
         heap->stack_flags[base-1] = 1;
         stack_data[-1] = nfunc->bytecode_ident_pc | (1<<31);
         stack_flags[-1] = 1;
         
         params = nfunc->num_params > PARAMS_ON_STACK? malloc(nfunc->num_params * sizeof(Value)) : params_on_stack;
         for (i=0; i<nfunc->num_params; i++) {
            params[i].value = heap->stack_data[base+i];
            params[i].is_array = heap->stack_flags[base+i];
         }
         error = fixscript_int(0);
         LEAVE();
         #ifdef FIXSCRIPT_ASYNC
            dynarray_add(&heap->async_continuations, NULL);
         #endif
         ret = nfunc->func(heap, &error, nfunc->num_params, params, nfunc->data);
         if (nfunc->num_params > PARAMS_ON_STACK) {
            free(params);
         }
         clear_roots(heap);
         #ifdef FIXSCRIPT_ASYNC
         {
            ResumeContinuation *cont;

            cont = heap->async_continuations.data[heap->async_continuations.len-1];
            if (cont) {
               cont->set_stack_len = base;
               return -pc;
            }
            else {
               heap->async_continuations.len--;
               if (heap->async_ret) {
                  heap->async_ret = 0;
                  ret = heap->async_ret_result;
                  error = heap->async_ret_error;
               }
            }
         }
         #endif
         ENTER();
         
         if (error.value) {
            error_pc = (intptr_t)heap->error_stack.data[heap->error_stack.len-2];
            stack_base = (intptr_t)heap->error_stack.data[heap->error_stack.len-1];
            heap->error_stack.len -= 2;

            heap->stack_data[stack_base+0] = ret.value;
            heap->stack_flags[stack_base+0] = ret.is_array;

            heap->stack_data[stack_base+1] = error.value;
            heap->stack_flags[stack_base+1] = error.is_array;

            stack_data = &heap->stack_data[stack_base+2];
            stack_flags = &heap->stack_flags[stack_base+2];

            if (error_pc == 0) {
               LEAVE();
               return 1;
            }
            bytecode = &heap->bytecode[error_pc];
         }
         else {
            stack_data = &heap->stack_data[base];
            stack_flags = &heap->stack_flags[base];
            stack_data[-1] = ret.value;
            stack_flags[-1] = ret.is_array;
         }
         DISPATCH();
      }

      op_call2_direct: {
         int func_id = stack_data[-1];
         int stack_len = stack_data - heap->stack_data;
         Function *func;
         func = heap->functions.data[func_id];
         pc = bytecode - heap->bytecode;
         stack_data[-func->num_params-2] = pc | (1<<31);
         stack_flags[-func->num_params-2] = 1;
         dynarray_add(&heap->error_stack, (void *)(intptr_t)(pc+1));
         dynarray_add(&heap->error_stack, (void *)(intptr_t)(stack_len - func->num_params-2));
         stack_data--;
         stack_flags--;
         bytecode = &heap->bytecode[func->addr];
         DISPATCH();
      }

      op_call2_dynamic: {
         int num_params = stack_data[-1];
         int func_id = stack_data[-num_params-2] - FUNC_REF_OFFSET;
         int is_array = stack_flags[-num_params-2];
         int stack_len = stack_data - heap->stack_data;
         Function *func;
         if (!is_array || func_id < 1 || func_id >= heap->functions.len) {
            ERROR("invalid function reference");
         }
         func = heap->functions.data[func_id];
         if (num_params != func->num_params) {
            ERROR("improper number of function parameters");
         }
         pc = bytecode - heap->bytecode;
         stack_data[-func->num_params-2] = pc | (1<<31);
         stack_flags[-func->num_params-2] = 1;
         dynarray_add(&heap->error_stack, (void *)(intptr_t)(pc+1));
         dynarray_add(&heap->error_stack, (void *)(intptr_t)(stack_len - func->num_params-2));
         stack_data--;
         stack_flags--;
         bytecode = &heap->bytecode[func->addr];
         DISPATCH();
      }

      op_clean_call2: {
         // note: no need to expand stack as there was at least 1 more value added during the preceding call
         *stack_data++ = 0;
         *stack_flags++ = 0;
         heap->error_stack.len -= 2;
         DISPATCH();
      }
      
      op_create_array: {
         int i, num, base;
         unsigned int val, max_value = 0;
         int stack_len = stack_data - heap->stack_data;
         Value arr_val;
         Array *arr;
         
         num = stack_data[-1];
         base = stack_len - (num+1);
         for (i=0; i<num; i++) {
            val = heap->stack_data[base+i];
            if ((unsigned int)val > max_value) {
               max_value = val;
            }
         }
         LEAVE();
         arr_val = create_array(heap, max_value <= 0xFF? ARR_BYTE : max_value <= 0xFFFF? ARR_SHORT : ARR_INT, num);
         if (!arr_val.is_array) {
            ERROR("out of memory");
         }
         ENTER();
         arr = &heap->data[arr_val.value];
         arr->len = num;
         for (i=0; i<num; i++) {
            set_array_value(arr, i, heap->stack_data[base+i]);
            ASSIGN_IS_ARRAY(arr, i, heap->stack_flags[base+i]);
         }
         heap->stack_data[base] = arr_val.value;
         heap->stack_flags[base] = 1;
         stack_data = &heap->stack_data[base+1];
         stack_flags = &heap->stack_flags[base+1];
         DISPATCH();
      }

      op_create_hash: {
         Value hash_val;
         Value key, value;
         int i, num, base, err;
         int stack_len = stack_data - heap->stack_data;
         
         num = stack_data[-1];
         base = stack_len - (num*2+1);
         LEAVE();
         hash_val = create_hash(heap);
         if (!hash_val.is_array) {
            ERROR("out of memory");
         }
         for (i=0; i<num; i++) {
            key = (Value) { heap->stack_data[base+i*2+0], heap->stack_flags[base+i*2+0] };
            value = (Value) { heap->stack_data[base+i*2+1], heap->stack_flags[base+i*2+1] };
            err = fixscript_set_hash_elem(heap, hash_val, key, value);
            if (err != FIXSCRIPT_SUCCESS) {
               ERROR(fixscript_get_error_msg(err));
            }
         }
         ENTER();
         heap->stack_data[base] = hash_val.value;
         heap->stack_flags[base] = 1;
         stack_data = &heap->stack_data[base+1];
         stack_flags = &heap->stack_flags[base+1];
         DISPATCH();
      }

      op_array_get: {
         Array *arr;
         int arr_val = stack_data[-2];
         int arr_is_array = stack_flags[-2];
         int idx = stack_data[-1];
         stack_data--;
         stack_flags--;

         if (!arr_is_array || arr_val <= 0 || arr_val >= heap->size) {
            ERROR("invalid array access");
         }

         arr = &heap->data[arr_val];
         if (arr->len == -1 || arr->hash_slots >= 0) {
            ERROR("invalid array access");
         }

         if (idx < 0 || idx >= arr->len) {
            ERROR("array out of bounds access");
         }

         stack_data[-1] = get_array_value(arr, idx);
         stack_flags[-1] = IS_ARRAY(arr, idx) != 0;
         DISPATCH();
      }

      op_array_set: {
         Array *arr;
         int arr_val = stack_data[-3];
         int arr_is_array = stack_flags[-3];
         int idx = stack_data[-2];
         int value = stack_data[-1];
         int value_is_array = stack_flags[-1];
         int err;
         stack_data -= 3;
         stack_flags -= 3;

         if (!arr_is_array || arr_val <= 0 || arr_val >= heap->size) {
            ERROR("invalid array access");
         }

         arr = &heap->data[arr_val];
         if (arr->len == -1 || arr->hash_slots >= 0) {
            ERROR("invalid array access");
         }

         if (arr->is_const) {
            ERROR("write access to constant string");
         }

         if (arr->is_shared && value_is_array && ((unsigned int)value) > 0 && ((unsigned int)value) < (1 << 23)) {
            ERROR("invalid shared array operation");
         }

         if (idx < 0 || idx >= arr->len) {
            ERROR("array out of bounds access");
         }

         if (ARRAY_NEEDS_UPGRADE(arr, value)) {
            LEAVE();
            err = upgrade_array(heap, arr, arr_val, value);
            ENTER();
            if (err != FIXSCRIPT_SUCCESS) {
               if (err == FIXSCRIPT_ERR_INVALID_SHARED_ARRAY_OPERATION) {
                  ERROR("invalid shared array operation");
               }
               else {
                  ERROR("out of memory");
               }
            }
         }

         if (!arr->is_shared) {
            ASSIGN_IS_ARRAY(arr, idx, value_is_array);
         }
         set_array_value(arr, idx, value);
         DISPATCH();
      }

      op_array_append: {
         Array *arr;
         int arr_val = stack_data[-2];
         int arr_is_array = stack_flags[-2];
         int value = stack_data[-1];
         int value_is_array = stack_flags[-1];
         int err;
         stack_data -= 2;
         stack_flags -= 2;

         if (!arr_is_array || arr_val <= 0 || arr_val >= heap->size) {
            ERROR("invalid array access");
         }

         arr = &heap->data[arr_val];
         if (arr->len == -1 || arr->hash_slots >= 0) {
            ERROR("invalid array access");
         }

         if (arr->is_const) {
            ERROR("write access to constant string");
         }

         if (arr->is_shared) {
            ERROR("invalid shared array operation");
         }

         if (ARRAY_NEEDS_UPGRADE(arr, value)) {
            LEAVE();
            err = upgrade_array(heap, arr, arr_val, value);
            ENTER();
            if (err != FIXSCRIPT_SUCCESS) {
               ERROR("out of memory");
            }
         }

         if (arr->len == arr->size) {
            int err;
            LEAVE();
            err = expand_array(heap, arr, arr->len);
            ENTER();
            if (err) {
               ERROR("out of memory");
            }
         }

         ASSIGN_IS_ARRAY(arr, arr->len, value_is_array);
         set_array_value(arr, arr->len++, value);
         DISPATCH();
      }

      op_hash_get: {
         Array *arr;
         int hash_val = stack_data[-2];
         int hash_is_array = stack_flags[-2];
         int key_val = stack_data[-1];
         int key_is_array = stack_flags[-1];
         int err;
         Value value;
         stack_data--;
         stack_flags--;

         if (!hash_is_array || hash_val <= 0 || hash_val >= heap->size) {
            ERROR("invalid hash access");
         }

         arr = &heap->data[hash_val];
         if (arr->len == -1 || arr->hash_slots < 0 || arr->is_handle) {
            ERROR("invalid hash access");
         }

         LEAVE();
         err = get_hash_elem(heap, arr, heap, (Value) { key_val, key_is_array }, &value);
         ENTER();
         if (err) {
            ERROR(fixscript_get_error_msg(err));
         }

         stack_data[-1] = value.value;
         stack_flags[-1] = value.is_array;
         DISPATCH();
      }

      op_hash_set: {
         Array *arr;
         int hash_val = stack_data[-3];
         int hash_is_array = stack_flags[-3];
         int key_val = stack_data[-2];
         int key_is_array = stack_flags[-2];
         int value = stack_data[-1];
         int value_is_array = stack_flags[-1];
         int err;
         stack_data -= 3;
         stack_flags -= 3;

         if (!hash_is_array || hash_val <= 0 || hash_val >= heap->size) {
            ERROR("invalid hash access");
         }

         arr = &heap->data[hash_val];
         if (arr->len == -1 || arr->hash_slots < 0 || arr->is_handle) {
            ERROR("invalid hash access");
         }

         LEAVE();
         err = fixscript_set_hash_elem(heap, (Value) { hash_val, hash_is_array }, (Value) { key_val, key_is_array }, (Value) { value, value_is_array });
         ENTER();
         if (err) {
            ERROR(fixscript_get_error_msg(err));
         }
         DISPATCH();
      }

      op_const_p8: {
         int val = (int)(*bytecode++) + 1;
         if (stack_data == stack_end) {
            ERROR("internal error: bad maximum stack computation");
         }
         *stack_data++ = val;
         *stack_flags++ = 0;
         DISPATCH();
      }

      op_const_n8: {
         int val = -((int)(*bytecode++)+1);
         if (stack_data == stack_end) {
            ERROR("internal error: bad maximum stack computation");
         }
         *stack_data++ = val;
         *stack_flags++ = 0;
         DISPATCH();
      }

      op_const_p16: {
         unsigned short short_val;
         int val;
         memcpy(&short_val, bytecode, sizeof(unsigned short));
         bytecode += 2;
         val = (int)short_val + 1;
         if (stack_data == stack_end) {
            ERROR("internal error: bad maximum stack computation");
         }
         *stack_data++ = val;
         *stack_flags++ = 0;
         DISPATCH();
      }

      op_const_n16: {
         unsigned short short_val;
         int val;
         memcpy(&short_val, bytecode, sizeof(unsigned short));
         bytecode += 2;
         val = -((int)short_val + 1);
         if (stack_data == stack_end) {
            ERROR("internal error: bad maximum stack computation");
         }
         *stack_data++ = val;
         *stack_flags++ = 0;
         DISPATCH();
      }

      op_const_i32: {
         int val;
         memcpy(&val, bytecode, sizeof(int));
         bytecode += 4;
         if (stack_data == stack_end) {
            ERROR("internal error: bad maximum stack computation");
         }
         *stack_data++ = val;
         *stack_flags++ = 0;
         DISPATCH();
      }

      op_const_f32: {
         unsigned int val;
         memcpy(&val, bytecode, sizeof(int));
         bytecode += 4;
         if (stack_data == stack_end) {
            ERROR("internal error: bad maximum stack computation");
         }
         *stack_data++ = val;
         *stack_flags++ = 1;
         DISPATCH();
      }

      op_const: {
         int val = (int)bc - 0x3F;
         if (stack_data == stack_end) {
            ERROR("internal error: bad maximum stack computation");
         }
         *stack_data++ = val;
         *stack_flags++ = 0;
         DISPATCH();
      }

      op_branch0: {
         int val = stack_data[-1];
         int inc = (int)(*bytecode++);
         stack_data--;
         stack_flags--;
         if (!val) {
            bytecode += inc;
         }
         DISPATCH();
      }

      op_jump0: {
         int inc = (int)(*bytecode++);
         bytecode += inc;
         DISPATCH();
      }

      op_branch: {
         int val = stack_data[-1];
         int inc = (((int)bc & 7) << 8) | (int)(*bytecode++);
         stack_data--;
         stack_flags--;
         if (!val) {
            bytecode += inc;
         }
         DISPATCH();
      }

      op_jump: {
         int inc = (((int)bc & 7) << 8) | (int)(*bytecode++);
         bytecode += inc;
         DISPATCH();
      }

      op_branch_long: {
         int val = stack_data[-1];
         int inc;
         stack_data--;
         stack_flags--;
         memcpy(&inc, bytecode, sizeof(int));
         bytecode += 4;
         if (!val) {
            bytecode += inc;
         }
         DISPATCH();
      }

      op_jump_long: {
         int inc;
         memcpy(&inc, bytecode, sizeof(int));
         bytecode += 4 + inc;
         DISPATCH();
      }

      op_loop_i8: {
         int dec = (int)(*bytecode);
         bytecode -= dec;
         DISPATCH();
      }

      op_loop_i16: {
         unsigned short dec;
         memcpy(&dec, bytecode, sizeof(unsigned short));
         bytecode -= (int)dec;
         DISPATCH();
      }

      op_loop_i32: {
         int dec;
         memcpy(&dec, bytecode, sizeof(int));
         bytecode -= dec;
         DISPATCH();
      }

      op_load_local: {
         int idx;
         memcpy(&idx, bytecode, sizeof(int));
         bytecode += 4;
         if (stack_data == stack_end) {
            ERROR("internal error: bad maximum stack computation");
         }
         *stack_data++ = heap->locals_data[idx];
         *stack_flags++ = heap->locals_flags[idx];
         DISPATCH();
      }

      op_store_local: {
         int val = stack_data[-1];
         int is_array = stack_flags[-1];
         int idx;
         
         stack_data--;
         stack_flags--;
         memcpy(&idx, bytecode, sizeof(int));
         bytecode += 4;
         heap->locals_data[idx] = val;
         heap->locals_flags[idx] = is_array;
         DISPATCH();
      }

      op_switch: {
         int val = stack_data[-1];
         int table_idx;
         int *table;
         int size, default_pc, case_pc;
         int i;

         memcpy(&table_idx, bytecode, sizeof(int));
         bytecode += 4;
         table = &((int *)heap->bytecode)[table_idx];
         size = table[-2];
         default_pc = table[-1];
         bytecode = &heap->bytecode[default_pc];
         if (size > 0) {
            // TODO: change to binary search
            for (i=size-1; i>=0; i--) {
               if (val >= table[i*2+0]) {
                  case_pc = table[i*2+1];
                  if (case_pc == 0) {
                     if (val != table[i*2+0]) {
                        break;
                     }
                     bytecode = &heap->bytecode[-table[(i-1)*2+1]];
                  }
                  else if (case_pc < 0) {
                     bytecode = &heap->bytecode[-case_pc];
                  }
                  else if (val == table[i*2+0]) {
                     bytecode = &heap->bytecode[case_pc];
                  }
                  break;
               }
            }
         }
         stack_data--;
         stack_flags--;
         DISPATCH();
      }

      op_length: {
         Array *arr;
         int arr_val = stack_data[-1];
         int arr_is_array = stack_flags[-1];

         if (!arr_is_array || arr_val <= 0 || arr_val >= heap->size) {
            ERROR("invalid array or hash access");
         }

         arr = &heap->data[arr_val];
         if (arr->len == -1) {
            ERROR("invalid array or hash access");
         }

         stack_data[-1] = arr->len;
         stack_flags[-1] = 0;
         DISPATCH();
      }

      op_const_string: {
         stack_flags[-1] = 1;
         DISPATCH();
      }

      op_string_concat: {
         Value value, result;
         int i, num, base, total_len = 0, len, err;
         struct {
            char *str;
            int len;
         } *strings = NULL;
         char *s;
         int stack_len = stack_data - heap->stack_data;

         num = stack_data[-1];
         base = stack_len - (num+1);

         strings = calloc(num, sizeof(*strings));
         if (!strings) {
            ERROR("out of memory");
         }

         LEAVE();

         for (i=0; i<num; i++) {
            value = (Value) { heap->stack_data[base+i], heap->stack_flags[base+i] };
            if (!fixscript_is_string(heap, value)) {
               err = fixscript_to_string(heap, value, 0, &strings[i].str, &len);
            }
            else {
               err = fixscript_get_string(heap, value, 0, -1, &strings[i].str, &len);
            }
            if (err || len > (INT_MAX - total_len)) {
               for (i=0; i<num; i++) {
                  free(strings[i].str);
               }
               free(strings);
               ENTER();
               ERROR(fixscript_get_error_msg(err));
            }
            strings[i].len = len;
            total_len += len;
         }

         heap->stack_len = base;

         s = malloc(total_len);
         if (!s) {
            for (i=0; i<num; i++) {
               free(strings[i].str);
            }
            free(strings);
            ENTER();
            ERROR("out of memory");
         }

         for (i=0, len=0; i<num; i++) {
            memcpy(s + len, strings[i].str, strings[i].len);
            len += strings[i].len;
         }

         result = fixscript_create_string(heap, s, total_len);

         for (i=0; i<num; i++) {
            free(strings[i].str);
         }
         free(strings);
         free(s);

         if (!result.value) {
            return 0;
         }

         ENTER();

         heap->stack_data[base] = result.value;
         heap->stack_flags[base] = result.is_array;
         stack_data++;
         stack_flags++;
         DISPATCH();
      }

      op_store: {
         int pos = (signed char)(bc) + 0x40;
         stack_data[pos] = stack_data[-1];
         stack_flags[pos] = stack_flags[-1];
         stack_data--;
         stack_flags--;
         DISPATCH();
      }

      op_load: {
         int pos = (signed char)bc;
         if (stack_data == stack_end) {
            ERROR("internal error: bad maximum stack computation");
         }
         stack_data[0] = stack_data[pos];
         stack_flags[0] = stack_flags[pos];
         stack_data++;
         stack_flags++;
         DISPATCH();
      }

      op_check_stack: {
         uint16_t val;
         memcpy(&val, bytecode, sizeof(uint16_t));
         bytecode += 2;
         while (stack_data + val > stack_end) {
            LEAVE();
            if (!expand_stack(heap)) {
               return 0;
            }
            ENTER();
         }
         DISPATCH();
      }

      op_extended: {
         EXT_DISPATCH()
      }
 
      op_unused: {
         LEAVE();
         return 0;
      }

      // extended bytecodes:

      INT_OP(op_ext_min, val1 < val2? val1 : val2)
      INT_OP(op_ext_max, val1 > val2? val1 : val2)

      op_ext_clamp: {
         int val1 = stack_data[-3];
         int val2 = stack_data[-2];
         int val3 = stack_data[-1];
         stack_data -= 2;
         stack_flags -= 2;
         stack_data[-1] = val1 < val2? val2 : (val1 > val3? val3 : val1);
         stack_flags[-1] = 0;
         DISPATCH();
      }

      op_ext_abs: {
         int val = stack_data[-1];
         if (val == INT_MIN) {
            ERROR("integer overflow");
         }
         stack_data[-1] = val < 0? -val : val;
         stack_flags[-1] = 0;
         DISPATCH();
      }
      
      #define INT_ADD32(label,expr) \
      label: { \
         uint64_t val1 = (uint32_t)stack_data[-3]; \
         uint64_t val2 = (uint32_t)stack_data[-2]; \
         uint64_t val3 = (uint32_t)stack_data[-1]; \
         uint64_t result = expr; \
         stack_data--; \
         stack_flags--; \
         stack_data[-2] = (int)result; \
         stack_data[-1] = (int)((result >> 32) & 1); \
         stack_flags[-2] = 0; \
         stack_flags[-1] = 0; \
         DISPATCH(); \
      }

      INT_ADD32(op_ext_add32, val1 + val2 + (val3 & 1))
      INT_ADD32(op_ext_sub32, val1 - val2 - (val3 & 1))
      
      #define INT_MUL64(label,type,expr) \
      label: { \
         type val1 = stack_data[-2]; \
         type val2 = stack_data[-1]; \
         type result = expr; \
         stack_data[-2] = (int)result; \
         stack_data[-1] = (int)(((uint64_t)result) >> 32); \
         stack_flags[-2] = 0; \
         stack_flags[-1] = 0; \
         DISPATCH(); \
      }

      INT_MUL64(op_ext_mul64, int64_t, val1 * val2)
      INT_MUL64(op_ext_umul64, uint64_t, (uint64_t)(uint32_t)val1 * (uint64_t)(uint32_t)val2)
      
      #define INT_OP64(label,type,expr,check) \
      label: { \
         type val1_lo = stack_data[-4]; \
         type val1_hi = stack_data[-3]; \
         type val2_lo = stack_data[-2]; \
         type val2_hi = stack_data[-1]; \
         type val1 = ((uint32_t)val1_lo) | (val1_hi << 32); \
         type val2 = ((uint32_t)val2_lo) | (val2_hi << 32); \
         check \
         type result = expr; \
         stack_data -= 2; \
         stack_flags -= 2; \
         stack_data[-2] = (int)result; \
         stack_data[-1] = (int)(((uint64_t)result) >> 32); \
         stack_flags[-2] = 0; \
         stack_flags[-1] = 0; \
         DISPATCH(); \
      }

      INT_OP64(op_ext_add64, uint64_t, val1 + val2, /* nothing */)
      INT_OP64(op_ext_sub64, uint64_t, val1 - val2, /* nothing */)
      INT_OP64(op_ext_mul64_long, int64_t, val1 * val2, /* nothing */)

      #define DIV_CHECK \
         if (val2 == 0) { \
            ERROR("division by zero"); \
         }

      #define DIV_OVERFLOW_CHECK \
         if (val2 == -1 && val1 == INT64_MIN) { \
            ERROR("integer overflow"); \
         }

      INT_OP64(op_ext_div64, int64_t, val1 / val2, DIV_CHECK DIV_OVERFLOW_CHECK)
      INT_OP64(op_ext_udiv64, uint64_t, val1 / val2, DIV_CHECK)
      INT_OP64(op_ext_rem64, int64_t, val1 % val2, DIV_CHECK DIV_OVERFLOW_CHECK)
      INT_OP64(op_ext_urem64, uint64_t, val1 % val2, DIV_CHECK)

      op_ext_float: {
         union {
            unsigned int i;
            float f;
         } u;
         u.f = (float)stack_data[-1];
         // flush denormals to zero:
         if ((u.i & (0xFF << 23)) == 0) {
            u.i &= ~((1<<23)-1);
         }
         stack_data[-1] = u.i;
         stack_flags[-1] = 1;
         DISPATCH();
      }

      op_ext_int: {
         union {
            int i;
            float f;
         } u;
         u.i = stack_data[-1];
         stack_data[-1] = (int)u.f;
         stack_flags[-1] = 0;
         DISPATCH();
      }

      FLOAT_UNARY_OP(op_ext_fabs, fabsf(val))
      FLOAT_OP(op_ext_fmin, fminf(val1, val2))
      FLOAT_OP(op_ext_fmax, fmaxf(val1, val2))

      op_ext_fclamp: {
         union {
            unsigned int i;
            float f;
         } u;
         float val1, val2, val3;
         u.i = stack_data[-3];
         val1 = u.f;
         u.i = stack_data[-2];
         val2 = u.f;
         u.i = stack_data[-1];
         val3 = u.f;
         stack_data -= 2;
         stack_flags -= 2;
         u.f = val1 < val2? val2 : (val1 > val3? val3 : val1);
         /* flush denormals to zero: */
         if ((u.i & (0xFF << 23)) == 0) {
            u.i &= ~((1<<23)-1);
         }
         stack_data[-1] = u.i;
         stack_flags[-1] = 1;
         DISPATCH();
      }

      FLOAT_UNARY_OP(op_ext_floor, floorf(val))
      FLOAT_UNARY_OP(op_ext_ceil, ceilf(val))
      FLOAT_UNARY_OP(op_ext_round, roundf(val))
      FLOAT_OP(op_ext_pow, powf(val1, val2))
      FLOAT_UNARY_OP(op_ext_sqrt, sqrtf(val))
      FLOAT_UNARY_OP(op_ext_cbrt, cbrtf(val))
      FLOAT_UNARY_OP(op_ext_exp, expf(val))
      FLOAT_UNARY_OP(op_ext_ln, logf(val))
      FLOAT_UNARY_OP(op_ext_log2, log2f(val))
      FLOAT_UNARY_OP(op_ext_log10, log10f(val))
      FLOAT_UNARY_OP(op_ext_sin, sinf(val))
      FLOAT_UNARY_OP(op_ext_cos, cosf(val))
      FLOAT_UNARY_OP(op_ext_asin, asinf(val))
      FLOAT_UNARY_OP(op_ext_acos, acosf(val))
      FLOAT_UNARY_OP(op_ext_tan, tanf(val))
      FLOAT_UNARY_OP(op_ext_atan, atanf(val))
      FLOAT_OP(op_ext_atan2, atan2f(val1, val2))

      op_ext_dbl_float: {
         union {
            uint64_t i;
            double f;
         } u;
         uint32_t lo, hi;
         lo = stack_data[-2];
         hi = stack_data[-1];
         u.f = (double)(int64_t)(((uint64_t)lo) | (((uint64_t)hi)<<32));
         stack_data[-2] = (uint32_t)u.i;
         stack_data[-1] = (uint32_t)(u.i>>32);
         stack_flags[-2] = 0;
         stack_flags[-1] = 0;
         DISPATCH();
      }

      op_ext_dbl_int: {
         union {
            uint64_t i;
            double f;
         } u;
         uint32_t lo, hi;
         int64_t result;
         lo = stack_data[-2];
         hi = stack_data[-1];
         u.i = (((uint64_t)lo) | (((uint64_t)hi)<<32));
         result = (int64_t)u.f;
         stack_data[-2] = (uint32_t)(uint64_t)result;
         stack_data[-1] = (uint32_t)(((uint64_t)result) >> 32);
         stack_flags[-2] = 0;
         stack_flags[-1] = 0;
         DISPATCH();
      }

      op_ext_dbl_conv_down: {
         union {
            unsigned int i;
            float f;
         } u32;
         union {
            uint64_t i;
            double f;
         } u;
         uint32_t lo, hi;
         lo = stack_data[-2];
         hi = stack_data[-1];
         u.i = ((uint64_t)lo) | (((uint64_t)hi)<<32);
         u32.f = (float)u.f;
         // flush denormals to zero:
         if ((u32.i & (0xFF << 23)) == 0) {
            u32.i &= ~((1<<23)-1);
         }
         stack_data[-2] = u32.i;
         stack_flags[-2] = 1;
         stack_data--;
         stack_flags--;
         DISPATCH();
      }

      op_ext_dbl_conv_up: {
         union {
            unsigned int i;
            float f;
         } u32;
         union {
            uint64_t i;
            double f;
         } u;
         if (stack_data == stack_end) {
            ERROR("internal error: bad maximum stack computation");
         }
         u32.i = stack_data[-1];
         u.f = u32.f;
         stack_data++;
         stack_flags++;
         stack_data[-2] = (uint32_t)u.i;
         stack_data[-1] = (uint32_t)(u.i >> 32);
         stack_flags[-2] = 0;
         stack_flags[-1] = 0;
         DISPATCH();
      }

      #define DOUBLE_OP(label, expr) \
      label: { \
         union { \
            uint64_t i; \
            double f; \
         } u; \
         uint32_t lo, hi; \
         double val1, val2; \
         lo = stack_data[-4]; \
         hi = stack_data[-3]; \
         u.i = ((uint64_t)lo) | (((uint64_t)hi)<<32); \
         val1 = u.f; \
         lo = stack_data[-2]; \
         hi = stack_data[-1]; \
         u.i = ((uint64_t)lo) | (((uint64_t)hi)<<32); \
         val2 = u.f; \
         stack_data -= 2; \
         stack_flags -= 2; \
         u.f = expr; \
         stack_data[-2] = (uint32_t)u.i; \
         stack_data[-1] = (uint32_t)(u.i >> 32); \
         stack_flags[-2] = 0; \
         stack_flags[-1] = 0; \
         DISPATCH(); \
      }

      #define DOUBLE_UNARY_OP(label, expr) \
      label: { \
         union { \
            uint64_t i; \
            double f; \
         } u; \
         uint32_t lo, hi; \
         double val; \
         lo = stack_data[-2]; \
         hi = stack_data[-1]; \
         u.i = ((uint64_t)lo) | (((uint64_t)hi)<<32); \
         val = u.f; \
         u.f = expr; \
         stack_data[-2] = (uint32_t)u.i; \
         stack_data[-1] = (uint32_t)(u.i >> 32); \
         stack_flags[-2] = 0; \
         stack_flags[-1] = 0; \
         DISPATCH(); \
      }

      #define DOUBLE_CMP_OP(label, expr) \
      label: { \
         union { \
            uint64_t i; \
            double f; \
         } u; \
         uint32_t lo, hi; \
         double val1, val2; \
         lo = stack_data[-4]; \
         hi = stack_data[-3]; \
         u.i = ((uint64_t)lo) | (((uint64_t)hi)<<32); \
         val1 = u.f; \
         lo = stack_data[-2]; \
         hi = stack_data[-1]; \
         u.i = ((uint64_t)lo) | (((uint64_t)hi)<<32); \
         val2 = u.f; \
         stack_data -= 3; \
         stack_flags -= 3; \
         stack_data[-1] = expr; \
         stack_flags[-1] = 0; \
         DISPATCH(); \
      }

      DOUBLE_OP(op_ext_dbl_add, val1 + val2)
      DOUBLE_OP(op_ext_dbl_sub, val1 - val2)
      DOUBLE_OP(op_ext_dbl_mul, val1 * val2)
      DOUBLE_OP(op_ext_dbl_div, val1 / val2)

      DOUBLE_CMP_OP(op_ext_dbl_cmp_lt, val1 < val2)
      DOUBLE_CMP_OP(op_ext_dbl_cmp_le, val1 <= val2)
      DOUBLE_CMP_OP(op_ext_dbl_cmp_gt, val1 > val2)
      DOUBLE_CMP_OP(op_ext_dbl_cmp_ge, val1 >= val2)
      DOUBLE_CMP_OP(op_ext_dbl_cmp_eq, val1 == val2)
      DOUBLE_CMP_OP(op_ext_dbl_cmp_ne, val1 != val2)

      DOUBLE_UNARY_OP(op_ext_dbl_fabs, fabs(val))
      DOUBLE_OP(op_ext_dbl_fmin, fmin(val1, val2))
      DOUBLE_OP(op_ext_dbl_fmax, fmax(val1, val2))

      op_ext_dbl_fclamp: {
         union {
            uint64_t i;
            double f;
         } u;
         uint32_t lo, hi;
         double val1, val2, val3;
         lo = stack_data[-6];
         hi = stack_data[-5];
         u.i = ((uint64_t)lo) | (((uint64_t)hi)<<32);
         val1 = u.f;
         lo = stack_data[-4];
         hi = stack_data[-3];
         u.i = ((uint64_t)lo) | (((uint64_t)hi)<<32);
         val2 = u.f;
         lo = stack_data[-2];
         hi = stack_data[-1];
         u.i = ((uint64_t)lo) | (((uint64_t)hi)<<32);
         val3 = u.f;
         stack_data -= 4;
         stack_flags -= 4;
         u.f = val1 < val2? val2 : (val1 > val3? val3 : val1);
         stack_data[-2] = (uint32_t)u.i;
         stack_data[-1] = (uint32_t)(u.i >> 32);
         stack_flags[-2] = 0;
         stack_flags[-1] = 0;
         DISPATCH();
      }

      op_ext_dbl_fclamp_short: {
         union {
            uint64_t i;
            double f;
         } u;
         union {
            uint32_t i;
            float f;
         } u2;
         uint32_t lo, hi;
         double val1, val2, val3;
         lo = stack_data[-4];
         hi = stack_data[-3];
         u.i = ((uint64_t)lo) | (((uint64_t)hi)<<32);
         val1 = u.f;
         u2.i = stack_data[-2];
         val2 = u2.f;
         u2.i = stack_data[-1];
         val3 = u2.f;
         stack_data -= 2;
         stack_flags -= 2;
         u.f = val1 < val2? val2 : (val1 > val3? val3 : val1);
         stack_data[-2] = (uint32_t)u.i;
         stack_data[-1] = (uint32_t)(u.i >> 32);
         stack_flags[-2] = 0;
         stack_flags[-1] = 0;
         DISPATCH();
      }

      DOUBLE_UNARY_OP(op_ext_dbl_floor, floor(val))
      DOUBLE_UNARY_OP(op_ext_dbl_ceil, ceil(val))
      DOUBLE_UNARY_OP(op_ext_dbl_round, round(val))
      DOUBLE_OP(op_ext_dbl_pow, pow(val1, val2))
      DOUBLE_UNARY_OP(op_ext_dbl_sqrt, sqrt(val))
      DOUBLE_UNARY_OP(op_ext_dbl_cbrt, cbrt(val))
      DOUBLE_UNARY_OP(op_ext_dbl_exp, exp(val))
      DOUBLE_UNARY_OP(op_ext_dbl_ln, log(val))
      DOUBLE_UNARY_OP(op_ext_dbl_log2, log2(val))
      DOUBLE_UNARY_OP(op_ext_dbl_log10, log10(val))
      DOUBLE_UNARY_OP(op_ext_dbl_sin, sin(val))
      DOUBLE_UNARY_OP(op_ext_dbl_cos, cos(val))
      DOUBLE_UNARY_OP(op_ext_dbl_asin, asin(val))
      DOUBLE_UNARY_OP(op_ext_dbl_acos, acos(val))
      DOUBLE_UNARY_OP(op_ext_dbl_tan, tan(val))
      DOUBLE_UNARY_OP(op_ext_dbl_atan, atan(val))
      DOUBLE_OP(op_ext_dbl_atan2, atan2(val1, val2))

      op_ext_is_int: {
         stack_data[-1] = stack_flags[-1] == 0;
         stack_flags[-1] = 0;
         DISPATCH();
      }

      op_ext_is_float: {
         unsigned int value = stack_data[-1];
         int is_array = stack_flags[-1];
         stack_data[-1] = is_array && (value == 0 || value >= (1 << 23));
         stack_flags[-1] = 0;
         DISPATCH();
      }

      op_ext_is_array:
      op_ext_is_string:
      op_ext_is_hash:
      op_ext_is_shared:
      op_ext_is_const:
      op_ext_is_funcref:
      op_ext_is_weakref:
      op_ext_is_handle: {
         Array *arr;
         int value = stack_data[-1];
         int is_array = stack_flags[-1];
         int result = 0;
         int ebc = bytecode[-1];
         int func_id;

         if (is_array && value > 0 && value < heap->size) {
            arr = &heap->data[value];
            if (arr->len != -1) {
               if (ebc == BC_EXT_IS_HANDLE) {
                  result = arr->is_handle != 0 && arr->type != FUNC_REF_HANDLE_TYPE;
               }
               else if (ebc == BC_EXT_IS_HASH) {
                  result = (arr->hash_slots >= 0 && !arr->is_handle);
               }
               else if (ebc == BC_EXT_IS_FUNCREF) {
                  result = arr->is_handle && arr->type == FUNC_REF_HANDLE_TYPE;
               }
               else if (ebc == BC_EXT_IS_WEAKREF) {
                  result = arr->is_handle && arr->type == WEAK_REF_HANDLE_TYPE;
               }
               else if (arr->hash_slots < 0) {
                  if (ebc == BC_EXT_IS_CONST) {
                     result = arr->is_const != 0;
                  }
                  else if (ebc == BC_EXT_IS_SHARED) {
                     result = arr->is_shared != 0;
                  }
                  else {
                     result = (ebc == BC_EXT_IS_STRING)? arr->is_string : 1;
                  }
               }
            }
         }
         else if (is_array && ebc == BC_EXT_IS_FUNCREF) {
            func_id = value - FUNC_REF_OFFSET;
            if (func_id > 0 && func_id < heap->functions.len) {
               result = 1;
            }
         }

         stack_data[-1] = result;
         stack_flags[-1] = 0;
         DISPATCH();
      }

      op_ext_check_time_limit: {
         uint64_t time = 0;
         int64_t diff;
         #ifdef FIXSCRIPT_ASYNC
            if (heap->auto_suspend_func && (int)(heap->instruction_limit - instruction_counter) <= 0) {
               heap->instruction_limit += (uint32_t)heap->auto_suspend_num_instructions;
               if (!heap->cur_load_func && heap->async_active) {
                  ResumeContinuation *cont;

                  if (!heap->async_active) {
                     ERROR("native error: improper async context");
                  }

                  cont = malloc(sizeof(ResumeContinuation));
                  cont->continue_pc = 0;
                  cont->set_stack_len = -1;
                  cont->auto_suspend_heap = heap;
                  dynarray_add(&heap->async_continuations, cont);

                  LEAVE();
                  heap->auto_suspend_func(auto_suspend_resume_func, cont, heap->auto_suspend_data);

                  cont = heap->async_continuations.data[heap->async_continuations.len-1];
                  if (cont) {
                     if (cont->set_stack_len != -1) {
                        heap->stack_len = cont->set_stack_len;
                     }
                     return -pc;
                  }
                  else {
                     heap->async_continuations.len--;
                  }
                  ENTER();
               }
            }
         #endif
         if (--heap->time_counter <= 0) {
            heap->time_counter = 1000;
            if (heap->stop_execution) {
               heap->time_counter = 0;
               ERROR("execution stop");
            }
            if (heap->time_limit != 0 && heap->time_limit != -1) {
               get_time(&time);
               diff = (int64_t)(heap->time_limit - time);
               if (diff <= 0) {
                  heap->time_counter = 0;
                  ERROR("execution time limit reached");
               }
            }
         }
         DISPATCH();
      }
   }

   LEAVE();
   return 1;

   #undef SAVE_DATA
   #undef RESTORE_DATA
   #undef INC_INSN_COUNT
   #undef DISPATCH
   #undef EXT_DISPATCH
   #undef ENTER
   #undef LEAVE
   #undef ERROR
   #undef INT_OP
   #undef INT_CHECKED_OP
   #undef INT_CMP_OP
   #undef INT_UNARY_OP
   #undef FLOAT_OP
   #undef FLOAT_UNARY_OP
   #undef FLOAT_CMP_OP
   #undef INT_ADD32
   #undef INT_MUL64
   #undef INT_OP64
   #undef DIV_CHECK
   #undef DIV_OVERFLOW_CHECK
   #undef DOUBLE_OP
   #undef DOUBLE_UNARY_OP
   #undef DOUBLE_CMP_OP
}

#endif /* JIT_RUN_CODE */


#ifdef FIXSCRIPT_ASYNC
static Value run(Heap *heap, Function *func, const char *func_name, Value *error, Value *args, va_list *ap, ResumeContinuation *cont, ContinuationResultFunc cont_func, void *cont_data)
#else
static Value run(Heap *heap, Function *func, const char *func_name, Value *error, Value *args, va_list *ap)
#endif
{
   Value ret = fixscript_int(0);
   Value val;
   int i, stack_base=0, error_stack_base=0, num_results=0;
   char *s;
#ifdef FIXSCRIPT_ASYNC
   int async_pc = 0;
   int restore_async_flag = 0;
#endif
#ifdef JIT_RUN_CODE
   void (*entry_func)(Heap *heap, void *func_addr, int stack_base) = (void *)heap->jit_code;
#else
   Value stack_error;
   int error_pc, stack_base2;
   int run_ret;
#endif

   #ifdef FIXSCRIPT_ASYNC
      if (cont) {
         if (!heap->async_active) {
            fixscript_dump_value(heap, fixscript_create_error_string(heap, "native error: improper async context"), 1);
            return fixscript_int(0);
         }
         if (heap->async_ret) {
            heap->async_ret = 0;
            if (heap->async_ret_error.value) {
               error_pc = (intptr_t)heap->error_stack.data[heap->error_stack.len-2];
               stack_base = (intptr_t)heap->error_stack.data[heap->error_stack.len-1];
               heap->error_stack.len -= 2;

               heap->stack_data[stack_base+0] = heap->async_ret_result.value;
               heap->stack_flags[stack_base+0] = heap->async_ret_result.is_array;

               heap->stack_data[stack_base+1] = heap->async_ret_error.value;
               heap->stack_flags[stack_base+1] = heap->async_ret_error.is_array;

               heap->stack_len = stack_base+2;
 
               if (error_pc == 0) {
                  stack_base = cont->stack_base;
                  error_stack_base = cont->error_stack_base;
                  free(cont);
                  goto async_return;
               }
               async_pc = error_pc;
            }
            else {
               heap->stack_data[heap->stack_len-1] = heap->async_ret_result.value;
               heap->stack_flags[heap->stack_len-1] = heap->async_ret_result.is_array;
               async_pc = cont->continue_pc;
            }
         }
         else {
            async_pc = cont->continue_pc;
         }

         stack_base = cont->stack_base;
         error_stack_base = cont->error_stack_base;
         if (cont->stack_overflow) {
            free(cont);
            goto async_stack_overflow_continue;
         }
         else {
            free(cont);
            goto async_normal_continue;
         }
      }
      if (heap->allow_sync_call) {
         heap->allow_sync_call = 0;
         if (heap->async_active) {
            heap->async_active = 0;
            restore_async_flag = 1;
         }
      }
      if (!cont_func && heap->async_active) {
         if (error) {
            *error = fixscript_create_error_string(heap, "native error: improper async context");
         }
         return fixscript_int(0);
      }
      if (heap->stack_len == 0 && cont_func) {
         heap->async_active = 1;
      }
   #endif

   stack_base = heap->stack_len;
   while (heap->stack_len+1+func->num_params > heap->stack_cap) {
      if (!expand_stack(heap)) {
         *error = fixscript_create_error_string(heap, "stack overflow");
         #ifdef FIXSCRIPT_ASYNC
            if (restore_async_flag) {
               heap->async_active = 1;
            }
            if (cont_func) {
               cont_func(heap, fixscript_int(0), *error, cont_data);
            }
         #endif
         return fixscript_int(0);
      }
   }
   heap->stack_data[heap->stack_len] = 0;
   heap->stack_flags[heap->stack_len++] = 0;
   if (args) {
      for (i=0; i<func->num_params; i++) {
         heap->stack_data[heap->stack_len] = args[i].value;
         heap->stack_flags[heap->stack_len++] = args[i].is_array;
      }
   }
   else {
      for (i=0; i<func->num_params; i++) {
         val = va_arg(*ap, Value);
         heap->stack_data[heap->stack_len] = val.value;
         heap->stack_flags[heap->stack_len++] = val.is_array;
      }
   }

   clear_roots(heap);

   error_stack_base = heap->error_stack.len;
   dynarray_add(&heap->error_stack, (void *)0);
   dynarray_add(&heap->error_stack, (void *)(intptr_t)stack_base);

   #ifdef JIT_RUN_CODE
      jit_update_exec(heap, 1);
      #ifdef JIT_DEBUG
         printf("jit_func_addr=%d %p %p\n", func->jit_addr, heap->jit_code, heap->jit_code + func->jit_addr);
         fflush(stdout);
      #endif
      entry_func(heap, heap->jit_code + func->jit_addr, stack_base);
      #ifdef JIT_DEBUG
         printf("jit_done!\n");
         fflush(stdout);
      #endif
   #else
      #ifdef FIXSCRIPT_ASYNC
         async_normal_continue:
         run_ret = run_bytecode(heap, async_pc? async_pc : func->addr);
         if (run_ret < 0) {
            cont = heap->async_continuations.data[--heap->async_continuations.len];
            cont->continue_pc = -run_ret;
            cont->stack_overflow = 0;
            cont->stack_base = stack_base;
            cont->error_stack_base = error_stack_base;
            cont->cont_func = cont_func;
            cont->cont_data = cont_data;
            return fixscript_int(0);
         }
      #else
         run_ret = run_bytecode(heap, func->addr);
      #endif
      if (!run_ret) {
         for (;;) {
            stack_error = create_error(heap, fixscript_create_string(heap, "stack overflow", -1), -1, 0);

            error_pc = (intptr_t)heap->error_stack.data[heap->error_stack.len-2];
            stack_base2 = (intptr_t)heap->error_stack.data[heap->error_stack.len-1];
            heap->error_stack.len -= 2;

            while (stack_base2+2 > heap->stack_cap) {
               if (!expand_stack(heap)) {
                  if (error) {
                     *error = stack_error;
                  }
                  heap->stack_len = stack_base;
                  heap->error_stack.len = error_stack_base;
                  #ifdef FIXSCRIPT_ASYNC
                     if (restore_async_flag) {
                        heap->async_active = 1;
                     }
                     if (cont_func) {
                        cont_func(heap, fixscript_int(0), *error, cont_data);
                     }
                  #endif
                  return fixscript_int(0);
               }
            }

            heap->stack_len = stack_base2;
            heap->stack_data[heap->stack_len] = 0;
            heap->stack_flags[heap->stack_len++] = 0;
            heap->stack_data[heap->stack_len] = stack_error.value;
            heap->stack_flags[heap->stack_len++] = stack_error.is_array;

            if (error_pc == 0) {
               break;
            }
            #ifdef FIXSCRIPT_ASYNC
               async_pc = 0;
               async_stack_overflow_continue:
               run_ret = run_bytecode(heap, async_pc? async_pc : error_pc);
               if (run_ret < 0) {
                  cont = heap->async_continuations.data[--heap->async_continuations.len];
                  cont->continue_pc = -run_ret;
                  cont->stack_overflow = 1;
                  cont->stack_base = stack_base;
                  cont->error_stack_base = error_stack_base;
                  cont->cont_func = cont_func;
                  cont->cont_data = cont_data;
                  return fixscript_int(0);
               }
            #else
               run_ret = run_bytecode(heap, error_pc);
            #endif
            if (run_ret) {
               break;
            }
         }
      }
   #endif

   #ifdef FIXSCRIPT_ASYNC
      async_return:
   #endif

   num_results = heap->stack_len - stack_base;

   if (num_results > 2) {
      if (error) {
         if (!func_name) {
            func_name = string_hash_find_name(&func->script->functions, func);
         }
         s = string_format("internal error: more than two results after call to function %s", func_name);
         *error = fixscript_create_string(heap, s, -1);
         free(s);
      }
      #ifdef FIXSCRIPT_ASYNC
         if (restore_async_flag) {
            heap->async_active = 1;
         }
         if (cont_func) {
            cont_func(heap, fixscript_int(0), *error, cont_data);
         }
      #endif
      return fixscript_int(0);
   }
   if (num_results < 1) {
      if (error) {
         if (!func_name) {
            func_name = string_hash_find_name(&func->script->functions, func);
         }
         s = string_format("internal error: less than one result after call to function %s", func_name);
         *error = fixscript_create_string(heap, s, -1);
         free(s);
      }
      #ifdef FIXSCRIPT_ASYNC
         if (restore_async_flag) {
            heap->async_active = 1;
         }
         if (cont_func) {
            cont_func(heap, fixscript_int(0), *error, cont_data);
         }
      #endif
      return fixscript_int(0);
   }

   if (error) {
      if (num_results == 2) {
         error->value = heap->stack_data[stack_base+1];
         error->is_array = heap->stack_flags[stack_base+1];
      }
      else {
         *error = fixscript_int(0);
      }
   }
   ret.value = heap->stack_data[stack_base];
   ret.is_array = heap->stack_flags[stack_base];
   heap->stack_len = stack_base;
   heap->error_stack.len = error_stack_base;
#ifdef FIXSCRIPT_ASYNC
   if (restore_async_flag) {
      heap->async_active = 1;
   }
   if (heap->stack_len == 0 && cont_func) {
      heap->async_active = 0;
   }
   if (cont_func) {
      cont_func(heap, ret, *error, cont_data);
      return fixscript_int(0);
   }
#endif
   return ret;
}


#ifdef FIXSCRIPT_ASYNC
static Value run_func(Heap *heap, Script *script, const char *func_name, Value *error, Value *args, va_list *ap, ContinuationResultFunc cont_func, void *cont_data)
#else
static Value run_func(Heap *heap, Script *script, const char *func_name, Value *error, Value *args, va_list *ap)
#endif
{
   Function *func;
   char *s;

   if (!script) {
      clear_roots(heap);
      if (error) {
         *error = fixscript_create_string(heap, "script not provided", -1);
      }
      #ifdef FIXSCRIPT_ASYNC
         if (cont_func) {
            cont_func(heap, fixscript_int(0), *error, cont_data);
         }
      #endif
      return fixscript_int(0);
   }

   func = string_hash_get(&script->functions, func_name);
   if (!func) {
      clear_roots(heap);
      if (error) {
         s = string_format("function %s not found", func_name);
         *error = fixscript_create_string(heap, s, -1);
         free(s);
      }
      #ifdef FIXSCRIPT_ASYNC
         if (cont_func) {
            cont_func(heap, fixscript_int(0), *error, cont_data);
         }
      #endif
      return fixscript_int(0);
   }

#ifdef FIXSCRIPT_ASYNC
   return run(heap, func, func_name, error, args, ap, NULL, cont_func, cont_data);
#else
   return run(heap, func, func_name, error, args, ap);
#endif
}


Value fixscript_run(Heap *heap, Script *script, const char *func_name, Value *error, ...)
{
   Value ret;
   va_list ap;

   va_start(ap, error);
#ifdef FIXSCRIPT_ASYNC
   ret = run_func(heap, script, func_name, error, NULL, &ap, NULL, NULL);
#else
   ret = run_func(heap, script, func_name, error, NULL, &ap);
#endif
   va_end(ap);
   return ret;
}


Value fixscript_run_args(Heap *heap, Script *script, const char *func_name, Value *error, Value *args)
{
#ifdef FIXSCRIPT_ASYNC
   return run_func(heap, script, func_name, error, args, NULL, NULL, NULL);
#else
   return run_func(heap, script, func_name, error, args, NULL);
#endif
}


#ifdef FIXSCRIPT_ASYNC
static Value call_func(Heap *heap, Value func, int num_params, Value *error, Value *args, va_list *ap, ContinuationResultFunc cont_func, void *cont_data)
#else
static Value call_func(Heap *heap, Value func, int num_params, Value *error, Value *args, va_list *ap)
#endif
{
   Function *fn;
   int func_id = func.value - FUNC_REF_OFFSET;

   if (!func.is_array || func_id < 1 || func_id >= heap->functions.len) {
      if (error) {
         *error = fixscript_create_string(heap, "invalid function reference", -1);
      }
      #ifdef FIXSCRIPT_ASYNC
         if (cont_func) {
            cont_func(heap, fixscript_int(0), *error, cont_data);
         }
      #endif
      return fixscript_int(0);
   }
   
   fn = heap->functions.data[func_id];
   if (num_params != fn->num_params) {
      if (error) {
         *error = fixscript_create_string(heap, "improper number of function parameters", -1);
      }
      #ifdef FIXSCRIPT_ASYNC
         if (cont_func) {
            cont_func(heap, fixscript_int(0), *error, cont_data);
         }
      #endif
      return fixscript_int(0);
   }

#ifdef FIXSCRIPT_ASYNC
   return run(heap, fn, NULL, error, args, ap, NULL, cont_func, cont_data);
#else
   return run(heap, fn, NULL, error, args, ap);
#endif
}


Value fixscript_call(Heap *heap, Value func, int num_params, Value *error, ...)
{
   Value ret;
   va_list ap;

   va_start(ap, error);
#ifdef FIXSCRIPT_ASYNC
   ret = call_func(heap, func, num_params, error, NULL, &ap, NULL, NULL);
#else
   ret = call_func(heap, func, num_params, error, NULL, &ap);
#endif
   va_end(ap);
   return ret;
}


Value fixscript_call_args(Heap *heap, Value func, int num_params, Value *error, Value *args)
{
#ifdef FIXSCRIPT_ASYNC
   return call_func(heap, func, num_params, error, args, NULL, NULL, NULL);
#else
   return call_func(heap, func, num_params, error, args, NULL);
#endif
}


void fixscript_register_native_func(Heap *heap, const char *name, NativeFunc func, void *data)
{
   NativeFunction *nfunc;
   char *s;

   s = strrchr(name, '#');
   if (!s) return;

   nfunc = string_hash_get(&heap->native_functions_hash, name);
   if (nfunc) {
      nfunc->func = func;
      nfunc->data = data;
      return;
   }

   nfunc = malloc(sizeof(NativeFunction));
   nfunc->func = func;
   nfunc->data = data;
   nfunc->id = heap->native_functions.len;
   nfunc->num_params = atoi(s+1);
   nfunc->bytecode_ident_pc = heap->bytecode_size;
   dynarray_add(&heap->native_functions, nfunc);

   heap->bytecode = realloc(heap->bytecode, heap->bytecode_size+1);
   heap->bytecode[heap->bytecode_size++] = 0;

   string_hash_set(&heap->native_functions_hash, strdup(name), nfunc);
}


NativeFunc fixscript_get_native_func(Heap *heap, const char *name, void **data)
{
   NativeFunction *nfunc;

   nfunc = string_hash_get(&heap->native_functions_hash, name);
   if (!nfunc) return NULL;

   if (data) {
      *data = nfunc->data;
   }
   return nfunc->func;
}


char *fixscript_dump_code(Heap *heap, Script *script, const char *func_name)
{
   struct SwitchTable {
      int start, end;
      struct SwitchTable *next;
   };
   String out;
   Function *func, *show_func;
   LineEntry *line;
   int i, pc, op, func_num=1, line_num=0;
   unsigned short short_val;
   int int_val;
   float float_val;
   int table_idx, size, default_pc;
   int *table;
   struct SwitchTable *switch_table = NULL, *new_switch_table;

   if (func_name) {
      if (!script) {
         return strdup("error: invalid script reference");
      }
      show_func = string_hash_get(&script->functions, func_name);
      if (!show_func) {
         return string_format("error: unknown function %s in %s", func_name, string_hash_find_name(&heap->scripts, script));
      }
   }
   else {
      show_func = NULL;
   }

   memset(&out, 0, sizeof(String));
   func = heap->functions.len > 1? heap->functions.data[1] : NULL;
   line = heap->lines_size > 0? &heap->lines[0] : NULL;

   if (show_func) {
      while (func != show_func) {
         func = heap->functions.data[++func_num];
      }
      line_num = show_func->lines_start;
      line = &heap->lines[line_num];
   }

   for (pc = (func? func->addr : 0); pc < heap->bytecode_size; pc++) {
      if (switch_table && pc == switch_table->start) {
         pc = switch_table->end;
         new_switch_table = switch_table->next;
         free(switch_table);
         switch_table = new_switch_table;
      }
      if (func && pc == func->addr) {
         if (func_num == -1) break;
         if (!string_append(&out, "\nfunction %s [%s]\n", string_hash_find_name(&func->script->functions, func), string_hash_find_name(&heap->scripts, func->script))) goto error;
         if (++func_num < heap->functions.len) {
            func = heap->functions.data[func_num];
         }
         else {
            func = NULL;
         }
         if (show_func) {
            func_num = -1;
         }
      }
      
      if (line && pc == line->pc) {
         if (!string_append(&out, "line=%d\n", line->line)) goto error;
         if (++line_num < heap->lines_size) {
            line = &heap->lines[line_num];
         }
         else {
            line = NULL;
         }
      }

      if (!string_append(&out, "%6d: ", pc)) goto error;
      op = heap->bytecode[pc];
      #define DUMP(...) if (!string_append(&out, __VA_ARGS__)) goto error; break
      #define DATA() (heap->bytecode[++pc])
      #define DATA_SBYTE() ((signed char)heap->bytecode[++pc])
      #define DATA_SHORT() *((unsigned short *)memcpy(&short_val, &heap->bytecode[(pc += 2)-1], sizeof(unsigned short)))
      #define DATA_INT() *((int *)memcpy(&int_val, &heap->bytecode[(pc += 4)-3], sizeof(int)))
      #define DATA_FLOAT() *((float *)memcpy(&float_val, &heap->bytecode[(pc += 4)-3], sizeof(float)))
      switch (op) {
         case BC_POP:           DUMP("pop");
         case BC_POPN:          DUMP("popn");
         case BC_LOADN:         DUMP("loadn");
         case BC_STOREN:        DUMP("storen");
         case BC_ADD:           DUMP("add");
         case BC_SUB:           DUMP("sub");
         case BC_MUL:           DUMP("mul");
         case BC_ADD_MOD:       DUMP("add_mod");
         case BC_SUB_MOD:       DUMP("sub_mod");
         case BC_MUL_MOD:       DUMP("mul_mod");
         case BC_DIV:           DUMP("div");
         case BC_REM:           DUMP("rem");
         case BC_SHL:           DUMP("shl");
         case BC_SHR:           DUMP("shr");
         case BC_USHR:          DUMP("ushr");
         case BC_AND:           DUMP("and");
         case BC_OR:            DUMP("or");
         case BC_XOR:           DUMP("xor");
         case BC_LT:            DUMP("lt");
         case BC_LE:            DUMP("le");
         case BC_GT:            DUMP("gt");
         case BC_GE:            DUMP("ge");
         case BC_EQ:            DUMP("eq");
         case BC_NE:            DUMP("ne");
         case BC_EQ_VALUE:      DUMP("eq_value");
         case BC_NE_VALUE:      DUMP("ne_value");
         case BC_BITNOT:        DUMP("bitnot");
         case BC_LOGNOT:        DUMP("lognot");
         case BC_INC:           DUMP("inc %d", DATA_SBYTE());
         case BC_DEC:           DUMP("dec %d", DATA_SBYTE());
         case BC_FLOAT_ADD:     DUMP("float_add");
         case BC_FLOAT_SUB:     DUMP("float_sub");
         case BC_FLOAT_MUL:     DUMP("float_mul");
         case BC_FLOAT_DIV:     DUMP("float_div");
         case BC_FLOAT_LT:      DUMP("float_lt");
         case BC_FLOAT_LE:      DUMP("float_le");
         case BC_FLOAT_GT:      DUMP("float_gt");
         case BC_FLOAT_GE:      DUMP("float_ge");
         case BC_FLOAT_EQ:      DUMP("float_eq");
         case BC_FLOAT_NE:      DUMP("float_ne");
         case BC_RETURN:        DUMP("return");
         case BC_RETURN2:       DUMP("return2");
         case BC_CALL_DIRECT:   DUMP("call_direct");
         case BC_CALL_DYNAMIC:  DUMP("call_dynamic");
         case BC_CALL_NATIVE:   DUMP("call_native");
         case BC_CALL2_DIRECT:  DUMP("call2_direct");
         case BC_CALL2_DYNAMIC: DUMP("call2_dynamic");
         case BC_CALL2_NATIVE:  DUMP("call2_native");
         case BC_CLEAN_CALL2:   DUMP("clean_call2");
         case BC_CREATE_ARRAY:  DUMP("create_array");
         case BC_CREATE_HASH:   DUMP("create_hash");
         case BC_ARRAY_GET:     DUMP("array_get");
         case BC_ARRAY_SET:     DUMP("array_set");
         case BC_ARRAY_APPEND:  DUMP("array_append");
         case BC_HASH_GET:      DUMP("hash_get");
         case BC_HASH_SET:      DUMP("hash_set");
         case BC_CONST_P8:      DUMP("const_p8 %d", DATA()+1);
         case BC_CONST_N8:      DUMP("const_n8 %d", -(DATA()+1));
         case BC_CONST_P16:     DUMP("const_p16 %d", DATA_SHORT()+1);
         case BC_CONST_N16:     DUMP("const_n16 %d", -(DATA_SHORT()+1));
         case BC_CONST_I32:     DUMP("const_i32 %d", DATA_INT());
         case BC_CONST_F32:     DUMP("const_f32 %f", DATA_FLOAT());

         case BC_BRANCH_LONG:
            int_val = DATA_INT();
            DUMP("branch_long %d => %d", int_val, pc+int_val+1);

         case BC_JUMP_LONG:
            int_val = DATA_INT();
            DUMP("jump_long %d => %d", int_val, pc+int_val+1);

         case BC_LOOP_I8:
            int_val = DATA();
            DUMP("loop_i8 %d => %d", int_val, pc-int_val+1-1);

         case BC_LOOP_I16:
            int_val = DATA_SHORT();
            DUMP("loop_i16 %d => %d", int_val, pc-int_val+1-2);

         case BC_LOOP_I32:
            int_val = DATA_INT();
            DUMP("loop_i32 %d => %d", int_val, pc-int_val+1-4);

         case BC_LOAD_LOCAL:  DUMP("load_local %d", DATA_INT());
         case BC_STORE_LOCAL: DUMP("store_local %d", DATA_INT());

         case BC_SWITCH:
            table_idx = DATA_INT();
            table = &((int *)heap->bytecode)[table_idx];
            size = table[-2];
            default_pc = table[-1];
            if (!string_append(&out, "switch table_start=%d table_end=%d default=%d\n", (table_idx-2)*4, (table_idx+size*2)*4, default_pc)) goto error;
            for (i=0; i<size; i++) {
               if (table[i*2+1] < 0) {
                  if (!string_append(&out, "        | case %d..%d => %d\n", table[i*2+0], table[(i+1)*2+0], -table[i*2+1])) goto error;
                  i++;
               }
               else {
                  if (!string_append(&out, "        | case %d => %d\n", table[i*2+0], table[i*2+1])) goto error;
               }
            }
            
            new_switch_table = malloc(sizeof(struct SwitchTable));
            new_switch_table->start = (table_idx-2)*4;
            new_switch_table->end = (table_idx+size*2)*4;
            new_switch_table->next = switch_table;
            switch_table = new_switch_table;
            continue;

         case BC_LENGTH:        DUMP("length");
         case BC_CONST_STRING:  DUMP("const_string");
         case BC_STRING_CONCAT: DUMP("string_concat");

         case BC_CHECK_STACK:   DUMP("check_stack %d", DATA_SHORT());

         case BC_EXTENDED:
            op = DATA();
            switch (op) {
               case BC_EXT_MIN:              DUMP("min");
               case BC_EXT_MAX:              DUMP("max");
               case BC_EXT_CLAMP:            DUMP("clamp");
               case BC_EXT_ABS:              DUMP("abs");
               case BC_EXT_ADD32:            DUMP("add32");
               case BC_EXT_SUB32:            DUMP("sub32");
               case BC_EXT_ADD64:            DUMP("add64");
               case BC_EXT_SUB64:            DUMP("sub64");
               case BC_EXT_MUL64:            DUMP("mul64");
               case BC_EXT_UMUL64:           DUMP("umul64");
               case BC_EXT_MUL64_LONG:       DUMP("mul64_long");
               case BC_EXT_DIV64:            DUMP("div64");
               case BC_EXT_UDIV64:           DUMP("udiv64");
               case BC_EXT_REM64:            DUMP("rem64");
               case BC_EXT_UREM64:           DUMP("urem64");
               case BC_EXT_FLOAT:            DUMP("float");
               case BC_EXT_INT:              DUMP("int");
               case BC_EXT_FABS:             DUMP("fabs");
               case BC_EXT_FMIN:             DUMP("fmin");
               case BC_EXT_FMAX:             DUMP("fmax");
               case BC_EXT_FCLAMP:           DUMP("fclamp");
               case BC_EXT_FLOOR:            DUMP("floor");
               case BC_EXT_CEIL:             DUMP("ceil");
               case BC_EXT_ROUND:            DUMP("round");
               case BC_EXT_POW:              DUMP("pow");
               case BC_EXT_SQRT:             DUMP("sqrt");
               case BC_EXT_CBRT:             DUMP("cbrt");
               case BC_EXT_EXP:              DUMP("exp");
               case BC_EXT_LN:               DUMP("ln");
               case BC_EXT_LOG2:             DUMP("log2");
               case BC_EXT_LOG10:            DUMP("log10");
               case BC_EXT_SIN:              DUMP("sin");
               case BC_EXT_COS:              DUMP("cos");
               case BC_EXT_ASIN:             DUMP("asin");
               case BC_EXT_ACOS:             DUMP("acos");
               case BC_EXT_TAN:              DUMP("tan");
               case BC_EXT_ATAN:             DUMP("atan");
               case BC_EXT_ATAN2:            DUMP("atan2");
               case BC_EXT_DBL_FLOAT:        DUMP("dbl_float");
               case BC_EXT_DBL_INT:          DUMP("dbl_int");
               case BC_EXT_DBL_CONV_DOWN:    DUMP("dbl_conv_down");
               case BC_EXT_DBL_CONV_UP:      DUMP("dbl_conv_up");
               case BC_EXT_DBL_ADD:          DUMP("dbl_add");
               case BC_EXT_DBL_SUB:          DUMP("dbl_sub");
               case BC_EXT_DBL_MUL:          DUMP("dbl_mul");
               case BC_EXT_DBL_DIV:          DUMP("dbl_div");
               case BC_EXT_DBL_CMP_LT:       DUMP("dbl_cmp_lt");
               case BC_EXT_DBL_CMP_LE:       DUMP("dbl_cmp_le");
               case BC_EXT_DBL_CMP_GT:       DUMP("dbl_cmp_gt");
               case BC_EXT_DBL_CMP_GE:       DUMP("dbl_cmp_ge");
               case BC_EXT_DBL_CMP_EQ:       DUMP("dbl_cmp_eq");
               case BC_EXT_DBL_CMP_NE:       DUMP("dbl_cmp_ne");
               case BC_EXT_DBL_FABS:         DUMP("dbl_fabs");
               case BC_EXT_DBL_FMIN:         DUMP("dbl_fmin");
               case BC_EXT_DBL_FMAX:         DUMP("dbl_fmax");
               case BC_EXT_DBL_FCLAMP:       DUMP("dbl_fclamp");
               case BC_EXT_DBL_FCLAMP_SHORT: DUMP("dbl_fclamp_short");
               case BC_EXT_DBL_FLOOR:        DUMP("dbl_floor");
               case BC_EXT_DBL_CEIL:         DUMP("dbl_ceil");
               case BC_EXT_DBL_ROUND:        DUMP("dbl_round");
               case BC_EXT_DBL_POW:          DUMP("dbl_pow");
               case BC_EXT_DBL_SQRT:         DUMP("dbl_sqrt");
               case BC_EXT_DBL_CBRT:         DUMP("dbl_cbrt");
               case BC_EXT_DBL_EXP:          DUMP("dbl_exp");
               case BC_EXT_DBL_LN:           DUMP("dbl_ln");
               case BC_EXT_DBL_LOG2:         DUMP("dbl_log2");
               case BC_EXT_DBL_LOG10:        DUMP("dbl_log10");
               case BC_EXT_DBL_SIN:          DUMP("dbl_sin");
               case BC_EXT_DBL_COS:          DUMP("dbl_cos");
               case BC_EXT_DBL_ASIN:         DUMP("dbl_asin");
               case BC_EXT_DBL_ACOS:         DUMP("dbl_acos");
               case BC_EXT_DBL_TAN:          DUMP("dbl_tan");
               case BC_EXT_DBL_ATAN:         DUMP("dbl_atan");
               case BC_EXT_DBL_ATAN2:        DUMP("dbl_atan2");
               case BC_EXT_IS_INT:           DUMP("is_int");
               case BC_EXT_IS_FLOAT:         DUMP("is_float");
               case BC_EXT_IS_ARRAY:         DUMP("is_array");
               case BC_EXT_IS_STRING:        DUMP("is_string");
               case BC_EXT_IS_HASH:          DUMP("is_hash");
               case BC_EXT_IS_SHARED:        DUMP("is_shared");
               case BC_EXT_IS_CONST:         DUMP("is_const");
               case BC_EXT_IS_FUNCREF:       DUMP("is_funcref");
               case BC_EXT_IS_WEAKREF:       DUMP("is_weakref");
               case BC_EXT_IS_HANDLE:        DUMP("is_handle");
               case BC_EXT_CHECK_TIME_LIMIT: DUMP("check_time_limit");
               default:
                  DUMP("(unknown_extended=%d)", op);
            }
            break;

         default:
            if ((op >= BC_CONSTM1 && op <= BC_CONST0+32) || op == BC_CONST0+63 || op == BC_CONST0+64) {
               DUMP("const %d", op - BC_CONST0);
            }
            if (op >= BC_BRANCH0 && op <= BC_BRANCH0+7) {
               int_val = ((op & 7) << 8) | DATA();
               DUMP("branch %d => %d", int_val, pc+int_val+1);
            }
            if (op >= BC_JUMP0 && op <= BC_JUMP0+7) {
               int_val = ((op & 7) << 8) | DATA();
               DUMP("jump %d => %d", int_val, pc+int_val+1);
            }
            if (op >= BC_STOREM64 && op <= BC_STOREM64+63) {
               DUMP("store %d", op - BC_STOREM64 - 64);
            }
            if (op >= BC_LOADM64 && op <= BC_LOADM64+63) {
               DUMP("load %d", op - BC_LOADM64 - 64);
            }
            DUMP("(unknown=%d)", op);
      }
      #undef DUMP
      #undef DATA
      #undef DATA_SBYTE
      #undef DATA_SHORT
      #undef DATA_INT
      #undef DATA_FLOAT
      if (!string_append(&out, "\n")) goto error;
   }
   
   if (switch_table) {
      goto error;
   }
   return out.data;

error:
   while (switch_table) {
      new_switch_table = switch_table->next;
      free(switch_table);
      switch_table = new_switch_table;
   }
   free(out.data);
   return strdup("internal error occurred");
}


static void dump_heap_value(String *out, Heap *heap, Value value)
{
   char buf[32], *s;

   if (fixscript_is_int(value)) {
      string_append(out, "%d", fixscript_get_int(value));
   }
   else if (fixscript_is_float(value)) {
      snprintf(buf, sizeof(buf), "%.9g", fixscript_get_float(value));
      for (s=buf; *s; s++) {
         if (*s == ',') *s = '.';
      }
      string_append(out, buf);
   }
   else {
      string_append(out, "#%d", value.value);
   }
}


char *fixscript_dump_heap(Heap *heap)
{
   String out;
   Array *arr;
   Value key, value;
   char *s;
   int i, j, used=0, len, num;

   memset(&out, 0, sizeof(String));

   for (i=1; i<heap->size; i++) {
      arr = &heap->data[i];
      if (arr->len != -1) used++;
   }

   string_append(&out, "used=%d size=%d\n", used, heap->size);

   for (i=1; i<heap->size; i++) {
      arr = &heap->data[i];
      if (arr->len == -1) continue;

      if (arr->ext_refcnt != 0) {
         string_append(&out, "#%d (ext=%d) = ", i, arr->ext_refcnt);
      }
      else {
         string_append(&out, "#%d = ", i);
      }

      if (arr->is_handle) {
         string_append(&out, "handle ptr=%p type=%d\n", arr->handle_ptr, arr->type);
      }
      else if (arr->hash_slots >= 0) {
         string_append(&out, "hash(");
         j = 0;
         num = 0;
         while (fixscript_iter_hash(heap, (Value) { i, 1 }, &key, &value, &j)) {
            if (num > 0) string_append(&out, ",");
            if (num >= 20) {
               string_append(&out, "...");
               break;
            }
            dump_heap_value(&out, heap, key);
            string_append(&out, "=>");
            dump_heap_value(&out, heap, value);
            num++;
         }
         string_append(&out, ")\n");
      }
      else if (arr->is_string) {
         s = NULL;
         len = 0;
         fixscript_get_string(heap, (Value) { i, 1 }, 0, -1, &s, &len);
         if (len >= 103) {
            s[100] = '.';
            s[101] = '.';
            s[102] = '.';
            len = 103;
         }
         for (j=0; j<len; j++) {
            if (s[j] == '\0') s[j] = '`';
            if (s[j] == '\r') s[j] = '`';
            if (s[j] == '\n') s[j] = '`';
            if (s[j] == '\t') s[j] = '`';
         }
         if (arr->is_const) {
            string_append(&out, "const_string(");
         }
         else {
            string_append(&out, "string(");
         }
         string_append(&out, "len=%d/%d,\"%.*s\")\n", arr->len, arr->size, len, s);
         free(s);
      }
      else {
         if (arr->is_shared) {
            string_append(&out, "shared_array(");
         }
         else {
            string_append(&out, "array(");
         }
         string_append(&out, "len=%d/%d,", arr->len, arr->size);
         for (j=0; j<arr->len; j++) {
            if (j > 0) string_append(&out, ",");
            if (j >= 100) {
               string_append(&out, "...");
               break;
            }
            value = (Value) { get_array_value(arr, j), IS_ARRAY(arr, j) != 0 };
            dump_heap_value(&out, heap, value);
         }
         string_append(&out, ")\n");
      }
   }

   return out.data;
}


#ifdef FIXSCRIPT_ASYNC

static void resume_func(Heap *heap, Value result, Value error, void *data)
{
   ResumeContinuation *cont = data;

   heap->async_ret = 1;
   heap->async_ret_result = result;
   heap->async_ret_error = error;

   if (cont->continue_pc == 0) {
      heap->async_continuations.data[heap->async_continuations.len-1] = NULL;
      free(cont);
      return;
   }

   if (cont->set_stack_len != -1) {
      heap->stack_len = cont->set_stack_len;
   }

   run(heap, NULL, "<resumed>", &error, NULL, NULL, cont, cont->cont_func, cont->cont_data);
}


static void empty_resume_func(Heap *heap, Value result, Value error, void *data)
{
}


static void auto_suspend_resume_func(void *data)
{
   ResumeContinuation *cont = data;
   Heap *heap = cont->auto_suspend_heap;
   Value error;

   if (cont->continue_pc == 0) {
      heap->async_continuations.data[heap->async_continuations.len-1] = NULL;
      free(cont);
      return;
   }

   if (cont->set_stack_len != -1) {
      heap->stack_len = cont->set_stack_len;
   }

   heap->async_ret = 0;
   run(heap, NULL, "<resumed>", &error, NULL, NULL, cont, cont->cont_func, cont->cont_data);
}


void fixscript_set_auto_suspend_handler(Heap *heap, int num_instructions, ContinuationSuspendFunc func, void *data)
{
   fixscript_set_time_limit(heap, -1);
   heap->auto_suspend_num_instructions = num_instructions;
   heap->auto_suspend_func = func;
   heap->auto_suspend_data = data;
   heap->instruction_limit = heap->instruction_counter + ((uint32_t)num_instructions);
}


void fixscript_get_auto_suspend_handler(Heap *heap, int *num_instructions, ContinuationSuspendFunc *func, void **data)
{
   if (num_instructions) *num_instructions = heap->auto_suspend_num_instructions;
   if (func) *func = heap->auto_suspend_func;
   if (data) *data = heap->auto_suspend_data;
}


void fixscript_suspend(Heap *heap, ContinuationResultFunc *func, void **data)
{
   ResumeContinuation *cont;
   
   if (!heap->async_active) {
      fixscript_dump_value(heap, fixscript_create_error_string(heap, "native error: improper async context"), 1);
      *func = empty_resume_func;
      *data = NULL;
      return;
   }

   cont = malloc(sizeof(ResumeContinuation));
   cont->continue_pc = 0;
   cont->set_stack_len = -1;
   cont->cont_func = NULL;
   heap->async_continuations.data[heap->async_continuations.len-1] = cont;
 
   *func = resume_func;
   *data = cont;
}


typedef struct {
   Heap *heap;
   ContinuationResultFunc func;
   void *data;
} VoidWrapperContinuation;

static void void_wrapper_func(void *data)
{
   VoidWrapperContinuation *cont = data;

   cont->func(cont->heap, fixscript_int(0), fixscript_int(0), cont->data);
   free(cont);
}


void fixscript_suspend_void(Heap *heap, ContinuationFunc *func, void **data)
{
   VoidWrapperContinuation *cont;

   cont = malloc(sizeof(VoidWrapperContinuation));
   cont->heap = heap;
   fixscript_suspend(heap, &cont->func, &cont->data);

   *func = void_wrapper_func;
   *data = cont;
}


void fixscript_run_async(Heap *heap, Script *script, const char *func_name, Value *args, ContinuationResultFunc cont_func, void *cont_data)
{
   Value error;

   run_func(heap, script, func_name, &error, args, NULL, cont_func, cont_data);
}


void fixscript_call_async(Heap *heap, Value func, int num_params, Value *args, ContinuationResultFunc cont_func, void *cont_data)
{
   Value error;

   call_func(heap, func, num_params, &error, args, NULL, cont_func, cont_data);
}


void fixscript_allow_sync_call(Heap *heap)
{
   heap->allow_sync_call = 1;
}
 

int fixscript_in_async_call(Heap *heap)
{
   return heap->async_active;
}

#endif /* FIXSCRIPT_ASYNC */


#ifndef FIXSCRIPT_NO_JIT

#define SH(cond) (cond)
//#define SH(cond) 0

enum {
   JIT_ERROR_INTEGER_OVERFLOW = 1,
   JIT_ERROR_DIVISION_BY_ZERO,
   JIT_ERROR_STACK_OVERFLOW,
   JIT_ERROR_INVALID_ARRAY,
   JIT_ERROR_OUT_OF_BOUNDS,
   JIT_ERROR_CONST_STRING,
   JIT_ERROR_INVALID_SHARED,
   JIT_ERROR_OUT_OF_MEMORY,
   JIT_ERROR_INVALID_HASH,
   JIT_ERROR_KEY_NOT_FOUND,
   JIT_ERROR_ARRAY_OR_HASH,
   JIT_ERROR_INVALID_FUNCREF,
   JIT_ERROR_IMPROPER_PARAMS,
   JIT_ERROR_EXECUTION_STOP,
   JIT_ERROR_TIME_LIMIT
};

#define JIT_PC_ERR(pc, err) (((pc) << 8) | (err))

typedef void *(*JitAdjPtrFunc)(Heap *);
typedef int (*JitAdjIntFunc)(Heap *);


static void jit_return_value(Heap *heap, int stack_pos, Value value)
{
   heap->stack_len = stack_pos;
   heap->stack_data[stack_pos-1] = value.value;
   heap->stack_flags[stack_pos-1] = value.is_array;
}


static void jit_return_error(Heap *heap, const char *msg, int pc)
{
   Value msg_val, error_val;

   msg_val = fixscript_create_string(heap, msg, -1);
   if (!msg_val.is_array) goto error;

   error_val = create_error(heap, msg_val, 0, pc);
   if (!error_val.is_array) goto error;

   heap->stack_len = heap->jit_error_base;
   heap->stack_data[heap->stack_len+0] = 0;
   heap->stack_flags[heap->stack_len+0] = 0;
   heap->stack_data[heap->stack_len+1] = error_val.value;
   heap->stack_flags[heap->stack_len+1] = error_val.is_array;
   heap->stack_len += 2;
   return;

error:
   heap->stack_len = heap->jit_error_base;
   heap->stack_data[heap->stack_len+0] = 0;
   heap->stack_flags[heap->stack_len+0] = 0;
   heap->stack_data[heap->stack_len+1] = 1;
   heap->stack_flags[heap->stack_len+1] = 0;
   heap->stack_len += 2;
}


static void jit_emit_error(Heap *heap, uint32_t pc_and_code)
{
   const char *msg;
   int pc = pc_and_code >> 8;
   int code = pc_and_code & 0xFF;
   
   switch (code) {
      case JIT_ERROR_INTEGER_OVERFLOW: msg = "integer overflow"; break;
      case JIT_ERROR_DIVISION_BY_ZERO: msg = "division by zero"; break;
      case JIT_ERROR_STACK_OVERFLOW:   msg = "stack overflow"; break;
      case JIT_ERROR_INVALID_ARRAY:    msg = "invalid array access"; break;
      case JIT_ERROR_OUT_OF_BOUNDS:    msg = "array out of bounds access"; break;
      case JIT_ERROR_CONST_STRING:     msg = "write access to constant string"; break;
      case JIT_ERROR_INVALID_SHARED:   msg = "invalid shared array operation"; break;
      case JIT_ERROR_OUT_OF_MEMORY:    msg = "out of memory"; break;
      case JIT_ERROR_INVALID_HASH:     msg = "invalid hash access"; break;
      case JIT_ERROR_KEY_NOT_FOUND:    msg = "hash key not found"; break;
      case JIT_ERROR_ARRAY_OR_HASH:    msg = "invalid array or hash access"; break;
      case JIT_ERROR_INVALID_FUNCREF:  msg = "invalid function reference"; break;
      case JIT_ERROR_IMPROPER_PARAMS:  msg = "improper number of function parameters"; break;
      case JIT_ERROR_EXECUTION_STOP:   msg = "execution stop"; break;
      case JIT_ERROR_TIME_LIMIT:       msg = "execution time limit reached"; break;

      default:
         msg = "internal error: wrong JIT error";
   }

   jit_return_error(heap, msg, pc);
}


static int jit_call_native(Heap *heap, NativeFunction *nfunc, void **native_ret, void **native_fp)
{
   Value params_on_stack[PARAMS_ON_STACK];
   Value ret = fixscript_int(0), error = fixscript_int(0), *params;
   StackBlock block;
   int i, base;

   base = heap->stack_len - nfunc->num_params;
   
   while (heap->stack_len+1 > heap->stack_cap) {
      if (!expand_stack(heap)) {
         error = fixscript_create_error_string(heap, "stack overflow");
         goto error;
      }
   }

   heap->stack_data[heap->stack_len] = nfunc->bytecode_ident_pc | (1<<31);
   heap->stack_flags[heap->stack_len++] = 1;

   params = nfunc->num_params > PARAMS_ON_STACK? malloc(nfunc->num_params * sizeof(Value)) : params_on_stack;
   for (i=0; i<nfunc->num_params; i++) {
      params[i].value = heap->stack_data[base+i];
      params[i].is_array = heap->stack_flags[base+i];
   }

   block.ret = native_ret;
   block.fp = native_fp;
   block.next = heap->jit_stack_block;
   heap->jit_stack_block = &block;

   ret = nfunc->func(heap, &error, nfunc->num_params, params, nfunc->data);

   heap->jit_stack_block = block.next;

   if (nfunc->num_params > PARAMS_ON_STACK) {
      free(params);
   }
   clear_roots(heap);

   jit_update_exec(heap, 1);

   if (error.value) {
      goto error;
   }

   jit_return_value(heap, base, ret);
   return 1;

error:
   heap->stack_len = heap->jit_error_base;
   heap->stack_data[heap->stack_len+0] = ret.value;
   heap->stack_flags[heap->stack_len+0] = ret.is_array;
   heap->stack_data[heap->stack_len+1] = error.value;
   heap->stack_flags[heap->stack_len+1] = error.is_array;
   heap->stack_len += 2;
   return 0;
}


static int jit_string_concat(Heap *heap, int num, int pc)
{
   Value value, result;
   int i, base, total_len = 0, len, err;
   struct {
      char *str;
      int len;
   } *strings = NULL;
   char *s;

   base = heap->stack_len - num;

   strings = calloc(num, sizeof(*strings));
   if (!strings) {
      err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
      goto error;
   }

   for (i=0; i<num; i++) {
      value = (Value) { heap->stack_data[base+i], heap->stack_flags[base+i] };
      if (!fixscript_is_string(heap, value)) {
         err = fixscript_to_string(heap, value, 0, &strings[i].str, &len);
      }
      else {
         err = fixscript_get_string(heap, value, 0, -1, &strings[i].str, &len);
      }
      if (err || len > (INT_MAX - total_len)) {
         for (i=0; i<num; i++) {
            free(strings[i].str);
         }
         free(strings);
         goto error;
      }
      strings[i].len = len;
      total_len += len;
   }

   s = malloc(total_len);
   if (!s) {
      for (i=0; i<num; i++) {
         free(strings[i].str);
      }
      free(strings);
      err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
      goto error;
   }

   for (i=0, len=0; i<num; i++) {
      memcpy(s + len, strings[i].str, strings[i].len);
      len += strings[i].len;
   }

   result = fixscript_create_string(heap, s, total_len);

   for (i=0; i<num; i++) {
      free(strings[i].str);
   }
   free(strings);
   free(s);

   jit_update_exec(heap, 1);

   if (!result.value) {
      err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
      goto error;
   }

   jit_return_value(heap, base+1, result);
   return 1;

error:
   jit_return_error(heap, fixscript_get_error_msg(err), pc);
   return 0;
}


static int jit_create_array(Heap *heap, int num, int pc)
{
   Value arr_val;
   Array *arr;
   unsigned int val, max_value = 0;
   int i, err, base;

   base = heap->stack_len - num;

   for (i=0; i<num; i++) {
      val = heap->stack_data[base+i];
      if ((unsigned int)val > max_value) {
         max_value = val;
      }
   }
   arr_val = create_array(heap, max_value <= 0xFF? ARR_BYTE : max_value <= 0xFFFF? ARR_SHORT : ARR_INT, num);
   if (!arr_val.is_array) {
      err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
      goto error;
   }
   arr = &heap->data[arr_val.value];
   arr->len = num;
   for (i=0; i<num; i++) {
      set_array_value(arr, i, heap->stack_data[base+i]);
      ASSIGN_IS_ARRAY(arr, i, heap->stack_flags[base+i]);
   }

   jit_update_exec(heap, 1);
   jit_return_value(heap, base+1, arr_val);
   return 1;

error:
   jit_update_exec(heap, 1);
   jit_return_error(heap, fixscript_get_error_msg(err), pc);
   return 0;
}


static int jit_create_hash(Heap *heap, int num, int pc)
{
   Value hash_val, key, value;
   int i, err, base;

   base = heap->stack_len - num;
   
   hash_val = create_hash(heap);
   if (!hash_val.is_array) {
      err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
      goto error;
   }
   for (i=0; i<num; i+=2) {
      key = (Value) { heap->stack_data[base+i+0], heap->stack_flags[base+i+0] };
      value = (Value) { heap->stack_data[base+i+1], heap->stack_flags[base+i+1] };
      err = fixscript_set_hash_elem(heap, hash_val, key, value);
      if (err) {
         goto error;
      }
   }

   jit_update_exec(heap, 1);
   jit_return_value(heap, base+1, hash_val);
   return 1;

error:
   jit_update_exec(heap, 1);
   jit_return_error(heap, fixscript_get_error_msg(err), pc);
   return 0;
}


static int jit_expand_array(Heap *heap, Array *arr, int arr_val, int int_val)
{
   return expand_array(heap, arr, arr->len);
}


static uint64_t jit_hash_get(Heap *heap, Value hash, Value key)
{
   Array *arr;
   Value value;
   int err;

   if (!hash.is_array || hash.value <= 0 || hash.value >= heap->size) {
      return 2ULL << 32; // invalid hash access
   }
   
   arr = &heap->data[hash.value];
   if (arr->len == -1 || arr->hash_slots < 0 || arr->is_handle) {
      return 2ULL << 32; // invalid hash access
   }

   err = get_hash_elem(heap, arr, heap, key, &value);
   if (err) {
      if (err == FIXSCRIPT_ERR_KEY_NOT_FOUND) {
         return 3ULL << 32; // key not found
      }
      return 2ULL << 32; // invalid hash access
   }

   return (uint32_t)value.value | (((uint64_t)value.is_array) << 32);
}


static int jit_hash_set(Heap *heap, Value hash, Value key, Value value)
{
   Array *arr;
   int err;

   if (!hash.is_array || hash.value <= 0 || hash.value >= heap->size) {
      return 1; // invalid hash access
   }
   
   arr = &heap->data[hash.value];
   if (arr->len == -1 || arr->hash_slots < 0 || arr->is_handle) {
      return 1; // invalid hash access
   }

   err = fixscript_set_hash_elem(heap, hash, key, value);
   if (err) {
      if (err == FIXSCRIPT_ERR_OUT_OF_MEMORY) {
         return 2; // out of memory
      }
      return 1; // invalid hash access
   }

   return 0;
}


static int jit_add_pc(Heap *heap, int pc)
{
   if (dynarray_add(&heap->jit_pc_mappings, (void *)(intptr_t)heap->jit_code_len)) return 0;
   if (dynarray_add(&heap->jit_pc_mappings, (void *)(intptr_t)pc)) return 0;
   return 1;
}


static int jit_get_pc(Heap *heap, void *ptr)
{
   intptr_t offset = (intptr_t)ptr - (intptr_t)heap->jit_code;
   int i;
   
   for (i=0; i<heap->jit_pc_mappings.len; i+=2) {
      if ((intptr_t)heap->jit_pc_mappings.data[i+0] == offset) {
         return (intptr_t)heap->jit_pc_mappings.data[i+1];
      }
   }
   return 0;
}


static int jit_add_heap_data_ref(Heap *heap)
{
   if (dynarray_add(&heap->jit_heap_data_refs, (void *)(intptr_t)heap->jit_code_len)) return 0;
   return 1;
}


static int jit_add_array_get_ref(Heap *heap)
{
   if (dynarray_add(&heap->jit_array_get_refs, (void *)(intptr_t)heap->jit_code_len)) return 0;
   return 1;
}


static int jit_add_array_set_ref(Heap *heap)
{
   if (dynarray_add(&heap->jit_array_set_refs, (void *)(intptr_t)heap->jit_code_len)) return 0;
   return 1;
}


static int jit_add_array_append_ref(Heap *heap)
{
   if (dynarray_add(&heap->jit_array_append_refs, (void *)(intptr_t)heap->jit_code_len)) return 0;
   return 1;
}


static int jit_add_length_ref(Heap *heap)
{
   if (dynarray_add(&heap->jit_length_refs, (void *)(intptr_t)heap->jit_code_len)) return 0;
   return 1;
}


static inline int jit_add_adjustment_ptr(Heap *heap, JitAdjPtrFunc func)
{
   if (dynarray_add(&heap->jit_adjustments, (void *)(intptr_t)heap->jit_code_len)) return 0;
   if (dynarray_add(&heap->jit_adjustments, func)) return 0;
   return 1;
}


static inline int jit_add_adjustment_int(Heap *heap, JitAdjIntFunc func)
{
   if (dynarray_add(&heap->jit_adjustments, (void *)(intptr_t)-heap->jit_code_len)) return 0;
   if (dynarray_add(&heap->jit_adjustments, func)) return 0;
   return 1;
}


static void jit_update_exec(Heap *heap, int exec)
{
   if (exec != heap->jit_exec) {
      #ifdef _WIN32
         DWORD old_protect;
         VirtualProtect(heap->jit_code, heap->jit_code_cap, exec? PAGE_EXECUTE_READ : PAGE_READWRITE, &old_protect);
      #else
         mprotect(heap->jit_code, heap->jit_code_cap, exec? PROT_READ | PROT_EXEC : PROT_READ | PROT_WRITE);
      #endif
      heap->jit_exec = exec;
   }
}


static void jit_fixup_stack(Heap *heap, void *old_code, int old_size, void *new_code)
{
   intptr_t diff = new_code - old_code;
   StackBlock *block;
   void **fp;
   int end;

   for (block = heap->jit_stack_block; block; block = block->next) {
      if (*block->ret >= old_code && *block->ret < old_code + old_size) {
         *block->ret += diff;
      }
      for (fp = block->fp; fp; fp = fp[0]) {
         if (fp[1] >= old_code && fp[1] < old_code + old_size) {
            end = fp[1] < old_code + heap->jit_entry_func_end;
            fp[1] += diff;
            if (end) {
               break;
            }
         }
      }
   }
}


static int jit_expand(Heap *heap)
{
   int new_cap;
   unsigned char *new_code;

   if (heap->jit_code_cap >= 1<<30) {
      return 0;
   }
   new_cap = heap->jit_code_cap * 2;
   #if defined(_WIN32)
      new_code = VirtualAlloc(NULL, new_cap, MEM_COMMIT, PAGE_READWRITE);
      if (new_code) {
         memcpy(new_code, heap->jit_code, heap->jit_code_cap);
         VirtualFree(heap->jit_code, 0, MEM_RELEASE);
         heap->jit_exec = 0;
      }
   #elif defined(__APPLE__) || defined(__HAIKU__)
      new_code = mmap(NULL, new_cap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, 0, 0);
      if (new_code) {
         memcpy(new_code, heap->jit_code, heap->jit_code_cap);
         munmap(heap->jit_code, heap->jit_code_cap);
         heap->jit_exec = 0;
      }
   #else
      new_code = mremap(heap->jit_code, heap->jit_code_cap, new_cap, MREMAP_MAYMOVE);
   #endif
   if (!new_code) {
      return 0;
   }

   jit_fixup_stack(heap, heap->jit_code, heap->jit_code_cap, new_code);
   heap->jit_code = new_code;
   heap->jit_code_cap = new_cap;
   jit_update_heap_refs(heap);
   return 1;
}


static inline int jit_align(Heap *heap, int align)
{
#ifdef JIT_X86
   while ((heap->jit_code_len & (align-1)) != 0) {
      if (heap->jit_code_len + 1 > heap->jit_code_cap) {
         if (!jit_expand(heap)) return 0;
      }
      heap->jit_code[heap->jit_code_len++] = 0x90;
   }
#else
   #error "not implemented"
#endif
   return 1;
}


static inline int jit_append(Heap *heap, char *buf, int len)
{
   while (heap->jit_code_len + len > heap->jit_code_cap) {
      if (!jit_expand(heap)) return 0;
   }
   memcpy(heap->jit_code + heap->jit_code_len, buf, len);
   heap->jit_code_len += len;
   return 1;
}


static inline int jit_append4(Heap *heap, uint32_t value, int len)
{
   return jit_append(heap, (char *)&value, len);
}


static inline int jit_append8(Heap *heap, uint64_t value, int len)
{
   return jit_append(heap, (char *)&value, len);
}


#define JIT_APPEND(len, ...) if (!jit_append(heap, (char[]) { __VA_ARGS__ }, len)) return 0
#define JIT_APPEND_BYTE(value) if (!jit_append4(heap, value, 1)) return 0
#define JIT_APPEND_SHORT(value) if (!jit_append4(heap, value, 2)) return 0
#define JIT_APPEND_INT(value) if (!jit_append4(heap, value, 4)) return 0
#define JIT_APPEND_LONG(value) if (!jit_append8(heap, value, 8)) return 0

#ifdef JIT_X86
#define adc____eax__ecx()                            JIT_APPEND(2, 0x11,0xC8)
#define adc____eax__imm8(value)                      JIT_APPEND(2, 0x83,0xD0); JIT_APPEND_BYTE(value)
#define add____ecx__ebx()                            JIT_APPEND(2, 0x01,0xD9)
#define add____ecx__eax()                            JIT_APPEND(2, 0x01,0xC1)
#define add____edx__ecx()                            JIT_APPEND(2, 0x01,0xCA)
#define add____esi__eax()                            JIT_APPEND(2, 0x01,0xC6)
#define add____eax__imm8(value)                      JIT_APPEND(2, 0x83,0xC0); JIT_APPEND_BYTE(value)
#define add____eax__imm32(value)                     JIT_APPEND(1, 0x05); JIT_APPEND_INT(value)
#define add____eax__DWORD_PTR_ecx_imm8(value)        JIT_APPEND(2, 0x03,0x41); JIT_APPEND_BYTE(value)
#define add____eax__DWORD_PTR_edx_imm32(value)       JIT_APPEND(2, 0x03,0x82); JIT_APPEND_INT(value)
#define add____eax__DWORD_PTR_redi_imm8(value)       JIT_APPEND(2, 0x03,0x47); JIT_APPEND_BYTE(value)
#define add____eax__DWORD_PTR_redi_imm32(value)      JIT_APPEND(2, 0x03,0x87); JIT_APPEND_INT(value)
#define add____ebx__DWORD_PTR_edx_imm8(value)        JIT_APPEND(2, 0x03,0x5A); JIT_APPEND_BYTE(value)
#define add____edx__imm32(value)                     JIT_APPEND(2, 0x81,0xC2); JIT_APPEND_INT(value)
#define add____ebx__imm32(value)                     JIT_APPEND(2, 0x81,0xC3); JIT_APPEND_INT(value)
#define add____edx__DWORD_PTR_redi_imm8(value)       JIT_APPEND(2, 0x03,0x57); JIT_APPEND_BYTE(value)
#define add____edx__DWORD_PTR_redi_imm32(value)      JIT_APPEND(2, 0x03,0x97); JIT_APPEND_INT(value)
#define add____esi__DWORD_PTR_edx_imm8(value)        JIT_APPEND(2, 0x03,0x72); JIT_APPEND_BYTE(value)
#define add____esi__DWORD_PTR_ebx_imm8(value)        JIT_APPEND(2, 0x03,0x73); JIT_APPEND_BYTE(value)
#define add____edi__DWORD_PTR_edx_imm8(value)        JIT_APPEND(2, 0x03,0x7A); JIT_APPEND_BYTE(value)
#define add____edi__DWORD_PTR_ebx_imm8(value)        JIT_APPEND(2, 0x03,0x7B); JIT_APPEND_BYTE(value)
#define add____esp__imm8(value)                      JIT_APPEND(2, 0x83,0xC4); JIT_APPEND_BYTE(value)
#define and____al__dl()                              JIT_APPEND(2, 0x20,0xD0)
#define and____dl__cl()                              JIT_APPEND(2, 0x20,0xCA)
#define and____eax__imm8(value)                      JIT_APPEND(2, 0x83,0xE0); JIT_APPEND_BYTE(value)
#define and____eax__imm32(value)                     JIT_APPEND(1, 0x25); JIT_APPEND_INT(value)
#define and____eax__DWORD_PTR_redi_imm8(value)       JIT_APPEND(2, 0x23,0x47); JIT_APPEND_BYTE(value)
#define and____eax__DWORD_PTR_redi_imm32(value)      JIT_APPEND(2, 0x23,0x87); JIT_APPEND_INT(value)
#define and____DWORD_PTR_redx_reax_4__ebx()          JIT_APPEND(3, 0x21,0x1C,0x82)
#define call___rel32(value)                          JIT_APPEND(1, 0xE8); JIT_APPEND_INT(value)
#define call___reax()                                JIT_APPEND(2, 0xFF,0xD0)
#define call___rebx()                                JIT_APPEND(2, 0xFF,0xD3)
#define call___DWORD_PTR_ebp_imm8(value)             JIT_APPEND(2, 0xFF,0x55); JIT_APPEND_BYTE(value)
#define cmp____eax__ecx()                            JIT_APPEND(2, 0x39,0xC8)
#define cmp____eax__edx()                            JIT_APPEND(2, 0x39,0xD0)
#define cmp____ecx__edx()                            JIT_APPEND(2, 0x39,0xD1)
#define cmp____ebx__ecx()                            JIT_APPEND(2, 0x39,0xCB)
#define cmp____eax__DWORD_PTR_redi_imm8(value)       JIT_APPEND(2, 0x3B,0x47); JIT_APPEND_BYTE(value)
#define cmp____eax__DWORD_PTR_redi_imm32(value)      JIT_APPEND(2, 0x3B,0x87); JIT_APPEND_INT(value)
#define cmp____eax__DWORD_PTR_redx_imm8(value)       JIT_APPEND(2, 0x3B,0x42); JIT_APPEND_BYTE(value)
#define cmp____eax__imm8(value)                      JIT_APPEND(2, 0x83,0xF8); JIT_APPEND_BYTE(value)
#define cmp____eax__imm32(value)                     JIT_APPEND(1, 0x3D); JIT_APPEND_INT(value)
#define cmp____ecx__imm8(value)                      JIT_APPEND(2, 0x83,0xF9); JIT_APPEND_BYTE(value)
#define cmp____ecx__imm32(value)                     JIT_APPEND(2, 0x81,0xF9); JIT_APPEND_INT(value)
#define cmp____ecx__DWORD_PTR_redx_imm8(value)       JIT_APPEND(2, 0x3B,0x4A); JIT_APPEND_BYTE(value)
#define cmp____ecx__DWORD_PTR_rebx_imm32(value)      JIT_APPEND(2, 0x3B,0x8B); JIT_APPEND_INT(value)
#define cmp____edx__imm8(value)                      JIT_APPEND(2, 0x83,0xFA); JIT_APPEND_BYTE(value)
#define cmp____edx__imm32(value)                     JIT_APPEND(2, 0x81,0xFA); JIT_APPEND_INT(value)
#define cmp____ebx__imm8(value)                      JIT_APPEND(2, 0x83,0xFB); JIT_APPEND_BYTE(value)
#define cmp____ebx__imm32(value)                     JIT_APPEND(2, 0x81,0xFB); JIT_APPEND_INT(value)
#define cmp____BYTE_PTR_resi_recx_1__imm8(value)     JIT_APPEND(3, 0x80,0x3C,0x0E); JIT_APPEND_BYTE(value)
#define cmp____BYTE_PTR_resi_imm8__imm8(val1, val2)  JIT_APPEND(2, 0x80,0x7E); JIT_APPEND_BYTE(val1); JIT_APPEND_BYTE(val2)
#define cmp____BYTE_PTR_resi_imm32__imm8(val1, val2) JIT_APPEND(2, 0x80,0xBE); JIT_APPEND_INT(val1); JIT_APPEND_BYTE(val2)
#define cmp____BYTE_PTR_resi_imm8__bl(value)         JIT_APPEND(2, 0x38,0x5E); JIT_APPEND_BYTE(value)
#define cmp____BYTE_PTR_resi_imm32__bl(value)        JIT_APPEND(2, 0x38,0x9E); JIT_APPEND_INT(value)
#define cmp____DWORD_PTR_recx_imm8__imm8(val1, val2) JIT_APPEND(2, 0x83,0x79); JIT_APPEND_BYTE(val1); JIT_APPEND_BYTE(val2)
#define cmp____DWORD_PTR_recx_imm8__imm32(val1, val2)JIT_APPEND(2, 0x81,0x79); JIT_APPEND_BYTE(val1); JIT_APPEND_INT(val2)
#define dec____eax()                                 JIT_APPEND(1, 0x48)
#define dec____edx()                                 JIT_APPEND(1, 0x4A)
#define dec____DWORD_PTR_redx_imm32(value)           JIT_APPEND(2, 0xFF,0x8A); JIT_APPEND_INT(value)
#define dec____DWORD_PTR_redi_imm8(value)            JIT_APPEND(2, 0xFF,0x4F); JIT_APPEND_BYTE(value)
#define dec____DWORD_PTR_redi_imm32(value)           JIT_APPEND(2, 0xFF,0x8F); JIT_APPEND_INT(value)
#define fadd___DWORD_PTR_edi_imm8(value)             JIT_APPEND(2, 0xD8,0x47); JIT_APPEND_BYTE(value)
#define fadd___DWORD_PTR_edi_imm32(value)            JIT_APPEND(2, 0xD8,0x87); JIT_APPEND_INT(value)
#define fadd___QWORD_PTR_edi_imm8(value)             JIT_APPEND(2, 0xDC,0x47); JIT_APPEND_BYTE(value)
#define fadd___QWORD_PTR_edi_imm32(value)            JIT_APPEND(2, 0xDC,0x87); JIT_APPEND_INT(value)
#define fdiv___DWORD_PTR_edi_imm8(value)             JIT_APPEND(2, 0xD8,0x77); JIT_APPEND_BYTE(value)
#define fdiv___DWORD_PTR_edi_imm32(value)            JIT_APPEND(2, 0xD8,0xB7); JIT_APPEND_INT(value)
#define fdiv___QWORD_PTR_edi_imm8(value)             JIT_APPEND(2, 0xDC,0x77); JIT_APPEND_BYTE(value)
#define fdiv___QWORD_PTR_edi_imm32(value)            JIT_APPEND(2, 0xDC,0xB7); JIT_APPEND_INT(value)
#define fild___DWORD_PTR_edi_imm8(value)             JIT_APPEND(2, 0xDB,0x47); JIT_APPEND_BYTE(value)
#define fild___DWORD_PTR_edi_imm32(value)            JIT_APPEND(2, 0xDB,0x87); JIT_APPEND_INT(value)
#define fild___QWORD_PTR_edi_imm8(value)             JIT_APPEND(2, 0xDF,0x6F); JIT_APPEND_BYTE(value)
#define fild___QWORD_PTR_edi_imm32(value)            JIT_APPEND(2, 0xDF,0xAF); JIT_APPEND_INT(value)
#define fistp__DWORD_PTR_edi_imm8(value)             JIT_APPEND(2, 0xDB,0x5F); JIT_APPEND_BYTE(value)
#define fistp__DWORD_PTR_edi_imm32(value)            JIT_APPEND(2, 0xDB,0x9F); JIT_APPEND_INT(value)
#define fistp__QWORD_PTR_edi_imm8(value)             JIT_APPEND(2, 0xDF,0x7F); JIT_APPEND_BYTE(value)
#define fistp__QWORD_PTR_edi_imm32(value)            JIT_APPEND(2, 0xDF,0xBF); JIT_APPEND_INT(value)
#define fld____DWORD_PTR_edi_imm8(value)             JIT_APPEND(2, 0xD9,0x47); JIT_APPEND_BYTE(value)
#define fld____DWORD_PTR_edi_imm32(value)            JIT_APPEND(2, 0xD9,0x87); JIT_APPEND_INT(value)
#define fld____QWORD_PTR_edi_imm8(value)             JIT_APPEND(2, 0xDD,0x47); JIT_APPEND_BYTE(value)
#define fld____QWORD_PTR_edi_imm32(value)            JIT_APPEND(2, 0xDD,0x87); JIT_APPEND_INT(value)
#define fldcw__WORD_PTR_esp_imm8(value)              JIT_APPEND(3, 0xD9,0x6C,0x24); JIT_APPEND_BYTE(value)
#define fmul___DWORD_PTR_edi_imm8(value)             JIT_APPEND(2, 0xD8,0x4F); JIT_APPEND_BYTE(value)
#define fmul___DWORD_PTR_edi_imm32(value)            JIT_APPEND(2, 0xD8,0x8F); JIT_APPEND_INT(value)
#define fmul___QWORD_PTR_edi_imm8(value)             JIT_APPEND(2, 0xDC,0x4F); JIT_APPEND_BYTE(value)
#define fmul___QWORD_PTR_edi_imm32(value)            JIT_APPEND(2, 0xDC,0x8F); JIT_APPEND_INT(value)
#define fnstcw_WORD_PTR_esp()                        JIT_APPEND(3, 0xD9,0x3C,0x24)
#define fnstsw_ax()                                  JIT_APPEND(2, 0xDF,0xE0)
#define fstp___DWORD_PTR_edi_imm8(value)             JIT_APPEND(2, 0xD9,0x5F); JIT_APPEND_BYTE(value)
#define fstp___DWORD_PTR_edi_imm32(value)            JIT_APPEND(2, 0xD9,0x9F); JIT_APPEND_INT(value)
#define fstp___QWORD_PTR_edi_imm8(value)             JIT_APPEND(2, 0xDD,0x5F); JIT_APPEND_BYTE(value)
#define fstp___QWORD_PTR_edi_imm32(value)            JIT_APPEND(2, 0xDD,0x9F); JIT_APPEND_INT(value)
#define fsub___DWORD_PTR_edi_imm8(value)             JIT_APPEND(2, 0xD8,0x67); JIT_APPEND_BYTE(value)
#define fsub___DWORD_PTR_edi_imm32(value)            JIT_APPEND(2, 0xD8,0xA7); JIT_APPEND_INT(value)
#define fsub___QWORD_PTR_edi_imm8(value)             JIT_APPEND(2, 0xDC,0x67); JIT_APPEND_BYTE(value)
#define fsub___QWORD_PTR_edi_imm32(value)            JIT_APPEND(2, 0xDC,0xA7); JIT_APPEND_INT(value)
#define fucompp()                                    JIT_APPEND(2, 0xDA,0xE9)
#define idiv___ecx()                                 JIT_APPEND(2, 0xF7,0xF9)
#define imul___DWORD_PTR_redi_imm8(value)            JIT_APPEND(2, 0xF7,0x6F); JIT_APPEND_BYTE(value)
#define imul___DWORD_PTR_redi_imm32(value)           JIT_APPEND(2, 0xF7,0xAF); JIT_APPEND_INT(value)
#define imul___eax__ebx()                            JIT_APPEND(3, 0x0F,0xAF,0xC3)
#define imul___ecx__edx()                            JIT_APPEND(3, 0x0F,0xAF,0xCA)
#define imul___eax__DWORD_PTR_redi_imm8(value)       JIT_APPEND(3, 0x0F,0xAF,0x47); JIT_APPEND_BYTE(value)
#define imul___eax__DWORD_PTR_redi_imm32(value)      JIT_APPEND(3, 0x0F,0xAF,0x87); JIT_APPEND_INT(value)
#define imul___edx__eax__imm8(value)                 JIT_APPEND(2, 0x6B,0xD0); JIT_APPEND_BYTE(value)
#define imul___edx__edx__imm8(value)                 JIT_APPEND(2, 0x6B,0xD2); JIT_APPEND_BYTE(value)
#define imul___ebx__edx__imm8(value)                 JIT_APPEND(2, 0x6B,0xDA); JIT_APPEND_BYTE(value)
#define inc____eax()                                 JIT_APPEND(1, 0x40)
#define inc____edx()                                 JIT_APPEND(1, 0x42)
#define inc____DWORD_PTR_redi_imm8(value)            JIT_APPEND(2, 0xFF,0x47); JIT_APPEND_BYTE(value)
#define inc____DWORD_PTR_redi_imm32(value)           JIT_APPEND(2, 0xFF,0x87); JIT_APPEND_INT(value)
#define ja_____rel8(value)                           JIT_APPEND(1, 0x77); JIT_APPEND_BYTE(value)
#define jae____rel32(value)                          JIT_APPEND(2, 0x0F,0x83); JIT_APPEND_INT(value)
#define jb_____rel8(value)                           JIT_APPEND(1, 0x72); JIT_APPEND_BYTE(value)
#define jb_____rel32(value)                          JIT_APPEND(2, 0x0F,0x82); JIT_APPEND_INT(value)
#define jbe____rel32(value)                          JIT_APPEND(2, 0x0F,0x86); JIT_APPEND_INT(value)
#define je_____rel8(value)                           JIT_APPEND(1, 0x74); JIT_APPEND_BYTE(value)
#define je_____rel32(value)                          JIT_APPEND(2, 0x0F,0x84); JIT_APPEND_INT(value)
#define jl_____rel8(value)                           JIT_APPEND(1, 0x7C); JIT_APPEND_BYTE(value)
#define jl_____rel32(value)                          JIT_APPEND(2, 0x0F,0x8C); JIT_APPEND_INT(value)
#define jle____rel8(value)                           JIT_APPEND(1, 0x7E); JIT_APPEND_BYTE(value)
#define jg_____rel8(value)                           JIT_APPEND(1, 0x7F); JIT_APPEND_BYTE(value)
#define jge____rel8(value)                           JIT_APPEND(1, 0x7D); JIT_APPEND_BYTE(value)
#define jge____rel32(value)                          JIT_APPEND(2, 0x0F,0x8D); JIT_APPEND_INT(value)
#define jne____rel32(value)                          JIT_APPEND(2, 0x0F,0x85); JIT_APPEND_INT(value)
#define jmp____reax()                                JIT_APPEND(2, 0xFF,0xE0)
#define jmp____rebx()                                JIT_APPEND(2, 0xFF,0xE3)
#define jmp____rel8(value)                           JIT_APPEND(1, 0xEB); JIT_APPEND_BYTE(value)
#define jmp____rel32(value)                          JIT_APPEND(1, 0xE9); JIT_APPEND_INT(value)
#define jne____rel8(value)                           JIT_APPEND(1, 0x75); JIT_APPEND_BYTE(value)
#define jnz____rel8(value)                           JIT_APPEND(1, 0x75); JIT_APPEND_BYTE(value)
#define jo_____rel32(value)                          JIT_APPEND(2, 0x0F,0x80); JIT_APPEND_INT(value)
#define lea____eax__rebx_imm8(value)                 JIT_APPEND(2, 0x8D,0x43); JIT_APPEND_BYTE(value)
#define lea____eax__resi_imm8(value)                 JIT_APPEND(2, 0x8D,0x46); JIT_APPEND_BYTE(value)
#define lea____eax__resi_imm32(value)                JIT_APPEND(2, 0x8D,0x86); JIT_APPEND_INT(value)
#define lea____eax__esp_imm8(value)                  JIT_APPEND(3, 0x8D,0x44,0x24); JIT_APPEND_BYTE(value)
#define lea____ebx__ebx_4_imm32(value)               JIT_APPEND(3, 0x8D,0x1C,0x9D); JIT_APPEND_INT(value)
#define lea____ebx__eax_imm8(value)                  JIT_APPEND(2, 0x8D,0x58); JIT_APPEND_BYTE(value)
#define lea____edx__eax_imm8(value)                  JIT_APPEND(2, 0x8D,0x50); JIT_APPEND_BYTE(value)
#define lea____ebx__ecx_imm8(value)                  JIT_APPEND(2, 0x8D,0x59); JIT_APPEND_BYTE(value)
#define lea____edi__resi_4()                         JIT_APPEND(7, 0x8D,0x3C,0xB5,0x00,0x00,0x00,0x00)
#define mov____eax__ecx()                            JIT_APPEND(2, 0x89,0xC8)
#define lea____ebp__esp_imm8(value)                  JIT_APPEND(3, 0x8D,0x6C,0x24); JIT_APPEND_BYTE(value)
#define lea____edi__edi_eax_4()                      JIT_APPEND(3, 0x8D,0x3C,0x87)
#define mov____dh__imm(value)                        JIT_APPEND(1, 0xB6); JIT_APPEND_BYTE(value)
#define mov____eax__edx()                            JIT_APPEND(2, 0x89,0xD0)
#define mov____eax__ebx()                            JIT_APPEND(2, 0x89,0xD8)
#define mov____eax__ebp()                            JIT_APPEND(2, 0x89,0xE8)
#define mov____ecx__eax()                            JIT_APPEND(2, 0x89,0xC1)
#define mov____ecx__edx()                            JIT_APPEND(2, 0x89,0xD1)
#define mov____ecx__imm(value)                       JIT_APPEND(1, 0xB9); JIT_APPEND_INT(value)
#define mov____edx__eax()                            JIT_APPEND(2, 0x89,0xC2)
#define mov____ebx__eax()                            JIT_APPEND(2, 0x89,0xC3)
#define mov____ebx__edx()                            JIT_APPEND(2, 0x89,0xD3)
#define mov____edx__ebx()                            JIT_APPEND(2, 0x89,0xDA)
#define mov____ebp__esp()                            JIT_APPEND(2, 0x89,0xE5)
#define mov____esp__ebp()                            JIT_APPEND(2, 0x89,0xEC)
#define mov____eax__imm(value)                       JIT_APPEND(1, 0xB8); JIT_APPEND_INT(value)
#define mov____ebx__imm(value)                       JIT_APPEND(1, 0xBB); JIT_APPEND_INT(value)
#define mov____edx__imm(value)                       JIT_APPEND(1, 0xBA); JIT_APPEND_INT(value)
#define mov____eax__DWORD_PTR_ebx_imm32(value)       JIT_APPEND(2, 0x8B,0x83); JIT_APPEND_INT(value)
#define mov____eax__DWORD_PTR_ebp_imm8(value)        JIT_APPEND(2, 0x8B,0x45); JIT_APPEND_BYTE(value)
#define mov____eax__DWORD_PTR_redi_imm32(value)      JIT_APPEND(2, 0x8B,0x87); JIT_APPEND_INT(value)
#define mov____eax__DWORD_PTR_redi_imm8(value)       JIT_APPEND(2, 0x8B,0x47); JIT_APPEND_BYTE(value)
#define mov____eax__DWORD_PTR_redx_imm8(value)       JIT_APPEND(2, 0x8B,0x42); JIT_APPEND_BYTE(value)
#define mov____eax__DWORD_PTR_reax_recx_4()          JIT_APPEND(3, 0x8B,0x04,0x88)
#define mov____eax__DWORD_PTR_rebx_reax_4()          JIT_APPEND(3, 0x8B,0x04,0x83)
#define mov____eax__DWORD_PTR_recx_imm32(value)      JIT_APPEND(2, 0x8B,0x81); JIT_APPEND_INT(value)
#define mov____ebx__DWORD_PTR_redx_imm8(value)       JIT_APPEND(2, 0x8B,0x5A); JIT_APPEND_BYTE(value)
#define mov____ebx__DWORD_PTR_ebp_imm8(value)        JIT_APPEND(2, 0x8B,0x5D); JIT_APPEND_BYTE(value)
#define mov____ecx__DWORD_PTR_eax_ecx_4()            JIT_APPEND(3, 0x8B,0x0C,0x88)
#define mov____ecx__DWORD_PTR_redx_imm8(value)       JIT_APPEND(2, 0x8B,0x4A); JIT_APPEND_BYTE(value)
#define mov____ecx__DWORD_PTR_redi_imm8(value)       JIT_APPEND(2, 0x8B,0x4F); JIT_APPEND_BYTE(value)
#define mov____ecx__DWORD_PTR_redi_imm32(value)      JIT_APPEND(2, 0x8B,0x8F); JIT_APPEND_INT(value)
#define mov____ebx__DWORD_PTR_redi_imm8(value)       JIT_APPEND(2, 0x8B,0x5F); JIT_APPEND_BYTE(value)
#define mov____ebx__DWORD_PTR_redi_imm32(value)      JIT_APPEND(2, 0x8B,0x9F); JIT_APPEND_INT(value)
#define mov____edi__DWORD_PTR_edx_imm8(value)        JIT_APPEND(2, 0x8B,0x7A); JIT_APPEND_BYTE(value)
#define mov____edx__DWORD_PTR_redi_recx_4()          JIT_APPEND(3, 0x8B,0x14,0x8F)
#define mov____edx__DWORD_PTR_ebp_imm8(value)        JIT_APPEND(2, 0x8B,0x55); JIT_APPEND_BYTE(value)
#define mov____edx__DWORD_PTR_redi_imm8(value)       JIT_APPEND(2, 0x8B,0x57); JIT_APPEND_BYTE(value)
#define mov____edx__DWORD_PTR_redi_imm32(value)      JIT_APPEND(2, 0x8B,0x97); JIT_APPEND_INT(value)
#define mov____edx__DWORD_PTR_edx_imm8(value)        JIT_APPEND(2, 0x8B,0x52); JIT_APPEND_BYTE(value)
#define mov____esi__DWORD_PTR_edx_imm8(value)        JIT_APPEND(2, 0x8B,0x72); JIT_APPEND_BYTE(value)
#define mov____esi__DWORD_PTR_ebp_imm8(value)        JIT_APPEND(2, 0x8B,0x75); JIT_APPEND_BYTE(value)
#define mov____edi__DWORD_PTR_ebp_imm8(value)        JIT_APPEND(2, 0x8B,0x7D); JIT_APPEND_BYTE(value)
#define mov____esp__DWORD_PTR_edx_imm8(value)        JIT_APPEND(2, 0x8B,0x62); JIT_APPEND_BYTE(value)
#define mov____BYTE_PTR_recx_imm8__al(value)         JIT_APPEND(2, 0x88,0x41); JIT_APPEND_BYTE(value)
#define mov____BYTE_PTR_recx_imm32__bl(value)        JIT_APPEND(2, 0x88,0x99); JIT_APPEND_INT(value)
#define mov____BYTE_PTR_rebx_recx_1__al()            JIT_APPEND(3, 0x88,0x04,0x0B)
#define mov____BYTE_PTR_resi_imm8__bl(value)         JIT_APPEND(2, 0x88,0x5E); JIT_APPEND_BYTE(value)
#define mov____BYTE_PTR_resi_imm32__bl(value)        JIT_APPEND(2, 0x88,0x9E); JIT_APPEND_INT(value)
#define mov____BYTE_PTR_resi_imm8__cl(value)         JIT_APPEND(2, 0x88,0x4E); JIT_APPEND_BYTE(value)
#define mov____BYTE_PTR_resi_imm32__cl(value)        JIT_APPEND(2, 0x88,0x8E); JIT_APPEND_INT(value)
#define mov____BYTE_PTR_resi_imm8__imm(val1, val2)   JIT_APPEND(2, 0xC6,0x46); JIT_APPEND_BYTE(val1); JIT_APPEND_BYTE(val2)
#define mov____BYTE_PTR_resi_imm32__imm(val1, val2)  JIT_APPEND(2, 0xC6,0x86); JIT_APPEND_INT(val1); JIT_APPEND_BYTE(val2)
#define mov____WORD_PTR_rebx_recx_2__ax()            JIT_APPEND(4, 0x66,0x89,0x04,0x4B)
#define mov____WORD_PTR_esp_imm8__dx(value)          JIT_APPEND(4, 0x66,0x89,0x54,0x24); JIT_APPEND_BYTE(value)
#define mov____DWORD_PTR_recx_imm32__eax(value)      JIT_APPEND(2, 0x89,0x81); JIT_APPEND_INT(value)
#define mov____DWORD_PTR_redx_imm8__eax(value)       JIT_APPEND(2, 0x89,0x42); JIT_APPEND_BYTE(value)
#define mov____DWORD_PTR_edx_imm8__esp(value)        JIT_APPEND(2, 0x89,0x62); JIT_APPEND_BYTE(value)
#define mov____DWORD_PTR_edx_imm8__imm(val1, val2)   JIT_APPEND(2, 0xC7,0x42); JIT_APPEND_BYTE(val1); JIT_APPEND_INT(val2)
#define mov____DWORD_PTR_redx_imm8__ebx(value)       JIT_APPEND(2, 0x89,0x5A); JIT_APPEND_BYTE(value)
#define mov____DWORD_PTR_redx_imm8__esi(value)       JIT_APPEND(2, 0x89,0x72); JIT_APPEND_BYTE(value)
#define mov____DWORD_PTR_rebx_imm8__eax(value)       JIT_APPEND(2, 0x89,0x43); JIT_APPEND_BYTE(value)
#define mov____DWORD_PTR_rebx_recx_4__eax()          JIT_APPEND(3, 0x89,0x04,0x8B)
#define mov____DWORD_PTR_redi_imm8__eax(value)       JIT_APPEND(2, 0x89,0x47); JIT_APPEND_BYTE(value)
#define mov____DWORD_PTR_redi_imm32__eax(value)      JIT_APPEND(2, 0x89,0x87); JIT_APPEND_INT(value)
#define mov____DWORD_PTR_redi_imm8__edx(value)       JIT_APPEND(2, 0x89,0x57); JIT_APPEND_BYTE(value)
#define mov____DWORD_PTR_redi_imm32__edx(value)      JIT_APPEND(2, 0x89,0x97); JIT_APPEND_INT(value)
#define mov____DWORD_PTR_redi_imm8__imm(v1, v2)      JIT_APPEND(2, 0xC7,0x47); JIT_APPEND_BYTE(v1); JIT_APPEND_INT(v2)
#define mov____DWORD_PTR_redi_imm32__imm(v1, v2)     JIT_APPEND(2, 0xC7,0x87); JIT_APPEND_INT(v1); JIT_APPEND_INT(v2)
#define movzx__eax__dl()                             JIT_APPEND(3, 0x0F,0xB6,0xC2)
#define movzx__eax__BYTE_PTR_resi_imm8(value)        JIT_APPEND(3, 0x0F,0xB6,0x46); JIT_APPEND_BYTE(value)
#define movzx__eax__BYTE_PTR_resi_imm32(value)       JIT_APPEND(3, 0x0F,0xB6,0x86); JIT_APPEND_INT(value)
#define movzx__eax__BYTE_PTR_reax_recx_1()           JIT_APPEND(4, 0x0F,0xB6,0x04,0x08)
#define movzx__eax__WORD_PTR_reax_recx_2()           JIT_APPEND(4, 0x0F,0xB7,0x04,0x48)
#define movzx__ebx__BYTE_PTR_recx_imm32(value)       JIT_APPEND(3, 0x0F,0xB6,0x99); JIT_APPEND_INT(value)
#define movzx__ecx__BYTE_PTR_resi_imm8(value)        JIT_APPEND(3, 0x0F,0xB6,0x4E); JIT_APPEND_BYTE(value)
#define movzx__ecx__BYTE_PTR_resi_imm32(value)       JIT_APPEND(3, 0x0F,0xB6,0x8E); JIT_APPEND_INT(value)
#define movzx__edx__WORD_PTR_esp()                   JIT_APPEND(4, 0x0F,0xB7,0x14,0x24)
#define movzx__ebx__BYTE_PTR_edx_imm32(value)        JIT_APPEND(3, 0x0F,0xB6,0x9A); JIT_APPEND_INT(value)
#define movzx__ebx__BYTE_PTR_resi_imm8(value)        JIT_APPEND(3, 0x0F,0xB6,0x5E); JIT_APPEND_BYTE(value)
#define movzx__ebx__BYTE_PTR_resi_imm32(value)       JIT_APPEND(3, 0x0F,0xB6,0x9E); JIT_APPEND_INT(value)
#define movzx__ebx__BYTE_PTR_ebx_edx_2_imm32(value)  JIT_APPEND(4, 0x0F,0xB6,0x9C,0x53); JIT_APPEND_INT(value)
#define mul____edx()                                 JIT_APPEND(2, 0xF7,0xE2)
#define mul____DWORD_PTR_redi_imm8(value)            JIT_APPEND(2, 0xF7,0x67); JIT_APPEND_BYTE(value)
#define mul____DWORD_PTR_redi_imm32(value)           JIT_APPEND(2, 0xF7,0xA7); JIT_APPEND_INT(value)
#define neg____eax()                                 JIT_APPEND(2, 0xF7,0xD8)
#define or_____al__dl()                              JIT_APPEND(2, 0x08,0xD0)
#define or_____eax__DWORD_PTR_redi_imm8(value)       JIT_APPEND(2, 0x0B,0x47); JIT_APPEND_BYTE(value)
#define or_____eax__DWORD_PTR_redi_imm32(value)      JIT_APPEND(2, 0x0B,0x87); JIT_APPEND_INT(value)
#define or_____DWORD_PTR_redx_reax_4__ebx()          JIT_APPEND(3, 0x09,0x1C,0x82)
#define pop____reax()                                JIT_APPEND(1, 0x58)
#define pop____recx()                                JIT_APPEND(1, 0x59)
#define pop____redx()                                JIT_APPEND(1, 0x5A)
#define pop____rebx()                                JIT_APPEND(1, 0x5B)
#define pop____rebp()                                JIT_APPEND(1, 0x5D)
#define pop____resi()                                JIT_APPEND(1, 0x5E)
#define pop____redi()                                JIT_APPEND(1, 0x5F)
#define pop____DWORD_PTR_edx_imm8(value)             JIT_APPEND(2, 0x8F,0x42); JIT_APPEND_BYTE(value)
#define push___reax()                                JIT_APPEND(1, 0x50)
#define push___recx()                                JIT_APPEND(1, 0x51)
#define push___redx()                                JIT_APPEND(1, 0x52)
#define push___rebx()                                JIT_APPEND(1, 0x53)
#define push___rebp()                                JIT_APPEND(1, 0x55)
#define push___resi()                                JIT_APPEND(1, 0x56)
#define push___redi()                                JIT_APPEND(1, 0x57)
#define push___imm8(value)                           JIT_APPEND(1, 0x6A); JIT_APPEND_BYTE(value)
#define push___imm32(value)                          JIT_APPEND(1, 0x68); JIT_APPEND_INT(value)
#define push___DWORD_PTR_edx_imm8(value)             JIT_APPEND(2, 0xFF,0x72); JIT_APPEND_BYTE(value)
#define push___DWORD_PTR_ebp_imm8(value)             JIT_APPEND(2, 0xFF,0x75); JIT_APPEND_BYTE(value)
#define push___DWORD_PTR_edi_imm8(value)             JIT_APPEND(2, 0xFF,0x77); JIT_APPEND_BYTE(value)
#define push___DWORD_PTR_edi_imm32(value)            JIT_APPEND(2, 0xFF,0xB7); JIT_APPEND_INT(value)
#define ret____()                                    JIT_APPEND(1, 0xC3)
#define rol____ebx__cl()                             JIT_APPEND(2, 0xD3,0xC3)
#define sahf___()                                    JIT_APPEND(1, 0x9E)
#define sar____eax__cl()                             JIT_APPEND(2, 0xD3,0xF8)
#define sar____edx__imm(value)                       JIT_APPEND(2, 0xC1,0xFA); JIT_APPEND_BYTE(value)
#define sbb____eax__ecx()                            JIT_APPEND(2, 0x19,0xC8)
#define sbb____eax__imm8(value)                      JIT_APPEND(2, 0x83,0xD8); JIT_APPEND_BYTE(value)
#define seta___cl()                                  JIT_APPEND(3, 0x0F,0x97,0xC1)
#define seta___dl()                                  JIT_APPEND(3, 0x0F,0x97,0xC2)
#define setae__cl()                                  JIT_APPEND(3, 0x0F,0x93,0xC1)
#define setb___dl()                                  JIT_APPEND(3, 0x0F,0x92,0xC2)
#define sete___dl()                                  JIT_APPEND(3, 0x0F,0x94,0xC2)
#define setl___dl()                                  JIT_APPEND(3, 0x0F,0x9C,0xC2)
#define setle__dl()                                  JIT_APPEND(3, 0x0F,0x9E,0xC2)
#define setne__cl()                                  JIT_APPEND(3, 0x0F,0x95,0xC1)
#define setne__dl()                                  JIT_APPEND(3, 0x0F,0x95,0xC2)
#define setne__bl()                                  JIT_APPEND(3, 0x0F,0x95,0xC3)
#define setnp__dl()                                  JIT_APPEND(3, 0x0F,0x9B,0xC2)
#define shl____eax__imm(value)                       JIT_APPEND(2, 0xC1,0xE0); JIT_APPEND_BYTE(value)
#define shl____eax__cl()                             JIT_APPEND(2, 0xD3,0xE0)
#define shl____edx__imm(value)                       JIT_APPEND(2, 0xC1,0xE2); JIT_APPEND_BYTE(value)
#define shl____ebx__imm(value)                       JIT_APPEND(2, 0xC1,0xE3); JIT_APPEND_BYTE(value)
#define shl____ebx__cl()                             JIT_APPEND(2, 0xD3,0xE3)
#define shr____eax__imm(value)                       JIT_APPEND(2, 0xC1,0xE8); JIT_APPEND_BYTE(value)
#define shr____eax__cl()                             JIT_APPEND(2, 0xD3,0xE8)
#define sub____ecx__eax()                            JIT_APPEND(2, 0x29,0xC1)
#define sub____ecx__imm32(value)                     JIT_APPEND(2, 0x81,0xE9); JIT_APPEND_INT(value)
#define sub____eax__imm8(value)                      JIT_APPEND(2, 0x83,0xE8); JIT_APPEND_BYTE(value)
#define sub____eax__imm32(value)                     JIT_APPEND(1, 0x2D); JIT_APPEND_INT(value)
#define sub____edx__imm8(value)                      JIT_APPEND(2, 0x83,0xEA); JIT_APPEND_BYTE(value)
#define sub____edx__imm32(value)                     JIT_APPEND(2, 0x81,0xEA); JIT_APPEND_INT(value)
#define sub____edx__ecx()                            JIT_APPEND(2, 0x29,0xCA)
#define sub____eax__DWORD_PTR_redi_imm8(value)       JIT_APPEND(2, 0x2B,0x47); JIT_APPEND_BYTE(value)
#define sub____eax__DWORD_PTR_redi_imm32(value)      JIT_APPEND(2, 0x2B,0x87); JIT_APPEND_INT(value)
#define sub____edx__DWORD_PTR_redi_imm8(value)       JIT_APPEND(2, 0x2B,0x57); JIT_APPEND_BYTE(value)
#define sub____edx__DWORD_PTR_redi_imm32(value)      JIT_APPEND(2, 0x2B,0x97); JIT_APPEND_INT(value)
#define sub____esi__DWORD_PTR_ebx_imm8(value)        JIT_APPEND(2, 0x2B,0x73); JIT_APPEND_BYTE(value)
#define sub____edi__DWORD_PTR_ebx_imm8(value)        JIT_APPEND(2, 0x2B,0x7B); JIT_APPEND_BYTE(value)
#define sub____esp__imm8(value)                      JIT_APPEND(2, 0x83,0xEC); JIT_APPEND_BYTE(value)
#define sub____DWORD_PTR_esp__imm8(value)            JIT_APPEND(3, 0x83,0x2C,0x24); JIT_APPEND_BYTE(value)
#define test___eax__imm(value)                       JIT_APPEND(1, 0xA9); JIT_APPEND_INT(value)
#define test___eax__ebx()                            JIT_APPEND(2, 0x85,0xD8)
#define xchg___ecx__eax()                            JIT_APPEND(1, 0x91)
#define xor____cl__imm(value)                        JIT_APPEND(2, 0x80,0xF1); JIT_APPEND_BYTE(value)
#define xor____dl__imm(value)                        JIT_APPEND(2, 0x80,0xF2); JIT_APPEND_BYTE(value)
#define xor____eax__eax()                            JIT_APPEND(2, 0x31,0xC0)
#define xor____ecx__ecx()                            JIT_APPEND(2, 0x31,0xC9)
#define xor____ebx__ebx()                            JIT_APPEND(2, 0x31,0xDB)
#define xor____eax__imm8(value)                      JIT_APPEND(2, 0x83,0xF0); JIT_APPEND_BYTE(value)
#define xor____eax__DWORD_PTR_redi_imm8(value)       JIT_APPEND(2, 0x33,0x47); JIT_APPEND_BYTE(value)
#define xor____eax__DWORD_PTR_redi_imm32(value)      JIT_APPEND(2, 0x33,0x87); JIT_APPEND_INT(value)

#ifdef JIT_X86_64
#define add____rax__QWORD_PTR_rbx_imm32(value)       JIT_APPEND(3, 0x48,0x03,0x83); JIT_APPEND_INT(value)
#define add____rax__QWORD_PTR_r12_imm32(value)       JIT_APPEND(4, 0x49,0x03,0x84,0x24); JIT_APPEND_INT(value)
#define add____rcx__rbx()                            JIT_APPEND(3, 0x48,0x01,0xD9)
#define add____rdx__r8()                             JIT_APPEND(3, 0x4C,0x01,0xC2)
#define add____rbx__r8()                             JIT_APPEND(3, 0x4C,0x01,0xC3)
#define add____rbx__r14()                            JIT_APPEND(3, 0x4C,0x01,0xF3)
#define add____rbx__r15()                            JIT_APPEND(3, 0x4C,0x01,0xFB)
#define add____rbx__QWORD_PTR_rdx_imm8(value)        JIT_APPEND(3, 0x48,0x03,0x5A); JIT_APPEND_BYTE(value)
#define add____rsp__imm8(value)                      JIT_APPEND(3, 0x48,0x83,0xC4); JIT_APPEND_BYTE(value)
#define add____rsi__rax()                            JIT_APPEND(3, 0x48,0x01,0xC6)
#define add____rsi__QWORD_PTR_rbx_imm8(value)        JIT_APPEND(3, 0x48,0x03,0x73); JIT_APPEND_BYTE(value)
#define add____rdi__QWORD_PTR_rbx_imm8(value)        JIT_APPEND(3, 0x48,0x03,0x7B); JIT_APPEND_BYTE(value)
#define addsd__xmm0__xmm1()                          JIT_APPEND(4, 0xF2,0x0F,0x58,0xC1)
#define addss__xmm0__DWORD_PTR_rdi_imm8(value)       JIT_APPEND(4, 0xF3,0x0F,0x58,0x47); JIT_APPEND_BYTE(value)
#define addss__xmm0__DWORD_PTR_rdi_imm32(value)      JIT_APPEND(4, 0xF3,0x0F,0x58,0x87); JIT_APPEND_INT(value)
#define call___rdx()                                 JIT_APPEND(2, 0xFF,0xD2)
#define call___rsi()                                 JIT_APPEND(2, 0xFF,0xD6)
#define cmp____rax__r8()                             JIT_APPEND(3, 0x4C,0x39,0xC0)
#define cmp____rcx__imm8(value)                      JIT_APPEND(3, 0x48,0x83,0xF9); JIT_APPEND_BYTE(value)
#define cmpsd__xmm0__xmm1(value)                     JIT_APPEND(4, 0xF2,0x0F,0xC2,0xC1); JIT_APPEND_BYTE(value)
#define cmpsd__xmm1__xmm0(value)                     JIT_APPEND(4, 0xF2,0x0F,0xC2,0xC8); JIT_APPEND_BYTE(value)
#define cmpss__xmm0__xmm1(value)                     JIT_APPEND(4, 0xF3,0x0F,0xC2,0xC1); JIT_APPEND_BYTE(value)
#define cmpss__xmm0__DWORD_PTR_rdi_imm8(val1, type)  JIT_APPEND(4, 0xF3,0x0F,0xC2,0x47); JIT_APPEND_BYTE(val1); JIT_APPEND_BYTE(type)
#define cmpss__xmm0__DWORD_PTR_rdi_imm32(val1, type) JIT_APPEND(4, 0xF3,0x0F,0xC2,0x87); JIT_APPEND_INT(val1); JIT_APPEND_BYTE(type)
#define cvtsd2ss__xmm0__xmm0()                       JIT_APPEND(4, 0xF2,0x0F,0x5A,0xC0)
#define cvtsi2sd__xmm0__rax()                        JIT_APPEND(5, 0xF2,0x48,0x0F,0x2A,0xC0)
#define cvtsi2ss__xmm0__eax()                        JIT_APPEND(4, 0xF3,0x0F,0x2A,0xC0)
#define cvtss2sd__xmm0__xmm0()                       JIT_APPEND(4, 0xF3,0x0F,0x5A,0xC0)
#define cvtss2sd__xmm1__xmm1()                       JIT_APPEND(4, 0xF3,0x0F,0x5A,0xC9)
#define cvtss2sd__xmm2__xmm2()                       JIT_APPEND(4, 0xF3,0x0F,0x5A,0xD2)
#define cvttsd2si__rax__xmm0()                       JIT_APPEND(5, 0xF2,0x48,0x0F,0x2C,0xC0)
#define cvttss2si__eax__xmm0()                       JIT_APPEND(4, 0xF3,0x0F,0x2C,0xC0)
#define div____rcx()                                 JIT_APPEND(3, 0x48,0xF7,0xF1)
#define divsd__xmm0__xmm1()                          JIT_APPEND(4, 0xF2,0x0F,0x5E,0xC1)
#define divss__xmm0__xmm1()                          JIT_APPEND(4, 0xF3,0x0F,0x5E,0xC1)
#define idiv___rcx()                                 JIT_APPEND(3, 0x48,0xF7,0xF9)
#define lea____rdx__rsp_imm8(value)                  JIT_APPEND(4, 0x48,0x8D,0x54,0x24); JIT_APPEND_BYTE(value)
#define lea____rbx__r10_rbx_4_imm32(value)           JIT_APPEND(4, 0x49,0x8D,0x9C,0x9A); JIT_APPEND_INT(value)
#define lea____rbp__rsp_imm8(value)                  JIT_APPEND(4, 0x48,0x8D,0x6C,0x24); JIT_APPEND_BYTE(value)
#define lea____rdi__rdi_rax_4()                      JIT_APPEND(4, 0x48,0x8D,0x3C,0x87)
#define lea____r8__rsp_imm8(value)                   JIT_APPEND(4, 0x4C,0x8D,0x44,0x24); JIT_APPEND_BYTE(value)
#define minsd__xmm0__xmm1()                          JIT_APPEND(4, 0xF2,0x0F,0x5D,0xC1)
#define minsd__xmm0__xmm2()                          JIT_APPEND(4, 0xF2,0x0F,0x5D,0xC2)
#define minss__xmm0__DWORD_PTR_rdi_imm8(value)       JIT_APPEND(4, 0xF3,0x0F,0x5D,0x47); JIT_APPEND_BYTE(value)
#define minss__xmm0__DWORD_PTR_rdi_imm32(value)      JIT_APPEND(4, 0xF3,0x0F,0x5D,0x87); JIT_APPEND_INT(value)
#define maxsd__xmm0__xmm1()                          JIT_APPEND(4, 0xF2,0x0F,0x5F,0xC1)
#define maxss__xmm0__DWORD_PTR_rdi_imm8(value)       JIT_APPEND(4, 0xF3,0x0F,0x5F,0x47); JIT_APPEND_BYTE(value)
#define maxss__xmm0__DWORD_PTR_rdi_imm32(value)      JIT_APPEND(4, 0xF3,0x0F,0x5F,0x87); JIT_APPEND_INT(value)
#define mov____eax__eax()                            JIT_APPEND(2, 0x89,0xC0)
#define mov____eax__DWORD_PTR_rcx_imm8(value)        JIT_APPEND(2, 0x8B,0x41); JIT_APPEND_BYTE(value)
#define mov____eax__DWORD_PTR_r12_imm8(value)        JIT_APPEND(4, 0x41,0x8B,0x44,0x24); JIT_APPEND_BYTE(value)
#define mov____rax__imm32(value)                     JIT_APPEND(3, 0x48,0xC7,0xC0); JIT_APPEND_INT(value)
#define mov____rax__imm64(value)                     JIT_APPEND(2, 0x48,0xB8); JIT_APPEND_LONG(value)
#define mov____rax__rdx()                            JIT_APPEND(3, 0x48,0x89,0xD0)
#define mov____rax__QWORD_PTR_rdx_imm8(value)        JIT_APPEND(3, 0x48,0x8B,0x42); JIT_APPEND_BYTE(value)
#define mov____rax__QWORD_PTR_rbx_imm32(value)       JIT_APPEND(3, 0x48,0x8B,0x83); JIT_APPEND_INT(value)
#define mov____rax__QWORD_PTR_r12_imm8(value)        JIT_APPEND(4, 0x49,0x8B,0x44,0x24); JIT_APPEND_BYTE(value)
#define mov____rcx__rax()                            JIT_APPEND(3, 0x48,0x89,0xC1)
#define mov____rcx__rdx()                            JIT_APPEND(3, 0x48,0x89,0xD1)
#define mov____rcx__rbp()                            JIT_APPEND(3, 0x48,0x89,0xE9)
#define mov____rcx__r12()                            JIT_APPEND(3, 0x4C,0x89,0xE1)
#define mov____rcx__imm(value)                       JIT_APPEND(3, 0x48,0xC7,0xC1); JIT_APPEND_INT(value)
#define mov____rcx__QWORD_PTR_rax_rcx_8()            JIT_APPEND(4, 0x48,0x8B,0x0C,0xC8)
#define mov____rcx__QWORD_PTR_rdx_imm8(value)        JIT_APPEND(3, 0x48,0x8B,0x4A); JIT_APPEND_BYTE(value)
#define mov____rcx__QWORD_PTR_r12_imm8(value)        JIT_APPEND(4, 0x49,0x8B,0x4C,0x24); JIT_APPEND_BYTE(value)
#define mov____rdx__imm64(value)                     JIT_APPEND(2, 0x48,0xBA); JIT_APPEND_LONG(value)
#define mov____rdx__rax()                            JIT_APPEND(3, 0x48,0x89,0xC2)
#define mov____rdx__rbx()                            JIT_APPEND(3, 0x48,0x89,0xDA)
#define mov____rdx__r12()                            JIT_APPEND(3, 0x4C,0x89,0xE2)
#define mov____rdx__QWORD_PTR_rdx_imm8(value)        JIT_APPEND(3, 0x48,0x8B,0x52); JIT_APPEND_BYTE(value)
#define mov____rdx__QWORD_PTR_rsp()                  JIT_APPEND(4, 0x48,0x8B,0x14,0x24)
#define mov____rbx__r12()                            JIT_APPEND(3, 0x4C,0x89,0xE3)
#define mov____rbx__QWORD_PTR_rdx_imm8(value)        JIT_APPEND(3, 0x48,0x8B,0x5A); JIT_APPEND_BYTE(value)
#define mov____esi__DWORD_PTR_redi_imm8(value)       JIT_APPEND(2, 0x8B,0x77); JIT_APPEND_BYTE(value)
#define mov____esi__DWORD_PTR_redi_imm32(value)      JIT_APPEND(2, 0x8B,0xB7); JIT_APPEND_INT(value)
#define mov____rsi__imm64(value)                     JIT_APPEND(2, 0x48,0xBE); JIT_APPEND_LONG(value)
#define mov____rsi__rbx()                            JIT_APPEND(3, 0x48,0x89,0xDE)
#define mov____rdi__r12()                            JIT_APPEND(3, 0x4C,0x89,0xE7)
#define mov____r8d__eax()                            JIT_APPEND(3, 0x41,0x89,0xC0)
#define mov____r8d__DWORD_PTR_rdi_imm8(value)        JIT_APPEND(3, 0x44,0x8B,0x47); JIT_APPEND_BYTE(value)
#define mov____r8d__DWORD_PTR_rdi_imm32(value)       JIT_APPEND(3, 0x44,0x8B,0x87); JIT_APPEND_INT(value)
#define mov____r8__rdx()                             JIT_APPEND(3, 0x49,0x89,0xD0)
#define mov____r9d__eax()                            JIT_APPEND(3, 0x41,0x89,0xC1)
#define mov____r9__rax()                             JIT_APPEND(3, 0x49,0x89,0xC1)
#define mov____r9__rbp()                             JIT_APPEND(3, 0x49,0x89,0xE9)
#define mov____esi__eax()                            JIT_APPEND(2, 0x89,0xC6)
#define mov____esi__edx()                            JIT_APPEND(2, 0x89,0xD6)
#define mov____esi__imm(value)                       JIT_APPEND(1, 0xBE); JIT_APPEND_INT(value)
#define mov____rsi__QWORD_PTR_rsp()                  JIT_APPEND(4, 0x48,0x8B,0x34,0x24)
#define mov____esi__DWORD_PTR_r12_imm8(value)        JIT_APPEND(4, 0x41,0x8B,0x74,0x24); JIT_APPEND_BYTE(value)
#define mov____rsi__QWORD_PTR_rdx_imm8(value)        JIT_APPEND(3, 0x48,0x8B,0x72); JIT_APPEND_BYTE(value)
#define mov____rdi__QWORD_PTR_rdx_imm8(value)        JIT_APPEND(3, 0x48,0x8B,0x7A); JIT_APPEND_BYTE(value)
#define mov____rbp__rsp()                            JIT_APPEND(3, 0x48,0x89,0xE5)
#define mov____rsp__rbp()                            JIT_APPEND(3, 0x48,0x89,0xEC)
#define mov____rsp__QWORD_PTR_r12_imm8(value)        JIT_APPEND(4, 0x49,0x8B,0x64,0x24); JIT_APPEND_BYTE(value)
#define mov____r8d__imm(value)                       JIT_APPEND(2, 0x41,0xB8); JIT_APPEND_INT(value)
#define mov____r8__r12()                             JIT_APPEND(3, 0x4D,0x89,0xE0)
#define mov____r8__imm64(value)                      JIT_APPEND(2, 0x49,0xB8); JIT_APPEND_LONG(value)
#define mov____r9d__edx()                            JIT_APPEND(3, 0x41,0x89,0xD1)
#define mov____r10__QWORD_PTR_r12_imm32(value)       JIT_APPEND(4, 0x4D,0x8B,0x94,0x24); JIT_APPEND_INT(value)
#define mov____r12__rcx()                            JIT_APPEND(3, 0x49,0x89,0xCC)
#define mov____r12__rdi()                            JIT_APPEND(3, 0x49,0x89,0xFC)
#define mov____r13__QWORD_PTR_r12_imm32(value)       JIT_APPEND(4, 0x4D,0x8B,0xAC,0x24); JIT_APPEND_INT(value)
#define mov____r14__QWORD_PTR_r12_imm32(value)       JIT_APPEND(4, 0x4D,0x8B,0xB4,0x24); JIT_APPEND_INT(value)
#define mov____r15__QWORD_PTR_r12_imm32(value)       JIT_APPEND(4, 0x4D,0x8B,0xBC,0x24); JIT_APPEND_INT(value)
#define mov____DWORD_PTR_r12_imm8__imm(val1, val2)   JIT_APPEND(4, 0x41,0xC7,0x44,0x24); JIT_APPEND_BYTE(val1); JIT_APPEND_INT(val2)
#define mov____DWORD_PTR_r12_imm8__eax(value)        JIT_APPEND(4, 0x41,0x89,0x44,0x24); JIT_APPEND_BYTE(value)
#define mov____DWORD_PTR_r12_imm8__edx(value)        JIT_APPEND(4, 0x41,0x89,0x54,0x24); JIT_APPEND_BYTE(value)
#define mov____DWORD_PTR_r12_imm8__r8d(value)        JIT_APPEND(4, 0x45,0x89,0x44,0x24); JIT_APPEND_BYTE(value)
#define mov____QWORD_PTR_r12_imm8__rax(value)        JIT_APPEND(4, 0x49,0x89,0x44,0x24); JIT_APPEND_BYTE(value)
#define mov____QWORD_PTR_r12_imm8__rsp(value)        JIT_APPEND(4, 0x49,0x89,0x64,0x24); JIT_APPEND_BYTE(value)
#define movd___eax__xmm0()                           JIT_APPEND(4, 0x66,0x0F,0x7E,0xC0)
#define movd___eax__xmm1()                           JIT_APPEND(4, 0x66,0x0F,0x7E,0xC8)
#define movd___xmm0__eax()                           JIT_APPEND(4, 0x66,0x0F,0x6E,0xC0)
#define movd___xmm0__DWORD_PTR_rdi_imm8(value)       JIT_APPEND(4, 0x66,0x0F,0x6E,0x47); JIT_APPEND_BYTE(value)
#define movd___xmm0__DWORD_PTR_rdi_imm32(value)      JIT_APPEND(4, 0x66,0x0F,0x6E,0x87); JIT_APPEND_INT(value)
#define movd___xmm1__eax()                           JIT_APPEND(4, 0x66,0x0F,0x6E,0xC8)
#define movd___xmm1__DWORD_PTR_rdi_imm8(value)       JIT_APPEND(4, 0x66,0x0F,0x6E,0x4F); JIT_APPEND_BYTE(value)
#define movd___xmm1__DWORD_PTR_rdi_imm32(value)      JIT_APPEND(4, 0x66,0x0F,0x6E,0x8F); JIT_APPEND_INT(value)
#define movd___xmm2__eax()                           JIT_APPEND(4, 0x66,0x0F,0x6E,0xD0)
#define movq___rax__xmm0()                           JIT_APPEND(5, 0x66,0x48,0x0F,0x7E,0xC0)
#define movq___xmm0__rax()                           JIT_APPEND(5, 0x66,0x48,0x0F,0x6E,0xC0)
#define movq___xmm0__QWORD_PTR_rdi_imm8(value)       JIT_APPEND(4, 0xF3,0x0F,0x7E,0x47); JIT_APPEND_BYTE(value)
#define movq___xmm0__QWORD_PTR_rdi_imm32(value)      JIT_APPEND(4, 0xF3,0x0F,0x7E,0x87); JIT_APPEND_INT(value)
#define movq___xmm1__rax()                           JIT_APPEND(5, 0x66,0x48,0x0F,0x6E,0xC8)
#define movq___xmm1__QWORD_PTR_rdi_imm8(value)       JIT_APPEND(4, 0xF3,0x0F,0x7E,0x4F); JIT_APPEND_BYTE(value)
#define movq___xmm1__QWORD_PTR_rdi_imm32(value)      JIT_APPEND(4, 0xF3,0x0F,0x7E,0x8F); JIT_APPEND_INT(value)
#define movq___xmm2__rax()                           JIT_APPEND(5, 0x66,0x48,0x0F,0x6E,0xD0)
#define movzx__ebx__BYTE_PTR_rbx_rdx_2()             JIT_APPEND(4, 0x0F,0xB6,0x1C,0x53)
#define movzx__rbx__BYTE_PTR_rdx_r13()               JIT_APPEND(5, 0x4A,0x0F,0xB6,0x1C,0x2A)
#define mulsd__xmm0__xmm1()                          JIT_APPEND(4, 0xF2,0x0F,0x59,0xC1)
#define mulss__xmm0__DWORD_PTR_rdi_imm8(value)       JIT_APPEND(4, 0xF3,0x0F,0x59,0x47); JIT_APPEND_BYTE(value)
#define mulss__xmm0__DWORD_PTR_rdi_imm32(value)      JIT_APPEND(4, 0xF3,0x0F,0x59,0x87); JIT_APPEND_INT(value)
#define or_____rax__rcx()                            JIT_APPEND(3, 0x48,0x09,0xC8)
#define or_____rcx__rbx()                            JIT_APPEND(3, 0x48,0x09,0xD9)
#define or_____rcx__rdx(value)                       JIT_APPEND(3, 0x48,0x09,0xD1)
#define or_____rdx__rax()                            JIT_APPEND(3, 0x48,0x09,0xC2)
#define or_____rdx__rbx()                            JIT_APPEND(3, 0x48,0x09,0xDA)
#define or_____rsi__rax()                            JIT_APPEND(3, 0x48,0x09,0xC6)
#define or_____rsi__rbx(value)                       JIT_APPEND(3, 0x48,0x09,0xDE)
#define or_____r8__rax()                             JIT_APPEND(3, 0x49,0x09,0xC0)
#define or_____r8__rbx()                             JIT_APPEND(3, 0x49,0x09,0xD8)
#define or_____r9__rcx()                             JIT_APPEND(3, 0x49,0x09,0xC9)
#define or_____r9__rbx()                             JIT_APPEND(3, 0x49,0x09,0xD9)
#define pop____r12()                                 JIT_APPEND(2, 0x41,0x5C)
#define pop____r13()                                 JIT_APPEND(2, 0x41,0x5D)
#define pop____r14()                                 JIT_APPEND(2, 0x41,0x5E)
#define pop____r15()                                 JIT_APPEND(2, 0x41,0x5F)
#define push___r12()                                 JIT_APPEND(2, 0x41,0x54)
#define push___r13()                                 JIT_APPEND(2, 0x41,0x55)
#define push___r14()                                 JIT_APPEND(2, 0x41,0x56)
#define push___r15()                                 JIT_APPEND(2, 0x41,0x57)
#define sar____rdx__imm(value)                       JIT_APPEND(3, 0x48,0xC1,0xFA); JIT_APPEND_BYTE(value)
#define shl____rax__imm(value)                       JIT_APPEND(3, 0x48,0xC1,0xE0); JIT_APPEND_BYTE(value)
#define shl____rcx__imm(value)                       JIT_APPEND(3, 0x48,0xC1,0xE1); JIT_APPEND_BYTE(value)
#define shl____rbx__imm(value)                       JIT_APPEND(3, 0x48,0xC1,0xE3); JIT_APPEND_BYTE(value)
#define shr____rax__imm(value)                       JIT_APPEND(3, 0x48,0xC1,0xE8); JIT_APPEND_BYTE(value)
#define shr____rdx__imm(value)                       JIT_APPEND(3, 0x48,0xC1,0xEA); JIT_APPEND_BYTE(value)
#define sub____rsp__imm8(value)                      JIT_APPEND(3, 0x48,0x83,0xEC); JIT_APPEND_BYTE(value)
#define sub____rsi__QWORD_PTR_rbx_imm8(value)        JIT_APPEND(3, 0x48,0x2B,0x73); JIT_APPEND_BYTE(value)
#define sub____rdi__QWORD_PTR_rbx_imm8(value)        JIT_APPEND(3, 0x48,0x2B,0x7B); JIT_APPEND_BYTE(value)
#define subsd__xmm0__xmm1()                          JIT_APPEND(4, 0xF2,0x0F,0x5C,0xC1)
#define subss__xmm0__xmm1()                          JIT_APPEND(4, 0xF3,0x0F,0x5C,0xC1)
#define xor____edx__edx()                            JIT_APPEND(2, 0x31,0xD2)
#undef inc____eax
#undef inc____edx
#undef dec____eax
#undef dec____edx
#define inc____eax()                                 JIT_APPEND(2, 0xFF,0xC0)
#define inc____edx()                                 JIT_APPEND(2, 0xFF,0xC2)
#define dec____eax()                                 JIT_APPEND(2, 0xFF,0xC8)
#define dec____edx()                                 JIT_APPEND(2, 0xFF,0xCA)
#endif /* JIT_X86_64 */

static inline int emit_func_call(Heap *heap, void *func, int stack_restore)
{
   intptr_t value = (intptr_t)func;
#if defined(JIT_X86)
   #ifdef JIT_X86_64
      if (value >= 0 && (uintptr_t)value <= 0xFFFFFFFFU) {
         mov____eax__imm(value);
      }
      else if (value >= INT_MIN && value <= INT_MAX) {
         mov____rax__imm32(value);
      }
      else {
         mov____rax__imm64(value);
      }
   #else
      mov____eax__imm(value);
   #endif
   #ifdef JIT_WIN64
      sub____rsp__imm8(0x20);
      stack_restore += 0x20;
   #endif
   call___reax();
   if (stack_restore > 0) {
      #ifdef JIT_X86_64
         add____rsp__imm8(stack_restore);
      #else
         add____esp__imm8(stack_restore);
      #endif
   }
#else
   return 0;
#endif
   return 1;
}

static inline int emit_overflow_check(Heap *heap, int pc)
{
   mov____edx__imm(JIT_PC_ERR(pc, JIT_ERROR_INTEGER_OVERFLOW));
   jo_____rel32(heap->jit_error_code - heap->jit_code_len - 4);
   return 1;
}

#ifdef JIT_X86_64
static inline int emit_reinit_volatile_regs(Heap *heap)
{
   mov____r10__QWORD_PTR_r12_imm32(OFFSETOF(Heap, jit_code));
   return 1;
}

static inline int emit_reinit_regs(Heap *heap, int ret)
{
   if (!emit_reinit_volatile_regs(heap)) return 0;
   mov____r13__QWORD_PTR_r12_imm32(OFFSETOF(Heap, jit_array_get_funcs));
   mov____r14__QWORD_PTR_r12_imm32(OFFSETOF(Heap, jit_array_set_funcs));
   mov____r15__QWORD_PTR_r12_imm32(OFFSETOF(Heap, jit_array_append_funcs));
   if (ret) {
      ret____();
   }
   return 1;
}
#endif /* JIT_X86_64 */

static inline int emit_call_reinit_regs(Heap *heap)
{
   #ifdef JIT_X86_64
      call___rel32(heap->jit_reinit_regs_func - heap->jit_code_len - 4);
   #endif
   return 1;
}
#endif /* JIT_X86 */


static inline int jit_append_entry_function(Heap *heap)
{
   int error_label;

#if defined(JIT_X86_64)
   push___rebp();
   mov____rbp__rsp();
   push___rebx();
   #ifdef JIT_WIN64
      push___resi();
      push___redi();
   #endif
   push___r12();
   push___r13();
   push___r14();
   push___r15();
   #ifdef JIT_WIN64
      mov____r12__rcx();
   #else
      mov____r12__rdi();
   #endif
   if (!emit_reinit_regs(heap, 0)) return 0;

   mov____rax__QWORD_PTR_r12_imm8(OFFSETOF(Heap, jit_error_sp));
   push___reax();
   mov____eax__DWORD_PTR_r12_imm8(OFFSETOF(Heap, jit_error_pc));
   push___reax();
   mov____eax__DWORD_PTR_r12_imm8(OFFSETOF(Heap, jit_error_stack));
   push___reax();
   mov____eax__DWORD_PTR_r12_imm8(OFFSETOF(Heap, jit_error_base));
   push___reax();

   mov____QWORD_PTR_r12_imm8__rsp(OFFSETOF(Heap, jit_error_sp));
   mov____DWORD_PTR_r12_imm8__imm(OFFSETOF(Heap, jit_error_pc), 0);
   error_label = heap->jit_code_len-4;
   #ifdef JIT_WIN64
      mov____DWORD_PTR_r12_imm8__r8d(OFFSETOF(Heap, jit_error_stack));
      mov____DWORD_PTR_r12_imm8__r8d(OFFSETOF(Heap, jit_error_base));
   #else
      mov____DWORD_PTR_r12_imm8__edx(OFFSETOF(Heap, jit_error_stack));
      mov____DWORD_PTR_r12_imm8__edx(OFFSETOF(Heap, jit_error_base));
   #endif
   sub____rsp__imm8(8); // align
   #ifdef JIT_WIN64
      call___rdx();
   #else
      call___rsi();
   #endif
   add____rsp__imm8(8);
   pop____reax();
   mov____DWORD_PTR_r12_imm8__eax(OFFSETOF(Heap, jit_error_base));
   pop____reax();
   mov____DWORD_PTR_r12_imm8__eax(OFFSETOF(Heap, jit_error_stack));
   pop____reax();
   mov____DWORD_PTR_r12_imm8__eax(OFFSETOF(Heap, jit_error_pc));
   pop____reax();
   mov____QWORD_PTR_r12_imm8__rax(OFFSETOF(Heap, jit_error_sp));
   pop____r15();
   pop____r14();
   pop____r13();
   pop____r12();
   #ifdef JIT_WIN64
      pop____redi();
      pop____resi();
   #endif
   pop____rebx();
   mov____rsp__rbp();
   pop____rebp();
   ret____();

   memcpy(heap->jit_code + error_label, &heap->jit_code_len, 4);

   #ifdef JIT_WIN64
      lea____rbp__rsp_imm8(0x58);
   #else
      lea____rbp__rsp_imm8(0x48);
   #endif
   pop____reax();
   mov____DWORD_PTR_r12_imm8__eax(OFFSETOF(Heap, jit_error_base));
   pop____reax();
   mov____DWORD_PTR_r12_imm8__eax(OFFSETOF(Heap, jit_error_stack));
   pop____reax();
   mov____DWORD_PTR_r12_imm8__eax(OFFSETOF(Heap, jit_error_pc));
   pop____reax();
   mov____QWORD_PTR_r12_imm8__rax(OFFSETOF(Heap, jit_error_sp));
   pop____r15();
   pop____r14();
   pop____r13();
   pop____r12();
   #ifdef JIT_WIN64
      pop____redi();
      pop____resi();
   #endif
   pop____rebx();
   mov____rsp__rbp();
   pop____rebp();
   ret____();

#elif defined(JIT_X86)
   push___rebp();
   mov____ebp__esp();
   push___rebx();
   push___resi();
   push___redi();
   mov____edx__DWORD_PTR_ebp_imm8(0x08);
   push___DWORD_PTR_edx_imm8(OFFSETOF(Heap, jit_error_sp));
   push___DWORD_PTR_edx_imm8(OFFSETOF(Heap, jit_error_pc));
   push___DWORD_PTR_edx_imm8(OFFSETOF(Heap, jit_error_stack));
   push___DWORD_PTR_edx_imm8(OFFSETOF(Heap, jit_error_base));
   mov____DWORD_PTR_edx_imm8__esp(OFFSETOF(Heap, jit_error_sp));
   mov____DWORD_PTR_edx_imm8__imm(OFFSETOF(Heap, jit_error_pc), 0);
   error_label = heap->jit_code_len-4;
   mov____eax__DWORD_PTR_ebp_imm8(0x10);
   mov____DWORD_PTR_redx_imm8__eax(OFFSETOF(Heap, jit_error_stack));
   mov____DWORD_PTR_redx_imm8__eax(OFFSETOF(Heap, jit_error_base));
   push___redx();
   call___DWORD_PTR_ebp_imm8(0x0C);
   add____esp__imm8(0x04);
   mov____edx__DWORD_PTR_ebp_imm8(0x08);
   pop____DWORD_PTR_edx_imm8(OFFSETOF(Heap, jit_error_base));
   pop____DWORD_PTR_edx_imm8(OFFSETOF(Heap, jit_error_stack));
   pop____DWORD_PTR_edx_imm8(OFFSETOF(Heap, jit_error_pc));
   pop____DWORD_PTR_edx_imm8(OFFSETOF(Heap, jit_error_sp));
   pop____redi();
   pop____resi();
   pop____rebx();
   mov____esp__ebp();
   pop____rebp();
   ret____();

   memcpy(heap->jit_code + error_label, &heap->jit_code_len, 4);

   lea____ebp__esp_imm8(0x1C);
   mov____edx__DWORD_PTR_ebp_imm8(0x08);
   pop____DWORD_PTR_edx_imm8(OFFSETOF(Heap, jit_error_base));
   pop____DWORD_PTR_edx_imm8(OFFSETOF(Heap, jit_error_stack));
   pop____DWORD_PTR_edx_imm8(OFFSETOF(Heap, jit_error_pc));
   pop____DWORD_PTR_edx_imm8(OFFSETOF(Heap, jit_error_sp));
   pop____redi();
   pop____resi();
   pop____rebx();
   mov____esp__ebp();
   pop____rebp();
   ret____();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_unwind(Heap *heap)
{
#if defined(JIT_X86)
   #ifdef JIT_X86_64
      mov____esi__DWORD_PTR_r12_imm8(OFFSETOF(Heap, jit_error_stack));
      lea____edi__resi_4();
      mov____rsp__QWORD_PTR_r12_imm8(OFFSETOF(Heap, jit_error_sp));
      mov____eax__DWORD_PTR_r12_imm8(OFFSETOF(Heap, jit_error_pc));
      add____rax__QWORD_PTR_r12_imm32(OFFSETOF(Heap, jit_code));
      jmp____reax();
   #else
      mov____edx__DWORD_PTR_ebp_imm8(0x08);
      mov____esi__DWORD_PTR_edx_imm8(OFFSETOF(Heap, jit_error_stack));
      lea____edi__resi_4();
      mov____esp__DWORD_PTR_edx_imm8(OFFSETOF(Heap, jit_error_sp));
      mov____eax__DWORD_PTR_redx_imm8(OFFSETOF(Heap, jit_error_pc));
      add____eax__DWORD_PTR_edx_imm32(OFFSETOF(Heap, jit_code));
      jmp____reax();
   #endif
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_error_code(Heap *heap)
{
#if defined(JIT_X86_64)
   #ifdef JIT_WIN64
      mov____rcx__r12();
   #else
      mov____rdi__r12();
      mov____esi__edx();
   #endif
   if (!emit_func_call(heap, jit_emit_error, 0)) return 0;
   if (!emit_call_reinit_regs(heap)) return 0;
   if (!jit_append_unwind(heap)) return 0;
#elif defined(JIT_X86)
   push___redx();
   push___DWORD_PTR_ebp_imm8(0x08);
   if (!emit_func_call(heap, jit_emit_error, 0)) return 0;
   if (!jit_append_unwind(heap)) return 0;
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_start(Heap *heap)
{
#if defined(JIT_X86)
   push___rebp();
   #ifdef JIT_X86_64
      mov____rbp__rsp();
      push___resi();
      push___redi();
   #else
      mov____ebp__esp();
      push___rebx();
      push___resi();
   #endif
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_end(Heap *heap)
{
#if defined(JIT_X86)
   #ifdef JIT_X86_64
      pop____redi();
      pop____resi();
      mov____rsp__rbp();
   #else
      pop____resi();
      lea____edi__resi_4();
      pop____rebx();
      mov____esp__ebp();
   #endif
   pop____rebp();
   ret____();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_const(Heap *heap, int value, int flag)
{
#ifdef JIT_DEBUG
   printf("   const %d,%d\n", value, flag);
#endif
#if defined(JIT_X86)
   if (value == 0) {
      xor____eax__eax();
   }
   else {
      mov____eax__imm(value);
   }
   if (flag) {
      mov____ebx__imm(1);
   }
   else {
      xor____ebx__ebx();
   }
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_get(Heap *heap, int slot)
{
#ifdef JIT_DEBUG
   printf("   get %d\n", slot);
#endif
#if defined(JIT_X86)
   if (SH(slot*4 >= -128 && slot*4 < 128)) {
      mov____eax__DWORD_PTR_redi_imm8(slot*4);
      movzx__ebx__BYTE_PTR_resi_imm8(slot);
   }
   else {
      mov____eax__DWORD_PTR_redi_imm32(slot*4);
      movzx__ebx__BYTE_PTR_resi_imm32(slot);
   }
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_set(Heap *heap, int slot)
{
#ifdef JIT_DEBUG
   printf("   set %d\n", slot);
#endif
#if defined(JIT_X86)
   if (SH(slot*4 >= -128 && slot*4 < 128)) {
      mov____DWORD_PTR_redi_imm8__eax(slot*4);
      mov____BYTE_PTR_resi_imm8__bl(slot);
   }
   else {
      mov____DWORD_PTR_redi_imm32__eax(slot*4);
      mov____BYTE_PTR_resi_imm32__bl(slot);
   }
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_get_tmp(Heap *heap, int slot)
{
#ifdef JIT_DEBUG
   printf("   get_tmp %d\n", slot);
#endif
#if defined(JIT_X86)
   if (SH(slot*4 >= -128 && slot*4 < 128)) {
      mov____edx__DWORD_PTR_redi_imm8(slot*4);
      movzx__ecx__BYTE_PTR_resi_imm8(slot);
   }
   else {
      mov____edx__DWORD_PTR_redi_imm32(slot*4);
      movzx__ecx__BYTE_PTR_resi_imm32(slot);
   }
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_set_tmp(Heap *heap, int slot)
{
#ifdef JIT_DEBUG
   printf("   set_tmp %d\n", slot);
#endif
#if defined(JIT_X86)
   if (SH(slot*4 >= -128 && slot*4 < 128)) {
      mov____DWORD_PTR_redi_imm8__edx(slot*4);
      mov____BYTE_PTR_resi_imm8__cl(slot);
   }
   else {
      mov____DWORD_PTR_redi_imm32__edx(slot*4);
      mov____BYTE_PTR_resi_imm32__cl(slot);
   }
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_branch(Heap *heap, int *label_addr)
{
#ifdef JIT_DEBUG
   printf("   branch\n");
#endif
#if defined(JIT_X86)
   cmp____eax__imm8(0);
   je_____rel32(0);
   *label_addr = heap->jit_code_len;
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_loop(Heap *heap, int addr, int target_addr)
{
#ifdef JIT_DEBUG
   printf("   loop\n");
#endif
#if defined(JIT_X86)
   if (SH(target_addr - addr - 2 >= -128)) {
      jmp____rel8(target_addr - addr - 2);
   }
   else {
      jmp____rel32(target_addr - addr - 5);
   }
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_jump(Heap *heap, int addr, int target_addr, int *label_addr)
{
#ifdef JIT_DEBUG
   printf("   jump\n");
#endif
#if defined(JIT_X86)
   if (!label_addr && SH(target_addr - addr - 2 >= -128)) {
      jmp____rel8(target_addr - addr - 2);
   }
   else {
      jmp____rel32(target_addr - addr - 5);
   }
   if (label_addr) {
      *label_addr = heap->jit_code_len;
   }
#else
   return 0;
#endif
   return 1;
}


static inline void jit_update_branch(Heap *heap, int label_addr, int target_addr)
{
#if defined(JIT_X86)
   int value = target_addr - label_addr;
   memcpy(heap->jit_code + (label_addr - 4), &value, 4);
#endif
}


static inline int jit_append_add(Heap *heap, int slot, int overflow_check, int pc)
{
#ifdef JIT_DEBUG
   printf("   add%s %d\n", overflow_check? "":".mod", slot);
#endif
#if defined(JIT_X86)
   if (SH(slot*4 >= -128 && slot*4 < 128)) {
      add____eax__DWORD_PTR_redi_imm8(slot*4);
   }
   else {
      add____eax__DWORD_PTR_redi_imm32(slot*4);
   }
   if (overflow_check) {
      if (!emit_overflow_check(heap, pc)) return 0;
   }
   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_sub(Heap *heap, int slot, int overflow_check, int pc)
{
#ifdef JIT_DEBUG
   printf("   sub%s %d\n", overflow_check? "":".mod", slot);
#endif
#if defined(JIT_X86)
   if (SH(slot*4 >= -128 && slot*4 < 128)) {
      sub____eax__DWORD_PTR_redi_imm8(slot*4);
   }
   else {
      sub____eax__DWORD_PTR_redi_imm32(slot*4);
   }
   if (overflow_check) {
      if (!emit_overflow_check(heap, pc)) return 0;
   }
   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_rsub(Heap *heap, int slot, int overflow_check, int pc)
{
#ifdef JIT_DEBUG
   printf("   rsub%s %d\n", overflow_check? "":".mod", slot);
#endif
#if defined(JIT_X86)
   if (SH(slot*4 >= -128 && slot*4 < 128)) {
      mov____ecx__DWORD_PTR_redi_imm8(slot*4);
   }
   else {
      mov____ecx__DWORD_PTR_redi_imm32(slot*4);
   }
   sub____ecx__eax();
   if (overflow_check) {
      if (!emit_overflow_check(heap, pc)) return 0;
   }
   mov____eax__ecx();
   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_mul(Heap *heap, int slot, int overflow_check, int pc)
{
#ifdef JIT_DEBUG
   printf("   mul%s %d\n", overflow_check? "":".mod", slot);
#endif
#if defined(JIT_X86)
   if (SH(slot*4 >= -128 && slot*4 < 128)) {
      imul___eax__DWORD_PTR_redi_imm8(slot*4);
   }
   else {
      imul___eax__DWORD_PTR_redi_imm32(slot*4);
   }
   if (overflow_check) {
      if (!emit_overflow_check(heap, pc)) return 0;
   }
   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_divrem(Heap *heap, int slot, int remainder, int reverse, int pc)
{
   int ref1;
#ifdef JIT_DEBUG
   printf("   %s%s %d\n", reverse? "r":"", remainder? "rem":"div", slot);
#endif
#if defined(JIT_X86)
   if (SH(slot*4 >= -128 && slot*4 < 128)) {
      mov____ecx__DWORD_PTR_redi_imm8(slot*4);
   }
   else {
      mov____ecx__DWORD_PTR_redi_imm32(slot*4);
   }
   if (reverse) {
      xchg___ecx__eax();
   }
   mov____edx__imm(JIT_PC_ERR(pc, JIT_ERROR_DIVISION_BY_ZERO));
   cmp____ecx__imm8(0);
   je_____rel32(heap->jit_error_code - heap->jit_code_len - 4);
   cmp____ecx__imm8(-1);
   jne____rel8(0); // label 1
   ref1 = heap->jit_code_len;
   
   mov____edx__imm(JIT_PC_ERR(pc, JIT_ERROR_INTEGER_OVERFLOW));
   cmp____eax__imm32(0x80000000);
   je_____rel32(heap->jit_error_code - heap->jit_code_len - 4);

   // label 1:
   heap->jit_code[ref1-1] = heap->jit_code_len - ref1;
   mov____edx__eax();
   sar____edx__imm(31);
   idiv___ecx();
   if (remainder) {
      mov____eax__edx();
   }
   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_shift(Heap *heap, int slot, int left, int logical, int reverse)
{
#ifdef JIT_DEBUG
   printf("   %s%s %d\n", reverse? "r":"", left? "shl": logical? "ushr" : "shr", slot);
#endif
#if defined(JIT_X86)
   if (SH(slot*4 >= -128 && slot*4 < 128)) {
      mov____ecx__DWORD_PTR_redi_imm8(slot*4);
   }
   else {
      mov____ecx__DWORD_PTR_redi_imm32(slot*4);
   }
   if (reverse) {
      xchg___ecx__eax();
   }
   if (left) {
      shl____eax__cl();
   }
   else {
      if (logical) {
         shr____eax__cl();
      }
      else {
         sar____eax__cl();
      }
   }
   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_and(Heap *heap, int slot)
{
#ifdef JIT_DEBUG
   printf("   and %d\n", slot);
#endif
#if defined(JIT_X86)
   if (SH(slot*4 >= -128 && slot*4 < 128)) {
      and____eax__DWORD_PTR_redi_imm8(slot*4);
   }
   else {
      and____eax__DWORD_PTR_redi_imm32(slot*4);
   }
   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_or(Heap *heap, int slot)
{
#ifdef JIT_DEBUG
   printf("   or %d\n", slot);
#endif
#if defined(JIT_X86)
   if (SH(slot*4 >= -128 && slot*4 < 128)) {
      or_____eax__DWORD_PTR_redi_imm8(slot*4);
   }
   else {
      or_____eax__DWORD_PTR_redi_imm32(slot*4);
   }
   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_xor(Heap *heap, int slot)
{
#ifdef JIT_DEBUG
   printf("   xor %d\n", slot);
#endif
#if defined(JIT_X86)
   if (SH(slot*4 >= -128 && slot*4 < 128)) {
      xor____eax__DWORD_PTR_redi_imm8(slot*4);
   }
   else {
      xor____eax__DWORD_PTR_redi_imm32(slot*4);
   }
   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_cmp(Heap *heap, int slot, int type)
{
#ifdef JIT_DEBUG
   printf("   cmp%s %d\n", type == BC_LT? ".lt" : type == BC_LE? ".le" : type == BC_GT? ".gt" : type == BC_GE? ".ge" : type == BC_EQ? ".eq" : ".ne", slot);
#endif
#if defined(JIT_X86)
   if (SH(slot*4 >= -128 && slot*4 < 128)) {
      cmp____eax__DWORD_PTR_redi_imm8(slot*4);
   }
   else {
      cmp____eax__DWORD_PTR_redi_imm32(slot*4);
   }
   switch (type) {
      case BC_LT: case BC_GE: setl___dl(); break;
      case BC_LE: case BC_GT: setle__dl(); break;
      case BC_EQ:             sete___dl(); break;
      case BC_NE:             setne__dl(); break;
   }
   movzx__eax__dl();
   if (type == BC_GT || type == BC_GE) {
      xor____eax__imm8(1);
   }
   if (type == BC_EQ || type == BC_NE) {
      if (SH(slot >= -128 && slot < 128)) {
         cmp____BYTE_PTR_resi_imm8__bl(slot);
      }
      else {
         cmp____BYTE_PTR_resi_imm32__bl(slot);
      }
      if (type == BC_EQ) {
         sete___dl();
         and____al__dl();
      }
      else {
         setne__dl();
         or_____al__dl();
      }
   }
   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_cmp_value(Heap *heap, int slot, int equal)
{
   int ref1, ref2, ref3a, ref3b;
#ifdef JIT_DEBUG
   printf("   cmp_value%s %d\n", equal? ".eq" : ".ne", slot);
#endif
#if defined(JIT_X86)
   if (SH(slot*4 >= -128 && slot*4 < 128)) {
      mov____edx__DWORD_PTR_redi_imm8(slot*4);
      movzx__ecx__BYTE_PTR_resi_imm8(slot);
   }
   else {
      mov____edx__DWORD_PTR_redi_imm32(slot*4);
      movzx__ecx__BYTE_PTR_resi_imm32(slot);
   }
   cmp____ebx__ecx();
   je_____rel8(0); // label 1
   ref1 = heap->jit_code_len;

   xor____eax__eax();
   jmp____rel8(0); // label 3
   ref3a = heap->jit_code_len;

   // label 1:
   heap->jit_code[ref1-1] = heap->jit_code_len - ref1;
   cmp____eax__edx();
   je_____rel8(0); // label 2
   ref2 = heap->jit_code_len;

   #if MAX_COMPARE_RECURSION >= 128
      #error "MAX_COMPARE_RECURSION is too big"
   #endif

   #ifdef JIT_X86_64
      #ifdef JIT_WIN64
         sub____rsp__imm8(0x08); // align
         push___imm8(MAX_COMPARE_RECURSION);
         shl____rcx__imm(32);
         shl____rbx__imm(32);
         mov____r9d__edx();
         or_____r9__rcx();
         mov____r8__r12();
         mov____edx__eax();
         or_____rdx__rbx();
         mov____rcx__r12();
         if (!emit_func_call(heap, compare_values, 0x10)) return 0;
      #else
         push___resi();
         push___redi();
         shl____rcx__imm(32);
         shl____rbx__imm(32);
         mov____r8d__imm(MAX_COMPARE_RECURSION);
         or_____rcx__rdx();
         mov____rdx__r12();
         mov____esi__eax();
         or_____rsi__rbx();
         mov____rdi__r12();
         if (!emit_func_call(heap, compare_values, 0)) return 0;
         pop____redi();
         pop____resi();
      #endif
      if (!emit_reinit_volatile_regs(heap)) return 0;
   #else
      push___imm8(MAX_COMPARE_RECURSION);
      push___recx();
      push___redx();
      mov____edx__DWORD_PTR_ebp_imm8(0x08);
      push___redx();
      push___rebx();
      push___reax();
      push___redx();
      if (!emit_func_call(heap, compare_values, 0x1C)) return 0;
   #endif

   jmp____rel8(0); // label 3
   ref3b = heap->jit_code_len;

   // label 2:
   heap->jit_code[ref2-1] = heap->jit_code_len - ref2;
   mov____eax__imm(1);

   // label 3:
   heap->jit_code[ref3a-1] = heap->jit_code_len - ref3a;
   heap->jit_code[ref3b-1] = heap->jit_code_len - ref3b;
   xor____ebx__ebx();
   if (!equal) {
      xor____eax__imm8(1);
   }
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_not(Heap *heap, int logical)
{
#ifdef JIT_DEBUG
   printf("   %snot\n", logical? "log" : "bit");
#endif
#if defined(JIT_X86)
   if (logical) {
      cmp____eax__imm8(0);
      sete___dl();
      movzx__eax__dl();
   }
   else {
      xor____eax__imm8(-1);
   }
   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_incdec(Heap *heap, int inc, int pc)
{
#ifdef JIT_DEBUG
   printf("   %s\n", inc? "inc" : "dec");
#endif
#if defined(JIT_X86)
   if (inc) {
      inc____eax();
   }
   else {
      dec____eax();
   }
   if (!emit_overflow_check(heap, pc)) return 0;
   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_incdec_tmp(Heap *heap, int inc, int pc)
{
#ifdef JIT_DEBUG
   printf("   %s.tmp\n", inc? "inc" : "dec");
#endif
#if defined(JIT_X86)
   if (inc) {
      inc____edx();
   }
   else {
      dec____edx();
   }
   push___redx();
   if (!emit_overflow_check(heap, pc)) return 0;
   pop____redx();
   xor____ecx__ecx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_incdec_stack(Heap *heap, int slot, int inc, int pc)
{
#ifdef JIT_DEBUG
   printf("   %s.stack %d\n", inc? "inc" : "dec", slot);
#endif
#if defined(JIT_X86)
   if (inc) {
      if (SH(slot*4 >= -128 && slot*4 < 128)) {
         inc____DWORD_PTR_redi_imm8(slot*4);
      }
      else {
         inc____DWORD_PTR_redi_imm32(slot*4);
      }
   }
   else {
      if (SH(slot*4 >= -128 && slot*4 < 128)) {
         dec____DWORD_PTR_redi_imm8(slot*4);
      }
      else {
         dec____DWORD_PTR_redi_imm32(slot*4);
      }
   }
   if (!emit_overflow_check(heap, pc)) return 0;
   if (SH(slot >= -128 && slot < 128)) {
      mov____BYTE_PTR_resi_imm8__imm(slot, 0);
   }
   else {
      mov____BYTE_PTR_resi_imm32__imm(slot, 0);
   }
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_float_op(Heap *heap, int dest, int src1, int src2_tmp, int type)
{
   int ref1;
#ifdef JIT_DEBUG
   printf("   f%s %d/acc <- %d, %d/acc\n", type == BC_FLOAT_ADD? "add" : type == BC_FLOAT_SUB? "sub" : type == BC_FLOAT_MUL? "mul" : "div", dest, src1, src2_tmp);
#endif
#if defined(JIT_X86)
   #ifdef JIT_X86_64
      if (type == BC_FLOAT_ADD || type == BC_FLOAT_MUL) {
         movd___xmm0__eax();
         if (type == BC_FLOAT_ADD) {
            if (SH(src1*4 >= -128 && src1*4 < 128)) {
               addss__xmm0__DWORD_PTR_rdi_imm8(src1*4);
            }
            else {
               addss__xmm0__DWORD_PTR_rdi_imm32(src1*4);
            }
         }
         else {
            if (SH(src1*4 >= -128 && src1*4 < 128)) {
               mulss__xmm0__DWORD_PTR_rdi_imm8(src1*4);
            }
            else {
               mulss__xmm0__DWORD_PTR_rdi_imm32(src1*4);
            }
         }
      }
      else {
         if (SH(src1*4 >= -128 && src1*4 < 128)) {
            movd___xmm0__DWORD_PTR_rdi_imm8(src1*4);
         }
         else {
            movd___xmm0__DWORD_PTR_rdi_imm32(src1*4);
         }
         movd___xmm1__eax();
         if (type == BC_FLOAT_SUB) {
            subss__xmm0__xmm1();
         }
         else {
            divss__xmm0__xmm1();
         }
      }
      movd___eax__xmm0();
   #else
      if (SH(src2_tmp*4 >= -128 && src2_tmp*4 < 128)) {
         mov____DWORD_PTR_redi_imm8__eax(src2_tmp*4);
      }
      else {
         mov____DWORD_PTR_redi_imm32__eax(src2_tmp*4);
      }
      if (SH(src1*4 >= -128 && src1*4 < 128)) {
         fld____DWORD_PTR_edi_imm8(src1*4);
      }
      else {
         fld____DWORD_PTR_edi_imm32(src1*4);
      }
      if (SH(src2_tmp*4 >= -128 && src2_tmp*4 < 128)) {
         switch (type) {
            case BC_FLOAT_ADD: fadd___DWORD_PTR_edi_imm8(src2_tmp*4); break;
            case BC_FLOAT_SUB: fsub___DWORD_PTR_edi_imm8(src2_tmp*4); break;
            case BC_FLOAT_MUL: fmul___DWORD_PTR_edi_imm8(src2_tmp*4); break;
            case BC_FLOAT_DIV: fdiv___DWORD_PTR_edi_imm8(src2_tmp*4); break;
         }
      }
      else {
         switch (type) {
            case BC_FLOAT_ADD: fadd___DWORD_PTR_edi_imm32(src2_tmp*4); break;
            case BC_FLOAT_SUB: fsub___DWORD_PTR_edi_imm32(src2_tmp*4); break;
            case BC_FLOAT_MUL: fmul___DWORD_PTR_edi_imm32(src2_tmp*4); break;
            case BC_FLOAT_DIV: fdiv___DWORD_PTR_edi_imm32(src2_tmp*4); break;
         }
      }
      if (SH(dest*4 >= -128 && dest*4 < 128)) {
         fstp___DWORD_PTR_edi_imm8(dest*4);
         mov____eax__DWORD_PTR_redi_imm8(dest*4);
      }
      else {
         fstp___DWORD_PTR_edi_imm32(dest*4);
         mov____eax__DWORD_PTR_redi_imm32(dest*4);
      }
   #endif
   test___eax__imm(0x7F800000);
   jnz____rel8(0); // label 1
   ref1 = heap->jit_code_len;

   and____eax__imm32(0xFF800000);

   // label 1:
   heap->jit_code[ref1-1] = heap->jit_code_len - ref1;
   mov____ebx__imm(1);
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_float_cmp(Heap *heap, int src1, int src2_tmp, int type)
{
#ifdef JIT_DEBUG
   printf("   fcmp%s acc <- %d, %d/acc\n", type == BC_FLOAT_LT? ".lt" : type == BC_FLOAT_LE? ".le" : type == BC_FLOAT_GT? ".gt" : type == BC_FLOAT_GE? ".ge" : type == BC_FLOAT_EQ? ".eq" : ".ne", src1, src2_tmp);
#endif
#if defined(JIT_X86)
   #ifdef JIT_X86_64
      int imm = 0;
      if (type == BC_FLOAT_EQ || type == BC_FLOAT_NE || type == BC_FLOAT_GT || type == BC_FLOAT_GE) {
         movd___xmm0__eax();
         switch (type) {
            case BC_FLOAT_EQ: imm = 0; break;
            case BC_FLOAT_NE: imm = 4; break;
            case BC_FLOAT_GT: imm = 1; break;
            case BC_FLOAT_GE: imm = 2; break;
         }
         if (SH(src1*4 >= -128 && src1*4 < 128)) {
            cmpss__xmm0__DWORD_PTR_rdi_imm8(src1*4, imm);
         }
         else {
            cmpss__xmm0__DWORD_PTR_rdi_imm32(src1*4, imm);
         }
      }
      else {
         if (SH(src1*4 >= -128 && src1*4 < 128)) {
            movd___xmm0__DWORD_PTR_rdi_imm8(src1*4);
         }
         else {
            movd___xmm0__DWORD_PTR_rdi_imm32(src1*4);
         }
         movd___xmm1__eax();
         switch (type) {
            case BC_FLOAT_LT: imm = 1; break;
            case BC_FLOAT_LE: imm = 2; break;
         }
         cmpss__xmm0__xmm1(imm);
      }
      movd___eax__xmm0();
      and____eax__imm8(1);
   #else
      if (SH(src2_tmp*4 >= -128 && src2_tmp*4 < 128)) {
         mov____DWORD_PTR_redi_imm8__eax(src2_tmp*4);
      }
      else {
         mov____DWORD_PTR_redi_imm32__eax(src2_tmp*4);
      }
      if (SH(src2_tmp*4 >= -128 && src2_tmp*4 < 128)) {
         fld____DWORD_PTR_edi_imm8(src2_tmp*4);
      }
      else {
         fld____DWORD_PTR_edi_imm32(src2_tmp*4);
      }
      if (SH(src1*4 >= -128 && src1*4 < 128)) {
         fld____DWORD_PTR_edi_imm8(src1*4);
      }
      else {
         fld____DWORD_PTR_edi_imm32(src1*4);
      }
      fucompp();
      fnstsw_ax();
      sahf___();
      switch (type) {
         case BC_FLOAT_LT: setb___dl(); setne__cl(); and____dl__cl(); break;
         case BC_FLOAT_LE: setnp__dl(); seta___cl(); xor____cl__imm(1); and____dl__cl(); break;
         case BC_FLOAT_GT: seta___dl(); break;
         case BC_FLOAT_GE: setnp__dl(); setae__cl(); and____dl__cl(); break;
         case BC_FLOAT_EQ: sete___dl(); setae__cl(); and____dl__cl(); break;
         case BC_FLOAT_NE: sete___dl(); setae__cl(); and____dl__cl(); xor____dl__imm(1); break;
      }
      movzx__eax__dl();
   #endif
   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_return(Heap *heap, int stack_pos)
{
#ifdef JIT_DEBUG
   printf("   return (%d)\n", stack_pos);
#endif
#if defined(JIT_X86)
   #ifdef JIT_X86_64
      mov____rbx__r12();
      sub____rsi__QWORD_PTR_rbx_imm8(OFFSETOF(Heap, stack_flags));
   #else
      mov____ebx__DWORD_PTR_ebp_imm8(0x08);
      sub____esi__DWORD_PTR_ebx_imm8(OFFSETOF(Heap, stack_flags));
   #endif
   if (SH(stack_pos >= -128 && stack_pos < 128)) {
      lea____eax__resi_imm8(stack_pos);
   }
   else {
      lea____eax__resi_imm32(stack_pos);
   }
   mov____DWORD_PTR_rebx_imm8__eax(OFFSETOF(Heap, stack_len));
   if (!jit_append_end(heap)) return 0;
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_return2(Heap *heap, int slot1, int slot2)
{
#ifdef JIT_DEBUG
   printf("   return2 %d, %d\n", slot1, slot2);
#endif
#if defined(JIT_X86)
   #ifdef JIT_X86_64
      mov____rdx__r12();
   #else
      mov____edx__DWORD_PTR_ebp_imm8(0x08); // heap
   #endif
   mov____ebx__DWORD_PTR_redx_imm8(OFFSETOF(Heap, jit_error_base));
   #ifdef JIT_X86_64
      mov____rcx__QWORD_PTR_rdx_imm8(OFFSETOF(Heap, stack_flags));
   #else
      mov____ecx__DWORD_PTR_redx_imm8(OFFSETOF(Heap, stack_flags));
   #endif
   lea____eax__rebx_imm8(0x02);
   mov____DWORD_PTR_redx_imm8__eax(OFFSETOF(Heap, stack_len));

   #ifdef JIT_X86_64
      add____rcx__rbx();
   #else
      add____ecx__ebx();
   #endif
   shl____ebx__imm(2);
   #ifdef JIT_X86_64
      add____rbx__QWORD_PTR_rdx_imm8(OFFSETOF(Heap, stack_data));
   #else
      add____ebx__DWORD_PTR_edx_imm8(OFFSETOF(Heap, stack_data));
   #endif

#define COPY_SLOT(dest, src) \
   if (SH(src*4 >= -128 && src*4 < 128)) { \
      mov____eax__DWORD_PTR_redi_imm8(src*4); \
   } \
   else { \
      mov____eax__DWORD_PTR_redi_imm32(src*4); \
   } \
   mov____DWORD_PTR_rebx_imm8__eax(dest*4); \
   \
   if (SH(src >= -128 && src < 128)) { \
      movzx__eax__BYTE_PTR_resi_imm8(src); \
   } \
   else { \
      movzx__eax__BYTE_PTR_resi_imm32(src); \
   } \
   mov____BYTE_PTR_recx_imm8__al(dest)

   COPY_SLOT(0, slot1);
   COPY_SLOT(1, slot2);

#undef COPY_SLOT

   if (!jit_append_unwind(heap)) return 0;
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_call(Heap *heap, int stack_pos, int marker_pos, int marker_value, int dest_addr, int *label_addr, NativeFunction *nfunc, int call2)
{
   int ref1=0, ref2, ref3, ref4=0, label;
#ifdef JIT_DEBUG
   printf("   call%s_%s (%d, %d, %d)\n", call2? "2":"", nfunc? "native" : dest_addr >= 0? "direct" : "dynamic", stack_pos, marker_pos, marker_value & 0x7FFFFFFF);
#endif
#if defined(JIT_X86)
   #ifdef JIT_X86_64
      mov____rbx__r12();
   #else
      mov____ebx__DWORD_PTR_ebp_imm8(0x08);
   #endif
   if (dest_addr < 0) {
      int num_params = stack_pos - marker_pos - 1;
      mov____edx__imm(JIT_PC_ERR(marker_value & 0x7FFFFFFF, JIT_ERROR_INVALID_FUNCREF));
      if (SH(marker_pos*4 >= -128 && marker_pos*4 < 128)) {
         mov____ecx__DWORD_PTR_redi_imm8(marker_pos*4);
         cmp____BYTE_PTR_resi_imm8__imm8(marker_pos, 0);
      }
      else {
         mov____ecx__DWORD_PTR_redi_imm32(marker_pos*4);
         cmp____BYTE_PTR_resi_imm32__imm8(marker_pos, 0);
      }
      je_____rel32(heap->jit_error_code - heap->jit_code_len - 4);

      sub____ecx__imm32(FUNC_REF_OFFSET);
      cmp____ecx__imm8(1);
      jl_____rel32(heap->jit_error_code - heap->jit_code_len - 4);

      cmp____ecx__DWORD_PTR_rebx_imm32(OFFSETOF(Heap, functions.len));
      jge____rel32(heap->jit_error_code - heap->jit_code_len - 4);

      #ifdef JIT_X86_64
         mov____rax__QWORD_PTR_rbx_imm32(OFFSETOF(Heap, functions.data));
         mov____rcx__QWORD_PTR_rax_rcx_8();
      #else
         mov____eax__DWORD_PTR_ebx_imm32(OFFSETOF(Heap, functions.data));
         mov____ecx__DWORD_PTR_eax_ecx_4();
      #endif

      mov____edx__imm(JIT_PC_ERR(marker_value & 0x7FFFFFFF, JIT_ERROR_IMPROPER_PARAMS));
      if (SH(num_params < 128)) {
         cmp____DWORD_PTR_recx_imm8__imm8(OFFSETOF(Function, num_params), num_params);
      }
      else {
         cmp____DWORD_PTR_recx_imm8__imm32(OFFSETOF(Function, num_params), num_params);
      }
      jne____rel32(heap->jit_error_code - heap->jit_code_len - 4);
   }
   if (SH(marker_pos*4 >= -128 && marker_pos*4 < 128)) {
      mov____DWORD_PTR_redi_imm8__imm(marker_pos*4, marker_value);
      mov____BYTE_PTR_resi_imm8__imm(marker_pos, 1);
   }
   else {
      mov____DWORD_PTR_redi_imm32__imm(marker_pos*4, marker_value);
      mov____BYTE_PTR_resi_imm32__imm(marker_pos, 1);
   }
   #ifdef JIT_X86_64
      sub____rdi__QWORD_PTR_rbx_imm8(OFFSETOF(Heap, stack_data));
      sub____rsi__QWORD_PTR_rbx_imm8(OFFSETOF(Heap, stack_flags));
   #else
      sub____edi__DWORD_PTR_ebx_imm8(OFFSETOF(Heap, stack_data));
      sub____esi__DWORD_PTR_ebx_imm8(OFFSETOF(Heap, stack_flags));
   #endif
   if (SH(stack_pos < 128)) {
      lea____eax__resi_imm8(stack_pos);
   }
   else {
      lea____eax__resi_imm32(stack_pos);
   }
   mov____DWORD_PTR_rebx_imm8__eax(OFFSETOF(Heap, stack_len));

   if (call2) {
      #ifdef JIT_X86_64
         mov____rdx__r12();
         mov____rax__QWORD_PTR_r12_imm8(OFFSETOF(Heap, jit_error_sp));
         push___reax();
         mov____eax__DWORD_PTR_r12_imm8(OFFSETOF(Heap, jit_error_pc));
         push___reax();
         mov____eax__DWORD_PTR_r12_imm8(OFFSETOF(Heap, jit_error_stack));
         push___reax();
         mov____eax__DWORD_PTR_r12_imm8(OFFSETOF(Heap, jit_error_base));
         push___reax();

         mov____QWORD_PTR_r12_imm8__rsp(OFFSETOF(Heap, jit_error_sp));
         mov____DWORD_PTR_r12_imm8__imm(OFFSETOF(Heap, jit_error_pc), 0);
      #else
         mov____edx__DWORD_PTR_ebp_imm8(0x08); // TODO
         push___DWORD_PTR_edx_imm8(OFFSETOF(Heap, jit_error_sp));
         push___DWORD_PTR_edx_imm8(OFFSETOF(Heap, jit_error_pc));
         push___DWORD_PTR_edx_imm8(OFFSETOF(Heap, jit_error_stack));
         push___DWORD_PTR_edx_imm8(OFFSETOF(Heap, jit_error_base));
         mov____DWORD_PTR_edx_imm8__esp(OFFSETOF(Heap, jit_error_sp));
         mov____DWORD_PTR_edx_imm8__imm(OFFSETOF(Heap, jit_error_pc), 0);
      #endif
      ref1 = heap->jit_code_len;
      if (SH(marker_pos >= -128 && marker_pos < 128)) {
         lea____eax__resi_imm8(marker_pos);
      }
      else {
         lea____eax__resi_imm32(marker_pos);
      }
      mov____DWORD_PTR_redx_imm8__esi(OFFSETOF(Heap, jit_error_stack));
      mov____DWORD_PTR_redx_imm8__eax(OFFSETOF(Heap, jit_error_base));
   }

   if (nfunc) {
      #ifdef JIT_X86_64
         #ifdef JIT_WIN64
            mov____rcx__r12();
            mov____rdx__imm64((intptr_t)nfunc);
            lea____r8__rsp_imm8(-0x28);
            mov____r9__rbp();
            if (!emit_func_call(heap, jit_call_native, 0)) return 0;
         #else
            push___resi();
            push___redi();
            mov____rdi__r12();
            mov____rsi__imm64((intptr_t)nfunc);
            lea____rdx__rsp_imm8(-8);
            mov____rcx__rbp();
            if (!emit_func_call(heap, jit_call_native, 0)) return 0;
            pop____redi();
            pop____resi();
         #endif
         if (!emit_call_reinit_regs(heap)) return 0;
      #else
         lea____eax__esp_imm8(-0x14);
         push___rebp();
         push___reax();
         push___imm32((intptr_t)nfunc);
         push___rebx();
         if (!emit_func_call(heap, jit_call_native, 0x10)) return 0;
      #endif
      cmp____eax__imm8(0);

      if (call2) {
         je_____rel8(0); // label 4
         ref4 = heap->jit_code_len;
      }
      else {
         jne____rel8(0); // label 3
         ref3 = heap->jit_code_len;
      
         if (!jit_append_unwind(heap)) return 0;
      
         // label 3:
         heap->jit_code[ref3-1] = heap->jit_code_len - ref3;
      }
   }
   else if (dest_addr < 0) {
      #ifdef JIT_X86_64
         mov____eax__DWORD_PTR_rcx_imm8(OFFSETOF(Function, jit_addr));
         add____rax__QWORD_PTR_rbx_imm32(OFFSETOF(Heap, jit_code));
         call___reax();
      #else
         push___rebx();
         mov____eax__DWORD_PTR_ebx_imm32(OFFSETOF(Heap, jit_code));
         add____eax__DWORD_PTR_ecx_imm8(OFFSETOF(Function, jit_addr));
         call___reax();
         add____esp__imm8(0x04);
      #endif
   }
   else {
      #ifndef JIT_X86_64
         push___rebx();
      #endif
      call___rel32(dest_addr - heap->jit_code_len - 4);
      *label_addr = heap->jit_code_len;
      #ifndef JIT_X86_64
         add____esp__imm8(0x04);
      #endif
   }
   
   #ifdef JIT_X86_64
      mov____rbx__r12();
      add____rdi__QWORD_PTR_rbx_imm8(OFFSETOF(Heap, stack_data));
      add____rsi__QWORD_PTR_rbx_imm8(OFFSETOF(Heap, stack_flags));
   #else
      add____edi__DWORD_PTR_ebx_imm8(OFFSETOF(Heap, stack_data));
      add____esi__DWORD_PTR_ebx_imm8(OFFSETOF(Heap, stack_flags));
   #endif

   if (call2) {
      if (SH((marker_pos+1)*4 >= -128 && (marker_pos+1)*4 < 128)) {
         mov____DWORD_PTR_redi_imm8__imm((marker_pos+1)*4, 0);
         mov____BYTE_PTR_resi_imm8__imm((marker_pos+1), 0);
      }
      else {
         mov____DWORD_PTR_redi_imm32__imm((marker_pos+1)*4, 0);
         mov____BYTE_PTR_resi_imm32__imm((marker_pos+1), 0);
      }

      jmp____rel8(0); // label 2
      ref2 = heap->jit_code_len;

      // label 1:
      label = heap->jit_code_len;
      memcpy(&heap->jit_code[ref1-4], &label, 4);

      if (nfunc) {
         // label 4:
         heap->jit_code[ref4-1] = heap->jit_code_len - ref4;
      }

      #ifdef JIT_X86_64
         lea____rbp__rsp_imm8(0x30);
         mov____rbx__r12();
         add____rdi__QWORD_PTR_rbx_imm8(OFFSETOF(Heap, stack_data));
         add____rsi__QWORD_PTR_rbx_imm8(OFFSETOF(Heap, stack_flags));
      #else
         lea____ebp__esp_imm8(0x18);
         mov____edx__DWORD_PTR_ebp_imm8(0x08); // TODO
         add____edi__DWORD_PTR_edx_imm8(OFFSETOF(Heap, stack_data));
         add____esi__DWORD_PTR_edx_imm8(OFFSETOF(Heap, stack_flags));
      #endif
   
      // label 2:
      heap->jit_code[ref2-1] = heap->jit_code_len - ref2;

      #ifdef JIT_X86_64
         pop____reax();
         mov____DWORD_PTR_r12_imm8__eax(OFFSETOF(Heap, jit_error_base));
         pop____reax();
         mov____DWORD_PTR_r12_imm8__eax(OFFSETOF(Heap, jit_error_stack));
         pop____reax();
         mov____DWORD_PTR_r12_imm8__eax(OFFSETOF(Heap, jit_error_pc));
         pop____reax();
         mov____QWORD_PTR_r12_imm8__rax(OFFSETOF(Heap, jit_error_sp));
      #else
         mov____edx__DWORD_PTR_ebp_imm8(0x08); // TODO
         pop____DWORD_PTR_edx_imm8(OFFSETOF(Heap, jit_error_base));
         pop____DWORD_PTR_edx_imm8(OFFSETOF(Heap, jit_error_stack));
         pop____DWORD_PTR_edx_imm8(OFFSETOF(Heap, jit_error_pc));
         pop____DWORD_PTR_edx_imm8(OFFSETOF(Heap, jit_error_sp));
      #endif
   }
#else
   return 0;
#endif
   return 1;
}


static inline int jit_add_error_stub(Heap *heap, DynArray *error_stubs, int pc, int error)
{
   if (dynarray_add(error_stubs, (void *)(intptr_t)heap->jit_code_len)) return 0;
   if (dynarray_add(error_stubs, (void *)(intptr_t)JIT_PC_ERR(pc, error))) return 0;
   return 1;
}


static inline int jit_append_stack_error_stub(Heap *heap, int error)
{
#if defined(JIT_X86)
   #ifdef JIT_X86_64
      #ifdef JIT_WIN64
         mov____rcx__r12();
         mov____rdx__QWORD_PTR_rsp();
      #else
         mov____rdi__r12();
         mov____rsi__QWORD_PTR_rsp();
      #endif
      sub____rsp__imm8(8); // align
   #else
      // return pc already on stack
      push___DWORD_PTR_ebp_imm8(0x08); // heap
   #endif
   if (!emit_func_call(heap, jit_get_pc, 0)) return 0;
   if (!emit_call_reinit_regs(heap)) return 0;
   shl____eax__imm(8);
   lea____edx__eax_imm8(error);
   jmp____rel32(heap->jit_error_code - heap->jit_code_len - 4);
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_array_get(Heap *heap, int slot, int pc, DynArray *error_stubs)
{
#ifdef JIT_DEBUG
   printf("   array_get %d, acc\n", slot);
#endif
#if defined(JIT_X86)
   if (!jit_add_array_get_ref(heap)) return 0;

   #ifdef JIT_X86_64
      mov____rcx__imm(slot);
   #else
      mov____ecx__imm(slot);
   #endif
   mov____edx__DWORD_PTR_redi_recx_4();
   cmp____edx__imm32(heap->size);
   jae____rel32(0);
   if (!jit_add_error_stub(heap, error_stubs, pc, JIT_ERROR_INVALID_ARRAY)) return 0;
   #ifdef JIT_X86_64
      movzx__rbx__BYTE_PTR_rdx_r13(); // jit_array_get_funcs
      lea____rbx__r10_rbx_4_imm32(heap->jit_array_get_func_base);
   #else
      movzx__ebx__BYTE_PTR_edx_imm32((intptr_t)heap->jit_array_get_funcs);
      lea____ebx__ebx_4_imm32((intptr_t)heap->jit_code + heap->jit_array_get_func_base);
   #endif
   call___rebx();
#else
   return 0;
#endif
   if (!jit_add_pc(heap, pc)) return 0;
   return 1;
}


static inline void jit_update_array_get(Heap *heap, unsigned char *ptr)
{
#if defined(JIT_X86)
   int value;

   value = heap->size;
   #ifdef JIT_X86_64
      memcpy(ptr+12, &value, sizeof(int));
   #else
      memcpy(ptr+10, &value, sizeof(int));
   #endif

   #ifndef JIT_X86_64
      value = (intptr_t)heap->jit_array_get_funcs;
      memcpy(ptr+23, &value, sizeof(int));

      value = (intptr_t)heap->jit_code + heap->jit_array_get_func_base;
      memcpy(ptr+30, &value, sizeof(int));
   #endif
#endif
}


static inline int jit_append_array_get_func(Heap *heap, int type)
{
#if defined(JIT_X86)
   cmp____BYTE_PTR_resi_recx_1__imm8(0);
   je_____rel32(heap->jit_invalid_array_stack_error_code - heap->jit_code_len - 4);

   if (sizeof(Array) == 32) {
      shl____edx__imm(5);
   }
   else {
      imul___edx__edx__imm8(sizeof(Array));
   }
   #ifdef JIT_X86_64
      mov____r8__imm64((intptr_t)heap->data);
      if (!jit_add_heap_data_ref(heap)) return 0;
      add____rdx__r8();
   #else
      add____edx__imm32((intptr_t)heap->data);
      if (!jit_add_heap_data_ref(heap)) return 0;
   #endif

   cmp____eax__DWORD_PTR_redx_imm8(OFFSETOF(Array, len));
   jae____rel32(heap->jit_out_of_bounds_stack_error_code - heap->jit_code_len - 4);

   #ifdef JIT_X86_64
      mov____rbx__QWORD_PTR_rdx_imm8(OFFSETOF(Array, flags));
   #else
      mov____ebx__DWORD_PTR_redx_imm8(OFFSETOF(Array, flags));
   #endif
   mov____ecx__eax();
   shr____eax__imm(5);
   mov____eax__DWORD_PTR_rebx_reax_4();

   mov____ebx__imm(1);
   shl____ebx__cl();
   test___eax__ebx();
   mov____ebx__imm(0);
   setne__bl();

   #ifdef JIT_X86_64
      mov____rax__QWORD_PTR_rdx_imm8(OFFSETOF(Array, data));
   #else
      mov____eax__DWORD_PTR_redx_imm8(OFFSETOF(Array, data));
   #endif
   if (type == ARR_BYTE) {
      movzx__eax__BYTE_PTR_reax_recx_1();
   }
   else if (type == ARR_SHORT) {
      movzx__eax__WORD_PTR_reax_recx_2();
   }
   else if (type == ARR_INT) {
      mov____eax__DWORD_PTR_reax_recx_4();
   }
   ret____();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_array_set(Heap *heap, int slot1, int slot2, int append, int pc, DynArray *error_stubs)
{
#ifdef JIT_DEBUG
   if (append) {
      printf("   array_append %d, acc\n", slot1);
   }
   else {
      printf("   array_set %d, %d, acc\n", slot1, slot2);
   }
#endif
#if defined(JIT_X86)
   if (append) {
      if (!jit_add_array_append_ref(heap)) return 0;
   }
   else {
      if (!jit_add_array_set_ref(heap)) return 0;
   }

   #ifdef JIT_X86_64
      mov____rcx__imm(slot1);
   #else
      mov____ecx__imm(slot1);
   #endif
   cmp____BYTE_PTR_resi_recx_1__imm8(0);
   je_____rel32(0);
   if (!jit_add_error_stub(heap, error_stubs, pc, JIT_ERROR_INVALID_ARRAY)) return 0;

   mov____edx__DWORD_PTR_redi_recx_4();
   cmp____edx__imm32(heap->size);
   jae____rel32(0);
   if (!jit_add_error_stub(heap, error_stubs, pc, JIT_ERROR_INVALID_ARRAY)) return 0;

   #ifdef JIT_X86_64
      if (append) {
         add____rbx__r15();
      }
      else {
         add____rbx__r14();
      }
      movzx__ebx__BYTE_PTR_rbx_rdx_2();
      lea____rbx__r10_rbx_4_imm32(append? heap->jit_array_append_func_base : heap->jit_array_set_func_base);
   #else
      movzx__ebx__BYTE_PTR_ebx_edx_2_imm32((intptr_t)(append? heap->jit_array_append_funcs : heap->jit_array_set_funcs));
      lea____ebx__ebx_4_imm32((intptr_t)heap->jit_code + (append? heap->jit_array_append_func_base : heap->jit_array_set_func_base));
   #endif

   if (!append) {
      if (SH(slot2*4 >= -128 && slot2*4 < 128)) {
         mov____ecx__DWORD_PTR_redi_imm8(slot2*4);
      }
      else {
         mov____ecx__DWORD_PTR_redi_imm32(slot2*4);
      }
   }
   call___rebx();
#else
   return 0;
#endif
   if (!jit_add_pc(heap, pc)) return 0;
   return 1;
}


static inline void jit_update_array_set(Heap *heap, unsigned char *ptr, int append)
{
#if defined(JIT_X86)
   int value;

   value = heap->size;
   #ifdef JIT_X86_64
      memcpy(ptr+22, &value, sizeof(int));
   #else
      memcpy(ptr+20, &value, sizeof(int));
   #endif

   #ifndef JIT_X86_64
      value = (intptr_t)(append? heap->jit_array_append_funcs : heap->jit_array_set_funcs);
      memcpy(ptr+34, &value, sizeof(int));

      value = (intptr_t)heap->jit_code + (append? heap->jit_array_append_func_base : heap->jit_array_set_func_base);
      memcpy(ptr+41, &value, sizeof(int));
   #endif
#endif
}


static inline int jit_append_array_set_func(Heap *heap, int type, int flag, int shared, int append)
{
#if defined(JIT_X86)
   if (shared && flag) {
      lea____ebx__eax_imm8(-1);
      cmp____ebx__imm32((1<<23)-1);
      jb_____rel32(heap->jit_invalid_shared_stack_error_code - heap->jit_code_len - 4);
   }

   if (type == ARR_BYTE) {
      test___eax__imm(0xFFFFFF00);
      jne____rel32(heap->jit_upgrade_code[(append? 2:0) + flag] - heap->jit_code_len - 4);
   }
   else if (type == ARR_SHORT) {
      test___eax__imm(0xFFFF0000);
      jne____rel32(heap->jit_upgrade_code[(append? 2:0) + flag] - heap->jit_code_len - 4);
   }

   if (append) {
      mov____ebx__edx();
   }

   if (sizeof(Array) == 32) {
      shl____edx__imm(5);
   }
   else {
      imul___edx__edx__imm8(sizeof(Array));
   }
   #ifdef JIT_X86_64
      mov____r8__imm64((intptr_t)heap->data);
      if (!jit_add_heap_data_ref(heap)) return 0;
      add____rdx__r8();
   #else
      add____edx__imm32((intptr_t)heap->data);
      if (!jit_add_heap_data_ref(heap)) return 0;
   #endif

   if (append) {
      mov____ecx__DWORD_PTR_redx_imm8(OFFSETOF(Array, len));
      cmp____ecx__DWORD_PTR_redx_imm8(OFFSETOF(Array, size));
      jae____rel32(heap->jit_upgrade_code[4 + flag] - heap->jit_code_len - 4);
      lea____ebx__ecx_imm8(1);
      mov____DWORD_PTR_redx_imm8__ebx(OFFSETOF(Array, len));
   }
   else {
      cmp____ecx__DWORD_PTR_redx_imm8(OFFSETOF(Array, len));
      jae____rel32(heap->jit_out_of_bounds_stack_error_code - heap->jit_code_len - 4);
   }

   #ifdef JIT_X86_64
      mov____rbx__QWORD_PTR_rdx_imm8(OFFSETOF(Array, data));
   #else
      mov____ebx__DWORD_PTR_redx_imm8(OFFSETOF(Array, data));
   #endif

   if (type == ARR_BYTE) {
      mov____BYTE_PTR_rebx_recx_1__al();
   }
   else if (type == ARR_SHORT) {
      mov____WORD_PTR_rebx_recx_2__ax();
   }
   else if (type == ARR_INT) {
      mov____DWORD_PTR_rebx_recx_4__eax();
   }

   if (!shared) {
      #ifdef JIT_X86_64
         mov____rdx__QWORD_PTR_rdx_imm8(OFFSETOF(Array, flags));
      #else
         mov____edx__DWORD_PTR_edx_imm8(OFFSETOF(Array, flags));
      #endif
      mov____eax__ecx();
      mov____ebx__imm(flag? 1 : ~1);
      shr____eax__imm(5);
      rol____ebx__cl();
      if (flag) {
         or_____DWORD_PTR_redx_reax_4__ebx();
      }
      else {
         and____DWORD_PTR_redx_reax_4__ebx();
      }
   }
   ret____();
#else
   return 0;
#endif
   return 1;
}


#if defined(JIT_X86)
#ifdef JIT_X86_64
static int jit_adj_array_set_func_base(Heap *heap) { return heap->jit_array_set_func_base; }
static int jit_adj_array_append_func_base(Heap *heap) { return heap->jit_array_append_func_base; }
#else
static void *jit_adj_array_set_funcs(Heap *heap) { return heap->jit_array_set_funcs; }
static void *jit_adj_array_append_funcs(Heap *heap) { return heap->jit_array_append_funcs; }
static void *jit_adj_array_set_func_base(Heap *heap) { return heap->jit_code + heap->jit_array_set_func_base; }
static void *jit_adj_array_append_func_base(Heap *heap) { return heap->jit_code + heap->jit_array_append_func_base; }
#endif
#endif

static inline int jit_append_array_upgrade_code(Heap *heap, int flag, int append, int expand)
{
#if defined(JIT_X86)
   if (expand) {
      mov____edx__ebx();
   }

   push___reax();
   push___recx();
   push___redx();

   if (sizeof(Array) == 32) {
      mov____ebx__edx();
      shl____ebx__imm(5);
   }
   else {
      imul___ebx__edx__imm8(sizeof(Array));
   }
   #ifdef JIT_X86_64
      mov____r8__imm64((intptr_t)heap->data);
      if (!jit_add_heap_data_ref(heap)) return 0;
      add____rbx__r8();
   #else
      add____ebx__imm32((intptr_t)heap->data);
      if (!jit_add_heap_data_ref(heap)) return 0;
   #endif

   #ifdef JIT_X86_64
      #ifdef JIT_WIN64
         mov____r9__rax();
         mov____r8__rdx();
         mov____rdx__rbx();
         mov____rcx__r12();
         if (!emit_func_call(heap, expand? jit_expand_array : upgrade_array, 0)) return 0;
      #else
         push___resi();
         push___redi();
         mov____rdi__r12();
         mov____rsi__rbx();
         mov____rcx__rax();
         if (!emit_func_call(heap, expand? jit_expand_array : upgrade_array, 0)) return 0;
         pop____redi();
         pop____resi();
      #endif
      if (!emit_call_reinit_regs(heap)) return 0;
   #else
      push___reax();
      push___redx();
      push___rebx();
      push___DWORD_PTR_ebp_imm8(0x08);
      if (!emit_func_call(heap, expand? jit_expand_array : upgrade_array, 0x10)) return 0;
   #endif

   mov____ebx__eax();
   pop____redx();
   pop____recx();
   pop____reax();

   cmp____ebx__imm8(FIXSCRIPT_ERR_INVALID_SHARED_ARRAY_OPERATION);
   je_____rel32(heap->jit_invalid_shared_stack_error_code - heap->jit_code_len - 4);

   cmp____ebx__imm8(0);
   jne____rel32(heap->jit_out_of_memory_stack_error_code - heap->jit_code_len - 4);

   if (flag) {
      mov____ebx__imm(1);
   }
   else {
      xor____ebx__ebx();
   }

   #ifdef JIT_X86_64
      if (append) {
         add____rbx__r15();
      }
      else {
         add____rbx__r14();
      }
      movzx__ebx__BYTE_PTR_rbx_rdx_2();
      lea____rbx__r10_rbx_4_imm32(0);
      if (!jit_add_adjustment_int(heap, append? jit_adj_array_append_func_base : jit_adj_array_set_func_base)) return 0;
   #else
      movzx__ebx__BYTE_PTR_ebx_edx_2_imm32(0);
      if (!jit_add_adjustment_ptr(heap, append? jit_adj_array_append_funcs : jit_adj_array_set_funcs)) return 0;
      lea____ebx__ebx_4_imm32(0);
      if (!jit_add_adjustment_ptr(heap, append? jit_adj_array_append_func_base : jit_adj_array_set_func_base)) return 0;
   #endif
   jmp____rebx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_hash_get(Heap *heap, int slot, int pc, DynArray *error_stubs)
{
#ifdef JIT_DEBUG
   printf("   hash_get %d, acc\n", slot);
#endif
#if defined(JIT_X86)
   #ifdef JIT_X86_64
      #ifdef JIT_WIN64
         mov____r8d__eax();
         shl____rbx__imm(32);
         or_____r8__rbx();
         if (SH(slot*4 >= -128 && slot*4 < 128)) {
            mov____edx__DWORD_PTR_redi_imm8(slot*4);
            movzx__eax__BYTE_PTR_resi_imm8(slot);
         }
         else {
            mov____edx__DWORD_PTR_redi_imm32(slot*4);
            movzx__eax__BYTE_PTR_resi_imm32(slot);
         }
         shl____rax__imm(32);
         or_____rdx__rax();
         mov____rcx__r12();
         if (!emit_func_call(heap, jit_hash_get, 0)) return 0;
      #else
         push___resi();
         push___redi();
         mov____edx__eax();
         shl____rbx__imm(32);
         or_____rdx__rbx();
         if (SH(slot*4 >= -128 && slot*4 < 128)) {
            movzx__eax__BYTE_PTR_resi_imm8(slot);
            mov____esi__DWORD_PTR_redi_imm8(slot*4);
         }
         else {
            movzx__eax__BYTE_PTR_resi_imm32(slot);
            mov____esi__DWORD_PTR_redi_imm32(slot*4);
         }
         shl____rax__imm(32);
         or_____rsi__rax();
         mov____rdi__r12();
         if (!emit_func_call(heap, jit_hash_get, 0)) return 0;
         pop____redi();
         pop____resi();
      #endif
      if (!emit_call_reinit_regs(heap)) return 0;
      mov____rdx__rax();
      shr____rdx__imm(32);
      mov____eax__eax();
   #else
      push___rebx();
      push___reax();
      if (SH(slot*4 >= -128 && slot*4 < 128)) {
         movzx__eax__BYTE_PTR_resi_imm8(slot);
         push___reax();
         push___DWORD_PTR_edi_imm8(slot*4);
      }
      else {
         movzx__eax__BYTE_PTR_resi_imm32(slot);
         push___reax();
         push___DWORD_PTR_edi_imm32(slot*4);
      }
      push___DWORD_PTR_ebp_imm8(0x08); // heap
      if (!emit_func_call(heap, jit_hash_get, 0x14)) return 0;
   #endif

   cmp____edx__imm8(2);
   je_____rel32(0);
   if (!jit_add_error_stub(heap, error_stubs, pc, JIT_ERROR_INVALID_HASH)) return 0;

   cmp____edx__imm8(3);
   je_____rel32(0);
   if (!jit_add_error_stub(heap, error_stubs, pc, JIT_ERROR_KEY_NOT_FOUND)) return 0;

   mov____ebx__edx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_hash_set(Heap *heap, int slot1, int slot2, int pc, DynArray *error_stubs)
{
#ifdef JIT_DEBUG
   printf("   hash_set %d, %d, acc\n", slot1, slot2);
#endif
#if defined(JIT_X86)
   #ifdef JIT_X86_64
      #ifdef JIT_WIN64
         mov____r9d__eax();
         shl____rbx__imm(32);
         or_____r9__rbx();
         if (SH(slot2*4 >= -128 && slot2*4 < 128)) {
            mov____r8d__DWORD_PTR_rdi_imm8(slot2*4);
            movzx__eax__BYTE_PTR_resi_imm8(slot2);
         }
         else {
            mov____r8d__DWORD_PTR_rdi_imm32(slot2*4);
            movzx__eax__BYTE_PTR_resi_imm32(slot2);
         }
         shl____rax__imm(32);
         or_____r8__rax();
         if (SH(slot1*4 >= -128 && slot1*4 < 128)) {
            mov____edx__DWORD_PTR_redi_imm8(slot1*4);
            movzx__eax__BYTE_PTR_resi_imm8(slot1);
         }
         else {
            mov____edx__DWORD_PTR_redi_imm32(slot1*4);
            movzx__eax__BYTE_PTR_resi_imm32(slot1);
         }
         shl____rax__imm(32);
         or_____rdx__rax();
         mov____rcx__r12();
         if (!emit_func_call(heap, jit_hash_set, 0)) return 0;
      #else
         push___resi();
         push___redi();

         mov____ecx__eax();
         shl____rbx__imm(32);
         or_____rcx__rbx();

         if (SH(slot2*4 >= -128 && slot2*4 < 128)) {
            mov____edx__DWORD_PTR_redi_imm8(slot2*4);
            movzx__eax__BYTE_PTR_resi_imm8(slot2);
         }
         else {
            mov____edx__DWORD_PTR_redi_imm32(slot2*4);
            movzx__eax__BYTE_PTR_resi_imm32(slot2);
         }
         shl____rax__imm(32);
         or_____rdx__rax();

         if (SH(slot1*4 >= -128 && slot1*4 < 128)) {
            movzx__eax__BYTE_PTR_resi_imm8(slot1);
            mov____esi__DWORD_PTR_redi_imm8(slot1*4);
         }
         else {
            movzx__eax__BYTE_PTR_resi_imm32(slot1);
            mov____esi__DWORD_PTR_redi_imm32(slot1*4);
         }
         shl____rax__imm(32);
         or_____rsi__rax();

         mov____rdi__r12();

         if (!emit_func_call(heap, jit_hash_set, 0)) return 0;

         pop____redi();
         pop____resi();
      #endif
      if (!emit_call_reinit_regs(heap)) return 0;
   #else
      push___rebx();
      push___reax();
      if (SH(slot2*4 >= -128 && slot2*4 < 128)) {
         movzx__eax__BYTE_PTR_resi_imm8(slot2);
         push___reax();
         push___DWORD_PTR_edi_imm8(slot2*4);
      }
      else {
         movzx__eax__BYTE_PTR_resi_imm32(slot2);
         push___reax();
         push___DWORD_PTR_edi_imm32(slot2*4);
      }
      if (SH(slot1*4 >= -128 && slot1*4 < 128)) {
         movzx__eax__BYTE_PTR_resi_imm8(slot1);
         push___reax();
         push___DWORD_PTR_edi_imm8(slot1*4);
      }
      else {
         movzx__eax__BYTE_PTR_resi_imm32(slot1);
         push___reax();
         push___DWORD_PTR_edi_imm32(slot1*4);
      }
      push___DWORD_PTR_ebp_imm8(0x08); // heap
      if (!emit_func_call(heap, jit_hash_set, 0x1C)) return 0;
   #endif

   cmp____eax__imm8(1);
   je_____rel32(0);
   if (!jit_add_error_stub(heap, error_stubs, pc, JIT_ERROR_INVALID_HASH)) return 0;

   cmp____eax__imm8(2);
   je_____rel32(0);
   if (!jit_add_error_stub(heap, error_stubs, pc, JIT_ERROR_OUT_OF_MEMORY)) return 0;
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_load_local(Heap *heap, int idx)
{
#ifdef JIT_DEBUG
   printf("   load_local %d\n", idx);
#endif
#if defined(JIT_X86)
   #ifdef JIT_X86_64
      mov____rcx__QWORD_PTR_r12_imm8(OFFSETOF(Heap, locals_data));
      mov____eax__DWORD_PTR_recx_imm32(idx*4);
      mov____rcx__QWORD_PTR_r12_imm8(OFFSETOF(Heap, locals_flags));
      movzx__ebx__BYTE_PTR_recx_imm32(idx);
   #else
      mov____edx__DWORD_PTR_ebp_imm8(0x08); // heap
      mov____ecx__DWORD_PTR_redx_imm8(OFFSETOF(Heap, locals_data));
      mov____eax__DWORD_PTR_recx_imm32(idx*4);
      mov____ecx__DWORD_PTR_redx_imm8(OFFSETOF(Heap, locals_flags));
      movzx__ebx__BYTE_PTR_recx_imm32(idx);
   #endif
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_store_local(Heap *heap, int idx)
{
#ifdef JIT_DEBUG
   printf("   store_local %d\n", idx);
#endif
#if defined(JIT_X86)
   #ifdef JIT_X86_64
      mov____rcx__QWORD_PTR_r12_imm8(OFFSETOF(Heap, locals_data));
      mov____DWORD_PTR_recx_imm32__eax(idx*4);
      mov____rcx__QWORD_PTR_r12_imm8(OFFSETOF(Heap, locals_flags));
      mov____BYTE_PTR_recx_imm32__bl(idx);
   #else
      mov____edx__DWORD_PTR_ebp_imm8(0x08); // heap
      mov____ecx__DWORD_PTR_redx_imm8(OFFSETOF(Heap, locals_data));
      mov____DWORD_PTR_recx_imm32__eax(idx*4);
      mov____ecx__DWORD_PTR_redx_imm8(OFFSETOF(Heap, locals_flags));
      mov____BYTE_PTR_recx_imm32__bl(idx);
   #endif
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_switch_case(Heap *heap, int value, int *label_addr)
{
#ifdef JIT_DEBUG
   printf("   switch_case %d\n", value);
#endif
#if defined(JIT_X86)
   if (SH(value >= -128 && value < 128)) {
      cmp____eax__imm8(value);
   }
   else {
      cmp____eax__imm32(value);
   }
   je_____rel32(0);
   *label_addr = heap->jit_code_len;
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_switch_range(Heap *heap, unsigned int from, unsigned int to, int *label_addr)
{
#ifdef JIT_DEBUG
   printf("   switch_range %d, %d\n", from, to);
#endif
#if defined(JIT_X86)
   mov____edx__eax();
   if (SH(from < 128)) {
      sub____edx__imm8(from);
   }
   else {
      sub____edx__imm32(from);
   }
   if (SH(to - from < 128)) {
      cmp____edx__imm8(to - from);
   }
   else {
      cmp____edx__imm32(to - from);
   }
   jbe____rel32(0);
   *label_addr = heap->jit_code_len;
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_length(Heap *heap, int pc, DynArray *error_stubs)
{
#ifdef JIT_DEBUG
   printf("   length\n");
#endif
#if defined(JIT_X86)
   if (!jit_add_length_ref(heap)) return 0;

   cmp____ebx__imm8(0);
   je_____rel32(0);
   if (!jit_add_error_stub(heap, error_stubs, pc, JIT_ERROR_ARRAY_OR_HASH)) return 0;

   lea____edx__eax_imm8(-1);
   cmp____edx__imm32(heap->size - 1);
   jae____rel32(0);
   if (!jit_add_error_stub(heap, error_stubs, pc, JIT_ERROR_ARRAY_OR_HASH)) return 0;

   if (sizeof(Array) == 32) {
      mov____edx__eax();
      shl____edx__imm(5);
   }
   else {
      imul___edx__eax__imm8(sizeof(Array));
   }
   #ifdef JIT_X86_64
      mov____r8__imm64((intptr_t)heap->data);
      add____rdx__r8();
   #else
      add____edx__imm32((intptr_t)heap->data);
   #endif

   mov____eax__DWORD_PTR_redx_imm8(OFFSETOF(Array, len));
   cmp____eax__imm8(-1);
   je_____rel32(0);
   if (!jit_add_error_stub(heap, error_stubs, pc, JIT_ERROR_ARRAY_OR_HASH)) return 0;

   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static inline void jit_update_length(Heap *heap, unsigned char *ptr)
{
#if defined(JIT_X86)
   intptr_t ptr_value;
   int value;

   value = heap->size - 1;
   memcpy(ptr+14, &value, sizeof(int));

   ptr_value = (intptr_t)heap->data;
   #ifdef JIT_X86_64
      memcpy(ptr+31, &ptr_value, sizeof(intptr_t));
   #else
      memcpy(ptr+29, &ptr_value, sizeof(intptr_t));
   #endif
#endif
}


static inline int jit_append_vararg_op(Heap *heap, int stack_pos, int num, int pc, int op)
{
   void *func = NULL;
   int ref1;
#ifdef JIT_DEBUG
   printf("   ");
   switch (op) {
      case BC_CREATE_ARRAY:  printf("create_array"); break;
      case BC_CREATE_HASH:   printf("create_hash"); break;
      case BC_STRING_CONCAT: printf("string_concat"); break;
   }
   printf(" %d, %d\n", stack_pos, num);
#endif
   switch (op) {
      case BC_CREATE_ARRAY:  func = jit_create_array; break;
      case BC_CREATE_HASH:   func = jit_create_hash; break;
      case BC_STRING_CONCAT: func = jit_string_concat; break;
   }
#if defined(JIT_X86)
   #ifdef JIT_X86_64
      mov____rbx__r12();
      sub____rdi__QWORD_PTR_rbx_imm8(OFFSETOF(Heap, stack_data));
      sub____rsi__QWORD_PTR_rbx_imm8(OFFSETOF(Heap, stack_flags));
   #else
      mov____ebx__DWORD_PTR_ebp_imm8(0x08);
      sub____edi__DWORD_PTR_ebx_imm8(OFFSETOF(Heap, stack_data));
      sub____esi__DWORD_PTR_ebx_imm8(OFFSETOF(Heap, stack_flags));
   #endif
   if (SH(stack_pos < 128)) {
      lea____eax__resi_imm8(stack_pos);
   }
   else {
      lea____eax__resi_imm32(stack_pos);
   }
   mov____DWORD_PTR_rebx_imm8__eax(OFFSETOF(Heap, stack_len));

   #ifdef JIT_X86_64
      #ifdef JIT_WIN64
         mov____rcx__r12();
         mov____edx__imm(num);
         mov____r8d__imm(pc);
         if (!emit_func_call(heap, func, 0)) return 0;
      #else
         push___resi();
         push___redi();
         mov____rdi__r12();
         mov____esi__imm(num);
         mov____edx__imm(pc);
         if (!emit_func_call(heap, func, 0)) return 0;
         pop____redi();
         pop____resi();
      #endif
      if (!emit_call_reinit_regs(heap)) return 0;
   #else
      push___imm32(pc);
      if (SH(num < 128)) {
         push___imm8(num);
      }
      else {
         push___imm32(num);
      }
      push___rebx();
      if (!emit_func_call(heap, func, 0x0C)) return 0;
   #endif
   cmp____eax__imm8(0);
   jne____rel8(0); // label 1
   ref1 = heap->jit_code_len;
      
   if (!jit_append_unwind(heap)) return 0;
      
   // label 1:
   heap->jit_code[ref1-1] = heap->jit_code_len - ref1;

   #ifdef JIT_X86_64
      add____rdi__QWORD_PTR_rbx_imm8(OFFSETOF(Heap, stack_data));
      add____rsi__QWORD_PTR_rbx_imm8(OFFSETOF(Heap, stack_flags));
   #else
      add____edi__DWORD_PTR_ebx_imm8(OFFSETOF(Heap, stack_data));
      add____esi__DWORD_PTR_ebx_imm8(OFFSETOF(Heap, stack_flags));
   #endif
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_check_stack(Heap *heap, int size, int pc)
{
   int label1, ref2;
#ifdef JIT_DEBUG
   printf("   check_stack %d\n", size);
#endif
#if defined(JIT_X86)
   // label 1:
   label1 = heap->jit_code_len;
   #ifdef JIT_X86_64
      mov____rdx__r12(); // heap
   #else
      mov____edx__DWORD_PTR_ebp_imm8(0x08); // heap
   #endif
   mov____eax__DWORD_PTR_redx_imm8(OFFSETOF(Heap, stack_len));
   if (SH(size < 128)) {
      add____eax__imm8(size);
   }
   else {
      add____eax__imm32(size);
   }
   cmp____eax__DWORD_PTR_redx_imm8(OFFSETOF(Heap, stack_cap));
   jle____rel8(0); // label 2
   ref2 = heap->jit_code_len;

   #ifdef JIT_X86_64
      #ifdef JIT_WIN64
         mov____rcx__r12();
      #else
         mov____rdi__r12();
      #endif
      if (!emit_func_call(heap, expand_stack, 0)) return 0;
      if (!emit_call_reinit_regs(heap)) return 0;
   #else
      push___redx();
      if (!emit_func_call(heap, expand_stack, 4)) return 0;
   #endif
   cmp____eax__imm8(0);
   jne____rel8(label1 - heap->jit_code_len - 1); // label 1

   mov____edx__imm(JIT_PC_ERR(pc, JIT_ERROR_STACK_OVERFLOW));
   jmp____rel32(heap->jit_error_code - heap->jit_code_len - 4);

   // label 2:
   heap->jit_code[ref2-1] = heap->jit_code_len - ref2;
   if (SH(size < 128)) {
      sub____eax__imm8(size);
   }
   else {
      sub____eax__imm32(size);
   }
   #ifdef JIT_X86_64
      mov____rdi__QWORD_PTR_rdx_imm8(OFFSETOF(Heap, stack_data));
      mov____rsi__QWORD_PTR_rdx_imm8(OFFSETOF(Heap, stack_flags));
      lea____rdi__rdi_rax_4();
      add____rsi__rax();
   #else
      mov____edi__DWORD_PTR_edx_imm8(OFFSETOF(Heap, stack_data));
      mov____esi__DWORD_PTR_edx_imm8(OFFSETOF(Heap, stack_flags));
      lea____edi__edi_eax_4();
      add____esi__eax();
   #endif
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_minmax(Heap *heap, int slot, int min)
{
#ifdef JIT_DEBUG
   printf("   %s %d\n", min? "min" : "max", slot);
#endif
#if defined(JIT_X86)
   if (SH(slot*4 >= -128 && slot*4 < 128)) {
      mov____edx__DWORD_PTR_redi_imm8(slot*4);
   }
   else {
      mov____edx__DWORD_PTR_redi_imm32(slot*4);
   }
   cmp____eax__edx();
   if (min) {
      jl_____rel8(2);
   }
   else {
      jg_____rel8(2);
   }
   mov____eax__edx();
   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_clamp(Heap *heap, int value_slot, int min_slot)
{
#ifdef JIT_DEBUG
   printf("   clamp %d, %d, acc\n", value_slot, min_slot);
#endif
#if defined(JIT_X86)
   if (SH(value_slot*4 >= -128 && value_slot*4 < 128)) {
      mov____ecx__DWORD_PTR_redi_imm8(value_slot*4);
   }
   else {
      mov____ecx__DWORD_PTR_redi_imm32(value_slot*4);
   }
   if (SH(min_slot*4 >= -128 && min_slot*4 < 128)) {
      mov____edx__DWORD_PTR_redi_imm8(min_slot*4);
   }
   else {
      mov____edx__DWORD_PTR_redi_imm32(min_slot*4);
   }
   cmp____ecx__edx();
   jg_____rel8(2);
   mov____ecx__edx();
   cmp____eax__ecx();
   jl_____rel8(2);
   mov____eax__ecx();
   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_abs(Heap *heap, int pc)
{
#ifdef JIT_DEBUG
   printf("   abs\n");
#endif
#if defined(JIT_X86)
   int ref1;

   cmp____eax__imm8(0);
   jge____rel8(0);
   ref1 = heap->jit_code_len;

   neg____eax();
   if (!emit_overflow_check(heap, pc)) return 0;

   // label 1:
   heap->jit_code[ref1-1] = heap->jit_code_len - ref1;

   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_addsub32(Heap *heap, int dest1, int src1, int src2, int add)
{
#ifdef JIT_DEBUG
   printf("   %s %d, acc <- %d, %d, acc\n", add? "add32" : "sub32", dest1, src1, src2);
#endif
#if defined(JIT_X86)
   and____eax__imm8(1);
   if (add) {
      mov____edx__eax();
      xor____eax__eax();
      if (SH(src1*4 >= -128 && src1*4 < 128)) {
         add____edx__DWORD_PTR_redi_imm8(src1*4);
      }
      else {
         add____edx__DWORD_PTR_redi_imm32(src1*4);
      }
      adc____eax__imm8(0);
      if (SH(src2*4 >= -128 && src2*4 < 128)) {
         add____edx__DWORD_PTR_redi_imm8(src2*4);
      }
      else {
         add____edx__DWORD_PTR_redi_imm32(src2*4);
      }
      adc____eax__imm8(0);
   }
   else {
      mov____ecx__eax();
      xor____eax__eax();
      if (SH(src1*4 >= -128 && src1*4 < 128)) {
         mov____edx__DWORD_PTR_redi_imm8(src1*4);
      }
      else {
         mov____edx__DWORD_PTR_redi_imm32(src1*4);
      }
      if (SH(src2*4 >= -128 && src2*4 < 128)) {
         sub____edx__DWORD_PTR_redi_imm8(src2*4);
      }
      else {
         sub____edx__DWORD_PTR_redi_imm32(src2*4);
      }
      sbb____eax__imm8(0);
      sub____edx__ecx();
      sbb____eax__imm8(0);
      and____eax__imm8(1);
   }
   if (SH(dest1*4 >= -128 && dest1*4 < 128)) {
      mov____DWORD_PTR_redi_imm8__edx(dest1*4);
      mov____BYTE_PTR_resi_imm8__imm(dest1, 0);
   }
   else {
      mov____DWORD_PTR_redi_imm32__edx(dest1*4);
      mov____BYTE_PTR_resi_imm32__imm(dest1, 0);
   }
   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_addsub64(Heap *heap, int dest1, int src1, int src2, int src3, int add)
{
#ifdef JIT_DEBUG
   printf("   %s %d, acc <- %d, %d, %d, acc\n", add? "add64" : "sub64", dest1, src1, src2, src3);
#endif
#if defined(JIT_X86)
   mov____ecx__eax();
   if (SH(src1*4 >= -128 && src1*4 < 128)) {
      mov____edx__DWORD_PTR_redi_imm8(src1*4);
   }
   else {
      mov____edx__DWORD_PTR_redi_imm32(src1*4);
   }
   if (SH(src2*4 >= -128 && src2*4 < 128)) {
      mov____eax__DWORD_PTR_redi_imm8(src2*4);
   }
   else {
      mov____eax__DWORD_PTR_redi_imm32(src2*4);
   }
   if (add) {
      if (SH(src3*4 >= -128 && src3*4 < 128)) {
         add____edx__DWORD_PTR_redi_imm8(src3*4);
      }
      else {
         add____edx__DWORD_PTR_redi_imm32(src3*4);
      }
      adc____eax__ecx();
   }
   else {
      if (SH(src3*4 >= -128 && src3*4 < 128)) {
         sub____edx__DWORD_PTR_redi_imm8(src3*4);
      }
      else {
         sub____edx__DWORD_PTR_redi_imm32(src3*4);
      }
      sbb____eax__ecx();
   }
   if (SH(dest1*4 >= -128 && dest1*4 < 128)) {
      mov____DWORD_PTR_redi_imm8__edx(dest1*4);
      mov____BYTE_PTR_resi_imm8__imm(dest1, 0);
   }
   else {
      mov____DWORD_PTR_redi_imm32__edx(dest1*4);
      mov____BYTE_PTR_resi_imm32__imm(dest1, 0);
   }
   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_mul64(Heap *heap, int dest1, int src1, int is_signed)
{
#ifdef JIT_DEBUG
   printf("   %s %d, acc <- %d, acc\n", is_signed? "mul64" : "umul64", dest1, src1);
#endif
#if defined(JIT_X86)
   if (SH(src1*4 >= -128 && src1*4 < 128)) {
      if (is_signed) {
         imul___DWORD_PTR_redi_imm8(src1*4);
      }
      else {
         mul____DWORD_PTR_redi_imm8(src1*4);
      }
   }
   else {
      if (is_signed) {
         imul___DWORD_PTR_redi_imm32(src1*4);
      }
      else {
         mul____DWORD_PTR_redi_imm32(src1*4);
      }
   }
   if (SH(dest1*4 >= -128 && dest1*4 < 128)) {
      mov____DWORD_PTR_redi_imm8__eax(dest1*4);
      mov____BYTE_PTR_resi_imm8__imm(dest1, 0);
   }
   else {
      mov____DWORD_PTR_redi_imm32__eax(dest1*4);
      mov____BYTE_PTR_resi_imm32__imm(dest1, 0);
   }
   mov____eax__edx();
   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_mul64_long(Heap *heap, int dest1, int src1, int src2, int src3)
{
#ifdef JIT_DEBUG
   printf("   mul64 %d, acc <- %d, %d, %d, acc\n", dest1, src1, src2, src3);
#endif
#if defined(JIT_X86)
   if (SH(src1*4 >= -128 && src1*4 < 128)) {
      mov____ebx__DWORD_PTR_redi_imm8(src1*4);
   }
   else {
      mov____ebx__DWORD_PTR_redi_imm32(src1*4);
   }
   if (SH(src2*4 >= -128 && src2*4 < 128)) {
      mov____ecx__DWORD_PTR_redi_imm8(src2*4);
   }
   else {
      mov____ecx__DWORD_PTR_redi_imm32(src2*4);
   }
   if (SH(src3*4 >= -128 && src3*4 < 128)) {
      mov____edx__DWORD_PTR_redi_imm8(src3*4);
   }
   else {
      mov____edx__DWORD_PTR_redi_imm32(src3*4);
   }
   imul___eax__ebx();
   imul___ecx__edx();
   add____ecx__eax();
   mov____eax__ebx();
   mul____edx();
   add____edx__ecx();
   if (SH(dest1*4 >= -128 && dest1*4 < 128)) {
      mov____DWORD_PTR_redi_imm8__eax(dest1*4);
      mov____BYTE_PTR_resi_imm8__imm(dest1, 0);
   }
   else {
      mov____DWORD_PTR_redi_imm32__eax(dest1*4);
      mov____BYTE_PTR_resi_imm32__imm(dest1, 0);
   }
   mov____eax__edx();
   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


#if defined(JIT_X86) && !defined(JIT_X86_64)
static int64_t jit_div64(int64_t a, int64_t b) { return a / b; }
static int64_t jit_rem64(int64_t a, int64_t b) { return a % b; }
static uint64_t jit_udiv64(uint64_t a, uint64_t b) { return a / b; }
static uint64_t jit_urem64(uint64_t a, uint64_t b) { return a % b; }
#endif

static inline int jit_append_divrem64(Heap *heap, int dest1, int src1, int src2, int src3, int div, int is_signed, int pc, DynArray *error_stubs)
{
#ifdef JIT_DEBUG
   printf("   %s%s %d, acc <- %d, %d, %d, acc\n", is_signed? "" : "u", div? "div" : "rem", dest1, src1, src2, src3);
#endif
#if defined(JIT_X86)
   int ref1;
   #ifndef JIT_X86_64
   int ref2=0, ref3=0;
   #endif

   if (SH(src1*4 >= -128 && src1*4 < 128)) {
      mov____ebx__DWORD_PTR_redi_imm8(src1*4);
   }
   else {
      mov____ebx__DWORD_PTR_redi_imm32(src1*4);
   }
   if (SH(src2*4 >= -128 && src2*4 < 128)) {
      mov____ecx__DWORD_PTR_redi_imm8(src2*4);
   }
   else {
      mov____ecx__DWORD_PTR_redi_imm32(src2*4);
   }
   if (SH(src3*4 >= -128 && src3*4 < 128)) {
      mov____edx__DWORD_PTR_redi_imm8(src3*4);
   }
   else {
      mov____edx__DWORD_PTR_redi_imm32(src3*4);
   }

   #ifdef JIT_X86_64
      shl____rax__imm(32);
      or_____rdx__rax();

      mov____eax__ebx();
      shl____rcx__imm(32);
      or_____rax__rcx();

      mov____rcx__rdx();
      if (is_signed) {
         mov____rdx__rax();
         sar____rdx__imm(63);
      }
      else {
         xor____edx__edx();
      }

      cmp____rcx__imm8(0);
      je_____rel32(0);
      if (!jit_add_error_stub(heap, error_stubs, pc, JIT_ERROR_DIVISION_BY_ZERO)) return 0;

      if (is_signed) {
         cmp____rcx__imm8(-1);
         jne____rel8(0); // label 1
         ref1 = heap->jit_code_len;

         mov____r8__imm64(0x8000000000000000ULL);
         cmp____rax__r8();
         je_____rel32(0);
         if (!jit_add_error_stub(heap, error_stubs, pc, JIT_ERROR_INTEGER_OVERFLOW)) return 0;

         // label 1:
         heap->jit_code[ref1-1] = heap->jit_code_len - ref1;
      }

      if (is_signed) {
         idiv___rcx();
      }
      else {
         div____rcx();
      }

      if (!div) {
         mov____rax__rdx();
      }

      if (SH(dest1*4 >= -128 && dest1*4 < 128)) {
         mov____DWORD_PTR_redi_imm8__eax(dest1*4);
         mov____BYTE_PTR_resi_imm8__imm(dest1, 0);
      }
      else {
         mov____DWORD_PTR_redi_imm32__eax(dest1*4);
         mov____BYTE_PTR_resi_imm32__imm(dest1, 0);
      }

      shr____rax__imm(32);
      xor____ebx__ebx();
   #else
      cmp____eax__edx();
      jne____rel8(0);
      ref1 = heap->jit_code_len;

      cmp____eax__imm8(0);
      je_____rel32(0);
      if (!jit_add_error_stub(heap, error_stubs, pc, JIT_ERROR_DIVISION_BY_ZERO)) return 0;

      if (is_signed) {
         cmp____eax__imm8(-1);
         jne____rel8(0);
         ref2 = heap->jit_code_len;

         cmp____ebx__imm8(0);
         jne____rel8(0);
         ref3 = heap->jit_code_len;

         cmp____ecx__imm32(0x80000000);
         je_____rel32(0);
         if (!jit_add_error_stub(heap, error_stubs, pc, JIT_ERROR_INTEGER_OVERFLOW)) return 0;
      }

      // label 1:
      heap->jit_code[ref1-1] = heap->jit_code_len - ref1;
      if (is_signed) {
         heap->jit_code[ref2-1] = heap->jit_code_len - ref2;
         heap->jit_code[ref3-1] = heap->jit_code_len - ref3;
      }

      push___reax();
      push___redx();
      push___recx();
      push___rebx();
      if (is_signed) {
         mov____eax__imm(div? (intptr_t)jit_div64 : (intptr_t)jit_rem64);
      }
      else {
         mov____eax__imm(div? (intptr_t)jit_udiv64 : (intptr_t)jit_urem64);
      }
      call___reax();
      add____esp__imm8(0x10);

      if (SH(dest1*4 >= -128 && dest1*4 < 128)) {
         mov____DWORD_PTR_redi_imm8__eax(dest1*4);
         mov____BYTE_PTR_resi_imm8__imm(dest1, 0);
      }
      else {
         mov____DWORD_PTR_redi_imm32__eax(dest1*4);
         mov____BYTE_PTR_resi_imm32__imm(dest1, 0);
      }
      mov____eax__edx();
      xor____ebx__ebx();
   #endif
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_float(Heap *heap, int dest)
{
#ifdef JIT_DEBUG
   printf("   float %d/acc\n", dest);
#endif
#if defined(JIT_X86)
   #ifdef JIT_X86_64
      cvtsi2ss__xmm0__eax();
      movd___eax__xmm0();
   #else
      if (SH(dest*4 >= -128 && dest*4 < 128)) {
         mov____DWORD_PTR_redi_imm8__eax(dest*4);
         fild___DWORD_PTR_edi_imm8(dest*4);
         fstp___DWORD_PTR_edi_imm8(dest*4);
         mov____eax__DWORD_PTR_redi_imm8(dest*4);
      }
      else {
         mov____DWORD_PTR_redi_imm32__eax(dest*4);
         fild___DWORD_PTR_edi_imm32(dest*4);
         fstp___DWORD_PTR_edi_imm32(dest*4);
         mov____eax__DWORD_PTR_redi_imm32(dest*4);
      }
   #endif
   mov____ebx__imm(1);
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_int(Heap *heap, int dest)
{
#ifdef JIT_DEBUG
   printf("   int %d/acc\n", dest);
#endif
#if defined(JIT_X86)
   #ifdef JIT_X86_64
      movd___xmm0__eax();
      cvttss2si__eax__xmm0();
   #else
      sub____esp__imm8(4);
      fnstcw_WORD_PTR_esp();
      movzx__edx__WORD_PTR_esp();
      mov____dh__imm(0x0C);
      mov____WORD_PTR_esp_imm8__dx(2);
      fldcw__WORD_PTR_esp_imm8(2);
      if (SH(dest*4 >= -128 && dest*4 < 128)) {
         mov____DWORD_PTR_redi_imm8__eax(dest*4);
         fld____DWORD_PTR_edi_imm8(dest*4);
         fistp__DWORD_PTR_edi_imm8(dest*4);
         mov____eax__DWORD_PTR_redi_imm8(dest*4);
      }
      else {
         mov____DWORD_PTR_redi_imm32__eax(dest*4);
         fld____DWORD_PTR_edi_imm32(dest*4);
         fistp__DWORD_PTR_edi_imm32(dest*4);
         mov____eax__DWORD_PTR_redi_imm32(dest*4);
      }
      fldcw__WORD_PTR_esp_imm8(0);
      add____esp__imm8(4);
   #endif
   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_float_abs(Heap *heap)
{
   int ref1;
#ifdef JIT_DEBUG
   printf("   fabs\n");
#endif
#if defined(JIT_X86)
   and____eax__imm32(0x7FFFFFFF);
   
   test___eax__imm(0x7F800000);
   jnz____rel8(0); // label 1
   ref1 = heap->jit_code_len;
   and____eax__imm32(0xFF800000);
   heap->jit_code[ref1-1] = heap->jit_code_len - ref1;
   mov____ebx__imm(1);
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_float_minmax(Heap *heap, int src1, int src2_tmp, int min)
{
#ifdef JIT_DEBUG
   printf("   %s acc <- %d, %d/acc\n", min? "fmin" : "fmax", src1, src2_tmp);
#endif
#if defined(JIT_X86)
   int ref1;
   #ifdef JIT_X86_64
      movd___xmm0__eax();
      if (SH(src1*4 >= -128 && src1*4 < 128)) {
         if (min) {
            minss__xmm0__DWORD_PTR_rdi_imm8(src1*4);
         }
         else {
            maxss__xmm0__DWORD_PTR_rdi_imm8(src1*4);
         }
      }
      else {
         if (min) {
            minss__xmm0__DWORD_PTR_rdi_imm32(src1*4);
         }
         else {
            maxss__xmm0__DWORD_PTR_rdi_imm32(src1*4);
         }
      }
      movd___eax__xmm0();
   #else
      int src2, orig_src1 = src1;
      if (SH(src2_tmp*4 >= -128 && src2_tmp*4 < 128)) {
         mov____DWORD_PTR_redi_imm8__eax(src2_tmp*4);
      }
      else {
         mov____DWORD_PTR_redi_imm32__eax(src2_tmp*4);
      }
      xchg___ecx__eax();
      if (min) {
         src2 = src2_tmp;
      }
      else {
         src2 = src1;
         src1 = src2_tmp;
      }
      if (SH(src2*4 >= -128 && src2*4 < 128)) {
         fld____DWORD_PTR_edi_imm8(src2*4);
      }
      else {
         fld____DWORD_PTR_edi_imm32(src2*4);
      }
      if (SH(src1*4 >= -128 && src1*4 < 128)) {
         fld____DWORD_PTR_edi_imm8(src1*4);
      }
      else {
         fld____DWORD_PTR_edi_imm32(src1*4);
      }
      fucompp();
      fnstsw_ax();
      sahf___();
      xchg___ecx__eax();
      ja_____rel8(0);
      ref1 = heap->jit_code_len;

      if (SH(orig_src1*4 >= -128 && orig_src1*4 < 128)) {
         mov____eax__DWORD_PTR_redi_imm8(orig_src1*4);
      }
      else {
         mov____eax__DWORD_PTR_redi_imm32(orig_src1*4);
      }

      // label 1:
      heap->jit_code[ref1-1] = heap->jit_code_len - ref1;
   #endif

   test___eax__imm(0x7F800000);
   jnz____rel8(0); // label 1
   ref1 = heap->jit_code_len;
   and____eax__imm32(0xFF800000);
   heap->jit_code[ref1-1] = heap->jit_code_len - ref1;
   mov____ebx__imm(1);
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_float_clamp(Heap *heap, int value_slot, int min_slot, int max_slot_tmp)
{
#ifdef JIT_DEBUG
   printf("   fclamp acc <- %d, %d, %d/acc\n", value_slot, min_slot, max_slot_tmp);
#endif
#if defined(JIT_X86)
   int ref1;
   #ifdef JIT_X86_64
      movd___xmm0__eax();
      if (SH(value_slot*4 >= -128 && value_slot*4 < 128)) {
         minss__xmm0__DWORD_PTR_rdi_imm8(value_slot*4);
      }
      else {
         minss__xmm0__DWORD_PTR_rdi_imm32(value_slot*4);
      }
      if (SH(min_slot*4 >= -128 && min_slot*4 < 128)) {
         maxss__xmm0__DWORD_PTR_rdi_imm8(min_slot*4);
      }
      else {
         maxss__xmm0__DWORD_PTR_rdi_imm32(min_slot*4);
      }
      movd___eax__xmm0();
   #else
      int ref2, ref3, ref4;
      if (SH(max_slot_tmp*4 >= -128 && max_slot_tmp*4 < 128)) {
         mov____DWORD_PTR_redi_imm8__eax(max_slot_tmp*4);
      }
      else {
         mov____DWORD_PTR_redi_imm32__eax(max_slot_tmp*4);
      }
      if (SH(value_slot*4 >= -128 && value_slot*4 < 128)) {
         fld____DWORD_PTR_edi_imm8(value_slot*4);
      }
      else {
         fld____DWORD_PTR_edi_imm32(value_slot*4);
      }
      if (SH(min_slot*4 >= -128 && min_slot*4 < 128)) {
         fld____DWORD_PTR_edi_imm8(min_slot*4);
      }
      else {
         fld____DWORD_PTR_edi_imm32(min_slot*4);
      }
      fucompp();
      fnstsw_ax();
      sahf___();
      ja_____rel8(0);
      ref1 = heap->jit_code_len;

      if (SH(max_slot_tmp*4 >= -128 && max_slot_tmp*4 < 128)) {
         fld____DWORD_PTR_edi_imm8(max_slot_tmp*4);
      }
      else {
         fld____DWORD_PTR_edi_imm32(max_slot_tmp*4);
      }
      if (SH(value_slot*4 >= -128 && value_slot*4 < 128)) {
         fld____DWORD_PTR_edi_imm8(value_slot*4);
      }
      else {
         fld____DWORD_PTR_edi_imm32(value_slot*4);
      }
      fucompp();
      fnstsw_ax();
      sahf___();
      ja_____rel8(0);
      ref3 = heap->jit_code_len;

      if (SH(value_slot*4 >= -128 && value_slot*4 < 128)) {
         mov____eax__DWORD_PTR_redi_imm8(value_slot*4);
      }
      else {
         mov____eax__DWORD_PTR_redi_imm32(value_slot*4);
      }

      jmp____rel8(0);
      ref2 = heap->jit_code_len;

      // label 3:
      heap->jit_code[ref3-1] = heap->jit_code_len - ref3;

      if (SH(max_slot_tmp*4 >= -128 && max_slot_tmp*4 < 128)) {
         mov____eax__DWORD_PTR_redi_imm8(max_slot_tmp*4);
      }
      else {
         mov____eax__DWORD_PTR_redi_imm32(max_slot_tmp*4);
      }

      jmp____rel8(0);
      ref4 = heap->jit_code_len;

      // label 1:
      heap->jit_code[ref1-1] = heap->jit_code_len - ref1;

      if (SH(min_slot*4 >= -128 && min_slot*4 < 128)) {
         mov____eax__DWORD_PTR_redi_imm8(min_slot*4);
      }
      else {
         mov____eax__DWORD_PTR_redi_imm32(min_slot*4);
      }

      // label 2:
      heap->jit_code[ref2-1] = heap->jit_code_len - ref2;
      heap->jit_code[ref4-1] = heap->jit_code_len - ref4;
   #endif

   test___eax__imm(0x7F800000);
   jnz____rel8(0); // label 1
   ref1 = heap->jit_code_len;
   and____eax__imm32(0xFF800000);
   heap->jit_code[ref1-1] = heap->jit_code_len - ref1;
   mov____ebx__imm(1);
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_float_func(Heap *heap, int slot_tmp, int type)
{
#ifdef JIT_DEBUG
   printf("   ");
   switch (type) {
      case BC_EXT_FLOOR: printf("floor"); break;
      case BC_EXT_CEIL:  printf("ceil"); break;
      case BC_EXT_ROUND: printf("round"); break;
      case BC_EXT_SQRT:  printf("sqrt"); break;
      case BC_EXT_CBRT:  printf("cbrt"); break;
      case BC_EXT_EXP:   printf("exp"); break;
      case BC_EXT_LN:    printf("ln"); break;
      case BC_EXT_LOG2:  printf("log2"); break;
      case BC_EXT_LOG10: printf("log10"); break;
      case BC_EXT_SIN:   printf("sin"); break;
      case BC_EXT_COS:   printf("cos"); break;
      case BC_EXT_ASIN:  printf("asin"); break;
      case BC_EXT_ACOS:  printf("acos"); break;
      case BC_EXT_TAN:   printf("tan"); break;
      case BC_EXT_ATAN:  printf("atan"); break;
   }
   printf(" %d/acc\n", slot_tmp);
#endif
   void *func = NULL;
   switch (type) {
      case BC_EXT_FLOOR: func = floorf; break;
      case BC_EXT_CEIL:  func = ceilf; break;
      case BC_EXT_ROUND: func = roundf; break;
      case BC_EXT_SQRT:  func = sqrtf; break;
      case BC_EXT_CBRT:  func = cbrtf; break;
      case BC_EXT_EXP:   func = expf; break;
      case BC_EXT_LN:    func = logf; break;
      case BC_EXT_LOG2:  func = log2f; break;
      case BC_EXT_LOG10: func = log10f; break;
      case BC_EXT_SIN:   func = sinf; break;
      case BC_EXT_COS:   func = cosf; break;
      case BC_EXT_ASIN:  func = asinf; break;
      case BC_EXT_ACOS:  func = acosf; break;
      case BC_EXT_TAN:   func = tanf; break;
      case BC_EXT_ATAN:  func = atanf; break;
   }
#if defined(JIT_X86)
   int ref1;

   #ifdef JIT_X86_64
      #ifndef JIT_WIN64
         push___resi();
         push___redi();
      #endif
      movd___xmm0__eax();
      if (!emit_func_call(heap, func, 0)) return 0;
      if (!emit_reinit_volatile_regs(heap)) return 0;
      #ifndef JIT_WIN64
         pop____redi();
         pop____resi();
      #endif
      movd___eax__xmm0();
   #else
      push___reax();
      if (!emit_func_call(heap, func, 0x04)) return 0;
      if (SH(slot_tmp*4 >= -128 && slot_tmp*4 < 128)) {
         fstp___DWORD_PTR_edi_imm8(slot_tmp*4);
         mov____eax__DWORD_PTR_redi_imm8(slot_tmp*4);
      }
      else {
         fstp___DWORD_PTR_edi_imm32(slot_tmp*4);
         mov____eax__DWORD_PTR_redi_imm32(slot_tmp*4);
      }
   #endif

   test___eax__imm(0x7F800000);
   jnz____rel8(0); // label 1
   ref1 = heap->jit_code_len;

   and____eax__imm32(0xFF800000);

   // label 1:
   heap->jit_code[ref1-1] = heap->jit_code_len - ref1;

   mov____ebx__imm(1);
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_float_func2(Heap *heap, int dest_tmp, int src1, int type)
{
#ifdef JIT_DEBUG
   printf("   ");
   switch (type) {
      case BC_EXT_POW:   printf("pow"); break;
      case BC_EXT_ATAN2: printf("atan2"); break;
   }
   printf(" %d/acc <- %d, acc\n", dest_tmp, src1);
#endif
   void *func = NULL;
   switch (type) {
      case BC_EXT_POW:   func = powf; break;
      case BC_EXT_ATAN2: func = atan2f; break;
   }
#if defined(JIT_X86)
   int ref1;

   #ifdef JIT_X86_64
      #ifndef JIT_WIN64
         push___resi();
         push___redi();
      #endif
      if (SH(src1*4 >= -128 && src1*4 < 128)) {
         movd___xmm0__DWORD_PTR_rdi_imm8(src1*4);
      }
      else {
         movd___xmm0__DWORD_PTR_rdi_imm32(src1*4);
      }
      movd___xmm1__eax();
      if (!emit_func_call(heap, func, 0)) return 0;
      if (!emit_reinit_volatile_regs(heap)) return 0;
      #ifndef JIT_WIN64
         pop____redi();
         pop____resi();
      #endif
      movd___eax__xmm0();
   #else
      push___reax();
      if (SH(src1*4 >= -128 && src1*4 < 128)) {
         push___DWORD_PTR_edi_imm8(src1*4);
      }
      else {
         push___DWORD_PTR_edi_imm32(src1*4);
      }
      if (!emit_func_call(heap, func, 0x08)) return 0;
      if (SH(dest_tmp*4 >= -128 && dest_tmp*4 < 128)) {
         fstp___DWORD_PTR_edi_imm8(dest_tmp*4);
         mov____eax__DWORD_PTR_redi_imm8(dest_tmp*4);
      }
      else {
         fstp___DWORD_PTR_edi_imm32(dest_tmp*4);
         mov____eax__DWORD_PTR_redi_imm32(dest_tmp*4);
      }
   #endif

   test___eax__imm(0x7F800000);
   jnz____rel8(0); // label 1
   ref1 = heap->jit_code_len;

   and____eax__imm32(0xFF800000);

   // label 1:
   heap->jit_code[ref1-1] = heap->jit_code_len - ref1;

   mov____ebx__imm(1);
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_double(Heap *heap, int dest1, int src1)
{
#ifdef JIT_DEBUG
   printf("   double %d, acc <- %d, acc\n", dest1, src1);
#endif
#if defined(JIT_X86)
   #ifdef JIT_X86_64
      if (SH(src1*4 >= -128 && src1*4 < 128)) {
         mov____ecx__DWORD_PTR_redi_imm8(src1*4);
      }
      else {
         mov____ecx__DWORD_PTR_redi_imm32(src1*4);
      }
      shl____rax__imm(32);
      or_____rax__rcx();
      cvtsi2sd__xmm0__rax();
      movq___rax__xmm0();
      if (SH(dest1*4 >= -128 && dest1*4 < 128)) {
         mov____DWORD_PTR_redi_imm8__eax(dest1*4);
         mov____BYTE_PTR_resi_imm8__imm(dest1, 0);
      }
      else {
         mov____DWORD_PTR_redi_imm32__eax(dest1*4);
         mov____BYTE_PTR_resi_imm32__imm(dest1, 0);
      }
      shr____rax__imm(32);
   #else
      if (SH((dest1+0)*4 >= -128 && (dest1+1)*4 < 128)) {
         mov____DWORD_PTR_redi_imm8__eax((dest1+1)*4);
         fild___QWORD_PTR_edi_imm8((dest1+0)*4);
         fstp___QWORD_PTR_edi_imm8((dest1+0)*4);
         mov____eax__DWORD_PTR_redi_imm8((dest1+1)*4);
         mov____BYTE_PTR_resi_imm8__imm((dest1+0), 0);
      }
      else {
         mov____DWORD_PTR_redi_imm32__eax((dest1+1)*4);
         fild___QWORD_PTR_edi_imm32((dest1+0)*4);
         fstp___QWORD_PTR_edi_imm32((dest1+0)*4);
         mov____eax__DWORD_PTR_redi_imm32((dest1+1)*4);
         mov____BYTE_PTR_resi_imm32__imm((dest1+0), 0);
      }
   #endif
   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_long(Heap *heap, int dest1, int src1)
{
#ifdef JIT_DEBUG
   printf("   long %d, acc <- %d, acc\n", dest1, src1);
#endif
#if defined(JIT_X86)
   #ifdef JIT_X86_64
      if (SH(src1*4 >= -128 && src1*4 < 128)) {
         mov____ecx__DWORD_PTR_redi_imm8(src1*4);
      }
      else {
         mov____ecx__DWORD_PTR_redi_imm32(src1*4);
      }
      shl____rax__imm(32);
      or_____rax__rcx();
      movq___xmm0__rax();
      cvttsd2si__rax__xmm0();
      if (SH(dest1*4 >= -128 && dest1*4 < 128)) {
         mov____DWORD_PTR_redi_imm8__eax(dest1*4);
         mov____BYTE_PTR_resi_imm8__imm(dest1, 0);
      }
      else {
         mov____DWORD_PTR_redi_imm32__eax(dest1*4);
         mov____BYTE_PTR_resi_imm32__imm(dest1, 0);
      }
      shr____rax__imm(32);
   #else
      sub____esp__imm8(4);
      fnstcw_WORD_PTR_esp();
      movzx__edx__WORD_PTR_esp();
      mov____dh__imm(0x0C);
      mov____WORD_PTR_esp_imm8__dx(2);
      fldcw__WORD_PTR_esp_imm8(2);
      if (SH((dest1+0)*4 >= -128 && (dest1+1)*4 < 128)) {
         mov____DWORD_PTR_redi_imm8__eax((dest1+1)*4);
         fld____QWORD_PTR_edi_imm8((dest1+0)*4);
         fistp__QWORD_PTR_edi_imm8((dest1+0)*4);
         mov____eax__DWORD_PTR_redi_imm8((dest1+1)*4);
         mov____BYTE_PTR_resi_imm8__imm((dest1+0), 0);
      }
      else {
         mov____DWORD_PTR_redi_imm32__eax((dest1+1)*4);
         fld____QWORD_PTR_edi_imm32((dest1+0)*4);
         fistp__QWORD_PTR_edi_imm32((dest1+0)*4);
         mov____eax__DWORD_PTR_redi_imm32((dest1+1)*4);
         mov____BYTE_PTR_resi_imm32__imm((dest1+0), 0);
      }
      fldcw__WORD_PTR_esp_imm8(0);
      add____esp__imm8(4);
   #endif
   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_double_conv_down(Heap *heap, int src1)
{
#ifdef JIT_DEBUG
   printf("   double_conv_down acc <- %d, acc\n", src1);
#endif
#if defined(JIT_X86)
   int ref1;

   #ifdef JIT_X86_64
      if (SH(src1*4 >= -128 && src1*4 < 128)) {
         mov____ecx__DWORD_PTR_redi_imm8(src1*4);
      }
      else {
         mov____ecx__DWORD_PTR_redi_imm32(src1*4);
      }
      shl____rax__imm(32);
      or_____rax__rcx();
      movq___xmm0__rax();
      cvtsd2ss__xmm0__xmm0();
      movd___eax__xmm0();
   #else
      if (SH((src1+0)*4 >= -128 && (src1+1)*4 < 128)) {
         mov____DWORD_PTR_redi_imm8__eax((src1+1)*4);
         fld____QWORD_PTR_edi_imm8((src1+0)*4);
         fstp___DWORD_PTR_edi_imm8((src1+0)*4);
         mov____eax__DWORD_PTR_redi_imm8((src1+0)*4);
      }
      else {
         mov____DWORD_PTR_redi_imm32__eax((src1+1)*4);
         fld____QWORD_PTR_edi_imm32((src1+0)*4);
         fstp___DWORD_PTR_edi_imm32((src1+0)*4);
         mov____eax__DWORD_PTR_redi_imm32((src1+0)*4);
      }
   #endif

   test___eax__imm(0x7F800000);
   jnz____rel8(0); // label 1
   ref1 = heap->jit_code_len;

   and____eax__imm32(0xFF800000);

   // label 1:
   heap->jit_code[ref1-1] = heap->jit_code_len - ref1;

   mov____ebx__imm(1);
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_double_conv_up(Heap *heap, int dest1)
{
#ifdef JIT_DEBUG
   printf("   double_conv_up %d, acc <- acc\n", dest1);
#endif
#if defined(JIT_X86)
   #ifdef JIT_X86_64
      movd___xmm0__eax();
      cvtss2sd__xmm0__xmm0();
      movq___rax__xmm0();
      if (SH(dest1*4 >= -128 && dest1*4 < 128)) {
         mov____DWORD_PTR_redi_imm8__eax(dest1*4);
         mov____BYTE_PTR_resi_imm8__imm(dest1, 0);
      }
      else {
         mov____DWORD_PTR_redi_imm32__eax(dest1*4);
         mov____BYTE_PTR_resi_imm32__imm(dest1, 0);
      }
      shr____rax__imm(32);
   #else
      if (SH((dest1+0)*4 >= -128 && (dest1+1)*4 < 128)) {
         mov____DWORD_PTR_redi_imm8__eax((dest1+0)*4);
         fld____DWORD_PTR_edi_imm8((dest1+0)*4);
         fstp___QWORD_PTR_edi_imm8((dest1+0)*4);
         mov____eax__DWORD_PTR_redi_imm8((dest1+1)*4);
         mov____BYTE_PTR_resi_imm8__imm((dest1+0), 0);
      }
      else {
         mov____DWORD_PTR_redi_imm32__eax((dest1+0)*4);
         fld____DWORD_PTR_edi_imm32((dest1+0)*4);
         fstp___QWORD_PTR_edi_imm32((dest1+0)*4);
         mov____eax__DWORD_PTR_redi_imm32((dest1+1)*4);
         mov____BYTE_PTR_resi_imm32__imm((dest1+0), 0);
      }
   #endif
   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_double_op(Heap *heap, int dest1, int src1, int src2, int src3, int type)
{
#ifdef JIT_DEBUG
   printf("   d%s %d, acc <- %d, %d, %d, acc\n", type == BC_EXT_DBL_ADD? "add" : type == BC_EXT_DBL_SUB? "sub" : type == BC_EXT_DBL_MUL? "mul" : "div", dest1, src1, src2, src3);
#endif
#if defined(JIT_X86)
   #ifdef JIT_X86_64
      if (SH(src3*4 >= -128 && src3*4 < 128)) {
         mov____ecx__DWORD_PTR_redi_imm8(src3*4);
      }
      else {
         mov____ecx__DWORD_PTR_redi_imm32(src3*4);
      }
      shl____rax__imm(32);
      or_____rax__rcx();
      movq___xmm1__rax();

      if (src2 == src1+1) {
         if (SH(src1*4 >= -128 && src1*4 < 128)) {
            movq___xmm0__QWORD_PTR_rdi_imm8(src1*4);
         }
         else {
            movq___xmm0__QWORD_PTR_rdi_imm32(src1*4);
         }
      }
      else {
         if (SH(src1*4 >= -128 && src1*4 < 128)) {
            mov____ecx__DWORD_PTR_redi_imm8(src1*4);
         }
         else {
            mov____ecx__DWORD_PTR_redi_imm32(src1*4);
         }
         if (SH(src2*4 >= -128 && src2*4 < 128)) {
            mov____eax__DWORD_PTR_redi_imm8(src2*4);
         }
         else {
            mov____eax__DWORD_PTR_redi_imm32(src2*4);
         }
         shl____rax__imm(32);
         or_____rax__rcx();
         movq___xmm0__rax();
      }
      switch (type) {
         case BC_EXT_DBL_ADD: addsd__xmm0__xmm1(); break;
         case BC_EXT_DBL_SUB: subsd__xmm0__xmm1(); break;
         case BC_EXT_DBL_MUL: mulsd__xmm0__xmm1(); break;
         case BC_EXT_DBL_DIV: divsd__xmm0__xmm1(); break;
      }
      movq___rax__xmm0();
      if (SH(dest1*4 >= -128 && dest1*4 < 128)) {
         mov____DWORD_PTR_redi_imm8__eax(dest1*4);
         mov____BYTE_PTR_resi_imm8__imm(dest1, 0);
      }
      else {
         mov____DWORD_PTR_redi_imm32__eax(dest1*4);
         mov____BYTE_PTR_resi_imm32__imm(dest1, 0);
      }
      shr____rax__imm(32);
   #else
      if (SH((dest1+0)*4 >= -128 && (dest1+3)*4 < 128)) {
         mov____DWORD_PTR_redi_imm8__eax((dest1+3)*4);
         fld____QWORD_PTR_edi_imm8((dest1+0)*4);
         switch (type) {
            case BC_EXT_DBL_ADD: fadd___QWORD_PTR_edi_imm8((dest1+2)*4); break;
            case BC_EXT_DBL_SUB: fsub___QWORD_PTR_edi_imm8((dest1+2)*4); break;
            case BC_EXT_DBL_MUL: fmul___QWORD_PTR_edi_imm8((dest1+2)*4); break;
            case BC_EXT_DBL_DIV: fdiv___QWORD_PTR_edi_imm8((dest1+2)*4); break;
         }
         fstp___QWORD_PTR_edi_imm8((dest1+0)*4);
         mov____eax__DWORD_PTR_redi_imm8((dest1+1)*4);
         mov____BYTE_PTR_resi_imm8__imm((dest1+0), 0);
      }
      else {
         mov____DWORD_PTR_redi_imm32__eax((dest1+3)*4);
         fld____QWORD_PTR_edi_imm32((dest1+0)*4);
         switch (type) {
            case BC_EXT_DBL_ADD: fadd___QWORD_PTR_edi_imm32((dest1+2)*4); break;
            case BC_EXT_DBL_SUB: fsub___QWORD_PTR_edi_imm32((dest1+2)*4); break;
            case BC_EXT_DBL_MUL: fmul___QWORD_PTR_edi_imm32((dest1+2)*4); break;
            case BC_EXT_DBL_DIV: fdiv___QWORD_PTR_edi_imm32((dest1+2)*4); break;
         }
         fstp___QWORD_PTR_edi_imm32((dest1+0)*4);
         mov____eax__DWORD_PTR_redi_imm32((dest1+1)*4);
         mov____BYTE_PTR_resi_imm32__imm((dest1+0), 0);
      }
   #endif
   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_double_cmp(Heap *heap, int src1, int src2, int src3, int type)
{
#ifdef JIT_DEBUG
   printf("   dcmp%s acc <- %d, %d, %d, acc\n", type == BC_EXT_DBL_CMP_LT? ".lt" : type == BC_EXT_DBL_CMP_LE? ".le" : type == BC_EXT_DBL_CMP_GT? ".gt" : type == BC_EXT_DBL_CMP_GE? ".ge" : type == BC_EXT_DBL_CMP_EQ? ".eq" : ".ne", src1, src2, src3);
#endif
#if defined(JIT_X86)
   #ifdef JIT_X86_64
      int imm = 0;
      if (SH(src3*4 >= -128 && src3*4 < 128)) {
         mov____ecx__DWORD_PTR_redi_imm8(src3*4);
      }
      else {
         mov____ecx__DWORD_PTR_redi_imm32(src3*4);
      }
      shl____rax__imm(32);
      or_____rax__rcx();
      movq___xmm1__rax();

      if (src2 == src1+1) {
         if (SH(src1*4 >= -128 && src1*4 < 128)) {
            movq___xmm0__QWORD_PTR_rdi_imm8(src1*4);
         }
         else {
            movq___xmm0__QWORD_PTR_rdi_imm32(src1*4);
         }
      }
      else {
         if (SH(src1*4 >= -128 && src1*4 < 128)) {
            mov____ecx__DWORD_PTR_redi_imm8(src1*4);
         }
         else {
            mov____ecx__DWORD_PTR_redi_imm32(src1*4);
         }
         if (SH(src2*4 >= -128 && src2*4 < 128)) {
            mov____eax__DWORD_PTR_redi_imm8(src2*4);
         }
         else {
            mov____eax__DWORD_PTR_redi_imm32(src2*4);
         }
         shl____rax__imm(32);
         or_____rax__rcx();
         movq___xmm0__rax();
      }

      if (type == BC_EXT_DBL_CMP_EQ || type == BC_EXT_DBL_CMP_NE || type == BC_EXT_DBL_CMP_GT || type == BC_EXT_DBL_CMP_GE) {
         switch (type) {
            case BC_EXT_DBL_CMP_EQ: imm = 0; break;
            case BC_EXT_DBL_CMP_NE: imm = 4; break;
            case BC_EXT_DBL_CMP_GT: imm = 1; break;
            case BC_EXT_DBL_CMP_GE: imm = 2; break;
         }
         cmpsd__xmm1__xmm0(imm);
         movd___eax__xmm1();
      }
      else {
         switch (type) {
            case BC_EXT_DBL_CMP_LT: imm = 1; break;
            case BC_EXT_DBL_CMP_LE: imm = 2; break;
         }
         cmpsd__xmm0__xmm1(imm);
         movd___eax__xmm0();
      }
      and____eax__imm8(1);
   #else
      if (SH((src1+0)*4 >= -128 && (src1+3)*4 < 128)) {
         mov____DWORD_PTR_redi_imm8__eax((src1+3)*4);
         fld____QWORD_PTR_edi_imm8((src1+2)*4);
         fld____QWORD_PTR_edi_imm8((src1+0)*4);
      }
      else {
         mov____DWORD_PTR_redi_imm32__eax((src1+3)*4);
         fld____QWORD_PTR_edi_imm32((src1+2)*4);
         fld____QWORD_PTR_edi_imm32((src1+0)*4);
      }
      fucompp();
      fnstsw_ax();
      sahf___();
      switch (type) {
         case BC_EXT_DBL_CMP_LT: setb___dl(); setne__cl(); and____dl__cl(); break;
         case BC_EXT_DBL_CMP_LE: setnp__dl(); seta___cl(); xor____cl__imm(1); and____dl__cl(); break;
         case BC_EXT_DBL_CMP_GT: seta___dl(); break;
         case BC_EXT_DBL_CMP_GE: setnp__dl(); setae__cl(); and____dl__cl(); break;
         case BC_EXT_DBL_CMP_EQ: sete___dl(); setae__cl(); and____dl__cl(); break;
         case BC_EXT_DBL_CMP_NE: sete___dl(); setae__cl(); and____dl__cl(); xor____dl__imm(1); break;
      }
      movzx__eax__dl();
   #endif
   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_double_abs(Heap *heap, int dest1, int src1)
{
#ifdef JIT_DEBUG
   printf("   dabs %d, acc <- %d, acc\n", dest1, src1);
#endif
#if defined(JIT_X86)
   if (SH(dest1*4 >= -128 && dest1*4 < 128 && src1*4 >= -128 && src1*4 < 128)) {
      if (dest1 != src1) {
         mov____edx__DWORD_PTR_redi_imm8(src1*4);
         mov____DWORD_PTR_redi_imm8__edx(dest1*4);
      }
      mov____BYTE_PTR_resi_imm8__imm(dest1, 0);
   }
   else {
      if (dest1 != src1) {
         mov____edx__DWORD_PTR_redi_imm32(src1*4);
         mov____DWORD_PTR_redi_imm32__edx(dest1*4);
      }
      mov____BYTE_PTR_resi_imm32__imm(dest1, 0);
   }
   and____eax__imm32(0x7FFFFFFF);
   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_double_minmax(Heap *heap, int dest1, int src1, int src2, int src3, int min)
{
#ifdef JIT_DEBUG
   printf("   %s %d, acc <- %d, %d, %d, acc\n", min? "dmin" : "dmax", dest1, src1, src2, src3);
#endif
#if defined(JIT_X86)
   #ifdef JIT_X86_64
      if (SH(src3*4 >= -128 && src3*4 < 128)) {
         mov____ecx__DWORD_PTR_redi_imm8(src3*4);
      }
      else {
         mov____ecx__DWORD_PTR_redi_imm32(src3*4);
      }
      shl____rax__imm(32);
      or_____rax__rcx();
      movq___xmm1__rax();

      if (src2 == src1+1) {
         if (SH(src1*4 >= -128 && src1*4 < 128)) {
            movq___xmm0__QWORD_PTR_rdi_imm8(src1*4);
         }
         else {
            movq___xmm0__QWORD_PTR_rdi_imm32(src1*4);
         }
      }
      else {
         if (SH(src1*4 >= -128 && src1*4 < 128)) {
            mov____ecx__DWORD_PTR_redi_imm8(src1*4);
         }
         else {
            mov____ecx__DWORD_PTR_redi_imm32(src1*4);
         }
         if (SH(src2*4 >= -128 && src2*4 < 128)) {
            mov____eax__DWORD_PTR_redi_imm8(src2*4);
         }
         else {
            mov____eax__DWORD_PTR_redi_imm32(src2*4);
         }
         shl____rax__imm(32);
         or_____rax__rcx();
         movq___xmm0__rax();
      }
      
      if (min) {
         minsd__xmm0__xmm1();
      }
      else {
         maxsd__xmm0__xmm1();
      }

      movq___rax__xmm0();
      if (SH(dest1*4 >= -128 && dest1*4 < 128)) {
         mov____DWORD_PTR_redi_imm8__eax(dest1*4);
         mov____BYTE_PTR_resi_imm8__imm(dest1, 0);
      }
      else {
         mov____DWORD_PTR_redi_imm32__eax(dest1*4);
         mov____BYTE_PTR_resi_imm32__imm(dest1, 0);
      }
      shr____rax__imm(32);
   #else
      int ref1, ref2;

      if (SH((src1+0)*4 >= -128 && (src1+3)*4 < 128)) {
         mov____DWORD_PTR_redi_imm8__eax((src1+3)*4);
         if (min) {
            fld____QWORD_PTR_edi_imm8((src1+2)*4);
            fld____QWORD_PTR_edi_imm8((src1+0)*4);
         }
         else {
            fld____QWORD_PTR_edi_imm8((src1+0)*4);
            fld____QWORD_PTR_edi_imm8((src1+2)*4);
         }
      }
      else {
         mov____DWORD_PTR_redi_imm32__eax((src1+3)*4);
         if (min) {
            fld____QWORD_PTR_edi_imm32((src1+2)*4);
            fld____QWORD_PTR_edi_imm32((src1+0)*4);
         }
         else {
            fld____QWORD_PTR_edi_imm32((src1+0)*4);
            fld____QWORD_PTR_edi_imm32((src1+2)*4);
         }
      }
      fucompp();
      fnstsw_ax();
      sahf___();
      ja_____rel8(0);
      ref1 = heap->jit_code_len;

      if (SH((src1+0)*4 >= -128 && (src1+3)*4 < 128)) {
         mov____eax__DWORD_PTR_redi_imm8((src1+1)*4);
         mov____BYTE_PTR_resi_imm8__imm((src1+0), 0);
      }
      else {
         mov____eax__DWORD_PTR_redi_imm32((src1+1)*4);
         mov____BYTE_PTR_resi_imm32__imm((src1+0), 0);
      }

      jmp____rel8(0);
      ref2 = heap->jit_code_len;

      // label 1:
      heap->jit_code[ref1-1] = heap->jit_code_len - ref1;

      if (SH((src1+0)*4 >= -128 && (src1+3)*4 < 128)) {
         mov____eax__DWORD_PTR_redi_imm8((src1+2)*4);
         mov____DWORD_PTR_redi_imm8__eax((src1+0)*4);
         mov____eax__DWORD_PTR_redi_imm8((src1+3)*4);
         mov____BYTE_PTR_resi_imm8__imm((src1+0), 0);
      }
      else {
         mov____eax__DWORD_PTR_redi_imm32((src1+2)*4);
         mov____DWORD_PTR_redi_imm32__eax((src1+0)*4);
         mov____eax__DWORD_PTR_redi_imm32((src1+3)*4);
         mov____BYTE_PTR_resi_imm32__imm((src1+0), 0);
      }

      // label 2:
      heap->jit_code[ref2-1] = heap->jit_code_len - ref2;
   #endif
   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_double_clamp(Heap *heap, int dest1, int src1, int src2, int src3, int src4, int src5)
{
#ifdef JIT_DEBUG
   printf("   dclamp %d, acc <- %d, %d, %d, %d, %d, acc\n", dest1, src1, src2, src3, src4, src5);
#endif
#if defined(JIT_X86)
   #ifdef JIT_X86_64
      if (SH(src5*4 >= -128 && src5*4 < 128)) {
         mov____ecx__DWORD_PTR_redi_imm8(src5*4);
      }
      else {
         mov____ecx__DWORD_PTR_redi_imm32(src5*4);
      }
      shl____rax__imm(32);
      or_____rax__rcx();
      movq___xmm2__rax();

      if (src4 == src3+1) {
         if (SH(src3*4 >= -128 && src3*4 < 128)) {
            movq___xmm1__QWORD_PTR_rdi_imm8(src3*4);
         }
         else {
            movq___xmm1__QWORD_PTR_rdi_imm32(src3*4);
         }
      }
      else {
         if (SH(src3*4 >= -128 && src3*4 < 128)) {
            mov____ecx__DWORD_PTR_redi_imm8(src3*4);
         }
         else {
            mov____ecx__DWORD_PTR_redi_imm32(src3*4);
         }
         if (SH(src4*4 >= -128 && src4*4 < 128)) {
            mov____eax__DWORD_PTR_redi_imm8(src4*4);
         }
         else {
            mov____eax__DWORD_PTR_redi_imm32(src4*4);
         }
         shl____rax__imm(32);
         or_____rax__rcx();
         movq___xmm1__rax();
      }

      if (src2 == src1+1) {
         if (SH(src1*4 >= -128 && src1*4 < 128)) {
            movq___xmm0__QWORD_PTR_rdi_imm8(src1*4);
         }
         else {
            movq___xmm0__QWORD_PTR_rdi_imm32(src1*4);
         }
      }
      else {
         if (SH(src1*4 >= -128 && src1*4 < 128)) {
            mov____ecx__DWORD_PTR_redi_imm8(src1*4);
         }
         else {
            mov____ecx__DWORD_PTR_redi_imm32(src1*4);
         }
         if (SH(src2*4 >= -128 && src2*4 < 128)) {
            mov____eax__DWORD_PTR_redi_imm8(src2*4);
         }
         else {
            mov____eax__DWORD_PTR_redi_imm32(src2*4);
         }
         shl____rax__imm(32);
         or_____rax__rcx();
         movq___xmm0__rax();
      }
      
      maxsd__xmm0__xmm1();
      minsd__xmm0__xmm2();

      movq___rax__xmm0();
      if (SH(dest1*4 >= -128 && dest1*4 < 128)) {
         mov____DWORD_PTR_redi_imm8__eax(dest1*4);
         mov____BYTE_PTR_resi_imm8__imm(dest1, 0);
      }
      else {
         mov____DWORD_PTR_redi_imm32__eax(dest1*4);
         mov____BYTE_PTR_resi_imm32__imm(dest1, 0);
      }
      shr____rax__imm(32);
   #else
      int ref1, ref2, ref3, ref4;

      if (SH((src1+0)*4 >= -128 && (src1+5)*4 < 128)) {
         mov____DWORD_PTR_redi_imm8__eax((src1+5)*4);
         fld____QWORD_PTR_edi_imm8((src1+0)*4);
         fld____QWORD_PTR_edi_imm8((src1+2)*4);
      }
      else {
         mov____DWORD_PTR_redi_imm32__eax((src1+5)*4);
         fld____QWORD_PTR_edi_imm32((src1+0)*4);
         fld____QWORD_PTR_edi_imm32((src1+2)*4);
      }
      fucompp();
      fnstsw_ax();
      sahf___();
      ja_____rel8(0);
      ref1 = heap->jit_code_len;

      if (SH((src1+0)*4 >= -128 && (src1+5)*4 < 128)) {
         fld____QWORD_PTR_edi_imm8((src1+4)*4);
         fld____QWORD_PTR_edi_imm8((src1+0)*4);
      }
      else {
         fld____QWORD_PTR_edi_imm32((src1+4)*4);
         fld____QWORD_PTR_edi_imm32((src1+0)*4);
      }
      fucompp();
      fnstsw_ax();
      sahf___();
      ja_____rel8(0);
      ref3 = heap->jit_code_len;

      if (SH((src1+0)*4 >= -128 && (src1+5)*4 < 128)) {
         mov____eax__DWORD_PTR_redi_imm8((src1+1)*4);
         mov____BYTE_PTR_resi_imm8__imm((src1+0), 0);
      }
      else {
         mov____eax__DWORD_PTR_redi_imm32((src1+1)*4);
         mov____BYTE_PTR_resi_imm32__imm((src1+0), 0);
      }

      jmp____rel8(0);
      ref2 = heap->jit_code_len;

      // label 3:
      heap->jit_code[ref3-1] = heap->jit_code_len - ref3;

      if (SH((src1+0)*4 >= -128 && (src1+5)*4 < 128)) {
         mov____eax__DWORD_PTR_redi_imm8((src1+4)*4);
         mov____DWORD_PTR_redi_imm8__eax((src1+0)*4);
         mov____eax__DWORD_PTR_redi_imm8((src1+5)*4);
         mov____BYTE_PTR_resi_imm8__imm((src1+0), 0);
      }
      else {
         mov____eax__DWORD_PTR_redi_imm32((src1+4)*4);
         mov____DWORD_PTR_redi_imm32__eax((src1+0)*4);
         mov____eax__DWORD_PTR_redi_imm32((src1+5)*4);
         mov____BYTE_PTR_resi_imm32__imm((src1+0), 0);
      }

      jmp____rel8(0);
      ref4 = heap->jit_code_len;

      // label 1:
      heap->jit_code[ref1-1] = heap->jit_code_len - ref1;

      if (SH((src1+0)*4 >= -128 && (src1+5)*4 < 128)) {
         mov____eax__DWORD_PTR_redi_imm8((src1+2)*4);
         mov____DWORD_PTR_redi_imm8__eax((src1+0)*4);
         mov____eax__DWORD_PTR_redi_imm8((src1+3)*4);
         mov____BYTE_PTR_resi_imm8__imm((src1+0), 0);
      }
      else {
         mov____eax__DWORD_PTR_redi_imm32((src1+2)*4);
         mov____DWORD_PTR_redi_imm32__eax((src1+0)*4);
         mov____eax__DWORD_PTR_redi_imm32((src1+3)*4);
         mov____BYTE_PTR_resi_imm32__imm((src1+0), 0);
      }

      // label 2:
      heap->jit_code[ref2-1] = heap->jit_code_len - ref2;
      heap->jit_code[ref4-1] = heap->jit_code_len - ref4;
   #endif
   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_double_clamp_short(Heap *heap, int dest1, int src1, int src2, int src3)
{
#ifdef JIT_DEBUG
   printf("   dclamp %d, acc <- %d, %d, %d, acc\n", dest1, src1, src2, src3);
#endif
#if defined(JIT_X86)
   #ifdef JIT_X86_64
      movd___xmm2__eax();
      if (SH(src3*4 >= -128 && src3*4 < 128)) {
         movd___xmm1__DWORD_PTR_rdi_imm8(src3*4);
      }
      else {
         movd___xmm1__DWORD_PTR_rdi_imm32(src3*4);
      }
      cvtss2sd__xmm1__xmm1();
      cvtss2sd__xmm2__xmm2();

      if (src2 == src1+1) {
         if (SH(src1*4 >= -128 && src1*4 < 128)) {
            movq___xmm0__QWORD_PTR_rdi_imm8(src1*4);
         }
         else {
            movq___xmm0__QWORD_PTR_rdi_imm32(src1*4);
         }
      }
      else {
         if (SH(src1*4 >= -128 && src1*4 < 128)) {
            mov____ecx__DWORD_PTR_redi_imm8(src1*4);
         }
         else {
            mov____ecx__DWORD_PTR_redi_imm32(src1*4);
         }
         if (SH(src2*4 >= -128 && src2*4 < 128)) {
            mov____eax__DWORD_PTR_redi_imm8(src2*4);
         }
         else {
            mov____eax__DWORD_PTR_redi_imm32(src2*4);
         }
         shl____rax__imm(32);
         or_____rax__rcx();
         movq___xmm0__rax();
      }
      
      maxsd__xmm0__xmm1();
      minsd__xmm0__xmm2();

      movq___rax__xmm0();
      if (SH(dest1*4 >= -128 && dest1*4 < 128)) {
         mov____DWORD_PTR_redi_imm8__eax(dest1*4);
         mov____BYTE_PTR_resi_imm8__imm(dest1, 0);
      }
      else {
         mov____DWORD_PTR_redi_imm32__eax(dest1*4);
         mov____BYTE_PTR_resi_imm32__imm(dest1, 0);
      }
      shr____rax__imm(32);
   #else
      int ref1, ref2, ref3, ref4;

      if (SH((src1+0)*4 >= -128 && (src1+3)*4 < 128)) {
         mov____DWORD_PTR_redi_imm8__eax((src1+3)*4);
         fld____QWORD_PTR_edi_imm8((src1+0)*4);
         fld____DWORD_PTR_edi_imm8((src1+2)*4);
      }
      else {
         mov____DWORD_PTR_redi_imm32__eax((src1+3)*4);
         fld____QWORD_PTR_edi_imm32((src1+0)*4);
         fld____DWORD_PTR_edi_imm32((src1+2)*4);
      }
      fucompp();
      fnstsw_ax();
      sahf___();
      ja_____rel8(0);
      ref1 = heap->jit_code_len;

      if (SH((src1+0)*4 >= -128 && (src1+3)*4 < 128)) {
         fld____DWORD_PTR_edi_imm8((src1+3)*4);
         fld____QWORD_PTR_edi_imm8((src1+0)*4);
      }
      else {
         fld____DWORD_PTR_edi_imm32((src1+3)*4);
         fld____QWORD_PTR_edi_imm32((src1+0)*4);
      }
      fucompp();
      fnstsw_ax();
      sahf___();
      ja_____rel8(0);
      ref3 = heap->jit_code_len;

      if (SH((src1+0)*4 >= -128 && (src1+3)*4 < 128)) {
         mov____eax__DWORD_PTR_redi_imm8((src1+1)*4);
         mov____BYTE_PTR_resi_imm8__imm((src1+0), 0);
      }
      else {
         mov____eax__DWORD_PTR_redi_imm32((src1+1)*4);
         mov____BYTE_PTR_resi_imm32__imm((src1+0), 0);
      }

      jmp____rel8(0);
      ref2 = heap->jit_code_len;

      // label 3:
      heap->jit_code[ref3-1] = heap->jit_code_len - ref3;

      if (SH((src1+0)*4 >= -128 && (src1+3)*4 < 128)) {
         fld____DWORD_PTR_edi_imm8((src1+3)*4);
         fstp___QWORD_PTR_edi_imm8((src1+0)*4);
         mov____eax__DWORD_PTR_redi_imm8((src1+1)*4);
         mov____BYTE_PTR_resi_imm8__imm((src1+0), 0);
      }
      else {
         fld____DWORD_PTR_edi_imm32((src1+3)*4);
         fstp___QWORD_PTR_edi_imm32((src1+0)*4);
         mov____eax__DWORD_PTR_redi_imm32((src1+1)*4);
         mov____BYTE_PTR_resi_imm32__imm((src1+0), 0);
      }

      jmp____rel8(0);
      ref4 = heap->jit_code_len;

      // label 1:
      heap->jit_code[ref1-1] = heap->jit_code_len - ref1;

      if (SH((src1+0)*4 >= -128 && (src1+3)*4 < 128)) {
         fld____DWORD_PTR_edi_imm8((src1+2)*4);
         fstp___QWORD_PTR_edi_imm8((src1+0)*4);
         mov____eax__DWORD_PTR_redi_imm8((src1+1)*4);
         mov____BYTE_PTR_resi_imm8__imm((src1+0), 0);
      }
      else {
         fld____DWORD_PTR_edi_imm32((src1+2)*4);
         fstp___QWORD_PTR_edi_imm32((src1+0)*4);
         mov____eax__DWORD_PTR_redi_imm32((src1+1)*4);
         mov____BYTE_PTR_resi_imm32__imm((src1+0), 0);
      }

      // label 2:
      heap->jit_code[ref2-1] = heap->jit_code_len - ref2;
      heap->jit_code[ref4-1] = heap->jit_code_len - ref4;
   #endif
   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_double_func(Heap *heap, int dest1, int src1, int type)
{
#ifdef JIT_DEBUG
   printf("   ");
   switch (type) {
      case BC_EXT_DBL_FLOOR: printf("dfloor"); break;
      case BC_EXT_DBL_CEIL:  printf("dceil"); break;
      case BC_EXT_DBL_ROUND: printf("dround"); break;
      case BC_EXT_DBL_SQRT:  printf("dsqrt"); break;
      case BC_EXT_DBL_CBRT:  printf("dcbrt"); break;
      case BC_EXT_DBL_EXP:   printf("dexp"); break;
      case BC_EXT_DBL_LN:    printf("dln"); break;
      case BC_EXT_DBL_LOG2:  printf("dlog2"); break;
      case BC_EXT_DBL_LOG10: printf("dlog10"); break;
      case BC_EXT_DBL_SIN:   printf("dsin"); break;
      case BC_EXT_DBL_COS:   printf("dcos"); break;
      case BC_EXT_DBL_ASIN:  printf("dasin"); break;
      case BC_EXT_DBL_ACOS:  printf("dacos"); break;
      case BC_EXT_DBL_TAN:   printf("dtan"); break;
      case BC_EXT_DBL_ATAN:  printf("datan"); break;
   }
   printf(" %d, acc <- %d, acc\n", dest1, src1);
#endif
   void *func = NULL;
   switch (type) {
      case BC_EXT_DBL_FLOOR: func = floor; break;
      case BC_EXT_DBL_CEIL:  func = ceil; break;
      case BC_EXT_DBL_ROUND: func = round; break;
      case BC_EXT_DBL_SQRT:  func = sqrt; break;
      case BC_EXT_DBL_CBRT:  func = cbrt; break;
      case BC_EXT_DBL_EXP:   func = exp; break;
      case BC_EXT_DBL_LN:    func = log; break;
      case BC_EXT_DBL_LOG2:  func = log2; break;
      case BC_EXT_DBL_LOG10: func = log10; break;
      case BC_EXT_DBL_SIN:   func = sin; break;
      case BC_EXT_DBL_COS:   func = cos; break;
      case BC_EXT_DBL_ASIN:  func = asin; break;
      case BC_EXT_DBL_ACOS:  func = acos; break;
      case BC_EXT_DBL_TAN:   func = tan; break;
      case BC_EXT_DBL_ATAN:  func = atan; break;
   }
#if defined(JIT_X86)
   #ifdef JIT_X86_64
      if (SH(src1*4 >= -128 && src1*4 < 128)) {
         mov____ecx__DWORD_PTR_redi_imm8(src1*4);
      }
      else {
         mov____ecx__DWORD_PTR_redi_imm32(src1*4);
      }
      shl____rax__imm(32);
      or_____rax__rcx();
      movq___xmm0__rax();

      #ifndef JIT_WIN64
         push___resi();
         push___redi();
      #endif
      if (!emit_func_call(heap, func, 0)) return 0;
      if (!emit_reinit_volatile_regs(heap)) return 0;
      #ifndef JIT_WIN64
         pop____redi();
         pop____resi();
      #endif

      movq___rax__xmm0();
      if (SH(dest1*4 >= -128 && dest1*4 < 128)) {
         mov____DWORD_PTR_redi_imm8__eax(dest1*4);
         mov____BYTE_PTR_resi_imm8__imm(dest1, 0);
      }
      else {
         mov____DWORD_PTR_redi_imm32__eax(dest1*4);
         mov____BYTE_PTR_resi_imm32__imm(dest1, 0);
      }
      shr____rax__imm(32);
   #else
      push___reax();
      if (SH(src1*4 >= -128 && src1*4 < 128)) {
         push___DWORD_PTR_edi_imm8(src1*4);
      }
      else {
         push___DWORD_PTR_edi_imm32(src1*4);
      }
      if (!emit_func_call(heap, func, 0x08)) return 0;
      if (SH((dest1+0)*4 >= -128 && (dest1+1)*4 < 128)) {
         fstp___QWORD_PTR_edi_imm8((dest1+0)*4);
         mov____eax__DWORD_PTR_redi_imm8((dest1+1)*4);
         mov____BYTE_PTR_resi_imm8__imm((dest1+0), 0);
      }
      else {
         fstp___QWORD_PTR_edi_imm32((dest1+0)*4);
         mov____eax__DWORD_PTR_redi_imm32((dest1+1)*4);
         mov____BYTE_PTR_resi_imm32__imm((dest1+0), 0);
      }
   #endif
   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_double_func2(Heap *heap, int dest1, int src1, int src2, int src3, int type)
{
#ifdef JIT_DEBUG
   printf("   ");
   switch (type) {
      case BC_EXT_DBL_POW:   printf("dpow"); break;
      case BC_EXT_DBL_ATAN2: printf("datan2"); break;
   }
   printf(" %d, acc <- %d, %d, %d, acc\n", dest1, src1, src2, src3);
#endif
   void *func = NULL;
   switch (type) {
      case BC_EXT_DBL_POW:   func = pow; break;
      case BC_EXT_DBL_ATAN2: func = atan2; break;
   }
#if defined(JIT_X86)
   #ifdef JIT_X86_64
      if (SH(src3*4 >= -128 && src3*4 < 128)) {
         mov____ecx__DWORD_PTR_redi_imm8(src3*4);
      }
      else {
         mov____ecx__DWORD_PTR_redi_imm32(src3*4);
      }
      shl____rax__imm(32);
      or_____rax__rcx();
      movq___xmm1__rax();

      if (src2 == src1+1) {
         if (SH(src1*4 >= -128 && src1*4 < 128)) {
            movq___xmm0__QWORD_PTR_rdi_imm8(src1*4);
         }
         else {
            movq___xmm0__QWORD_PTR_rdi_imm32(src1*4);
         }
      }
      else {
         if (SH(src1*4 >= -128 && src1*4 < 128)) {
            mov____ecx__DWORD_PTR_redi_imm8(src1*4);
         }
         else {
            mov____ecx__DWORD_PTR_redi_imm32(src1*4);
         }
         if (SH(src2*4 >= -128 && src2*4 < 128)) {
            mov____eax__DWORD_PTR_redi_imm8(src2*4);
         }
         else {
            mov____eax__DWORD_PTR_redi_imm32(src2*4);
         }
         shl____rax__imm(32);
         or_____rax__rcx();
         movq___xmm0__rax();
      }

      #ifndef JIT_WIN64
         push___resi();
         push___redi();
      #endif
      if (!emit_func_call(heap, func, 0)) return 0;
      if (!emit_reinit_volatile_regs(heap)) return 0;
      #ifndef JIT_WIN64
         pop____redi();
         pop____resi();
      #endif

      movq___rax__xmm0();
      if (SH(dest1*4 >= -128 && dest1*4 < 128)) {
         mov____DWORD_PTR_redi_imm8__eax(dest1*4);
         mov____BYTE_PTR_resi_imm8__imm(dest1, 0);
      }
      else {
         mov____DWORD_PTR_redi_imm32__eax(dest1*4);
         mov____BYTE_PTR_resi_imm32__imm(dest1, 0);
      }
      shr____rax__imm(32);
   #else
      push___reax();
      if (SH((src1+0)*4 >= -128 && (src1+2)*4 < 128)) {
         push___DWORD_PTR_edi_imm8((src1+2)*4);
         push___DWORD_PTR_edi_imm8((src1+1)*4);
         push___DWORD_PTR_edi_imm8((src1+0)*4);
      }
      else {
         push___DWORD_PTR_edi_imm32((src1+2)*4);
         push___DWORD_PTR_edi_imm32((src1+1)*4);
         push___DWORD_PTR_edi_imm32((src1+0)*4);
      }
      if (!emit_func_call(heap, func, 0x10)) return 0;
      if (SH((dest1+0)*4 >= -128 && (dest1+1)*4 < 128)) {
         fstp___QWORD_PTR_edi_imm8((dest1+0)*4);
         mov____eax__DWORD_PTR_redi_imm8((dest1+1)*4);
         mov____BYTE_PTR_resi_imm8__imm((dest1+0), 0);
      }
      else {
         fstp___QWORD_PTR_edi_imm32((dest1+0)*4);
         mov____eax__DWORD_PTR_redi_imm32((dest1+1)*4);
         mov____BYTE_PTR_resi_imm32__imm((dest1+0), 0);
      }
   #endif
   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_check_int(Heap *heap)
{
#ifdef JIT_DEBUG
   printf("   is_int\n");
#endif
#if defined(JIT_X86)
   mov____eax__ebx();
   xor____eax__imm8(1);
   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static inline int jit_append_check_float(Heap *heap)
{
#ifdef JIT_DEBUG
   printf("   is_float\n");
#endif
#if defined(JIT_X86)
   int ref1, ref2, ref3;
   
   cmp____ebx__imm8(0);
   je_____rel8(0);
   ref1 = heap->jit_code_len;
   
   dec____eax();
   cmp____eax__imm32((1<<23)-1);
   jb_____rel8(0);
   ref2 = heap->jit_code_len;

   mov____eax__imm(1);
   jmp____rel8(0);
   ref3 = heap->jit_code_len;

   // label 1:
   heap->jit_code[ref1-1] = heap->jit_code_len - ref1;
   heap->jit_code[ref2-1] = heap->jit_code_len - ref2;

   xor____eax__eax();

   // label 2:
   heap->jit_code[ref3-1] = heap->jit_code_len - ref3;

   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static int jit_is_array(Heap *heap, Value value)
{
   Array *arr;
   if (value.is_array && value.value > 0 && value.value < heap->size) {
      arr = &heap->data[value.value];
      if (arr->len != -1 && arr->hash_slots < 0) {
         return 1;
      }
   }
   return 0;
}


static int jit_is_string(Heap *heap, Value value)
{
   Array *arr;
   if (value.is_array && value.value > 0 && value.value < heap->size) {
      arr = &heap->data[value.value];
      if (arr->len != -1 && arr->hash_slots < 0 && arr->is_string) {
         return 1;
      }
   }
   return 0;
}


static int jit_is_hash(Heap *heap, Value value)
{
   Array *arr;
   if (value.is_array && value.value > 0 && value.value < heap->size) {
      arr = &heap->data[value.value];
      if (arr->len != -1 && arr->hash_slots >= 0 && !arr->is_handle) {
         return 1;
      }
   }
   return 0;
}


static int jit_is_shared(Heap *heap, Value value)
{
   Array *arr;
   if (value.is_array && value.value > 0 && value.value < heap->size) {
      arr = &heap->data[value.value];
      if (arr->len != -1 && arr->hash_slots < 0 && arr->is_shared) {
         return 1;
      }
   }
   return 0;
}


static int jit_is_const(Heap *heap, Value value)
{
   Array *arr;
   if (value.is_array && value.value > 0 && value.value < heap->size) {
      arr = &heap->data[value.value];
      if (arr->len != -1 && arr->hash_slots < 0 && arr->is_const) {
         return 1;
      }
   }
   return 0;
}


static int jit_is_funcref(Heap *heap, Value value)
{
   Array *arr;
   int func_id;

   if (value.is_array && value.value > 0 && value.value < heap->size) {
      arr = &heap->data[value.value];
      if (arr->len != -1 && arr->is_handle && arr->type == FUNC_REF_HANDLE_TYPE) {
         return 1;
      }
   }
   else if (value.is_array) {
      func_id = value.value - FUNC_REF_OFFSET;
      if (func_id > 0 && func_id < heap->functions.len) {
         return 1;
      }
   }
   return 0;
}


static int jit_is_weakref(Heap *heap, Value value)
{
   Array *arr;
   if (value.is_array && value.value > 0 && value.value < heap->size) {
      arr = &heap->data[value.value];
      if (arr->len != -1 && arr->is_handle && arr->type == WEAK_REF_HANDLE_TYPE) {
         return 1;
      }
   }
   return 0;
}


static int jit_is_handle(Heap *heap, Value value)
{
   Array *arr;
   if (value.is_array && value.value > 0 && value.value < heap->size) {
      arr = &heap->data[value.value];
      if (arr->len != -1 && arr->is_handle && arr->type != FUNC_REF_HANDLE_TYPE) {
         return 1;
      }
   }
   return 0;
}


static inline int jit_append_check_type(Heap *heap, int type)
{
#ifdef JIT_DEBUG
   printf("   ");
   switch (type) {
      case BC_EXT_IS_ARRAY:   printf("is_array\n"); break;
      case BC_EXT_IS_STRING:  printf("is_string\n"); break;
      case BC_EXT_IS_HASH:    printf("is_hash\n"); break;
      case BC_EXT_IS_SHARED:  printf("is_shared\n"); break;
      case BC_EXT_IS_CONST:   printf("is_const\n"); break;
      case BC_EXT_IS_FUNCREF: printf("is_funcref\n"); break;
      case BC_EXT_IS_WEAKREF: printf("is_weakref\n"); break;
      case BC_EXT_IS_HANDLE:  printf("is_handle\n"); break;
   }
#endif
   void *func = NULL;
   switch (type) {
      case BC_EXT_IS_ARRAY:   func = jit_is_array; break;
      case BC_EXT_IS_STRING:  func = jit_is_string; break;
      case BC_EXT_IS_HASH:    func = jit_is_hash; break;
      case BC_EXT_IS_SHARED:  func = jit_is_shared; break;
      case BC_EXT_IS_CONST:   func = jit_is_const; break;
      case BC_EXT_IS_FUNCREF: func = jit_is_funcref; break;
      case BC_EXT_IS_WEAKREF: func = jit_is_weakref; break;
      case BC_EXT_IS_HANDLE:  func = jit_is_handle; break;
   }
#if defined(JIT_X86)
   #ifdef JIT_X86_64
      #ifdef JIT_WIN64
         mov____rcx__r12();
         mov____edx__eax();
         shl____rbx__imm(32);
         or_____rdx__rbx();
         if (!emit_func_call(heap, func, 0)) return 0;
         if (!emit_reinit_volatile_regs(heap)) return 0;
      #else
         push___resi();
         push___redi();
         mov____rdi__r12();
         mov____esi__eax();
         shl____rbx__imm(32);
         or_____rsi__rbx();
         if (!emit_func_call(heap, func, 0)) return 0;
         if (!emit_reinit_volatile_regs(heap)) return 0;
         pop____redi();
         pop____resi();
      #endif
   #else
      push___rebx();
      push___reax();
      push___DWORD_PTR_ebp_imm8(0x08); // heap
      if (!emit_func_call(heap, func, 0x0C)) return 0;
   #endif
   xor____ebx__ebx();
#else
   return 0;
#endif
   return 1;
}


static int jit_check_time_limit(Heap *heap, int pc_err)
{
   uint64_t time = 0;
   int64_t diff;

   heap->time_counter = 10000;
   if (heap->stop_execution) {
      heap->time_counter = 0;
      return pc_err | JIT_ERROR_EXECUTION_STOP;
   }
   if (heap->time_limit != 0 && heap->time_limit != -1) {
      get_time(&time);
      diff = (int64_t)(heap->time_limit - time);
      if (diff <= 0) {
         heap->time_counter = 0;
         return pc_err | JIT_ERROR_TIME_LIMIT;
      }
   }
   return 0;
}


static inline int jit_append_check_time_limit(Heap *heap, int pc)
{
#ifdef JIT_DEBUG
   printf("   check_time_limit\n");
#endif
#if defined(JIT_X86)
   int ref1, ref2;
   #ifdef JIT_X86_64
      mov____rdx__r12();
   #else
      mov____edx__DWORD_PTR_ebp_imm8(0x08); // heap
   #endif
   dec____DWORD_PTR_redx_imm32(OFFSETOF(Heap, time_counter));
   jg_____rel8(0);
   ref1 = heap->jit_code_len;

   #ifdef JIT_X86_64
      #ifdef JIT_WIN64
         mov____rcx__r12();
         mov____edx__imm(JIT_PC_ERR(pc, 0));
         if (!emit_func_call(heap, jit_check_time_limit, 0)) return 0;
         if (!emit_reinit_volatile_regs(heap)) return 0;
      #else
         push___resi();
         push___redi();
         mov____rdi__r12();
         mov____esi__imm(JIT_PC_ERR(pc, 0));
         if (!emit_func_call(heap, jit_check_time_limit, 0)) return 0;
         if (!emit_reinit_volatile_regs(heap)) return 0;
         pop____redi();
         pop____resi();
      #endif
   #else
      push___imm32(JIT_PC_ERR(pc, 0));
      push___redx();
      if (!emit_func_call(heap, jit_check_time_limit, 0x08)) return 0;
   #endif

   cmp____eax__imm8(0);
   je_____rel8(0);
   ref2 = heap->jit_code_len;

   mov____edx__eax();
   jmp____rel32(heap->jit_error_code - heap->jit_code_len - 4);

   heap->jit_code[ref1-1] = heap->jit_code_len - ref1;
   heap->jit_code[ref2-1] = heap->jit_code_len - ref2;
#else
   return 0;
#endif
   return 1;
}


static void jit_scan_jump_targets(Heap *heap, int addr_start, int addr_end, uint32_t *jump_targets)
{
   struct SwitchTable {
      int start, end;
      struct SwitchTable *next;
   };
   int i, pc, op, dest_pc;
   unsigned short short_val;
   int int_val;
   int table_idx, size, default_pc;
   int *table;
   struct SwitchTable *switch_table = NULL, *new_switch_table;
   
   for (pc = addr_start; pc < addr_end; pc++) {
      if (switch_table && pc == switch_table->start) {
         pc = switch_table->end;
         new_switch_table = switch_table->next;
         free(switch_table);
         switch_table = new_switch_table;
      }

      op = heap->bytecode[pc];
      #define DATA() (heap->bytecode[++pc])
      #define DATA_SHORT() *((unsigned short *)memcpy(&short_val, &heap->bytecode[(pc += 2)-1], sizeof(unsigned short)))
      #define DATA_INT() *((int *)memcpy(&int_val, &heap->bytecode[(pc += 4)-3], sizeof(int)))
      #define MARK_TARGET(pc) \
         jump_targets[((pc)-addr_start) >> 5] |= 1 << (((pc)-addr_start) & 31);
      switch (op) {
         case BC_INC:
         case BC_DEC:
            pc++;
            break;

         case BC_CONST_P8:
         case BC_CONST_N8:
            pc++;
            break;

         case BC_CONST_P16:
         case BC_CONST_N16:
            pc += 2;
            break;

         case BC_CONST_I32:
         case BC_CONST_F32:
            pc += 4;
            break;

         case BC_BRANCH_LONG:
            int_val = DATA_INT();
            dest_pc = pc+int_val+1;
            MARK_TARGET(dest_pc);
            break;

         case BC_JUMP_LONG:
            int_val = DATA_INT();
            dest_pc = pc+int_val+1;
            MARK_TARGET(dest_pc);
            break;

         case BC_LOOP_I8:
            int_val = DATA();
            dest_pc = pc-int_val+1-1;
            MARK_TARGET(dest_pc);
            break;

         case BC_LOOP_I16:
            int_val = DATA_SHORT();
            dest_pc = pc-int_val+1-2;
            MARK_TARGET(dest_pc);
            break;

         case BC_LOOP_I32:
            int_val = DATA_INT();
            dest_pc = pc-int_val+1-4;
            MARK_TARGET(dest_pc);
            break;

         case BC_LOAD_LOCAL:
            pc += 4;
            break;

         case BC_STORE_LOCAL:
            pc += 4;
            break;

         case BC_SWITCH:
            table_idx = DATA_INT();
            table = &((int *)heap->bytecode)[table_idx];
            size = table[-2];
            default_pc = table[-1];
            for (i=0; i<size; i++) {
               if (table[i*2+1] < 0) {
                  MARK_TARGET(-table[i*2+1]);
                  i++;
               }
               else {
                  MARK_TARGET(table[i*2+1]);
               }
            }
            MARK_TARGET(default_pc);
            
            new_switch_table = malloc(sizeof(struct SwitchTable));
            new_switch_table->start = (table_idx-2)*4;
            new_switch_table->end = (table_idx+size*2)*4;
            new_switch_table->next = switch_table;
            switch_table = new_switch_table;
            break;

         case BC_CHECK_STACK:
            pc += 2;
            break;

         case BC_EXTENDED:
            pc++;
            break;

         default:
            if (op >= BC_BRANCH0 && op <= BC_BRANCH0+7) {
               int_val = ((op & 7) << 8) | DATA();
               dest_pc = pc+int_val+1;
               MARK_TARGET(dest_pc);
               break;
            }
            if (op >= BC_JUMP0 && op <= BC_JUMP0+7) {
               int_val = ((op & 7) << 8) | DATA();
               dest_pc = pc+int_val+1;
               MARK_TARGET(dest_pc);
               break;
            }
      }
      #undef DATA
      #undef DATA_SHORT
      #undef DATA_INT
      #undef MARK_TARGET
   }

   while (switch_table) {
      new_switch_table = switch_table->next;
      free(switch_table);
      switch_table = new_switch_table;
   }
}


enum {
   SLOT_VALUE,
   SLOT_INDIRECT,
   SLOT_IMMEDIATE
};

typedef struct {
   int indirect_slot : 17;
   int type : 3;
} StackEntry;


static inline int get_real_slot(StackEntry *stack, int idx)
{
   if (stack[idx].type == SLOT_INDIRECT) {
      return stack[idx].indirect_slot;
   }
   return idx;
}


static const char *jit_compile_function(Heap *heap, int addr_start, int addr_end, int num_params, uint16_t *labels, int *addrs, uint32_t *jump_targets, DynArray *forward_refs, DynArray *func_refs, DynArray *error_stubs, StackEntry *stack, int *max_stack_out)
{
   struct SwitchTable {
      int start, end;
      struct SwitchTable *next;
   };
   const char *error = NULL;
   int i, pc, op, dest_pc, next_bc;
#ifdef JIT_DEBUG
   int orig_pc;
#endif
   unsigned short short_val;
   int int_val;
   int table_idx, size, default_pc;
   int *table;
   struct SwitchTable *switch_table = NULL, *new_switch_table;
   int cur_stack = 0, max_stack = 0, deadcode = 0, accum_valid = 0, lowest_indir = INT_MAX;
   int last_int_const = 0;
   
   if (!jit_append_start(heap)) goto out_of_memory_error;

   for (pc = addr_start; pc < addr_end; pc++) {
      if (switch_table && pc == switch_table->start) {
         pc = switch_table->end;
         new_switch_table = switch_table->next;
         free(switch_table);
         switch_table = new_switch_table;
      }

      #define DATA() (heap->bytecode[++pc])
      #define DATA_SBYTE() ((signed char)heap->bytecode[++pc])
      #define DATA_SHORT() *((unsigned short *)memcpy(&short_val, &heap->bytecode[(pc += 2)-1], sizeof(unsigned short)))
      #define DATA_INT() *((int *)memcpy(&int_val, &heap->bytecode[(pc += 4)-3], sizeof(int)))
      #define DATA_FLOAT() *((float *)memcpy(&float_val, &heap->bytecode[(pc += 4)-3], sizeof(float)))
      #define MARK_LABEL(pc) \
         if (!deadcode) { \
            if (labels[(pc) - addr_start] != 0xFFFF && labels[(pc) - addr_start] != cur_stack) { \
               error = "internal error: stack mismatch at label"; \
               goto error; \
            } \
            if (cur_stack < 0) { \
               error = "internal error: stack mismatch"; \
               goto error; \
            } \
            if (cur_stack >= 0xFFFF) { \
               error = "internal error: stack too big"; \
               goto error; \
            } \
            labels[(pc) - addr_start] = cur_stack; \
         }
      #define STORE_ACCUM() \
         if (accum_valid) { \
            if (!jit_append_set(heap, cur_stack-1)) goto out_of_memory_error; \
            stack[cur_stack-1].type = SLOT_VALUE; \
            accum_valid = 0; \
         }
      #define LOAD_ACCUM() \
         if (!accum_valid) { \
            if (!jit_append_get(heap, get_real_slot(stack, cur_stack-1))) goto out_of_memory_error; \
            stack[cur_stack-1].type = SLOT_VALUE; \
            accum_valid = 1; \
         }
      #define UPDATE_ACCUM() \
         if (accum_valid) { \
            if (!jit_append_set(heap, cur_stack-1)) goto out_of_memory_error; \
         }
      #define CLEAN_INDIRECTS() \
         for (i=cur_stack-1; i>=lowest_indir; i--) { \
            if (stack[i].type == SLOT_INDIRECT) { \
               if (!jit_append_get_tmp(heap, get_real_slot(stack, i))) goto out_of_memory_error; \
               if (!jit_append_set_tmp(heap, i)) goto out_of_memory_error; \
               stack[i].type = SLOT_VALUE; \
            } \
         } \
         lowest_indir = INT_MAX;
      #define HANDLE_CONST(value, flag) \
         if (!deadcode) { \
            next_bc = pc+1 < addr_end? heap->bytecode[pc+1] : -1; \
            switch (next_bc) { \
               case BC_LOADN: \
               case BC_CREATE_ARRAY: \
               case BC_CREATE_HASH: \
               case BC_STRING_CONCAT: \
               case BC_CALL_DIRECT: \
               case BC_CALL2_DIRECT: \
               case BC_CALL_DYNAMIC: \
               case BC_CALL2_DYNAMIC: \
               case BC_CALL_NATIVE: \
               case BC_CALL2_NATIVE: \
                  STORE_ACCUM(); \
                  stack[cur_stack].type = SLOT_IMMEDIATE; \
                  break; \
               case BC_POPN: \
                  stack[cur_stack].type = SLOT_IMMEDIATE; \
                  break; \
               case BC_STOREN: \
               case BC_RETURN: \
                  LOAD_ACCUM(); \
                  stack[cur_stack].type = SLOT_IMMEDIATE; \
                  break; \
               default: \
                  STORE_ACCUM(); \
                  if (!jit_append_const(heap, value, next_bc == BC_CONST_STRING? 1 : (flag))) goto out_of_memory_error; \
                  stack[cur_stack].type = SLOT_VALUE; \
                  accum_valid = 1; \
            } \
         }
      #define HANDLE_LOAD(slot) \
         if (!deadcode) { \
            STORE_ACCUM(); \
            stack[cur_stack].indirect_slot = get_real_slot(stack, slot); \
            stack[cur_stack].type = SLOT_INDIRECT; \
            if (cur_stack < lowest_indir) { \
               lowest_indir = cur_stack; \
            } \
         }
      #define HANDLE_STORE(slot) \
         if (!deadcode) { \
            int value_retrieved = 0; \
            LOAD_ACCUM(); \
            if (stack[slot].type == SLOT_VALUE) { \
               for (i=(slot)+1; i<cur_stack; i++) { \
                  if (stack[i].type == SLOT_INDIRECT && stack[i].indirect_slot == (slot)) { \
                     if (!value_retrieved) { \
                        if (!jit_append_get_tmp(heap, get_real_slot(stack, slot))) goto out_of_memory_error; \
                        value_retrieved = 1; \
                     } \
                     if (!jit_append_set_tmp(heap, i)) goto out_of_memory_error; \
                     stack[i].type = SLOT_VALUE; \
                  } \
               } \
            } \
            if (!jit_append_set(heap, slot)) goto out_of_memory_error; \
            stack[slot].type = SLOT_VALUE; \
            accum_valid = 0; \
         }
      #define HANDLE_BRANCH(dest_pc) \
         if (!deadcode) { \
            int label_addr; \
            LOAD_ACCUM(); \
            CLEAN_INDIRECTS(); \
            if (!jit_append_branch(heap, &label_addr)) goto out_of_memory_error; \
            if (dynarray_add(forward_refs, (void *)(intptr_t)label_addr)) goto out_of_memory_error; \
            if (dynarray_add(forward_refs, (void *)(intptr_t)dest_pc)) goto out_of_memory_error; \
            accum_valid = 0; \
         }
      #define HANDLE_LOOP(dest_pc) \
         if (!deadcode) { \
            STORE_ACCUM(); \
            CLEAN_INDIRECTS(); \
            if (!jit_append_loop(heap, heap->jit_code_len, addrs[dest_pc - addr_start])) goto out_of_memory_error; \
            accum_valid = 0; \
         }
      #define HANDLE_JUMP(dest_pc) \
         if (!deadcode) { \
            int label_addr; \
            STORE_ACCUM(); \
            CLEAN_INDIRECTS(); \
            if (dest_pc > pc) { \
               if (!jit_append_jump(heap, heap->jit_code_len, heap->jit_code_len, &label_addr)) goto out_of_memory_error; \
               if (dynarray_add(forward_refs, (void *)(intptr_t)label_addr)) goto out_of_memory_error; \
               if (dynarray_add(forward_refs, (void *)(intptr_t)dest_pc)) goto out_of_memory_error; \
            } \
            else { \
               if (!jit_append_jump(heap, heap->jit_code_len, addrs[dest_pc - addr_start], NULL)) goto out_of_memory_error; \
            } \
            accum_valid = 0; \
         }

      if (jump_targets[(pc - addr_start) >> 5] & (1 << ((pc - addr_start) & 31))) {
         if (!deadcode) {
            STORE_ACCUM();
            CLEAN_INDIRECTS();
         }
         #ifdef JIT_DEBUG
            printf("jump target (pc=%d)\n", pc);
         #endif
      }

      if (labels[pc - addr_start] != 0xFFFF) {
         cur_stack = labels[pc - addr_start];
         deadcode = 0;
         accum_valid = 0;
         for (i=forward_refs->len-2; i>=0; i-=2) {
            if ((intptr_t)forward_refs->data[i+1] == pc) {
               jit_update_branch(heap, (intptr_t)forward_refs->data[i+0], heap->jit_code_len);
               forward_refs->len -= 2;
               if (forward_refs->len > i) {
                  memmove(&forward_refs->data[i], &forward_refs->data[i+2], (forward_refs->len - i) * sizeof(void *));
               }
            }
         }
      }

      addrs[pc - addr_start] = heap->jit_code_len;
      #ifdef JIT_DEBUG
         orig_pc = pc;
      #endif
      op = heap->bytecode[pc];
      switch (op) {
         case BC_POP:
            if (!deadcode) {
               accum_valid = 0;
            }
            cur_stack--;
            break;
            
         case BC_POPN:
            if (!deadcode) {
               accum_valid = 0;
            }
            cur_stack -= last_int_const+1;
            break;

         case BC_LOADN:
            if (!deadcode) {
               if (accum_valid) goto accum_error;
               if (last_int_const == 1) {
                  // handles cases like ++a[0] and x = a[0] += 1
                  // search for "buf_append_load(par, 2);"
                  stack[cur_stack-1].type = SLOT_VALUE;
                  accum_valid = 1;
               }
               else if (last_int_const >= 0) {
                  error = "internal error: load above top";
                  goto error;
               }
               else {
                  stack[cur_stack-1].indirect_slot = get_real_slot(stack, cur_stack+last_int_const);
                  stack[cur_stack-1].type = SLOT_INDIRECT;
                  if (cur_stack-1 < lowest_indir) {
                     lowest_indir = cur_stack-1;
                  }
               }
            }
            break;

         case BC_STOREN:
            HANDLE_STORE(cur_stack + last_int_const);
            cur_stack -= 2;
            break;

         case BC_ADD:
            if (!deadcode) {
               LOAD_ACCUM();
               if (!jit_append_add(heap, get_real_slot(stack, cur_stack-2), 1, pc+1)) goto out_of_memory_error;
               stack[cur_stack-2].type = SLOT_VALUE;
            }
            cur_stack--;
            break;

         case BC_SUB:
            if (!deadcode) {
               if (accum_valid) {
                  if (!jit_append_rsub(heap, get_real_slot(stack, cur_stack-2), 1, pc+1)) goto out_of_memory_error;
               }
               else {
                  if (!jit_append_get(heap, get_real_slot(stack, cur_stack-2))) goto out_of_memory_error;
                  if (!jit_append_sub(heap, get_real_slot(stack, cur_stack-1), 1, pc+1)) goto out_of_memory_error;
               }
               stack[cur_stack-2].type = SLOT_VALUE;
               accum_valid = 1;
            }
            cur_stack--;
            break;

         case BC_MUL:
            if (!deadcode) {
               LOAD_ACCUM();
               if (!jit_append_mul(heap, get_real_slot(stack, cur_stack-2), 1, pc+1)) goto out_of_memory_error;
               stack[cur_stack-2].type = SLOT_VALUE;
            }
            cur_stack--;
            break;

         case BC_ADD_MOD:
            if (!deadcode) {
               LOAD_ACCUM();
               if (!jit_append_add(heap, get_real_slot(stack, cur_stack-2), 0, -1)) goto out_of_memory_error;
               stack[cur_stack-2].type = SLOT_VALUE;
            }
            cur_stack--;
            break;

         case BC_SUB_MOD:
            if (!deadcode) {
               if (accum_valid) {
                  if (!jit_append_rsub(heap, get_real_slot(stack, cur_stack-2), 0, -1)) goto out_of_memory_error;
               }
               else {
                  if (!jit_append_get(heap, get_real_slot(stack, cur_stack-2))) goto out_of_memory_error;
                  if (!jit_append_sub(heap, get_real_slot(stack, cur_stack-1), 0, -1)) goto out_of_memory_error;
               }
               stack[cur_stack-2].type = SLOT_VALUE;
               accum_valid = 1;
            }
            cur_stack--;
            break;

         case BC_MUL_MOD:
            if (!deadcode) {
               LOAD_ACCUM();
               if (!jit_append_mul(heap, get_real_slot(stack, cur_stack-2), 0, -1)) goto out_of_memory_error;
               stack[cur_stack-2].type = SLOT_VALUE;
            }
            cur_stack--;
            break;

         case BC_DIV:
         case BC_REM:
            if (!deadcode) {
               if (accum_valid) {
                  if (!jit_append_divrem(heap, get_real_slot(stack, cur_stack-2), op == BC_REM, 1, pc+1)) goto out_of_memory_error;
               }
               else {
                  if (!jit_append_get(heap, get_real_slot(stack, cur_stack-2))) goto out_of_memory_error;
                  if (!jit_append_divrem(heap, get_real_slot(stack, cur_stack-1), op == BC_REM, 0, pc+1)) goto out_of_memory_error;
               }
               stack[cur_stack-2].type = SLOT_VALUE;
               accum_valid = 1;
            }
            cur_stack--;
            break;

         case BC_SHL:
         case BC_SHR:
         case BC_USHR:
            if (!deadcode) {
               if (accum_valid) {
                  if (!jit_append_shift(heap, get_real_slot(stack, cur_stack-2), op == BC_SHL, op == BC_USHR, 1)) goto out_of_memory_error;
               }
               else {
                  if (!jit_append_get(heap, get_real_slot(stack, cur_stack-2))) goto out_of_memory_error;
                  if (!jit_append_shift(heap, get_real_slot(stack, cur_stack-1), op == BC_SHL, op == BC_USHR, 0)) goto out_of_memory_error;
               }
               stack[cur_stack-2].type = SLOT_VALUE;
               accum_valid = 1;
            }
            cur_stack--;
            break;

         case BC_AND:
            if (!deadcode) {
               LOAD_ACCUM();
               if (!jit_append_and(heap, get_real_slot(stack, cur_stack-2))) goto out_of_memory_error;
               stack[cur_stack-2].type = SLOT_VALUE;
            }
            cur_stack--;
            break;

         case BC_OR:
            if (!deadcode) {
               LOAD_ACCUM();
               if (!jit_append_or(heap, get_real_slot(stack, cur_stack-2))) goto out_of_memory_error;
               stack[cur_stack-2].type = SLOT_VALUE;
            }
            cur_stack--;
            break;

         case BC_XOR:
            if (!deadcode) {
               LOAD_ACCUM();
               if (!jit_append_xor(heap, get_real_slot(stack, cur_stack-2))) goto out_of_memory_error;
               stack[cur_stack-2].type = SLOT_VALUE;
            }
            cur_stack--;
            break;

         case BC_LT:
         case BC_LE:
         case BC_GT:
         case BC_GE:
         case BC_EQ:
         case BC_NE:
            if (!deadcode) {
               int swap_op = op;
               LOAD_ACCUM();
               switch (op) {
                  case BC_LT: swap_op = BC_GT; break;
                  case BC_LE: swap_op = BC_GE; break;
                  case BC_GT: swap_op = BC_LT; break;
                  case BC_GE: swap_op = BC_LE; break;
               }
               if (!jit_append_cmp(heap, get_real_slot(stack, cur_stack-2), swap_op)) goto out_of_memory_error;
               stack[cur_stack-2].type = SLOT_VALUE;
            }
            cur_stack--;
            break;

         case BC_EQ_VALUE:
         case BC_NE_VALUE:
            if (!deadcode) {
               LOAD_ACCUM();
               if (!jit_append_cmp_value(heap, get_real_slot(stack, cur_stack-2), op == BC_EQ_VALUE)) goto out_of_memory_error;
               stack[cur_stack-2].type = SLOT_VALUE;
            }
            cur_stack--;
            break;

         case BC_BITNOT:
         case BC_LOGNOT:
            if (!deadcode) {
               LOAD_ACCUM();
               if (!jit_append_not(heap, op == BC_LOGNOT)) goto out_of_memory_error;
            }
            break;

         case BC_INC:
         case BC_DEC:
            int_val = cur_stack + DATA_SBYTE();
            if (!deadcode) {
               if (int_val == cur_stack-1) {
                  LOAD_ACCUM();
                  if (!jit_append_incdec(heap, op == BC_INC, pc+2)) goto out_of_memory_error;
               }
               else {
                  CLEAN_INDIRECTS();
                  if (stack[int_val].type == SLOT_INDIRECT) {
                     if (!jit_append_get_tmp(heap, stack[int_val].indirect_slot)) goto out_of_memory_error;
                     if (!jit_append_incdec_tmp(heap, op == BC_INC, pc+2)) goto out_of_memory_error;
                     if (!jit_append_set_tmp(heap, int_val)) goto out_of_memory_error;
                  }
                  else {
                     if (!jit_append_incdec_stack(heap, int_val, op == BC_INC, pc+2)) goto out_of_memory_error;
                  }
                  stack[int_val].type = SLOT_VALUE;
               }
            }
            break;

         case BC_FLOAT_ADD:
         case BC_FLOAT_SUB:
         case BC_FLOAT_MUL:
         case BC_FLOAT_DIV:
            if (!deadcode) {
               LOAD_ACCUM();
               if (!jit_append_float_op(heap, cur_stack-2, get_real_slot(stack, cur_stack-2), cur_stack-1, op)) goto out_of_memory_error;
               stack[cur_stack-2].type = SLOT_VALUE;
            }
            cur_stack--;
            break;

         case BC_FLOAT_LT:
         case BC_FLOAT_LE:
         case BC_FLOAT_GT:
         case BC_FLOAT_GE:
         case BC_FLOAT_EQ:
         case BC_FLOAT_NE:
            if (!deadcode) {
               LOAD_ACCUM();
               if (!jit_append_float_cmp(heap, get_real_slot(stack, cur_stack-2), cur_stack-1, op)) goto out_of_memory_error;
               stack[cur_stack-2].type = SLOT_VALUE;
            }
            cur_stack--;
            break;

         case BC_RETURN:
            if (!deadcode) {
               if (!jit_append_set(heap, -num_params-1)) goto out_of_memory_error;
               if (!jit_append_return(heap, -num_params)) goto out_of_memory_error;
            }
            cur_stack -= last_int_const+1;
            if (!deadcode && cur_stack != -num_params) {
               error = "internal error: stack mismatch";
               goto error;
            }
            deadcode = 1;
            break;

         case BC_RETURN2:
            if (!deadcode) {
               STORE_ACCUM();
               if (!jit_append_return2(heap, get_real_slot(stack, cur_stack-2), get_real_slot(stack, cur_stack-1))) goto out_of_memory_error;
            }
            cur_stack -= 2;
            deadcode = 1;
            break;

         case BC_CALL_DIRECT:
         case BC_CALL2_DIRECT: {
            Function *func;
            if (last_int_const < 1 || last_int_const >= heap->functions.len) {
               error = "internal error: bad function index";
               goto error;
            }
            func = heap->functions.data[last_int_const];
            if (!deadcode) {
               int label_addr;
               CLEAN_INDIRECTS();
               if (!jit_append_call(heap, cur_stack-1, cur_stack-func->num_params-2, (pc+1) | (1<<31), func->jit_addr, &label_addr, NULL, op == BC_CALL2_DIRECT)) goto out_of_memory_error;
               if (func->jit_addr == 0) {
                  if (dynarray_add(func_refs, (void *)(intptr_t)label_addr)) goto out_of_memory_error;
                  if (dynarray_add(func_refs, func)) goto out_of_memory_error;
               }
            }
            cur_stack -= func->num_params+1;
            break;
         }

         case BC_CALL_DYNAMIC:
         case BC_CALL2_DYNAMIC:
            if (!deadcode) {
               CLEAN_INDIRECTS();
               if (!jit_append_call(heap, cur_stack-1, cur_stack-last_int_const-2, (pc+1) | (1<<31), -1, NULL, NULL, op == BC_CALL2_DYNAMIC)) goto out_of_memory_error;
            }
            cur_stack -= last_int_const+1;
            break;

         case BC_CALL_NATIVE:
         case BC_CALL2_NATIVE: {
            NativeFunction *nfunc;
            if (last_int_const < 0 || last_int_const >= heap->native_functions.len) {
               error = "internal error: bad native function index";
               goto error;
            }
            nfunc = heap->native_functions.data[last_int_const];
            if (!deadcode) {
               CLEAN_INDIRECTS();
               if (!jit_append_call(heap, cur_stack-1, cur_stack-nfunc->num_params-2, (pc+1) | (1<<31), 0, NULL, nfunc, op == BC_CALL2_NATIVE)) goto out_of_memory_error;
            }
            cur_stack -= nfunc->num_params+1;
            break;
         }

         case BC_CLEAN_CALL2:
            if (!deadcode) {
               stack[cur_stack].type = SLOT_VALUE;
            }
            cur_stack++;
            break;

         case BC_CREATE_ARRAY:
            if (!deadcode) {
               CLEAN_INDIRECTS();
               if (!jit_append_vararg_op(heap, cur_stack-1, last_int_const, pc+1, op)) goto out_of_memory_error;
               stack[cur_stack-last_int_const-1].type = SLOT_VALUE;
            }
            cur_stack -= last_int_const;
            break;

         case BC_CREATE_HASH:
            if (!deadcode) {
               CLEAN_INDIRECTS();
               if (!jit_append_vararg_op(heap, cur_stack-1, last_int_const*2, pc+1, op)) goto out_of_memory_error;
               stack[cur_stack-last_int_const*2-1].type = SLOT_VALUE;
            }
            cur_stack -= last_int_const*2;
            break;

         case BC_ARRAY_GET:
            if (!deadcode) {
               LOAD_ACCUM();
               if (!jit_append_array_get(heap, get_real_slot(stack, cur_stack-2), pc+1, error_stubs)) goto out_of_memory_error;
               stack[cur_stack-2].type = SLOT_VALUE;
            }
            cur_stack--;
            break;

         case BC_ARRAY_SET:
            if (!deadcode) {
               int reload_accum = (heap->bytecode[pc+1] == BC_CONST0+1 && heap->bytecode[pc+2] == BC_LOADN);
               LOAD_ACCUM();
               if (reload_accum) {
                  UPDATE_ACCUM();
               }
               if (!jit_append_array_set(heap, get_real_slot(stack, cur_stack-3), get_real_slot(stack, cur_stack-2), 0, pc+1, error_stubs)) goto out_of_memory_error;
               accum_valid = 0;
               if (reload_accum) {
                  LOAD_ACCUM();
                  accum_valid = 0;
               }
            }
            cur_stack -= 3;
            break;

         case BC_ARRAY_APPEND:
            if (!deadcode) {
               LOAD_ACCUM();
               if (!jit_append_array_set(heap, get_real_slot(stack, cur_stack-2), 0, 1, pc+1, error_stubs)) goto out_of_memory_error;
               accum_valid = 0;
            }
            cur_stack -= 2;
            break;

         case BC_HASH_GET:
            if (!deadcode) {
               LOAD_ACCUM();
               if (!jit_append_hash_get(heap, get_real_slot(stack, cur_stack-2), pc+1, error_stubs)) goto out_of_memory_error;
               stack[cur_stack-2].type = SLOT_VALUE;
            }
            cur_stack--;
            break;

         case BC_HASH_SET:
            if (!deadcode) {
               int reload_accum = (heap->bytecode[pc+1] == BC_CONST0+1 && heap->bytecode[pc+2] == BC_LOADN);
               LOAD_ACCUM();
               if (reload_accum) {
                  UPDATE_ACCUM();
               }
               if (!jit_append_hash_set(heap, get_real_slot(stack, cur_stack-3), get_real_slot(stack, cur_stack-2), pc+1, error_stubs)) goto out_of_memory_error;
               accum_valid = 0;
               if (reload_accum) {
                  LOAD_ACCUM();
                  accum_valid = 0;
               }
            }
            cur_stack -= 3;
            break;

         case BC_CONST_P8:
         case BC_CONST_N8:
            last_int_const = DATA()+1;
            if (op == BC_CONST_N8) last_int_const = -last_int_const;
            HANDLE_CONST(last_int_const, 0);
            cur_stack++;
            break;

         case BC_CONST_P16:
         case BC_CONST_N16:
            last_int_const = DATA_SHORT()+1;
            if (op == BC_CONST_N16) last_int_const = -last_int_const;
            HANDLE_CONST(last_int_const, 0);
            cur_stack++;
            break;

         case BC_CONST_I32:
            last_int_const = DATA_INT();
            HANDLE_CONST(last_int_const, 0);
            cur_stack++;
            break;

         case BC_CONST_F32:
            int_val = DATA_INT();
            HANDLE_CONST(int_val, 1);
            cur_stack++;
            break;

         case BC_BRANCH_LONG:
            int_val = DATA_INT();
            dest_pc = pc+int_val+1;
            HANDLE_BRANCH(dest_pc);
            cur_stack--;
            MARK_LABEL(dest_pc);
            break;

         case BC_JUMP_LONG:
            int_val = DATA_INT();
            dest_pc = pc+int_val+1;
            HANDLE_JUMP(dest_pc);
            MARK_LABEL(dest_pc);
            deadcode = 1;
            break;

         case BC_LOOP_I8:
            int_val = DATA();
            dest_pc = pc-int_val+1-1;
            HANDLE_LOOP(dest_pc);
            MARK_LABEL(dest_pc);
            deadcode = 1;
            break;

         case BC_LOOP_I16:
            int_val = DATA_SHORT();
            dest_pc = pc-int_val+1-2;
            HANDLE_LOOP(dest_pc);
            MARK_LABEL(dest_pc);
            deadcode = 1;
            break;

         case BC_LOOP_I32:
            int_val = DATA_INT();
            dest_pc = pc-int_val+1-4;
            HANDLE_LOOP(dest_pc);
            MARK_LABEL(dest_pc);
            deadcode = 1;
            break;

         case BC_LOAD_LOCAL:
            int_val = DATA_INT();
            if (!deadcode) {
               STORE_ACCUM();
               if (!jit_append_load_local(heap, int_val)) goto out_of_memory_error;
               stack[cur_stack].type = SLOT_VALUE;
               accum_valid = 1;
            }
            cur_stack++;
            break;

         case BC_STORE_LOCAL:
            int_val = DATA_INT();
            if (!deadcode) {
               LOAD_ACCUM();
               if (!jit_append_store_local(heap, int_val)) goto out_of_memory_error;
               accum_valid = 0;
            }
            cur_stack--;
            break;

         case BC_SWITCH:
            table_idx = DATA_INT();
            table = &((int *)heap->bytecode)[table_idx];
            size = table[-2];
            default_pc = table[-1];
            if (!deadcode) {
               int label_addr;
               LOAD_ACCUM();
               CLEAN_INDIRECTS();
               for (i=0; i<size; i++) {
                  if (table[i*2+1] < 0) {
                     if (!jit_append_switch_range(heap, table[i*2+0], table[(i+1)*2+0], &label_addr)) goto out_of_memory_error;
                     if (dynarray_add(forward_refs, (void *)(intptr_t)label_addr)) goto out_of_memory_error;
                     if (dynarray_add(forward_refs, (void *)(intptr_t)-table[i*2+1])) goto out_of_memory_error;
                     i++;
                  }
                  else {
                     if (!jit_append_switch_case(heap, table[i*2+0], &label_addr)) goto out_of_memory_error;
                     if (dynarray_add(forward_refs, (void *)(intptr_t)label_addr)) goto out_of_memory_error;
                     if (dynarray_add(forward_refs, (void *)(intptr_t)table[i*2+1])) goto out_of_memory_error;
                  }
               }
               if (!jit_append_jump(heap, heap->jit_code_len, 0, &label_addr)) goto out_of_memory_error;
               if (dynarray_add(forward_refs, (void *)(intptr_t)label_addr)) goto out_of_memory_error;
               if (dynarray_add(forward_refs, (void *)(intptr_t)default_pc)) goto out_of_memory_error;
               accum_valid = 0;
            }
            cur_stack--;
            MARK_LABEL(default_pc);
            for (i=0; i<size; i++) {
               if (table[i*2+1] < 0) {
                  MARK_LABEL(-table[i*2+1]);
                  i++;
               }
               else {
                  MARK_LABEL(table[i*2+1]);
               }
            }
            deadcode = 1;
            
            new_switch_table = malloc(sizeof(struct SwitchTable));
            new_switch_table->start = (table_idx-2)*4;
            new_switch_table->end = (table_idx+size*2)*4;
            new_switch_table->next = switch_table;
            switch_table = new_switch_table;
            break;

         case BC_LENGTH:
            if (!deadcode) {
               LOAD_ACCUM();
               if (!jit_append_length(heap, pc+1, error_stubs)) goto out_of_memory_error;
            }
            break;

         case BC_CONST_STRING:
            if (!deadcode) {
               if (!accum_valid) goto accum_error;
            }
            break;

         case BC_STRING_CONCAT:
            if (!deadcode) {
               CLEAN_INDIRECTS();
               if (!jit_append_vararg_op(heap, cur_stack-1, last_int_const, pc+1, op)) goto out_of_memory_error;
               stack[cur_stack-last_int_const-1].type = SLOT_VALUE;
            }
            cur_stack -= last_int_const;
            break;

         case BC_CHECK_STACK:
            int_val = DATA_SHORT();
            if (!deadcode) {
               if (!jit_append_check_stack(heap, int_val, pc+1)) goto out_of_memory_error;
            }
            break;

         case BC_EXTENDED:
            op = DATA();
            switch (op) {
               case BC_EXT_MIN:
               case BC_EXT_MAX:
                  if (!deadcode) {
                     LOAD_ACCUM();
                     if (!jit_append_minmax(heap, get_real_slot(stack, cur_stack-2), op == BC_EXT_MIN)) goto out_of_memory_error;
                     stack[cur_stack-2].type = SLOT_VALUE;
                  }
                  cur_stack--;
                  break;

               case BC_EXT_CLAMP:
                  if (!deadcode) {
                     LOAD_ACCUM();
                     if (!jit_append_clamp(heap, get_real_slot(stack, cur_stack-3), get_real_slot(stack, cur_stack-2))) goto out_of_memory_error;
                     stack[cur_stack-3].type = SLOT_VALUE;
                  }
                  cur_stack -= 2;
                  break;

               case BC_EXT_ABS:
                  if (!deadcode) {
                     LOAD_ACCUM();
                     if (!jit_append_abs(heap, pc+1)) goto out_of_memory_error;
                  }
                  break;

               case BC_EXT_ADD32:
               case BC_EXT_SUB32:
                  if (!deadcode) {
                     LOAD_ACCUM();
                     if (!jit_append_addsub32(heap, cur_stack-3, get_real_slot(stack, cur_stack-3), get_real_slot(stack, cur_stack-2), op == BC_EXT_ADD32)) goto out_of_memory_error;
                     stack[cur_stack-3].type = SLOT_VALUE;
                     stack[cur_stack-2].type = SLOT_VALUE;
                  }
                  cur_stack--;
                  break;

               case BC_EXT_ADD64:
               case BC_EXT_SUB64:
                  if (!deadcode) {
                     LOAD_ACCUM();
                     if (!jit_append_addsub64(heap, cur_stack-4, get_real_slot(stack, cur_stack-4), get_real_slot(stack, cur_stack-3), get_real_slot(stack, cur_stack-2), op == BC_EXT_ADD64)) goto out_of_memory_error;
                     stack[cur_stack-4].type = SLOT_VALUE;
                     stack[cur_stack-3].type = SLOT_VALUE;
                  }
                  cur_stack -= 2;
                  break;

               case BC_EXT_MUL64:
               case BC_EXT_UMUL64:
                  if (!deadcode) {
                     LOAD_ACCUM();
                     if (!jit_append_mul64(heap, cur_stack-2, get_real_slot(stack, cur_stack-2), op == BC_EXT_MUL64)) goto out_of_memory_error;
                     stack[cur_stack-2].type = SLOT_VALUE;
                     stack[cur_stack-1].type = SLOT_VALUE;
                  }
                  break;

               case BC_EXT_MUL64_LONG:
                  if (!deadcode) {
                     LOAD_ACCUM();
                     if (!jit_append_mul64_long(heap, cur_stack-4, get_real_slot(stack, cur_stack-4), get_real_slot(stack, cur_stack-3), get_real_slot(stack, cur_stack-2))) goto out_of_memory_error;
                     stack[cur_stack-4].type = SLOT_VALUE;
                     stack[cur_stack-3].type = SLOT_VALUE;
                  }
                  cur_stack -= 2;
                  break;

               case BC_EXT_DIV64:
               case BC_EXT_UDIV64:
               case BC_EXT_REM64:
               case BC_EXT_UREM64:
                  if (!deadcode) {
                     LOAD_ACCUM();
                     if (!jit_append_divrem64(heap, cur_stack-4, get_real_slot(stack, cur_stack-4), get_real_slot(stack, cur_stack-3), get_real_slot(stack, cur_stack-2), op == BC_EXT_DIV64 || op == BC_EXT_UDIV64, op == BC_EXT_DIV64 || op == BC_EXT_REM64, pc+1, error_stubs)) goto out_of_memory_error;
                     stack[cur_stack-4].type = SLOT_VALUE;
                     stack[cur_stack-3].type = SLOT_VALUE;
                  }
                  cur_stack -= 2;
                  break;

               case BC_EXT_FLOAT:
                  if (!deadcode) {
                     LOAD_ACCUM();
                     if (!jit_append_float(heap, cur_stack-1)) goto out_of_memory_error;
                  }
                  break;

               case BC_EXT_INT:
                  if (!deadcode) {
                     LOAD_ACCUM();
                     if (!jit_append_int(heap, cur_stack-1)) goto out_of_memory_error;
                  }
                  break;

               case BC_EXT_FABS:
                  if (!deadcode) {
                     LOAD_ACCUM();
                     if (!jit_append_float_abs(heap)) goto out_of_memory_error;
                  }
                  break;

               case BC_EXT_FMIN:
               case BC_EXT_FMAX:
                  if (!deadcode) {
                     LOAD_ACCUM();
                     if (!jit_append_float_minmax(heap, get_real_slot(stack, cur_stack-2), cur_stack-1, op == BC_EXT_FMIN)) goto out_of_memory_error;
                     stack[cur_stack-2].type = SLOT_VALUE;
                  }
                  cur_stack--;
                  break;

               case BC_EXT_FCLAMP:
                  if (!deadcode) {
                     LOAD_ACCUM();
                     if (!jit_append_float_clamp(heap, get_real_slot(stack, cur_stack-3), get_real_slot(stack, cur_stack-2), cur_stack-1)) goto out_of_memory_error;
                     stack[cur_stack-3].type = SLOT_VALUE;
                  }
                  cur_stack -= 2;
                  break;
               
               case BC_EXT_FLOOR:
               case BC_EXT_CEIL:
               case BC_EXT_ROUND:
               case BC_EXT_SQRT:
               case BC_EXT_CBRT:
               case BC_EXT_EXP:
               case BC_EXT_LN:
               case BC_EXT_LOG2:
               case BC_EXT_LOG10:
               case BC_EXT_SIN:
               case BC_EXT_COS:
               case BC_EXT_ASIN:
               case BC_EXT_ACOS:
               case BC_EXT_TAN:
               case BC_EXT_ATAN:
                  if (!deadcode) {
                     LOAD_ACCUM();
                     if (!jit_append_float_func(heap, cur_stack-1, op)) goto out_of_memory_error;
                  }
                  break;

               case BC_EXT_POW:
               case BC_EXT_ATAN2:
                  if (!deadcode) {
                     LOAD_ACCUM();
                     if (!jit_append_float_func2(heap, cur_stack-2, get_real_slot(stack, cur_stack-2), op)) goto out_of_memory_error;
                     stack[cur_stack-2].type = SLOT_VALUE;
                  }
                  cur_stack--;
                  break;

               case BC_EXT_DBL_FLOAT:
                  if (!deadcode) {
                     LOAD_ACCUM();
                     #ifdef JIT_X86
                        CLEAN_INDIRECTS();
                     #endif
                     if (!jit_append_double(heap, cur_stack-2, get_real_slot(stack, cur_stack-2))) goto out_of_memory_error;
                     stack[cur_stack-2].type = SLOT_VALUE;
                     stack[cur_stack-1].type = SLOT_VALUE;
                  }
                  break;

               case BC_EXT_DBL_INT:
                  if (!deadcode) {
                     LOAD_ACCUM();
                     #ifdef JIT_X86
                        CLEAN_INDIRECTS();
                     #endif
                     if (!jit_append_long(heap, cur_stack-2, get_real_slot(stack, cur_stack-2))) goto out_of_memory_error;
                     stack[cur_stack-2].type = SLOT_VALUE;
                     stack[cur_stack-1].type = SLOT_VALUE;
                  }
                  break;

               case BC_EXT_DBL_CONV_DOWN:
                  if (!deadcode) {
                     LOAD_ACCUM();
                     #ifdef JIT_X86
                        CLEAN_INDIRECTS();
                     #endif
                     if (!jit_append_double_conv_down(heap, get_real_slot(stack, cur_stack-2))) goto out_of_memory_error;
                     stack[cur_stack-2].type = SLOT_VALUE;
                  }
                  cur_stack--;
                  break;

               case BC_EXT_DBL_CONV_UP:
                  if (!deadcode) {
                     LOAD_ACCUM();
                     if (!jit_append_double_conv_up(heap, cur_stack-1)) goto out_of_memory_error;
                     stack[cur_stack-1].type = SLOT_VALUE;
                     stack[cur_stack].type = SLOT_VALUE;
                  }
                  cur_stack++;
                  break;

               case BC_EXT_DBL_ADD:
               case BC_EXT_DBL_SUB:
               case BC_EXT_DBL_MUL:
               case BC_EXT_DBL_DIV:
                  if (!deadcode) {
                     LOAD_ACCUM();
                     #ifdef JIT_X86
                        CLEAN_INDIRECTS();
                     #endif
                     if (!jit_append_double_op(heap, cur_stack-4, get_real_slot(stack, cur_stack-4), get_real_slot(stack, cur_stack-3), get_real_slot(stack, cur_stack-2), op)) goto out_of_memory_error;
                     stack[cur_stack-4].type = SLOT_VALUE;
                     stack[cur_stack-3].type = SLOT_VALUE;
                  }
                  cur_stack -= 2;
                  break;

               case BC_EXT_DBL_CMP_LT:
               case BC_EXT_DBL_CMP_LE:
               case BC_EXT_DBL_CMP_GT:
               case BC_EXT_DBL_CMP_GE:
               case BC_EXT_DBL_CMP_EQ:
               case BC_EXT_DBL_CMP_NE:
                  if (!deadcode) {
                     LOAD_ACCUM();
                     #ifdef JIT_X86
                        CLEAN_INDIRECTS();
                     #endif
                     if (!jit_append_double_cmp(heap, get_real_slot(stack, cur_stack-4), get_real_slot(stack, cur_stack-3), get_real_slot(stack, cur_stack-2), op)) goto out_of_memory_error;
                     stack[cur_stack-4].type = SLOT_VALUE;
                  }
                  cur_stack -= 3;
                  break;

               case BC_EXT_DBL_FABS:
                  if (!deadcode) {
                     LOAD_ACCUM();
                     if (!jit_append_double_abs(heap, cur_stack-2, get_real_slot(stack, cur_stack-2))) goto out_of_memory_error;
                     stack[cur_stack-2].type = SLOT_VALUE;
                     stack[cur_stack-1].type = SLOT_VALUE;
                  }
                  break;

               case BC_EXT_DBL_FMIN:
               case BC_EXT_DBL_FMAX:
                  if (!deadcode) {
                     LOAD_ACCUM();
                     #ifdef JIT_X86
                        CLEAN_INDIRECTS();
                     #endif
                     if (!jit_append_double_minmax(heap, cur_stack-4, get_real_slot(stack, cur_stack-4), get_real_slot(stack, cur_stack-3), get_real_slot(stack, cur_stack-2), op == BC_EXT_DBL_FMIN)) goto out_of_memory_error;
                     stack[cur_stack-4].type = SLOT_VALUE;
                     stack[cur_stack-3].type = SLOT_VALUE;
                  }
                  cur_stack -= 2;
                  break;

               case BC_EXT_DBL_FCLAMP:
                  if (!deadcode) {
                     LOAD_ACCUM();
                     #ifdef JIT_X86
                        CLEAN_INDIRECTS();
                     #endif
                     if (!jit_append_double_clamp(heap, cur_stack-6, get_real_slot(stack, cur_stack-6), get_real_slot(stack, cur_stack-5), get_real_slot(stack, cur_stack-4), get_real_slot(stack, cur_stack-3), get_real_slot(stack, cur_stack-2))) goto out_of_memory_error;
                     stack[cur_stack-6].type = SLOT_VALUE;
                     stack[cur_stack-5].type = SLOT_VALUE;
                  }
                  cur_stack -= 4;
                  break;

               case BC_EXT_DBL_FCLAMP_SHORT:
                  if (!deadcode) {
                     LOAD_ACCUM();
                     #ifdef JIT_X86
                        CLEAN_INDIRECTS();
                     #endif
                     if (!jit_append_double_clamp_short(heap, cur_stack-4, get_real_slot(stack, cur_stack-4), get_real_slot(stack, cur_stack-3), get_real_slot(stack, cur_stack-2))) goto out_of_memory_error;
                     stack[cur_stack-4].type = SLOT_VALUE;
                     stack[cur_stack-3].type = SLOT_VALUE;
                  }
                  cur_stack -= 2;
                  break;

               case BC_EXT_DBL_FLOOR:
               case BC_EXT_DBL_CEIL:
               case BC_EXT_DBL_ROUND:
               case BC_EXT_DBL_SQRT:
               case BC_EXT_DBL_CBRT:
               case BC_EXT_DBL_EXP:
               case BC_EXT_DBL_LN:
               case BC_EXT_DBL_LOG2:
               case BC_EXT_DBL_LOG10:
               case BC_EXT_DBL_SIN:
               case BC_EXT_DBL_COS:
               case BC_EXT_DBL_ASIN:
               case BC_EXT_DBL_ACOS:
               case BC_EXT_DBL_TAN:
               case BC_EXT_DBL_ATAN:
                  if (!deadcode) {
                     LOAD_ACCUM();
                     if (!jit_append_double_func(heap, cur_stack-2, get_real_slot(stack, cur_stack-2), op)) goto out_of_memory_error;
                     stack[cur_stack-2].type = SLOT_VALUE;
                     stack[cur_stack-1].type = SLOT_VALUE;
                  }
                  break;

               case BC_EXT_DBL_POW:
               case BC_EXT_DBL_ATAN2:
                  if (!deadcode) {
                     LOAD_ACCUM();
                     #ifdef JIT_X86
                        CLEAN_INDIRECTS();
                     #endif
                     if (!jit_append_double_func2(heap, cur_stack-4, get_real_slot(stack, cur_stack-4), get_real_slot(stack, cur_stack-3), get_real_slot(stack, cur_stack-2), op)) goto out_of_memory_error;
                     stack[cur_stack-4].type = SLOT_VALUE;
                     stack[cur_stack-3].type = SLOT_VALUE;
                  }
                  cur_stack -= 2;
                  break;

               case BC_EXT_IS_INT:
                  if (!deadcode) {
                     LOAD_ACCUM();
                     if (!jit_append_check_int(heap)) goto out_of_memory_error;
                  }
                  break;

               case BC_EXT_IS_FLOAT:
                  if (!deadcode) {
                     LOAD_ACCUM();
                     if (!jit_append_check_float(heap)) goto out_of_memory_error;
                  }
                  break;

               case BC_EXT_IS_ARRAY:
               case BC_EXT_IS_STRING:
               case BC_EXT_IS_HASH:
               case BC_EXT_IS_SHARED:
               case BC_EXT_IS_CONST:
               case BC_EXT_IS_FUNCREF:
               case BC_EXT_IS_WEAKREF:
               case BC_EXT_IS_HANDLE:
                  if (!deadcode) {
                     LOAD_ACCUM();
                     if (!jit_append_check_type(heap, op)) goto out_of_memory_error;
                  }
                  break;

               case BC_EXT_CHECK_TIME_LIMIT:
                  if (!deadcode) {
                     STORE_ACCUM();
                     if (!jit_append_check_time_limit(heap, pc+1)) goto out_of_memory_error;
                  }
                  break;

               default:
                  error = "internal error: unknown extended bytecode";
                  goto error;
            }
            break;

         default:
            if ((op >= BC_CONSTM1 && op <= BC_CONST0+32) || op == BC_CONST0+63 || op == BC_CONST0+64) {
               last_int_const = (int)op - (int)BC_CONST0;
               HANDLE_CONST(last_int_const, 0);
               cur_stack++;
               break;
            }
            if (op >= BC_BRANCH0 && op <= BC_BRANCH0+7) {
               int_val = ((op & 7) << 8) | DATA();
               dest_pc = pc+int_val+1;
               HANDLE_BRANCH(dest_pc);
               cur_stack--;
               MARK_LABEL(dest_pc);
               break;
            }
            if (op >= BC_JUMP0 && op <= BC_JUMP0+7) {
               int_val = ((op & 7) << 8) | DATA();
               dest_pc = pc+int_val+1;
               HANDLE_JUMP(dest_pc);
               MARK_LABEL(dest_pc);
               deadcode = 1;
               break;
            }
            if (op >= BC_STOREM64 && op <= BC_STOREM64+63) {
               HANDLE_STORE(cur_stack + (signed char)op + 0x40);
               cur_stack--;
               break;
            }
            if (op >= BC_LOADM64 && op <= BC_LOADM64+63) {
               HANDLE_LOAD(cur_stack + (signed char)op);
               cur_stack++;
               break;
            }
            error = "internal error: unknown bytecode";
            goto error;
      }
      #undef DATA
      #undef DATA_SBYTE
      #undef DATA_SHORT
      #undef DATA_INT
      #undef DATA_FLOAT
      #undef MARK_LABEL
      #undef STORE_ACCUM
      #undef LOAD_ACCUM
      #undef CLEAN_INDIRECTS
      #undef HANDLE_CONST
      #undef HANDLE_LOAD
      #undef HANDLE_STORE
      #undef HANDLE_BRANCH
      #undef HANDLE_LOOP
      #undef HANDLE_JUMP

      #ifdef JIT_DEBUG
         printf("stack(%d,%d)=", orig_pc, cur_stack);
         for (i=0; i<cur_stack; i++) {
            int has_imm = cur_stack > 0 && stack[cur_stack-1].type == SLOT_IMMEDIATE;
            int is_accum = accum_valid && ((i == cur_stack-1 && !has_imm) || (i == cur_stack-2 && has_imm));
            printf(is_accum? " [" : " ");
            switch (stack[i].type) {
               case SLOT_VALUE:     printf("val"); break;
               case SLOT_INDIRECT:  printf("ind(%d)", stack[i].indirect_slot); break;
               case SLOT_IMMEDIATE: printf("imm"); break;
            }
            if (is_accum) {
               printf("]");
            }
         }
         printf("\n");
      #endif

      if (cur_stack > max_stack) {
         max_stack = cur_stack;
      }
      if (cur_stack < 0 && !deadcode) {
         error = "internal error: stack below zero";
         goto error;
      }
   }
   
   if (switch_table) {
      error = "internal error: pending switch table";
      goto error;
   }
   *max_stack_out = max_stack;
   return NULL;

error:
   while (switch_table) {
      new_switch_table = switch_table->next;
      free(switch_table);
      switch_table = new_switch_table;
   }
   return error;

out_of_memory_error:
   error = "out of memory";
   goto error;

accum_error:
   error = "internal error: bad state of accumulator";
   goto error;
}


static int jit_init(Heap *heap)
{
   Array *arr;
   int i;

#ifdef _WIN32
   SYSTEM_INFO si;
   GetSystemInfo(&si);
   heap->jit_code_cap = si.dwAllocationGranularity;
#else
   heap->jit_code_cap = sysconf(_SC_PAGESIZE);
#endif
   if (heap->jit_code_cap < 1) {
      return 0;
   }

   #if defined(_WIN32)
      heap->jit_code = VirtualAlloc(NULL, heap->jit_code_cap, MEM_COMMIT, PAGE_READWRITE);
   #elif defined(__APPLE__)
      heap->jit_code = mmap(NULL, heap->jit_code_cap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, 0, 0);
   #else
      heap->jit_code = mmap(NULL, heap->jit_code_cap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
   #endif
   if (!heap->jit_code) {
      return 0;
   }

   if (!jit_append_entry_function(heap)) return 0;
   heap->jit_entry_func_end = heap->jit_code_len;

   #ifdef JIT_X86_64
      heap->jit_reinit_regs_func = heap->jit_code_len;
      if (!emit_reinit_regs(heap, 1)) return 0;
   #endif

   heap->jit_error_code = heap->jit_code_len;
   if (!jit_append_error_code(heap)) return 0;

   heap->jit_invalid_array_stack_error_code = heap->jit_code_len;
   if (!jit_append_stack_error_stub(heap, JIT_ERROR_INVALID_ARRAY)) return 0;

   heap->jit_out_of_bounds_stack_error_code = heap->jit_code_len;
   if (!jit_append_stack_error_stub(heap, JIT_ERROR_OUT_OF_BOUNDS)) return 0;

   heap->jit_invalid_shared_stack_error_code = heap->jit_code_len;
   if (!jit_append_stack_error_stub(heap, JIT_ERROR_INVALID_SHARED)) return 0;

   heap->jit_out_of_memory_stack_error_code = heap->jit_code_len;
   if (!jit_append_stack_error_stub(heap, JIT_ERROR_OUT_OF_MEMORY)) return 0;

   heap->jit_upgrade_code[0] = heap->jit_code_len;
   if (!jit_append_array_upgrade_code(heap, 0, 0, 0)) return 0;

   heap->jit_upgrade_code[1] = heap->jit_code_len;
   if (!jit_append_array_upgrade_code(heap, 1, 0, 0)) return 0;

   heap->jit_upgrade_code[2] = heap->jit_code_len;
   if (!jit_append_array_upgrade_code(heap, 0, 1, 0)) return 0;

   heap->jit_upgrade_code[3] = heap->jit_code_len;
   if (!jit_append_array_upgrade_code(heap, 1, 1, 0)) return 0;

   heap->jit_upgrade_code[4] = heap->jit_code_len;
   if (!jit_append_array_upgrade_code(heap, 0, 1, 1)) return 0;

   heap->jit_upgrade_code[5] = heap->jit_code_len;
   if (!jit_append_array_upgrade_code(heap, 1, 1, 1)) return 0;

#ifdef JIT_X86
   if (!jit_align(heap, 4)) return 0;
   heap->jit_array_get_func_base = heap->jit_code_len;
   jmp____rel32(heap->jit_invalid_array_stack_error_code - heap->jit_code_len - 4);

   #define FUNC(name, type) \
      if (!jit_align(heap, 4)) return 0; \
      heap->name = (heap->jit_code_len - heap->jit_array_get_func_base) / 4; \
      if (!jit_append_array_get_func(heap, type)) return 0

   FUNC(jit_array_get_byte_func, ARR_BYTE);
   FUNC(jit_array_get_short_func, ARR_SHORT);
   FUNC(jit_array_get_int_func, ARR_INT);

   #undef FUNC

   if (!jit_align(heap, 4)) return 0;
   heap->jit_array_set_func_base = heap->jit_code_len;
   jmp____rel32(heap->jit_invalid_array_stack_error_code - heap->jit_code_len - 4);

   if (!jit_align(heap, 4)) return 0;
   heap->jit_array_set_const_string = (heap->jit_code_len - heap->jit_array_set_func_base) / 4;
   if (!jit_append_stack_error_stub(heap, JIT_ERROR_CONST_STRING)) return 0;

   #define FUNC(name, type, flag, shared) \
      if (!jit_align(heap, 4)) return 0; \
      heap->name[flag] = (heap->jit_code_len - heap->jit_array_set_func_base) / 4; \
      if (!jit_append_array_set_func(heap, type, flag, shared, 0)) return 0

   #define FUNC2(name, type, shared) \
      FUNC(name, type, 0, shared); \
      FUNC(name, type, 1, shared)

   FUNC2(jit_array_set_byte_func, ARR_BYTE, 0);
   FUNC2(jit_array_set_short_func, ARR_SHORT, 0);
   FUNC2(jit_array_set_int_func, ARR_INT, 0);

   FUNC2(jit_shared_set_byte_func, ARR_BYTE, 1);
   FUNC2(jit_shared_set_short_func, ARR_SHORT, 1);
   FUNC2(jit_shared_set_int_func, ARR_INT, 1);

   #undef FUNC
   #undef FUNC2

   if (!jit_align(heap, 4)) return 0;
   heap->jit_array_append_func_base = heap->jit_code_len;
   jmp____rel32(heap->jit_invalid_array_stack_error_code - heap->jit_code_len - 4);

   if (!jit_align(heap, 4)) return 0;
   heap->jit_array_append_const_string = (heap->jit_code_len - heap->jit_array_append_func_base) / 4;
   if (!jit_append_stack_error_stub(heap, JIT_ERROR_CONST_STRING)) return 0;

   if (!jit_align(heap, 4)) return 0;
   heap->jit_array_append_shared = (heap->jit_code_len - heap->jit_array_append_func_base) / 4;
   if (!jit_append_stack_error_stub(heap, JIT_ERROR_INVALID_SHARED)) return 0;

   #define FUNC(name, type, flag, shared) \
      if (!jit_align(heap, 4)) return 0; \
      heap->name[flag] = (heap->jit_code_len - heap->jit_array_append_func_base) / 4; \
      if (!jit_append_array_set_func(heap, type, flag, shared, 1)) return 0

   #define FUNC2(name, type, shared) \
      FUNC(name, type, 0, shared); \
      FUNC(name, type, 1, shared)

   FUNC2(jit_array_append_byte_func, ARR_BYTE, 0);
   FUNC2(jit_array_append_short_func, ARR_SHORT, 0);
   FUNC2(jit_array_append_int_func, ARR_INT, 0);

   #undef FUNC
   #undef FUNC2

   jit_update_heap_refs(heap);
   
#else
   #error "not implemented"
#endif

   for (i=1; i<heap->size; i++) {
      arr = &heap->data[i];
      if (arr->len == -1 || arr->hash_slots >= 0) continue;

      if (arr->type == ARR_BYTE) {
         heap->jit_array_get_funcs[i] = heap->jit_array_get_byte_func;
         heap->jit_array_set_funcs[i*2+0] = heap->jit_array_set_byte_func[0];
         heap->jit_array_set_funcs[i*2+1] = heap->jit_array_set_byte_func[1];
         heap->jit_array_append_funcs[i*2+0] = heap->jit_array_append_byte_func[0];
         heap->jit_array_append_funcs[i*2+1] = heap->jit_array_append_byte_func[1];
      }
      else if (arr->type == ARR_SHORT) {
         heap->jit_array_get_funcs[i] = heap->jit_array_get_short_func;
         heap->jit_array_set_funcs[i*2+0] = heap->jit_array_set_short_func[0];
         heap->jit_array_set_funcs[i*2+1] = heap->jit_array_set_short_func[1];
         heap->jit_array_append_funcs[i*2+0] = heap->jit_array_append_short_func[0];
         heap->jit_array_append_funcs[i*2+1] = heap->jit_array_append_short_func[1];
      }
      else if (arr->type == ARR_INT) {
         heap->jit_array_get_funcs[i] = heap->jit_array_get_int_func;
         heap->jit_array_set_funcs[i*2+0] = heap->jit_array_set_int_func[0];
         heap->jit_array_set_funcs[i*2+1] = heap->jit_array_set_int_func[1];
         heap->jit_array_append_funcs[i*2+0] = heap->jit_array_append_int_func[0];
         heap->jit_array_append_funcs[i*2+1] = heap->jit_array_append_int_func[1];
      }
      if (arr->is_const) {
         heap->jit_array_set_funcs[i*2+0] = heap->jit_array_set_const_string;
         heap->jit_array_set_funcs[i*2+1] = heap->jit_array_set_const_string;
         heap->jit_array_append_funcs[i*2+0] = heap->jit_array_append_const_string;
         heap->jit_array_append_funcs[i*2+1] = heap->jit_array_append_const_string;
      }
      if (arr->is_shared) {
         if (arr->type == ARR_BYTE) {
            heap->jit_array_set_funcs[i*2+0] = heap->jit_shared_set_byte_func[0];
            heap->jit_array_set_funcs[i*2+1] = heap->jit_shared_set_byte_func[1];
         }
         else if (arr->type == ARR_SHORT) {
            heap->jit_array_set_funcs[i*2+0] = heap->jit_shared_set_short_func[0];
            heap->jit_array_set_funcs[i*2+1] = heap->jit_shared_set_short_func[1];
         }
         else if (arr->type == ARR_INT) {
            heap->jit_array_set_funcs[i*2+0] = heap->jit_shared_set_int_func[0];
            heap->jit_array_set_funcs[i*2+1] = heap->jit_shared_set_int_func[1];
         }
         heap->jit_array_append_funcs[i*2+0] = heap->jit_array_append_shared;
         heap->jit_array_append_funcs[i*2+1] = heap->jit_array_append_shared;
      }
   }

   return 1;
}


static const char *jit_compile(Heap *heap, int func_start)
{
   Function *func;
   const char *error = NULL;
   uint16_t *labels = NULL;
   int *addrs = NULL;
   uint32_t *jump_targets = NULL;
   DynArray forward_refs, func_refs, error_stubs;
   StackEntry *stack = NULL;
   int i, j, start, end, max_stack, max_code_size, max_total_stack, max_num_params, orig_code_len, orig_pc_mappings, orig_heap_data_refs, orig_array_get_refs, orig_array_set_refs, orig_array_append_refs, orig_length_refs, orig_adjustments;

   memset(&forward_refs, 0, sizeof(DynArray));
   memset(&func_refs, 0, sizeof(DynArray));
   memset(&error_stubs, 0, sizeof(DynArray));

   if (!heap->jit_code) {
      if (!jit_init(heap)) {
         return "out of memory";
      }
   }

   jit_update_exec(heap, 0);
   orig_code_len = heap->jit_code_len;
   orig_pc_mappings = heap->jit_pc_mappings.len;
   orig_heap_data_refs = heap->jit_heap_data_refs.len;
   orig_array_get_refs = heap->jit_array_get_refs.len;
   orig_array_set_refs = heap->jit_array_set_refs.len;
   orig_array_append_refs = heap->jit_array_append_refs.len;
   orig_length_refs = heap->jit_length_refs.len;
   orig_adjustments = heap->jit_adjustments.len;

   max_code_size = 0;
   max_total_stack = 0;
   max_num_params = 0;
   for (i=func_start; i<heap->functions.len; i++) {
      func = heap->functions.data[i];
      start = func->addr;
      end = i+1 < heap->functions.len? ((Function *)heap->functions.data[i+1])->addr : heap->bytecode_size;
      if (end - start > max_code_size) {
         max_code_size = end - start;
      }
      if (func->max_stack > max_total_stack) {
         max_total_stack = func->max_stack;
      }
      if (func->num_params > max_num_params) {
         max_num_params = func->num_params;
      }
   }

   labels = malloc_array(max_code_size, sizeof(uint16_t));
   addrs = malloc_array(max_code_size, sizeof(int));
   jump_targets = malloc_array((max_code_size+31) >> 5, sizeof(uint32_t));
   stack = malloc_array(max_total_stack + max_num_params, sizeof(StackEntry));
   if (!labels || !addrs || !stack) {
      error = "out of memory";
      goto error;
   }

   for (i=0; i<max_num_params; i++) {
      stack[i].type = SLOT_VALUE;
   }

   for (i=func_start; i<heap->functions.len; i++) {
      func = heap->functions.data[i];
      start = func->addr;
      end = i+1 < heap->functions.len? ((Function *)heap->functions.data[i+1])->addr : heap->bytecode_size;
      for (j=0; j<end-start; j++) {
         labels[j] = 0xFFFF;
      }
      memset(jump_targets, 0, ((end-start+31) >> 5) * sizeof(uint32_t));
      #ifdef JIT_DEBUG
         JIT_APPEND(3, 0x90,0x90,0x90);
         printf("%s\n", fixscript_dump_code(heap, func->script, string_hash_find_name(&func->script->functions, func)));
      #endif
      func->jit_addr = heap->jit_code_len;
      jit_scan_jump_targets(heap, start, end, jump_targets);
      error = jit_compile_function(heap, start, end, func->num_params, labels, addrs, jump_targets, &forward_refs, &func_refs, &error_stubs, stack + max_num_params, &max_stack);
      #ifdef JIT_DEBUG
         fflush(stdout);
      #endif
      if (error) {
         goto error;
      }
      if (forward_refs.len != 0) {
         error = "internal error: unmatched forward label";
         goto error;
      }
      if (max_stack > func->max_stack) {
         // note: dead code analysis is not done by bytecode output so it can overestimate
         error = "internal error: max stack mismatch";
         goto error;
      }
   }

   for (i=0; i<func_refs.len; i+=2) {
      jit_update_branch(heap, (intptr_t)func_refs.data[i+0], ((Function *)func_refs.data[i+1])->jit_addr);
   }

   for (i=0; i<error_stubs.len; i+=2) {
      jit_update_branch(heap, (intptr_t)error_stubs.data[i+0], heap->jit_code_len);

      #ifdef JIT_X86
         mov____edx__imm((intptr_t)error_stubs.data[i+1]);
         jmp____rel32(heap->jit_error_code - heap->jit_code_len - 4);
      #else
         #error "not implemented"
      #endif
   }

   jit_update_heap_refs(heap);

   #ifdef JIT_DEBUG
      {
         static int cnt = 0;
         FILE *f;
         char buf[64];
         sprintf(buf, "__jit_output_%d.bin", cnt++);
         f = fopen(buf, "wb");
         fwrite(heap->jit_code, heap->jit_code_len, 1, f);
         fclose(f);
      }
   #endif

error:
   if (error) {
      heap->jit_code_len = orig_code_len;
      heap->jit_pc_mappings.len = orig_pc_mappings;
      heap->jit_heap_data_refs.len = orig_heap_data_refs;
      heap->jit_array_get_refs.len = orig_array_get_refs;
      heap->jit_array_set_refs.len = orig_array_set_refs;
      heap->jit_array_append_refs.len = orig_array_append_refs;
      heap->jit_length_refs.len = orig_length_refs;
      heap->jit_adjustments.len = orig_adjustments;
   }
   free(labels);
   free(addrs);
   free(jump_targets);
   free(forward_refs.data);
   free(func_refs.data);
   free(error_stubs.data);
   free(stack);
   return error;
}


static void jit_update_heap_refs(Heap *heap)
{
   void *ptr;
   intptr_t value;
   int i, offset;

   jit_update_exec(heap, 0);

   for (i=0; i<heap->jit_heap_data_refs.len; i++) {
      offset = (intptr_t)heap->jit_heap_data_refs.data[i];
      value = (intptr_t)heap->data;
      memcpy(heap->jit_code + (offset - sizeof(intptr_t)), &value, sizeof(intptr_t));
   }

   for (i=0; i<heap->jit_array_get_refs.len; i++) {
      offset = (intptr_t)heap->jit_array_get_refs.data[i];
      jit_update_array_get(heap, heap->jit_code + offset);
   }

   for (i=0; i<heap->jit_array_set_refs.len; i++) {
      offset = (intptr_t)heap->jit_array_set_refs.data[i];
      jit_update_array_set(heap, heap->jit_code + offset, 0);
   }

   for (i=0; i<heap->jit_array_append_refs.len; i++) {
      offset = (intptr_t)heap->jit_array_append_refs.data[i];
      jit_update_array_set(heap, heap->jit_code + offset, 1);
   }

   for (i=0; i<heap->jit_length_refs.len; i++) {
      offset = (intptr_t)heap->jit_length_refs.data[i];
      jit_update_length(heap, heap->jit_code + offset);
   }

   for (i=0; i<heap->jit_adjustments.len; i+=2) {
      offset = (intptr_t)heap->jit_adjustments.data[i+0];
      if (offset >= 0) {
         ptr = ((JitAdjPtrFunc)heap->jit_adjustments.data[i+1])(heap);
         memcpy(heap->jit_code + offset - sizeof(void *), &ptr, sizeof(void *));
      }
      else {
         offset = -offset;
         value = ((JitAdjIntFunc)heap->jit_adjustments.data[i+1])(heap);
         memcpy(heap->jit_code + offset - 4, &value, 4);
      }
   }
}

#endif /* FIXSCRIPT_NO_JIT */
