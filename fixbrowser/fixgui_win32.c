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

#define UNICODE
#define _UNICODE
#define WINVER 0x0500
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <windows.h>
#include <commctrl.h>
#include "fixgui_common.h"

#define MIN(a, b) ((a)<(b)? (a):(b))
#define MAX(a, b) ((a)>(b)? (a):(b))

#ifndef WM_MOUSEHWHEEL
#define WM_MOUSEHWHEEL 0x020E
#endif

enum {
   FONT_CAPTION,
   FONT_SMALL_CAPTION,
   FONT_MENU,
   FONT_STATUS,
   FONT_MESSAGE
};

enum {
   THEME_PROPS_BOOL,
   THEME_PROPS_COLOR,
   THEME_PROPS_ENUM,
   THEME_PROPS_MARGINS,
   THEME_PROPS_SIZE,
   THEME_PROPS_POSITION
};

struct View {
   ViewCommon common;
   HWND hwnd;
   Rect rect;
   int cursor;
   uint32_t last_click_time;
   int last_click_x, last_click_y;
   int last_click_count;
   union {
      struct {
         wchar_t *title;
         int flags;
         int min_width;
         int min_height;
         int maximized;
         HWND status_hwnd;
         HWND last_focus;
      } window;
      struct {
         wchar_t *label;
         int adj_y;
         int adj_height;
      } label;
      struct {
         wchar_t *text;
      } text_field;
      struct {
         wchar_t *label;
         int flags;
      } button;
      struct {
         int flags;
         struct {
            int pos;
            int max;
            int page_size;
            int always_show;
         } scroll[2];
         int view_scroll_x;
         int view_scroll_y;
         int active;
         View *next_active;
         Value image;
         int overdraw;
         int focusable;
         int disable_painting;
      } canvas;
   };
};

struct Menu {
   MenuCommon common;
   HMENU menu;
   int default_item;
};

struct Worker {
   WorkerCommon common;
   CRITICAL_SECTION section;
   HANDLE event;
};

typedef struct Timer {
   Heap *heap;
   Value instance;
   int interval;
   uint32_t next_time;
   struct Timer *next;
   struct Timer *fast_next;
} Timer;

typedef struct {
   int off_x, off_y;
   int width, height;
   int adv_x, adv_y;
   uint32_t *pixels;
} Glyph;

struct SystemFont {
   HFONT hfont;
   Heap *heap;
   Value hash;
   Glyph *glyphs;
   int glyphs_cap;
   int size;
   int ascent;
   int descent;
   int height;
};

struct NotifyIcon {
   NotifyIconCommon common;
   int id;
   HICON icon;
   Menu *menu;
   struct NotifyIcon *next;
};

typedef struct ThemeNotify {
   Heap *heap;
   Value func;
   struct ThemeNotify *next;
} ThemeNotify;

static HINSTANCE g_hInstance;
static HMODULE module;
static HDC temp_hdc;
static HFONT default_font;
static int base_unit_x, base_unit_y;
static View *new_window_view;
static UINT_PTR gc_timer = 0;
static Heap *gc_heap;
static HWND event_hwnd;
static HCURSOR cursors[NUM_CURSORS];
static View *hover_view = NULL;
static View *focus_view = NULL;
static uint8_t gamma_table[512];
static int use_cleartype = 0;
static int focus_type = FOCUS_NORMAL;
static CRITICAL_SECTION timer_section;
static HANDLE timer_event;
static HANDLE timers_processed_event;
static View *active_canvases = NULL;
static View *cur_next_active_canvas = NULL;
static Timer *active_timers = NULL;
static Timer *active_fast_timers = NULL;
static Timer *cur_next_timer = NULL;
static int min_timer_period = 1000;
static View *relative_view = NULL;
static int ignore_relative_event = 0;
static POINT relative_prev_pos = { 0 };
static int relative_has_pos = 0;
static NotifyIcon *notify_icons = NULL;
static Menu *cur_popup_menu = NULL;
static ThemeNotify *theme_notify_funcs = NULL;
static int uxtheme_init = 0;
static HMODULE uxtheme_lib;
static HANDLE WINAPI (*uxtheme_OpenThemeData)(HWND hwnd, LPCWSTR class_list);
static HRESULT WINAPI (*uxtheme_CloseThemeData)(HANDLE theme);
static HRESULT WINAPI (*uxtheme_GetThemeBool)(HANDLE theme, int part, int state, int prop, BOOL *val);
static HRESULT WINAPI (*uxtheme_GetThemeColor)(HANDLE theme, int part, int state, int prop, COLORREF *color);
static HRESULT WINAPI (*uxtheme_GetThemeEnumValue)(HANDLE theme, int part, int state, int prop, int *val);
static HRESULT WINAPI (*uxtheme_GetThemeMargins)(HANDLE theme, HDC hdc, int part, int state, int prop, LPCRECT rect, int *margins);
static HRESULT WINAPI (*uxtheme_GetThemePartSize)(HANDLE theme, HDC hdc, int part, int state, LPCRECT rect, int prop, LONG *size);
static HRESULT WINAPI (*uxtheme_GetThemePosition)(HANDLE theme, int part, int state, int prop, LONG *pos);
static HRESULT WINAPI (*uxtheme_DrawThemeBackground)(HANDLE theme, HDC hdc, int part, int state, LPCRECT rect, LPCRECT clip);
static HWND console_hwnd;

static void menu_real_create(Menu *menu, int popup);


static CALLBACK void gc_timer_handler(HWND arg1, UINT arg2, UINT_PTR arg3, DWORD arg4)
{
   fixscript_collect_heap(gc_heap);

   KillTimer(NULL, gc_timer);
   gc_timer = 0;
}


void trigger_delayed_gc(Heap *heap)
{
   if (gc_timer != 0) {
      KillTimer(NULL, gc_timer);
   }
   gc_timer = SetTimer(NULL, 0, 500, gc_timer_handler);
   gc_heap = heap;
}


void free_view(View *view)
{
   if (focus_view == view) {
      focus_view = NULL;
   }

   switch (view->common.type) {
      case TYPE_WINDOW:
         free(view->window.title);
         break;

      case TYPE_LABEL:
         free(view->label.label);
         break;

      case TYPE_TEXT_FIELD:
         free(view->text_field.text);
         break;

      case TYPE_BUTTON:
         free(view->button.label);
         break;

      case TYPE_CANVAS:
         fixscript_unref(view->common.heap, view->canvas.image);
         break;
   }
   
   free(view);
}


static void update_menu_after_destroying(Menu *menu)
{
   MenuItem *item;

   menu->menu = NULL;

   for (item=menu->common.items; item; item=item->next) {
      if (item->submenu) {
         update_menu_after_destroying(item->submenu);
      }
   }
}


void free_menu(Menu *menu)
{
   if (menu->menu) {
      DestroyMenu(menu->menu);
      update_menu_after_destroying(menu);
   }

   free(menu);
}


void free_notify_icon(NotifyIcon *icon)
{
   free(icon);
}


void view_destroy(View *view)
{
   View *top;

   top = view;
   while (top->common.parent) {
      top = top->common.parent;
   }

   if (top && top->common.type == TYPE_WINDOW) {
      if (top->window.last_focus == view->hwnd) {
         top->window.last_focus = NULL;
      }
   }

   DestroyWindow(view->hwnd);
}


void view_get_rect(View *view, Rect *rect)
{
   RECT r;
   int scroll_x = 0, scroll_y = 0;

   if (!view->hwnd || view->common.type != TYPE_WINDOW) {
      *rect = view->rect;
      return;
   }

   if (view->common.parent && view->common.parent->common.type == TYPE_CANVAS) {
      scroll_x = GetScrollPos(view->common.parent->hwnd, SB_HORZ);
      scroll_y = GetScrollPos(view->common.parent->hwnd, SB_VERT);
   }

   GetWindowRect(view->hwnd, &r);
   MapWindowPoints(HWND_DESKTOP, GetParent(view->hwnd), (LPPOINT)&r, 2);
   rect->x1 = r.left + scroll_x;
   rect->y1 = r.top + scroll_y;
   rect->x2 = r.right + scroll_x;
   rect->y2 = r.bottom + scroll_y;
}


void view_set_rect(View *view, Rect *rect)
{
   int scroll_x = 0, scroll_y = 0;

   if (view->common.parent && view->common.parent->common.type == TYPE_CANVAS) {
      scroll_x = GetScrollPos(view->common.parent->hwnd, SB_HORZ);
      scroll_y = GetScrollPos(view->common.parent->hwnd, SB_VERT);
   }

   SetWindowPos(view->hwnd, NULL, rect->x1 - scroll_x, rect->y1 - scroll_y, rect->x2 - rect->x1, rect->y2 - rect->y1, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
   view->rect = *rect;
}


void view_get_content_rect(View *view, Rect *rect)
{
   RECT r, r2;

   GetClientRect(view->hwnd, &r);
   rect->x1 = r.left;
   rect->y1 = r.top;
   rect->x2 = r.right;
   rect->y2 = r.bottom;

   if (view->common.type == TYPE_WINDOW) {
      if (view->window.status_hwnd) {
         GetWindowRect(view->window.status_hwnd, &r2);
         rect->y2 -= r2.bottom - r2.top;
      }
   }
}


void view_get_inner_rect(View *view, Rect *rect)
{
   RECT r;
   int style, exStyle;

   style = GetWindowLong(view->hwnd, GWL_STYLE);
   exStyle = GetWindowLong(view->hwnd, GWL_EXSTYLE);

   GetWindowRect(view->hwnd, &r);
   MapWindowPoints(HWND_DESKTOP, GetParent(view->hwnd), (LPPOINT)&r, 2);

   rect->x1 = r.left;
   rect->y1 = r.top;
   rect->x2 = r.right;
   rect->y2 = r.bottom;

   AdjustWindowRectEx(&r, style, 0, exStyle);

   rect->x1 += rect->x1 - r.left;
   rect->y1 += rect->y1 - r.top;
   rect->x2 -= r.right - rect->x2;
   rect->y2 -= r.bottom - rect->y2;
}


void view_set_visible(View *view, int visible)
{
   int flags = 0;

   if (view->common.type == TYPE_WINDOW) {
      flags = view->window.flags;
   }

   if (flags & WIN_MAXIMIZE) {
      ShowWindow(view->hwnd, SW_MAXIMIZE);
   }
   else if (flags & WIN_MINIMIZE) {
      ShowWindow(view->hwnd, SW_MINIMIZE);
   }
   else {
      ShowWindow(view->hwnd, SW_SHOWNORMAL);
   }
   UpdateWindow(view->hwnd);
}


static HWND create_control(HWND owner, short id, Rect *rect, const wchar_t *type, int style, int exStyle)
{
   HWND hwnd;
   
   hwnd = CreateWindowEx(exStyle, type, L"", style, rect->x1, rect->y1, rect->x2 - rect->x1, rect->y2 - rect->y1, owner, (HMENU)(int)id, module, NULL);
   SendMessage(hwnd, WM_SETFONT, (WPARAM)default_font, MAKELPARAM(FALSE, 0));
   return hwnd;
}


int view_add(View *parent, View *view)
{
   Rect *rect, adj_rect;
   int scroll_x, scroll_y;
   int i, style;

   if (view->hwnd) return 0;

   rect = &view->rect;
   if (parent->common.type == TYPE_CANVAS) {
      adj_rect = *rect;
      scroll_x = GetScrollPos(parent->hwnd, SB_HORZ);
      scroll_y = GetScrollPos(parent->hwnd, SB_VERT);
      adj_rect.x1 -= scroll_x;
      adj_rect.y1 -= scroll_y;
      adj_rect.x2 -= scroll_x;
      adj_rect.y2 -= scroll_y;
      rect = &adj_rect;
   }

   switch (view->common.type) {
      case TYPE_LABEL:
         style = WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE;
         view->hwnd = create_control(parent->hwnd, 0, rect, L"STATIC", style, 0);
         SendMessage(view->hwnd, WM_SETTEXT, 0, (LPARAM)view->label.label);
         break;

      case TYPE_TEXT_FIELD:
         style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_LEFT | ES_AUTOHSCROLL;
         view->hwnd = create_control(parent->hwnd, 0, rect, L"EDIT", style, WS_EX_CLIENTEDGE);
         if (view->text_field.text) {
            SendMessage(view->hwnd, WM_SETTEXT, 0, (LPARAM)view->text_field.text);
            free(view->text_field.text);
            view->text_field.text = NULL;
         }
         break;

      case TYPE_BUTTON:
         style = WS_CHILD | WS_VISIBLE | WS_TABSTOP;
         if (view->button.flags & BTN_DEFAULT) {
            style |= BS_DEFPUSHBUTTON;
         }
         else {
            style |= BS_PUSHBUTTON;
         }
         view->hwnd = create_control(parent->hwnd, 0, rect, L"BUTTON", style, 0);
         SendMessage(view->hwnd, WM_SETTEXT, 0, (LPARAM)view->button.label);
         break;

      case TYPE_CANVAS:
         style = WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN;
         if (view->canvas.flags & CANVAS_SCROLLABLE) {
            style |= WS_HSCROLL | WS_VSCROLL;
         }
         if (view->canvas.focusable) {
            style |= WS_TABSTOP;
         }
         new_window_view = view;
         view->hwnd = create_control(parent->hwnd, 0, rect, L"Canvas", style, (view->canvas.flags & CANVAS_BORDER)? WS_EX_CLIENTEDGE : 0);
         new_window_view = NULL;
         for (i=0; i<2; i++) {
            canvas_set_scroll_state(view, i, view->canvas.scroll[i].pos, view->canvas.scroll[i].max, view->canvas.scroll[i].page_size, view->canvas.scroll[i].always_show);
         }
         if (view->canvas.active) {
            view->canvas.active = 0;
            canvas_set_active_rendering(view, 1);
         }
         break;
   }
   return 1;
}


void view_focus(View *view)
{
   if (view->common.type == TYPE_CANVAS && !view->canvas.focusable) {
      return;
   }

   if (view->hwnd) {
      SetFocus(view->hwnd);
   }
}


int view_has_focus(View *view)
{
   if (!view->hwnd) return 0;

   return GetFocus() == view->hwnd;
}


void view_get_sizing(View *view, float *grid_x, float *grid_y, int *form_small, int *form_medium, int *form_large, int *view_small, int *view_medium, int *view_large)
{
   float scale = base_unit_x/4.0f/1.5f;
   //*grid_x = base_unit_x/4.0f; // default=6
   //*grid_y = base_unit_y/8.0f; // default=13
   *grid_x = 4 * scale;
   *grid_y = 4 * scale;
   *form_small = roundf(4 * base_unit_x / 4.0f);
   *form_medium = roundf(7 * base_unit_x / 4.0f);
   *form_large = roundf(14 * base_unit_x / 4.0f);
   *view_small = roundf(4 * base_unit_x / 4.0f);
   *view_medium = roundf(7 * base_unit_x / 4.0f);
   *view_large = roundf(14 * base_unit_x / 4.0f);
}


void view_get_default_size(View *view, int *width, int *height)
{
   switch (view->common.type) {
      case TYPE_LABEL:
      case TYPE_TEXT_FIELD:
         *width = 32;
         *height = roundf(14 * base_unit_y / 8.0f);
         break;

      case TYPE_BUTTON:
         *width = roundf(50 * base_unit_x / 4.0f);
         *height = roundf(14 * base_unit_y / 8.0f);
         break;

      default:
         *width = 0;
         *height = 0;
         break;
   }
}


float view_get_scale(View *view)
{
   return base_unit_x/4.0f/1.5f;
}


void view_set_cursor(View *view, int cursor)
{
   POINT mouse;

   if (cursor < 0 || cursor >= NUM_CURSORS) {
      return;
   }

   if (view->cursor == cursor) {
      return;
   }

   view->cursor = cursor;

   if (view->hwnd) {
      GetCursorPos(&mouse);
      if (WindowFromPoint(mouse) == view->hwnd || GetCapture() == view->hwnd) {
         SetCursor(cursors[cursor]);
      }
   }
}


int view_get_cursor(View *view)
{
   return view->cursor;
}


View *window_create(plat_char *title, int width, int height, int flags)
{
   View *view;
   HWND hwnd;
   DWORD dwStyle, dwExStyle;
   RECT rect;
   Rect rect2;
   int parts[1];

   dwStyle = (flags & WIN_RESIZABLE? WS_OVERLAPPEDWINDOW : (WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME))) | WS_CLIPCHILDREN;
   dwExStyle = /*WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE*/0;

   rect.left = 0;
   rect.top = 0;
   rect.right = width;
   rect.bottom = height;
   AdjustWindowRectEx(&rect, dwStyle, (flags & WIN_MENUBAR) != 0, dwExStyle);
   OffsetRect(&rect, -rect.left, -rect.top);

   if (flags & WIN_CENTER) {
      rect.left = (GetSystemMetrics(SM_CXSCREEN) - rect.right) / 2;
      rect.top = (GetSystemMetrics(SM_CYSCREEN) - rect.bottom) / 2;
   }
   else {
      rect.left = CW_USEDEFAULT;
      rect.top = CW_USEDEFAULT;
   }

   view = calloc(1, sizeof(View));
   view->window.title = _wcsdup(title);
   view->window.flags = flags;
   new_window_view = view;

   hwnd = CreateWindowEx(
      dwExStyle,
      L"TopLevelWindow",
      title,
      dwStyle,
      rect.left, rect.top, rect.right, rect.bottom,
      NULL, NULL, g_hInstance, NULL
   );

   new_window_view = NULL;

   if (hwnd == NULL) {
      free_view(view);
      return NULL;
   }

   if (flags & WIN_STATUSBAR) {
      rect2.x1 = 0;
      rect2.y1 = 0;
      rect2.x2 = 0;
      rect2.y2 = 0;
      view->window.status_hwnd = create_control(view->hwnd, 0, &rect2, STATUSCLASSNAME, WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0);
      parts[0] = -1;
      SendMessage(view->window.status_hwnd, SB_SETPARTS, 1, (LPARAM)parts);
      SendMessage(view->window.status_hwnd, SB_SETTEXT, 0 | SBT_NOBORDERS, (LPARAM)L"");
   }
   return view;
}


