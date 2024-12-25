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

// ZLIB/PNG code available at http://public-domain.advel.cz/ under CC0 license

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <float.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#endif
#ifdef __APPLE__
#include <sys/time.h>
#endif
#include "fiximage.h"

#ifdef __SSE2__
#include <xmmintrin.h>
#include <emmintrin.h>
#endif

#define MIN(a, b) ((a)<(b)? (a):(b))
#define MAX(a, b) ((a)>(b)? (a):(b))

#define FORCE_INLINE inline __attribute__((always_inline))

#define MAX_IMAGE_DIM   32768
#define MAX_RECURSION   10
#define MAX_DIST_SQR    (0.1*0.1)
#define BATCH_TILE_SIZE 256

#define REFCNT_LIMIT ((1<<30)-1)

#if defined(FIXBUILD_BINCOMPAT) && defined(__linux__)
   #if defined(__i386__)
   #elif defined(__x86_64__)
      asm(".symver memcpy,memcpy@GLIBC_2.2.5");
   #elif defined(__arm__)
   #endif
#endif

#ifdef _WIN32
#define ETIMEDOUT -1000
typedef CRITICAL_SECTION pthread_mutex_t;
typedef HANDLE pthread_cond_t;
#endif

enum {
   IMAGE_to_string_func,
   IMAGE_data,
   IMAGE_width,
   IMAGE_height,
   IMAGE_stride,
   IMAGE_SIZE
};

enum {
   PAINTER_m00,
   PAINTER_m01,
   PAINTER_m02,
   PAINTER_m10,
   PAINTER_m11,
   PAINTER_m12,
   PAINTER_type,
   PAINTER_clip_x1,
   PAINTER_clip_y1,
   PAINTER_clip_x2,
   PAINTER_clip_y2,
   PAINTER_clip_shapes,
   PAINTER_clip_count,
   PAINTER_flags,
   PAINTER_blend_table,
   PAINTER_handle,
   PAINTER_image,
   PAINTER_states,
   PAINTER_SIZE
};

enum {
   PART_MOVE_TO,
   PART_LINE_TO,
   PART_QUAD_TO,
   PART_CUBIC_TO,
   PART_CLOSE_PATH
};

enum {
   FLAGS_SUBPIXEL_RENDERING = 0x01,
   FLAGS_SUBPIXEL_REVERSED  = 0x02
};

typedef struct {
   int x1, y1, x2, y2;
} Rect;

typedef struct ImageData {
   volatile unsigned int refcnt;
   struct ImageData *parent;
   uint32_t *pixels;
   int width, height, stride;
   int own_data;
   ImageFreeFunc free_func;
   void *free_data;
   int type;
} ImageData;

typedef struct {
   union {
      struct {
         float m00, m01, m02;
         float m10, m11, m12;
      };
      float m[6];
   };
   uint32_t dx, dy;
} Transform;

typedef struct {
   uint8_t *bytecode;
   int num_inputs;
   uint32_t *inputs;
   ImageData **images;
   Transform *transforms;
   int subpixel;
} Shader;

#define POS_BLOCK_SIZE (4096-1)

typedef struct Pos {
   float x, slope, height;
   float negative;
   struct Pos *next;
} Pos;

typedef struct PosBlock {
   Pos pos[POS_BLOCK_SIZE];
   int cnt;
   struct PosBlock *next;
} PosBlock;

typedef struct {
   int x1, x2, stride;
   uint32_t *pixels;
   int type;
   uint32_t color;
   Shader shader;
} FillRectData;

typedef struct {
   Value *coords;
   Value *clip_coords;
   int coords_len;
   int clip_coords_len;
   Transform tr;
   Rect clip;
   int subpixel;
} FillShapeGeometry;

typedef struct {
   uint32_t *pixels;
   int stride;
   Rect clip;
   int clip_count;
   Pos **positions, **clip_positions;
   PosBlock *block;
   int use_shader;
   Shader shader;
   uint32_t color;
   int flags;
   uint8_t *blend_table;
   MulticoreFunc func;
} FillShapeData;

enum {
   BATCH_OP_FILL_RECT,
   BATCH_OP_FILL_SHAPE
};

typedef struct BatchOp {
   int type;
   union {
      struct {
         FillRectData data;
         int y1, y2;
      } fill_rect;
      struct {
         FillShapeData data;
         int y1, y2;
      } fill_shape;
   };
   struct BatchOp *next;
} BatchOp;

typedef struct BatchGeom {
   FillShapeGeometry sg;
   BatchOp *op;
   struct BatchGeom *next;
} BatchGeom;

typedef struct BatchTile {
   int x1, y1, x2, y2;
   BatchOp **ops;
   int cnt, cap;
   struct BatchTile *cur_next;
} BatchTile;

struct CoreThread;

typedef struct {
   ImageData *data;
   int tile_width, tile_height;
   BatchTile *tiles;
   BatchOp *ops;
   pthread_mutex_t mutex;
   pthread_cond_t *conds;
   BatchTile *cur_tiles;
   struct CoreThread **geom_threads;
   BatchGeom *geoms;
   int geom_done;
} Painter;

typedef struct {
   Heap *heap;
   Value array;
   Value data[256];
   int cnt, total_len;
} ArrayAppend;

enum {
   BC_COLOR,
   BC_SAMPLE_NEAREST,
   BC_SAMPLE_BILINEAR,
   BC_SAMPLE_BICUBIC,
   BC_COPY,
   BC_ADD,
   BC_SUB,
   BC_MUL,
   BC_MIX,
   BC_OUTPUT_BLEND,
   BC_OUTPUT_REPLACE,

   // internal:
   BC_OUTPUT_BLEND_SUBPIXEL,
   BC_OUTPUT_REPLACE_SUBPIXEL
};

enum {
   TEX_CLAMP_X = 0x01,
   TEX_CLAMP_Y = 0x02
};

#ifdef _WIN32
static FORCE_INLINE int pthread_mutex_init(pthread_mutex_t *mutex, void *attr)
{
   InitializeCriticalSection(mutex);
   return 0;
}

static FORCE_INLINE int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
   DeleteCriticalSection(mutex);
   return 0;
}

static FORCE_INLINE int pthread_mutex_lock(pthread_mutex_t *mutex)
{
   EnterCriticalSection(mutex);
   return 0;
}

static FORCE_INLINE int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
   LeaveCriticalSection(mutex);
   return 0;
}

static FORCE_INLINE int pthread_cond_init(pthread_cond_t *cond, void *attr)
{
   *cond = CreateEvent(NULL, FALSE, FALSE, NULL);
   if (!(*cond)) {
      return -1;
   }
   return 0;
}

static FORCE_INLINE int pthread_cond_destroy(pthread_cond_t *cond)
{
   CloseHandle(*cond);
   *cond = NULL;
   return 0;
}

static FORCE_INLINE int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
   LeaveCriticalSection(mutex);
   WaitForSingleObject(*cond, INFINITE);
   EnterCriticalSection(mutex);
   return 0;
}

static FORCE_INLINE int pthread_cond_timedwait_relative(pthread_cond_t *cond, pthread_mutex_t *mutex, int64_t timeout)
{
   int ret = 0;
   LeaveCriticalSection(mutex);
   if (WaitForSingleObject(*cond, timeout/1000000) == WAIT_TIMEOUT) {
      ret = ETIMEDOUT;
   }
   EnterCriticalSection(mutex);
   return ret;
}

static FORCE_INLINE int pthread_cond_signal(pthread_cond_t *cond)
{
   SetEvent(*cond);
   return 0;
}

#else

static FORCE_INLINE int pthread_cond_timedwait_relative(pthread_cond_t *cond, pthread_mutex_t *mutex, int64_t timeout)
{
   struct timespec ts;
#if defined(__APPLE__)
   struct timeval tv;
   gettimeofday(&tv, NULL);
   ts.tv_sec = tv.tv_sec;
   ts.tv_nsec = tv.tv_usec * 1000;
#else
   clock_gettime(CLOCK_REALTIME, &ts);
#endif
   ts.tv_nsec += timeout % 1000000;
   ts.tv_sec += ts.tv_nsec / 1000000 + timeout / 1000000;
   ts.tv_nsec %= 1000000;
   return pthread_cond_timedwait(cond, mutex, &ts);
}
#endif

#ifdef __SYMBIAN32__
#define __sync_add_and_fetch x__sync_add_and_fetch
static inline int x__sync_add_and_fetch(volatile int *ptr, int amount)
{
   *ptr = (*ptr) + amount;
   return *ptr;
}

#define __sync_sub_and_fetch x__sync_sub_and_fetch
static inline int x__sync_sub_and_fetch(volatile int *ptr, int amount)
{
   *ptr = (*ptr) - amount;
   return *ptr;
}

#define __sync_val_compare_and_swap(ptr,old,new) x__sync_val_compare_and_swap((void **)(ptr),old,new)
static inline void *x__sync_val_compare_and_swap(void **ptr, void *old_value, void *new_value)
{
   void *prev = *ptr;
   if (prev == old_value) {
      *ptr = new_value;
   }
   return prev;
}
#endif

#define NUM_HANDLE_TYPES 2
#define HANDLE_TYPE_IMAGE_DATA (handles_offset+0)
#define HANDLE_TYPE_PAINTER    (handles_offset+1)

static volatile int handles_offset;

typedef struct CoreThread {
   pthread_mutex_t mutex;
   pthread_cond_t cond, cond2;
   MulticoreFunc func;
   void *data;
   int from, to;
   int ack;
   struct CoreThread *next;
   char padding[128];
} CoreThread;

static volatile int multicore_num_cores;
static pthread_mutex_t *multicore_mutex;
static CoreThread *multicore_threads;

static uint32_t *load_png(const unsigned char *buf, int len, int *width, int *height);
static int save_png(const uint32_t *pixels, int stride, int width, int height, unsigned char **dest_out, int *dest_len_out);


#if defined(_WIN32)
static DWORD WINAPI thread_main(void *data)
#else
static void *thread_main(void *data)
#endif
{
   CoreThread *thread = data, *th;

   pthread_mutex_lock(&thread->mutex);
   for (;;) {
      while (!thread->func) {
         if (pthread_cond_timedwait_relative(&thread->cond, &thread->mutex, 5000*1000000LL) == ETIMEDOUT && !thread->func) {
            pthread_mutex_lock(multicore_mutex);
            if (thread == multicore_threads) {
               multicore_threads = thread->next;
               pthread_mutex_unlock(multicore_mutex);
               goto end;
            }
            for (th = multicore_threads; th; th = th->next) {
               if (th->next == thread) {
                  th->next = thread->next;
                  pthread_mutex_unlock(multicore_mutex);
                  goto end;
               }
            }
            pthread_mutex_unlock(multicore_mutex);
         }
      }
      pthread_mutex_unlock(&thread->mutex);

      thread->func(thread->from, thread->to, thread->data);

      pthread_mutex_lock(&thread->mutex);
      thread->ack = 1;
      pthread_cond_signal(&thread->cond2);

      while (thread->ack != 2) {
         pthread_cond_wait(&thread->cond, &thread->mutex);
      }
      thread->ack = 3;
      pthread_cond_signal(&thread->cond2);
   }

end:
   pthread_mutex_unlock(&thread->mutex);

   pthread_cond_destroy(&thread->cond2);
   pthread_cond_destroy(&thread->cond);
   pthread_mutex_destroy(&thread->mutex);
   free(thread);

#if defined(_WIN32)
   return 0;
#else
   return NULL;
#endif
}


static CoreThread *acquire_thread()
{
   CoreThread *thread;
#if defined(_WIN32)
   HANDLE handle;
#else
   pthread_t handle;
#endif
   int init = 0;

   if (multicore_threads) {
      thread = multicore_threads;
      multicore_threads = thread->next;
      return thread;
   }

   thread = calloc(1, sizeof(CoreThread));
   if (!thread) goto error;

   if (pthread_mutex_init(&thread->mutex, NULL) != 0) goto error;
   init = 1;

   if (pthread_cond_init(&thread->cond, NULL) != 0) goto error;
   init = 2;

   if (pthread_cond_init(&thread->cond2, NULL) != 0) goto error;
   init = 3;

#if defined(_WIN32)
   handle = CreateThread(NULL, 0, thread_main, thread, 0, NULL);
   if (!handle) {
      goto error;
   }
   CloseHandle(handle);
#else
   if (pthread_create(&handle, NULL, thread_main, thread) != 0) {
      goto error;
   }
   pthread_detach(handle);
#endif

   return thread;

error:
   if (init >= 3) pthread_cond_destroy(&thread->cond2);
   if (init >= 2) pthread_cond_destroy(&thread->cond);
   if (init >= 1) pthread_mutex_destroy(&thread->mutex);
   free(thread);
   return NULL;
}


static void release_thread(CoreThread *thread)
{
   thread->next = multicore_threads;
   multicore_threads = thread;
}


static void start_in_thread(CoreThread *thread, int from, int to, MulticoreFunc func, void *data)
{
   pthread_mutex_lock(&thread->mutex);
   thread->from = from;
   thread->to = to;
   thread->func = func;
   thread->data = data;
   thread->ack = 0;
   pthread_cond_signal(&thread->cond);
   pthread_mutex_unlock(&thread->mutex);
}


static void finish_in_thread(CoreThread *thread)
{
   pthread_mutex_lock(&thread->mutex);
   while (thread->ack != 1) {
      pthread_cond_wait(&thread->cond2, &thread->mutex);
   }
   thread->func = NULL;
   thread->data = NULL;
   thread->ack = 2;
   pthread_cond_signal(&thread->cond);
   while (thread->ack != 3) {
      pthread_cond_wait(&thread->cond2, &thread->mutex);
   }
   pthread_mutex_unlock(&thread->mutex);
}


static void dummy_multicore_func(int from, int to, void *data)
{
}


int fiximage_get_core_count()
{
   if (multicore_num_cores == 0) {
      fiximage_multicore_run(0, 1, 0, dummy_multicore_func, NULL);
   }
   return multicore_num_cores;
}


void fiximage_multicore_run(int from, int to, int min_iters, MulticoreFunc func, void *data)
{
   int i, cores, iters_per_core;
#ifdef _WIN32
   SYSTEM_INFO si;
#endif
   pthread_mutex_t *mutex, *cur_mutex;
   CoreThread *threads[64];
   int thread_from, thread_to;

   if (from >= to) return;

   if (to - from <= min_iters || multicore_num_cores == 1) {
      func(from, to, data);
      return;
   }

   mutex = multicore_mutex;
   if (!mutex) {
      mutex = calloc(1, sizeof(pthread_mutex_t));
      if (!mutex) goto failure;
      if (pthread_mutex_init(mutex, NULL) != 0) {
         free(mutex);
         goto failure;
      }
      cur_mutex = __sync_val_compare_and_swap(&multicore_mutex, NULL, mutex);
      if (cur_mutex) {
         pthread_mutex_destroy(mutex);
         free(mutex);
         mutex = cur_mutex;
      }
   }
   
   pthread_mutex_lock(mutex);

   if (multicore_num_cores == 0) {
#if defined(_WIN32)
      GetSystemInfo(&si);
      multicore_num_cores = si.dwNumberOfProcessors;
#elif defined(__EMSCRIPTEN__)
      multicore_num_cores = 1;
#else
      multicore_num_cores = sysconf(_SC_NPROCESSORS_ONLN);
#endif
      if (multicore_num_cores < 1) {
         multicore_num_cores = 1;
      }
      if (multicore_num_cores > sizeof(threads)/sizeof(CoreThread *)) {
         multicore_num_cores = sizeof(threads)/sizeof(CoreThread *);
      }
   }

   if (multicore_num_cores == 1) {
      pthread_mutex_unlock(mutex);
      func(from, to, data);
      return;
   }

   if (min_iters < 1) {
      min_iters = 1;
   }
   cores = multicore_num_cores;
   if (to - from < min_iters * cores) {
      cores = (to - from) / min_iters;
      min_iters = (to - from + cores - 1) / cores;
   }
   iters_per_core = (to - from) / cores;
   if (iters_per_core < min_iters) {
      iters_per_core = min_iters;
   }

   for (i=0; i<cores; i++) {
      threads[i] = acquire_thread();
      if (!threads[i]) {
         cores = i;
         iters_per_core = (to - from) / cores;
         break;
      }
   }

   pthread_mutex_unlock(mutex);

   if (cores == 0) {
      func(from, to, data);
      return;
   }
   
   for (i=0; i<cores; i++) {
      thread_from = from + iters_per_core * i;
      thread_to = thread_from + iters_per_core;
      if (i == cores-1 && thread_to < to) {
         thread_to = to;
      }
      if (thread_to > to) {
         thread_to = to;
      }
      start_in_thread(threads[i], thread_from, thread_to, func, data);
   }

   for (i=0; i<cores; i++) {
      finish_in_thread(threads[i]);
   }

   pthread_mutex_lock(mutex);
   for (i=cores-1; i>=0; i--) {
      release_thread(threads[i]);
   }
   pthread_mutex_unlock(mutex);
   return;

failure:
   multicore_num_cores = 1;
   func(from, to, data);
   return;
}


static FORCE_INLINE int fast_floor(float a)
{
   return (int)a - ((int)a > a);
}


static FORCE_INLINE int fast_round(float a)
{
   return a + 0.5f;
}


static FORCE_INLINE uint32_t div255(uint32_t a)
{
   return ((a << 8) + a + 255) >> 16;
}


static FORCE_INLINE uint32_t interpolate_color(uint32_t c1, uint32_t c2, uint32_t fract)
{
   uint32_t ifract = 256-fract;
   uint32_t rb = (((c1 & 0x00FF00FF)*ifract + (c2 & 0x00FF00FF)*fract) >> 8) & 0x00FF00FF;
   uint32_t ag = (((c1 >> 8) & 0x00FF00FF)*ifract + ((c2 >> 8) & 0x00FF00FF)*fract) & 0xFF00FF00;
   return rb | ag;
}


#ifdef __SSE2__
static FORCE_INLINE __m128i interpolate_color_sse2(uint32_t *r0, uint32_t *r1, uint32_t frac_x, uint32_t frac_y)
{
   __m128i c0_1, c2_3, c0, c1;

   c0_1 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)r0), _mm_setzero_si128());
   c2_3 = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i *)r1), _mm_setzero_si128());

   c0 = _mm_add_epi16(c0_1, _mm_srai_epi16(_mm_mullo_epi16(_mm_sub_epi16(c2_3, c0_1), _mm_set1_epi16(frac_y >> 1)), 7));
   c1 = _mm_unpackhi_epi64(c0, c0);

   c0 = _mm_add_epi16(c0, _mm_srai_epi16(_mm_mullo_epi16(_mm_sub_epi16(c1, c0), _mm_set1_epi16(frac_x >> 1)), 7));

   /*
   c0_1 = _mm_mullo_epi16(c0_1, _mm_set1_epi16(256 - frac_y));
   c2_3 = _mm_mullo_epi16(c2_3, _mm_set1_epi16(frac_y));

   c0 = _mm_srli_epi16(_mm_add_epi16(c0_1, c2_3), 8);
   c1 = _mm_unpackhi_epi64(c0, c0);

   c0 = _mm_mullo_epi16(c0, _mm_set1_epi16(256 - frac_x));
   c1 = _mm_mullo_epi16(c1, _mm_set1_epi16(frac_x));
   c0 = _mm_srli_epi16(_mm_add_epi16(c0, c1), 8);
   */

   return _mm_packus_epi16(c0, c0);
}
#endif /* __SSE2__ */


static int16_t bicubic_weights[1024] = {
   0x0000, 0x4040, 0x0000, 0x0000, 0xffe1, 0x403f, 0x0020, 0x0000,
   0xffc1, 0x403d, 0x0042, 0x0000, 0xffa2, 0x403a, 0x0064, 0xffff,
   0xff84, 0x4036, 0x0088, 0xffff, 0xff66, 0x4030, 0x00ac, 0xfffd,
   0xff49, 0x4029, 0x00d2, 0xfffc, 0xff2c, 0x4021, 0x00f8, 0xfffb,
   0xff0f, 0x4018, 0x0120, 0xfff9, 0xfef3, 0x400e, 0x0148, 0xfff7,
   0xfed8, 0x4002, 0x0171, 0xfff4, 0xfebd, 0x3ff6, 0x019c, 0xfff2,
   0xfea2, 0x3fe8, 0x01c7, 0xffef, 0xfe88, 0x3fd9, 0x01f3, 0xffec,
   0xfe6f, 0x3fc9, 0x0220, 0xffe9, 0xfe55, 0x3fb7, 0x024d, 0xffe6,
   0xfe3d, 0x3fa5, 0x027c, 0xffe2, 0xfe24, 0x3f91, 0x02ab, 0xffdf,
   0xfe0d, 0x3f7d, 0x02dc, 0xffdb, 0xfdf5, 0x3f67, 0x030d, 0xffd7,
   0xfdde, 0x3f50, 0x033f, 0xffd2, 0xfdc8, 0x3f38, 0x0372, 0xffce,
   0xfdb2, 0x3f1f, 0x03a6, 0xffc9, 0xfd9c, 0x3f05, 0x03da, 0xffc4,
   0xfd87, 0x3eea, 0x040f, 0xffbf, 0xfd73, 0x3ece, 0x0445, 0xffba,
   0xfd5e, 0x3eb1, 0x047c, 0xffb4, 0xfd4a, 0x3e93, 0x04b4, 0xffaf,
   0xfd37, 0x3e74, 0x04ec, 0xffa9, 0xfd24, 0x3e54, 0x0525, 0xffa3,
   0xfd11, 0x3e33, 0x055f, 0xff9d, 0xfcff, 0x3e10, 0x059a, 0xff97,
   0xfced, 0x3ded, 0x05d5, 0xff90, 0xfcdc, 0x3dc9, 0x0611, 0xff89,
   0xfccb, 0x3da4, 0x064e, 0xff83, 0xfcbb, 0x3d7e, 0x068c, 0xff7c,
   0xfcaa, 0x3d57, 0x06ca, 0xff75, 0xfc9b, 0x3d2f, 0x0709, 0xff6e,
   0xfc8b, 0x3d06, 0x0748, 0xff66, 0xfc7c, 0x3cdc, 0x0789, 0xff5f,
   0xfc6e, 0x3cb2, 0x07ca, 0xff57, 0xfc5f, 0x3c86, 0x080b, 0xff4f,
   0xfc52, 0x3c5a, 0x084d, 0xff47, 0xfc44, 0x3c2c, 0x0890, 0xff3f,
   0xfc37, 0x3bfe, 0x08d4, 0xff37, 0xfc2a, 0x3bcf, 0x0918, 0xff2f,
   0xfc1e, 0x3b9f, 0x095c, 0xff27, 0xfc12, 0x3b6e, 0x09a2, 0xff1e,
   0xfc07, 0x3b3d, 0x09e7, 0xff16, 0xfbfb, 0x3b0a, 0x0a2e, 0xff0d,
   0xfbf0, 0x3ad7, 0x0a75, 0xff04, 0xfbe6, 0x3aa3, 0x0abc, 0xfefb,
   0xfbdc, 0x3a6e, 0x0b05, 0xfef2, 0xfbd2, 0x3a38, 0x0b4d, 0xfee9,
   0xfbc8, 0x3a01, 0x0b96, 0xfee0, 0xfbbf, 0x39ca, 0x0be0, 0xfed6,
   0xfbb6, 0x3992, 0x0c2a, 0xfecd, 0xfbae, 0x3959, 0x0c75, 0xfec4,
   0xfba6, 0x3920, 0x0cc0, 0xfeba, 0xfb9e, 0x38e5, 0x0d0c, 0xfeb0,
   0xfb97, 0x38aa, 0x0d58, 0xfea7, 0xfb8f, 0x386f, 0x0da5, 0xfe9d,
   0xfb89, 0x3832, 0x0df2, 0xfe93, 0xfb82, 0x37f5, 0x0e40, 0xfe89,
   0xfb7c, 0x37b7, 0x0e8e, 0xfe7f, 0xfb76, 0x3778, 0x0edd, 0xfe75,
   0xfb71, 0x3739, 0x0f2b, 0xfe6b, 0xfb6b, 0x36f9, 0x0f7b, 0xfe61,
   0xfb66, 0x36b9, 0x0fcb, 0xfe56, 0xfb62, 0x3677, 0x101b, 0xfe4c,
   0xfb5d, 0x3635, 0x106b, 0xfe42, 0xfb59, 0x35f3, 0x10bc, 0xfe37,
   0xfb56, 0x35b0, 0x110e, 0xfe2d, 0xfb52, 0x356c, 0x115f, 0xfe22,
   0xfb4f, 0x3528, 0x11b2, 0xfe18, 0xfb4c, 0x34e3, 0x1204, 0xfe0d,
   0xfb49, 0x349d, 0x1257, 0xfe03, 0xfb47, 0x3457, 0x12aa, 0xfdf8,
   0xfb45, 0x3410, 0x12fd, 0xfdee, 0xfb43, 0x33c9, 0x1351, 0xfde3,
   0xfb42, 0x3381, 0x13a5, 0xfdd8, 0xfb41, 0x3338, 0x13f9, 0xfdce,
   0xfb40, 0x32ef, 0x144e, 0xfdc3, 0xfb3f, 0x32a6, 0x14a3, 0xfdb8,
   0xfb3e, 0x325c, 0x14f8, 0xfdae, 0xfb3e, 0x3211, 0x154e, 0xfda3,
   0xfb3e, 0x31c6, 0x15a3, 0xfd98, 0xfb3e, 0x317b, 0x15f9, 0xfd8d,
   0xfb3f, 0x312f, 0x164f, 0xfd83, 0xfb40, 0x30e2, 0x16a6, 0xfd78,
   0xfb41, 0x3095, 0x16fd, 0xfd6d, 0xfb42, 0x3048, 0x1753, 0xfd63,
   0xfb44, 0x2ffa, 0x17aa, 0xfd58, 0xfb45, 0x2fac, 0x1802, 0xfd4d,
   0xfb47, 0x2f5d, 0x1859, 0xfd43, 0xfb49, 0x2f0e, 0x18b1, 0xfd38,
   0xfb4c, 0x2ebe, 0x1908, 0xfd2e, 0xfb4e, 0x2e6e, 0x1960, 0xfd23,
   0xfb51, 0x2e1e, 0x19b8, 0xfd19, 0xfb54, 0x2dcd, 0x1a11, 0xfd0e,
   0xfb58, 0x2d7c, 0x1a69, 0xfd04, 0xfb5b, 0x2d2a, 0x1ac1, 0xfcf9,
   0xfb5f, 0x2cd8, 0x1b1a, 0xfcef, 0xfb63, 0x2c86, 0x1b73, 0xfce5,
   0xfb67, 0x2c33, 0x1bcb, 0xfcdb, 0xfb6b, 0x2be0, 0x1c24, 0xfcd0,
   0xfb6f, 0x2b8d, 0x1c7d, 0xfcc6, 0xfb74, 0x2b39, 0x1cd6, 0xfcbc,
   0xfb79, 0x2ae6, 0x1d2f, 0xfcb2, 0xfb7e, 0x2a91, 0x1d88, 0xfca8,
   0xfb83, 0x2a3d, 0x1de2, 0xfc9f, 0xfb89, 0x29e8, 0x1e3b, 0xfc95,
   0xfb8e, 0x2993, 0x1e94, 0xfc8b, 0xfb94, 0x293e, 0x1eed, 0xfc81,
   0xfb9a, 0x28e8, 0x1f46, 0xfc78, 0xfba0, 0x2892, 0x1fa0, 0xfc6e,
   0xfba6, 0x283c, 0x1ff9, 0xfc65, 0xfbac, 0x27e6, 0x2052, 0xfc5c,
   0xfbb3, 0x278f, 0x20ab, 0xfc53, 0xfbba, 0x2738, 0x2104, 0xfc4a,
   0xfbc1, 0x26e1, 0x215d, 0xfc41, 0xfbc8, 0x268a, 0x21b7, 0xfc38,
   0xfbcf, 0x2633, 0x220f, 0xfc2f, 0xfbd6, 0x25db, 0x2268, 0xfc26,
   0xfbdd, 0x2584, 0x22c1, 0xfc1e, 0xfbe5, 0x252c, 0x231a, 0xfc15,
   0xfbed, 0x24d4, 0x2373, 0xfc0d, 0xfbf5, 0x247c, 0x23cb, 0xfc05,
   0xfbfc, 0x2424, 0x2424, 0xfbfc, 0xfc05, 0x23cb, 0x247c, 0xfbf5,
   0xfc0d, 0x2373, 0x24d4, 0xfbed, 0xfc15, 0x231a, 0x252c, 0xfbe5,
   0xfc1e, 0x22c1, 0x2584, 0xfbdd, 0xfc26, 0x2268, 0x25db, 0xfbd6,
   0xfc2f, 0x220f, 0x2633, 0xfbcf, 0xfc38, 0x21b7, 0x268a, 0xfbc8,
   0xfc41, 0x215d, 0x26e1, 0xfbc1, 0xfc4a, 0x2104, 0x2738, 0xfbba,
   0xfc53, 0x20ab, 0x278f, 0xfbb3, 0xfc5c, 0x2052, 0x27e6, 0xfbac,
   0xfc65, 0x1ff9, 0x283c, 0xfba6, 0xfc6e, 0x1fa0, 0x2892, 0xfba0,
   0xfc78, 0x1f46, 0x28e8, 0xfb9a, 0xfc81, 0x1eed, 0x293e, 0xfb94,
   0xfc8b, 0x1e94, 0x2993, 0xfb8e, 0xfc95, 0x1e3b, 0x29e8, 0xfb89,
   0xfc9f, 0x1de2, 0x2a3d, 0xfb83, 0xfca8, 0x1d88, 0x2a91, 0xfb7e,
   0xfcb2, 0x1d2f, 0x2ae6, 0xfb79, 0xfcbc, 0x1cd6, 0x2b39, 0xfb74,
   0xfcc6, 0x1c7d, 0x2b8d, 0xfb6f, 0xfcd0, 0x1c24, 0x2be0, 0xfb6b,
   0xfcdb, 0x1bcb, 0x2c33, 0xfb67, 0xfce5, 0x1b73, 0x2c86, 0xfb63,
   0xfcef, 0x1b1a, 0x2cd8, 0xfb5f, 0xfcf9, 0x1ac1, 0x2d2a, 0xfb5b,
   0xfd04, 0x1a69, 0x2d7c, 0xfb58, 0xfd0e, 0x1a11, 0x2dcd, 0xfb54,
   0xfd19, 0x19b8, 0x2e1e, 0xfb51, 0xfd23, 0x1960, 0x2e6e, 0xfb4e,
   0xfd2e, 0x1908, 0x2ebe, 0xfb4c, 0xfd38, 0x18b1, 0x2f0e, 0xfb49,
   0xfd43, 0x1859, 0x2f5d, 0xfb47, 0xfd4d, 0x1802, 0x2fac, 0xfb45,
   0xfd58, 0x17aa, 0x2ffa, 0xfb44, 0xfd63, 0x1753, 0x3048, 0xfb42,
   0xfd6d, 0x16fd, 0x3095, 0xfb41, 0xfd78, 0x16a6, 0x30e2, 0xfb40,
   0xfd83, 0x164f, 0x312f, 0xfb3f, 0xfd8d, 0x15f9, 0x317b, 0xfb3e,
   0xfd98, 0x15a3, 0x31c6, 0xfb3e, 0xfda3, 0x154e, 0x3211, 0xfb3e,
   0xfdae, 0x14f8, 0x325c, 0xfb3e, 0xfdb8, 0x14a3, 0x32a6, 0xfb3f,
   0xfdc3, 0x144e, 0x32ef, 0xfb40, 0xfdce, 0x13f9, 0x3338, 0xfb41,
   0xfdd8, 0x13a5, 0x3381, 0xfb42, 0xfde3, 0x1351, 0x33c9, 0xfb43,
   0xfdee, 0x12fd, 0x3410, 0xfb45, 0xfdf8, 0x12aa, 0x3457, 0xfb47,
   0xfe03, 0x1257, 0x349d, 0xfb49, 0xfe0d, 0x1204, 0x34e3, 0xfb4c,
   0xfe18, 0x11b2, 0x3528, 0xfb4f, 0xfe22, 0x115f, 0x356c, 0xfb52,
   0xfe2d, 0x110e, 0x35b0, 0xfb56, 0xfe37, 0x10bc, 0x35f3, 0xfb59,
   0xfe42, 0x106b, 0x3635, 0xfb5d, 0xfe4c, 0x101b, 0x3677, 0xfb62,
   0xfe56, 0x0fcb, 0x36b9, 0xfb66, 0xfe61, 0x0f7b, 0x36f9, 0xfb6b,
   0xfe6b, 0x0f2b, 0x3739, 0xfb71, 0xfe75, 0x0edd, 0x3778, 0xfb76,
   0xfe7f, 0x0e8e, 0x37b7, 0xfb7c, 0xfe89, 0x0e40, 0x37f5, 0xfb82,
   0xfe93, 0x0df2, 0x3832, 0xfb89, 0xfe9d, 0x0da5, 0x386f, 0xfb8f,
   0xfea7, 0x0d58, 0x38aa, 0xfb97, 0xfeb0, 0x0d0c, 0x38e5, 0xfb9e,
   0xfeba, 0x0cc0, 0x3920, 0xfba6, 0xfec4, 0x0c75, 0x3959, 0xfbae,
   0xfecd, 0x0c2a, 0x3992, 0xfbb6, 0xfed6, 0x0be0, 0x39ca, 0xfbbf,
   0xfee0, 0x0b96, 0x3a01, 0xfbc8, 0xfee9, 0x0b4d, 0x3a38, 0xfbd2,
   0xfef2, 0x0b05, 0x3a6e, 0xfbdc, 0xfefb, 0x0abc, 0x3aa3, 0xfbe6,
   0xff04, 0x0a75, 0x3ad7, 0xfbf0, 0xff0d, 0x0a2e, 0x3b0a, 0xfbfb,
   0xff16, 0x09e7, 0x3b3d, 0xfc07, 0xff1e, 0x09a2, 0x3b6e, 0xfc12,
   0xff27, 0x095c, 0x3b9f, 0xfc1e, 0xff2f, 0x0918, 0x3bcf, 0xfc2a,
   0xff37, 0x08d4, 0x3bfe, 0xfc37, 0xff3f, 0x0890, 0x3c2c, 0xfc44,
   0xff47, 0x084d, 0x3c5a, 0xfc52, 0xff4f, 0x080b, 0x3c86, 0xfc5f,
   0xff57, 0x07ca, 0x3cb2, 0xfc6e, 0xff5f, 0x0789, 0x3cdc, 0xfc7c,
   0xff66, 0x0748, 0x3d06, 0xfc8b, 0xff6e, 0x0709, 0x3d2f, 0xfc9b,
   0xff75, 0x06ca, 0x3d57, 0xfcaa, 0xff7c, 0x068c, 0x3d7e, 0xfcbb,
   0xff83, 0x064e, 0x3da4, 0xfccb, 0xff89, 0x0611, 0x3dc9, 0xfcdc,
   0xff90, 0x05d5, 0x3ded, 0xfced, 0xff97, 0x059a, 0x3e10, 0xfcff,
   0xff9d, 0x055f, 0x3e33, 0xfd11, 0xffa3, 0x0525, 0x3e54, 0xfd24,
   0xffa9, 0x04ec, 0x3e74, 0xfd37, 0xffaf, 0x04b4, 0x3e93, 0xfd4a,
   0xffb4, 0x047c, 0x3eb1, 0xfd5e, 0xffba, 0x0445, 0x3ece, 0xfd73,
   0xffbf, 0x040f, 0x3eea, 0xfd87, 0xffc4, 0x03da, 0x3f05, 0xfd9c,
   0xffc9, 0x03a6, 0x3f1f, 0xfdb2, 0xffce, 0x0372, 0x3f38, 0xfdc8,
   0xffd2, 0x033f, 0x3f50, 0xfdde, 0xffd7, 0x030d, 0x3f67, 0xfdf5,
   0xffdb, 0x02dc, 0x3f7d, 0xfe0d, 0xffdf, 0x02ab, 0x3f91, 0xfe24,
   0xffe2, 0x027c, 0x3fa5, 0xfe3d, 0xffe6, 0x024d, 0x3fb7, 0xfe55,
   0xffe9, 0x0220, 0x3fc9, 0xfe6f, 0xffec, 0x01f3, 0x3fd9, 0xfe88,
   0xffef, 0x01c7, 0x3fe8, 0xfea2, 0xfff2, 0x019c, 0x3ff6, 0xfebd,
   0xfff4, 0x0171, 0x4002, 0xfed8, 0xfff7, 0x0148, 0x400e, 0xfef3,
   0xfff9, 0x0120, 0x4018, 0xff0f, 0xfffb, 0x00f8, 0x4021, 0xff2c,
   0xfffc, 0x00d2, 0x4029, 0xff49, 0xfffd, 0x00ac, 0x4030, 0xff66,
   0xffff, 0x0088, 0x4036, 0xff84, 0xffff, 0x0064, 0x403a, 0xffa2,
   0x0000, 0x0042, 0x403d, 0xffc1, 0x0000, 0x0020, 0x403f, 0xffe1
};

