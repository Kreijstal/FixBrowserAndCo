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
#include <string.h>
#ifdef _WIN32
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <winsock.h>
#else
#include <signal.h>
#endif
#include "fixio.h"
#include "fiximage.h"
#include "fixgui.h"
#include "fixtask.h"
#include "browser.h"
#include "util.h"
#include "embed_scripts.h"
#include "embed_resources.h"

enum {
   SM_selector,
   SM_properties,
   SM_specificity,
   SM_cnt,
   SM_SIZE
};

static int test_scripts = 0;


static int selector_compare(const void *p1, const void *p2)
{
   const Value *v1 = p1;
   const Value *v2 = p2;

   if (v1[SM_specificity].value < v2[SM_specificity].value) {
      return -1;
   }
   if (v1[SM_specificity].value > v2[SM_specificity].value) {
      return +1;
   }
   if (v1[SM_cnt].value < v2[SM_cnt].value) {
      return -1;
   }
   if (v1[SM_cnt].value > v2[SM_cnt].value) {
      return +1;
   }
   return 0;
}


static Value sort_current_selectors(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value *values;
   int err, len;

   err = fixscript_get_array_length(heap, params[0], &len);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   if (len > 1000000*SM_SIZE || (len % SM_SIZE) != 0) {
      *error = fixscript_create_string(heap, "invalid selector array", -1);
      return fixscript_int(0);
   }

   values = malloc(len * sizeof(Value));
   if (!values) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   err = fixscript_get_array_range(heap, params[0], 0, len, values);
   if (err) {
      free(values);
      return fixscript_error(heap, error, err);
   }

   qsort(values, len / SM_SIZE, SM_SIZE * sizeof(Value), selector_compare);

   err = fixscript_set_array_range(heap, params[0], 0, len, values);
   free(values);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   
   return fixscript_int(0);
}


static Script *load_script(Heap *heap, const char *fname, Value *error, void *data)
{
   if (test_scripts) {
      return script_load_file(heap, fname, error, ".");
   }
   else {
      return fixscript_load_embed(heap, fname, error, embed_scripts);
   }
}


static Heap *create_worker_heap(void *data);

static Heap *create_gui_heap()
{
   Heap *heap;

   heap = fixscript_create_heap();
   fixio_register_functions(heap);
   fiximage_register_functions(heap);
   fixtask_register_functions(heap, create_worker_heap, NULL, load_script, NULL);
   register_image_functions(heap);
   register_css_functions(heap);
   fixscript_register_native_func(heap, "resource_get#1", embed_resources_get_func, NULL);
   return heap;
}


static Heap *create_worker_heap(void *data)
{
   Heap *heap;
   
   heap = create_gui_heap();
   fixio_register_functions(heap);
   fixgui_register_worker_functions(heap);
   register_bigint_functions(heap);
   register_crypto_functions(heap);
   register_util_functions(heap);
   register_script_functions(heap);
   fixscript_register_native_func(heap, "sort_current_selectors#1", sort_current_selectors, NULL);
   return heap;
}


static Script *worker_load(Heap **heap, const char *fname, Value *error, void *data)
{
   *heap = create_worker_heap(NULL);
   return load_script(*heap, fname, error, NULL);
}


int app_main(int argc, char **argv)
{
   Heap *heap;
   Script *script;
   Value error, args;
   int i;
#ifdef _WIN32
   WSADATA wsa_data;
#endif

#ifdef _WIN32
   init_critical_sections();
   WSAStartup(MAKEWORD(2,2), &wsa_data);
#else
   signal(SIGPIPE, SIG_IGN);
#endif

   if (!init_self_file(argv[0])) {
      fprintf(stderr, "error: can't determine path of the executable\n");
      return 0;
   }
   start_memory_cache_cleanup_thread();
   
   if (argc >= 2 && !strcmp(argv[1], "-t")) {
      test_scripts = 1;
   }

   heap = create_gui_heap();
   fixgui_register_functions(heap, worker_load, NULL);
   register_util_functions(heap);

   script = load_script(heap, "browser/main", &error, NULL);
   if (!script) {
      fprintf(stderr, "%s\n", fixscript_get_compiler_error(heap, error));
      fflush(stderr);
      return 0;
   }

   args = fixscript_create_array(heap, 0);
   for (i=1; i<argc; i++) {
      fixscript_append_array_elem(heap, args, fixscript_create_string(heap, argv[i], -1));
   }

   fixscript_run(heap, script, "init#1", &error, args);
   if (error.value) {
      fixscript_dump_value(heap, error, 1);
      return 0;
   }

   return 1;
}