plat_char *window_get_title(View *view)
{
   return _wcsdup(view->window.title);
}


void window_set_title(View *view, plat_char *title)
{
   free(view->window.title);
   view->window.title = _wcsdup(title);
   if (view->hwnd) {
      SendMessage(view->hwnd, WM_SETTEXT, 0, (LPARAM)view->window.title);
   }
}


void window_set_minimum_size(View *view, int width, int height)
{
   view->window.min_width = width;
   view->window.min_height = height;
}


int window_is_maximized(View *view)
{
   return view->window.maximized;
}


void window_set_status_text(View *view, plat_char *text)
{
   if (view->window.status_hwnd) {
      SendMessage(view->window.status_hwnd, SB_SETTEXT, 0 | SBT_NOBORDERS, (LPARAM)text);
   }
}


int window_set_menu(View *view, Menu *old_menu, Menu *new_menu)
{
   if ((view->window.flags & WIN_MENUBAR) == 0) return 0;
   if (new_menu && new_menu->menu) return 0;

   if (new_menu) {
      menu_real_create(new_menu, 0);
      SetMenu(view->hwnd, new_menu->menu);
   }
   else {
      SetMenu(view->hwnd, NULL);
   }
   DrawMenuBar(view->hwnd);

   if (old_menu) {
      DestroyMenu(old_menu->menu);
      update_menu_after_destroying(old_menu);
   }
   return 1;
}


View *label_create(plat_char *label)
{
   View *view;

   view = calloc(1, sizeof(View));
   view->label.label = _wcsdup(label);
   return view;
}


plat_char *label_get_label(View *view)
{
   return _wcsdup(view->label.label);
}


void label_set_label(View *view, plat_char *label)
{
   free(view->label.label);
   view->label.label = _wcsdup(label);
   if (view->hwnd) {
      SendMessage(view->hwnd, WM_SETTEXT, 0, (LPARAM)view->label.label);
   }
}


View *text_field_create()
{
   View *view;

   view = calloc(1, sizeof(View));
   return view;
}


plat_char *text_field_get_text(View *view)
{
   wchar_t *s;
   int len;

   if (view->hwnd) {
      len = GetWindowText(view->hwnd, NULL, 0);
      s = malloc((len+1)*2);
      if (!s) return _wcsdup(L"");
      if (GetWindowText(view->hwnd, s, len+1) != len) {
         free(s);
         return _wcsdup(L"");
      }
      return s;
   }
   else {
      return _wcsdup(view->text_field.text? view->text_field.text : L"");
   }
}


void text_field_set_text(View *view, plat_char *text)
{
   if (view->hwnd) {
      SendMessage(view->hwnd, WM_SETTEXT, 0, (LPARAM)text);
   }
   else {
      free(view->text_field.text);
      view->text_field.text = _wcsdup(text);
   }
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
   return _wcsdup(L"");
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
   view->button.label = _wcsdup(label);
   view->button.flags = flags;
   return view;
}


plat_char *button_get_label(View *view)
{
   return _wcsdup(view->button.label);
}


void button_set_label(View *view, plat_char *label)
{
   free(view->button.label);
   view->button.label = _wcsdup(label);
   if (view->hwnd) {
      SendMessage(view->hwnd, WM_SETTEXT, 0, (LPARAM)view->button.label);
   }
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
   view->canvas.flags = flags;
   return view;
}