static FORCE_INLINE uint8_t interpolate_bicubic(uint8_t c0, uint8_t c1, uint8_t c2, uint8_t c3, int fract)
{
   int16_t f0 = bicubic_weights[fract*4+0];
   int16_t f1 = bicubic_weights[fract*4+1];
   int16_t f2 = bicubic_weights[fract*4+2];
   int16_t f3 = bicubic_weights[fract*4+3];
   int result = (f0*c0 + f1*c1 + f2*c2 + f3*c3) >> 14;
   if (result < 0) result = 0;
   if (result > 255) result = 255;
   return result;
}


#if 0
static void precompute_bicubic()
{
   int i;
   
   printf("static int16_t bicubic_weights[1024] = {\n   ");
   for (i=0; i<256; i++) {
      float x = i/256.0f;
      float w0 = -0.5*x + 0.5*x*x*2.0 - 0.5*x*x*x;
      float w1 = 1.0 - 0.5*x*x*5.0 + 0.5*x*x*x*3.0;
      float w2 = 0.5*x + 0.5*x*x*4.0 - 0.5*x*x*x*3.0;
      float w3 = -0.5*x*x + 0.5*x*x*x;
      int f0 = w0 * (16384.0f+64.0f);
      int f1 = w1 * (16384.0f+64.0f);
      int f2 = w2 * (16384.0f+64.0f);
      int f3 = w3 * (16384.0f+64.0f);
      printf("0x%04x, 0x%04x, 0x%04x, 0x%04x", f0 & 0xFFFF, f1 & 0xFFFF, f2 & 0xFFFF, f3 & 0xFFFF);
      if (i == 255) {
         printf("\n");
      }
      else {
         if (i % 2 == 1) {
            printf(",\n   ");
         }
         else {
            printf(", ");
         }
      }
   }
   printf("};\n");
}
#endif


static FORCE_INLINE uint32_t interpolate_color_bicubic(uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4, uint32_t fract)
{
   int a1 = (c1 >> 24) & 0xFF;
   int r1 = (c1 >> 16) & 0xFF;
   int g1 = (c1 >>  8) & 0xFF;
   int b1 = (c1 >>  0) & 0xFF;

   int a2 = (c2 >> 24) & 0xFF;
   int r2 = (c2 >> 16) & 0xFF;
   int g2 = (c2 >>  8) & 0xFF;
   int b2 = (c2 >>  0) & 0xFF;
   
   int a3 = (c3 >> 24) & 0xFF;
   int r3 = (c3 >> 16) & 0xFF;
   int g3 = (c3 >>  8) & 0xFF;
   int b3 = (c3 >>  0) & 0xFF;

   int a4 = (c4 >> 24) & 0xFF;
   int r4 = (c4 >> 16) & 0xFF;
   int g4 = (c4 >>  8) & 0xFF;
   int b4 = (c4 >>  0) & 0xFF;

   int a = interpolate_bicubic(a1, a2, a3, a4, fract);
   int r = interpolate_bicubic(r1, r2, r3, r4, fract);
   int g = interpolate_bicubic(g1, g2, g3, g4, fract);
   int b = interpolate_bicubic(b1, b2, b3, b4, fract);
   return (a << 24) | (r << 16) | (g << 8) | b;
}


#ifdef __SSE2__
static FORCE_INLINE __m128i interpolate_color_bicubic_sse2(__m128i c0_1, __m128i c2_3, uint32_t fract)
{
   __m128i weights, weights0_1, weights2_3, tmp0, tmp1;

   weights = _mm_loadl_epi64((__m128i *)&bicubic_weights[fract*4]);
   weights = _mm_unpacklo_epi16(weights, weights);

   weights0_1 = _mm_unpacklo_epi32(weights, weights);
   weights2_3 = _mm_unpackhi_epi32(weights, weights);

   c0_1 = _mm_slli_epi16(_mm_unpacklo_epi8(c0_1, _mm_setzero_si128()), 2);
   c0_1 = _mm_add_epi16(c0_1, _mm_set1_epi16(5));
   c0_1 = _mm_mulhi_epi16(c0_1, weights0_1);

   c2_3 = _mm_slli_epi16(_mm_unpacklo_epi8(c2_3, _mm_setzero_si128()), 2);
   c2_3 = _mm_add_epi16(c2_3, _mm_set1_epi16(5));
   c2_3 = _mm_mulhi_epi16(c2_3, weights2_3);

   tmp0 = _mm_add_epi16(c0_1, c2_3);
   tmp1 = _mm_unpackhi_epi64(tmp0, tmp0);
   tmp0 = _mm_add_epi16(tmp0, tmp1);
   
   return _mm_packus_epi16(tmp0, tmp0);
}
#endif /* __SSE2__ */


static FORCE_INLINE void rect_translate(Rect *rect, int off_x, int off_y)
{
   rect->x1 += off_x;
   rect->y1 += off_y;
   rect->x2 += off_x;
   rect->y2 += off_y;
}


static FORCE_INLINE int rect_clip(Rect *rect, Rect *clip)
{
   rect->x1 = MAX(rect->x1, clip->x1);
   rect->y1 = MAX(rect->y1, clip->y1);
   rect->x2 = MIN(rect->x2, clip->x2);
   rect->y2 = MIN(rect->y2, clip->y2);
   return (rect->x1 < rect->x2 && rect->y1 < rect->y2);
}


static FORCE_INLINE float transform_x(Transform *tr, float x, float y)
{
   return x * tr->m00 + y * tr->m01 + tr->m02;
}


static FORCE_INLINE float transform_y(Transform *tr, float x, float y)
{
   return x * tr->m10 + y * tr->m11 + tr->m12;
}


static void free_image_data(void *user_data)
{
   ImageData *data = user_data;
   
   if (data->refcnt < REFCNT_LIMIT && __sync_sub_and_fetch(&data->refcnt, 1) == 0) {
      if (data->parent) {
         free_image_data(data->parent);
      }
      if (data->own_data) {
         free(data->pixels);
      }
      if (data->free_func) {
         data->free_func(data->free_data);
      }
      free(data);
   }
}


static void shader_unref_data(Shader *shader);
static void free_shader(Shader *shader);
static void free_fill_shape_data(FillShapeData *fs);

static void free_batch_op(BatchOp *op)
{
   if (op->type == BATCH_OP_FILL_RECT) {
      if (op->fill_rect.data.type == 2) {
         shader_unref_data(&op->fill_rect.data.shader);
         free_shader(&op->fill_rect.data.shader);
      }
   }
   else if (op->type == BATCH_OP_FILL_SHAPE) {
      if (op->fill_shape.data.use_shader) {
         shader_unref_data(&op->fill_shape.data.shader);
      }
      free_fill_shape_data(&op->fill_shape.data);
   }
   free(op);
}


static void free_painter(void *ptr)
{
   Painter *p = ptr;
   BatchOp *op, *next_op;
   int i;

   if (p->tiles) {
      if (p->geom_threads) {
         pthread_mutex_lock(&p->mutex);
         p->geom_done = 1;
         for (i=0; i<multicore_num_cores; i++) {
            pthread_cond_signal(&p->conds[i]);
         }
         pthread_mutex_unlock(&p->mutex);

         for (i=0; i<multicore_num_cores; i++) {
            finish_in_thread(p->geom_threads[i]);
            release_thread(p->geom_threads[i]);
         }
         free(p->geom_threads);
      }
      for (i=0; i<p->tile_width*p->tile_height; i++) {
         free(p->tiles[i].ops);
      }
      free(p->tiles);
      for (i=0; i<multicore_num_cores; i++) {
         pthread_cond_destroy(&p->conds[i]);
      }
      pthread_mutex_destroy(&p->mutex);
   }
   op = p->ops;
   while (op) {
      next_op = op->next;
      free_batch_op(op);
      op = next_op;
   }
   free_image_data(p->data);
   free(p);
}


static ImageData *get_image_data(Heap *heap, Value *error, Value value);

static int get_transform(Transform *tr, Heap *heap, Value value, Transform *base_tr, int invert)
{
   Value values[6];
   int i, err;
   float r00, r01, r02, r10, r11, r12, invdet;

   err = fixscript_get_array_range(heap, value, 0, 6, values);
   if (err) return 0;

   for (i=0; i<6; i++) {
      if (!fixscript_is_float(values[i])) return 0;
      tr->m[i] = fixscript_get_float(values[i]);
   }

   if (base_tr) {
      r00 = base_tr->m00 * tr->m00 + base_tr->m01 * tr->m10;
      r01 = base_tr->m00 * tr->m01 + base_tr->m01 * tr->m11;
      r02 = base_tr->m00 * tr->m02 + base_tr->m01 * tr->m12 + base_tr->m02;
      r10 = base_tr->m10 * tr->m00 + base_tr->m11 * tr->m10;
      r11 = base_tr->m10 * tr->m01 + base_tr->m11 * tr->m11;
      r12 = base_tr->m10 * tr->m02 + base_tr->m11 * tr->m12 + base_tr->m12;
      tr->m00 = r00;
      tr->m01 = r01;
      tr->m02 = r02;
      tr->m10 = r10;
      tr->m11 = r11;
      tr->m12 = r12;
   }

   if (invert) {
      r00 = tr->m11;
      r01 = -tr->m01;
      r10 = -tr->m10;
      r11 = tr->m00;
      invdet = 1.0 / (tr->m00*tr->m11 - tr->m01*tr->m10);
      r00 *= invdet;
      r01 *= invdet;
      r10 *= invdet;
      r11 *= invdet;
      r02 = -tr->m02*r00 - tr->m12*r01;
      r12 = -tr->m02*r10 - tr->m12*r11;
      tr->m00 = r00;
      tr->m01 = r01;
      tr->m10 = r10;
      tr->m11 = r11;
      tr->m02 = r02;
      tr->m12 = r12;
   }

   tr->dx = (int)(tr->m00 * 65536.0f);
   tr->dy = (int)(tr->m10 * 65536.0f);
   return 1;
}


static int init_shader(Shader *shader, Heap *heap, Value shader_val, Value inputs_val, Transform *tr, int subpixel)
{
   #define MARK_REG(reg) written_regs[(reg) >> 5] |= 1 << ((reg)&31)
   #define VALID_REG(reg) (written_regs[(reg) >> 5] & (1 << ((reg)&31)))

   Value *values = NULL, error;
   int i, err, len, idx, val, dest_reg, src1_reg, src2_reg, flags, has_output=0;
   int retval = 0;
   uint32_t written_regs[8];

   memset(written_regs, 0, sizeof(written_regs));

   err = fixscript_get_array_length(heap, shader_val, &len);
   if (err || len == 0) goto error;

   err = fixscript_get_array_length(heap, inputs_val, &shader->num_inputs);
   if (err) goto error;

   values = malloc(shader->num_inputs * sizeof(Value));
   shader->bytecode = malloc(len);
   shader->inputs = malloc(shader->num_inputs * sizeof(uint32_t));
   shader->images = calloc(shader->num_inputs, sizeof(ImageData *));
   shader->transforms = malloc(shader->num_inputs * sizeof(Transform));
   shader->subpixel = subpixel;
   if (!values || !shader->bytecode || !shader->inputs || !shader->images || !shader->transforms) goto error;

   err = fixscript_get_array_bytes(heap, shader_val, 0, len, (char *)shader->bytecode);
   if (err) goto error;

   err = fixscript_get_array_range(heap, inputs_val, 0, shader->num_inputs, values);
   if (err) goto error;

   for (i=0; i<len; i++) {
      switch (shader->bytecode[i]) {
         case BC_COLOR:
            if (i+2 >= len) goto error;
            dest_reg = shader->bytecode[++i];
            idx = shader->bytecode[++i];
            MARK_REG(dest_reg);
            if (idx >= shader->num_inputs) goto error;
            shader->inputs[idx] = values[idx].value;
            break;

         case BC_SAMPLE_NEAREST:
         case BC_SAMPLE_BILINEAR:
         case BC_SAMPLE_BICUBIC:
            if (i+3 >= len) goto error;
            dest_reg = shader->bytecode[++i];

            idx = shader->bytecode[++i];
            if (idx >= shader->num_inputs) goto error;
            shader->images[idx] = get_image_data(heap, &error, values[idx]);
            if (!shader->images[idx]) goto error;

            idx = shader->bytecode[++i];
            if (idx >= shader->num_inputs) goto error;
            if (!get_transform(&shader->transforms[idx], heap, values[idx], tr, 1)) goto error;

            flags = shader->bytecode[++i];
            (void)flags;

            MARK_REG(dest_reg);
            break;

         case BC_COPY:
            if (i+2 >= len) goto error;
            dest_reg = shader->bytecode[++i];
            src1_reg = shader->bytecode[++i];
            if (!VALID_REG(src1_reg)) goto error;
            MARK_REG(dest_reg);
            break;

         case BC_ADD:
         case BC_SUB:
         case BC_MUL:
            if (i+3 >= len) goto error;
            dest_reg = shader->bytecode[++i];
            src1_reg = shader->bytecode[++i];
            src2_reg = shader->bytecode[++i];
            if (!VALID_REG(src1_reg) || !VALID_REG(src2_reg)) goto error;
            MARK_REG(dest_reg);
            break;

         case BC_MIX:
            if (i+4 >= len) goto error;
            dest_reg = shader->bytecode[++i];
            src1_reg = shader->bytecode[++i];
            src2_reg = shader->bytecode[++i];
            if (!VALID_REG(src1_reg) || !VALID_REG(src2_reg)) goto error;
            idx = shader->bytecode[++i];
            if (idx >= shader->num_inputs) goto error;
            val = fixscript_get_float(values[idx]) * 256.0f;
            if (val < 0) val = 0;
            if (val > 256) val = 256;
            shader->inputs[idx] = val;
            MARK_REG(dest_reg);
            break;

         case BC_OUTPUT_BLEND:
         case BC_OUTPUT_REPLACE:
            if (i+1 >= len) goto error;
            if (subpixel) {
               shader->bytecode[i] += BC_OUTPUT_BLEND_SUBPIXEL - BC_OUTPUT_BLEND;
            }
            src1_reg = shader->bytecode[++i];
            if (!VALID_REG(src1_reg)) goto error;
            if (i != len-1) goto error;
            has_output = 1;
            break;

         default:
            goto error;
      }
   }

   if (!has_output) {
      goto error;
   }

   retval = 1;

error:
   free(values);
   return retval;

   #undef MARK_REG
   #undef VALID_REG
}


static void free_shader(Shader *shader)
{
   free(shader->bytecode);
   free(shader->inputs);
   free(shader->images);
   free(shader->transforms);
}


static void shader_ref_data(Shader *shader)
{
   int i;

   for (i=0; i<shader->num_inputs; i++) {
      if (shader->images[i] && shader->images[i]->refcnt < REFCNT_LIMIT) {
         __sync_add_and_fetch(&shader->images[i]->refcnt, 1);
      }
   }
}


static void shader_unref_data(Shader *shader)
{
   int i;

   for (i=0; i<shader->num_inputs; i++) {
      if (shader->images[i]) {
         free_image_data(shader->images[i]);
      }
   }
}


