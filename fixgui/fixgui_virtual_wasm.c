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
#include <wasm-support.h>
#include "fixgui_common.h"

enum {
   LAYER_SCROLL_X    = 0x01,
   LAYER_SCROLL_Y    = 0x02,
   LAYER_SCROLL_XY   = 0x03,
   LAYER_TRANSLUCENT = 0x04,
   LAYER_ACCELERATED = 0x08
};

typedef void (*PaintFunc)(int x, int y, int width, int height, void *data);

extern void set_auto_fullscreen(int enabled) IMPORT("env", "set_auto_fullscreen");
extern void create_canvas(int *width, int *height, float *scale, int *safe_left, int *safe_top, int *safe_right, int *safe_bottom) IMPORT("env", "create_canvas");

extern int layer_create(int level, int x, int y, int width, int height, int extra_horiz, int extra_vert, int tile_size, int flags, PaintFunc paint_func, void *paint_data) IMPORT("env", "layer_create");
extern void layer_destroy(int handle) IMPORT("env", "layer_destroy");
extern void layer_move(int handle, int x, int y) IMPORT("env", "layer_move");
extern void layer_resize(int handle, int width, int height) IMPORT("env", "layer_resize");
extern void layer_set_opacity(int handle, float opacity) IMPORT("env", "layer_set_opacity");
extern void layer_scroll(int handle, int scroll_x, int scroll_y) IMPORT("env", "layer_scroll");
extern void layer_update(int handle, int x, int y, int width, int height, PaintFunc paint_func, void *paint_data) IMPORT("env", "layer_update");

extern void put_pixels(uint32_t *pixels, int width, int height, int dx, int dy, int sx, int sy, int sw, int sh) IMPORT("env", "put_pixels");
extern void clear_rect(int x, int y, int width, int height) IMPORT("env", "clear_rect");
extern void fill_rect(int x, int y, int width, int height, int r, int g, int b, float a) IMPORT("env", "fill_rect");
extern void fill_rect_image(int x, int y, int width, int height, int handle, float m00, float m01, float m02, float m10, float m11, float m12, int flags, int smoothing, float alpha) IMPORT("env", "fill_rect_image");
extern void fill_shape(int clip_x, int clip_y, int clip_width, int clip_height, float m00, float m01, float m02, float m10, float m11, float m12, uint32_t *shape_ptr, uint32_t *shape_end, int r, int g, int b, float a) IMPORT("env", "fill_shape");
extern void fill_shape_image(int clip_x, int clip_y, int clip_width, int clip_height, float m00, float m01, float m02, float m10, float m11, float m12, uint32_t *shape_ptr, uint32_t *shape_end, int handle, float i00, float i01, float i02, float i10, float i11, float i12, int flags, int smoothing, float alpha) IMPORT("env", "fill_shape_image");
extern void draw_image(int handle, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh, int smoothing, float alpha) IMPORT("env", "draw_image");

extern int create_image(int width, int height) IMPORT("env", "create_image");
extern void destroy_image(int handle) IMPORT("env", "destroy_image");
extern void update_image(int handle, int x, int y, int width, int height, uint32_t *pixels) IMPORT("env", "update_image");

extern void request_animation_frame(void (*func)(uint32_t time, void *data), void *data) IMPORT("env", "request_animation_frame");
extern double get_monotonic_time() IMPORT("env", "get_monotonic_time");
extern int is_mobile() IMPORT("env", "is_mobile");
extern int get_subpixel_order() IMPORT("env", "get_subpixel_order");
extern int set_clipboard(uint16_t *str) IMPORT("env", "set_clipboard");
extern void set_cursor(int type) IMPORT("env", "set_cursor");

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
   BC_OUTPUT_REPLACE
};

enum {
   TEX_CLAMP_X = 0x01,
   TEX_CLAMP_Y = 0x02
};

typedef struct {
   union {
      struct {
         float m00, m01, m02;
         float m10, m11, m12;
      };
      float m[6];
   };
} Transform;

typedef struct {
   Value func;
   Value data;
   int accelerated;
} LayerPaint;

typedef struct {
   int handle;
   LayerPaint paint;
   int x, y, width, height;
   float opacity;
   int scroll_x;
   int scroll_y;
} Layer;

typedef struct {
   int handle;
   int width;
   int height;
} CachedImage;

typedef struct {
   int finished;
   int x;
   int y;
} MouseFinish;

struct Worker {
   WorkerCommon common;
};

typedef struct Timer {
   Heap *heap;
   Value instance;
   int interval;
   wasm_timer_t id;
   struct Timer *next;
} Timer;

typedef struct Animation {
   Value func;
   Value data;
   struct Animation *next;
} Animation;

#define NUM_HANDLE_TYPES 2
#define HANDLE_TYPE_LAYER        (handles_offset+0)
#define HANDLE_TYPE_CACHED_IMAGE (handles_offset+1)

static volatile int handles_offset = 0;

static Heap *gui_heap = NULL;
static int canvas_width, canvas_height;
static float canvas_scale;
static int safe_area[4];
static int main_layer_handle;
static int cached_width, cached_height;
static Value cached_image;
static int repaint_notified = 0;
static int ignore_repaint = 0;
static int touch_active_points = 0;
static int touch_mouse_emitter = 0;
static int touch_mouse_x;
static int touch_mouse_y;
static uint32_t touch_mouse_time;
static MouseFinish *touch_mouse_finish = NULL;
static int canvas_accel = 0;
static Value canvas_painter, canvas_painter_values[PAINTER_SIZE];
static Value image_map;
static Timer *active_timers = NULL;
static Animation *active_animations = NULL;
static char *clipboard_text = NULL;

static NativeFunc orig_painter_create_func;
static NativeFunc orig_painter_clear_rect_func;
static NativeFunc orig_painter_fill_rect_color_func;
static NativeFunc orig_painter_fill_rect_shader_func;
static NativeFunc orig_painter_fill_shape_color_func;
static NativeFunc orig_painter_fill_shape_shader_func;
static void *orig_painter_create_data;
static void *orig_painter_clear_rect_data;
static void *orig_painter_fill_rect_color_data;
static void *orig_painter_fill_rect_shader_data;
static void *orig_painter_fill_shape_color_data;
static void *orig_painter_fill_shape_shader_data;


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
   return (int64_t)get_monotonic_time();
}


uint32_t timer_get_micro_time()
{
   return (int64_t)(get_monotonic_time() * 1000.0);
}


