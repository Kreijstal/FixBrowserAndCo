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
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#include "fixgui_common.h"

typedef enum {
   G_CONNECT_AFTER   = 1 << 0,
   G_CONNECT_SWAPPED = 1 << 1
} GConnectFlags;

typedef enum {
   CAIRO_FORMAT_INVALID   = -1,
   CAIRO_FORMAT_ARGB32    = 0,
   CAIRO_FORMAT_RGB24     = 1,
   CAIRO_FORMAT_A8        = 2,
   CAIRO_FORMAT_A1        = 3,
   CAIRO_FORMAT_RGB16_565 = 4,
   CAIRO_FORMAT_RGB30     = 5
} cairo_format_t;

typedef enum {
   CAIRO_FONT_SLANT_NORMAL,
   CAIRO_FONT_SLANT_ITALIC,
   CAIRO_FONT_SLANT_OBLIQUE
} cairo_font_slant_t;

typedef enum {
   CAIRO_FONT_WEIGHT_NORMAL,
   CAIRO_FONT_WEIGHT_BOLD
} cairo_font_weight_t;

typedef struct {
   double ascent;
   double descent;
   double height;
   double max_x_advance;
   double max_y_advance;
} cairo_font_extents_t;

typedef struct {
   double x_bearing;
   double y_bearing;
   double width;
   double height;
   double x_advance;
   double y_advance;
} cairo_text_extents_t;

typedef struct PangoContext PangoContext;
typedef struct PangoFontFamily PangoFontFamily; 

typedef enum {
   GTK_WINDOW_TOPLEVEL,
   GTK_WINDOW_POPUP
} GtkWindowType;

typedef enum {
   GDK_NOTHING           = -1,
   GDK_DELETE            = 0,
   GDK_DESTROY           = 1,
   GDK_EXPOSE            = 2,
   GDK_MOTION_NOTIFY     = 3,
   GDK_BUTTON_PRESS      = 4,
   GDK_2BUTTON_PRESS     = 5,
   GDK_3BUTTON_PRESS     = 6,
   GDK_BUTTON_RELEASE    = 7,
   GDK_KEY_PRESS         = 8,
   GDK_KEY_RELEASE       = 9,
   GDK_ENTER_NOTIFY      = 10,
   GDK_LEAVE_NOTIFY      = 11,
   GDK_FOCUS_CHANGE      = 12,
   GDK_CONFIGURE         = 13,
   GDK_MAP               = 14,
   GDK_UNMAP             = 15,
   GDK_PROPERTY_NOTIFY   = 16,
   GDK_SELECTION_CLEAR   = 17,
   GDK_SELECTION_REQUEST = 18,
   GDK_SELECTION_NOTIFY  = 19,
   GDK_PROXIMITY_IN      = 20,
   GDK_PROXIMITY_OUT     = 21,
   GDK_DRAG_ENTER        = 22,
   GDK_DRAG_LEAVE        = 23,
   GDK_DRAG_MOTION       = 24,
   GDK_DRAG_STATUS       = 25,
   GDK_DROP_START        = 26,
   GDK_DROP_FINISHED     = 27,
   GDK_CLIENT_EVENT      = 28,
   GDK_VISIBILITY_NOTIFY = 29,
   GDK_NO_EXPOSE         = 30,
   GDK_SCROLL            = 31,
   GDK_WINDOW_STATE      = 32,
   GDK_SETTING           = 33
} GdkEventType;

typedef enum {
   GDK_EXPOSURE_MASK            = 1 << 1,
   GDK_POINTER_MOTION_MASK      = 1 << 2,
   GDK_POINTER_MOTION_HINT_MASK = 1 << 3,
   GDK_BUTTON_MOTION_MASK       = 1 << 4,
   GDK_BUTTON1_MOTION_MASK      = 1 << 5,
   GDK_BUTTON2_MOTION_MASK      = 1 << 6,
   GDK_BUTTON3_MOTION_MASK      = 1 << 7,
   GDK_BUTTON_PRESS_MASK        = 1 << 8,
   GDK_BUTTON_RELEASE_MASK      = 1 << 9,
   GDK_KEY_PRESS_MASK           = 1 << 10,
   GDK_KEY_RELEASE_MASK         = 1 << 11,
   GDK_ENTER_NOTIFY_MASK        = 1 << 12,
   GDK_LEAVE_NOTIFY_MASK        = 1 << 13,
   GDK_FOCUS_CHANGE_MASK        = 1 << 14,
   GDK_STRUCTURE_MASK           = 1 << 15,
   GDK_PROPERTY_CHANGE_MASK     = 1 << 16,
   GDK_VISIBILITY_NOTIFY_MASK   = 1 << 17,
   GDK_PROXIMITY_IN_MASK        = 1 << 18,
   GDK_PROXIMITY_OUT_MASK       = 1 << 19,
   GDK_SUBSTRUCTURE_MASK        = 1 << 20,
   GDK_SCROLL_MASK              = 1 << 21,
   GDK_ALL_EVENTS_MASK          = 0x3FFFFE
} GdkEventMask;

typedef enum {
   GDK_SHIFT_MASK    = 1 << 0,
   GDK_LOCK_MASK     = 1 << 1,
   GDK_CONTROL_MASK  = 1 << 2,
   GDK_MOD1_MASK     = 1 << 3,
   GDK_MOD2_MASK     = 1 << 4,
   GDK_MOD3_MASK     = 1 << 5,
   GDK_MOD4_MASK     = 1 << 6,
   GDK_MOD5_MASK     = 1 << 7,
   GDK_BUTTON1_MASK  = 1 << 8,
   GDK_BUTTON2_MASK  = 1 << 9,
   GDK_BUTTON3_MASK  = 1 << 10,
   GDK_BUTTON4_MASK  = 1 << 11,
   GDK_BUTTON5_MASK  = 1 << 12,
   GDK_RELEASE_MASK  = 1 << 30,
   GDK_MODIFIER_MASK = GDK_RELEASE_MASK | 0x1fff
} GdkModifierType;

typedef enum {
   GTK_POLICY_ALWAYS,
   GTK_POLICY_AUTOMATIC,
   GTK_POLICY_NEVER
} GtkPolicyType;

typedef enum {
   GDK_HINT_POS         = 1 << 0,
   GDK_HINT_MIN_SIZE    = 1 << 1,
   GDK_HINT_MAX_SIZE    = 1 << 2,
   GDK_HINT_BASE_SIZE   = 1 << 3,
   GDK_HINT_ASPECT      = 1 << 4,
   GDK_HINT_RESIZE_INC  = 1 << 5,
   GDK_HINT_WIN_GRAVITY = 1 << 6,
   GDK_HINT_USER_POS    = 1 << 7,
   GDK_HINT_USER_SIZE   = 1 << 8
} GdkWindowHints;

typedef enum {
   GDK_GRAVITY_NORTH_WEST = 1,
   GDK_GRAVITY_NORTH,
   GDK_GRAVITY_NORTH_EAST,
   GDK_GRAVITY_WEST,
   GDK_GRAVITY_CENTER,
   GDK_GRAVITY_EAST,
   GDK_GRAVITY_SOUTH_WEST,
   GDK_GRAVITY_SOUTH,
   GDK_GRAVITY_SOUTH_EAST,
   GDK_GRAVITY_STATIC
} GdkGravity; 

typedef enum {
   GDK_CROSSING_NORMAL,
   GDK_CROSSING_GRAB,
   GDK_CROSSING_UNGRAB
} GdkCrossingMode;

typedef enum {
   GDK_NOTIFY_ANCESTOR          = 0,
   GDK_NOTIFY_VIRTUAL           = 1,
   GDK_NOTIFY_INFERIOR          = 2,
   GDK_NOTIFY_NONLINEAR         = 3,
   GDK_NOTIFY_NONLINEAR_VIRTUAL = 4,
   GDK_NOTIFY_UNKNOWN           = 5
} GdkNotifyType;

typedef enum {
   GDK_SCROLL_UP,
   GDK_SCROLL_DOWN,
   GDK_SCROLL_LEFT,
   GDK_SCROLL_RIGHT
} GdkScrollDirection;

typedef enum {
   GDK_X_CURSOR            = 0,
   GDK_ARROW               = 2,
   GDK_BASED_ARROW_DOWN    = 4,
   GDK_BASED_ARROW_UP      = 6,
   GDK_BOAT                = 8,
   GDK_BOGOSITY            = 10,
   GDK_BOTTOM_LEFT_CORNER  = 12,
   GDK_BOTTOM_RIGHT_CORNER = 14,
   GDK_BOTTOM_SIDE         = 16,
   GDK_BOTTOM_TEE          = 18,
   GDK_BOX_SPIRAL          = 20,
   GDK_CENTER_PTR          = 22,
   GDK_CIRCLE              = 24,
   GDK_CLOCK               = 26,
   GDK_COFFEE_MUG          = 28,
   GDK_CROSS               = 30,
   GDK_CROSS_REVERSE       = 32,
   GDK_CROSSHAIR           = 34,
   GDK_DIAMOND_CROSS       = 36,
   GDK_DOT                 = 38,
   GDK_DOTBOX              = 40,
   GDK_DOUBLE_ARROW        = 42,
   GDK_DRAFT_LARGE         = 44,
   GDK_DRAFT_SMALL         = 46,
   GDK_DRAPED_BOX          = 48,
   GDK_EXCHANGE            = 50,
   GDK_FLEUR               = 52,
   GDK_GOBBLER             = 54,
   GDK_GUMBY               = 56,
   GDK_HAND1               = 58,
   GDK_HAND2               = 60,
   GDK_HEART               = 62,
   GDK_ICON                = 64,
   GDK_IRON_CROSS          = 66,
   GDK_LEFT_PTR            = 68,
   GDK_LEFT_SIDE           = 70,
   GDK_LEFT_TEE            = 72,
   GDK_LEFTBUTTON          = 74,
   GDK_LL_ANGLE            = 76,
   GDK_LR_ANGLE            = 78,
   GDK_MAN                 = 80,
   GDK_MIDDLEBUTTON        = 82,
   GDK_MOUSE               = 84,
   GDK_PENCIL              = 86,
   GDK_PIRATE              = 88,
   GDK_PLUS                = 90,
   GDK_QUESTION_ARROW      = 92,
   GDK_RIGHT_PTR           = 94,
   GDK_RIGHT_SIDE          = 96,
   GDK_RIGHT_TEE           = 98,
   GDK_RIGHTBUTTON         = 100,
   GDK_RTL_LOGO            = 102,
   GDK_SAILBOAT            = 104,
   GDK_SB_DOWN_ARROW       = 106,
   GDK_SB_H_DOUBLE_ARROW   = 108,
   GDK_SB_LEFT_ARROW       = 110,
   GDK_SB_RIGHT_ARROW      = 112,
   GDK_SB_UP_ARROW         = 114,
   GDK_SB_V_DOUBLE_ARROW   = 116,
   GDK_SHUTTLE             = 118,
   GDK_SIZING              = 120,
   GDK_SPIDER              = 122,
   GDK_SPRAYCAN            = 124,
   GDK_STAR                = 126,
   GDK_TARGET              = 128,
   GDK_TCROSS              = 130,
   GDK_TOP_LEFT_ARROW      = 132,
   GDK_TOP_LEFT_CORNER     = 134,
   GDK_TOP_RIGHT_CORNER    = 136,
   GDK_TOP_SIDE            = 138,
   GDK_TOP_TEE             = 140,
   GDK_TREK                = 142,
   GDK_UL_ANGLE            = 144,
   GDK_UMBRELLA            = 146,
   GDK_UR_ANGLE            = 148,
   GDK_WATCH               = 150,
   GDK_XTERM               = 152,
   GDK_LAST_CURSOR,
   GDK_BLANK_CURSOR        = -2,
   GDK_CURSOR_IS_PIXMAP    = -1
} GdkCursorType;

