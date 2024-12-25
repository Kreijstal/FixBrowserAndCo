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

#define MIN(a, b) ((a)<(b)? (a):(b))
#define MAX(a, b) ((a)>(b)? (a):(b))

struct View {
   ViewCommon common;
   Value handle;
};

struct Menu {
   MenuCommon common;
};

struct NotifyIcon {
   NotifyIconCommon common;
};

struct SystemFont {
   Value handle;
};

static Heap *gui_heap = NULL;
static Script *handler_script = NULL;
static Value init_func;
static Value window_create_func;
static Value handle_resize_func;
static Value handle_paint_func;
static Value handle_mouse_event_func;
static Value handle_touch_event_func;
static Value handle_key_event_func;
static Value register_font_func;
static Value get_font_func;
static Value get_font_list_func;
static Value font_get_size_func;
static Value font_get_ascent_func;
static Value font_get_descent_func;
static Value font_get_height_func;
static Value font_get_string_advance_func;
static Value font_get_string_position_func;
static Value font_draw_string_func;
static Value set_content_view_func;
static Value set_content_opacity_func;
static Value set_keyboard_func;
static Value show_keyboard_func;
static Value hide_keyboard_func;
static Value is_keyboard_visible_func;
static Value get_visible_screen_area_func;
static Value log_func;

static int dirty_x1=0, dirty_y1=0, dirty_x2=0, dirty_y2=0;

