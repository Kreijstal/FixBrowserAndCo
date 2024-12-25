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

#ifndef UTIL_H
#define UTIL_H

#include <stdint.h>

#define MIN(a, b) ((a)<(b)? (a):(b))
#define MAX(a, b) ((a)>(b)? (a):(b))

void *malloc_array(int nmemb, int size);
void *realloc_array(void *ptr, int nmemb, int size);

char *string_format(const char *fmt, ...);
Script *script_load(Heap *heap, const char *fname, char **error_msg, void *data);
Value create_stdlib_error(Heap *heap, const char *msg);

#ifdef _WIN32
void init_critical_sections();
#endif
 
void start_global_cleanup_thread();
void register_util_functions(Heap *heap);

#endif /* UTIL_H */
