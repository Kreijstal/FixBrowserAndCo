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
#include <math.h>
#if defined(__APPLE__) || defined(__HAIKU__) || defined(__SYMBIAN32__)
#include <pthread.h>
#endif
#include "fixgui_common.h"
#include "fiximage.h"

enum {
   VIEW_handle,
   VIEW_design_width,
   VIEW_design_height,
   VIEW_design_anchors,
   VIEW_handle_mouse_event,
   VIEW_handle_touch_event,
   VIEW_handle_key_event,
   VIEW_handle_focus_event,
   VIEW_SIZE
};

enum {
   WIN_handle_destroy = VIEW_SIZE,
   WIN_handle_close,
   WIN_handle_resize,
   WIN_handle_activate,
   WIN_SIZE
};

enum {
   LABEL_SIZE = VIEW_SIZE
};

enum {
   TEXTFIELD_SIZE = VIEW_SIZE
};

enum {
   TEXTAREA_SIZE = VIEW_SIZE
};

enum {
   BTN_click_action = VIEW_SIZE,
   BTN_click_data,
   BTN_SIZE
};

enum {
   TABLE_click_action = VIEW_SIZE,
   TABLE_click_data,
   TABLE_right_click_action,
   TABLE_right_click_data,
   TABLE_space_key_action,
   TABLE_space_key_data,
   TABLE_sort_action,
   TABLE_sort_data,
   TABLE_SIZE
};

enum {
   CANVAS_handle_destroy = VIEW_SIZE,
   CANVAS_handle_resize,
   CANVAS_handle_paint,
   CANVAS_SIZE
};

enum {
   MENU_handle,
   MENU_SIZE
};

enum {
   MENU_ITEM_menu,
   MENU_ITEM_idx,
   MENU_ITEM_title,
   MENU_ITEM_submenu,
   MENU_ITEM_action,
   MENU_ITEM_data,
   MENU_ITEM_id,
   MENU_ITEM_SIZE
};

enum {
   VIEW_SIZING_grid_x,
   VIEW_SIZING_grid_y,
   VIEW_SIZING_form_small,
   VIEW_SIZING_form_medium,
   VIEW_SIZING_form_large,
   VIEW_SIZING_view_small,
   VIEW_SIZING_view_medium,
   VIEW_SIZING_view_large,
   VIEW_SIZING_SIZE
};

enum {
   TIMER_interval,
   TIMER_callback,
   TIMER_data,
   TIMER_mode,
   TIMER_run,
   TIMER_SIZE
};

enum {
   NOTIFYICON_handle,
   NOTIFYICON_handle_click_action,
   NOTIFYICON_SIZE
};

enum {
   SFM_SIZE,
   SFM_ASCENT,
   SFM_DESCENT,
   SFM_HEIGHT
};

typedef struct {
   MainThreadFunc func;
   void *data;
} MainThreadData;

#define NUM_HANDLE_TYPES 5
#define HANDLE_TYPE_VIEW       (handles_offset+0)
#define HANDLE_TYPE_MENU       (handles_offset+1)
#define HANDLE_TYPE_WORKER     (handles_offset+2)
#define HANDLE_TYPE_FONT       (handles_offset+3)
#define HANDLE_TYPE_NOTIFYICON (handles_offset+4)

#define MAX_MESSAGES 1000

static volatile int handles_offset = 0;

#ifndef __EMSCRIPTEN__
#if defined(__APPLE__) || defined(__HAIKU__) || defined(__SYMBIAN32__)
static pthread_key_t cur_thread_worker_key;
#else
static __thread WorkerCommon *cur_thread_worker = NULL;
#endif
#endif

static Heap *gui_heap;
static Heap *fixio_heap;
static void (*fixio_process_func)(Heap *);

static int num_active_windows = 0;

#if !defined(_WIN32) && !defined(__SYMBIAN32__)
float roundf(float x);
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
#endif


static plat_char *get_plat_string_range(Heap *heap, Value *error, Value str_val, int off, int len)
{
   int err;
   plat_char *str;

   #if PLAT_CHAR == 1
      err = fixscript_get_string(heap, str_val, off, len, (char **)&str, NULL);
   #elif PLAT_CHAR == 2
      err = fixscript_get_string_utf16(heap, str_val, off, len, (unsigned short **)&str, NULL);
   #else
      #error "unknown platform char type"
   #endif

   if (err) {
      fixscript_error(heap, error, err);
      return NULL;
   }
   return str;
}


static plat_char *get_plat_string(Heap *heap, Value *error, Value str_val)
{
   return get_plat_string_range(heap, error, str_val, 0, -1);
}


static Value create_plat_string(Heap *heap, const plat_char *str)
{
   #if PLAT_CHAR == 1
      return fixscript_create_string(heap, (char *)str, -1);
   #elif PLAT_CHAR == 2
      return fixscript_create_string_utf16(heap, (unsigned short *)str, -1);
   #else
      #error "unknown platform char type"
   #endif
}


static void free_plat_string(plat_char *str)
{
   free(str);
}


static plat_char *dup_plat_string(plat_char *str)
{
   #if PLAT_CHAR == 1
      return strdup(str);
   #elif PLAT_CHAR == 2
      return _wcsdup(str);
   #else
      #error "unknown platform char type"
   #endif
}


static void *get_handle(Heap *heap, Value *error, int expected_type, Value value, int idx)
{
   void *handle;
   int err;
   
   err = fixscript_get_array_elem(heap, value, idx, &value);
   if (err) {
      if (error) fixscript_error(heap, error, err);
      return NULL;
   }
   if (!value.value) {
      if (error) *error = fixscript_create_error_string(heap, "invalid native handle");
      return NULL;
   }

   handle = fixscript_get_handle(heap, value, expected_type, NULL);
   if (!handle) {
      if (error) *error = fixscript_create_error_string(heap, "invalid native handle");
      return NULL;
   }

   return handle;
}


static void *view_handler_func(Heap *heap, int op, void *p1, void *p2)
{
   ViewCommon *view = p1;

   switch (op) {
      case HANDLE_OP_FREE:
         free_view(p1);
         break;

      case HANDLE_OP_MARK_REFS:
         if (view->parent) {
            fixscript_mark_ref(heap, ((ViewCommon *)view->parent)->instance);
         }
         if (view->prev) {
            fixscript_mark_ref(heap, ((ViewCommon *)view->prev)->instance);
         }
         if (view->next) {
            fixscript_mark_ref(heap, ((ViewCommon *)view->next)->instance);
         }
         if (view->first_child) {
            fixscript_mark_ref(heap, ((ViewCommon *)view->first_child)->instance);
         }
         if (view->last_child) {
            fixscript_mark_ref(heap, ((ViewCommon *)view->last_child)->instance);
         }

         if (view->type == TYPE_WINDOW) {
            if (view->window.menu.value) {
               fixscript_mark_ref(heap, view->window.menu);
            }
         }

         #ifdef FIXGUI_VIRTUAL
            virtual_view_mark_refs((View *)view);
         #endif
         break;
   }
   return NULL;
}


static Value view_create(Heap *heap, View *view, int size, int type)
{
   Value instance, handle_val;
   int err;

   instance = fixscript_create_array(heap, size);
   if (!instance.value) return fixscript_int(0);

   handle_val = fixscript_create_value_handle(heap, HANDLE_TYPE_VIEW, view, view_handler_func);
   if (!handle_val.value) return fixscript_int(0);

   err = fixscript_set_array_elem(heap, instance, VIEW_handle, handle_val);
   if (err) return fixscript_int(0);

   ((ViewCommon *)view)->heap = heap;
   ((ViewCommon *)view)->instance = instance;
   ((ViewCommon *)view)->type = type;
   return instance;
}


View *view_get_native(Heap *heap, Value *error, Value instance, int type)
{
   View *view = get_handle(heap, error, HANDLE_TYPE_VIEW, instance, VIEW_handle);
   if (view && type != -1 && ((ViewCommon *)view)->type != type) {
      *error = fixscript_create_error_string(heap, "invalid view type");
      return NULL;
   }
   return view;
}


Menu *menu_get_native(Heap *heap, Value *error, Value instance)
{
   return get_handle(heap, error, HANDLE_TYPE_MENU, instance, MENU_handle);
}


NotifyIcon *notify_icon_get_native(Heap *heap, Value *error, Value instance)
{
   return get_handle(heap, error, HANDLE_TYPE_NOTIFYICON, instance, NOTIFYICON_handle);
}


static Value create_rect_array(Heap *heap, Value *error, Rect *rect)
{
   Value rect_val;
   Value values[4];
   int err;
   
   rect_val = fixscript_create_array(heap, 4);
   if (!rect_val.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   values[0] = fixscript_int(rect->x1);
   values[1] = fixscript_int(rect->y1);
   values[2] = fixscript_int(rect->x2);
   values[3] = fixscript_int(rect->y2);

   err = fixscript_set_array_range(heap, rect_val, 0, 4, values);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   return rect_val;
}


static int get_rect_from_array(Heap *heap, Value rect_val, Rect *rect)
{
   Value values[4];
   int err;

   err = fixscript_get_array_range(heap, rect_val, 0, 4, values);
   if (err) return err;

   rect->x1 = fixscript_get_int(values[0]);
   rect->y1 = fixscript_get_int(values[1]);
   rect->x2 = fixscript_get_int(values[2]);
   rect->y2 = fixscript_get_int(values[3]);

   return FIXSCRIPT_SUCCESS;
}


static Value func_view_destroy(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ViewCommon *view;

   view = (ViewCommon *)view_get_native(heap, error, params[0], -1);
   if (!view) return fixscript_int(0);

   view_destroy((View *)view);

   if (view->type == TYPE_WINDOW) {
      fixscript_unref(heap, view->instance);
      if (--num_active_windows == 0) {
         quit_app();
      }
   }

   if (view->parent) {
      if (view->prev) {
         ((ViewCommon *)view->prev)->next = view->next;
         if (view->next) {
            ((ViewCommon *)view->next)->prev = view->prev;
         }
      }
      else {
         ((ViewCommon *)view->parent)->first_child = view->next;
         if (view->next) {
            ((ViewCommon *)view->next)->prev = NULL;
         }
      }

      if (view->next) {
         ((ViewCommon *)view->next)->prev = view->prev;
         if (view->prev) {
            ((ViewCommon *)view->prev)->next = view->next;
         }
      }
      else {
         ((ViewCommon *)view->parent)->last_child = view->prev;
         if (view->prev) {
            ((ViewCommon *)view->prev)->next = NULL;
         }
      }

      view->parent = NULL;
      view->prev = NULL;
      view->next = NULL;
   }

   return fixscript_int(0);
}


static Value func_view_get_rect(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;
   Rect rect;

   view = view_get_native(heap, error, params[0], -1);
   if (!view) return fixscript_int(0);

   view_get_rect(view, &rect);
   return create_rect_array(heap, error, &rect);
}


static Value func_view_set_rect(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;
   Rect rect;
   int err;

   view = view_get_native(heap, error, params[0], -1);
   if (!view) return fixscript_int(0);

   if (num_params == 5) {
      rect.x1 = params[1].value;
      rect.y1 = params[2].value;
      rect.x2 = rect.x1 + params[3].value;
      rect.y2 = rect.y1 + params[4].value;
   }
   else {
      err = get_rect_from_array(heap, params[1], &rect);
      if (err) {
         return fixscript_error(heap, error, err);
      }
   }

   view_set_rect(view, &rect);
   return fixscript_int(0);
}


static Value func_view_get_content_rect(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;
   Rect rect;

   view = view_get_native(heap, error, params[0], -1);
   if (!view) return fixscript_int(0);

   view_get_content_rect(view, &rect);
   return create_rect_array(heap, error, &rect);
}


static Value func_view_get_inner_rect(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;
   Rect rect;

   view = view_get_native(heap, error, params[0], -1);
   if (!view) return fixscript_int(0);

   view_get_inner_rect(view, &rect);
   return create_rect_array(heap, error, &rect);
}


static Value func_view_set_visible(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;

   view = view_get_native(heap, error, params[0], -1);
   if (!view) return fixscript_int(0);

   view_set_visible(view, fixscript_get_int(params[1]));
   return fixscript_int(0);
}


static Value func_view_add(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ViewCommon *parent, *view;

   parent = (ViewCommon *)view_get_native(heap, error, params[0], -1);
   if (!parent) return fixscript_int(0);

   view = (ViewCommon *)view_get_native(heap, error, params[1], -1);
   if (!view) return fixscript_int(0);

   if (view->parent) {
      *error = fixscript_create_error_string(heap, "view already has parent");
      return fixscript_int(0);
   }

   if (parent->type != TYPE_WINDOW && parent->type != TYPE_CANVAS) {
      *error = fixscript_create_error_string(heap, "parent must be either window or canvas");
      return fixscript_int(0);
   }

   if (view->type == TYPE_WINDOW) {
      *error = fixscript_create_error_string(heap, "can't add window to another view");
      return fixscript_int(0);
   }

   if (!view_add((View *)parent, (View *)view)) {
      *error = fixscript_create_error_string(heap, "can't add view");
      return fixscript_int(0);
   }

   view->parent = (View *)parent;
   if (parent->last_child) {
      view->prev = parent->last_child;
      ((ViewCommon *)view->prev)->next = (View *)view;
      parent->last_child = (View *)view;
   }
   else {
      parent->first_child = (View *)view;
      parent->last_child = (View *)view;
   }

   return fixscript_int(0);
}


static Value func_view_get_parent(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ViewCommon *view;

   view = (ViewCommon *)view_get_native(heap, error, params[0], -1);
   if (!view) return fixscript_int(0);

   if (view->parent) {
      return ((ViewCommon *)view->parent)->instance;
   }
   return fixscript_int(0);
}


static Value func_view_get_next(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ViewCommon *view;

   view = (ViewCommon *)view_get_native(heap, error, params[0], -1);
   if (!view) return fixscript_int(0);

   if (view->next) {
      return ((ViewCommon *)view->next)->instance;
   }
   return fixscript_int(0);
}


static Value func_view_get_prev(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ViewCommon *view;

   view = (ViewCommon *)view_get_native(heap, error, params[0], -1);
   if (!view) return fixscript_int(0);

   if (view->prev) {
      return ((ViewCommon *)view->prev)->instance;
   }
   return fixscript_int(0);
}


static Value func_view_get_first_child(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ViewCommon *view;

   view = (ViewCommon *)view_get_native(heap, error, params[0], -1);
   if (!view) return fixscript_int(0);

   if (view->first_child) {
      return ((ViewCommon *)view->first_child)->instance;
   }
   return fixscript_int(0);
}


static Value func_view_get_last_child(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ViewCommon *view;

   view = (ViewCommon *)view_get_native(heap, error, params[0], -1);
   if (!view) return fixscript_int(0);

   if (view->last_child) {
      return ((ViewCommon *)view->last_child)->instance;
   }
   return fixscript_int(0);
}


static Value func_view_get_child_count(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ViewCommon *view, *v;
   int cnt;

   view = (ViewCommon *)view_get_native(heap, error, params[0], -1);
   if (!view) return fixscript_int(0);

   for (v = (ViewCommon *)view->first_child, cnt=0; v; v = (ViewCommon *)v->next) {
      cnt++;
   }

   return fixscript_int(cnt);
}


static Value func_view_get_child(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ViewCommon *view, *v;
   int idx = params[1].value;
   int cnt;

   view = (ViewCommon *)view_get_native(heap, error, params[0], -1);
   if (!view) return fixscript_int(0);

   for (v = (ViewCommon *)view->first_child, cnt=0; v; v = (ViewCommon *)v->next, cnt++) {
      if (idx == cnt) {
         return ((ViewCommon *)v)->instance;
      }
   }

   return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_BOUNDS);
}