static const uint8_t simple_font[] = {
   0x00,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x00,0x0C,0x00,0x00, // '!'
   0x00,0x1B,0x1B,0x1B,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, // '"'
   0x00,0x1E,0x1E,0x3F,0x1E,0x1E,0x1E,0x3F,0x1E,0x1E,0x00,0x00, // '#'
   0x0C,0x1E,0x33,0x03,0x03,0x1E,0x30,0x30,0x33,0x1E,0x0C,0x00, // '$'
   0x00,0x00,0x23,0x33,0x18,0x0C,0x06,0x33,0x31,0x00,0x00,0x00, // '%'
   0x00,0x0E,0x1B,0x1B,0x0E,0x27,0x3B,0x1B,0x3B,0x2E,0x00,0x00, // '&'
   0x00,0x0C,0x0C,0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, // '''
   0x00,0x18,0x0C,0x06,0x06,0x06,0x06,0x06,0x0C,0x18,0x00,0x00, // '('
   0x00,0x06,0x0C,0x18,0x18,0x18,0x18,0x18,0x0C,0x06,0x00,0x00, // ')'
   0x00,0x00,0x04,0x15,0x1F,0x0E,0x1F,0x15,0x04,0x00,0x00,0x00, // '*'
   0x00,0x00,0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00,0x00,0x00, // '+'
   0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06,0x00, // ','
   0x00,0x00,0x00,0x00,0x00,0x1F,0x00,0x00,0x00,0x00,0x00,0x00, // '-'
   0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00,0x00, // '.'
   0x00,0x18,0x18,0x18,0x0C,0x0C,0x0C,0x06,0x06,0x06,0x00,0x00, // '/'
   0x00,0x1E,0x33,0x3B,0x3B,0x37,0x37,0x33,0x33,0x1E,0x00,0x00, // '0'
   0x00,0x0C,0x0E,0x0F,0x0D,0x0C,0x0C,0x0C,0x0C,0x3F,0x00,0x00, // '1'
   0x00,0x1E,0x33,0x33,0x30,0x18,0x0C,0x06,0x03,0x3F,0x00,0x00, // '2'
   0x00,0x1E,0x33,0x30,0x30,0x1C,0x30,0x30,0x33,0x1E,0x00,0x00, // '3'
   0x00,0x38,0x3C,0x36,0x33,0x33,0x3F,0x30,0x30,0x30,0x00,0x00, // '4'
   0x00,0x3F,0x03,0x03,0x1F,0x30,0x30,0x30,0x33,0x1E,0x00,0x00, // '5'
   0x00,0x1E,0x33,0x03,0x03,0x1F,0x33,0x33,0x33,0x1E,0x00,0x00, // '6'
   0x00,0x3F,0x30,0x30,0x18,0x18,0x0C,0x0C,0x06,0x06,0x00,0x00, // '7'
   0x00,0x1E,0x33,0x33,0x33,0x1E,0x33,0x33,0x33,0x1E,0x00,0x00, // '8'
   0x00,0x1E,0x33,0x33,0x33,0x3E,0x30,0x30,0x33,0x1E,0x00,0x00, // '9'
   0x00,0x00,0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00,0x00,0x00, // ':'
   0x00,0x00,0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06,0x00,0x00, // ';'
   0x00,0x00,0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00,0x00,0x00, // '<'
   0x00,0x00,0x00,0x00,0x1F,0x00,0x1F,0x00,0x00,0x00,0x00,0x00, // '='
   0x00,0x00,0x03,0x06,0x0C,0x18,0x0C,0x06,0x03,0x00,0x00,0x00, // '>'
   0x00,0x1E,0x33,0x30,0x18,0x0C,0x0C,0x00,0x0C,0x0C,0x00,0x00, // '?'
   0x00,0x1E,0x33,0x3F,0x37,0x37,0x37,0x1F,0x03,0x1E,0x00,0x00, // '@'
   0x00,0x1E,0x33,0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00,0x00, // 'A'
   0x00,0x1F,0x33,0x33,0x33,0x1F,0x33,0x33,0x33,0x1F,0x00,0x00, // 'B'
   0x00,0x1E,0x33,0x03,0x03,0x03,0x03,0x03,0x33,0x1E,0x00,0x00, // 'C'
   0x00,0x1F,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x1F,0x00,0x00, // 'D'
   0x00,0x3F,0x03,0x03,0x03,0x1F,0x03,0x03,0x03,0x3F,0x00,0x00, // 'E'
   0x00,0x3F,0x03,0x03,0x03,0x1F,0x03,0x03,0x03,0x03,0x00,0x00, // 'F'
   0x00,0x1E,0x33,0x03,0x03,0x3B,0x33,0x33,0x33,0x1E,0x00,0x00, // 'G'
   0x00,0x33,0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x33,0x00,0x00, // 'H'
   0x00,0x3F,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3F,0x00,0x00, // 'I'
   0x00,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x33,0x1E,0x00,0x00, // 'J'
   0x00,0x33,0x33,0x1B,0x0F,0x07,0x0F,0x1B,0x33,0x33,0x00,0x00, // 'K'
   0x00,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x3F,0x00,0x00, // 'L'
   0x00,0x33,0x3F,0x3F,0x3F,0x33,0x33,0x33,0x33,0x33,0x00,0x00, // 'M'
   0x00,0x33,0x33,0x37,0x37,0x3B,0x3B,0x33,0x33,0x33,0x00,0x00, // 'N'
   0x00,0x1E,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x1E,0x00,0x00, // 'O'
   0x00,0x1F,0x33,0x33,0x33,0x1F,0x03,0x03,0x03,0x03,0x00,0x00, // 'P'
   0x00,0x1E,0x33,0x33,0x33,0x33,0x33,0x3B,0x3B,0x3E,0x00,0x00, // 'Q'
   0x00,0x1F,0x33,0x33,0x33,0x1F,0x1B,0x33,0x33,0x33,0x00,0x00, // 'R'
   0x00,0x1E,0x33,0x03,0x03,0x1E,0x30,0x30,0x33,0x1E,0x00,0x00, // 'S'
   0x00,0x3F,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x00,0x00, // 'T'
   0x00,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x1E,0x00,0x00, // 'U'
   0x00,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00,0x00, // 'V'
   0x00,0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x3F,0x33,0x00,0x00, // 'W'
   0x00,0x33,0x33,0x33,0x1E,0x0C,0x1E,0x33,0x33,0x33,0x00,0x00, // 'X'
   0x00,0x33,0x33,0x33,0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x00,0x00, // 'Y'
   0x00,0x3F,0x30,0x30,0x18,0x0C,0x06,0x03,0x03,0x3F,0x00,0x00, // 'Z'
   0x00,0x1E,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x1E,0x00,0x00, // '['
   0x00,0x06,0x06,0x06,0x0C,0x0C,0x0C,0x18,0x18,0x18,0x00,0x00, // '\'
   0x00,0x1E,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x1E,0x00,0x00, // ']'
   0x00,0x0C,0x1E,0x33,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, // '^'
   0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3F, // '_'
   0x00,0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, // '`'
   0x00,0x00,0x00,0x1E,0x33,0x30,0x3E,0x33,0x33,0x3E,0x00,0x00, // 'a'
   0x00,0x03,0x03,0x1F,0x33,0x33,0x33,0x33,0x33,0x1F,0x00,0x00, // 'b'
   0x00,0x00,0x00,0x1E,0x33,0x03,0x03,0x03,0x33,0x1E,0x00,0x00, // 'c'
   0x00,0x30,0x30,0x3E,0x33,0x33,0x33,0x33,0x33,0x3E,0x00,0x00, // 'd'
   0x00,0x00,0x00,0x1E,0x33,0x33,0x3F,0x03,0x33,0x1E,0x00,0x00, // 'e'
   0x00,0x1C,0x06,0x06,0x1F,0x06,0x06,0x06,0x06,0x06,0x00,0x00, // 'f'
   0x00,0x00,0x00,0x3E,0x33,0x33,0x33,0x33,0x3E,0x30,0x33,0x1E, // 'g'
   0x00,0x03,0x03,0x1F,0x33,0x33,0x33,0x33,0x33,0x33,0x00,0x00, // 'h'
   0x00,0x0C,0x00,0x0F,0x0C,0x0C,0x0C,0x0C,0x0C,0x3F,0x00,0x00, // 'i'
   0x00,0x18,0x00,0x1E,0x18,0x18,0x18,0x18,0x18,0x1B,0x0E,0x00, // 'j'
   0x00,0x03,0x03,0x33,0x1B,0x0F,0x07,0x0F,0x1B,0x33,0x00,0x00, // 'k'
   0x00,0x0F,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3F,0x00,0x00, // 'l'
   0x00,0x00,0x00,0x33,0x3F,0x3F,0x3F,0x33,0x33,0x33,0x00,0x00, // 'm'
   0x00,0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x33,0x33,0x00,0x00, // 'n'
   0x00,0x00,0x00,0x1E,0x33,0x33,0x33,0x33,0x33,0x1E,0x00,0x00, // 'o'
   0x00,0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x33,0x1F,0x03,0x03, // 'p'
   0x00,0x00,0x00,0x3E,0x33,0x33,0x33,0x33,0x33,0x3E,0x30,0x30, // 'q'
   0x00,0x00,0x00,0x33,0x3B,0x07,0x03,0x03,0x03,0x03,0x00,0x00, // 'r'
   0x00,0x00,0x00,0x1E,0x33,0x03,0x1E,0x30,0x33,0x1E,0x00,0x00, // 's'
   0x00,0x00,0x06,0x1F,0x06,0x06,0x06,0x06,0x06,0x1C,0x00,0x00, // 't'
   0x00,0x00,0x00,0x33,0x33,0x33,0x33,0x33,0x33,0x1E,0x00,0x00, // 'u'
   0x00,0x00,0x00,0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00,0x00, // 'v'
   0x00,0x00,0x00,0x33,0x33,0x33,0x33,0x3F,0x3F,0x33,0x00,0x00, // 'w'
   0x00,0x00,0x00,0x33,0x33,0x1E,0x0C,0x1E,0x33,0x33,0x00,0x00, // 'x'
   0x00,0x00,0x00,0x33,0x33,0x33,0x33,0x33,0x3E,0x30,0x33,0x1E, // 'y'
   0x00,0x00,0x00,0x3F,0x30,0x18,0x0C,0x06,0x03,0x3F,0x00,0x00, // 'z'
   0x00,0x38,0x0C,0x0C,0x0C,0x06,0x0C,0x0C,0x0C,0x38,0x00,0x00, // '{'
   0x00,0x0C,0x0C,0x0C,0x0C,0x00,0x0C,0x0C,0x0C,0x0C,0x00,0x00, // '|'
   0x00,0x07,0x0C,0x0C,0x0C,0x18,0x0C,0x0C,0x0C,0x07,0x00,0x00, // '}'
   0x00,0x2E,0x1B,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00  // '~'
};


