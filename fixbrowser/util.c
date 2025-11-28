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

#if !defined(__APPLE__)
#define _XOPEN_SOURCE 600
#define _BSD_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <memory.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#if defined(_WIN32)
#define UNICODE
#define _UNICODE
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#endif
#ifdef __APPLE__
#include <mach/mach_time.h>
#include <sys/time.h>
#endif
#include "browser.h"
#include "fixscript.h"
#include "util.h"
#include "embed_charsets.h"

typedef struct {
   void **data;
   uint64_t *expiry_time;
   int size, len, slots;
} MemoryCacheHash;

#if defined(_WIN32)
enum {
   DT_REG,
   DT_DIR
};

struct dirent {
   int d_type;
   char d_name[MAX_PATH];
};
#endif

#if defined(_WIN32)
static CRITICAL_SECTION monotonic_clock_section;
static CRITICAL_SECTION strerror_section;
static CRITICAL_SECTION memory_cache_section;
static HANDLE memory_cache_event;
#else
static pthread_mutex_t memory_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t memory_cache_cond = PTHREAD_COND_INITIALIZER;
#endif
static MemoryCacheHash memory_cache;
#if defined(_WIN32)
static uint16_t *exec_path = NULL;
#else
static char *exec_path = NULL;
#endif


#ifdef _WIN32
static int win32_vsnprintf(char *str, size_t size, const char *format, va_list ap)
{
   va_list aq;
   char *s, buf[128];
   int len, cap;

   va_copy(aq, ap);
   len = vsnprintf(str, size, format, aq);
   va_end(aq);

   if (len < 0 && !str && size == 0) {
      va_copy(aq, ap);
      len = vsnprintf(buf, sizeof(buf), format, aq);
      va_end(aq);
      if (len >= 0 && len < sizeof(buf)) {
         return len;
      }
      cap = sizeof(buf);
      while (cap < (1<<30)) {
         cap *= 2;
         s = malloc(cap);
         if (!s) return -1;
         va_copy(aq, ap);
         len = vsnprintf(s, cap, format, aq);
         va_end(aq);
         free(s);
         if (len >= 0 && len < cap) {
            return len;
         }
      }
      return -1;
   }
   return len;
}
#define vsnprintf win32_vsnprintf
#endif


void *malloc_array(int nmemb, int size)
{
   int64_t mul = ((int64_t)nmemb) * ((int64_t)size);
   if (mul < 0 || mul > INTPTR_MAX) {
      return NULL;
   }
   return malloc(mul);
}


void *realloc_array(void *ptr, int nmemb, int size)
{
   int64_t mul = ((int64_t)nmemb) * ((int64_t)size);
   if (mul < 0 || mul > INTPTR_MAX) {
      return NULL;
   }
   return realloc(ptr, mul);
}


char *string_format(const char *fmt, ...)
{
   va_list ap;
   int len;
   char *s;

   va_start(ap, fmt);
   len = vsnprintf(NULL, 0, fmt, ap);
   va_end(ap);
   if (len < 0) return NULL;
    
   s = malloc(len+1);
   if (!s) return NULL;

   va_start(ap, fmt);
   vsnprintf(s, len+1, fmt, ap);
   va_end(ap);
   return s;
}


Value create_stdlib_error(Heap *heap, const char *msg)
{
   Value error;
   char *s;
   char buf[128];

   if (!msg) {
      msg = "I/O error occurred";
   }

   strcpy(buf, "unknown error");
#if defined(_WIN32)
   EnterCriticalSection(&strerror_section);
   strncpy(buf, strerror(errno), sizeof(buf));
   buf[sizeof(buf)-1] = 0;
   LeaveCriticalSection(&strerror_section);
#else
   strerror_r(errno, buf, sizeof(buf));
#endif
   s = string_format("%s (%d)", buf, errno);
   error = fixscript_create_error_string(heap, s);
   free(s);
   return error;
}


