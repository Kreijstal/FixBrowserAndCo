/*
 * This file was written in 2024 by Martin Dvorak <jezek2@advel.cz>
 * You can download latest version at http://public-domain.advel.cz/
 *
 * To the extent possible under law, the author(s) have dedicated all
 * copyright and related and neighboring rights to this file to the
 * public domain worldwide. This software is distributed without any
 * warranty.
 *
 * You should have received a copy of the CC0 Public Domain Dedication
 * along with this software. If not, see:
 * http://creativecommons.org/publicdomain/zero/1.0/ 
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <wasm-support.h>
#include "fixscript.h"
#include "scripts.h"

#define STR2(x) #x
#define STR(x) STR2(x)

static Heap *heap;

#define FUNC_VAR(name) static Value precise_##name##_func

FUNC_VAR(create_variable);
FUNC_VAR(get);
FUNC_VAR(set);
FUNC_VAR(fill);
FUNC_VAR(fill_inc);
FUNC_VAR(add);
FUNC_VAR(sub);
FUNC_VAR(mul);
FUNC_VAR(div);
FUNC_VAR(log);
FUNC_VAR(log2);
FUNC_VAR(log10);
FUNC_VAR(exp);
FUNC_VAR(pow);
FUNC_VAR(sqrt);
FUNC_VAR(cbrt);
FUNC_VAR(sin);
FUNC_VAR(cos);
FUNC_VAR(tan);
FUNC_VAR(asin);
FUNC_VAR(acos);
FUNC_VAR(atan);
FUNC_VAR(min);
FUNC_VAR(max);


#define DOUBLE_OP1(name)                                                                                                  \
void double_##name(double *src, double *dest, int width) EXPORT(STR(double_##name))                                       \
{                                                                                                                         \
   int i;                                                                                                                 \
                                                                                                                          \
   for (i=0; i<width; i++) {                                                                                              \
      *dest++ = name(*src++);                                                                                             \
   }                                                                                                                      \
}

#define DOUBLE_OP2(name, op)                                                                                              \
void double_##name(double *src1, double *src2, double *dest, int width) EXPORT(STR(double_##name))                        \
{                                                                                                                         \
   double a, b;                                                                                                           \
   int i;                                                                                                                 \
                                                                                                                          \
   for (i=0; i<width; i++) {                                                                                              \
      a = (*src1++);                                                                                                      \
      b = (*src2++);                                                                                                      \
      *dest++ = op;                                                                                                       \
   }                                                                                                                      \
}

#define PRECISE_OP1(name)                                                                                                 \
void precise_##name(int src, int dest) EXPORT(STR(precise_##name))                                                        \
{                                                                                                                         \
   Value error;                                                                                                           \
                                                                                                                          \
   fixscript_call(heap, precise_##name##_func, 2, &error, (Value) { src, 1 }, (Value) { dest, 1 });                       \
   if (error.value) {                                                                                                     \
      fixscript_dump_value(heap, error, 1);                                                                               \
      return;                                                                                                             \
   }                                                                                                                      \
}

#define PRECISE_OP2(name)                                                                                                 \
void precise_##name(int src1, int src2, int dest) EXPORT(STR(precise_##name))                                             \
{                                                                                                                         \
   Value error;                                                                                                           \
                                                                                                                          \
   fixscript_call(heap, precise_##name##_func, 3, &error, (Value) { src1, 1 }, (Value) { src2, 1 }, (Value) { dest, 1 }); \
   if (error.value) {                                                                                                     \
      fixscript_dump_value(heap, error, 1);                                                                               \
      return;                                                                                                             \
   }                                                                                                                      \
}

DOUBLE_OP2(add, a + b)
DOUBLE_OP2(sub, a - b)
DOUBLE_OP2(mul, a * b)
DOUBLE_OP2(div, a / b)
DOUBLE_OP1(log)
DOUBLE_OP1(log2)
DOUBLE_OP1(log10)
DOUBLE_OP1(exp)
DOUBLE_OP2(pow, pow(a, b))
DOUBLE_OP1(sqrt)
DOUBLE_OP1(cbrt)
DOUBLE_OP1(sin)
DOUBLE_OP1(cos)
DOUBLE_OP1(tan)
DOUBLE_OP1(asin)
DOUBLE_OP1(acos)
DOUBLE_OP1(atan)
DOUBLE_OP2(min, fmin(a, b))
DOUBLE_OP2(max, fmax(a, b))

PRECISE_OP2(add)
PRECISE_OP2(sub)
PRECISE_OP2(mul)
PRECISE_OP2(div)
PRECISE_OP1(log)
PRECISE_OP1(log2)
PRECISE_OP1(log10)
PRECISE_OP1(exp)
PRECISE_OP2(pow)
PRECISE_OP1(sqrt)
PRECISE_OP1(cbrt)
PRECISE_OP1(sin)
PRECISE_OP1(cos)
PRECISE_OP1(tan)
PRECISE_OP1(asin)
PRECISE_OP1(acos)
PRECISE_OP1(atan)
PRECISE_OP2(min)
PRECISE_OP2(max)


int precise_create_variable(int width) EXPORT("precise_create_variable")
{
   Value ret, error;

   ret = fixscript_call(heap, precise_create_variable_func, 1, &error, fixscript_int(width));
   if (error.value) {
      fixscript_dump_value(heap, error, 1);
      return 0;
   }
   fixscript_ref(heap, ret);
   return ret.value;
}


void precise_destroy_variable(int var) EXPORT("precise_destroy_variable")
{
   fixscript_unref(heap, (Value) { var, 1 });
}


double *precise_get(int var, int width) EXPORT("precise_get")
{
   double *buf;
   Value ret, error;

   buf = malloc(width*8);
   ret = fixscript_create_or_get_shared_array(heap, -1, buf, width*2, 4, NULL, NULL, NULL);
   fixscript_call(heap, precise_get_func, 2, &error, (Value) { var, 1 }, ret);
   if (error.value) {
      fixscript_dump_value(heap, error, 1);
      return 0;
   }
   return buf;
}


void precise_set(int var, int width, double *values) EXPORT("precise_set")
{
   Value arr, error;

   arr = fixscript_create_or_get_shared_array(heap, -1, values, width*2, 4, NULL, NULL, NULL);
   fixscript_call(heap, precise_set_func, 2, &error, arr, (Value) { var, 1 });
   if (error.value) {
      fixscript_dump_value(heap, error, 1);
      return;
   }
}


void precise_fill(int var, double value) EXPORT("precise_fill")
{
   union {
      double d;
      int i[2];
   } u;
   Value error;

   u.d = value;
   fixscript_call(heap, precise_fill_func, 3, &error, (Value) { var, 1 }, fixscript_int(u.i[0]), fixscript_int(u.i[1]));
   if (error.value) {
      fixscript_dump_value(heap, error, 1);
      return;
   }
}


void precise_fill_inc(int var, double value, double inc, int offset) EXPORT("precise_fill_inc")
{
   union {
      double d;
      int i[2];
   } u1, u2;
   Value error;

   u1.d = value;
   u2.d = inc;
   fixscript_call(heap, precise_fill_inc_func, 6, &error, (Value) { var, 1 }, fixscript_int(u1.i[0]), fixscript_int(u1.i[1]), fixscript_int(u2.i[0]), fixscript_int(u2.i[1]), fixscript_int(offset));
   if (error.value) {
      fixscript_dump_value(heap, error, 1);
      return;
   }
}


int main(int argc, char **argv)
{
   Script *script;
   Value error;

   heap = fixscript_create_heap();

   script = fixscript_load_embed(heap, "main", &error, scripts);
   if (!script) {
      fprintf(stderr, "%s\n", fixscript_get_compiler_error(heap, error));
      fflush(stderr);
      return 0;
   }

   fixscript_run(heap, script, "init#0", &error);
   if (error.value) {
      fixscript_dump_value(heap, error, 1);
      return 0;
   }

   #define GET_FUNC(name, num_params) \
      precise_##name##_func = fixscript_get_function(heap, script, "precise_" STR(name) "#" STR(num_params))

   GET_FUNC(create_variable, 1);
   GET_FUNC(get, 2);
   GET_FUNC(set, 2);
   GET_FUNC(fill, 3);
   GET_FUNC(fill_inc, 6);
   GET_FUNC(add, 3);
   GET_FUNC(sub, 3);
   GET_FUNC(mul, 3);
   GET_FUNC(div, 3);
   GET_FUNC(log, 2);
   GET_FUNC(log2, 2);
   GET_FUNC(log10, 2);
   GET_FUNC(exp, 2);
   GET_FUNC(pow, 3);
   GET_FUNC(sqrt, 2);
   GET_FUNC(cbrt, 2);
   GET_FUNC(sin, 2);
   GET_FUNC(cos, 2);
   GET_FUNC(tan, 2);
   GET_FUNC(asin, 2);
   GET_FUNC(acos, 2);
   GET_FUNC(atan, 2);
   GET_FUNC(min, 3);
   GET_FUNC(max, 3);

   #undef GET_FUNC

   return 0;
}