static void run_shader(Shader *shader, uint32_t *dest, uint8_t *coverage, int len, int sx, int sy, uint8_t *blend_table)
{
   #define RUN_LENGTH 32

   static void *dispatch[13] = {
      &&op_color,
      &&op_sample_nearest,
      &&op_sample_bilinear,
      &&op_sample_bicubic,
      &&op_copy,
      &&op_add,
      &&op_sub,
      &&op_mul,
      &&op_mix,
      &&op_output_blend,
      &&op_output_replace,
      &&op_output_blend_subpixel,
      &&op_output_replace_subpixel
   };
   
   uint32_t pixel, color;
   int pa, pr, pg, pb;
   int ca, cr, cg, cb, inv_ca;
   int i, c;
   int amount;
   union {
      uint8_t u8[4*RUN_LENGTH];
      uint32_t u32[RUN_LENGTH];
   #ifdef _WIN32
   // TODO: appears to be some mingw-w64 bug or windows related issue
   } *regs = alloca(4*RUN_LENGTH*256);
   #else
   } regs[256];
   #endif

   #define DISPATCH() goto *dispatch[bc = *bytecode++];

   uint8_t *bytecode;
   unsigned char bc;

   while (len > 0) {
      bytecode = shader->bytecode;
      amount = MIN(RUN_LENGTH, len);

      DISPATCH();
      for (;;) {
         op_color: {
            uint32_t *rdest = regs[*bytecode++].u32;
            color = shader->inputs[*bytecode++];
            for (i=0; i<amount; i++) {
               *rdest++ = color;
            }
            DISPATCH();
         }

         op_sample_nearest: {
            uint32_t *rdest = regs[*bytecode++].u32;
            ImageData *img = shader->images[*bytecode++];
            Transform *tr = &shader->transforms[*bytecode++];
            int flags = *bytecode++;
            float fx = transform_x(tr, sx+0.5f, sy+0.5f);
            float fy = transform_y(tr, sx+0.5f, sy+0.5f);
            if ((flags & TEX_CLAMP_X) == 0) {
               fx /= img->width;
               fx = (fx - fast_floor(fx)) * img->width;
            }
            if ((flags & TEX_CLAMP_Y) == 0) {
               fy /= img->height;
               fy = (fy - fast_floor(fy)) * img->height;
            }
            int dx = tr->dx;
            int dy = tr->dy;
            int tx = (int)(fx * 65536.0f) - dx;
            int ty = (int)(fy * 65536.0f) - dy;
            for (i=0; i<amount; i++) {
               tx += dx;
               ty += dy;
               if ((flags & TEX_CLAMP_X) == 0) {
                  while (tx < 0) tx += img->width << 16;
                  while (tx >= (img->width<<16)) tx -= img->width << 16;
               }
               if ((flags & TEX_CLAMP_Y) == 0) {
                  while (ty < 0) ty += img->height << 16;
                  while (ty >= (img->height<<16)) ty -= img->height << 16;
               }
               int px = tx >> 16;
               int py = ty >> 16;
               if (flags & TEX_CLAMP_X) {
                  if (px < 0) px = 0;
                  if (px > img->width-1) px = img->width-1;
               }
               if (flags & TEX_CLAMP_Y) {
                  if (py < 0) py = 0;
                  if (py > img->height-1) py = img->height-1;
               }
               *rdest++ = img->pixels[py * img->stride + px];
            }
            DISPATCH();
         }

         op_sample_bilinear: {
            uint32_t *rdest = regs[*bytecode++].u32;
            ImageData *img = shader->images[*bytecode++];
            Transform *tr = &shader->transforms[*bytecode++];
            int flags = *bytecode++;
            float fx = transform_x(tr, sx+0.5f, sy+0.5f) - 0.5f;
            float fy = transform_y(tr, sx+0.5f, sy+0.5f) - 0.5f;
            if ((flags & TEX_CLAMP_X) == 0) {
               fx /= img->width;
               fx = (fx - fast_floor(fx)) * img->width;
            }
            if ((flags & TEX_CLAMP_Y) == 0) {
               fy /= img->height;
               fy = (fy - fast_floor(fy)) * img->height;
            }
            int dx = tr->dx;
            int dy = tr->dy;
            int tx = (int)(fx * 65536.0f) - dx;
            int ty = (int)(fy * 65536.0f) - dy;
            for (i=0; i<amount; i++) {
               tx += dx;
               ty += dy;
               if ((flags & TEX_CLAMP_X) == 0) {
                  while (tx < 0) tx += img->width << 16;
                  while (tx >= (img->width<<16)) tx -= img->width << 16;
               }
               if ((flags & TEX_CLAMP_Y) == 0) {
                  while (ty < 0) ty += img->height << 16;
                  while (ty >= (img->height<<16)) ty -= img->height << 16;
               }
               int px = tx >> 16;
               int py = ty >> 16;
               uint32_t frac_x = (tx >> 8) & 0xFF;
               uint32_t frac_y = (ty >> 8) & 0xFF;
               if ((flags & TEX_CLAMP_X) && px < 0) { px = 0; frac_x = 0; }
               if ((flags & TEX_CLAMP_Y) && py < 0) { py = 0; frac_y = 0; }
               #ifdef __SSE2__
               if (px >= 0 && py >= 0 && px+1 < img->width && py+1 < img->height) {
                  uint32_t *p = &img->pixels[py * img->stride + px];
                  union {
                     __m128i m128;
                     uint32_t u32[4];
                  } u;
                  u.m128 = interpolate_color_sse2(p, p + img->stride, frac_x, frac_y);
                  *rdest++ = u.u32[0];
               }
               else
               #endif
               {
                  uint32_t c0, c1, c2, c3;
                  uint32_t px2 = px+1;
                  uint32_t py2 = py+1;
                  if ((flags & TEX_CLAMP_X) && px > img->width-1) { px = img->width-1; frac_x = 0; }
                  if ((flags & TEX_CLAMP_Y) && py > img->height-1) { py = img->height-1; frac_y = 0; }
                  if (px2 >= img->width) px2 = flags & TEX_CLAMP_X? img->width-1 : 0;
                  if (py2 >= img->height) py2 = flags & TEX_CLAMP_Y? img->height-1 : 0;
                  c0 = img->pixels[py * img->stride + px];
                  c1 = img->pixels[py * img->stride + px2];
                  c2 = img->pixels[py2 * img->stride + px];
                  c3 = img->pixels[py2 * img->stride + px2];
                  *rdest++ = interpolate_color(
                     interpolate_color(c0, c1, frac_x),
                     interpolate_color(c2, c3, frac_x),
                     frac_y
                  );
               }
            }
            DISPATCH();
         }

         op_sample_bicubic: {
            uint32_t *rdest = regs[*bytecode++].u32;
            ImageData *img = shader->images[*bytecode++];
            Transform *tr = &shader->transforms[*bytecode++];
            int flags = *bytecode++;
            float fx = transform_x(tr, sx+0.5f, sy+0.5f) - 0.5f;
            float fy = transform_y(tr, sx+0.5f, sy+0.5f) - 0.5f;
            if ((flags & TEX_CLAMP_X) == 0) {
               fx /= img->width;
               fx = (fx - fast_floor(fx)) * img->width;
            }
            if ((flags & TEX_CLAMP_Y) == 0) {
               fy /= img->height;
               fy = (fy - fast_floor(fy)) * img->height;
            }
            int dx = tr->dx;
            int dy = tr->dy;
            int tx = (int)(fx * 65536.0f) - dx;
            int ty = (int)(fy * 65536.0f) - dy;
            for (i=0; i<amount; i++) {
               tx += dx;
               ty += dy;
               if ((flags & TEX_CLAMP_X) == 0) {
                  while (tx < 0) tx += img->width << 16;
                  while (tx >= (img->width<<16)) tx -= img->width << 16;
               }
               if ((flags & TEX_CLAMP_Y) == 0) {
                  while (ty < 0) ty += img->height << 16;
                  while (ty >= (img->height<<16)) ty -= img->height << 16;
               }
               int px1 = tx >> 16;
               int py1 = ty >> 16;
               uint32_t frac_x = (tx >> 8) & 0xFF;
               uint32_t frac_y = (ty >> 8) & 0xFF;
               if (px1 > 0 && py1 > 0 && px1+2 < img->width && py1+2 < img->height) {
                  uint32_t *p = &img->pixels[(py1-1) * img->stride + (px1-1)];
                  #ifdef __SSE2__
                     __m128i c0_1, c2_3;
                     __m128i tmp0, tmp1, r0_1, r2_3;
                     union {
                        __m128i m128;
                        uint32_t u32[4];
                     } u;

                     c0_1 = _mm_loadl_epi64((__m128i *)(p+0));
                     c2_3 = _mm_loadl_epi64((__m128i *)(p+2));
                     tmp0 = interpolate_color_bicubic_sse2(c0_1, c2_3, frac_x);

                     p += img->stride;
                     c0_1 = _mm_loadl_epi64((__m128i *)(p+0));
                     c2_3 = _mm_loadl_epi64((__m128i *)(p+2));
                     tmp1 = interpolate_color_bicubic_sse2(c0_1, c2_3, frac_x);

                     r0_1 = _mm_unpacklo_epi32(tmp0, tmp1);

                     p += img->stride;
                     c0_1 = _mm_loadl_epi64((__m128i *)(p+0));
                     c2_3 = _mm_loadl_epi64((__m128i *)(p+2));
                     tmp0 = interpolate_color_bicubic_sse2(c0_1, c2_3, frac_x);
                     
                     p += img->stride;
                     c0_1 = _mm_loadl_epi64((__m128i *)(p+0));
                     c2_3 = _mm_loadl_epi64((__m128i *)(p+2));
                     tmp1 = interpolate_color_bicubic_sse2(c0_1, c2_3, frac_x);

                     r2_3 = _mm_unpacklo_epi32(tmp0, tmp1);

                     u.m128 = interpolate_color_bicubic_sse2(r0_1, r2_3, frac_y);
                     *rdest++ = u.u32[0];
                  #else
                     uint32_t r0, r1, r2, r3;
                     r0 = interpolate_color_bicubic(p[0], p[1], p[2], p[3], frac_x); p += img->stride;
                     r1 = interpolate_color_bicubic(p[0], p[1], p[2], p[3], frac_x); p += img->stride;
                     r2 = interpolate_color_bicubic(p[0], p[1], p[2], p[3], frac_x); p += img->stride;
                     r3 = interpolate_color_bicubic(p[0], p[1], p[2], p[3], frac_x);
                     *rdest++ = interpolate_color_bicubic(r0, r1, r2, r3, frac_y);
                  #endif
               }
               else {
                  uint32_t c0, c1, c2, c3, r0, r1, r2, r3;
                  if (flags & TEX_CLAMP_X) {
                     if (px1 < 0) { px1 = 0; frac_x = 0; }
                     if (px1 >= img->width) px1 = img->width-1;
                  }
                  if (flags & TEX_CLAMP_Y) {
                     if (py1 < 0) { py1 = 0; frac_y = 0; }
                     if (py1 >= img->height) py1 = img->height-1;
                  }
                  int px0 = px1-1;
                  int py0 = py1-1;
                  int px2 = px1+1;
                  int py2 = py1+1;
                  if (px0 < 0) px0 = flags & TEX_CLAMP_X? 0 : img->width-1;
                  if (py0 < 0) py0 = flags & TEX_CLAMP_Y? 0 : img->height-1;
                  if (px2 >= img->width) px2 = flags & TEX_CLAMP_X? img->width-1 : 0;
                  if (py2 >= img->height) py2 = flags & TEX_CLAMP_Y? img->height-1 : 0;
                  uint32_t px3 = px2+1;
                  uint32_t py3 = py2+1;
                  if (px3 >= img->width) px3 = flags & TEX_CLAMP_X? img->width-1 : 0;
                  if (py3 >= img->height) py3 = flags & TEX_CLAMP_Y? img->height-1 : 0;

                  c0 = img->pixels[py0 * img->stride + px0];
                  c1 = img->pixels[py0 * img->stride + px1];
                  c2 = img->pixels[py0 * img->stride + px2];
                  c3 = img->pixels[py0 * img->stride + px3];
                  r0 = interpolate_color_bicubic(c0, c1, c2, c3, frac_x);

                  c0 = img->pixels[py1 * img->stride + px0];
                  c1 = img->pixels[py1 * img->stride + px1];
                  c2 = img->pixels[py1 * img->stride + px2];
                  c3 = img->pixels[py1 * img->stride + px3];
                  r1 = interpolate_color_bicubic(c0, c1, c2, c3, frac_x);

                  c0 = img->pixels[py2 * img->stride + px0];
                  c1 = img->pixels[py2 * img->stride + px1];
                  c2 = img->pixels[py2 * img->stride + px2];
                  c3 = img->pixels[py2 * img->stride + px3];
                  r2 = interpolate_color_bicubic(c0, c1, c2, c3, frac_x);

                  c0 = img->pixels[py3 * img->stride + px0];
                  c1 = img->pixels[py3 * img->stride + px1];
                  c2 = img->pixels[py3 * img->stride + px2];
                  c3 = img->pixels[py3 * img->stride + px3];
                  r3 = interpolate_color_bicubic(c0, c1, c2, c3, frac_x);

                  *rdest++ = interpolate_color_bicubic(r0, r1, r2, r3, frac_y);
               }
            }
            DISPATCH();
         }

         op_copy: {
            uint32_t *rdest = regs[*bytecode++].u32;
            uint32_t *rsrc = regs[*bytecode++].u32;
            for (i=0; i<amount; i++) {
               *rdest++ = *rsrc++;
            }
            DISPATCH();
         }

         op_add: {
            uint8_t *rdest = regs[*bytecode++].u8;
            uint8_t *rsrc1 = regs[*bytecode++].u8;
            uint8_t *rsrc2 = regs[*bytecode++].u8;
            for (i=0; i<amount*4; i++) {
               int c = (*rsrc1++) + (*rsrc2++);
               if (c > 255) c = 255;
               *rdest++ = c;
            }
            DISPATCH();
         }

         op_sub: {
            uint8_t *rdest = regs[*bytecode++].u8;
            uint8_t *rsrc1 = regs[*bytecode++].u8;
            uint8_t *rsrc2 = regs[*bytecode++].u8;
            for (i=0; i<amount*4; i++) {
               int c = (*rsrc1++) - (*rsrc2++);
               if (c < 0) c = 0;
               *rdest++ = c;
            }
            DISPATCH();
         }

         op_mul: {
            uint8_t *rdest = regs[*bytecode++].u8;
            uint8_t *rsrc1 = regs[*bytecode++].u8;
            uint8_t *rsrc2 = regs[*bytecode++].u8;
            for (i=0; i<amount*4; i++) {
               int c = div255((*rsrc1++) * (*rsrc2++));
               *rdest++ = c;
            }
            DISPATCH();
         }

         op_mix: {
            uint8_t *rdest = regs[*bytecode++].u8;
            uint8_t *rsrc1 = regs[*bytecode++].u8;
            uint8_t *rsrc2 = regs[*bytecode++].u8;
            int alpha = shader->inputs[*bytecode++];
            for (i=0; i<amount*4; i++) {
               int a = *rsrc1++;
               int b = *rsrc2++;
               int c = (a * (256-alpha) + b * alpha) >> 8;
               *rdest++ = c;
            }
            DISPATCH();
         }

         op_output_blend: {
            uint32_t *rsrc = regs[*bytecode++].u32;

            if (coverage) {
               for (i=0; i<amount; i++) {
                  color = *rsrc++;
                  ca = (color >> 24) & 0xFF;
                  cr = (color >> 16) & 0xFF;
                  cg = (color >>  8) & 0xFF;
                  cb = (color >>  0) & 0xFF;
                  if (blend_table && ca != 0) {
                     cr = cr * 255 / ca;
                     cg = cg * 255 / ca;
                     cb = cb * 255 / ca;
                     if (cr > 255) cr = 255;
                     if (cg > 255) cg = 255;
                     if (cb > 255) cb = 255;
                     cr = div255(blend_table[cr] * ca);
                     cg = div255(blend_table[cg] * ca);
                     cb = div255(blend_table[cb] * ca);
                  }

                  pixel = dest[i];
                  c = coverage[i];

                  pa = (pixel >> 24) & 0xFF;
                  pr = (pixel >> 16) & 0xFF;
                  pg = (pixel >>  8) & 0xFF;
                  pb = (pixel >>  0) & 0xFF;

                  inv_ca = 255 - div255(ca * c);
                  pa = div255(ca * c) + div255(pa * inv_ca);
                  if (blend_table) {
                     pr = div255(cr * c) + div255(blend_table[pr] * inv_ca);
                     pg = div255(cg * c) + div255(blend_table[pg] * inv_ca);
                     pb = div255(cb * c) + div255(blend_table[pb] * inv_ca);
                  }
                  else {
                     pr = div255(cr * c) + div255(pr * inv_ca);
                     pg = div255(cg * c) + div255(pg * inv_ca);
                     pb = div255(cb * c) + div255(pb * inv_ca);
                  }

                  if (pr > 255) pr = 255;
                  if (pg > 255) pg = 255;
                  if (pb > 255) pb = 255;

                  if (blend_table) {
                     pr = blend_table[pr+256];
                     pg = blend_table[pg+256];
                     pb = blend_table[pb+256];
                  }
                  dest[i] = (pa << 24) | (pr << 16) | (pg << 8) | pb;
               }
            }
            else {
               for (i=0; i<amount; i++) {
                  color = *rsrc++;
                  ca = (color >> 24) & 0xFF;
                  cr = (color >> 16) & 0xFF;
                  cg = (color >>  8) & 0xFF;
                  cb = (color >>  0) & 0xFF;

                  pixel = dest[i];
                  pa = (pixel >> 24) & 0xFF;
                  pr = (pixel >> 16) & 0xFF;
                  pg = (pixel >>  8) & 0xFF;
                  pb = (pixel >>  0) & 0xFF;

                  inv_ca = 255 - ca;
                  pa = ca + div255(pa * inv_ca);
                  pr = cr + div255(pr * inv_ca);
                  pg = cg + div255(pg * inv_ca);
                  pb = cb + div255(pb * inv_ca);

                  if (pr > 255) pr = 255;
                  if (pg > 255) pg = 255;
                  if (pb > 255) pb = 255;

                  dest[i] = (pa << 24) | (pr << 16) | (pg << 8) | pb;
               }
            }
            break;
         }

         op_output_replace: {
            uint32_t *rsrc = regs[*bytecode++].u32;

            if (coverage) {
               for (i=0; i<amount; i++) {
                  c = coverage[i];
                  if (c == 0) continue;
                  if (c == 255) {
                     dest[i] = *rsrc++;
                     continue;
                  }

                  color = *rsrc++;
                  ca = (color >> 24) & 0xFF;
                  cr = (color >> 16) & 0xFF;
                  cg = (color >>  8) & 0xFF;
                  cb = (color >>  0) & 0xFF;

                  pixel = dest[i];

                  pa = (pixel >> 24) & 0xFF;
                  pr = (pixel >> 16) & 0xFF;
                  pg = (pixel >>  8) & 0xFF;
                  pb = (pixel >>  0) & 0xFF;

                  inv_ca = 255 - c;
                  pa = div255(ca * c + pa * inv_ca);
                  pr = div255(cr * c + pa * inv_ca);
                  pg = div255(cg * c + pa * inv_ca);
                  pb = div255(cb * c + pa * inv_ca);

                  dest[i] = (pa << 24) | (pr << 16) | (pg << 8) | pb;
               }
            }
            else {
               for (i=0; i<amount; i++) {
                  dest[i] = *rsrc++;
               }
            }
            break;
         }

         op_output_blend_subpixel: {
            uint32_t *rsrc = regs[*bytecode++].u32;
            int ma, mr, mg, mb, inv_ma, inv_mr, inv_mg, inv_mb;

            for (i=0; i<amount; i++) {
               color = *rsrc++;
               ca = (color >> 24) & 0xFF;
               cr = (color >> 16) & 0xFF;
               cg = (color >>  8) & 0xFF;
               cb = (color >>  0) & 0xFF;
               if (blend_table && ca != 0) {
                  cr = cr * 255 / ca;
                  cg = cg * 255 / ca;
                  cb = cb * 255 / ca;
                  if (cr > 255) cr = 255;
                  if (cg > 255) cg = 255;
                  if (cb > 255) cb = 255;
                  cr = div255(blend_table[cr] * ca);
                  cg = div255(blend_table[cg] * ca);
                  cb = div255(blend_table[cb] * ca);
               }

               pixel = dest[i];
               mr = coverage[i*3+0];
               mg = coverage[i*3+1];
               mb = coverage[i*3+2];

               pa = (pixel >> 24) & 0xFF;
               pr = (pixel >> 16) & 0xFF;
               pg = (pixel >>  8) & 0xFF;
               pb = (pixel >>  0) & 0xFF;

               ma = MAX(MAX(mr, mg), mb);
               inv_ma = 255 - div255(ma * ca);
               inv_mr = 255 - div255(mr * ca);
               inv_mg = 255 - div255(mg * ca);
               inv_mb = 255 - div255(mb * ca);

               pa = div255(ca * ma) + div255(pa * inv_ma);
               if (blend_table) {
                  pr = div255(cr * mr) + div255(blend_table[pr] * inv_mr);
                  pg = div255(cg * mg) + div255(blend_table[pg] * inv_mg);
                  pb = div255(cb * mb) + div255(blend_table[pb] * inv_mb);
               }
               else {
                  pr = div255(cr * mr) + div255(pr * inv_mr);
                  pg = div255(cg * mg) + div255(pg * inv_mg);
                  pb = div255(cb * mb) + div255(pb * inv_mb);
               }

               if (pr > 255) pr = 255;
               if (pg > 255) pg = 255;
               if (pb > 255) pb = 255;

               if (blend_table) {
                  pr = blend_table[pr+256];
                  pg = blend_table[pg+256];
                  pb = blend_table[pb+256];
               }
               dest[i] = (pa << 24) | (pr << 16) | (pg << 8) | pb;
            }
            break;
         }

         op_output_replace_subpixel: {
            uint32_t *rsrc = regs[*bytecode++].u32;

            for (i=0; i<amount; i++) {
               c = coverage[i*3+1];
               if (c == 0) continue;
               if (c == 255) {
                  dest[i] = *rsrc++;
                  continue;
               }

               color = *rsrc++;
               ca = (color >> 24) & 0xFF;
               cr = (color >> 16) & 0xFF;
               cg = (color >>  8) & 0xFF;
               cb = (color >>  0) & 0xFF;

               pixel = dest[i];

               pa = (pixel >> 24) & 0xFF;
               pr = (pixel >> 16) & 0xFF;
               pg = (pixel >>  8) & 0xFF;
               pb = (pixel >>  0) & 0xFF;

               inv_ca = 255 - c;
               pa = div255(ca * c + pa * inv_ca);
               pr = div255(cr * c + pa * inv_ca);
               pg = div255(cg * c + pa * inv_ca);
               pb = div255(cb * c + pa * inv_ca);

               dest[i] = (pa << 24) | (pr << 16) | (pg << 8) | pb;
            }
            break;
         }
      }

      dest += amount;
      len -= amount;
      sx += amount;
      if (coverage) {
         coverage += shader->subpixel? amount*3 : amount;
      }
   }

   #undef DISPATCH
   #undef RUN_LENGTH
}


static Value image_create_internal(Heap *heap, Value *error, int width, int height, int stride, uint32_t *pixels, int own_data, ImageData *parent, ImageFreeFunc free_func, void *free_data, int type)
{
   ImageData *data;
   Value handle, img;
   Value values[IMAGE_SIZE];
   int err;

   if (width < 0 || height < 0) {
      *error = fixscript_create_error_string(heap, "negative image dimensions");
      goto error;
   }
   if (width == 0 || height == 0) {
      *error = fixscript_create_error_string(heap, "zero image dimensions");
      goto error;
   }
   if (width > MAX_IMAGE_DIM || height > MAX_IMAGE_DIM) {
      *error = fixscript_create_error_string(heap, "image dimensions are too big");
      goto error;
   }
   
   data = calloc(1, sizeof(ImageData));
   if (!data) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   data->refcnt = 1;
   data->parent = parent;
   data->pixels = pixels;
   data->width = width;
   data->height = height;
   data->stride = stride;
   data->own_data = own_data;
   data->free_func = free_func;
   data->free_data = free_data;
   data->type = type;

   if (parent) {
      if (parent->refcnt < REFCNT_LIMIT) {
         __sync_add_and_fetch(&parent->refcnt, 1);
      }
   }
   
   handle = fixscript_create_or_get_shared_array(heap, HANDLE_TYPE_IMAGE_DATA, pixels, (height-1)*stride+width, 4, free_image_data, data, NULL);
   pixels = NULL;
   if (!handle.value) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   img = fixscript_create_array(heap, IMAGE_SIZE);
   if (!img.value) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   values[IMAGE_to_string_func] = fixscript_get_function(heap, fixscript_get(heap, "image/image.fix"), "image_to_string#1");
   values[IMAGE_data] = handle;
   values[IMAGE_width] = fixscript_int(width);
   values[IMAGE_height] = fixscript_int(height);
   values[IMAGE_stride] = fixscript_int(stride);

   err = fixscript_set_array_range(heap, img, 0, IMAGE_SIZE, values);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return img;

error:
   if (own_data) {
      free(pixels);
   }
   return fixscript_int(0);
}


static ImageData *get_image_data(Heap *heap, Value *error, Value value)
{
   ImageData *data = NULL;
   Value handle;
   int err;

   err = fixscript_get_array_elem(heap, value, IMAGE_data, &handle);
   if (err) {
      fixscript_error(heap, error, err);
      return NULL;
   }

   if (!fixscript_get_shared_array_data(heap, handle, NULL, NULL, (void **)&data, HANDLE_TYPE_IMAGE_DATA, NULL)) {
      *error = fixscript_create_error_string(heap, "invalid image handle");
      return NULL;
   }

   return data;
}


Value fiximage_create(Heap *heap, int width, int height)
{
   Value error;
   uint32_t *pixels;

   if (width <= 0 || height <= 0) {
      return fixscript_int(0);
   }
   if (width > MAX_IMAGE_DIM || height > MAX_IMAGE_DIM) {
      return fixscript_int(0);
   }

   pixels = calloc(1, width*height*4);
   if (!pixels) {
      return fixscript_int(0);
   }

   return image_create_internal(heap, &error, width, height, width, pixels, 1, NULL, NULL, NULL, -1);
}


Value fiximage_create_from_pixels(Heap *heap, int width, int height, int stride, uint32_t *pixels, ImageFreeFunc free_func, void *user_data, int type)
{
   Value error;

   return image_create_internal(heap, &error, width, height, stride, pixels, 0, NULL, free_func, user_data, type);
}


Value fiximage_create_painter(Heap *heap, Value img, int offset_x, int offset_y)
{
   ImageData *data;
   Painter *p;
   Value values[PAINTER_SIZE], error, painter, painter_handle;
   int err;

   data = get_image_data(heap, &error, img);
   if (!data) {
      return fixscript_int(0);
   }

   p = calloc(1, sizeof(Painter));
   if (!p) {
      return fixscript_int(0);
   }

   p->data = data;
   if (data->refcnt < REFCNT_LIMIT) {
      __sync_add_and_fetch(&data->refcnt, 1);
   }

   painter_handle = fixscript_create_handle(heap, HANDLE_TYPE_PAINTER, p, free_painter);
   if (!painter_handle.value) {
      return fixscript_int(0);
   }

   painter = fixscript_create_array(heap, PAINTER_SIZE);
   if (!painter.value) {
      return fixscript_int(0);
   }
   
   memset(values, 0, sizeof(Value)*PAINTER_SIZE);
   values[PAINTER_m00] = fixscript_float(1.0f);
   values[PAINTER_m11] = fixscript_float(1.0f);
   values[PAINTER_m02] = fixscript_float(offset_x);
   values[PAINTER_m12] = fixscript_float(offset_y);
   values[PAINTER_type] = fixscript_int(1); // TYPE_SIMPLE
   values[PAINTER_clip_x2] = fixscript_int(data->width);
   values[PAINTER_clip_y2] = fixscript_int(data->height);
   values[PAINTER_handle] = painter_handle;
   values[PAINTER_image] = img;

   err = fixscript_set_array_range(heap, painter, 0, PAINTER_SIZE, values);
   if (err) {
      return fixscript_int(0);
   }
   return painter;
}


int fiximage_get_data(Heap *heap, Value img, int *width, int *height, int *stride, uint32_t **pixels, void **user_data, int *type)
{
   ImageData *data;
   Value error;
   
   data = get_image_data(heap, &error, img);
   if (!data) return 0;
   if (width) *width = data->width;
   if (height) *height = data->height;
   if (stride) *stride = data->stride;
   if (pixels) *pixels = data->pixels;
   if (user_data) *user_data = data->free_data;
   if (type) *type = data->type;
   return 1;
}


int fiximage_get_painter_data(Heap *heap, Value p, float *tr, int *clip, Value *image)
{
   ImageData *data = NULL;
   Value painter[PAINTER_SIZE], handle;
   int err;

   err = fixscript_get_array_range(heap, p, 0, PAINTER_SIZE, painter);
   if (err) {
      return 0;
   }

   if (tr) {
      tr[0] = fixscript_get_float(painter[PAINTER_m00]);
      tr[1] = fixscript_get_float(painter[PAINTER_m01]);
      tr[2] = fixscript_get_float(painter[PAINTER_m02]);
      tr[3] = fixscript_get_float(painter[PAINTER_m10]);
      tr[4] = fixscript_get_float(painter[PAINTER_m11]);
      tr[5] = fixscript_get_float(painter[PAINTER_m12]);
   }

   if (clip) {
      err = fixscript_get_array_elem(heap, painter[PAINTER_image], IMAGE_data, &handle);
      if (err) {
         return 0;
      }

      if (!fixscript_get_shared_array_data(heap, handle, NULL, NULL, (void **)&data, HANDLE_TYPE_IMAGE_DATA, NULL)) {
         return 0;
      }

      clip[0] = fixscript_get_int(painter[PAINTER_clip_x1]);
      clip[1] = fixscript_get_int(painter[PAINTER_clip_y1]);
      clip[2] = fixscript_get_int(painter[PAINTER_clip_x2]);
      clip[3] = fixscript_get_int(painter[PAINTER_clip_y2]);
   
      if (clip[0] < 0) clip[0] = 0;
      if (clip[1] < 0) clip[1] = 0;
      if (clip[2] > data->width) clip[2] = data->width;
      if (clip[3] > data->height) clip[3] = data->height;

      if (clip[0] >= clip[2] || clip[1] >= clip[3]) {
         clip[0] = 0;
         clip[1] = 0;
         clip[2] = 0;
         clip[3] = 0;
      }
   }

   if (image) {
      *image = painter[PAINTER_image];
   }

   return 1;
}


static Value image_create(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   uint32_t *pixels;
   int width, height;

   width = fixscript_get_int(params[0]);
   height = fixscript_get_int(params[1]);

   if (width < 0 || height < 0) {
      *error = fixscript_create_error_string(heap, "negative image dimensions");
      return fixscript_int(0);
   }
   if (width == 0 || height == 0) {
      *error = fixscript_create_error_string(heap, "zero image dimensions");
      return fixscript_int(0);
   }
   if (width > MAX_IMAGE_DIM || height > MAX_IMAGE_DIM) {
      *error = fixscript_create_error_string(heap, "image dimensions are too big");
      return fixscript_int(0);
   }

   pixels = calloc(1, width*height*4);
   if (!pixels) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   return image_create_internal(heap, error, width, height, width, pixels, 1, NULL, NULL, NULL, -1);
}


static Value image_clone(Heap *heap, Value *error, int num_params, Value *params, void *_data)
{
   ImageData *data;
   uint32_t *pixels;
   int i, j;

   data = get_image_data(heap, error, params[0]);
   if (!data) {
      return fixscript_int(0);
   }

   pixels = malloc(data->width*data->height*4);
   if (!pixels) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   for (i=0; i<data->height; i++) {
      for (j=0; j<data->width; j++) {
         pixels[i*data->width+j] = data->pixels[i*data->stride+j];
      }
   }

   return image_create_internal(heap, error, data->width, data->height, data->width, pixels, 1, NULL, NULL, NULL, -1);
}


static Value image_get_subimage(Heap *heap, Value *error, int num_params, Value *params, void *_data)
{
   ImageData *data;
   int x, y, width, height;

   data = get_image_data(heap, error, params[0]);
   if (!data) {
      return fixscript_int(0);
   }

   x = params[1].value;
   y = params[2].value;
   width = params[3].value;
   height = params[4].value;

   if (x < 0 || y < 0 || width > data->width || height > data->height || x+width > data->width || y+height > data->height) {
      *error = fixscript_create_error_string(heap, "invalid size");
      return fixscript_int(0);
   }

   return image_create_internal(heap, error, width, height, data->stride, data->pixels + (y*data->stride+x), 0, data, NULL, NULL, -1);
}


