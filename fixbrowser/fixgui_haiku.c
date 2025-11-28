/*
 * FixScript GUI v0.8 (modified) - https://www.fixscript.org/
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
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <dlfcn.h>
#include "fixgui_common.h"

enum {
   MSG_INIT_APP,
   MSG_ASYNC_MSG_RESULT,
   MSG_WINDOW_RESIZED,
   MSG_WINDOW_CLOSE,
   MSG_DRAW_CANVAS,
   MSG_CANVAS_MOUSE_EVENT,
   MSG_CANVAS_MOUSE_WHEEL_EVENT,
   MSG_CANVAS_KEY_EVENT,
   MSG_CANVAS_KEY_TYPED_EVENT,
   MSG_BUTTON_CLICKED,
   MSG_MENU_ITEM_ACTION,
   MSG_POPUP_MENU_DELETED,
   MSG_POPUP_ACTION,
   MSG_POPUP_MENU,
   MSG_PROCESS_TIMERS,
   MSG_WORKER_NOTIFY
};

enum {
   FIXVALUE_TYPE = 'fxvl'
};

struct View {
   ViewCommon common;
   void *view;
   void *locked_looper;
   union {
      struct {
         void *win;
         void *contents;
         int created;
      } window;
      struct {
         void *canvas;
         void *scroll;
         int flags;
      } canvas;
   };
};

struct Menu {
   MenuCommon common;
   void *menu;
};

struct Worker {
   WorkerCommon common;
   pthread_mutex_t mutex;
   pthread_cond_t cond;
};

typedef struct Timer {
   Heap *heap;
   Value instance;
   int interval;
   uint32_t next_time;
   struct Timer *next;
} Timer;

struct NotifyIcon {
   NotifyIconCommon common;
};

struct SystemFont {
   void *font;
};

void fixgui__tls_init();
static pthread_condattr_t cond_attr;
static pthread_mutex_t mutex;
static pthread_cond_t cond;
static void *script_looper;
static void **fixwindow_vtable = NULL;
static void **fixwindowview_vtable = NULL;
static void (*fixcanvas_orig_message_received)(void *view, void *msg);
static void **fixcanvas_vtable = NULL;
static void **fixpopupmenu_vtable = NULL;
static void *app;
static pthread_mutex_t timer_mutex;
static pthread_cond_t timer_cond;
static pthread_cond_t timer_cond_processed;
static Timer *active_timers = NULL;

#define VTABLE(ptr, slot) (((void **)(*(void **)ptr))[slot])

#if defined(__LP64__)

#define BAPPLICATION_SIZE 368
#define BLOOPER_SIZE 240
#define BMESSAGE_SIZE 112
#define BWINDOW_SIZE 504
#define BVIEW_SIZE 272
#define BSTRINGVIEW_SIZE 296
#define BTEXTCONTROL_SIZE 440
#define BBUTTON_SIZE 392
#define BBUTTON_BINVOKER_OFFSET 272
#define BSCROLLVIEW_SIZE 320
#define BBITMAP_SIZE 96
#define BPOPUPMENU_SIZE 496
#define BMENUBAR_SIZE 504
#define BMENUITEM_SIZE 160
#define BMENUITEM_BINVOKER_OFFSET 16
#define BSEPARATORITEM_SIZE 168
#define BALERT_SIZE 608
#define BINVOKER_SIZE 64
#define BFONT_SIZE 48

#else

#define BAPPLICATION_SIZE 264
#define BLOOPER_SIZE 172
#define BMESSAGE_SIZE 72
#define BWINDOW_SIZE 376
#define BVIEW_SIZE 176
#define BSTRINGVIEW_SIZE 196
#define BTEXTCONTROL_SIZE 312
#define BBUTTON_SIZE 276
#define BBUTTON_BINVOKER_OFFSET 176
#define BSCROLLVIEW_SIZE 212
#define BBITMAP_SIZE 76
#define BPOPUPMENU_SIZE 340
#define BMENUBAR_SIZE 344
#define BMENUITEM_SIZE 128
#define BMENUITEM_BINVOKER_OFFSET 12
#define BSEPARATORITEM_SIZE 132
#define BALERT_SIZE 432
#define BINVOKER_SIZE 52
#define BFONT_SIZE 48

#endif

#define B_NORMAL_PRIORITY 10
#define B_LOOPER_PORT_DEFAULT_CAPACITY 200

enum {
   B_WIDTH_AS_USUAL,
   B_WIDTH_FROM_WIDEST,
   B_WIDTH_FROM_LABEL
};

enum {
   B_EMPTY_ALERT,
   B_INFO_ALERT,
   B_IDEA_ALERT,
   B_WARNING_ALERT,
   B_STOP_ALERT
};

enum {
   B_NOT_MOVABLE                     = 0x00000001,
   B_NOT_CLOSABLE                    = 0x00000020,
   B_NOT_ZOOMABLE                    = 0x00000040,
   B_NOT_MINIMIZABLE                 = 0x00004000,
   B_NOT_RESIZABLE                   = 0x00000002,
   B_NOT_H_RESIZABLE                 = 0x00000004,
   B_NOT_V_RESIZABLE                 = 0x00000008,
   B_AVOID_FRONT                     = 0x00000080,
   B_AVOID_FOCUS                     = 0x00002000,
   B_WILL_ACCEPT_FIRST_CLICK         = 0x00000010,
   B_OUTLINE_RESIZE                  = 0x00001000,
   B_NO_WORKSPACE_ACTIVATION         = 0x00000100,
   B_NOT_ANCHORED_ON_ACTIVATE        = 0x00020000,
   B_ASYNCHRONOUS_CONTROLS           = 0x00080000,
   B_QUIT_ON_WINDOW_CLOSE            = 0x00100000,
   B_SAME_POSITION_IN_ALL_WORKSPACES = 0x00200000,
   B_AUTO_UPDATE_SIZE_LIMITS         = 0x00400000,
   B_CLOSE_ON_ESCAPE                 = 0x00800000,
   B_NO_SERVER_SIDE_WINDOW_MODIFIERS = 0x00000200
};

enum {
   B_UNTYPED_WINDOW  = 0,
   B_TITLED_WINDOW   = 1,
   B_MODAL_WINDOW    = 3,
   B_DOCUMENT_WINDOW = 11,
   B_BORDERED_WINDOW = 20,
   B_FLOATING_WINDOW = 21
};

enum {
   B_CURRENT_WORKSPACE = 0
};

enum {
   B_FOLLOW_NONE       = 0,
   B_FOLLOW_ALL_SIDES  = 0x1234,
   B_FOLLOW_LEFT_TOP   = 0x1212,
   B_FOLLOW_LEFT_RIGHT = 0x0204,
   B_FOLLOW_TOP        = 0x1010
};

enum {
   B_NAVIGABLE             = 1 << 25,
   B_FRAME_EVENTS          = 1 << 26,
   B_WILL_DRAW             = 1 << 29,
   B_FULL_UPDATE_ON_RESIZE = 1 << 31
};

enum {
   B_PANEL_BACKGROUND_COLOR = 1
};

#define B_NO_TINT 1.0f

enum {
   B_OK = 0
};

enum {
   B_PLAIN_BORDER,
   B_FANCY_BORDER,
   B_NO_BORDER
};

enum {
   B_RGB32  = 0x0008,
   B_RGBA32 = 0x2008
};

enum {
   B_HORIZONTAL,
   B_VERTICAL
};

enum {
   B_ITEMS_IN_ROW = 0,
   B_ITEMS_IN_COLUMN,
   B_ITEMS_IN_MATRIX
};

enum {
   B_EVEN_SPACING = 0,
   B_OFFSET_SPACING
};

enum {
   B_FONT_ALL = 0x000001FF
};

enum {
   B_MOUSE_DOWN          = '_MDN',
   B_MOUSE_WHEEL_CHANGED = '_MWC',
   B_KEY_DOWN            = '_KYD',
   B_KEY_UP              = '_KYU',
};

enum {
   B_STRING_TYPE         = 'CSTR',
   B_MIME_TYPE           = 'MIME'
};

enum {
   B_POINTER_EVENTS = 0x00000001
};

enum {
   B_ENTERED_VIEW = 0,
   B_INSIDE_VIEW,
   B_EXITED_VIEW,
   B_OUTSIDE_VIEW
};

enum {
   B_LOCK_WINDOW_FOCUS    = 0x00000001,
   B_SUSPEND_WINDOW_FOCUS = 0x00000002,
   B_NO_POINTER_HISTORY   = 0x00000004,
   B_FULL_POINTER_HISTORY = 0x00000008
};

typedef uint8_t bool;
typedef int32_t status_t;
typedef int32_t thread_id;

#define B_FONT_FAMILY_LENGTH 63
typedef char font_family[B_FONT_FAMILY_LENGTH+1];

typedef struct {
   void **vtable;
   uint32_t what;
} BMessage;

typedef struct {
   float left;
   float top;
   float right;
   float bottom;
} BRect;

typedef struct {
   float x;
   float y;
} BPoint;

typedef struct {
   uint8_t red;
   uint8_t green;
   uint8_t blue;
   uint8_t alpha;
} rgb_color;

typedef struct {
   float ascent;
   float descent;
   float leading;
} font_height;

typedef struct BClipboard BClipboard;

static void *(*operator_new)(unsigned long);
static void (*operator_delete)(void *p);

static void (*BApplication_new)(void *, const char *);
static void (*BLooper_new)(void *, const char *, int32_t, int32_t);
static status_t (*BLooper_PostMessage)(void *, void *);
static status_t (*BLooper_PostMessage_id)(void *, uint32_t);
static void (*BLooper_MessageReceived)(void *, void *);
static inline thread_id BLooper_Run(void *obj) { return ((thread_id (*)(void *))VTABLE(obj, 19))(obj); }
static bool (*BLooper_Lock)(void *);
static void (*BLooper_Unlock)(void *);
static void *(*BLooper_CurrentMessage)(void *);
static void (*BMessage_new)(void *, uint32_t);
static status_t (*BMessage_AddInt32)(void *, const char *, int32_t);
static status_t (*BMessage_AddPointer)(void *, const char *, const void *);
static status_t (*BMessage_AddBool)(void *, const char *, bool);
static status_t (*BMessage_AddData)(void *, const char *, uint32_t, const void *, ssize_t, bool, int32_t);
static int32_t (*BMessage_GetInt32)(void *, const char *, int32_t);
static void *(*BMessage_GetPointer)(void *, const char *, const void *);
static bool (*BMessage_GetBool)(void *, const char *, bool);
static status_t (*BMessage_FindData)(void *, const char *, uint32_t, void **, ssize_t *);
static status_t (*BMessage_FindPoint)(void *, const char *, BPoint *);
static status_t (*BMessage_FindFloat)(void *, const char *, float *);
static void (*BMessage_PrintToStream)(void *);
static void (*BWindow_new)(void *, BRect *, const char *, int, uint32_t, uint32_t);
static inline void BWindow_Show(void *obj) { ((void (*)(void *))VTABLE(obj, 43))(obj); }
static inline void BWindow_Hide(void *obj) { ((void (*)(void *))VTABLE(obj, 44))(obj); }
#ifdef __LP64__
static BRect *(*BWindow_Bounds)(BRect *, void *);
#else
static BRect (*BWindow_Bounds)(void *);
#endif
static void (*BWindow_AddChild)(void *, void *, void *);
static void (*BWindow_CenterOnScreen)(void *);
static void (*BWindow_Zoom)(void *);
static inline void BWindow_Minimize(void *obj, bool b) { ((void (*)(void *, bool))VTABLE(obj, 37))(obj, b); }
static inline void BWindow_Quit(void *obj) { ((void (*)(void *))VTABLE(obj, 20))(obj); }
#ifdef __LP64__
static BRect *(*BWindow_Frame)(BRect *, void *);
#else
static BRect (*BWindow_Frame)(void *);
#endif
static const char *(*BWindow_Title)(void *);
static void (*BWindow_SetTitle)(void *, const char *);
static void (*BWindow_SetSizeLimits)(void *, float, float, float, float);
static void *(*BHandler_Looper)(void *);
static bool (*BHandler_LockLooper)(void *);
static void (*BHandler_UnlockLooper)(void *);
static void (*BView_new)(void *, BRect *, const char *, uint32_t, uint32_t);
static void (*BView_SetViewUIColor)(void *, int, float);
#ifdef __LP64__
static BRect *(*BView_Bounds)(BRect *, void *);
static BRect *(*BView_Frame)(BRect *, void *);
#else
static BRect (*BView_Bounds)(void *);
static BRect (*BView_Frame)(void *);
#endif
static void (*BView_MoveTo)(void *, float, float);
static void (*BView_ResizeTo)(void *, float, float);
static void (*BView_AddChild)(void *, void *, void *);
static inline void BView_MakeFocus(void *obj, bool b) { ((void (*)(void *, bool))VTABLE(obj, 43))(obj, b); }
static bool (*BView_IsFocus)(void *);
static inline void BView_SetViewColor(void *obj, rgb_color c) { ((void (*)(void *, rgb_color))VTABLE(obj, 36))(obj, c); }
static void (*BView_DrawBitmap)(void *, void *, BRect *);
static inline void BView_SetFlags(void *obj, uint32_t f) { ((void (*)(void *, uint32_t))VTABLE(obj, 40))(obj, f); }
static uint32_t (*BView_Flags)(void *);
static status_t (*BView_SetMouseEventMask)(void *, uint32_t mask, uint32_t options);
static uint32_t (*modifiers)();
static void (*BView_Invalidate)(void *);
static void (*BView_Invalidate_rect)(void *, BRect *);
static void (*BView_ConvertToScreen)(void *, BPoint *);
static inline void BView_SetHighColor(void *obj, rgb_color p) { ((void (*)(void *, rgb_color))VTABLE(obj, 37))(obj, p); }
static inline void BView_SetFont(void *obj, void *p1, uint32_t p2) { ((void (*)(void *, void *, uint32_t))VTABLE(obj, 39))(obj, p1, p2); }
static void (*BView_DrawString)(void *, const char *, BPoint *, void *);
static void (*BView_Sync)(void *);
static void *(*BView_Window)(void *);
static void (*BBitmap_new)(void *, BRect *, int, bool, bool);
#ifdef __LP64__
static BRect *(*BBitmap_Bounds)(BRect *, void *);
#else
static BRect (*BBitmap_Bounds)(void *);
#endif
static int32_t (*BBitmap_BytesPerRow)(void *);
static void *(*BBitmap_Bits)(void *);
static bool (*BBitmap_Lock)(void *);
static void (*BBitmap_Unlock)(void *);
static inline void BBitmap_AddChild(void *obj, void *p) { ((void (*)(void *, void *))VTABLE(obj, 7))(obj, p); }
static inline bool BBitmap_RemoveChild(void *obj, void *p) { return ((bool (*)(void *, void *))VTABLE(obj, 8))(obj, p); }
static void (*BStringView_new)(void *, BRect *, const char *, const char *, uint32_t, uint32_t);
static const char *(*BStringView_Text)(void *);
static void (*BStringView_SetText)(void *, const char *);
static void (*BTextControl_new)(void *, BRect *, const char *, const char *, const char *, void *, uint32_t, uint32_t);
static const char *(*BTextControl_Text)(void *);
static inline void BTextControl_SetText(void *obj, const char *s) { ((void (*)(void *, const char *))VTABLE(obj, 72))(obj, s); }
static void (*BButton_new)(void *, BRect *, const char *, const char *, void *, uint32_t, uint32_t);
static inline void BButton_SetTarget(void *obj, void *p1, void *p2) { ((void (*)(void *, void *, void *))VTABLE(obj, 81))(obj + BBUTTON_BINVOKER_OFFSET, p1, p2); }
static inline void BButton_SetLabel(void *obj, const char *s) { ((void (*)(void *, const char *))VTABLE(obj, 64))(obj, s); }
static const char *(*BControl_Label)(void *);
static void (*BScrollView_new)(void *, const char *, void *, uint32_t, bool, bool, int);
static void *(*BScrollView_ScrollBar)(void *, int);
static void (*BScrollBar_SetRange)(void *, float, float);
static void (*BScrollBar_SetValue)(void *, float);
static void (*BScrollBar_SetProportion)(void *, float);
static void (*BScrollBar_SetSteps)(void *, float, float);
static float (*BScrollBar_Value)(void *);
static void (*BPopUpMenu_new)(void *, const char *, bool, bool, int);
static void (*BPopUpMenu_SetAsyncAutoDestruct)(void *, bool);
static void *(*BPopUpMenu_Go)(void *, BPoint *, bool, bool, bool);
static void (*BMenuBar_new)(void *, BRect *, const char *, uint32_t, int, bool);
static bool (*BMenu_AddItem_menu)(void *, void *);
static bool (*BMenu_AddItem_menu_idx)(void *, void *, int32_t);
static bool (*BMenu_AddItem_item)(void *, void *);
static bool (*BMenu_AddItem_item_idx)(void *, void *, int32_t);
static bool (*BMenu_AddSeparatorItem)(void *);
static void *(*BMenu_RemoveItem)(void *, int32_t);
static void (*BMenuItem_new)(void *, const char *, void *, char, uint32_t);
static inline void BMenuItem_SetTarget(void *obj, void *p1, void *p2) { ((void (*)(void *, void *, void *))VTABLE(obj, 27))(obj + BMENUITEM_BINVOKER_OFFSET, p1, p2); }
static void (*BSeparatorItem_new)(void *);
static void (*BAlert_new)(void *, const char *, const char *, const char *, const char *, const char *, int, int, int);
static int32_t (*BAlert_Go)(void *);
static status_t (*BAlert_Go_invoker)(void *, void *);
static void (*BInvoker_new)(void *, void *, void *, void *);
static inline void BApplication_Quit(void *obj) { ((void (*)(void *))VTABLE(obj, 20))(obj); }
static void (*BFont_new)(void *);
static status_t (*BFont_SetFamilyAndStyle)(void *, const char *, const char *);
static void (*BFont_SetSize)(void *, float);
static float (*BFont_Size)(void *);
static void (*BFont_GetHeight)(void *, font_height *);
static float (*BFont_StringWidth)(void *, const char *);
static rgb_color *B_TRANSPARENT_COLOR;
static int32_t (*count_font_families)();
static status_t (*get_font_family)(int32_t index, font_family *name, uint32_t flags);
static BClipboard *be_clipboard;
static bool (*BClipboard_Lock)(void *);
static void (*BClipboard_Unlock)(void *);
static status_t (*BClipboard_Clear)(void *);
static BMessage *(*BClipboard_Data)(void *);
static status_t (*BClipboard_Commit)(void *);

typedef struct {
   uint8_t data[BWINDOW_SIZE];
   View *view;
} FixWindow;

typedef struct {
   uint8_t data[BVIEW_SIZE];
   View *view;
} FixWindowView;

typedef struct {
   uint8_t data[BVIEW_SIZE];
   View *view;
   int last_x, last_y;
   int last_buttons;
} FixCanvas;

typedef struct {
   uint8_t data[BPOPUPMENU_SIZE];
   void (*orig_destructor)(void *);
   Menu *menu;
} FixPopUpMenu;


static int pthread_cond_timedwait_relative(pthread_cond_t *cond, pthread_mutex_t *mutex, int64_t timeout)
{
   struct timespec ts;

   if (timeout == 0) return 0;
   if (timeout < 0) {
      return pthread_cond_wait(cond, mutex);
   }

   clock_gettime(CLOCK_MONOTONIC, &ts);
   ts.tv_nsec += timeout % 1000000000;
   ts.tv_sec += ts.tv_nsec / 1000000000 + timeout / 1000000000;
   ts.tv_nsec %= 1000000000;
   return pthread_cond_timedwait(cond, mutex, &ts);
}


static inline void *new(size_t size)
{
   return operator_new(size);
}


static inline void delete_static(void *p)
{
   operator_delete(p);
}


static inline void delete_virtual(void *p)
{
   void (*destructor)(void *p) = VTABLE(p, 1);
   destructor(p);
}


static void **clone_vtable(void *obj, int offset, int num_entries)
{
   void **vtable = (*(void ***)obj) - offset;
   void **new_vtable;

   new_vtable = malloc(num_entries * sizeof(void *));
   memcpy(new_vtable, vtable, num_entries * sizeof(void *));
   return new_vtable;
}


static inline uint32_t div255(uint32_t a)
{
   return ((a << 8) + a + 255) >> 16;
}


static void AddValue(void *msg, const char *name, Value value)
{
   BMessage_AddData(msg, name, FIXVALUE_TYPE, &value, sizeof(Value), 1, 1);
}


static Value GetValue(void *msg, const char *name)
{
   void *data;
   ssize_t num_bytes;

   if (BMessage_FindData(msg, name, FIXVALUE_TYPE, &data, &num_bytes) != B_OK) {
      return fixscript_int(0);
   }
   if (num_bytes != sizeof(Value)) {
      return fixscript_int(0);
   }
   return *(Value *)data;
}


static void prepare_event_finish(void *msg)
{
   int *event_finished = (int *)calloc(1, sizeof(int));

   BMessage_AddPointer(msg, "event_finished", event_finished);
}


static void notify_event_finish(void *msg)
{
   int *event_finished = BMessage_GetPointer(msg, "event_finished", NULL);
   
   pthread_mutex_lock(&mutex);
   *event_finished = 1;
   pthread_cond_broadcast(&cond);
   pthread_mutex_unlock(&mutex);
}


static void wait_event_finish(void *msg)
{
   int *event_finished = BMessage_GetPointer(msg, "event_finished", NULL);

   pthread_mutex_lock(&mutex);
   while (*event_finished == 0) {
      pthread_cond_wait(&cond, &mutex);
   }
   pthread_mutex_unlock(&mutex);

   free(event_finished);
}


static void view_lock(View *view)
{
   void *looper;

   if (view->common.type == TYPE_WINDOW) {
      looper = view->window.win;
   }
   else {
      if (!view->view) return;
      looper = BHandler_Looper(view->view);
   }

   if (!looper) {
      return;
   }

   if (BLooper_Lock(looper)) {
      view->locked_looper = looper;
   }
}


static void view_unlock(View *view)
{
   if (view->locked_looper) {
      BLooper_Unlock(view->locked_looper);
      view->locked_looper = NULL;
   }
}


static void *create_alert(int msg_type, plat_char *title, plat_char *msg)
{
   const char *btn1, *btn2 = NULL, *btn3 = NULL;
   int type;
   void *alert;

   switch (msg_type & 0xFF) {
      default:
      case MSG_OK:            btn1 = "OK"; break;
      case MSG_OK_CANCEL:     btn1 = "Cancel"; btn2 = "OK"; break;
      case MSG_YES_NO:        btn1 = "No"; btn2 = "Yes"; break;
      case MSG_YES_NO_CANCEL: btn1 = "Cancel"; btn2 = "No"; btn3 = "Yes"; break;
   }

   switch (msg_type & 0xFF00) {
      default:
      case MSG_ICON_INFO:     type = B_INFO_ALERT; break;
      case MSG_ICON_QUESTION: type = B_IDEA_ALERT; break;
      case MSG_ICON_ERROR:    type = B_STOP_ALERT; break;
      case MSG_ICON_WARNING:  type = B_WARNING_ALERT; break;
   }

   alert = new(BALERT_SIZE);
   BAlert_new(alert, title, msg, btn1, btn2, btn3, B_WIDTH_AS_USUAL, btn3? B_OFFSET_SPACING : B_EVEN_SPACING, type);
   return alert;
}


static int get_alert_button(int msg_type, int idx)
{
   int ret = MSG_BTN_CANCEL;

   switch (msg_type & 0xFF) {
      default:
      case MSG_OK:
         if (idx == 0) { ret = MSG_BTN_OK; break; }
         break;

      case MSG_OK_CANCEL:
         if (idx == 0) { ret = MSG_BTN_CANCEL; break; }
         if (idx == 1) { ret = MSG_BTN_OK; break; }
         break;

      case MSG_YES_NO:
         if (idx == 0) { ret = MSG_BTN_NO; break; }
         if (idx == 1) { ret = MSG_BTN_YES; break; }
         break;

      case MSG_YES_NO_CANCEL:
         if (idx == 0) { ret = MSG_BTN_CANCEL; break; }
         if (idx == 1) { ret = MSG_BTN_NO; break; }
         if (idx == 2) { ret = MSG_BTN_YES; break; }
         break;
   }

   return ret;
}


static void update_menu_after_destroying(Menu *menu);

static void script_looper_message_received(void *looper, BMessage *msg)
{
   //printf("looper %p msg %x\n", looper, msg->what);
   switch (msg->what) {
      case MSG_INIT_APP: {
         int argc = BMessage_GetInt32(msg, "argc", 0);
         char **argv = BMessage_GetPointer(msg, "argv", NULL);
         app_main(argc, argv);
         break;
      }

      case MSG_ASYNC_MSG_RESULT: {
         int idx = BMessage_GetInt32(msg, "which", 0);
         int type = BMessage_GetInt32(msg, "type", 0);
         Heap *heap = BMessage_GetPointer(msg, "heap", NULL);
         Value func = GetValue(msg, "func");
         Value data = GetValue(msg, "data");
         Value error;

         if (func.value) {
            fixscript_call(heap, func, 2, &error, data, fixscript_int(get_alert_button(type, idx)));
            if (error.value) {
               fprintf(stderr, "error while running async message callback:\n");
               fixscript_dump_value(heap, error, 1);
            }
         }

         fixscript_unref(heap, data);
         break;
      }

      case MSG_WINDOW_RESIZED: {
         View *view = BMessage_GetPointer(msg, "view", NULL);
         call_view_callback(view, CALLBACK_WINDOW_RESIZE);
         if (!BMessage_GetBool(msg, "async", 0)) {
            notify_event_finish(msg);
         }
         break;
      }

      case MSG_WINDOW_CLOSE: {
         View *view = BMessage_GetPointer(msg, "view", NULL);
         call_view_callback(view, CALLBACK_WINDOW_CLOSE);
         break;
      }

      case MSG_DRAW_CANVAS: {
         View *view = BMessage_GetPointer(msg, "view", NULL);
         void *bitmap = BMessage_GetPointer(msg, "bitmap", NULL);
         int offset_x = BMessage_GetInt32(msg, "offset_x", 0);
         int offset_y = BMessage_GetInt32(msg, "offset_y", 0);
         int *finished_flag = BMessage_GetPointer(msg, "finished_flag", NULL);
         Heap *heap = view->common.heap;
         Value img, painter, error = fixscript_int(0);
         BRect rect;
         int width, height;
#ifdef __LP64__
         BBitmap_Bounds(&rect, bitmap);
#else
         rect = BBitmap_Bounds(bitmap);
#endif
         width = ceilf(rect.right - rect.left);
         height = ceilf(rect.bottom - rect.top);

         img = fiximage_create_from_pixels(heap, width, height, BBitmap_BytesPerRow(bitmap)/4, BBitmap_Bits(bitmap), delete_virtual, bitmap, -1);
         if (!img.value) {
            fixscript_error(heap, &error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         }
         fixscript_ref(heap, img);

         if (!error.value) {
            painter = fiximage_create_painter(heap, img, -offset_x, -offset_y);
            if (!painter.value) {
               fixscript_error(heap, &error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
            }
         }

         if (!error.value) {
            call_view_callback_with_value(view, CALLBACK_CANVAS_PAINT, painter);
         }

         if (error.value) {
            fprintf(stderr, "error while painting:\n");
            fixscript_dump_value(heap, error, 1);
         }

         pthread_mutex_lock(&mutex);
         *finished_flag = 1;
         pthread_cond_broadcast(&cond);
         while (*finished_flag != 2) {
            pthread_cond_wait(&cond, &mutex);
         }
         pthread_mutex_unlock(&mutex);

         fixscript_unref(heap, img);

         pthread_mutex_lock(&mutex);
         *finished_flag = 3;
         pthread_cond_broadcast(&cond);
         pthread_mutex_unlock(&mutex);
         break;
      }

      case MSG_CANVAS_MOUSE_EVENT: {
         View *view = BMessage_GetPointer(msg, "view", NULL);
         int type = BMessage_GetInt32(msg, "type", 0);
         int x = BMessage_GetInt32(msg, "x", 0);
         int y = BMessage_GetInt32(msg, "y", 0);
         int button = BMessage_GetInt32(msg, "button", 0);
         int mod = BMessage_GetInt32(msg, "mod", 0);
         int click_count = BMessage_GetInt32(msg, "click_count", 0);
         call_mouse_event_callback(view, type, x, y, button, mod, click_count, 0);
         break;
      }

      case MSG_CANVAS_MOUSE_WHEEL_EVENT: {
         View *view = BMessage_GetPointer(msg, "view", NULL);
         int x = BMessage_GetInt32(msg, "x", 0);
         int y = BMessage_GetInt32(msg, "y", 0);
         int mod = BMessage_GetInt32(msg, "mod", 0);
         int wheel_y = BMessage_GetInt32(msg, "wheel_y", 0);
         call_mouse_wheel_callback(view, x, y, mod, 0, wheel_y/1000.0f, 0, 0);
         break;
      }

      case MSG_CANVAS_KEY_EVENT: {
         View *view = BMessage_GetPointer(msg, "view", NULL);
         int type = BMessage_GetInt32(msg, "type", 0);
         int key = BMessage_GetInt32(msg, "key", 0);
         int mod = BMessage_GetInt32(msg, "mod", 0);
         call_key_event_callback(view, type, key, mod);
         break;
      }

      case MSG_CANVAS_KEY_TYPED_EVENT: {
         View *view = BMessage_GetPointer(msg, "view", NULL);
         char *chars = BMessage_GetPointer(msg, "chars", NULL);
         int mod = BMessage_GetInt32(msg, "mod", 0);
         call_key_typed_event_callback(view, chars, mod);
         free(chars);
         break;
      }

      case MSG_BUTTON_CLICKED: {
         View *view = BMessage_GetPointer(msg, "view", NULL);
         call_action_callback(view, CALLBACK_BUTTON_ACTION);
         break;
      }

      case MSG_MENU_ITEM_ACTION: {
         Menu *menu = BMessage_GetPointer(msg, "menu", NULL);
         MenuItem *item = BMessage_GetPointer(msg, "item", NULL);
         MenuItem *mi;
         int i;

         for (i=0,mi=menu->common.items; mi; mi=mi->next, i++) {
            if (mi == item) {
               call_menu_callback(menu, i);
               break;
            }
         }
         break;
      }

      case MSG_POPUP_MENU_DELETED: {
         Menu *menu = BMessage_GetPointer(msg, "menu", NULL);
         update_menu_after_destroying(menu);
         fixscript_unref(menu->common.heap, menu->common.instance);
         notify_event_finish(msg);
         break;
      }

      /*
      case MSG_POPUP_ACTION: {
         NotifyIcon *icon = BMessage_GetPointer(msg, "icon", NULL);
         call_notify_icon_click_callback(icon);
         break;
      }

      case MSG_POPUP_MENU: {
         NotifyIcon *icon = (NotifyIcon *)msg->GetPointer("icon");
         FixPopUpMenu *popup;
         int x = msg->GetInt32("screen_x", 0);
         int y = msg->GetInt32("screen_y", 0);

         fixscript_ref(icon->menu->common.heap, icon->menu->common.instance);
         menu_real_create(icon->menu, "popup menu");
         popup = (FixPopUpMenu *)icon->menu->menu;
         popup->menu = icon->menu;
         popup->SetAsyncAutoDestruct(true);
         popup->Go(BPoint(x, y), true, false, true);
         break;
      }
      */

      case MSG_PROCESS_TIMERS: {
         Timer *timer, *next_timer;
         uint32_t time;

         pthread_mutex_lock(&timer_mutex);
         time = timer_get_time();
         for (timer = active_timers; timer; timer = next_timer) {
            next_timer = timer->next;
            if (timer->interval == 0 || time >= timer->next_time) {
               timer->next_time = time + timer->interval;
               timer_run(timer->heap, timer->instance);
            }
         }
         pthread_cond_broadcast(&timer_cond_processed);
         pthread_mutex_unlock(&timer_mutex);
         break;
      }

      case MSG_WORKER_NOTIFY: {
         Worker *worker = BMessage_GetPointer(msg, "worker", NULL);
         worker->common.notify_func(worker);
         break;
      }

      default:
         BLooper_MessageReceived(looper, msg);
   }
}


