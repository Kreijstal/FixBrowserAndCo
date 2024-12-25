/*
 * FixScript v0.9 - https://www.fixscript.org/
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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <locale.h>
#include <pthread.h>
#include <unistd.h>
#include <wasm-support.h>
#include "fixscript.h"

// Math code available at http://public-domain.advel.cz/ under CC0 license

#ifdef __wasi__
static uint64_t time_origin = 0;

static void print(const char *s)
{
   wasi_iovec_t iovec;
   int ret;

   iovec.buf = (char *)s;
   iovec.len = strlen(s);
   wasi_fd_write(2, &iovec, 1, &ret);
}

static int grow_memory(int num_pages)
{
   return __builtin_wasm_memory_grow(0, num_pages);
}

static double get_monotonic_time()
{
   uint64_t time = 0;

   wasi_clock_time_get(WASI_CLOCKID_MONOTONIC, 0, &time);
   if (time_origin == 0) {
      if (time == 0) time++;
      time_origin = time;
   }
   return (time - time_origin) / 1000000.0;
}
#else
extern void print(const char *s) IMPORT("env", "print");
extern void refresh_memory() IMPORT("env", "refresh_memory");
extern double get_monotonic_time() IMPORT("env", "get_monotonic_time");

static int grow_memory(int num_pages)
{
   int ret;

   ret = __builtin_wasm_memory_grow(0, num_pages);
   refresh_memory();
   return ret;
}
#endif

static void *free_blocks[32];
static int heap_start, heap_end, heap_limit, heap_free;

extern int __heap_base;

#ifdef __wasi__
typedef struct Timer {
   uint64_t time;
   int repeat, interval;
   void (*func)(void *);
   void *data;
   struct Timer *next;
} Timer;

typedef struct DirPrefix {
   char *path;
   int len;
   int fd;
   struct DirPrefix *next;
} DirPrefix;

static Timer *timers = NULL;
static int exit_loop = 0;
static DirPrefix *dir_prefixes = NULL;
#endif

static int auto_suspend_num_instructions = 10000;

int errno;

extern int main(int argc, char **argv);

double string_to_double(const char *s);
char *double_to_string(char *buf, double input_value);


void *malloc(size_t size) EXPORT("malloc")
{
   void *block;
   int start, grow;
   
   if (size > 128*1024*1024) return NULL;
   if (size < 4) size = 4;
   size = 32 - __builtin_clz(size-1);

   block = free_blocks[size];
   if (block) {
      free_blocks[size] = *((void **)block);
      heap_free -= 4+(1<<size);
      return block;
   }

   if (heap_start == 0) {
      heap_start = (intptr_t)&__heap_base;
      #ifdef WASM_EMBEDDED_DATA
      {
         int len;
         memcpy(&len, (void *)heap_start, 4);
         heap_start += 4 + len;
      }
      #endif
      heap_start = (heap_start + 3) & ~3;
      heap_end = heap_start;
      heap_limit = __builtin_wasm_memory_size(0) << 16;
      #ifndef __wasi__
         refresh_memory();
      #endif
   }

   start = heap_end+4;
   if (size > 2) {
      start = (start+7)&~7;
   }

   if (start + (1<<size) > heap_limit) {
      grow = (start + (1<<size) - heap_limit + 0xFFFF) >> 16;
      if (grow < 16) grow = 16;
      if (grow_memory(grow) < 0) {
         return NULL;
      }
      heap_limit += grow << 16;
   }

   ((int *)start)[-1] = size;
   heap_end = start + (1<<size);
   return (void *)start;
}


void free(void *ptr) EXPORT("free")
{
   int size;
   
   if (!ptr) return;

   size = ((int *)ptr)[-1];
   *((void **)ptr) = free_blocks[size];
   free_blocks[size] = ptr;
   heap_free += 4+(1<<size);
}


void *calloc(size_t nmemb, size_t size) EXPORT("calloc")
{
   void *ptr;
   int64_t full_size = (int64_t)nmemb * (int64_t)size;

   if (full_size < 0 || full_size > INT_MAX) {
      return NULL;
   }

   ptr = malloc(full_size);
   if (!ptr) {
      return NULL;
   }

   memset(ptr, 0, full_size);
   return ptr;
}


void *realloc(void *ptr, size_t size) EXPORT("realloc")
{
   void *new_ptr;
   int cur_size;

   if (!ptr) {
      return malloc(size);
   }

   cur_size = 1 << (((int *)ptr)[-1]);
   if (size <= cur_size) {
      return ptr;
   }

   new_ptr = malloc(size);
   if (!new_ptr) return NULL;

   memcpy(new_ptr, ptr, cur_size);
   free(ptr);
   return new_ptr;
}


int malloc_get_heap_used()
{
   return (heap_end - heap_start) - heap_free;
}


int malloc_get_heap_free()
{
   return heap_free;
}


int malloc_get_heap_size()
{
   return heap_end - heap_start;
}


#ifdef WASM_EMBEDDED_DATA
const char *wasm_get_embedded_data(int *len)
{
   int start;

   start = (intptr_t)&__heap_base;
   if (len) {
      memcpy(len, (void *)start, 4);
   }
   return (void *)(start + 4);
}
#endif


void *memset(void *s, int c, size_t n)
{
   char *sc = s;
   uint64_t *start, *end, c8;

   if (n >= 16) {
      start = (uint64_t *)((((intptr_t)sc) + 7) & ~7);
      end = (uint64_t *)(((intptr_t)sc + n) & ~7);
      c8 = ((uint8_t)c) * 0x0101010101010101ULL;
      while (sc < (char *)start) {
         *sc++ = c;
      }
      while (start < end) {
         *start++ = c8;
      }
      sc = (char *)start;
      while (sc < ((char *)s)+n) {
         *sc++ = c;
      }
      return s;
   }

   while (n-- > 0) {
      *sc++ = c;
   }
   return s;
}


void *memcpy(void *dest, const void *src, size_t n)
{
   char *cdest = dest;
   const char *csrc = src;
   while (n-- > 0) {
      *cdest++ = *csrc++;
   }
   return dest;
}


void *memmove(void *dest, const void *src, size_t n)
{
   char *cdest = dest;
   const char *csrc = src;
   
   if ((intptr_t)cdest < (intptr_t)csrc) {
      while (n-- > 0) {
         *cdest++ = *csrc++;
      }
   }
   else {
      if (n == 0) return dest;
      cdest += n-1;
      csrc += n-1;
      while (n-- > 0) {
         *cdest-- = *csrc--;
      }
   }
   return dest;
}


int memcmp(const void *s1, const void *s2, size_t n)
{
   const char *c1 = s1;
   const char *c2 = s2;
   char v1, v2;
   while (n-- > 0) {
      v1 = *c1++;
      v2 = *c2++;
      if (v1 < v2) return -1;
      if (v1 > v2) return +1;
   }
   return 0;
}


char *strcpy(char *dest, const char *src)
{
   char *orig_dest = dest;
   while ((*dest++ = *src++));
   return orig_dest;
}


char *strcat(char *dest, const char *src)
{
   char *orig_dest = dest;
   for (; *dest; dest++);
   while ((*dest++ = *src++));
   return orig_dest;
}


size_t strlen(const char *s)
{
   size_t len = 0;
   while (*s++) {
      len++;
   }
   return len;
}


int strcmp(const char *s1, const char *s2)
{
   char c1, c2;
   for (;;) {
      c1 = *s1++;
      c2 = *s2++;
      if (c1 < c2) return -1;
      if (c1 > c2) return +1;
      if (c1 == 0) return 0;
   }
   return 0;
}


int strncmp(const char *s1, const char *s2, size_t n)
{
   char c1, c2;
   while (n-- > 0) {
      c1 = *s1++;
      c2 = *s2++;
      if (c1 < c2) return -1;
      if (c1 > c2) return +1;
      if (c1 == 0) return 0;
   }
   return 0;
}


char *strdup(const char *s)
{
   char *dup;
   int len;

   len = strlen(s);
   dup = malloc(len+1);
   if (!dup) return NULL;
   memcpy(dup, s, len+1);
   return dup;
}


char *strchr(const char *s, int c)
{
   for (; *s; s++) {
      if (*s == c) return (char *)s;
   }
   return NULL;
}


char *strrchr(const char *s, int c)
{
   const char *e = s;
   for (; *e; e++);
   for (; e >= s; e--) {
      if (*e == c) return (char *)e;
   }
   return NULL;
}


char *strstr(const char *haystack, const char *needle)
{
   const char *s = haystack, *s1, *s2;
   char c = *needle++, c1, c2;
   if (c == 0) return (char *)s;
   for (; *s; s++) {
      if (*s == c) {
         s1 = s+1;
         s2 = needle;
         for (;;) {
            c1 = *s1++;
            c2 = *s2++;
            if (c2 == 0) return (char *)s;
            if (c1 == 0) break;
            if (c1 != c2) break;
         }
      }
   }
   return NULL;
}


double strtod(const char *nptr, char **endptr)
{
   return strtod_l(nptr, endptr, 0);
}


double strtod_l(const char *nptr, char **endptr, locale_t locale)
{
   double value;

   value = string_to_double(nptr);
   if (endptr) {
      *endptr = value == value? (char *)nptr + strlen(nptr) : (char *)nptr;
   }
   return value;
}


long int strtol(const char *nptr, char **endptr, int base)
{
   int value = 0, neg = 0;
   char c;
   if (strncmp(nptr, "-2147483648", 11) == 0) {
      if (endptr) *endptr = (char *)nptr+11;
      return 0x80000000;
   }
   if (*nptr == '+') {
      nptr++;
   }
   else if (*nptr == '-') {
      nptr++;
      neg = 1;
   }
   for (;;) {
      c = *nptr;
      if (c < '0' || c > '9') break;
      if (value > 214748364) {
         if (endptr) *endptr = (char *)nptr;
         return value;
      }
      value *= 10;
      c -= '0';
      if (c > 2147483647 - value) {
         if (endptr) *endptr = (char *)nptr;
         return value;
      }
      value += c;
      nptr++;
   }
   if (neg) value = -value;
   if (endptr) *endptr = (char *)nptr;
   return value;
}


long long int strtoll(const char *nptr, char **endptr, int base)
{
   int64_t value = 0, neg = 0;
   char c;
   if (strncmp(nptr, "-9223372036854775808", 20) == 0) {
      if (endptr) *endptr = (char *)nptr+20;
      return 0x8000000000000000ULL;
   }
   if (*nptr == '+') {
      nptr++;
   }
   else if (*nptr == '-') {
      nptr++;
      neg = 1;
   }
   for (;;) {
      c = *nptr;
      if (c < '0' || c > '9') break;
      if (value > 922337203685477580LL) {
         if (endptr) *endptr = (char *)nptr;
         return value;
      }
      value *= 10;
      c -= '0';
      if (c > 9223372036854775807LL - value) {
         if (endptr) *endptr = (char *)nptr;
         return value;
      }
      value += c;
      nptr++;
   }
   if (neg) value = -value;
   if (endptr) *endptr = (char *)nptr;
   return value;
}


unsigned long int strtoul(const char *nptr, char **endptr, int base)
{
   uint32_t value = 0;
   const char *start;
   char c;
   if (nptr[0] == '0' && nptr[1] == 'x') {
      nptr += 2;
   }
   start = nptr;
   for (;;) {
      c = *nptr;
      if (c >= '0' && c <= '9') {
         value = (value << 4) + (c - '0');
         nptr++;
         continue;
      }
      if (c >= 'a' && c <= 'f') {
         value = (value << 4) + (c - 'a' + 10);
         nptr++;
         continue;
      }
      if (c >= 'A' && c <= 'F') {
         value = (value << 4) + (c - 'A' + 10);
         nptr++;
         continue;
      }
      break;
   }
   if (endptr) {
      if (nptr - start > 8) {
         *endptr = (char *)start + 8;
      }
      else {
         *endptr = (char *)nptr;
      }
   }
   return value;
}


int atoi(const char *nptr)
{
   return strtol(nptr, NULL, 10);
}


void qsort(void *base, size_t nmemb, size_t size, int(*compar)(const void *, const void *))
{
   int i, left = 0, right = ((int)nmemb)-1, left_idx = left, right_idx = right, pivot;
   char *p1, *p2, tmp;

   if (right_idx > left_idx) {
      pivot = left + ((right - left) >> 1);
      while (left_idx <= pivot && right_idx >= pivot) {
         for (;;) {
            if (compar(base + left_idx*size, base + pivot*size) < 0 && left_idx <= pivot) {
               left_idx++;
               continue;
            }
            break;
         }
         for (;;) {
            if (compar(base + pivot*size, base + right_idx*size) < 0 && right_idx >= pivot) {
               right_idx--;
               continue;
            }
            break;
         }
         p1 = base + left_idx*size;
         p2 = base + right_idx*size;
         for (i=0; i<size; i++) {
            tmp = *p1;
            *p1++ = *p2;
            *p2++ = tmp;
         }
         left_idx++;
         right_idx--;
         if (left_idx - 1 == pivot) {
            pivot = right_idx = right_idx + 1;
         }
         else if (right_idx + 1 == pivot) {
            pivot = left_idx = left_idx - 1;
         }
      }
      if (pivot > 0) qsort(base + left*size, (size_t)(pivot - left), size, compar);
      qsort(base + (pivot+1)*size, (size_t)(right - (pivot + 1) + 1), size, compar);
   }
}


int abs(int j)
{
   return j < 0? -j : j;
}


void abort()
{
   __builtin_trap();
}


int fprintf(FILE *stream, const char *format, ...)
{
   char buf[4096];
   va_list ap;

   va_start(ap, format);
   vsnprintf(buf, sizeof(buf), format, ap);
   va_end(ap);
   print(buf);
   return 0;
}


int sprintf(char *str, const char *format, ...)
{
   va_list ap;
   int ret;

   va_start(ap, format);
   ret = vsnprintf(str, INT_MAX, format, ap);
   va_end(ap);
   return ret;
}


int snprintf(char *str, size_t size, const char *format, ...)
{
   va_list ap;
   int ret;

   va_start(ap, format);
   ret = vsnprintf(str, size, format, ap);
   va_end(ap);
   return ret;
}


static void int_to_string(char *str, int value)
{
   char buf[12], *s = buf+12;
   int sign = 0;

   if (value == 0x80000000) {
      strcpy(str, "-2147483648");
      return;
   }
   if (value == 0) {
      strcpy(str, "0");
      return;
   }

   if (value < 0) {
      value = -value;
      sign = 1;
   }

   *(--s) = 0;
   while (value) {
      *(--s) = (value % 10) + '0';
      value /= 10;
   }
   if (sign) {
      *(--s) = '-';
   }
   strcpy(str, s);
}


static void long_to_string(char *str, int64_t value)
{
   char buf[21], *s = buf+21;
   int sign = 0;

   if (value == 0x8000000000000000ULL) {
      strcpy(str, "-9223372036854775808");
      return;
   }
   if (value == 0) {
      strcpy(str, "0");
      return;
   }

   if (value < 0) {
      value = -value;
      sign = 1;
   }

   *(--s) = 0;
   while (value) {
      *(--s) = (value % 10) + '0';
      value /= 10;
   }
   if (sign) {
      *(--s) = '-';
   }
   strcpy(str, s);
}


static void adjust_fractional_digits(char *s, int num_digits, int add)
{
   char *p, *after_dot = NULL;

   for (p=s; *p; p++) {
      if (*p == 'e') {
         return;
      }
      if (*p == '.') {
         after_dot = p+1;
      }
   }

   if (after_dot) {
      if (p - after_dot > num_digits) {
         *(after_dot + num_digits) = '\0';
      }
      else if (add) {
         num_digits -= p - after_dot;
         while (num_digits > 0) {
            *p++ = '0';
            num_digits--;
         }
         *p = '\0';
      }
   }
   else if (add) {
      *p++ = '.';
      while (num_digits > 0) {
         *p++ = '0';
         num_digits--;
      }
      *p = '\0';
   }
}


int vsnprintf(char *str, size_t size, const char *format, va_list ap)
{
   int i, cnt=0, limit, num_digits;
   char c, *s2, buf[64];

   if (size > INT_MAX) {
      size = INT_MAX;
   }
   limit = str? size : -1;

   for (;;) {
      c = *format++;
      if (!c) break;

      if (c == '%') {
         c = *format++;
         switch (c) {
            case '%':
               if (cnt++ < limit) {
                  *str++ = c;
               }
               break;

            case 'c':
               c = (char)va_arg(ap, int);
               if (cnt++ < limit) {
                  *str++ = c;
               }
               break;

            case 's':
               s2 = va_arg(ap, char *);
               for (;;) {
                  c = *s2++;
                  if (!c) break;
                  if (cnt++ < limit) {
                     *str++ = c;
                  }
               }
               break;

            case 'd':
            case 'p':
               int_to_string(buf, va_arg(ap, int));
               s2 = buf;
               for (;;) {
                  c = *s2++;
                  if (!c) break;
                  if (cnt++ < limit) {
                     *str++ = c;
                  }
               }
               break;

            case 'l':
               if (*format++ == 'l') {
                  if (*format++ == 'd') {
                     long_to_string(buf, va_arg(ap, int64_t));
                     s2 = buf;
                     for (;;) {
                        c = *s2++;
                        if (!c) break;
                        if (cnt++ < limit) {
                           *str++ = c;
                        }
                     }
                  }
               }
               break;

            case 'f':
               s2 = double_to_string(buf, va_arg(ap, double));
               for (;;) {
                  c = *s2++;
                  if (!c) break;
                  if (cnt++ < limit) {
                     *str++ = c;
                  }
               }
               break;

            case '.':
               num_digits = 0;
               for (;;) {
                  c = *format++;
                  if (c >= '0' && c <= '9') {
                     num_digits = num_digits*10 + (c - '0');
                  }
                  else break;
               }
               if (c == '*') {
                  num_digits = va_arg(ap, int);
                  c = *format++;
               }
               switch (c) {
                  case 's':
                     s2 = va_arg(ap, char *);
                     for (i=0; i<num_digits; i++) {
                        if (cnt++ < limit) {
                           *str++ = *s2++;
                        }
                     }
                     break;

                  case 'f':
                  case 'g':
                     s2 = double_to_string(buf, va_arg(ap, double));
                     if (num_digits >= 0) {
                        adjust_fractional_digits(s2, num_digits < 20? num_digits : 20, c == 'f');
                     }
                     for (;;) {
                        c = *s2++;
                        if (!c) break;
                        if (cnt++ < limit) {
                           *str++ = c;
                        }
                     }
                     break;
               }
               break;
         }
      }
      else {
         if (cnt++ < limit) {
            *str++ = c;
         }
      }
   }
   if (limit >= 0) {
      *str = 0;
   }
   return cnt;
}


FILE *fopen(const char *path, const char *mode)
{
   return NULL;
}


size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
   return 0;
}


size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
   fprintf(stderr, "%.*s", (int)size*(int)nmemb, (const char *)ptr);
   return 0;
}


int fflush(FILE *stream)
{
   return 0;
}


int ferror(FILE *stream)
{
   return 0;
}


int fclose(FILE *fp)
{
   return 0;
}


int fputc(int c, FILE *stream)
{
   char buf[2];

   buf[0] = c;
   buf[1] = 0;
   print(buf);
   return (unsigned char)c;
}


int fputs(const char *s, FILE *stream)
{
   print(s);
   return 0;
}


locale_t newlocale(int category, const char *name, locale_t base)
{
   return (void *)1;
}


void freelocale(locale_t locale)
{
}


locale_t uselocale(locale_t locale)
{
   return locale;
}


int clock_gettime(clockid_t clk_id, struct timespec *tp)
{
   double time = get_monotonic_time();
   tp->tv_sec = time / 1000.0;
   tp->tv_nsec = (time - tp->tv_sec * 1000.0) * 1000000.0;
   return 0;
}


int pthread_mutex_init(pthread_mutex_t *mutex, pthread_mutexattr_t *attr)
{
   return 0;
}


int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
   return 0;
}


int pthread_mutex_lock(pthread_mutex_t *mutex)
{
   return 0;
}


int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
   return 0;
}


int pthread_cond_init(pthread_cond_t *cond, void *attr)
{
   return 0;
}


int pthread_cond_destroy(pthread_cond_t *cond)
{
   return 0;
}


int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
   return 0;
}


int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abstime)
{
   return 0;
}


int pthread_cond_signal(pthread_cond_t *cond)
{
   return 0;
}


int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg)
{
   start_routine(arg);
   return 0;
}


int pthread_detach(pthread_t thread)
{
   return 0;
}


int pthread_mutexattr_init(pthread_mutexattr_t *attr)
{
   return 0;
}


int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type)
{
   return 0;
}


long sysconf(int name)
{
   if (name == _SC_NPROCESSORS_ONLN) return 1;
   return -1;
}


#ifdef __wasi__

static void register_dir_prefix(const char *path, int fd)
{
   DirPrefix *prefix;

   prefix = malloc(sizeof(DirPrefix));
   prefix->path = strdup(path);
   prefix->len = strlen(path);
   prefix->fd = fd;
   prefix->next = dir_prefixes;
   dir_prefixes = prefix;

   if (strcmp(path, ".") == 0) {
      register_dir_prefix("", fd);
   }

   if (strncmp(path, "./", 2) == 0) {
      register_dir_prefix(path+2, fd);
   }
}


static int find_dir_prefix(const char *fname, char **actual_name_out, int *fd_out)
{
   DirPrefix *prefix;
   char *actual_name;

   for (prefix = dir_prefixes; prefix; prefix = prefix->next) {
      if (strncmp(fname, prefix->path, prefix->len) == 0) {
         actual_name = strdup(fname + prefix->len);
         if (!actual_name) {
            return WASM_ERROR_OUT_OF_MEMORY;
         }
         *actual_name_out = actual_name;
         *fd_out = prefix->fd;
         return WASM_SUCCESS;
      }
   }
   return WASM_ERROR_FILE_NOT_FOUND;
}


void _start() EXPORT("_start")
{
   wasi_prestat_t prestat;
   int argc, buf_size;
   char **argv, *argv_buf, *s;
   wasi_errno_t wasi_err;
   int i;

   wasi_args_sizes_get(&argc, &buf_size);
   
   argv = calloc(argc+1, sizeof(char *));
   argv_buf = malloc(buf_size);
   wasi_args_get((uint8_t **)argv, (uint8_t *)argv_buf);

   for (i=3; ; i++) {
      wasi_err = wasi_fd_prestat_get(i, &prestat);
      if (wasi_err == WASI_ERRNO_BADF || wasi_err != WASI_ERRNO_SUCCESS) {
         break;
      }
      if (prestat.tag == WASI_PREOPENTYPE_DIR) {
         s = malloc(prestat.dir.pr_name_len+1);
         if (s) {
            wasi_err = wasi_fd_prestat_dir_name(i, (uint8_t *)s, prestat.dir.pr_name_len);
            if (wasi_err == WASI_ERRNO_SUCCESS) {
               s[prestat.dir.pr_name_len] = 0;
               register_dir_prefix(s, i);
            }
            free(s);
         }
      }
   }

   wasi_proc_exit(main(argc, argv));
}


static void insert_timer(Timer *timer)
{
   Timer *t, **prev;

   prev = &timers;
   if (timer->time == 0) {
      for (t = timers; t; t = t->next) {
         if (t->time != 0) {
            timer->next = t;
            *prev = timer;
            return;
         }
         prev = &t->next;
      }
   }
   else {
      for (t = timers; t; t = t->next) {
         if (t->time != 0 && timer->time < t->time) {
            timer->next = t;
            *prev = timer;
            return;
         }
         prev = &t->next;
      }
   }

   timer->next = NULL;
   *prev = timer;
}


wasm_timer_t wasm_timer_start(int interval, int repeat, void (*func)(void *), void *data)
{
   Timer *timer;
   uint64_t time = 0;

   timer = malloc(sizeof(Timer));
   if (!timer) {
      fprintf(stderr, "error: out of memory\n");
      __builtin_trap();
   }

   if (interval > 0) {
      wasi_clock_time_get(WASI_CLOCKID_MONOTONIC, 0, &time);
      if (time_origin == 0) {
         if (time == 0) time++;
         time_origin = time;
      }
      timer->time = (time - time_origin) + interval * 1000000ULL;
   }
   else {
      timer->time = 0;
   }

   timer->repeat = repeat;
   timer->interval = interval;
   timer->func = func;
   timer->data = data;

   insert_timer(timer);
   return timer;
}


void wasm_timer_stop(wasm_timer_t timer)
{
   Timer *t, **prev;

   prev = &timers;
   for (t = timers; t; t = t->next) {
      if (t == timer) {
         *prev = t->next;
         free(t);
         break;
      }
   }
}


static void do_sleep(int64_t timeout)
{
   wasi_subscription_t sub;
   wasi_event_t evt;
   int num_events;

   sub.tag = WASI_EVENTTYPE_CLOCK;
   sub.clock.id = WASI_CLOCKID_MONOTONIC;
   sub.clock.timeout = timeout;
   sub.clock.precision = 0;
   sub.clock.flags = 0;

   wasi_poll_oneoff(&sub, &evt, 1, &num_events);
}


void wasi_run_loop()
{
   Timer *t, **prev, *immediates = NULL;
   uint64_t time = 0;
   int64_t delay;
   void (*func)(void *);
   void *data;

   while (!exit_loop) {
      if (!immediates) {
         if (timers && timers->time == 0) {
            immediates = timers;
            prev = &timers;
            for (t = timers; t; t = t->next) {
               if (t->time != 0) {
                  *prev = NULL;
                  timers = t;
                  break;
               }
               prev = &t->next;
            }
            if (immediates == timers) {
               timers = NULL;
            }
         }
      }

      func = NULL;
      delay = 0;

      prev = &timers;
      for (t = timers; t; t = t->next) {
         if (t->time != 0) {
            wasi_clock_time_get(WASI_CLOCKID_MONOTONIC, 0, &time);
            if (time_origin == 0) {
               if (time == 0) time++;
               time_origin = time;
            }
            delay = t->time - (time - time_origin);
            if (delay <= 0) {
               func = t->func;
               data = t->data;
               *prev = t->next;
               if (t->repeat) {
                  t->time += t->interval * 1000000ULL;
                  if (t->time - (time - time_origin) < 0) {
                     t->time = time - time_origin;
                  }
                  insert_timer(t);
               }
               else {
                  free(t);
               }
            }
            break;
         }
         prev = &t->next;
      }

      if (func) {
         func(data);
         continue;
      }

      if (immediates) {
         t = immediates;
         t->func(t->data);
         immediates = t->next;
         if (t->repeat) {
            insert_timer(t);
         }
         else {
            free(t);
         }
         continue;
      }

      if (!timers) {
         fprintf(stderr, "error: async failure\n");
         fflush(stderr);
         break;
      }
      do_sleep(delay);
   }

   exit_loop = 0;

   if (immediates) {
      for (t = immediates; t; t = t->next) {
         if (!t->next) {
            t->next = timers;
            timers = immediates;
            break;
         }
      }
   }
}


void wasi_exit_loop()
{
   exit_loop = 1;
}

#else

int wasm_main() EXPORT("main")
{
   return main(0, NULL);
}

#endif /* __wasi__ */