static uint64_t get_monotonic_time()
{
#if defined(_WIN32)
   static uint64_t base = 0;
   static uint32_t last_time = 0;
   uint32_t time;
   uint64_t result;

   EnterCriticalSection(&monotonic_clock_section);
   time = GetTickCount();
   if (time < last_time) {
      base += (1ULL<<32);
   }
   last_time = time;
   result = base + time;
   LeaveCriticalSection(&monotonic_clock_section);
   return result / 1000;
#elif defined(__APPLE__)
   static uint64_t start_time = 0;
   uint64_t time;
   mach_timebase_info_data_t info;
   time = mach_absolute_time();
   if (start_time == 0) {
      start_time = time;
   }
   mach_timebase_info(&info);
   time -= start_time;
   return time * info.numer / info.denom / 1000000000ULL;
#else
   struct timespec ts;
   
   if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
      fprintf(stderr, "can't get monotonic time");
      exit(1);
   }
   return (uint64_t)ts.tv_sec;
#endif
}


static Value clock_get_real_time(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(_WIN32)
   FILETIME time_struct;
   uint64_t time;

   GetSystemTimeAsFileTime(&time_struct);
   memcpy(&time, &time_struct, sizeof(uint64_t));
   return fixscript_int(time / 10000000 - 11644473600ULL);
#elif defined(__APPLE__)
   struct timeval tv;
   if (gettimeofday(&tv, NULL) != 0) {
      *error = fixscript_create_error_string(heap, "can't get real time");
      return fixscript_int(0);
   }
   return fixscript_int(tv.tv_sec);
#else
   struct timespec ts;
   
   if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
      *error = fixscript_create_error_string(heap, "can't get real time");
      return fixscript_int(0);
   }
   return fixscript_int(ts.tv_sec);
#endif
}


static Value file_rename(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(_WIN32)
   Value ret = fixscript_int(0);
   uint16_t *oldpath = NULL, *newpath = NULL;
   int err;

   err = fixscript_get_string_utf16(heap, params[0], 0, -1, &oldpath, NULL);
   if (!err) {
      err = fixscript_get_string_utf16(heap, params[1], 0, -1, &newpath, NULL);
   }
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   ret = fixscript_int(MoveFileExW(oldpath, newpath, MOVEFILE_REPLACE_EXISTING) != 0);

error:
   free(oldpath);
   free(newpath);
   return ret;
#else
   Value ret = fixscript_int(0);
   char *oldpath = NULL, *newpath = NULL;
   int err;

   err = fixscript_get_string(heap, params[0], 0, -1, &oldpath, NULL);
   if (!err) {
      err = fixscript_get_string(heap, params[1], 0, -1, &newpath, NULL);
   }
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   ret = fixscript_int(rename(oldpath, newpath) == 0);

error:
   free(oldpath);
   free(newpath);
   return ret;
#endif
}


static unsigned int rehash(unsigned int a)
{
   a = (a+0x7ed55d16) + (a<<12);
   a = (a^0xc761c23c) ^ (a>>19);
   a = (a+0x165667b1) + (a<<5);
   a = (a+0xd3a2646c) ^ (a<<9);
   a = (a+0xfd7046c5) + (a<<3);
   a = (a^0xb55a4f09) ^ (a>>16);
   return a;
}


static uint32_t compute_hash(const char *key)
{
   uint32_t hash = 0;
   int i, len;

   memcpy(&len, key, sizeof(int));
   for (i=0; i<len; i++) {
      hash = hash*31 + key[sizeof(int) + i];
   }
   return rehash(hash);
}


static int key_equals(const char *key1, const char *key2)
{
   int len1, len2;

   memcpy(&len1, key1, sizeof(int));
   memcpy(&len2, key2, sizeof(int));
   if (len1 != len2) return 0;

   return !memcmp(key1 + sizeof(int), key2 + sizeof(int), len1);
}