static bool fixwindow_quit_requested(FixWindow *win)
{
   void *msg;

   msg = new(BMESSAGE_SIZE);
   BMessage_new(msg, MSG_WINDOW_CLOSE);
   BMessage_AddPointer(msg, "view", win->view);
   BLooper_PostMessage(script_looper, msg);
   delete_virtual(msg);
   return 0;
}


static void fixwindowview_attached_to_window(FixWindowView *view)
{
   void *msg;

   msg = new(BMESSAGE_SIZE);
   BMessage_new(msg, MSG_WINDOW_RESIZED);
   BMessage_AddPointer(msg, "view", view->view);
   BMessage_AddBool(msg, "async", 1);
   BLooper_PostMessage(script_looper, msg);
   delete_virtual(msg);
}


static void fixwindowview_frame_resized(FixWindowView *view, float new_width, float new_height)
{
   void *msg;

   msg = new(BMESSAGE_SIZE);
   BMessage_new(msg, MSG_WINDOW_RESIZED);
   BMessage_AddPointer(msg, "view", view->view);
   prepare_event_finish(msg);

   BHandler_UnlockLooper(view);
   BLooper_PostMessage(script_looper, msg);
   wait_event_finish(msg);
   BHandler_LockLooper(view);
   delete_virtual(msg);
}