void wasm_auto_suspend_set_num_instructions(int num_instructions)
{
   auto_suspend_num_instructions = num_instructions;
}


int wasm_auto_suspend_get_num_instructions()
{
   return auto_suspend_num_instructions;
}


static void reschedule_thread(ContinuationFunc resume_func, void *resume_data, void *data)
{
   wasm_sleep(0, resume_func, resume_data);
}


void wasm_auto_suspend_heap(void *heap)
{
   ContinuationSuspendFunc func;

   fixscript_get_auto_suspend_handler(heap, NULL, &func, NULL);
   if (!func) {
      fixscript_set_auto_suspend_handler(heap, auto_suspend_num_instructions, reschedule_thread, NULL);
   }
}


const char *wasm_get_error_msg(int error)
{
   switch (error) {
      case WASM_SUCCESS:              return "success";
      case WASM_ERROR_NOT_SUPPORTED:  return "not supported";
      case WASM_ERROR_OUT_OF_MEMORY:  return "out of memory";
      case WASM_ERROR_FILE_NOT_FOUND: return "file not found";
      case WASM_ERROR_IO_ERROR:       return "I/O error";
   }
   return "unknown error";
}


////////////////////////////////////////////////////////////////////////////////
// File library:
////////////////////////////////////////////////////////////////////////////////


struct wasm_file_t {
   #ifdef __wasi__
      int fd;
   #endif
};

#ifdef __wasi__

static int wasi_impl_path_create_directory(const char *path)
{
   char *actual_name;
   int err, dir_fd;

   err = find_dir_prefix(path, &actual_name, &dir_fd);
   if (err) {
      return err;
   }

   err = wasi_path_create_directory(dir_fd, actual_name, strlen(actual_name));
   free(actual_name);
   if (err != WASI_ERRNO_SUCCESS) {
      return WASM_ERROR_IO_ERROR;
   }
   return WASM_SUCCESS;
}


static int wasi_impl_path_delete_file(const char *path)
{
   char *actual_name;
   int err, dir_fd;

   err = find_dir_prefix(path, &actual_name, &dir_fd);
   if (err) {
      return err;
   }

   err = wasi_path_unlink_file(dir_fd, actual_name, strlen(actual_name));
   free(actual_name);
   if (err != WASI_ERRNO_SUCCESS) {
      return WASM_ERROR_IO_ERROR;
   }
   return WASM_SUCCESS;
}


static int wasi_impl_path_delete_directory(const char *path)
{
   char *actual_name;
   int err, dir_fd;

   err = find_dir_prefix(path, &actual_name, &dir_fd);
   if (err) {
      return err;
   }

   err = wasi_path_remove_directory(dir_fd, actual_name, strlen(actual_name));
   free(actual_name);
   if (err != WASI_ERRNO_SUCCESS) {
      return WASM_ERROR_IO_ERROR;
   }
   return WASM_SUCCESS;
}


static int wasi_impl_path_get_files(const char *path, int *num_files_out, char ***files_out)
{
   wasi_dirent_t dirent;
   char *actual_name;
   char **files = NULL, **new_files, *fname;
   uint8_t *buf = NULL, *new_buf;
   uint64_t cookie = 0;
   int i, err, dir_fd, fd = -1, files_cnt = 0, files_cap = 0, buf_size = 4096, buf_pos, buf_used, ret_code = WASM_ERROR_IO_ERROR;

   err = find_dir_prefix(path, &actual_name, &dir_fd);
   if (err) {
      return err;
   }

   err = wasi_path_open(dir_fd, WASI_LOOKUPFLAGS_SYMLINK_FOLLOW, actual_name, strlen(actual_name), WASI_OFLAGS_DIRECTORY, WASI_RIGHTS_FD_READDIR, WASI_RIGHTS_FD_READDIR, 0, &fd);
   free(actual_name);
   if (err) {
      if (err == WASI_ERRNO_NOENT) {
         return WASM_ERROR_FILE_NOT_FOUND;
      }
      else if (err == WASI_ERRNO_NOTDIR) {
         return WASM_ERROR_NOT_DIRECTORY;
      }
      return WASM_ERROR_IO_ERROR;
   }

   buf = malloc(buf_size);
   if (!buf) {
      goto error;
   }

   for (;;) {
      err = wasi_fd_readdir(fd, buf, buf_size, cookie, &buf_used);
      if (err) {
         if (err == WASI_ERRNO_NOENT) {
            ret_code = WASM_ERROR_FILE_NOT_FOUND;
         }
         else if (err == WASI_ERRNO_NOTDIR) {
            ret_code = WASM_ERROR_NOT_DIRECTORY;
         }
         else {
            ret_code = WASM_ERROR_IO_ERROR;
         }
         goto error;
      }
      if (buf_used == 0) break;
      buf_pos = 0;

      while (buf_used - buf_pos >= sizeof(wasi_dirent_t)) {
         memcpy(&dirent, buf + buf_pos, sizeof(wasi_dirent_t));
         buf_pos += sizeof(wasi_dirent_t);
         if (buf_used - buf_pos < dirent.d_namlen) {
            while (buf_size < sizeof(wasi_dirent_t) + dirent.d_namlen) {
               if (buf_size > INT_MAX/2) {
                  goto error;
               }
               buf_size <<= 1;
               new_buf = malloc(buf_size);
               if (!new_buf) {
                  goto error;
               }
               buf = new_buf;
            }
            break;
         }

         fname = malloc(dirent.d_namlen+1);
         if (!fname) {
            goto error;
         }
         memcpy(fname, buf + buf_pos, dirent.d_namlen);
         fname[dirent.d_namlen] = 0;
         buf_pos += dirent.d_namlen;
         if (files_cnt == files_cap) {
            if (files_cap > (INT_MAX/2)/sizeof(char *)) {
               goto error;
            }
            if (files_cap == 0) {
               files_cap = 64;
            }
            else {
               files_cap <<= 1;
            }
            new_files = realloc(files, files_cap * sizeof(char *));
            if (!new_files) {
               goto error;
            }
            files = new_files;
         }
         files[files_cnt++] = fname;
         cookie = dirent.d_next;
      }
      if (buf_used - buf_pos == 0 && buf_used < buf_size) break;
   }

   ret_code = WASM_SUCCESS;

error:
   free(buf);
   if (ret_code == WASM_SUCCESS) {
      *num_files_out = files_cnt;
      *files_out = files;
   }
   else {
      for (i=0; i<files_cnt; i++) {
         free(files[i]);
      }
      free(files);
      *num_files_out = 0;
      *files_out = NULL;
   }
   if (fd != -1) {
      wasi_fd_close(fd);
   }
   return ret_code;
}


static int wasi_impl_path_exists(const char *path, int *exists_out)
{
   wasi_filestat_t filestat;
   char *actual_name;
   int err, dir_fd;

   err = find_dir_prefix(path, &actual_name, &dir_fd);
   if (err) {
      return err;
   }

   err = wasi_path_filestat_get(dir_fd, WASI_LOOKUPFLAGS_SYMLINK_FOLLOW, actual_name, strlen(actual_name), &filestat);
   free(actual_name);
   if (err == WASI_ERRNO_NOENT) {
      *exists_out = 0;
      return WASM_SUCCESS;
   }
   if (err == WASI_ERRNO_PERM) {
      *exists_out = 0;
      return WASM_SUCCESS;
      //return WASM_ERROR_ACCESS_DENIED;
   }
   if (err != WASI_ERRNO_SUCCESS) {
      *exists_out = 0;
      return WASM_ERROR_IO_ERROR;
   }
   *exists_out = 1;
   return WASM_SUCCESS;
}


static int wasi_impl_path_get_info(const char *path, int *type_out, int64_t *length_out, int64_t *mtime_out)
{
   wasi_filestat_t filestat, filestat2;
   char *actual_name;
   int err, err2, dir_fd;

   err = find_dir_prefix(path, &actual_name, &dir_fd);
   if (err) {
      return err;
   }

   err = wasi_path_filestat_get(dir_fd, WASI_LOOKUPFLAGS_SYMLINK_FOLLOW, actual_name, strlen(actual_name), &filestat);
   err2 = wasi_path_filestat_get(dir_fd, 0, actual_name, strlen(actual_name), &filestat2);
   free(actual_name);
   if (err != WASI_ERRNO_SUCCESS || err2 != WASI_ERRNO_SUCCESS) {
      if (err == WASI_ERRNO_PERM || err2 == WASI_ERRNO_PERM) {
         if (type_out) {
            *type_out = WASM_FILE_TYPE_SPECIAL;
         }
         if (length_out) {
            *length_out = 0;
         }
         if (mtime_out) {
            *mtime_out = 0;
         }
         return WASM_SUCCESS;
         //return WASM_ERROR_ACCESS_DENIED;
      }
      if (err == WASI_ERRNO_NOENT) {
         if (err != WASI_ERRNO_SUCCESS && err2 == WASI_ERRNO_SUCCESS) {
            if (type_out) {
               *type_out = WASM_FILE_TYPE_SPECIAL | WASM_FILE_TYPE_SYMLINK;
            }
            if (length_out) {
               *length_out = 0;
            }
            if (mtime_out) {
               *mtime_out = filestat2.mtim / 1000000000LL;
            }
            return WASM_SUCCESS;
         }
         return WASM_ERROR_FILE_NOT_FOUND;
      }
      return WASM_ERROR_IO_ERROR;
   }
   if (type_out) {
      if (filestat.filetype == WASI_FILETYPE_REGULAR_FILE) {
         *type_out = WASM_FILE_TYPE_FILE;
      }
      else if (filestat.filetype == WASI_FILETYPE_DIRECTORY) {
         *type_out = WASM_FILE_TYPE_DIRECTORY;
      }
      else {
         *type_out = WASM_FILE_TYPE_SPECIAL;
      }
      if (filestat2.filetype == WASI_FILETYPE_SYMBOLIC_LINK) {
         *type_out |= WASM_FILE_TYPE_SYMLINK;
      }
   }
   if (length_out) {
      *length_out = filestat.size;
   }
   if (mtime_out) {
      *mtime_out = filestat.mtim / 1000000000LL;
   }
   return WASM_SUCCESS;
}


static int wasi_impl_path_resolve_symlink(const char *path, char **orig_path_out)
{
   char *actual_name, *buf;
   int err, dir_fd, len, new_len;

   err = find_dir_prefix(path, &actual_name, &dir_fd);
   if (err) {
      return err;
   }

   len = 1;
   buf = malloc(len);
   if (!buf) {
      free(actual_name);
      return WASM_ERROR_OUT_OF_MEMORY;
   }

   for (;;) {
      err = wasi_path_readlink(dir_fd, path, strlen(path), (uint8_t *)buf, len, &new_len);
      if (err) {
         free(actual_name);
         free(buf);
         *orig_path_out = NULL;
         return WASM_ERROR_IO_ERROR;
      }
      if (new_len < len) {
         break;
      }
      if (len > INT_MAX/2) {
         free(actual_name);
         free(buf);
         *orig_path_out = NULL;
         return WASM_ERROR_OUT_OF_MEMORY;
      }
      len *= 2;
      free(buf);
      buf = malloc(len);
      if (!buf) {
         free(actual_name);
         free(buf);
         *orig_path_out = NULL;
         return WASM_ERROR_OUT_OF_MEMORY;
      }
   }

   free(actual_name);
   *orig_path_out = buf;
   return WASM_SUCCESS;
}


static int wasi_impl_file_open(const char *path, int flags, wasm_file_t **file_out)
{
   wasm_file_t *file;
   char *actual_name = NULL;
   uint16_t oflags = 0, fdflags = 0;
   uint64_t fs_rights = 0;
   int err, dir_fd = 0;

   err = find_dir_prefix(path, &actual_name, &dir_fd);
   if (err) {
      return err;
   }

   if (flags & WASM_FILE_READ) {
      fs_rights |= WASI_RIGHTS_FD_READ;
      fs_rights |= WASI_RIGHTS_FD_SEEK;
      fs_rights |= WASI_RIGHTS_FD_TELL;
      fs_rights |= WASI_RIGHTS_FD_FILESTAT_GET;
   }
   if (flags & WASM_FILE_WRITE) {
      fs_rights |= WASI_RIGHTS_FD_DATASYNC;
      fs_rights |= WASI_RIGHTS_FD_SYNC;
      fs_rights |= WASI_RIGHTS_FD_WRITE;
      fs_rights |= WASI_RIGHTS_FD_FILESTAT_SET_SIZE;
   }
   if (flags & WASM_FILE_CREATE) oflags |= WASI_OFLAGS_CREAT;
   if (flags & WASM_FILE_TRUNCATE) oflags |= WASI_OFLAGS_TRUNC;
   if (flags & WASM_FILE_APPEND) fdflags |= WASI_FDFLAGS_APPEND;

   file = calloc(1, sizeof(wasm_file_t));
   if (!file) {
      return WASM_ERROR_OUT_OF_MEMORY;
   }

   err = wasi_path_open(dir_fd, WASI_LOOKUPFLAGS_SYMLINK_FOLLOW, actual_name, strlen(actual_name), oflags, fs_rights, fs_rights, fdflags, &file->fd);
   free(actual_name);
   if (err != WASI_ERRNO_SUCCESS) {
      free(file);
      if (err == WASI_ERRNO_NOENT) {
         return WASM_ERROR_FILE_NOT_FOUND;
      }
      return WASM_ERROR_IO_ERROR;
   }

   *file_out = file;
   return WASM_SUCCESS;
}


static int wasi_impl_file_close(wasm_file_t *file)
{
   int err;

   err = wasi_fd_close(file->fd);
   if (err) {
      return WASM_ERROR_IO_ERROR;
   }
   file->fd = -1;
   return WASM_SUCCESS;
}


static int wasi_impl_file_read(wasm_file_t *file, void *buf, int len, int *read_bytes_out)
{
   wasi_iovec_t iovec;
   int err;

   iovec.buf = (char *)buf;
   iovec.len = len;
   err = wasi_fd_read(file->fd, &iovec, 1, read_bytes_out);
   if (err) {
      return WASM_ERROR_IO_ERROR;
   }
   return WASM_SUCCESS;
}