static inline uint32_t div255(uint32_t a)
{
   return ((a << 8) + a + 255) >> 16;
}


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
   Heap *heap = gui_heap;
   View *view;
   Value title_val, error;
   
   view = calloc(1, sizeof(View));
   if (!view) return NULL;

   title_val = fixscript_create_string(heap, title, -1);

   view->handle = fixscript_call(heap, window_create_func, 4, &error, title_val, fixscript_int(width), fixscript_int(height), fixscript_int(flags));
   if (error.value) {
      fprintf(stderr, "error while creating window:\n");
      fixscript_dump_value(heap, error, 1);
   }

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


SystemFont *system_font_create(Heap *heap, plat_char *family, float size, int style)
{
   SystemFont *font;
   Value error;
   
   font = calloc(1, sizeof(SystemFont));
   if (!font) return NULL;

   font->handle = fixscript_call(heap, get_font_func, 3, &error, fixscript_create_string(heap, family, -1), fixscript_float(size), fixscript_int(style));
   fixscript_ref(heap, font->handle);
   return font;
}


void system_font_destroy(SystemFont *font)
{
   fixscript_unref(gui_heap, font->handle);
   free(font);
}


plat_char **system_font_get_list()
{
   Heap *heap = gui_heap;
   Value list, value, error;
   char **arr, *s;
   int i, idx=0, len;

   list = fixscript_call(heap, get_font_list_func, 0, &error);
   if (!fixscript_is_array(heap, list) || fixscript_get_array_length(heap, list, &len) != 0) {
      arr = calloc(2, sizeof(char *));
      arr[0] = strdup("Default");
      return arr;
   }

   arr = calloc(len+2, sizeof(char *));
   arr[idx++] = strdup("Default");
   for (i=0; i<len; i++) {
      if (fixscript_get_array_elem(heap, list, i, &value) != 0) continue;
      if (fixscript_get_string(heap, value, 0, -1, &s, NULL) != 0) continue;
      if (strcmp(s, "Default") == 0) {
         free(s);
         continue;
      }
      arr[idx++] = s;
   }
   return arr;
}