static Value painter_create(Heap *heap, Value *error, int num_params, Value *params, void *func_data)
{
   ImageData *data;
   Painter *p;
   Value handle;

   data = get_image_data(heap, error, params[0]);
   if (!data) {
      return fixscript_int(0);
   }

   p = calloc(1, sizeof(Painter));
   if (!p) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   p->data = data;
   if (data->refcnt < REFCNT_LIMIT) {
      __sync_add_and_fetch(&data->refcnt, 1);
   }

   handle = fixscript_create_handle(heap, HANDLE_TYPE_PAINTER, p, free_painter);
   if (!handle.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return handle;
}


static int painter_get(Heap *heap, Value *error, Value painter_val, ImageData **data, Rect *clip, Value **clip_coords, int *clip_coords_len, int *clip_count, Transform *tr, int *flags, uint8_t **blend_table, Painter **p)
{
   Value painter[PAINTER_SIZE];
   Value handle;
   int i, err, len, elem_size;

   err = fixscript_get_array_range(heap, painter_val, 0, PAINTER_SIZE, painter);
   if (!err) {
      err = fixscript_get_array_elem(heap, painter[PAINTER_image], IMAGE_data, &handle);
   }
   if (err) {
      fixscript_error(heap, error, err);
      return 0;
   }

   if (!fixscript_get_shared_array_data(heap, handle, NULL, NULL, (void **)data, HANDLE_TYPE_IMAGE_DATA, NULL)) {
      *error = fixscript_create_error_string(heap, "invalid image handle");
      return 0;
   }

   clip->x1 = painter[PAINTER_clip_x1].value;
   clip->y1 = painter[PAINTER_clip_y1].value;
   clip->x2 = painter[PAINTER_clip_x2].value;
   clip->y2 = painter[PAINTER_clip_y2].value;

   if (clip->x1 < 0) clip->x1 = 0;
   if (clip->y1 < 0) clip->y1 = 0;
   if (clip->x2 > (*data)->width) clip->x2 = (*data)->width;
   if (clip->y2 > (*data)->height) clip->y2 = (*data)->height;

   if (clip->x1 >= clip->x2 || clip->y1 >= clip->y2) {
      return 0;
   }

   for (i=0; i<6; i++) {
      tr->m[i] = fixscript_get_float(painter[i]);
   }

   if (flags) {
      *flags = painter[PAINTER_flags].value;
   }

   if (blend_table) {
      if (painter[PAINTER_blend_table].value) {
         *blend_table = fixscript_get_shared_array_data(heap, painter[PAINTER_blend_table], &len, &elem_size, NULL, -1, NULL);
         if (*blend_table == NULL || len != 512 || elem_size != 1) {
            return 0;
         }
      }
      else {
         *blend_table = NULL;
      }
   }

   if (clip_coords) {
      *clip_coords = NULL;
      *clip_count = painter[PAINTER_clip_count].value;
      if (*clip_count == 0) {
         *clip_coords_len = 0;
      }
      else {
         err = fixscript_get_array_length(heap, painter[PAINTER_clip_shapes], clip_coords_len);
         if (!err) {
            *clip_coords = malloc((*clip_coords_len) * sizeof(Value));
            if (!(*clip_coords)) err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
         }
         if (!err) {
            err = fixscript_get_array_range(heap, painter[PAINTER_clip_shapes], 0, *clip_coords_len, *clip_coords);
         }
         if (err) {
            if (*clip_coords) {
               free(*clip_coords);
               *clip_coords = NULL;
            }
            fixscript_error(heap, error, err);
            return 0;
         }
      }
   }

   if (p) {
      *p = fixscript_get_handle(heap, painter[PAINTER_handle], HANDLE_TYPE_PAINTER, NULL);
      if (!(*p)) {
         *error = fixscript_create_error_string(heap, "invalid painter handle");
         return 0;
      }
   }
   return 1;
}


static void painter_add_batch_op(Painter *p, BatchOp *op, int x1, int y1, int x2, int y2)
{
   BatchTile *tile;
   BatchOp **new_ops;
   int i, j, new_cap;

   op->next = p->ops;
   p->ops = op;

   x1 = x1 / BATCH_TILE_SIZE;
   y1 = y1 / BATCH_TILE_SIZE;
   x2 = (x2 + BATCH_TILE_SIZE-1) / BATCH_TILE_SIZE;
   y2 = (y2 + BATCH_TILE_SIZE-1) / BATCH_TILE_SIZE;

   if (x1 < 0 || y1 < 0 || x2 > p->tile_width || y2 > p->tile_height) return;

   for (i=y1; i<y2; i++) {
      for (j=x1; j<x2; j++) {
         tile = &p->tiles[i*p->tile_width+j];
         if (tile->cnt == tile->cap) {
            new_cap = tile->cap? tile->cap*2 : 8;
            new_ops = realloc(tile->ops, new_cap*sizeof(BatchOp *));
            if (!new_ops) return;
            tile->cap = new_cap;
            tile->ops = new_ops;
         }
         tile->ops[tile->cnt++] = op;
      }
   }
}


static void fill_rect(int from, int to, void *data)
{
   FillRectData *fr = data;
   int i, j;
   uint32_t pixel;
   int ca, cr, cg, cb, inv_ca;
   int pa, pr, pg, pb;

   if (fr->type == 0) {
      for (i=from; i<to; i++) {
         for (j=fr->x1; j<fr->x2; j++) {
            fr->pixels[i*fr->stride+j] = fr->color;
         }
      }
   }
   else if (fr->type == 1) {
      ca = (fr->color >> 24) & 0xFF;
      cr = (fr->color >> 16) & 0xFF;
      cg = (fr->color >>  8) & 0xFF;
      cb = (fr->color >>  0) & 0xFF;
      inv_ca = 255 - ca;

      for (i=from; i<to; i++) {
         for (j=fr->x1; j<fr->x2; j++) {
            pixel = fr->pixels[i*fr->stride+j];

            pa = (pixel >> 24) & 0xFF;
            pr = (pixel >> 16) & 0xFF;
            pg = (pixel >>  8) & 0xFF;
            pb = (pixel >>  0) & 0xFF;

            pa = ca + div255(pa * inv_ca);
            pr = cr + div255(pr * inv_ca);
            pg = cg + div255(pg * inv_ca);
            pb = cb + div255(pb * inv_ca);

            if (pr > 255) pr = 255;
            if (pg > 255) pg = 255;
            if (pb > 255) pb = 255;

            fr->pixels[i*fr->stride+j] = (pa << 24) | (pr << 16) | (pg << 8) | pb;
         }
      }
   }
   else if (fr->type == 2) {
      for (i=from; i<to; i++) {
         run_shader(&fr->shader, &fr->pixels[i*fr->stride+fr->x1], NULL, fr->x2 - fr->x1, fr->x1, i, NULL);
      }
   }
}


static Value painter_fill_rect(Heap *heap, Value *error, int num_params, Value *params, void *func_data)
{
   ImageData *data;
   Rect clip, rect;
   FillRectData fr;
   Transform tr;
   Painter *p;
   BatchOp *op;

   if (!painter_get(heap, error, params[0], &data, &clip, NULL, NULL, NULL, &tr, NULL, NULL, &p)) {
      return fixscript_int(0);
   }

   rect.x1 = fixscript_get_int(params[1]);
   rect.y1 = fixscript_get_int(params[2]);
   rect.x2 = rect.x1 + fixscript_get_int(params[3]);
   rect.y2 = rect.y1 + fixscript_get_int(params[4]);

   rect_translate(&rect, tr.m02, tr.m12);

   if (!rect_clip(&rect, &clip)) {
      return fixscript_int(0);
   }

   fr.x1 = rect.x1;
   fr.x2 = rect.x2;
   fr.stride = data->stride;
   fr.pixels = data->pixels;
   fr.type = (intptr_t)func_data;

   if (fr.type == 0 || fr.type == 1) {
      fr.color = fixscript_get_int(params[5]);
   }
   else if (fr.type == 2) {
      memset(&fr.shader, 0, sizeof(Shader));
      if (!init_shader(&fr.shader, heap, params[5], params[6], &tr, 0)) {
         free_shader(&fr.shader);
         *error = fixscript_create_error_string(heap, "invalid shader");
         return fixscript_int(0);
      }
   }

   if (p->tiles) {
      op = calloc(1, sizeof(BatchOp));
      if (!op) {
         if (fr.type == 2) {
            free_shader(&fr.shader);
         }
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
      op->type = BATCH_OP_FILL_RECT;
      op->fill_rect.data = fr;
      op->fill_rect.y1 = rect.y1;
      op->fill_rect.y2 = rect.y2;
      if (fr.type == 2) {
         shader_ref_data(&fr.shader);
      }
      painter_add_batch_op(p, op, rect.x1, rect.y1, rect.x2, rect.y2);
   }
   else {
      fiximage_multicore_run(rect.y1, rect.y2, 100000/(rect.x2-rect.x1), fill_rect, &fr);

      if (fr.type == 2) {
         free_shader(&fr.shader);
      }
   }

   return fixscript_int(0);
}


static FORCE_INLINE Pos *alloc_pos(PosBlock **block)
{
   PosBlock *cur_block = *block, *new_block;

   if (cur_block->cnt == POS_BLOCK_SIZE) {
      new_block = malloc(sizeof(PosBlock));
      if (!new_block) return NULL;
      new_block->cnt = 0;
      new_block->next = NULL;
      cur_block->next = new_block;
      cur_block = new_block;
      *block = new_block;
   }

   return &cur_block->pos[cur_block->cnt++];
}


static int scan_line(Rect *clip, float px1, float py1, float px2, float py2, Pos **positions, PosBlock **block)
{
   Pos *pos, **pos_ptr;
   float min_py, max_py, min_py_floor, max_py_floor, fp, slope;
   int i, y1, y2;

   min_py = MIN(py1, py2);
   max_py = MAX(py1, py2);
   if (max_py - min_py < 0.001f) return 1;

   min_py_floor = fast_floor(min_py);
   max_py_floor = fast_floor(max_py);
   y1 = min_py_floor;
   y2 = max_py_floor;
   
   y1 = MAX(y1, clip->y1);
   y2 = MIN(y2, clip->y2-1);
   if (y1 > y2) return 1;

   pos_ptr = &positions[y1 - clip->y1];
   slope = (px2 - px1) / (py2 - py1);

   if (py1 < py2) {
      fp = px1;
   }
   else {
      fp = px1 - slope * (py1 - py2);
   }

   if (y1 == min_py_floor && y1 == max_py_floor) {
      pos = alloc_pos(block);
      if (!pos) return 0;
      pos->height = max_py - min_py;
      if (slope >= 0.0f) {
         pos->x = fp;
         pos->slope = slope;
      }
      else {
         pos->x = fp + slope * pos->height;
         pos->slope = -slope;
      }
      pos->negative = py1 > py2? -1.0f : 1.0f;
      pos->next = *pos_ptr;
      *pos_ptr = pos;
      return 1;
   }

   if (y1 == min_py_floor) {
      pos = alloc_pos(block);
      if (!pos) return 0;
      pos->height = 1.0f - (min_py - min_py_floor);
      if (slope >= 0.0f) {
         pos->x = fp;
         pos->slope = slope;
      }
      else {
         pos->x = fp + slope * pos->height;
         pos->slope = -slope;
      }
      fp += slope * pos->height;
      pos->negative = py1 > py2? -1.0f : 1.0f;
      pos->next = *pos_ptr;
      *pos_ptr++ = pos;
      y1++;
   }
   else {
      fp += slope * (y1 - min_py);
   }

   for (i=y1; i<=y2; i++) {
      pos = alloc_pos(block);
      if (!pos) return 0;
      if (i == max_py_floor) {
         pos->height = max_py - max_py_floor;
      }
      else {
         pos->height = 1.0f;
      }
      if (slope >= 0.0f) {
         pos->x = fp;
         pos->slope = slope;
      }
      else {
         pos->x = fp + slope * pos->height;
         pos->slope = -slope;
      }
      fp += slope;
      pos->negative = py1 > py2? -1.0f : 1.0f;
      pos->next = *pos_ptr;
      *pos_ptr++ = pos;
   }

   return 1;
}


static float point_distance_squared(float x1, float y1, float x2, float y2, float px, float py)
{
   float dx = x2 - x1;
   float dy = y2 - y1;
   float u = ((px - x1)*(x2 - x1) + (py - y1)*(y2 - y1)) / (dx*dx + dy*dy);
   float x = x1 + u * (x2 - x1);
   float y = y1 + u * (y2 - y1);
   dx = px - x;
   dy = py - y;
   return dx*dx + dy*dy;
}


static int quad_needs_split(float x1, float y1, float x2, float y2, float x3, float y3, float max_dist_sqr)
{
   return point_distance_squared(x1, y1, x3, y3, x2, y2) > max_dist_sqr;
}


static void quad_split(float x1, float y1, float x2, float y2, float x3, float y3, float *result)
{
   float p1_x = (x1 + x2) * 0.5f;
   float p1_y = (y1 + y2) * 0.5f;
   float p2_x = (x2 + x3) * 0.5f;
   float p2_y = (y2 + y3) * 0.5f;
   float p3_x = (p1_x + p2_x) * 0.5f;
   float p3_y = (p1_y + p2_y) * 0.5f;

   result[ 0] = x1;
   result[ 1] = y1;
   result[ 2] = p1_x;
   result[ 3] = p1_y;
   result[ 4] = p3_x;
   result[ 5] = p3_y;

   result[ 6] = p3_x;
   result[ 7] = p3_y;
   result[ 8] = p2_x;
   result[ 9] = p2_y;
   result[10] = x3;
   result[11] = y3;
}


static int scan_quad(Rect *clip, float x1, float y1, float x2, float y2, float x3, float y3, Pos **positions, PosBlock **block, int level)
{
   float r[12];

   if (level >= MAX_RECURSION || !quad_needs_split(x1, y1, x2, y2, x3, y3, MAX_DIST_SQR)) {
      return scan_line(clip, x1, y1, x3, y3, positions, block);
   }

   quad_split(x1, y1, x2, y2, x3, y3, r);
   if (!scan_quad(clip, r[0], r[1], r[2], r[3], r[4], r[5], positions, block, level+1)) return 0;
   if (!scan_quad(clip, r[6], r[7], r[8], r[9], r[10], r[11], positions, block, level+1)) return 0;
   return 1;
}


static int cubic_needs_split(float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, float max_dist_sqr)
{
   float d1, d2;

   d1 = point_distance_squared(x1, y1, x4, y4, x2, y2);
   d2 = point_distance_squared(x1, y1, x4, y4, x3, y3);
   return MAX(d1, d2) > max_dist_sqr;
}


static void cubic_split(float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, float *result)
{
   float p1_x = (x1 + x2) * 0.5f;
   float p1_y = (y1 + y2) * 0.5f;
   float p2_x = (x2 + x3) * 0.5f;
   float p2_y = (y2 + y3) * 0.5f;
   float p3_x = (x3 + x4) * 0.5f;
   float p3_y = (y3 + y4) * 0.5f;

   float p4_x = (p1_x + p2_x) * 0.5f;
   float p4_y = (p1_y + p2_y) * 0.5f;
   float p5_x = (p2_x + p3_x) * 0.5f;
   float p5_y = (p2_y + p3_y) * 0.5f;

   float p6_x = (p4_x + p5_x) * 0.5f;
   float p6_y = (p4_y + p5_y) * 0.5f;

   result[ 0] = x1;
   result[ 1] = y1;
   result[ 2] = p1_x;
   result[ 3] = p1_y;
   result[ 4] = p4_x;
   result[ 5] = p4_y;
   result[ 6] = p6_x;
   result[ 7] = p6_y;

   result[ 8] = p6_x;
   result[ 9] = p6_y;
   result[10] = p5_x;
   result[11] = p5_y;
   result[12] = p3_x;
   result[13] = p3_y;
   result[14] = x4;
   result[15] = y4;
}


static int scan_cubic(Rect *clip, float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, Pos **positions, PosBlock **block, int level)
{
   float r[16];

   if (level >= MAX_RECURSION || !cubic_needs_split(x1, y1, x2, y2, x3, y3, x4, y4, MAX_DIST_SQR)) {
      return scan_line(clip, x1, y1, x4, y4, positions, block);
   }

   cubic_split(x1, y1, x2, y2, x3, y3, x4, y4, r);
   if (!scan_cubic(clip, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], positions, block, level+1)) return 0;
   if (!scan_cubic(clip, r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15], positions, block, level+1)) return 0;
   return 1;
}


static FORCE_INLINE void fill_coverage(Pos *pos, int clip_x1, int clip_x2, float *accum, int *min_x_out, int *max_x_out)
{
   int i, x, min_x, max_x, x1, x2;
   float fx, h, n, fi, ri, area, inv_slope;

   min_x = INT_MAX;
   max_x = INT_MIN;
   for (; pos; pos = pos->next) {
      if (pos->slope < 0.001f) {
         x = fast_floor(pos->x);
         if (x < min_x) min_x = x;
         if (x > max_x) max_x = x;
         if (x >= clip_x2) {
            continue;
         }
         if (x < clip_x1) {
            accum[0] += pos->height * pos->negative;
         }
         else {
            area = (1.0f - (pos->x - fast_floor(pos->x))) * pos->height;
            accum[x - clip_x1] += area * pos->negative;
            accum[x - clip_x1 + 1] += (pos->height - area) * pos->negative;
         }
         continue;
      }

      fx = pos->x;
      h = pos->height;
      inv_slope = 1.0f / pos->slope;

      x1 = fast_floor(pos->x);
      x2 = fast_floor(pos->x + pos->slope * pos->height);
      if (x1 < min_x) min_x = x1;
      if (x2 > max_x) max_x = x2;

      for (i=x1; i<=x2; i++) {
         if (i >= clip_x2) {
            break;
         }
         fi = fast_floor(fx) + 1.0f;
         n = (fi - fx) * inv_slope;
         if (n <= h) {
            area = (fi-fx) * n * 0.5f;
            fx = fi;
            h -= n;
            x = i;
            if (x < clip_x1) {
               accum[0] += n * pos->negative;
            }
            else {
               accum[x - clip_x1] += area * pos->negative;
               accum[x - clip_x1 + 1] += (n - area) * pos->negative;
            }
         }
         else {
            ri = fx + pos->slope * h;
            area = (ri-fx) * h * 0.5f + (fi-ri) * h;
            x = i;
            if (x < clip_x1) {
               accum[0] += h * pos->negative;
            }
            else {
               accum[x - clip_x1] += area * pos->negative;
               accum[x - clip_x1 + 1] += (h - area) * pos->negative;
            }
            break;
         }
      }
   }

   if (min_x <= max_x) {
      if (min_x < clip_x1) min_x = clip_x1;
      if (max_x >= clip_x2) max_x = clip_x2-1;
      min_x -= clip_x1;
      max_x -= clip_x1;
   }

   *min_x_out = min_x;
   *max_x_out = max_x;
}


static void fill_shape_shader(int from, int to, void *data)
{
   FillShapeData *fs = data;
   uint32_t *pixels;
   int i, j;
   int min_x, max_x, clip_min_x, clip_max_x;
   float *accum = NULL, *clip_accum = NULL, accum_value, clip_accum_value;
   int value, clip_value;
   uint8_t *coverage = NULL;

   accum = calloc(fs->clip.x2 - fs->clip.x1 + 1, sizeof(float));
   if (fs->clip_positions) {
      clip_accum = calloc(fs->clip.x2 - fs->clip.x1 + 1, sizeof(float));
   }
   coverage = calloc(fs->clip.x2 - fs->clip.x1, sizeof(uint8_t));

   pixels = fs->pixels + from * fs->stride;

   for (i=from; i<to; i++) {
      fill_coverage(fs->positions[i - fs->clip.y1], fs->clip.x1, fs->clip.x2, accum, &min_x, &max_x);
      if (clip_accum) {
         fill_coverage(fs->clip_positions[i - fs->clip.y1], fs->clip.x1, fs->clip.x2, clip_accum, &clip_min_x, &clip_max_x);
         min_x = MIN(min_x, clip_min_x);
         max_x = MAX(max_x, clip_max_x);
      }
      if (min_x > max_x) {
         pixels += fs->stride;
         continue;
      }

      accum_value = 0.0f;
      clip_accum_value = 0.0f;
      for (j=min_x; j<=max_x; j++) {
         accum_value += accum[j];
         accum[j] = 0.0f;
         if (accum_value < 0.0f) {
            value = fast_round(accum_value * -255.0f);
         }
         else {
            value = fast_round(accum_value * 255.0f);
         }
         if (value > 255) value = 255;
         if (clip_accum) {
            clip_accum_value += clip_accum[j];
            clip_accum[j] = 0.0f;
            clip_value = fast_round(clip_accum_value * -256.0f) - ((fs->clip_count-1) << 8);
            if (clip_value < 0) clip_value = 0;
            if (clip_value > 256) clip_value = 256;
            value = (value * clip_value) >> 8;
         }
         coverage[j] = value;
      }
      accum[max_x+1] = 0;
      if (clip_accum) {
         clip_accum[max_x+1] = 0;
      }
      
      run_shader(&fs->shader, pixels + min_x, coverage + min_x, max_x - min_x + 1, fs->clip.x1 + min_x, i, fs->blend_table);
      pixels += fs->stride;
   }

   free(accum);
   free(clip_accum);
   free(coverage);
}


static void fill_shape_color(int from, int to, void *data)
{
   FillShapeData *fs = data;
   uint32_t *pixels, pixel;
   int i, j, min_x, max_x, clip_min_x, clip_max_x;
   int ca, cr, cg, cb, inv_ca, pa, pr, pg, pb;
   float *accum = NULL, *clip_accum = NULL, accum_value, clip_accum_value;
   int value, clip_value;

   ca = (fs->color >> 24) & 0xFF;
   cr = (fs->color >> 16) & 0xFF;
   cg = (fs->color >>  8) & 0xFF;
   cb = (fs->color >>  0) & 0xFF;

   if (fs->blend_table && ca != 0) {
      cr = cr * 255 / ca;
      cg = cg * 255 / ca;
      cb = cb * 255 / ca;
      if (cr > 255) cr = 255;
      if (cg > 255) cg = 255;
      if (cb > 255) cb = 255;
      cr = div255(fs->blend_table[cr] * ca);
      cg = div255(fs->blend_table[cg] * ca);
      cb = div255(fs->blend_table[cb] * ca);
   }

   accum = calloc(fs->clip.x2 - fs->clip.x1 + 1, sizeof(float));
   if (fs->clip_positions) {
      clip_accum = calloc(fs->clip.x2 - fs->clip.x1 + 1, sizeof(float));
   }

   pixels = fs->pixels + from * fs->stride;

   for (i=from; i<to; i++) {
      fill_coverage(fs->positions[i - fs->clip.y1], fs->clip.x1, fs->clip.x2, accum, &min_x, &max_x);
      if (clip_accum) {
         fill_coverage(fs->clip_positions[i - fs->clip.y1], fs->clip.x1, fs->clip.x2, clip_accum, &clip_min_x, &clip_max_x);
         min_x = MIN(min_x, clip_min_x);
         max_x = MAX(max_x, clip_max_x);
      }
      if (min_x > max_x) {
         pixels += fs->stride;
         continue;
      }
      
      accum_value = 0.0f;
      clip_accum_value = 0.0f;
      for (j=min_x; j<=max_x; j++) {
         accum_value += accum[j];
         accum[j] = 0.0f;
         if (accum_value < 0.0f) {
            value = fast_round(accum_value * -255.0f);
         }
         else {
            value = fast_round(accum_value * 255.0f);
         }
         if (value > 255) value = 255;
         if (clip_accum) {
            clip_accum_value += clip_accum[j];
            clip_accum[j] = 0.0f;
            clip_value = fast_round(clip_accum_value * -256.0f) - ((fs->clip_count-1) << 8);
            if (clip_value < 0) clip_value = 0;
            if (clip_value > 256) clip_value = 256;
            value = (value * clip_value) >> 8;
         }
         if (value > 0) {
            pixel = pixels[j];
            pa = (pixel >> 24) & 0xFF;
            pr = (pixel >> 16) & 0xFF;
            pg = (pixel >>  8) & 0xFF;
            pb = (pixel >>  0) & 0xFF;

            inv_ca = 255 - div255(ca * value);
            pa = div255(ca * value) + div255(pa * inv_ca);
            if (fs->blend_table) {
               pr = div255(cr * value) + div255(fs->blend_table[pr] * inv_ca);
               pg = div255(cg * value) + div255(fs->blend_table[pg] * inv_ca);
               pb = div255(cb * value) + div255(fs->blend_table[pb] * inv_ca);
            }
            else {
               pr = div255(cr * value) + div255(pr * inv_ca);
               pg = div255(cg * value) + div255(pg * inv_ca);
               pb = div255(cb * value) + div255(pb * inv_ca);
            }

            if (pr > 255) pr = 255;
            if (pg > 255) pg = 255;
            if (pb > 255) pb = 255;

            if (fs->blend_table) {
               pr = fs->blend_table[pr+256];
               pg = fs->blend_table[pg+256];
               pb = fs->blend_table[pb+256];
            }
            pixels[j] = (pa << 24) | (pr << 16) | (pg << 8) | pb;
         }
      }
      accum[max_x+1] = 0;
      if (clip_accum) {
         clip_accum[max_x+1] = 0;
      }
      
      pixels += fs->stride;
   }

   free(accum);
   free(clip_accum);
}


static void fill_shape_shader_subpixel(int from, int to, void *data)
{
   FillShapeData *fs = data;
   uint32_t *pixels;
   int i, j, min_x, max_x, clip_min_x, clip_max_x;
   int m0, m1, m2, m3, m4, mr, mg, mb, tmp;
   float *accum = NULL, *clip_accum = NULL, v1, v2, v3, a1, a2, a3, cv1, cv2, cv3=0;
   uint8_t *coverage = NULL;

   accum = calloc((fs->clip.x2 - fs->clip.x1)*3 + 1, sizeof(float));
   if (fs->clip_positions) {
      clip_accum = calloc((fs->clip.x2 - fs->clip.x1)*3 + 1, sizeof(float));
   }
   coverage = calloc((fs->clip.x2 - fs->clip.x1)*3, sizeof(uint8_t));

   pixels = fs->pixels + from * fs->stride;

   for (i=from; i<to; i++) {
      fill_coverage(fs->positions[i - fs->clip.y1], fs->clip.x1*3, fs->clip.x2*3, accum, &min_x, &max_x);
      if (clip_accum) {
         fill_coverage(fs->clip_positions[i - fs->clip.y1], fs->clip.x1*3, fs->clip.x2*3, clip_accum, &clip_min_x, &clip_max_x);
         min_x = MIN(min_x, clip_min_x);
         max_x = MAX(max_x, clip_max_x);
      }
      if (min_x > max_x) {
         pixels += fs->stride;
         continue;
      }

      min_x = min_x/3;
      max_x = max_x/3;

      if (min_x > 0) min_x--;
      if (max_x < fs->clip.x2 - fs->clip.x1 - 1) max_x++;
      
      v1 = 0.0f;
      v2 = 0.0f;
      v3 = accum[min_x*3+0];
      accum[min_x*3+0] = 0;
      m0 = 0;
      m1 = 0;
      m2 = 0;
      m3 = 0;
      m4 = fast_round((v3 >= 0.0f? v3 : -v3) * 255.0f);
      if (m4 > 255) m4 = 255;
      if (clip_accum) {
         cv1 = 0.0f;
         cv2 = 0.0f;
         cv3 = clip_accum[min_x*3+0];
         clip_accum[min_x*3+0] = 0;
         m4 = div255(m4 * MIN(MAX(0, fast_round(cv3 * -255.0f) - (fs->clip_count-1)*255), 255));
      }
      for (j=min_x; j<=max_x; j++) {
         v1 = v3 + accum[j*3+1];
         v2 = v1 + accum[j*3+2];
         v3 = v2 + accum[j*3+3];
         accum[j*3+1] = 0.0f;
         accum[j*3+2] = 0.0f;
         accum[j*3+3] = 0.0f;
         a1 = v1 >= 0.0f? v1 : -v1;
         a2 = v2 >= 0.0f? v2 : -v2;
         a3 = v3 >= 0.0f? v3 : -v3;
         m0 = m3;
         m1 = m4;
         m2 = fast_round(a1 * 255.0f);
         m3 = fast_round(a2 * 255.0f);
         m4 = fast_round(a3 * 255.0f);
         if (m2 > 255) m2 = 255;
         if (m3 > 255) m3 = 255;
         if (m4 > 255) m4 = 255;
         if (clip_accum) {
            cv1 = cv3 + clip_accum[j*3+1];
            cv2 = cv1 + clip_accum[j*3+2];
            cv3 = cv2 + clip_accum[j*3+3];
            clip_accum[j*3+1] = 0.0f;
            clip_accum[j*3+2] = 0.0f;
            clip_accum[j*3+3] = 0.0f;
            m2 = div255(m2 * MIN(MAX(0, fast_round(cv1 * -255.0f) - (fs->clip_count-1)*255), 255));
            m3 = div255(m3 * MIN(MAX(0, fast_round(cv2 * -255.0f) - (fs->clip_count-1)*255), 255));
            m4 = div255(m4 * MIN(MAX(0, fast_round(cv3 * -255.0f) - (fs->clip_count-1)*255), 255));
         }

         mr = (m0*85 + m1*86 + m2*85) >> 8;
         mg = (m1*85 + m2*86 + m3*85) >> 8;
         mb = (m2*85 + m3*86 + m4*85) >> 8;

         if (fs->flags & FLAGS_SUBPIXEL_REVERSED) {
            tmp = mr;
            mr = mb;
            mb = tmp;
         }
            
         coverage[j*3+0] = mr;
         coverage[j*3+1] = mg;
         coverage[j*3+2] = mb;
      }
      /*
      accum[(max_x+1)*3] = 0;
      if (clip_accum) {
         clip_accum[(max_x+1)*3] = 0;
      }
      */
      
      run_shader(&fs->shader, pixels + min_x, coverage + min_x*3, max_x - min_x + 1, fs->clip.x1 + min_x, i, fs->blend_table);
      pixels += fs->stride;
   }

   free(accum);
   free(clip_accum);
   free(coverage);
}