static int wasi_impl_file_write(wasm_file_t *file, void *buf, int len, int *written_out)
{
   wasi_iovec_t iovec;
   int err;

   iovec.buf = (char *)buf;
   iovec.len = len;
   err = wasi_fd_write(file->fd, &iovec, 1, written_out);
   if (err) {
      return WASM_ERROR_IO_ERROR;
   }
   return WASM_SUCCESS;
}


static int wasi_impl_file_get_length(wasm_file_t *file, int64_t *length_out)
{
   wasi_filestat_t filestat;
   int err;

   err = wasi_fd_filestat_get(file->fd, &filestat);
   if (err) {
      return WASM_ERROR_IO_ERROR;
   }
   *length_out = filestat.size;
   return WASM_SUCCESS;
}


static int wasi_impl_file_set_length(wasm_file_t *file, int64_t new_length)
{
   int err;

   err = wasi_fd_filestat_set_size(file->fd, new_length);
   if (err) {
      return WASM_ERROR_IO_ERROR;
   }
   return WASM_SUCCESS;
}


static int wasi_impl_file_get_position(wasm_file_t *file, int64_t *position_out)
{
   int err;

   err = wasi_fd_tell(file->fd, (uint64_t *)position_out);
   if (err) {
      return WASM_ERROR_IO_ERROR;
   }
   return WASM_SUCCESS;
}


static int wasi_impl_file_set_position(wasm_file_t *file, int64_t new_position)
{
   uint64_t pos;
   int err;

   err = wasi_fd_seek(file->fd, new_position, WASI_WHENCE_SET, &pos);
   if (err) {
      return WASM_ERROR_IO_ERROR;
   }
   return WASM_SUCCESS;
}


static int wasi_impl_file_sync(wasm_file_t *file)
{
   int err;

   err = wasi_fd_datasync(file->fd);
   if (err) {
      return WASM_ERROR_IO_ERROR;
   }
   return WASM_SUCCESS;
}

#endif /* __wasi__ */

//#define TEST_ALWAYS_ASYNC

typedef struct {
   char *path;
   wasm_path_create_directory_cont func;
   void *data;
} DummyCreateDirectoryCont;

typedef struct {
   char *path;
   wasm_path_delete_file_cont func;
   void *data;
} DummyDeleteFileCont;

typedef struct {
   char *path;
   wasm_path_delete_directory_cont func;
   void *data;
} DummyDeleteDirectoryCont;

typedef struct {
   char *path;
   wasm_path_get_files_cont func;
   void *data;
} DummyGetFilesCont;

typedef struct {
   char *path;
   wasm_path_exists_cont func;
   void *data;
} DummyExistsCont;

typedef struct {
   char *path;
   wasm_path_get_info_cont func;
   void *data;
} DummyGetInfoCont;

typedef struct {
   char *path;
   wasm_path_resolve_symlink_cont func;
   void *data;
} DummyResolveSymlinkCont;

typedef struct {
   char *path;
   int flags;
   wasm_file_open_cont func;
   void *data;
} DummyOpenCont;

typedef struct {
   wasm_file_t *file;
   wasm_file_close_cont func;
   void *data;
} DummyCloseCont;

typedef struct {
   wasm_file_t *file;
   void *buf;
   int len;
   wasm_file_read_cont func;
   void *data;
} DummyReadCont;

typedef struct {
   wasm_file_t *file;
   void *buf;
   int len;
   wasm_file_write_cont func;
   void *data;
} DummyWriteCont;

typedef struct {
   wasm_file_t *file;
   wasm_file_get_length_cont func;
   void *data;
} DummyGetLengthCont;

typedef struct {
   wasm_file_t *file;
   int64_t new_length;
   wasm_file_set_length_cont func;
   void *data;
} DummySetLengthCont;

typedef struct {
   wasm_file_t *file;
   wasm_file_get_position_cont func;
   void *data;
} DummyGetPositionCont;

typedef struct {
   wasm_file_t *file;
   int64_t new_position;
   wasm_file_set_position_cont func;
   void *data;
} DummySetPositionCont;

typedef struct {
   wasm_file_t *file;
   wasm_file_sync_cont func;
   void *data;
} DummySyncCont;

typedef struct {
   wasm_file_lock_cont func;
   void *data;
} DummyLockCont;

typedef struct {
   wasm_file_unlock_cont func;
   void *data;
} DummyUnlockCont;


static void dummy_create_directory_cont(void *data)
{
   DummyCreateDirectoryCont *cont = data;
   wasm_path_create_directory_cont cont_func = cont->func;
   void *cont_data = cont->data;

#ifdef __wasi__
   int err = 0;

   if (!cont->path) {
      err = WASM_ERROR_OUT_OF_MEMORY;
   }
   if (!err) {
      err = wasi_impl_path_create_directory(cont->path);
      free(cont->path);
   }
   free(cont);
   cont_func(err, cont_data);
#else
   free(cont->path);
   free(cont);
   cont_func(WASM_ERROR_NOT_SUPPORTED, cont_data);
#endif
}


static void dummy_delete_file_cont(void *data)
{
   DummyDeleteFileCont *cont = data;
   wasm_path_delete_file_cont cont_func = cont->func;
   void *cont_data = cont->data;

#ifdef __wasi__
   int err = 0;

   if (!cont->path) {
      err = WASM_ERROR_OUT_OF_MEMORY;
   }
   if (!err) {
      err = wasi_impl_path_delete_file(cont->path);
      free(cont->path);
   }
   free(cont);
   cont_func(err, cont_data);
#else
   free(cont->path);
   free(cont);
   cont_func(WASM_ERROR_NOT_SUPPORTED, cont_data);
#endif
}


static void dummy_delete_directory_cont(void *data)
{
   DummyDeleteDirectoryCont *cont = data;
   wasm_path_delete_directory_cont cont_func = cont->func;
   void *cont_data = cont->data;

#ifdef __wasi__
   int err = 0;

   if (!cont->path) {
      err = WASM_ERROR_OUT_OF_MEMORY;
   }
   if (!err) {
      err = wasi_impl_path_delete_directory(cont->path);
      free(cont->path);
   }
   free(cont);
   cont_func(err, cont_data);
#else
   free(cont->path);
   free(cont);
   cont_func(WASM_ERROR_NOT_SUPPORTED, cont_data);
#endif
}


static void dummy_get_files_cont(void *data)
{
   DummyGetFilesCont *cont = data;
   wasm_path_get_files_cont cont_func = cont->func;
   void *cont_data = cont->data;

#ifdef __wasi__
   char **files = NULL;
   int err = 0, num_files = 0;

   if (!cont->path) {
      err = WASM_ERROR_OUT_OF_MEMORY;
   }
   if (!err) {
      err = wasi_impl_path_get_files(cont->path, &num_files, &files);
      free(cont->path);
   }
   free(cont);
   cont_func(num_files, files, err, cont_data);
#else
   free(cont->path);
   free(cont);
   cont_func(0, NULL, WASM_ERROR_NOT_SUPPORTED, cont_data);
#endif
}


static void dummy_exists_cont(void *data)
{
   DummyExistsCont *cont = data;
   wasm_path_exists_cont cont_func = cont->func;
   void *cont_data = cont->data;

#ifdef __wasi__
   int err = 0, exists = 0;

   if (!cont->path) {
      err = WASM_ERROR_OUT_OF_MEMORY;
   }
   if (!err) {
      err = wasi_impl_path_exists(cont->path, &exists);
      free(cont->path);
   }
   free(cont);
   cont_func(exists, err, cont_data);
#else
   free(cont->path);
   free(cont);
   cont_func(0, WASM_ERROR_NOT_SUPPORTED, cont_data);
#endif
}


static void dummy_get_info_cont(void *data)
{
   DummyGetInfoCont *cont = data;
   wasm_path_get_info_cont cont_func = cont->func;
   void *cont_data = cont->data;

#ifdef __wasi__
   int64_t length = 0, mtime = 0;
   int err = 0, type = 0;

   if (!cont->path) {
      err = WASM_ERROR_OUT_OF_MEMORY;
   }
   if (!err) {
      err = wasi_impl_path_get_info(cont->path, &type, &length, &mtime);
      free(cont->path);
   }
   free(cont);
   cont_func(type, length, mtime, err, cont_data);
#else
   free(cont->path);
   free(cont);
   cont_func(0, 0, 0, WASM_ERROR_NOT_SUPPORTED, cont_data);
#endif
}


static void dummy_resolve_symlink_cont(void *data)
{
   DummyResolveSymlinkCont *cont = data;
   wasm_path_resolve_symlink_cont cont_func = cont->func;
   void *cont_data = cont->data;

#ifdef __wasi__
   char *orig_path = NULL;
   int err = 0;

   if (!cont->path) {
      err = WASM_ERROR_OUT_OF_MEMORY;
   }
   if (!err) {
      err = wasi_impl_path_resolve_symlink(cont->path, &orig_path);
      free(cont->path);
   }
   free(cont);
   cont_func(orig_path, err, cont_data);
#else
   free(cont->path);
   free(cont);
   cont_func(NULL, WASM_ERROR_NOT_SUPPORTED, cont_data);
#endif
}


static void dummy_open_cont(void *data)
{
   DummyOpenCont *cont = data;
   wasm_file_open_cont cont_func = cont->func;
   void *cont_data = cont->data;

#ifdef __wasi__
   wasm_file_t *file = NULL;
   int err = 0;

   if (!cont->path) {
      err = WASM_ERROR_OUT_OF_MEMORY;
   }
   if (!err) {
      err = wasi_impl_file_open(cont->path, cont->flags, &file);
      free(cont->path);
   }
   free(cont);
   cont_func(file, err, cont_data);
#else
   free(cont->path);
   free(cont);
   cont_func(NULL, WASM_ERROR_NOT_SUPPORTED, cont_data);
#endif
}


static void dummy_close_cont(void *data)
{
   DummyCloseCont *cont = data;
   wasm_file_close_cont cont_func = cont->func;
   void *cont_data = cont->data;

#ifdef __wasi__
   int err;

   err = wasi_impl_file_close(cont->file);
   free(cont);
   cont_func(err, cont_data);
#else
   free(cont);
   cont_func(WASM_ERROR_NOT_SUPPORTED, cont_data);
#endif
}


static void dummy_read_cont(void *data)
{
   DummyReadCont *cont = data;
   wasm_file_read_cont cont_func = cont->func;
   void *cont_data = cont->data;

#ifdef __wasi__
   int err, read_bytes;

   err = wasi_impl_file_read(cont->file, cont->buf, cont->len, &read_bytes);
   free(cont);
   cont_func(read_bytes, err, cont_data);
#else
   free(cont);
   cont_func(0, WASM_ERROR_NOT_SUPPORTED, cont_data);
#endif
}


static void dummy_write_cont(void *data)
{
   DummyWriteCont *cont = data;
   wasm_file_write_cont cont_func = cont->func;
   void *cont_data = cont->data;

#ifdef __wasi__
   int err, written_bytes;

   err = wasi_impl_file_write(cont->file, cont->buf, cont->len, &written_bytes);
   free(cont);
   cont_func(written_bytes, err, cont_data);
#else
   free(cont);
   cont_func(0, WASM_ERROR_NOT_SUPPORTED, cont_data);
#endif
}


static void dummy_get_length_cont(void *data)
{
   DummyGetLengthCont *cont = data;
   wasm_file_get_length_cont cont_func = cont->func;
   void *cont_data = cont->data;

#ifdef __wasi__
   int64_t length;
   int err;

   err = wasi_impl_file_get_length(cont->file, &length);
   free(cont);
   cont_func(length, err, cont_data);
#else
   free(cont);
   cont_func(0, WASM_ERROR_NOT_SUPPORTED, cont_data);
#endif
}


static void dummy_set_length_cont(void *data)
{
   DummySetLengthCont *cont = data;
   wasm_file_set_length_cont cont_func = cont->func;
   void *cont_data = cont->data;

#ifdef __wasi__
   int err;

   err = wasi_impl_file_set_length(cont->file, cont->new_length);
   free(cont);
   cont_func(err, cont_data);
#else
   free(cont);
   cont_func(WASM_ERROR_NOT_SUPPORTED, cont_data);
#endif
}


static void dummy_get_position_cont(void *data)
{
   DummyGetPositionCont *cont = data;
   wasm_file_get_position_cont cont_func = cont->func;
   void *cont_data = cont->data;

#ifdef __wasi__
   int64_t position;
   int err;

   err = wasi_impl_file_get_position(cont->file, &position);
   free(cont);
   cont_func(position, err, cont_data);
#else
   free(cont);
   cont_func(0, WASM_ERROR_NOT_SUPPORTED, cont_data);
#endif
}


static void dummy_set_position_cont(void *data)
{
   DummySetPositionCont *cont = data;
   wasm_file_set_position_cont cont_func = cont->func;
   void *cont_data = cont->data;

#ifdef __wasi__
   int err;

   err = wasi_impl_file_set_position(cont->file, cont->new_position);
   free(cont);
   cont_func(err, cont_data);
#else
   free(cont);
   cont_func(WASM_ERROR_NOT_SUPPORTED, cont_data);
#endif
}


static void dummy_sync_cont(void *data)
{
   DummySyncCont *cont = data;
   wasm_file_sync_cont cont_func = cont->func;
   void *cont_data = cont->data;

#ifdef __wasi__
   int err;

   err = wasi_impl_file_sync(cont->file);
   free(cont);
   cont_func(err, cont_data);
#else
   free(cont);
   cont_func(WASM_ERROR_NOT_SUPPORTED, cont_data);
#endif
}


static void dummy_lock_cont(void *data)
{
   DummyLockCont *cont = data;
   wasm_file_lock_cont cont_func = cont->func;
   void *cont_data = cont->data;

   free(cont);
   cont_func(0, WASM_ERROR_NOT_SUPPORTED, cont_data);
}


static void dummy_unlock_cont(void *data)
{
   DummyUnlockCont *cont = data;
   wasm_file_unlock_cont cont_func = cont->func;
   void *cont_data = cont->data;

   free(cont);
   cont_func(WASM_ERROR_NOT_SUPPORTED, cont_data);
}


void wasm_path_create_directory(const char *path, wasm_path_create_directory_cont func, void *data)
{
   DummyCreateDirectoryCont *cont;

   cont = malloc(sizeof(DummyCreateDirectoryCont));
   cont->path = strdup(path);
   cont->func = func;
   cont->data = data;
   wasm_sleep(0, dummy_create_directory_cont, cont);
}


void wasm_path_delete_file(const char *path, wasm_path_delete_file_cont func, void *data)
{
   DummyDeleteFileCont *cont;

   cont = malloc(sizeof(DummyDeleteFileCont));
   cont->path = strdup(path);
   cont->func = func;
   cont->data = data;
   wasm_sleep(0, dummy_delete_file_cont, cont);
}


void wasm_path_delete_directory(const char *path, wasm_path_delete_directory_cont func, void *data)
{
   DummyDeleteDirectoryCont *cont;

   cont = malloc(sizeof(DummyDeleteDirectoryCont));
   cont->path = strdup(path);
   cont->func = func;
   cont->data = data;
   wasm_sleep(0, dummy_delete_directory_cont, cont);
}


void wasm_path_get_files(const char *path, wasm_path_get_files_cont func, void *data)
{
   DummyGetFilesCont *cont;

   cont = malloc(sizeof(DummyGetFilesCont));
   cont->path = strdup(path);
   cont->func = func;
   cont->data = data;
   wasm_sleep(0, dummy_get_files_cont, cont);
}


void wasm_path_exists(const char *path, wasm_path_exists_cont func, void *data)
{
   DummyExistsCont *cont;

   cont = malloc(sizeof(DummyExistsCont));
   cont->path = strdup(path);
   cont->func = func;
   cont->data = data;
   wasm_sleep(0, dummy_exists_cont, cont);
}


void wasm_path_get_info(const char *path, wasm_path_get_info_cont func, void *data)
{
   DummyGetInfoCont *cont;

   cont = malloc(sizeof(DummyGetInfoCont));
   cont->path = strdup(path);
   cont->func = func;
   cont->data = data;
   wasm_sleep(0, dummy_get_info_cont, cont);
}


void wasm_path_resolve_symlink(const char *path, wasm_path_resolve_symlink_cont func, void *data)
{
   DummyResolveSymlinkCont *cont;

   cont = malloc(sizeof(DummyResolveSymlinkCont));
   cont->path = strdup(path);
   cont->func = func;
   cont->data = data;
   wasm_sleep(0, dummy_resolve_symlink_cont, cont);
}


void wasm_file_open(const char *path, int flags, wasm_file_open_cont func, void *data)
{
   DummyOpenCont *cont;

   cont = malloc(sizeof(DummyOpenCont));
   cont->path = strdup(path);
   cont->flags = flags;
   cont->func = func;
   cont->data = data;
   wasm_sleep(0, dummy_open_cont, cont);
}


void wasm_file_close(wasm_file_t *file, wasm_file_close_cont func, void *data)
{
   DummyCloseCont *cont;

   cont = malloc(sizeof(DummyCloseCont));
   cont->file = file;
   cont->func = func;
   cont->data = data;
   wasm_sleep(0, dummy_close_cont, cont);
}


void wasm_file_read(wasm_file_t *file, void *buf, int len, wasm_file_read_cont func, void *data)
{
   DummyReadCont *cont;

   cont = malloc(sizeof(DummyReadCont));
   cont->file = file;
   cont->buf = buf;
   cont->len = len;
   cont->func = func;
   cont->data = data;
   wasm_sleep(0, dummy_read_cont, cont);
}


void wasm_file_write(wasm_file_t *file, void *buf, int len, wasm_file_write_cont func, void *data)
{
   DummyWriteCont *cont;

   cont = malloc(sizeof(DummyWriteCont));
   cont->file = file;
   cont->buf = buf;
   cont->len = len;
   cont->func = func;
   cont->data = data;
   wasm_sleep(0, dummy_write_cont, cont);
}


void wasm_file_get_length(wasm_file_t *file, wasm_file_get_length_cont func, void *data)
{
   DummyGetLengthCont *cont;

   cont = malloc(sizeof(DummyGetLengthCont));
   cont->file = file;
   cont->func = func;
   cont->data = data;
   wasm_sleep(0, dummy_get_length_cont, cont);
}


void wasm_file_set_length(wasm_file_t *file, int64_t new_length, wasm_file_set_length_cont func, void *data)
{
   DummySetLengthCont *cont;

   cont = malloc(sizeof(DummySetLengthCont));
   cont->file = file;
   cont->new_length = new_length;
   cont->func = func;
   cont->data = data;
   wasm_sleep(0, dummy_set_length_cont, cont);
}


void wasm_file_get_position(wasm_file_t *file, wasm_file_get_position_cont func, void *data)
{
   DummyGetPositionCont *cont;

   cont = malloc(sizeof(DummyGetPositionCont));
   cont->file = file;
   cont->func = func;
   cont->data = data;
   wasm_sleep(0, dummy_get_position_cont, cont);
}


void wasm_file_set_position(wasm_file_t *file, int64_t new_position, wasm_file_set_position_cont func, void *data)
{
   DummySetPositionCont *cont;

   cont = malloc(sizeof(DummySetPositionCont));
   cont->file = file;
   cont->new_position = new_position;
   cont->func = func;
   cont->data = data;
   wasm_sleep(0, dummy_set_position_cont, cont);
}


void wasm_file_sync(wasm_file_t *file, wasm_file_sync_cont func, void *data)
{
   DummySyncCont *cont;

   cont = malloc(sizeof(DummySyncCont));
   cont->file = file;
   cont->func = func;
   cont->data = data;
   wasm_sleep(0, dummy_sync_cont, cont);
}


void wasm_file_lock(wasm_file_t *file, int exclusive, int timeout, wasm_file_lock_cont func, void *data)
{
   DummyLockCont *cont;

   cont = malloc(sizeof(DummyLockCont));
   cont->func = func;
   cont->data = data;
   wasm_sleep(0, dummy_lock_cont, cont);
}


void wasm_file_unlock(wasm_file_t *file, wasm_file_unlock_cont func, void *data)
{
   DummyUnlockCont *cont;

   cont = malloc(sizeof(DummyUnlockCont));
   cont->func = func;
   cont->data = data;
   wasm_sleep(0, dummy_unlock_cont, cont);
}