static void handle_timer(void *data)
{
   Timer *timer = data;

   timer_run(timer->heap, timer->instance);
}


int timer_is_active(Heap *heap, Value instance)
{
   Timer *timer;

   for (timer = active_timers; timer; timer = timer->next) {
      if (timer->heap == heap && timer->instance.value == instance.value) {
         return 1;
      }
   }
   return 0;
}


void timer_start(Heap *heap, Value instance, int interval, int restart)
{
   Timer *timer;
   
   for (timer = active_timers; timer; timer = timer->next) {
      if (timer->heap == heap && timer->instance.value == instance.value) {
         break;
      }
   }
   if (timer) {
      if (restart) {
         wasm_timer_stop(timer->id);
         timer->id = wasm_timer_start(timer->interval, 1, handle_timer, timer);
      }
   }
   else {
      timer = calloc(1, sizeof(Timer));
      timer->heap = heap;
      timer->instance = instance;
      timer->interval = interval;
      timer->id = wasm_timer_start(interval, 1, handle_timer, timer);
      timer->next = active_timers;
      active_timers = timer;
      fixscript_ref(heap, instance);
   }
}


void timer_stop(Heap *heap, Value instance)
{
   Timer *timer = NULL, **prev;
   
   prev = &active_timers;
   for (timer = active_timers; timer; timer = timer->next) {
      if (timer->heap == heap && timer->instance.value == instance.value) {
         break;
      }
      prev = &timer->next;
   }
   if (timer) {
      *prev = timer->next;
      wasm_timer_stop(timer->id);
      fixscript_unref(timer->heap, timer->instance);
      free(timer);
   }
}


void clipboard_set_text(plat_char *text)
{
   Value value;
   uint16_t *str = NULL;
   int ret;

   if (!is_mobile()) {
      value = fixscript_create_string(gui_heap, text, text? -1 : 0);
      fixscript_get_string_utf16(gui_heap, value, 0, -1, &str, NULL);
      if (str) {
         ret = set_clipboard(str);
         free(str);
         if (ret) {
            return;
         }
      }
   }

   free(clipboard_text);
   clipboard_text = text? strdup(text) : NULL;
}