static void update_canvas_subviews(View *view)
{
   RECT r;
   View *v;
   int scroll_x, scroll_y;
   int delta_x, delta_y;

   scroll_x = GetScrollPos(view->hwnd, SB_HORZ);
   scroll_y = GetScrollPos(view->hwnd, SB_VERT);

   delta_x = scroll_x - view->canvas.view_scroll_x;
   delta_y = scroll_y - view->canvas.view_scroll_y;

   if (delta_x != 0 || delta_y != 0) {
      for (v = view->common.first_child; v; v = v->common.next) {
         GetWindowRect(v->hwnd, &r);
         MapWindowPoints(HWND_DESKTOP, GetParent(v->hwnd), (LPPOINT)&r, 2);
         r.left -= delta_x;
         r.top -= delta_y;
         r.right -= delta_x;
         r.bottom -= delta_y;
         SetWindowPos(v->hwnd, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
         UpdateWindow(v->hwnd);
      }
   }

   view->canvas.view_scroll_x = scroll_x;
   view->canvas.view_scroll_y = scroll_y;
}


void canvas_set_scroll_state(View *view, int type, int pos, int max, int page_size, int always_show)
{
   SCROLLINFO si;

   if ((view->canvas.flags & CANVAS_SCROLLABLE) == 0) return;

   view->canvas.scroll[type].pos = pos;
   view->canvas.scroll[type].max = max;
   view->canvas.scroll[type].page_size = page_size;
   view->canvas.scroll[type].always_show = always_show;

   if (view->hwnd) {
      si.cbSize = sizeof(SCROLLINFO);
      si.fMask = SIF_PAGE | SIF_POS | SIF_RANGE;
      if (always_show) {
         si.fMask |= SIF_DISABLENOSCROLL;
      }
      si.nMin = 0;
      si.nMax = max;
      si.nPage = page_size;
      si.nPos = pos;
      SetScrollInfo(view->hwnd, type == SCROLL_HORIZ? SB_HORZ : SB_VERT, &si, TRUE);
      update_canvas_subviews(view);
   }
}


void canvas_set_scroll_position(View *view, int type, int pos)
{
   if ((view->canvas.flags & CANVAS_SCROLLABLE) == 0) return;
   view->canvas.scroll[type].pos = pos;
   if (view->hwnd) {
      SetScrollPos(view->hwnd, type == SCROLL_HORIZ? SB_HORZ : SB_VERT, pos, TRUE);
   }
}


int canvas_get_scroll_position(View *view, int type)
{
   if ((view->canvas.flags & CANVAS_SCROLLABLE) == 0) return 0;
   if (view->hwnd) {
      view->canvas.scroll[type].pos = GetScrollPos(view->hwnd, type == SCROLL_HORIZ? SB_HORZ : SB_VERT);
   }
   return view->canvas.scroll[type].pos;
}


static void free_hbmp(void *data)
{
   DeleteObject((HBITMAP)data);
}


static void canvas_handle_active_rendering(View *view)
{
   Heap *heap = view->common.heap;
   RECT rect;
   HBITMAP hbmp, prev_hbmp;
   BITMAPINFO bi;
   HDC hdc;
   int width, height;
   int cur_width=0, cur_height=0;
   uint32_t *pixels;
   Value error, painter;
   int scroll_x, scroll_y;

   if (!view->hwnd) return;

   GetClientRect(view->hwnd, &rect);
   width = rect.right - rect.left;
   height = rect.bottom - rect.top;
   if (width < 1) width = 1;
   if (height < 1) height = 1;

   if (view->canvas.image.value) {
      fiximage_get_data(heap, view->canvas.image, &cur_width, &cur_height, NULL, NULL, (void **)&hbmp, NULL);
   }

   if (width != cur_width || height != cur_height) {
      memset(&bi, 0, sizeof(BITMAPINFO));
      bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      bi.bmiHeader.biWidth = width;
      bi.bmiHeader.biHeight = -height;
      bi.bmiHeader.biBitCount = 32;
      bi.bmiHeader.biPlanes = 1;

      hbmp = CreateDIBSection(temp_hdc, &bi, DIB_RGB_COLORS, (void **)&pixels, NULL, 0);
      if (hbmp) {
         fixscript_unref(heap, view->canvas.image);
         view->canvas.image = fiximage_create_from_pixels(heap, width, height, width, pixels, free_hbmp, hbmp, -1);
         fixscript_ref(heap, view->canvas.image);
      }
      if (!hbmp || !view->canvas.image.value) {
         fprintf(stderr, "error while painting:\n");
         fixscript_error(heap, &error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         fixscript_dump_value(heap, error, 1);
         return;
      }
   }

   scroll_x = GetScrollPos(view->hwnd, SB_HORZ);
   scroll_y = GetScrollPos(view->hwnd, SB_VERT);

   painter = fiximage_create_painter(heap, view->canvas.image, -scroll_x, -scroll_y);
   if (!painter.value) {
      fprintf(stderr, "error while painting:\n");
      fixscript_error(heap, &error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      fixscript_dump_value(heap, error, 1);
      return;
   }

   call_view_callback_with_value(view, CALLBACK_CANVAS_PAINT, painter);

   if (!view->canvas.disable_painting) {
      hdc = GetDC(view->hwnd);
      prev_hbmp = SelectObject(temp_hdc, hbmp);
      BitBlt(hdc, 0, 0, width, height, temp_hdc, 0, 0, SRCCOPY);
      SelectObject(temp_hdc, prev_hbmp);
      ReleaseDC(view->hwnd, hdc);
   }

   trigger_delayed_gc(view->common.heap);
}


static DWORD WINAPI timer_thread(void *data)
{
   Timer *timer;
   uint32_t time;
   int32_t delta;
   int interval;

   for (;;) {
      EnterCriticalSection(&timer_section);
      for (;;) {
         if (active_canvases || active_fast_timers) {
            break;
         }

         time = timeGetTime();
         interval = INT_MAX;

         for (timer = active_timers; timer; timer = timer->next) {
            delta = (int32_t)(timer->next_time - time);
            if (delta < 0) delta = 0;
            if (delta < interval) {
               interval = delta;
            }
         }

         if (interval == 0) break;
         if (interval == INT_MAX) {
            interval = -1;
         }
         
         LeaveCriticalSection(&timer_section);
         WaitForSingleObject(timer_event, interval);
         EnterCriticalSection(&timer_section);
      }
      LeaveCriticalSection(&timer_section);

      PostMessage(event_hwnd, WM_USER+102, 0, 0);
      WaitForSingleObject(timers_processed_event, -1);
      Sleep(1);
   }
   return 0;
}


static void update_timer_period()
{
   Timer *timer;
   int min_period = 1000;

   if (active_canvases || active_fast_timers) {
      min_period = 0;
   }
   else {
      for (timer = active_timers; timer; timer = timer->next) {
         if (timer->interval < min_period) {
            min_period = timer->interval;
         }
      }
   }

   if (min_period < 1) {
      min_period = 1;
   }
   if (min_period != min_timer_period) {
      if (min_timer_period < 1000) {
         timeEndPeriod(min_timer_period);
      }
      min_timer_period = min_period;
      if (min_timer_period < 1000) {
         timeBeginPeriod(min_timer_period);
      }
   }
}


void canvas_set_active_rendering(View *view, int enable)
{
   View *v, *next;

   if (view->hwnd) {
      EnterCriticalSection(&timer_section);
      if (enable && !view->canvas.active) {
         if (!active_canvases) {
            SetEvent(timer_event);
         }
         view->canvas.next_active = active_canvases;
         active_canvases = view;
         update_timer_period();
      }
      else if (!enable && view->canvas.active) {
         if (cur_next_active_canvas == view) {
            cur_next_active_canvas = view->canvas.next_active;
         }
         if (active_canvases == view) {
            active_canvases = view->canvas.next_active;
            view->canvas.next_active = NULL;
         }
         else {
            for (v = active_canvases; v; v = v->canvas.next_active) {
               next = v->canvas.next_active;
               if (next == view) {
                  v->canvas.next_active = next->canvas.next_active;
                  next->canvas.next_active = NULL;
                  break;
               }
            }
         }
         if (!active_canvases) {
            trigger_delayed_gc(view->common.heap);
            update_timer_period();
         }
      }
      LeaveCriticalSection(&timer_section);
   }
   view->canvas.active = enable;
}


int canvas_get_active_rendering(View *view)
{
   return view->canvas.active;
}


void canvas_set_relative_mode(View *view, int enable)
{
   if (enable) {
      if (relative_view != view) {
         ignore_relative_event = 2;
      }
      relative_view = view;
   }
   else {
      if (relative_view == view) {
         ClipCursor(NULL);
         if (relative_has_pos) {
            SetCursorPos(relative_prev_pos.x, relative_prev_pos.y);
            relative_has_pos = 0;
         }
         relative_view = NULL;
      }
   }
}


int canvas_get_relative_mode(View *view)
{
   return relative_view == view;
}


void canvas_set_overdraw_size(View *view, int size)
{
   view->canvas.overdraw = size;
}


int canvas_get_overdraw_size(View *view)
{
   return view->canvas.overdraw;
}


void canvas_set_focusable(View *view, int enable)
{
   LONG style;
   
   view->canvas.focusable = enable;
   if (view->hwnd) {
      style = GetWindowLong(view->hwnd, GWL_STYLE);
      if (enable) {
         style |= WS_TABSTOP;
      }
      else {
         style &= ~WS_TABSTOP;
      }
      SetWindowLong(view->hwnd, GWL_STYLE, style);
   }
}


int canvas_is_focusable(View *view)
{
   return view->canvas.focusable;
}


void canvas_repaint(View *view, Rect *rect)
{
   RECT r;
   int scroll_x, scroll_y;

   if (!view->hwnd) return;
   if (view->canvas.active) return;
   
   if (rect) {
      scroll_x = GetScrollPos(view->hwnd, SB_HORZ);
      scroll_y = GetScrollPos(view->hwnd, SB_VERT);
      r.left = rect->x1 - view->canvas.overdraw - scroll_x;
      r.top = rect->y1 - view->canvas.overdraw - scroll_y;
      r.right = rect->x2 + view->canvas.overdraw - scroll_x;
      r.bottom = rect->y2 + view->canvas.overdraw - scroll_y;
      InvalidateRect(view->hwnd, &r, FALSE);
   }
   else {
      InvalidateRect(view->hwnd, NULL, FALSE);
   }
}


Menu *menu_create()
{
   Menu *menu;

   menu = calloc(1, sizeof(Menu));
   menu->default_item = -1;
   return menu;
}


static void menu_real_create(Menu *menu, int popup)
{
   MENUINFO info;
   MenuItem *item;

   menu->menu = popup? CreatePopupMenu() : CreateMenu();

   memset(&info, 0, sizeof(MENUINFO));
   info.cbSize = sizeof(MENUINFO);
   info.fMask = MIM_STYLE | MIM_MENUDATA;

   if (!GetMenuInfo(menu->menu, &info)) return;
   info.dwStyle |= MNS_NOTIFYBYPOS;
   info.dwMenuData = (ULONG_PTR)menu;
   if (!SetMenuInfo(menu->menu, &info)) return;

   for (item=menu->common.items; item; item=item->next) {
      if (item->submenu) {
         menu_real_create(item->submenu, 1);
         AppendMenu(menu->menu, MF_STRING | MF_POPUP, (UINT_PTR)item->submenu->menu, item->title);
      }
      else if (item->title) {
         AppendMenu(menu->menu, MF_STRING, 0, item->title);
      }
      else {
         AppendMenu(menu->menu, MF_SEPARATOR, 0, NULL);
      }
   }

   if (menu->default_item >= 0) {
      SetMenuDefaultItem(menu->menu, menu->default_item, TRUE);
   }
}


void menu_insert_item(Menu *menu, int idx, plat_char *title, MenuItem *item)
{
   if (menu->menu) {
      if (idx == -1) {
         AppendMenu(menu->menu, MF_STRING, 0, title);
      }
      else {
         InsertMenu(menu->menu, idx, MF_BYPOSITION | MF_STRING, 0, title);
      }
      // TODO: call DrawMenuBar
   }
}


void menu_insert_separator(Menu *menu, int idx)
{
   if (menu->menu) {
      if (idx == -1) {
         AppendMenu(menu->menu, MF_SEPARATOR, 0, NULL);
      }
      else {
         InsertMenu(menu->menu, idx, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
      }
      // TODO: call DrawMenuBar
   }
}


int menu_insert_submenu(Menu *menu, int idx, plat_char *title, Menu *submenu)
{
   if (submenu->menu) return 0;

   if (menu->menu) {
      menu_real_create(submenu, 1);
      if (idx == -1) {
         AppendMenu(menu->menu, MF_STRING | MF_POPUP, (UINT_PTR)submenu->menu, title);
      }
      else {
         InsertMenu(menu->menu, idx, MF_BYPOSITION | MF_STRING | MF_POPUP, (UINT_PTR)submenu->menu, title);
      }
      // TODO: call DrawMenuBar
   }
   return 1;
}


void menu_remove_item(Menu *menu, int idx, MenuItem *item)
{
   if (menu->menu) {
      RemoveMenu(menu->menu, idx, MF_BYPOSITION);
      if (item->submenu && item->submenu->menu) {
         DestroyMenu(item->submenu->menu);
         update_menu_after_destroying(item->submenu);
      }
      // TODO: call DrawMenuBar
   }
}


void menu_show(Menu *menu, View *view, int x, int y)
{
   RECT r;

   if (!view->hwnd) return;
   if (menu->menu) return;

   menu_real_create(menu, 1);

   GetWindowRect(view->hwnd, &r);
   x += r.left;
   y += r.top;

   cur_popup_menu = menu;
   TrackPopupMenu(menu->menu, TPM_RIGHTBUTTON, x, y, 0, event_hwnd, NULL);
}


int show_message(View *window, int type, plat_char *title, plat_char *msg)
{
   UINT win_type;
   int ret;

   switch (type & 0xFF) {
      default:
      case MSG_OK:            win_type = MB_OK; break;
      case MSG_OK_CANCEL:     win_type = MB_OKCANCEL; break;
      case MSG_YES_NO:        win_type = MB_YESNO; break;
      case MSG_YES_NO_CANCEL: win_type = MB_YESNOCANCEL; break;
   }

   switch (type & 0xFF00) {
      default:
      case MSG_ICON_INFO:     win_type |= MB_ICONINFORMATION; break;
      case MSG_ICON_QUESTION: win_type |= MB_ICONQUESTION; break;
      case MSG_ICON_ERROR:    win_type |= MB_ICONERROR; break;
      case MSG_ICON_WARNING:  win_type |= MB_ICONWARNING; break;
   }

   ret = MessageBox(window? window->hwnd : NULL, msg, title, win_type);

   switch (ret) {
      case IDOK:     ret = MSG_BTN_OK; break;
      case IDCANCEL: ret = MSG_BTN_CANCEL; break;
      case IDYES:    ret = MSG_BTN_YES; break;
      case IDNO:     ret = MSG_BTN_NO; break;

      default:
         ret = MSG_BTN_CANCEL;
   }

   return ret;
}


Worker *worker_create()
{
   Worker *worker;

   worker = calloc(1, sizeof(Worker));
   worker->event = CreateEvent(NULL, FALSE, FALSE, NULL);
   if (!worker->event) {
      free(worker);
      return NULL;
   }
   InitializeCriticalSection(&worker->section);
   return worker;
}


static DWORD WINAPI worker_main(void *data)
{
   Worker *worker = data;

   worker->common.main_func(worker);
   return 0;
}


int worker_start(Worker *worker)
{
   HANDLE thread;

   thread = CreateThread(NULL, 0, worker_main, worker, 0, NULL);
   if (!thread) {
      return 0;
   }
   CloseHandle(thread);
   return 1;
}


void worker_notify(Worker *worker)
{
   PostMessage(event_hwnd, WM_USER+101, 0, (LPARAM)worker);
}


void worker_lock(Worker *worker)
{
   EnterCriticalSection(&worker->section);
}


void worker_wait(Worker *worker, int timeout)
{
   if (timeout == 0) return;
   if (timeout < 0) timeout = -1;
   LeaveCriticalSection(&worker->section);
   WaitForSingleObject(worker->event, timeout);
   EnterCriticalSection(&worker->section);
}


void worker_unlock(Worker *worker)
{
   SetEvent(worker->event);
   LeaveCriticalSection(&worker->section);
}


void worker_destroy(Worker *worker)
{
   DeleteCriticalSection(&worker->section);
   CloseHandle(worker->event);
   free(worker);
}


uint32_t timer_get_time()
{
   uint64_t freq, time;
   QueryPerformanceFrequency((LARGE_INTEGER *)&freq);
   QueryPerformanceCounter((LARGE_INTEGER *)&time);
   return time * 1000 / freq;
}


uint32_t timer_get_micro_time()
{
   uint64_t freq, time;
   QueryPerformanceFrequency((LARGE_INTEGER *)&freq);
   QueryPerformanceCounter((LARGE_INTEGER *)&time);
   return time * 1000000 / freq;
}


int timer_is_active(Heap *heap, Value instance)
{
   Timer *timer;
   int found = 0;

   EnterCriticalSection(&timer_section);
   for (timer = active_timers; timer; timer = timer->next) {
      if (timer->heap == heap && timer->instance.value == instance.value && timer->instance.is_array == instance.is_array) {
         found = 1;
         break;
      }
   }
   LeaveCriticalSection(&timer_section);

   return found;
}


void timer_start(Heap *heap, Value instance, int interval, int restart)
{
   Timer *timer;
   
   EnterCriticalSection(&timer_section);
   for (timer = active_timers; timer; timer = timer->next) {
      if (timer->heap == heap && timer->instance.value == instance.value && timer->instance.is_array == instance.is_array) {
         break;
      }
   }
   if (timer) {
      if (restart) {
         timer->next_time = timeGetTime() + (uint32_t)timer->interval;
      }
   }
   else {
      timer = calloc(1, sizeof(Timer));
      timer->heap = heap;
      timer->instance = instance;
      timer->interval = interval;
      timer->next_time = timeGetTime() + (uint32_t)interval;
      timer->next = active_timers;
      active_timers = timer;
      fixscript_ref(heap, instance);

      if (interval == 0) {
         timer->fast_next = active_fast_timers;
         active_fast_timers = timer;
      }
      
      SetEvent(timer_event);
      update_timer_period();
   }
   LeaveCriticalSection(&timer_section);
}


void timer_stop(Heap *heap, Value instance)
{
   Timer *timer = NULL, **prev_timer, *t;
   
   EnterCriticalSection(&timer_section);
   prev_timer = &active_timers;
   for (timer = active_timers; timer; timer = timer->next) {
      if (timer->heap == heap && timer->instance.value == instance.value && timer->instance.is_array == instance.is_array) {
         break;
      }
      prev_timer = &timer->next;
   }
   if (timer) {
      if (cur_next_timer == timer) {
         cur_next_timer = timer->next;
      }
      
      if (timer->interval == 0) {
         if (active_fast_timers == timer) {
            active_fast_timers = timer->fast_next;
         }
         else {
            t = active_fast_timers;
            while (t->fast_next) {
               if (t->fast_next == timer) {
                  t->fast_next = timer->fast_next;
                  break;
               }
               t = t->fast_next;
            }
         }
      }

      *prev_timer = timer->next;
      fixscript_unref(timer->heap, timer->instance);
      free(timer);

      update_timer_period();
   }
   LeaveCriticalSection(&timer_section);
}


void clipboard_set_text(plat_char *text)
{
   HGLOBAL data;
   uint16_t *ptr;

   if (OpenClipboard(NULL)) {
      EmptyClipboard();
      data = GlobalAlloc(GHND, (wcslen(text)+1)*2);
      if (data) {
         ptr = GlobalLock(data);
         if (ptr) {
            wcscpy(ptr, text);
            GlobalUnlock(data);
            SetClipboardData(CF_UNICODETEXT, data);
         }
      }
      CloseClipboard();
   }
}


plat_char *clipboard_get_text()
{
   HGLOBAL data;
   uint16_t *ptr, *ret = NULL;

   if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
      if (OpenClipboard(NULL)) {
         data = GetClipboardData(CF_UNICODETEXT);
         if (data) {
            ptr = GlobalLock(data);
            if (ptr) {
               ret = _wcsdup(ptr);
               GlobalUnlock(data);
            }
         }
         CloseClipboard();
      }
   }
   return ret;
}


static SystemFont *create_font(Heap *heap, HFONT hfont, float size)
{
   SystemFont *font;
   Value hash;
   HFONT prev_font;
   TEXTMETRIC tm;

   hash = fixscript_create_hash(heap);
   if (!hash.value) {
      return NULL;
   }

   prev_font = SelectObject(temp_hdc, hfont);
   GetTextMetrics(temp_hdc, &tm);
   SelectObject(temp_hdc, prev_font);

   font = calloc(1, sizeof(SystemFont));
   font->hfont = hfont;
   font->heap = heap;
   font->hash = hash;
   font->size = (int)(size + 0.5f);
   font->ascent = tm.tmAscent;
   font->descent = tm.tmDescent;
   font->height = tm.tmHeight;
   fixscript_ref(heap, hash);
   return font;
}


SystemFont *system_font_create(Heap *heap, plat_char *family, float size, int style)
{
   HFONT hfont;

   hfont = CreateFont(
      -(int)(size + 0.5f),
      0, 0, 0,
      style & FONT_BOLD? FW_BOLD : FW_NORMAL,
      style & FONT_ITALIC? 1 : 0,
      0, 0,
      DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS,
      CLIP_DEFAULT_PRECIS,
      DEFAULT_QUALITY,
      DEFAULT_PITCH,
      family
   );
   if (!hfont) {
      return NULL;
   }
   return create_font(heap, hfont, size);
}


void system_font_destroy(SystemFont *font)
{
   Heap *heap = font->heap;
   int i, len = 0;

   DeleteObject(font->hfont);
   
   fixscript_get_array_length(heap, font->hash, &len);
   for (i=0; i<len; i++) {
      free(font->glyphs[i].pixels);
   }
   free(font->glyphs);
   fixscript_unref(heap, font->hash);
   free(font);
}


typedef struct {
   plat_char **list;
   int len, cap;
} EnumFontData;


static int CALLBACK EnumFontProc(const LOGFONT *font, const TEXTMETRIC *metric, DWORD type, LPARAM lParam)
{
   EnumFontData *data = (EnumFontData *)lParam;
   plat_char **new_list;
   int i;

   for (i=0; i<data->len; i++) {
      if (wcscmp(data->list[i], font->lfFaceName) == 0) {
         return 1;
      }
   }

   while (data->len+1 > data->cap) {
      data->cap = data->cap? data->cap*2 : 32;
      new_list = realloc(data->list, data->cap * sizeof(plat_char *));
      if (!new_list) return 0;
      data->list = new_list;
   }
   data->list[data->len++] = _wcsdup(font->lfFaceName);
   return 1;
}


plat_char **system_font_get_list()
{
   LOGFONT font;
   EnumFontData data;

   memset(&font, 0, sizeof(LOGFONT));
   memset(&data, 0, sizeof(EnumFontData));
   font.lfCharSet = DEFAULT_CHARSET;
   if (EnumFontFamiliesEx(temp_hdc, &font, EnumFontProc, (LPARAM)&data, 0) == 0) {
      free(data.list);
      return NULL;
   }
   if (data.list) {
      data.list[data.len] = NULL;
   }
   return data.list;
}


int system_font_get_size(SystemFont *font)
{
   return font->size;
}


int system_font_get_ascent(SystemFont *font)
{
   return font->ascent;
}


int system_font_get_descent(SystemFont *font)
{
   return font->descent;
}


int system_font_get_height(SystemFont *font)
{
   return font->height;
}


static Glyph *get_glyph(SystemFont *font, int c)
{
   Heap *heap = font->heap;
   Glyph *glyph, *new_glyphs;
   Value idx;
   HFONT prev_font;
   GLYPHMETRICS gm;
   MAT2 mat;
   BITMAPINFO bi;
   HBITMAP hbmp, prev_hbmp;
   uint16_t buf[2];
   uint32_t *pixels, p;
   int i, len = 0, new_cap, buf_size;

   if (fixscript_get_hash_elem(heap, font->hash, fixscript_int(c), &idx) == FIXSCRIPT_SUCCESS) {
      return &font->glyphs[idx.value];
   }

   fixscript_get_array_length(heap, font->hash, &len);

   if (len == font->glyphs_cap) {
      new_cap = font->glyphs_cap? font->glyphs_cap*2 : 8;
      if (new_cap > (1<<24)) {
         return NULL;
      }
      new_glyphs = realloc(font->glyphs, new_cap * sizeof(Glyph));
      if (!new_glyphs) {
         return NULL;
      }
      font->glyphs = new_glyphs;
      font->glyphs_cap = new_cap;
   }

   if (fixscript_set_hash_elem(heap, font->hash, fixscript_int(c), fixscript_int(len)) != FIXSCRIPT_SUCCESS) {
      return NULL;
   }

   glyph = &font->glyphs[len];
   memset(glyph, 0, sizeof(Glyph));

   prev_font = SelectObject(temp_hdc, font->hfont);
   mat.eM11.value = 1; mat.eM11.fract = 0;
   mat.eM12.value = 0; mat.eM12.fract = 0;
   mat.eM21.value = 0; mat.eM21.fract = 0;
   mat.eM22.value = 1; mat.eM22.fract = 0;
   if (GetGlyphOutline(temp_hdc, c, GGO_METRICS, &gm, 0, NULL, &mat) == GDI_ERROR) {
      SelectObject(temp_hdc, prev_font);
      return glyph;
   }

   glyph->off_x = gm.gmptGlyphOrigin.x-1;
   glyph->off_y = -gm.gmptGlyphOrigin.y;
   glyph->width = gm.gmBlackBoxX+3;
   glyph->height = gm.gmBlackBoxY;
   glyph->adv_x = gm.gmCellIncX;
   glyph->adv_y = gm.gmCellIncY;
   if (glyph->width < 1) glyph->width = 1;
   if (glyph->height < 1) glyph->height = 1;
   glyph->pixels = calloc(1, glyph->width * glyph->height * sizeof(uint32_t));

   memset(&bi, 0, sizeof(BITMAPINFO));
   bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
   bi.bmiHeader.biWidth = glyph->width;
   bi.bmiHeader.biHeight = -glyph->height;
   bi.bmiHeader.biBitCount = 32;
   bi.bmiHeader.biPlanes = 1;

   hbmp = CreateDIBSection(temp_hdc, &bi, DIB_RGB_COLORS, (void **)&pixels, NULL, 0);
   if (!hbmp) {
      SelectObject(temp_hdc, prev_font);
      return glyph;
   }

   prev_hbmp = SelectObject(temp_hdc, hbmp);
   SetBkMode(temp_hdc, TRANSPARENT);
   SetTextColor(temp_hdc, 0xFFFFFF);
   SetTextAlign(temp_hdc, TA_BASELINE);
   if (c < 1 || (c >= 0xD800 && c < 0xE000) || c > 0x10FFFF) {
      buf[0] = 0xFFFD;
      buf_size = 1;
   }
   else if (c <= 0xFFFF) {
      buf[0] = c;
      buf_size = 1;
   }
   else {
      buf[0] = 0xD800 + ((c - 0x10000) >> 10);
      buf[1] = 0xDC00 + ((c - 0x10000) & 0x3FF);
      buf_size = 2;
   }
   TextOut(temp_hdc, -glyph->off_x, -glyph->off_y, buf, buf_size);
   SelectObject(temp_hdc, prev_hbmp);

   SelectObject(temp_hdc, prev_font);

   for (i=0; i<glyph->width*glyph->height; i++) {
      p = pixels[i];
      glyph->pixels[i] = (
         (gamma_table[256 + ((p >> 16) & 0xFF)] << 16) |
         (gamma_table[256 + ((p >>  8) & 0xFF)] << 8) |
         (gamma_table[256 + ((p >>  0) & 0xFF)] << 0)
      );
   }

   DeleteObject(hbmp);

   return glyph;
}


static void blit_char(Glyph *glyph, int x, int y, uint32_t color, uint32_t *pixels, int width, int height, int stride)
{
   int sx = 0, sy = 0;
   int w = glyph->width, h = glyph->height;
   int ca, cr, cg, cb, m, ma, mr, mg, mb, p, pa, pr, pg, pb;
   int i, j;

   if (x < 0) {
      w += x;
      sx -= x;
      x = 0;
   }
   if (y < 0) {
      h += y;
      sy -= y;
      y = 0;
   }
   if (x+w > width) {
      w = width-x;
   }
   if (y+h > height) {
      h = height-y;
   }
   if (w < 1 || h < 1) return;

   ca = (color >> 24);
   cr = (color >> 16) & 0xFF;
   cg = (color >>  8) & 0xFF;
   cb = color & 0xFF;
   if (ca != 0) {
      cr = cr * 255 / ca;
      cg = cg * 255 / ca;
      cb = cb * 255 / ca;
      if (cr > 255) cr = 255;
      if (cg > 255) cg = 255;
      if (cb > 255) cb = 255;
   }

   cr = gamma_table[256 + cr];
   cg = gamma_table[256 + cg];
   cb = gamma_table[256 + cb];

   for (i=0; i<h; i++) {
      for (j=0; j<w; j++) {
         m = glyph->pixels[(sy+i)*glyph->width+(sx+j)];
         if (m == 0) continue;

         mr = (m >> 16) & 0xFF;
         mg = (m >>  8) & 0xFF;
         mb = (m >>  0) & 0xFF;

         mr = mr * ca / 255;
         mg = mg * ca / 255;
         mb = mb * ca / 255;
         ma = MAX(MAX(mr, mg), mb);

         p = pixels[(y+i)*stride+(x+j)];
         pa = (p >> 24) & 0xFF;
         pr = (p >> 16) & 0xFF;
         pg = (p >>  8) & 0xFF;
         pb = (p >>  0) & 0xFF;
         if (pa != 0) {
            pr = pr * 255 / pa;
            pg = pg * 255 / pa;
            pb = pb * 255 / pa;
            if (pr > 255) pr = 255;
            if (pg > 255) pg = 255;
            if (pb > 255) pb = 255;
         }
         pr = gamma_table[256 + pr];
         pg = gamma_table[256 + pg];
         pb = gamma_table[256 + pb];

         pa = ma + (255 - ma)*pa/255;
         pr = (cr * mr / 255) + (pr * (255 - mr) / 255);
         pg = (cg * mg / 255) + (pg * (255 - mg) / 255);
         pb = (cb * mb / 255) + (pb * (255 - mb) / 255);

         pr = gamma_table[pr];
         pg = gamma_table[pg];
         pb = gamma_table[pb];

         pr = pr * pa / 255;
         pg = pg * pa / 255;
         pb = pb * pa / 255;

         pixels[(y+i)*stride+(x+j)] = (pa << 24) | (pr << 16) | (pg << 8) | pb;
      }
   }
}


int system_font_get_string_advance(SystemFont *font, plat_char *text)
{
   Glyph *glyph;
   uint16_t *s;
   int c, x=0;

   for (s=text; *s; s++) {
      c = *s;
      if (*s >= 0xD800 && *s < 0xDC00) {
         c = (*s - 0xD800) << 10;
         s++;
         if (*s >= 0xDC00 && *s < 0xE000) {
             c = 0x10000 + (c | (*s - 0xDC00));
         }
         else {
            c = 0xFFFD;
         }
         if (*s == 0) s--;
      }
      glyph = get_glyph(font, c);
      if (!glyph) continue;

      x += glyph->adv_x;
   }
   return x;
}


float system_font_get_string_position(SystemFont *font, plat_char *text, int x)
{
   Glyph *glyph;
   uint16_t *s;
   int c, i, adv = 0, prev = 0;
   float frac;
   
   if (x < 0) return 0.0f;

   for (s=text,i=0; *s; s++,i++) {
      c = *s;
      if (*s >= 0xD800 && *s < 0xDC00) {
         c = (*s - 0xD800) << 10;
         s++;
         if (*s >= 0xDC00 && *s < 0xE000) {
             c = 0x10000 + (c | (*s - 0xDC00));
         }
         else {
            c = 0xFFFD;
         }
         if (*s == 0) s--;
      }
      glyph = get_glyph(font, c);
      if (!glyph) continue;

      adv += glyph->adv_x;
      if (x >= prev && x < adv) {
         frac = (x - prev) / (float)(adv - prev);
         return i + frac;
      }
      prev = adv;
   }
   return i;
}


void system_font_draw_string(SystemFont *font, int x, int y, plat_char *text, uint32_t color, uint32_t *pixels, int width, int height, int stride)
{
   Glyph *glyph;
   uint16_t *s;
   int c;

   for (s=text; *s; s++) {
      c = *s;
      if (*s >= 0xD800 && *s < 0xDC00) {
         c = (*s - 0xD800) << 10;
         s++;
         if (*s >= 0xDC00 && *s < 0xE000) {
             c = 0x10000 + (c | (*s - 0xDC00));
         }
         else {
            c = 0xFFFD;
         }
         if (*s == 0) s--;
      }
      glyph = get_glyph(font, c);
      if (!glyph) continue;

      blit_char(glyph, x + glyph->off_x, y + glyph->off_y, color, pixels, width, height, stride);
      x += glyph->adv_x;
   }
}


static HICON create_icon(Heap *heap, Value image)
{
   HICON icon = NULL;
   ICONINFO ii;
   int width, height, stride;
   uint32_t *pixels, *bits, p;
   char *mask_bits;
   int i, j;
   int a, r, g, b;

   if (!fiximage_get_data(heap, image, &width, &height, &stride, &pixels, NULL, NULL)) {
      return NULL;
   }

   ii.fIcon = TRUE;

   bits = malloc(width*height*4);
   if (bits) {
      mask_bits = calloc(1, (width*height+7)/8);
      if (mask_bits) {
         for (i=0; i<height; i++) {
            for (j=0; j<width; j++) {
               p = pixels[i*stride+j];
               a = (p >> 24) & 0xFF;
               r = (p >> 16) & 0xFF;
               g = (p >>  8) & 0xFF;
               b = (p >>  0) & 0xFF;
               if (a != 0) {
                  r = r*255/a;
                  g = g*255/a;
                  b = b*255/a;
               }
               bits[i*width+j] = (a << 24) | (r << 16) | (g << 8) | b;
            }
         }
         ii.hbmColor = CreateBitmap(width, height, 1, 32, bits);
         if (ii.hbmColor) {
            ii.hbmMask = CreateBitmap(width, height, 1, 1, mask_bits);
            if (ii.hbmMask) {
               icon = CreateIconIndirect(&ii);
            }
            DeleteObject(ii.hbmMask);
         }
         DeleteObject(ii.hbmColor);
      }
      free(mask_bits);
   }
   free(bits);

   return icon;
}


NotifyIcon *notify_icon_create(Heap *heap, Value *images, int num_images, char **error_msg)
{
   NOTIFYICONDATA nid;
   NotifyIcon *icon, *ic;
   Value best_image = fixscript_int(0);
   int id, i, width, size, best_dist, abs_dist;

   icon = calloc(1, sizeof(NotifyIcon));
   if (!icon) return NULL;

   id = 0;
   ic = notify_icons;
   while (ic) {
      if (ic->id == id) {
         id++;
         ic = notify_icons;
         continue;
      }
      ic = ic->next;
   }

   memset(&nid, 0, sizeof(NOTIFYICONDATA));
   nid.cbSize = NOTIFYICONDATA_V2_SIZE;
   nid.hWnd = event_hwnd;
   nid.uID = id;
   nid.uFlags = NIF_MESSAGE | NIF_ICON;
   nid.uCallbackMessage = WM_USER+103;

   size = GetSystemMetrics(SM_CXSMICON);
   best_dist = INT_MAX;
   for (i=0; i<num_images; i++) {
      if (fiximage_get_data(heap, images[i], &width, NULL, NULL, NULL, NULL, NULL)) {
         abs_dist = abs(width - size);
         if (abs_dist <= best_dist) {
            if (abs_dist != best_dist || width >= size) {
               best_image = images[i];
            }
            best_dist = abs_dist;
         }
      }
   }
   if (best_image.value) {
      icon->icon = create_icon(heap, best_image);
      nid.hIcon = icon->icon;
   }
   if (!nid.hIcon) {
      nid.hIcon = LoadIcon(g_hInstance, MAKEINTRESOURCE(1));
   }

   if (!Shell_NotifyIcon(NIM_ADD, &nid)) {
      free(icon);
      return NULL;
   }

   icon->id = id;

   icon->next = notify_icons;
   notify_icons = icon;
   
   return icon;
}


void notify_icon_get_sizes(int **sizes, int *cnt)
{
   *cnt = 1;
   *sizes = calloc(*cnt, sizeof(int));
   (*sizes)[0] = GetSystemMetrics(SM_CXSMICON);
}


void notify_icon_destroy(NotifyIcon *icon)
{
   NOTIFYICONDATA nid;
   NotifyIcon *ic, **prev;

   memset(&nid, 0, sizeof(NOTIFYICONDATA));
   nid.cbSize = NOTIFYICONDATA_V2_SIZE;
   nid.hWnd = event_hwnd;
   nid.uID = icon->id;

   prev = &notify_icons;
   for (ic=notify_icons; ic; ic=ic->next) {
      if (ic == icon) {
         *prev = icon->next;
         Shell_NotifyIcon(NIM_DELETE, &nid);
         break;
      }
   }

   if (icon->icon) {
      DestroyIcon(icon->icon);
      icon->icon = NULL;
   }
}


int notify_icon_set_menu(NotifyIcon *icon, Menu *menu)
{
   if (menu && menu->menu) return 0;

   icon->menu = menu;
   return 1;
}


void io_notify()
{
   PostMessage(event_hwnd, WM_USER+104, 0, 0);
}


void post_to_main_thread(void *data)
{
   PostMessage(event_hwnd, WM_USER+105, 0, (LPARAM)data);
}


int modifiers_cmd_mask()
{
   return SCRIPT_MOD_CTRL | SCRIPT_MOD_CMD;
}


void quit_app()
{
   PostQuitMessage(0);
}


static Value func_windows_is_present(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   return fixscript_int(1);
}


static Value func_windows_set_default_menu_item(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Menu *menu;
   
   menu = menu_get_native(heap, error, params[0]);
   if (!menu) {
      return fixscript_int(0);
   }

   menu->default_item = params[1].value;
   if (menu->menu) {
      SetMenuDefaultItem(menu->menu, menu->default_item, TRUE);
   }

   return fixscript_int(0);
}


static Value func_windows_get_system_color(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   uint32_t value;
   int r, g, b;

   if (!GetSysColorBrush(params[0].value)) {
      return fixscript_int(0);
   }
   value = GetSysColor(params[0].value);
   r = (value >> 0) & 0xFF;
   g = (value >> 8) & 0xFF;
   b = (value >> 16) & 0xFF;
   return fixscript_int(0xFF000000 | (r << 16) | (g << 8) | b);
}


static Value func_windows_get_system_font(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   NONCLIENTMETRICS nc;
   LOGFONT *font = NULL;
   HFONT hfont;
   int size;

   memset(&nc, 0, sizeof(NONCLIENTMETRICS));
   nc.cbSize = sizeof(NONCLIENTMETRICS);

   if (!SystemParametersInfo(SPI_GETNONCLIENTMETRICS, sizeof(NONCLIENTMETRICS), &nc, 0)) {
      return fixscript_int(0);
   }

   switch (params[0].value) {
      case FONT_CAPTION:       font = &nc.lfCaptionFont; break;
      case FONT_SMALL_CAPTION: font = &nc.lfSmCaptionFont; break;
      case FONT_MENU:          font = &nc.lfMenuFont; break;
      case FONT_STATUS:        font = &nc.lfStatusFont; break;
      case FONT_MESSAGE:       font = &nc.lfMessageFont; break;
   }

   if (font) {
      hfont = CreateFontIndirect(font);
      if (hfont) {
         if (font->lfHeight < 0) {
            size = -font->lfHeight;
         }
         else {
            size = MulDiv(font->lfHeight, GetDeviceCaps(temp_hdc, LOGPIXELSY), 72);
         }
         return system_font_create_handle(heap, error, create_font(heap, hfont, size));
      }
   }
   return fixscript_int(0);
}


static int is_uxtheme_loaded()
{
   if (uxtheme_init < 0) {
      return 0;
   }
   if (uxtheme_init > 0) {
      return 1;
   }

   uxtheme_lib = LoadLibrary(L"uxtheme.dll");
   if (!uxtheme_lib) {
      uxtheme_init = -1;
      return 0;
   }
   uxtheme_OpenThemeData = (void *)GetProcAddress(uxtheme_lib, "OpenThemeData");
   uxtheme_CloseThemeData = (void *)GetProcAddress(uxtheme_lib, "CloseThemeData");
   uxtheme_GetThemeBool = (void *)GetProcAddress(uxtheme_lib, "GetThemeBool");
   uxtheme_GetThemeColor = (void *)GetProcAddress(uxtheme_lib, "GetThemeColor");
   uxtheme_GetThemeEnumValue = (void *)GetProcAddress(uxtheme_lib, "GetThemeEnumValue");
   uxtheme_GetThemeMargins = (void *)GetProcAddress(uxtheme_lib, "GetThemeMargins");
   uxtheme_GetThemePartSize = (void *)GetProcAddress(uxtheme_lib, "GetThemePartSize");
   uxtheme_GetThemePosition = (void *)GetProcAddress(uxtheme_lib, "GetThemePosition");
   uxtheme_DrawThemeBackground = (void *)GetProcAddress(uxtheme_lib, "DrawThemeBackground");
   uxtheme_init = 1;
   return 1;
}


static Value func_windows_get_theme_props(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   int type = (int)(intptr_t)data;
   HANDLE theme;
   uint16_t *str;
   Value ret = fixscript_int(0);
   int part = params[1].value;
   int state = params[2].value;
   int prop = params[3].value;
   BOOL bool_val;
   COLORREF color;
   int margins[4];
   LONG size[2];
   Value values[4];
   int i, err, val;

   if (!is_uxtheme_loaded()) {
      return fixscript_int(0);
   }

   err = fixscript_get_string_utf16(heap, params[0], 0, -1, (uint16_t **)&str, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   theme = uxtheme_OpenThemeData(NULL, str);
   free(str);
   if (!theme) {
      return fixscript_int(0);
   }

   switch (type) {
      case THEME_PROPS_BOOL:
         if (uxtheme_GetThemeBool(theme, part, state, prop, &bool_val) == 0) {
            ret = fixscript_int(bool_val != 0);
         }
         break;
         
      case THEME_PROPS_COLOR:
         if (uxtheme_GetThemeColor(theme, part, state, prop, &color) == 0) {
            ret = fixscript_int(color | 0xFF000000);
         }
         break;

      case THEME_PROPS_ENUM:
         if (uxtheme_GetThemeEnumValue(theme, part, state, prop, &val) == 0) {
            ret = fixscript_int(val);
         }
         break;

      case THEME_PROPS_MARGINS:
         if (uxtheme_GetThemeMargins(theme, NULL, part, state, prop, NULL, margins) == 0) {
            values[0] = fixscript_int(margins[0]);
            values[1] = fixscript_int(margins[2]);
            values[2] = fixscript_int(margins[1]);
            values[3] = fixscript_int(margins[3]);
            err = fixscript_set_array_range(heap, params[4], 0, 4, values);
            ret = fixscript_int(1);
         }
         break;

      case THEME_PROPS_SIZE:
         if (uxtheme_GetThemePartSize(theme, NULL, part, state, NULL, prop, size) == 0) {
            for (i=0; i<2; i++) {
               values[i] = fixscript_int(size[i]);
            }
            err = fixscript_set_array_range(heap, params[4], 0, 2, values);
            ret = fixscript_int(1);
         }
         break;

      case THEME_PROPS_POSITION:
         if (uxtheme_GetThemePosition(theme, part, state, prop, size) == 0) {
            for (i=0; i<2; i++) {
               values[i] = fixscript_int(size[i]);
            }
            err = fixscript_set_array_range(heap, params[4], 0, 2, values);
            ret = fixscript_int(1);
         }
         break;
   }

   //uxtheme_CloseThemeData(theme); - must not be called
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return ret;
}


static Value func_windows_get_theme_image(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   HANDLE theme;
   BITMAPINFO bi;
   HBITMAP hbmp, prev_hbmp;
   RECT rect, clip;
   uint16_t *str;
   int width, height;
   uint32_t *pixels, p;
   int i, j, a, a1, a2, a3, r1, g1, b1, r2, g2, b2;
   int err;

   if (!is_uxtheme_loaded()) {
      return fixscript_int(0);
   }

   err = fixscript_get_string_utf16(heap, params[6], 0, -1, (uint16_t **)&str, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   theme = uxtheme_OpenThemeData(NULL, str);
   free(str);
   if (!theme) {
      return fixscript_int(0);
   }

   width = params[0].value;
   height = params[1].value;
   if (width < 1 || height < 1) {
      //uxtheme_CloseThemeData(theme); - must not be called
      return fixscript_int(0);
   }

   memset(&bi, 0, sizeof(BITMAPINFO));
   bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
   bi.bmiHeader.biWidth = width;
   bi.bmiHeader.biHeight = -(height*2);
   bi.bmiHeader.biBitCount = 32;
   bi.bmiHeader.biPlanes = 1;

   hbmp = CreateDIBSection(temp_hdc, &bi, DIB_RGB_COLORS, (void **)&pixels, NULL, 0);
   if (!hbmp) {
      //uxtheme_CloseThemeData(theme); - must not be called
      return fixscript_int(0);
   }
   
   for (i=0; i<height; i++) {
      for (j=0; j<width; j++) {
         pixels[i*width+j] = 0xFF000000;
      }
   }
   for (i=height; i<height*2; i++) {
      for (j=0; j<width; j++) {
         pixels[i*width+j] = 0xFFFFFFFF;
      }
   }

   prev_hbmp = SelectObject(temp_hdc, hbmp);
   rect.left = params[2].value;
   rect.top = params[3].value;
   rect.right = rect.left + params[4].value;
   rect.bottom = rect.top + params[5].value;
   clip.left = 0;
   clip.top = 0;
   clip.right = width;
   clip.bottom = height;
   if ((err = uxtheme_DrawThemeBackground(theme, temp_hdc, params[7].value, params[8].value, &rect, &clip)) != 0) {
      SelectObject(temp_hdc, prev_hbmp);
      DeleteObject(hbmp);
      //uxtheme_CloseThemeData(theme); - must not be called
      return fixscript_int(0);
   }

   rect.top += height;
   rect.bottom += height;
   clip.top += height;
   clip.bottom += height;
   uxtheme_DrawThemeBackground(theme, temp_hdc, params[7].value, params[8].value, &rect, &clip);

   for (i=0; i<height; i++) {
      for (j=0; j<width; j++) {
         p = pixels[i*width+j];
         r1 = (p >> 16) & 0xFF;
         g1 = (p >>  8) & 0xFF;
         b1 = (p >>  0) & 0xFF;
         p = pixels[(i+height)*width+j];
         r2 = (p >> 16) & 0xFF;
         g2 = (p >>  8) & 0xFF;
         b2 = (p >>  0) & 0xFF;
         a = 255;
         if (r1 != r2 || g1 != g2 || b1 != b2) {
            // dest = dest * (1-a) + src * a
            //
            // dest1 = src * a
            // dest2 = 1 - a + src * a
            // a = 1 - (dest2 - dest1)

            a1 = 255 - (r2 - r1);
            a2 = 255 - (g2 - g1);
            a3 = 255 - (b2 - b1);
            a = MAX(a1, MAX(a2, a3));
         }
         pixels[i*width+j] = (a << 24) | (r1 << 16) | (g1 << 8) | b1;
      }
   }

   SelectObject(temp_hdc, prev_hbmp);
   //uxtheme_CloseThemeData(theme); - must not be called
   return fiximage_create_from_pixels(heap, width, height, width, pixels, free_hbmp, hbmp, -1);
}


static Value func_windows_register_theme_change_notify(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ThemeNotify *notify, **prev;
   int found = 0;

   if (!params[0].is_array) {
      return fixscript_int(0);
   }

   notify = theme_notify_funcs;
   prev = &theme_notify_funcs;
   while (notify) {
      if (notify->heap == heap && notify->func.value == params[0].value) {
         found = 1;
         break;
      }
      prev = &notify->next;
      notify = notify->next;
   }

   if (!found) {
      notify = calloc(1, sizeof(ThemeNotify));
      notify->heap = heap;
      notify->func = params[0];
      *prev = notify;
   }
   
   return fixscript_int(0);
}


static Value func_windows_get_window_handle(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;
   uint64_t ptr;
   
   view = view_get_native(heap, error, params[0], -1);
   if (!view) {
      return fixscript_int(0);
   }

   ptr = (uint64_t)(uintptr_t)view->hwnd;

   *error = fixscript_int((uint32_t)(ptr >> 32));
   return fixscript_int((uint32_t)ptr);
}


static Value func_windows_disable_painting(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;
   
   view = view_get_native(heap, error, params[0], TYPE_CANVAS);
   if (!view) {
      return fixscript_int(0);
   }

   view->canvas.disable_painting = 1;
   return fixscript_int(0);
}


static Value func_common_get_double_click_delay(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   return fixscript_int(GetDoubleClickTime());
}


static Value func_common_get_double_click_distance(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   int w, h;

   w = GetSystemMetrics(SM_CXDOUBLECLK);
   h = GetSystemMetrics(SM_CYDOUBLECLK);
   return fixscript_int(MAX(w, h)/2);
}


static Value func_common_get_cursor_blink_interval(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   uint32_t value;

   value = GetCaretBlinkTime();
   if (value <= 0 || value == INFINITE) {
      value = 0;
   }
   return fixscript_int(value);
}


void register_platform_gui_functions(Heap *heap)
{
   fixscript_register_native_func(heap, "common_get_double_click_delay#0", func_common_get_double_click_delay, NULL);
   fixscript_register_native_func(heap, "common_get_double_click_distance#0", func_common_get_double_click_distance, NULL);
   fixscript_register_native_func(heap, "common_get_cursor_blink_interval#0", func_common_get_cursor_blink_interval, NULL);

   fixscript_register_native_func(heap, "windows_is_present#0", func_windows_is_present, NULL);
   fixscript_register_native_func(heap, "windows_set_default_menu_item#2", func_windows_set_default_menu_item, NULL);
   fixscript_register_native_func(heap, "windows_get_system_color#1", func_windows_get_system_color, NULL);
   fixscript_register_native_func(heap, "windows_get_system_font#1", func_windows_get_system_font, NULL);
   fixscript_register_native_func(heap, "windows_get_theme_bool#4", func_windows_get_theme_props, (void *)THEME_PROPS_BOOL);
   fixscript_register_native_func(heap, "windows_get_theme_color#4", func_windows_get_theme_props, (void *)THEME_PROPS_COLOR);
   fixscript_register_native_func(heap, "windows_get_theme_enum#4", func_windows_get_theme_props, (void *)THEME_PROPS_ENUM);
   fixscript_register_native_func(heap, "windows_get_theme_margins#5", func_windows_get_theme_props, (void *)THEME_PROPS_MARGINS);
   fixscript_register_native_func(heap, "windows_get_theme_size#5", func_windows_get_theme_props, (void *)THEME_PROPS_SIZE);
   fixscript_register_native_func(heap, "windows_get_theme_position#5", func_windows_get_theme_props, (void *)THEME_PROPS_POSITION);
   fixscript_register_native_func(heap, "windows_get_theme_image#9", func_windows_get_theme_image, NULL);
   fixscript_register_native_func(heap, "windows_register_theme_change_notify#1", func_windows_register_theme_change_notify, NULL);
   fixscript_register_native_func(heap, "windows_get_window_handle#1", func_windows_get_window_handle, NULL);
   fixscript_register_native_func(heap, "windows_disable_painting#1", func_windows_disable_painting, NULL);
}


static int get_current_key_modifiers()
{
   int mod = 0;
   if (GetKeyState(VK_CONTROL) & 0x8000) mod |= SCRIPT_MOD_CTRL | SCRIPT_MOD_CMD;
   if (GetKeyState(VK_MENU) & 0x8000) mod |= SCRIPT_MOD_ALT;
   if (GetKeyState(VK_SHIFT) & 0x8000) mod |= SCRIPT_MOD_SHIFT;
   return mod;
}


static int handle_common_events(View *view, UINT msg, WPARAM wParam, LPARAM lParam)
{
   switch (msg) {
      case WM_MOUSEMOVE:
      case WM_LBUTTONDOWN:
      case WM_MBUTTONDOWN:
      case WM_RBUTTONDOWN:
      case WM_LBUTTONUP:
      case WM_MBUTTONUP:
      case WM_RBUTTONUP: {
         int x = (int16_t)LOWORD(lParam);
         int y = (int16_t)HIWORD(lParam);
         int btn = -1, mod = 0;
         if (view->common.type == TYPE_CANVAS) {
            x += GetScrollPos(view->hwnd, SB_HORZ);
            y += GetScrollPos(view->hwnd, SB_VERT);
         }
         switch (msg) {
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
               btn = MOUSE_BUTTON_LEFT;
               break;
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:
               btn = MOUSE_BUTTON_MIDDLE;
               break;
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
               btn = MOUSE_BUTTON_RIGHT;
               break;
         }
         if (wParam & MK_CONTROL) mod |= SCRIPT_MOD_CTRL | SCRIPT_MOD_CMD;
         if (wParam & MK_SHIFT) mod |= SCRIPT_MOD_SHIFT;
         if (GetKeyState(VK_MENU) & 0x8000) mod |= SCRIPT_MOD_ALT;
         if (wParam & MK_LBUTTON) mod |= SCRIPT_MOD_LBUTTON;
         if (wParam & MK_MBUTTON) mod |= SCRIPT_MOD_MBUTTON;
         if (wParam & MK_RBUTTON) mod |= SCRIPT_MOD_RBUTTON;
         switch (msg) {
            case WM_MOUSEMOVE: {
               RECT rect;
               int cx, cy;
               int type = (GetCapture() == view->hwnd? EVENT_MOUSE_DRAG : EVENT_MOUSE_MOVE);
               if (hover_view != view && view->common.parent) {
                  call_mouse_event_callback(view, EVENT_MOUSE_ENTER, x, y, btn, mod, 0, 0);
                  hover_view = view;
               }
               if (type == EVENT_MOUSE_DRAG && (wParam & (MK_LBUTTON | MK_MBUTTON | MK_RBUTTON)) == 0) {
                  type = EVENT_MOUSE_MOVE;
               }
               if (relative_view == view) {
                  type = EVENT_MOUSE_RELATIVE;
                  GetWindowRect(view->hwnd, &rect);
                  cx = (rect.right - rect.left) / 2;
                  cy = (rect.bottom - rect.top) / 2;
                  x = x - cx;
                  y = y - cy;
                  if (ignore_relative_event) {
                     x = 0;
                     y = 0;
                     ignore_relative_event = 0;
                  }
                  if (x == 0 && y == 0) {
                     return 0;
                  }
                  SetCursorPos(rect.left + cx, rect.top + cy);
               }
               call_mouse_event_callback(view, type, x, y, btn, mod, 0, 0);
               return 0;
            }

            case WM_LBUTTONDOWN:
            case WM_MBUTTONDOWN:
            case WM_RBUTTONDOWN: {
               uint32_t time = timeGetTime();
               POINT p;
               p.x = x;
               p.y = y;
               MapWindowPoints(view->hwnd, HWND_DESKTOP, (LPPOINT)&p, 2);
               int rx = abs(p.x - view->last_click_x);
               int ry = abs(p.y - view->last_click_y);
               if (rx <= 2 && ry <= 2 && time - view->last_click_time <= GetDoubleClickTime()) {
                  view->last_click_count++;
               }
               else {
                  view->last_click_count = 1;
               }
               view->last_click_time = time;
               view->last_click_x = p.x;
               view->last_click_y = p.y;
               SetCapture(view->hwnd);
               return call_mouse_event_callback(view, EVENT_MOUSE_DOWN, x, y, btn, mod, view->last_click_count, 0);
            }

            case WM_LBUTTONUP:
            case WM_MBUTTONUP:
            case WM_RBUTTONUP: {
               RECT rect;
               POINT p;
               int ret;
               ret = call_mouse_event_callback(view, EVENT_MOUSE_UP, x, y, btn, mod, 0, 0);
               if ((wParam & (MK_LBUTTON | MK_MBUTTON | MK_RBUTTON)) == 0) {
                  ReleaseCapture();
                  if (view->common.type == TYPE_WINDOW) {
                     GetClientRect(view->hwnd, &rect);
                     p.x = (int16_t)LOWORD(lParam);
                     p.y = (int16_t)HIWORD(lParam);
                     if (!PtInRect(&rect, p) || ChildWindowFromPoint(view->hwnd, p) != view->hwnd) {
                        call_mouse_event_callback(view, EVENT_MOUSE_LEAVE, 0, 0, 0, 0, 0, 0);
                        hover_view = NULL;
                     }
                  }
               }
               return ret;
            }
         }
         break;
      }

      case WM_MOUSEWHEEL:
      case WM_MOUSEHWHEEL: {
         POINT p;
         int mod = 0;
         float wheel_x = 0.0f, wheel_y = 0.0f;
         p.x = (int16_t)LOWORD(lParam);
         p.y = (int16_t)HIWORD(lParam);
         MapWindowPoints(HWND_DESKTOP, view->hwnd, &p, 1);
         if (view->common.type == TYPE_CANVAS) {
            p.x += GetScrollPos(view->hwnd, SB_HORZ);
            p.y += GetScrollPos(view->hwnd, SB_VERT);
         }
         if (wParam & MK_CONTROL) mod |= SCRIPT_MOD_CTRL | SCRIPT_MOD_CMD;
         if (wParam & MK_SHIFT) mod |= SCRIPT_MOD_SHIFT;
         if (GetKeyState(VK_MENU) & 0x8000) mod |= SCRIPT_MOD_ALT;
         if (wParam & MK_LBUTTON) mod |= SCRIPT_MOD_LBUTTON;
         if (wParam & MK_MBUTTON) mod |= SCRIPT_MOD_MBUTTON;
         if (wParam & MK_RBUTTON) mod |= SCRIPT_MOD_RBUTTON;
         if (msg == WM_MOUSEWHEEL) {
            wheel_y = (int16_t)HIWORD(wParam) / -120.0f;
         }
         else {
            wheel_x = (int16_t)HIWORD(wParam) / -120.0f;
         }
         return call_mouse_wheel_callback(view, p.x, p.y, mod, wheel_x, wheel_y, 0, 0);
      }

      case WM_KEYDOWN:
      case WM_KEYUP:
      case WM_SYSKEYDOWN:
      case WM_SYSKEYUP: {
         int key = KEY_NONE;
         switch (wParam) {
            case VK_ESCAPE:             key = KEY_ESCAPE; break;
            case VK_F1:                 key = KEY_F1; break;
            case VK_F2:                 key = KEY_F2; break;
            case VK_F3:                 key = KEY_F3; break;
            case VK_F4:                 key = KEY_F4; break;
            case VK_F5:                 key = KEY_F5; break;
            case VK_F6:                 key = KEY_F6; break;
            case VK_F7:                 key = KEY_F7; break;
            case VK_F8:                 key = KEY_F8; break;
            case VK_F9:                 key = KEY_F9; break;
            case VK_F10:                key = KEY_F10; break;
            case VK_F11:                key = KEY_F11; break;
            case VK_F12:                key = KEY_F12; break;
            case VK_SNAPSHOT:           key = KEY_PRINT_SCREEN; break;
            case VK_SCROLL:             key = KEY_SCROLL_LOCK; break;
            case VK_PAUSE:              key = KEY_PAUSE; break;
            case 0xC0:                  key = KEY_GRAVE; break;
            case '1':                   key = KEY_NUM1; break;
            case '2':                   key = KEY_NUM2; break;
            case '3':                   key = KEY_NUM3; break;
            case '4':                   key = KEY_NUM4; break;
            case '5':                   key = KEY_NUM5; break;
            case '6':                   key = KEY_NUM6; break;
            case '7':                   key = KEY_NUM7; break;
            case '8':                   key = KEY_NUM8; break;
            case '9':                   key = KEY_NUM9; break;
            case '0':                   key = KEY_NUM0; break;
            case VK_OEM_MINUS:          key = KEY_MINUS; break;
            case 0xBB:                  key = KEY_EQUAL; break;
            case VK_BACK:               key = KEY_BACKSPACE; break;
            case VK_TAB:                key = KEY_TAB; break;
            case 'Q':                   key = KEY_Q; break;
            case 'W':                   key = KEY_W; break;
            case 'E':                   key = KEY_E; break;
            case 'R':                   key = KEY_R; break;
            case 'T':                   key = KEY_T; break;
            case 'Y':                   key = KEY_Y; break;
            case 'U':                   key = KEY_U; break;
            case 'I':                   key = KEY_I; break;
            case 'O':                   key = KEY_O; break;
            case 'P':                   key = KEY_P; break;
            case 0xDB:                  key = KEY_LBRACKET; break;
            case 0xDD:                  key = KEY_RBRACKET; break;
            case 0xDC:                  key = KEY_BACKSLASH; break;
            case VK_CAPITAL:            key = KEY_CAPS_LOCK; break;
            case 'A':                   key = KEY_A; break;
            case 'S':                   key = KEY_S; break;
            case 'D':                   key = KEY_D; break;
            case 'F':                   key = KEY_F; break;
            case 'G':                   key = KEY_G; break;
            case 'H':                   key = KEY_H; break;
            case 'J':                   key = KEY_J; break;
            case 'K':                   key = KEY_K; break;
            case 'L':                   key = KEY_L; break;
            case 0xBA:                  key = KEY_SEMICOLON; break;
            case 0xDE:                  key = KEY_APOSTROPHE; break;
            case VK_RETURN:             key = KEY_ENTER; break;
            case VK_SHIFT:              key = KEY_LSHIFT; break;
            case 'Z':                   key = KEY_Z; break;
            case 'X':                   key = KEY_X; break;
            case 'C':                   key = KEY_C; break;
            case 'V':                   key = KEY_V; break;
            case 'B':                   key = KEY_B; break;
            case 'N':                   key = KEY_N; break;
            case 'M':                   key = KEY_M; break;
            case VK_OEM_COMMA:          key = KEY_COMMA; break;
            case VK_OEM_PERIOD:         key = KEY_PERIOD; break;
            case 0xBF:                  key = KEY_SLASH; break;
            //case VK_RSHIFT:             key = KEY_RSHIFT; break;
            case VK_CONTROL:            key = KEY_LCONTROL; break;
            case VK_LWIN:               key = KEY_LMETA; break;
            case VK_MENU:               key = KEY_LALT; break;
            case VK_SPACE:              key = KEY_SPACE; break;
            //case VK_RALT:               key = KEY_RALT; break;
            case VK_RWIN:               key = KEY_RMETA; break;
            case 0x5D:                  key = KEY_RMENU; break;
            //case VK_RCONTROL:           key = KEY_RCONTROL; break;
            case VK_INSERT:             key = KEY_INSERT; break;
            case VK_DELETE:             key = KEY_DELETE; break;
            case VK_HOME:               key = KEY_HOME; break;
            case VK_END:                key = KEY_END; break;
            case VK_PRIOR:              key = KEY_PAGE_UP; break;
            case VK_NEXT:               key = KEY_PAGE_DOWN; break;
            case VK_LEFT:               key = KEY_LEFT; break;
            case VK_UP:                 key = KEY_UP; break;
            case VK_RIGHT:              key = KEY_RIGHT; break;
            case VK_DOWN:               key = KEY_DOWN; break;
            case VK_NUMLOCK:            key = KEY_NUM_LOCK; break;
            case 0x6F:                  key = KEY_NUMPAD_SLASH; break;
            case 0x6A:                  key = KEY_NUMPAD_STAR; break;
            case 0x6D:                  key = KEY_NUMPAD_MINUS; break;
            case 0x6B:                  key = KEY_NUMPAD_PLUS; break;
            //case VK_NUMPAD_ENTER:       key = KEY_NUMPAD_ENTER; break;
            case 0x6E:                  key = KEY_NUMPAD_DOT; break;
            case VK_NUMPAD0:            key = KEY_NUMPAD0; break;
            case VK_NUMPAD1:            key = KEY_NUMPAD1; break;
            case VK_NUMPAD2:            key = KEY_NUMPAD2; break;
            case VK_NUMPAD3:            key = KEY_NUMPAD3; break;
            case VK_NUMPAD4:            key = KEY_NUMPAD4; break;
            case VK_NUMPAD5:            key = KEY_NUMPAD5; break;
            case VK_NUMPAD6:            key = KEY_NUMPAD6; break;
            case VK_NUMPAD7:            key = KEY_NUMPAD7; break;
            case VK_NUMPAD8:            key = KEY_NUMPAD8; break;
            case VK_NUMPAD9:            key = KEY_NUMPAD9; break;
         }
         return call_key_event_callback(view, (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)? EVENT_KEY_DOWN : EVENT_KEY_UP, key, get_current_key_modifiers());
      }

      case WM_CHAR: {
         if (wParam >= 32 && wParam <= 0xFFFF && (wParam < 0xD800 || wParam >= 0xE000)) {
            int mods = get_current_key_modifiers();
            wchar_t c[2];
            c[0] = wParam;
            c[1] = 0;
            if (mods & (SCRIPT_MOD_CTRL | SCRIPT_MOD_ALT)) {
               break;
            }
            return call_key_typed_event_callback(view, c, mods);
         }
         break;
      }

      case WM_SETFOCUS: {
         focus_view = view;
         call_focus_event_callback(view, EVENT_FOCUS_GAINED, focus_type);
         break;
      }

      case WM_KILLFOCUS: {
         call_focus_event_callback(view, EVENT_FOCUS_LOST, FOCUS_NORMAL);
         break;
      }
   }
   return 0;
}


static void notify_theme_changed()
{
   ThemeNotify *notify = theme_notify_funcs;
   Value error;

   while (notify) {
      fixscript_call(notify->heap, notify->func, 0, &error);
      if (error.value) {
         fprintf(stderr, "error while running theme change callback:\n");
         fixscript_dump_value(notify->heap, error, 1);
      }
      notify = notify->next;
   }
}


static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
   View *view = (View *)GetWindowLongPtr(hwnd, 0);
   View *v;

   if (view && handle_common_events(view, msg, wParam, lParam)) {
      return 0;
   }

   switch (msg) {
      case WM_CREATE: {
         new_window_view->hwnd = hwnd;
         SetWindowLongPtr(hwnd, 0, (LONG_PTR)new_window_view);
         // TODO: this is needed to update themes if the event was missed (in case no window was open)
         notify_theme_changed();
         break;
      }

      case WM_DESTROY: {
         call_view_callback(view, CALLBACK_WINDOW_DESTROY);
         SetWindowLongPtr(hwnd, 0, (LONG_PTR)NULL);
         break;
      }

      case WM_CLOSE: {
         call_view_callback(view, CALLBACK_WINDOW_CLOSE);
         break;
      }

      case WM_GETMINMAXINFO: {
         MINMAXINFO *mmi = (MINMAXINFO *)lParam;
         RECT rect;
         int border_x, border_y;
         int min_width, min_height;

         rect.left = 0;
         rect.top = 0;
         rect.right = 0;
         rect.bottom = 0;
         AdjustWindowRectEx(&rect, GetWindowLong(hwnd, GWL_STYLE), 0, GetWindowLong(hwnd, GWL_EXSTYLE));
         border_x = rect.right - rect.left;
         border_y = rect.bottom - rect.top;

         min_width = mmi->ptMinTrackSize.x - border_x;
         min_height = mmi->ptMinTrackSize.y - border_y;

         if (view) {
            min_width = MAX(min_width, view->window.min_width);
            min_height = MAX(min_height, view->window.min_height);
         }

         mmi->ptMinTrackSize.x = min_width + border_x;
         mmi->ptMinTrackSize.y = min_height + border_y;
         break;
      }

      case WM_SIZE: {
         if (wParam == SIZE_MAXIMIZED) {
            view->window.maximized = 1;
         }
         else if (wParam == SIZE_MINIMIZED || wParam == SIZE_RESTORED) {
            view->window.maximized = 0;
         }
         if (view->window.status_hwnd) {
            SendMessage(view->window.status_hwnd, WM_SIZE, 0, 0);
         }
         call_view_callback(view, CALLBACK_WINDOW_RESIZE);
         break;
      }

      case WM_ACTIVATE: {
         if (LOWORD(wParam) == WA_ACTIVE || LOWORD(wParam) == WA_CLICKACTIVE) {
            if (view->window.last_focus) {
               SetFocus(view->window.last_focus);
            }
            call_view_callback(view, CALLBACK_WINDOW_ACTIVATE);
         }
         else {
            view->window.last_focus = GetFocus();
         }
         break;
      }

      case WM_COMMAND: {
         if (HIWORD(wParam) == BN_CLICKED) {
            for (v = view->common.first_child; v; v = v->common.next) {
               if (v->hwnd == (HWND)lParam) {
                  call_action_callback(v, CALLBACK_BUTTON_ACTION);
                  break;
               }
            }
         }
         break;
      }

      case WM_MENUCOMMAND: {
         MENUINFO info;
         Menu *menu;

         memset(&info, 0, sizeof(MENUINFO));
         info.cbSize = sizeof(MENUINFO);
         info.fMask = MIM_MENUDATA;
         if (!GetMenuInfo((HMENU)lParam, &info)) break;
         menu = (Menu *)info.dwMenuData;

         call_menu_callback(menu, wParam);
         break;
      }

      case WM_SETCURSOR: {
         for (v = view->common.first_child; v; v = v->common.next) {
            if (v->hwnd == (HWND)wParam && v->cursor != CURSOR_DEFAULT) {
               SetCursor(cursors[v->cursor]);
               return 1;
            }
         }
         return DefWindowProc(hwnd, msg, wParam, lParam);
      }

      case WM_THEMECHANGED: {
         notify_theme_changed();
         break;
      }

      default: {
         return DefWindowProc(hwnd, msg, wParam, lParam);
      }
   }
   return 0;
}


static void handle_scroll(HWND hwnd, int dir, int action)
{
   SCROLLINFO si;
   int old_pos, new_pos;

   si.cbSize = sizeof(SCROLLINFO);
   si.fMask = SIF_PAGE | SIF_POS | SIF_RANGE | SIF_TRACKPOS;
   GetScrollInfo(hwnd, dir, &si);

   old_pos = si.nPos;

   switch (action) {
      case SB_TOP:           new_pos = si.nMin; break;
      case SB_BOTTOM:        new_pos = si.nMax; break;
      case SB_LINEUP:        new_pos = si.nPos - 40; break;
      case SB_LINEDOWN:      new_pos = si.nPos + 40; break;
      case SB_PAGEUP:        new_pos = si.nPos - si.nPage; break;
      case SB_PAGEDOWN:      new_pos = si.nPos + si.nPage; break;
      case SB_THUMBTRACK:    new_pos = si.nTrackPos; break;
      default:
      case SB_THUMBPOSITION: new_pos = si.nPos; break;
   }
   
   SetScrollPos(hwnd, dir, new_pos, TRUE);
   new_pos = GetScrollPos(hwnd, dir);

   if (dir == SB_HORZ) {
      ScrollWindowEx(hwnd, old_pos - new_pos, 0, NULL, NULL, NULL, NULL, SW_ERASE | SW_INVALIDATE);
   }
   else {
      ScrollWindowEx(hwnd, 0, old_pos - new_pos, NULL, NULL, NULL, NULL, SW_ERASE | SW_INVALIDATE);
   }
}


static void handle_scroll_wheel(HWND hwnd, int dir, int delta)
{
   int old_pos, new_pos;

   old_pos = GetScrollPos(hwnd, dir);
   new_pos = old_pos + delta * 40 / 120;
   SetScrollPos(hwnd, dir, new_pos, TRUE);
   new_pos = GetScrollPos(hwnd, dir);

   if (dir == SB_HORZ) {
      ScrollWindowEx(hwnd, old_pos - new_pos, 0, NULL, NULL, NULL, NULL, SW_ERASE | SW_INVALIDATE);
   }
   else {
      ScrollWindowEx(hwnd, 0, old_pos - new_pos, NULL, NULL, NULL, NULL, SW_ERASE | SW_INVALIDATE);
   }
}


static void draw_canvas(Heap *heap, View *view, HDC hdc, RECT rect, int scroll_x, int scroll_y)
{
   BITMAPINFO bi;
   HBITMAP hbmp, prev_hbmp;
   RECT rect2;
   Value image, painter, error;
   uint32_t *pixels;
   int width, height;
   int canvas_width, canvas_height;
   int ol, ot, or, ob;

   ol = view->canvas.overdraw;
   ot = view->canvas.overdraw;
   or = view->canvas.overdraw;
   ob = view->canvas.overdraw;

   GetClientRect(view->hwnd, &rect2);
   canvas_width = rect2.right - rect2.left + view->canvas.scroll[0].max;
   canvas_height = rect2.bottom - rect2.top + view->canvas.scroll[1].max;

   if (rect.left + scroll_x < ol) {
      ol = rect.left + scroll_x;
   }
   if (rect.top + scroll_y < ot) {
      ot = rect.top + scroll_y;
   }
   if (rect.right + scroll_x + or > canvas_width) {
      or = canvas_width - (rect.right + scroll_x);
   }
   if (rect.bottom + scroll_y + ob > canvas_height) {
      ob = canvas_height - (rect.bottom + scroll_y);
   }

   width = rect.right - rect.left + ol + or;
   height = rect.bottom - rect.top + ot + ob;

   memset(&bi, 0, sizeof(BITMAPINFO));
   bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
   bi.bmiHeader.biWidth = width;
   bi.bmiHeader.biHeight = -height;
   bi.bmiHeader.biBitCount = 32;
   bi.bmiHeader.biPlanes = 1;

   hbmp = CreateDIBSection(temp_hdc, &bi, DIB_RGB_COLORS, (void **)&pixels, NULL, 0);
   if (hbmp) {
      image = fiximage_create_from_pixels(heap, width, height, width, pixels, free_hbmp, hbmp, -1);
      if (!image.value) {
         fprintf(stderr, "error while painting:\n");
         fixscript_error(heap, &error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         fixscript_dump_value(heap, error, 1);
      }
      else {
         painter = fiximage_create_painter(heap, image, -rect.left - scroll_x + ol, -rect.top - scroll_y + ot);
         if (!painter.value) {
            fprintf(stderr, "error while painting:\n");
            fixscript_error(heap, &error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
            fixscript_dump_value(heap, error, 1);
         }
         else {
            call_view_callback_with_value(view, CALLBACK_CANVAS_PAINT, painter);
         }
         prev_hbmp = SelectObject(temp_hdc, hbmp);
         BitBlt(hdc, rect.left, rect.top, width - ol - or, height - ot - ob, temp_hdc, ol, ot, SRCCOPY);
         SelectObject(temp_hdc, prev_hbmp);
      }
   }
}


static LRESULT CALLBACK CanvasProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
   View *view = (View *)GetWindowLongPtr(hwnd, 0);
   Heap *heap = view? view->common.heap : NULL;
   PAINTSTRUCT ps;
   HDC hdc;
   RECT rect, r;
   int width, height;
   int scroll_x, scroll_y;
   View *v;

   if (view && handle_common_events(view, msg, wParam, lParam)) {
      return 0;
   }

   switch (msg) {
      case WM_CREATE: {
         new_window_view->hwnd = hwnd;
         SetWindowLongPtr(hwnd, 0, (LONG_PTR)new_window_view);
         break;
      }

      case WM_DESTROY: {
         call_view_callback(view, CALLBACK_CANVAS_DESTROY);
         canvas_set_active_rendering(view, 0);
         if (relative_view == view) {
            canvas_set_relative_mode(view, 0);
         }
         SetWindowLongPtr(hwnd, 0, (LONG_PTR)NULL);
         break;
      }

      case WM_HSCROLL: {
         if ((view->canvas.flags & CANVAS_SCROLLABLE) == 0) break;
         handle_scroll(hwnd, SB_HORZ, LOWORD(wParam));
         update_canvas_subviews(view);
         break;
      }

      case WM_VSCROLL: {
         if ((view->canvas.flags & CANVAS_SCROLLABLE) == 0) break;
         handle_scroll(hwnd, SB_VERT, LOWORD(wParam));
         update_canvas_subviews(view);
         break;
      }

      case WM_KEYDOWN: {
         if ((view->canvas.flags & CANVAS_SCROLLABLE) == 0) break;
         switch (wParam) {
            case VK_HOME:  handle_scroll(hwnd, SB_VERT, SB_TOP); break;
            case VK_END:   handle_scroll(hwnd, SB_VERT, SB_BOTTOM); break;
            case VK_UP:    handle_scroll(hwnd, SB_VERT, SB_LINEUP); break;
            case VK_DOWN:  handle_scroll(hwnd, SB_VERT, SB_LINEDOWN); break;
            case VK_PRIOR: handle_scroll(hwnd, SB_VERT, SB_PAGEUP); break;
            case VK_NEXT:  handle_scroll(hwnd, SB_VERT, SB_PAGEDOWN); break;
            case VK_LEFT:  handle_scroll(hwnd, SB_HORZ, SB_LINEUP); break;
            case VK_RIGHT: handle_scroll(hwnd, SB_HORZ, SB_LINEDOWN); break;
         }
         update_canvas_subviews(view);
         break;
      }

      case WM_LBUTTONDOWN:
      case WM_MBUTTONDOWN:
      case WM_RBUTTONDOWN:
      case WM_XBUTTONDOWN: {
         if (view->canvas.focusable) {
            SetFocus(hwnd);
         }
         break;
      }

      case WM_MOUSEWHEEL: {
         if (view->canvas.focusable) {
            SetFocus(hwnd);
         }
         if ((view->canvas.flags & CANVAS_SCROLLABLE) == 0) break;
         handle_scroll_wheel(hwnd, SB_VERT, -(int16_t)HIWORD(wParam));
         update_canvas_subviews(view);
         break;
      }

      case WM_MOUSEHWHEEL: {
         if (view->canvas.focusable) {
            SetFocus(hwnd);
         }
         if ((view->canvas.flags & CANVAS_SCROLLABLE) == 0) break;
         handle_scroll_wheel(hwnd, SB_HORZ, (int16_t)HIWORD(wParam));
         update_canvas_subviews(view);
         break;
      }

      case WM_SIZE: {
         update_canvas_subviews(view);
         call_view_callback(view, CALLBACK_CANVAS_RESIZE);
         break;
      }
      
      case WM_PAINT: {
         scroll_x = GetScrollPos(hwnd, SB_HORZ);
         scroll_y = GetScrollPos(hwnd, SB_VERT);

         hdc = BeginPaint(hwnd, &ps);

         width = ps.rcPaint.right - ps.rcPaint.left;
         height = ps.rcPaint.bottom - ps.rcPaint.top;
         if (width <= 0 || height <= 0) {
            EndPaint(hwnd, &ps);
            break;
         }
         if (!canvas_get_active_rendering(view)) {
            draw_canvas(heap, view, hdc, ps.rcPaint, scroll_x, scroll_y);
         }

         EndPaint(hwnd, &ps);
         break;
      }

      case WM_COMMAND: {
         if (HIWORD(wParam) == BN_CLICKED) {
            for (v = view->common.first_child; v; v = v->common.next) {
               if (v->hwnd == (HWND)lParam) {
                  call_action_callback(v, CALLBACK_BUTTON_ACTION);
                  break;
               }
            }
         }
         break;
      }

      case WM_CTLCOLORBTN: {
         GetClientRect((HWND)lParam, &rect);
         //FillRect((HDC)wParam, &rect, GetStockObject(WHITE_BRUSH));
         scroll_x = GetScrollPos(hwnd, SB_HORZ);
         scroll_y = GetScrollPos(hwnd, SB_VERT);
         GetWindowRect((HWND)lParam, &r);
         MapWindowPoints(HWND_DESKTOP, GetParent((HWND)lParam), (LPPOINT)&r, 2);
         draw_canvas(heap, view, (HDC)wParam, rect, scroll_x + r.left, scroll_y + r.top);
         return (LRESULT)GetStockObject(NULL_BRUSH);
      }

      case WM_MOUSEMOVE: {
         TRACKMOUSEEVENT track;
         track.cbSize = sizeof(TRACKMOUSEEVENT);
         track.dwFlags = TME_LEAVE;
         track.hwndTrack = hwnd;
         track.dwHoverTime = 0;
         TrackMouseEvent(&track);
         break;
      }

      case WM_MOUSELEAVE: {
         call_mouse_event_callback(view, EVENT_MOUSE_LEAVE, 0, 0, 0, 0, 0, 0);
         hover_view = NULL;
         break;
      }

      default: {
         return DefWindowProc(hwnd, msg, wParam, lParam);
      }
   }
   return 0;
}


static void run_timers()
{
   View *view;
   Timer *timer;
   uint32_t time;

   for (view = active_canvases; view; view = cur_next_active_canvas) {
      cur_next_active_canvas = view->canvas.next_active;
      canvas_handle_active_rendering(view);
   }

   time = timeGetTime();
   for (timer = active_timers; timer; timer = cur_next_timer) {
      cur_next_timer = timer->next;
      if (timer->interval == 0 || time >= timer->next_time) {
         timer->next_time = time + timer->interval;
         timer_run(timer->heap, timer->instance);
      }
   }
}


static LRESULT CALLBACK EventProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
   switch (msg) {
      case WM_USER+101: {
         Worker *worker = (Worker *)lParam;
         worker->common.notify_func(worker);
         return 0;
      }

      case WM_USER+102: {
         EnterCriticalSection(&timer_section);
         run_timers();
         SetEvent(timers_processed_event);
         LeaveCriticalSection(&timer_section);
         return 0;
      }

      case WM_USER+103: {
         NotifyIcon *icon;
         for (icon=notify_icons; icon; icon=icon->next) {
            if (icon->id == wParam) {
               switch (lParam) {
                  case WM_LBUTTONUP:
                     call_notify_icon_click_callback(icon);
                     break;

                  case WM_RBUTTONUP: {
                     POINT pt;
                     if (icon->menu && !icon->menu->menu) {
                        menu_real_create(icon->menu, 1);
                        GetCursorPos(&pt);
                        cur_popup_menu = icon->menu;
                        SetForegroundWindow(event_hwnd);
                        TrackPopupMenu(icon->menu->menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, event_hwnd, NULL);
                     }
                     break;
                  }
               }
               break;
            }
         }
         return 0;
      }

      case WM_USER+104: {
         io_process();
         return 0;
      }

      case WM_USER+105: {
         run_in_main_thread((void *)lParam);
         return 0;
      }

      case WM_EXITMENULOOP: {
         if (cur_popup_menu) {
            DestroyMenu(cur_popup_menu->menu);
            update_menu_after_destroying(cur_popup_menu);
         }
         break;
      }

      case WM_MENUCOMMAND: {
         if (cur_popup_menu) {
            call_menu_callback(cur_popup_menu, wParam);
         }
         break;
      }
      
      default: {
         return DefWindowProc(hwnd, msg, wParam, lParam);
      }
   }
   return 0;
}


static void attach_console()
{
   void *lib;
   BOOL WINAPI (*func_AttachConsole)(DWORD);
   HANDLE handle;
   DWORD type;
   int stdout_redir=0, stderr_redir=0;

   handle = GetStdHandle(STD_OUTPUT_HANDLE);
   if (handle) {
      type = GetFileType(handle);
      stdout_redir = (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE);
   }

   handle = GetStdHandle(STD_ERROR_HANDLE);
   if (handle) {
      type = GetFileType(handle);
      stderr_redir = (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE);
   }

   if (stdout_redir || stderr_redir) {
      return;
   }

   lib = LoadLibrary(L"kernel32.dll");
   if (!lib) return;

   func_AttachConsole = (void *)GetProcAddress(lib, "AttachConsole");
   if (!func_AttachConsole) {
      return;
   }

   if (!func_AttachConsole(-1)) {
      return;
   }

   freopen("CONOUT$", "w", stdout);
   freopen("CONOUT$", "w", stderr);
   setvbuf(stdout, NULL, _IONBF, 0);
   setvbuf(stderr, NULL, _IONBF, 0);
   printf("\n");
   fflush(stdout);

   console_hwnd = GetConsoleWindow();
}


static void finish_console()
{
   if (console_hwnd) {
      PostMessage(console_hwnd, WM_KEYUP, VK_RETURN, 0);
   }
}


int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
   WNDCLASSEX wc;
   NONCLIENTMETRICS ncm;
   HFONT prev_font;
   TEXTMETRIC tm;
   SIZE size;
   MSG msg;
   HWND hwnd;
   POINT mouse;
   OSVERSIONINFO os;
   HANDLE thread;
   RECT rect;
   int i, contrast, mod, len;
   int argc;
   char **argv;
   wchar_t **wargv;

   attach_console();

   g_hInstance = hInstance;

   InitCommonControls();

   memset(&wc, 0, sizeof(WNDCLASSEX));
   wc.cbSize = sizeof(WNDCLASSEX);
   wc.style = 0;
   wc.lpfnWndProc = WindowProc;
   wc.cbClsExtra = 0;
   wc.cbWndExtra = sizeof(View *);
   wc.hInstance = hInstance;
   wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(1));
   wc.hCursor = LoadCursor(NULL, IDC_ARROW);
   wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE+1);
   wc.lpszMenuName = NULL;
   wc.lpszClassName = L"TopLevelWindow";
   wc.hIconSm = NULL;

   if (!RegisterClassEx(&wc)) {
      MessageBox(NULL, L"Window Registration Failed!", L"Error!", MB_ICONEXCLAMATION | MB_OK);
      return 0;
   }

   memset(&wc, 0, sizeof(WNDCLASSEX));
   wc.cbSize = sizeof(WNDCLASSEX);
   wc.style = CS_HREDRAW | CS_VREDRAW;
   wc.lpfnWndProc = CanvasProc;
   wc.cbWndExtra = sizeof(View *);
   wc.hCursor = LoadCursor(NULL, IDC_ARROW);
   wc.lpszClassName = L"Canvas";

   if (!RegisterClassEx(&wc)) {
      MessageBox(NULL, L"Window Registration Failed!", L"Error!", MB_ICONEXCLAMATION | MB_OK);
      return 0;
   }

   memset(&wc, 0, sizeof(WNDCLASSEX));
   wc.cbSize = sizeof(WNDCLASSEX);
   wc.lpfnWndProc = EventProc;
   wc.lpszClassName = L"EventReceiveWindow";

   if (!RegisterClassEx(&wc)) {
      MessageBox(NULL, L"Window Registration Failed!", L"Error!", MB_ICONEXCLAMATION | MB_OK);
      return 0;
   }

   event_hwnd = CreateWindowEx(0, L"EventReceiveWindow", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, NULL);
   if (!event_hwnd) {
      MessageBox(NULL, L"Event Window Creation Failed!", L"Error!", MB_ICONEXCLAMATION | MB_OK);
      return 0;
   }

   module = GetModuleHandle(NULL);

   temp_hdc = CreateCompatibleDC(NULL);
   ncm.cbSize = sizeof(NONCLIENTMETRICS);
   if (SystemParametersInfo(SPI_GETNONCLIENTMETRICS, sizeof(NONCLIENTMETRICS), &ncm, 0)) {
      //ncm.lfMessageFont.lfHeight = (ncm.lfMessageFont.lfHeight * 13 + 9) / 10;
      default_font = CreateFontIndirect(&ncm.lfMessageFont);
   }
   else {
      default_font = GetStockObject(DEFAULT_GUI_FONT);
   }

   prev_font = SelectObject(temp_hdc, default_font);
   GetTextMetrics(temp_hdc, &tm);
   GetTextExtentPoint32(temp_hdc, L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz", 52, &size);
   base_unit_x = (size.cx / 26 + 1) / 2;
   base_unit_y = tm.tmHeight;
   SelectObject(temp_hdc, prev_font);

   cursors[CURSOR_DEFAULT] = NULL;
   cursors[CURSOR_ARROW] = LoadCursor(NULL, IDC_ARROW);
   cursors[CURSOR_EMPTY] = NULL;
   cursors[CURSOR_TEXT] = LoadCursor(NULL, IDC_IBEAM);
   cursors[CURSOR_CROSS] = LoadCursor(NULL, IDC_CROSS);
   cursors[CURSOR_HAND] = LoadCursor(NULL, IDC_HAND);
   cursors[CURSOR_MOVE] = LoadCursor(NULL, IDC_SIZEALL);
   cursors[CURSOR_RESIZE_N] = LoadCursor(NULL, IDC_SIZENS);
   cursors[CURSOR_RESIZE_NE] = LoadCursor(NULL, IDC_SIZENESW);
   cursors[CURSOR_RESIZE_E] = LoadCursor(NULL, IDC_SIZEWE);
   cursors[CURSOR_RESIZE_SE] = LoadCursor(NULL, IDC_SIZENWSE);
   cursors[CURSOR_RESIZE_S] = LoadCursor(NULL, IDC_SIZENS);
   cursors[CURSOR_RESIZE_SW] = LoadCursor(NULL, IDC_SIZENESW);
   cursors[CURSOR_RESIZE_W] = LoadCursor(NULL, IDC_SIZEWE);
   cursors[CURSOR_RESIZE_NW] = LoadCursor(NULL, IDC_SIZENWSE);
   cursors[CURSOR_WAIT] = LoadCursor(NULL, IDC_WAIT);

   os.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
   if (GetVersionEx(&os) != 0 && ((os.dwMajorVersion > 5) || (os.dwMajorVersion == 5 && os.dwMinorVersion >= 1))) {
      SystemParametersInfo(SPI_GETFONTSMOOTHING, 0, &i, 0);
      if (i != 0) {
         SystemParametersInfo(SPI_GETFONTSMOOTHINGTYPE, 0, &i, 0);
         if (i == FE_FONTSMOOTHINGCLEARTYPE) {
            use_cleartype = 1;
         }
      }
   }

   if (use_cleartype) {
      SystemParametersInfo(SPI_GETFONTSMOOTHINGCONTRAST, 0, &contrast, 0);
      if (contrast < 1000 || contrast > 2200) contrast = 1400;
      for (i=0; i<256; i++) {
         gamma_table[i] = roundf(powf(i/255.0f, 1000.0f/contrast)*255.0f);
         gamma_table[i+256] = roundf(powf(i/255.0f, contrast/1000.0f)*255.0f);
      }
   }
   else {
      for (i=0; i<256; i++) {
         gamma_table[i] = roundf(powf(i/255.0f, 1.0f/2.3f)*255.0f);
         gamma_table[i+256] = roundf(powf(i/255.0f, 2.3f)*255.0f);
      }
   }
   
   InitializeCriticalSection(&timer_section);
   timer_event = CreateEvent(NULL, FALSE, FALSE, NULL);
   timers_processed_event = CreateEvent(NULL, FALSE, FALSE, NULL);
   thread = CreateThread(NULL, 0, timer_thread, NULL, 0, NULL);
   if (!thread) {
      MessageBox(NULL, L"Thread Creation Failed!", L"Error!", MB_ICONEXCLAMATION | MB_OK);
      return 0;
   }
   CloseHandle(thread);

   wargv = CommandLineToArgvW(GetCommandLine(), &argc);
   argv = calloc(argc, sizeof(char *));
   for (i=0; i<argc; i++) {
      len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
      argv[i] = malloc(len+1);
      WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, argv[i], len, NULL, NULL);
      argv[i][len] = '\0';
   }
   LocalFree(wargv);

   if (!app_main(argc, argv)) return 0;

   for (;;) {
      if (relative_view) {
         HWND active_window = GetActiveWindow();
         int cx, cy, found = 0;
         
         hwnd = relative_view->hwnd;
         while (hwnd) {
            if (hwnd == active_window) {
               found = 1;
               break;
            }
            hwnd = GetParent(hwnd);
         }
         if (found) {
            GetWindowRect(relative_view->hwnd, &rect);
            if (ignore_relative_event == 2) {
               relative_has_pos = GetCursorPos(&relative_prev_pos);
               cx = (rect.right - rect.left) / 2;
               cy = (rect.bottom - rect.top) / 2;
               SetCursorPos(rect.left + cx, rect.top + cy);
               ignore_relative_event = 1;
            }
            ClipCursor(&rect);
         }
         else {
            ClipCursor(NULL);
            if (relative_has_pos) {
               SetCursorPos(relative_prev_pos.x, relative_prev_pos.y);
               relative_has_pos = 0;
            }
            ignore_relative_event = 2;
         }
      }

      EnterCriticalSection(&timer_section);
      if ((active_canvases || active_fast_timers) && !PeekMessage(&msg, NULL, 0, 0, PM_NOREMOVE)) {
         run_timers();
         LeaveCriticalSection(&timer_section);
         continue;
      }
      LeaveCriticalSection(&timer_section);
      if (GetMessage(&msg, NULL, 0, 0) <= 0) {
         break;
      }
      EnterCriticalSection(&timer_section);
      if ((active_canvases || active_fast_timers) && msg.message == WM_USER+102 && msg.hwnd == event_hwnd) {
         SetEvent(timers_processed_event);
         LeaveCriticalSection(&timer_section);
         continue;
      }
      LeaveCriticalSection(&timer_section);

      if (msg.message == WM_MOUSEWHEEL || msg.message == WM_MOUSEHWHEEL) {
         GetCursorPos(&mouse);
         hwnd = WindowFromPoint(mouse);
         if (hwnd && GetAncestor(hwnd, GA_ROOT) == GetActiveWindow()) {
            msg.hwnd = hwnd;
            DispatchMessage(&msg);
         }
         continue;
      }

      if (msg.message == WM_SYSKEYDOWN || msg.message == WM_SYSKEYUP) {
         TranslateMessage(&msg);
         DispatchMessage(&msg);
         continue;
      }
      
      if ((msg.message == WM_KEYDOWN || msg.message == WM_KEYUP) && (msg.wParam == VK_LEFT || msg.wParam == VK_RIGHT || msg.wParam == VK_UP || msg.wParam == VK_DOWN || msg.wParam == VK_RETURN || msg.wParam == VK_ESCAPE)) {
         TranslateMessage(&msg);
         DispatchMessage(&msg);
         continue;
      }
      
      focus_type = FOCUS_NORMAL;
      if (msg.message == WM_KEYDOWN && msg.wParam == VK_TAB) {
         mod = get_current_key_modifiers();
         focus_type = (mod & SCRIPT_MOD_SHIFT)? FOCUS_PREV : FOCUS_NEXT;
         if (focus_view && focus_view->hwnd && focus_view->hwnd == GetFocus()) {
            if (call_key_event_callback(focus_view, EVENT_KEY_DOWN, KEY_TAB, mod)) {
               continue;
            }
         }
      }
      if (msg.message != WM_CHAR) {
         hwnd = GetActiveWindow();
         if (hwnd && IsDialogMessage(hwnd, &msg)) {
            focus_type = FOCUS_NORMAL;
            continue;
         }
      }
      focus_type = FOCUS_NORMAL;

      TranslateMessage(&msg);
      DispatchMessage(&msg);
   }

   finish_console();
   return 0;
}