int wasm_path_create_directory_sync(const char *path, int *error_out)
{
#ifdef TEST_ALWAYS_ASYNC
   return 0;
#endif
#ifdef __wasi__
   *error_out = wasi_impl_path_create_directory(path);
   return 1;
#else
   *error_out = WASM_ERROR_NOT_SUPPORTED;
   return 1;
#endif
}


int wasm_path_delete_file_sync(const char *path, int *error_out)
{
#ifdef TEST_ALWAYS_ASYNC
   return 0;
#endif
#ifdef __wasi__
   *error_out = wasi_impl_path_delete_file(path);
   return 1;
#else
   *error_out = WASM_ERROR_NOT_SUPPORTED;
   return 1;
#endif
}


int wasm_path_delete_directory_sync(const char *path, int *error_out)
{
#ifdef TEST_ALWAYS_ASYNC
   return 0;
#endif
#ifdef __wasi__
   *error_out = wasi_impl_path_delete_directory(path);
   return 1;
#else
   *error_out = WASM_ERROR_NOT_SUPPORTED;
   return 1;
#endif
}


int wasm_path_get_files_sync(const char *path, int *num_files_out, char ***files_out, int *error_out)
{
#ifdef TEST_ALWAYS_ASYNC
   return 0;
#endif
#ifdef __wasi__
   *error_out = wasi_impl_path_get_files(path, num_files_out, files_out);
   return 1;
#else
   *num_files_out = 0;
   *files_out = NULL;
   *error_out = WASM_ERROR_NOT_SUPPORTED;
   return 1;
#endif
}


int wasm_path_exists_sync(const char *path, int *exists_out, int *error_out)
{
#ifdef TEST_ALWAYS_ASYNC
   return 0;
#endif
#ifdef __wasi__
   *error_out = wasi_impl_path_exists(path, exists_out);
   return 1;
#else
   *exists_out = 0;
   *error_out = WASM_ERROR_NOT_SUPPORTED;
   return 1;
#endif
}


int wasm_path_get_info_sync(const char *path, int *type_out, int64_t *length_out, int64_t *mtime_out, int *error_out)
{
#ifdef TEST_ALWAYS_ASYNC
   return 0;
#endif
#ifdef __wasi__
   *error_out = wasi_impl_path_get_info(path, type_out, length_out, mtime_out);
   return 1;
#else
   *type_out = 0;
   *length_out = 0;
   *mtime_out = 0;
   *error_out = WASM_ERROR_NOT_SUPPORTED;
   return 1;
#endif
}


int wasm_path_resolve_symlink_sync(const char *path, char **orig_path_out, int *error_out)
{
#ifdef TEST_ALWAYS_ASYNC
   return 0;
#endif
#ifdef __wasi__
   *error_out = wasi_impl_path_resolve_symlink(path, orig_path_out);
   return 1;
#else
   *orig_path_out = NULL;
   *error_out = WASM_ERROR_NOT_SUPPORTED;
   return 1;
#endif
}


int wasm_file_open_sync(const char *path, int flags, wasm_file_t **file_out, int *error_out)
{
#ifdef TEST_ALWAYS_ASYNC
   return 0;
#endif
#ifdef __wasi__
   *error_out = wasi_impl_file_open(path, flags, file_out);
   return 1;
#else
   *file_out = NULL;
   *error_out = WASM_ERROR_NOT_SUPPORTED;
   return 1;
#endif
}


int wasm_file_close_sync(wasm_file_t *file, int *error_out)
{
#ifdef TEST_ALWAYS_ASYNC
   if (error_out) {
      return 0;
   }
#endif
#ifdef __wasi__
   int err = wasi_impl_file_close(file);
   if (error_out) {
      *error_out = err;
   }
   return 1;
#else
   *error_out = WASM_ERROR_NOT_SUPPORTED;
   return 1;
#endif
}


int wasm_file_read_sync(wasm_file_t *file, void *buf, int len, int *read_bytes_out, int *error_out)
{
#ifdef TEST_ALWAYS_ASYNC
   return 0;
#endif
#ifdef __wasi__
   *error_out = wasi_impl_file_read(file, buf, len, read_bytes_out);
   return 1;
#else
   *read_bytes_out = 0;
   *error_out = WASM_ERROR_NOT_SUPPORTED;
   return 1;
#endif
}


int wasm_file_write_sync(wasm_file_t *file, void *buf, int len, int *written_bytes_out, int *error_out)
{
#ifdef TEST_ALWAYS_ASYNC
   return 0;
#endif
#ifdef __wasi__
   *error_out = wasi_impl_file_write(file, buf, len, written_bytes_out);
   return 1;
#else
   *written_bytes_out = 0;
   *error_out = WASM_ERROR_NOT_SUPPORTED;
   return 1;
#endif
}


int wasm_file_get_length_sync(wasm_file_t *file, int64_t *length_out, int *error_out)
{
#ifdef TEST_ALWAYS_ASYNC
   return 0;
#endif
#ifdef __wasi__
   *error_out = wasi_impl_file_get_length(file, length_out);
   return 1;
#else
   *length_out = 0;
   *error_out = WASM_ERROR_NOT_SUPPORTED;
   return 1;
#endif
}


int wasm_file_set_length_sync(wasm_file_t *file, int64_t new_length, int *error_out)
{
#ifdef TEST_ALWAYS_ASYNC
   return 0;
#endif
#ifdef __wasi__
   *error_out = wasi_impl_file_set_length(file, new_length);
   return 1;
#else
   *error_out = WASM_ERROR_NOT_SUPPORTED;
   return 1;
#endif
}


int wasm_file_get_position_sync(wasm_file_t *file, int64_t *position_out, int *error_out)
{
#ifdef TEST_ALWAYS_ASYNC
   return 0;
#endif
#ifdef __wasi__
   *error_out = wasi_impl_file_get_position(file, position_out);
   return 1;
#else
   *position_out = 0;
   *error_out = WASM_ERROR_NOT_SUPPORTED;
   return 1;
#endif
}


int wasm_file_set_position_sync(wasm_file_t *file, int64_t new_position, int *error_out)
{
#ifdef TEST_ALWAYS_ASYNC
   return 0;
#endif
#ifdef __wasi__
   *error_out = wasi_impl_file_set_position(file, new_position);
   return 1;
#else
   *error_out = WASM_ERROR_NOT_SUPPORTED;
   return 1;
#endif
}


int wasm_file_sync_sync(wasm_file_t *file, int *error_out)
{
#ifdef TEST_ALWAYS_ASYNC
   return 0;
#endif
#ifdef __wasi__
   *error_out = wasi_impl_file_sync(file);
   return 1;
#else
   *error_out = WASM_ERROR_NOT_SUPPORTED;
   return 1;
#endif
}


int wasm_file_lock_sync(wasm_file_t *file, int exclusive, int timeout, int *locked_out, int *error_out)
{
#ifdef TEST_ALWAYS_ASYNC
   return 0;
#endif
   *locked_out = 0;
   *error_out = WASM_ERROR_NOT_SUPPORTED;
   return 1;
}


int wasm_file_unlock_sync(wasm_file_t *file, int *error_out)
{
#ifdef TEST_ALWAYS_ASYNC
   return 0;
#endif
   *error_out = WASM_ERROR_NOT_SUPPORTED;
   return 1;
}


////////////////////////////////////////////////////////////////////////////////
// Math library:
////////////////////////////////////////////////////////////////////////////////


static const uint64_t float_log10_2 = 0x3FD34413509F79FFULL;
#if 0
static const uint64_t float_powers_of_ten[99] = {
   0x34EEA6608E29B24DULL,0x352327FC58DA0F70ULL,0x3557F1FB6F10934CULL,0x358DEE7A4AD4B81FULL,0x35C2B50C6EC4F313ULL,0x35F7624F8A762FD8ULL,
   0x362D3AE36D13BBCEULL,0x366244CE242C5561ULL,0x3696D601AD376AB9ULL,0x36CC8B8218854567ULL,0x3701D7314F534B61ULL,0x37364CFDA3281E39ULL,
   0x376BE03D0BF225C7ULL,0x37A16C262777579CULL,0x37D5C72FB1552D83ULL,0x380B38FB9DAA78E4ULL,0x3841039D428A8B8FULL,0x38754484932D2E72ULL,
   0x38AA95A5B7F87A0FULL,0x38E09D8792FB4C49ULL,0x3914C4E977BA1F5CULL,0x3949F623D5A8A733ULL,0x398039D665896880ULL,0x39B4484BFEEBC2A0ULL,
   0x39E95A5EFEA6B347ULL,0x3A1FB0F6BE506019ULL,0x3A53CE9A36F23C10ULL,0x3A88C240C4AECB14ULL,0x3ABEF2D0F5DA7DD9ULL,0x3AF357C299A88EA7ULL,
   0x3B282DB34012B251ULL,0x3B5E392010175EE6ULL,0x3B92E3B40A0E9B4FULL,0x3BC79CA10C924223ULL,0x3BFD83C94FB6D2ACULL,0x3C32725DD1D243ACULL,
   0x3C670EF54646D497ULL,0x3C9CD2B297D889BCULL,0x3CD203AF9EE75616ULL,0x3D06849B86A12B9BULL,0x3D3C25C268497682ULL,0x3D719799812DEA11ULL,
   0x3DA5FD7FE1796495ULL,0x3DDB7CDFD9D7BDBBULL,0x3E112E0BE826D695ULL,0x3E45798EE2308C3AULL,0x3E7AD7F29ABCAF48ULL,0x3EB0C6F7A0B5ED8DULL,
   0x3EE4F8B588E368F1ULL,0x3F1A36E2EB1C432DULL,0x3F50624DD2F1A9FCULL,0x3F847AE147AE147BULL,0x3FB999999999999AULL,0x3FF0000000000000ULL,
   0x4024000000000000ULL,0x4059000000000000ULL,0x408F400000000000ULL,0x40C3880000000000ULL,0x40F86A0000000000ULL,0x412E848000000000ULL,
   0x416312D000000000ULL,0x4197D78400000000ULL,0x41CDCD6500000000ULL,0x4202A05F20000000ULL,0x42374876E8000000ULL,0x426D1A94A2000000ULL,
   0x42A2309CE5400000ULL,0x42D6BCC41E900000ULL,0x430C6BF526340000ULL,0x4341C37937E08000ULL,0x4376345785D8A000ULL,0x43ABC16D674EC800ULL,
   0x43E158E460913D00ULL,0x4415AF1D78B58C40ULL,0x444B1AE4D6E2EF50ULL,0x4480F0CF064DD592ULL,0x44B52D02C7E14AF7ULL,0x44EA784379D99DB4ULL,
   0x45208B2A2C280291ULL,0x4554ADF4B7320335ULL,0x4589D971E4FE8402ULL,0x45C027E72F1F1281ULL,0x45F431E0FAE6D721ULL,0x46293E5939A08CEAULL,
   0x465F8DEF8808B024ULL,0x4693B8B5B5056E17ULL,0x46C8A6E32246C99CULL,0x46FED09BEAD87C03ULL,0x4733426172C74D82ULL,0x476812F9CF7920E3ULL,
   0x479E17B84357691BULL,0x47D2CED32A16A1B1ULL,0x48078287F49C4A1DULL,0x483D6329F1C35CA5ULL,0x48725DFA371A19E7ULL,0x48A6F578C4E0A061ULL,
   0x48DCB2D6F618C879ULL,0x4911EFC659CF7D4CULL,0x49466BB7F0435C9EULL
};
#endif

