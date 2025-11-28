/*
 * FixBrowser v0.4 - https://www.fixbrowser.org/
 * Copyright (c) 2018-2025 Martin Dvorak <jezek2@advel.cz>
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
#include <limits.h>
#include "fixscript.h"
#include "util.h"


static Value mul(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value val1_val = params[0];
   Value val2_val = params[1];
   Value result_val = params[2];
   Value *values1 = NULL, *values2 = NULL, *result = NULL;
   Value err_val;
   uint32_t mult, mul_carry, add_carry;
   uint64_t mul_res, add_res;
   int i, j, idx, err, len1, len2, result_len;

   err = fixscript_get_array_length(heap, val1_val, &len1);
   if (err) goto error;
   err = fixscript_get_array_length(heap, val2_val, &len2);
   if (err) goto error;
   if (len1 < 2 || len2 < 2) { err = FIXSCRIPT_ERR_OUT_OF_BOUNDS; goto error; }

   result_len = len1 + len2 - 2;

   values1 = malloc_array(len1, sizeof(Value));
   values2 = malloc_array(len2, sizeof(Value));
   result = calloc(result_len, sizeof(Value));
   if (!values1 || !values2 || !result) {
      err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
      goto error;
   }

   err = fixscript_get_array_range(heap, val1_val, 0, len1, values1);
   if (err != FIXSCRIPT_SUCCESS) goto error;

   err = fixscript_get_array_range(heap, val2_val, 0, len2, values2);
   if (err != FIXSCRIPT_SUCCESS) goto error;

   for (i=1; i<len1-1; i++) {
      mult = values1[i].value;
      if (!mult) continue;
      mul_carry = 0;
      add_carry = 0;
      idx = i;
      for (j=1; j<len2; j++) {
         mul_res = (uint64_t)((uint32_t)values2[j].value) * mult;
         add_res = (uint64_t)(uint32_t)mul_res + mul_carry;
         mul_carry = (uint64_t)(uint32_t)(mul_res >> 32) + ((add_res >> 32) & 1);

         add_res = (uint64_t)(uint32_t)result[idx].value + (uint32_t)add_res + add_carry;
         result[idx++].value = (uint32_t)add_res;
         add_carry = (add_res >> 32) & 1;
      }
   }

   while (result_len > 2 && !result[result_len-2].value) {
      result_len--;
   }

   err = fixscript_set_array_length(heap, result_val, result_len);
   if (err != FIXSCRIPT_SUCCESS) goto error;

   err = fixscript_set_array_range(heap, result_val, 1, result_len-1, &result[1]);
   if (err != FIXSCRIPT_SUCCESS) goto error;
   
   err = FIXSCRIPT_SUCCESS;

error:
   free(values1);
   free(values2);
   free(result);
   if (err != FIXSCRIPT_SUCCESS) {
      fixscript_error(heap, &err_val, err);
      return err_val;
   }
   return fixscript_int(0);
}


static int shl1(Value *values, int *len, int max_len)
{
   int i, c, prev_carry = 0;

   for (i=1; i<*len; i++) {
      c = ((uint32_t)values[i].value) >> 31;
      values[i].value = (values[i].value << 1) | prev_carry;
      prev_carry = c;
   }

   if (values[(*len)-1].value) {
      if (*len >= max_len) {
         return FIXSCRIPT_ERR_OUT_OF_BOUNDS;
      }
      values[(*len)++] = fixscript_int(0);
   }

   return FIXSCRIPT_SUCCESS;
}


static int ge(Value *values1, int len1, Value *values2, int len2)
{
   int idx;

   if (len1 < len2) return 0;
   if (len1 > len2) return 1;
   if (len1 == 2) return 1;

   idx = len1 - 2;
   while (idx > 1 && values1[idx].value == values2[idx].value) {
      idx--;
   }

   return ((uint32_t)values1[idx].value) >= ((uint32_t)values2[idx].value);
}


static void sub(Value *values1, int *len1, Value *values2, int len2)
{
   uint32_t d1, d2, b, prev_borrow;
   uint64_t sub;
   int i;

   prev_borrow = 0;
   for (i=1; i<(*len1)-1; i++) {
      d1 = values1[i].value;
      d2 = values2[MIN(i, len2-1)].value;
      sub = (uint64_t)d1 - (uint64_t)d2 - prev_borrow;
      b = (sub >> 32) & 1;
      prev_borrow = b;
      values1[i].value = sub;
   }

   while (*len1 > 2 && !values1[(*len1)-2].value) {
      (*len1)--;
   }
}


static Value divrem(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value val1_val = params[0];
   Value val2_val = params[1];
   Value quot_val = params[2];
   Value rem_val = params[3];
   Value *values1 = NULL, *values2 = NULL, *quot = NULL, *rem = NULL;
   Value err_val;
   int64_t bit_len;
   int i, err, len1, len2, quot_len, rem_len;

   err = fixscript_get_array_length(heap, val1_val, &len1);
   if (err) goto error;
   err = fixscript_get_array_length(heap, val2_val, &len2);
   if (err) goto error;
   if (len1 < 2 || len2 < 2) { err = FIXSCRIPT_ERR_OUT_OF_BOUNDS; goto error; }

   values1 = malloc_array(len1, sizeof(Value));
   values2 = malloc_array(len2, sizeof(Value));
   quot = calloc(len1, sizeof(Value));
   rem = calloc(len1, sizeof(Value));
   if (!values1 || !values2 || !quot || !rem) {
      err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
      goto error;
   }

   quot_len = len1;
   rem_len = 2;

   err = fixscript_get_array_range(heap, val1_val, 0, len1, values1);
   if (err != FIXSCRIPT_SUCCESS) goto error;

   err = fixscript_get_array_range(heap, val2_val, 0, len2, values2);
   if (err != FIXSCRIPT_SUCCESS) goto error;

   bit_len = (int64_t)(len1-1) * 32 - 1;
   if (bit_len > INT_MAX) { err = FIXSCRIPT_ERR_OUT_OF_BOUNDS; goto error; }

   for (i=bit_len; i>=32; i--) {
      err = shl1(rem, &rem_len, len1);
      if (err != FIXSCRIPT_SUCCESS) goto error;

      rem[1].value |= (((uint32_t)values1[i >> 5].value) >> (i & 31)) & 1;
      if (rem[rem_len-1].value) {
         if (rem_len >= len1) {
            err = FIXSCRIPT_ERR_OUT_OF_BOUNDS;
            goto error;
         }
         rem[rem_len++] = fixscript_int(0);
      }

      if (ge(rem, rem_len, values2, len2)) {
         sub(rem, &rem_len, values2, len2);
         quot[i >> 5].value |= 1 << (i & 31);
      }
   }

   while (quot_len > 2 && !quot[quot_len-2].value) {
      quot_len--;
   }

   err = fixscript_set_array_length(heap, quot_val, quot_len);
   if (err != FIXSCRIPT_SUCCESS) goto error;

   err = fixscript_set_array_length(heap, rem_val, rem_len);
   if (err != FIXSCRIPT_SUCCESS) goto error;

   err = fixscript_set_array_range(heap, quot_val, 1, quot_len-1, &quot[1]);
   if (err != FIXSCRIPT_SUCCESS) goto error;

   err = fixscript_set_array_range(heap, rem_val, 1, rem_len-1, &rem[1]);
   if (err != FIXSCRIPT_SUCCESS) goto error;
   
   err = FIXSCRIPT_SUCCESS;

error:
   free(values1);
   free(values2);
   free(quot);
   free(rem);
   if (err != FIXSCRIPT_SUCCESS) {
      fixscript_error(heap, &err_val, err);
      return err_val;
   }
   return fixscript_int(0);
}


void register_bigint_functions(Heap *heap)
{
   fixscript_register_native_func(heap, "native_bigint_mul#3", mul, NULL);
   fixscript_register_native_func(heap, "native_bigint_divrem#4", divrem, NULL);
}
