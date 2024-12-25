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

#ifndef FIXGUI_COMMON_H
#define FIXGUI_COMMON_H

#include <stdint.h>
#include "fixgui.h"
#include "fiximage.h"

#ifdef __cplusplus
extern "C" {
#endif 

#if defined(_WIN32) && !defined(FIXGUI_VIRTUAL)
#define PLAT_CHAR 2
typedef unsigned short plat_char;
#else
#define PLAT_CHAR 1
typedef char plat_char;
#endif

enum {
   WIN_RESIZABLE = 0x01,
   WIN_CENTER    = 0x02,
   WIN_MAXIMIZE  = 0x04,
   WIN_MINIMIZE  = 0x08,
   WIN_MENUBAR   = 0x10,
   WIN_STATUSBAR = 0x20
};

enum {
   CANVAS_SCROLLABLE = 0x01,
   CANVAS_BORDER     = 0x02
};

enum {
   BTN_DEFAULT = 1 << 0
};

enum {
   CURSOR_DEFAULT,
   CURSOR_ARROW,
   CURSOR_EMPTY,
   CURSOR_TEXT,
   CURSOR_CROSS,
   CURSOR_HAND,
   CURSOR_MOVE,
   CURSOR_RESIZE_N,
   CURSOR_RESIZE_NE,
   CURSOR_RESIZE_E,
   CURSOR_RESIZE_SE,
   CURSOR_RESIZE_S,
   CURSOR_RESIZE_SW,
   CURSOR_RESIZE_W,
   CURSOR_RESIZE_NW,
   CURSOR_WAIT,
   NUM_CURSORS
};

enum {
   SCROLL_HORIZ,
   SCROLL_VERT
};

enum {
   MSG_OK,
   MSG_OK_CANCEL,
   MSG_YES_NO,
   MSG_YES_NO_CANCEL,

   MSG_ICON_INFO     = 1 << 8,
   MSG_ICON_QUESTION = 2 << 8,
   MSG_ICON_ERROR    = 3 << 8,
   MSG_ICON_WARNING  = 4 << 8
};

enum {
   MSG_BTN_OK,
   MSG_BTN_CANCEL,
   MSG_BTN_YES,
   MSG_BTN_NO
};

enum {
   CALLBACK_WINDOW_DESTROY,
   CALLBACK_WINDOW_CLOSE,
   CALLBACK_WINDOW_RESIZE,
   CALLBACK_WINDOW_ACTIVATE,

   CALLBACK_CANVAS_DESTROY,
   CALLBACK_CANVAS_RESIZE,
   CALLBACK_CANVAS_PAINT
};

enum {
   CALLBACK_BUTTON_ACTION
};

enum {
   CALLBACK_TABLE_CLICK_ACTION,
   CALLBACK_TABLE_RIGHT_CLICK_ACTION,
   CALLBACK_TABLE_SPACE_KEY_ACTION,
   CALLBACK_TABLE_SORT_ACTION
};

enum {
   EVENT_HIT_TEST,
   EVENT_MOUSE_MOVE,
   EVENT_MOUSE_DRAG,
   EVENT_MOUSE_DOWN,
   EVENT_MOUSE_UP,
   EVENT_MOUSE_ENTER,
   EVENT_MOUSE_LEAVE,
   EVENT_MOUSE_WHEEL,
   EVENT_MOUSE_RELATIVE,
   EVENT_TOUCH_START,
   EVENT_TOUCH_END,
   EVENT_TOUCH_MOVE,
   EVENT_TOUCH_ENTER,
   EVENT_TOUCH_LEAVE,
   EVENT_KEY_DOWN,
   EVENT_KEY_UP,
   EVENT_KEY_TYPED,
   EVENT_FOCUS_GAINED,
   EVENT_FOCUS_LOST
};

enum {
   EVENT_type,
   EVENT_view,
   EVENT_SIZE
};

enum {
   MOUSE_EVENT_x = EVENT_SIZE,
   MOUSE_EVENT_y,
   MOUSE_EVENT_button,
   MOUSE_EVENT_modifiers,
   MOUSE_EVENT_click_count,
   MOUSE_EVENT_wheel_x,
   MOUSE_EVENT_wheel_y,
   MOUSE_EVENT_scroll_x,
   MOUSE_EVENT_scroll_y,
   MOUSE_EVENT_touch,
   MOUSE_EVENT_SIZE
};

enum {
   TOUCH_EVENT_id = EVENT_SIZE,
   TOUCH_EVENT_x,
   TOUCH_EVENT_y,
   TOUCH_EVENT_mouse_emitter,
   TOUCH_EVENT_cancelled,
   TOUCH_EVENT_time,
   TOUCH_EVENT_SIZE
};

enum {
   KEY_EVENT_key = EVENT_SIZE,
   KEY_EVENT_chars,
   KEY_EVENT_modifiers,
   KEY_EVENT_SIZE
};

enum {
   FOCUS_EVENT_subtype = EVENT_SIZE,
   FOCUS_EVENT_SIZE
};

enum {
   FOCUS_NORMAL,
   FOCUS_NEXT,
   FOCUS_PREV
};

enum {
   MOUSE_BUTTON_LEFT,
   MOUSE_BUTTON_MIDDLE,
   MOUSE_BUTTON_RIGHT
};

enum {
   SCRIPT_MOD_CTRL    = 0x01,
   SCRIPT_MOD_SHIFT   = 0x02,
   SCRIPT_MOD_ALT     = 0x04,
   SCRIPT_MOD_CMD     = 0x08,
   SCRIPT_MOD_LBUTTON = 0x10,
   SCRIPT_MOD_MBUTTON = 0x20,
   SCRIPT_MOD_RBUTTON = 0x40,

   SCRIPT_MOD_KEY_BUTTONS   = 0x0F, // MOD_CTRL | MOD_SHIFT | MOD_ALT | MOD_CMD
   SCRIPT_MOD_MOUSE_BUTTONS = 0x70  // MOD_LBUTTON | MOD_MBUTTON | MOD_RBUTTON
};

enum {
   KEY_NONE,
   KEY_ESCAPE,
   KEY_F1,
   KEY_F2,
   KEY_F3,
   KEY_F4,
   KEY_F5,
   KEY_F6,
   KEY_F7,
   KEY_F8,
   KEY_F9,
   KEY_F10,
   KEY_F11,
   KEY_F12,
   KEY_PRINT_SCREEN,
   KEY_SCROLL_LOCK,
   KEY_PAUSE,
   KEY_GRAVE,
   KEY_NUM1,
   KEY_NUM2,
   KEY_NUM3,
   KEY_NUM4,
   KEY_NUM5,
   KEY_NUM6,
   KEY_NUM7,
   KEY_NUM8,
   KEY_NUM9,
   KEY_NUM0,
   KEY_MINUS,
   KEY_EQUAL,
   KEY_BACKSPACE,
   KEY_TAB,
   KEY_Q,
   KEY_W,
   KEY_E,
   KEY_R,
   KEY_T,
   KEY_Y,
   KEY_U,
   KEY_I,
   KEY_O,
   KEY_P,
   KEY_LBRACKET,
   KEY_RBRACKET,
   KEY_BACKSLASH,
   KEY_CAPS_LOCK,
   KEY_A,
   KEY_S,
   KEY_D,
   KEY_F,
   KEY_G,
   KEY_H,
   KEY_J,
   KEY_K,
   KEY_L,
   KEY_SEMICOLON,
   KEY_APOSTROPHE,
   KEY_ENTER,
   KEY_LSHIFT,
   KEY_Z,
   KEY_X,
   KEY_C,
   KEY_V,
   KEY_B,
   KEY_N,
   KEY_M,
   KEY_COMMA,
   KEY_PERIOD,
   KEY_SLASH,
   KEY_RSHIFT,
   KEY_LCONTROL,
   KEY_LMETA,
   KEY_LALT,
   KEY_SPACE,
   KEY_RALT,
   KEY_RMETA,
   KEY_RMENU,
   KEY_RCONTROL,
   KEY_INSERT,
   KEY_DELETE,
   KEY_HOME,
   KEY_END,
   KEY_PAGE_UP,
   KEY_PAGE_DOWN,
   KEY_LEFT,
   KEY_UP,
   KEY_RIGHT,
   KEY_DOWN,
   KEY_NUM_LOCK,
   KEY_NUMPAD_SLASH,
   KEY_NUMPAD_STAR,
   KEY_NUMPAD_MINUS,
   KEY_NUMPAD_PLUS,
   KEY_NUMPAD_ENTER,
   KEY_NUMPAD_DOT,
   KEY_NUMPAD0,
   KEY_NUMPAD1,
   KEY_NUMPAD2,
   KEY_NUMPAD3,
   KEY_NUMPAD4,
   KEY_NUMPAD5,
   KEY_NUMPAD6,
   KEY_NUMPAD7,
   KEY_NUMPAD8,
   KEY_NUMPAD9
};

enum {
   FONT_NORMAL = 0x00,
   FONT_BOLD   = 0x01,
   FONT_ITALIC = 0x02
};

enum {
   TYPE_WINDOW,
   TYPE_LABEL,
   TYPE_TEXT_FIELD,
   TYPE_TEXT_AREA,
   TYPE_BUTTON,
   TYPE_TABLE,
   TYPE_CANVAS
};

struct View;

typedef struct {
   Heap *heap;
   Value instance;
   struct View *parent;
   struct View *prev, *next;
   struct View *first_child, *last_child;
   int type;
   union {
      struct {
         Value menu;
      } window;
   };
} ViewCommon;

typedef struct MenuItem {
   plat_char *title;
   struct Menu *submenu;
   Value action;
   Value data;
   Value id;
   struct MenuItem *next;
} MenuItem;

typedef struct {
   Heap *heap;
   Value instance;
   struct Menu *parent;
   MenuItem *items;
   int num_items;
} MenuCommon;

#ifndef __EMSCRIPTEN__

typedef struct {
   WorkerLoadFunc func;
   void *data;
} WorkerLoad;

typedef struct {
   volatile int refcnt;
   Heap *main_heap;
   Value handle;
   Heap *comm_heap;
   Value comm_input, comm_output;
   Value callback_func;
   Value callback_data;
   WorkerLoad load;
   char *script_name;
   char *func_name;
   Value params;
   void (*main_func)(void *);
   void (*notify_func)(void *);
   int finished;
} WorkerCommon;

#endif

typedef struct {
   Heap *heap;
   Value instance;
   Value menu;
} NotifyIconCommon;

typedef struct View View;
typedef struct Menu Menu;
typedef struct Worker Worker;
typedef struct SystemFont SystemFont;
typedef struct NotifyIcon NotifyIcon;

typedef struct {
   int x1, y1, x2, y2;
} Rect;

#define view_get_native fixgui__view_get_native
#define menu_get_native fixgui__menu_get_native
#define notify_icon_get_native fixgui__notify_icon_get_native
#define notify_icon_create_handle fixgui__notify_icon_create_handle
#define system_font_create_handle fixgui__system_font_create_handle

#define trigger_delayed_gc fixgui__trigger_delayed_gc
#define free_view fixgui__free_view
#define free_menu fixgui__free_menu
#define free_notify_icon fixgui__free_notify_icon
#define view_destroy fixgui__view_destroy
#define view_get_rect fixgui__view_get_rect
#define view_set_rect fixgui__view_set_rect
#define view_get_content_rect fixgui__view_get_content_rect
#define view_get_inner_rect fixgui__view_get_inner_rect
#define view_set_visible fixgui__view_set_visible
#define view_add fixgui__view_add
#define view_focus fixgui__view_focus
#define view_has_focus fixgui__view_has_focus
#define view_get_sizing fixgui__view_get_sizing
#define view_get_default_size fixgui__view_get_default_size
#define view_get_scale fixgui__view_get_scale
#define view_set_cursor fixgui__view_set_cursor
#define view_get_cursor fixgui__view_get_cursor
#define window_create fixgui__window_create
#define window_get_title fixgui__window_get_title
#define window_set_title fixgui__window_set_title
#define window_set_minimum_size fixgui__window_set_minimum_size
#define window_is_maximized fixgui__window_is_maximized
#define window_set_status_text fixgui__window_set_status_text
#define window_set_menu fixgui__window_set_menu
#define label_create fixgui__label_create
#define label_get_label fixgui__label_get_label
#define label_set_label fixgui__label_set_label
#define text_field_create fixgui__text_field_create
#define text_field_get_text fixgui__text_field_get_text
#define text_field_set_text fixgui__text_field_set_text
#define text_field_is_enabled fixgui__text_field_is_enabled
#define text_field_set_enabled fixgui__text_field_set_enabled
#define text_area_create fixgui__text_area_create
#define text_area_get_text fixgui__text_area_get_text
#define text_area_set_text fixgui__text_area_set_text
#define text_area_append_text fixgui__text_area_append_text
#define text_area_set_read_only fixgui__text_area_set_read_only
#define text_area_is_read_only fixgui__text_area_is_read_only
#define text_area_is_enabled fixgui__text_area_is_enabled
#define text_area_set_enabled fixgui__text_area_set_enabled
#define button_create fixgui__button_create
#define button_get_label fixgui__button_get_label
#define button_set_label fixgui__button_set_label
#define button_is_enabled fixgui__button_is_enabled
#define button_set_enabled fixgui__button_set_enabled
#define table_create fixgui__table_create
#define table_set_columns fixgui__table_set_columns
#define table_get_column_width fixgui__table_get_column_width
#define table_set_column_width fixgui__table_set_column_width
#define table_clear fixgui__table_clear
#define table_insert_row fixgui__table_insert_row
#define table_get_selected_row fixgui__table_get_selected_row
#define table_set_selected_row fixgui__table_set_selected_row
#define canvas_create fixgui__canvas_create
#define canvas_set_scroll_state fixgui__canvas_set_scroll_state
#define canvas_set_scroll_position fixgui__canvas_set_scroll_position
#define canvas_get_scroll_position fixgui__canvas_get_scroll_position
#define canvas_set_active_rendering fixgui__canvas_set_active_rendering
#define canvas_get_active_rendering fixgui__canvas_get_active_rendering
#define canvas_set_relative_mode fixgui__canvas_set_relative_mode
#define canvas_get_relative_mode fixgui__canvas_get_relative_mode
#define canvas_set_overdraw_size fixgui__canvas_set_overdraw_size
#define canvas_get_overdraw_size fixgui__canvas_get_overdraw_size
#define canvas_set_focusable fixgui__canvas_set_focusable
#define canvas_is_focusable fixgui__canvas_is_focusable
#define canvas_repaint fixgui__canvas_repaint
#define menu_create fixgui__menu_create
#define menu_insert_item fixgui__menu_insert_item
#define menu_insert_separator fixgui__menu_insert_separator
#define menu_insert_submenu fixgui__menu_insert_submenu
#define menu_remove_item fixgui__menu_remove_item
#define menu_show fixgui__menu_show
#define show_message fixgui__show_message
#define call_view_callback fixgui__call_view_callback
#define call_view_callback_with_value fixgui__call_view_callback_with_value
#define call_action_callback fixgui__call_action_callback
#define call_table_action_callback fixgui__call_table_action_callback
#define call_menu_callback fixgui__call_menu_callback
#define call_mouse_event_callback fixgui__call_mouse_event_callback
#define call_mouse_wheel_callback fixgui__call_mouse_wheel_callback
#define call_touch_event_callback fixgui__call_touch_event_callback
#define call_key_event_callback fixgui__call_key_event_callback
#define call_key_typed_event_callback fixgui__call_key_typed_event_callback
#define call_focus_event_callback fixgui__call_focus_event_callback
#define worker_create fixgui__worker_create
#define worker_start fixgui__worker_start
#define worker_notify fixgui__worker_notify
#define worker_lock fixgui__worker_lock
#define worker_wait fixgui__worker_wait
#define worker_unlock fixgui__worker_unlock
#define worker_destroy fixgui__worker_destroy
#define worker_ref fixgui__worker_ref
#define worker_unref fixgui__worker_unref
#define timer_get_time fixgui__timer_get_time
#define timer_get_micro_time fixgui__timer_get_micro_time
#define timer_is_active fixgui__timer_is_active
#define timer_start fixgui__timer_start
#define timer_stop fixgui__timer_stop
#define timer_run fixgui__timer_run
#define clipboard_set_text fixgui__clipboard_set_text
#define clipboard_get_text fixgui__clipboard_get_text
#define system_font_create fixgui__system_font_create
#define system_font_destroy fixgui__system_font_destroy
#define system_font_get_list fixgui__system_font_get_list
#define system_font_get_size fixgui__system_font_get_size
#define system_font_get_ascent fixgui__system_font_get_ascent
#define system_font_get_descent fixgui__system_font_get_descent
#define system_font_get_height fixgui__system_font_get_height
#define system_font_get_string_advance fixgui__system_font_get_string_advance
#define system_font_get_string_position fixgui__system_font_get_string_position
#define system_font_draw_string_custom fixgui__system_font_draw_string_custom
#define system_font_draw_string fixgui__system_font_draw_string
#define notify_icon_create fixgui__notify_icon_create
#define notify_icon_get_sizes fixgui__notify_icon_get_sizes
#define notify_icon_destroy fixgui__notify_icon_destroy
#define call_notify_icon_click_callback fixgui__call_notify_icon_click_callback
#define notify_icon_set_menu fixgui__notify_icon_set_menu
#define io_notify fixgui__io_notify
#define io_process fixgui__io_process
#define post_to_main_thread fixgui__post_to_main_thread
#define run_in_main_thread fixgui__run_in_main_thread
#define modifiers_cmd_mask fixgui__modifiers_cmd_mask
#define quit_app fixgui__quit_app
#define register_platform_gui_functions fixgui__register_platform_gui_functions
#define virtual_view_mark_refs fixgui__virtual_view_mark_refs
#define virtual_system_font_mark_refs fixgui__virtual_system_font_mark_refs
#define virtual_handle_resize fixgui__virtual_handle_resize
#define virtual_handle_paint fixgui__virtual_handle_paint
#define virtual_handle_mouse_event fixgui__virtual_handle_mouse_event
#define virtual_handle_mouse_wheel fixgui__virtual_handle_mouse_wheel
#define virtual_handle_touch_event fixgui__virtual_handle_touch_event
#define virtual_handle_key_event fixgui__virtual_handle_key_event
#define virtual_handle_key_typed_event fixgui__virtual_handle_key_typed_event
#define virtual_get_dirty_rect fixgui__virtual_get_dirty_rect
#define register_native_platform_gui_functions fixgui__register_native_platform_gui_functions
#define virtual_repaint_notify fixgui__virtual_repaint_notify
#define virtual_set_cursor fixgui__virtual_set_cursor
#define emscripten_register_worker_functions fixgui__emscripten_register_worker_functions

View *view_get_native(Heap *heap, Value *error, Value instance, int type);
Menu *menu_get_native(Heap *heap, Value *error, Value instance);
NotifyIcon *notify_icon_get_native(Heap *heap, Value *error, Value instance);
Value notify_icon_create_handle(Heap *heap, Value *error, NotifyIcon *icon);
Value system_font_create_handle(Heap *heap, Value *error, SystemFont *font);

void trigger_delayed_gc(Heap *heap);

void free_view(View *view);
void free_menu(Menu *menu);
void free_notify_icon(NotifyIcon *icon);

void view_destroy(View *view);
void view_get_rect(View *view, Rect *rect);
void view_set_rect(View *view, Rect *rect);
void view_get_content_rect(View *view, Rect *rect);
void view_get_inner_rect(View *view, Rect *rect);
void view_set_visible(View *view, int visible);
int view_add(View *parent, View *view);
void view_focus(View *view);
int view_has_focus(View *view);
void view_get_sizing(View *view, float *grid_x, float *grid_y, int *form_small, int *form_medium, int *form_large, int *view_small, int *view_medium, int *view_large);
void view_get_default_size(View *view, int *width, int *height);
float view_get_scale(View *view);
void view_set_cursor(View *view, int cursor);
int view_get_cursor(View *view);

View *window_create(plat_char *title, int width, int height, int flags);
plat_char *window_get_title(View *view);
void window_set_title(View *view, plat_char *title);
void window_set_minimum_size(View *view, int width, int height);
int window_is_maximized(View *view);
void window_set_status_text(View *view, plat_char *text);
int window_set_menu(View *view, Menu *old_menu, Menu *new_menu);

View *label_create(plat_char *label);
plat_char *label_get_label(View *view);
void label_set_label(View *view, plat_char *label);

View *text_field_create();
plat_char *text_field_get_text(View *view);
void text_field_set_text(View *view, plat_char *text);
int text_field_is_enabled(View *view);
void text_field_set_enabled(View *view, int enabled);

View *text_area_create();
plat_char *text_area_get_text(View *view);
void text_area_set_text(View *view, plat_char *text);
void text_area_append_text(View *view, plat_char *text);
void text_area_set_read_only(View *view, int read_only);
int text_area_is_read_only(View *view);
int text_area_is_enabled(View *view);
void text_area_set_enabled(View *view, int enabled);

View *button_create(plat_char *label, int flags);
plat_char *button_get_label(View *view);
void button_set_label(View *view, plat_char *label);
int button_is_enabled(View *view);
void button_set_enabled(View *view, int enabled);

View *table_create();
void table_set_columns(View *view, int num_columns, plat_char **titles);
int table_get_column_width(View *view, int idx);
void table_set_column_width(View *view, int idx, int width);
void table_clear(View *view);
void table_insert_row(View *view, int row, int num_columns, plat_char **values);
int table_get_selected_row(View *view);
void table_set_selected_row(View *view, int row);

View *canvas_create(int flags);
void canvas_set_scroll_state(View *view, int type, int pos, int max, int page_size, int always_show);
void canvas_set_scroll_position(View *view, int type, int pos);
int canvas_get_scroll_position(View *view, int type);
void canvas_set_active_rendering(View *view, int enable);
int canvas_get_active_rendering(View *view);
void canvas_set_relative_mode(View *view, int enable);
int canvas_get_relative_mode(View *view);
void canvas_set_overdraw_size(View *view, int size);
int canvas_get_overdraw_size(View *view);
void canvas_set_focusable(View *view, int enable);
int canvas_is_focusable(View *view);
void canvas_repaint(View *view, Rect *rect);

Menu *menu_create();
void menu_insert_item(Menu *menu, int idx, plat_char *title, MenuItem *item);
void menu_insert_separator(Menu *menu, int idx);
int menu_insert_submenu(Menu *menu, int idx, plat_char *title, Menu *submenu);
void menu_remove_item(Menu *menu, int idx, MenuItem *item);
void menu_show(Menu *menu, View *view, int x, int y);

int show_message(View *window, int type, plat_char *title, plat_char *msg);

void call_view_callback(View *view, int type);
void call_view_callback_with_value(View *view, int type, Value value);
void call_action_callback(View *view, int type);
int call_table_action_callback(View *view, int type, int column, int row, int x, int y);
void call_menu_callback(Menu *menu, int idx);
int call_mouse_event_callback(View *view, int type, int x, int y, int button, int mod, int click_count, int touch);
int call_mouse_wheel_callback(View *view, int x, int y, int mod, float wheel_x, float wheel_y, int scroll_x, int scroll_y);
int call_touch_event_callback(View *view, int type, int id, int x, int y, int mouse_emitter, int cancelled, uint32_t time);
int call_key_event_callback(View *view, int type, int key, int mod);
int call_key_typed_event_callback(View *view, const plat_char *chars, int mod);
void call_focus_event_callback(View *view, int type, int subtype);

Worker *worker_create();
int worker_start(Worker *worker);
void worker_notify(Worker *worker);
void worker_lock(Worker *worker);
void worker_wait(Worker *worker, int timeout);
void worker_unlock(Worker *worker);
void worker_destroy(Worker *worker);
void worker_ref(Worker *worker);
void worker_unref(Worker *worker);

uint32_t timer_get_time();
uint32_t timer_get_micro_time();
int timer_is_active(Heap *heap, Value instance);
void timer_start(Heap *heap, Value instance, int interval, int restart);
void timer_stop(Heap *heap, Value instance);
void timer_run(Heap *heap, Value instance);

void clipboard_set_text(plat_char *text);
plat_char *clipboard_get_text();

SystemFont *system_font_create(Heap *heap, plat_char *family, float size, int style);
void system_font_destroy(SystemFont *font);
plat_char **system_font_get_list();
int system_font_get_size(SystemFont *font);
int system_font_get_ascent(SystemFont *font);
int system_font_get_descent(SystemFont *font);
int system_font_get_height(SystemFont *font);
#ifdef FIXGUI_VIRTUAL
int system_font_get_string_advance(SystemFont *font, Value text, int off, int len);
float system_font_get_string_position(SystemFont *font, Value text, int off, int len, int x);
int system_font_draw_string_custom(SystemFont *font, Value painter, int x, int y, Value text, int off, int len, uint32_t color);
#else
int system_font_get_string_advance(SystemFont *font, plat_char *text);
float system_font_get_string_position(SystemFont *font, plat_char *text, int x);
#endif
void system_font_draw_string(SystemFont *font, int x, int y, plat_char *text, uint32_t color, uint32_t *pixels, int width, int height, int stride);

NotifyIcon *notify_icon_create(Heap *heap, Value *images, int num_images, char **error_msg);
void notify_icon_get_sizes(int **sizes, int *cnt);
void notify_icon_destroy(NotifyIcon *icon);
void call_notify_icon_click_callback(NotifyIcon *icon);
int notify_icon_set_menu(NotifyIcon *icon, Menu *menu);

void io_notify();
void io_process();
void post_to_main_thread(void *data);
void run_in_main_thread(void *data);

int modifiers_cmd_mask();
void quit_app();

void register_platform_gui_functions(Heap *heap);

#ifdef FIXGUI_VIRTUAL
void virtual_view_mark_refs(View *view);
void virtual_system_font_mark_refs(SystemFont *font);
void virtual_handle_resize(int width, int height, float scale);
void virtual_handle_paint(Value painter);
int virtual_handle_mouse_event(int type, int x, int y, int button, int mod, int click_count, int touch);
int virtual_handle_mouse_wheel(int x, int y, int mod, float wheel_x, float wheel_y, int scroll_x, int scroll_y);
int virtual_handle_touch_event(int type, int id, int x, int y, int mouse_emitter, int cancelled, uint32_t time);
int virtual_handle_key_event(int type, int key, int mod);
int virtual_handle_key_typed_event(const plat_char *chars, int mod);
int virtual_get_dirty_rect(int *x, int *y, int *width, int *height, int max_width, int max_height);

void register_native_platform_gui_functions(Heap *heap);
void virtual_repaint_notify();
void virtual_set_cursor(int type);
#endif

#ifdef __EMSCRIPTEN__
void emscripten_register_worker_functions(Heap *heap);
#endif

#ifdef __cplusplus
}
#endif 

#endif /* FIXGUI_COMMON_H */