static const uint64_t double_powers_of_ten[1330] = {
   0x7EECBAF379E01A5CULL,0x3B95755DC2FF447DULL,0xDEA7E9B0585820F3ULL,0x3B98D2B533BF159CULL,0x0B28F20E37371498ULL,0x3B9C23B140576D82ULL,
   0x8DF32E91C504D9BEULL,0x3B9F6C9D906D48E2ULL,0x316FFA363646102DULL,0x3BA2C7C4F4889B1BULL,0xFEE5FC61E1EBCA1CULL,0x3BA61CDB18D560F0ULL,
   0x3E9F7B7A5A66BCA3ULL,0x3BA96411DF0AB92DULL,0x8E475A58F1006BCCULL,0x3BACBD1656CD6778ULL,0x58EC987796A04360ULL,0x3BB0162DF64060ABULL,
   0x2F27BE957C485437ULL,0x3BB35BB973D078D6ULL,0xBAF1AE3ADB5A6945ULL,0x3BB6B2A7D0C4970BULL,0x54D70CE4C91881CBULL,0x3BBA0FA8E27ADE67ULL,
   0x2A0CD01DFB5EA23EULL,0x3BBD53931B199601ULL,0x749004257A364ACEULL,0x3BC0A877E1DFFB81ULL,0xE8DA02976C61EEC1ULL,0x3BC4094AED2BFD30ULL,
   0x2310833D477A6A71ULL,0x3BC74B9DA876FC7DULL,0x6BD4A40C9959050DULL,0x3BCA9E851294BB9CULL,0xC364E687DFD7A328ULL,0x3BCE03132B9CF541ULL,
   0x343E2029D7CD8BF2ULL,0x3BD143D7F6843292ULL,0xC14DA8344DC0EEEFULL,0x3BD494CDF4253F36ULL,0x71A1124161312AAAULL,0x3BD7FA01712E8F04ULL,
   0xC704AB68DCBEBAAAULL,0x3BDB3C40E6BD1962ULL,0x78C5D64313EE6955ULL,0x3BDE8B51206C5FBBULL,0x56F74BD3D8EA03AAULL,0x3BE1EE25688777AAULL,
   0x765A8F646792424AULL,0x3BE534D76154AACAULL,0x13F1333D8176D2DDULL,0x3BE8820D39A9D57DULL,0x58ED800CE1D48794ULL,0x3BEBE29088144ADCULL,
   0xB79470080D24D4BDULL,0x3BEF2D9A550CAEC9ULL,0x25798C0A106E09ECULL,0x3BF27900EA4FDA7CULL,0x2ED7EF0C94898C67ULL,0x3BF5D74124E3D11BULL,
   0xFD46F567DCD5F7C0ULL,0x3BF92688B70E62B0ULL,0x3C98B2C1D40B75B0ULL,0x3BFC702AE4D1FB5DULL,0x8BBEDF72490E531CULL,0x3BFFCC359E067A34ULL,
   0xD7574BA76DA8F3F2ULL,0x3C031FA182C40C60ULL,0x0D2D1E91491330EEULL,0x3C066789E3750F79ULL,0x507866359B57FD2AULL,0x3C09C16C5C525357ULL,
   0x924B3FE18116FE3AULL,0x3C0D18E3B9B37416ULL,0x36DE0FD9E15CBDC9ULL,0x3C105F1CA820511CULL,0x449593D059B3ED3BULL,0x3C13B6E3D2286563ULL,
   0x0ADD7C6238107445ULL,0x3C17124E63593F5EULL,0x8D94DB7AC6149156ULL,0x3C1A56E1FC2F8F35ULL,0xF0FA12597799B5ABULL,0x3C1DAC9A7B3B7302ULL,
   0xD69C4B77EAC0118BULL,0x3C210BE08D0527E1ULL,0x4C435E55E57015EEULL,0x3C244ED8B04671DAULL,0xDF5435EB5ECC1B69ULL,0x3C27A28EDC580E50ULL,
   0x8B94A1B31B3F9122ULL,0x3C2B059949B708F2ULL,0x2E79CA1FE20F756AULL,0x3C2E46FF9C24CB2FULL,0xFA183CA7DA9352C5ULL,0x3C3198BF832DFDFAULL,
   0xB89E4BD1D1382776ULL,0x3C34FEEF63F97D79ULL,0x1362EF6322C318AAULL,0x3C383F559E7BEE6CULL,0x183BAB3BEB73DED4ULL,0x3C3B8F2B061AEA07ULL,
   0xDE4A960AE650D689ULL,0x3C3EF2F5C7A1A488ULL,0x8AEE9DC6CFF28616ULL,0x3C4237D99CC506D5ULL,0xEDAA453883EF279BULL,0x3C4585D003F6488AULL,
   0xA914D686A4EAF182ULL,0x3C48E74404F3DAADULL,0x89AD06142712D6F1ULL,0x3C4C308A831868ACULL,0xAC18479930D78CAEULL,0x3C4F7CAD23DE82D7ULL,
   0x971E597F7D0D6FD9ULL,0x3C52DBD86CD6238DULL,0x7E72F7EFAE2865E8ULL,0x3C5629674405D638ULL,0x9E0FB5EB99B27F62ULL,0x3C5973C115074BC6ULL,
   0x4593A366801F1F3AULL,0x3C5CD0B15A491EB8ULL,0x2B7C462010137384ULL,0x3C60226ED86DB333ULL,0xF65B57A814185065ULL,0x3C636B0A8E891FFFULL,
   0xF3F22D92191E647FULL,0x3C66C5CD322B67FFULL,0xF8775C7B4FB2FECFULL,0x3C6A1BA03F5B20FFULL,0xF695339A239FBE83ULL,0x3C6D62884F31E93FULL,
   0xF43A8080AC87AE24ULL,0x3C70BB2A62FE638FULL,0xF8A490506BD4CCD6ULL,0x3C7414FA7DDEFE39ULL,0x76CDB46486CA000CULL,0x3C775A391D56BDC8ULL,
   0x9481217DA87C800FULL,0x3C7AB0C764AC6D3AULL,0x9CD0B4EE894DD009ULL,0x3C7E0E7C9EEBC444ULL,0xC404E22A2BA1440CULL,0x3C81521BC6A6B555ULL,
   0x35061AB4B689950EULL,0x3C84A6A2B85062ABULL,0x0123D0B0F215FD29ULL,0x3C880825B3323DABULL,0xC16CC4DD2E9B7C73ULL,0x3C8B4A2F1FFECD15ULL,
   0x31C7F6147A425B90ULL,0x3C8E9CBAE7FE805BULL,0xFF1CF9CCCC69793AULL,0x3C9201F4D0FF1038ULL,0x3EE4383FFF83D789ULL,0x3C954272053ED447ULL,
   0x0E9D464FFF64CD6BULL,0x3C98930E868E8959ULL,0x524497E3FF3E00C5ULL,0x3C9BF7D228322BAFULL,0x936ADEEE7F86C07BULL,0x3C9F3AE3591F5B4DULL,
   0xF84596AA1F68709AULL,0x3CA2899C2F673220ULL,0x3656FC54A7428CC1ULL,0x3CA5EC033B40FEA9ULL,0xC1F65DB4E88997F9ULL,0x3CA9338205089F29ULL,
   0x3273F52222ABFDF7ULL,0x3CAC8062864AC6F4ULL,0x3F10F26AAB56FD74ULL,0x3CAFE07B27DD78B1ULL,0xC76A9782AB165E69ULL,0x3CB32C4CF8EA6B6EULL,
   0x79453D6355DBF603ULL,0x3CB677603725064AULL,0x17968CBC2B52F384ULL,0x3CB9D53844EE47DDULL,0x2EBE17F59B13D832ULL,0x3CBD25432B14ECEAULL,
   0xBA6D9DF301D8CE3FULL,0x3CC06E93F5DA2824ULL,0xE909056FC24F01CFULL,0x3CC3CA38F350B22DULL,0xB1A5A365D9716121ULL,0x3CC71E6398126F5CULL,
   0xDE0F0C3F4FCDB969ULL,0x3CCA65FC7E170B33ULL,0xD592CF4F23C127C4ULL,0x3CCDBF7B9D9CCE00ULL,0x857BC1917658B8DAULL,0x3CD117AD428200C0ULL,
   0xA6DAB1F5D3EEE711ULL,0x3CD45D98932280F0ULL,0xD0915E7348EAA0D5ULL,0x3CD7B4FEB7EB212CULL,0x025ADB080D92A485ULL,0x3CDB111F32F2F4BCULL,
   0x02F191CA10F74DA6ULL,0x3CDE5566FFAFB1EBULL,0xC3ADF63C95352110ULL,0x3CE1AAC0BF9B9E65ULL,0x9A4CB9E5DD4134AAULL,0x3CE50AB877C142FFULL,
   0x80DFE85F549181D5ULL,0x3CE84D6695B193BFULL,0x6117E27729B5E24AULL,0x3CEBA0C03B1DF8AFULL,0x9CAEED8A7A11AD6EULL,0x3CEF047824F2BB6DULL,
   0x03DAA8ED189618CAULL,0x3CF245962E2F6A49ULL,0x44D153285EBB9EFCULL,0x3CF596FBB9BB44DBULL,0x1605A7F2766A86BBULL,0x3CF8FCBAA82A1612ULL,
   0x4DC388F78A029435ULL,0x3CFC3DF4A91A4DCBULL,0x21346B356C833942ULL,0x3CFF8D71D360E13EULL,0xA9818602C7A40793ULL,0x3D02F0CE4839198DULL,
   0x89F0F3C1BCC684BCULL,0x3D063680ED23AFF8ULL,0xAC6D30B22BF825EBULL,0x3D098421286C9BF6ULL,0x57887CDEB6F62F65ULL,0x3D0CE5297287C2F4ULL,
   0xB6B54E0B3259DD9FULL,0x3D102F39E794D9D8ULL,0xE462A18DFEF05507ULL,0x3D137B08617A104EULL,0x9D7B49F17EAC6A49ULL,0x3D16D9CA79D89462ULL,
   0xA26D0E36EF2BC26DULL,0x3D1A281E8C275CBDULL,0x0B0851C4AAF6B309ULL,0x3D1D72262F3133EDULL,0x4DCA6635D5B45FCBULL,0x3D20CEAFBAFD80E8ULL,
   0x309E7FE1A590BBDFULL,0x3D24212DD4DE7091ULL,0x7CC61FDA0EF4EAD7ULL,0x3D2769794A160CB5ULL,0xDBF7A7D092B2258CULL,0x3D2AC3D79C9B8FE2ULL,
   0xC97AC8E25BAF5778ULL,0x3D2E1A66C1E139EDULL,0x3BD97B1AF29B2D56ULL,0x3D31610072598869ULL,0x8ACFD9E1AF41F8ABULL,0x3D34B9408EEFEA83ULL,
   0x36C1E82D0D893B6BULL,0x3D3813C85955F292ULL,0xC472623850EB8A46ULL,0x3D3B58BA6FAB6F36ULL,0x758EFAC665266CD7ULL,0x3D3EAEE90B964B04ULL,
   0xC9795CBBFF380406ULL,0x3D420D51A73DEEE2ULL,0x7BD7B3EAFF060508ULL,0x3D4550A6110D6A9BULL,0x5ACDA0E5BEC7864AULL,0x3D48A4CF9550C542ULL,
   0x78C0848F973CB3EEULL,0x3D4C0701BD527B49ULL,0xD6F0A5B37D0BE0EAULL,0x3D4F48C22CA71A1BULL,0xCCACCF205C4ED924ULL,0x3D529AF2B7D0E0A2ULL,
   0xBFEC017439B147B7ULL,0x3D5600D7B2E28C65ULL,0x2FE701D1481D99A4ULL,0x3D59410D9F9B2F7FULL,0xFBE0C2459A25000DULL,0x3D5C91510781FB5EULL,
   0xBAD8F2D700AE4011ULL,0x3D5FF5A549627A36ULL,0x34C797C6606CE80AULL,0x3D6339874DDD8C62ULL,0xC1F97DB7F888220DULL,0x3D6687E92154EF7AULL,
   0x7277DD25F6AA2A90ULL,0x3D69E9E369AA2B59ULL,0xE78AEA37BA2A5A9AULL,0x3D6D322E220A5B17ULL,0xE16DA4C5A8B4F141ULL,0x3D707EB9AA8CF1DDULL,
   0x59C90DF712E22D91ULL,0x3D73DE6815302E55ULL,0x581DA8BA6BCD5C7BULL,0x3D772B010D3E1CF5ULL,0xAE2512E906C0B399ULL,0x3D7A75C1508DA432ULL,
   0x59AE57A34870E080ULL,0x3D7DD331A4B10D3FULL,0x980CF6C60D468C50ULL,0x3D8123FF06EEA847ULL,0x7E10347790982F64ULL,0x3D846CFEC8AA5259ULL,
   0xDD94419574BE3B3DULL,0x3D87C83E7AD4E6EFULL,0xEA7CA8FD68F6E506ULL,0x3D8B1D270CC51055ULL,0x651BD33CC3349E47ULL,0x3D8E6470CFF6546BULL,
   0x3E62C80BF401C5D9ULL,0x3D91BD8D03F3E986ULL,0xE6FDBD0778811BA8ULL,0x3D951678227871F3ULL,0xE0BD2C4956A16292ULL,0x3D985C162B168E70ULL,
   0x18EC775BAC49BB36ULL,0x3D9BB31BB5DC320DULL,0x2F93CA994BAE1502ULL,0x3D9F0FF151A99F48ULL,0x3B78BD3F9E999A42ULL,0x3DA253EDA614071AULL,
   0xCA56EC8F864000D3ULL,0x3DA5A8E90F9908E0ULL,0x7E7653D9B3E80084ULL,0x3DA90991A9BFA58CULL,0x9E13E8D020E200A5ULL,0x3DAC4BF6142F8EEFULL,
   0x8598E304291A80CEULL,0x3DAF9EF3993B72ABULL,0x337F8DE299B09081ULL,0x3DB303583FC527ABULL,0x005F715B401CB4A1ULL,0x3DB6442E4FB67196ULL,
   0x80774DB21023E1C9ULL,0x3DB99539E3A40DFBULL,0x6095211E942CDA3BULL,0x3DBCFA885C8D117AULL,0x7C5D34B31C9C0865ULL,0x3DC03C9539D82AECULL,
   0x9B7481DFE3C30A7EULL,0x3DC38BBA884E35A7ULL,0x8251A257DCB3CD1EULL,0x3DC6EEA92A61C311ULL,0xF1730576E9F06033ULL,0x3DCA3529BA7D19EAULL,
   0xADCFC6D4A46C783FULL,0x3DCD8274291C6065ULL,0x1943B889CD87964FULL,0x3DD0E3113363787FULL,0x6FCA53562074BDF2ULL,0x3DD42DEAC01E2B4FULL,
   0x4BBCE82BA891ED6EULL,0x3DD779657025B623ULL,0x1EAC223692B668C9ULL,0x3DDAD7BECC2F23ACULL,0x932B95621BB2017EULL,0x3DDE26D73F9D764BULL,
   0x77F67ABAA29E81DDULL,0x3DE1708D0F84D3DEULL,0x15F419694B462255ULL,0x3DE4CCB0536608D6ULL,0xCDB88FE1CF0BD575ULL,0x3DE81FEE341FC585ULL,
   0x4126B3DA42CECAD2ULL,0x3DEB67E9C127B6E7ULL,0x117060D0D3827D87ULL,0x3DEEC1E43171A4A1ULL,0xAAE63C8284318E74ULL,0x3DF2192E9EE706E4ULL,
   0xD59FCBA3253DF211ULL,0x3DF55F7A46A0C89DULL,0x4B07BE8BEE8D6E95ULL,0x3DF8B758D848FAC5ULL,0x4EE4D7177518651DULL,0x3DFC1297872D9CBBULL,
   0x229E0CDD525E7E65ULL,0x3DFF573D68F903EAULL,0xAB459014A6F61DFEULL,0x3E02AD0CC33744E4ULL,0xEB0B7A0CE859D2BFULL,0x3E060C27FA028B0EULL,
   0xA5CE58902270476EULL,0x3E094F31F8832DD2ULL,0x4F41EEB42B0C594AULL,0x3E0CA2FE76A3F947ULL,0x918935309AE7B7CEULL,0x3E1005DF0A267BCCULL,
   0xB5EB827CC1A1A5C2ULL,0x3E134756CCB01ABFULL,0xA366631BF20A0F32ULL,0x3E16992C7FDC216FULL,0x8C3FFBE2EE8C92FFULL,0x3E19FF779FD329CBULL,
   0x37A7FD6DD517DBDFULL,0x3E1D3FAAC3E3FA1FULL,0x0591FCC94A5DD2D7ULL,0x3E208F9574DCF8A7ULL,0xC6F67BFB9CF5478DULL,0x3E23F37AD21436D0ULL,
   0x7C5A0D7D42194CB8ULL,0x3E27382CC34CA242ULL,0x1B7090DC929F9FE6ULL,0x3E2A8637F41FCAD3ULL,0xE24CB513B74787E0ULL,0x3E2DE7C5F127BD87ULL,
   0xED6FF12C528CB4ECULL,0x3E3130DBB6B8D674ULL,0x28CBED77672FE227ULL,0x3E347D12A4670C12ULL,0xB2FEE8D540FBDAB0ULL,0x3E37DC574D80CF16ULL,
   0x2FDF5185489D68AEULL,0x3E3B29B69070816EULL,0xBBD725E69AC4C2DAULL,0x3E3E7424348CA1C9ULL,0x2ACCEF604175F390ULL,0x3E41D12D41AFCA3CULL,
   0x9AC0159C28E9B83AULL,0x3E4522BC490DDE65ULL,0x01701B0333242649ULL,0x3E486B6B5B5155FFULL,0xC1CC21C3FFED2FDBULL,0x3E4BC6463225AB7EULL,
   0x391F951A7FF43DE9ULL,0x3E4F1BEBDF578B2FULL,0x07677A611FF14D63ULL,0x3E5262E6D72D6DFBULL,0xC94158F967EDA0BCULL,0x3E55BBA08CF8C979ULL,
   0x1DC8D79BE0F48475ULL,0x3E591544581B7DECULL,0x253B0D82D931A593ULL,0x3E5C5A956E225D67ULL,0xEE89D0E38F7E0EF7ULL,0x3E5FB13AC9AAF4C0ULL,
   0x9516228E39AEC95BULL,0x3E630EC4BE0AD8F8ULL,0xBA5BAB31C81A7BB1ULL,0x3E665275ED8D8F36ULL,0x68F295FE3A211A9EULL,0x3E69A71368F0F304ULL,
   0xC1979DBEE454B0A2ULL,0x3E6D086C219697E2ULL,0x71FD852E9D69DCCBULL,0x3E704A8729FC3DDBULL,0x4E7CE67A44C453FEULL,0x3E739D28F47B4D52ULL,
   0x710E100C6AFAB47FULL,0x3E77023998CD1053ULL,0x4D51940F85B9619EULL,0x3E7A42C7FF005468ULL,0x60A5F9136727BA06ULL,0x3E7D9379FEC06982ULL,
   0xF8CF775840F1A887ULL,0x3E80F8587E7083E2ULL,0xDB81AA9728970955ULL,0x3E843B374F06526DULL,0x5262153CF2BCCBAAULL,0x3E878A0522C7E709ULL,
   0xA6FA9A8C2F6BFE94ULL,0x3E8AEC866B79E0CBULL,0x485CA0979DA37F1DULL,0x3E8E33D4032C2C7FULL,0x1A73C8BD850C5EE4ULL,0x3E9180C903F7379FULL,
   0xE110BAECE64F769DULL,0x3E94E0FB44F50586ULL,0x4CAA74D40FF1AA22ULL,0x3E982C9D0B192374ULL,0x5FD5120913EE14AAULL,0x3E9B77C44DDF6C51ULL,
   0xB7CA568B58E999D5ULL,0x3E9ED5B561574765ULL,0x92DE761717920025ULL,0x3EA225915CD68C9FULL,0x7796139CDD76802EULL,0x3EA56EF5B40C2FC7ULL,
   0x557B988414D4203AULL,0x3EA8CAB3210F3BB9ULL,0xD56D3F528D049424ULL,0x3EAC1EAFF4A98553ULL,0xCAC88F273045B92DULL,0x3EAF665BF1D3E6A8ULL,
   0xFD7AB2F0FC572779ULL,0x3EB2BFF2EE48E052ULL,0xDE6CAFD69DB678ABULL,0x3EB617F7D4ED8C33ULL,0xD607DBCC452416D6ULL,0x3EB95DF5CA28EF40ULL,
   0x0B89D2BF566D1C8CULL,0x3EBCB5733CB32B11ULL,0xA73623B7960431D7ULL,0x3EC0116805EFFAEAULL,0x5103ACA57B853E4DULL,0x3EC355C2076BF9A5ULL,
   0xA54497CEDA668DE1ULL,0x3EC6AB328946F80EULL,0x274ADEE1488018ACULL,0x3ECA0AFF95CC5B09ULL,0x711D96999AA01ED7ULL,0x3ECD4DBF7B3F71CBULL,
   0x4D64FC400148268DULL,0x3ED0A12F5A0F4E3EULL,0xF05F1DA800CD1818ULL,0x3ED404BD984990E6ULL,0xAC76E51201005E1EULL,0x3ED745ECFE5BF520ULL,
   0xD7949E56814075A6ULL,0x3EDA97683DF2F268ULL,0x0D79C5EC2190930FULL,0x3EDDFD424D6FAF03ULL,0xE86C1BB394FA5BEAULL,0x3EE13E497065CD61ULL,
   0x628722A07A38F2E4ULL,0x3EE48DDBCC7F40BAULL,0xFB28EB4898C72F9DULL,0x3EE7F152BF9F10E8ULL,0x9CF9930D5F7C7DC2ULL,0x3EEB36D3B7C36A91ULL,
   0x0437F7D0B75B9D33ULL,0x3EEE8488A5B44536ULL,0x8545F5C4E532847FULL,0x3EF1E5AACF215683ULL,0x334BB99B0F3F92D0ULL,0x3EF52F8AC174D612ULL,
   0xC01EA801D30F7784ULL,0x3EF87B6D71D20B96ULL,0x7026520247D35564ULL,0x3EFBDA48CE468E7CULL,0xC617F3416CE4155FULL,0x3EFF286D80EC190DULL,
   0x379DF011C81D1AB6ULL,0x3F027288E1271F51ULL,0x85856C163A246164ULL,0x3F05CF2B1970E725ULL,0x7373638DE456BCDFULL,0x3F09217AEFE69077ULL,
   0x50503C715D6C6C16ULL,0x3F0C69D9ABE03495ULL,0xA4644B8DB4C7871CULL,0x3F0FC45016D841BAULL,0xA6BEAF3890FCB471ULL,0x3F131AB20E472914ULL,
   0xD06E5B06B53BE18EULL,0x3F16615E91D8F359ULL,0x4489F1C8628AD9F1ULL,0x3F19B9B6364F3030ULL,0x2AD6371D3D96C837ULL,0x3F1D1411E1F17E1EULL,
   0xB58BC4E48CFC7A44ULL,0x3F2059165A6DDDA5ULL,0x22EEB61DB03B98D5ULL,0x3F23AF5BF109550FULL,0x75D531D28E253F85ULL,0x3F270D9976A5D529ULL,
   0xD34A7E4731AE8F67ULL,0x3F2A50FFD44F4A73ULL,0xC81D1DD8FE1A3340ULL,0x3F2DA53FC9631D10ULL,0x7D1232A79ED06008ULL,0x3F310747DDDDF22AULL,
   0x1C56BF518684780AULL,0x3F344919D5556EB5ULL,0x636C6F25E825960DULL,0x3F379B604AAACA62ULL,0x7E23C577B1177DC8ULL,0x3F3B011C2EAABE7DULL,
   0xDDACB6D59D5D5D3AULL,0x3F3E41633A556E1CULL,0x1517E48B04B4B489ULL,0x3F4191BC08EAC9A4ULL,0x1A5DDDADC5E1E1ABULL,0x3F44F62B0B257C0DULL,
   0x307AAA8C9BAD2D0BULL,0x3F4839DAE6F76D88ULL,0x3C99552FC298784DULL,0x3F4B8851A0B548EAULL,0xCBBFAA7BB33E9661ULL,0x3F4EEA6608E29B24ULL,
   0xFF57CA8D50071DFDULL,0x3F52327FC58DA0F6ULL,0xBF2DBD30A408E57CULL,0x3F557F1FB6F10934ULL,0xEEF92C7CCD0B1EDBULL,0x3F58DEE7A4AD4B81ULL,
   0x355BBBCE0026F349ULL,0x3F5C2B50C6EC4F31ULL,0x82B2AAC18030B01BULL,0x3F5F7624F8A762FDULL,0xE35F5571E03CDC21ULL,0x3F62D3AE36D13BBCULL,
   0x0E1B95672C260995ULL,0x3F66244CE242C556ULL,0x91A27AC0F72F8BFAULL,0x3F696D601AD376ABULL,0x760B197134FB6EF9ULL,0x3F6CC8B821885456ULL,
   0x09C6EFE6C11D255BULL,0x3F701D7314F534B6ULL,0x8C38ABE071646EB2ULL,0x3F7364CFDA3281E3ULL,0x6F46D6D88DBD8A5FULL,0x3F76BE03D0BF225CULL,
   0xC58C46475896767BULL,0x3F7A16C262777579ULL,0x36EF57D92EBC141AULL,0x3F7D5C72FB1552D8ULL,0x44AB2DCF7A6B1921ULL,0x3F80B38FB9DAA78EULL,
   0xEAEAFCA1AC82EFB4ULL,0x3F841039D428A8B8ULL,0x25A5BBCA17A3ABA1ULL,0x3F8754484932D2E7ULL,0xEF0F2ABC9D8C968AULL,0x3F8AA95A5B7F87A0ULL,
   0x95697AB5E277DE16ULL,0x3F8E09D8792FB4C4ULL,0xBAC3D9635B15D59CULL,0x3F914C4E977BA1F5ULL,0x2974CFBC31DB4B03ULL,0x3F949F623D5A8A73ULL,
   0xF9E901D59F290EE2ULL,0x3F98039D66589687ULL,0xF863424B06F3529AULL,0x3F9B4484BFEEBC29ULL,0x767C12DDC8B02741ULL,0x3F9E95A5EFEA6B34ULL,
   0x941B17953ADC3111ULL,0x3FA1FB0F6BE50601ULL,0xFC90EEBD44C99EAAULL,0x3FA53CE9A36F23C0ULL,0x3BB52A6C95FC0655ULL,0x3FA88C240C4AECB1ULL,
   0x8AA27507BB7B07EAULL,0x3FABEF2D0F5DA7DDULL,0x76A58924D52CE4F2ULL,0x3FAF357C299A88EAULL,0x144EEB6E0A781E2FULL,0x3FB282DB34012B25ULL,
   0x5962A6498D1625BBULL,0x3FB5E392010175EEULL,0xF7DDA7EDF82DD795ULL,0x3FB92E3B40A0E9B4ULL,0x35D511E976394D7AULL,0x3FBC79CA10C92422ULL,
   0xC34A5663D3C7A0D8ULL,0x3FBFD83C94FB6D2AULL,0xBA0E75FE645CC487ULL,0x3FC32725DD1D243AULL,0x6892137DFD73F5A9ULL,0x3FC670EF54646D49ULL,
   0xC2B6985D7CD0F313ULL,0x3FC9CD2B297D889BULL,0x59B21F3A6E0297ECULL,0x3FCD203AF9EE7561ULL,0xB01EA70909833DE7ULL,0x3FD06849B86A12B9ULL,
   0x1C2650CB4BE40D61ULL,0x3FD3C25C26849768ULL,0x1197F27F0F6E885DULL,0x3FD719799812DEA1ULL,0x55FDEF1ED34A2A74ULL,0x3FDA5FD7FE179649ULL,
   0xAB7D6AE6881CB511ULL,0x3FDDB7CDFD9D7BDBULL,0x4B2E62D01511F12AULL,0x3FE112E0BE826D69ULL,0x9DF9FB841A566D75ULL,0x3FE45798EE2308C3ULL,
   0x85787A6520EC08D2ULL,0x3FE7AD7F29ABCAF4ULL,0xD36B4C7F34938583ULL,0x3FEB0C6F7A0B5ED8ULL,0x08461F9F01B866E4ULL,0x3FEE4F8B588E368FULL,
   0xCA57A786C226809DULL,0x3FF1A36E2EB1C432ULL,0xBE76C8B439581062ULL,0x3FF50624DD2F1A9FULL,0xAE147AE147AE147BULL,0x3FF847AE147AE147ULL,
   0x999999999999999AULL,0x3FFB999999999999ULL,0x0000000000000000ULL,0x3FFF000000000000ULL,0x0000000000000000ULL,0x4002400000000000ULL,
   0x0000000000000000ULL,0x4005900000000000ULL,0x0000000000000000ULL,0x4008F40000000000ULL,0x0000000000000000ULL,0x400C388000000000ULL,
   0x0000000000000000ULL,0x400F86A000000000ULL,0x0000000000000000ULL,0x4012E84800000000ULL,0x0000000000000000ULL,0x4016312D00000000ULL,
   0x0000000000000000ULL,0x40197D7840000000ULL,0x0000000000000000ULL,0x401CDCD650000000ULL,0x0000000000000000ULL,0x40202A05F2000000ULL,
   0x0000000000000000ULL,0x402374876E800000ULL,0x0000000000000000ULL,0x4026D1A94A200000ULL,0x0000000000000000ULL,0x402A2309CE540000ULL,
   0x0000000000000000ULL,0x402D6BCC41E90000ULL,0x0000000000000000ULL,0x4030C6BF52634000ULL,0x0000000000000000ULL,0x40341C37937E0800ULL,
   0x0000000000000000ULL,0x40376345785D8A00ULL,0x0000000000000000ULL,0x403ABC16D674EC80ULL,0x0000000000000000ULL,0x403E158E460913D0ULL,
   0x0000000000000000ULL,0x40415AF1D78B58C4ULL,0x0000000000000000ULL,0x4044B1AE4D6E2EF5ULL,0x2000000000000000ULL,0x40480F0CF064DD59ULL,
   0x6800000000000000ULL,0x404B52D02C7E14AFULL,0x4200000000000000ULL,0x404EA784379D99DBULL,0x0940000000000000ULL,0x405208B2A2C28029ULL,
   0x4B90000000000000ULL,0x40554ADF4B732033ULL,0x1E74000000000000ULL,0x40589D971E4FE840ULL,0x1308800000000000ULL,0x405C027E72F1F128ULL,
   0x17CAA00000000000ULL,0x405F431E0FAE6D72ULL,0x9DBD480000000000ULL,0x406293E5939A08CEULL,0x452C9A0000000000ULL,0x4065F8DEF8808B02ULL,
   0x6B3BE04000000000ULL,0x40693B8B5B5056E1ULL,0xC60AD85000000000ULL,0x406C8A6E32246C99ULL,0x378D8E6400000000ULL,0x406FED09BEAD87C0ULL,
   0x22B878FE80000000ULL,0x40733426172C74D8ULL,0x2B66973E20000000ULL,0x4076812F9CF7920EULL,0xB6403D0DA8000000ULL,0x4079E17B84357691ULL,
   0x11E8262889000000ULL,0x407D2CED32A16A1BULL,0xD6622FB2AB400000ULL,0x408078287F49C4A1ULL,0x4BFABB9F56100000ULL,0x4083D6329F1C35CAULL,
   0x6F7CB54395CA0000ULL,0x408725DFA371A19EULL,0x0B5BE2947B3C8000ULL,0x408A6F578C4E0A06ULL,0x8E32DB399A0BA000ULL,0x408DCB2D6F618C87ULL,
   0xB8DFC90400474400ULL,0x40911EFC659CF7D4ULL,0xE717BB4500591500ULL,0x409466BB7F0435C9ULL,0x60DDAA16406F5A40ULL,0x4097C06A5EC5433CULL,
   0xBC8A8A4DE8459868ULL,0x409B18427B3B4A05ULL,0x2BAD2CE16256FE82ULL,0x409E5E531A0A1C87ULL,0xF6987819BAECBE23ULL,0x40A1B5E7E08CA3A8ULL,
   0x9A1F4B1014D3F6D6ULL,0x40A511B0EC57E649ULL,0x00A71DD41A08F48BULL,0x40A8561D276DDFDCULL,0x00D0E549208B31AEULL,0x40ABABA4714957D3ULL,
   0xE0828F4DB456FF0DULL,0x40AF0B46C6CDD6E3ULL,0xD8A33321216CBED0ULL,0x40B24E1878814C9CULL,0x0ECBFFE969C7EE84ULL,0x40B5A19E96A19FC4ULL,
   0x893F7FF1E21CF512ULL,0x40B905031E2503DAULL,0x2B8F5FEE5AA43257ULL,0x40BC4643E5AE44D1ULL,0x767337E9F14D3EEDULL,0x40BF97D4DF19D605ULL,
   0xD41005E46DA08EA8ULL,0x40C2FDCA16E04B86ULL,0x448A03AEC4845929ULL,0x40C63E9E4E4C2F34ULL,0x55AC849A75A56F73ULL,0x40C98E45E1DF3B01ULL,
   0xAB17A5C1130ECB50ULL,0x40CCF1D75A5709C1ULL,0x0AEEC798ABE93F12ULL,0x40D0372698766619ULL,0x4DAA797ED6E38ED6ULL,0x40D384F03E93FF9FULL,
   0x211517DE8C9C728CULL,0x40D6E62C4E38FF87ULL,0x74AD2EEB17E1C797ULL,0x40DA2FDBB0E39FB4ULL,0x91D87AA5DDDA397DULL,0x40DD7BD29D1C87A1ULL,
   0xF64E994F5550C7DDULL,0x40E0DAC74463A989ULL,0x39F11FD195527CEAULL,0x40E428BC8ABE49F6ULL,0xC86D67C5FAA71C24ULL,0x40E772EBAD6DDC73ULL,
   0xBA88C1B77950E32DULL,0x40EACFA698C95390ULL,0x74957912ABD28DFCULL,0x40EE21C81F7DD43AULL,0x11BAD75756C7317BULL,0x40F16A3A275D4949ULL,
   0x56298D2D2C78FDDAULL,0x40F4C4C8B1349B9BULL,0x15D9F83C3BCB9EA8ULL,0x40F81AFD6EC0E141ULL,0x5B50764B4ABE8653ULL,0x40FB61BCCA711991ULL,
   0xB22493DE1D6E27E7ULL,0x40FEBA2BFD0D5FF5ULL,0x8F56DC6AD264D8F1ULL,0x4102145B7E285BF9ULL,0xF32C938586FE0F2DULL,0x410559725DB272F7ULL,
   0xEFF7B866E8BD92F8ULL,0x4108AFCEF51F0FB5ULL,0xB5FAD34051767BDBULL,0x410C0DE1593369D1ULL,0x2379881065D41AD2ULL,0x410F5159AF804446ULL,
   0xAC57EA147F492186ULL,0x4112A5B01B605557ULL,0xCBB6F24CCF8DB4F4ULL,0x4116078E111C3556ULL,0x7EA4AEE003712231ULL,0x41194971956342ACULL,
   0x9E4DDA98044D6ABDULL,0x411C9BCDFABC1357ULL,0xC2F0A89F02B062B6ULL,0x41200160BCB58C16ULL,0x73ACD2C6C35C7B64ULL,0x412341B8EBE2EF1CULL,
   0x9098077874339A3CULL,0x4126922726DBAAE3ULL,0x74BE0956914080CCULL,0x4129F6B0F092959CULL,0xC8F6C5D61AC8507FULL,0x412D3A2E965B9D81ULL,
   0x3B34774BA17A649FULL,0x413088BA3BF284E2ULL,0xCA01951E89D8FDC7ULL,0x4133EAE8CAEF261AULL,0xBE40FD3316279E9CULL,0x413732D17ED577D0ULL,
   0xEDD13C7FDBB18643ULL,0x413A7F85DE8AD5C4ULL,0x29458B9FD29DE7D4ULL,0x413DDF67562D8B36ULL,0xD9CB7743E3A2B0E5ULL,0x41412BA095DC7701ULL,
   0x503E5514DC8B5D1EULL,0x41447688BB5394C2ULL,0xE44DEA5A13AE3465ULL,0x4147D42AEA2879F2ULL,0xCEB0B2784C4CE0BFULL,0x414B249AD2594C37ULL,
   0xC25CDF165F6018EFULL,0x414E6DC186EF9F45ULL,0x32F416DBF7381F2BULL,0x4151C931E8AB8717ULL,0x7FD88E497A83137BULL,0x41551DBF316B346EULL,
   0x1FCEB1DBD923D859ULL,0x4158652EFDC6018AULL,0xA7C25E52CF6CCE70ULL,0x415BBE7ABD3781ECULL,0xE8D97AF3C1A40106ULL,0x415F170CB642B133ULL,
   0xE30FD9B0B20D0147ULL,0x41625CCFE3D35D80ULL,0x1BD3D01CDE904199ULL,0x4165B403DCC834E1ULL,0xB16462120B1A2900ULL,0x4169108269FD210CULL,
   0xDDBD7A968DE0B340ULL,0x416C54A3047C694FULL,0xD52CD93C3158E010ULL,0x416FA9CBC59B83A3ULL,0x653C07C59ED78C0AULL,0x41730A1F5B813246ULL,
   0xFE8B09B7068D6F0CULL,0x41764CA732617ED7ULL,0xFE2DCC24C830CACFULL,0x41799FD0FEF9DE8DULL,0xBEDC9F96FD1E7EC2ULL,0x417D03E29F5C2B18ULL,
   0xEE93C77CBC661E72ULL,0x418044DB473335DEULL,0xAA38B95BEB7FA60EULL,0x4183961219000356ULL,0x54C6E7B2E65F8F92ULL,0x4186FB969F40042CULL,
   0xB4FC50CFCFFBB9BBULL,0x418A3D3E2388029BULL,0xA23B6503C3FAA82AULL,0x418D8C8DAC6A0342ULL,0x4ACA3E44B4F95235ULL,0x4190EFB117848413ULL,
   0x0EBE66EAF11BD361ULL,0x419435CEAEB2D28CULL,0x126E00A5AD62C839ULL,0x419783425A5F872FULL,0xD70980CF18BB7A47ULL,0x419AE412F0F768FAULL,
   0xC665F0816F752C6DULL,0x419E2E8BD69AA19CULL,0xF7FF6CA1CB527788ULL,0x41A17A2ECC414A03ULL,0xF5FF47CA3E27156AULL,0x41A4D8BA7F519C84ULL,
   0x19BF8CDE66D86D62ULL,0x41A827748F9301D3ULL,0xE02F7016008E88BBULL,0x41AB7151B377C247ULL,0xD83B4C1B80B22AE9ULL,0x41AECDA62055B2D9ULL,
   0x27250F91306F5AD2ULL,0x41B22087D4358FC8ULL,0x30EE53757C8B3186ULL,0x41B568A9C942F3BAULL,0xBD29E852DBADFDE8ULL,0x41B8C2D43B93B0A8ULL,
   0x763A3133C94CBEB1ULL,0x41BC19C4A53C4E69ULL,0xD3C8BD80BB9FEE5DULL,0x41BF6035CE8B6203ULL,0xC8BAECE0EA87E9F4ULL,0x41C2B843422E3A84ULL,
   0xFD74D40C9294F239ULL,0x41C6132A095CE492ULL,0xBCD2090FB73A2EC7ULL,0x41C957F48BB41DB7ULL,0xAC068B53A508BA79ULL,0x41CCADF1AEA12525ULL,
   0x8B8417144725748BULL,0x41D00CB70D24B737ULL,0x6E651CD958EED1AEULL,0x41D34FE4D06DE505ULL,0xC9FE640FAF2A861AULL,0x41D6A3DE04895E46ULL,
   0x3E3EFE89CD7A93D0ULL,0x41DA066AC2D5DAECULL,0x4DCEBE2C40D938C4ULL,0x41DD4805738B51A7ULL,0x21426DB7510F86F5ULL,0x41E09A06D06E2611ULL,
   0xB4C9849292A9B459ULL,0x41E400444244D7CAULL,0x61FBE5B73754216FULL,0x41E7405552D60DBDULL,0xBA7ADF25052929CBULL,0x41EA906AA78B912CULL,
   0xE91996EE4673743EULL,0x41EDF485516E7577ULL,0xF1AFFE54EC0828A7ULL,0x41F138D352E5096AULL,0xAE1BFDEA270A32D1ULL,0x41F48708279E4BC5ULL,
   0x19A2FD64B0CCBF85ULL,0x41F7E8CA3185DEB7ULL,0x7005DE5EEE7FF7B3ULL,0x41FB317E5EF3AB32ULL,0x0C0755F6AA1FF5A0ULL,0x41FE7DDDF6B095FFULL,
   0xCF092B7454A7F308ULL,0x4201DD55745CBB7EULL,0x4165BB28B4E8F7E5ULL,0x42052A5568B9F52FULL,0x11BF29F2E22335DEULL,0x420874EAC2E8727BULL,
   0xD62EF46F9AAC0355ULL,0x420BD22573A28F19ULL,0x25DD58C5C0AB8215ULL,0x420F235768459970ULL,0x2F54AEF730D6629BULL,0x42126C2D4256FFCCULL,
   0x3B29DAB4FD0BFB41ULL,0x4215C73892ECBFBFULL,0x84FA28B11E277D09ULL,0x42191C835BD3F7D7ULL,0x6638B2DD65B15C4BULL,0x421C63A432C8F5CDULL,
   0xBFC6DF94BF1DB35EULL,0x421FBC8D3F7B3340ULL,0x77DC4BBCF772901BULL,0x422315D847AD0008ULL,0x95D35EAC354F3421ULL,0x42265B4E5998400AULL,
   0x3B48365742A3012AULL,0x4229B221EFFE500DULL,0x450D21F689A5E0BAULL,0x422D0F5535FEF208ULL,0x56506A742C0F58E9ULL,0x4230532A837EAE8AULL,
   0xEBE4851137132F23ULL,0x4233A7F5245E5A2CULL,0x136ED32AC26BFD76ULL,0x423708F936BAF85CULL,0x184A87F57306FCD3ULL,0x423A4B378469B673ULL,
   0xDE5D29F2CFC8BC08ULL,0x423D9E056584240FULL,0xEAFA3A37C1DD7585ULL,0x424102C35F729689ULL,0x65B8C8C5B254D2E6ULL,0x42444374374F3C2CULL,
   0x7F26FAF71EEA07A0ULL,0x4247945145230B37ULL,0x5EF0B9B4E6A48988ULL,0x424AF965966BCE05ULL,0x5B5674111026D5F5ULL,0x424E3BDF7E0360C3ULL,
   0x322C111554308B72ULL,0x42518AD75D8438F4ULL,0x3EB7155AA93CAE4EULL,0x4254ED8D34E54731ULL,0xC7326D58A9C5ECF1ULL,0x42583478410F4C7EULL,
   0x78FF08AED437682DULL,0x425B819651531F9EULL,0x173ECADA89454239ULL,0x425EE1FBE5A7E786ULL,0xCE873EC895CB4963ULL,0x42622D3D6F88F0B3ULL,
   0xC2290E7ABB3E1BBCULL,0x4265788CCB6B2CE0ULL,0xF2B352196A0DA2ABULL,0x4268D6AFFE45F818ULL,0x97B0134FE24885ABULL,0x426C262DFEEBBB0FULL,
   0x7D9C1823DADAA716ULL,0x426F6FB97EA6A9D3ULL,0x5D031E2CD19150DBULL,0x4272CBA7DE505448ULL,0x3A21F2DC02FAD289ULL,0x42761F48EAF234ADULL,
   0x88AA6F9303B9872BULL,0x4279671B25AEC1D8ULL,0xAAD50B77C4A7E8F6ULL,0x427CC0E1EF1A724EULL,0x2AC5272ADAE8F19AULL,0x4280188D35708771ULL,
   0x757670F591A32E00ULL,0x42835EB082CCA94DULL,0xD2D40D32F60BF980ULL,0x4286B65CA37FD3A0ULL,0x83C4883FD9C77BF0ULL,0x428A11F9E62FE444ULL,
   0xA4B5AA4FD0395AECULL,0x428D56785FBBDD55ULL,0x0DE314E3C447B1A7ULL,0x4290AC1677AAD4ABULL,0xE8ADED0E5AACCF09ULL,0x42940B8E0ACAC4EAULL,
   0xA2D96851F15802CBULL,0x42974E718D7D7625ULL,0x0B8FC2666DAE037DULL,0x429AA20DF0DCD3AFULL,0x6739D980048CC22EULL,0x429E0548B68A044DULL,
   0xC1084FE005AFF2BAULL,0x42A1469AE42C8560ULL,0xF14A63D8071BEF69ULL,0x42A498419D37A6B8ULL,0x2D9CFCCE08E2EB43ULL,0x42A7FE5204859067ULL,
   0x7C821E00C58DD30AULL,0x42AB3EF342D37A40ULL,0x9BA2A580F6F147CCULL,0x42AE8EB0138858D0ULL,0xC28B4EE134AD99BFULL,0x42B1F25C186A6F04ULL,
   0xF997114CC0EC8017ULL,0x42B537798F428562ULL,0xB7FCD59FF127A01DULL,0x42B88557F31326BBULL,0xA5FC0B07ED718825ULL,0x42BBE6ADEFD7F06AULL,
   0xA7BD86E4F466F517ULL,0x42BF302CB5E6F642ULL,0x51ACE89E3180B25DULL,0x42C27C37E360B3D3ULL,0x261822C5BDE0DEF4ULL,0x42C5DB45DC38E0C8ULL,
   0x17CF15BB96AC8B58ULL,0x42C9290BA9A38C7DULL,0x5DC2DB2A7C57AE2EULL,0x42CC734E940C6F9CULL,0x753391F51B6D99BAULL,0x42CFD022390F8B83ULL,
   0x29403B3931248014ULL,0x42D3221563A9B732ULL,0xB3904A077D6DA019ULL,0x42D66A9ABC9424FEULL,0x60745C895CC90820ULL,0x42D9C5416BB92E3EULL,
   0xFC48B9D5D9FDA514ULL,0x42DD1B48E353BCE6ULL,0xBB5AE84B507D0E59ULL,0x42E0621B1C28AC20ULL,0xEA31A25E249C51EFULL,0x42E3BAA1E332D728ULL,
   0x925F057AD6E1B335ULL,0x42E714A52DFFC679ULL,0xF6F6C6D98C9A2003ULL,0x42EA59CE797FB817ULL,0xF4B4788FEFC0A803ULL,0x42EDB04217DFA61DULL,
   0xB8F0CB59F5D86902ULL,0x42F10E294EEBC7D2ULL,0x672CFE30734E8343ULL,0x42F451B3A2A6B9C7ULL,0x40F83DBC90222413ULL,0x42F7A6208B506839ULL,
   0xC89B2695DA15568CULL,0x42FB07D457124123ULL,0xBAC1F03B509AAC2FULL,0x42FE49C96CD6D16CULL,0xE9726C4A24C1573BULL,0x43019C3BC80C85C7ULL,
   0xF1E783AE56F8D685ULL,0x430501A55D07D39CULL,0x2E616499ECB70C26ULL,0x4308420EB449C884ULL,0x39F9BDC067E4CF2FULL,0x430B9292615C3AA5ULL,
   0x88782D3081DE02FBULL,0x430EF736F9B3494EULL,0x154B1C3E512AC1DDULL,0x43123A825C100DD1ULL,0x5A9DE34DE5757254ULL,0x43158922F3141145ULL,
   0xB1455C215ED2CEE9ULL,0x4318EB6BAFD91596ULL,0x2ECB5994DB43C152ULL,0x431C33234DE7AD7EULL,0xBA7E2FFA1214B1A6ULL,0x431F7FEC216198DDULL,
   0x291DBBF89699DE10ULL,0x4322DFE729B9FF15ULL,0x39B2957B5E202ACAULL,0x43262BF07A143F6DULL,0x881F3ADA35A8357CULL,0x432976EC98994F48ULL,
   0xAA270990C31242DCULL,0x432CD4A7BEBFA31AULL,0xAA5865FA79EB69C9ULL,0x433024E8D737C5F0ULL,0xD4EE7F791866443CULL,0x43336E230D05B76CULL,
   0x0A2A1F575E7FD54AULL,0x4336C9ABD0472548ULL,0x065A53969B0FE54FULL,0x433A1E0B622C774DULL,0x47F0E87C41D3DEA2ULL,0x433D658E3AB79520ULL,
   0x59ED229B5248D64BULL,0x4340BEF1C9657A68ULL,0x383435A1136D85EFULL,0x434417571DDF6C81ULL,0x864143095848E76AULL,0x43475D2CE55747A1ULL,
   0xE7D193CBAE5B2145ULL,0x434AB4781EAD1989ULL,0x30E2FC5F4CF8F4CBULL,0x434E10CB132C2FF6ULL,0xBD1BBB77203731FEULL,0x435154FDD7F73BF3ULL,
   0xAC62AA54E844FE7DULL,0x4354AA3D4DF50AF0ULL,0x6BBDAA75112B1F0EULL,0x43580A6650B926D6ULL,0x06AD15125575E6D2ULL,0x435B4CFFE4E7708CULL,
   0x08585A56EAD36086ULL,0x435EA03FDE214CAFULL,0x6537387652C41C54ULL,0x43620427EAD4CFEDULL,0xBE850693E7752369ULL,0x43654531E58A03E8ULL,
   0xEE264838E1526C43ULL,0x4368967E5EEC84E2ULL,0xA9AFDA4719A70754ULL,0x436BFC1DF6A7A61BULL,0x4A0DE86C70086495ULL,0x436F3D92BA28C7D1ULL,
   0x9C9162878C0A7DBAULL,0x43728CF768B2F9C5ULL,0x03B5BB296F0D1D28ULL,0x4375F03542DFB837ULL,0x625194F9E5683239ULL,0x4379362149CBD322ULL,
   0xFAE5FA385EC23EC7ULL,0x437C83A99C3EC7EAULL,0xB99F78C67672CE79ULL,0x437FE494034E79E5ULL,0x9403AB7C0A07C10CULL,0x43832EDC82110C2FULL,
   0x7904965B0C89B14FULL,0x43867A93A2954F3BULL,0x5745BBF1CFAC1DA2ULL,0x4389D9388B3AA30AULL,0x768B957721CB9285ULL,0x438D27C35704A5E6ULL,
   0x142E7AD4EA3E7727ULL,0x439071B42CC5CF60ULL,0x193A198A24CE14F0ULL,0x4393CE2137F74338ULL,0x0FC44FF65700CD16ULL,0x439720D4C2FA8A03ULL,
   0xD3B563F3ECC1005CULL,0x439A6909F3B92C83ULL,0xC8A2BCF0E7F14073ULL,0x439DC34C70A777A4ULL,0xFD65B61690F6C848ULL,0x43A11A0FC668AAC6ULL,
   0xBCBF239C35347A5AULL,0x43A46093B802D578ULL,0xEBEEEC83428198F0ULL,0x43A7B8B8A6038AD6ULL,0x537553D20990FF96ULL,0x43AB137367C236C6ULL,
   0xE852A8C68BF53F7CULL,0x43AE585041B2C477ULL,0xE26752F82EF28F5BULL,0x43B1AE64521F7595ULL,0xAD8093DB1D579999ULL,0x43B50CFEB353A97DULL,
   0x18E0B8D1E4AD7FFFULL,0x43B8503E602893DDULL,0x5F18E7065DD8DFFEULL,0x43BBA44DF832B8D4ULL,0xBB6F9063FAA78BFFULL,0x43BF06B0BB1FB384ULL,
   0xEA4B747CF9516EFFULL,0x43C2485CE9E7A065ULL,0x64DE519C37A5CABEULL,0x43C59A742461887FULL,0x9F0AF301A2C79EB7ULL,0x43C9008896BCF54FULL,
   0x86CDAFC20B798665ULL,0x43CC40AABC6C32A3ULL,0x68811BB28E57E7FEULL,0x43CF90D56B873F4CULL,0x82A1629F31EDE1FDULL,0x43D2F50AC6690F1FULL,
   0xB1A4DDA37F34AD3EULL,0x43D63926BC01A973ULL,0x9E0E150C5F01D88EULL,0x43D987706B0213D0ULL,0xC5919A4F76C24EB2ULL,0x43DCE94C85C298C4ULL,
   0xFB7B0071AA39712FULL,0x43E031CFD3999F7AULL,0xBA59C08E14C7CD7BULL,0x43E37E43C8800759ULL,0x28F030B199F9C0D9ULL,0x43E6DDD4BAA00930ULL,
   0x19961E6F003C1888ULL,0x43EA2AA4F4A405BEULL,0x9FFBA60AC04B1EAAULL,0x43ED754E31CD072DULL,0x07FA8F8D705DE654ULL,0x43F0D2A1BE4048F9ULL,
   0xA4FC99B8663AAFF5ULL,0x43F423A516E82D9BULL,0x8E3BC0267FC95BF2ULL,0x43F76C8E5CA23902ULL,0x31CAB0301FBBB2EEULL,0x43FAC7B1F3CAC743ULL,
   0xFF1EAE1E13D54FD5ULL,0x43FE1CCF385EBC89ULL,0x7EE659A598CAA3CAULL,0x4401640306766BACULL,0x9E9FF00EFEFD4CBDULL,0x4404BD03C8140697ULL,
   0xC323F6095F5E4FF6ULL,0x440816225D0C841EULL,0x73ECF38BB735E3F3ULL,0x440B5BAAF44FA526ULL,0x10E8306EA5035CF0ULL,0x440EB295B1638E70ULL,
   0x0A911E4527221A16ULL,0x44120F9D8EDE3906ULL,0x8D3565D670EAA09CULL,0x44155384F295C747ULL,0x7082BF4C0D2548C3ULL,0x4418A8662F3B3919ULL,
   0xE651B78F88374D7AULL,0x441C093FDD8503AFULL,0xDFE625736A4520D8ULL,0x441F4B8FD4E6449BULL,0xD7DFAED044D6690EULL,0x44229E73CA1FD5C2ULL,
   0xC6EBCD422B0601A9ULL,0x442603085E53E599ULL,0x38A6C092B5C78213ULL,0x442943CA75E8DF00ULL,0x46D070B763396298ULL,0x442C94BD136316C0ULL,
   0x58848CE53C07BB3EULL,0x442FF9EC583BDC70ULL,0x3752D80F4584D507ULL,0x44333C33B72569C6ULL
};