static Value func_view_focus(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;

   view = view_get_native(heap, error, params[0], -1);
   if (!view) return fixscript_int(0);

   view_focus(view);
   return fixscript_int(0);
}


static Value func_view_has_focus(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;

   view = view_get_native(heap, error, params[0], -1);
   if (!view) return fixscript_int(0);

   return fixscript_int(view_has_focus(view));
}


static Value func_view_get_sizing(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view = NULL;
   Value ret, values[VIEW_SIZING_SIZE];
   float x, y;
   int form_small, form_medium, form_large;
   int view_small, view_medium, view_large;
   int err;

   if (params[0].value) {
      view = view_get_native(heap, NULL, params[0], -1);
   }

   view_get_sizing(view, &x, &y, &form_small, &form_medium, &form_large, &view_small, &view_medium, &view_large);

   values[VIEW_SIZING_grid_x] = fixscript_float(x);
   values[VIEW_SIZING_grid_y] = fixscript_float(y);
   values[VIEW_SIZING_form_small] = fixscript_int(form_small);
   values[VIEW_SIZING_form_medium] = fixscript_int(form_medium);
   values[VIEW_SIZING_form_large] = fixscript_int(form_large);
   values[VIEW_SIZING_view_small] = fixscript_int(view_small);
   values[VIEW_SIZING_view_medium] = fixscript_int(view_medium);
   values[VIEW_SIZING_view_large] = fixscript_int(view_large);

   ret = fixscript_create_array(heap, VIEW_SIZING_SIZE);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   err = fixscript_set_array_range(heap, ret, 0, VIEW_SIZING_SIZE, values);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   return ret;
}


static Value func_view_get_default_size(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;
   int width, height;

   view = view_get_native(heap, error, params[0], -1);
   if (!view) return fixscript_int(0);

   view_get_default_size(view, &width, &height);
   *error = fixscript_int(height);
   return fixscript_int(width);
}


static Value func_view_get_scale(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view = NULL;

   if (params[0].value) {
      view = view_get_native(heap, error, params[0], -1);
      if (!view) return fixscript_int(0);
   }

   return fixscript_float(view_get_scale(view));
}


static Value func_view_set_cursor(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;

   view = view_get_native(heap, error, params[0], -1);
   if (!view) return fixscript_int(0);

   view_set_cursor(view, fixscript_get_int(params[1]));
   return fixscript_int(0);
}


static Value func_view_get_cursor(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;

   view = view_get_native(heap, error, params[0], -1);
   if (!view) return fixscript_int(0);

   return fixscript_int(view_get_cursor(view));
}