static void fixcanvas_draw(FixCanvas *canvas, BRect *update_rect)
{
   int *finished_flag = (int *)calloc(1, sizeof(int));

   int x1 = (int)floorf(update_rect->left);
   int y1 = (int)floorf(update_rect->top);
   int x2 = (int)ceilf(update_rect->right)+2; // TODO: why is this needed to be incremented by 2 and not 1?
   int y2 = (int)ceilf(update_rect->bottom)+2;

   void *bitmap, *msg;
   BRect rect;
   
   bitmap = new(BBITMAP_SIZE);
   rect.left = 0;
   rect.top = 0;
   rect.right = x2-x1-1;
   rect.bottom = y2-y1-1;
   BBitmap_new(bitmap, &rect, B_RGB32, 0, 0);

   msg = new(BMESSAGE_SIZE);
   BMessage_new(msg, MSG_DRAW_CANVAS);
   BMessage_AddPointer(msg, "view", canvas->view);
   BMessage_AddPointer(msg, "bitmap", bitmap);
   BMessage_AddInt32(msg, "offset_x", x1);
   BMessage_AddInt32(msg, "offset_y", y1);
   BMessage_AddPointer(msg, "finished_flag", finished_flag);

   BHandler_UnlockLooper(canvas);
   BLooper_PostMessage(script_looper, msg);
   delete_virtual(msg);

   pthread_mutex_lock(&mutex);
   while (*finished_flag != 1) {
      pthread_cond_wait(&cond, &mutex);
   }
   pthread_mutex_unlock(&mutex);

   BHandler_LockLooper(canvas);
   rect.left = x1;
   rect.top = y1;
   rect.right = x2-1;
   rect.bottom = y2-1;
   BView_DrawBitmap(canvas, bitmap, &rect);

   pthread_mutex_lock(&mutex);
   *finished_flag = 2;
   pthread_cond_broadcast(&cond);
   while (*finished_flag != 3) {
      pthread_cond_wait(&cond, &mutex);
   }
   pthread_mutex_unlock(&mutex);

   free(finished_flag);
}


static void send_converted_mouse_message(View *view, int type, int x, int y, int button, int mod, int click_count)
{
   void *msg;

   msg = new(BMESSAGE_SIZE);
   BMessage_new(msg, MSG_CANVAS_MOUSE_EVENT);
   BMessage_AddPointer(msg, "view", view);
   BMessage_AddInt32(msg, "type", type);
   BMessage_AddInt32(msg, "x", x);
   BMessage_AddInt32(msg, "y", y);
   BMessage_AddInt32(msg, "button", button);
   BMessage_AddInt32(msg, "mod", mod);
   BMessage_AddInt32(msg, "click_count", click_count);
   BLooper_PostMessage(script_looper, msg);
}


