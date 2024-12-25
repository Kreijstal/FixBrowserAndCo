/*
 * CellSplit v0.2 - https://www.cellsplit.org/
 * Copyright (c) 2021-2024 Martin Dvorak <jezek2@advel.cz>
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

#include <stdint.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STB_IMAGE_STATIC
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_STATIC
#include "stb_image_write.h"

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif

#if defined(__linux__)
   #if defined(__x86_64__)
      asm(".symver memcpy,memcpy@GLIBC_2.2.5");
   #endif
#endif

EXPORT int get_image_info(stbi_uc *buf, int len, int *width, int *height)
{
   return stbi_info_from_memory(buf, len, width, height, NULL);
}

EXPORT stbi_uc *load_image(stbi_uc *buf, int len, int *width, int *height)
{
   return stbi_load_from_memory(buf, len, width, height, NULL, 3);
}

EXPORT void convert_image(stbi_uc *img, int width, int height, uint32_t *pixels, int stride)
{
   uint8_t r, g, b;
   int i, j;

   for (i=0; i<height; i++) {
      for (j=0; j<width; j++) {
         r = img[(i*width+j)*3+0];
         g = img[(i*width+j)*3+1];
         b = img[(i*width+j)*3+2];
         pixels[i*stride+j] = 0xFF000000 | (r << 16) | (g << 8) | b;
      }
   }
}

EXPORT void free_image(stbi_uc *img)
{
   stbi_image_free(img);
}

typedef struct {
   int len, cap;
   void *data;
} Array;

static void write_func(void *context, void *data, int size)
{
   Array *arr = context;
   int new_cap;
   void *new_data;

   if (!arr->data) {
      return;
   }

   while (arr->len + size > arr->cap) {
      new_cap = arr->cap * 2;
      new_data = realloc(arr->data, new_cap);
      if (!new_data) {
         free(arr->data);
         arr->data = NULL;
         return;
      }
      arr->cap = new_cap;
      arr->data = new_data;
   }

   memcpy(arr->data + arr->len, data, size);
   arr->len += size;
}

EXPORT void *save_jpg(int width, int height, int stride, uint32_t *pixels, int quality, int *len_out)
{
   Array arr;
   uint8_t *buf;
   uint8_t r, g, b;
   int i, j;

   buf = malloc(width*height*3);
   if (!buf) { 
      return NULL;
   }

   for (i=0; i<height; i++) {
      for (j=0; j<width; j++) {
         r = pixels[i*stride+j] >> 16;
         g = pixels[i*stride+j] >> 8;
         b = pixels[i*stride+j];
         buf[(i*width+j)*3+0] = r;
         buf[(i*width+j)*3+1] = g;
         buf[(i*width+j)*3+2] = b;
      }
   }

   arr.len = 0;
   arr.cap = 4096;
   arr.data = malloc(arr.cap);
   if (!arr.data) {
      free(buf);
      return NULL;
   }

   if (!stbi_write_jpg_to_func(write_func, &arr, width, height, 3, buf, quality)) {
      free(arr.data);
      free(buf);
      return NULL;
   }

   *len_out = arr.len;
   free(buf);
   return arr.data;
}