static Value func_window_create(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   plat_char *title;
   int width, height, flags;
   View *view;
   Value instance;

   title = get_plat_string(heap, error, params[0]);
   if (!title) {
      return fixscript_int(0);
   }

   width = fixscript_get_int(params[1]);
   height = fixscript_get_int(params[2]);
   flags = fixscript_get_int(params[3]);

   view = window_create(title, width, height, flags);
   free_plat_string(title);
   if (!view) {
      *error = fixscript_create_error_string(heap, "window creation failed");
      return fixscript_int(0);
   }

   instance = view_create(heap, view, WIN_SIZE, TYPE_WINDOW);
   if (!instance.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   fixscript_ref(heap, instance);
   num_active_windows++;
   return instance;
}


static Value func_window_get_title(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;
   plat_char *title;
   Value ret;

   view = view_get_native(heap, error, params[0], TYPE_WINDOW);
   if (!view) return fixscript_int(0);

   title = window_get_title(view);

   ret = create_plat_string(heap, title);
   free(title);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return ret;
}


static Value func_window_set_title(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;
   plat_char *title;

   view = view_get_native(heap, error, params[0], TYPE_WINDOW);
   if (!view) return fixscript_int(0);

   title = get_plat_string(heap, error, params[1]);
   if (!title) return fixscript_int(0);

   window_set_title(view, title);
   free(title);
   return fixscript_int(0);
}


static Value func_window_set_minimum_size(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;

   view = view_get_native(heap, error, params[0], TYPE_WINDOW);
   if (!view) return fixscript_int(0);

   window_set_minimum_size(view, fixscript_get_int(params[1]), fixscript_get_int(params[2]));
   return fixscript_int(0);
}


static Value func_window_is_maximized(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;

   view = view_get_native(heap, error, params[0], TYPE_WINDOW);
   if (!view) return fixscript_int(0);
   
   return fixscript_int(window_is_maximized(view));
}


static Value func_window_set_status_text(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;
   plat_char *text;

   view = view_get_native(heap, error, params[0], TYPE_WINDOW);
   if (!view) return fixscript_int(0);

   text = get_plat_string(heap, error, params[1]);
   if (!text) {
      return fixscript_int(0);
   }

   window_set_status_text(view, text);

   free_plat_string(text);
   return fixscript_int(0);
}


static Value func_window_set_menu(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ViewCommon *view;
   Menu *old_menu = NULL, *new_menu = NULL;

   view = (ViewCommon *)view_get_native(heap, error, params[0], TYPE_WINDOW);
   if (!view) return fixscript_int(0);

   if (view->type != TYPE_WINDOW) {
      *error = fixscript_create_error_string(heap, "not a window handle");
      return fixscript_int(0);
   }

   if (view->window.menu.value) {
      old_menu = menu_get_native(heap, error, view->window.menu);
      if (!old_menu) return fixscript_int(0);
   }
   
   if (params[1].value) {
      new_menu = menu_get_native(heap, error, params[1]);
      if (!new_menu) return fixscript_int(0);
   }

   if (old_menu == new_menu) {
      return fixscript_int(0);
   }

   if (((MenuCommon *)new_menu)->parent) {
      *error = fixscript_create_error_string(heap, "can't set submenu");
      return fixscript_int(0);
   }

   if (window_set_menu((View *)view, old_menu, new_menu)) {
      view->window.menu = params[1];
   }
   else {
      *error = fixscript_create_error_string(heap, "can't set menu");
   }
   return fixscript_int(0);
}


static Value func_window_get_menu(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ViewCommon *view;

   view = (ViewCommon *)view_get_native(heap, error, params[0], TYPE_WINDOW);
   if (!view) return fixscript_int(0);

   if (view->type != TYPE_WINDOW) {
      *error = fixscript_create_error_string(heap, "not a window handle");
      return fixscript_int(0);
   }

   return view->window.menu;
}


static Value func_label_create(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   plat_char *label;
   View *view;
   Value instance;

   label = get_plat_string(heap, error, params[0]);
   if (!label) {
      return fixscript_int(0);
   }

   view = label_create(label);
   free_plat_string(label);
   if (!view) {
      *error = fixscript_create_error_string(heap, "label creation failed");
      return fixscript_int(0);
   }

   instance = view_create(heap, view, LABEL_SIZE, TYPE_LABEL);
   if (!instance.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   return instance;
}


static Value func_label_get_label(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;
   plat_char *label;
   Value ret;

   view = view_get_native(heap, error, params[0], TYPE_LABEL);
   if (!view) return fixscript_int(0);

   label = label_get_label(view);

   ret = create_plat_string(heap, label);
   free(label);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return ret;
}


static Value func_label_set_label(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;
   plat_char *label;

   view = view_get_native(heap, error, params[0], TYPE_LABEL);
   if (!view) return fixscript_int(0);

   label = get_plat_string(heap, error, params[1]);
   if (!label) return fixscript_int(0);

   label_set_label(view, label);
   free(label);
   return fixscript_int(0);
}


static Value func_text_field_create(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;
   Value instance;

   view = text_field_create();
   if (!view) {
      *error = fixscript_create_error_string(heap, "text field creation failed");
      return fixscript_int(0);
   }

   instance = view_create(heap, view, TEXTFIELD_SIZE, TYPE_TEXT_FIELD);
   if (!instance.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   return instance;
}


static Value func_text_field_get_text(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;
   plat_char *text;
   Value ret;

   view = view_get_native(heap, error, params[0], TYPE_TEXT_FIELD);
   if (!view) return fixscript_int(0);

   text = text_field_get_text(view);

   ret = create_plat_string(heap, text);
   free(text);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return ret;
}


static Value func_text_field_set_text(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;
   plat_char *text;

   view = view_get_native(heap, error, params[0], TYPE_TEXT_FIELD);
   if (!view) return fixscript_int(0);

   text = get_plat_string(heap, error, params[1]);
   if (!text) return fixscript_int(0);

   text_field_set_text(view, text);
   free(text);
   return fixscript_int(0);
}


static Value func_text_field_is_enabled(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;

   view = view_get_native(heap, error, params[0], TYPE_TEXT_FIELD);
   if (!view) return fixscript_int(0);

   return fixscript_int(text_field_is_enabled(view));
}


static Value func_text_field_set_enabled(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;

   view = view_get_native(heap, error, params[0], TYPE_TEXT_FIELD);
   if (!view) return fixscript_int(0);

   text_field_set_enabled(view, params[1].value);
   return fixscript_int(0);
}


static Value func_text_area_create(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;
   Value instance;

   view = text_area_create();
   if (!view) {
      *error = fixscript_create_error_string(heap, "text area creation failed");
      return fixscript_int(0);
   }

   instance = view_create(heap, view, TEXTAREA_SIZE, TYPE_TEXT_AREA);
   if (!instance.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   return instance;
}


static Value func_text_area_get_text(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;
   plat_char *text;
   Value ret;

   view = view_get_native(heap, error, params[0], TYPE_TEXT_AREA);
   if (!view) return fixscript_int(0);

   text = text_area_get_text(view);

   ret = create_plat_string(heap, text);
   free(text);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return ret;
}


static Value func_text_area_set_text(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   int append_mode = (data == (void *)1);
   View *view;
   plat_char *text;

   view = view_get_native(heap, error, params[0], TYPE_TEXT_AREA);
   if (!view) return fixscript_int(0);

   text = get_plat_string(heap, error, params[1]);
   if (!text) return fixscript_int(0);

   if (append_mode) {
      text_area_append_text(view, text);
   }
   else {
      text_area_set_text(view, text);
   }
   free(text);
   return fixscript_int(0);
}


static Value func_text_area_set_read_only(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;

   view = view_get_native(heap, error, params[0], TYPE_TEXT_AREA);
   if (!view) return fixscript_int(0);

   text_area_set_read_only(view, params[1].value);
   return fixscript_int(0);
}


static Value func_text_area_is_read_only(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;

   view = view_get_native(heap, error, params[0], TYPE_TEXT_AREA);
   if (!view) return fixscript_int(0);

   return fixscript_int(text_area_is_read_only(view));
}


static Value func_text_area_is_enabled(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;

   view = view_get_native(heap, error, params[0], TYPE_TEXT_AREA);
   if (!view) return fixscript_int(0);

   return fixscript_int(text_area_is_enabled(view));
}


static Value func_text_area_set_enabled(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;

   view = view_get_native(heap, error, params[0], TYPE_TEXT_AREA);
   if (!view) return fixscript_int(0);

   text_area_set_enabled(view, params[1].value);
   return fixscript_int(0);
}


static Value func_button_create(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   plat_char *label;
   int flags;
   View *view;
   Value instance;

   label = get_plat_string(heap, error, params[0]);
   if (!label) {
      return fixscript_int(0);
   }

   flags = fixscript_get_int(params[1]);

   view = button_create(label, flags);
   free_plat_string(label);
   if (!view) {
      *error = fixscript_create_error_string(heap, "button creation failed");
      return fixscript_int(0);
   }

   instance = view_create(heap, view, BTN_SIZE, TYPE_BUTTON);
   if (!instance.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   return instance;
}


static Value func_button_get_label(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;
   plat_char *label;
   Value ret;

   view = view_get_native(heap, error, params[0], TYPE_BUTTON);
   if (!view) return fixscript_int(0);

   label = button_get_label(view);

   ret = create_plat_string(heap, label);
   free(label);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return ret;
}


static Value func_button_set_label(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;
   plat_char *label;

   view = view_get_native(heap, error, params[0], TYPE_BUTTON);
   if (!view) return fixscript_int(0);

   label = get_plat_string(heap, error, params[1]);
   if (!label) return fixscript_int(0);

   button_set_label(view, label);
   free(label);
   return fixscript_int(0);
}


static Value func_button_is_enabled(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;

   view = view_get_native(heap, error, params[0], TYPE_BUTTON);
   if (!view) return fixscript_int(0);

   return fixscript_int(button_is_enabled(view));
}


static Value func_button_set_enabled(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;

   view = view_get_native(heap, error, params[0], TYPE_BUTTON);
   if (!view) return fixscript_int(0);

   button_set_enabled(view, params[1].value);
   return fixscript_int(0);
}


static Value func_table_create(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;
   Value instance;

   view = table_create();
   if (!view) {
      *error = fixscript_create_error_string(heap, "table creation failed");
      return fixscript_int(0);
   }

   instance = view_create(heap, view, TABLE_SIZE, TYPE_TABLE);
   if (!instance.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   return instance;
}


static Value func_table_set_columns(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;
   Value value;
   plat_char **titles = NULL;
   int i, err, num_columns = 0;

   view = view_get_native(heap, error, params[0], TYPE_TABLE);
   if (!view) return fixscript_int(0);

   err = fixscript_get_array_length(heap, params[1], &num_columns);
   if (!err) {
      titles = calloc(num_columns, sizeof(plat_char *));
      if (!titles) {
         err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
   }
   if (!err) {
      for (i=0; i<num_columns; i++) {
         err = fixscript_get_array_elem(heap, params[1], i, &value);
         if (err) break;
         titles[i] = get_plat_string(heap, error, value);
         if (!titles[i]) {
            goto error;
         }
      }
   }
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   table_set_columns(view, num_columns, titles);

error:
   for (i=0; i<num_columns; i++) {
      free_plat_string(titles[i]);
   }
   free(titles);
   return fixscript_int(0);
}


static Value func_table_get_column_width(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;

   view = view_get_native(heap, error, params[0], TYPE_TABLE);
   if (!view) return fixscript_int(0);

   return fixscript_int(table_get_column_width(view, params[1].value));
}


static Value func_table_set_column_width(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;

   view = view_get_native(heap, error, params[0], TYPE_TABLE);
   if (!view) return fixscript_int(0);

   table_set_column_width(view, params[1].value, params[2].value);
   return fixscript_int(0);
}


static Value func_table_clear(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;

   view = view_get_native(heap, error, params[0], TYPE_TABLE);
   if (!view) return fixscript_int(0);

   table_clear(view);
   return fixscript_int(0);
}


static Value func_table_insert_row(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;
   Value value;
   plat_char **titles = NULL;
   int i, err, num_columns = 0;

   view = view_get_native(heap, error, params[0], TYPE_TABLE);
   if (!view) return fixscript_int(0);

   err = fixscript_get_array_length(heap, params[2], &num_columns);
   if (!err) {
      titles = calloc(num_columns, sizeof(plat_char *));
      if (!titles) {
         err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
   }
   if (!err) {
      for (i=0; i<num_columns; i++) {
         err = fixscript_get_array_elem(heap, params[2], i, &value);
         if (err) break;
         titles[i] = get_plat_string(heap, error, value);
         if (!titles[i]) {
            goto error;
         }
      }
   }
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   table_insert_row(view, params[1].value, num_columns, titles);

error:
   for (i=0; i<num_columns; i++) {
      free_plat_string(titles[i]);
   }
   free(titles);
   return fixscript_int(0);
}


static Value func_table_get_selected_row(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;

   view = view_get_native(heap, error, params[0], TYPE_TABLE);
   if (!view) return fixscript_int(0);

   return fixscript_int(table_get_selected_row(view));
}


static Value func_table_set_selected_row(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;

   view = view_get_native(heap, error, params[0], TYPE_TABLE);
   if (!view) return fixscript_int(0);

   table_set_selected_row(view, params[1].value);
   return fixscript_int(0);
}


static Value func_canvas_create(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;
   Value instance;

   view = canvas_create(fixscript_get_int(params[0]));
   if (!view) {
      *error = fixscript_create_error_string(heap, "canvas creation failed");
      return fixscript_int(0);
   }

   instance = view_create(heap, view, CANVAS_SIZE, TYPE_CANVAS);
   if (!instance.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   return instance;
}


static Value func_canvas_set_scroll_state(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;
   int type;

   view = view_get_native(heap, error, params[0], TYPE_CANVAS);
   if (!view) return fixscript_int(0);

   type = fixscript_get_int(params[1]);
   if (type != SCROLL_HORIZ && type != SCROLL_VERT) {
      *error = fixscript_create_error_string(heap, "invalid scroll type");
      return fixscript_int(0);
   }
   
   canvas_set_scroll_state(view, type, fixscript_get_int(params[2]), fixscript_get_int(params[3]), fixscript_get_int(params[4]), fixscript_get_int(params[5]));
   return fixscript_int(0);
}


static Value func_canvas_set_scroll_position(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;
   int type;

   view = view_get_native(heap, error, params[0], TYPE_CANVAS);
   if (!view) return fixscript_int(0);

   type = fixscript_get_int(params[1]);
   if (type != SCROLL_HORIZ && type != SCROLL_VERT) {
      *error = fixscript_create_error_string(heap, "invalid scroll type");
      return fixscript_int(0);
   }
   
   canvas_set_scroll_position(view, type, fixscript_get_int(params[2]));
   return fixscript_int(0);
}


static Value func_canvas_get_scroll_position(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;
   int type;

   view = view_get_native(heap, error, params[0], TYPE_CANVAS);
   if (!view) return fixscript_int(0);

   type = fixscript_get_int(params[1]);
   if (type != SCROLL_HORIZ && type != SCROLL_VERT) {
      *error = fixscript_create_error_string(heap, "invalid scroll type");
      return fixscript_int(0);
   }
   
   return fixscript_int(canvas_get_scroll_position(view, type));
}


static Value func_canvas_set_active_rendering(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;

   view = view_get_native(heap, error, params[0], TYPE_CANVAS);
   if (!view) return fixscript_int(0);

   canvas_set_active_rendering(view, params[1].value != 0);
   return fixscript_int(0);
}


static Value func_canvas_get_active_rendering(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;

   view = view_get_native(heap, error, params[0], TYPE_CANVAS);
   if (!view) return fixscript_int(0);

   return fixscript_int(canvas_get_active_rendering(view));
}


static Value func_canvas_set_relative_mode(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;

   view = view_get_native(heap, error, params[0], TYPE_CANVAS);
   if (!view) return fixscript_int(0);

   canvas_set_relative_mode(view, params[1].value != 0);
   return fixscript_int(0);
}


static Value func_canvas_get_relative_mode(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;

   view = view_get_native(heap, error, params[0], TYPE_CANVAS);
   if (!view) return fixscript_int(0);

   return fixscript_int(canvas_get_relative_mode(view));
}


static Value func_canvas_set_overdraw_size(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;

   view = view_get_native(heap, error, params[0], TYPE_CANVAS);
   if (!view) return fixscript_int(0);

   if (fixscript_get_int(params[1]) < 0) {
      *error = fixscript_create_error_string(heap, "negative value");
      return fixscript_int(0);
   }

   canvas_set_overdraw_size(view, fixscript_get_int(params[1]));
   return fixscript_int(0);
}


static Value func_canvas_get_overdraw_size(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;

   view = view_get_native(heap, error, params[0], TYPE_CANVAS);
   if (!view) return fixscript_int(0);

   return fixscript_int(canvas_get_overdraw_size(view));
}


static Value func_canvas_set_focusable(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;

   view = view_get_native(heap, error, params[0], TYPE_CANVAS);
   if (!view) return fixscript_int(0);

   canvas_set_focusable(view, fixscript_get_int(params[1]));
   return fixscript_int(0);
}


static Value func_canvas_is_focusable(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;

   view = view_get_native(heap, error, params[0], TYPE_CANVAS);
   if (!view) return fixscript_int(0);

   return fixscript_int(canvas_is_focusable(view));
}


static Value func_canvas_repaint(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;
   Rect rect;

   view = view_get_native(heap, error, params[0], TYPE_CANVAS);
   if (!view) return fixscript_int(0);
   
   if (num_params == 5) {
      rect.x1 = fixscript_get_int(params[1]);
      rect.y1 = fixscript_get_int(params[2]);
      rect.x2 = rect.x1 + fixscript_get_int(params[3]);
      rect.y2 = rect.y1 + fixscript_get_int(params[4]);
      canvas_repaint(view, &rect);
   }
   else {
      canvas_repaint(view, NULL);
   }
   return fixscript_int(0);
}


static void *menu_handler_func(Heap *heap, int op, void *p1, void *p2)
{
   MenuCommon *menu = p1;
   MenuItem *item, *next;

   switch (op) {
      case HANDLE_OP_FREE:
         item = menu->items;
         free_menu(p1);

         while (item) {
            free(item->title);
            next = item->next;
            free(item);
            item = next;
         }
         break;

      case HANDLE_OP_MARK_REFS:
         for (item=menu->items; item; item=item->next) {
            if (item->submenu) {
               fixscript_mark_ref(heap, ((MenuCommon *)item->submenu)->instance);
            }
            if (item->data.is_array) {
               fixscript_mark_ref(heap, item->data);
            }
            if (item->id.is_array) {
               fixscript_mark_ref(heap, item->id);
            }
         }
         break;
   }
   return NULL;
}


static Value func_menu_create(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Menu *menu;
   Value instance, handle_val, itemdata;
   int err;

   menu = menu_create();
   if (!menu) {
      *error = fixscript_create_error_string(heap, "menu creation failed");
      return fixscript_int(0);
   }

   handle_val = fixscript_create_value_handle(heap, HANDLE_TYPE_MENU, menu, menu_handler_func);
   if (!handle_val.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   instance = fixscript_create_array(heap, MENU_SIZE);
   if (!instance.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   itemdata = fixscript_create_array(heap, 0);
   if (!itemdata.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   err = fixscript_set_array_elem(heap, instance, MENU_handle, handle_val);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   ((MenuCommon *)menu)->heap = heap;
   ((MenuCommon *)menu)->instance = instance;
   return instance;
}


static void insert_menuitem(MenuCommon *menu, int idx, MenuItem *item)
{
   MenuItem *mi, **prev = &menu->items;
   int i;
   
   menu->num_items++;

   for (i=0, mi=*prev; mi; i++, mi=mi->next) {
      if (i == idx) {
         item->next = mi;
         *prev = item;
         return;
      }
      prev = &mi->next;
   }

   *prev = item;
}


static Value func_menu_insert_item(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Menu *menu;
   MenuItem *item;
   plat_char *title;
   int idx;
   
   menu = menu_get_native(heap, error, params[0]);
   if (!menu) return fixscript_int(0);

   title = get_plat_string(heap, error, params[2]);
   if (!title) return fixscript_int(0);

   idx = fixscript_get_int(params[1]);
   if (idx < -1 || idx > ((MenuCommon *)menu)->num_items) {
      *error = fixscript_create_error_string(heap, "invalid index");
      return fixscript_int(0);
   }

   item = calloc(1, sizeof(MenuItem));
   item->title = dup_plat_string(title);
   item->action = params[3];
   item->data = params[4];
   item->id = params[5];

   insert_menuitem(((MenuCommon *)menu), idx, item);
   menu_insert_item(menu, idx, item->title, item);

   free_plat_string(title);
   return fixscript_int(0);
}


static Value func_menu_insert_separator(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Menu *menu;
   MenuItem *item;
   int idx;
   
   menu = menu_get_native(heap, error, params[0]);
   if (!menu) return fixscript_int(0);

   idx = fixscript_get_int(params[1]);
   if (idx < -1 || idx > ((MenuCommon *)menu)->num_items) {
      *error = fixscript_create_error_string(heap, "invalid index");
      return fixscript_int(0);
   }

   item = calloc(1, sizeof(MenuItem));
   insert_menuitem(((MenuCommon *)menu), idx, item);
   menu_insert_separator(menu, idx);
   return fixscript_int(0);
}


static Value func_menu_insert_submenu(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Menu *menu, *submenu;
   MenuItem *item;
   plat_char *title;
   int idx;
   
   menu = menu_get_native(heap, error, params[0]);
   if (!menu) return fixscript_int(0);

   submenu = menu_get_native(heap, error, params[3]);
   if (!submenu) return fixscript_int(0);

   if (((MenuCommon *)submenu)->parent) {
      *error = fixscript_create_error_string(heap, "menu is already submenu");
      return fixscript_int(0);
   }

   idx = fixscript_get_int(params[1]);
   if (idx < -1 || idx > ((MenuCommon *)menu)->num_items) {
      *error = fixscript_create_error_string(heap, "invalid index");
      return fixscript_int(0);
   }

   title = get_plat_string(heap, error, params[2]);
   if (!title) return fixscript_int(0);

   item = calloc(1, sizeof(MenuItem));
   item->title = dup_plat_string(title);
   item->submenu = submenu;

   if (menu_insert_submenu(menu, idx, title, submenu)) {
      insert_menuitem(((MenuCommon *)menu), idx, item);
      ((MenuCommon *)submenu)->parent = menu;
   }
   else {
      *error = fixscript_create_error_string(heap, "can't add submenu");
   }

   free_plat_string(title);
   return fixscript_int(0);
}


static Value func_menu_remove_item(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   MenuCommon *menu;
   MenuItem *item, **prev;
   int idx = params[1].value;
   int i;
   
   menu = (MenuCommon *)menu_get_native(heap, error, params[0]);
   if (!menu) return fixscript_int(0);

   if (idx < 0 || idx >= menu->num_items) {
      *error = fixscript_create_error_string(heap, "invalid index");
      return fixscript_int(0);
   }
   
   prev = &menu->items;
   for (i=0, item=*prev; item; item=item->next, i++) {
      if (i == idx) {
         menu_remove_item((Menu *)menu, idx, item);
         *prev = item->next;
         free(item->title);
         if (item->submenu) {
            ((MenuCommon *)item->submenu)->parent = NULL;
         }
         free(item);
         menu->num_items--;
         return fixscript_int(0);
      }
      prev = &item->next;
   }

   return fixscript_int(0);
}


static Value func_menu_get_item_count(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   MenuCommon *menu;
   
   menu = (MenuCommon *)menu_get_native(heap, error, params[0]);
   if (!menu) return fixscript_int(0);

   return fixscript_int(menu->num_items);
}


static Value func_menu_get_item(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   MenuCommon *menu;
   MenuItem *item;
   Value values[MENU_ITEM_SIZE], ret;
   int idx = params[1].value;
   int i, err;
   
   menu = (MenuCommon *)menu_get_native(heap, error, params[0]);
   if (!menu) return fixscript_int(0);

   if (idx < 0 || idx >= menu->num_items) {
      *error = fixscript_create_error_string(heap, "invalid index");
      return fixscript_int(0);
   }

   for (i=0, item=menu->items; item; item=item->next, i++) {
      if (i == idx) {
         memset(values, 0, sizeof(values));
         values[MENU_ITEM_menu] = params[0];
         values[MENU_ITEM_idx] = fixscript_int(idx);
         values[MENU_ITEM_title] = item->title? create_plat_string(heap, item->title) : fixscript_int(0);
         values[MENU_ITEM_submenu] = item->submenu? ((MenuCommon *)item->submenu)->instance : fixscript_int(0);
         values[MENU_ITEM_action] = item->action;
         values[MENU_ITEM_data] = item->data;
         values[MENU_ITEM_id] = item->id;
         
         err = 0;
         if (item->title && !values[MENU_ITEM_title].value) {
            err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
         }
         if (!err) {
            ret = fixscript_create_array(heap, MENU_ITEM_SIZE);
            if (!ret.value) {
               err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
            }
         }
         if (!err) {
            err = fixscript_set_array_range(heap, ret, 0, MENU_ITEM_SIZE, values);
         }
         if (err) {
            return fixscript_error(heap, error, err);
         }
         return ret;
      }
   }

   return fixscript_int(0);
}


static Value func_menu_show(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Menu *menu;
   View *view;
   
   menu = menu_get_native(heap, error, params[0]);
   if (!menu) return fixscript_int(0);

   view = view_get_native(heap, error, params[1], -1);
   if (!view) return fixscript_int(0);

   menu_show(menu, view, fixscript_get_int(params[2]), fixscript_get_int(params[3]));
   return fixscript_int(0);
}


static Value func_show_message(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *window = NULL;
   int type;
   plat_char *title = NULL, *msg = NULL;
   Value ret = fixscript_int(0);

   if (params[0].value) {
      window = view_get_native(heap, error, params[0], TYPE_WINDOW);
      if (!window) {
         return fixscript_int(0);
      }
   }

   type = fixscript_get_int(params[1]);
   
   title = get_plat_string(heap, error, params[2]);
   if (!title) {
      goto error;
   }

   msg = get_plat_string(heap, error, params[3]);
   if (!msg) {
      goto error;
   }

   if (!(type >> 8)) {
      switch (type & 0xFF) {
         case MSG_OK:
            type |= MSG_ICON_INFO;
            break;

         case MSG_OK_CANCEL:     
         case MSG_YES_NO:
         case MSG_YES_NO_CANCEL:
            type |= MSG_ICON_QUESTION;
            break;
      }
   }

   ret = fixscript_int(show_message(window, type, title, msg));

error:
   free_plat_string(title);
   free_plat_string(msg);
   return ret;
}


void call_view_callback(View *view, int type)
{
   Heap *heap = ((ViewCommon *)view)->heap;
   Value instance = ((ViewCommon *)view)->instance;
   Value func, error;
   int idx, err;

   trigger_delayed_gc(heap);

   switch (type) {
      case CALLBACK_WINDOW_DESTROY:  idx = WIN_handle_destroy; break;
      case CALLBACK_WINDOW_CLOSE:    idx = WIN_handle_close; break;
      case CALLBACK_WINDOW_RESIZE:   idx = WIN_handle_resize; break;
      case CALLBACK_WINDOW_ACTIVATE: idx = WIN_handle_activate; break;

      case CALLBACK_CANVAS_DESTROY:  idx = CANVAS_handle_destroy; break;
      case CALLBACK_CANVAS_RESIZE:   idx = CANVAS_handle_resize; break;

      default:
         return;
   }

   err = fixscript_get_array_elem(heap, instance, idx, &func);
   if (err) {
      fixscript_error(heap, &error, err);
      fprintf(stderr, "error while running view callback (type=%d):\n", type);
      fixscript_dump_value(heap, error, 1);
      return;
   }

   if (!func.value) {
      return;
   }

   fixscript_call(heap, func, 1, &error, instance);
   if (error.value) {
      fprintf(stderr, "error while running view callback (type=%d):\n", type);
      fixscript_dump_value(heap, error, 1);
      return;
   }
}


void call_view_callback_with_value(View *view, int type, Value value)
{
   Heap *heap = ((ViewCommon *)view)->heap;
   Value instance = ((ViewCommon *)view)->instance;
   Value func, error;
   int idx, err;

   trigger_delayed_gc(heap);

   switch (type) {
      case CALLBACK_CANVAS_PAINT: idx = CANVAS_handle_paint; break;

      default:
         return;
   }

   err = fixscript_get_array_elem(heap, instance, idx, &func);
   if (err) {
      fixscript_error(heap, &error, err);
      fprintf(stderr, "error while running view callback (type=%d):\n", type);
      fixscript_dump_value(heap, error, 1);
      return;
   }

   if (!func.value) {
      return;
   }

   fixscript_call(heap, func, 2, &error, instance, value);
   if (error.value) {
      fprintf(stderr, "error while running view callback (type=%d):\n", type);
      fixscript_dump_value(heap, error, 1);
      return;
   }
}


void call_action_callback(View *view, int type)
{
   Heap *heap = ((ViewCommon *)view)->heap;
   Value instance = ((ViewCommon *)view)->instance;
   Value error;
   Value values[2];
   int idx, err;

   trigger_delayed_gc(heap);

   switch (type) {
      case CALLBACK_BUTTON_ACTION: idx = BTN_click_action; break;

      default:
         return;
   }

   err = fixscript_get_array_range(heap, instance, idx, 2, values);
   if (err) {
      fixscript_error(heap, &error, err);
      fprintf(stderr, "error while running action callback (type=%d):\n", type);
      fixscript_dump_value(heap, error, 1);
      return;
   }

   if (!values[0].value) {
      return;
   }

   fixscript_call(heap, values[0], 2, &error, values[1], instance);
   if (error.value) {
      fprintf(stderr, "error while running action callback (type=%d):\n", type);
      fixscript_dump_value(heap, error, 1);
      return;
   }
}


int call_table_action_callback(View *view, int type, int column, int row, int x, int y)
{
   Heap *heap = ((ViewCommon *)view)->heap;
   Value instance = ((ViewCommon *)view)->instance;
   Value ret, error;
   Value values[2];
   int idx, err;

   trigger_delayed_gc(heap);

   switch (type) {
      case CALLBACK_TABLE_CLICK_ACTION:       idx = TABLE_click_action; break;
      case CALLBACK_TABLE_RIGHT_CLICK_ACTION: idx = TABLE_right_click_action; break;
      case CALLBACK_TABLE_SPACE_KEY_ACTION:   idx = TABLE_space_key_action; break;
      case CALLBACK_TABLE_SORT_ACTION:        idx = TABLE_sort_action; break;

      default:
         return 0;
   }

   err = fixscript_get_array_range(heap, instance, idx, 2, values);
   if (err) {
      fixscript_error(heap, &error, err);
      fprintf(stderr, "error while running table action callback (type=%d):\n", type);
      fixscript_dump_value(heap, error, 1);
      return 0;
   }

   if (!values[0].value) {
      return 0;
   }

   if (type == CALLBACK_TABLE_RIGHT_CLICK_ACTION) {
      ret = fixscript_call(heap, values[0], 5, &error, values[1], fixscript_int(column), fixscript_int(row), fixscript_int(x), fixscript_int(y));
   }
   else if (type == CALLBACK_TABLE_SPACE_KEY_ACTION) {
      ret = fixscript_call(heap, values[0], 2, &error, values[1], fixscript_int(row));
   }
   else if (type == CALLBACK_TABLE_SORT_ACTION) {
      ret = fixscript_call(heap, values[0], 2, &error, values[1], fixscript_int(column));
   }
   else {
      ret = fixscript_call(heap, values[0], 3, &error, values[1], fixscript_int(column), fixscript_int(row));
   }
   if (error.value) {
      fprintf(stderr, "error while running table action callback (type=%d):\n", type);
      fixscript_dump_value(heap, error, 1);
      return 0;
   }
   return ret.value != 0;
}


void call_menu_callback(Menu *menu_ptr, int idx)
{
   MenuCommon *menu = (MenuCommon *)menu_ptr;
   Heap *heap = menu->heap;
   MenuItem *item;
   Value error;
   int i;

   trigger_delayed_gc(heap);

   for (i=0, item=menu->items; item; item=item->next, i++) {
      if (i == idx) {
         if (item->action.value) {
            fixscript_call(heap, item->action, 2, &error, item->data, item->id);
            if (error.value) {
               fprintf(stderr, "error while running menu callback:\n");
               fixscript_dump_value(heap, error, 1);
               return;
            }
         }
         break;
      }
   }
}


int call_mouse_event_callback(View *view, int type, int x, int y, int button, int mod, int click_count, int touch)
{
   Heap *heap = ((ViewCommon *)view)->heap;
   Value instance = ((ViewCommon *)view)->instance;
   Value func, error, ret, event, values[MOUSE_EVENT_SIZE];
   int err;

   trigger_delayed_gc(heap);

   err = fixscript_get_array_elem(heap, instance, VIEW_handle_mouse_event, &func);
   if (err) {
      fixscript_error(heap, &error, err);
      goto error;
   }

   if (!func.value) {
      return 0;
   }

   event = fixscript_create_array(heap, MOUSE_EVENT_SIZE);
   if (!event.value) {
      fixscript_error(heap, &error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   memset(values, 0, sizeof(values));
   values[EVENT_type] = fixscript_int(type);
   values[EVENT_view] = instance;
   values[MOUSE_EVENT_x] = fixscript_int(x);
   values[MOUSE_EVENT_y] = fixscript_int(y);
   values[MOUSE_EVENT_button] = fixscript_int(button);
   values[MOUSE_EVENT_modifiers] = fixscript_int(mod);
   values[MOUSE_EVENT_click_count] = fixscript_int(click_count);
   values[MOUSE_EVENT_touch] = fixscript_int(touch);

   err = fixscript_set_array_range(heap, event, 0, MOUSE_EVENT_SIZE, values);
   if (err) {
      fixscript_error(heap, &error, err);
      goto error;
   }

   ret = fixscript_call(heap, func, 2, &error, instance, event);
   if (error.value) {
      goto error;
   }
   return ret.value != 0;

error:
   fprintf(stderr, "error while running mouse event callback (type=%d):\n", type);
   fixscript_dump_value(heap, error, 1);
   return 0;
}


int call_mouse_wheel_callback(View *view, int x, int y, int mod, float wheel_x, float wheel_y, int scroll_x, int scroll_y)
{
   Heap *heap = ((ViewCommon *)view)->heap;
   Value instance = ((ViewCommon *)view)->instance;
   Value func, error, ret, event, values[MOUSE_EVENT_SIZE];
   int err;

   trigger_delayed_gc(heap);

   err = fixscript_get_array_elem(heap, instance, VIEW_handle_mouse_event, &func);
   if (err) {
      fixscript_error(heap, &error, err);
      goto error;
   }

   if (!func.value) {
      return 0;
   }

   event = fixscript_create_array(heap, MOUSE_EVENT_SIZE);
   if (!event.value) {
      fixscript_error(heap, &error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   memset(values, 0, sizeof(values));
   values[EVENT_type] = fixscript_int(EVENT_MOUSE_WHEEL);
   values[EVENT_view] = instance;
   values[MOUSE_EVENT_x] = fixscript_int(x);
   values[MOUSE_EVENT_y] = fixscript_int(y);
   values[MOUSE_EVENT_modifiers] = fixscript_int(mod);
   values[MOUSE_EVENT_wheel_x] = fixscript_float(wheel_x);
   values[MOUSE_EVENT_wheel_y] = fixscript_float(wheel_y);
   values[MOUSE_EVENT_scroll_x] = fixscript_int(scroll_x);
   values[MOUSE_EVENT_scroll_y] = fixscript_int(scroll_y);

   err = fixscript_set_array_range(heap, event, 0, MOUSE_EVENT_SIZE, values);
   if (err) {
      fixscript_error(heap, &error, err);
      goto error;
   }

   ret = fixscript_call(heap, func, 2, &error, instance, event);
   if (error.value) {
      goto error;
   }
   return ret.value != 0;

error:
   fprintf(stderr, "error while running mouse event callback (type=%d):\n", EVENT_MOUSE_WHEEL);
   fixscript_dump_value(heap, error, 1);
   return 0;
}


int call_touch_event_callback(View *view, int type, int id, int x, int y, int mouse_emitter, int cancelled, uint32_t time)
{
   Heap *heap = ((ViewCommon *)view)->heap;
   Value instance = ((ViewCommon *)view)->instance;
   Value func, error, ret, event, values[TOUCH_EVENT_SIZE];
   int err;

   trigger_delayed_gc(heap);

   err = fixscript_get_array_elem(heap, instance, VIEW_handle_touch_event, &func);
   if (err) {
      fixscript_error(heap, &error, err);
      goto error;
   }

   if (!func.value) {
      return 0;
   }

   event = fixscript_create_array(heap, TOUCH_EVENT_SIZE);
   if (!event.value) {
      fixscript_error(heap, &error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   memset(values, 0, sizeof(values));
   values[EVENT_type] = fixscript_int(type);
   values[EVENT_view] = instance;
   values[TOUCH_EVENT_id] = fixscript_int(id);
   values[TOUCH_EVENT_x] = fixscript_int(x);
   values[TOUCH_EVENT_y] = fixscript_int(y);
   values[TOUCH_EVENT_mouse_emitter] = fixscript_int(mouse_emitter);
   values[TOUCH_EVENT_cancelled] = fixscript_int(cancelled);
   values[TOUCH_EVENT_time] = fixscript_int(time);

   err = fixscript_set_array_range(heap, event, 0, TOUCH_EVENT_SIZE, values);
   if (err) {
      fixscript_error(heap, &error, err);
      goto error;
   }

   ret = fixscript_call(heap, func, 2, &error, instance, event);
   if (error.value) {
      goto error;
   }
   return ret.value != 0;

error:
   fprintf(stderr, "error while running touch event callback (type=%d):\n", type);
   fixscript_dump_value(heap, error, 1);
   return 0;
}


int call_key_event_callback(View *view, int type, int key, int mod)
{
   Heap *heap = ((ViewCommon *)view)->heap;
   Value instance = ((ViewCommon *)view)->instance;
   Value func, error, ret, event, values[KEY_EVENT_SIZE];
   int err;

   trigger_delayed_gc(heap);

   err = fixscript_get_array_elem(heap, instance, VIEW_handle_key_event, &func);
   if (err) {
      fixscript_error(heap, &error, err);
      goto error;
   }

   if (!func.value) {
      return 0;
   }

   event = fixscript_create_array(heap, KEY_EVENT_SIZE);
   if (!event.value) {
      fixscript_error(heap, &error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   memset(values, 0, sizeof(values));
   values[EVENT_type] = fixscript_int(type);
   values[EVENT_view] = instance;
   values[KEY_EVENT_key] = fixscript_int(key);
   values[KEY_EVENT_modifiers] = fixscript_int(mod);

   err = fixscript_set_array_range(heap, event, 0, KEY_EVENT_SIZE, values);
   if (err) {
      fixscript_error(heap, &error, err);
      goto error;
   }

   ret = fixscript_call(heap, func, 2, &error, instance, event);
   if (error.value) {
      goto error;
   }
   return ret.value != 0;

error:
   fprintf(stderr, "error while running key event callback (type=%d):\n", type);
   fixscript_dump_value(heap, error, 1);
   return 0;
}


int call_key_typed_event_callback(View *view, const plat_char *chars, int mod)
{
   Heap *heap = ((ViewCommon *)view)->heap;
   Value instance = ((ViewCommon *)view)->instance;
   Value func, error, ret, event, values[KEY_EVENT_SIZE];
   int err;

   trigger_delayed_gc(heap);

   err = fixscript_get_array_elem(heap, instance, VIEW_handle_key_event, &func);
   if (err) {
      fixscript_error(heap, &error, err);
      goto error;
   }

   if (!func.value) {
      return 0;
   }

   event = fixscript_create_array(heap, KEY_EVENT_SIZE);
   if (!event.value) {
      fixscript_error(heap, &error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   memset(values, 0, sizeof(values));
   values[EVENT_type] = fixscript_int(EVENT_KEY_TYPED);
   values[EVENT_view] = instance;
   values[KEY_EVENT_chars] = create_plat_string(heap, chars);
   values[KEY_EVENT_modifiers] = fixscript_int(mod);

   err = fixscript_set_array_range(heap, event, 0, KEY_EVENT_SIZE, values);
   if (err) {
      fixscript_error(heap, &error, err);
      goto error;
   }

   ret = fixscript_call(heap, func, 2, &error, instance, event);
   if (error.value) {
      goto error;
   }
   return ret.value != 0;

error:
   fprintf(stderr, "error while running key typed event callback:\n");
   fixscript_dump_value(heap, error, 1);
   return 0;
}


void call_focus_event_callback(View *view, int type, int subtype)
{
   Heap *heap = ((ViewCommon *)view)->heap;
   Value instance = ((ViewCommon *)view)->instance;
   Value func, error, event, values[FOCUS_EVENT_SIZE];
   int err;

   trigger_delayed_gc(heap);

   err = fixscript_get_array_elem(heap, instance, VIEW_handle_focus_event, &func);
   if (err) {
      fixscript_error(heap, &error, err);
      goto error;
   }

   if (!func.value) {
      return;
   }

   event = fixscript_create_array(heap, FOCUS_EVENT_SIZE);
   if (!event.value) {
      fixscript_error(heap, &error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   memset(values, 0, sizeof(values));
   values[EVENT_type] = fixscript_int(type);
   values[EVENT_view] = instance;
   values[FOCUS_EVENT_subtype] = fixscript_int(subtype);

   err = fixscript_set_array_range(heap, event, 0, FOCUS_EVENT_SIZE, values);
   if (err) {
      fixscript_error(heap, &error, err);
      goto error;
   }

   fixscript_call(heap, func, 2, &error, instance, event);
   if (error.value) {
      goto error;
   }
   return;

error:
   fprintf(stderr, "error while running focus event callback (type=%d):\n", type);
   fixscript_dump_value(heap, error, 1);
   return;
}


#ifndef __EMSCRIPTEN__

static void worker_free(void *data)
{
   WorkerCommon *worker = data;
   
   if (__sync_sub_and_fetch(&worker->refcnt, 1) == 0) {
      fixscript_free_heap(worker->comm_heap);
      free(worker->script_name);
      free(worker->func_name);
      worker_destroy((Worker *)worker);
   }
}


void worker_ref(Worker *worker)
{
   __sync_add_and_fetch(&((WorkerCommon *)worker)->refcnt, 1);
}


void worker_unref(Worker *worker)
{
   worker_free((WorkerCommon *)worker);
}


static void worker_main_func(void *data)
{
   WorkerCommon *worker = data;
   Heap *heap = NULL;
   Script *script;
   Value params, error, *values = NULL, func_val;
   char buf[128];
   int err, num_params;

   script = worker->load.func(&heap, worker->script_name, &error, worker->load.data);
   if (!script) {
      if (heap) {
         fprintf(stderr, "%s\n", fixscript_get_compiler_error(heap, error));
      }
      else {
         fprintf(stderr, "failed to create heap for worker\n");
      }
      fflush(stderr);
      goto error;
   }

   err = fixscript_clone_between(heap, worker->comm_heap, worker->params, &params, fixscript_resolve_existing, NULL, &error);
   if (err) {
      if (!error.value) {
         fixscript_error(heap, &error, err);
      }
      fixscript_dump_value(heap, error, 1);
      goto error;
   }

   fixscript_unref(worker->comm_heap, worker->params);

   err = fixscript_get_array_length(heap, params, &num_params);
   if (err) {
      fixscript_error(heap, &error, err);
      fixscript_dump_value(heap, error, 1);
      goto error;
   }

   values = malloc(num_params * sizeof(Value));
   if (!values) goto error;

   err = fixscript_get_array_range(heap, params, 0, num_params, values);
   if (err) {
      fixscript_error(heap, &error, err);
      fixscript_dump_value(heap, error, 1);
      goto error;
   }

   func_val = fixscript_get_function(heap, script, worker->func_name);
   if (!func_val.value) {
      snprintf(buf, sizeof(buf), "can't find %s in %s", worker->func_name, worker->script_name);
      fixscript_dump_value(heap, fixscript_create_error_string(heap, buf), 1);
      goto error;
   }

#if defined(__APPLE__) || defined(__HAIKU__) || defined(__SYMBIAN32__)
   pthread_setspecific(cur_thread_worker_key, worker);
#else
   cur_thread_worker = worker;
#endif

   fixscript_call_args(heap, func_val, num_params, &error, values);
   if (error.value) {
      fixscript_dump_value(heap, error, 1);
   }

#if defined(__APPLE__) || defined(__HAIKU__) || defined(__SYMBIAN32__)
   pthread_setspecific(cur_thread_worker_key, NULL);
#else
   cur_thread_worker = NULL;
#endif

error:
   free(values);

   worker_lock((Worker *)worker);
   worker->finished = 1;
   worker_unlock((Worker *)worker);

   __sync_add_and_fetch(&worker->refcnt, 1);
   worker_notify((Worker *)worker);

   worker_free(worker);
   if (heap) {
      fixscript_free_heap(heap);
   }
}


static void worker_notify_func(void *data)
{
   Heap *heap;
   WorkerCommon *worker = data;
   Value msg, error;
   int err, len, collect_cnt = 10, received_msg = 0;

   worker_lock((Worker *)worker);
   heap = worker->main_heap;

   for (;;) {
      err = fixscript_get_array_length(worker->comm_heap, worker->comm_output, &len);
      if (err) {
         fixscript_error(heap, &error, err);
         fixscript_dump_value(heap, error, 1);
         break;
      }
      if (len == 0) break;
      
      err = fixscript_get_array_elem(worker->comm_heap, worker->comm_output, 0, &msg);
      if (!err) {
         err = fixscript_copy_array(worker->comm_heap, worker->comm_output, 0, worker->comm_output, 1, len-1);
      }
      if (!err) {
         err = fixscript_set_array_length(worker->comm_heap, worker->comm_output, len-1);
      }
      if (!err) {
         err = fixscript_clone_between(heap, worker->comm_heap, msg, &msg, fixscript_resolve_existing, NULL, &error);
      }
      if (err) {
         if (!error.value) {
            fixscript_error(heap, &error, err);
         }
         fixscript_dump_value(heap, error, 1);
         continue;
      }

      if (--collect_cnt <= 0) {
         fixscript_collect_heap(worker->comm_heap);
         collect_cnt = 10;
      }

      worker_unlock((Worker *)worker);

      fixscript_call(heap, worker->callback_func, 2, &error, worker->callback_data, msg);
      if (error.value) {
         fixscript_dump_value(heap, error, 1);
      }

      worker_lock((Worker *)worker);
      received_msg = 1;
   }

   if (received_msg) {
      fixscript_collect_heap(worker->comm_heap);
   }
   
   if (worker->finished) {
      fixscript_unref(worker->main_heap, worker->handle);
      fixscript_unref(worker->main_heap, worker->callback_data);
      worker->handle = fixscript_int(0);
      worker->callback_data = fixscript_int(0);
   }
   worker_unlock((Worker *)worker);

   worker_free(worker);
}


static Value func_worker_create(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   WorkerLoad *wl = data;
   WorkerCommon *worker = NULL;
   int err;

   if (!wl->func) {
      *error = fixscript_create_error_string(heap, "worker load function not set");
      return fixscript_int(0);
   }

   worker = (WorkerCommon *)worker_create();
   if (!worker) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   worker->refcnt = 1;
   worker->load = *wl;
   worker->main_func = worker_main_func;
   worker->notify_func = worker_notify_func;
   worker->handle = fixscript_create_handle(heap, HANDLE_TYPE_WORKER, worker, worker_free);
   if (!worker->handle.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   worker->comm_heap = fixscript_create_heap();
   if (!worker->comm_heap) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   worker->comm_input = fixscript_create_array(worker->comm_heap, 0);
   worker->comm_output = fixscript_create_array(worker->comm_heap, 0);
   if (!worker->comm_input.value || !worker->comm_output.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   fixscript_ref(worker->comm_heap, worker->comm_input);
   fixscript_ref(worker->comm_heap, worker->comm_output);

   err = fixscript_get_string(heap, params[0], 0, -1, &worker->script_name, NULL);
   if (!err) {
      err = fixscript_get_string(heap, params[1], 0, -1, &worker->func_name, NULL);
   }
   if (!err) {
      err = fixscript_clone_between(worker->comm_heap, heap, params[2], &worker->params, NULL, NULL, NULL);
   }
   if (err) {
      return fixscript_error(heap, error, err);
   }

   fixscript_ref(heap, worker->params);

   worker->callback_func = params[3];
   worker->callback_data = params[4];
   fixscript_ref(heap, worker->callback_data);

   worker->refcnt++;
   if (!worker_start((Worker *)worker)) {
      worker->refcnt--;
      fixscript_unref(heap, worker->params);
      fixscript_unref(heap, worker->callback_data);
      *error = fixscript_create_error_string(heap, "can't start worker");
      return fixscript_int(0);
   }

   worker->main_heap = heap;
   fixscript_ref(heap, worker->handle);
   return worker->handle;
}


static Value func_worker_send(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   int inside = (num_params == 1);
   WorkerCommon *worker;
   Value arr, msg;
   int err = 0, len;
   
   if (inside) {
#if defined(__APPLE__) || defined(__HAIKU__) || defined(__SYMBIAN32__)
      worker = pthread_getspecific(cur_thread_worker_key);
#else
      worker = cur_thread_worker;
#endif
      if (!worker) {
         *error = fixscript_create_error_string(heap, "called outside of worker thread");
         return fixscript_int(0);
      }
   }
   else {
      worker = fixscript_get_handle(heap, params[0], HANDLE_TYPE_WORKER, NULL);
      if (!worker) {
         *error = fixscript_create_error_string(heap, "invalid worker handle");
         return fixscript_int(0);
      }
   }

   worker_lock((Worker *)worker);

   arr = inside? worker->comm_output : worker->comm_input;

   for (;;) {
      err = fixscript_get_array_length(worker->comm_heap, arr, &len);
      if (err) break;
      if (len < MAX_MESSAGES) break;
      worker_wait((Worker *)worker, -1);
   }
   
   if (!err) {
      err = fixscript_clone_between(worker->comm_heap, heap, inside? params[0] : params[1], &msg, NULL, NULL, NULL);
   }
   if (!err) {
      err = fixscript_append_array_elem(worker->comm_heap, arr, msg);
   }

   worker_unlock((Worker *)worker);

   if (inside) {
      __sync_add_and_fetch(&worker->refcnt, 1);
      worker_notify((Worker *)worker);
   }

   if (err) {
      return fixscript_error(heap, error, err);
   }
   return fixscript_int(0);
}


static Value func_worker_receive(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   WorkerCommon *worker;
   Value msg = fixscript_int(0);
   uint32_t timer_end = 0;
   int timeout = -1;
   int err = 0, len;

#if defined(__APPLE__) || defined(__HAIKU__) || defined(__SYMBIAN32__)
   worker = pthread_getspecific(cur_thread_worker_key);
#else
   worker = cur_thread_worker;
#endif

   if (!worker) {
      *error = fixscript_create_error_string(heap, "called outside of worker thread");
      return fixscript_int(0);
   }

   if (num_params == 1) {
      timeout = fixscript_get_int(params[0]);
      if (timeout < 0) timeout = -1;
   }

   if (timeout > 0) {
      timer_end = timer_get_time() + (uint32_t)timeout;
   }

   worker_lock((Worker *)worker);

   for (;;) {
      err = fixscript_get_array_length(worker->comm_heap, worker->comm_input, &len);
      if (err) break;
      if (len > 0) break;
      if (timeout > 0) {
         timeout = timer_end - timer_get_time();
         if (timeout < 0) timeout = 0;
      }
      if (timeout == 0) break;
      worker_wait((Worker *)worker, timeout);
   }

   if (len > 0) {
      if (!err) {
         err = fixscript_get_array_elem(worker->comm_heap, worker->comm_input, 0, &msg);
      }
      if (!err) {
         err = fixscript_copy_array(worker->comm_heap, worker->comm_input, 0, worker->comm_input, 1, len-1);
      }
      if (!err) {
         err = fixscript_set_array_length(worker->comm_heap, worker->comm_input, len-1);
      }
      if (!err) {
         err = fixscript_clone_between(heap, worker->comm_heap, msg, &msg, fixscript_resolve_existing, NULL, error);
      }
   }

   worker_unlock((Worker *)worker);

   if (err) {
      if (!error->value) {
         fixscript_error(heap, error, err);
      }
      return fixscript_int(0);
   }
   return msg;
}


#if defined(__APPLE__) || defined(__HAIKU__) || defined(__SYMBIAN32__)
void fixgui__tls_init()
{
   pthread_key_create(&cur_thread_worker_key, NULL);
}
#endif

#endif /* __EMSCRIPTEN__ */


static Value func_timer_get_time(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   return fixscript_int(timer_get_time());
}


static Value func_timer_get_micro_time(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   return fixscript_int(timer_get_micro_time());
}


static Value func_timer_is_active(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value interval;
   int err;
   
   err = fixscript_get_array_elem(heap, params[0], TIMER_interval, &interval);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   return fixscript_int(timer_is_active(heap, params[0]));
}


static Value func_timer_start(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value interval;
   int err;
   
   err = fixscript_get_array_elem(heap, params[0], TIMER_interval, &interval);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   if (interval.value < 0) {
      *error = fixscript_create_error_string(heap, "negative interval");
      return fixscript_int(0);
   }

   timer_start(heap, params[0], interval.value, (intptr_t)data);
   return fixscript_int(0);
}


static Value func_timer_stop(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   timer_stop(heap, params[0]);
   return fixscript_int(0);
}


static Value func_clipboard_set_text(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   plat_char *text;

   text = get_plat_string(heap, error, params[0]);
   if (!text) return fixscript_int(0);

   clipboard_set_text(text);
   free(text);
   return fixscript_int(0);
}


static Value func_clipboard_get_text(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   plat_char *text;
   Value ret;

   text = clipboard_get_text();
   if (!text) return fixscript_int(0);

   ret = create_plat_string(heap, text);
   free(text);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return ret;
}


void timer_run(Heap *heap, Value instance)
{
   Value func, error;
   int err;

   trigger_delayed_gc(heap);

   err = fixscript_get_array_elem(heap, instance, TIMER_run, &func);
   if (err) {
      fixscript_error(heap, &error, err);
      goto error;
   }

   fixscript_call(heap, func, 1, &error, instance);
   if (error.value) {
      goto error;
   }
   return;

error:
   fprintf(stderr, "error while running timer event callback:\n");
   fixscript_dump_value(heap, error, 1);
   timer_stop(heap, instance);
}


static void *system_font_handler_func(Heap *heap, int op, void *p1, void *p2)
{
   SystemFont *font = p1;

   switch (op) {
      case HANDLE_OP_FREE:
         system_font_destroy(font);
         break;

      case HANDLE_OP_MARK_REFS:
         #ifdef FIXGUI_VIRTUAL
            virtual_system_font_mark_refs(font);
         #endif
         break;
   }
   return NULL;
}


Value system_font_create_handle(Heap *heap, Value *error, SystemFont *font)
{
   Value handle_val;

   handle_val = fixscript_create_value_handle(heap, HANDLE_TYPE_FONT, font, system_font_handler_func);
   if (!handle_val.value) {
      system_font_destroy(font);
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return handle_val;
}


static Value func_system_font_create(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   plat_char *family;
   SystemFont *font;

   family = get_plat_string(heap, error, params[0]);
   if (!family) {
      return fixscript_int(0);
   }

   font = system_font_create(heap, family, fixscript_get_float(params[1]), fixscript_get_int(params[2]));
   free_plat_string(family);
   if (!font) {
      *error = fixscript_create_error_string(heap, "font creation failed");
      return fixscript_int(0);
   }
   return system_font_create_handle(heap, error, font);
}


static Value func_system_font_get_list(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value arr, str, ret = fixscript_int(0);
   plat_char **list, **p;
   int err;

   list = system_font_get_list();
   if (!list) {
      *error = fixscript_create_error_string(heap, "error while retrieving font list");
      return fixscript_int(0);
   }
   
   arr = fixscript_create_array(heap, 0);
   if (!arr.value) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   for (p=list; *p; p++) {
      str = create_plat_string(heap, *p);
      if (!str.value) {
         fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         goto error;
      }
      err = fixscript_append_array_elem(heap, arr, str);
      if (err) {
         fixscript_error(heap, error, err);
         goto error;
      }
   }

   ret = arr;

error:
   for (p=list; *p; p++) {
      free(*p);
   }
   free(list);
   return ret;
}


static Value func_system_font_get_metrics(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   SystemFont *font;
   int type = (int)(intptr_t)data;
   int result = 0;

   font = fixscript_get_handle(heap, params[0], HANDLE_TYPE_FONT, NULL);
   if (!font) {
      *error = fixscript_create_error_string(heap, "invalid system font handle");
      return fixscript_int(0);
   }

   switch (type) {
      case SFM_SIZE:    result = system_font_get_size(font); break;
      case SFM_ASCENT:  result = system_font_get_ascent(font); break;
      case SFM_DESCENT: result = system_font_get_descent(font); break;
      case SFM_HEIGHT:  result = system_font_get_height(font); break;
   }

   return fixscript_int(result);
}


static Value func_system_font_get_string_advance(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   SystemFont *font;
#ifndef FIXGUI_VIRTUAL
   plat_char *s;
#endif
   int off = 0, len = -1;
   int result;

   if (num_params == 4) {
      off = params[2].value;
      len = params[3].value;
   }

   font = fixscript_get_handle(heap, params[0], HANDLE_TYPE_FONT, NULL);
   if (!font) {
      *error = fixscript_create_error_string(heap, "invalid system font handle");
      return fixscript_int(0);
   }

#ifdef FIXGUI_VIRTUAL
   if (num_params < 4) {
      off = 0;
      fixscript_get_array_length(heap, params[1], &len);
   }
   result = system_font_get_string_advance(font, params[1], off, len);
#else
   s = get_plat_string_range(heap, error, params[1], off, len);
   if (!s) {
      return fixscript_int(0);
   }

   result = system_font_get_string_advance(font, s);

   free_plat_string(s);
#endif
   return fixscript_int(result);
}


static Value func_system_font_get_string_position(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   SystemFont *font;
#ifndef FIXGUI_VIRTUAL
   plat_char *s;
#endif
   int off = 0, len = -1, x;
   float result;

   if (num_params == 5) {
      off = params[2].value;
      len = params[3].value;
      x = params[4].value;
   }
   else {
      x = params[2].value;
   }

   font = fixscript_get_handle(heap, params[0], HANDLE_TYPE_FONT, NULL);
   if (!font) {
      *error = fixscript_create_error_string(heap, "invalid system font handle");
      return fixscript_int(0);
   }

#ifdef FIXGUI_VIRTUAL
   if (num_params < 5) {
      off = 0;
      fixscript_get_array_length(heap, params[1], &len);
   }
   result = system_font_get_string_position(font, params[1], off, len, x);
#else
   s = get_plat_string_range(heap, error, params[1], off, len);
   if (!s) {
      return fixscript_int(0);
   }

   result = system_font_get_string_position(font, s, x);

   free_plat_string(s);
#endif
   return fixscript_float(result);
}


static Value func_system_font_draw_string(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   SystemFont *font;
   Value img;
   float tr[6];
   int clip[4];
   int width, height, stride;
   uint32_t *pixels;
   int off = 0, len = -1;
   uint32_t color;
   int x, y;
   plat_char *text;

   if (num_params == 8) {
      off = params[5].value;
      len = params[6].value;
      color = params[7].value;
   }
   else {
      color = params[5].value;
   }

   font = fixscript_get_handle(heap, params[0], HANDLE_TYPE_FONT, NULL);
   if (!font) {
      *error = fixscript_create_error_string(heap, "invalid system font handle");
      return fixscript_int(0);
   }
   
#ifdef FIXGUI_VIRTUAL
   if (num_params < 8) {
      off = 0;
      fixscript_get_array_length(heap, params[4], &len);
   }
   if (system_font_draw_string_custom(font, params[1], params[2].value, params[3].value, params[4], off, len, color)) {
      return fixscript_int(0);
   }
#endif

   if (!fiximage_get_painter_data(heap, params[1], tr, clip, &img)) {
      *error = fixscript_create_error_string(heap, "invalid painter");
      return fixscript_int(0);
   }

   if (clip[2] - clip[0] <= 0 || clip[3] - clip[1] <= 0) {
      return fixscript_int(0);
   }

   if (!fiximage_get_data(heap, img, &width, &height, &stride, &pixels, NULL, NULL)) {
      *error = fixscript_create_error_string(heap, "invalid image");
      return fixscript_int(0);
   }

   x = roundf(fixscript_get_int(params[2]) * tr[0] + fixscript_get_int(params[3]) * tr[1] + tr[2]) - clip[0];
   y = roundf(fixscript_get_int(params[2]) * tr[3] + fixscript_get_int(params[3]) * tr[4] + tr[5]) - clip[1];

   pixels += clip[0] + clip[1] * stride;
   width = clip[2] - clip[0];
   height = clip[3] - clip[1];

   text = get_plat_string_range(heap, error, params[4], off, len);
   if (!text) {
      return fixscript_int(0);
   }
   
   system_font_draw_string(font, x, y, text, color, pixels, width, height, stride);
   free_plat_string(text);

   return fixscript_int(0);
}


static void *notify_icon_handler_func(Heap *heap, int op, void *p1, void *p2)
{
   NotifyIconCommon *icon = p1;

   switch (op) {
      case HANDLE_OP_FREE: {
         free_notify_icon(p1);
         break;
      }

      case HANDLE_OP_MARK_REFS:
         if (icon->menu.value) {
            fixscript_mark_ref(heap, icon->menu);
         }
         break;
   }
   return NULL;
}


Value notify_icon_create_handle(Heap *heap, Value *error, NotifyIcon *icon)
{
   Value instance, handle_val;
   int err;

   instance = fixscript_create_array(heap, NOTIFYICON_SIZE);
   if (!instance.value) {
      notify_icon_destroy(icon);
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   handle_val = fixscript_create_value_handle(heap, HANDLE_TYPE_NOTIFYICON, icon, notify_icon_handler_func);
   if (!handle_val.value) {
      notify_icon_destroy(icon);
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   err = fixscript_set_array_elem(heap, instance, NOTIFYICON_handle, handle_val);
   if (err) {
      notify_icon_destroy(icon);
      return fixscript_error(heap, error, err);
   }

   ((NotifyIconCommon *)icon)->heap = heap;
   ((NotifyIconCommon *)icon)->instance = instance;
   fixscript_ref(heap, instance);
   num_active_windows++;
   return instance;
}


static Value func_notify_icon_create(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value *images;
   NotifyIcon *icon;
   char *error_msg;
   int err, num_images;

   err = fixscript_get_array_length(heap, params[0], &num_images);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   images = calloc(num_images, sizeof(Value));
   if (!images) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   err = fixscript_get_array_range(heap, params[0], 0, num_images, images);
   if (err) {
      free(images);
      return fixscript_error(heap, error, err);
   }

   error_msg = NULL;
   icon = notify_icon_create(heap, images, num_images, &error_msg);
   free(images);
   if (!icon) {
      *error = fixscript_create_error_string(heap, error_msg? error_msg : "creation failed");
      free(error_msg);
      return fixscript_int(0);
   }

   return notify_icon_create_handle(heap, error, icon);
}


static Value func_notify_icon_get_sizes(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value arr;
   int *sizes = NULL;
   int i, cnt = 0;

   notify_icon_get_sizes(&sizes, &cnt);
   arr = fixscript_create_array(heap, cnt);
   for (i=0; i<cnt; i++) {
      fixscript_set_array_elem(heap, arr, i, fixscript_int(sizes[i]));
   }
   free(sizes);
   return arr;
}


static Value func_notify_icon_destroy(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   NotifyIcon *icon;

   icon = notify_icon_get_native(heap, error, params[0]);
   if (!icon) return fixscript_int(0);

   notify_icon_destroy(icon);
   fixscript_unref(heap, ((NotifyIconCommon *)icon)->instance);
   if (--num_active_windows == 0) {
      quit_app();
   }
   return fixscript_int(0);
}


void call_notify_icon_click_callback(NotifyIcon *icon)
{
   Heap *heap = ((NotifyIconCommon *)icon)->heap;
   Value instance = ((NotifyIconCommon *)icon)->instance;
   Value callback, error;
   int err;

   trigger_delayed_gc(heap);

   err = fixscript_get_array_elem(heap, instance, NOTIFYICON_handle_click_action, &callback);
   if (err) {
      fixscript_error(heap, &error, err);
      fprintf(stderr, "error while running notify icon click callback:\n");
      fixscript_dump_value(heap, error, 1);
      return;
   }

   if (callback.value) {
      fixscript_call(heap, callback, 1, &error, instance);
      if (error.value) {
         fprintf(stderr, "error while running notify icon click callback:\n");
         fixscript_dump_value(heap, error, 1);
         return;
      }
   }
}


static Value func_notify_icon_set_menu(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   NotifyIcon *icon;
   Menu *menu = NULL;

   icon = notify_icon_get_native(heap, error, params[0]);
   if (!icon) return fixscript_int(0);

   if (params[1].value) {
      menu = menu_get_native(heap, error, params[1]);
      if (!menu) return fixscript_int(0);
   }

   if (notify_icon_set_menu(icon, menu)) {
      ((NotifyIconCommon *)icon)->menu = params[1];
   }
   else {
      *error = fixscript_create_error_string(heap, "can't set menu");
   }
   return fixscript_int(0);
}


static Value func_notify_icon_get_menu(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   NotifyIconCommon *icon;

   icon = (NotifyIconCommon *)notify_icon_get_native(heap, error, params[0]);
   if (!icon) return fixscript_int(0);

   return icon->menu;
}


static Value func_modifiers_cmd_mask(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   return fixscript_int(modifiers_cmd_mask());
}


void fixgui_register_functions(Heap *heap, WorkerLoadFunc load_func, void *load_data)
{
#ifndef __EMSCRIPTEN__
   WorkerLoad *wl;
#endif

   if (gui_heap) {
      fprintf(stderr, "error: only single heap can be initialized for GUI\n");
      fflush(stderr);
      #ifdef __wasm__
         return;
      #else
         exit(1);
      #endif
   }
   gui_heap = heap;

#ifndef __EMSCRIPTEN__
   wl = malloc(sizeof(WorkerLoad));
   wl->func = load_func;
   wl->data = load_data;
   fixscript_register_cleanup(heap, free, wl);
#endif
   
   fixscript_register_handle_types(&handles_offset, NUM_HANDLE_TYPES);

   fixscript_register_native_func(heap, "view_destroy#1", func_view_destroy, NULL);
   fixscript_register_native_func(heap, "view_get_rect#1", func_view_get_rect, NULL);
   fixscript_register_native_func(heap, "view_set_rect#5", func_view_set_rect, NULL);
   fixscript_register_native_func(heap, "view_set_rect#2", func_view_set_rect, NULL);
   fixscript_register_native_func(heap, "view_get_content_rect#1", func_view_get_content_rect, NULL);
   fixscript_register_native_func(heap, "view_get_inner_rect#1", func_view_get_inner_rect, NULL);
   fixscript_register_native_func(heap, "view_set_visible#2", func_view_set_visible, NULL);
   fixscript_register_native_func(heap, "view_add#2", func_view_add, NULL);
   fixscript_register_native_func(heap, "view_get_parent#1", func_view_get_parent, NULL);
   fixscript_register_native_func(heap, "view_get_next#1", func_view_get_next, NULL);
   fixscript_register_native_func(heap, "view_get_prev#1", func_view_get_prev, NULL);
   fixscript_register_native_func(heap, "view_get_first_child#1", func_view_get_first_child, NULL);
   fixscript_register_native_func(heap, "view_get_last_child#1", func_view_get_last_child, NULL);
   fixscript_register_native_func(heap, "view_get_child_count#1", func_view_get_child_count, NULL);
   fixscript_register_native_func(heap, "view_get_child#2", func_view_get_child, NULL);
   fixscript_register_native_func(heap, "view_focus#1", func_view_focus, NULL);
   fixscript_register_native_func(heap, "view_has_focus#1", func_view_has_focus, NULL);
   fixscript_register_native_func(heap, "view_get_sizing#1", func_view_get_sizing, NULL);
   fixscript_register_native_func(heap, "view_get_default_size#1", func_view_get_default_size, NULL);
   fixscript_register_native_func(heap, "view_get_scale#1", func_view_get_scale, NULL);
   fixscript_register_native_func(heap, "view_set_cursor#2", func_view_set_cursor, NULL);
   fixscript_register_native_func(heap, "view_get_cursor#1", func_view_get_cursor, NULL);
   fixscript_register_native_func(heap, "window_create#4", func_window_create, NULL);
   fixscript_register_native_func(heap, "window_get_title#1", func_window_get_title, NULL);
   fixscript_register_native_func(heap, "window_set_title#2", func_window_set_title, NULL);
   fixscript_register_native_func(heap, "window_set_minimum_size#3", func_window_set_minimum_size, NULL);
   fixscript_register_native_func(heap, "window_is_maximized#1", func_window_is_maximized, NULL);
   fixscript_register_native_func(heap, "window_set_status_text#2", func_window_set_status_text, NULL);
   fixscript_register_native_func(heap, "window_set_menu#2", func_window_set_menu, NULL);
   fixscript_register_native_func(heap, "window_get_menu#1", func_window_get_menu, NULL);
   fixscript_register_native_func(heap, "label_create#1", func_label_create, NULL);
   fixscript_register_native_func(heap, "label_get_label#1", func_label_get_label, NULL);
   fixscript_register_native_func(heap, "label_set_label#2", func_label_set_label, NULL);
   fixscript_register_native_func(heap, "text_field_create#0", func_text_field_create, NULL);
   fixscript_register_native_func(heap, "text_field_get_text#1", func_text_field_get_text, NULL);
   fixscript_register_native_func(heap, "text_field_set_text#2", func_text_field_set_text, NULL);
   fixscript_register_native_func(heap, "text_field_is_enabled#1", func_text_field_is_enabled, NULL);
   fixscript_register_native_func(heap, "text_field_set_enabled#2", func_text_field_set_enabled, NULL);
   fixscript_register_native_func(heap, "text_area_create#0", func_text_area_create, NULL);
   fixscript_register_native_func(heap, "text_area_get_text#1", func_text_area_get_text, NULL);
   fixscript_register_native_func(heap, "text_area_set_text#2", func_text_area_set_text, (void *)0);
   fixscript_register_native_func(heap, "text_area_append_text#2", func_text_area_set_text, (void *)1);
   fixscript_register_native_func(heap, "text_area_set_read_only#2", func_text_area_set_read_only, NULL);
   fixscript_register_native_func(heap, "text_area_is_read_only#1", func_text_area_is_read_only, NULL);
   fixscript_register_native_func(heap, "text_area_is_enabled#1", func_text_area_is_enabled, NULL);
   fixscript_register_native_func(heap, "text_area_set_enabled#2", func_text_area_set_enabled, NULL);
   fixscript_register_native_func(heap, "button_create#2", func_button_create, NULL);
   fixscript_register_native_func(heap, "button_get_label#1", func_button_get_label, NULL);
   fixscript_register_native_func(heap, "button_set_label#2", func_button_set_label, NULL);
   fixscript_register_native_func(heap, "button_is_enabled#1", func_button_is_enabled, NULL);
   fixscript_register_native_func(heap, "button_set_enabled#2", func_button_set_enabled, NULL);
   fixscript_register_native_func(heap, "table_create#0", func_table_create, NULL);
   fixscript_register_native_func(heap, "table_set_columns#2", func_table_set_columns, NULL);
   fixscript_register_native_func(heap, "table_get_column_width#2", func_table_get_column_width, NULL);
   fixscript_register_native_func(heap, "table_set_column_width#3", func_table_set_column_width, NULL);
   fixscript_register_native_func(heap, "table_clear#1", func_table_clear, NULL);
   fixscript_register_native_func(heap, "table_insert_row#3", func_table_insert_row, NULL);
   fixscript_register_native_func(heap, "table_get_selected_row#1", func_table_get_selected_row, NULL);
   fixscript_register_native_func(heap, "table_set_selected_row#2", func_table_set_selected_row, NULL);
   fixscript_register_native_func(heap, "canvas_create#1", func_canvas_create, NULL);
   fixscript_register_native_func(heap, "canvas_set_scroll_state#6", func_canvas_set_scroll_state, NULL);
   fixscript_register_native_func(heap, "canvas_set_scroll_position#3", func_canvas_set_scroll_position, NULL);
   fixscript_register_native_func(heap, "canvas_get_scroll_position#2", func_canvas_get_scroll_position, NULL);
   fixscript_register_native_func(heap, "canvas_set_active_rendering#2", func_canvas_set_active_rendering, NULL);
   fixscript_register_native_func(heap, "canvas_get_active_rendering#1", func_canvas_get_active_rendering, NULL);
   fixscript_register_native_func(heap, "canvas_set_relative_mode#2", func_canvas_set_relative_mode, NULL);
   fixscript_register_native_func(heap, "canvas_get_relative_mode#1", func_canvas_get_relative_mode, NULL);
   fixscript_register_native_func(heap, "canvas_set_overdraw_size#2", func_canvas_set_overdraw_size, NULL);
   fixscript_register_native_func(heap, "canvas_get_overdraw_size#1", func_canvas_get_overdraw_size, NULL);
   fixscript_register_native_func(heap, "canvas_set_focusable#2", func_canvas_set_focusable, NULL);
   fixscript_register_native_func(heap, "canvas_is_focusable#1", func_canvas_is_focusable, NULL);
   fixscript_register_native_func(heap, "canvas_repaint#1", func_canvas_repaint, NULL);
   fixscript_register_native_func(heap, "canvas_repaint#5", func_canvas_repaint, NULL);
   fixscript_register_native_func(heap, "menu_create#0", func_menu_create, NULL);
   fixscript_register_native_func(heap, "menu_insert_item#6", func_menu_insert_item, NULL);
   fixscript_register_native_func(heap, "menu_insert_separator#2", func_menu_insert_separator, NULL);
   fixscript_register_native_func(heap, "menu_insert_submenu#4", func_menu_insert_submenu, NULL);
   fixscript_register_native_func(heap, "menu_remove_item#2", func_menu_remove_item, NULL);
   fixscript_register_native_func(heap, "menu_get_item_count#1", func_menu_get_item_count, NULL);
   fixscript_register_native_func(heap, "menu_get_item#2", func_menu_get_item, NULL);
   fixscript_register_native_func(heap, "menu_show#4", func_menu_show, NULL);
   fixscript_register_native_func(heap, "show_message#4", func_show_message, NULL);
#ifndef __EMSCRIPTEN__
   fixscript_register_native_func(heap, "worker_create#5", func_worker_create, wl);
   fixscript_register_native_func(heap, "worker_send#2", func_worker_send, NULL);
#endif
   fixscript_register_native_func(heap, "timer_get_time#0", func_timer_get_time, NULL);
   fixscript_register_native_func(heap, "timer_get_micro_time#0", func_timer_get_micro_time, NULL);
   fixscript_register_native_func(heap, "timer_is_active#1", func_timer_is_active, (void *)0);
   fixscript_register_native_func(heap, "timer_start#1", func_timer_start, (void *)0);
   fixscript_register_native_func(heap, "timer_stop#1", func_timer_stop, NULL);
   fixscript_register_native_func(heap, "timer_restart#1", func_timer_start, (void *)1);
   fixscript_register_native_func(heap, "clipboard_set_text#1", func_clipboard_set_text, NULL);
   fixscript_register_native_func(heap, "clipboard_get_text#0", func_clipboard_get_text, NULL);
   fixscript_register_native_func(heap, "system_font_create#3", func_system_font_create, NULL);
   fixscript_register_native_func(heap, "system_font_get_list#0", func_system_font_get_list, NULL);
   fixscript_register_native_func(heap, "system_font_get_size#1", func_system_font_get_metrics, (void *)SFM_SIZE);
   fixscript_register_native_func(heap, "system_font_get_ascent#1", func_system_font_get_metrics, (void *)SFM_ASCENT);
   fixscript_register_native_func(heap, "system_font_get_descent#1", func_system_font_get_metrics, (void *)SFM_DESCENT);
   fixscript_register_native_func(heap, "system_font_get_height#1", func_system_font_get_metrics, (void *)SFM_HEIGHT);
   fixscript_register_native_func(heap, "system_font_get_string_advance#2", func_system_font_get_string_advance, NULL);
   fixscript_register_native_func(heap, "system_font_get_string_advance#4", func_system_font_get_string_advance, NULL);
   fixscript_register_native_func(heap, "system_font_get_string_position#3", func_system_font_get_string_position, NULL);
   fixscript_register_native_func(heap, "system_font_get_string_position#5", func_system_font_get_string_position, NULL);
   fixscript_register_native_func(heap, "system_font_draw_string#6", func_system_font_draw_string, NULL);
   fixscript_register_native_func(heap, "system_font_draw_string#8", func_system_font_draw_string, NULL);
   fixscript_register_native_func(heap, "notify_icon_create#1", func_notify_icon_create, NULL);
   fixscript_register_native_func(heap, "notify_icon_get_sizes#0", func_notify_icon_get_sizes, NULL);
   fixscript_register_native_func(heap, "notify_icon_destroy#1", func_notify_icon_destroy, NULL);
   fixscript_register_native_func(heap, "notify_icon_set_menu#2", func_notify_icon_set_menu, NULL);
   fixscript_register_native_func(heap, "notify_icon_get_menu#1", func_notify_icon_get_menu, NULL);
   fixscript_register_native_func(heap, "modifiers_cmd_mask#0", func_modifiers_cmd_mask, NULL);

   register_platform_gui_functions(heap);
}


static Value func_worker_is_present(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   return fixscript_int(1);
}


void fixgui_register_worker_functions(Heap *heap)
{
   fixscript_register_native_func(heap, "worker_is_present#0", func_worker_is_present, NULL);
#ifndef __EMSCRIPTEN__
   fixscript_register_native_func(heap, "worker_send#1", func_worker_send, NULL);
   fixscript_register_native_func(heap, "worker_receive#0", func_worker_receive, NULL);
   fixscript_register_native_func(heap, "worker_receive#1", func_worker_receive, NULL);
#endif
   fixscript_register_native_func(heap, "timer_get_time#0", func_timer_get_time, NULL);
   fixscript_register_native_func(heap, "timer_get_micro_time#0", func_timer_get_micro_time, NULL);

#ifdef __EMSCRIPTEN__
   fixgui__emscripten_register_worker_functions(heap);
#endif
}


void fixgui_run_in_main_thread(MainThreadFunc func, void *data)
{
   MainThreadData *mtd;

   mtd = malloc(sizeof(MainThreadData));
   mtd->func = func;
   mtd->data = data;
   post_to_main_thread(mtd);
}


void run_in_main_thread(void *data)
{
   MainThreadData *mtd = data;

   mtd->func(gui_heap, mtd->data);
   free(mtd);
}


static void event_loop_notify(void *data)
{
   io_notify();
}


void io_process()
{
   fixio_process_func(fixio_heap);
}


void __fixgui_integrate_io_event_loop(Heap *heap, void (*integrate_func)(Heap *, void (*)(void *), void *), void (*process_func)(Heap *))
{
   fixio_heap = heap;
   fixio_process_func = process_func;
   integrate_func(heap, event_loop_notify, NULL);
}