static int convert_modifiers(int buttons, int modifiers)
{
   int mod = 0;

   if (modifiers & 1) mod |= SCRIPT_MOD_SHIFT;
   if (modifiers & 2) mod |= SCRIPT_MOD_ALT;
   if (modifiers & 4) mod |= SCRIPT_MOD_CTRL;

   if (buttons & 1) mod |= SCRIPT_MOD_LBUTTON;
   if (buttons & 2) mod |= SCRIPT_MOD_RBUTTON;
   if (buttons & 4) mod |= SCRIPT_MOD_MBUTTON;

   return mod;
}


static void send_mouse_message(View *view, int type, int x, int y, int new_buttons, int old_buttons, int modifiers, int click_count)
{
   int mod = convert_modifiers(new_buttons, modifiers);

   if (new_buttons != old_buttons) {
      if ((new_buttons ^ old_buttons) & 1) {
         send_converted_mouse_message(view, type, x, y, MOUSE_BUTTON_LEFT, mod, click_count);
      }
      if ((new_buttons ^ old_buttons) & 2) {
         send_converted_mouse_message(view, type, x, y, MOUSE_BUTTON_RIGHT, mod, click_count);
      }
      if ((new_buttons ^ old_buttons) & 4) {
         send_converted_mouse_message(view, type, x, y, MOUSE_BUTTON_MIDDLE, mod, click_count);
      }
   }
   else {
      send_converted_mouse_message(view, type, x, y, -1, mod, click_count);
   }
}


static void fixcanvas_mouse_down(FixCanvas *canvas, BPoint where)
{
   void *win, *msg;
   BPoint coord;
   int32_t buttons, modifiers, clicks;

   win = BView_Window(canvas);
   msg = BLooper_CurrentMessage(win);
   BMessage_FindPoint(msg, "where", &coord);
   buttons = BMessage_GetInt32(msg, "buttons", 0);
   modifiers = BMessage_GetInt32(msg, "modifiers", 0);
   clicks = BMessage_GetInt32(msg, "clicks", 0);

   send_mouse_message(canvas->view, EVENT_MOUSE_DOWN, (int)coord.x, (int)coord.y, buttons, canvas->last_buttons, modifiers, clicks);

   canvas->last_x = (int)coord.x;
   canvas->last_y = (int)coord.y;
   canvas->last_buttons = buttons;

   BView_SetMouseEventMask(canvas, B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS | B_NO_POINTER_HISTORY);
}


static void fixcanvas_mouse_up(FixCanvas *canvas, BPoint where)
{
   void *win, *msg;
   BPoint coord;
   int32_t buttons, modifiers, clicks;

   win = BView_Window(canvas);
   msg = BLooper_CurrentMessage(win);
   BMessage_FindPoint(msg, "where", &coord);
   buttons = BMessage_GetInt32(msg, "buttons", 0);
   modifiers = BMessage_GetInt32(msg, "modifiers", 0);
   clicks = BMessage_GetInt32(msg, "clicks", 0);

   send_mouse_message(canvas->view, EVENT_MOUSE_UP, (int)coord.x, (int)coord.y, buttons, canvas->last_buttons, modifiers, clicks);

   canvas->last_x = (int)coord.x;
   canvas->last_y = (int)coord.y;
   canvas->last_buttons = buttons;
}


static void fixcanvas_mouse_moved(FixCanvas *canvas, BPoint where, uint32_t code, BMessage *msg)
{
   void *win;
   BPoint coord;
   int32_t buttons, modifiers, transit;

   win = BView_Window(canvas);
   msg = BLooper_CurrentMessage(win);
   BMessage_FindPoint(msg, "where", &coord);
   buttons = BMessage_GetInt32(msg, "buttons", 0);
   modifiers = BMessage_GetInt32(msg, "modifiers", 0);
   transit = BMessage_GetInt32(msg, "be:transit", 0);

   if (transit == B_ENTERED_VIEW && buttons == 0) {
      send_mouse_message(canvas->view, EVENT_MOUSE_ENTER, (int)coord.x, (int)coord.y, buttons, buttons, modifiers, 0);
   }

   send_mouse_message(canvas->view, buttons != 0? EVENT_MOUSE_DRAG : EVENT_MOUSE_MOVE, (int)coord.x, (int)coord.y, buttons, buttons, modifiers, 0);

   if (transit == B_EXITED_VIEW && buttons == 0) {
      send_mouse_message(canvas->view, EVENT_MOUSE_LEAVE, 0, 0, 0, 0, 0, 0);
   }

   canvas->last_x = (int)coord.x;
   canvas->last_y = (int)coord.y;
   canvas->last_buttons = buttons;
}


static int convert_key(int key)
{
   switch (key) {
      case 0x01: key = KEY_ESCAPE; break;
      case 0x02: key = KEY_F1; break;
      case 0x03: key = KEY_F2; break;
      case 0x04: key = KEY_F3; break;
      case 0x05: key = KEY_F4; break;
      case 0x06: key = KEY_F5; break;
      case 0x07: key = KEY_F6; break;
      case 0x08: key = KEY_F7; break;
      case 0x09: key = KEY_F8; break;
      case 0x0A: key = KEY_F9; break;
      case 0x0B: key = KEY_F10; break;
      case 0x0C: key = KEY_F11; break;
      case 0x0D: key = KEY_F12; break;
      //case : key = KEY_PRINT_SCREEN; break;
      case 0x0F: key = KEY_SCROLL_LOCK; break;
      //case : key = KEY_PAUSE; break;
      case 0x11: key = KEY_GRAVE; break;
      case 0x12: key = KEY_NUM1; break;
      case 0x13: key = KEY_NUM2; break;
      case 0x14: key = KEY_NUM3; break;
      case 0x15: key = KEY_NUM4; break;
      case 0x16: key = KEY_NUM5; break;
      case 0x17: key = KEY_NUM6; break;
      case 0x18: key = KEY_NUM7; break;
      case 0x19: key = KEY_NUM8; break;
      case 0x1A: key = KEY_NUM9; break;
      case 0x1B: key = KEY_NUM0; break;
      case 0x1C: key = KEY_MINUS; break;
      case 0x1D: key = KEY_EQUAL; break;
      case 0x1E: key = KEY_BACKSPACE; break;
      case 0x26: key = KEY_TAB; break;
      case 0x27: key = KEY_Q; break;
      case 0x28: key = KEY_W; break;
      case 0x29: key = KEY_E; break;
      case 0x2A: key = KEY_R; break;
      case 0x2B: key = KEY_T; break;
      case 0x2C: key = KEY_Y; break;
      case 0x2D: key = KEY_U; break;
      case 0x2E: key = KEY_I; break;
      case 0x2F: key = KEY_O; break;
      case 0x30: key = KEY_P; break;
      case 0x31: key = KEY_LBRACKET; break;
      case 0x32: key = KEY_RBRACKET; break;
      case 0x33: key = KEY_BACKSLASH; break;
      //case : key = KEY_CAPS_LOCK; break;
      case 0x3C: key = KEY_A; break;
      case 0x3D: key = KEY_S; break;
      case 0x3E: key = KEY_D; break;
      case 0x3F: key = KEY_F; break;
      case 0x40: key = KEY_G; break;
      case 0x41: key = KEY_H; break;
      case 0x42: key = KEY_J; break;
      case 0x43: key = KEY_K; break;
      case 0x44: key = KEY_L; break;
      case 0x45: key = KEY_SEMICOLON; break;
      case 0x46: key = KEY_APOSTROPHE; break;
      case 0x47: key = KEY_ENTER; break;
      //case : key = KEY_LSHIFT; break;
      case 0x4C: key = KEY_Z; break;
      case 0x4D: key = KEY_X; break;
      case 0x4E: key = KEY_C; break;
      case 0x4F: key = KEY_V; break;
      case 0x50: key = KEY_B; break;
      case 0x51: key = KEY_N; break;
      case 0x52: key = KEY_M; break;
      case 0x53: key = KEY_COMMA; break;
      case 0x54: key = KEY_PERIOD; break;
      //case : key = KEY_SLASH; break;
      //case : key = KEY_RSHIFT; break;
      //case : key = KEY_LCONTROL; break;
      //case : key = KEY_LMETA; break;
      //case : key = KEY_LALT; break;
      case 0x5E: key = KEY_SPACE; break;
      //case : key = KEY_RALT; break;
      //case : key = KEY_RMETA; break;
      //case : key = KEY_RMENU; break;
      //case : key = KEY_RCONTROL; break;
      case 0x1F: key = KEY_INSERT; break;
      case 0x34: key = KEY_DELETE; break;
      case 0x20: key = KEY_HOME; break;
      case 0x35: key = KEY_END; break;
      case 0x21: key = KEY_PAGE_UP; break;
      case 0x36: key = KEY_PAGE_DOWN; break;
      case 0x61: key = KEY_LEFT; break;
      case 0x57: key = KEY_UP; break;
      case 0x63: key = KEY_RIGHT; break;
      case 0x62: key = KEY_DOWN; break;
      //case : key = KEY_NUM_LOCK; break;
      case 0x23: key = KEY_NUMPAD_SLASH; break;
      case 0x24: key = KEY_NUMPAD_STAR; break;
      case 0x25: key = KEY_NUMPAD_MINUS; break;
      case 0x3A: key = KEY_NUMPAD_PLUS; break;
      case 0x5B: key = KEY_NUMPAD_ENTER; break;
      case 0x65: key = KEY_NUMPAD_DOT; break;
      case 0x64: key = KEY_NUMPAD0; break;
      case 0x58: key = KEY_NUMPAD1; break;
      case 0x59: key = KEY_NUMPAD2; break;
      case 0x5A: key = KEY_NUMPAD3; break;
      case 0x48: key = KEY_NUMPAD4; break;
      case 0x49: key = KEY_NUMPAD5; break;
      case 0x4A: key = KEY_NUMPAD6; break;
      case 0x37: key = KEY_NUMPAD7; break;
      case 0x38: key = KEY_NUMPAD8; break;
      case 0x39: key = KEY_NUMPAD9; break;

      default:
         key = -1;
   }
   return key;
}


static void fixcanvas_key_down(FixCanvas *canvas, const char *_bytes, int32_t _num_bytes)
{
   void *win, *msg, *new_msg;
   int what;
   int32_t key, modifiers;
   int mod = 0;
   void *data;
   ssize_t num_bytes;

   win = BView_Window(canvas);
   msg = BLooper_CurrentMessage(win);
   what = ((BMessage *)msg)->what;
   key = BMessage_GetInt32(msg, "key", 0);
   modifiers = BMessage_GetInt32(msg, "modifiers", 0);

   if (modifiers & 1) mod |= SCRIPT_MOD_SHIFT;
   if (modifiers & 2) mod |= SCRIPT_MOD_ALT;
   if (modifiers & 4) mod |= SCRIPT_MOD_CTRL;

   key = convert_key(key);
   if (key != -1) {
      new_msg = new(BMESSAGE_SIZE);
      BMessage_new(new_msg, MSG_CANVAS_KEY_EVENT);
      BMessage_AddPointer(new_msg, "view", canvas->view);
      BMessage_AddInt32(new_msg, "type", what == B_KEY_DOWN? EVENT_KEY_DOWN : EVENT_KEY_UP);
      BMessage_AddInt32(new_msg, "key", key);
      BMessage_AddInt32(new_msg, "mod", mod);
      BLooper_PostMessage(script_looper, new_msg);
   }

   if (what == B_KEY_DOWN) {
      if (BMessage_FindData(msg, "bytes", B_STRING_TYPE, &data, &num_bytes) == B_OK) {
         if (num_bytes > 1 && ((char *)data)[0] >= ' ' && ((char *)data)[0] != 127) {
            new_msg = new(BMESSAGE_SIZE);
            BMessage_new(new_msg, MSG_CANVAS_KEY_TYPED_EVENT);
            BMessage_AddPointer(new_msg, "view", canvas->view);
            BMessage_AddPointer(new_msg, "chars", strdup(data));
            BMessage_AddInt32(new_msg, "mod", mod);
            BLooper_PostMessage(script_looper, new_msg);
         }
      }
   }
}