typedef char gchar;
typedef short gshort;
typedef long glong;
typedef int gint;
typedef int8_t gint8;
typedef int16_t gint16;
typedef uint8_t guint8;
typedef uint16_t guint16;
typedef uint32_t guint32;
typedef gint gboolean;
typedef unsigned char guchar;
typedef unsigned short gushort;
typedef unsigned long gulong;
typedef unsigned int guint;
typedef float gfloat;
typedef double gdouble;
typedef void *gpointer;
typedef void *GCallback;
typedef void *GClosureNotify;
typedef void GdkWindow;
typedef void GdkRegion;
typedef void GdkDevice;
typedef void GdkCursor;
typedef void GdkPixmap;
typedef void GdkBitmap;
typedef void GtkSettings;

typedef struct {
   void *class;
   guint ref_count;
   void *qdata;
} GObject;

typedef gboolean (*GSourceFunc)(gpointer data);

typedef struct {
   gint x;
   gint y;
   gint width;
   gint height;
} GdkRectangle;

typedef struct {
   GdkEventType type;
   GdkWindow *window;
   gint8 send_event;
   GdkRectangle area;
   GdkRegion *region;
   gint count;
} GdkEventExpose;

typedef struct {
   GdkEventType type;
   GdkWindow *window;
   gint8 send_event;
   gint x, y;
   gint width;
   gint height;
} GdkEventConfigure;

typedef struct {
   GdkEventType type;
   GdkWindow *window;
   gint8 send_event;
   guint32 time;
   gdouble x;
   gdouble y;
   gdouble *axes;
   guint state;
   gint16 is_hint;
   GdkDevice *device;
   gdouble x_root, y_root;
} GdkEventMotion;

typedef struct {
   GdkEventType type;
   GdkWindow *window;
   gint8 send_event;
   guint32 time;
   gdouble x;
   gdouble y;
   gdouble *axes;
   guint state;
   guint button;
   GdkDevice *device;
   gdouble x_root, y_root;
} GdkEventButton;

typedef struct {
   GdkEventType type;
   GdkWindow *window;
   gint8 send_event;
   guint32 time;
   gdouble x;
   gdouble y;
   guint state;
   GdkScrollDirection direction;
   GdkDevice *device;
   gdouble x_root, y_root;
} GdkEventScroll;

typedef struct {
   GdkEventType type;
   GdkWindow *window;
   gint8 send_event;
   guint32 time;
   guint state;
   guint keyval;
   gint length;
   gchar *string;
   guint16 hardware_keycode;
   guint8 group;
} GdkEventKey;

typedef struct {
   GdkEventType type;
   GdkWindow *window;
   gint8 send_event;
   GdkWindow *subwindow;
   guint32 time;
   gdouble x;
   gdouble y;
   gdouble x_root;
   gdouble y_root;
   GdkCrossingMode mode;
   GdkNotifyType detail;
   gboolean focus;
   guint state;
} GdkEventCrossing;

typedef union {
   GdkEventType type;
   GdkEventExpose expose;
   GdkEventConfigure configure;
   GdkEventMotion motion;
   GdkEventButton button;
   GdkEventScroll scroll;
   GdkEventKey key;
   GdkEventCrossing crossing;
} GdkEvent;

typedef struct {
   gint min_width;
   gint min_height;
   gint max_width;
   gint max_height;
   gint base_width;
   gint base_height;
   gint width_inc;
   gint height_inc;
   gdouble min_aspect;
   gdouble max_aspect;
   GdkGravity win_gravity;
} GdkGeometry;

typedef struct {
   guint32 pixel;
   guint16 red;
   guint16 green;
   guint16 blue;
} GdkColor;

typedef struct cairo_t cairo_t;
typedef struct cairo_surface_t cairo_surface_t;
typedef struct cairo_pattern_t cairo_pattern_t;
typedef struct cairo_font_face_t cairo_font_face_t;

typedef enum {
   GTK_TOPLEVEL         = 1 << 4,
   GTK_NO_WINDOW        = 1 << 5,
   GTK_REALIZED         = 1 << 6,
   GTK_MAPPED           = 1 << 7,
   GTK_VISIBLE          = 1 << 8,
   GTK_SENSITIVE        = 1 << 9,
   GTK_PARENT_SENSITIVE = 1 << 10,
   GTK_CAN_FOCUS        = 1 << 11,
   GTK_HAS_FOCUS        = 1 << 12,
   GTK_CAN_DEFAULT      = 1 << 13,
   GTK_HAS_DEFAULT      = 1 << 14,
   GTK_HAS_GRAB         = 1 << 15,
   GTK_RC_STYLE         = 1 << 16,
   GTK_COMPOSITE_CHILD  = 1 << 17,
   GTK_NO_REPARENT      = 1 << 18,
   GTK_APP_PAINTABLE    = 1 << 19,
   GTK_RECEIVES_DEFAULT = 1 << 20,
   GTK_DOUBLE_BUFFERED  = 1 << 21
} GtkWidgetFlags;

typedef struct {
   GObject parent_instance;
   uint32_t flags;
} GtkObject;

typedef struct {
   gint width;
   gint height;
} GtkRequisition;

typedef GdkRectangle GtkAllocation;

typedef struct GtkWidget {
   GtkObject object;
   uint16_t private_flags;
   uint8_t state;
   uint8_t saved_state;
   char *name;
   void *style;
   GtkRequisition requisition;
   GtkAllocation allocation;
   GdkWindow *window;
   struct GtkWidget *parent;
} GtkWidget;

typedef struct {
   GtkWidget widget;
} GtkWindow;

typedef struct {
   GtkWidget widget;
} GtkFixed;

typedef struct {
   GtkWidget widget;
} GtkContainer;

typedef struct {
   GtkWidget widget;
} GtkButton;

typedef struct {
   GtkObject parent_instance;
   gdouble lower;
   gdouble upper;
   gdouble value;
   gdouble step_increment;
   gdouble page_increment;
   gdouble page_size;
} GtkAdjustment;

typedef struct {
   GtkWidget widget;
} GtkScrolledWindow;

typedef struct {
   GtkWidget widget;
} GtkRange;

typedef struct {
   GtkWidget widget;
} GtkLabel;

typedef struct {
   GtkWidget widget;
} GtkEntry;

typedef struct {
   GtkWidget widget;
} GtkMisc;

typedef struct {
   gulong g_type;
} GTypeClass;

typedef struct {
   GTypeClass g_type_class;
   void *priv[1];
   void *funcs[7];
   void *dummy[8];
} GObjectClass;

typedef struct {
   GObjectClass parent_class;
   guint activate_signal;
   void *funcs1[9];
   void (*size_allocate)(GtkWidget *widget, GtkAllocation *allocation);
   void *funcs2[10];
   void (*get_preferred_height)(GtkWidget *widget, gint *minimum_height, gint *natural_height);
   void (*get_preferred_width_for_height)(GtkWidget *widget, gint height, gint *minimum_width, gint *natural_width);
   void (*get_preferred_width)(GtkWidget *widget, gint *minimum_width, gint *natural_width);
   void (*get_preferred_height_for_width)(GtkWidget *widget, gint width, gint *minimum_height, gint *natural_height);
} GtkWidgetClass;

static int version;

static void (*g_free)(gpointer mem);

static gpointer (*g_object_ref)(gpointer object);
static void (*g_object_unref)(gpointer object);
static void (*g_object_get)(gpointer object, const gchar *first_property_name, ...);
static gulong (*g_signal_connect_data)(gpointer instance, const gchar *detailed_signal, GCallback c_handler, gpointer data, GClosureNotify destroy_data, GConnectFlags connect_flags);
#define g_signal_connect(instance, detailed_signal, c_handler, data) g_signal_connect_data(instance, detailed_signal, c_handler, data, NULL, 0)
static guint (*g_idle_add)(GSourceFunc function, gpointer data);
static guint (*g_timeout_add)(guint32 interval, GSourceFunc function, gpointer data);

static cairo_t *(*gdk_cairo_create)(GdkWindow *window);
static PangoContext *(*gdk_pango_context_get)();
static GdkCursor *(*gdk_cursor_new)(GdkCursorType cursor_type);
static GdkCursor *(*gdk_cursor_new_from_pixmap)(GdkPixmap *source, GdkPixmap *mask, GdkColor *fg, GdkColor *bg, gint x, gint y);
static GdkBitmap *(*gdk_bitmap_create_from_data)(GdkWindow *window, const gchar *data, gint width, gint height);
static void (*gdk_window_set_cursor)(GdkWindow *window, GdkCursor *cursor);

static void (*pango_context_list_families)(PangoContext *context, PangoFontFamily ***families, int *n_families);
static const char *(*pango_font_family_get_name)(PangoFontFamily *family);