static void fill_shape_color_subpixel(int from, int to, void *data)
{
   FillShapeData *fs = data;
   uint32_t *pixels, pixel;
   int i, j, min_x, max_x, clip_min_x, clip_max_x;
   int ca, cr, cg, cb, pa, pr, pg, pb, ma, m0, m1, m2, m3, m4, mr, mg, mb, inv_ma, inv_mr, inv_mg, inv_mb, tmp;
   float *accum = NULL, *clip_accum = NULL, v1, v2, v3, a1, a2, a3, cv1, cv2, cv3=0;

   ca = (fs->color >> 24) & 0xFF;
   cr = (fs->color >> 16) & 0xFF;
   cg = (fs->color >>  8) & 0xFF;
   cb = (fs->color >>  0) & 0xFF;

   if (fs->blend_table && ca != 0) {
      cr = cr * 255 / ca;
      cg = cg * 255 / ca;
      cb = cb * 255 / ca;
      if (cr > 255) cr = 255;
      if (cg > 255) cg = 255;
      if (cb > 255) cb = 255;
      cr = div255(fs->blend_table[cr] * ca);
      cg = div255(fs->blend_table[cg] * ca);
      cb = div255(fs->blend_table[cb] * ca);
   }

   accum = calloc((fs->clip.x2 - fs->clip.x1)*3 + 1, sizeof(float));
   if (fs->clip_positions) {
      clip_accum = calloc((fs->clip.x2 - fs->clip.x1)*3 + 1, sizeof(float));
   }

   pixels = fs->pixels + from * fs->stride;

   for (i=from; i<to; i++) {
      fill_coverage(fs->positions[i - fs->clip.y1], fs->clip.x1*3, fs->clip.x2*3, accum, &min_x, &max_x);
      if (clip_accum) {
         fill_coverage(fs->clip_positions[i - fs->clip.y1], fs->clip.x1*3, fs->clip.x2*3, clip_accum, &clip_min_x, &clip_max_x);
         min_x = MIN(min_x, clip_min_x);
         max_x = MAX(max_x, clip_max_x);
      }
      if (min_x > max_x) {
         pixels += fs->stride;
         continue;
      }

      min_x = min_x/3;
      max_x = max_x/3;

      if (min_x > 0) min_x--;
      if (max_x < fs->clip.x2 - fs->clip.x1 - 1) max_x++;
      
      v1 = 0.0f;
      v2 = 0.0f;
      v3 = accum[min_x*3+0];
      accum[min_x*3+0] = 0;
      m0 = 0;
      m1 = 0;
      m2 = 0;
      m3 = 0;
      m4 = fast_round((v3 >= 0.0f? v3 : -v3) * 255.0f);
      if (m4 > 255) m4 = 255;
      if (clip_accum) {
         cv1 = 0.0f;
         cv2 = 0.0f;
         cv3 = clip_accum[min_x*3+0];
         clip_accum[min_x*3+0] = 0;
         m4 = div255(m4 * MIN(MAX(0, fast_round(cv3 * -255.0f) - (fs->clip_count-1)*255), 255));
      }
      for (j=min_x; j<=max_x; j++) {
         v1 = v3 + accum[j*3+1];
         v2 = v1 + accum[j*3+2];
         v3 = v2 + accum[j*3+3];
         accum[j*3+1] = 0.0f;
         accum[j*3+2] = 0.0f;
         accum[j*3+3] = 0.0f;
         a1 = v1 >= 0.0f? v1 : -v1;
         a2 = v2 >= 0.0f? v2 : -v2;
         a3 = v3 >= 0.0f? v3 : -v3;
         m0 = m3;
         m1 = m4;
         m2 = fast_round(a1 * 255.0f);
         m3 = fast_round(a2 * 255.0f);
         m4 = fast_round(a3 * 255.0f);
         if (m2 > 255) m2 = 255;
         if (m3 > 255) m3 = 255;
         if (m4 > 255) m4 = 255;
         if (clip_accum) {
            cv1 = cv3 + clip_accum[j*3+1];
            cv2 = cv1 + clip_accum[j*3+2];
            cv3 = cv2 + clip_accum[j*3+3];
            clip_accum[j*3+1] = 0.0f;
            clip_accum[j*3+2] = 0.0f;
            clip_accum[j*3+3] = 0.0f;
            m2 = div255(m2 * MIN(MAX(0, fast_round(cv1 * -255.0f) - (fs->clip_count-1)*255), 255));
            m3 = div255(m3 * MIN(MAX(0, fast_round(cv2 * -255.0f) - (fs->clip_count-1)*255), 255));
            m4 = div255(m4 * MIN(MAX(0, fast_round(cv3 * -255.0f) - (fs->clip_count-1)*255), 255));
         }
         if (m0+m1+m2+m3+m4 > 0) {
            mr = (m0*85 + m1*86 + m2*85) >> 8;
            mg = (m1*85 + m2*86 + m3*85) >> 8;
            mb = (m2*85 + m3*86 + m4*85) >> 8;

            if (fs->flags & FLAGS_SUBPIXEL_REVERSED) {
               tmp = mr;
               mr = mb;
               mb = tmp;
            }

            pixel = pixels[j];
            pa = (pixel >> 24) & 0xFF;
            pr = (pixel >> 16) & 0xFF;
            pg = (pixel >>  8) & 0xFF;
            pb = (pixel >>  0) & 0xFF;

            ma = MAX(MAX(mr, mg), mb);
            inv_ma = 255 - div255(ma * ca);
            inv_mr = 255 - div255(mr * ca);
            inv_mg = 255 - div255(mg * ca);
            inv_mb = 255 - div255(mb * ca);

            pa = div255(ca * ma) + div255(pa * inv_ma);
            if (fs->blend_table) {
               pr = div255(cr * mr) + div255(fs->blend_table[pr] * inv_mr);
               pg = div255(cg * mg) + div255(fs->blend_table[pg] * inv_mg);
               pb = div255(cb * mb) + div255(fs->blend_table[pb] * inv_mb);
            }
            else {
               pr = div255(cr * mr) + div255(pr * inv_mr);
               pg = div255(cg * mg) + div255(pg * inv_mg);
               pb = div255(cb * mb) + div255(pb * inv_mb);
            }

            if (pr > 255) pr = 255;
            if (pg > 255) pg = 255;
            if (pb > 255) pb = 255;

            if (fs->blend_table) {
               pr = fs->blend_table[pr+256];
               pg = fs->blend_table[pg+256];
               pb = fs->blend_table[pb+256];
            }
            pixels[j] = (pa << 24) | (pr << 16) | (pg << 8) | pb;
         }
      }
      /*
      accum[(max_x+1)*3] = 0;
      if (clip_accum) {
         clip_accum[(max_x+1)*3] = 0;
      }
      */
      
      pixels += fs->stride;
   }

   free(accum);
   free(clip_accum);
}


static FORCE_INLINE void transform_coord(Transform *tr, float *x, float *y)
{
   float new_x = transform_x(tr, *x, *y);
   float new_y = transform_y(tr, *x, *y);
   *x = new_x;
   *y = new_y;
}


static int pre_scan_coords(Value *coords, int coords_len, Transform *tr, float *min_x_out, float *min_y_out, float *max_x_out, float *max_y_out)
{
   float min_x, min_y, max_x, max_y;
   float x1, y1, x2, y2, x3, y3;
   int i;

   min_x = *min_x_out;
   min_y = *min_y_out;
   max_x = *max_x_out;
   max_y = *max_y_out;

   for (i=0; i<coords_len; i++) {
      switch (fixscript_get_int(coords[i])) {
         case PART_MOVE_TO:
         case PART_LINE_TO:
            if (i+2 >= coords_len) goto error;
            x1 = fixscript_get_float(coords[++i]);
            y1 = fixscript_get_float(coords[++i]);
            if (tr) {
               transform_coord(tr, &x1, &y1);
               coords[i-1] = fixscript_float(x1);
               coords[i-0] = fixscript_float(y1);
            }
            if (x1 < min_x) min_x = x1;
            if (y1 < min_y) min_y = y1;
            if (x1 > max_x) max_x = x1;
            if (y1 > max_y) max_y = y1;
            break;

         case PART_QUAD_TO:
            if (i+4 >= coords_len) goto error;
            x1 = fixscript_get_float(coords[++i]);
            y1 = fixscript_get_float(coords[++i]);
            x2 = fixscript_get_float(coords[++i]);
            y2 = fixscript_get_float(coords[++i]);
            if (tr) {
               transform_coord(tr, &x1, &y1);
               transform_coord(tr, &x2, &y2);
               coords[i-3] = fixscript_float(x1);
               coords[i-2] = fixscript_float(y1);
               coords[i-1] = fixscript_float(x2);
               coords[i-0] = fixscript_float(y2);
            }
            if (x1 < min_x) min_x = x1;
            if (y1 < min_y) min_y = y1;
            if (x1 > max_x) max_x = x1;
            if (y1 > max_y) max_y = y1;
            if (x2 < min_x) min_x = x2;
            if (y2 < min_y) min_y = y2;
            if (x2 > max_x) max_x = x2;
            if (y2 > max_y) max_y = y2;
            break;

         case PART_CUBIC_TO:
            if (i+6 >= coords_len) goto error;
            x1 = fixscript_get_float(coords[++i]);
            y1 = fixscript_get_float(coords[++i]);
            x2 = fixscript_get_float(coords[++i]);
            y2 = fixscript_get_float(coords[++i]);
            x3 = fixscript_get_float(coords[++i]);
            y3 = fixscript_get_float(coords[++i]);
            if (tr) {
               transform_coord(tr, &x1, &y1);
               transform_coord(tr, &x2, &y2);
               transform_coord(tr, &x3, &y3);
               coords[i-5] = fixscript_float(x1);
               coords[i-4] = fixscript_float(y1);
               coords[i-3] = fixscript_float(x2);
               coords[i-2] = fixscript_float(y2);
               coords[i-1] = fixscript_float(x3);
               coords[i-0] = fixscript_float(y3);
            }
            if (x1 < min_x) min_x = x1;
            if (y1 < min_y) min_y = y1;
            if (x1 > max_x) max_x = x1;
            if (y1 > max_y) max_y = y1;
            if (x2 < min_x) min_x = x2;
            if (y2 < min_y) min_y = y2;
            if (x2 > max_x) max_x = x2;
            if (y2 > max_y) max_y = y2;
            if (x3 < min_x) min_x = x3;
            if (y3 < min_y) min_y = y3;
            if (x3 > max_x) max_x = x3;
            if (y3 > max_y) max_y = y3;
            break;

         case PART_CLOSE_PATH:
            break;

         default:
            goto error;
      }
   }

   *min_x_out = min_x;
   *min_y_out = min_y;
   *max_x_out = max_x;
   *max_y_out = max_y;
   return 1;

error:
   return 0;
}


static int scan_coords(Rect *clip, Value *coords, int coords_len, Pos **positions, PosBlock **block)
{
   float first_x = 0.0f, first_y = 0.0f;
   float x0 = 0.0f, y0 = 0.0f;
   float x1, y1, x2, y2, x3, y3;
   int i;

   for (i=0; i<coords_len; i++) {
      switch (fixscript_get_int(coords[i])) {
         case PART_MOVE_TO:
            first_x = fixscript_get_float(coords[++i]);
            first_y = fixscript_get_float(coords[++i]);
            x0 = first_x;
            y0 = first_y;
            break;

         case PART_LINE_TO:
            x1 = fixscript_get_float(coords[++i]);
            y1 = fixscript_get_float(coords[++i]);
            if (!scan_line(clip, x0, y0, x1, y1, positions, block)) goto error;
            x0 = x1;
            y0 = y1;
            break;

         case PART_QUAD_TO:
            x1 = fixscript_get_float(coords[++i]);
            y1 = fixscript_get_float(coords[++i]);
            x2 = fixscript_get_float(coords[++i]);
            y2 = fixscript_get_float(coords[++i]);
            if (!scan_quad(clip, x0, y0, x1, y1, x2, y2, positions, block, 0)) goto error;
            x0 = x2;
            y0 = y2;
            break;

         case PART_CUBIC_TO:
            x1 = fixscript_get_float(coords[++i]);
            y1 = fixscript_get_float(coords[++i]);
            x2 = fixscript_get_float(coords[++i]);
            y2 = fixscript_get_float(coords[++i]);
            x3 = fixscript_get_float(coords[++i]);
            y3 = fixscript_get_float(coords[++i]);
            if (!scan_cubic(clip, x0, y0, x1, y1, x2, y2, x3, y3, positions, block, 0)) goto error;
            x0 = x3;
            y0 = y3;
            break;

         case PART_CLOSE_PATH:
            if (!scan_line(clip, x0, y0, first_x, first_y, positions, block)) goto error;
            x0 = first_x;
            y0 = first_y;
            break;
      }
   }

   return 1;

error:
   return 0;
}


static int process_shape_geometry(FillShapeGeometry *sg, FillShapeData *fs)
{
   PosBlock *cur_block;
   int ret = 0;

   fs->positions = calloc(fs->clip.y2 - fs->clip.y1, sizeof(Pos *));
   if (!fs->positions) {
      goto error;
   }

   if (fs->clip_count > 0) {
      fs->clip_positions = calloc(fs->clip.y2 - fs->clip.y1, sizeof(Pos *));
      if (!fs->clip_positions) {
         goto error;
      }
   }

   fs->block = malloc(sizeof(PosBlock));
   if (!fs->block) {
      goto error;
   }
   fs->block->cnt = 0;
   fs->block->next = NULL;
   cur_block = fs->block;

   if (!scan_coords(&fs->clip, sg->coords, sg->coords_len, fs->positions, &cur_block)) {
      goto error;
   }

   if (fs->clip_count > 0) {
      if (!scan_coords(&fs->clip, sg->clip_coords, sg->clip_coords_len, fs->clip_positions, &cur_block)) {
         goto error;
      }
   }

   ret = 1;

error:
   free(sg->coords);
   free(sg->clip_coords);
   sg->coords = NULL;
   sg->clip_coords = NULL;
   return ret;
}


static void free_fill_shape_data(FillShapeData *fs)
{
   PosBlock *block, *next_block;

   free(fs->positions);
   free(fs->clip_positions);
   block = fs->block;
   while (block) {
      next_block = block->next;
      free(block);
      block = next_block;
   }
   if (fs->use_shader) {
      free_shader(&fs->shader);
   }
}


static void process_geometry_in_thread(int from, int to, void *data)
{
   Painter *p = data;
   BatchGeom *geom;

   for (;;) {
      pthread_mutex_lock(&p->mutex);
      for (;;) {
         geom = p->geoms;
         if (!geom && p->geom_done) break;
         if (geom) {
            p->geoms = geom->next;
            break;
         }
         pthread_cond_wait(&p->conds[from], &p->mutex);
      }
      pthread_mutex_unlock(&p->mutex);

      if (!geom) break;
      
      if (!process_shape_geometry(&geom->sg, &geom->op->fill_shape.data)) {
         // TODO
      }

      free(geom);
   }
}


static Value painter_fill_shape(Heap *heap, Value *error, int num_params, Value *params, void *func_data)
{
   ImageData *data;
   float min_x, min_y, max_x, max_y;
   int err;
   int size_x;
   FillShapeGeometry sg;
   FillShapeData fs;
   Transform tr;
   Painter *p;
   BatchOp *op = NULL;
   BatchGeom *geom = NULL;
   int in_batch = 0;
   int i;

   memset(&sg, 0, sizeof(FillShapeGeometry));
   memset(&fs, 0, sizeof(FillShapeData));

   if (!painter_get(heap, error, params[0], &data, &sg.clip, &sg.clip_coords, &sg.clip_coords_len, &fs.clip_count, &sg.tr, &fs.flags, &fs.blend_table, &p)) {
      return fixscript_int(0);
   }

   if (num_params == 4) {
      fs.use_shader = 1;
      memset(&fs.shader, 0, sizeof(Shader));
      if (!init_shader(&fs.shader, heap, params[2], params[3], &sg.tr, fs.flags & FLAGS_SUBPIXEL_RENDERING)) {
         *error = fixscript_create_error_string(heap, "invalid shader");
         goto error;
      }
   }
   else {
      fs.use_shader = 0;
      fs.color = fixscript_get_int(params[2]);
   }

   if (fs.flags & FLAGS_SUBPIXEL_RENDERING) {
      sg.tr.m00 *= 3.0f;
      sg.tr.m01 *= 3.0f;
      sg.tr.m02 *= 3.0f;
   }

   err = fixscript_get_array_length(heap, params[1], &sg.coords_len);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   sg.coords = malloc(sg.coords_len * sizeof(Value));
   if (!sg.coords) {
      goto out_of_memory_error;
   }

   err = fixscript_get_array_range(heap, params[1], 0, sg.coords_len, sg.coords);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   sg.subpixel = fs.flags & FLAGS_SUBPIXEL_RENDERING;

   min_x = +FLT_MAX;
   min_y = +FLT_MAX;
   max_x = -FLT_MAX;
   max_y = -FLT_MAX;

   if (!pre_scan_coords(sg.coords, sg.coords_len, &sg.tr, &min_x, &min_y, &max_x, &max_y)) {
      goto garbled_coords_error;
   }

   if (fs.clip_count > 0) {
      if (sg.subpixel) {
         tr.m00 = 3.0f;
         tr.m01 = 0.0f;
         tr.m02 = 0.0f;
         tr.m10 = 0.0f;
         tr.m11 = 1.0f;
         tr.m12 = 0.0f;
      }
      if (!pre_scan_coords(sg.clip_coords, sg.clip_coords_len, sg.subpixel? &tr : NULL, &min_x, &min_y, &max_x, &max_y)) {
         goto garbled_coords_error;
      }
   }

   if (sg.subpixel) {
      min_x *= 0.3333f;
      max_x *= 0.3333f;
   }
   size_x = (int)(max_x - min_x + 0.5f);

   sg.clip.x1 = MAX(sg.clip.x1, ((int)min_x)-1);
   sg.clip.x2 = MIN(sg.clip.x2, ((int)max_x)+2);
   fs.clip = sg.clip;

   if ((int)min_y > sg.clip.y1) {
      sg.clip.y1 = (int)min_y;
   }
   if ((int)(max_y+1.0f)+1 < sg.clip.y2) {
      sg.clip.y2 = (int)(max_y+1.0f)+1;
   }

   if (sg.clip.x1 < sg.clip.x2 && sg.clip.y1 < sg.clip.y2) {
      fs.pixels = data->pixels + sg.clip.x1;
      fs.stride = data->stride;
      if (fs.use_shader) {
         fs.func = fs.flags & FLAGS_SUBPIXEL_RENDERING? fill_shape_shader_subpixel : fill_shape_shader;
      }
      else {
         fs.func = fs.flags & FLAGS_SUBPIXEL_RENDERING? fill_shape_color_subpixel : fill_shape_color;
      }

      if (p->tiles) {
         if (!p->geom_threads) {
            p->geom_threads = calloc(multicore_num_cores, sizeof(CoreThread *));
            if (!p->geom_threads) {
               goto out_of_memory_error;
            }
            for (i=0; i<multicore_num_cores; i++) {
               p->geom_threads[i] = acquire_thread();
               if (!p->geom_threads[i]) {
                  *error = fixscript_create_error_string(heap, "can't create thread");
                  goto error;
               }
            }
            for (i=0; i<multicore_num_cores; i++) {
               start_in_thread(p->geom_threads[i], i, i, process_geometry_in_thread, p);
            }
         }

         op = calloc(1, sizeof(BatchOp));
         geom = calloc(1, sizeof(BatchGeom));
         if (!op || !geom) {
            free(op);
            free(geom);
            goto out_of_memory_error;
         }
         op->type = BATCH_OP_FILL_SHAPE;
         op->fill_shape.data = fs;
         op->fill_shape.y1 = sg.clip.y1;
         op->fill_shape.y2 = sg.clip.y2;
         if (fs.use_shader) {
            shader_ref_data(&fs.shader);
         }

         geom->sg = sg;
         geom->op = op;
         pthread_mutex_lock(&p->mutex);
         geom->next = p->geoms;
         p->geoms = geom;
         for (i=0; i<multicore_num_cores; i++) {
            pthread_cond_signal(&p->conds[i]);
         }
         pthread_mutex_unlock(&p->mutex);

         painter_add_batch_op(p, op, sg.clip.x1, sg.clip.y1, sg.clip.x2, sg.clip.y2);
         in_batch = 1;
         return fixscript_int(0);
      }
      else {
         if (!process_shape_geometry(&sg, &fs)) {
            goto out_of_memory_error;
         }
         fiximage_multicore_run(sg.clip.y1, sg.clip.y2, size_x > 0? 100000/size_x : 100000, fs.func, &fs);
      }
   }

error:
   free(sg.coords);
   free(sg.clip_coords);
   if (!in_batch) {
      free_fill_shape_data(&fs);
   }
   return fixscript_int(0);

garbled_coords_error:
   *error = fixscript_create_error_string(heap, "garbled coordinate values");
   goto error;

out_of_memory_error:
   fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   goto error;
}


static Painter *get_painter(Heap *heap, Value *error, Value instance)
{
   Painter *p;
   Value handle;
   int err;
   
   err = fixscript_get_array_elem(heap, instance, PAINTER_handle, &handle);
   if (err) {
      fixscript_error(heap, error, err);
      return NULL;
   }

   p = fixscript_get_handle(heap, handle, HANDLE_TYPE_PAINTER, NULL);
   if (!p) {
      *error = fixscript_create_error_string(heap, "invalid painter handle");
      return NULL;
   }

   return p;
}


static Value painter_batch_begin(Heap *heap, Value *error, int num_params, Value *params, void *func_data)
{
   Painter *p;
   BatchTile *tile;
   int i, j;

   p = get_painter(heap, error, params[0]);
   if (!p || p->tiles) {
      return fixscript_int(0);
   }

   if (multicore_num_cores == 0) {
      fiximage_multicore_run(0, 1, 0, dummy_multicore_func, NULL);
   }

   p->tile_width = (p->data->width + BATCH_TILE_SIZE - 1) / BATCH_TILE_SIZE;
   p->tile_height = (p->data->height + BATCH_TILE_SIZE - 1) / BATCH_TILE_SIZE;
   p->tiles = calloc(p->tile_width * p->tile_height, sizeof(BatchTile));
   if (!p->tiles) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   p->conds = calloc(multicore_num_cores, sizeof(pthread_cond_t));
   if (!p->conds) {
      free(p->tiles);
      p->tiles = NULL;
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   
   if (pthread_mutex_init(&p->mutex, NULL) != 0) {
      free(p->conds);
      free(p->tiles);
      p->tiles = NULL;
      *error = fixscript_create_error_string(heap, "can't create mutex");
      return fixscript_int(0);
   }
   
   for (i=0; i<multicore_num_cores; i++) {
      if (pthread_cond_init(&p->conds[i], NULL) != 0) {
         while (--i >= 0) {
            pthread_cond_destroy(&p->conds[i]);
         }
         pthread_mutex_destroy(&p->mutex);
         free(p->conds);
         free(p->tiles);
         p->tiles = NULL;
         *error = fixscript_create_error_string(heap, "can't create condition");
         return fixscript_int(0);
      }
   }

   for (i=0; i<p->tile_height; i++) {
      for (j=0; j<p->tile_width; j++) {
         tile = &p->tiles[i*p->tile_width+j];
         tile->x1 = j*BATCH_TILE_SIZE;
         tile->y1 = i*BATCH_TILE_SIZE;
         tile->x2 = tile->x1 + BATCH_TILE_SIZE;
         tile->y2 = tile->y1 + BATCH_TILE_SIZE;
         if (tile->x2 > p->data->width) tile->x2 = p->data->width;
         if (tile->y2 > p->data->height) tile->y2 = p->data->height;
      }
   }

   return fixscript_int(0);
}


static void draw_tile(BatchTile *tile)
{
   FillRectData fr;
   FillShapeData fs;
   BatchOp *op;
   int i, y1, y2;

   for (i=0; i<tile->cnt; i++) {
      op = tile->ops[i];
      if (op->type == BATCH_OP_FILL_RECT) {
         fr = op->fill_rect.data;
         if (fr.x1 < tile->x1) fr.x1 = tile->x1;
         if (fr.x2 > tile->x2) fr.x2 = tile->x2;
         if (fr.x1 >= fr.x2) continue;
         y1 = op->fill_rect.y1;
         y2 = op->fill_rect.y2;
         if (y1 < tile->y1) y1 = tile->y1;
         if (y2 > tile->y2) y2 = tile->y2;
         if (y1 >= y2) continue;
         fill_rect(y1, y2, &fr);
      }
      else if (op->type == BATCH_OP_FILL_SHAPE) {
         fs = op->fill_shape.data;
         if (fs.clip.x1 < tile->x1) fs.clip.x1 = tile->x1;
         if (fs.clip.x2 > tile->x2) fs.clip.x2 = tile->x2;
         if (fs.clip.x1 >= fs.clip.x2) continue;
         fs.pixels += fs.clip.x1 - op->fill_shape.data.clip.x1;
         y1 = op->fill_shape.y1;
         y2 = op->fill_shape.y2;
         if (y1 < tile->y1) y1 = tile->y1;
         if (y2 > tile->y2) y2 = tile->y2;
         if (y1 >= y2) continue;
         fs.func(y1, y2, &fs);
      }
   }
}


static void draw_tiles(int from, int to, void *data)
{
   Painter *p = data;
   BatchTile *tile;

   for (;;) {
      pthread_mutex_lock(&p->mutex);
      tile = p->cur_tiles;
      if (tile) {
         p->cur_tiles = tile->cur_next;
      }
      pthread_mutex_unlock(&p->mutex);

      if (!tile) break;
      draw_tile(tile);
   }
}


static void flush_batch(Painter *p)
{
   BatchOp *op, *next_op;
   BatchTile *tile;
   int i;

   if (p->geom_threads) {
      pthread_mutex_lock(&p->mutex);
      p->geom_done = 1;
      for (i=0; i<multicore_num_cores; i++) {
         pthread_cond_signal(&p->conds[i]);
      }
      pthread_mutex_unlock(&p->mutex);

      for (i=0; i<multicore_num_cores; i++) {
         finish_in_thread(p->geom_threads[i]);
         release_thread(p->geom_threads[i]);
      }
      free(p->geom_threads);
      p->geom_threads = NULL;
      p->geom_done = 0;
   }

   p->cur_tiles = NULL;
   for (i=p->tile_width*p->tile_height-1; i>=0; i--) {
      tile = &p->tiles[i];
      tile->cur_next = p->cur_tiles;
      p->cur_tiles = tile;
   }
   fiximage_multicore_run(0, 1000, 0, draw_tiles, p);

   for (i=0; i<p->tile_width*p->tile_height; i++) {
      p->tiles[i].cnt = 0;
   }

   op = p->ops;
   while (op) {
      next_op = op->next;
      free_batch_op(op);
      op = next_op;
   }
   p->ops = NULL;
}


static Value painter_batch_flush(Heap *heap, Value *error, int num_params, Value *params, void *func_data)
{
   Painter *p;

   p = get_painter(heap, error, params[0]);
   if (!p || !p->tiles) {
      return fixscript_int(0);
   }

   flush_batch(p);

   return fixscript_int(0);
}


static Value painter_batch_end(Heap *heap, Value *error, int num_params, Value *params, void *func_data)
{
   Painter *p;
   int i;

   p = get_painter(heap, error, params[0]);
   if (!p || !p->tiles) {
      return fixscript_int(0);
   }

   flush_batch(p);

   for (i=0; i<p->tile_width*p->tile_height; i++) {
      free(p->tiles[i].ops);
   }
   free(p->tiles);
   p->tiles = NULL;
   for (i=0; i<multicore_num_cores; i++) {
      pthread_cond_destroy(&p->conds[i]);
   }
   pthread_mutex_destroy(&p->mutex);

   return fixscript_int(0);
}


static int hit_line(float px1, float py1, float px2, float py2, float x, float y)
{
   float px;

   if (py1 < py2) {
      if (y < py1 || y >= py2) return 0;
      px = px1 + (px2 - px1) / (py2 - py1) * (y - py1);
      if (px < x) {
         return 1;
      }
   }
   else {
      if (y < py2 || y >= py1) return 0;
      px = px2 + (px1 - px2) / (py1 - py2) * (y - py2);
      if (px < x) {
         return -1;
      }
   }
   return 0;
}


static int hit_quad(float x1, float y1, float x2, float y2, float x3, float y3, float x, float y, int level)
{
   float r[12];
   float min_y1, max_y1, min_y2, max_y2;
   int cnt = 0;

   if (level >= MAX_RECURSION || !quad_needs_split(x1, y1, x2, y2, x3, y3, MAX_DIST_SQR)) {
      return hit_line(x1, y1, x3, y3, x, y);
   }

   quad_split(x1, y1, x2, y2, x3, y3, r);
   min_y1 = MIN(r[1], MIN(r[3], r[5]));
   max_y1 = MAX(r[1], MAX(r[3], r[5]));
   min_y2 = MIN(r[7], MIN(r[9], r[11]));
   max_y2 = MAX(r[7], MAX(r[9], r[11]));
   if (y >= min_y1 && y < max_y1) {
      cnt += hit_quad(r[0], r[1], r[2], r[3], r[4], r[5], x, y, level+1);
   }
   if (y >= min_y2 && y < max_y2) {
      cnt += hit_quad(r[6], r[7], r[8], r[9], r[10], r[11], x, y, level+1);
   }
   return cnt;
}


static int hit_cubic(float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, float x, float y, int level)
{
   float r[16];
   float min_y1, max_y1, min_y2, max_y2;
   int cnt = 0;

   if (level >= MAX_RECURSION || !cubic_needs_split(x1, y1, x2, y2, x3, y3, x4, y4, MAX_DIST_SQR)) {
      return hit_line(x1, y1, x4, y4, x, y);
   }

   cubic_split(x1, y1, x2, y2, x3, y3, x4, y4, r);
   min_y1 = MIN(MIN(r[1], r[3]), MIN(r[5], r[7]));
   max_y1 = MAX(MAX(r[1], r[3]), MAX(r[5], r[7]));
   min_y2 = MIN(MIN(r[9], r[11]), MIN(r[13], r[15]));
   max_y2 = MAX(MAX(r[9], r[11]), MAX(r[13], r[15]));
   if (y >= min_y1 && y < max_y1) {
      cnt += hit_cubic(r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], x, y, level+1);
   }
   if (y >= min_y2 && y < max_y2) {
      cnt += hit_cubic(r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15], x, y, level+1);
   }
   return cnt;
}


static Value shape_hit_test(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value *coords = NULL, retval = fixscript_int(0);
   int coords_len;
   float first_x = 0.0f, first_y = 0.0f;
   float x0 = 0.0f, y0 = 0.0f;
   float x1, y1, x2, y2, x3, y3;
   float x, y;
   int i, err, cnt=0;

   err = fixscript_get_array_length(heap, params[0], &coords_len);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   coords = malloc(coords_len * sizeof(Value));
   if (!coords) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   err = fixscript_get_array_range(heap, params[0], 0, coords_len, coords);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   x = fixscript_get_float(params[1]);
   y = fixscript_get_float(params[2]);

   for (i=0; i<coords_len; i++) {
      switch (fixscript_get_int(coords[i])) {
         case PART_MOVE_TO:
            if (i+2 >= coords_len) goto garbled_coords_error;
            first_x = fixscript_get_float(coords[++i]);
            first_y = fixscript_get_float(coords[++i]);
            x0 = first_x;
            y0 = first_y;
            break;

         case PART_LINE_TO:
            if (i+2 >= coords_len) goto garbled_coords_error;
            x1 = fixscript_get_float(coords[++i]);
            y1 = fixscript_get_float(coords[++i]);
            cnt += hit_line(x0, y0, x1, y1, x, y);
            x0 = x1;
            y0 = y1;
            break;

         case PART_QUAD_TO:
            if (i+4 >= coords_len) goto garbled_coords_error;
            x1 = fixscript_get_float(coords[++i]);
            y1 = fixscript_get_float(coords[++i]);
            x2 = fixscript_get_float(coords[++i]);
            y2 = fixscript_get_float(coords[++i]);
            cnt += hit_quad(x0, y0, x1, y1, x2, y2, x, y, 0);
            x0 = x2;
            y0 = y2;
            break;

         case PART_CUBIC_TO:
            if (i+6 >= coords_len) goto garbled_coords_error;
            x1 = fixscript_get_float(coords[++i]);
            y1 = fixscript_get_float(coords[++i]);
            x2 = fixscript_get_float(coords[++i]);
            y2 = fixscript_get_float(coords[++i]);
            x3 = fixscript_get_float(coords[++i]);
            y3 = fixscript_get_float(coords[++i]);
            cnt += hit_cubic(x0, y0, x1, y1, x2, y2, x3, y3, x, y, 0);
            x0 = x3;
            y0 = y3;
            break;

         case PART_CLOSE_PATH:
            cnt += hit_line(x0, y0, first_x, first_y, x, y);
            x0 = first_x;
            y0 = first_y;
            break;

         default:
            goto garbled_coords_error;
      }
   }

   retval = fixscript_int(cnt != 0);

error:
   free(coords);
   return retval;

garbled_coords_error:
   *error = fixscript_create_error_string(heap, "garbled coordinate values");
   goto error;
}