static void fixcanvas_message_received(FixCanvas *canvas, BMessage *msg)
{
   float wheel_y;

   if (msg->what == B_MOUSE_WHEEL_CHANGED) {
      BMessage_FindFloat(msg, "be:wheel_delta_y", &wheel_y);

      msg = new(BMESSAGE_SIZE);
      BMessage_new(msg, MSG_CANVAS_MOUSE_WHEEL_EVENT);
      BMessage_AddPointer(msg, "view", canvas->view);
      BMessage_AddInt32(msg, "x", canvas->last_x);
      BMessage_AddInt32(msg, "y", canvas->last_y);
      BMessage_AddInt32(msg, "mod", convert_modifiers(canvas->last_buttons, modifiers()));
      BMessage_AddInt32(msg, "wheel_y", (int)(wheel_y*1000.0f));
      BLooper_PostMessage(script_looper, msg);
   }

   fixcanvas_orig_message_received(canvas, msg);
}


static void fixpopupmenu_destroy(FixPopUpMenu *popup)
{
   void *msg;

   msg = new(BMESSAGE_SIZE);
   BMessage_new(msg, MSG_POPUP_MENU_DELETED);
   BMessage_AddPointer(msg, "menu", popup->menu);
   prepare_event_finish(msg);
   BLooper_PostMessage(script_looper, msg);
   wait_event_finish(msg);
   delete_virtual(msg);

   popup->orig_destructor(popup);
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
   if (view->common.type == TYPE_WINDOW) {
      call_view_callback(view, CALLBACK_WINDOW_DESTROY);
      BLooper_Lock(view->window.win);
      BWindow_Quit(view->window.win);
   }
}


void view_get_rect(View *view, Rect *rect)
{
   BRect brect;
   
   if (view->common.type == TYPE_WINDOW) {
      view_lock(view);
#ifdef __LP64__
      BWindow_Frame(&brect, view->window.win);
#else
      brect = BWindow_Frame(view->window.win);
#endif
      view_unlock(view);
   }
   else if (view->view) {
      view_lock(view);
#ifdef __LP64__
      BView_Frame(&brect, view->view);
#else
      brect = BView_Frame(view->view);
#endif
      view_unlock(view);
   }
   else {
      return;
   }

   rect->x1 = (int)roundf(brect.left);
   rect->y1 = (int)roundf(brect.top);
   rect->x2 = (int)roundf(brect.right)+1;
   rect->y2 = (int)roundf(brect.bottom)+1;
}


void view_set_rect(View *view, Rect *rect)
{
   if (!view->view) return;

   view_lock(view);
   BView_MoveTo(view->view, rect->x1, rect->y1);
   BView_ResizeTo(view->view, rect->x2 - rect->x1, rect->y2 - rect->y1);
   view_unlock(view);

   if (view->common.type == TYPE_CANVAS) {
      call_view_callback(view, CALLBACK_CANVAS_RESIZE);
   }
}


void view_get_content_rect(View *view, Rect *rect)
{
   void *bview;
   BRect brect;
   
   if (view->common.type == TYPE_WINDOW) {
      bview = view->window.contents;
   }
   else if (view->view) {
      bview = view->view;
   }
   else {
      return;
   }

   if (!bview) return;
   
   view_lock(view);
#ifdef __LP64__
   BView_Bounds(&brect, bview);
#else
   brect = BView_Bounds(bview);
#endif
   view_unlock(view);

   rect->x1 = (int)roundf(brect.left);
   rect->y1 = (int)roundf(brect.top);
   rect->x2 = (int)roundf(brect.right)+1;
   rect->y2 = (int)roundf(brect.bottom)+1;
}


void view_get_inner_rect(View *view, Rect *rect)
{
   view_get_content_rect(view, rect);
}


void view_set_visible(View *view, int visible)
{
   if (view->common.type == TYPE_WINDOW) {
      void *win = view->window.win;
      if (visible) {
         if (!view->window.created) {
            BWindow_Show(win);
         }
         else {
            view_lock(view);
            BWindow_Show(win);
            view_unlock(view);
         }
      }
      else {
         if (view->window.created) {
            view_lock(view);
            BWindow_Hide(win);
            view_unlock(view);
         }
      }
   }
}


int view_add(View *parent, View *view)
{
   void *parent_bview;

   if (!view->view) return 0;

   if (parent->common.type == TYPE_WINDOW) {
      parent_bview = parent->window.contents;
   }
   else if (parent->common.type == TYPE_CANVAS) {
      // TODO: add support for canvases
      //parent_bview = parent->canvas.canvas;
      return 1;
   }
   else {
      parent_bview = parent->view;
   }

   view_lock(parent);
   BView_AddChild(parent_bview, view->view, NULL);
   view_unlock(parent);
   return 1;
}


void view_focus(View *view)
{
   if (!view->view) return;

   view_lock(view);
   BView_MakeFocus(view->view, 1);
   view_unlock(view);
}


int view_has_focus(View *view)
{
   if (!view->view) return 0;

   view_lock(view);
   int ret = BView_IsFocus(view->view);
   view_unlock(view);
   return ret;
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
   if (view->common.type == TYPE_BUTTON) {
      *width = 64;
      *height = 31-1;
   }
   else {
      *width = 64;
      *height = 25;
   }
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
   uint32_t win_flags = 0;
   BRect rect;
   FixWindow *win;
   FixWindowView *winview;
   
   view = (View *)calloc(1, sizeof(View));
   if (!view) return NULL;

   if ((flags & WIN_RESIZABLE) == 0) {
      win_flags |= B_NOT_RESIZABLE | B_NOT_ZOOMABLE;
   }
   
   win = new(sizeof(FixWindow));
   win->view = view;
   rect.left = 50;
   rect.top = 50;
   rect.right = rect.left + width;
   rect.bottom = rect.top + height;
   BWindow_new(win, &rect, title, B_TITLED_WINDOW, win_flags, B_CURRENT_WORKSPACE);
   if (!fixwindow_vtable) {
      fixwindow_vtable = clone_vtable(win, 2, 55);
      fixwindow_vtable[2+21] = fixwindow_quit_requested;
   }
   *(void ***)win = fixwindow_vtable+2;

   view->window.win = win;

#ifdef __LP64__
   BWindow_Bounds(&rect, win);
#else
   rect = BWindow_Bounds(win);
#endif
   winview = new(sizeof(FixWindowView));
   winview->view = view;
   BView_new(winview, &rect, NULL, B_FOLLOW_ALL_SIDES, B_FRAME_EVENTS);
   BView_SetViewUIColor(winview, B_PANEL_BACKGROUND_COLOR, B_NO_TINT);
   if (!fixwindowview_vtable) {
      fixwindowview_vtable = clone_vtable(winview, 2, 66);
      fixwindowview_vtable[2+18] = fixwindowview_attached_to_window;
      fixwindowview_vtable[2+31] = fixwindowview_frame_resized;
   }
   *(void ***)winview = fixwindowview_vtable+2;

   view->window.contents = winview;
   BWindow_AddChild(win, winview, NULL);

   if (flags & WIN_CENTER) {
      BWindow_CenterOnScreen(win);
   }
   if (flags & WIN_MAXIMIZE) {
      BWindow_Zoom(win);
   }
   if (flags & WIN_MINIMIZE) {
      // TODO: not working
      BWindow_Minimize(win, 1);
   }

   return view;
}


plat_char *window_get_title(View *view)
{
   char *ret;

   view_lock(view);
   ret = strdup(BWindow_Title(view->window.win));
   view_unlock(view);
   return ret;
}


void window_set_title(View *view, plat_char *title)
{
   view_lock(view);
   BWindow_SetTitle(view->window.win, title);
   view_unlock(view);
}


void window_set_minimum_size(View *view, int width, int height)
{
   view_lock(view);
   BWindow_SetSizeLimits(view->window.win, width, 1000000.0f, height, 1000000.0f);
   view_unlock(view);
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
   BRect rect;

   view = (View *)calloc(1, sizeof(View));
   if (!view) return NULL;

   view->view = new(BSTRINGVIEW_SIZE);
   rect.left = 0;
   rect.top = 0;
   rect.right = 100;
   rect.bottom = 100;
   BStringView_new(view->view, &rect, NULL, label, B_FOLLOW_LEFT_TOP, B_WILL_DRAW);
   return view;
}


plat_char *label_get_label(View *view)
{
   char *label;

   view_lock(view);
   label = strdup(BStringView_Text(view->view));
   view_unlock(view);
   return label;
}


void label_set_label(View *view, plat_char *label)
{
   view_lock(view);
   BStringView_SetText(view->view, label);
   view_unlock(view);
}


View *text_field_create()
{
   View *view;
   BRect rect;

   view = (View *)calloc(1, sizeof(View));
   if (!view) return NULL;

   view->view = new(BTEXTCONTROL_SIZE);
   rect.left = 0;
   rect.top = 0;
   rect.right = 100;
   rect.bottom = 100;
   BTextControl_new(view->view, &rect, NULL, NULL, "text", NULL, B_FOLLOW_LEFT_TOP, B_WILL_DRAW | B_NAVIGABLE);
   return view;
}


plat_char *text_field_get_text(View *view)
{
   char *text;

   view_lock(view);
   text = strdup(BTextControl_Text(view->view));
   view_unlock(view);
   return text;
}


void text_field_set_text(View *view, plat_char *text)
{
   view_lock(view);
   BTextControl_SetText(view->view, text);
   view_unlock(view);
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
   void *msg;
   BRect rect;

   view = (View *)calloc(1, sizeof(View));
   if (!view) return NULL;

   msg = new(BMESSAGE_SIZE);
   BMessage_new(msg, MSG_BUTTON_CLICKED);
   BMessage_AddPointer(msg, "view", view);

   view->view = new(BBUTTON_SIZE);
   rect.left = 0;
   rect.top = 0;
   rect.right = 100;
   rect.bottom = 100;
   BButton_new(view->view, &rect, NULL, label, msg, B_FOLLOW_LEFT_TOP, B_WILL_DRAW | B_NAVIGABLE | B_FULL_UPDATE_ON_RESIZE);
   BButton_SetTarget(view->view, script_looper, NULL);
   return view;
}


plat_char *button_get_label(View *view)
{
   char *label;

   view_lock(view);
   label = strdup(BControl_Label(view->view));
   view_unlock(view);
   return label;
}


void button_set_label(View *view, plat_char *label)
{
   view_lock(view);
   BButton_SetLabel(view->view, label);
   view_unlock(view);
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
   BRect rect;
   FixCanvas *canvas;

   view = (View *)calloc(1, sizeof(View));
   if (!view) return NULL;

   canvas = new(sizeof(FixCanvas));
   canvas->view = view;
   canvas->last_buttons = 0;
   rect.left = 0;
   rect.top = 0;
   rect.right = 100;
   rect.bottom = 100;
   BView_new(canvas, &rect, NULL, 0, B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE);
   if (!fixcanvas_vtable) {
      fixcanvas_vtable = clone_vtable(canvas, 2, 66);
      fixcanvas_orig_message_received = fixcanvas_vtable[2+7];
      fixcanvas_vtable[2+7] = fixcanvas_message_received;
      fixcanvas_vtable[2+22] = fixcanvas_draw;
      fixcanvas_vtable[2+23] = fixcanvas_mouse_down;
      fixcanvas_vtable[2+24] = fixcanvas_mouse_up;
      fixcanvas_vtable[2+25] = fixcanvas_mouse_moved;
      fixcanvas_vtable[2+27] = fixcanvas_key_down;
      fixcanvas_vtable[2+28] = fixcanvas_key_down;
   }
   *(void ***)canvas = fixcanvas_vtable+2;
   BView_SetViewColor(canvas, *B_TRANSPARENT_COLOR);

   view->canvas.canvas = canvas;
   view->canvas.flags = flags;

   if (flags & (CANVAS_SCROLLABLE | CANVAS_BORDER)) {
      bool scroll = flags & CANVAS_SCROLLABLE;
      view->canvas.scroll = new(BSCROLLVIEW_SIZE);
      BScrollView_new(view->canvas.scroll, NULL, view->canvas.canvas, 0, scroll, scroll, flags & CANVAS_BORDER? B_PLAIN_BORDER : B_NO_BORDER);
      view->view = view->canvas.scroll;
   }
   else {
      view->view = view->canvas.canvas;
   }

   return view;
}