plat_char *clipboard_get_text()
{
   return clipboard_text? strdup(clipboard_text) : NULL;
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


static Value func_log(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   NativeFunc orig_log = data;
   char *s;
   int err, len;

   if (fixscript_is_string(heap, params[0])) {
      err = fixscript_get_string(heap, params[0], 0, -1, &s, &len);
   }
   else {
      err = fixscript_to_string(heap, params[0], 0, &s, &len);
   }
   if (err) {
      return fixscript_error(heap, error, err);
   }

   fprintf(stderr, "%.*s\n", len, s);
   fflush(stderr);
   free(s);

   orig_log(heap, error, num_params, params, NULL);
   return fixscript_int(0);
}


static Value func_web_is_present(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   return fixscript_int(1);
}


static Value func_web_set_auto_fullscreen(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   set_auto_fullscreen(params[0].value);
   return fixscript_int(0);
}


static Value func_web_set_canvas_acceleration(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   canvas_accel = params[0].value != 0;
   return fixscript_int(0);
}


static Value func_web_is_painter_accelerated(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   return fixscript_int(params[0].value == canvas_painter.value && params[0].is_array);
}


static void repaint(void *data);

static Value func_web_flush_drawing(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   repaint((void *)1);
   return fixscript_int(0);
}


static Value func_web_ignore_drawing(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   int x, y, width, height;

   virtual_get_dirty_rect(&x, &y, &width, &height, canvas_width, canvas_height);
   return fixscript_int(0);
}


static void run_animations(uint32_t time, void *data)
{
   Heap *heap = gui_heap;
   Animation *anim, **prev;
   Value ret, error;

   prev = &active_animations;
   for (anim = active_animations; anim; ) {
      ret = fixscript_call(heap, anim->func, 2, &error, anim->data, fixscript_int(time));
      if (error.value) {
         fixscript_dump_value(heap, error, 1);
      }
      if (!ret.value) {
         *prev = anim->next;
         fixscript_unref(heap, anim->data);
         free(anim);
         anim = *prev;
         continue;
      }
      prev = &anim->next;
      anim = anim->next;
   }

   if (active_animations) {
      request_animation_frame(run_animations, NULL);
   }
}


static Value func_web_run_animation(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Animation *new_anim, *anim, **prev;

   new_anim = calloc(1, sizeof(Animation));
   if (!new_anim) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   new_anim->func = params[0];
   new_anim->data = params[1];
   fixscript_ref(heap, new_anim->data);

   if (!active_animations) {
      request_animation_frame(run_animations, NULL);
   }

   prev = &active_animations;
   for (anim = active_animations; ; anim = anim->next) {
      if (!anim) {
         *prev = new_anim;
         break;
      }
      prev = &anim->next;
   }
   return fixscript_int(0);
}


static Value func_web_is_mobile(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   return fixscript_int(is_mobile());
}


static Value func_web_get_subpixel_order(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   return fixscript_int(get_subpixel_order());
}


static Value func_web_get_safe_area(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value arr, values[4];
   int err;

   values[0] = fixscript_int(safe_area[0]);
   values[1] = fixscript_int(safe_area[1]);
   values[2] = fixscript_int(safe_area[2]);
   values[3] = fixscript_int(safe_area[3]);

   arr = fixscript_create_array(heap, 4);
   if (!arr.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   err = fixscript_set_array_range(heap, arr, 0, 4, values);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return arr;
}


static void layer_paint_func(int x, int y, int width, int height, void *data)
{
   Heap *heap = gui_heap;
   LayerPaint *lp = data;
   Value image, painter, error;
   uint32_t *pixels, *ptr, pixel;
   int a, r, g, b;
   int i, j, stride;

   if ((lp && lp->accelerated) || (!lp && canvas_accel)) {
      canvas_painter_values[PAINTER_clip_x1] = fixscript_int(x);
      canvas_painter_values[PAINTER_clip_y1] = fixscript_int(y);
      canvas_painter_values[PAINTER_clip_x2] = fixscript_int(x+width);
      canvas_painter_values[PAINTER_clip_y2] = fixscript_int(y+height);
      fixscript_set_array_range(heap, canvas_painter, 0, PAINTER_SIZE, canvas_painter_values);
      fixscript_set_array_length(heap, canvas_painter, PAINTER_SIZE);

      if (lp) {
         fixscript_call(heap, lp->func, 6, &error, lp->data, canvas_painter, fixscript_int(x), fixscript_int(y), fixscript_int(width), fixscript_int(height));
         if (error.value) {
            fixscript_dump_value(heap, error, 1);
         }
      }
      else {
         virtual_handle_paint(canvas_painter);
      }
      return;
   }

   if (width > cached_width || height > cached_height) {
      fixscript_unref(heap, cached_image);
      fixscript_collect_heap(heap);

      if (width > cached_width) {
         cached_width = width;
      }
      if (height > cached_height) {
         cached_height = height;
      }
      cached_image = fiximage_create(heap, cached_width, cached_height);
      fixscript_ref(heap, cached_image);
   }

   fiximage_get_data(heap, cached_image, NULL, NULL, NULL, &pixels, NULL, NULL);
   image = fiximage_create_from_pixels(heap, width, height, cached_width, pixels, NULL, NULL, -1);
   painter = fiximage_create_painter(heap, image, -x, -y);

   if (lp) {
      fixscript_call(heap, lp->func, 6, &error, lp->data, painter, fixscript_int(x), fixscript_int(y), fixscript_int(width), fixscript_int(height));
      if (error.value) {
         fixscript_dump_value(heap, error, 1);
      }
   }
   else {
      virtual_handle_paint(painter);
   }

   stride = cached_width - width;
   ptr = pixels;
   for (i=0; i<height; i++) {
      for (j=0; j<width; j++) {
         pixel = *ptr;
         a = pixel >> 24;
         r = (pixel >> 16) & 0xFF;
         g = (pixel >> 8) & 0xFF;
         b = pixel & 0xFF;
         if ((uint32_t)(a - 1) < (uint32_t)254) {
            r = (r*255) / a;
            g = (g*255) / a;
            b = (b*255) / a;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
         }
         *ptr++ = (a << 24) | (b << 16) | (g << 8) | r;
      }
      ptr += stride;
   }

   put_pixels(pixels, cached_width, cached_height, x, y, 0, 0, width, height);
}


static void *layer_handle_func(Heap *heap, int op, void *p1, void *p2)
{
   Layer *layer = p1;

   switch (op) {
      case HANDLE_OP_FREE:
         layer_destroy(layer->handle);
         free(layer);
         break;

      case HANDLE_OP_MARK_REFS:
         fixscript_mark_ref(heap, layer->paint.data);
         break;
   }
   return NULL;
}


static Value func_layer_create(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Layer *layer;
   PaintFunc paint_func = NULL;
   void *paint_data = NULL;
   Value handle;

   layer = calloc(1, sizeof(Layer));
   if (!layer) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   if (params[9].value) {
      layer->paint.func = params[9];
      layer->paint.data = params[10];

      paint_func = layer_paint_func;
      paint_data = &layer->paint;
   }

   layer->paint.accelerated = (params[8].value & LAYER_ACCELERATED) != 0;

   layer->handle = layer_create(params[0].value, params[1].value, params[2].value, params[3].value, params[4].value, params[5].value, params[6].value, params[7].value, params[8].value, paint_func, paint_data);
   layer->x = params[1].value;
   layer->y = params[2].value;
   layer->width = params[3].value;
   layer->height = params[4].value;
   layer->opacity = 1.0f;

   handle = fixscript_create_value_handle(heap, HANDLE_TYPE_LAYER, layer, layer_handle_func);
   if (!handle.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return handle;
}


static Value func_layer_get_position(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Layer *layer;
   int ret;

   layer = fixscript_get_handle(heap, params[0], HANDLE_TYPE_LAYER, NULL);
   if (!layer) {
      *error = fixscript_create_error_string(heap, "invalid layer handle");
      return fixscript_int(0);
   }

   switch ((int)(intptr_t)data) {
      case 0: ret = layer->x; break;
      case 1: ret = layer->y; break;
      case 2: ret = layer->width; break;
      case 3: ret = layer->height; break;
      case 4: ret = layer->scroll_x; break;
      case 5: ret = layer->scroll_y; break;
   }
   return fixscript_int(ret);
}


static Value func_layer_move(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Layer *layer;

   layer = fixscript_get_handle(heap, params[0], HANDLE_TYPE_LAYER, NULL);
   if (!layer) {
      *error = fixscript_create_error_string(heap, "invalid layer handle");
      return fixscript_int(0);
   }

   layer->x = params[1].value;
   layer->y = params[2].value;
   layer_move(layer->handle, params[1].value, params[2].value);
   return fixscript_int(0);
}


static Value func_layer_resize(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Layer *layer;

   layer = fixscript_get_handle(heap, params[0], HANDLE_TYPE_LAYER, NULL);
   if (!layer) {
      *error = fixscript_create_error_string(heap, "invalid layer handle");
      return fixscript_int(0);
   }

   layer->width = params[1].value;
   layer->height = params[2].value;
   layer_resize(layer->handle, params[1].value, params[2].value);
   return fixscript_int(0);
}


static Value func_layer_set_opacity(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Layer *layer;
   float opacity = fixscript_get_float(params[1]);

   layer = fixscript_get_handle(heap, params[0], HANDLE_TYPE_LAYER, NULL);
   if (!layer) {
      *error = fixscript_create_error_string(heap, "invalid layer handle");
      return fixscript_int(0);
   }

   if (opacity != layer->opacity) {
      layer->opacity = opacity;
      layer_set_opacity(layer->handle, opacity);
   }
   return fixscript_int(0);
}


static Value func_layer_scroll(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Layer *layer;

   layer = fixscript_get_handle(heap, params[0], HANDLE_TYPE_LAYER, NULL);
   if (!layer) {
      *error = fixscript_create_error_string(heap, "invalid layer handle");
      return fixscript_int(0);
   }

   layer->scroll_x = params[1].value;
   layer->scroll_y = params[2].value;
   layer_scroll(layer->handle, params[1].value, params[2].value);
   return fixscript_int(0);
}


static Value func_layer_update(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Layer *layer;
   LayerPaint lp;

   layer = fixscript_get_handle(heap, params[0], HANDLE_TYPE_LAYER, NULL);
   if (!layer) {
      *error = fixscript_create_error_string(heap, "invalid layer handle");
      return fixscript_int(0);
   }

   lp.func = params[5];
   lp.data = params[6];
   lp.accelerated = layer->paint.accelerated;
   layer_update(layer->handle, params[1].value, params[2].value, params[3].value, params[4].value, layer_paint_func, &lp);
   return fixscript_int(0);
}


static void free_cached_image(void *ptr)
{
   CachedImage *cimg = ptr;

   destroy_image(cimg->handle);
   free(cimg);
}


static void convert_and_update_image(int handle, int x, int y, int width, int height, uint32_t *src, int stride)
{
   uint32_t *dest, *ptr, pixel;
   int a, r, g, b;
   int i, j;

   dest = malloc(width*height*4);
   if (!dest) return;

   stride -= width;
   ptr = dest;
   for (i=0; i<height; i++) {
      for (j=0; j<width; j++) {
         pixel = *src++;
         a = pixel >> 24;
         r = (pixel >> 16) & 0xFF;
         g = (pixel >> 8) & 0xFF;
         b = pixel & 0xFF;
         if ((uint32_t)(a - 1) < (uint32_t)254) {
            r = (r*255) / a;
            g = (g*255) / a;
            b = (b*255) / a;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
         }
         *ptr++ = (a << 24) | (b << 16) | (g << 8) | r;
      }
      src += stride;
   }

   update_image(handle, x, y, width, height, dest);
   free(dest);
}


static CachedImage *get_cached_image(Heap *heap, Value *error, Value img)
{
   CachedImage *cimg = NULL;
   Value key, handle;
   uint32_t *pixels;
   int err, width, height, stride;

   err = fixscript_create_weak_ref(heap, img, &image_map, NULL, &key);
   if (!err) {
      err = fixscript_get_hash_elem(heap, image_map, key, &handle);
      if (err == FIXSCRIPT_ERR_KEY_NOT_FOUND) {
         err = 0;
         if (!fiximage_get_data(heap, img, &width, &height, &stride, &pixels, NULL, NULL)) {
            *error = fixscript_create_error_string(heap, "invalid image");
            return NULL;
         }
         cimg = calloc(1, sizeof(CachedImage));
         if (!cimg) {
            err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
         }
         if (!err) {
            cimg->handle = create_image(width, height);
            cimg->width = width;
            cimg->height = height;
            convert_and_update_image(cimg->handle, 0, 0, width, height, pixels, stride);

            handle = fixscript_create_handle(heap, HANDLE_TYPE_CACHED_IMAGE, cimg, free_cached_image);
            if (!handle.value) {
               err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
            }
         }
         if (!err) {
            err = fixscript_set_hash_elem(heap, image_map, key, handle);
         }
      }
   }
   if (!err) {
      if (cimg) return cimg;
      cimg = fixscript_get_handle(heap, handle, HANDLE_TYPE_CACHED_IMAGE, NULL);
      if (!cimg) {
         *error = fixscript_create_error_string(heap, "internal error: shouldn't happen");
      }
   }
   if (err) {
      fixscript_error(heap, error, err);
      return NULL;
   }
   return cimg;
}


static int decode_shader(Heap *heap, Value *error, Value shader_val, Value params_val, Transform *tr, CachedImage **cimg_out, Transform *img_tr_out, int *flags, int *smoothing, float *alpha)
{
   Transform img_tr;
   CachedImage *cimg;
   Value shader_params[3], shader_transform[6];
   uint8_t shader[14];
   int err, len, valid=0;

   err = fixscript_get_array_length(heap, shader_val, &len);
   if (!err && (len == 7 || len == 14)) {
      err = fixscript_get_array_bytes(heap, shader_val, 0, len, (char *)shader);
      if (!err) {
         if ((shader[0] == BC_SAMPLE_NEAREST || shader[0] == BC_SAMPLE_BILINEAR || shader[0] == BC_SAMPLE_BICUBIC) &&
             (shader[1] == 0 && shader[2] == 0 && shader[3] == 1))
         {
            *flags = shader[4];

            if (len == 7 && (shader[5] == BC_OUTPUT_BLEND || shader[5] == BC_OUTPUT_REPLACE) && shader[6] == 0) {
               valid = 1;
            }
            else if (len == 14 && shader[5] == BC_COLOR && shader[6] == 1 && shader[7] == 2 &&
                     shader[8] == BC_MUL && shader[9] == 0 && shader[10] == 0 && shader[11] == 1 &&
                     (shader[12] == BC_OUTPUT_BLEND || shader[12] == BC_OUTPUT_REPLACE) && shader[13] == 0)
            {
               valid = 1;
            }
         }

         if (valid) {
            err = fixscript_get_array_range(heap, params_val, 0, len == 14? 3 : 2, shader_params);
            if (!err) {
               if (len == 14) {
                  *alpha = ((uint32_t)(shader_params[2].value) >> 24) * (1.0f/255.0f);
               }
               else {
                  *alpha = 1.0f;
               }
               cimg = get_cached_image(heap, error, shader_params[0]);
               if (!cimg) {
                  return 0;
               }
            }
            if (!err) {
               err = fixscript_get_array_range(heap, shader_params[1], 0, 6, shader_transform);
            }
            if (!err) {
               img_tr.m00 = fixscript_get_float(shader_transform[0]);
               img_tr.m01 = fixscript_get_float(shader_transform[1]);
               img_tr.m02 = fixscript_get_float(shader_transform[2]);
               img_tr.m10 = fixscript_get_float(shader_transform[3]);
               img_tr.m11 = fixscript_get_float(shader_transform[4]);
               img_tr.m12 = fixscript_get_float(shader_transform[5]);
               *cimg_out = cimg;
               img_tr_out->m00 = tr->m00 * img_tr.m00 + tr->m01 * img_tr.m10;
               img_tr_out->m01 = tr->m00 * img_tr.m01 + tr->m01 * img_tr.m11;
               img_tr_out->m02 = tr->m00 * img_tr.m02 + tr->m01 * img_tr.m12 + tr->m02;
               img_tr_out->m10 = tr->m10 * img_tr.m00 + tr->m11 * img_tr.m10;
               img_tr_out->m11 = tr->m10 * img_tr.m01 + tr->m11 * img_tr.m11;
               img_tr_out->m12 = tr->m10 * img_tr.m02 + tr->m11 * img_tr.m12 + tr->m12;
               *smoothing = shader[0] != BC_SAMPLE_NEAREST;
               return 1;
            }
         }
      }
   }
   else {
      char buf[1024], shader_big[128];
      int i;
      buf[0] = 0;
      fixscript_get_array_bytes(heap, shader_val, 0, len, (char *)shader_big);
      for (i=0; i<len; i++) {
         sprintf(buf + strlen(buf), "buf[%d]=%d\n", i, shader_big[i]);
      }
      fprintf(stderr, "shader len=%d\n%s", len, buf);
   }
   if (err) {
      fixscript_error(heap, error, err);
   }
   return 0;
}


static int painter_get(Heap *heap, Value *error, Value painter_val, Rect *clip, Transform *tr)
{
   Value painter[PAINTER_SIZE];
   int i, err;

   err = fixscript_get_array_range(heap, painter_val, 0, PAINTER_SIZE, painter);
   if (err) {
      fixscript_error(heap, error, err);
      return 0;
   }

   clip->x1 = painter[PAINTER_clip_x1].value;
   clip->y1 = painter[PAINTER_clip_y1].value;
   clip->x2 = painter[PAINTER_clip_x2].value;
   clip->y2 = painter[PAINTER_clip_y2].value;

   if (clip->x1 >= clip->x2 || clip->y1 >= clip->y2) {
      return 0;
   }

   for (i=0; i<6; i++) {
      tr->m[i] = fixscript_get_float(painter[i]);
   }
   return 1;
}


static Value painter_create(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   return orig_painter_create_func(heap, error, num_params, params, orig_painter_create_data);
}


static Value painter_fill_rect(Heap *heap, Value *error, int num_params, Value *params, void *func_data)
{
   Rect clip;
   Transform tr, img_tr;
   CachedImage *cimg;
   int flags, smoothing;
   float alpha;
   uint32_t color;
   int sx1, sy1, sx2, sy2;
   int x1, y1, x2, y2;

   if (params[0].value == canvas_painter.value && params[0].is_array) {
      if (!painter_get(heap, error, params[0], &clip, &tr)) {
         return fixscript_int(0);
      }

      x1 = params[1].value + (int)tr.m02;
      y1 = params[2].value + (int)tr.m12;
      x2 = x1 + params[3].value;
      y2 = y1 + params[4].value;

      if (clip.x1 > x1) x1 = clip.x1;
      if (clip.y1 > y1) y1 = clip.y1;
      if (clip.x2 < x2) x2 = clip.x2;
      if (clip.y2 < y2) y2 = clip.y2;

      if (x1 >= x2 || y1 >= y2) {
         return fixscript_int(0);
      }

      if (num_params == 6) {
         color = params[5].value;
         if (func_data == (void *)0) {
            if ((color >> 24) != 0xFF) {
               clear_rect(x1, y1, x2-x1, y2-y1);
            }
            if (color != 0) {
               fill_rect(x1, y1, x2-x1, y2-y1, (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF, (color >> 24) * (1.0f/255.0f));
            }
         }
         else {
            fill_rect(x1, y1, x2-x1, y2-y1, (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF, (color >> 24) * (1.0f/255.0f));
         }
      }
      else {
         if (decode_shader(heap, error, params[5], params[6], &tr, &cimg, &img_tr, &flags, &smoothing, &alpha)) {
            if (tr.m00 == 1.0f && tr.m11 == 1.0f && img_tr.m00 == 1.0f && img_tr.m01 == 0.0f && img_tr.m10 == 0.0f && img_tr.m11 == 1.0f && img_tr.m02 == (int)img_tr.m02 && img_tr.m12 == (int)img_tr.m12) {
               sx1 = x1 - (int)img_tr.m02;
               sy1 = y1 - (int)img_tr.m12;
               sx2 = x2 - (int)img_tr.m02;
               sy2 = y2 - (int)img_tr.m12;
               if (sx1 >= 0 && sy1 >= 0 && sx2 <= cimg->width && sy2 <= cimg->height && sx2-sx1 == x2-x1 && sy2-sy1 == y2-y1) {
                  draw_image(cimg->handle, x1, y1, x2-x1, y2-y1, sx1, sy1, sx2-sx1, sy2-sy1, smoothing, alpha);
                  return fixscript_int(0);
               }
            }
            fill_rect_image(x1, y1, x2-x1, y2-y1, cimg->handle, img_tr.m00, img_tr.m01, img_tr.m02, img_tr.m10, img_tr.m11, img_tr.m12, flags, smoothing, alpha);
         }
      }
      return fixscript_int(0);
   }
   
   if (num_params == 6) {
      if (func_data == (void *)0) {
         return orig_painter_clear_rect_func(heap, error, num_params, params, orig_painter_clear_rect_data);
      }
      else {
         return orig_painter_fill_rect_color_func(heap, error, num_params, params, orig_painter_fill_rect_color_data);
      }
   }
   else {
      return orig_painter_fill_rect_shader_func(heap, error, num_params, params, orig_painter_fill_rect_shader_data);
   }
}


static Value painter_fill_shape(Heap *heap, Value *error, int num_params, Value *params, void *func_data)
{
   Rect clip;
   Transform tr, img_tr;
   CachedImage *cimg;
   int flags, smoothing;
   float alpha;
   uint32_t *shape, color;
   int err, len;

   if (params[0].value == canvas_painter.value && params[0].is_array) {
      if (!painter_get(heap, error, params[0], &clip, &tr)) {
         return fixscript_int(0);
      }

      err = fixscript_get_array_length(heap, params[1], &len);
      if (!err) {
         err = fixscript_lock_array(heap, params[1], 0, len, (void **)&shape, 4, ACCESS_READ_ONLY);
      }
      if (err) {
         return fixscript_error(heap, error, err);
      }

      if (num_params == 3) {
         color = params[2].value;
         fill_shape(clip.x1, clip.y1, clip.x2-clip.x1, clip.y2-clip.y1, tr.m00, tr.m01, tr.m02, tr.m10, tr.m11, tr.m12, shape, shape + len, (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF, (color >> 24) * (1.0f/255.0f));
      }
      else {
         if (decode_shader(heap, error, params[2], params[3], &tr, &cimg, &img_tr, &flags, &smoothing, &alpha)) {
            fill_shape_image(
               clip.x1, clip.y1, clip.x2-clip.x1, clip.y2-clip.y1, tr.m00, tr.m01, tr.m02, tr.m10, tr.m11, tr.m12, shape, shape + len,
               cimg->handle, img_tr.m00, img_tr.m01, img_tr.m02, img_tr.m10, img_tr.m11, img_tr.m12, flags, smoothing, alpha
            );
         }
      }

      fixscript_unlock_array(heap, params[1], 0, len, (void **)&shape, 4, ACCESS_READ_ONLY);
      return fixscript_int(0);
   }

   if (num_params == 3) {
      return orig_painter_fill_shape_color_func(heap, error, num_params, params, orig_painter_fill_shape_color_data);
   }
   else {
      return orig_painter_fill_shape_shader_func(heap, error, num_params, params, orig_painter_fill_shape_shader_data);
   }
}


static Value painter_batch(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   return fixscript_int(0);
}


static void init_painter(Heap *heap)
{
   Value img, error;
   int i;

   img = fiximage_create(heap, 1, 1);
   if (!img.value) {
      fixscript_error(heap, &error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      fixscript_dump_value(heap, error, 1);
      return;
   }

   canvas_painter = fiximage_create_painter(heap, img, 0, 0);
   if (!canvas_painter.value) {
      fixscript_error(heap, &error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      fixscript_dump_value(heap, error, 1);
      return;
   }

   image_map = fixscript_create_hash(heap);
   if (!image_map.value) {
      fixscript_error(heap, &error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      fixscript_dump_value(heap, error, 1);
      return;
   }

   fixscript_ref(heap, canvas_painter);
   fixscript_ref(heap, image_map);

   fixscript_get_array_range(heap, canvas_painter, 0, PAINTER_SIZE, canvas_painter_values);
   for (i=0; i<PAINTER_SIZE; i++) {
      fixscript_ref(heap, canvas_painter_values[i]);
   }
}


void register_native_platform_gui_functions(Heap *heap)
{
   gui_heap = heap;

   fixscript_register_handle_types(&handles_offset, NUM_HANDLE_TYPES);

   fixscript_register_native_func(heap, "log#1", func_log, fixscript_get_native_func(heap, "log#1", NULL));

   fixscript_register_native_func(heap, "web_is_present#0", func_web_is_present, NULL);
   fixscript_register_native_func(heap, "web_set_auto_fullscreen#1", func_web_set_auto_fullscreen, NULL);
   fixscript_register_native_func(heap, "web_set_canvas_acceleration#1", func_web_set_canvas_acceleration, NULL);
   fixscript_register_native_func(heap, "web_is_painter_accelerated#1", func_web_is_painter_accelerated, NULL);
   fixscript_register_native_func(heap, "web_flush_drawing#0", func_web_flush_drawing, NULL);
   fixscript_register_native_func(heap, "web_ignore_drawing#0", func_web_ignore_drawing, NULL);
   fixscript_register_native_func(heap, "web_run_animation#2", func_web_run_animation, NULL);
   fixscript_register_native_func(heap, "web_is_mobile#0", func_web_is_mobile, NULL);
   fixscript_register_native_func(heap, "web_get_subpixel_order#0", func_web_get_subpixel_order, NULL);
   fixscript_register_native_func(heap, "web_get_safe_area#0", func_web_get_safe_area, NULL);

   fixscript_register_native_func(heap, "layer_create#11", func_layer_create, NULL);
   fixscript_register_native_func(heap, "layer_get_x#1", func_layer_get_position, (void *)0);
   fixscript_register_native_func(heap, "layer_get_y#1", func_layer_get_position, (void *)1);
   fixscript_register_native_func(heap, "layer_get_width#1", func_layer_get_position, (void *)2);
   fixscript_register_native_func(heap, "layer_get_height#1", func_layer_get_position, (void *)3);
   fixscript_register_native_func(heap, "layer_get_scroll_x#1", func_layer_get_position, (void *)4);
   fixscript_register_native_func(heap, "layer_get_scroll_y#1", func_layer_get_position, (void *)5);
   fixscript_register_native_func(heap, "layer_move#3", func_layer_move, NULL);
   fixscript_register_native_func(heap, "layer_resize#3", func_layer_resize, NULL);
   fixscript_register_native_func(heap, "layer_set_opacity#2", func_layer_set_opacity, NULL);
   fixscript_register_native_func(heap, "layer_scroll#3", func_layer_scroll, NULL);
   fixscript_register_native_func(heap, "layer_update#7", func_layer_update, NULL);

   orig_painter_create_func = fixscript_get_native_func(heap, "painter_create#1", &orig_painter_create_data);
   orig_painter_clear_rect_func = fixscript_get_native_func(heap, "painter_clear_rect#6", &orig_painter_clear_rect_data);
   orig_painter_fill_rect_color_func = fixscript_get_native_func(heap, "painter_fill_rect#6", &orig_painter_fill_rect_color_data);
   orig_painter_fill_rect_shader_func = fixscript_get_native_func(heap, "painter_fill_rect#7", &orig_painter_fill_rect_shader_data);
   orig_painter_fill_shape_color_func = fixscript_get_native_func(heap, "painter_fill_shape#3", &orig_painter_fill_shape_color_data);
   orig_painter_fill_shape_shader_func = fixscript_get_native_func(heap, "painter_fill_shape#4", &orig_painter_fill_shape_shader_data);

   if (!orig_painter_create_func) {
      fprintf(stderr, "error: FixImage functions must be registered before FixGUI\n");
      return;
   }

   fixscript_register_native_func(heap, "painter_create#1", painter_create, NULL);
   fixscript_register_native_func(heap, "painter_clear_rect#6", painter_fill_rect, (void *)0);
   fixscript_register_native_func(heap, "painter_fill_rect#6", painter_fill_rect, (void *)1);
   fixscript_register_native_func(heap, "painter_fill_rect#7", painter_fill_rect, (void *)1);
   fixscript_register_native_func(heap, "painter_fill_shape#3", painter_fill_shape, NULL);
   fixscript_register_native_func(heap, "painter_fill_shape#4", painter_fill_shape, NULL);
   fixscript_register_native_func(heap, "painter_batch_begin#1", painter_batch, NULL);
   fixscript_register_native_func(heap, "painter_batch_flush#1", painter_batch, NULL);
   fixscript_register_native_func(heap, "painter_batch_end#1", painter_batch, NULL);

   init_painter(heap);
}


static void repaint(void *data)
{
   int x=0, y=0, width=0, height=0;

   if (data == NULL) {
      repaint_notified = 0;
   }

   if (virtual_get_dirty_rect(&x, &y, &width, &height, canvas_width, canvas_height)) {
      layer_update(main_layer_handle, x, y, width, height, layer_paint_func, NULL);
   }
}


static void handle_repainting(uint32_t time, void *data)
{
   repaint(NULL);
}


void virtual_repaint_notify()
{
   if (ignore_repaint) return;

   if (!repaint_notified) {
      repaint_notified = 1;
      //wasm_sleep(0, repaint, NULL);
      request_animation_frame(handle_repainting, NULL);
   }
}


void virtual_set_cursor(int type)
{
   set_cursor(type);
}


void canvas_resized(int width, int height, float scale, int safe_left, int safe_top, int safe_right, int safe_bottom) EXPORT("canvas_resized")
{
   canvas_width = width;
   canvas_height = height;
   canvas_scale = scale;
   safe_area[0] = safe_left;
   safe_area[1] = safe_top;
   safe_area[2] = safe_right;
   safe_area[3] = safe_bottom;

   layer_resize(main_layer_handle, width, height);

   ignore_repaint = 1;
   virtual_handle_resize(canvas_width, canvas_height, canvas_scale);
   ignore_repaint = 0;
   repaint(NULL);
}


void mouse_moved(int x, int y, int mod) EXPORT("mouse_moved")
{
   ignore_repaint = 1;
   virtual_handle_mouse_event(mod & SCRIPT_MOD_MOUSE_BUTTONS? EVENT_MOUSE_DRAG : EVENT_MOUSE_MOVE, x, y, 0, mod, 0, 0);
   ignore_repaint = 0;
   repaint(NULL);
}


void mouse_down(int x, int y, int button, int mod, int click_count) EXPORT("mouse_down")
{
   ignore_repaint = 1;
   virtual_handle_mouse_event(EVENT_MOUSE_DOWN, x, y, button, mod, click_count, 0);
   ignore_repaint = 0;
   repaint(NULL);
}


void mouse_up(int x, int y, int button, int mod, int click_count) EXPORT("mouse_up")
{
   ignore_repaint = 1;
   virtual_handle_mouse_event(EVENT_MOUSE_UP, x, y, button, mod, click_count, 0);
   ignore_repaint = 0;
   repaint(NULL);
}


void mouse_wheel(int x, int y, int mod, float wheel_x, float wheel_y, int scroll_x, int scroll_y) EXPORT("mouse_wheel")
{
   ignore_repaint = 1;
   virtual_handle_mouse_wheel(x, y, mod, wheel_x, wheel_y, scroll_x, scroll_y);
   ignore_repaint = 0;
   repaint(NULL);
}


static int map_key(int key, int location)
{
   switch (key) {
      case 27:  return KEY_ESCAPE;
      case 112: return KEY_F1;
      case 113: return KEY_F2;
      case 114: return KEY_F3;
      case 115: return KEY_F4;
      case 116: return KEY_F5;
      case 117: return KEY_F6;
      case 118: return KEY_F7;
      case 119: return KEY_F8;
      case 120: return KEY_F9;
      case 121: return KEY_F10;
      case 122: return KEY_F11;
      case 123: return KEY_F12;
      case 44:  return KEY_PRINT_SCREEN;
      case 145: return KEY_SCROLL_LOCK;
      case 19:  return KEY_PAUSE;
      case 192: return KEY_GRAVE;
      case 49:  return KEY_NUM1;
      case 50:  return KEY_NUM2;
      case 51:  return KEY_NUM3;
      case 52:  return KEY_NUM4;
      case 53:  return KEY_NUM5;
      case 54:  return KEY_NUM6;
      case 55:  return KEY_NUM7;
      case 56:  return KEY_NUM8;
      case 57:  return KEY_NUM9;
      case 48:  return KEY_NUM0;
      case 173: return KEY_MINUS;
      case 61:  return KEY_EQUAL;
      case 8:   return KEY_BACKSPACE;
      case 9:   return KEY_TAB;
      case 81:  return KEY_Q;
      case 87:  return KEY_W;
      case 69:  return KEY_E;
      case 82:  return KEY_R;
      case 84:  return KEY_T;
      case 89:  return KEY_Y;
      case 85:  return KEY_U;
      case 73:  return KEY_I;
      case 79:  return KEY_O;
      case 80:  return KEY_P;
      case 219: return KEY_LBRACKET;
      case 221: return KEY_RBRACKET;
      case 220: return KEY_BACKSLASH;
      case 20:  return KEY_CAPS_LOCK;
      case 65:  return KEY_A;
      case 83:  return KEY_S;
      case 68:  return KEY_D;
      case 70:  return KEY_F;
      case 71:  return KEY_G;
      case 72:  return KEY_H;
      case 74:  return KEY_J;
      case 75:  return KEY_K;
      case 76:  return KEY_L;
      case 59:  return KEY_SEMICOLON;
      case 222: return KEY_APOSTROPHE;
      case 13:  return location == 3? KEY_NUMPAD_ENTER : KEY_ENTER;
      case 16:  return location == 2? KEY_RSHIFT : KEY_LSHIFT;
      case 90:  return KEY_Z;
      case 88:  return KEY_X;
      case 67:  return KEY_C;
      case 86:  return KEY_V;
      case 66:  return KEY_B;
      case 78:  return KEY_N;
      case 77:  return KEY_M;
      case 188: return KEY_COMMA;
      case 190: return KEY_PERIOD;
      case 191: return KEY_SLASH;
      case 17:  return location == 2? KEY_RCONTROL : KEY_LCONTROL;
      case 91:  return location == 2? KEY_RMETA : KEY_LMETA;
      case 18:  return location == 2? KEY_RALT : KEY_LALT;
      case 32:  return KEY_SPACE;
      case 93:  return KEY_RMENU;
      case 45:  return location == 3? KEY_NUMPAD0 : KEY_INSERT;
      case 46:  return location == 3? KEY_NUMPAD_DOT : KEY_DELETE;
      case 36:  return location == 3? KEY_NUMPAD7 : KEY_HOME;
      case 35:  return location == 3? KEY_NUMPAD1 : KEY_END;
      case 33:  return location == 3? KEY_NUMPAD9 : KEY_PAGE_UP;
      case 34:  return location == 3? KEY_NUMPAD3 : KEY_PAGE_DOWN;
      case 37:  return location == 3? KEY_NUMPAD4 : KEY_LEFT;
      case 38:  return location == 3? KEY_NUMPAD8 : KEY_UP;
      case 39:  return location == 3? KEY_NUMPAD6 : KEY_RIGHT;
      case 40:  return location == 3? KEY_NUMPAD2 : KEY_DOWN;
      case 144: return KEY_NUM_LOCK;
      case 111: return KEY_NUMPAD_SLASH;
      case 106: return KEY_NUMPAD_STAR;
      case 109: return KEY_NUMPAD_MINUS;
      case 107: return KEY_NUMPAD_PLUS;
      case 110: return KEY_NUMPAD_DOT;
      case 96:  return KEY_NUMPAD0;
      case 97:  return KEY_NUMPAD1;
      case 98:  return KEY_NUMPAD2;
      case 99:  return KEY_NUMPAD3;
      case 100: return KEY_NUMPAD4;
      case 101: return KEY_NUMPAD5;
      case 102: return KEY_NUMPAD6;
      case 103: return KEY_NUMPAD7;
      case 104: return KEY_NUMPAD8;
      case 105: return KEY_NUMPAD9;
      case 12:  return location == 3? KEY_NUMPAD5 : -1;
   }
   return -1;
}


static int allow_key(int key, int mod)
{
   switch (key) {
      case KEY_V:
         return (mod & SCRIPT_MOD_CTRL) != 0? 1 : 0;
   }
   return 0;
}


static int prevent_key(int key, int mod)
{
   switch (key) {
      case KEY_TAB:
      case KEY_ENTER:
      case KEY_NUMPAD_ENTER:
         return 1;

      case KEY_A:
         return (mod & SCRIPT_MOD_CTRL) != 0? 1 : 0;
   }
   return 0;
}


int key_pressed(int key, int location, int mod) EXPORT("key_pressed")
{
   int ret;

   key = map_key(key, location);
   if (key < 0) return 0;

   ignore_repaint = 1;
   ret = virtual_handle_key_event(EVENT_KEY_DOWN, key, mod);
   ignore_repaint = 0;
   repaint(NULL);

   if (allow_key(key, mod)) {
      return 0;
   }
   if (prevent_key(key, mod)) {
      return 1;
   }
   return ret;
}


int key_released(int key, int location, int mod) EXPORT("key_released")
{
   int ret;

   key = map_key(key, location);
   if (key < 0) return 0;

   ignore_repaint = 1;
   ret = virtual_handle_key_event(EVENT_KEY_UP, key, mod);
   ignore_repaint = 0;
   repaint(NULL);

   if (allow_key(key, mod)) {
      return 0;
   }
   if (prevent_key(key, mod)) {
      return 1;
   }
   return ret;
}


void text_typed(uint16_t *chars, int mod) EXPORT("text_typed")
{
   Value value;
   char *s = NULL;
   
   value = fixscript_create_string_utf16(gui_heap, chars, -1);
   fixscript_get_string(gui_heap, value, 0, -1, &s, NULL);
   if (!s) return;
   ignore_repaint = 1;
   virtual_handle_key_typed_event(s, mod);
   ignore_repaint = 0;
   repaint(NULL);
   free(s);
}


void touch_start(int id, int x, int y, uint32_t time) EXPORT("touch_start")
{
   touch_active_points++;
   if (touch_active_points == 1 && id == 0) {
      touch_mouse_emitter = 1;
      touch_mouse_x = x;
      touch_mouse_y = y;
      touch_mouse_time = time;
   }
   ignore_repaint = 1;
   virtual_handle_touch_event(EVENT_TOUCH_START, id, x, y, touch_mouse_emitter && id == 0, 0, time);
   ignore_repaint = 0;
   repaint(NULL);
}


void touch_move(int id, int x, int y, uint32_t time) EXPORT("touch_move")
{
   ignore_repaint = 1;
   virtual_handle_touch_event(EVENT_TOUCH_MOVE, id, x, y, touch_mouse_emitter && id == 0, 0, time);
   ignore_repaint = 0;
   repaint(NULL);
}


static void do_mouse_up(MouseFinish *finish)
{
   ignore_repaint = 1;
   virtual_handle_mouse_event(EVENT_MOUSE_UP, finish->x, finish->y, MOUSE_BUTTON_LEFT, 0, 1, 1);
   virtual_handle_mouse_event(EVENT_MOUSE_LEAVE, 0, 0, 0, 0, 0, 1);
   ignore_repaint = 0;
   repaint(NULL);
}


static void finish_mouse_up(void *data)
{
   MouseFinish *finish = data;

   if (!finish->finished) {
      do_mouse_up(finish);
   }

   free(finish);
   if (finish == touch_mouse_finish) {
      touch_mouse_finish = NULL;
   }
}


void touch_end(int id, int x, int y, int cancel, uint32_t time) EXPORT("touch_end")
{
   touch_active_points--;
   ignore_repaint = 1;
   virtual_handle_touch_event(EVENT_TOUCH_END, id, x, y, touch_mouse_emitter && id == 0, cancel, time);
   if (touch_mouse_emitter && id == 0) {
      if (!cancel) {
         if (time - touch_mouse_time <= 250 && abs(x - touch_mouse_x) <= 10*canvas_scale && abs(y - touch_mouse_y) < 10*canvas_scale) {
            virtual_handle_mouse_event(EVENT_MOUSE_ENTER, x, y, 0, 0, 0, 1);
            virtual_handle_mouse_event(EVENT_MOUSE_DOWN, x, y, MOUSE_BUTTON_LEFT, SCRIPT_MOD_LBUTTON, 1, 1);

            if (touch_mouse_finish) {
               do_mouse_up(touch_mouse_finish);
               touch_mouse_finish->finished = 1;
            }
            touch_mouse_finish = malloc(sizeof(MouseFinish));
            touch_mouse_finish->finished = 0;
            touch_mouse_finish->x = x;
            touch_mouse_finish->y = y;
            wasm_sleep(75, finish_mouse_up, touch_mouse_finish);
         }
      }
      touch_mouse_emitter = 0;
   }
   ignore_repaint = 0;
   repaint(NULL);
}


int main(int argc, char **argv)
{
   if (!app_main(0, NULL) || !gui_heap) {
      return 0;
   }

   create_canvas(&canvas_width, &canvas_height, &canvas_scale, &safe_area[0], &safe_area[1], &safe_area[2], &safe_area[3]);
   main_layer_handle = layer_create(0, 0, 0, canvas_width, canvas_height, 0, 0, 1, LAYER_TRANSLUCENT, NULL, NULL);
   canvas_resized(canvas_width, canvas_height, canvas_scale, safe_area[0], safe_area[1], safe_area[2], safe_area[3]);
   return 0;
}