static int array_append_init(ArrayAppend *aa, Heap *heap, Value array)
{
   aa->heap = heap;
   aa->array = array;
   aa->cnt = 0;
   return fixscript_get_array_length(heap, array, &aa->total_len);
}


static int array_append_flush(ArrayAppend *aa)
{
   int err;
   
   err = fixscript_set_array_length(aa->heap, aa->array, aa->total_len + aa->cnt);
   if (err) return err;

   err = fixscript_set_array_range(aa->heap, aa->array, aa->total_len, aa->cnt, aa->data);
   if (err) return err;

   aa->total_len += aa->cnt;
   aa->cnt = 0;
   return FIXSCRIPT_SUCCESS;
}


static FORCE_INLINE int array_append_reserve(ArrayAppend *aa, int cnt)
{
   int err, limit = sizeof(aa->data)/sizeof(Value);
   if (cnt > limit) {
      return FIXSCRIPT_ERR_OUT_OF_MEMORY;
   }
   if (aa->cnt + cnt > limit) {
      err = array_append_flush(aa);
      if (err) return err;
   }
   return FIXSCRIPT_SUCCESS;
}


static FORCE_INLINE void array_append_value(ArrayAppend *aa, Value value)
{
   aa->data[aa->cnt++] = value;
}


static FORCE_INLINE void quad_point(float x1, float y1, float x2, float y2, float x3, float y3, float t, float *rx, float *ry)
{
   float t2 = t * t;
   float mt = 1 - t;
   float mt2 = mt * mt;
   float mtt2 = 2*mt*t;
   *rx = x1*mt2 + x2*mtt2 + x3*t2;
   *ry = y1*mt2 + y2*mtt2 + y3*t2;
}


static FORCE_INLINE void quad_tangent(float x1, float y1, float x2, float y2, float x3, float y3, float t, float *tx, float *ty)
{
   float px1, py1, px2, py2, dx, dy, len;
   quad_point(x1, y1, x2, y2, x3, y3, t, &px1, &py1);
   quad_point(x1, y1, x2, y2, x3, y3, t+0.001f, &px2, &py2);
   dx = px2 - px1;
   dy = py2 - py1;
   len = sqrtf(dx*dx + dy*dy);
   if (len <= 0.000001f) len = 1.0f;
   *tx = dx / len;
   *ty = dy / len;
}


static int quad_split_offset(float x1, float y1, float x2, float y2, float x3, float y3, ArrayAppend *coords, ArrayAppend *tangents, float max_dist_sqr, int level, float *first_tangent)
{
   float p1_x = (x1 + x2) * 0.5f;
   float p1_y = (y1 + y2) * 0.5f;
   float p2_x = (x2 + x3) * 0.5f;
   float p2_y = (y2 + y3) * 0.5f;
   float p3_x = (p1_x + p2_x) * 0.5f;
   float p3_y = (p1_y + p2_y) * 0.5f;

   float cx = (x1 + x2 + x3) * 0.333333f;
   float cy = (y1 + y2 + y3) * 0.333333f;
   float dx = p3_x - cx;
   float dy = p3_y - cy;
   float dist_sqr = dx*dx + dy*dy;

   int err;
   float tx, ty;

   if (dist_sqr < max_dist_sqr || level >= MAX_RECURSION) {
      err = array_append_reserve(coords, 5);
      if (err) return err;

      err = array_append_reserve(tangents, 6);
      if (err) return err;

      array_append_value(coords, fixscript_int(PART_QUAD_TO));
      array_append_value(coords, fixscript_float(x2));
      array_append_value(coords, fixscript_float(y2));
      array_append_value(coords, fixscript_float(x3));
      array_append_value(coords, fixscript_float(y3));

      quad_tangent(x1, y1, x2, y2, x3, y3, 0.0f, &tx, &ty);
      array_append_value(tangents, fixscript_float(tx));
      array_append_value(tangents, fixscript_float(ty));
      if (first_tangent) {
         first_tangent[0] = tx;
         first_tangent[1] = ty;
      }
      quad_tangent(x1, y1, x2, y2, x3, y3, 0.5f, &tx, &ty);
      array_append_value(tangents, fixscript_float(tx));
      array_append_value(tangents, fixscript_float(ty));
      quad_tangent(x1, y1, x2, y2, x3, y3, 1.0f, &tx, &ty);
      array_append_value(tangents, fixscript_float(tx));
      array_append_value(tangents, fixscript_float(ty));
      return FIXSCRIPT_SUCCESS;
   }

   err = quad_split_offset(x1, y1, p1_x, p1_y, p3_x, p3_y, coords, tangents, max_dist_sqr, level+1, first_tangent);
   if (err) return err;

   err = quad_split_offset(p3_x, p3_y, p2_x, p2_y, x3, y3, coords, tangents, max_dist_sqr, level+1, NULL);
   if (err) return err;

   return FIXSCRIPT_SUCCESS;
}


static FORCE_INLINE int point_side(float x1, float y1, float x2, float y2, float px, float py)
{
   float a = -(y2 - y1);
   float b = x2 - x1;
   float c = x1*a + y1*b;
   float sign = a*px + b*py - c;
   return sign < 0? -1 : sign > 0? +1 : 0;
}


static FORCE_INLINE void cubic_point(float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, float t, float *px, float *py)
{
   float t2 = t * t;
   float t3 = t2 * t;
   float mt = 1.0f - t;
   float mt2 = mt * mt;
   float mt3 = mt2 * mt;
   float mt2t3 = 3*mt2*t;
   float mtt23 = 3*mt*t2;
   *px = x1*mt3 + x2*mt2t3 + x3*mtt23 + x4*t3;
   *py = y1*mt3 + y2*mt2t3 + y3*mtt23 + y4*t3;
}


static FORCE_INLINE void cubic_tangent(float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, float t, float *tx, float *ty)
{
   float px1, py1, px2, py2, dx, dy, len;
   cubic_point(x1, y1, x2, y2, x3, y3, x4, y4, t, &px1, &py1);
   cubic_point(x1, y1, x2, y2, x3, y3, x4, y4, t+0.001f, &px2, &py2);
   dx = px2 - px1;
   dy = py2 - py1;
   len = sqrtf(dx*dx + dy*dy);
   if (len <= 0.000001f) len = 1.0f;
   *tx = dx / len;
   *ty = dy / len;
}


static int cubic_split_offset(float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, ArrayAppend *coords, ArrayAppend *tangents, float max_dist_sqr, int level, float *first_tangent)
{
   int side1 = point_side(x1, y1, x4, y4, x2, y2);
   int side2 = point_side(x1, y1, x4, y4, x3, y3);

   float p1_x = (x1 + x2) * 0.5f;
   float p1_y = (y1 + y2) * 0.5f;
   float p2_x = (x2 + x3) * 0.5f;
   float p2_y = (y2 + y3) * 0.5f;
   float p3_x = (x3 + x4) * 0.5f;
   float p3_y = (y3 + y4) * 0.5f;

   float p4_x = (p1_x + p2_x) * 0.5f;
   float p4_y = (p1_y + p2_y) * 0.5f;
   float p5_x = (p2_x + p3_x) * 0.5f;
   float p5_y = (p2_y + p3_y) * 0.5f;

   float p6_x = (p4_x + p5_x) * 0.5f;
   float p6_y = (p4_y + p5_y) * 0.5f;

   float cx = (x1 + x2 + x3 + x4) * 0.25f;
   float cy = (y1 + y2 + y3 + y4) * 0.25f;
   float dx = p6_x - cx;
   float dy = p6_y - cy;
   float dist_sqr = dx*dx + dy*dy;

   int err;
   float tx, ty;

   if (side1 == 0 || side2 == 0 || (side1 == side2 && dist_sqr < max_dist_sqr) || level >= MAX_RECURSION) {
      err = array_append_reserve(coords, 7);
      if (err) return err;

      err = array_append_reserve(tangents, 6);
      if (err) return err;

      array_append_value(coords, fixscript_int(PART_CUBIC_TO));
      array_append_value(coords, fixscript_float(x2));
      array_append_value(coords, fixscript_float(y2));
      array_append_value(coords, fixscript_float(x3));
      array_append_value(coords, fixscript_float(y3));
      array_append_value(coords, fixscript_float(x4));
      array_append_value(coords, fixscript_float(y4));

      cubic_tangent(x1, y1, x2, y2, x3, y3, x4, y4, 0.0f, &tx, &ty);
      array_append_value(tangents, fixscript_float(tx));
      array_append_value(tangents, fixscript_float(ty));
      if (first_tangent) {
         first_tangent[0] = tx;
         first_tangent[1] = ty;
      }
      cubic_tangent(x1, y1, x2, y2, x3, y3, x4, y4, 0.5f, &tx, &ty);
      array_append_value(tangents, fixscript_float(tx));
      array_append_value(tangents, fixscript_float(ty));
      cubic_tangent(x1, y1, x2, y2, x3, y3, x4, y4, 1.0f, &tx, &ty);
      array_append_value(tangents, fixscript_float(tx));
      array_append_value(tangents, fixscript_float(ty));
      return FIXSCRIPT_SUCCESS;
   }

   err = cubic_split_offset(x1, y1, p1_x, p1_y, p4_x, p4_y, p6_x, p6_y, coords, tangents, max_dist_sqr, level+1, first_tangent);
   if (err) return err;

   err = cubic_split_offset(p6_x, p6_y, p5_x, p5_y, p3_x, p3_y, x4, y4, coords, tangents, max_dist_sqr, level+1, NULL);
   if (err) return err;

   return FIXSCRIPT_SUCCESS;
}


static Value shape_offset_subdivide(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value *coords = NULL;
   int coords_len;
   float max_dist_sqr;
   float first_x = 0.0f, first_y = 0.0f, first_tx = 0.0f, first_ty = 0.0f;
   float x0 = 0.0f, y0 = 0.0f;
   float x1, y1, x2, y2, x3, y3;
   float tx, ty, len, first_tangent[2];
   ArrayAppend coords_out, tangents_out;
   Value coords_out_val, ret_val = fixscript_int(0);
   int i, err, first=1;

   err = fixscript_get_array_length(heap, params[0], &coords_len);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   coords = malloc(coords_len * sizeof(Value));
   if (!coords) {
      goto out_of_memory_error;
   }

   err = fixscript_get_array_range(heap, params[0], 0, coords_len, coords);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   coords_out_val = fixscript_create_array(heap, 0);
   if (!coords_out_val.value) {
      goto out_of_memory_error;
   }

   err = array_append_init(&coords_out, heap, coords_out_val);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   err = array_append_init(&tangents_out, heap, params[2]);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   max_dist_sqr = fixscript_get_float(params[1]);
   max_dist_sqr *= max_dist_sqr;

   for (i=0; i<coords_len; i++) {
      switch (fixscript_get_int(coords[i])) {
         case PART_MOVE_TO:
            if (i+2 >= coords_len) goto garbled_coords_error;
            first_x = fixscript_get_float(coords[++i]);
            first_y = fixscript_get_float(coords[++i]);
            err = array_append_reserve(&coords_out, 3);
            if (err) {
               fixscript_error(heap, error, err);
               goto error;
            }
            array_append_value(&coords_out, fixscript_int(PART_MOVE_TO));
            array_append_value(&coords_out, fixscript_float(first_x));
            array_append_value(&coords_out, fixscript_float(first_y));
            x0 = first_x;
            y0 = first_y;
            first = 1;
            break;

         case PART_LINE_TO:
            if (i+2 >= coords_len) goto garbled_coords_error;
            x1 = fixscript_get_float(coords[++i]);
            y1 = fixscript_get_float(coords[++i]);

            err = array_append_reserve(&coords_out, 3);
            if (err) {
               fixscript_error(heap, error, err);
               goto error;
            }
            array_append_value(&coords_out, fixscript_int(PART_LINE_TO));
            array_append_value(&coords_out, fixscript_float(x1));
            array_append_value(&coords_out, fixscript_float(y1));

            err = array_append_reserve(&tangents_out, 2);
            if (err) {
               fixscript_error(heap, error, err);
               goto error;
            }
            tx = x1 - x0;
            ty = y1 - y0;
            len = sqrtf(tx*tx + ty*ty);
            if (len <= 0.000001f) len = 1.0f;
            tx /= len;
            ty /= len;
            array_append_value(&tangents_out, fixscript_float(tx));
            array_append_value(&tangents_out, fixscript_float(ty));
            if (first) {
               first_tx = tx;
               first_ty = ty;
            }

            x0 = x1;
            y0 = y1;
            first = 0;
            break;

         case PART_QUAD_TO:
            if (i+4 >= coords_len) goto garbled_coords_error;
            x1 = fixscript_get_float(coords[++i]);
            y1 = fixscript_get_float(coords[++i]);
            x2 = fixscript_get_float(coords[++i]);
            y2 = fixscript_get_float(coords[++i]);
            err = quad_split_offset(x0, y0, x1, y1, x2, y2, &coords_out, &tangents_out, max_dist_sqr, 0, first_tangent);
            if (err) {
               fixscript_error(heap, error, err);
               goto error;
            }
            if (first) {
               first_tx = first_tangent[0];
               first_ty = first_tangent[1];
            }
            x0 = x2;
            y0 = y2;
            first = 0;
            break;

         case PART_CUBIC_TO:
            if (i+6 >= coords_len) goto garbled_coords_error;
            x1 = fixscript_get_float(coords[++i]);
            y1 = fixscript_get_float(coords[++i]);
            x2 = fixscript_get_float(coords[++i]);
            y2 = fixscript_get_float(coords[++i]);
            x3 = fixscript_get_float(coords[++i]);
            y3 = fixscript_get_float(coords[++i]);
            err = cubic_split_offset(x0, y0, x1, y1, x2, y2, x3, y3, &coords_out, &tangents_out, max_dist_sqr, 0, first_tangent);
            if (err) {
               fixscript_error(heap, error, err);
               goto error;
            }
            if (first) {
               first_tx = first_tangent[0];
               first_ty = first_tangent[1];
            }
            x0 = x3;
            y0 = y3;
            first = 0;
            break;

         case PART_CLOSE_PATH:
            tx = first_x - x0;
            ty = first_y - y0;
            len = sqrtf(tx*tx + ty*ty);

            if (len > 0.000001f) {
               err = array_append_reserve(&coords_out, 3);
               if (err) {
                  fixscript_error(heap, error, err);
                  goto error;
               }
               array_append_value(&coords_out, fixscript_int(PART_LINE_TO));
               array_append_value(&coords_out, fixscript_float(first_x));
               array_append_value(&coords_out, fixscript_float(first_y));

               err = array_append_reserve(&tangents_out, 2);
               if (err) {
                  fixscript_error(heap, error, err);
                  goto error;
               }
               array_append_value(&tangents_out, fixscript_float(tx / len));
               array_append_value(&tangents_out, fixscript_float(ty / len));
            }

            err = array_append_reserve(&coords_out, 1);
            if (err) {
               fixscript_error(heap, error, err);
               goto error;
            }
            array_append_value(&coords_out, fixscript_int(PART_CLOSE_PATH));

            err = array_append_reserve(&tangents_out, 2);
            if (err) {
               fixscript_error(heap, error, err);
               goto error;
            }
            array_append_value(&tangents_out, fixscript_float(first_tx));
            array_append_value(&tangents_out, fixscript_float(first_ty));

            x0 = first_x;
            y0 = first_y;
            first = 1;
            break;

         default:
            goto garbled_coords_error;
      }
   }

   err = array_append_flush(&coords_out);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   err = array_append_flush(&tangents_out);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   ret_val = coords_out_val;

error:
   free(coords);
   return ret_val;

garbled_coords_error:
   *error = fixscript_create_error_string(heap, "garbled coordinate values");
   goto error;

out_of_memory_error:
   fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   goto error;
}


static Value shape_reverse(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value *coords = NULL, *rev_coords = NULL, ret_val = fixscript_int(0);
   int coords_len, rev_coords_len, len;
   float first_x = 0.0f, first_y = 0.0f;
   float x0 = 0.0f, y0 = 0.0f;
   float x1, y1, x2, y2, x3, y3;
   int i, err, idx, cnt=0, first=1, off=0;

   err = fixscript_get_array_length(heap, params[0], &coords_len);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   coords = malloc(coords_len * sizeof(Value));
   rev_coords = malloc((coords_len+3) * sizeof(Value));
   if (!coords || !rev_coords) {
      goto out_of_memory_error;
   }

   err = fixscript_get_array_range(heap, params[0], 0, coords_len, coords);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   rev_coords_len = coords_len;
   if (coords_len > 0 && (!fixscript_is_int(coords[0]) || fixscript_get_int(coords[0]) != PART_MOVE_TO)) {
      rev_coords_len += 3;
   }

   idx = rev_coords_len;

   for (i=0; i<coords_len; i++) {
      switch (fixscript_get_int(coords[i])) {
         case PART_MOVE_TO:
            if (!first) {
               idx -= 3;
               if (idx < 0) goto garbled_coords_error;
               rev_coords[idx+0] = fixscript_int(PART_MOVE_TO);
               rev_coords[idx+1] = fixscript_float(x0);
               rev_coords[idx+2] = fixscript_float(y0);
               if (cnt + (rev_coords_len - idx) > rev_coords_len) {
                  goto garbled_coords_error;
               }
               memmove(rev_coords + cnt, rev_coords + idx, (rev_coords_len - idx) * sizeof(Value));
               cnt += (rev_coords_len - idx);
               idx = rev_coords_len;
            }
            if (i+2 >= coords_len) goto garbled_coords_error;
            first_x = fixscript_get_float(coords[++i]);
            first_y = fixscript_get_float(coords[++i]);
            x0 = first_x;
            y0 = first_y;
            first = 1;
            break;

         case PART_LINE_TO:
            if (i+2 >= coords_len) goto garbled_coords_error;
            x1 = fixscript_get_float(coords[++i]);
            y1 = fixscript_get_float(coords[++i]);
            idx -= 3;
            if (idx < cnt) goto garbled_coords_error;
            rev_coords[idx+0] = fixscript_int(PART_LINE_TO);
            rev_coords[idx+1] = fixscript_float(x0);
            rev_coords[idx+2] = fixscript_float(y0);
            x0 = x1;
            y0 = y1;
            first = 0;
            break;

         case PART_QUAD_TO:
            if (i+4 >= coords_len) goto garbled_coords_error;
            x1 = fixscript_get_float(coords[++i]);
            y1 = fixscript_get_float(coords[++i]);
            x2 = fixscript_get_float(coords[++i]);
            y2 = fixscript_get_float(coords[++i]);
            idx -= 5;
            if (idx < cnt) goto garbled_coords_error;
            rev_coords[idx+0] = fixscript_int(PART_QUAD_TO);
            rev_coords[idx+1] = fixscript_float(x1);
            rev_coords[idx+2] = fixscript_float(y1);
            rev_coords[idx+3] = fixscript_float(x0);
            rev_coords[idx+4] = fixscript_float(y0);
            x0 = x2;
            y0 = y2;
            first = 0;
            break;

         case PART_CUBIC_TO:
            if (i+6 >= coords_len) goto garbled_coords_error;
            x1 = fixscript_get_float(coords[++i]);
            y1 = fixscript_get_float(coords[++i]);
            x2 = fixscript_get_float(coords[++i]);
            y2 = fixscript_get_float(coords[++i]);
            x3 = fixscript_get_float(coords[++i]);
            y3 = fixscript_get_float(coords[++i]);
            idx -= 7;
            if (idx < cnt) goto garbled_coords_error;
            rev_coords[idx+0] = fixscript_int(PART_CUBIC_TO);
            rev_coords[idx+1] = fixscript_float(x2);
            rev_coords[idx+2] = fixscript_float(y2);
            rev_coords[idx+3] = fixscript_float(x1);
            rev_coords[idx+4] = fixscript_float(y1);
            rev_coords[idx+5] = fixscript_float(x0);
            rev_coords[idx+6] = fixscript_float(y0);
            x0 = x3;
            y0 = y3;
            first = 0;
            break;

         case PART_CLOSE_PATH:
            if (!first) {
               idx -= 3;
               if (idx < 0) goto garbled_coords_error;
               rev_coords[idx+0] = fixscript_int(PART_MOVE_TO);
               rev_coords[idx+1] = fixscript_float(x0);
               rev_coords[idx+2] = fixscript_float(y0);
               if (cnt + (rev_coords_len - idx) + 1 > rev_coords_len) {
                  goto garbled_coords_error;
               }
               memmove(rev_coords + cnt, rev_coords + idx, (rev_coords_len - idx) * sizeof(Value));
               cnt += (rev_coords_len - idx);
               rev_coords[cnt++] = fixscript_int(PART_CLOSE_PATH);
               idx = rev_coords_len;
            }
            x0 = first_x;
            y0 = first_y;
            first = 1;
            break;

         default:
            goto garbled_coords_error;
      }
   }

   if (!first) {
      idx -= 3;
      if (idx < cnt) goto garbled_coords_error;
      rev_coords[idx+0] = fixscript_int(PART_MOVE_TO);
      rev_coords[idx+1] = fixscript_float(x0);
      rev_coords[idx+2] = fixscript_float(y0);
      memmove(rev_coords + cnt, rev_coords + idx, (rev_coords_len - idx) * sizeof(Value));
      cnt += (rev_coords_len - idx);
   }
   
   /*
   if (cnt != rev_coords_len) {
      goto garbled_coords_error;
   }
   */

   if (params[2].value) {
      cnt -= 3;
      if (cnt < 0) goto garbled_coords_error;
      off = 3;
   }

   err = fixscript_get_array_length(heap, params[1], &len);
   if (!err) {
      err = fixscript_set_array_length(heap, params[1], len + cnt);
   }
   if (!err) {
      err = fixscript_set_array_range(heap, params[1], len, cnt, rev_coords + off);
   }
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   ret_val = params[1];

error:
   free(coords);
   free(rev_coords);
   return ret_val;

garbled_coords_error:
   *error = fixscript_create_error_string(heap, "garbled coordinate values");
   goto error;

out_of_memory_error:
   fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   goto error;
}


static Value image_to_png(Heap *heap, Value *error, int num_params, Value *params, void *func_data)
{
   ImageData *data;
   Value ret;
   unsigned char *dest;
   int dest_len;

   data = get_image_data(heap, error, params[0]);
   if (!data) {
      return fixscript_int(0);
   }

   if (!save_png(data->pixels, data->stride, data->width, data->height, &dest, &dest_len)) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   ret = fixscript_create_or_get_shared_array(heap, -1, dest, dest_len, 1, free, dest, NULL);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return ret;
}


static Value image_load(Heap *heap, Value *error, int num_params, Value *params, void *func_data)
{
   int err, width=0, height=0, len;
   uint32_t *pixels;
   unsigned char *buf;

   err = fixscript_get_array_length(heap, params[0], &len);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   err = fixscript_lock_array(heap, params[0], 0, len, (void **)&buf, 1, ACCESS_READ_ONLY);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   pixels = load_png(buf, len, &width, &height);
   fixscript_unlock_array(heap, params[0], 0, len, (void **)&buf, 1, ACCESS_READ_ONLY);

   if (!pixels) {
      *error = fixscript_create_error_string(heap, "cannot read image");
      return fixscript_int(0);
   }

   if (width > MAX_IMAGE_DIM || height > MAX_IMAGE_DIM) {
      free(pixels);
      *error = fixscript_create_error_string(heap, "image dimensions are too big");
      return fixscript_int(0);
   }

   return image_create_internal(heap, error, width, height, width, pixels, 1, NULL, NULL, NULL, -1);
}


static FORCE_INLINE void blur_horiz(uint32_t *dest, uint32_t *src, int width, int irx, int frac_x)
{
   int acc_a, acc_r, acc_g, acc_b, a, r, g, b, pa1, pr1, pg1, pb1, pa2, pr2, pg2, pb2;
   uint32_t c;
   float idiv;
   int i, wx;

   wx = ((irx*2+1)<<8) + frac_x*2;
   idiv = 1.0f / wx;

   c = src[0];
   pa1 = (c >> 24) & 0xFF;
   pr1 = (c >> 16) & 0xFF;
   pg1 = (c >>  8) & 0xFF;
   pb1 = (c >>  0) & 0xFF;
   acc_a = pa1 * ((irx+1) << 8) + pa1 * frac_x;
   acc_r = pr1 * ((irx+1) << 8) + pr1 * frac_x;
   acc_g = pg1 * ((irx+1) << 8) + pg1 * frac_x;
   acc_b = pb1 * ((irx+1) << 8) + pb1 * frac_x;

   for (i=1; i<irx+1; i++) {
      c = src[MIN(i, width-1)];
      acc_a += ((c >> 24) & 0xFF) << 8;
      acc_r += ((c >> 16) & 0xFF) << 8;
      acc_g += ((c >>  8) & 0xFF) << 8;
      acc_b += ((c >>  0) & 0xFF) << 8;
   }

   c = src[MIN(irx+1, width-1)];
   pa2 = (c >> 24) & 0xFF;
   pr2 = (c >> 16) & 0xFF;
   pg2 = (c >>  8) & 0xFF;
   pb2 = (c >>  0) & 0xFF;
   acc_a += pa2 * frac_x;
   acc_r += pr2 * frac_x;
   acc_g += pg2 * frac_x;
   acc_b += pb2 * frac_x;

   for (i=0; i<width; i++) {
      a = acc_a * idiv;
      r = acc_r * idiv;
      g = acc_g * idiv;
      b = acc_b * idiv;
      dest[i] = (a << 24) | (r << 16) | (g << 8) | b;

      acc_a -= pa1 * frac_x;
      acc_r -= pr1 * frac_x;
      acc_g -= pg1 * frac_x;
      acc_b -= pb1 * frac_x;
      c = src[MAX(0, i-irx)];
      pa1 = (c >> 24) & 0xFF;
      pr1 = (c >> 16) & 0xFF;
      pg1 = (c >>  8) & 0xFF;
      pb1 = (c >>  0) & 0xFF;
      acc_a -= pa1 * (256 - frac_x);
      acc_r -= pr1 * (256 - frac_x);
      acc_g -= pg1 * (256 - frac_x);
      acc_b -= pb1 * (256 - frac_x);

      acc_a += pa2 * (256 - frac_x);
      acc_r += pr2 * (256 - frac_x);
      acc_g += pg2 * (256 - frac_x);
      acc_b += pb2 * (256 - frac_x);
      c = src[MIN(i+irx+2, width-1)];
      pa2 = (c >> 24) & 0xFF;
      pr2 = (c >> 16) & 0xFF;
      pg2 = (c >>  8) & 0xFF;
      pb2 = (c >>  0) & 0xFF;
      acc_a += pa2 * frac_x;
      acc_r += pr2 * frac_x;
      acc_g += pg2 * frac_x;
      acc_b += pb2 * frac_x;
   }
}


