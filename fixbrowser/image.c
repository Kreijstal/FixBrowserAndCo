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
#include "fiximage.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#include "stb_image.h"

typedef struct {
   SharedArrayHandle *sah;
   stbi__context s;
   stbi__gif g;
   Value img;
} GIF;

#define NUM_HANDLE_TYPES 1
#define HANDLE_TYPE_GIF (handles_offset+0)

static volatile int handles_offset = 0;


static uint32_t div255(uint32_t a)
{
   return ((a << 8) + a + 255) >> 16;
}


static void *gif_handler(Heap *heap, int op, void *p1, void *p2)
{
   GIF *gif = p1;

   switch (op) {
      case HANDLE_OP_FREE:
         free(gif->g.out);
         free(gif->g.history);
         free(gif->g.background);
         fixscript_unref_shared_array(gif->sah);
         fixscript_unref(heap, gif->img);
         free(gif);
         break;
   }

   return NULL;
}


static Value gif_create(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   GIF *gif;
   SharedArrayHandle *sah;
   Value ret;
   void *ptr;
   int len, elem_size;

   sah = fixscript_get_shared_array_handle(heap, params[0], -1, NULL);
   if (!sah) {
      *error = fixscript_create_error_string(heap, "invalid shared array");
      return fixscript_int(0);
   }

   ptr = fixscript_get_shared_array_handle_data(sah, &len, &elem_size, NULL, -1, NULL);
   if (elem_size != 1) {
      *error = fixscript_create_error_string(heap, "invalid shared array");
      return fixscript_int(0);
   }

   gif = calloc(1, sizeof(GIF));
   if (!gif) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   stbi__start_mem(&gif->s, ptr, len);
   if (!stbi__gif_test(&gif->s)) {
      free(gif);
      *error = fixscript_create_error_string(heap, "not GIF format");
      return fixscript_int(0);
   }

   gif->sah = sah;
   fixscript_ref_shared_array(gif->sah);

   ret = fixscript_create_value_handle(heap, HANDLE_TYPE_GIF, gif, gif_handler);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return ret;
}


static Value gif_reset(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   GIF *gif;
   void *ptr;
   int len;

   gif = fixscript_get_handle(heap, params[0], HANDLE_TYPE_GIF, NULL);
   if (!gif) {
      *error = fixscript_create_error_string(heap, "invalid GIF handle");
      return fixscript_int(0);
   }

   free(gif->g.out);
   free(gif->g.history);
   free(gif->g.background);
   memset(&gif->g, 0, sizeof(gif->g));
   ptr = fixscript_get_shared_array_handle_data(gif->sah, &len, NULL, NULL, -1, NULL);
   stbi__start_mem(&gif->s, ptr, len);
   stbi__gif_test(&gif->s);
   return fixscript_int(0);
}


static Value gif_next(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   GIF *gif;
   stbi_uc *u = NULL;
   uint32_t *pixels = NULL;
   char buf[128];
   int i, comp, r, g, b, a;

   gif = fixscript_get_handle(heap, params[0], HANDLE_TYPE_GIF, NULL);
   if (!gif) {
      *error = fixscript_create_error_string(heap, "invalid GIF handle");
      return fixscript_int(0);
   }

   u = stbi__gif_load_next(&gif->s, &gif->g, &comp, 4, /*two_back*/NULL);
   if (!gif->img.value && gif->g.out) {
      gif->img = fiximage_create(heap, gif->g.w, gif->g.h);
      if (!gif->img.value) {
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
      fixscript_ref(heap, gif->img);
   }
   if (u == (stbi_uc *)&gif->s) {
      return fixscript_int(0);
   }
   
   if (!u) {
      snprintf(buf, sizeof(buf), "decode error (%s)", stbi_failure_reason());
      *error = fixscript_create_error_string(heap, buf);
      return fixscript_int(0);
   }

   fiximage_get_data(heap, gif->img, NULL, NULL, NULL, &pixels, NULL, NULL);
   for (i=0; i<gif->g.w*gif->g.h; i++) {
      r = gif->g.out[i*4+0];
      g = gif->g.out[i*4+1];
      b = gif->g.out[i*4+2];
      a = gif->g.out[i*4+3];
      pixels[i] = (a<<24) | (r<<16) | (g<<8) | b;
   }

   *error = fixscript_int(gif->g.delay);
   return gif->img;
}


static Value load_image(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value ret;
   void *ptr;
   stbi_uc *u;
   char buf[128];
   uint32_t *pixels = NULL;
   int i, len, elem_size, width, height, r, g, b, a;

   ptr = fixscript_get_shared_array_data(heap, params[0], &len, &elem_size, NULL, -1, NULL);
   if (!ptr || elem_size != 1) {
      *error = fixscript_create_error_string(heap, "invalid shared array");
      return fixscript_int(0);
   }

   u = stbi_load_from_memory(ptr, len, &width, &height, NULL, 4);
   if (!u) {
      snprintf(buf, sizeof(buf), "can't load image (%s)", stbi_failure_reason());
      *error = fixscript_create_error_string(heap, buf);
      return fixscript_int(0);
   }

   ret = fiximage_create(heap, width, height);
   if (!ret.value) {
      free(u);
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   fiximage_get_data(heap, ret, NULL, NULL, NULL, &pixels, NULL, NULL);

   for (i=0; i<width*height; i++) {
      r = u[i*4+0];
      g = u[i*4+1];
      b = u[i*4+2];
      a = u[i*4+3];

      r = div255(r * a);
      g = div255(g * a);
      b = div255(b * a);
      pixels[i] = (a << 24) | (r << 16) | (g << 8) | b;
   }

   free(u);
   return ret;
}


void register_image_functions(Heap *heap)
{
   fixscript_register_handle_types(&handles_offset, NUM_HANDLE_TYPES);

   fixscript_register_native_func(heap, "gif_create#1", gif_create, NULL);
   fixscript_register_native_func(heap, "gif_reset#1", gif_reset, NULL);
   fixscript_register_native_func(heap, "gif_next#1", gif_next, NULL);
   fixscript_register_native_func(heap, "load_image#1", load_image, NULL);
}