int system_font_get_size(SystemFont *font)
{
   Heap *heap = gui_heap;
   Value error;

   if (font->handle.value) {
      return fixscript_call(heap, font_get_size_func, 1, &error, font->handle).value;
   }
   return 12;
}


int system_font_get_ascent(SystemFont *font)
{
   Heap *heap = gui_heap;
   Value error;

   if (font->handle.value) {
      return fixscript_call(heap, font_get_ascent_func, 1, &error, font->handle).value;
   }
   return 11;
}


int system_font_get_descent(SystemFont *font)
{
   Heap *heap = gui_heap;
   Value error;

   if (font->handle.value) {
      return fixscript_call(heap, font_get_descent_func, 1, &error, font->handle).value;
   }
   return 3;
}


int system_font_get_height(SystemFont *font)
{
   Heap *heap = gui_heap;
   Value error;

   if (font->handle.value) {
      return fixscript_call(heap, font_get_height_func, 1, &error, font->handle).value;
   }
   return 14;
}


int system_font_get_string_advance(SystemFont *font, Value text, int off, int len)
{
   Heap *heap = gui_heap;
   Value error;

   if (font->handle.value) {
      return fixscript_call(heap, font_get_string_advance_func, 4, &error, font->handle, text, fixscript_int(off), fixscript_int(len)).value;
   }
   return len*7;
}


float system_font_get_string_position(SystemFont *font, Value text, int off, int len, int x)
{
   Heap *heap = gui_heap;
   Value error;
   float result;

   if (font->handle.value) {
      return fixscript_get_float(fixscript_call(heap, font_get_string_position_func, 5, &error, font->handle, text, fixscript_int(off), fixscript_int(len), fixscript_int(x)));
   }

   result = x / 7.0f;
   if (result < 0.0f) result = 0.0f;
   if (result > len) result = len;
   return result;
}