#ifdef __SSE2__
static FORCE_INLINE void blur_horiz_sse2(uint32_t *dest, int dest_stride, uint32_t *src, int src_stride, int width, int irx, int frac_x)
{
   union {
      __m128i m;
      uint32_t i[4];
   } u;
   __m128 acc0, acc1, acc2, acc3, factor, idiv, frac, ifrac;
   __m128i c, p1, p2, tmp0, tmp1, tmp2, tmp3;
   uint32_t *ptr;
   int i, wx;

   wx = ((irx*2+1)<<8) + frac_x*2;
   idiv = _mm_set_ps1(1.0f / wx);
   frac = _mm_set_ps1(frac_x);
   ifrac = _mm_set_ps1(256 - frac_x);

   #define UNPACK0(src) _mm_cvtepi32_ps(_mm_unpacklo_epi16(_mm_unpacklo_epi8(src, _mm_setzero_si128()), _mm_setzero_si128()))
   #define UNPACK1(src) _mm_cvtepi32_ps(_mm_unpackhi_epi16(_mm_unpacklo_epi8(src, _mm_setzero_si128()), _mm_setzero_si128()))
   #define UNPACK2(src) _mm_cvtepi32_ps(_mm_unpacklo_epi16(_mm_unpackhi_epi8(src, _mm_setzero_si128()), _mm_setzero_si128()))
   #define UNPACK3(src) _mm_cvtepi32_ps(_mm_unpackhi_epi16(_mm_unpackhi_epi8(src, _mm_setzero_si128()), _mm_setzero_si128()))

   #define UNPACK0_SHL8(src) _mm_cvtepi32_ps(_mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_setzero_si128(), src), _mm_setzero_si128()))
   #define UNPACK1_SHL8(src) _mm_cvtepi32_ps(_mm_unpackhi_epi16(_mm_unpacklo_epi8(_mm_setzero_si128(), src), _mm_setzero_si128()))
   #define UNPACK2_SHL8(src) _mm_cvtepi32_ps(_mm_unpacklo_epi16(_mm_unpackhi_epi8(_mm_setzero_si128(), src), _mm_setzero_si128()))
   #define UNPACK3_SHL8(src) _mm_cvtepi32_ps(_mm_unpackhi_epi16(_mm_unpackhi_epi8(_mm_setzero_si128(), src), _mm_setzero_si128()))

   p1 = _mm_set_epi32(src[3*src_stride], src[2*src_stride], src[1*src_stride], src[0*src_stride]);
   factor = _mm_set_ps1(((irx+1) << 8) + frac_x);
   acc0 = _mm_mul_ps(UNPACK0(p1), factor);
   acc1 = _mm_mul_ps(UNPACK1(p1), factor);
   acc2 = _mm_mul_ps(UNPACK2(p1), factor);
   acc3 = _mm_mul_ps(UNPACK3(p1), factor);

   for (i=1; i<irx+1; i++) {
      ptr = src + MIN(i, width-1);
      c = _mm_set_epi32(ptr[3*src_stride], ptr[2*src_stride], ptr[1*src_stride], ptr[0*src_stride]);
      acc0 = _mm_add_ps(acc0, UNPACK0_SHL8(c));
      acc1 = _mm_add_ps(acc1, UNPACK1_SHL8(c));
      acc2 = _mm_add_ps(acc2, UNPACK2_SHL8(c));
      acc3 = _mm_add_ps(acc3, UNPACK3_SHL8(c));
   }

   ptr = src + MIN(irx+1, width-1);
   p2 = _mm_set_epi32(ptr[3*src_stride], ptr[2*src_stride], ptr[1*src_stride], ptr[0*src_stride]);
   acc0 = _mm_add_ps(acc0, _mm_mul_ps(UNPACK0(p2), frac));
   acc1 = _mm_add_ps(acc1, _mm_mul_ps(UNPACK1(p2), frac));
   acc2 = _mm_add_ps(acc2, _mm_mul_ps(UNPACK2(p2), frac));
   acc3 = _mm_add_ps(acc3, _mm_mul_ps(UNPACK3(p2), frac));

   for (i=0; i<width; i++) {
      tmp0 = _mm_cvtps_epi32(_mm_mul_ps(acc0, idiv));
      tmp1 = _mm_cvtps_epi32(_mm_mul_ps(acc1, idiv));
      tmp2 = _mm_cvtps_epi32(_mm_mul_ps(acc2, idiv));
      tmp3 = _mm_cvtps_epi32(_mm_mul_ps(acc3, idiv));
      u.m = _mm_packus_epi16(_mm_packs_epi32(tmp0, tmp1), _mm_packs_epi32(tmp2, tmp3));
      ptr = dest + i;
      ptr[0*dest_stride] = u.i[0];
      ptr[1*dest_stride] = u.i[1];
      ptr[2*dest_stride] = u.i[2];
      ptr[3*dest_stride] = u.i[3];

      acc0 = _mm_sub_ps(acc0, _mm_mul_ps(UNPACK0(p1), frac));
      acc1 = _mm_sub_ps(acc1, _mm_mul_ps(UNPACK1(p1), frac));
      acc2 = _mm_sub_ps(acc2, _mm_mul_ps(UNPACK2(p1), frac));
      acc3 = _mm_sub_ps(acc3, _mm_mul_ps(UNPACK3(p1), frac));
      ptr = src + MAX(0, i-irx);
      p1 = _mm_set_epi32(ptr[3*src_stride], ptr[2*src_stride], ptr[1*src_stride], ptr[0*src_stride]);
      acc0 = _mm_sub_ps(acc0, _mm_mul_ps(UNPACK0(p1), ifrac));
      acc1 = _mm_sub_ps(acc1, _mm_mul_ps(UNPACK1(p1), ifrac));
      acc2 = _mm_sub_ps(acc2, _mm_mul_ps(UNPACK2(p1), ifrac));
      acc3 = _mm_sub_ps(acc3, _mm_mul_ps(UNPACK3(p1), ifrac));

      acc0 = _mm_add_ps(acc0, _mm_mul_ps(UNPACK0(p2), ifrac));
      acc1 = _mm_add_ps(acc1, _mm_mul_ps(UNPACK1(p2), ifrac));
      acc2 = _mm_add_ps(acc2, _mm_mul_ps(UNPACK2(p2), ifrac));
      acc3 = _mm_add_ps(acc3, _mm_mul_ps(UNPACK3(p2), ifrac));
      ptr = src + MIN(i+irx+2, width-1);
      p2 = _mm_set_epi32(ptr[3*src_stride], ptr[2*src_stride], ptr[1*src_stride], ptr[0*src_stride]);
      acc0 = _mm_add_ps(acc0, _mm_mul_ps(UNPACK0(p2), frac));
      acc1 = _mm_add_ps(acc1, _mm_mul_ps(UNPACK1(p2), frac));
      acc2 = _mm_add_ps(acc2, _mm_mul_ps(UNPACK2(p2), frac));
      acc3 = _mm_add_ps(acc3, _mm_mul_ps(UNPACK3(p2), frac));
   }

   #undef UNPACK0
   #undef UNPACK1
   #undef UNPACK2
   #undef UNPACK3

   #undef UNPACK0_SHL8
   #undef UNPACK1_SHL8
   #undef UNPACK2_SHL8
   #undef UNPACK3_SHL8
}

static FORCE_INLINE void blur_vert_sse2(uint32_t *dest, int dest_stride, uint32_t *src, int src_stride, int height, int iry, int frac_y)
{
   __m128 acc0, acc1, acc2, acc3, factor, idiv, frac, ifrac;
   __m128i c, tmp, p1, p2, tmp0, tmp1, tmp2, tmp3;
   int i, wy;

   wy = ((iry*2+1)<<8) + frac_y*2;
   idiv = _mm_set_ps1(1.0f / wy);
   frac = _mm_set_ps1(frac_y);
   ifrac = _mm_set_ps1(256 - frac_y);

   #define UNPACK0(src) _mm_cvtepi32_ps(_mm_unpacklo_epi16(_mm_unpacklo_epi8(src, _mm_setzero_si128()), _mm_setzero_si128()))
   #define UNPACK1(src) _mm_cvtepi32_ps(_mm_unpackhi_epi16(_mm_unpacklo_epi8(src, _mm_setzero_si128()), _mm_setzero_si128()))
   #define UNPACK2(src) _mm_cvtepi32_ps(_mm_unpacklo_epi16(_mm_unpackhi_epi8(src, _mm_setzero_si128()), _mm_setzero_si128()))
   #define UNPACK3(src) _mm_cvtepi32_ps(_mm_unpackhi_epi16(_mm_unpackhi_epi8(src, _mm_setzero_si128()), _mm_setzero_si128()))

   #define UNPACK0_SHL8(src) _mm_cvtepi32_ps(_mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_setzero_si128(), src), _mm_setzero_si128()))
   #define UNPACK1_SHL8(src) _mm_cvtepi32_ps(_mm_unpackhi_epi16(_mm_unpacklo_epi8(_mm_setzero_si128(), src), _mm_setzero_si128()))
   #define UNPACK2_SHL8(src) _mm_cvtepi32_ps(_mm_unpacklo_epi16(_mm_unpackhi_epi8(_mm_setzero_si128(), src), _mm_setzero_si128()))
   #define UNPACK3_SHL8(src) _mm_cvtepi32_ps(_mm_unpackhi_epi16(_mm_unpackhi_epi8(_mm_setzero_si128(), src), _mm_setzero_si128()))

   p1 = _mm_loadu_si128((__m128i *)src);
   factor = _mm_set_ps1(((iry+1) << 8) + frac_y);
   acc0 = _mm_mul_ps(UNPACK0(p1), factor);
   acc1 = _mm_mul_ps(UNPACK1(p1), factor);
   acc2 = _mm_mul_ps(UNPACK2(p1), factor);
   acc3 = _mm_mul_ps(UNPACK3(p1), factor);

   for (i=1; i<iry+1; i++) {
      c = _mm_loadu_si128((__m128i *)&src[MIN(i, height-1) * src_stride]);
      acc0 = _mm_add_ps(acc0, UNPACK0_SHL8(c));
      acc1 = _mm_add_ps(acc1, UNPACK1_SHL8(c));
      acc2 = _mm_add_ps(acc2, UNPACK2_SHL8(c));
      acc3 = _mm_add_ps(acc3, UNPACK3_SHL8(c));
   }

   p2 = _mm_loadu_si128((__m128i *)&src[MIN(iry+1, height-1) * src_stride]);
   acc0 = _mm_add_ps(acc0, _mm_mul_ps(UNPACK0(p2), frac));
   acc1 = _mm_add_ps(acc1, _mm_mul_ps(UNPACK1(p2), frac));
   acc2 = _mm_add_ps(acc2, _mm_mul_ps(UNPACK2(p2), frac));
   acc3 = _mm_add_ps(acc3, _mm_mul_ps(UNPACK3(p2), frac));

   for (i=0; i<height; i++) {
      tmp0 = _mm_cvtps_epi32(_mm_mul_ps(acc0, idiv));
      tmp1 = _mm_cvtps_epi32(_mm_mul_ps(acc1, idiv));
      tmp2 = _mm_cvtps_epi32(_mm_mul_ps(acc2, idiv));
      tmp3 = _mm_cvtps_epi32(_mm_mul_ps(acc3, idiv));
      tmp = _mm_packus_epi16(_mm_packs_epi32(tmp0, tmp1), _mm_packs_epi32(tmp2, tmp3));
      _mm_storeu_si128((__m128i *)&dest[i*dest_stride], tmp);

      acc0 = _mm_sub_ps(acc0, _mm_mul_ps(UNPACK0(p1), frac));
      acc1 = _mm_sub_ps(acc1, _mm_mul_ps(UNPACK1(p1), frac));
      acc2 = _mm_sub_ps(acc2, _mm_mul_ps(UNPACK2(p1), frac));
      acc3 = _mm_sub_ps(acc3, _mm_mul_ps(UNPACK3(p1), frac));
      p1 = _mm_loadu_si128((__m128i *)&src[MAX(0, i-iry) * src_stride]);
      acc0 = _mm_sub_ps(acc0, _mm_mul_ps(UNPACK0(p1), ifrac));
      acc1 = _mm_sub_ps(acc1, _mm_mul_ps(UNPACK1(p1), ifrac));
      acc2 = _mm_sub_ps(acc2, _mm_mul_ps(UNPACK2(p1), ifrac));
      acc3 = _mm_sub_ps(acc3, _mm_mul_ps(UNPACK3(p1), ifrac));

      acc0 = _mm_add_ps(acc0, _mm_mul_ps(UNPACK0(p2), ifrac));
      acc1 = _mm_add_ps(acc1, _mm_mul_ps(UNPACK1(p2), ifrac));
      acc2 = _mm_add_ps(acc2, _mm_mul_ps(UNPACK2(p2), ifrac));
      acc3 = _mm_add_ps(acc3, _mm_mul_ps(UNPACK3(p2), ifrac));
      p2 = _mm_loadu_si128((__m128i *)&src[MIN(i+iry+2, height-1) * src_stride]);
      acc0 = _mm_add_ps(acc0, _mm_mul_ps(UNPACK0(p2), frac));
      acc1 = _mm_add_ps(acc1, _mm_mul_ps(UNPACK1(p2), frac));
      acc2 = _mm_add_ps(acc2, _mm_mul_ps(UNPACK2(p2), frac));
      acc3 = _mm_add_ps(acc3, _mm_mul_ps(UNPACK3(p2), frac));
   }

   #undef UNPACK0
   #undef UNPACK1
   #undef UNPACK2
   #undef UNPACK3

   #undef UNPACK0_SHL8
   #undef UNPACK1_SHL8
   #undef UNPACK2_SHL8
   #undef UNPACK3_SHL8
}
#endif /* __SSE2__ */


typedef struct {
   int width, height, stride;
   uint32_t *pixels;
   int steps;
   int irx, iry, frac_x, frac_y;
} BlurData;

static void blur_horiz_pass(int from, int to, void *data)
{
   BlurData *bd = data;
   uint32_t *p, *line, *src, *dest, *tmp;
   int i, j;

   from *= 4;
   to *= 4;
   if (to > bd->height) {
      to = bd->height;
   }

   line = malloc(MAX(bd->width, bd->height) * (1+16) * sizeof(uint32_t));

#ifdef __SSE2__
   for (i=from; i<to-3; i+=4) {
      p = bd->pixels + i*bd->stride;
      src = p;
      dest = line;
      for (j=0; j<bd->steps; j++) {
         blur_horiz_sse2(dest, dest == line? bd->width : bd->stride, src, src == line? bd->width : bd->stride, bd->width, bd->irx, bd->frac_x);
         tmp = src;
         src = dest;
         dest = tmp;
      }
      if (src != p) {
         for (j=0; j<4; j++) {
            memcpy(p + j*bd->stride, line + j*bd->width, bd->width*4);
         }
      }
   }
#else
   i = from;
#endif
   p = bd->pixels + i*bd->stride;
   for (; i<to; i++) {
      src = p;
      dest = line;
      for (j=0; j<bd->steps; j++) {
         blur_horiz(dest, src, bd->width, bd->irx, bd->frac_x);
         tmp = src;
         src = dest;
         dest = tmp;
      }
      if (src != p) {
         memcpy(p, line, bd->width * sizeof(uint32_t));
      }
      p += bd->stride;
   }

   free(line);
}

static void blur_vert_pass(int from, int to, void *data)
{
   BlurData *bd = data;
   uint32_t *p, *line, *src, *dest, *tmp;
   int i, j, k;

   from *= 4;
   to *= 4;
   if (to > bd->width) {
      to = bd->width;
   }

   line = malloc(MAX(bd->width, bd->height) * (1+16) * sizeof(uint32_t));
#ifdef __SSE2__
   for (i=from; i<to-3; i+=4) {
      p = bd->pixels + i;
      src = p;
      dest = line;
      for (j=0; j<bd->steps; j++) {
         blur_vert_sse2(dest, dest == line? 4 : bd->stride, src, src == line? 4 : bd->stride, bd->height, bd->iry, bd->frac_y);
         tmp = src;
         src = dest;
         dest = tmp;
      }
      if (src != p) {
         for (j=0; j<bd->height; j++) {
            memcpy(p + j*bd->stride, line + j*4, 4*4);
         }
      }
   }
#else
   i = from;
#endif

   for (; i<to; i+=16) {
      for (j=0; j<bd->height; j++) {
         p = bd->pixels + j*bd->stride + i;
         for (k=i; k<MIN(i+16, bd->width); k++) {
            line[(k-i+1)*bd->height+j] = *p++;
         }
      }

      p = line + bd->height;
      for (k=i; k<MIN(i+16, bd->width); k++) {
         src = p;
         dest = line;
         for (j=0; j<bd->steps; j++) {
            blur_horiz(dest, src, bd->height, bd->iry, bd->frac_y);
            tmp = src;
            src = dest;
            dest = tmp;
         }
         if (src != p) {
            memcpy(p, line, bd->height * sizeof(uint32_t));
         }
         p += bd->height;
      }

      for (j=0; j<bd->height; j++) {
         p = bd->pixels + j*bd->stride + i;
         for (k=i; k<MIN(i+16, bd->width); k++) {
            *p++ = line[(k-i+1)*bd->height+j];
         }
      }
   }

   free(line);
}


static Value image_blur_box(Heap *heap, Value *error, int num_params, Value *params, void *func_data)
{
   ImageData *data;
   float rx, ry;
   int steps;
   BlurData bd;

   data = get_image_data(heap, error, params[0]);
   if (!data) {
      return fixscript_int(0);
   }

   rx = fixscript_get_float(params[1]);
   ry = fixscript_get_float(params[2]);
   steps = fixscript_get_int(params[3]);

   if (rx < 0.0f || ry < 0.0f) {
      *error = fixscript_create_error_string(heap, "negative radius");
      return fixscript_int(0);
   }

   if (steps < 0) {
      *error = fixscript_create_error_string(heap, "negative steps");
      return fixscript_int(0);
   }

   if (steps == 0 || (rx == 0.0f && ry == 0.0f)) {
      return fixscript_int(0);
   }

   bd.width = data->width;
   bd.height = data->height;
   bd.stride = data->stride;
   bd.pixels = data->pixels;
   bd.steps = steps;
   bd.irx = (int)rx;
   bd.iry = (int)ry;
   bd.frac_x = (rx - (int)rx) * 256.0f + 0.5f;
   bd.frac_y = (ry - (int)ry) * 256.0f + 0.5f;
   
   fiximage_multicore_run(0, (data->height+3)/4, 100000/data->width/4, blur_horiz_pass, &bd);
   fiximage_multicore_run(0, (data->width+3)/4, 100000/data->height/4, blur_vert_pass, &bd);
   return fixscript_int(0);
}


typedef struct {
   uint32_t *pixels;
   int width, height, stride;
   uint8_t *table;
} RemapData;


static void remap_color1(int from, int to, void *data)
{
   RemapData *rd = data;
   uint32_t pixel;
   int pa, pr, pg, pb;
   int i, j;

   for (i=from; i<to; i++) {
      for (j=0; j<rd->width; j++) {
         pixel = rd->pixels[i*rd->stride+j];
         pa = (pixel >> 24) & 0xFF;
         pr = (pixel >> 16) & 0xFF;
         pg = (pixel >>  8) & 0xFF;
         pb = (pixel >>  0) & 0xFF;
         pr = rd->table[pr];
         pg = rd->table[pg];
         pb = rd->table[pb];
         rd->pixels[i*rd->stride+j] = (pa << 24) | (pr << 16) | (pg << 8) | pb;
      }
   }
}


static void remap_color3(int from, int to, void *data)
{
   RemapData *rd = data;
   uint32_t pixel;
   int pa, pr, pg, pb;
   int i, j;

   for (i=from; i<to; i++) {
      for (j=0; j<rd->width; j++) {
         pixel = rd->pixels[i*rd->stride+j];
         pa = (pixel >> 24) & 0xFF;
         pr = (pixel >> 16) & 0xFF;
         pg = (pixel >>  8) & 0xFF;
         pb = (pixel >>  0) & 0xFF;
         pr = rd->table[pr+0*256];
         pg = rd->table[pg+1*256];
         pb = rd->table[pb+2*256];
         rd->pixels[i*rd->stride+j] = (pa << 24) | (pr << 16) | (pg << 8) | pb;
      }
   }
}


static Value image_remap_color_ramps(Heap *heap, Value *error, int num_params, Value *params, void *func_data)
{
   ImageData *data;
   RemapData rd;
   int err, len;

   data = get_image_data(heap, error, params[0]);
   if (!data) {
      return fixscript_int(0);
   }

   err = fixscript_get_array_length(heap, params[1], &len);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   if (len != 256 && len != 768) {
      *error = fixscript_create_error_string(heap, "table must have either 256 or 768 entries");
      return fixscript_int(0);
   }

   err = fixscript_lock_array(heap, params[1], 0, len, (void **)&rd.table, 1, ACCESS_READ_ONLY);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   rd.pixels = data->pixels;
   rd.width = data->width;
   rd.height = data->height;
   rd.stride = data->stride;

   fiximage_multicore_run(0, data->height, 100000/data->width, len == 256? remap_color1 : remap_color3, &rd);

   fixscript_unlock_array(heap, params[1], 0, len, (void **)&rd.table, 1, ACCESS_READ_ONLY);
   return fixscript_int(0);
}


void fiximage_register_functions(Heap *heap)
{
   fixscript_register_handle_types(&handles_offset, NUM_HANDLE_TYPES);
   
   fixscript_register_native_func(heap, "image_create#2", image_create, NULL);
   fixscript_register_native_func(heap, "image_clone#1", image_clone, NULL);
   fixscript_register_native_func(heap, "image_get_subimage#5", image_get_subimage, NULL);
   fixscript_register_native_func(heap, "painter_create#1", painter_create, NULL);
   fixscript_register_native_func(heap, "painter_clear_rect#6", painter_fill_rect, (void *)0);
   fixscript_register_native_func(heap, "painter_fill_rect#6", painter_fill_rect, (void *)1);
   fixscript_register_native_func(heap, "painter_fill_rect#7", painter_fill_rect, (void *)2);
   fixscript_register_native_func(heap, "painter_fill_shape#3", painter_fill_shape, NULL);
   fixscript_register_native_func(heap, "painter_fill_shape#4", painter_fill_shape, NULL);
   fixscript_register_native_func(heap, "painter_batch_begin#1", painter_batch_begin, NULL);
   fixscript_register_native_func(heap, "painter_batch_flush#1", painter_batch_flush, NULL);
   fixscript_register_native_func(heap, "painter_batch_end#1", painter_batch_end, NULL);
   fixscript_register_native_func(heap, "shape_hit_test#3", shape_hit_test, NULL);
   fixscript_register_native_func(heap, "shape_offset_subdivide#3", shape_offset_subdivide, NULL);
   fixscript_register_native_func(heap, "shape_reverse#3", shape_reverse, NULL);
   fixscript_register_native_func(heap, "image_to_png#1", image_to_png, NULL);
   fixscript_register_native_func(heap, "image_load#1", image_load, NULL);
   fixscript_register_native_func(heap, "image_blur_box#4", image_blur_box, NULL);
   fixscript_register_native_func(heap, "image_remap_color_ramps#2", image_remap_color_ramps, NULL);
}


/*
The Canonical Huffman decompression works as follow:

1. the code length is obtained for each symbol
2. the number of symbols for each code length is computed (ignoring zero code lengths)
3. sorted table of symbols is created, it is sorted by code length
4. when decoding the different code lengths are iterated over, with these steps:
   a) starting code word is computed for given code length
   b) code word is matched when current code word matches the interval of values for current code length
   c) the index to the sorted table is simply incremented by the count of symbols for given code length
*/

