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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fixgui_common.h"

struct Worker {
   WorkerCommon common;
};

static Heap *gui_heap = NULL;


Worker *worker_create()
{
   Worker *worker;
   
   worker = calloc(1, sizeof(Worker));
   if (!worker) return NULL;

   return worker;
}


int worker_start(Worker *worker)
{
   return 0;
}


void worker_notify(Worker *worker)
{
}


void worker_lock(Worker *worker)
{
}


void worker_wait(Worker *worker, int timeout)
{
}


void worker_unlock(Worker *worker)
{
}


void worker_destroy(Worker *worker)
{
   free(worker);
}


uint32_t timer_get_time()
{
   return 0;
}


uint32_t timer_get_micro_time()
{
   return 0;
}


int timer_is_active(Heap *heap, Value instance)
{
   return 0;
}


void timer_start(Heap *heap, Value instance, int interval, int restart)
{
}


void timer_stop(Heap *heap, Value instance)
{
}


void clipboard_set_text(plat_char *text)
{
}


plat_char *clipboard_get_text()
{
   return NULL;
}


void io_notify()
{
}


void post_to_main_thread(void *data)
{
}


void quit_app()
{
}


void register_native_platform_gui_functions(Heap *heap)
{
   gui_heap = heap;
}


void virtual_repaint_notify()
{
}


void virtual_set_cursor(int type)
{
}