static void *memory_cache_hash_set(MemoryCacheHash *hash, char *key, uint32_t keyhash, void *value, uint64_t expiry_time)
{
   MemoryCacheHash new_hash;
   int i, idx;
   void *old_val;
   
   if (hash->slots >= (hash->size >> 2)) {
      new_hash.size = hash->size;
      if (hash->len >= (hash->size >> 2)) {
         new_hash.size <<= 1;
      }
      if (new_hash.size == 0) {
         new_hash.size = 4*2;
      }
      new_hash.len = 0;
      new_hash.slots = 0;
      new_hash.data = calloc(new_hash.size, sizeof(void *));
      new_hash.expiry_time = calloc(new_hash.size >> 1, sizeof(uint64_t));
      for (i=0; i<hash->size; i+=2) {
         if (hash->data[i+0]) {
            if (hash->data[i+1]) {
               memory_cache_hash_set(&new_hash, hash->data[i+0], compute_hash(hash->data[i+0]), hash->data[i+1], hash->expiry_time[i>>1]);
            }
            else {
               free(hash->data[i+0]);
            }
         }
      }
      free(hash->data);
      free(hash->expiry_time);
      *hash = new_hash;
   }

   idx = (keyhash << 1) & (hash->size-1);
   for (;;) {
      if (!hash->data[idx+0]) break;
      if (key_equals(hash->data[idx+0], key)) {
         free(hash->data[idx+0]);
         old_val = hash->data[idx+1];
         if (value) {
            hash->data[idx+0] = key;
            hash->data[idx+1] = value;
            hash->expiry_time[idx>>1] = expiry_time;
         }
         else {
            free(key);
            hash->data[idx+0] = calloc(1, sizeof(int));
            hash->data[idx+1] = NULL;
            hash->expiry_time[idx>>1] = 0;
         }
         if (old_val) hash->len--;
         if (value) hash->len++;
         return old_val;
      }
      idx = (idx + 2) & (hash->size-1);
   }

   if (!value) {
      free(key);
      return NULL;
   }

   hash->len++;
   hash->slots++;
   hash->data[idx+0] = key;
   hash->data[idx+1] = value;
   hash->expiry_time[idx>>1] = expiry_time;
   return NULL;
}


static void *memory_cache_hash_get(MemoryCacheHash *hash, const char *key, int keyhash)
{
   int idx;

   if (!hash->data) {
      return NULL;
   }

   idx = (keyhash << 1) & (hash->size-1);
   for (;;) {
      if (!hash->data[idx+0]) break;
      if (key_equals(hash->data[idx+0], key)) {
         return hash->data[idx+1];
      }
      idx = (idx + 2) & (hash->size-1);
   }
   return NULL;
}


static void set_serialized_zero(char *buf)
{
   int i;
   
   i = 1; // size in bytes
   memcpy(buf, &i, sizeof(int));
   buf[4] = 0; // zero
}


