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

#include <stdlib.h>
#include <string.h>
#include "fixgui_common.h"

struct View {
   ViewCommon common;
};

struct Menu {
   MenuCommon common;
};

struct Worker {
   WorkerCommon common;
};

struct NotifyIcon {
   NotifyIconCommon common;
};

struct SystemFont {
};


void trigger_delayed_gc(Heap *heap)
{
}


void free_view(View *view)
{
   free(view);
}


void free_menu(Menu *menu)
{
   free(menu);
}


void free_notify_icon(NotifyIcon *icon)
{
   free(icon);
}


void view_destroy(View *view)
{
}


void view_get_rect(View *view, Rect *rect)
{
   rect->x1 = 0;
   rect->y1 = 0;
   rect->x2 = 0;
   rect->y2 = 0;
}


void view_set_rect(View *view, Rect *rect)
{
}


void view_get_content_rect(View *view, Rect *rect)
{
   rect->x1 = 0;
   rect->y1 = 0;
   rect->x2 = 0;
   rect->y2 = 0;
}


void view_get_inner_rect(View *view, Rect *rect)
{
   rect->x1 = 0;
   rect->y1 = 0;
   rect->x2 = 0;
   rect->y2 = 0;
}


void view_set_visible(View *view, int visible)
{
}


int view_add(View *parent, View *view)
{
   return 1;
}


void view_focus(View *view)
{
}


int view_has_focus(View *view)
{
   return 0;
}


void view_get_sizing(View *view, float *grid_x, float *grid_y, int *form_small, int *form_medium, int *form_large, int *view_small, int *view_medium, int *view_large)
{
   *grid_x = 4;
   *grid_y = 4;
   *form_small = 4;
   *form_medium = 8;
   *form_large = 16;
   *view_small = 4;
   *view_medium = 8;
   *view_large = 16;
}


void view_get_default_size(View *view, int *width, int *height)
{
   *width = 64;
   *height = 16;
}


float view_get_scale(View *view)
{
   return 1.0f;
}


void view_set_cursor(View *view, int cursor)
{
}


int view_get_cursor(View *view)
{
   return CURSOR_DEFAULT;
}


View *window_create(plat_char *title, int width, int height, int flags)
{
   View *view;
   
   view = calloc(1, sizeof(View));
   if (!view) return NULL;

   return view;
}


plat_char *window_get_title(View *view)
{
   return strdup("");
}


void window_set_title(View *view, plat_char *title)
{
}


void window_set_minimum_size(View *view, int width, int height)
{
}


int window_is_maximized(View *view)
{
   return 0;
}


void window_set_status_text(View *view, plat_char *text)
{
}


int window_set_menu(View *view, Menu *old_menu, Menu *new_menu)
{
   return 1;
}


View *label_create(plat_char *label)
{
   View *view;
   
   view = calloc(1, sizeof(View));
   if (!view) return NULL;

   return view;
}


plat_char *label_get_label(View *view)
{
   return strdup("");
}


void label_set_label(View *view, plat_char *label)
{
}


View *text_field_create()
{
   View *view;
   
   view = calloc(1, sizeof(View));
   if (!view) return NULL;

   return view;
}


plat_char *text_field_get_text(View *view)
{
   return strdup("");
}


void text_field_set_text(View *view, plat_char *text)
{
}


int text_field_is_enabled(View *view)
{
   return 1;
}


void text_field_set_enabled(View *view, int enabled)
{
}


View *text_area_create()
{
   View *view;
   
   view = calloc(1, sizeof(View));
   if (!view) return NULL;

   return view;
}


plat_char *text_area_get_text(View *view)
{
   return strdup("");
}


void text_area_set_text(View *view, plat_char *text)
{
}


void text_area_append_text(View *view, plat_char *text)
{
}


void text_area_set_read_only(View *view, int read_only)
{
}


int text_area_is_read_only(View *view)
{
   return 0;
}


int text_area_is_enabled(View *view)
{
   return 1;
}


void text_area_set_enabled(View *view, int enabled)
{
}


View *button_create(plat_char *label, int flags)
{
   View *view;
   
   view = calloc(1, sizeof(View));
   if (!view) return NULL;

   return view;
}


plat_char *button_get_label(View *view)
{
   return strdup("");
}