void canvas_set_scroll_state(View *view, int type, int pos, int max, int page_size, int always_show)
{
   void *bar;

   if ((view->canvas.flags & CANVAS_SCROLLABLE) == 0) return;

   view_lock(view);
   bar = BScrollView_ScrollBar(view->canvas.scroll, type == SCROLL_HORIZ? B_HORIZONTAL : B_VERTICAL);
   BScrollBar_SetRange(bar, 0, max);
   BScrollBar_SetValue(bar, pos);
   BScrollBar_SetProportion(bar, max > 0? (float)page_size / (float)max : 0.0f);
   BScrollBar_SetSteps(bar, 16, page_size);
   view_unlock(view);
}


void canvas_set_scroll_position(View *view, int type, int pos)
{
   void *bar;

   if ((view->canvas.flags & CANVAS_SCROLLABLE) == 0) return;

   view_lock(view);
   bar = BScrollView_ScrollBar(view->canvas.scroll, type == SCROLL_HORIZ? B_HORIZONTAL : B_VERTICAL);
   BScrollBar_SetValue(bar, pos);
   view_unlock(view);
}


int canvas_get_scroll_position(View *view, int type)
{
   void *bar;
   int ret;

   if ((view->canvas.flags & CANVAS_SCROLLABLE) == 0) return 0;

   view_lock(view);
   bar = BScrollView_ScrollBar(view->canvas.scroll, type == SCROLL_HORIZ? B_HORIZONTAL : B_VERTICAL);
   ret = (int)roundf(BScrollBar_Value(bar));
   view_unlock(view);
   return ret;
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
   view_lock(view);
   BView_SetFlags(view->canvas.canvas, BView_Flags(view->canvas.canvas) | B_NAVIGABLE);
   view_unlock(view);
}


int canvas_is_focusable(View *view)
{
   view_lock(view);
   int ret = (BView_Flags(view->canvas.canvas) & B_NAVIGABLE) != 0;
   view_unlock(view);
   return ret;
}


void canvas_repaint(View *view, Rect *rect)
{
   BRect brect;
   
   view_lock(view);
   if (rect) {
      brect.left = rect->x1;
      brect.top = rect->y1;
      brect.right = rect->x2;
      brect.bottom = rect->y2;
      BView_Invalidate_rect(view->canvas.canvas, &brect);
   }
   else {
      BView_Invalidate(view->canvas.canvas);
   }
   view_unlock(view);
}


Menu *menu_create()
{
   Menu *menu;
   
   menu = (Menu *)calloc(1, sizeof(Menu));
   if (!menu) return NULL;

   return menu;
}


static void menu_real_create(Menu *menu, const char *popup_title)
{
   MenuItem *item;
   BRect rect;
   FixPopUpMenu *popup;
   void *msg, *bitem;

   if (popup_title) {
      popup = new(sizeof(FixPopUpMenu));
      BPopUpMenu_new(popup, popup_title, 1, 1, B_ITEMS_IN_COLUMN);
      popup->orig_destructor = VTABLE(popup, 1);
      if (!fixpopupmenu_vtable) {
         fixpopupmenu_vtable = clone_vtable(popup, 2, 82);
         fixpopupmenu_vtable[2+1] = fixpopupmenu_destroy;
      }
      *(void ***)popup = fixpopupmenu_vtable+2;
      menu->menu = popup;
   }
   else {
      rect.left = 0;
      rect.top = 0;
      rect.right = 32;
      rect.bottom = 32;
      menu->menu = new(BMENUBAR_SIZE);
      BMenuBar_new(menu->menu, &rect, "main menu", B_FOLLOW_LEFT_RIGHT | B_FOLLOW_TOP, B_ITEMS_IN_ROW, 1);
   }

   for (item=menu->common.items; item; item=item->next) {
      if (item->submenu) {
         menu_real_create(item->submenu, item->title);
         BMenu_AddItem_menu(menu->menu, item->submenu->menu);
      }
      else if (item->title) {
         msg = new(BMESSAGE_SIZE);
         BMessage_new(msg, MSG_MENU_ITEM_ACTION);
         BMessage_AddPointer(msg, "menu", menu);
         BMessage_AddPointer(msg, "item", item);
         bitem = new(BMENUITEM_SIZE);
         BMenuItem_new(bitem, item->title, msg, 0, 0);
         BMenuItem_SetTarget(bitem, script_looper, NULL);
         BMenu_AddItem_item(menu->menu, bitem);
      }
      else {
         BMenu_AddSeparatorItem(menu->menu);
      }
   }
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


void menu_insert_item(Menu *menu, int idx, plat_char *title, MenuItem *item)
{
   void *msg, *bitem;
   bool locked;

   if (menu->menu) {
      msg = new(BMESSAGE_SIZE);
      BMessage_new(msg, MSG_MENU_ITEM_ACTION);
      BMessage_AddPointer(msg, "menu", menu);
      BMessage_AddPointer(msg, "item", item);
      bitem = new(BMENUITEM_SIZE);
      BMenuItem_new(bitem, title, msg, 0, 0);
      BMenuItem_SetTarget(bitem, script_looper, NULL);
      locked = BHandler_LockLooper(menu->menu);
      if (idx < 0) {
         BMenu_AddItem_item(menu->menu, bitem);
      }
      else {
         BMenu_AddItem_item_idx(menu->menu, bitem, idx);
      }
      if (locked) {
         BHandler_UnlockLooper(menu->menu);
      }
   }
}


void menu_insert_separator(Menu *menu, int idx)
{
   bool locked;
   void *separator;

   if (menu->menu) {
      locked = BHandler_LockLooper(menu->menu);
      if (idx < 0) {
         BMenu_AddSeparatorItem(menu->menu);
      }
      else {
         separator = new(BSEPARATORITEM_SIZE);
         BSeparatorItem_new(separator);
         BMenu_AddItem_item_idx(menu->menu, separator, idx);
      }
      if (locked) {
         BHandler_UnlockLooper(menu->menu);
      }
   }
}


int menu_insert_submenu(Menu *menu, int idx, plat_char *title, Menu *submenu)
{
   bool locked;

   if (submenu->menu) return 0;

   if (menu->menu) {
      menu_real_create(submenu, title);
      locked = BHandler_LockLooper(menu->menu);
      if (idx < 0) {
         BMenu_AddItem_menu(menu->menu, submenu->menu);
      }
      else {
         BMenu_AddItem_menu_idx(menu->menu, submenu->menu, idx);
      }
      if (locked) {
         BHandler_UnlockLooper(menu->menu);
      }
   }
   return 1;
}


void menu_remove_item(Menu *menu, int idx, MenuItem *item)
{
   bool locked;

   if (menu->menu) {
      locked = BHandler_LockLooper(menu->menu);
      delete_virtual(BMenu_RemoveItem(menu->menu, idx));
      menu->menu = NULL;
      if (item->submenu) {
         if (item->submenu->menu) {
            delete_virtual(item->submenu->menu);
            update_menu_after_destroying(item->submenu);
         }
      }
      if (locked) {
         BHandler_UnlockLooper(menu->menu);
      }
   }
}


void menu_show(Menu *menu, View *view, int x, int y)
{
   void *bview;
   BPoint point;
   FixPopUpMenu *popup;
   bool locked;

   point.x = x;
   point.y = y;

   if (menu->menu) return;

   if (view->common.type == TYPE_WINDOW) {
      bview = view->window.contents;
   }
   else {
      bview = view->view;
   }
   if (!bview) return;

   locked = BHandler_LockLooper(bview);
   BView_ConvertToScreen(bview, &point);
   if (locked) {
      BHandler_UnlockLooper(bview);
   }

   fixscript_ref(menu->common.heap, menu->common.instance);
   menu_real_create(menu, "popup menu");
   popup = (FixPopUpMenu *)menu->menu;
   popup->menu = menu;
   BPopUpMenu_SetAsyncAutoDestruct(popup, 1);
   BPopUpMenu_Go(popup, &point, 1, 0, 1);
}


int show_message(View *window, int type, plat_char *title, plat_char *msg)
{
   void *alert;

   alert = create_alert(type, title, msg);
   return get_alert_button(type, BAlert_Go(alert));
}


Worker *worker_create()
{
   Worker *worker;

   worker = calloc(1, sizeof(Worker));
   if (!worker) return NULL;

   if (pthread_mutex_init(&worker->mutex, NULL) != 0) {
      free(worker);
      return NULL;
   }

   if (pthread_cond_init(&worker->cond, &cond_attr) != 0) {
      pthread_mutex_destroy(&worker->mutex);
      free(worker);
      return NULL;
   }

   return worker;
}


static void *worker_main(void *data)
{
   Worker *worker = data;

   pthread_setname_np(pthread_self(), "worker thread");
   worker->common.main_func(worker);
   return NULL;
}


int worker_start(Worker *worker)
{
   pthread_t thread;

   if (pthread_create(&thread, NULL, worker_main, worker) != 0) {
      return 0;
   }
   pthread_detach(thread);
   return 1;
}


void worker_notify(Worker *worker)
{
   void* msg;

   msg = new(BMESSAGE_SIZE);
   BMessage_new(msg, MSG_WORKER_NOTIFY);
   BMessage_AddPointer(msg, "worker", worker);
   BLooper_PostMessage(script_looper, msg);
   delete_virtual(msg);
}


void worker_lock(Worker *worker)
{
   pthread_mutex_lock(&worker->mutex);
}


void worker_wait(Worker *worker, int timeout)
{
   if (timeout == 0) return;
   if (timeout < 0) {
      pthread_cond_wait(&worker->cond, &worker->mutex);
   }
   else {
      pthread_cond_timedwait_relative(&worker->cond, &worker->mutex, timeout*1000000LL);
   }
}


void worker_unlock(Worker *worker)
{
   pthread_cond_signal(&worker->cond);
   pthread_mutex_unlock(&worker->mutex);
}


void worker_destroy(Worker *worker)
{
   pthread_cond_destroy(&worker->cond);
   pthread_mutex_destroy(&worker->mutex);
   free(worker);
}


static void *timer_thread(void *data)
{
   Timer *timer;
   uint32_t time;
   int32_t delta;
   int interval;
   void *msg;

   pthread_setname_np(pthread_self(), "timer thread");
   pthread_mutex_lock(&timer_mutex);
   for (;;) {
      for (;;) {
         time = timer_get_time();
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
         
         pthread_cond_timedwait_relative(&timer_cond, &timer_mutex, interval*1000000LL);
      }

      msg = new(BMESSAGE_SIZE);
      BMessage_new(msg, MSG_PROCESS_TIMERS);
      BLooper_PostMessage(script_looper, msg);
      delete_virtual(msg);

      pthread_cond_wait(&timer_cond_processed, &timer_mutex);
   }
   pthread_mutex_unlock(&timer_mutex);
   return NULL;
}


uint32_t timer_get_time()
{
#ifdef __linux__
   struct timespec ts;
   
   if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
      ts.tv_sec = 0;
      ts.tv_nsec = 0;
   }

   return ts.tv_sec * 1000LL + (ts.tv_nsec + 500000) / 1000000;
#else
   struct timeval tv;

   if (gettimeofday(&tv, NULL) != 0) {
      tv.tv_sec = 0;
      tv.tv_usec = 0;
   }

   return tv.tv_sec * 1000LL + (tv.tv_usec + 500) / 1000;
#endif
}


uint32_t timer_get_micro_time()
{
#ifdef __linux__
   struct timespec ts;
   
   if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
      ts.tv_sec = 0;
      ts.tv_nsec = 0;
   }

   return ts.tv_sec * 1000000LL + (ts.tv_nsec + 500) / 1000;
#else
   struct timeval tv;

   if (gettimeofday(&tv, NULL) != 0) {
      tv.tv_sec = 0;
      tv.tv_usec = 0;
   }

   return tv.tv_sec * 1000000LL + tv.tv_usec;
#endif
}


int timer_is_active(Heap *heap, Value instance)
{
   Timer *timer;
   int found = 0;

   pthread_mutex_lock(&timer_mutex);
   for (timer = active_timers; timer; timer = timer->next) {
      if (timer->heap == heap && timer->instance.value == instance.value && timer->instance.is_array == instance.is_array) {
         found = 1;
         break;
      }
   }
   pthread_mutex_unlock(&timer_mutex);

   return found;
}


