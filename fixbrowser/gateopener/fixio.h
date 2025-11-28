/*
 * FixScript IO v0.8 - https://www.fixscript.org/
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

#ifndef FIXIO_H
#define FIXIO_H

#include "fixscript.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*IOEventNotifyFunc)(void *data);

void fixio_register_functions(Heap *heap);

void *fixio_get_console_mutex();
int fixio_is_console_active();
void fixio_flush_console();

void fixio_integrate_event_loop(Heap *heap, IOEventNotifyFunc notify_func, void *notify_data);
void fixio_process_events(Heap *heap);

#ifdef __cplusplus
}
#endif

#endif /* FIXIO_H */