static cairo_t *(*cairo_create)(cairo_surface_t *target);
static void (*cairo_clip_extents)(cairo_t *cr, double *x1, double *y1, double *x2, double *y2);
static void (*cairo_translate)(cairo_t *cr, double tx, double ty);
static void (*cairo_set_source_rgba)(cairo_t *cr, double red, double green, double blue, double alpha);
static void (*cairo_set_source)(cairo_t *cr, cairo_pattern_t *source);
static void (*cairo_rectangle)(cairo_t *cr, double x, double y, double width, double height);
static void (*cairo_fill)(cairo_t *cr);
static void (*cairo_paint)(cairo_t *cr);
static void (*cairo_destroy)(cairo_t *cr);
static void (*cairo_select_font_face)(cairo_t *cr, const char *family, cairo_font_slant_t slant, cairo_font_weight_t weight);
static cairo_font_face_t *(*cairo_get_font_face)(cairo_t *cr);
static void (*cairo_set_font_face)(cairo_t *cr, cairo_font_face_t *font_face);
static void (*cairo_set_font_size)(cairo_t *cr, double size);
static void (*cairo_show_text)(cairo_t *cr, const char *utf8);
static void (*cairo_font_extents)(cairo_t *cr, cairo_font_extents_t *extents);
static void (*cairo_text_extents)(cairo_t *cr, const char *utf8, cairo_text_extents_t *extents);
static cairo_font_face_t *(*cairo_font_face_reference)(cairo_font_face_t *font_face);
static void (*cairo_font_face_destroy)(cairo_font_face_t *font_face);
static int (*cairo_format_stride_for_width)(cairo_format_t format, int width);
static cairo_surface_t *(*cairo_image_surface_create)(cairo_format_t format, int width, int height);
static cairo_surface_t *(*cairo_image_surface_create_for_data)(unsigned char *data, cairo_format_t format, int width, int height, int stride);
static unsigned char *(*cairo_image_surface_get_data)(cairo_surface_t *surface);
static int (*cairo_image_surface_get_stride)(cairo_surface_t *surface);
static void (*cairo_surface_mark_dirty)(cairo_surface_t *surface);
static void (*cairo_surface_destroy)(cairo_surface_t *surface);
static cairo_pattern_t *(*cairo_pattern_create_for_surface)(cairo_surface_t *surface);
static void (*cairo_pattern_destroy)(cairo_pattern_t *pattern);

static void (*gtk_init)(int *argc, char ***argv);
static gboolean (*gtk_init_check)(int *argc, char ***argv);
static void (*gtk_main)();
static void (*gtk_main_quit)();
static GtkWidget *(*gtk_window_new)(GtkWindowType type);
static void (*gtk_window_set_default)(GtkWindow *window, GtkWidget *default_widget);
static void (*gtk_window_set_title)(GtkWindow *window, const char *title);
static const char *(*gtk_window_get_title)(GtkWindow *window);
static void (*gtk_window_set_resizable)(GtkWindow *window, gboolean resizable);
static void (*gtk_window_set_default_size)(GtkWindow *window, gint width, gint height);
static void (*gtk_window_resize)(GtkWindow *window, gint width, gint height);
static void (*gtk_widget_set_size_request)(GtkWidget *widget, gint width, gint height);
static void (*gtk_widget_set_usize)(GtkWidget *widget, gint width, gint height);
static void (*gtk_widget_size_request)(GtkWidget *widget, GtkRequisition *requisition);
static void (*gtk_widget_add_events)(GtkWidget *widget, gint events);
static void (*gtk_widget_show)(GtkWidget *widget);
static void (*gtk_widget_hide)(GtkWidget *widget);
//static void (*gtk_widget_get_allocation)(GtkWidget *widget, GtkAllocation *allocation);
static void (*gtk_widget_size_allocate)(GtkWidget *widget, GtkAllocation *allocation);
static void (*gtk_widget_set_allocation)(GtkWidget *widget, const GtkAllocation *allocation);
static void (*gtk_widget_grab_focus)(GtkWidget *widget);
static void (*gtk_widget_destroy)(GtkWidget *widget);
static GtkWidget *(*gtk_fixed_new)();
static void (*gtk_fixed_put)(GtkFixed *fixed, GtkWidget *widget, gint x, gint y);
static void (*gtk_fixed_move)(GtkFixed *fixed, GtkWidget *widget, gint x, gint y);
static void (*gtk_container_add)(GtkContainer *container, GtkWidget *widget);
static GtkWidget *(*gtk_button_new_with_label)(const gchar *label);
static const gchar *(*gtk_button_get_label)(GtkButton *button);
static void (*gtk_button_set_label)(GtkButton *button, const gchar *label);
static GtkWidget *(*gtk_drawing_area_new)();
static void (*gtk_widget_queue_draw)(GtkWidget *widget);
static void (*gtk_widget_queue_draw_area)(GtkWidget *widget, gint x, gint y, gint width, gint height);
static GtkWidget *(*gtk_scrolled_window_new)(GtkAdjustment *hadjustment, GtkAdjustment *vadjustment);
static void (*gtk_scrolled_window_add_with_viewport)(GtkScrolledWindow *scrolled_window, GtkWidget *child);
static GtkAdjustment *(*gtk_scrolled_window_get_hadjustment)(GtkScrolledWindow *scrolled_window);
static GtkAdjustment *(*gtk_scrolled_window_get_vadjustment)(GtkScrolledWindow *scrolled_window);
static void (*gtk_adjustment_configure)(GtkAdjustment *adjustment, gdouble value, gdouble lower, gdouble upper, gdouble step_increment, gdouble page_increment, gdouble page_size);
static void (*gtk_adjustment_value_changed)(GtkAdjustment *adjustment);
static void (*gtk_scrolled_window_get_policy)(GtkScrolledWindow *scrolled_window, GtkPolicyType *hscrollbar_policy, GtkPolicyType *vscrollbar_policy);
static void (*gtk_scrolled_window_set_policy)(GtkScrolledWindow *scrolled_window, GtkPolicyType hscrollbar_policy, GtkPolicyType vscrollbar_policy);
static GtkWidget *(*gtk_viewport_new)(GtkAdjustment *hadjustment, GtkAdjustment *vadjustment);
static GtkObject *(*gtk_adjustment_new)(gdouble value, gdouble lower, gdouble upper, gdouble step_increment, gdouble page_increment, gdouble page_size);
static GtkWidget *(*gtk_scrolled_window_get_hscrollbar)(GtkScrolledWindow *scrolled_window);
static GtkWidget *(*gtk_scrolled_window_get_vscrollbar)(GtkScrolledWindow *scrolled_window);
static void (*gtk_range_set_adjustment)(GtkRange *range, GtkAdjustment *adjustment);
static int (*gtk_widget_get_allocated_width)(GtkWidget *widget);
static int (*gtk_widget_get_allocated_height)(GtkWidget *widget);
static void (*gtk_window_set_geometry_hints)(GtkWindow *window, GtkWidget *geometry_widget, GdkGeometry *geometry, GdkWindowHints geom_mask);
static gdouble (*gtk_adjustment_get_value)(GtkAdjustment *adjustment);
static GtkWidget *(*gtk_label_new)(const gchar *str);
static void (*gtk_label_set_text)(GtkLabel *label, const gchar *str);
static const gchar *(*gtk_label_get_text)(GtkLabel *label);
static GtkWidget *(*gtk_entry_new)();
static void (*gtk_entry_set_text)(GtkEntry *entry, const gchar *text);
static const gchar *(*gtk_entry_get_text)(GtkEntry *entry);
static void (*gtk_misc_set_alignment)(GtkMisc *misc, gfloat xalign, gfloat yalign);
static GtkSettings *(*gtk_settings_get_default)();

struct View {
   ViewCommon common;
   GtkWidget *widget;
   Rect rect;
   uint32_t last_click_time;
   int last_click_x, last_click_y;
   int last_click_count;
   union {
      struct {
         GtkWidget *fixed;
         int x, y, width, height;
      } window;
      struct {
         int flags;
         GtkWidget *area;
         GtkWidget *scroll;
         GtkWidget *viewport;
         GtkAdjustment *hadj, *vadj;
         int hover;
      } canvas;
   };
};

struct Menu {
   MenuCommon common;
};

struct Worker {
   WorkerCommon common;
   pthread_mutex_t mutex;
   pthread_cond_t cond;
   struct Worker *notify_next;
};

struct NotifyIcon {
   NotifyIconCommon common;
};

struct SystemFont {
   cairo_font_face_t *font_face;
   float size;
   float ascent;
   float descent;
   float height;
};

typedef struct WidgetInfo {
   GtkWidget *widget;
   View *view;
   struct WidgetInfo *next;
} WidgetInfo;

typedef struct Timer {
   Heap *heap;
   Value instance;
   int removed;
   struct Timer *next;
} Timer;

static WidgetInfo *widget_infos = NULL;
static void (*orig_fixed_size_allocate)(GtkWidget *widget, GtkAllocation *allocation);
static void (*orig_fixed_preferred_width)(GtkWidget *widget, int *min, int *natural);
static void (*orig_fixed_preferred_height)(GtkWidget *widget, int *min, int *natural);
static cairo_t *tmp_cr;
static pthread_mutex_t global_mutex;
static Worker *notify_workers;
static Timer *active_timers;
static int io_pending;
static GdkCursor *cursors[NUM_CURSORS];
static int windows_count = 0;


static int pthread_cond_timedwait_relative(pthread_cond_t *cond, pthread_mutex_t *mutex, int64_t timeout)
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
   ts.tv_nsec += timeout % 1000000000;
   ts.tv_sec += ts.tv_nsec / 1000000000 + timeout / 1000000000;
   ts.tv_nsec %= 1000000000;
   return pthread_cond_timedwait(cond, mutex, &ts);
}


void trigger_delayed_gc(Heap *heap)
{
}


void free_view(View *view)
{
   WidgetInfo *info, **prev;

   switch (view->common.type) {
      case TYPE_WINDOW:
         for (prev = &widget_infos, info = *prev; info; prev = &info->next, info = info->next) {
            if (info->widget == view->window.fixed) {
               *prev = info->next;
               free(info);
               break;
            }
         }
         gtk_widget_destroy(view->widget);
         break;

      case TYPE_CANVAS:
         if (view->canvas.flags & CANVAS_SCROLLABLE) {
            g_object_unref(view->canvas.hadj);
            g_object_unref(view->canvas.vadj);
         }
         g_object_unref(view->widget);
         break;

      default:
         g_object_unref(view->widget);
         break;
   }
   
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
   gtk_widget_destroy(view->widget);
   view->widget = NULL;

   switch (view->common.type) {
      case TYPE_WINDOW:
         view->window.fixed = NULL;
         if (--windows_count <= 0) {
            gtk_main_quit();
         }
         break;

      case TYPE_CANVAS:
         view->canvas.area = NULL;
         view->canvas.scroll = NULL;
         view->canvas.viewport = NULL;
         break;
   }
}