void button_set_label(View *view, plat_char *label)
{
}


int button_is_enabled(View *view)
{
   return 1;
}


void button_set_enabled(View *view, int enabled)
{
}


View *table_create()
{
   View *view;
   
   view = calloc(1, sizeof(View));
   if (!view) return NULL;

   return view;
}


void table_set_columns(View *view, int num_columns, plat_char **titles)
{
}


int table_get_column_width(View *view, int idx)
{
   return 0;
}


void table_set_column_width(View *view, int idx, int width)
{
}


void table_clear(View *view)
{
}


void table_insert_row(View *view, int row, int num_columns, plat_char **values)
{
}


int table_get_selected_row(View *view)
{
   return -1;
}


void table_set_selected_row(View *view, int row)
{
}


View *canvas_create(int flags)
{
   View *view;
   
   view = calloc(1, sizeof(View));
   if (!view) return NULL;

   return view;
}


void canvas_set_scroll_state(View *view, int type, int pos, int max, int page_size, int always_show)
{
}


void canvas_set_scroll_position(View *view, int type, int pos)
{
}


int canvas_get_scroll_position(View *view, int type)
{
   return 0;
}


void canvas_set_active_rendering(View *view, int enable)
{
}


int canvas_get_active_rendering(View *view)
{
   return 0;
}


void canvas_set_relative_mode(View *view, int enable)
{
}


int canvas_get_relative_mode(View *view)
{
   return 0;
}


void canvas_set_overdraw_size(View *view, int size)
{
}


int canvas_get_overdraw_size(View *view)
{
   return 0;
}


void canvas_set_focusable(View *view, int enable)
{
}


int canvas_is_focusable(View *view)
{
   return 0;
}


void canvas_repaint(View *view, Rect *rect)
{
}


Menu *menu_create()
{
   Menu *menu;
   
   menu = calloc(1, sizeof(Menu));
   if (!menu) return NULL;

   return menu;
}


void menu_insert_item(Menu *menu, int idx, plat_char *title, MenuItem *item)
{
}


void menu_insert_separator(Menu *menu, int idx)
{
}


int menu_insert_submenu(Menu *menu, int idx, plat_char *title, Menu *submenu)
{
   return 1;
}


void menu_remove_item(Menu *menu, int idx, MenuItem *item)
{
}


void menu_show(Menu *menu, View *view, int x, int y)
{
}


int show_message(View *window, int type, plat_char *title, plat_char *msg)
{
   return 0;
}


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


SystemFont *system_font_create(Heap *heap, plat_char *family, float size, int style)
{
   SystemFont *font;
   
   font = calloc(1, sizeof(SystemFont));
   if (!font) return NULL;

   return font;
}


void system_font_destroy(SystemFont *font)
{
   free(font);
}


plat_char **system_font_get_list()
{
   return NULL;
}


int system_font_get_size(SystemFont *font)
{
   return 0;
}


int system_font_get_ascent(SystemFont *font)
{
   return 0;
}


int system_font_get_descent(SystemFont *font)
{
   return 0;
}


int system_font_get_height(SystemFont *font)
{
   return 0;
}


int system_font_get_string_advance(SystemFont *font, plat_char *s)
{
   return 0;
}


float system_font_get_string_position(SystemFont *font, plat_char *s, int x)
{
   return 0.0f;
}


void system_font_draw_string(SystemFont *font, int x, int y, plat_char *text, uint32_t color, uint32_t *pixels, int width, int height, int stride)
{
}


NotifyIcon *notify_icon_create(Heap *heap, Value *images, int num_images, char **error_msg)
{
   NotifyIcon *icon;
   
   icon = calloc(1, sizeof(NotifyIcon));
   if (!icon) return NULL;

   return icon;
}


void notify_icon_get_sizes(int **sizes, int *cnt)
{
}


void notify_icon_destroy(NotifyIcon *icon)
{
}


int notify_icon_set_menu(NotifyIcon *icon, Menu *menu)
{
   return 1;
}


void io_notify()
{
}


void post_to_main_thread(void *data)
{
}


int modifiers_cmd_mask()
{
   return SCRIPT_MOD_CMD;
}


void quit_app()
{
}


void register_platform_gui_functions(Heap *heap)
{
}