static void mul128(uint64_t result_out[4], uint64_t a[2], uint64_t b[2])
{
   uint32_t src1[4];
   uint32_t src2[4];
   uint32_t result[8];
   uint64_t mul[16];
   uint64_t tmp1, tmp2;

   src1[0] = a[0];
   src1[1] = a[0] >> 32;
   src1[2] = a[1];
   src1[3] = a[1] >> 32;
   src2[0] = b[0];
   src2[1] = b[0] >> 32;
   src2[2] = b[1];
   src2[3] = b[1] >> 32;

   //                                     a0*b0
   //                               a0*b1
   //                         a0*b2 a1*b0
   //                   a0*b3 a1*b1
   //                   a1*b2 a2*b0
   //             a1*b3 a2*b1
   //             a2*b2 a3*b0
   //       a2*b3 a3*b1
   //       a3*b2
   // a3*b3

   mul[ 0] = (uint64_t)src1[0] * (uint64_t)src2[0];
   mul[ 1] = (uint64_t)src1[0] * (uint64_t)src2[1];
   mul[ 2] = (uint64_t)src1[0] * (uint64_t)src2[2];
   mul[ 3] = (uint64_t)src1[0] * (uint64_t)src2[3];
   mul[ 4] = (uint64_t)src1[1] * (uint64_t)src2[0];
   mul[ 5] = (uint64_t)src1[1] * (uint64_t)src2[1];
   mul[ 6] = (uint64_t)src1[1] * (uint64_t)src2[2];
   mul[ 7] = (uint64_t)src1[1] * (uint64_t)src2[3];
   mul[ 8] = (uint64_t)src1[2] * (uint64_t)src2[0];
   mul[ 9] = (uint64_t)src1[2] * (uint64_t)src2[1];
   mul[10] = (uint64_t)src1[2] * (uint64_t)src2[2];
   mul[11] = (uint64_t)src1[2] * (uint64_t)src2[3];
   mul[12] = (uint64_t)src1[3] * (uint64_t)src2[0];
   mul[13] = (uint64_t)src1[3] * (uint64_t)src2[1];
   mul[14] = (uint64_t)src1[3] * (uint64_t)src2[2];
   mul[15] = (uint64_t)src1[3] * (uint64_t)src2[3];

   result[0] = mul[0];
   tmp1 = (mul[0] >> 32) + (mul[1] & 0xFFFFFFFF) + (mul[4] & 0xFFFFFFFF);
   result[1] = tmp1;
   tmp2 = (tmp1 >> 32) + (mul[1] >> 32) + (mul[4] >> 32) + (mul[2] & 0xFFFFFFFF) + (mul[5] & 0xFFFFFFFF) + (mul[8] & 0xFFFFFFFF);
   result[2] = tmp2;
   tmp1 = (tmp2 >> 32) + (mul[2] >> 32) + (mul[5] >> 32) + (mul[8] >> 32) + (mul[3] & 0xFFFFFFFF) + (mul[6] & 0xFFFFFFFF) + (mul[9] & 0xFFFFFFFF) + (mul[12] & 0xFFFFFFFF);
   result[3] = tmp1;
   tmp2 = (tmp1 >> 32) + (mul[3] >> 32) + (mul[6] >> 32) + (mul[9] >> 32) + (mul[12] >> 32) + (mul[7] & 0xFFFFFFFF) + (mul[10] & 0xFFFFFFFF) + (mul[13] & 0xFFFFFFFF);
   result[4] = tmp2;
   tmp1 = (tmp2 >> 32) + (mul[7] >> 32) + (mul[10] >> 32) + (mul[13] >> 32) + (mul[11] & 0xFFFFFFFF) + (mul[14] & 0xFFFFFFFF);
   result[5] = tmp1;
   tmp2 = (tmp1 >> 32) + (mul[11] >> 32) + (mul[14] >> 32) + (mul[15] & 0xFFFFFFFF);
   result[6] = tmp2;
   tmp1 = (tmp2 >> 32) + (mul[15] >> 32);
   result[7] = tmp1;

   result_out[0] = ((uint64_t)result[0]) | (((uint64_t)result[1]) << 32);
   result_out[1] = ((uint64_t)result[2]) | (((uint64_t)result[3]) << 32);
   result_out[2] = ((uint64_t)result[4]) | (((uint64_t)result[5]) << 32);
   result_out[3] = ((uint64_t)result[6]) | (((uint64_t)result[7]) << 32);
}