void view_get_rect(View *view, Rect *rect)
{
   //GdkRectangle alloc;

   if (view->common.type == TYPE_WINDOW) {
      /*
      //gtk_widget_get_allocation(view->widget, &alloc);
      alloc = view->window.fixed->allocation;
      rect->x1 = alloc.x;
      rect->y1 = alloc.y;
      rect->x2 = alloc.x + alloc.width;
      rect->y2 = alloc.y + alloc.height;
      */
      rect->x1 = view->window.x;
      rect->y1 = view->window.y;
      rect->x2 = view->window.x + view->window.width;
      rect->y2 = view->window.y + view->window.height;
      return;
   }
   *rect = view->rect;
}


void view_set_rect(View *view, Rect *rect)
{
   view->rect = *rect;

   if (view->widget && view->common.parent && view->common.parent->common.type == TYPE_WINDOW) {
      if (version == 2) {
         gtk_widget_set_usize(view->widget, rect->x2 - rect->x1, rect->y2 - rect->y1);
      }
      else {
         gtk_widget_set_size_request(view->widget, rect->x2 - rect->x1, rect->y2 - rect->y1);
      }
      gtk_fixed_move((GtkFixed *)view->common.parent->window.fixed, view->widget, rect->x1, rect->y1);
      gtk_widget_queue_draw(view->widget);
   }
}


void view_get_content_rect(View *view, Rect *rect)
{
   view_get_rect(view, rect);
   rect->x2 -= rect->x1;
   rect->y2 -= rect->y1;
   rect->x1 = 0;
   rect->y1 = 0;
}


void view_get_inner_rect(View *view, Rect *rect)
{
   view_get_rect(view, rect);
   rect->x2 -= rect->x1;
   rect->y2 -= rect->y1;
   rect->x1 = 0;
   rect->y1 = 0;
}


void view_set_visible(View *view, int visible)
{
   if (view->common.type == TYPE_WINDOW) {
      if (visible) {
         gtk_widget_show(view->widget);
      }
      else {
         gtk_widget_hide(view->widget);
      }
   }
}


int view_add(View *parent, View *view)
{
   if (parent->common.type != TYPE_WINDOW) return 0;
   if (!view->widget) return 1;

   if (version == 2) {
      gtk_widget_set_usize(view->widget, view->rect.x2 - view->rect.x1, view->rect.y2 - view->rect.y1);
   }
   else {
      gtk_widget_set_size_request(view->widget, view->rect.x2 - view->rect.x1, view->rect.y2 - view->rect.y1);
   }
   gtk_fixed_put((GtkFixed *)parent->window.fixed, view->widget, view->rect.x1, view->rect.y1);
   return 1;
}


void view_focus(View *view)
{
   switch (view->common.type) {
      case TYPE_CANVAS:
         gtk_widget_grab_focus(view->canvas.area);
         break;

      default:
         gtk_widget_grab_focus(view->widget);
   }
}


int view_has_focus(View *view)
{
   return 0;
}


void view_get_sizing(View *view, float *grid_x, float *grid_y, int *form_small, int *form_medium, int *form_large, int *view_small, int *view_medium, int *view_large)
{
   *grid_x = 5;
   *grid_y = 6;
   *form_small = 6;
   *form_medium = 12;
   *form_large = 24;
   *view_small = 6;
   *view_medium = 12;
   *view_large = 24;
}


void view_get_default_size(View *view, int *width, int *height)
{
   GtkRequisition requisition;

   gtk_widget_size_request(view->widget, &requisition);
   *width = requisition.width;
   *height = requisition.height;
}


float view_get_scale(View *view)
{
   return 1.0f;
}


void view_set_cursor(View *view, int cursor)
{
   if (cursor < 0 || cursor >= NUM_CURSORS) {
      return;
   }

   if (view->common.type == TYPE_CANVAS && view->canvas.area->window) {
      gdk_window_set_cursor(view->canvas.area->window, cursors[cursor]);
   }
}


int view_get_cursor(View *view)
{
   return CURSOR_DEFAULT;
}


static gboolean window_delete(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
   View *view = user_data;

   call_view_callback(view, CALLBACK_WINDOW_CLOSE);
   return 0;
}


static gboolean window_destroy(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
   View *view = user_data;

   call_view_callback(view, CALLBACK_WINDOW_DESTROY);
   return 0;
}


static gboolean window_configure(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
   View *view = user_data;

   view->window.x = event->configure.x;
   view->window.y = event->configure.y;
   view->window.width = event->configure.width;
   view->window.height = event->configure.height;
   call_view_callback(view, CALLBACK_WINDOW_RESIZE);
   return 0;
}


static void fake_size_allocate(GtkWidget *widget, GtkAllocation *allocation)
{
   WidgetInfo *info;
   View *view;
   GtkAllocation child_allocation;

   for (info = widget_infos; info; info = info->next) {
      if (info->widget == widget) {
         gtk_widget_set_allocation(widget, allocation);
         for (view = info->view->common.first_child; view; view = view->common.next) {
            child_allocation.x = view->rect.x1;
            child_allocation.y = view->rect.y1;
            child_allocation.width = view->rect.x2 - view->rect.x1;
            child_allocation.height = view->rect.y2 - view->rect.y1;
            gtk_widget_size_allocate(view->widget, &child_allocation);
         }
         return;
      }
   }

   orig_fixed_size_allocate(widget, allocation);
}


static void fake_preferred_width(GtkWidget *widget, int *min, int *natural)
{
   WidgetInfo *info;

   for (info = widget_infos; info; info = info->next) {
      if (info->widget == widget) {
         *min = 1;
         *natural = 1;
         return;
      }
   }

   orig_fixed_preferred_width(widget, min, natural);
}


static void fake_preferred_height(GtkWidget *widget, int *min, int *natural)
{
   WidgetInfo *info;

   for (info = widget_infos; info; info = info->next) {
      if (info->widget == widget) {
         *min = 1;
         *natural = 1;
         return;
      }
   }

   orig_fixed_preferred_height(widget, min, natural);
}


View *window_create(plat_char *title, int width, int height, int flags)
{
   View *view;
   //GdkGeometry geometry;
   GtkWidgetClass *class;
   WidgetInfo *info;
   
   view = calloc(1, sizeof(View));
   if (!view) return NULL;

   view->widget = gtk_window_new(GTK_WINDOW_TOPLEVEL);
   view->window.fixed = gtk_fixed_new();
   view->window.width = width;
   view->window.height = height;

   if (version >= 3) {
      if (!orig_fixed_size_allocate) {
         class = view->window.fixed->object.parent_instance.class;
         orig_fixed_size_allocate = class->size_allocate;
         orig_fixed_preferred_width = class->get_preferred_width;
         orig_fixed_preferred_height = class->get_preferred_height;
         class->size_allocate = fake_size_allocate;
         class->get_preferred_width = fake_preferred_width;
         class->get_preferred_height = fake_preferred_height;
      }

      info = calloc(1, sizeof(WidgetInfo));
      info->widget = view->window.fixed;
      info->view = view;
      info->next = widget_infos;
      widget_infos = info;
   }
   
   g_signal_connect(view->widget, "delete_event", window_delete, view);
   g_signal_connect(view->widget, "destroy_event", window_destroy, view);
   g_signal_connect(view->widget, "configure_event", window_configure, view);
   gtk_widget_set_size_request(view->window.fixed, 32, 32);
   gtk_window_set_default_size((GtkWindow *)view->widget, width, height);
   gtk_window_set_title((GtkWindow *)view->widget, title);
   gtk_window_set_resizable((GtkWindow *)view->widget, (flags & WIN_RESIZABLE) != 0);
   /*
   geometry.min_width = 320;
   geometry.min_height = 240;
   geometry.base_width = 640;
   geometry.base_height = 480;
   gtk_window_set_geometry_hints((GtkWindow *)view->widget, view->window.fixed, &geometry, GDK_HINT_MIN_SIZE | GDK_HINT_BASE_SIZE);
   */

   gtk_container_add((GtkContainer *)view->widget, view->window.fixed);
   gtk_widget_show(view->window.fixed);
   windows_count++;
   return view;
}


plat_char *window_get_title(View *view)
{
   return strdup(gtk_window_get_title((GtkWindow *)view->widget));
}


void window_set_title(View *view, plat_char *title)
{
   gtk_window_set_title((GtkWindow *)view->widget, title);
}


void window_set_minimum_size(View *view, int width, int height)
{
   gtk_widget_set_size_request(view->window.fixed, width, height);
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

   view->widget = gtk_label_new(label);
   gtk_widget_show(view->widget);
   g_object_ref(view->widget);
   gtk_misc_set_alignment((GtkMisc *)view->widget, 0.0f, 0.5f);
   return view;
}


plat_char *label_get_label(View *view)
{
   return strdup(gtk_label_get_text((GtkLabel *)view->widget));
}


void label_set_label(View *view, plat_char *label)
{
   gtk_label_set_text((GtkLabel *)view->widget, label);
}


View *text_field_create()
{
   View *view;
   
   view = calloc(1, sizeof(View));
   if (!view) return NULL;

   view->widget = gtk_entry_new();
   gtk_widget_show(view->widget);
   g_object_ref(view->widget);
   return view;
}


plat_char *text_field_get_text(View *view)
{
   return strdup(gtk_entry_get_text((GtkEntry *)view->widget));
}