int system_font_draw_string_custom(SystemFont *font, Value painter, int x, int y, Value text, int off, int len, uint32_t color)
{
   Heap *heap = gui_heap;
   Value error;

   if (font->handle.value) {
      fixscript_call(heap, font_draw_string_func, 8, &error, font->handle, painter, fixscript_int(x), fixscript_int(y), text, fixscript_int(off), fixscript_int(len), fixscript_int(color));
      return 1;
   }
   return 0;
}


void system_font_draw_string(SystemFont *font, int x, int y, plat_char *text, uint32_t color, uint32_t *pixels, int width, int height, int stride)
{
   char *s = text;
   uint8_t value;
   uint32_t p, pa, pr, pg, pb, ca, cr, cg, cb, inv_ca;
   int i, j, idx;

   y -= 11;
   ca = (color >> 24) & 0xFF;
   cr = (color >> 16) & 0xFF;
   cg = (color >>  8) & 0xFF;
   cb = (color >>  0) & 0xFF;
   inv_ca = 255 - ca;

   for (;;) {
      idx = *s++;
      if (idx == 0) break;
      if (idx == 32) {
         x += 7;
         continue;
      }
      if (idx < 33) idx = '?';
      if (idx > 126) idx = '?';
      idx = (idx-33)*12;
      for (i=0; i<12; i++) {
         value = simple_font[idx+i];
         for (j=0; j<6; j++) {
            if (value & (1 << j)) {
               if (x+j >= 0 && y+i >= 0 && x+j < width && y+i < height) {
                  p = pixels[(y+i)*stride+(x+j)];
                  pa = (p >> 24) & 0xFF;
                  pr = (p >> 16) & 0xFF;
                  pg = (p >>  8) & 0xFF;
                  pb = (p >>  0) & 0xFF;

                  pa = ca + div255(pa * inv_ca);
                  pr = cr + div255(pr * inv_ca);
                  pg = cg + div255(pg * inv_ca);
                  pb = cb + div255(pb * inv_ca);

                  if (pr > 255) pr = 255;
                  if (pg > 255) pg = 255;
                  if (pb > 255) pb = 255;

                  pixels[(y+i)*stride+(x+j)] = (pa << 24) | (pr << 16) | (pg << 8) | pb;
               }
            }
         }
      }
      x += 7;
   }
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


int modifiers_cmd_mask()
{
   return SCRIPT_MOD_CTRL | SCRIPT_MOD_CMD;
}


void virtual_view_mark_refs(View *view)
{
   Heap *heap = view->common.heap;

   fixscript_mark_ref(heap, view->handle);
}


void virtual_system_font_mark_refs(SystemFont *font)
{
   Heap *heap = gui_heap;

   fixscript_mark_ref(heap, font->handle);
}


void fixgui_init_virtual(Heap *heap, LoadScriptFunc func, void *data)
{
   Value error;

   gui_heap = heap;
   handler_script = func(heap, "gui/virtual/handler", &error, data);
   if (!handler_script) {
      fprintf(stderr, "error: %s\n", fixscript_get_compiler_error(heap, error));
      fflush(stderr);
      return;
   }
   init_func = fixscript_get_function(heap, handler_script, "init#0");
   window_create_func = fixscript_get_function(heap, handler_script, "window_create#4");
   handle_resize_func = fixscript_get_function(heap, handler_script, "handle_resize#3");
   handle_paint_func = fixscript_get_function(heap, handler_script, "handle_paint#1");
   handle_mouse_event_func = fixscript_get_function(heap, handler_script, "handle_mouse_event#1");
   handle_touch_event_func = fixscript_get_function(heap, handler_script, "handle_touch_event#1");
   handle_key_event_func = fixscript_get_function(heap, handler_script, "handle_key_event#1");
   register_font_func = fixscript_get_function(heap, handler_script, "register_font#3");
   get_font_func = fixscript_get_function(heap, handler_script, "get_font#3");
   get_font_list_func = fixscript_get_function(heap, handler_script, "get_font_list#0");
   font_get_size_func = fixscript_get_function(heap, handler_script, "font_get_size#1");
   font_get_ascent_func = fixscript_get_function(heap, handler_script, "font_get_ascent#1");
   font_get_descent_func = fixscript_get_function(heap, handler_script, "font_get_descent#1");
   font_get_height_func = fixscript_get_function(heap, handler_script, "font_get_height#1");
   font_get_string_advance_func = fixscript_get_function(heap, handler_script, "font_get_string_advance#4");
   font_get_string_position_func = fixscript_get_function(heap, handler_script, "font_get_string_position#5");
   font_draw_string_func = fixscript_get_function(heap, handler_script, "font_draw_string#8");
   set_content_view_func = fixscript_get_function(heap, handler_script, "set_content_view#1");
   set_content_opacity_func = fixscript_get_function(heap, handler_script, "set_content_opacity#1");
   set_keyboard_func = fixscript_get_function(heap, handler_script, "set_keyboard#1");
   show_keyboard_func = fixscript_get_function(heap, handler_script, "show_keyboard#0");
   hide_keyboard_func = fixscript_get_function(heap, handler_script, "hide_keyboard#0");
   is_keyboard_visible_func = fixscript_get_function(heap, handler_script, "is_keyboard_visible#0");
   get_visible_screen_area_func = fixscript_get_function(heap, handler_script, "get_visible_screen_area#1");
   log_func = fixscript_get_function(heap, handler_script, "console_log#1");

   fixscript_call(heap, init_func, 0, &error);
}


void virtual_handle_resize(int width, int height, float scale)
{
   Heap *heap = gui_heap;
   Value error;

   trigger_delayed_gc(heap);

   fixscript_call(heap, handle_resize_func, 3, &error, fixscript_int(width), fixscript_int(height), fixscript_float(scale));
   if (error.value) {
      fprintf(stderr, "error while running resize handler:\n");
      fixscript_dump_value(heap, error, 1);
      return;
   }
}


void virtual_handle_paint(Value painter)
{
   Heap *heap = gui_heap;
   Value error;

   trigger_delayed_gc(heap);

   fixscript_call(heap, handle_paint_func, 1, &error, painter);
   if (error.value) {
      fprintf(stderr, "error while running paint handler:\n");
      fixscript_dump_value(heap, error, 1);
      return;
   }
}


int virtual_handle_mouse_event(int type, int x, int y, int button, int mod, int click_count, int touch)
{
   Heap *heap = gui_heap;
   Value error, ret, event, values[MOUSE_EVENT_SIZE];
   int err;

   trigger_delayed_gc(heap);

   event = fixscript_create_array(heap, MOUSE_EVENT_SIZE);
   if (!event.value) {
      fixscript_error(heap, &error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   memset(values, 0, sizeof(values));
   values[EVENT_type] = fixscript_int(type);
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

   ret = fixscript_call(heap, handle_mouse_event_func, 1, &error, event);
   if (error.value) {
      goto error;
   }
   return ret.value != 0;

error:
   fprintf(stderr, "error while running mouse event handler (type=%d):\n", type);
   fixscript_dump_value(heap, error, 1);
   return 0;
}


int virtual_handle_mouse_wheel(int x, int y, int mod, float wheel_x, float wheel_y, int scroll_x, int scroll_y)
{
   Heap *heap = gui_heap;
   Value error, ret, event, values[MOUSE_EVENT_SIZE];
   int err;

   trigger_delayed_gc(heap);

   event = fixscript_create_array(heap, MOUSE_EVENT_SIZE);
   if (!event.value) {
      fixscript_error(heap, &error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   memset(values, 0, sizeof(values));
   values[EVENT_type] = fixscript_int(EVENT_MOUSE_WHEEL);
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

   ret = fixscript_call(heap, handle_mouse_event_func, 1, &error, event);
   if (error.value) {
      goto error;
   }
   return ret.value != 0;

error:
   fprintf(stderr, "error while running mouse event callback (type=%d):\n", EVENT_MOUSE_WHEEL);
   fixscript_dump_value(heap, error, 1);
   return 0;
}


int virtual_handle_touch_event(int type, int id, int x, int y, int mouse_emitter, int cancelled, uint32_t time)
{
   Heap *heap = gui_heap;
   Value error, ret, event, values[TOUCH_EVENT_SIZE];
   int err;

   trigger_delayed_gc(heap);

   event = fixscript_create_array(heap, TOUCH_EVENT_SIZE);
   if (!event.value) {
      fixscript_error(heap, &error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   memset(values, 0, sizeof(values));
   values[EVENT_type] = fixscript_int(type);
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

   ret = fixscript_call(heap, handle_touch_event_func, 1, &error, event);
   if (error.value) {
      goto error;
   }
   return ret.value != 0;

error:
   fprintf(stderr, "error while running touch event handler (type=%d):\n", type);
   fixscript_dump_value(heap, error, 1);
   return 0;
}


int virtual_handle_key_event(int type, int key, int mod)
{
   Heap *heap = gui_heap;
   Value error, ret, event, values[KEY_EVENT_SIZE];
   int err;

   trigger_delayed_gc(heap);

   event = fixscript_create_array(heap, KEY_EVENT_SIZE);
   if (!event.value) {
      fixscript_error(heap, &error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   memset(values, 0, sizeof(values));
   values[EVENT_type] = fixscript_int(type);
   values[KEY_EVENT_key] = fixscript_int(key);
   values[KEY_EVENT_modifiers] = fixscript_int(mod);

   err = fixscript_set_array_range(heap, event, 0, KEY_EVENT_SIZE, values);
   if (err) {
      fixscript_error(heap, &error, err);
      goto error;
   }

   ret = fixscript_call(heap, handle_key_event_func, 1, &error, event);
   if (error.value) {
      goto error;
   }
   return ret.value != 0;

error:
   fprintf(stderr, "error while running key event handler (type=%d):\n", type);
   fixscript_dump_value(heap, error, 1);
   return 0;
}


int virtual_handle_key_typed_event(const plat_char *chars, int mod)
{
   Heap *heap = gui_heap;
   Value error, ret, event, values[KEY_EVENT_SIZE];
   int err;

   trigger_delayed_gc(heap);

   event = fixscript_create_array(heap, KEY_EVENT_SIZE);
   if (!event.value) {
      fixscript_error(heap, &error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   memset(values, 0, sizeof(values));
   values[EVENT_type] = fixscript_int(EVENT_KEY_TYPED);
   values[KEY_EVENT_chars] = fixscript_create_string(heap, chars, -1);
   values[KEY_EVENT_modifiers] = fixscript_int(mod);

   err = fixscript_set_array_range(heap, event, 0, KEY_EVENT_SIZE, values);
   if (err) {
      fixscript_error(heap, &error, err);
      goto error;
   }

   ret = fixscript_call(heap, handle_key_event_func, 1, &error, event);
   if (error.value) {
      goto error;
   }
   return ret.value != 0;

error:
   fprintf(stderr, "error while running key typed event handler:\n");
   fixscript_dump_value(heap, error, 1);
   return 0;
}


int virtual_get_dirty_rect(int *x, int *y, int *width, int *height, int max_width, int max_height)
{
   if (dirty_x1 < 0) dirty_x1 = 0;
   if (dirty_y1 < 0) dirty_y1 = 0;
   if (dirty_x2 > max_width) dirty_x2 = max_width;
   if (dirty_y2 > max_height) dirty_y2 = max_height;

   if (dirty_x1 >= dirty_x2 || dirty_y1 >= dirty_y2) {
      return 0;
   }

   *x = dirty_x1;
   *y = dirty_y1;
   *width = dirty_x2 - dirty_x1;
   *height = dirty_y2 - dirty_y1;
   dirty_x1 = 0;
   dirty_y1 = 0;
   dirty_x2 = 0;
   dirty_y2 = 0;
   return 1;
}


static Value func_virtual_repaint(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   if (dirty_x1 >= dirty_x2 || dirty_y1 >= dirty_y2) {
      dirty_x1 = params[0].value;
      dirty_y1 = params[1].value;
      dirty_x2 = params[0].value + params[2].value;
      dirty_y2 = params[1].value + params[3].value;
   }
   else {
      dirty_x1 = MIN(dirty_x1, params[0].value);
      dirty_y1 = MIN(dirty_y1, params[1].value);
      dirty_x2 = MAX(dirty_x2, params[0].value + params[2].value);
      dirty_y2 = MAX(dirty_y2, params[1].value + params[3].value);
   }
   virtual_repaint_notify();
   return fixscript_int(0);
}


static Value func_virtual_set_cursor(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   virtual_set_cursor(params[0].value);
   return fixscript_int(0);
}


static Value func_log(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   fixscript_call(heap, log_func, 1, error, params[0]);
   return fixscript_int(0);
}


static Value func_virtual_is_present(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   return fixscript_int(1);
}


static Value func_virtual_register_font(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   fixscript_call(heap, register_font_func, 3, error, params[0], params[1], params[2]);
   return fixscript_int(0);
}


static Value func_virtual_set_content_view(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   fixscript_call(heap, set_content_view_func, 1, error, params[0]);
   return fixscript_int(0);
}


static Value func_virtual_set_content_opacity(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   fixscript_call(heap, set_content_opacity_func, 1, error, params[0]);
   return fixscript_int(0);
}


static Value func_virtual_post_key_event(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   fixscript_call(heap, handle_key_event_func, 1, error, params[0]);
   return fixscript_int(0);
}


static Value func_virtual_set_keyboard(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   fixscript_call(heap, set_keyboard_func, 1, error, params[0]);
   return fixscript_int(0);
}


static Value func_virtual_show_keyboard(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   fixscript_call(heap, show_keyboard_func, 0, error);
   return fixscript_int(0);
}


static Value func_virtual_hide_keyboard(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   fixscript_call(heap, hide_keyboard_func, 0, error);
   return fixscript_int(0);
}


static Value func_virtual_is_keyboard_visible(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   return fixscript_call(heap, is_keyboard_visible_func, 0, error);
}


static Value func_virtual_get_visible_screen_area(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   return fixscript_call(heap, get_visible_screen_area_func, 1, error, params[0]);
}


static Value func_virtual_force_quit(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#ifndef __wasm__
   exit(0);
#endif
   return fixscript_int(0);
}


void register_platform_gui_functions(Heap *heap)
{
   fixscript_register_native_func(heap, "virtual_repaint#4", func_virtual_repaint, NULL);
   fixscript_register_native_func(heap, "virtual_set_cursor#1", func_virtual_set_cursor, NULL);
   fixscript_register_native_func(heap, "log#1", func_log, NULL);

   fixscript_register_native_func(heap, "virtual_is_present#0", func_virtual_is_present, NULL);
   fixscript_register_native_func(heap, "virtual_register_font#3", func_virtual_register_font, NULL);
   fixscript_register_native_func(heap, "virtual_set_content_view#1", func_virtual_set_content_view, NULL);
   fixscript_register_native_func(heap, "virtual_set_content_opacity#1", func_virtual_set_content_opacity, NULL);
   fixscript_register_native_func(heap, "virtual_post_key_event#1", func_virtual_post_key_event, NULL);
   fixscript_register_native_func(heap, "virtual_set_keyboard#1", func_virtual_set_keyboard, NULL);
   fixscript_register_native_func(heap, "virtual_show_keyboard#0", func_virtual_show_keyboard, NULL);
   fixscript_register_native_func(heap, "virtual_hide_keyboard#0", func_virtual_hide_keyboard, NULL);
   fixscript_register_native_func(heap, "virtual_is_keyboard_visible#0", func_virtual_is_keyboard_visible, NULL);
   fixscript_register_native_func(heap, "virtual_get_visible_screen_area#1", func_virtual_get_visible_screen_area, NULL);
   fixscript_register_native_func(heap, "virtual_force_quit#0", func_virtual_force_quit, NULL);

   register_native_platform_gui_functions(heap);
}