static void shl128(uint64_t value[2], int amount)
{
   if (amount < 64) {
      value[1] = (value[1] << amount) | (value[0] >> (64 - amount));
      value[0] <<= amount;
   }
   else {
      value[1] = value[0] << (amount - 64);
      value[0] = 0;
   }
}


static void shr128(uint64_t value[2])
{
   value[0] = (value[1] << 63) | (value[0] >> 1);
   value[1] >>= 1;
}


static void inc128(uint64_t value[2])
{
   if (value[0] == 0xFFFFFFFFFFFFFFFFULL) {
      value[0] = 0;
      value[1]++;
   }
   else {
      value[0]++;
   }
}


static void float128_mul(uint64_t result[2], uint64_t a[2], uint64_t b[2])
{
   int e1, e2, e;
   uint64_t ma[2], mb[2], mul[4];

   ma[0] = a[0];
   ma[1] = (a[1] & ((1ULL << 48)-1)) | (1ULL << 48);
   mb[0] = b[0];
   mb[1] = (b[1] & ((1ULL << 48)-1)) | (1ULL << 48);

   e1 = ((a[1] >> 48) & 0x7FFF) - 16383;
   e2 = ((b[1] >> 48) & 0x7FFF) - 16383;

   e = e1 + e2;
   shl128(ma, 9);
   shl128(mb, 8);
   mul128(mul, ma, mb);
   
   if (mul[3] & (1ULL << 50)) {
      e++;
      mul[0] |= mul[2] & 1;
      shr128(&mul[2]);
   }

   if (mul[2] & 1) {
      if (mul[0] | mul[1]) {
         inc128(&mul[2]);
      }
      else if (mul[2] & 2) {
         inc128(&mul[2]);
      }
      if (mul[3] & (1ULL << 50)) {
         e++;
         shr128(&mul[2]);
      }
   }

   shr128(&mul[2]);
   result[0] = mul[2];
   result[1] = ((uint64_t)(e+16383) << 48) | (mul[3] & ((1ULL << 48)-1));
}


static void float128_from_long(uint64_t f[2], uint64_t value)
{
   int lz;

   if (value == 0) {
      f[0] = 0;
      f[1] = 0;
      return;
   }

   f[0] = value;
   f[1] = 0;
   lz = __builtin_clzll(value);
   shl128(f, 112 - 64 + lz + 1);
   f[1] = (f[1] & ((1ULL << 48)-1)) | (((uint64_t)(64 - lz - 1 + 16383)) << 48);
}


static void float128_from_double(uint64_t f[2], double value)
{
   union {
      double f;
      uint64_t i;
   } u;
   int e;

   u.f = value;
   e = (int)((u.i >> 52) & 0x7FF) - 1023;
   if (e == -1023) {
      u.i <<= 1;
      while ((u.i & (1ULL << 52)) == 0) {
         e--;
         u.i <<= 1;
      }
   }
   f[0] = u.i & ((1ULL<<52)-1);
   f[1] = 0;
   shl128(f, 60);
   f[1] |= ((uint64_t)(e+16383) << 48);
}


static double float128_to_double(uint64_t f[2])
{
   union {
      double f;
      uint64_t i;
   } u;
   int e;
   uint64_t tmp[2];

   if ((f[0] | f[1]) == 0) {
      return 0.0;
   }

   e = ((f[1] >> 48) & 0x7FFF) - 16383;

   tmp[0] = f[0];
   tmp[1] = (f[1] & ((1ULL << 48)-1)) | (1ULL << 48);
   shl128(tmp, 5);

   if (tmp[1] & 1) {
      if (tmp[0]) {
         tmp[1]++;
      }
      else if (tmp[1] & 2) {
         tmp[1]++;
      }
      if (tmp[1] & (1ULL << 54)) {
         e++;
         tmp[1] >>= 1;
      }
   }

   if (e <= -1023) {
      e--;
      tmp[1] = (tmp[1] | (1ULL<<53)) >> (-(1023 + e));
      e = -1023;
   }

   u.i = ((uint64_t)(e+1023) << 52) | ((tmp[1] >> 1) & ((1ULL << 52)-1));
   return u.f;
}


#if 0
// buffer must be at least 20 bytes long
static char *float_to_string(char *buf, float input_value)
{
   union {
      double f;
      uint64_t i;
   } u;
   double value;
   int s, e, de, pos, num_digits, dot_pos;
   char *end;
   uint64_t digits, m;

   u.f = input_value;
   s = u.i >> 63;
   e = (int)((u.i >> 52) & 0x7FF) - 1023;
   u.i &= (1ULL << 63)-1;
   value = u.f;

   if (e == -1023) {
      strcpy(buf, s? "-0" : "0");
      return buf;
   }

   if (e == 1024) {
      m = u.i & ((1ULL<<52)-1);
      if (m) {
         strcpy(buf, "nan");
      }
      else {
         strcpy(buf, s? "-inf" : "inf");
      }
      return buf;
   }

   if (u.i < 0x41E0000000000000ULL && value == (int64_t)value && (int64_t)value < 1000000000) {
      digits = (int64_t)value;
      buf += 12;
      *(--buf) = 0;
      while (digits) {
         *(--buf) = '0' + (digits % 10);
         digits /= 10;
      }
      if (s) {
         *(--buf) = '-';
      }
      return buf;
   }

   u.i = float_log10_2;
   de = floor(e * u.f);
   if (de > 38) {
      de = 38;
   }
   if (de < -45) {
      de = -45;
   }

   u.i = float_powers_of_ten[53-de];
   value *= u.f;

   digits = value;
   num_digits = 1;
   if (digits >= 10) {
      de++;
      num_digits++;
   }
   if (digits >= 100) {
      de++;
      num_digits++;
   }

   u.f = value;
   e = (int)((u.i >> 52) & 0x7FF) - 1023;
   m = u.i & ((1ULL<<52)-1);

   pos = 52 - e;

   while (m) {
      m &= (1ULL<<pos)-1;
      m *= 10;
      if (num_digits >= 9) {
         if ((m >> pos) >= 5) {
            digits++;
         }
         break;
      }
      digits = digits*10 + (m >> pos);
      num_digits++;
   }

   dot_pos = num_digits-de-1;
   if (dot_pos == 0) dot_pos--;
   if (de >= num_digits || de <= -5) {
      dot_pos = num_digits-1;
   }

   end = buf + 15;
   buf = end; 
   *buf = 0;
   while (digits) {
      if (dot_pos-- == 0) {
         *(--buf) = '.';
      }
      *(--buf) = '0' + (digits % 10);
      digits /= 10;
   }
   if (dot_pos >= 0) {
      while (--dot_pos >= 0) {
         *(--buf) = '0';
      }
      *(--buf) = '.';
      *(--buf) = '0';
   }
   if (s) {
      *(--buf) = '-';
   }

   while (*(end-1) == '0') {
      *(--end) = 0;
   }

   if (de >= num_digits) {
      *end++ = 'e';
      *end++ = '+';
      *end++ = '0' + (de / 10);
      *end++ = '0' + (de % 10);
      *end++ = 0;
   }
   else if (de <= -5) {
      de = -de;
      *end++ = 'e';
      *end++ = '-';
      *end++ = '0' + (de / 10);
      *end++ = '0' + (de % 10);
      *end++ = 0;
   }

   return buf;
}
#endif


// buffer must be at least 29 bytes long
char *double_to_string(char *buf, double input_value) EXPORT("double_to_string")
{
   union {
      double f;
      uint64_t i;
   } u;
   uint64_t value[2], power[2];
   int s, e, de, pos, num_digits, dot_pos;
   char *end;
   uint64_t digits, m;

   u.f = input_value;
   s = u.i >> 63;
   e = (int)((u.i >> 52) & 0x7FF) - 1023;
   u.i &= (1ULL << 63)-1;
   input_value = u.f;

   if (u.i == 0) {
      strcpy(buf, s? "-0" : "0");
      return buf;
   }

   if (e == 1024) {
      m = u.i & ((1ULL<<52)-1);
      if (m) {
         strcpy(buf, "nan");
      }
      else {
         strcpy(buf, s? "-inf" : "inf");
      }
      return buf;
   }

   if (u.i < 0x43E0000000000000ULL && input_value == (int64_t)input_value && (int64_t)input_value < 100000000000000000LL) {
      digits = (int64_t)input_value;
      buf += 19;
      *(--buf) = 0;
      while (digits) {
         *(--buf) = '0' + (digits % 10);
         digits /= 10;
      }
      if (s) {
         *(--buf) = '-';
      }
      return buf;
   }
   
   float128_from_double(value, input_value);
   e = (int)((value[1] >> 48) & 0x7FFF) - 16383;

   u.i = float_log10_2;
   de = floor(e * u.f);
   if (de > 308) {
      de = 308;
   }
   if (de < -324) {
      de = -324;
   }

   power[0] = double_powers_of_ten[(340-de)*2+0];
   power[1] = double_powers_of_ten[(340-de)*2+1];
   float128_mul(value, value, power);

   e = (int)((value[1] >> 48) & 0x7FFF) - 16383;

   digits = ((value[1] & ((1ULL << 48)-1)) | (1ULL << 48)) >> (48 - e);

   num_digits = 1;
   if (digits >= 10) {
      de++;
      num_digits++;
   }
   if (digits >= 100) {
      de++;
      num_digits++;
   }

   shl128(value, 12);
   m = value[1];

   pos = 60 - e;

   while (m) {
      m &= (1ULL<<pos)-1;
      m *= 10;
      if (num_digits >= 17) {
         if ((m >> pos) >= 5) {
            digits++;
         }
         break;
      }
      digits = digits*10 + (m >> pos);
      num_digits++;
   }

   dot_pos = num_digits-de-1;
   if (dot_pos == 0) dot_pos--;
   if (de >= num_digits || de <= -5) {
      dot_pos = num_digits-1;
   }

   end = buf + 23;
   buf = end; 
   *buf = 0;
   while (digits) {
      if (dot_pos-- == 0) {
         *(--buf) = '.';
      }
      *(--buf) = '0' + (digits % 10);
      digits /= 10;
   }
   if (dot_pos >= 0) {
      while (--dot_pos >= 0) {
         *(--buf) = '0';
      }
      *(--buf) = '.';
      *(--buf) = '0';
   }
   if (s) {
      *(--buf) = '-';
   }

   while (*(end-1) == '0') {
      *(--end) = 0;
   }

   if (de >= num_digits) {
      *end++ = 'e';
      *end++ = '+';
      if (de / 100) {
         *end++ = '0' + (de / 100);
      }
      *end++ = '0' + ((de / 10) % 10);
      *end++ = '0' + (de % 10);
      *end++ = 0;
   }
   else if (de <= -5) {
      de = -de;
      *end++ = 'e';
      *end++ = '-';
      if (de / 100) {
         *end++ = '0' + (de / 100);
      }
      *end++ = '0' + ((de / 10) % 10);
      *end++ = '0' + (de % 10);
      *end++ = 0;
   }

   return buf;
}