void text_field_set_text(View *view, plat_char *text)
{
   gtk_entry_set_text((GtkEntry *)view->widget, text);
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


static void button_clicked(GtkButton *button, gpointer user_data)
{
   View *view = user_data;

   call_action_callback(view, CALLBACK_BUTTON_ACTION);
}


View *button_create(plat_char *label, int flags)
{
   View *view;
   
   view = calloc(1, sizeof(View));
   if (!view) return NULL;

   view->widget = gtk_button_new_with_label(label);
   gtk_widget_show(view->widget);
   g_object_ref(view->widget);

   g_signal_connect(view->widget, "clicked", button_clicked, view);
   return view;
}


plat_char *button_get_label(View *view)
{
   return strdup(gtk_button_get_label((GtkButton *)view->widget));
}


void button_set_label(View *view, plat_char *label)
{
   gtk_button_set_label((GtkButton *)view->widget, label);
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


static void free_surface(void *data)
{
   cairo_surface_t *surface = data;

   cairo_surface_destroy(surface);
}


static gboolean canvas_configure(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
   View *view = user_data;

   view->rect.x1 = event->configure.x;
   view->rect.y1 = event->configure.y;
   view->rect.x2 = view->rect.x1 + event->configure.width;
   view->rect.y2 = view->rect.y1 + event->configure.height;
   call_view_callback(view, CALLBACK_CANVAS_RESIZE);
   return 0;
}


static void canvas_paint(View *view, cairo_t *cr, int xoff, int yoff)
{
   Heap *heap = view->common.heap;
   cairo_surface_t *surface;
   cairo_pattern_t *pattern;
   Value image, painter, error;
   double x1, y1, x2, y2;
   uint32_t *pixels;
   int x, y, width, height, stride;

   cairo_clip_extents(cr, &x1, &y1, &x2, &y2);
   x = (int)floor(x1);
   y = (int)floor(y1);
   width = (int)ceil(x2) - x;
   height = (int)ceil(y2) - y;
   if (width < 1 || height < 1) return;

   surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
   pixels = (uint32_t *)cairo_image_surface_get_data(surface);
   stride = cairo_image_surface_get_stride(surface) / 4;
   
   image = fiximage_create_from_pixels(heap, width, height, stride, pixels, free_surface, surface, -1);
   fixscript_ref(heap, image);

   if (!image.value) {
      fprintf(stderr, "error while painting:\n");
      fixscript_error(heap, &error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      fixscript_dump_value(heap, error, 1);
   }
   else {
      painter = fiximage_create_painter(heap, image, xoff-x, yoff-y);
      if (!painter.value) {
         fprintf(stderr, "error while painting:\n");
         fixscript_error(heap, &error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         fixscript_dump_value(heap, error, 1);
      }
      else {
         call_view_callback_with_value(view, CALLBACK_CANVAS_PAINT, painter);
      }
   }
   
   cairo_surface_mark_dirty(surface);
   cairo_translate(cr, x, y);
   pattern = cairo_pattern_create_for_surface(surface);
   cairo_set_source(cr, pattern);
   cairo_paint(cr);
   cairo_pattern_destroy(pattern);
   fixscript_unref(heap, image);
}


static gboolean canvas_expose(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
   View *view = user_data;
   cairo_t *cr;
   int xoff, yoff;

   if (view->canvas.flags & CANVAS_SCROLLABLE) {
      xoff = -view->canvas.hadj->value;
      yoff = -view->canvas.vadj->value;
   }
   else {
      xoff = 0;
      yoff = 0;
   }

   cr = gdk_cairo_create(event->expose.window);
   canvas_paint(view, cr, xoff, yoff);
   cairo_destroy(cr);
   return 0;
}


static gboolean canvas_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
   View *view = user_data;
   int xoff, yoff;

   if (view->canvas.flags & CANVAS_SCROLLABLE) {
      xoff = -gtk_adjustment_get_value(view->canvas.hadj);
      yoff = -gtk_adjustment_get_value(view->canvas.vadj);
   }
   else {
      xoff = 0;
      yoff = 0;
   }

   canvas_paint(view, cr, xoff, yoff);
   return 0;
}


static void canvas_hscroll(GtkAdjustment *adjustment, gpointer user_data)
{
   View *view = user_data;

   gtk_widget_queue_draw(view->canvas.area);
}


static void canvas_vscroll(GtkAdjustment *adjustment, gpointer user_data)
{
   View *view = user_data;

   gtk_widget_queue_draw(view->canvas.area);
}


static int get_x(View *view, int x)
{
   if (view->canvas.flags & CANVAS_SCROLLABLE) {
      if (version >= 3) {
         x += gtk_adjustment_get_value(view->canvas.hadj);
      }
      else {
         x += view->canvas.hadj->value;
      }
   }
   return x;
}


static int get_y(View *view, int y)
{
   if (view->canvas.flags & CANVAS_SCROLLABLE) {
      if (version >= 3) {
         y += gtk_adjustment_get_value(view->canvas.vadj);
      }
      else {
         y += view->canvas.vadj->value;
      }
   }
   return y;
}


static int get_button(int button)
{
   switch (button) {
      case 1: return MOUSE_BUTTON_LEFT;
      case 2: return MOUSE_BUTTON_MIDDLE;
      case 3: return MOUSE_BUTTON_RIGHT;
   }
   return -1;
}


static int get_modifiers(int state)
{
   int mod = 0;
   if (state & GDK_CONTROL_MASK) mod |= SCRIPT_MOD_CTRL;
   if (state & GDK_SHIFT_MASK) mod |= SCRIPT_MOD_SHIFT;
   if (state & GDK_MOD1_MASK) mod |= SCRIPT_MOD_ALT;
   if (state & GDK_BUTTON1_MASK) mod |= SCRIPT_MOD_LBUTTON;
   if (state & GDK_BUTTON2_MASK) mod |= SCRIPT_MOD_MBUTTON;
   if (state & GDK_BUTTON3_MASK) mod |= SCRIPT_MOD_RBUTTON;
   return mod;
}


static int get_key_modifiers(int state)
{
   int mod = 0;
   if (state & GDK_CONTROL_MASK) mod |= SCRIPT_MOD_CTRL;
   if (state & GDK_SHIFT_MASK) mod |= SCRIPT_MOD_SHIFT;
   if (state & GDK_MOD1_MASK) mod |= SCRIPT_MOD_ALT;
   return mod;
}


static gboolean canvas_motion_notify(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
   View *view = user_data;
   int x = get_x(view, event->motion.x);
   int y = get_y(view, event->motion.y);
   int mod = get_modifiers(event->motion.state);

   if (!view->canvas.hover) {
      view->canvas.hover = 1;
      call_mouse_event_callback(view, EVENT_MOUSE_ENTER, x, y, 0, 0, 0, 0);
   }
   call_mouse_event_callback(view, mod & SCRIPT_MOD_MOUSE_BUTTONS? EVENT_MOUSE_DRAG : EVENT_MOUSE_MOVE, x, y, 0, mod, 0, 0);
   return 1;
}


static gboolean canvas_leave_notify(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
   View *view = user_data;

   if (event->crossing.state & (GDK_BUTTON1_MASK | GDK_BUTTON2_MASK | GDK_BUTTON3_MASK | GDK_BUTTON4_MASK | GDK_BUTTON5_MASK)) {
      return 1;
   }
   call_mouse_event_callback(view, EVENT_MOUSE_LEAVE, 0, 0, 0, 0, 0, 0);
   view->canvas.hover = 0;
   return 1;
}


static gboolean canvas_button_press(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
   View *view = user_data;
   int x = get_x(view, event->button.x);
   int y = get_y(view, event->button.y);
   int button = get_button(event->button.button);
   int mod = get_modifiers(event->button.state);
   uint32_t time = event->button.time;
   int rx, ry;
   int double_click_time = 500;
   int double_click_dist = 2;

   if (event->type != GDK_BUTTON_PRESS) return 1;
   if (button == -1) return 1;

   switch (button) {
      case MOUSE_BUTTON_LEFT:   mod |= SCRIPT_MOD_LBUTTON; break;
      case MOUSE_BUTTON_MIDDLE: mod |= SCRIPT_MOD_MBUTTON; break;
      case MOUSE_BUTTON_RIGHT:  mod |= SCRIPT_MOD_RBUTTON; break;
   }

   g_object_get(gtk_settings_get_default(), "gtk-double-click-time", &double_click_time, NULL);
   if (version >= 3) {
      g_object_get(gtk_settings_get_default(), "gtk-double-click-distance", &double_click_dist, NULL);
   }

   rx = abs(x - view->last_click_x);
   ry = abs(y - view->last_click_y);
   if (rx <= double_click_dist && ry <= double_click_dist && time - view->last_click_time <= double_click_time) {
      view->last_click_count++;
   }
   else {
      view->last_click_count = 1;
   }
   view->last_click_time = time;
   view->last_click_x = x;
   view->last_click_y = y;

   return call_mouse_event_callback(view, EVENT_MOUSE_DOWN, x, y, button, mod, view->last_click_count, 0);
}


static gboolean canvas_button_release(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
   View *view = user_data;
   int x = get_x(view, event->button.x);
   int y = get_y(view, event->button.y);
   int button = get_button(event->button.button);
   int mod = get_modifiers(event->button.state);

   if (button == -1) return 1;

   switch (button) {
      case MOUSE_BUTTON_LEFT:   mod &= ~SCRIPT_MOD_LBUTTON; break;
      case MOUSE_BUTTON_MIDDLE: mod &= ~SCRIPT_MOD_MBUTTON; break;
      case MOUSE_BUTTON_RIGHT:  mod &= ~SCRIPT_MOD_RBUTTON; break;
   }

   return call_mouse_event_callback(view, EVENT_MOUSE_UP, x, y, button, mod, 0, 0);
}


static gboolean canvas_scroll(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
   View *view = user_data;
   int x = get_x(view, event->scroll.x);
   int y = get_y(view, event->scroll.y);
   int mod = get_modifiers(event->scroll.state);
   float wheel_x = 0.0f;
   float wheel_y = 0.0f;

   switch (event->scroll.direction) {
      case GDK_SCROLL_UP:    wheel_y = -1.0f; break;
      case GDK_SCROLL_DOWN:  wheel_y = +1.0f; break;
      case GDK_SCROLL_LEFT:  wheel_x = -1.0f; break;
      case GDK_SCROLL_RIGHT: wheel_x = +1.0f; break;
   }
   return call_mouse_wheel_callback(view, x, y, mod, wheel_x, wheel_y, 0, 0);
}


static int get_key(int key)
{
   switch (key) {
      case 0xFF1B:              return KEY_ESCAPE;
      case 0xFFBE:              return KEY_F1;
      case 0xFFBF:              return KEY_F2;
      case 0xFFC0:              return KEY_F3;
      case 0xFFC1:              return KEY_F4;
      case 0xFFC2:              return KEY_F5;
      case 0xFFC3:              return KEY_F6;
      case 0xFFC4:              return KEY_F7;
      case 0xFFC5:              return KEY_F8;
      case 0xFFC6:              return KEY_F9;
      case 0xFFC7:              return KEY_F10;
      case 0xFFC8:              return KEY_F11;
      case 0xFFC9:              return KEY_F12;
      case 0xFF61:              return KEY_PRINT_SCREEN;
      case 0xFF14:              return KEY_SCROLL_LOCK;
      case 0xFF13:              return KEY_PAUSE;
      case 0x60: case 0x7E:     return KEY_GRAVE;
      case 0x31: case 0x21:     return KEY_NUM1;
      case 0x32: case 0x40:     return KEY_NUM2;
      case 0x33: case 0x23:     return KEY_NUM3;
      case 0x34: case 0x24:     return KEY_NUM4;
      case 0x35: case 0x25:     return KEY_NUM5;
      case 0x36: case 0x5E:     return KEY_NUM6;
      case 0x37: case 0x26:     return KEY_NUM7;
      case 0x38: case 0x2A:     return KEY_NUM8;
      case 0x39: case 0x28:     return KEY_NUM9;
      case 0x30: case 0x29:     return KEY_NUM0;
      case 0x2D: case 0x5F:     return KEY_MINUS;
      case 0x3D: case 0x2B:     return KEY_EQUAL;
      case 0xFF08:              return KEY_BACKSPACE;
      case 0xFF09: case 0xFE20: return KEY_TAB;
      case 0x71: case 0x51:     return KEY_Q;
      case 0x77: case 0x57:     return KEY_W;
      case 0x65: case 0x45:     return KEY_E;
      case 0x72: case 0x52:     return KEY_R;
      case 0x74: case 0x54:     return KEY_T;
      case 0x79: case 0x59:     return KEY_Y;
      case 0x75: case 0x55:     return KEY_U;
      case 0x69: case 0x49:     return KEY_I;
      case 0x6F: case 0x4F:     return KEY_O;
      case 0x70: case 0x50:     return KEY_P;
      case 0x5B: case 0x7B:     return KEY_LBRACKET;
      case 0x5D: case 0x7D:     return KEY_RBRACKET;
      case 0x5C: case 0x7C:     return KEY_BACKSLASH;
      case 0xFFE5:              return KEY_CAPS_LOCK;
      case 0x61: case 0x41:     return KEY_A;
      case 0x73: case 0x53:     return KEY_S;
      case 0x64: case 0x44:     return KEY_D;
      case 0x66: case 0x46:     return KEY_F;
      case 0x67: case 0x47:     return KEY_G;
      case 0x68: case 0x48:     return KEY_H;
      case 0x6A: case 0x4A:     return KEY_J;
      case 0x6B: case 0x4B:     return KEY_K;
      case 0x6C: case 0x4C:     return KEY_L;
      case 0x3B: case 0x3A:     return KEY_SEMICOLON;
      case 0x27: case 0x22:     return KEY_APOSTROPHE;
      case 0xFF0D:              return KEY_ENTER;
      case 0xFFE1:              return KEY_LSHIFT;
      case 0x7A: case 0x5A:     return KEY_Z;
      case 0x78: case 0x58:     return KEY_X;
      case 0x63: case 0x43:     return KEY_C;
      case 0x76: case 0x56:     return KEY_V;
      case 0x62: case 0x42:     return KEY_B;
      case 0x6E: case 0x4E:     return KEY_N;
      case 0x6D: case 0x4D:     return KEY_M;
      case 0x2C: case 0x3C:     return KEY_COMMA;
      case 0x2E: case 0x3E:     return KEY_PERIOD;
      case 0x2F: case 0x3F:     return KEY_SLASH;
      case 0xFFE2:              return KEY_RSHIFT;
      case 0xFFE3:              return KEY_LCONTROL;
      case 0xFFEB:              return KEY_LMETA;
      case 0xFFE9:              return KEY_LALT;
      case 0x20:                return KEY_SPACE;
      case 0xFFEA:              return KEY_RALT;
      case 0xFFEC:              return KEY_RMETA;
      case 0xFF67:              return KEY_RMENU;
      case 0xFFE4:              return KEY_RCONTROL;
      case 0xFF63:              return KEY_INSERT;
      case 0xFFFF:              return KEY_DELETE;
      case 0xFF50:              return KEY_HOME;
      case 0xFF57:              return KEY_END;
      case 0xFF55:              return KEY_PAGE_UP;
      case 0xFF56:              return KEY_PAGE_DOWN;
      case 0xFF51:              return KEY_LEFT;
      case 0xFF52:              return KEY_UP;
      case 0xFF53:              return KEY_RIGHT;
      case 0xFF54:              return KEY_DOWN;
      case 0xFF7F:              return KEY_NUM_LOCK;
      case 0xFFAF:              return KEY_NUMPAD_SLASH;
      case 0xFFAA:              return KEY_NUMPAD_STAR;
      case 0xFFAD:              return KEY_NUMPAD_MINUS;
      case 0xFFAB:              return KEY_NUMPAD_PLUS;
      case 0xFF8D:              return KEY_NUMPAD_ENTER;
      case 0xFF9F: case 0xFFAE: return KEY_NUMPAD_DOT;
      case 0xFF9E: case 0xFFB0: return KEY_NUMPAD0;
      case 0xFF9C: case 0xFFB1: return KEY_NUMPAD1;
      case 0xFF99: case 0xFFB2: return KEY_NUMPAD2;
      case 0xFF9B: case 0xFFB3: return KEY_NUMPAD3;
      case 0xFF96: case 0xFFB4: return KEY_NUMPAD4;
      case 0xFF9D: case 0xFFB5: return KEY_NUMPAD5;
      case 0xFF98: case 0xFFB6: return KEY_NUMPAD6;
      case 0xFF95: case 0xFFB7: return KEY_NUMPAD7;
      case 0xFF97: case 0xFFB8: return KEY_NUMPAD8;
      case 0xFF9A: case 0xFFB9: return KEY_NUMPAD9;
   }
   return -1;
}


static gboolean canvas_key_press(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
   View *view = user_data;
   int key = get_key(event->key.keyval);
   int mod = get_key_modifiers(event->key.state);
   int ret;

   ret = call_key_event_callback(view, EVENT_KEY_DOWN, key, mod);
   if (event->key.string && strlen(event->key.string) > 0 && (mod & (SCRIPT_MOD_CTRL | SCRIPT_MOD_ALT)) == 0) {
      switch (key) {
         case KEY_ESCAPE:
         case KEY_ENTER:
         case KEY_NUMPAD_ENTER:
            break;

         default:
            call_key_typed_event_callback(view, event->key.string, mod);
      }
   }
   return ret;
}


static gboolean canvas_key_release(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
   View *view = user_data;
   int mod = get_key_modifiers(event->key.state);

   return call_key_event_callback(view, EVENT_KEY_UP, get_key(event->key.keyval), mod);
}


View *canvas_create(int flags)
{
   View *view;

   view = calloc(1, sizeof(View));
   if (!view) return NULL;

   view->canvas.flags = flags;
   view->canvas.area = gtk_drawing_area_new();
   gtk_widget_show(view->canvas.area);

   if (flags & (CANVAS_SCROLLABLE | CANVAS_BORDER)) {
      view->canvas.scroll = gtk_scrolled_window_new(NULL, NULL);
      gtk_scrolled_window_add_with_viewport((GtkScrolledWindow *)view->canvas.scroll, view->canvas.area);
      if (flags & CANVAS_SCROLLABLE) {
         gtk_scrolled_window_set_policy((GtkScrolledWindow *)view->canvas.scroll, GTK_POLICY_ALWAYS, GTK_POLICY_ALWAYS);
      }
      else {
         gtk_scrolled_window_set_policy((GtkScrolledWindow *)view->canvas.scroll, GTK_POLICY_NEVER, GTK_POLICY_NEVER);
      }

      if (flags & CANVAS_SCROLLABLE) {
         view->canvas.hadj = (GtkAdjustment *)gtk_adjustment_new(0, 0, 0, 0, 0, 0);
         view->canvas.vadj = (GtkAdjustment *)gtk_adjustment_new(0, 0, 0, 0, 0, 0);
         g_object_ref(view->canvas.hadj);
         g_object_ref(view->canvas.vadj);
         gtk_range_set_adjustment((GtkRange *)gtk_scrolled_window_get_hscrollbar((GtkScrolledWindow *)view->canvas.scroll), view->canvas.hadj);
         gtk_range_set_adjustment((GtkRange *)gtk_scrolled_window_get_vscrollbar((GtkScrolledWindow *)view->canvas.scroll), view->canvas.vadj);
         g_signal_connect(view->canvas.hadj, "changed", canvas_hscroll, view);
         g_signal_connect(view->canvas.hadj, "value_changed", canvas_hscroll, view);
         g_signal_connect(view->canvas.vadj, "changed", canvas_vscroll, view);
         g_signal_connect(view->canvas.vadj, "value_changed", canvas_vscroll, view);
      }

      gtk_widget_show(view->canvas.scroll);
      view->widget = view->canvas.scroll;
   }
   else {
      view->widget = view->canvas.area;
   }
   g_object_ref(view->widget);

   gtk_widget_add_events(view->canvas.area, GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_SCROLL_MASK | GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK);

   g_signal_connect(view->canvas.area, "configure_event", canvas_configure, view);
   g_signal_connect(view->canvas.area, "motion_notify_event", canvas_motion_notify, view);
   g_signal_connect(view->canvas.area, "leave_notify_event", canvas_leave_notify, view);
   g_signal_connect(view->canvas.area, "button_press_event", canvas_button_press, view);
   g_signal_connect(view->canvas.area, "button_release_event", canvas_button_release, view);
   g_signal_connect(view->canvas.area, "scroll_event", canvas_scroll, view);
   g_signal_connect(view->canvas.area, "key_press_event", canvas_key_press, view);
   g_signal_connect(view->canvas.area, "key_release_event", canvas_key_release, view);
   if (version >= 3) {
      g_signal_connect(view->canvas.area, "draw", canvas_draw, view);
   }
   else {
      g_signal_connect(view->canvas.area, "expose_event", canvas_expose, view);
   }
   return view;
}


void canvas_set_scroll_state(View *view, int type, int pos, int max, int page_size, int always_show)
{
   GtkAdjustment *adj;
   //GtkPolicyType hpolicy, vpolicy;

   if ((view->canvas.flags & CANVAS_SCROLLABLE) == 0) return;
   
   //gtk_scrolled_window_get_policy((GtkScrolledWindow *)view->canvas.scroll, &hpolicy, &vpolicy);
   if (type == SCROLL_HORIZ) {
      //adj = gtk_scrolled_window_get_hadjustment((GtkScrolledWindow *)view->canvas.scroll);
      adj = view->canvas.hadj;
      //hpolicy = always_show? GTK_POLICY_ALWAYS : GTK_POLICY_AUTOMATIC;
      //gtk_widget_set_size_request(view->canvas.area, 100, 2000);
   }
   else {
      //adj = gtk_scrolled_window_get_vadjustment((GtkScrolledWindow *)view->canvas.scroll);
      adj = view->canvas.vadj;
      //vpolicy = always_show? GTK_POLICY_ALWAYS : GTK_POLICY_AUTOMATIC;
   }
   //gtk_scrolled_window_set_policy((GtkScrolledWindow *)view->canvas.scroll, hpolicy, vpolicy);
   if (version >= 3) {
      gtk_adjustment_configure(adj, pos, 0, max, 1, page_size, page_size);
   }
   else {
      adj->lower = 0;
      adj->upper = max;
      adj->value = pos;
      adj->step_increment = 8;
      adj->page_increment = page_size;
      adj->page_size = page_size;
      gtk_adjustment_value_changed(adj);
   }
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
   GtkWidget *widget;
   
   switch (view->common.type) {
      case TYPE_CANVAS:
         widget = view->canvas.area;
         break;

      default:
         widget = view->widget;
   }

   if (enable) {
      widget->object.flags |= GTK_CAN_FOCUS;
   }
   else {
      widget->object.flags &= ~(GTK_CAN_FOCUS);
   }
}


int canvas_is_focusable(View *view)
{
   return 0;
}


void canvas_repaint(View *view, Rect *rect)
{
   int x_off, y_off;

   if (rect) {
      x_off = get_x(view, 0);
      y_off = get_y(view, 0);
      gtk_widget_queue_draw_area(view->canvas.area, rect->x1 - x_off, rect->y1 - y_off, rect->x2 - rect->x1, rect->y2 - rect->y1);
   }
   else {
      gtk_widget_queue_draw(view->canvas.area);
   }
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

   if (pthread_mutex_init(&worker->mutex, NULL) != 0) {
      free(worker);
      return NULL;
   }

   if (pthread_cond_init(&worker->cond, NULL) != 0) {
      pthread_mutex_destroy(&worker->mutex);
      free(worker);
      return NULL;
   }

   return worker;
}


static void *worker_main(void *data)
{
   Worker *worker = data;

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


static gboolean worker_notify_callbacks(void *data)
{
   Worker *worker;

   pthread_mutex_lock(&global_mutex);
   while (notify_workers) {
      worker = notify_workers;
      notify_workers = worker->notify_next;
      worker->common.notify_func(worker);
      worker_unref(worker);
   }
   if (io_pending) {
      io_pending = 0;
      io_process();
   }
   pthread_mutex_unlock(&global_mutex);
   return 1;
}


void worker_notify(Worker *worker)
{
   Worker *w;
   int found = 0;
   
   pthread_mutex_lock(&global_mutex);
   for (w = notify_workers; w; w = w->notify_next) {
      if (w == worker) {
         found = 1;
         break;
      }
   }
   if (!found) {
      worker->notify_next = notify_workers;
      notify_workers = worker;
      worker_ref(worker);
   }
   pthread_mutex_unlock(&global_mutex);
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

   for (timer = active_timers; timer; timer = timer->next) {
      if (timer->heap == heap && timer->instance.value == instance.value && timer->instance.is_array == instance.is_array) {
         return 1;
      }
   }
   return 0;
}


static gboolean handle_timer(void *data)
{
   Timer *timer = data;

   if (timer->removed) {
      fixscript_unref(timer->heap, timer->instance);
      free(timer);
      return 0;
   }

   timer_run(timer->heap, timer->instance);

   if (timer->removed) {
      fixscript_unref(timer->heap, timer->instance);
      free(timer);
      return 0;
   }
   return 1;
}


void timer_start(Heap *heap, Value instance, int interval, int restart)
{
   Timer *timer, **prev = &active_timers;
   
   for (timer = active_timers; timer; timer = timer->next) {
      if (timer->heap == heap && timer->instance.value == instance.value && timer->instance.is_array == instance.is_array) {
         break;
      }
      prev = &timer->next;
   }

   if (timer) {
      if (restart) {
         timer->removed = 1;
         *prev = timer->next;
      }
      else {
         return;
      }
   }

   timer = calloc(1, sizeof(Timer));
   timer->heap = heap;
   timer->instance = instance;
   timer->next = active_timers;
   active_timers = timer;
   fixscript_ref(heap, instance);
   g_timeout_add(interval, handle_timer, timer);
}


void timer_stop(Heap *heap, Value instance)
{
   Timer *timer, **prev = &active_timers;
   
   for (timer = active_timers; timer; timer = timer->next) {
      if (timer->heap == heap && timer->instance.value == instance.value && timer->instance.is_array == instance.is_array) {
         timer->removed = 1;
         *prev = timer->next;
         break;
      }
      prev = &timer->next;
   }
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
   cairo_font_extents_t extents;
   
   font = calloc(1, sizeof(SystemFont));
   if (!font) return NULL;

   cairo_select_font_face(tmp_cr, family, style & FONT_ITALIC? CAIRO_FONT_SLANT_ITALIC : CAIRO_FONT_SLANT_NORMAL, style & FONT_BOLD? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
   cairo_set_font_size(tmp_cr, size);
   cairo_font_extents(tmp_cr, &extents);
   font->font_face = cairo_get_font_face(tmp_cr);
   font->size = size;
   font->ascent = extents.ascent;
   font->descent = extents.descent;
   font->height = extents.height;
   cairo_font_face_reference(font->font_face);
   return font;
}


void system_font_destroy(SystemFont *font)
{
   cairo_font_face_destroy(font->font_face);
   free(font);
}


plat_char **system_font_get_list()
{
   PangoContext *ctx;
   PangoFontFamily **families;
   char **list;
   int i, n_families;
   
   ctx = gdk_pango_context_get();
   pango_context_list_families(ctx, &families, &n_families);
   list = malloc((n_families+1)*sizeof(char *));
   for (i=0; i<n_families; i++) {
      list[i] = strdup(pango_font_family_get_name(families[i]));
   }
   list[n_families] = NULL;
   g_object_unref(ctx);
   return list;
}


int system_font_get_size(SystemFont *font)
{
   return font->size + 0.5f;
}


int system_font_get_ascent(SystemFont *font)
{
   return font->ascent + 0.5f;
}


int system_font_get_descent(SystemFont *font)
{
   return font->descent + 0.5f;
}


int system_font_get_height(SystemFont *font)
{
   return font->height + 0.5f;
}


int system_font_get_string_advance(SystemFont *font, plat_char *s)
{
   cairo_text_extents_t extents;
   
   cairo_set_font_face(tmp_cr, font->font_face);
   cairo_set_font_size(tmp_cr, font->size);
   cairo_text_extents(tmp_cr, s, &extents);
   return extents.x_advance + 0.5f;
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


void system_font_draw_string(SystemFont *font, int x, int y, plat_char *text, uint32_t color, uint32_t *pixels, int width, int height, int stride)
{
   cairo_surface_t *surface;
   cairo_t *cr;
   float r, g, b, a;

   if (cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, 1) != 4) {
      // safeguard in case the implementation changes in incompatible way (possible, but not likely)
      return;
   }

   r = ((color >> 16) & 0xFF)/255.0f;
   g = ((color >>  8) & 0xFF)/255.0f;
   b = ((color >>  0) & 0xFF)/255.0f;
   a = ((color >> 24) & 0xFF)/255.0f;
   if (a != 0.0f) {
      r /= a;
      g /= a;
      b /= a;
   }
   
   surface = cairo_image_surface_create_for_data((unsigned char *)pixels, CAIRO_FORMAT_ARGB32, width, height, stride*4);
   cr = cairo_create(surface);
   cairo_set_font_face(cr, font->font_face);
   cairo_set_font_size(cr, font->size);
   cairo_translate(cr, x, y);
   cairo_set_source_rgba(cr, r, g, b, a);
   cairo_show_text(cr, text);
   cairo_destroy(cr);
   cairo_surface_destroy(surface);
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
   pthread_mutex_lock(&global_mutex);
   io_pending = 1;
   pthread_mutex_unlock(&global_mutex);
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


static Value func_gtk_is_present(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   return fixscript_int(1);
}


static Value func_gtk_get_widget_handle(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;
   uint64_t ptr;
   
   view = view_get_native(heap, error, params[0], -1);
   if (!view) {
      return fixscript_int(0);
   }

   ptr = (uint64_t)(uintptr_t)view->widget;

   *error = fixscript_int((uint32_t)(ptr >> 32));
   return fixscript_int((uint32_t)ptr);
}


static Value func_common_get_double_click_delay(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   int delay = 500;

   g_object_get(gtk_settings_get_default(), "gtk-double-click-time", &delay, NULL);
   return fixscript_int(delay);
}


static Value func_common_get_double_click_distance(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   int dist = 2;

   if (version >= 3) {
      g_object_get(gtk_settings_get_default(), "gtk-double-click-distance", &dist, NULL);
   }
   return fixscript_int(dist);
}


static Value func_common_get_cursor_blink_interval(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   int blink = 1;
   int time = 500;

   g_object_get(gtk_settings_get_default(),
      "gtk-cursor-blink", &blink,
      "gtk-cursor-blink-time", &time,
      NULL
   );
   return fixscript_int(blink? time/2 : 0);
}


void register_platform_gui_functions(Heap *heap)
{
   fixscript_register_native_func(heap, "common_get_double_click_delay#0", func_common_get_double_click_delay, NULL);
   fixscript_register_native_func(heap, "common_get_double_click_distance#0", func_common_get_double_click_distance, NULL);
   fixscript_register_native_func(heap, "common_get_cursor_blink_interval#0", func_common_get_cursor_blink_interval, NULL);

   fixscript_register_native_func(heap, "gtk_is_present#0", func_gtk_is_present, NULL);
   fixscript_register_native_func(heap, "gtk_get_widget_handle#1", func_gtk_get_widget_handle, NULL);
}


static void *load_symbol(void *lib, const char *name)
{
   void *ptr;

   ptr = dlsym(lib, name);
   if (!ptr) {
      #ifndef FIXGUI_CONSOLE_FALLBACK
         fprintf(stderr, "error: can't find symbol %s\n", name);
         exit(1);
      #endif
   }
   return ptr;
}

#define SYMBOL(lib, name) if (!(name = load_symbol(lib##_lib, #name))) goto fallback;


int main(int argc, char **argv)
{
   cairo_surface_t *surface;
   GdkBitmap *empty_bitmap;
   GdkColor empty_color;
   gchar bitmap_data = 0;
   void *cairo_lib;
   void *pango_lib;
   void *glib_lib;
   void *gobject2_lib;
   void *gdk2_lib;
   void *gtk2_lib;
#ifdef FIXGUI_USE_GTK3
   void *gdk3_lib;
   void *gtk3_lib;
#endif
   void *gdk_lib;
   void *gtk_lib;

   cairo_lib = dlopen("libcairo.so.2", RTLD_LAZY);
   pango_lib = dlopen("libpango-1.0.so.0", RTLD_LAZY);
   glib_lib = dlopen("libglib-2.0.so.0", RTLD_LAZY);
   gobject2_lib = dlopen("libgobject-2.0.so.0", RTLD_LAZY);

#ifdef FIXGUI_USE_GTK3
   gdk3_lib = dlopen("libgdk-3.so.0", RTLD_LAZY);
   gtk3_lib = dlopen("libgtk-3.so.0", RTLD_LAZY);
   if (gdk3_lib && gtk3_lib && glib_lib && gobject2_lib && cairo_lib && pango_lib) {
      version = 3;
      gdk_lib = gdk3_lib;
      gtk_lib = gtk3_lib;
   }
   else
#endif
   {
      gdk2_lib = dlopen("libgdk-x11-2.0.so.0", RTLD_LAZY);
      gtk2_lib = dlopen("libgtk-x11-2.0.so.0", RTLD_LAZY);
      if (!glib_lib || !gobject2_lib || !gdk2_lib || !cairo_lib || !gtk2_lib || !pango_lib) {
         #ifdef FIXGUI_CONSOLE_FALLBACK
            goto fallback;
         #else
            fprintf(stderr, "error: can't load libgtk-x11-2.0.so.0 library\n");
            return 1;
         #endif
      }
      version = 2;
      gdk_lib = gdk2_lib;
      gtk_lib = gtk2_lib;
   }

   SYMBOL(glib, g_free);

   SYMBOL(gobject2, g_object_ref);
   SYMBOL(gobject2, g_object_unref);
   SYMBOL(gobject2, g_object_get);
   SYMBOL(gobject2, g_signal_connect_data);
   SYMBOL(gobject2, g_idle_add);
   SYMBOL(gobject2, g_timeout_add);

   if (version == 2) {
      SYMBOL(gdk, gdk_cairo_create);
   }
   SYMBOL(gdk, gdk_pango_context_get);
   SYMBOL(gdk, gdk_cursor_new);
   if (version == 2) {
      SYMBOL(gdk, gdk_cursor_new_from_pixmap);
      SYMBOL(gdk, gdk_bitmap_create_from_data);
   }
   SYMBOL(gdk, gdk_window_set_cursor);

   SYMBOL(pango, pango_context_list_families);
   SYMBOL(pango, pango_font_family_get_name);

   SYMBOL(cairo, cairo_create);
   SYMBOL(cairo, cairo_clip_extents);
   SYMBOL(cairo, cairo_translate);
   SYMBOL(cairo, cairo_set_source_rgba);
   SYMBOL(cairo, cairo_set_source);
   SYMBOL(cairo, cairo_rectangle);
   SYMBOL(cairo, cairo_fill);
   SYMBOL(cairo, cairo_paint);
   SYMBOL(cairo, cairo_destroy);
   SYMBOL(cairo, cairo_select_font_face);
   SYMBOL(cairo, cairo_get_font_face);
   SYMBOL(cairo, cairo_set_font_face);
   SYMBOL(cairo, cairo_set_font_size);
   SYMBOL(cairo, cairo_show_text);
   SYMBOL(cairo, cairo_font_extents);
   SYMBOL(cairo, cairo_text_extents);
   SYMBOL(cairo, cairo_font_face_reference);
   SYMBOL(cairo, cairo_font_face_destroy);
   SYMBOL(cairo, cairo_format_stride_for_width);
   SYMBOL(cairo, cairo_image_surface_create);
   SYMBOL(cairo, cairo_image_surface_create_for_data);
   SYMBOL(cairo, cairo_image_surface_get_data);
   SYMBOL(cairo, cairo_image_surface_get_stride);
   SYMBOL(cairo, cairo_surface_mark_dirty);
   SYMBOL(cairo, cairo_surface_destroy);
   SYMBOL(cairo, cairo_pattern_create_for_surface);
   SYMBOL(cairo, cairo_pattern_destroy);

   SYMBOL(gtk, gtk_init);
   SYMBOL(gtk, gtk_init_check);
   SYMBOL(gtk, gtk_main);
   SYMBOL(gtk, gtk_main_quit);
   SYMBOL(gtk, gtk_window_new);
   SYMBOL(gtk, gtk_window_set_default);
   SYMBOL(gtk, gtk_window_set_title);
   SYMBOL(gtk, gtk_window_get_title);
   SYMBOL(gtk, gtk_window_set_resizable);
   SYMBOL(gtk, gtk_window_set_default_size);
   SYMBOL(gtk, gtk_window_resize);
   SYMBOL(gtk, gtk_widget_set_size_request);
   if (version == 2) {
      SYMBOL(gtk, gtk_widget_set_usize);
   }
   SYMBOL(gtk, gtk_widget_size_request);
   SYMBOL(gtk, gtk_widget_add_events);
   SYMBOL(gtk, gtk_widget_show);
   SYMBOL(gtk, gtk_widget_hide);
   //SYMBOL(gtk, gtk_widget_get_allocation);
   SYMBOL(gtk, gtk_widget_size_allocate);
   if (version >= 3) {
      SYMBOL(gtk, gtk_widget_set_allocation);
   }
   SYMBOL(gtk, gtk_widget_grab_focus);
   SYMBOL(gtk, gtk_widget_destroy);
   SYMBOL(gtk, gtk_fixed_new);
   SYMBOL(gtk, gtk_fixed_put);
   SYMBOL(gtk, gtk_fixed_move);
   SYMBOL(gtk, gtk_container_add);
   SYMBOL(gtk, gtk_button_new_with_label);
   SYMBOL(gtk, gtk_button_get_label);
   SYMBOL(gtk, gtk_button_set_label);
   SYMBOL(gtk, gtk_drawing_area_new);
   SYMBOL(gtk, gtk_widget_queue_draw);
   SYMBOL(gtk, gtk_widget_queue_draw_area);
   SYMBOL(gtk, gtk_scrolled_window_new);
   SYMBOL(gtk, gtk_scrolled_window_add_with_viewport);
   SYMBOL(gtk, gtk_scrolled_window_get_hadjustment);
   SYMBOL(gtk, gtk_scrolled_window_get_vadjustment);
   if (version >= 3) {
      SYMBOL(gtk, gtk_adjustment_configure);
   }
   SYMBOL(gtk, gtk_adjustment_value_changed);
   SYMBOL(gtk, gtk_scrolled_window_get_policy);
   SYMBOL(gtk, gtk_scrolled_window_set_policy);
   SYMBOL(gtk, gtk_viewport_new);
   SYMBOL(gtk, gtk_adjustment_new);
   SYMBOL(gtk, gtk_scrolled_window_get_hscrollbar);
   SYMBOL(gtk, gtk_scrolled_window_get_vscrollbar);
   SYMBOL(gtk, gtk_range_set_adjustment);
   if (version >= 3) {
      SYMBOL(gtk, gtk_widget_get_allocated_width);
      SYMBOL(gtk, gtk_widget_get_allocated_height);
   }
   SYMBOL(gtk, gtk_window_set_geometry_hints);
   if (version >= 3) {
      SYMBOL(gtk, gtk_adjustment_get_value);
   }
   SYMBOL(gtk, gtk_label_new);
   SYMBOL(gtk, gtk_label_set_text);
   SYMBOL(gtk, gtk_label_get_text);
   SYMBOL(gtk, gtk_entry_new);
   SYMBOL(gtk, gtk_entry_set_text);
   SYMBOL(gtk, gtk_entry_get_text);
   SYMBOL(gtk, gtk_misc_set_alignment);
   SYMBOL(gtk, gtk_settings_get_default);

   #ifdef FIXGUI_CONSOLE_FALLBACK
      if (!gtk_init_check(&argc, &argv)) {
         goto fallback;
      }
   #else
      gtk_init(&argc, &argv);
   #endif

   surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
   tmp_cr = cairo_create(surface);
   cairo_surface_destroy(surface);

   cursors[CURSOR_DEFAULT] = NULL;
   cursors[CURSOR_ARROW] = NULL;
   cursors[CURSOR_EMPTY] = NULL;
   cursors[CURSOR_TEXT] = gdk_cursor_new(GDK_XTERM);
   cursors[CURSOR_CROSS] = gdk_cursor_new(GDK_CROSSHAIR);
   cursors[CURSOR_HAND] = gdk_cursor_new(GDK_HAND2);
   cursors[CURSOR_MOVE] = gdk_cursor_new(GDK_FLEUR);
   cursors[CURSOR_RESIZE_N] = gdk_cursor_new(GDK_TOP_SIDE);
   cursors[CURSOR_RESIZE_NE] = gdk_cursor_new(GDK_TOP_RIGHT_CORNER);
   cursors[CURSOR_RESIZE_E] = gdk_cursor_new(GDK_RIGHT_SIDE);
   cursors[CURSOR_RESIZE_SE] = gdk_cursor_new(GDK_BOTTOM_RIGHT_CORNER);
   cursors[CURSOR_RESIZE_S] = gdk_cursor_new(GDK_BOTTOM_SIDE);
   cursors[CURSOR_RESIZE_SW] = gdk_cursor_new(GDK_BOTTOM_LEFT_CORNER);
   cursors[CURSOR_RESIZE_W] = gdk_cursor_new(GDK_LEFT_SIDE);
   cursors[CURSOR_RESIZE_NW] = gdk_cursor_new(GDK_TOP_LEFT_CORNER);
   cursors[CURSOR_WAIT] = gdk_cursor_new(GDK_WATCH);

   if (version >= 3) {
      cursors[CURSOR_EMPTY] = gdk_cursor_new(GDK_BLANK_CURSOR);
   }
   else {
      empty_bitmap = gdk_bitmap_create_from_data(NULL, &bitmap_data, 1, 1);
      empty_color.pixel = 0;
      empty_color.red = 0;
      empty_color.green = 0;
      empty_color.blue = 0;
      cursors[CURSOR_EMPTY] = gdk_cursor_new_from_pixmap((GdkPixmap *)empty_bitmap, (GdkPixmap *)empty_bitmap, &empty_color, &empty_color, 0, 0);
   }

   if (pthread_mutex_init(&global_mutex, NULL) != 0) {
      fprintf(stderr, "error: can't create mutex\n");
      return 1;
   }
   
   g_timeout_add(10, worker_notify_callbacks, NULL);

   app_main(argc, argv);
   gtk_main();
   return 0;

fallback:
   #ifdef FIXGUI_CONSOLE_FALLBACK
      console_main(argc, argv);
      return 0;
   #else
      return 1;
   #endif
}