static int zlib_uncompress(const unsigned char *src, int src_len, unsigned char **dest_out, int *dest_len_out, int init_len, int max_dest_len)
{
   #define GET_BITS(dest, nb)                                         \
   {                                                                  \
      while (num_bits < nb) {                                         \
         if (src == end) goto error;                                  \
         bits |= (*src++) << num_bits;                                \
         num_bits += 8;                                               \
      }                                                               \
      dest = bits & ((1 << (nb))-1);                                  \
      bits >>= nb;                                                    \
      num_bits -= nb;                                                 \
   }

   #define HUFF_BUILD(lengths, num_symbols, max_len, symbols, counts) \
   {                                                                  \
      int i, j, cnt=0;                                                \
                                                                      \
      for (i=1; i<max_len; i++) {                                     \
         for (j=0; j<(num_symbols); j++) {                            \
            if ((lengths)[j] == i) {                                  \
               symbols[cnt++] = j;                                    \
            }                                                         \
         }                                                            \
      }                                                               \
      if (cnt == 0) goto error;                                       \
                                                                      \
      memset(counts, 0, sizeof(counts));                              \
      for (i=0; i<(num_symbols); i++) {                               \
         counts[(lengths)[i]]++;                                      \
      }                                                               \
      counts[0] = 0;                                                  \
   }

   #define HUFF_DECODE(sym, symbols, counts, max_len)                 \
   {                                                                  \
      int bit, match_bits=0, idx=0, code=0, i;                        \
      sym = -1;                                                       \
      for (i=1; i<max_len; i++) {                                     \
         GET_BITS(bit, 1);                                            \
         match_bits = (match_bits << 1) | bit;                        \
         code = (code + counts[i-1]) << 1;                            \
         if (match_bits >= code && match_bits < code + counts[i]) {   \
            sym = symbols[idx + (match_bits - code)];                 \
            break;                                                    \
         }                                                            \
         idx += counts[i];                                            \
      }                                                               \
      if (sym == -1) goto error;                                      \
   }

   #define PUT_BYTE(val)                                              \
   {                                                                  \
      if (out_len == out_cap) {                                       \
         if (out_cap >= (1<<29)) goto error;                          \
         out_cap <<= 1;                                               \
         if (out_cap > max_dest_len) {                                \
            out_cap = max_dest_len;                                   \
            if (out_len >= out_cap) goto error;                       \
         }                                                            \
         new_out = realloc(out, out_cap);                             \
         if (!new_out) goto error;                                    \
         out = new_out;                                               \
      }                                                               \
      out[out_len++] = val;                                           \
   }

   static const uint8_t prelength_reorder[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
   static const uint16_t len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
   static const uint8_t  len_bits[29] = { 0, 0, 0, 0, 0, 0, 0,  0,  1,  1,  1,  1,  2,  2,  2,  2,  3,  3,  3,  3,  4,  4,  4,   4,   5,   5,   5,   5,   0 };
   static const uint16_t dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
   static const uint8_t  dist_bits[30] = { 0, 0, 0, 0, 1, 1, 2,  2,  3,  3,  4,  4,  5,  5,   6,   6,   7,   7,   8,   8,    9,    9,   10,   10,   11,   11,   12,    12,    13,    13 };
   
   const unsigned char *end = src + src_len;
   uint32_t bits = 0;
   int num_bits = 0;

   unsigned char *out = NULL, *new_out;
   int out_len=0, out_cap=0;
   
   int final, type;
   int len, nlen;
   int hlit, hdist, hclen, pos, limit;
   
   uint8_t prelengths[19], precounts[8], presymbols[19];
   uint8_t lengths[320];
   uint16_t lit_symbols[288], lit_counts[16];
   uint8_t dist_symbols[32], dist_counts[16];

   int i, sym, dist;

   out_cap = init_len;
   out = malloc(out_cap);
   if (!out) goto error;

   for (;;) {
      GET_BITS(final, 1);
      GET_BITS(type, 2);
      if (type == 3) goto error;

      if (type == 0) {
         // no compression:

         bits = 0;
         num_bits = 0;

         if (end - src < 4) goto error;
         len = src[0] | (src[1] << 8);
         nlen = src[2] | (src[3] << 8);
         if (len != ((~nlen) & 0xFFFF)) goto error;
         src += 4;
         if (end - src < len) goto error;
         for (i=0; i<len; i++) {
            PUT_BYTE(*src++);
         }
         if (final) break;
         continue;
      }

      if (type == 2) {
         // dynamic tree:

         GET_BITS(hlit, 5);
         GET_BITS(hdist, 5);
         GET_BITS(hclen, 4);

         limit = 257 + hlit + 1 + hdist;

         for (i=0; i<4+hclen; i++) {
            GET_BITS(prelengths[prelength_reorder[i]], 3);
         }
         for (; i<19; i++) {
            prelengths[prelength_reorder[i]] = 0;
         }
         HUFF_BUILD(prelengths, 19, 8, presymbols, precounts);

         pos = 0;
         while (pos < limit) {
            HUFF_DECODE(sym, presymbols, precounts, 8);
            if (sym < 16) {
               lengths[pos++] = sym;
            }
            else if (sym == 16) {
               GET_BITS(len, 2);
               len += 3;
               if (pos == 0 || pos + len > limit) goto error;
               for (i=0; i<len; i++) {
                  lengths[pos+i] = lengths[pos-1];
               }
               pos += len;
            }
            else if (sym == 17) {
               GET_BITS(len, 3);
               len += 3;
               if (pos + len > limit) goto error;
               for (i=0; i<len; i++) {
                  lengths[pos++] = 0;
               }
            }
            else if (sym == 18) {
               GET_BITS(len, 7);
               len += 11;
               if (pos + len > limit) goto error;
               for (i=0; i<len; i++) {
                  lengths[pos++] = 0;
               }
            }
            else goto error;
         }

         if (lengths[256] == 0) goto error;
      }
      else {
         // static tree:

         for (i=0; i<144; i++) {
            lengths[i] = 8;
         }
         for (i=144; i<256; i++) {
            lengths[i] = 9;
         }
         for (i=256; i<280; i++) {
            lengths[i] = 7;
         }
         for (i=280; i<288; i++) {
            lengths[i] = 8;
         }
         for (i=288; i<320; i++) {
            lengths[i] = 5;
         }
         hlit = 31;
         hdist = 31;
      }

      HUFF_BUILD(lengths, 257+hlit, 16, lit_symbols, lit_counts);
      HUFF_BUILD(lengths+(257+hlit), 1+hdist, 16, dist_symbols, dist_counts);

      for (;;) {
         HUFF_DECODE(sym, lit_symbols, lit_counts, 16);
         if (sym < 256) {
            PUT_BYTE(sym);
            continue;
         }
         if (sym == 256) {
            break;
         }
         if (sym > 285) {
            goto error;
         }

         GET_BITS(len, len_bits[sym-257]);
         len += len_base[sym-257];

         HUFF_DECODE(sym, dist_symbols, dist_counts, 16);
         if (sym > 29) goto error;

         GET_BITS(dist, dist_bits[sym]);
         dist += dist_base[sym];

         if (out_len - dist < 0) goto error;

         for (i=0; i<len; i++) {
            unsigned char b = out[out_len-dist];
            PUT_BYTE(b);
         }
      }

      if (final) break;
   }
   
   *dest_out = out;
   *dest_len_out = out_len;
   return 1;

error:
   free(out);
   return 0;

   #undef GET_BITS
   #undef HUFF_BUILD
   #undef HUFF_DECODE
   #undef PUT_BYTE
}


static int zlib_compress(const unsigned char *src, int src_len, unsigned char **dest_out, int *dest_len_out)
{
   #define PUT_BYTE(val)                                              \
   {                                                                  \
      if (out_len == out_cap) {                                       \
         if (out_cap >= (1<<29)) goto error;                          \
         out_cap <<= 1;                                               \
         new_out = realloc(out, out_cap);                             \
         if (!new_out) goto error;                                    \
         out = new_out;                                               \
      }                                                               \
      out[out_len++] = val;                                           \
   }

   #define PUT_BITS(val, nb)                                          \
   {                                                                  \
      bits |= (val) << num_bits;                                      \
      num_bits += nb;                                                 \
      while (num_bits >= 8) {                                         \
         PUT_BYTE(bits);                                              \
         bits >>= 8;                                                  \
         num_bits -= 8;                                               \
      }                                                               \
   }

   #define PUT_SYM(val)                                               \
   {                                                                  \
      int v = val, b = syms[val], nb=8;                               \
      if (v >= 144 && v < 256) {                                      \
         b = (b << 1) | 1;                                            \
         nb = 9;                                                      \
      }                                                               \
      else if (v >= 256 && v < 280) {                                 \
         nb = 7;                                                      \
      }                                                               \
      PUT_BITS(b, nb);                                                \
   }

   #define PUT_LEN(val)                                               \
   {                                                                  \
      int i, vv = val, b=0, nb=0;                                     \
      if (vv == 258) {                                                \
         vv = 285;                                                    \
      }                                                               \
      else {                                                          \
         for (i=0; i<6; i++) {                                        \
            if (vv < len_base[i+1]) {                                 \
               vv -= len_base[i];                                     \
               b = vv & ((1 << i)-1);                                 \
               nb = i;                                                \
               vv = i > 0? 261+i*4 + (vv >> i) : 257+vv;              \
               break;                                                 \
            }                                                         \
         }                                                            \
      }                                                               \
      PUT_SYM(vv);                                                    \
      PUT_BITS(b, nb);                                                \
   }

   #define PUT_DIST(val)                                              \
   {                                                                  \
      int i, v = val, b=0, nb=0;                                      \
      for (i=0; i<14; i++) {                                          \
         if (v < dist_base[i+1]) {                                    \
            v -= dist_base[i];                                        \
            b = v & ((1 << i)-1);                                     \
            nb = i;                                                   \
            v = i > 0? 2+i*2 + (v >> i) : v;                          \
            break;                                                    \
         }                                                            \
      }                                                               \
      PUT_BITS(dists[v], 5);                                          \
      PUT_BITS(b, nb);                                                \
   }

   #define SELECT_BUCKET(c1, c2, c3)                                  \
   {                                                                  \
      uint32_t idx = ((c1) << 16) | ((c2) << 8) | (c3);               \
      idx = (idx+0x7ed55d16) + (idx<<12);                             \
      idx = (idx^0xc761c23c) ^ (idx>>19);                             \
      idx = (idx+0x165667b1) + (idx<<5);                              \
      idx = (idx+0xd3a2646c) ^ (idx<<9);                              \
      idx = (idx+0xfd7046c5) + (idx<<3);                              \
      idx = (idx^0xb55a4f09) ^ (idx>>16);                             \
      bucket = hash + (idx & (num_buckets-1)) * num_slots;            \
   }

   #define GET_INDEX(i, val)                                          \
   (                                                                  \
      ((i) & ~32767) + (val) - ((val) >= ((i) & 32767)? 32768 : 0)    \
   )

   const uint8_t syms[288] = {
      0x0c, 0x8c, 0x4c, 0xcc, 0x2c, 0xac, 0x6c, 0xec, 0x1c, 0x9c, 0x5c, 0xdc, 0x3c, 0xbc, 0x7c, 0xfc,
      0x02, 0x82, 0x42, 0xc2, 0x22, 0xa2, 0x62, 0xe2, 0x12, 0x92, 0x52, 0xd2, 0x32, 0xb2, 0x72, 0xf2,
      0x0a, 0x8a, 0x4a, 0xca, 0x2a, 0xaa, 0x6a, 0xea, 0x1a, 0x9a, 0x5a, 0xda, 0x3a, 0xba, 0x7a, 0xfa,
      0x06, 0x86, 0x46, 0xc6, 0x26, 0xa6, 0x66, 0xe6, 0x16, 0x96, 0x56, 0xd6, 0x36, 0xb6, 0x76, 0xf6,
      0x0e, 0x8e, 0x4e, 0xce, 0x2e, 0xae, 0x6e, 0xee, 0x1e, 0x9e, 0x5e, 0xde, 0x3e, 0xbe, 0x7e, 0xfe,
      0x01, 0x81, 0x41, 0xc1, 0x21, 0xa1, 0x61, 0xe1, 0x11, 0x91, 0x51, 0xd1, 0x31, 0xb1, 0x71, 0xf1,
      0x09, 0x89, 0x49, 0xc9, 0x29, 0xa9, 0x69, 0xe9, 0x19, 0x99, 0x59, 0xd9, 0x39, 0xb9, 0x79, 0xf9,
      0x05, 0x85, 0x45, 0xc5, 0x25, 0xa5, 0x65, 0xe5, 0x15, 0x95, 0x55, 0xd5, 0x35, 0xb5, 0x75, 0xf5,
      0x0d, 0x8d, 0x4d, 0xcd, 0x2d, 0xad, 0x6d, 0xed, 0x1d, 0x9d, 0x5d, 0xdd, 0x3d, 0xbd, 0x7d, 0xfd,
      0x09, 0x89, 0x49, 0xc9, 0x29, 0xa9, 0x69, 0xe9, 0x19, 0x99, 0x59, 0xd9, 0x39, 0xb9, 0x79, 0xf9,
      0x05, 0x85, 0x45, 0xc5, 0x25, 0xa5, 0x65, 0xe5, 0x15, 0x95, 0x55, 0xd5, 0x35, 0xb5, 0x75, 0xf5,
      0x0d, 0x8d, 0x4d, 0xcd, 0x2d, 0xad, 0x6d, 0xed, 0x1d, 0x9d, 0x5d, 0xdd, 0x3d, 0xbd, 0x7d, 0xfd,
      0x03, 0x83, 0x43, 0xc3, 0x23, 0xa3, 0x63, 0xe3, 0x13, 0x93, 0x53, 0xd3, 0x33, 0xb3, 0x73, 0xf3,
      0x0b, 0x8b, 0x4b, 0xcb, 0x2b, 0xab, 0x6b, 0xeb, 0x1b, 0x9b, 0x5b, 0xdb, 0x3b, 0xbb, 0x7b, 0xfb,
      0x07, 0x87, 0x47, 0xc7, 0x27, 0xa7, 0x67, 0xe7, 0x17, 0x97, 0x57, 0xd7, 0x37, 0xb7, 0x77, 0xf7,
      0x0f, 0x8f, 0x4f, 0xcf, 0x2f, 0xaf, 0x6f, 0xef, 0x1f, 0x9f, 0x5f, 0xdf, 0x3f, 0xbf, 0x7f, 0xff,
      0x00, 0x40, 0x20, 0x60, 0x10, 0x50, 0x30, 0x70, 0x08, 0x48, 0x28, 0x68, 0x18, 0x58, 0x38, 0x78,
      0x04, 0x44, 0x24, 0x64, 0x14, 0x54, 0x34, 0x74, 0x03, 0x83, 0x43, 0xc3, 0x23, 0xa3, 0x63, 0xe3
   };
   const uint8_t dists[30] = {
      0x00, 0x10, 0x08, 0x18, 0x04, 0x14, 0x0c, 0x1c, 0x02, 0x12, 0x0a, 0x1a, 0x06, 0x16, 0x0e,
      0x1e, 0x01, 0x11, 0x09, 0x19, 0x05, 0x15, 0x0d, 0x1d, 0x03, 0x13, 0x0b, 0x1b, 0x07, 0x17
   };
   const uint16_t len_base[7] = { 3, 11, 19, 35, 67, 131, 258 };
   const uint16_t dist_base[15] = { 1, 5, 9, 17, 33, 65, 129, 257, 513, 1025, 2049, 4097, 8193, 16385, 32769 };

   int num_buckets = 4096; // 4096*8*2 = 64KB
   int num_slots = 8;

   unsigned char *out = NULL, *new_out;
   int out_len=0, out_cap=0;

   uint32_t bits = 0;
   int num_bits = 0;

   int i, j, k, idx, len, dist, best_len, best_dist=0, slot, worst_slot, worst_dist;
   unsigned short *hash = NULL, *bucket;

   out_cap = 4096;
   out = malloc(out_cap);
   if (!out) goto error;

   hash = calloc(num_buckets * num_slots, sizeof(unsigned short));
   if (!hash) goto error;

   PUT_BITS(1, 1); // final block
   PUT_BITS(1, 2); // fixed Huffman codes

   for (i=0; i<src_len-2; i++) {
      SELECT_BUCKET(src[i], src[i+1], src[i+2]);
      best_len = 0;
      slot = -1;
      worst_slot = 0;
      worst_dist = 0;
      for (j=0; j<num_slots; j++) {
         idx = GET_INDEX(i, bucket[j]);
         if (idx >= 0 && idx+2 < i && src[i+0] == src[idx+0] && src[i+1] == src[idx+1] && src[i+2] == src[idx+2]) {
            len = 3;
            for (k=3; k<(src_len-i) && k<258; k++) {
               if (src[i+k] != src[idx+k]) break;
               len++;
            }
            dist = i - idx;
            if (len > best_len || (len == best_len && dist < best_dist)) {
               best_len = len;
               best_dist = dist;
            }
            if (dist > worst_dist) {
               worst_slot = j;
               worst_dist = dist;
            }
         }
         else if (slot < 0) {
            slot = j;
         }
      }

      if (slot < 0) {
         slot = worst_slot;
      }
      bucket[slot] = i & 32767;

      if (best_len >= 3) {
         PUT_LEN(best_len);
         PUT_DIST(best_dist);
         i += best_len-1;
      }
      else {
         PUT_SYM(src[i]);
      }
   }
   for (; i<src_len; i++) {
      PUT_SYM(src[i]);
   }
   PUT_SYM(256); // end of block

   // flush last byte:
   if (num_bits > 0) {
      PUT_BITS(0, 8);
   }
   
   *dest_out = out;
   *dest_len_out = out_len;
   free(hash);
   return 1;

error:
   free(out);
   free(hash);
   return 0;

   #undef PUT_BYTE
   #undef PUT_BITS
   #undef PUT_SYM
   #undef PUT_LEN
   #undef PUT_DIST
   #undef SELECT_BUCKET
   #undef GET_INDEX
}


static uint32_t calc_crc32(const unsigned char *buf, int len)
{
   static uint32_t table[256] = {
      0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f, 0xe963a535, 0x9e6495a3,
      0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988, 0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91,
      0x1db71064, 0x6ab020f2, 0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
      0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9, 0xfa0f3d63, 0x8d080df5,
      0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172, 0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b,
      0x35b5a8fa, 0x42b2986c, 0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
      0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423, 0xcfba9599, 0xb8bda50f,
      0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924, 0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d,
      0x76dc4190, 0x01db7106, 0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
      0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d, 0x91646c97, 0xe6635c01,
      0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e, 0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457,
      0x65b0d9c6, 0x12b7e950, 0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
      0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7, 0xa4d1c46d, 0xd3d6f4fb,
      0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0, 0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9,
      0x5005713c, 0x270241aa, 0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
      0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81, 0xb7bd5c3b, 0xc0ba6cad,
      0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a, 0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683,
      0xe3630b12, 0x94643b84, 0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
      0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb, 0x196c3671, 0x6e6b06e7,
      0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc, 0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5,
      0xd6d6a3e8, 0xa1d1937e, 0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
      0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55, 0x316e8eef, 0x4669be79,
      0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236, 0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f,
      0xc5ba3bbe, 0xb2bd0b28, 0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
      0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f, 0x72076785, 0x05005713,
      0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38, 0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21,
      0x86d3d2d4, 0xf1d4e242, 0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
      0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69, 0x616bffd3, 0x166ccf45,
      0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2, 0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db,
      0xaed16a4a, 0xd9d65adc, 0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
      0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693, 0x54de5729, 0x23d967bf,
      0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94, 0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
   };
   uint32_t crc = 0xFFFFFFFF;
   int i;
   
   for (i=0; i<len; i++) {
      crc = table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
   }
   return crc ^ 0xFFFFFFFF;
}


static uint32_t *load_png(const unsigned char *buf, int len, int *width_out, int *height_out)
{
   #define CHUNK_NAME(c0,c1,c2,c3) ((c0 << 24) | (c1 << 16) | (c2 << 8) | c3)

   unsigned char *comp = NULL, *data = NULL, *p;
   int comp_len = 0, data_len, data_size;
   uint32_t chunk_len, chunk_type, crc;
   int width=0, height=0, bit_depth=0, color_type=0, samples, bpp=0, scanline_bytes=0;
   uint32_t palette[256];
   int palette_len = 0;
   int done = 0, first = 1;
   int i, j;
   uint32_t s1, s2;
   uint32_t *pixels = NULL, *retval = NULL;
   unsigned char *scanlines = NULL, *cur, *prev, *tmp;
   int a, b, c, pp, pa, pb, pc, r, g;

   comp = malloc(len);
   if (!comp) goto error;
   
   if (len < 8) goto error;
   if (memcmp(buf, "\x89PNG\r\n\x1A\n", 8) != 0) goto error;
   buf += 8;
   len -= 8;

   while (!done) {
      if (len < 8) goto error;
      chunk_len = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
      chunk_type = (buf[4] << 24) | (buf[5] << 16) | (buf[6] << 8) | buf[7];
      if (chunk_len > (1<<30)) goto error;
      crc = calc_crc32(buf+4, 4+chunk_len);
      buf += 8;
      len -= 8;

      switch (chunk_type) {
         case CHUNK_NAME('I','H','D','R'):
            if (!first) goto error;
            if (len < 13) goto error;
            width = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
            height = (buf[4] << 24) | (buf[5] << 16) | (buf[6] << 8) | buf[7];
            if (width <= 0 || height <= 0 || width > MAX_IMAGE_DIM || height > MAX_IMAGE_DIM) goto error;
            bit_depth = buf[8];
            color_type = buf[9];
            if (buf[10] != 0 || buf[11] != 0) goto error;
            if (buf[12] != 0) goto error; // interlace not supported
            buf += 13;
            len -= 13;
            if (bit_depth != 1 && bit_depth != 2 && bit_depth != 4 && bit_depth != 8 && bit_depth != 16) {
               goto error;
            }
            switch (color_type) {
               case 0: samples = 1; break;
               case 2: samples = 3; if (bit_depth <= 4) goto error; break;
               case 3: samples = 1; if (bit_depth >= 16) goto error; break;
               case 4: samples = 2; if (bit_depth <= 4) goto error; break;
               case 6: samples = 4; if (bit_depth <= 4) goto error; break;
               default: goto error;
            }
            bpp = (bit_depth * samples) / 8;
            scanline_bytes = (width * bit_depth * samples + 7) / 8;
            if (bpp < 1) bpp = 1;
            //printf("width=%d height=%d bit_depth=%d color_type=%d bpp=%d scanline_bytes=%d\n", width, height, bit_depth, color_type, bpp, scanline_bytes);
            break;

         case CHUNK_NAME('P','L','T','E'):
            if (chunk_len == 0 || chunk_len > 256*3 || chunk_len % 3 != 0) goto error;
            if (chunk_len > len) goto error;
            while (chunk_len > 0) {
               palette[palette_len++] = 0xFF000000 | (buf[0] << 16) | (buf[1] << 8) | buf[2];
               buf += 3;
               len -= 3;
               chunk_len -= 3;
            }
            break;

         case CHUNK_NAME('I','D','A','T'):
            if (color_type == 3 && palette_len == 0) goto error;
            if (chunk_len > len) goto error;
            memcpy(comp+comp_len, buf, chunk_len);
            comp_len += chunk_len;
            buf += chunk_len;
            len -= chunk_len;
            break;

         case CHUNK_NAME('I','E','N','D'):
            if (chunk_len != 0) goto error;
            done = 1;
            break;

         default:
            if ((chunk_type & (1 << (5+24))) == 0) goto error; // critical chunk
            if (len < chunk_len) goto error;
            buf += chunk_len;
            len -= chunk_len;
      }

      if (len < 4) goto error;
      if (buf[0] != (crc >> 24)) goto error;
      if (buf[1] != ((crc >> 16) & 0xFF)) goto error;
      if (buf[2] != ((crc >> 8) & 0xFF)) goto error;
      if (buf[3] != (crc & 0xFF)) goto error;
      buf += 4;
      len -= 4;
      first = 0;
   }

   if (width == 0 || height == 0) goto error;
   if (len != 0) goto error;

   if (comp_len < 6) goto error;
   if ((comp[0] & 15) != 8) goto error; // deflate
   if ((comp[0] >> 4) > 7) goto error; // 32K window or less
   if (comp[1] & (1<<5)) goto error; // no dictionary
   if (((comp[0] << 8) | comp[1]) % 31 != 0) goto error; // fcheck

   data_size = (1 + scanline_bytes) * height;
   if (!zlib_uncompress(comp+2, comp_len-6, &data, &data_len, data_size, data_size)) goto error;
   if (data_len != data_size) goto error;

   s1 = 1;
   s2 = 0;
   for (i=0; i<data_len; i++) {
      s1 += data[i];
      s2 += s1;
      if ((i & 4095) == 4095) {
         s1 %= 65521;
         s2 %= 65521;
      }
   }
   s1 %= 65521;
   s2 %= 65521;
   if (comp[comp_len-4] != (s2 >> 8)) goto error;
   if (comp[comp_len-3] != (s2 & 0xFF)) goto error;
   if (comp[comp_len-2] != (s1 >> 8)) goto error;
   if (comp[comp_len-1] != (s1 & 0xFF)) goto error;

   pixels = malloc(width*height*4);
   if (!pixels) goto error;

   scanlines = calloc(1, 2*scanline_bytes+width);
   if (!scanlines) goto error;
   cur = scanlines;
   prev = scanlines + scanline_bytes;

   p = data;
   for (i=0; i<height; i++) {
      switch (*p++) {
         case 0: // none
            for (j=0; j<scanline_bytes; j++) {
               cur[j] = *p++;
            }
            break;

         case 1: // sub
            for (j=0; j<bpp; j++) {
               cur[j] = *p++;
            }
            for (j=bpp; j<scanline_bytes; j++) {
               cur[j] = cur[j-bpp] + *p++;
            }
            break;

         case 2: // up
            for (j=0; j<scanline_bytes; j++) {
               cur[j] = prev[j] + *p++;
            }
            break;

         case 3: // average
            for (j=0; j<bpp; j++) {
               cur[j] = (prev[j] >> 1) + *p++;
            }
            for (j=bpp; j<scanline_bytes; j++) {
               cur[j] = ((cur[j-bpp] + prev[j]) >> 1) + *p++;
            }
            break;

         case 4: // paeth
            for (j=0; j<bpp; j++) {
               cur[j] = prev[j] + *p++;
            }
            for (j=bpp; j<scanline_bytes; j++) {
               a = cur[j-bpp];
               b = prev[j];
               c = prev[j-bpp];
               pp = a + b - c;
               pa = abs(pp - a);
               pb = abs(pp - b);
               pc = abs(pp - c);
               if (pa <= pb && pa <= pc) {
                  cur[j] = a + *p++;
               }
               else if (pb <= pc) {
                  cur[j] = b + *p++;
               }
               else {
                  cur[j] = c + *p++;
               }
            }
            break;

         default:
            goto error;
      }

      if (bit_depth < 8) {
         tmp = scanlines + 2*scanline_bytes;
         for (j=0; j<width; j++) {
            tmp[j] = cur[(j*bit_depth) >> 3];
            tmp[j] >>= (8 - bit_depth) - ((j*bit_depth) & 7);
            tmp[j] &= (1 << bit_depth)-1;
         }

         if (color_type == 0) {
            b = (bit_depth == 1? 0xFF : bit_depth == 2? 0x55 : 0x11);
            for (j=0; j<width; j++) {
               a = tmp[j] * b;
               pixels[i*width+j] = 0xFF000000 | (a << 16) | (a << 8) | a;
            }
         }
         else {
            for (j=0; j<width; j++) {
               a = tmp[j];
               if (a >= palette_len) goto error;
               pixels[i*width+j] = palette[a];
            }
         }
      }
      else if (bit_depth == 8) {
         switch (color_type) {
            case 0: // gray
               for (j=0; j<width; j++) {
                  r = cur[j];
                  pixels[i*width+j] = 0xFF000000 | (r << 16) | (r << 8) | r;
               }
               break;

            case 2: // rgb
               for (j=0; j<width; j++) {
                  r = cur[j*3+0];
                  g = cur[j*3+1];
                  b = cur[j*3+2];
                  pixels[i*width+j] = 0xFF000000 | (r << 16) | (g << 8) | b;
               }
               break;

            case 3: // palette
               for (j=0; j<width; j++) {
                  a = cur[j];
                  if (a >= palette_len) goto error;
                  pixels[i*width+j] = palette[a];
               }
               break;

            case 4: // gray+alpha
               for (j=0; j<width; j++) {
                  r = cur[j*2+0];
                  a = cur[j*2+1];
                  r = div255(r * a);
                  pixels[i*width+j] = (a << 24) | (r << 16) | (r << 8) | r;
               }
               break;

            case 6: // rgba
               for (j=0; j<width; j++) {
                  r = cur[j*4+0];
                  g = cur[j*4+1];
                  b = cur[j*4+2];
                  a = cur[j*4+3];
                  r = div255(r * a);
                  g = div255(g * a);
                  b = div255(b * a);
                  pixels[i*width+j] = (a << 24) | (r << 16) | (g << 8) | b;
               }
               break;
         }
      }
      else {
         switch (color_type) {
            case 0: // gray
               for (j=0; j<width; j++) {
                  r = cur[j*2+0];
                  pixels[i*width+j] = 0xFF000000 | (r << 16) | (r << 8) | r;
               }
               break;

            case 2: // rgb
               for (j=0; j<width; j++) {
                  r = cur[j*6+0];
                  g = cur[j*6+2];
                  b = cur[j*6+4];
                  pixels[i*width+j] = 0xFF000000 | (r << 16) | (g << 8) | b;
               }
               break;

            case 4: // gray+alpha
               for (j=0; j<width; j++) {
                  r = cur[j*4+0];
                  a = cur[j*4+2];
                  r = div255(r * a);
                  pixels[i*width+j] = (a << 24) | (r << 16) | (r << 8) | r;
               }
               break;

            case 6: // rgba
               for (j=0; j<width; j++) {
                  r = cur[j*8+0];
                  g = cur[j*8+2];
                  b = cur[j*8+4];
                  a = cur[j*8+6];
                  r = div255(r * a);
                  g = div255(g * a);
                  b = div255(b * a);
                  pixels[i*width+j] = (a << 24) | (r << 16) | (g << 8) | b;
               }
               break;
         }
      }

      tmp = prev;
      prev = cur;
      cur = tmp;
   }

   *width_out = width;
   *height_out = height;
   retval = pixels;
   pixels = NULL;

error:
   free(comp);
   free(data);
   free(pixels);
   free(scanlines);
   return retval;

   #undef CHUNK_NAME
}


static int save_png(const uint32_t *pixels, int stride, int width, int height, unsigned char **dest_out, int *dest_len_out)
{
   unsigned char *data = NULL, *comp = NULL, *dest = NULL, *p, *s;
   uint32_t crc;
   uint32_t s1, s2;
   unsigned char *scanlines = NULL, *cur, *prev, *tmp, *filter[5], *sp;
   int score[5];
   int samples, color_mask, alpha_mask;
   int i, j, k, r, g, b, a, c, pp, pa, pb, pc, data_len, comp_len, dest_len, retval=0;

   color_mask = 0;
   alpha_mask = 0xFF;
   for (i=0; i<height; i++) {
      for (j=0; j<width; j++) {
         c = pixels[i*stride+j];
         a = (c >> 24) & 0xFF;
         r = (c >> 16) & 0xFF;
         g = (c >>  8) & 0xFF;
         b = (c >>  0) & 0xFF;
         color_mask |= (r ^ g) | (g ^ b);
         alpha_mask &= a;
      }
   }

   samples = (color_mask? 3 : 1) + (alpha_mask != 0xFF? 1 : 0);

   scanlines = calloc(1, width*samples*(2+5));
   if (!scanlines) goto error;
   cur = scanlines;
   prev = scanlines + (width*samples);
   for (i=0; i<5; i++) {
      filter[i] = scanlines + (width*samples)*(2+i);
   }

   data_len = (width*samples+1)*height;
   data = malloc(data_len);
   if (!data) goto error;

   p = data;
   for (i=0; i<height; i++) {
      sp = cur;
      for (j=0; j<width; j++) {
         c = pixels[i*stride+j];
         a = (c >> 24) & 0xFF;
         r = (c >> 16) & 0xFF;
         g = (c >>  8) & 0xFF;
         b = (c >>  0) & 0xFF;
         if (a != 0) {
            r = (r*255) / a;
            g = (g*255) / a;
            b = (b*255) / a;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
         }
         *sp++ = r;
         if (samples >= 3) {
            *sp++ = g;
            *sp++ = b;
         }
         if ((samples & 1) == 0) {
            *sp++ = a;
         }
      }

      for (j=0; j<samples; j++) {
         filter[0][j] = cur[j];
         filter[1][j] = cur[j];
         filter[2][j] = cur[j] - prev[j];
         filter[3][j] = cur[j] - (prev[j] >> 1);
         filter[4][j] = cur[j] - prev[j];
      }
      for (j=samples; j<width*samples; j++) {
         filter[0][j] = cur[j];
         filter[1][j] = cur[j] - cur[j-samples];
         filter[2][j] = cur[j] - prev[j];
         filter[3][j] = cur[j] - ((cur[j-samples] + prev[j]) >> 1);
         a = cur[j-samples];
         b = prev[j];
         c = prev[j-samples];
         pp = a + b - c;
         pa = abs(pp - a);
         pb = abs(pp - b);
         pc = abs(pp - c);
         if (pa <= pb && pa <= pc) {
            filter[4][j] = cur[j] - a;
         }
         else if (pb <= pc) {
            filter[4][j] = cur[j] - b;
         }
         else {
            filter[4][j] = cur[j] - c;
         }
      }

      for (j=0; j<5; j++) {
         score[j] = 0;
         for (k=0; k<width*samples; k++) {
            score[j] += abs((signed char)filter[j][k]);
         }
      }

      k = 0;
      for (j=1; j<5; j++) {
         if (score[j] < score[k]) {
            k = j;
         }
      }

      *p++ = k;
      for (j=0; j<width*samples; j++) {
         *p++ = filter[k][j];
      }

      tmp = prev;
      prev = cur;
      cur = tmp;
   }

   s1 = 1;
   s2 = 0;
   for (i=0; i<data_len; i++) {
      s1 += data[i];
      s2 += s1;
      if ((i & 4095) == 4095) {
         s1 %= 65521;
         s2 %= 65521;
      }
   }
   s1 %= 65521;
   s2 %= 65521;

   if (!zlib_compress(data, data_len, &comp, &comp_len)) goto error;

   dest_len = 8 + 3*(4+4+4) + 13 + (2+comp_len+4) + 0;
   dest = malloc(dest_len);
   if (!dest) goto error;

   p = dest;
   memcpy(p, "\x89PNG\r\n\x1A\n", 8);
   p += 8;

   *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 13; // chunk len
   s = p;
   *p++ = 'I'; *p++ = 'H'; *p++ = 'D'; *p++ = 'R'; // chunk type
   *p++ = width >> 24;
   *p++ = width >> 16;
   *p++ = width >> 8;
   *p++ = width;
   *p++ = height >> 24;
   *p++ = height >> 16;
   *p++ = height >> 8;
   *p++ = height;
   *p++ = 8; // bit depth
   *p++ = samples == 4? 6 : samples == 3? 2 : samples == 2? 4 : 0; // color type
   *p++ = 0; // deflate
   *p++ = 0; // filter
   *p++ = 0; // interlace
   crc = calc_crc32(s, p-s);
   *p++ = crc >> 24;
   *p++ = crc >> 16;
   *p++ = crc >> 8;
   *p++ = crc;

   *p++ = (2+comp_len+4) >> 24;
   *p++ = (2+comp_len+4) >> 16;
   *p++ = (2+comp_len+4) >> 8;
   *p++ = (2+comp_len+4);
   s = p;
   *p++ = 'I'; *p++ = 'D'; *p++ = 'A'; *p++ = 'T'; // chunk type
   *p++ = 0x78; // deflate + 32K window
   *p++ = (1 << 6) | 30; // fcheck + fast
   memcpy(p, comp, comp_len);
   p += comp_len;
   *p++ = s2 >> 8;
   *p++ = s2;
   *p++ = s1 >> 8;
   *p++ = s1;
   crc = calc_crc32(s, p-s);
   *p++ = crc >> 24;
   *p++ = crc >> 16;
   *p++ = crc >> 8;
   *p++ = crc;

   *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 0; // chunk len
   s = p;
   *p++ = 'I'; *p++ = 'E'; *p++ = 'N'; *p++ = 'D'; // chunk type
   crc = calc_crc32(s, p-s);
   *p++ = crc >> 24;
   *p++ = crc >> 16;
   *p++ = crc >> 8;
   *p++ = crc;

   *dest_out = dest;
   *dest_len_out = dest_len;
   dest = NULL;
   retval = 1;

error:
   free(data);
   free(comp);
   free(dest);
   free(scanlines);
   return retval;
}