void timer_start(Heap *heap, Value instance, int interval, int restart)
{
   Timer *timer;
   
   pthread_mutex_lock(&timer_mutex);
   for (timer = active_timers; timer; timer = timer->next) {
      if (timer->heap == heap && timer->instance.value == instance.value && timer->instance.is_array == instance.is_array) {
         break;
      }
   }
   if (timer) {
      if (restart) {
         timer->next_time = timer_get_time() + (uint32_t)timer->interval;
         pthread_cond_broadcast(&timer_cond);
      }
   }
   else {
      timer = calloc(1, sizeof(Timer));
      timer->heap = heap;
      timer->instance = instance;
      timer->interval = interval;
      timer->next_time = timer_get_time() + (uint32_t)interval;
      timer->next = active_timers;
      active_timers = timer;
      fixscript_ref(heap, instance);
      pthread_cond_broadcast(&timer_cond);
   }
   pthread_mutex_unlock(&timer_mutex);
}


void timer_stop(Heap *heap, Value instance)
{
   Timer *timer = NULL, **prev_timer;
   
   pthread_mutex_lock(&timer_mutex);
   prev_timer = &active_timers;
   for (timer = active_timers; timer; timer = timer->next) {
      if (timer->heap == heap && timer->instance.value == instance.value && timer->instance.is_array == instance.is_array) {
         break;
      }
      prev_timer = &timer->next;
   }
   if (timer) {
      *prev_timer = timer->next;
      fixscript_unref(timer->heap, timer->instance);
      free(timer);
   }
   pthread_mutex_unlock(&timer_mutex);
}


void clipboard_set_text(plat_char *text)
{
   /*
   TODO: doesn't work
   BMessage *msg;

   if (BClipboard_Lock(be_clipboard)) {
      BClipboard_Clear(be_clipboard);
      if (text) {
         msg = BClipboard_Data(be_clipboard);
         BMessage_AddData(msg, "text/plain", B_MIME_TYPE, text, strlen(text), 1, 1);
      }
      BClipboard_Commit(be_clipboard);
      BClipboard_Unlock(be_clipboard);
   }
   */
}


plat_char *clipboard_get_text()
{
   /*
   TODO: doesn't work
   char *ret = NULL;
   BMessage *msg;
   void *data;
   ssize_t len;

   if (BClipboard_Lock(be_clipboard)) {
      printf("locked\n");
      BClipboard_Clear(be_clipboard);
      msg = BClipboard_Data(be_clipboard);
      if (BMessage_FindData(msg, "text/plain", B_MIME_TYPE, &data, &len) == B_OK) {
         printf("found\n");
         ret = malloc(len+1);
         if (ret) {
            memcpy(ret, data, len);
            ret[len] = 0;
            printf("got data '%s'\n", ret);
         }
      }
      BClipboard_Unlock(be_clipboard);
      printf("unlocked\n");
   }
   return ret;
   */
   return NULL;
}


SystemFont *system_font_create(Heap *heap, plat_char *family, float size, int style)
{
   SystemFont *font;
   const char *style_str;

   font = (SystemFont *)calloc(1, sizeof(SystemFont));
   if (!font) return NULL;

   if (style & FONT_BOLD) {
      if (style & FONT_ITALIC) {
         style_str = "bold italic";
      }
      else {
         style_str = "bold";
      }
   }
   else {
      if (style & FONT_ITALIC) {
         style_str = "italic";
      }
      else {
         style_str = "plain";
      }
   }

   font->font = new(BFONT_SIZE);
   BFont_new(font->font);
   BFont_SetFamilyAndStyle(font->font, family, style_str);
   BFont_SetSize(font->font, size);
   return font;
}


void system_font_destroy(SystemFont *font)
{
   delete_static(font->font);
   free(font);
}


plat_char **system_font_get_list()
{
   char **list;
   font_family name;
   int i, cnt;

   cnt = count_font_families();
   list = calloc(cnt+1, sizeof(char *));
   for (i=0; i<cnt; i++) {
      get_font_family(i, &name, 0);
      list[i] = strdup(name);
   }
   return list;
}


int system_font_get_size(SystemFont *font)
{
   return (int)roundf(BFont_Size(font->font));
}


int system_font_get_ascent(SystemFont *font)
{
   font_height fh;
   BFont_GetHeight(font->font, &fh);
   return (int)roundf(fh.ascent);
}


int system_font_get_descent(SystemFont *font)
{
   font_height fh;
   BFont_GetHeight(font->font, &fh);
   return (int)roundf(fh.descent);
}


int system_font_get_height(SystemFont *font)
{
   font_height fh;
   BFont_GetHeight(font->font, &fh);
   return (int)roundf(fh.leading);
}


int system_font_get_string_advance(SystemFont *font, plat_char *s)
{
   return (int)ceilf(BFont_StringWidth(font->font, s));
}


float system_font_get_string_position(SystemFont *font, plat_char *text, int x)
{
   char *s;
   int width;
   int min, max, middle, w, w1, w2, pos;

   if (x < 0) return 0.0f;
   width = system_font_get_string_advance(font, text);
   if (x >= width) return (float)strlen(text);

   s = (char *)malloc(strlen(text)+1);

   min = 0;
   max = strlen(text);
   while (min < max) {
      middle = min+(max-min)/2;
      strcpy(s, text);
      s[middle] = 0;
      w = system_font_get_string_advance(font, s);
      if (w < x) {
         min = middle+1;
      }
      else {
         max = middle;
      }
   }

   pos = min-1;
   if (pos < 0) pos = 0;

   strcpy(s, text);
   s[pos+1] = 0;
   w2 = system_font_get_string_advance(font, s);
   s[pos] = 0;
   w1 = system_font_get_string_advance(font, s);

   free(s);

   return pos + (x - w1) / (float)(w2 - w1);
}


