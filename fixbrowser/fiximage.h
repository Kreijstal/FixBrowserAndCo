/*
 * FixScript Image v0.7 - https://www.fixscript.org/
 * Copyright (c) 2019-2024 Martin Dvorak <jezek2@advel.cz>
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

#ifndef FIXIMAGE_H
#define FIXIMAGE_H

#include <stdint.h>
#include "fixscript.h"

#ifdef __cplusplus
extern "C" {
#endif 

typedef void (*ImageFreeFunc)(void *data);
typedef void (*MulticoreFunc)(int from, int to, void *data);

Value fiximage_create(Heap *heap, int width, int height);
Value fiximage_create_from_pixels(Heap *heap, int width, int height, int stride, uint32_t *pixels, ImageFreeFunc free_func, void *user_data, int type);
Value fiximage_create_painter(Heap *heap, Value img, int offset_x, int offset_y);
int fiximage_get_data(Heap *heap, Value img, int *width, int *height, int *stride, uint32_t **pixels, void **user_data, int *type);
int fiximage_get_painter_data(Heap *heap, Value p, float *tr, int *clip, Value *image);

void fiximage_register_functions(Heap *heap);

int fiximage_get_core_count();
void fiximage_multicore_run(int from, int to, int min_iters, MulticoreFunc func, void *data);

#ifdef __cplusplus
}
#endif 

#endif /* FIXIMAGE_H */
