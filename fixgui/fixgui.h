/*
 * FixScript GUI v0.8 - https://www.fixscript.org/
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

#ifndef FIXGUI_H
#define FIXGUI_H

#include "fixscript.h"

#ifdef __cplusplus
extern "C" {
#endif 

typedef Script *(*WorkerLoadFunc)(Heap **heap, const char *fname, Value *error, void *data);
typedef void (*MainThreadFunc)(Heap *heap, void *data);

void fixgui_register_functions(Heap *heap, WorkerLoadFunc load_func, void *load_data);
void fixgui_register_worker_functions(Heap *heap);
void fixgui_run_in_main_thread(MainThreadFunc func, void *data);
#ifdef FIXGUI_VIRTUAL
void fixgui_init_virtual(Heap *heap, LoadScriptFunc func, void *data);
#endif

int app_main(int argc, char **argv);
int console_main(int argc, char **argv);

#ifdef __EMSCRIPTEN__
Script *emscripten_worker_load(Heap **heap, const char *fname, Value *error);
#endif

#define fixgui_integrate_io_event_loop(heap) __fixgui_integrate_io_event_loop(heap, fixio_integrate_event_loop, fixio_process_events)
void __fixgui_integrate_io_event_loop(Heap *heap, void (*integrate_func)(Heap *, void (*)(void *), void *), void (*process_func)(Heap *));

#ifdef __cplusplus
}
#endif 

#endif /* FIXGUI_H */
