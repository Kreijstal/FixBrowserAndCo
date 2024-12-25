/*
 * FixScript Task v0.6 - https://www.fixscript.org/
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

#ifndef FIXTASK_H
#define FIXTASK_H

#include "fixscript.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef Heap *(*HeapCreateFunc)(void *data);
typedef void (*ComputeHeapRunFunc)(Heap *heap, int core_id, void *data);

void fixtask_register_functions(Heap *heap, HeapCreateFunc create_func, void *create_data, LoadScriptFunc load_func, void *load_data);

void fixtask_get_script_load_function(Heap *heap, LoadScriptFunc *load_func, void **load_data);
int fixtask_get_core_count(Heap *heap);

void fixtask_run_on_compute_threads(Heap *heap, Value *error, ComputeHeapRunFunc func, void *data);

void *fixtask_get_atomic_mutex(void *ptr);

#define fixtask_integrate_io_event_loop(heap) __fixtask_integrate_io_event_loop(heap, fixio_integrate_event_loop, fixio_process_events)
void __fixtask_integrate_io_event_loop(Heap *heap, void (*integrate_func)(Heap *, void (*)(void *), void *), void (*process_func)(Heap *));

#ifdef __cplusplus
}
#endif

#endif /* FIXTASK_H */