#if 0
static float string_to_float(char *s)
{
   union {
      float f;
      uint32_t i;
   } u;
   union {
      double f;
      uint64_t i;
   } u2;
   uint64_t digits = 0;
   double value;
   int num_digits = 0;
   int sign=0, e=0, e2, esign;
   int round=0, rest=0;

   if (*s == '+') {
      s++;
   }
   else if (*s == '-') {
      sign = 1;
      s++;
   }

   if (*s != '.') {
      while (*s == '0') {
         s++;
      }
      if (*s != '.') {
         for (;;) {
            if (*s >= '0' && *s <= '9') {
               if (num_digits < 9) {
                  digits = digits*10 + (*s - '0');
                  num_digits++;
               }
               else {
                  if (num_digits == 9) {
                     if (*s == '5') {
                        round = 1;
                     }
                     else if (*s > '5') {
                        round = 2;
                     }
                     num_digits++;
                  }
                  else {
                     if (*s != '0') {
                        rest = 1;
                     }
                  }
                  e++;
               }
               s++;
            }
            else break;
         }
      }
   }
   if (*s == '.') {
      s++;
      if (digits == 0) {
         while (*s == '0') {
            s++;
            e--;
         }
      }
      for (;;) {
         if (*s >= '0' && *s <= '9') {
            if (num_digits < 9) {
               digits = digits*10 + (*s - '0');
               e--;
               num_digits++;
            }
            else {
               if (num_digits == 9) {
                  if (*s == '5') {
                     round = 1;
                  }
                  else if (*s > '5') {
                     round = 2;
                  }
                  num_digits++;
               }
               else {
                  if (*s != '0') {
                     rest = 1;
                  }
               }
            }
            s++;
         }
         else break;
      }
   }
   if (round) {
      if (round == 1 && rest == 0) {
         if (digits & 1) {
            digits++;
         }
      }
      else if (round != 0) {
         digits++;
      }
      if (digits >= 1000000000) {
         digits /= 10;
         e++;
      }
   }
   if (*s == 'e' || *s == 'E') {
      s++;
      e2 = 0;
      esign = 0;
      if (*s == '+') {
         s++;
      }
      else if (*s == '-') {
         esign = 1;
         s++;
      }
      while (*s == '0') {
         s++;
      }
      for (;;) {
         if (*s >= '0' && *s <= '9') {
            e2 = e2*10 + (*s - '0');
            if (!esign && e+e2 > 38) {
               u.i = (0xFF << 23) | (sign << 31); // +/-inf
               return u.f;
            }
            else if (esign && e-e2 < -53) {
               u.i = sign << 31; // +/-0
               return u.f;
            }
            s++;
         }
         else break;
      }
      if (esign) {
         e -= e2;
      }
      else {
         e += e2;
      }
   }
   if (*s != 0) {
      u.i = (0xFF << 23) | (1 << 22); // NaN
      return u.f;
   }
   if (e > 38) {
      u.i = (0xFF << 23) | (sign << 31); // +/-inf
      return u.f;
   }
   if (e < -53) {
      u.i = sign << 31; // +/-0
      return u.f;
   }

   u2.i = float_powers_of_ten[53+e];
   value = (double)digits * u2.f;

   u.f = value;
   u.i |= sign << 31;
   return u.f;
}
#endif


double string_to_double(const char *s) EXPORT("string_to_double")
{
   union {
      double f;
      uint64_t i;
   } u;
   uint64_t digits = 0;
   uint64_t value[2], power[2];
   int num_digits = 0;
   int sign=0, e=0, e2, esign;
   int round=0, rest=0;

   if (*s == '+') {
      s++;
   }
   else if (*s == '-') {
      sign = 1;
      s++;
   }

   if (*s != '.') {
      while (*s == '0') {
         s++;
      }
      if (*s != '.') {
         for (;;) {
            if (*s >= '0' && *s <= '9') {
               if (num_digits < 17) {
                  digits = digits*10 + (*s - '0');
                  num_digits++;
               }
               else {
                  if (num_digits == 17) {
                     if (*s == '5') {
                        round = 1;
                     }
                     else if (*s > '5') {
                        round = 2;
                     }
                     num_digits++;
                  }
                  else {
                     if (*s != '0') {
                        rest = 1;
                     }
                  }
                  e++;
               }
               s++;
            }
            else break;
         }
      }
   }
   if (*s == '.') {
      s++;
      if (digits == 0) {
         while (*s == '0') {
            s++;
            e--;
         }
      }
      for (;;) {
         if (*s >= '0' && *s <= '9') {
            if (num_digits < 17) {
               digits = digits*10 + (*s - '0');
               e--;
               num_digits++;
            }
            else {
               if (num_digits == 17) {
                  if (*s == '5') {
                     round = 1;
                  }
                  else if (*s > '5') {
                     round = 2;
                  }
                  num_digits++;
               }
               else {
                  if (*s != '0') {
                     rest = 1;
                  }
               }
            }
            s++;
         }
         else break;
      }
   }
   if (round) {
      if (round == 1 && rest == 0) {
         if (digits & 1) {
            digits++;
         }
      }
      else if (round != 0) {
         digits++;
      }
      if (digits >= 100000000000000000ULL) {
         digits /= 10;
         e++;
      }
   }
   if (*s == 'e' || *s == 'E') {
      s++;
      e2 = 0;
      esign = 0;
      if (*s == '+') {
         s++;
      }
      else if (*s == '-') {
         esign = 1;
         s++;
      }
      while (*s == '0') {
         s++;
      }
      for (;;) {
         if (*s >= '0' && *s <= '9') {
            e2 = e2*10 + (*s - '0');
            if (!esign && e+e2 > 308) {
               u.i = (0x7FFULL << 52) | (((uint64_t)sign) << 63); // +/-inf
               return u.f;
            }
            else if (esign && e-e2 < -340) {
               u.i = ((uint64_t)sign) << 31; // +/-0
               return u.f;
            }
            s++;
         }
         else break;
      }
      if (esign) {
         e -= e2;
      }
      else {
         e += e2;
      }
   }
   if (*s != 0) {
      u.i = (0x7FFULL << 52) | (1ULL << 51); // NaN
      return u.f;
   }
   if (e > 308) {
      u.i = (0x7FFULL << 52) | (((uint64_t)sign) << 63); // +/-inf
      return u.f;
   }
   if (e < -340) {
      u.i = ((uint64_t)sign) << 63; // +/-0
      return u.f;
   }
   if (digits == 0) {
      e = 0;
   }

   float128_from_long(value, digits);
   power[0] = double_powers_of_ten[(340+e)*2+0];
   power[1] = double_powers_of_ten[(340+e)*2+1];
   float128_mul(value, value, power);

   u.f = float128_to_double(value);
   u.i |= ((uint64_t)sign) << 63;
   return u.f;
}


static float ftrunc(float value)
{
   union {
      float f;
      uint32_t i;
   } u;
   int e;

   u.f = value;
   e = ((u.i >> 23) & 0xFF) - 127;

   if (e < 0) {
      u.i &= 0x80000000;
   }
   else if (e < 23) {
      u.i &= ~((1 << (23-e))-1);
   }

   return u.f;
}


static float ftrunc_up(float value)
{
   union {
      float f;
      uint32_t i;
   } u;
   uint32_t m;
   int e;

   u.f = value;
   e = ((u.i >> 23) & 0xFF) - 127;

   if (e < 0) {
      if (u.i & 0x7FFFFFFF) {
         return u.i & 0x80000000? -1.0f : 1.0f;
      }
      return u.i & 0x80000000? -0.0f : 0.0f;
   }
   else if (e < 23) {
      if (u.i & ((1 << (23-e))-1)) {
         m = (u.i & ((1 << 23)-1)) | (1 << 23);
         m &= ~((1 << (23-e))-1);
         m += 1 << (23-e);
         if (m & (1 << 24)) {
            m >>= 1;
            e++;
         }
         u.i = (u.i & 0x80000000) | ((e+127) << 23) | (m & ((1 << 23)-1));
      }
   }

   return u.f;
}


static double dtrunc(double value)
{
   union {
      double f;
      uint64_t i;
   } u;
   int e;

   u.f = value;
   e = ((u.i >> 52) & 0x7FF) - 1023;

   if (e < 0) {
      u.i &= 0x8000000000000000ULL;
   }
   else if (e < 52) {
      u.i &= ~((1ULL << (52-e))-1);
   }

   return u.f;
}


static double dtrunc_up(double value)
{
   union {
      double f;
      uint64_t i;
   } u;
   uint64_t m;
   int e;

   u.f = value;
   e = ((u.i >> 52) & 0x7FF) - 1023;

   if (e < 0) {
      if (u.i & 0x7FFFFFFFFFFFFFFFULL) {
         return u.i & 0x8000000000000000ULL? -1.0 : 1.0;
      }
      return u.i & 0x8000000000000000ULL? -0.0 : 0.0;
   }
   else if (e < 52) {
      if (u.i & ((1ULL << (52-e))-1)) {
         m = (u.i & ((1ULL << 52)-1)) | (1ULL << 52);
         m &= ~((1ULL << (52-e))-1);
         m += 1ULL << (52-e);
         if (m & (1ULL << 53)) {
            m >>= 1;
            e++;
         }
         u.i = (u.i & 0x8000000000000000ULL) | (((uint64_t)(e+1023)) << 52) | (m & ((1ULL << 52)-1));
      }
   }

   return u.f;
}


float floorf(float value)
{
   return value >= 0.0f? ftrunc(value) : ftrunc_up(value);
}


float ceilf(float value)
{
   return value >= 0.0f? ftrunc_up(value) : ftrunc(value);
}


float roundf(float value)
{
   return ftrunc(value >= 0.0f? value + 0.5f : value - 0.5f);
}


double floor(double value)
{
   return value >= 0.0f? dtrunc(value) : dtrunc_up(value);
}


double ceil(double value)
{
   return value >= 0.0f? dtrunc_up(value) : dtrunc(value);
}


double round(double value)
{
   return dtrunc(value >= 0.0f? value + 0.5f : value - 0.5f);
}


// https://en.wikipedia.org/wiki/Exponentiation_by_squaring#With_constant_auxiliary_memory

static double exp_sqr(double x, int n)
{
   double y = 1.0;

   if (n == 0) {
      return 1.0;
   }
   while (n > 1) {
      if (n & 1) {
         y *= x;
         x *= x;
      }
      else {
         x *= x;
      }
      n >>= 1;
   }
   return x * y;
}


// https://en.wikipedia.org/wiki/Exponential_function#Computation

static double exp_taylor(double x)
{
   double x2, x3, x4, x8;
   
   x2 = x*x;
   x3 = x2*x;
   x4 = x2*x2;
   x8 = x4*x4;

   return (
      1.0 + x +
      x2 * 0.5 +
      x3 * 0.16666666666666667 +
      x4 * 0.041666666666666667 +
      x4*x * 0.0083333333333333333 +
      x4*x2 * 0.0013888888888888889 +
      x4*x3 * 0.00019841269841269841 +
      x8 * 0.000024801587301587302 +
      x8*x * 0.0000027557319223985891 +
      x8*x2 * 0.00000027557319223985891 +
      x8*x3 * 0.000000025052108385441719
   );
}


float expf(float x)
{
   return exp(x);
}


double exp(double value)
{
   union {
      double f;
      uint64_t i;
   } u;
   double n, frac, result, taylor;
   int neg = 0;

   if (value < 0.0) {
      value = -value;
      neg = 1;
   }

   n = dtrunc(value);
   if (n > 709.0) {
      u.i = 0x7FFULL << 52; // inf
      return u.f;
   }
   frac = value - n;

   result = exp_sqr(2.7182818284590452, n);
   taylor = exp_taylor(frac * 0.25);
   taylor *= taylor;
   taylor *= taylor;
   result *= taylor;

   if (neg) {
      return 1.0 / result;
   }
   return result;
}


float log2f(float x)
{
   return log2(x);
}


// https://en.wikipedia.org/wiki/Binary_logarithm#Iterative_approximation

double log2(double value)
{
   union {
      double f;
      uint64_t i;
   } u;
   double m, tmp, result;
   int e, i, cnt;

   u.f = value;
   if (u.i >> 63) {
      u.i = (0x7FFULL << 52) | (1ULL << 51); // nan
      return u.f;
   }
   if (u.i == 0) {
      u.i = (0x7FFULL << 52) | (1ULL << 63); // -inf
      return u.f;
   }

   e = ((u.i >> 52) & 0x7FF) - 1023;
   u.i = (u.i & ((1ULL<<52)-1)) | (1023ULL << 52);
   m = u.f;

   result = e;
   e = 0;

   for (i=0; i<64; i++) {
      if (m == 1.0) break;
      cnt = 0;
      do {
         m *= m;
         cnt++;
      }
      while (m < 2.0);
      e -= cnt;
      if (e <= -1023) break;

      u.i = (uint64_t)(e + 1023) << 52;
      tmp = result + u.f;
      if (tmp == result) break;
      result = tmp;
      m *= 0.5;
   }

   return result;
}


float log10f(float x)
{
   return log10(x);
}


double log10(double value)
{
   return log2(value) * 0.30102999566398120;
}


float logf(float x)
{
   return log(x);
}


double log(double value)
{
   return log2(value) * 0.69314718055994531;
}


float powf(float x, float y)
{
   return pow(x, y);
}


double pow(double x, double y)
{
   if (x == 0.0) {
      return x;
   }
   if (y == 0.0) {
      return 1.0;
   }
   return exp(log(x) * y);
}


float sqrtf(float x)
{
   return sqrt(x);
}


double sqrt(double x)
{
   return pow(x, 0.5);
}


float cbrtf(float x)
{
   return cbrt(x);
}


double cbrt(double x)
{
   if (x < 0) {
      return -pow(-x, 0.33333333333333333);
   }
   return pow(x, 0.33333333333333333);
}


// https://en.wikipedia.org/wiki/Sine_and_cosine#Series_definitions

static double sin_taylor(double x)
{
   double x2, x3, x4, x5, x7, x8, x9;

   x2 = x*x;
   x3 = x2*x;
   x4 = x2*x2;
   x5 = x3*x2;
   x7 = x4*x3;
   x8 = x4*x4;
   x9 = x5*x4;

   return (
      x -
      x3 * 0.16666666666666667 +
      x5 * 0.0083333333333333333 -
      x7 * 0.00019841269841269841 +
      x9 * 0.0000027557319223985891 -
      x8*x3 * 0.000000025052108385441719 +
      x8*x5 * 0.00000000016059043836821615 -
      x8*x7 * 0.00000000000076471637318198165 +
      x9*x8 * 0.0000000000000028114572543455208 -
      x8*x8*x3 * 0.0000000000000000082206352466243297
   );
}


float sinf(float x)
{
   return sin(x);
}


double sin(double x)
{
   double tmp;
   int neg = 0, quadrant;
   
   if (x < 0.0) {
      x = -x;
      neg = 1;
   }

   tmp = dtrunc(x * 0.63661977236758134); // 1.0/(pi/2)
   x = x - tmp * 1.5707963267948966;
   tmp *= 0.25;
   quadrant = (tmp - dtrunc(tmp)) * 4.0;

   if (quadrant == 1 || quadrant == 3) {
      x = 1.5707963267948966 - x;
   }
   x = sin_taylor(x);
   if (quadrant == 2 || quadrant == 3) {
      x = -x;
   }
   if (neg) {
      x = -x;
   }
   return x;
}


float cosf(float x)
{
   return cos(x);
}


double cos(double x)
{
   return sin(x + 1.5707963267948966);
}


float tanf(float x)
{
   return tan(x);
}


double tan(double x)
{
   return sin(x) / cos(x);
}


// https://en.wikipedia.org/wiki/Inverse_trigonometric_functions#Infinite_series

static double asin_leibniz(double x)
{
   double x2, x3, x4, x5, x7, x8, x9, x11, x13, x15, x16, x17, x24;

   x2 = x*x;
   x3 = x2*x;
   x4 = x2*x2;
   x5 = x3*x2;
   x7 = x4*x3;
   x8 = x4*x4;
   x9 = x5*x4;
   x11 = x8*x3;
   x13 = x8*x5;
   x15 = x8*x7;
   x16 = x8*x8;
   x17 = x9*x8;
   x24 = x16*x8;

   return (
      x +
      x3 * 0.16666666666666667 +
      x5 * 0.075 +
      x7 * 0.044642857142857144 +
      x9 * 0.030381944444444444 +
      x11 * 0.022372159090909091 +
      x13 * 0.017352764423076923 +
      x15 * 0.01396484375 +
      x17 * 0.011551800896139706 +
      x16*x3 * 0.0097616095291940789 +
      x16*x5 * 0.0083903358096168155 +
      x16*x7 * 0.0073125258735988451 +
      x16*x9 * 0.0064472103118896484 +
      x16*x11 * 0.0057400376708419235 +
      x16*x13 * 0.0051533096823199042 +
      x16*x15 * 0.0046601434869150962 +
      x24*x9 * 0.0042409070936793631 +
      x24*x11 * 0.0038809645588376692 +
      x24*x13 * 0.0035692053938259345 +
      x24*x15 * 0.0032970595034734847
   );
}


float asinf(float x)
{
   return asin(x);
}


double asin(double x)
{
   union {
      double f;
      uint64_t i;
   } u;
   int neg = 0, invert = 0;

   if (x < 0.0) {
      x = -x;
      neg = 1;
   }
   if (x > 1.0) {
      u.i = (0x7FFULL << 52) | (1ULL << 51); // nan
      return u.f;
   }
   if (x > 0.5) {
      x = sqrt((1.0 - x) * 0.5);
      invert = 1;
   }
   x = asin_leibniz(x);
   if (invert) {
		x = 1.5707963267948966 - x * 2.0;
   }
   if (neg) {
      x = -x;
   }
   return x;
}


float acosf(float x)
{
   return acos(x);
}


double acos(double x)
{
   return 1.5707963267948966 - asin(x);
}


// https://en.wikipedia.org/wiki/Inverse_trigonometric_functions#Infinite_series

static double atan_leibniz(double x)
{
   double x2, x3, x4, x5, x7, x8, x9;

   x2 = x*x;
   x3 = x2*x;
   x4 = x2*x2;
   x5 = x3*x2;
   x7 = x4*x3;
   x8 = x4*x4;
   x9 = x5*x4;

   return (
      x -
      x3 * 0.33333333333333333 +
      x5 * 0.2 -
      x7 * 0.14285714285714286 +
      x9 * 0.11111111111111111 -
      x8*x3 * 0.090909090909090909 +
      x8*x5 * 0.076923076923076923 -
      x8*x7 * 0.066666666666666667 +
      x9*x8 * 0.058823529411764706 -
      x8*x8*x3 * 0.052631578947368421 +
      x8*x8*x5 * 0.047619047619047619 -
      x8*x8*x7 * 0.043478260869565217
   );
}


float atanf(float x)
{
   return atan(x);
}


// https://en.wikipedia.org/wiki/Inverse_trigonometric_functions#Arctangent_addition_formula

double atan(double x)
{
   double add;
   int neg = 0, invert = 0, adjust1 = 0, adjust2 = 0;
   
   if (x < 0.0) {
      x = -x;
      neg = 1;
   }
   if (x > 1.0) {
      x = 1.0 / x;
      invert = 1;
   }
   if (x > 0.5) {
      add = -0.54630248984379051; // tan(-0.5)
      x = (x + add) / (1.0 - x * add);
      adjust1 = 1;
   }
   if (x > 0.25) {
      add = -0.25534192122103627; // tan(-0.25)
      x = (x + add) / (1.0 - x * add);
      adjust2 = 1;
   }
   x = atan_leibniz(x);
   if (adjust2) {
      x += 0.25;
   }
   if (adjust1) {
      x += 0.5;
   }
   if (invert) {
      x = 1.5707963267948966 - x;
   }
   if (neg) {
      x = -x;
   }
   return x;
}


float atan2f(float y, float x)
{
   return atan2(y, x);
}


double atan2(double y, double x)
{
   union {
      double f;
      uint64_t i;
   } u;
   double angle;
   int ys, xs;

   u.f = y;
   ys = u.i >> 63;
   u.f = x;
   xs = u.i >> 63;

   if (x == 0.0 && y == 0.0) {
      if (xs) {
         return ys? -3.1415926535897932 : 3.1415926535897932;
      }
      else {
         return ys? -0.0 : 0.0;
      }
   }

   if (ys) {
      y = -y;
   }
   angle = atan(y / x);
   if (xs) {
      angle = 3.1415926535897932 + angle;
   }
   if (ys) {
      angle = -angle;
   }
   return angle;
}


float fabsf(float x)
{
   union {
      float f;
      uint32_t i;
   } u;
   
   u.f = x;
   u.i &= 0x7FFFFFFF;
   return u.f;
}


double fabs(double x)
{
   union {
      double f;
      uint64_t i;
   } u;
   
   u.f = x;
   u.i &= 0x7FFFFFFFFFFFFFFFULL;
   return u.f;
}


float fminf(float x, float y)
{
   return x < y? x : y;
}


double fmin(double x, double y)
{
   return x < y? x : y;
}


float fmaxf(float x, float y)
{
   return x > y? x : y;
}


double fmax(double x, double y)
{
   return x > y? x : y;
}
