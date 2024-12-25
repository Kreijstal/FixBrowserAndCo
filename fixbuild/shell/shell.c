/*
 * FixBuild v0.6 - https://www.fixscript.org/
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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#ifdef _WIN32
#define UNICODE
#define _UNICODE
#include <windows.h>
#else
#include <pthread.h>
#endif
#ifdef __wasm__
#include <wasm-support.h>
#endif
#include "fixio.h"
#include "fixtask.h"
#include "fiximage.h"
#include "fixnative.h"
#include "fixutil.h"
#ifdef FIXBUILD_GUI
#include "fixgui.h"
#endif

#define MAGIC1 0x093C1FBE
#define MAGIC2 0x8404665A

enum {
   FLAG_BINARY     = 1 << 0,
   FLAG_COMPRESSED = 1 << 1
};

typedef struct File {
   char *name;
   int off, len;
   char flags;
   struct File *next;
} File;

#if defined(_WIN32)
static HANDLE self_file;
#elif !defined(__wasm__)
static FILE *self_file;
#endif
static File *files = NULL;
#ifdef _WIN32
static CRITICAL_SECTION critical_section;
#else
static pthread_mutex_t mutex;
#endif

#if defined(FIXBUILD_BINCOMPAT) && defined(__linux__)
   #if defined(__i386__)
   #elif defined(__x86_64__)
      asm(".symver memcpy,memcpy@GLIBC_2.2.5");
   #elif defined(__arm__)
   #endif
#endif


#if !defined(_WIN32) && !defined(__wasm__)
static char *search_file_on_path(const char *fname)
{
   FILE *f;
   char *path;
   char *s, *p;
   char buf[256];

   if (strchr(fname, '/')) {
      return strdup(fname);
   }

   path = strdup(getenv("PATH"));
   s = path;
   do {
      p = strchr(s, ':');
      if (p) {
         *p = 0;
      }
      snprintf(buf, sizeof(buf), "%s/%s", s, fname);
      f = fopen(buf, "rb");
      if (f) {
         fclose(f);
         free(path);
         return strdup(buf);
      }
      if (p) {
         s = p+1;
      }
   }
   while (p);

   free(path);
   return NULL;
}
#endif


static void read_data(int off, void *data, int len)
{
#if defined(_WIN32)
   DWORD read;
   
   EnterCriticalSection(&critical_section);
   if (SetFilePointer(self_file, off, 0, FILE_BEGIN) != off) {
      fprintf(stderr, "error reading from executable\n");
      fflush(stderr);
      exit(1);
   }
   while (len > 0) {
      if (!ReadFile(self_file, data, len, &read, NULL)) {
         fprintf(stderr, "error reading from executable\n");
         fflush(stderr);
         exit(1);
      }
      data += read;
      len -= read;
   }
   LeaveCriticalSection(&critical_section);
#elif defined(__wasm__)
   const char *embedded;
   int embedded_len;

   embedded = wasm_get_embedded_data(&embedded_len);
   memcpy(data, embedded + off, len);
#else
   if (pthread_mutex_lock(&mutex) != 0) {
      fprintf(stderr, "error: can't lock mutex\n");
      fflush(stderr);
      exit(1);
   }

   if (fseek(self_file, off, SEEK_SET) != 0) {
      fprintf(stderr, "error reading from executable\n");
      fflush(stderr);
      exit(1);
   }
   if (fread(data, len, 1, self_file) != 1) {
      fprintf(stderr, "error reading from executable\n");
      fflush(stderr);
      exit(1);
   }

   pthread_mutex_unlock(&mutex);
#endif
}


static void read_file_header(char *ptr, char *end, char **main_script)
{
   File *file, **prev = &files;

   *main_script = strdup(ptr);
   ptr += strlen(ptr)+1;

   while (ptr < end) {
      file = calloc(1, sizeof(File));
      file->name = strdup(ptr);
      ptr += strlen(ptr)+1;
      memcpy(&file->off, ptr, sizeof(int));
      ptr += sizeof(int);
      memcpy(&file->len, ptr, sizeof(int));
      ptr += sizeof(int);
      memcpy(&file->flags, ptr, 1);
      ptr++;
      //printf("name='%s' off=%d len=%d flag=%d\n", file->name, file->off, file->len, file->flags);
      *prev = file;
      prev = &file->next;
   }
}


static int uncompress_script(const char *in, char **dest_out)
{
   char *out = NULL;
   int in_size, out_size;
   int in_idx = 0, out_idx = 0;
   int literal_len, match_len, match_off, amount;
   uint8_t token, b;
   uint16_t offset;

   memcpy(&in_size, &in[0], sizeof(int));
   memcpy(&out_size, &in[4], sizeof(int));
   in += 8;

   out = malloc(out_size+1);
   if (!out) goto error;

   while (in_idx < in_size) {
      token = in[in_idx++];

      literal_len = token >> 4;
      if (literal_len == 15) {
         do {
            if (in_idx >= in_size) goto error;
            b = in[in_idx++];
            literal_len += b;
            if (literal_len > out_size) goto error;
         }
         while (b == 255);
      }
      if (literal_len > 0) {
         if (in_idx + literal_len > in_size) goto error;
         if (out_idx + literal_len > out_size) goto error;
         memcpy(out + out_idx, in + in_idx, literal_len);
         in_idx += literal_len;
         out_idx += literal_len;
      }
      
      if (in_idx == in_size) break;

      if (in_idx+2 > in_size) goto error;
      memcpy(&offset, in + in_idx, 2);
      in_idx += 2;
      if (offset == 0) goto error;

      match_off = out_idx - offset;
      if (match_off < 0) goto error;

      match_len = (token & 0xF) + 4;
      if (match_len == 19) {
         do {
            if (in_idx >= in_size) goto error;
            b = in[in_idx++];
            match_len += b;
            if (match_len > out_size) goto error;
         }
         while (b == 255);
      }
      if (out_idx + match_len > out_size) goto error;

      if (match_off + match_len <= out_idx) {
         memcpy(out + out_idx, out + match_off, match_len);
         out_idx += match_len;
      }
      else {
         amount = out_idx - match_off;
         while (match_len > 0) {
            if (amount > match_len) {
               amount = match_len;
            }
            memcpy(out + out_idx, out + match_off, amount);
            out_idx += amount;
            match_len -= amount;
         }
      }
   }

   if (out_idx != out_size) goto error;

   out[out_size] = '\0';
   *dest_out = out;
   return 1;

error:
   free(out);
   return 0;
}


static Script *load_script(Heap *heap, const char *name, Value *error, void *data)
{
   Script *script;
   File *file;
   char *fname;
   char *buf, *uncompressed;
   char tmp[256];

   fname = malloc(strlen(name)+4+1);
   strcpy(fname, name);
   strcat(fname, ".fix");
   
   script = fixscript_get(heap, fname);
   if (script) {
      free(fname);
      return script;
   }

   file = files;
   while (file) {
      if (strcmp(file->name, fname) == 0 && (file->flags & FLAG_BINARY) == 0) break;
      file = file->next;
   }

   if (!file) {
      free(fname);
      if (error) {
         snprintf(tmp, sizeof(tmp), "script %s not found", name);
         *error = fixscript_create_string(heap, tmp, -1);
      }
      return NULL;
   }

   buf = malloc(file->len+1);
   if (!buf) {
      free(fname);
      if (error) {
         *error = fixscript_create_string(heap, "out of memory", -1);
      }
      return NULL;
   }

   read_data(file->off, buf, file->len);
   buf[file->len] = 0;

   if (file->flags & FLAG_COMPRESSED) {
      if (!uncompress_script(buf+1, &uncompressed)) {
         free(buf);
         free(fname);
         if (error) {
            *error = fixscript_create_string(heap, "script decompression error", -1);
         }
         return NULL;
      }

      free(buf);
      buf = uncompressed;
   }

   script = fixscript_load(heap, buf, fname, error, load_script, NULL);
   free(buf);
   free(fname);
   return script;
}


static Value get_resource(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   File *file = files;
   Value ret;
   char tmp[256], *fname, *ptr;
   int err;

   err = fixscript_get_string(heap, params[0], 0, -1, &fname, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   while (file) {
      if (strcmp(file->name, fname) == 0 && (file->flags & FLAG_BINARY)) break;
      file = file->next;
   }

   if (!file) {
      snprintf(tmp, sizeof(tmp), "resource %s not found", fname);
      free(fname);
      *error = fixscript_create_error_string(heap, tmp);
      return fixscript_int(0);
   }
   free(fname);

   ptr = malloc(file->len);
   if (!ptr) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   read_data(file->off, ptr, file->len);

   if (data) {
      ret = fixscript_create_string(heap, NULL, 0);
      if (!ret.value) {
         free(ptr);
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
      err = fixscript_set_array_length(heap, ret, file->len);
      if (!err) {
         err = fixscript_set_array_bytes(heap, ret, 0, file->len, ptr);
      }
      free(ptr);
      if (err) {
         return fixscript_error(heap, error, err);
      }
   }
   else {
      ret = fixscript_create_or_get_shared_array(heap, -1, ptr, file->len, 1, free, ptr, NULL);
      if (!ret.value) {
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
   }
   return ret;
}


static Value get_resource_list(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   File *file = files;
   Value arr, fname;
   int err;
   
   arr = fixscript_create_array(heap, 0);
   if (!arr.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   while (file) {
      if ((file->flags & FLAG_BINARY) == 0) {
         file = file->next;
         continue;
      }

      fname = fixscript_create_string(heap, file->name, -1);
      if (!fname.value) {
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
      err = fixscript_append_array_elem(heap, arr, fname);
      if (err) {
         return fixscript_error(heap, error, err);
      }
      file = file->next;
   }

   return arr;
}


static Heap *create_heap(void *data)
{
   Heap *heap;

   heap = fixscript_create_heap();
   fixio_register_functions(heap);
   fixtask_register_functions(heap, create_heap, NULL, load_script, NULL);
   fixtask_integrate_io_event_loop(heap);
   fiximage_register_functions(heap);
   fixnative_register_functions(heap);
   fixutil_register_functions(heap);
   fixscript_register_native_func(heap, "get_resource#1", get_resource, (void *)0);
   fixscript_register_native_func(heap, "get_resource_string#1", get_resource, (void *)1);
   fixscript_register_native_func(heap, "get_resource_list#0", get_resource_list, NULL);
   return heap;
}


#ifdef FIXBUILD_GUI

#ifdef __APPLE__
char *fixgui__get_cocoa_exec_path();
#endif

static Script *worker_load(Heap **heap, const char *fname, Value *error, void *data)
{
   *heap = create_heap(NULL);
   fixgui_register_worker_functions(*heap);
   return load_script(*heap, fname, error, NULL);
}

int app_main(int argc, char **argv)
{
   Heap *heap;
   Script *script;
   Value error, args, func;
   char *main_script;
   char *buf;
   int magic[3];
   int self_len;
   int i, off, len;
#ifdef _WIN32
   uint16_t filename[256];
   uint32_t dwret;
#endif

#ifdef _WIN32
   InitializeCriticalSection(&critical_section);
#else
   if (pthread_mutex_init(&mutex, NULL) != 0) {
      fprintf(stderr, "error: can't initialize mutex\n");
      fflush(stderr);
      return 0;
   }
#endif

#if defined(_WIN32)
   dwret = GetModuleFileName(NULL, filename, 255);
   if (dwret == 0 || dwret >= 254) {
      fprintf(stderr, "error: can't open executable\n");
      fflush(stderr);
      return 0;
   }
   self_file = CreateFile(filename, GENERIC_READ, FILE_SHARE_DELETE | FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
   if (self_file == INVALID_HANDLE_VALUE) {
      fprintf(stderr, "error: can't open executable\n");
      fflush(stderr);
      return 0;
   }
   self_len = GetFileSize(self_file, NULL);
#elif defined(__wasm__)
   wasm_get_embedded_data(&self_len);
#else
   #ifdef __APPLE__
      buf = fixgui__get_cocoa_exec_path();
      if (buf) {
         buf = strdup(buf);
      }
      else {
         buf = search_file_on_path(argv[0]);
      }
   #else
      buf = search_file_on_path(argv[0]);
   #endif
   if (!buf) {
      fprintf(stderr, "error: can't open executable\n");
      fflush(stderr);
      return 1;
   }
   self_file = fopen(buf, "rb");
   free(buf);
   if (!self_file) {
      fprintf(stderr, "error: can't open executable\n");
      fflush(stderr);
      return 0;
   }
   if (fseek(self_file, 0, SEEK_END) != 0) {
      fprintf(stderr, "error reading from executable\n");
      fflush(stderr);
      return 0;
   }
   self_len = ftell(self_file);
   //printf("len=%d\n", self_len);
#endif

   read_data(self_len-12, magic, 12);
   if (magic[0] != MAGIC1 || magic[1] != MAGIC2) {
      fprintf(stderr, "error reading from executable (scripts not present)\n");
      fflush(stderr);
      return 0;
   }
   off = magic[2];
   //printf("off=%d\n", off);

   len = self_len-12-off;
#ifdef __wasm__
   buf = (char *)wasm_get_embedded_data(NULL) + off;
#else
   buf = malloc(len);
   if (!buf) {
      fprintf(stderr, "error reading from executable\n");
      fflush(stderr);
      return 0;
   }
   read_data(off, buf, len);
#endif
   read_file_header(buf, buf+len, &main_script);

   heap = create_heap(NULL);
   fixgui_register_functions(heap, worker_load, NULL);
   fixgui_integrate_io_event_loop(heap);
#ifdef FIXGUI_VIRTUAL
   fixgui_init_virtual(heap, load_script, NULL);
#endif

   script = load_script(heap, main_script, &error, NULL);
   if (!script) {
      fprintf(stderr, "%s\n", fixscript_get_compiler_error(heap, error));
      fflush(stderr);
      return 0;
   }

   args = fixscript_create_array(heap, 0);
   for (i=1; i<argc; i++) {
      fixscript_append_array_elem(heap, args, fixscript_create_string(heap, argv[i], -1));
   }
   
   func = fixscript_get_function(heap, script, "init#2");
   if (func.value) {
      fixscript_call(heap, func, 2, &error, fixscript_create_string(heap, argv[0], -1), args);
   }
   else {
      func = fixscript_get_function(heap, script, "init#1");
      if (func.value) {
         fixscript_call(heap, func, 1, &error, args);
      }
      else {
         func = fixscript_get_function(heap, script, "init#0");
         if (func.value) {
            fixscript_call(heap, func, 0, &error);
         }
      }
   }
   
   if (!func.value) {
      fprintf(stderr, "error: can't find init function\n");
      fflush(stderr);
      return 0;
   }

   if (error.value) {
      fixscript_dump_value(heap, error, 1);
      return 0;
   }
   return 1;
}

#else

int main(int argc, char **argv)
{
   Heap *heap;
   Script *script;
   Value error, args, func, ret;
   char *main_script;
   char *buf;
   int magic[3];
   int self_len;
   int i, off, len;
#ifdef _WIN32
   uint16_t filename[256];
   uint32_t dwret;
#endif

#ifdef _WIN32
   InitializeCriticalSection(&critical_section);
#else
   if (pthread_mutex_init(&mutex, NULL) != 0) {
      fprintf(stderr, "error: can't initialize mutex\n");
      fflush(stderr);
      return 1;
   }
#endif

#if defined(_WIN32)
   dwret = GetModuleFileName(NULL, filename, 255);
   if (dwret == 0 || dwret >= 254) {
      fprintf(stderr, "error: can't open executable\n");
      fflush(stderr);
      return 0;
   }
   self_file = CreateFile(filename, GENERIC_READ, FILE_SHARE_DELETE | FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
   if (self_file == INVALID_HANDLE_VALUE) {
      fprintf(stderr, "error: can't open executable\n");
      fflush(stderr);
      return 0;
   }
   self_len = GetFileSize(self_file, NULL);
#elif defined(__wasm__)
   wasm_get_embedded_data(&self_len);
#else
   buf = search_file_on_path(argv[0]);
   if (!buf) {
      fprintf(stderr, "error: can't open executable\n");
      fflush(stderr);
      return 1;
   }
   self_file = fopen(buf, "rb");
   free(buf);
   if (!self_file) {
      fprintf(stderr, "error: can't open executable\n");
      fflush(stderr);
      return 1;
   }
   if (fseek(self_file, 0, SEEK_END) != 0) {
      fprintf(stderr, "error reading from executable\n");
      fflush(stderr);
      return 1;
   }
   self_len = ftell(self_file);
   //printf("len=%d\n", self_len);
#endif

   read_data(self_len-12, magic, 12);
   if (magic[0] != MAGIC1 || magic[1] != MAGIC2) {
      fprintf(stderr, "error reading from executable (scripts not present)\n");
      fflush(stderr);
      return 1;
   }
   off = magic[2];
   //printf("off=%d\n", off);

   len = self_len-12-off;
#ifdef __wasm__
   buf = (char *)wasm_get_embedded_data(NULL) + off;
#else
   buf = malloc(len);
   if (!buf) {
      fprintf(stderr, "error reading from executable\n");
      fflush(stderr);
      return 1;
   }
   read_data(off, buf, len);
#endif
   read_file_header(buf, buf+len, &main_script);

   heap = create_heap(NULL);

   script = load_script(heap, main_script, &error, NULL);
   if (!script) {
      fprintf(stderr, "%s\n", fixscript_get_compiler_error(heap, error));
      fflush(stderr);
      return 1;
   }

   args = fixscript_create_array(heap, 0);
   for (i=1; i<argc; i++) {
      fixscript_append_array_elem(heap, args, fixscript_create_string(heap, argv[i], -1));
   }
   
   func = fixscript_get_function(heap, script, "main#2");
   if (func.value) {
      ret = fixscript_call(heap, func, 2, &error, fixscript_create_string(heap, argv[0], -1), args);
   }
   else {
      func = fixscript_get_function(heap, script, "main#1");
      if (func.value) {
         ret = fixscript_call(heap, func, 1, &error, args);
      }
      else {
         func = fixscript_get_function(heap, script, "main#0");
         if (func.value) {
            ret = fixscript_call(heap, func, 0, &error);
         }
      }
   }
   
   if (!func.value) {
      fprintf(stderr, "error: can't find main function\n");
      fflush(stderr);
      return 1;
   }

   if (error.value) {
      fixscript_dump_value(heap, error, 1);
      return 1;
   }
   return fixscript_get_int(ret);
}

#endif