static Value memory_cache_ops_get(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value ret_val = fixscript_int(0);
   char *key = NULL, *value_get, *tmp;
   int err, len;
   uint32_t hash = 0;

   err = fixscript_serialize_to_array(heap, &key, NULL, params[0]);
   if (err != FIXSCRIPT_SUCCESS) {
      fixscript_error(heap, error, err);
      goto error;
   }

   hash = compute_hash(key);
   err = FIXSCRIPT_SUCCESS;

#if defined(_WIN32)
   EnterCriticalSection(&memory_cache_section);
#else
   if (pthread_mutex_lock(&memory_cache_mutex) != 0) {
      *error = fixscript_create_error_string(heap, "can't lock memory cache mutex");
      goto error;
   }
#endif

   value_get = memory_cache_hash_get(&memory_cache, key, hash);
   if (value_get) {
      memcpy(&len, value_get, sizeof(int));
      tmp = malloc(sizeof(int)+len);
      if (tmp) {
         memcpy(tmp, value_get, sizeof(int)+len);
         value_get = tmp;
      }
      else {
         err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
   }

#if defined(_WIN32)
   LeaveCriticalSection(&memory_cache_section);
#else
   pthread_mutex_unlock(&memory_cache_mutex);
#endif

   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   if (value_get) {
      err = fixscript_unserialize_from_array(heap, value_get, NULL, -1, &ret_val);
      free(value_get);
      if (err != FIXSCRIPT_SUCCESS) {
         fixscript_error(heap, error, err);
         goto error;
      }
   }
   else if (num_params == 2) {
      ret_val = params[1];
   }

error:
   free(key);
   return ret_val;
}


static Value memory_cache_ops_set(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value ret_val = fixscript_int(0), timeout;
   char *key = NULL, *value = NULL;
   int err;
   uint32_t hash = 0;
   uint64_t expiry_time = 0;

   err = fixscript_serialize_to_array(heap, &key, NULL, params[0]);
   if (err != FIXSCRIPT_SUCCESS) {
      fixscript_error(heap, error, err);
      goto error;
   }

   err = fixscript_serialize_to_array(heap, &value, NULL, params[1]);
   if (err != FIXSCRIPT_SUCCESS) {
      fixscript_error(heap, error, err);
      goto error;
   }

   if (num_params == 3) {
      timeout = params[num_params-1];
      if (!fixscript_is_int(timeout)) {
         *error = fixscript_create_error_string(heap, "timeout must be an integer");
         goto error;
      }
      if (timeout.value < 0) {
         *error = fixscript_create_error_string(heap, "timeout must not be negative");
         goto error;
      }
      if (timeout.value) {
         expiry_time = get_monotonic_time() + timeout.value;
      }
   }

   hash = compute_hash(key);

#if defined(_WIN32)
   EnterCriticalSection(&memory_cache_section);
#else
   if (pthread_mutex_lock(&memory_cache_mutex) != 0) {
      *error = fixscript_create_error_string(heap, "can't lock memory cache mutex");
      goto error;
   }
#endif

   free(memory_cache_hash_set(&memory_cache, key, hash, value, expiry_time));
   key = NULL;
   value = NULL;

#if defined(_WIN32)
   SetEvent(memory_cache_event);
   LeaveCriticalSection(&memory_cache_section);
#else
   pthread_cond_broadcast(&memory_cache_cond);
   pthread_mutex_unlock(&memory_cache_mutex);
#endif

error:
   free(key);
   free(value);
   return ret_val;
}


static Value memory_cache_ops_remove(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value ret_val = fixscript_int(0);
   char *key = NULL;
   int err;
   uint32_t hash = 0;

   err = fixscript_serialize_to_array(heap, &key, NULL, params[0]);
   if (err != FIXSCRIPT_SUCCESS) {
      fixscript_error(heap, error, err);
      goto error;
   }

   hash = compute_hash(key);

#if defined(_WIN32)
   EnterCriticalSection(&memory_cache_section);
#else
   if (pthread_mutex_lock(&memory_cache_mutex) != 0) {
      *error = fixscript_create_error_string(heap, "can't lock memory cache mutex");
      goto error;
   }
#endif

   free(memory_cache_hash_set(&memory_cache, key, hash, NULL, 0));
   key = NULL;

#if defined(_WIN32)
   SetEvent(memory_cache_event);
   LeaveCriticalSection(&memory_cache_section);
#else
   pthread_cond_broadcast(&memory_cache_cond);
   pthread_mutex_unlock(&memory_cache_mutex);
#endif

error:
   free(key);
   return ret_val;
}


static Value memory_cache_ops_cond_swap(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value ret_val = fixscript_int(0), timeout;
   char *key = NULL, *value = NULL, *expect = NULL, *value_get, *tmp;
   int err, len;
   uint32_t hash = 0;
   uint64_t expiry_time = 0;
   char buf[5];

   err = fixscript_serialize_to_array(heap, &key, NULL, params[0]);
   if (err != FIXSCRIPT_SUCCESS) {
      fixscript_error(heap, error, err);
      goto error;
   }

   err = fixscript_serialize_to_array(heap, &expect, NULL, params[1]);
   if (err != FIXSCRIPT_SUCCESS) {
      fixscript_error(heap, error, err);
      goto error;
   }

   err = fixscript_serialize_to_array(heap, &value, NULL, params[2]);
   if (err != FIXSCRIPT_SUCCESS) {
      fixscript_error(heap, error, err);
      goto error;
   }

   if (num_params == 4) {
      timeout = params[num_params-1];
      if (!fixscript_is_int(timeout)) {
         *error = fixscript_create_error_string(heap, "timeout must be an integer");
         goto error;
      }
      if (timeout.value < 0) {
         *error = fixscript_create_error_string(heap, "timeout must not be negative");
         goto error;
      }
      if (timeout.value) {
         expiry_time = get_monotonic_time() + timeout.value;
      }
   }

   hash = compute_hash(key);
   err = FIXSCRIPT_SUCCESS;

#if defined(_WIN32)
   EnterCriticalSection(&memory_cache_section);
#else
   if (pthread_mutex_lock(&memory_cache_mutex) != 0) {
      *error = fixscript_create_error_string(heap, "can't lock memory cache mutex");
      goto error;
   }
#endif

   value_get = memory_cache_hash_get(&memory_cache, key, hash);
   if (!value_get) {
      set_serialized_zero(buf);
      value_get = buf;
   }

   if (key_equals(value_get, expect)) {
      value_get = memory_cache_hash_set(&memory_cache, key, hash, value, expiry_time);
      key = NULL;
      value = NULL;
#if defined(_WIN32)
      SetEvent(memory_cache_event);
#else
      pthread_cond_broadcast(&memory_cache_cond);
#endif
   }
   else {
      memcpy(&len, value_get, sizeof(int));
      tmp = malloc(sizeof(int)+len);
      if (tmp) {
         memcpy(tmp, value_get, sizeof(int)+len);
         value_get = tmp;
      }
      else {
         err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
   }

#if defined(_WIN32)
   LeaveCriticalSection(&memory_cache_section);
#else
   pthread_mutex_unlock(&memory_cache_mutex);
#endif

   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   if (value_get) {
      err = fixscript_unserialize_from_array(heap, value_get, NULL, -1, &ret_val);
      free(value_get);
      if (err) {
         fixscript_error(heap, error, err);
         goto error;
      }
   }

error:
   free(key);
   free(value);
   free(expect);
   return ret_val;
}


static Value memory_cache_wait(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   char *key = NULL, *expect = NULL, *value_get;
   Value ret_val = fixscript_int(0);
   uint32_t hash;
   int err;
   char buf[5];

   err = fixscript_serialize_to_array(heap, &key, NULL, params[0]);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   err = fixscript_serialize_to_array(heap, &expect, NULL, params[1]);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   hash = compute_hash(key);

#if defined(_WIN32)
   EnterCriticalSection(&memory_cache_section);
#else
   if (pthread_mutex_lock(&memory_cache_mutex) != 0) {
      *error = fixscript_create_error_string(heap, "can't lock memory cache mutex");
      goto error;
   }
#endif

   for (;;) {
      value_get = memory_cache_hash_get(&memory_cache, key, hash);
      if (!value_get) {
         set_serialized_zero(buf);
         value_get = buf;
      }
      if (!key_equals(value_get, expect)) {
         break;
      }

#if defined(_WIN32)
      LeaveCriticalSection(&memory_cache_section);
      WaitForSingleObject(memory_cache_event, INFINITE);
      EnterCriticalSection(&memory_cache_section);
#else
      pthread_cond_wait(&memory_cache_cond, &memory_cache_mutex);
#endif
   }

error:
#if defined(_WIN32)
   LeaveCriticalSection(&memory_cache_section);
#else
   pthread_mutex_unlock(&memory_cache_mutex);
#endif
   free(key);
   free(expect);
   return ret_val;
}


#if defined(_WIN32)
static DWORD WINAPI cleanup_thread(void *data)
#else
static void *cleanup_thread(void *data)
#endif
{
   uint64_t time, expiry_time;
   int i;
   
   for (;;) {
      time = get_monotonic_time();

#if defined(_WIN32)
      EnterCriticalSection(&memory_cache_section);
#else
      if (pthread_mutex_lock(&memory_cache_mutex) != 0) {
         fprintf(stderr, "can't lock memory cache mutex");
         exit(1);
      }
#endif

      for (i=0; i<memory_cache.size; i+=2) {
         expiry_time = memory_cache.expiry_time[i>>1];
         if (expiry_time != 0 && time >= expiry_time) {
            if (memory_cache.data[i+1]) {
               free(memory_cache.data[i+0]);
               free(memory_cache.data[i+1]);
               memory_cache.data[i+0] = calloc(1, sizeof(int));
               memory_cache.data[i+1] = NULL;
               memory_cache.expiry_time[i>>1] = 0;
               memory_cache.len--;
            }
         }
      }

#if defined(_WIN32)
      LeaveCriticalSection(&memory_cache_section);
#else
      pthread_mutex_unlock(&memory_cache_mutex);
#endif
      
#if defined(_WIN32)
      Sleep(1000);
#else
      sleep(1);
#endif
   }

#if defined(_WIN32)
   return 0;
#else
   return NULL;
#endif
}


#ifdef _WIN32
void init_critical_sections()
{
   InitializeCriticalSection(&monotonic_clock_section);
   InitializeCriticalSection(&strerror_section);
   InitializeCriticalSection(&memory_cache_section);
   memory_cache_event = CreateEvent(NULL, FALSE, FALSE, NULL);
   if (!memory_cache_event) {
      fprintf(stderr, "event creation failed\n");
      abort();
   }
}
#endif


#if !defined(_WIN32)
static char *search_file_on_path(const char *fname)
{
   FILE *f;
   char *path;
   char *s, *p, *buf;

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
      buf = string_format("%s/%s", s, fname);
      if (!buf) {
         return NULL;
      }
      f = fopen(buf, "rb");
      if (f) {
         fclose(f);
         free(path);
         return buf;
      }
      free(buf);
      if (p) {
         s = p+1;
      }
   }
   while (p);

   free(path);
   return NULL;
}
#endif


#if !defined(_WIN32)
static char *get_original_file_from_symlink(const char *fname)
{
   char *buf;
   int ret, len=256;

   for (;;) {
      buf = malloc(len);
      if (!buf) {
         return NULL;
      }
      ret = readlink(fname, buf, len-1);
      if (ret < 0) {
         free(buf);
         return NULL;
      }
      if (ret >= len-1) {
         free(buf);
         if (len > INT_MAX/2) {
            return NULL;
         }
         len *= 2;
         continue;
      }
      buf[ret] = '\0';
      return buf;
   }
}
#endif


int init_self_file(const char *argv0)
{
#if defined(_WIN32)
   uint16_t filename[256], *p;
   DWORD dwret;

   dwret = GetModuleFileNameW(NULL, filename, 255);
   if (dwret == 0 || dwret >= 254) {
      return 0;
   }
   p = wcsrchr(filename, '\\');
   if (p) *p = 0;
   exec_path = wcsdup(filename);
   return 1;
#else
   char *buf, *p, *orig;

   buf = search_file_on_path(argv0);
   if (buf) {
      for (;;) {
         orig = get_original_file_from_symlink(buf);
         if (!orig) break;
         free(buf);
         buf = orig;
      }
      p = strrchr(buf, '/');
      if (p) *p = '\0';
      if (strlen(buf) == 0) {
         free(buf);
         exec_path = strdup(".");
      }
      else {
         exec_path = buf;
      }
      return 1;
   }
   return 0;
#endif
}


void start_memory_cache_cleanup_thread()
{
#if defined(_WIN32)
   if (!CreateThread(NULL, 0, cleanup_thread, NULL, 0, NULL)) {
      fprintf(stderr, "error: can't start cleanup thread\n");
      exit(1);
   }
#else
   pthread_t thread;
   int err;

   err = pthread_create(&thread, NULL, cleanup_thread, NULL);
   if (err) {
      errno = err;
      perror("pthread_create");
      exit(1);
   }
#endif
}


static Value do_sleep(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(_WIN32)
   Sleep(fixscript_get_int(params[0]));
#else
   usleep(fixscript_get_int(params[0])*1000);
#endif
   return fixscript_int(0);
}


static Value charset_create_table(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   const char * const *p;
   const uint16_t *src = NULL, *s;
   char *name;
   Value arr, *values;
   int i, err, len;

   err = fixscript_get_string(heap, params[0], 0, -1, &name, NULL);
   if (err) return fixscript_error(heap, error, err);

   for (p = embed_charsets; *p; p += 2) {
      if (!strcmp(*p, name)) {
         src = (const uint16_t *)*(p+1);
         break;
      }
   }

   free(name);

   if (!src) {
      *error = fixscript_create_error_string(heap, "unknown charset name");
      return fixscript_int(0);
   }

   len = 0;
   for (s = src; *s != 0xFFFF; s++) {
      len++;
   }

   arr = fixscript_create_array(heap, len);
   if (!arr.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   values = malloc(len * sizeof(Value));
   if (!values) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   for (i=0; i<len; i++) {
      values[i] = fixscript_int(src[i]);
   }

   err = fixscript_set_array_range(heap, arr, 0, len, values);
   free(values);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   return arr;
}


static Value date_get_current(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   int mode = (int)data;
   Value arr, values[12];
#if defined(_WIN32)
   SYSTEMTIME st;
#else
   time_t t;
   struct tm tm;
#endif
   int i, err;

   arr = fixscript_create_array(heap, mode == 2? 12 : 6);
   if (!arr.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   for (i=0; i<6; i++) {
      values[i] = fixscript_int(0);
   }

#if defined(_WIN32)
   if (mode == 1) {
      GetLocalTime(&st);
   }
   else {
      GetSystemTime(&st);
   }
   values[0] = fixscript_int(st.wYear);
   values[1] = fixscript_int(st.wMonth);
   values[2] = fixscript_int(st.wDay);
   values[3] = fixscript_int(st.wHour);
   values[4] = fixscript_int(st.wMinute);
   values[5] = fixscript_int(st.wSecond);
#else
   time(&t);
   if (mode == 1) {
      localtime_r(&t, &tm);
   }
   else {
      gmtime_r(&t, &tm);
   }
   values[0] = fixscript_int(tm.tm_year+1900);
   values[1] = fixscript_int(tm.tm_mon+1);
   values[2] = fixscript_int(tm.tm_mday);
   values[3] = fixscript_int(tm.tm_hour);
   values[4] = fixscript_int(tm.tm_min);
   values[5] = fixscript_int(MIN(tm.tm_sec, 59));
#endif

#if defined(_WIN32)
   if (mode == 2) {
      // TODO: can get slightly different results
      GetLocalTime(&st);
      values[6] = fixscript_int(st.wYear);
      values[7] = fixscript_int(st.wMonth);
      values[8] = fixscript_int(st.wDay);
      values[9] = fixscript_int(st.wHour);
      values[10] = fixscript_int(st.wMinute);
      values[11] = fixscript_int(st.wSecond);
   }
#else
   if (mode == 2) {
      localtime_r(&t, &tm);
      values[6] = fixscript_int(tm.tm_year+1900);
      values[7] = fixscript_int(tm.tm_mon+1);
      values[8] = fixscript_int(tm.tm_mday);
      values[9] = fixscript_int(tm.tm_hour);
      values[10] = fixscript_int(tm.tm_min);
      values[11] = fixscript_int(MIN(tm.tm_sec, 59));
   }
#endif

   err = fixscript_set_array_range(heap, arr, 0, mode == 2? 12 : 6, values);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return arr;
}


static Value get_executable_path(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value ret;

   if (!exec_path) {
      *error = fixscript_create_error_string(heap, "internal error: executable path wasn't initialized");
      return fixscript_int(0);
   }

#if defined(_WIN32)
   ret = fixscript_create_string_utf16(heap, exec_path, -1);
#else
   ret = fixscript_create_string(heap, exec_path, -1);
#endif
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return ret;
}


void register_util_functions(Heap *heap)
{
   fixscript_register_native_func(heap, "clock_get_real_time#0", clock_get_real_time, NULL);
   fixscript_register_native_func(heap, "file_rename#2", file_rename, NULL);
   fixscript_register_native_func(heap, "memory_cache_get#1", memory_cache_ops_get, NULL);
   fixscript_register_native_func(heap, "memory_cache_get#2", memory_cache_ops_get, NULL);
   fixscript_register_native_func(heap, "memory_cache_set#2", memory_cache_ops_set, NULL);
   fixscript_register_native_func(heap, "memory_cache_set#3", memory_cache_ops_set, NULL);
   fixscript_register_native_func(heap, "memory_cache_remove#1", memory_cache_ops_remove, NULL);
   fixscript_register_native_func(heap, "memory_cache_cond_swap#3", memory_cache_ops_cond_swap, NULL);
   fixscript_register_native_func(heap, "memory_cache_cond_swap#4", memory_cache_ops_cond_swap, NULL);
   fixscript_register_native_func(heap, "memory_cache_wait#2", memory_cache_wait, NULL);
   fixscript_register_native_func(heap, "sleep#1", do_sleep, NULL);
   fixscript_register_native_func(heap, "charset_create_table#1", charset_create_table, NULL);
   fixscript_register_native_func(heap, "date_get_utc#0", date_get_current, (void *)0);
   fixscript_register_native_func(heap, "date_get_local#0", date_get_current, (void *)1);
   fixscript_register_native_func(heap, "date_get_both#0", date_get_current, (void *)2);
   fixscript_register_native_func(heap, "get_executable_path#0", get_executable_path, NULL);
}


#ifdef _WIN32
static void append_path_win(uint16_t **dest_ptr, const uint16_t *src, int len)
{
   memcpy(*dest_ptr, src, len*2);
   *dest_ptr += len;
}

static void append_path_cstr(uint16_t **dest_ptr, const char *src, int len)
{
   int i;

   for (i=0; i<len; i++) {
      if (src[i] == '/') {
         *(*dest_ptr)++ = '\\';
      }
      else {
         *(*dest_ptr)++ = (uint8_t)src[i];
      }
   }
}
#endif


Script *script_load_file(Heap *heap, const char *name, Value *error, const char *dirname)
{
#ifdef _WIN32
   HANDLE handle = INVALID_HANDLE_VALUE;
   Script *script = NULL;
   DWORD read;
   int len, len1, len2, len3;
   char *src = NULL, *src_ptr, *sname, *tmp;
   uint16_t *fname_utf16 = NULL, *fname_ptr;

   sname = string_format("%s.fix", name);
   script = fixscript_get(heap, sname);
   if (script) {
      free(sname);
      return script;
   }

   if (!exec_path) {
      if (error) {
         *error = fixscript_create_error_string(heap, "internal error: executable path wasn't initialized");
      }
      goto error;
   }
   
   if (!dirname) {
      dirname = ".";
   }

   len1 = (int)wcslen(exec_path);
   len2 = (int)strlen(dirname);
   len3 = (int)strlen(name);
   fname_utf16 = malloc((len1+1+len2+1+len3+4+1)*2);
   if (!fname_utf16) {
      if (error) {
         fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
      goto error;
   }
   fname_ptr = fname_utf16;
   append_path_win(&fname_ptr, exec_path, len1);
   *fname_ptr++ = '\\';
   append_path_cstr(&fname_ptr, dirname, len2);
   *fname_ptr++ = '\\';
   append_path_cstr(&fname_ptr, name, len3);
   append_path_cstr(&fname_ptr, ".fix", 4+1);

   handle = CreateFile(fname_utf16, GENERIC_READ, FILE_SHARE_DELETE | FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
   if (handle == INVALID_HANDLE_VALUE) {
      if (error) {
         tmp = string_format("script %s not found", name);
         *error = fixscript_create_string(heap, tmp, -1);
         free(tmp);
      }
      goto error;
   }
   len = GetFileSize(handle, NULL);
   src = malloc(len+1);
   if (!src) {
      goto error;
   }
   src_ptr = src;
   while (len > 0) {
      if (!ReadFile(handle, src_ptr, len, &read, NULL)) {
         if (error) {
            tmp = string_format("reading of script %s failed", name);
            *error = fixscript_create_string(heap, tmp, -1);
            free(tmp);
         }
         goto error;
      }
      src_ptr += read;
      len -= read;
   }
   *src_ptr = 0;

   script = fixscript_load(heap, src, sname, error, (LoadScriptFunc)script_load_file, (void *)dirname);

error:
   free(fname_utf16);
   free(src);
   free(sname);
   if (handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
   }
   return script;
#else
   Script *ret;
   char *tmp;

   if (!exec_path) {
      if (error) {
         *error = fixscript_create_error_string(heap, "internal error: executable path wasn't initialized");
      }
      return NULL;
   }

   if (!dirname) {
      return fixscript_load_file(heap, name, error, exec_path);
   }

   tmp = string_format("%s/%s", exec_path, dirname);
   if (!tmp) { 
      if (error) {
         fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
      return NULL;
   }
   ret = fixscript_load_file(heap, name, error, tmp);
   free(tmp);
   return ret;
#endif
}