void system_font_draw_string(SystemFont *font, int x, int y, plat_char *text, uint32_t color, uint32_t *pixels, int dest_width, int dest_height, int dest_stride)
{
   font_height fh;
   float size;
   int offset, width, height, ascent, descent, off_x, off_y;
   int i, j;
   int a, r, g, b;
   uint32_t p;
   rgb_color rgb_color;
   BRect rect;
   BPoint point;
   void *bitmap, *view;

   BFont_GetHeight(font->font, &fh);
   size = BFont_Size(font->font);
   offset = (int)ceilf(0.2f*size);
   ascent = (int)ceilf(fh.ascent * 1.025f);
   descent = (int)ceilf(fh.descent * 1.1f);
   width = (int)ceilf(BFont_StringWidth(font->font, text)) + offset*2;
   height = ascent+descent;
   off_x = offset;
   off_y = ascent;

   x -= offset;
   y -= ascent;

   if (x+width <= 0 || x >= dest_width) return;
   if (x < 0) {
      off_x += x;
      width += x;
      x = 0;
   }
   if (x+width > dest_width) {
      width = dest_width - x;
      if (width <= 0) return;
   }

   if (y+height <= 0 || y >= dest_height) return;
   if (y < 0) {
      off_y += y;
      height += y;
      y = 0;
   }
   if (y+height > dest_height) {
      height = dest_height - y;
      if (height <= 0) return;
   }

   bitmap = new(BBITMAP_SIZE);
   rect.left = 0;
   rect.top = 0;
   rect.right = width-1;
   rect.bottom = height-1;
   BBitmap_new(bitmap, &rect, B_RGBA32, 1, 0);

   view = new(BVIEW_SIZE);
   BView_new(view, &rect, "", 0, 0);

   uint32_t *bits = (uint32_t *)BBitmap_Bits(bitmap);
   int stride = BBitmap_BytesPerRow(bitmap)/4;

   for (i=0; i<height; i++) {
      for (j=0; j<width; j++) {
         p = pixels[(y+i)*dest_stride+(x+j)];
         a = (p >> 24) & 0xFF;
         r = (p >> 16) & 0xFF;
         g = (p >>  8) & 0xFF;
         b = (p >>  0) & 0xFF;
         if (a != 0 && a != 255) {
            r = r * 255 / a;
            g = g * 255 / a;
            b = b * 255 / a;
         }
         bits[i*stride+j] = (a << 24) | (r << 16) | (g << 8) | b;
      }
   }

   rgb_color.alpha = (color >> 24) & 0xFF;
   rgb_color.red = (color >> 16) & 0xFF;
   rgb_color.green = (color >> 8) & 0xFF;
   rgb_color.blue = (color >> 0) & 0xFF;
   if (rgb_color.alpha != 0) {
      rgb_color.red = rgb_color.red * 255 / rgb_color.alpha;
      rgb_color.green = rgb_color.green * 255 / rgb_color.alpha;
      rgb_color.blue = rgb_color.blue * 255 / rgb_color.alpha;
   }

   BBitmap_Lock(bitmap);
   BBitmap_AddChild(bitmap, view);
   BView_SetHighColor(view, rgb_color);
   BView_SetFont(view, font->font, B_FONT_ALL);
   point.x = off_x;
   point.y = off_y;
   BView_DrawString(view, text, &point, NULL);
   BView_Sync(view);
   BBitmap_RemoveChild(bitmap, view);
   BBitmap_Unlock(bitmap);

   delete_virtual(view);
   delete_virtual(bitmap);

   for (i=0; i<height; i++) {
      for (j=0; j<width; j++) {
         p = pixels[(y+i)*dest_stride+(x+j)];
         a = (p >> 24) & 0xFF;
         r = (p >> 16) & 0xFF;
         g = (p >>  8) & 0xFF;
         b = (p >>  0) & 0xFF;
         if (a != 0 && a != 255) {
            r = r * 255 / a;
            g = g * 255 / a;
            b = b * 255 / a;
         }
         p = bits[i*stride+j];
         if (p != (uint32_t)((a << 24) | (r << 16) | (g << 8) | b)) {
            a = (p >> 24) & 0xFF;
            r = (p >> 16) & 0xFF;
            g = (p >>  8) & 0xFF;
            b = (p >>  0) & 0xFF;
            r = div255(r * a);
            g = div255(g * a);
            b = div255(b * a);
            pixels[(y+i)*dest_stride+(x+j)] = (a << 24) | (r << 16) | (g << 8) | b;
         }
      }
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
   BLooper_Lock(app);
   BApplication_Quit(app);
   BLooper_Unlock(app);
}


static Value func_common_show_async_message(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   char *title = NULL, *msg = NULL;
   int err, type;
   void *alert;
   void *bmsg;
   void *invoker;

   type = fixscript_get_int(params[1]);

   err = fixscript_get_string(heap, params[2], 0, -1, (char **)&title, NULL);
   if (!err) {
      err = fixscript_get_string(heap, params[3], 0, -1, (char **)&msg, NULL);
   }
   if (err) {
      fixscript_error(heap, error, err);
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

   alert = create_alert(type, title, msg);

   bmsg = new(BMESSAGE_SIZE);
   BMessage_new(bmsg, MSG_ASYNC_MSG_RESULT);
   BMessage_AddInt32(bmsg, "type", type);
   BMessage_AddPointer(bmsg, "heap", heap);
   AddValue(bmsg, "func", params[4]);
   AddValue(bmsg, "data", params[5]);
   fixscript_ref(heap, params[5]);

   invoker = new(BINVOKER_SIZE);
   BInvoker_new(invoker, bmsg, script_looper, NULL);

   BAlert_Go_invoker(alert, invoker);

error:
   free(title);
   free(msg);
   return fixscript_int(0);
}


static Value func_haiku_is_present(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   return fixscript_int(1);
}


void register_platform_gui_functions(Heap *heap)
{
   fixscript_register_native_func(heap, "common_show_async_message#6", func_common_show_async_message, NULL);
   fixscript_register_native_func(heap, "haiku_is_present#0", func_haiku_is_present, NULL);
}


int main(int argc, char **argv)
{
   pthread_t thread;
   void *lib, **vtable, *msg;

   lib = dlopen("libbe.so", RTLD_LAZY);
   if (!lib) {
      fprintf(stderr, "can't open library libbe.so\n");
      return 1;
   }

   #define SYM(name, sym) \
      name = dlsym(lib, sym); \
      if (!name) { \
         fprintf(stderr, "can't find symbol %s: %s\n", #name, sym); \
         return 1; \
      }

   #ifdef __LP64__
   #define SYM2(name, sym_32, sym_64) SYM(name, sym_64)
   #else
   #define SYM2(name, sym_32, sym_64) SYM(name, sym_32)
   #endif

   SYM(operator_new, "_Znwm");
   SYM(operator_delete, "_ZdlPv");
   SYM(BApplication_new, "_ZN12BApplicationC1EPKc");
   SYM2(BLooper_new, "_ZN7BLooperC1EPKcll", "_ZN7BLooperC1EPKcii");
   SYM(BLooper_PostMessage, "_ZN7BLooper11PostMessageEP8BMessage");
   SYM2(BLooper_PostMessage_id, "_ZN7BLooper11PostMessageEm", "_ZN7BLooper11PostMessageEj");
   SYM(BLooper_MessageReceived, "_ZN7BLooper15MessageReceivedEP8BMessage");
   SYM(BLooper_Lock, "_ZN7BLooper4LockEv");
   SYM(BLooper_Unlock, "_ZN7BLooper6UnlockEv");
   SYM(BLooper_CurrentMessage, "_ZNK7BLooper14CurrentMessageEv");
   SYM2(BMessage_new, "_ZN8BMessageC1Em", "_ZN8BMessageC1Ej");
   SYM2(BMessage_AddInt32, "_ZN8BMessage8AddInt32EPKcl", "_ZN8BMessage8AddInt32EPKci");
   SYM(BMessage_AddPointer, "_ZN8BMessage10AddPointerEPKcPKv");
   SYM(BMessage_AddBool, "_ZN8BMessage7AddBoolEPKcb");
   SYM2(BMessage_AddData, "_ZN8BMessage7AddDataEPKcmPKvlbl", "_ZN8BMessage7AddDataEPKcjPKvlbi");
   SYM2(BMessage_GetInt32, "_ZNK8BMessage8GetInt32EPKcl", "_ZNK8BMessage8GetInt32EPKci");
   SYM(BMessage_GetPointer, "_ZNK8BMessage10GetPointerEPKcPKv");
   SYM(BMessage_GetBool, "_ZNK8BMessage7GetBoolEPKcb");
   SYM2(BMessage_FindData, "_ZNK8BMessage8FindDataEPKcmPPKvPl", "_ZNK8BMessage8FindDataEPKcjPPKvPl");
   SYM(BMessage_FindPoint, "_ZNK8BMessage9FindPointEPKcP6BPoint");
   SYM(BMessage_FindFloat, "_ZNK8BMessage9FindFloatEPKcPf");
   SYM(BMessage_PrintToStream, "_ZNK8BMessage13PrintToStreamEv");
   SYM2(BWindow_new, "_ZN7BWindowC1E5BRectPKc11window_typemm", "_ZN7BWindowC1E5BRectPKc11window_typejj");
   SYM(BWindow_Bounds, "_ZNK7BWindow6BoundsEv");
   SYM(BWindow_AddChild, "_ZN7BWindow8AddChildEP5BViewS1_");
   SYM(BWindow_CenterOnScreen, "_ZN7BWindow14CenterOnScreenEv");
   SYM(BWindow_Zoom, "_ZN7BWindow4ZoomEv");
   SYM(BWindow_Frame, "_ZNK7BWindow5FrameEv");
   SYM(BWindow_Title, "_ZNK7BWindow5TitleEv");
   SYM(BWindow_SetTitle, "_ZN7BWindow8SetTitleEPKc");
   SYM(BWindow_SetSizeLimits, "_ZN7BWindow13SetSizeLimitsEffff");
   SYM(BHandler_Looper, "_ZNK8BHandler6LooperEv");
   SYM(BHandler_LockLooper, "_ZN8BHandler10LockLooperEv");
   SYM(BHandler_UnlockLooper, "_ZN8BHandler12UnlockLooperEv");
   SYM2(BView_new, "_ZN5BViewC1E5BRectPKcmm", "_ZN5BViewC1E5BRectPKcjj");
   SYM(BView_SetViewUIColor, "_ZN5BView14SetViewUIColorE11color_whichf");
   SYM(BView_Bounds, "_ZNK5BView6BoundsEv");
   SYM(BView_Frame, "_ZNK5BView5FrameEv");
   SYM(BView_MoveTo, "_ZN5BView6MoveToEff");
   SYM(BView_ResizeTo, "_ZN5BView8ResizeToEff");
   SYM(BView_AddChild, "_ZN5BView8AddChildEPS_S0_");
   SYM(BView_IsFocus, "_ZNK5BView7IsFocusEv");
   SYM(BView_DrawBitmap, "_ZN5BView10DrawBitmapEPK7BBitmap5BRect");
   SYM(BView_Flags, "_ZNK5BView5FlagsEv");
   SYM2(BView_SetMouseEventMask, "_ZN5BView17SetMouseEventMaskEmm", "_ZN5BView17SetMouseEventMaskEjj");
   SYM(modifiers, "_Z9modifiersv");
   SYM(BView_Invalidate, "_ZN5BView10InvalidateEv");
   SYM(BView_Invalidate_rect, "_ZN5BView10InvalidateE5BRect");
   SYM(BView_ConvertToScreen, "_ZNK5BView15ConvertToScreenEP6BPoint");
   SYM(BView_DrawString, "_ZN5BView10DrawStringEPKc6BPointP16escapement_delta");
   SYM(BView_Sync, "_ZNK5BView4SyncEv");
   SYM(BView_Window, "_ZNK5BView6WindowEv");
   SYM(BBitmap_new, "_ZN7BBitmapC1E5BRect11color_spacebb");
   SYM(BBitmap_Bounds, "_ZNK7BBitmap6BoundsEv");
   SYM(BBitmap_BytesPerRow, "_ZNK7BBitmap11BytesPerRowEv");
   SYM(BBitmap_Bits, "_ZNK7BBitmap4BitsEv");
   SYM(BBitmap_Lock, "_ZN7BBitmap4LockEv");
   SYM(BBitmap_Unlock, "_ZN7BBitmap6UnlockEv");
   SYM2(BStringView_new, "_ZN11BStringViewC1E5BRectPKcS2_mm", "_ZN11BStringViewC1E5BRectPKcS2_jj");
   SYM(BStringView_Text, "_ZNK11BStringView4TextEv");
   SYM(BStringView_SetText, "_ZN11BStringView7SetTextEPKc");
   SYM2(BTextControl_new, "_ZN12BTextControlC1E5BRectPKcS2_S2_P8BMessagemm", "_ZN12BTextControlC1E5BRectPKcS2_S2_P8BMessagejj");
   SYM(BTextControl_Text, "_ZNK12BTextControl4TextEv");
   SYM2(BButton_new, "_ZN7BButtonC1E5BRectPKcS2_P8BMessagemm", "_ZN7BButtonC1E5BRectPKcS2_P8BMessagejj");
   SYM(BControl_Label, "_ZNK8BControl5LabelEv");
   SYM2(BScrollView_new, "_ZN11BScrollViewC1EPKcP5BViewmbb12border_style", "_ZN11BScrollViewC1EPKcP5BViewjbb12border_style");
   SYM(BScrollView_ScrollBar, "_ZNK11BScrollView9ScrollBarE11orientation");
   SYM(BScrollBar_SetRange, "_ZN10BScrollBar8SetRangeEff");
   SYM(BScrollBar_SetValue, "_ZN10BScrollBar8SetValueEf");
   SYM(BScrollBar_SetProportion, "_ZN10BScrollBar13SetProportionEf");
   SYM(BScrollBar_SetSteps, "_ZN10BScrollBar8SetStepsEff");
   SYM(BScrollBar_Value, "_ZNK10BScrollBar5ValueEv");
   SYM(BPopUpMenu_new, "_ZN10BPopUpMenuC1EPKcbb11menu_layout");
   SYM(BPopUpMenu_SetAsyncAutoDestruct, "_ZN10BPopUpMenu20SetAsyncAutoDestructEb");
   SYM(BPopUpMenu_Go, "_ZN10BPopUpMenu2GoE6BPointbbb");
   SYM2(BMenuBar_new, "_ZN8BMenuBarC1E5BRectPKcm11menu_layoutb", "_ZN8BMenuBarC1E5BRectPKcj11menu_layoutb");
   SYM(BMenu_AddItem_menu, "_ZN5BMenu7AddItemEPS_");
   SYM2(BMenu_AddItem_menu_idx, "_ZN5BMenu7AddItemEPS_l", "_ZN5BMenu7AddItemEPS_i");
   SYM(BMenu_AddItem_item, "_ZN5BMenu7AddItemEP9BMenuItem");
   SYM2(BMenu_AddItem_item_idx, "_ZN5BMenu7AddItemEP9BMenuIteml", "_ZN5BMenu7AddItemEP9BMenuItemi");
   SYM(BMenu_AddSeparatorItem, "_ZN5BMenu16AddSeparatorItemEv");
   SYM2(BMenu_RemoveItem, "_ZN5BMenu10RemoveItemEl", "_ZN5BMenu10RemoveItemEi");
   SYM2(BMenuItem_new, "_ZN9BMenuItemC1EPKcP8BMessagecm", "_ZN9BMenuItemC1EPKcP8BMessagecj");
   SYM(BSeparatorItem_new, "_ZN14BSeparatorItemC1Ev");
   SYM(BAlert_new, "_ZN6BAlertC1EPKcS1_S1_S1_S1_12button_width14button_spacing10alert_type");
   SYM(BAlert_Go, "_ZN6BAlert2GoEv");
   SYM(BAlert_Go_invoker, "_ZN6BAlert2GoEP8BInvoker");
   SYM(BInvoker_new, "_ZN8BInvokerC1EP8BMessagePK8BHandlerPK7BLooper");
   SYM(BFont_new, "_ZN5BFontC1Ev");
   SYM(BFont_SetFamilyAndStyle, "_ZN5BFont17SetFamilyAndStyleEPKcS1_");
   SYM(BFont_SetSize, "_ZN5BFont7SetSizeEf");
   SYM(BFont_Size, "_ZNK5BFont4SizeEv");
   SYM(BFont_GetHeight, "_ZNK5BFont9GetHeightEP11font_height");
   SYM(BFont_StringWidth, "_ZNK5BFont11StringWidthEPKc");
   SYM(B_TRANSPARENT_COLOR, "B_TRANSPARENT_COLOR");
   SYM(count_font_families, "_Z19count_font_familiesv");
   SYM2(get_font_family, "_Z15get_font_familylPA64_cPm", "_Z15get_font_familyiPA64_cPj");
   SYM(be_clipboard, "be_clipboard");
   SYM(BClipboard_Lock, "_ZN10BClipboard4LockEv");
   SYM(BClipboard_Unlock, "_ZN10BClipboard6UnlockEv");
   SYM(BClipboard_Clear, "_ZN10BClipboard5ClearEv");
   SYM(BClipboard_Data, "_ZNK10BClipboard4DataEv");
   SYM(BClipboard_Commit, "_ZN10BClipboard6CommitEv");

   pthread_condattr_init(&cond_attr);
   pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
   pthread_mutex_init(&mutex, NULL);
   pthread_cond_init(&cond, &cond_attr);
   pthread_mutex_init(&timer_mutex, NULL);
   pthread_cond_init(&timer_cond, &cond_attr);
   pthread_cond_init(&timer_cond_processed, &cond_attr);
   fixgui__tls_init();

   if (pthread_create(&thread, NULL, timer_thread, NULL) != 0) {
      return 1;
   }
   pthread_detach(thread);

   app = new(BAPPLICATION_SIZE);
   BApplication_new(app, "application/x-vnd.FixGUI-Application");

   script_looper = new(BLOOPER_SIZE);
   BLooper_new(script_looper, "main thread", B_NORMAL_PRIORITY, B_LOOPER_PORT_DEFAULT_CAPACITY);
   vtable = clone_vtable(script_looper, 2, 35);
   vtable[2+7] = script_looper_message_received;
   *(void ***)script_looper = vtable+2;
   BLooper_Run(script_looper);
   BLooper_PostMessage_id(script_looper, 0x12345678);

   msg = new(BMESSAGE_SIZE);
   BMessage_new(msg, MSG_INIT_APP);
   BMessage_AddInt32(msg, "argc", argc);
   BMessage_AddPointer(msg, "argv", argv);
   BLooper_PostMessage(script_looper, msg);
   delete_virtual(msg);

   BLooper_Run(app);

   delete_virtual(app);
   return 0;

   #undef SYM
   #undef SYM2
}
