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
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <unistd.h>
#include <time.h>
#include <limits.h>
#include <dlfcn.h>
#include <sys/time.h>
#include <objc/message.h>
#include "fixgui_common.h"

#if defined(__GNUC__) && !defined(__clang__)
static void (*objc_msgSend_GCCFIX)(void) = (void (*)(void))objc_msgSend;
static void (*objc_msgSend_stret_GCCFIX)(void) = (void (*)(void))objc_msgSend_stret;
static void (*objc_msgSendSuper_GCCFIX)(void) = (void (*)(void))objc_msgSendSuper;
#define objc_msgSend objc_msgSend_GCCFIX
#define objc_msgSend_stret objc_msgSend_stret_GCCFIX
#define objc_msgSendSuper objc_msgSendSuper_GCCFIX
#endif

#define ARGS(...) ,##__VA_ARGS__
#define call(obj, name, ret, args, ...) ((ret (*) (id, SEL ARGS args))objc_msgSend)(obj, sel(name), ##__VA_ARGS__)
#define call_stret(obj, name, ret, args, ...) ((ret (*) (id, SEL ARGS args))objc_msgSend_stret)(obj, sel(name), ##__VA_ARGS__)
#define call_super(obj, cls, sel, ret, args, ...) ((ret (*) (struct objc_super *, SEL ARGS args))objc_msgSendSuper)(&(struct objc_super) { obj, (Class)cls }, sel, ##__VA_ARGS__)
#define class(name) objc_getClass(name)
#define sel(name) sel_registerName(name)
#define alloc(name) call((id)class(name), "alloc", id, ())
#define init(obj) call(obj, "init", id, ())
#define retain(obj) call(obj, "retain", void, ())
#define release(obj) call(obj, "release", void, ())
#define string(s) call(alloc("NSString"), "initWithUTF8String:", id, (const char *), s)
#define ivar(obj, name, type) *(type *)(((void *)(obj)) + ivar_getOffset(class_getInstanceVariable(object_getClass(obj), name)))

typedef void (*BlockFunc)(void);

// https://clang.llvm.org/docs/Block-ABI-Apple.html
typedef struct {
   void *isa;
   int flags;
   int reserved;
   BlockFunc invoke;
   void *desc;
   void *data;
   void *next;
} Block;

enum {
   NSAlertStyleWarning       = 0,
   NSAlertStyleInformational = 1,
   NSAlertStyleCritical      = 2
};

enum {
   NSWindowStyleMaskTitled         = 0x01,
   NSWindowStyleMaskClosable       = 0x02,
   NSWindowStyleMaskMiniaturizable = 0x04,
   NSWindowStyleMaskResizable      = 0x08
};

enum {
   NSViewWidthSizable  = 0x02,
   NSViewHeightSizable = 0x10
};

enum {
   NSBackingStoreBuffered = 2
};

enum {
   NSBezelStyleRounded           = 1,
   NSBezelStyleRegularSquare     = 2,
   NSBezelStyleDisclosure        = 5,
   NSBezelStyleShadowlessSquare  = 6,
   NSBezelStyleCircular          = 7,
   NSBezelStyleTexturedSquare    = 8,
   NSBezelStyleHelpButton        = 9,
   NSBezelStyleSmallSquare       = 10,
   NSBezelStyleTexturedRounded   = 11,
   NSBezelStyleRoundRect         = 12,
   NSBezelStyleRecessed          = 13,
   NSBezelStyleRoundedDisclosure = 14,
   NSBezelStyleInline            = 15  // 10.7+
};

enum {
   NSControlSizeRegular = 0,
   NSControlSizeSmall   = 1,
   NSControlSizeMini    = 2
};

enum {
   NSBitmapFormatAlphaFirst = 1 << 0,
   NSBitmapFormatThirtyTwoBitLittleEndian = 1 << 9,
   NSBitmapFormatThirtyTwoBitBigEndian = 1 << 11
};

enum {
   kCGImageAlphaPremultipliedLast = 1,
   kCGImageAlphaPremultipliedFirst = 2,
   kCGBitmapByteOrder32Little = 2 << 12,
   kCGBitmapByteOrder32Big = 4 << 12,
   kCGRenderingIntentDefault = 0,
   NSCompositeCopy = 1,
   NSCompositeSourceOver = 2
};

enum {
   NSEventTypeLeftMouseDown      = 1,
   NSEventTypeLeftMouseUp        = 2,
   NSEventTypeRightMouseDown     = 3,
   NSEventTypeRightMouseUp       = 4,
   NSEventTypeMouseMoved         = 5,
   NSEventTypeLeftMouseDragged   = 6,
   NSEventTypeRightMouseDragged  = 7,
   NSEventTypeMouseEntered       = 8,
   NSEventTypeMouseExited        = 9,
   NSEventTypeKeyDown            = 10,
   NSEventTypeKeyUp              = 11,
   NSEventTypeFlagsChanged       = 12,
   NSEventTypeAppKitDefined      = 13,
   NSEventTypeSystemDefined      = 14,
   NSEventTypeApplicationDefined = 15,
   NSEventTypePeriodic           = 16,
   NSEventTypeCursorUpdate       = 17,
   NSEventTypeScrollWheel        = 22,
   NSEventTypeTabletPoint        = 23,
   NSEventTypeTabletProximity    = 24,
   NSEventTypeOtherMouseDown     = 25,
   NSEventTypeOtherMouseUp       = 26,
   NSEventTypeOtherMouseDragged  = 27
};

enum {
   NSEventModifierFlagShift   = 1 << 17,
   NSEventModifierFlagControl = 1 << 18,
   NSEventModifierFlagOption  = 1 << 19,
   NSEventModifierFlagCommand = 1 << 20
};

enum {
   NSTrackingMouseEnteredAndExited    = 0x01,
   NSTrackingMouseMoved               = 0x02,
   NSTrackingCursorUpdate             = 0x04,
   NSTrackingActiveWhenFirstResponder = 0x10,
   NSTrackingActiveInKeyWindow        = 0x20,
   NSTrackingActiveInActiveApp        = 0x40,
   NSTrackingActiveAlways             = 0x80,
   NSTrackingAssumeInside             = 0x100,
   NSTrackingInVisibleRect            = 0x200,
   NSTrackingEnabledDuringMouseDrag   = 0x400
};

#ifdef __LP64__
typedef double CGFloat;
#else
typedef float CGFloat;
#endif

typedef struct {
   CGFloat x, y;
} CGPoint;

typedef struct {
   CGFloat width, height;
} CGSize;

typedef struct {
   CGPoint origin;
   CGSize size;
} CGRect;

typedef CGPoint NSPoint;
typedef CGSize NSSize;
typedef CGRect NSRect;

typedef long NSInteger;
typedef unsigned long NSUInteger;

typedef struct {
   NSUInteger location;
   NSUInteger length;
} NSRange;

typedef void *CGColorSpaceRef;
typedef void *CGContextRef;
typedef void *CGImageRef;
typedef void *CGDataProviderRef;
typedef void (*CGDataProviderReleaseDataCallback)(void *info, const void *data, size_t size);
typedef void *CFStringRef;

struct View {
   ViewCommon common;
   id obj;
   Rect rect;
   union {
      struct {
         int flags;
         int close_requested;
         Menu *menu;
      } window;
      struct {
         id obj;
      } label;
      struct {
         int flags;
         id wrapper;
         id obj;
         id scroll_view;
         struct {
            int pos, max;
            int always_show;
         } scroll[2];
         int placed;
         int focusable;
         int send_leave;
         int cursor;
      } canvas;
      struct {
         id obj;
      } text_area;
      struct {
         id obj, data_obj;
         int num_rows, num_columns;
         char **data;
         int dragged_column;
      } table;
   };
};

struct Menu {
   MenuCommon common;
   id obj;
   int has_app_menu;
};

struct Worker {
   WorkerCommon common;
};

typedef struct {
   CGDataProviderRef provider;
   CGImageRef img;
   uint32_t *pixels;
} ImageData;

struct NotifyIcon {
   NotifyIconCommon common;
   id image;
   CGColorSpaceRef space;
   ImageData *images;
   int num_images;
   id item;
   Menu *menu;
};

struct SystemFont {
   id font;
};

typedef struct Timer {
   Heap *heap;
   Value instance;
   id timer;
   struct Timer *next;
} Timer;

enum {
   MH_ABOUT,
   MH_PREFERENCES,
   MH_NUM_HANDLERS
};

extern const double NSAppKitVersionNumber;
#define NSAppKitVersionNumber10_7 1138
#define NSAppKitVersionNumber10_9 1265
#define NSAppKitVersionNumber10_10 1343
extern id NSCalibratedRGBColorSpace;
extern id NSFontAttributeName;
extern id NSForegroundColorAttributeName;
extern id NSBackgroundColorAttributeName;
extern CFStringRef kCGColorSpaceSRGB;

static Block *free_blocks = NULL;
static int main_argc;
static char **main_argv;
static char *exec_path;

void NSRectFill(NSRect rect);
void CGContextSetRGBFillColor(CGContextRef c, CGFloat red, CGFloat green, CGFloat blue, CGFloat alpha);
void CGContextFillRect(CGContextRef c, CGRect rect);
void CGContextShowTextAtPoint(CGContextRef c, CGFloat x, CGFloat y, const char *string, size_t length);
void CGContextSelectFont(CGContextRef c, const char *name, CGFloat size, int32_t textEncoding);
void CGContextSaveGState(CGContextRef c);
void CGContextRestoreGState(CGContextRef c);

CGColorSpaceRef CGColorSpaceCreateDeviceRGB();
CGColorSpaceRef CGColorSpaceCreateWithName(CFStringRef name);
void CGColorSpaceRelease(CGColorSpaceRef space);
CGContextRef CGBitmapContextCreate(void *data, size_t width, size_t height, size_t bitsPerComponent, size_t bytesPerRow, CGColorSpaceRef space, uint32_t bitmapInfo);
CGDataProviderRef CGDataProviderCreateWithData(void *info, const void *data, size_t size, CGDataProviderReleaseDataCallback releaseData);
void CGDataProviderRelease(CGDataProviderRef provider);
CGImageRef CGBitmapContextCreateImage(CGContextRef context);
CGImageRef CGImageCreate(size_t width, size_t height, size_t bitsPerComponent, size_t bitsPerPixel, size_t bytesPerRow, CGColorSpaceRef space, uint32_t bitmapInfo, CGDataProviderRef provider, const CGFloat *decode, bool shouldInterpolate, int32_t intent);

void CGImageRelease(CGImageRef image);
void CGContextRelease(CGContextRef c);

static char *app_name;
static id cursors[NUM_CURSORS];
static struct {
   Heap *heap;
   Value func;
   Value data;
} menu_handlers[MH_NUM_HANDLERS];
static id default_menubar;
static Menu *main_menubar;
static int menubar_set;
static Timer *active_timers;
static int create_search_field = 0;
static id preview_panel = NULL;
static id preview_data_source = NULL;
static char *preview_panel_path = NULL;

void fixgui__tls_init();


static Block *get_block(BlockFunc func, void *data)
{
   Block *block;

   if (free_blocks) {
      block = free_blocks;
      free_blocks = block->next;
   }
   else {
      block = calloc(1, sizeof(Block));
   }

   block->isa = class("__NSGlobalBlock__");
   block->flags = 1<<28; // global block
   block->invoke = func;
   block->data = data;
   return block;
}


static void release_block(Block *block)
{
   block->next = free_blocks;
   free_blocks = block;
}


void trigger_delayed_gc(Heap *heap)
{
}


void free_view(View *view)
{
   int i;

   switch (view->common.type) {
      case TYPE_TABLE: {
         for (i=0; i<view->table.num_columns*view->table.num_rows; i++) {
            free(view->table.data[i]);
         }
         free(view->table.data);

         view->table.num_columns = 0;
         view->table.num_rows = 0;
         view->table.data = NULL;

         release(view->table.data_obj);
         break;
      }
   }
   free(view);
}


void free_menu(Menu *menu)
{
   free(menu);
}


void free_notify_icon(NotifyIcon *icon)
{
   ImageData *idat;
   int i;

   release(icon->item);
   if (icon->num_images > 0) {
      for (i=0; i<icon->num_images; i++) {
         idat = &icon->images[i];
         CGImageRelease(idat->img);
         CGDataProviderRelease(idat->provider);
         free(idat->pixels);
      }
      CGColorSpaceRelease(icon->space);
   }
   free(icon);
}


void view_destroy(View *view)
{
   switch (view->common.type) {
      case TYPE_WINDOW: {
         if (view->window.close_requested) {
            view->window.close_requested = 2;
         }
         else {
            call(view->obj, "close", void, ());
         }
         break;
      }
   }
}


static void flip_rect(NSRect *rect, CGFloat parent_height)
{
   rect->origin.y = parent_height - rect->size.height - rect->origin.y;
}


static void flip_screen_rect(NSRect *rect)
{
   NSRect frame;
   id screens, screen;

   screens = call((id)class("NSScreen"), "screens", id, ());
   screen = call(screens, "objectAtIndex:", id, (int), 0);
   frame = call_stret(screen, "frame", NSRect, ());

   flip_rect(rect, frame.size.height);
}


static void from_nsrect(Rect *rect, NSRect ns, float scale)
{
   rect->x1 = roundf(ns.origin.x * scale);
   rect->y1 = roundf(ns.origin.y * scale);
   rect->x2 = roundf((ns.origin.x + ns.size.width)*scale);
   rect->y2 = roundf((ns.origin.y + ns.size.height)*scale);
}


static NSRect to_nsrect(Rect *rect, float scale)
{
   NSRect ns;
   ns.origin.x = rect->x1 / scale;
   ns.origin.y = rect->y1 / scale;
   ns.size.width = (rect->x2 - rect->x1) / scale;
   ns.size.height = (rect->y2 - rect->y1) / scale;
   return ns;
}


void view_get_rect(View *view, Rect *rect)
{
   NSRect frame;
   float scale;

   if (view->common.type == TYPE_WINDOW) {
      scale = view_get_scale(view);
      frame = call_stret(view->obj, "frame", NSRect, ());
      flip_screen_rect(&frame);
      from_nsrect(rect, frame, scale);
   }
   else {
      *rect = view->rect;
   }
}


static void get_view_metrics(View *view, Rect *rect, int *width, int *height)
{
   float scale = view_get_scale(view);

   #define METRICS(w, h, left, top, right, bottom) \
   { \
      if (width) *width = roundf(w * scale); \
      if (height) *height = roundf(h * scale); \
      if (rect) { \
         rect->x1 -= roundf(left * scale); \
         rect->y1 -= roundf(top * scale); \
         rect->x2 += roundf(right * scale); \
         rect->y2 += roundf(bottom * scale); \
      } \
   }

   #define M1(w, h, left, top, right, bottom) \
   { \
      if (control_size == NSControlSizeRegular) METRICS(w, h, left, top, right, bottom); \
   }

   #define M2(w, h, left, top, right, bottom) \
   { \
      if (control_size == NSControlSizeSmall) METRICS(w, h, left, top, right, bottom); \
   }

   #define M3(w, h, left, top, right, bottom) \
   { \
      if (control_size == NSControlSizeMini) METRICS(w, h, left, top, right, bottom); \
   }

   switch (view->common.type) {
      case TYPE_LABEL:
      case TYPE_TEXT_FIELD:
      case TYPE_TEXT_AREA:
      case TYPE_TABLE: {
         METRICS(16, 21, 0, 0, 0, 0);
         break;
      }

      case TYPE_BUTTON: {
         int bezel_style = call(view->obj, "bezelStyle", int, ());
         int control_size;
         id cell;
   
         if (call(view->obj, "respondsToSelector:", BOOL, (SEL), sel("controlSize"))) {
            control_size = call(view->obj, "controlSize", int, ());
         }
         else {
            cell = call(view->obj, "cell", id, ());
            control_size = call(cell, "controlSize", int, ());
         }
         
         if (NSAppKitVersionNumber >= NSAppKitVersionNumber10_9) {
            switch (bezel_style) {
               default:
               case NSBezelStyleRounded:           M1(70,21, 6,4,6,7); M2(60,18, 5,4,5,6); M3(55,15, 1,0,1,1); break; // Push Button
               case NSBezelStyleRegularSquare:     M1(60,59, 2,2,2,3); M2(60,59, 2,2,2,3); M3(60,59, 2,2,2,3); break; // Bevel Button
               case NSBezelStyleDisclosure:        M1(13,13, 0,0,0,0); M2(13,13, 0,0,0,0); M3(13,13, 0,0,0,0); break; // Disclosure Triangle
               case NSBezelStyleShadowlessSquare:  M1(48,48, 0,0,0,0); M2(48,48, 0,0,0,0); M3(48,48, 0,0,0,0); break; // Square Button
               case NSBezelStyleCircular:          M1(26,26, 6,3,7,9); M2(20,20, 6,5,6,7); M3(17,17, 6,3,7,10); break; // Round Button
               case NSBezelStyleTexturedSquare:    M1(70,20, 0,1,0,2); M2(60,18, 0,0,0,1); M3(55,15, 0,1,0,1); break; // Textured Button
               case NSBezelStyleHelpButton:        M1(21,21, 2,1,2,3); M2(18,18, 2,1,2,3); M3(15,15, 1,2,3,2); break; // Help Button
               case NSBezelStyleSmallSquare:       M1(30,24, 0,1,0,1); M2(26,21, 0,1,0,1); M3(18,15, 0,1,0,1); break; // Gradient Button
               case NSBezelStyleTexturedRounded:   M1(70,22, 0,1,0,2); M2(60,18, 0,0,0,1); M3(55,15, 0,1,0,1); break; // Rounded Textured Button
               case NSBezelStyleRoundRect:         M1(18,18, 0,0,0,1); M2(16,16, 0,0,0,1); M3(14,14, 0,1,0,2); break; // Rounded Rect Button
               case NSBezelStyleRecessed:          M1(70,18, 0,0,0,1); M2(60,16, 0,0,0,1); M3(55,14, 0,1,0,2); break; // Recessed Button
               case NSBezelStyleRoundedDisclosure: M1(21,21, 4,2,4,3); M2(19,18, 3,2,3,3); M3(15,15, 1,0,1,1); break; // Disclosure Button
               case NSBezelStyleInline:            M1(70,14, 0,0,0,1); M2(60,14, 0,0,0,1); M3(55,14, 0,0,0,1); break; // Inline Button
            }
         }
         else {
            switch (bezel_style) {
               default:
               case NSBezelStyleRounded:           M1(84,20, 6,4,6,8); M2(72,17, 5,4,5,7); M3(60,14, 1,0,1,2); break; // Push Button
               case NSBezelStyleRegularSquare:     M1(60,58, 2,2,2,4); M2(60,58, 2,2,2,4); M3(60,58, 2,2,2,4); break; // Bevel Button
               case NSBezelStyleDisclosure:        M1(13,13, 0,0,0,0); M2(13,13, 0,0,0,0); M3(13,13, 0,0,0,0); break; // Disclosure Triangle
               case NSBezelStyleShadowlessSquare:  M1(48,48, 0,0,0,0); M2(48,48, 0,0,0,0); M3(48,48, 0,0,0,0); break; // Square Button
               case NSBezelStyleCircular:          M1(26,27, 6,4,7,7); M2(20,21, 6,5,6,6); M3(17,18, 6,5,7,7); break; // Round Button
               case NSBezelStyleTexturedSquare:    M1(84,21, 1,1,1,1); M2(72,16, 0,1,0,1); M3(60,16, 1,1,1,1); break; // Textured Button
               case NSBezelStyleHelpButton:        M1(19,20, 3,1,3,4); M2(19,20, 3,1,3,4); M3(19,20, 3,1,3,4); break; // Help Button
               case NSBezelStyleSmallSquare:       M1(30,24, 0,1,0,1); M2(26,21, 0,1,0,1); M3(18,15, 0,1,0,1); break; // Gradient Button
               case NSBezelStyleTexturedRounded:   M1(84,22, 0,1,0,2); M2(72,18, 0,0,0,0); M3(60,15, 0,0,0,0); break; // Rounded Textured Button
               case NSBezelStyleRoundRect:         M1(30,17, 0,0,0,2); M2(27,15, 0,0,0,2); M3(25,14, 0,1,0,2); break; // Rounded Rect Button
               case NSBezelStyleRecessed:          M1(84,17, 0,0,0,2); M2(72,15, 0,0,0,2); M3(60,15, 0,0,0,2); break; // Recessed Button
               case NSBezelStyleRoundedDisclosure: M1(21,21, 4,2,4,3); M2(19,18, 3,2,3,3); M3(17,16, 0,0,0,0); break; // Disclosure Button
               case NSBezelStyleInline:            M1(84,14, 0,0,0,1); M2(72,14, 0,0,0,1); M3(60,14, 0,0,0,1); break; // Inline Button
            }
         }
         break;
      }
   }

   #undef METRICS
   #undef M1
   #undef M2
   #undef M3
}


static void center_label(View *view)
{
   NSRect frame;

   frame = call_stret(view->obj, "frame", NSRect, ());
   frame.origin.x = 0;
   frame.origin.y = (frame.size.height - 17) / 2;
   frame.size.height = 17;
   call(view->label.obj, "setFrame:", void, (NSRect), frame);
}


void view_set_rect(View *view, Rect *rect)
{
   NSRect frame, superframe;
   id superview;
   float scale;
   
   if (view->common.type == TYPE_WINDOW) {
      scale = view_get_scale(view);
      frame = to_nsrect(rect, scale);
      flip_screen_rect(&frame);
      call(view->obj, "setFrame:display:", void, (NSRect, BOOL), frame, 0);
   }
   else {
      view->rect = *rect;
      if (view->common.parent) {
         scale = view_get_scale(view);
         superview = call(view->obj, "superview", id, ());
         superframe = call_stret(superview, "frame", NSRect, ());
         get_view_metrics(view, rect, NULL, NULL);
         frame = to_nsrect(rect, scale);
         flip_rect(&frame, superframe.size.height);
         call(view->obj, "setFrame:", void, (NSRect), frame);

         if (view->common.type == TYPE_CANVAS && (view->canvas.flags & CANVAS_SCROLLABLE) != 0) {
            NSRect rect;

            call(view->obj, "setHasHorizontalScroller:", void, (BOOL), (view->canvas.scroll[0].max > 0 || view->canvas.scroll[0].always_show) != 0);
            call(view->obj, "setHasVerticalScroller:", void, (BOOL), (view->canvas.scroll[1].max > 0 || view->canvas.scroll[1].always_show) != 0);

            rect.origin.x = 0;
            rect.origin.y = 0;
            #ifdef __LP64__
               rect.size = call(view->obj, "contentSize", NSSize, ());
            #else
               rect.size = call_stret(view->obj, "contentSize", NSSize, ());
            #endif

            rect.size.width += view->canvas.scroll[0].max / scale;
            rect.size.height += view->canvas.scroll[1].max / scale;
         
            call(view->canvas.wrapper, "setFrame:", void, (NSRect), rect);
            call(view->canvas.obj, "setFrame:", void, (NSRect), rect);

            if (!view->canvas.placed) {
               NSPoint point;
               view->canvas.placed = 1;
               point.x = view->canvas.scroll[0].pos / scale;
               point.y = view->canvas.scroll[1].pos / scale;
               call(view->canvas.wrapper, "scrollPoint:", void, (NSPoint), point);
            }
         }

         if (view->common.type == TYPE_LABEL) {
            center_label(view);
         }
         if (view->common.type == TYPE_CANVAS) {
            call_view_callback(view, CALLBACK_CANVAS_RESIZE);
         }
      }
   }
}


void view_get_content_rect(View *view, Rect *rect)
{
   NSRect frame, parent_frame;
   float scale;
   id content;

   rect->x1 = 0;
   rect->y1 = 0;
   rect->x2 = 0;
   rect->y2 = 0;

   if (view->common.type == TYPE_WINDOW) {
      scale = view_get_scale(view);
      content = call(view->obj, "contentView", id, ());
      
      frame = call_stret(content, "frame", NSRect, ());
      parent_frame = call_stret(view->obj, "frame", NSRect, ());
      flip_rect(&frame, parent_frame.size.height);
      from_nsrect(rect, frame, scale);
   }
}


void view_get_inner_rect(View *view, Rect *rect)
{
   view_get_content_rect(view, rect);
}


void view_set_visible(View *view, int visible)
{
   if (view->common.type == TYPE_WINDOW) {
      if (visible) {
         call_view_callback(view, CALLBACK_WINDOW_RESIZE);

         if (view->window.flags & WIN_MAXIMIZE) {
            call(view->obj, "makeKeyAndOrderFront:", void, (id), NULL);
            call(view->obj, "zoom:", void, (id), NULL);
         }
         else if (view->window.flags & WIN_MINIMIZE) {
            call(view->obj, "miniaturize:", void, (id), NULL);
         }
         else {
            call(view->obj, "makeKeyAndOrderFront:", void, (id), NULL);
         }
      }
   }
}


int view_add(View *parent, View *view)
{
   NSRect super_frame, frame;
   Rect rect;
   id superview;
   float scale;
   
   superview = parent->obj;
   if (parent->common.type == TYPE_WINDOW) {
      superview = call(superview, "contentView", id, ());
   }

   scale = view_get_scale(parent);
   super_frame = call_stret(superview, "frame", NSRect, ());
   rect = view->rect;
   get_view_metrics(view, &rect, NULL, NULL);
   frame = to_nsrect(&rect, scale);
   flip_rect(&frame, super_frame.size.height);
   call(view->obj, "setFrame:", void, (NSRect), frame);
   
   if (view->common.type == TYPE_LABEL) {
      center_label(view);
   }

   call(superview, "addSubview:", void, (id), view->obj);
   return 1;
}


void view_focus(View *view)
{
   View *top = view;
   id obj;

   while (top->common.parent) {
      top = top->common.parent;
   }
   if (top->common.type != TYPE_WINDOW) return;
   if (top == view) return;

   obj = view->obj;
   if (view->common.type == TYPE_CANVAS) {
      obj = view->canvas.obj;
   }

   if (call(obj, "acceptsFirstResponder", BOOL, ())) {
      call(top->obj, "makeFirstResponder:", BOOL, (id), obj);
   }
}


int view_has_focus(View *view)
{
   View *top = view;
   id obj;

   while (top->common.parent) {
      top = top->common.parent;
   }
   if (top->common.type != TYPE_WINDOW) return 0;
   if (top == view) return 0;

   obj = view->obj;
   if (view->common.type == TYPE_CANVAS) {
      obj = view->canvas.obj;
   }

   return call(top->obj, "firstResponder", id, ()) == obj;
}


void view_get_sizing(View *view, float *grid_x, float *grid_y, int *form_small, int *form_medium, int *form_large, int *view_small, int *view_medium, int *view_large)
{
   float scale = view_get_scale(view);
   *grid_x = 5 * scale;
   *grid_y = 5 * scale;
   *form_small = roundf(10 * scale);
   *form_medium = roundf(20 * scale);
   *form_large = roundf(30 * scale);
   *view_small = roundf(12 * scale);
   *view_medium = roundf(24 * scale);
   *view_large = roundf(32 * scale);
}


void view_get_default_size(View *view, int *width, int *height)
{
   get_view_metrics(view, NULL, width, height);
}


float view_get_scale(View *view)
{
   View *top = view;
   float scale = 1.0f;
   id screen;
   
   if (NSAppKitVersionNumber < NSAppKitVersionNumber10_7) {
      return scale;
   }

   if (top) {
      while (top->common.parent) {
         top = top->common.parent;
      }
   }
   if (top && top->common.type == TYPE_WINDOW) {
      scale = call(top->obj, "backingScaleFactor", CGFloat, ());
   }
   else {
      screen = call((id)class("NSScreen"), "mainScreen", id, ());
      scale = call(screen, "backingScaleFactor", CGFloat, ());
   }
   
   return scale;
}


void view_set_cursor(View *view, int cursor)
{
   if (view->common.type != TYPE_CANVAS) return; // TODO: add support for other views

   if (cursor < 0 || cursor >= NUM_CURSORS) {
      return;
   }

   if (view->canvas.cursor == cursor) {
      //return;
   }

   view->canvas.cursor = cursor;
   call(cursors[cursor], "set", void, ());
}


int view_get_cursor(View *view)
{
   if (view->common.type == TYPE_CANVAS) {
      return view->canvas.cursor;
   }
   return CURSOR_DEFAULT;
}


View *window_create(plat_char *title, int width, int height, int flags)
{
   View *view;
   NSRect rect;
   NSSize size;
   id delegate, str;
   float scale;
   int style;
   
   view = calloc(1, sizeof(View));
   if (!view) return NULL;

   scale = view_get_scale(NULL);
   rect.origin.x = 0;
   rect.origin.y = 0;
   rect.size.width = width / scale;
   rect.size.height = height / scale;

   style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
   if (flags & WIN_RESIZABLE) {
      style |= NSWindowStyleMaskResizable;
   }

   view->obj = call(alloc("NSWindow"), "initWithContentRect:styleMask:backing:defer:", id, (NSRect, uint32_t, int, BOOL),
      rect,
      style,
      NSBackingStoreBuffered,
      0
   );
   view->window.flags = flags;

   delegate = init(alloc("WindowDelegate"));
   ivar(delegate, "window_view", void *) = view;

   call(view->obj, "setDelegate:", void, (id), delegate);

   str = string(title);
   call(view->obj, "setTitle:", void, (id), str);
   release(str);

   size.width = width / scale;
   size.height = height / scale;
   call(view->obj, "setContentSize:", void, (NSSize), size);

   if (flags & WIN_CENTER) {
      call(view->obj, "center", void, ());
   }

   return view;
}


plat_char *window_get_title(View *view)
{
   const char *s;
   id str;
   
   str = call(view->obj, "title", id, ());
   s = call(str, "UTF8String", const char *, ());
   return strdup(s);
}


void window_set_title(View *view, plat_char *title)
{
   id str;

   str = string(title);
   call(view->obj, "setTitle:", void, (id), str);
   release(str);
}


void window_set_minimum_size(View *view, int width, int height)
{
   NSSize size;
   float scale;

   scale = view_get_scale(view);
   size.width = width / scale;
   size.height = height / scale;
   call(view->obj, "setContentMinSize:", void, (NSSize), size);
}


int window_is_maximized(View *view)
{
   return call(view->obj, "isZoomed", BOOL, ());
}


void window_set_status_text(View *view, plat_char *text)
{
}


static void add_menu_item(id menu, char *title, char *key, int mod, SEL sel, id submenu)
{
   id str1, str2, item;

   if (!title) {
      item = call((id)class("NSMenuItem"), "separatorItem", id, ());
      call(menu, "addItem:", void, (id), item);
      return;
   }

   str1 = string(title);
   str2 = string(key);
   item = call(menu, "addItemWithTitle:action:keyEquivalent:", id, (id, SEL, id),
      str1, sel, str2
   );
   if (mod) {
      call(item, "setKeyEquivalentModifierMask:", void, (NSUInteger), mod);
   }
   if (submenu) {
      call(item, "setSubmenu:", void, (id), submenu);
   }
   release(str1);
   release(str2);
}


static void insert_app_menu(id menu)
{
   char buf[256];
   id app, appmenu, str, services, item;

   app = call((id)class("NSApplication"), "sharedApplication", id, ());

   str = string("");
   appmenu = call(alloc("NSMenu"), "initWithTitle:", id, (id), str);
   services = call(alloc("NSMenu"), "initWithTitle:", id, (id), str);
   item = call(menu, "insertItemWithTitle:action:keyEquivalent:atIndex:", id, (id, SEL, id, NSInteger),
      str, NULL, str, 0
   );
   call(item, "setSubmenu:", void, (id), appmenu);
   call(app, "setServicesMenu:", void, (id), services);
   release(str);

   snprintf(buf, sizeof(buf), "About %s", app_name);
   add_menu_item(appmenu, buf, "", 0, sel("showAboutDialog:"), NULL);
   add_menu_item(appmenu, NULL, NULL, 0, NULL, NULL);
   add_menu_item(appmenu, "Preferences...", ",", 0, menu_handlers[MH_PREFERENCES].func.value? sel("showPreferencesDialog:") : NULL, NULL);
   add_menu_item(appmenu, NULL, NULL, 0, NULL, NULL);
   add_menu_item(appmenu, "Services", "", 0, NULL, services);
   add_menu_item(appmenu, NULL, NULL, 0, NULL, NULL);
   snprintf(buf, sizeof(buf), "Hide %s", app_name);
   add_menu_item(appmenu, buf, "h", 0, sel("hide:"), NULL);
   add_menu_item(appmenu, "Hide Others", "h", NSEventModifierFlagOption | NSEventModifierFlagCommand, sel("hideOtherApplications:"), NULL);
   add_menu_item(appmenu, "Show All", "", 0, sel("unhideAllApplications:"), NULL);
   add_menu_item(appmenu, NULL, NULL, 0, NULL, NULL);
   snprintf(buf, sizeof(buf), "Quit %s", app_name);
   add_menu_item(appmenu, buf, "q", 0, sel("terminate:"), NULL);
}


static void create_default_menubar()
{
   id app, str;

   app = call((id)class("NSApplication"), "sharedApplication", id, ());

   str = string("");
   default_menubar = call(alloc("NSMenu"), "initWithTitle:", id, (id), str);
   release(str);

   insert_app_menu(default_menubar);

   if (!menubar_set) {
      call(app, "setMainMenu:", void, (id), default_menubar);
   }
}


int window_set_menu(View *view, Menu *old_menu, Menu *menu)
{
   id app;

   if (main_menubar) return 0;
   if ((view->window.flags & WIN_MENUBAR) == 0) return 0;
   
   app = call((id)class("NSApplication"), "sharedApplication", id, ());

   if (!menu->has_app_menu) {
      insert_app_menu(menu->obj);
      menu->has_app_menu = 1;
   }

   if (call(view->obj, "isKeyWindow", BOOL, ())) {
      call(app, "setMainMenu:", void, (id), menu->obj);
      menubar_set = 1;
   }

   if (view->window.menu) {
      fixscript_unref(view->window.menu->common.heap, view->window.menu->common.instance);
   }
   view->window.menu = menu;
   fixscript_ref(menu->common.heap, menu->common.instance);
   return 1;
}


View *label_create(plat_char *label)
{
   NSRect rect;
   View *view;
   
   view = calloc(1, sizeof(View));
   if (!view) return NULL;

   rect.origin.x = 0;
   rect.origin.y = 0;
   rect.size.width = 32;
   rect.size.height = 32;
   view->label.obj = call(alloc("NSTextField"), "initWithFrame:", id, (NSRect), rect);
   call(view->label.obj, "setEditable:", void, (BOOL), 0);
   call(view->label.obj, "setBezeled:", void, (BOOL), 0);
   call(view->label.obj, "setDrawsBackground:", void, (BOOL), 0);
   label_set_label(view, label);

   view->obj = call(alloc("NSView"), "initWithFrame:", id, (NSRect), rect);
   call(view->obj, "addSubview:", void, (id), view->label.obj);
   return view;
}


plat_char *label_get_label(View *view)
{
   const char *s;
   plat_char *ret;
   id str;
   
   str = call(view->label.obj, "stringValue", id, ());
   s = call(str, "UTF8String", const char *, ());
   ret = strdup(s);
   return ret;
}


void label_set_label(View *view, plat_char *label)
{
   id str;

   str = string(label);
   call(view->label.obj, "setStringValue:", void, (id), str);
   release(str);
}


View *text_field_create()
{
   NSRect rect;
   View *view;
   
   view = calloc(1, sizeof(View));
   if (!view) return NULL;

   rect.origin.x = 0;
   rect.origin.y = 0;
   rect.size.width = 32;
   rect.size.height = 32;
   view->obj = call(alloc(create_search_field? "NSSearchField" : "NSTextField"), "initWithFrame:", id, (NSRect), rect);
   return view;
}


plat_char *text_field_get_text(View *view)
{
   const char *s;
   plat_char *ret;
   id str;

   str = call(view->obj, "stringValue", id, ());
   s = call(str, "UTF8String", const char *, ());
   ret = strdup(s);
   return ret;
}


void text_field_set_text(View *view, plat_char *text)
{
   id str;
   
   str = string(text);
   call(view->obj, "setStringValue:", void, (id), str);
   release(str);
}


int text_field_is_enabled(View *view)
{
   return call(view->obj, "isEnabled", BOOL, ());
}


void text_field_set_enabled(View *view, int enabled)
{
   call(view->obj, "setEnabled:", void, (BOOL), enabled);
}


View *text_area_create()
{
   NSRect rect;
   NSSize size;
   View *view;
   id container;
   
   view = calloc(1, sizeof(View));
   if (!view) return NULL;

   rect.origin.x = 0;
   rect.origin.y = 0;
   rect.size.width = 32;
   rect.size.height = 32;
   view->text_area.obj = call(alloc("NSTextView"), "initWithFrame:", id, (NSRect), rect);
   view->obj = call(alloc("NSScrollView"), "initWithFrame:", id, (NSRect), rect);
   call(view->obj, "setBorderType:", void, (int), 2);
   call(view->obj, "setHasHorizontalScroller:", void, (BOOL), 1);
   call(view->obj, "setHasVerticalScroller:", void, (BOOL), 1);
   call(view->obj, "setAutohidesScrollers:", void, (BOOL), 1);
   call(view->obj, "setAutoresizingMask:", void, (int), NSViewWidthSizable | NSViewHeightSizable);
   call(view->text_area.obj, "setHorizontallyResizable:", void, (BOOL), 1);
   call(view->text_area.obj, "setVerticallyResizable:", void, (BOOL), 1);
   call(view->text_area.obj, "setRichText:", void, (BOOL), 0);
   container = call(view->text_area.obj, "textContainer", id, ());
   size.width = DBL_MAX;
   size.height = DBL_MAX;
   call(container, "setWidthTracksTextView:", void, (BOOL), 0);
   call(container, "setContainerSize:", void, (NSSize), size);
   call(view->text_area.obj, "setMaxSize:", void, (NSSize), size);
   call(view->obj, "setDocumentView:", void, (id), view->text_area.obj);
   return view;
}


plat_char *text_area_get_text(View *view)
{
   const char *s;
   plat_char *ret;
   id str;

   str = call(view->text_area.obj, "string", id, ());
   s = call(str, "UTF8String", const char *, ());
   ret = strdup(s);
   return ret;
}


void text_area_set_text(View *view, plat_char *text)
{
   NSRange range;
   id str, storage;
   int read_only;
   
   read_only = text_area_is_read_only(view);
   if (read_only) {
      text_area_set_read_only(view, 0);
   }
   str = string(text);
   storage = call(view->text_area.obj, "textStorage", id, ());
   range.location = 0;
   range.length = call(storage, "length", NSUInteger, ());
   call(view->text_area.obj, "insertText:replacementRange:", void, (id, NSRange), str, range);
   release(str);
   if (read_only) {
      text_area_set_read_only(view, 1);
   }
}


void text_area_append_text(View *view, plat_char *text)
{
   plat_char *prev_text, *new_text;

   prev_text = text_area_get_text(view);
   new_text = malloc(strlen(prev_text)+strlen(text)+1);
   strcpy(new_text, prev_text);
   strcat(new_text, text);
   text_area_set_text(view, new_text);
   free(new_text);
   free(prev_text);
}


void text_area_set_read_only(View *view, int read_only)
{
   call(view->text_area.obj, "setEditable:", void, (BOOL), read_only? 0 : 1);
}


int text_area_is_read_only(View *view)
{
   return !call(view->text_area.obj, "isEditable", BOOL, ());
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
   NSRect rect;
   View *view;
   
   view = calloc(1, sizeof(View));
   if (!view) return NULL;

   rect.origin.x = 0;
   rect.origin.y = 0;
   rect.size.width = 32;
   rect.size.height = 32;
   view->obj = call(alloc("FixButton"), "initWithFrame:", id, (NSRect), rect);
   ivar(view->obj, "button_view", void *) = view;
   button_set_label(view, label);
   call(view->obj, "setBezelStyle:", void, (int), NSBezelStyleRounded);
   call(view->obj, "setTarget:", void, (id), view->obj);
   call(view->obj, "setAction:", void, (SEL), sel("buttonAction"));
   return view;
}


plat_char *button_get_label(View *view)
{
   const char *s;
   id str;

   str = call(view->obj, "title", id, ());
   s = call(str, "UTF8String", const char *, ());
   return strdup(s);
}


void button_set_label(View *view, plat_char *label)
{
   id str;
   
   str = string(label);
   call(view->obj, "setTitle:", void, (id), str);
   release(str);
}


int button_is_enabled(View *view)
{
   return call(view->obj, "isEnabled", BOOL, ());
}


void button_set_enabled(View *view, int enabled)
{
   call(view->obj, "setEnabled:", void, (BOOL), enabled);
}


View *table_create()
{
   NSRect rect;
   View *view;
   id header;
   
   view = calloc(1, sizeof(View));
   if (!view) return NULL;

   rect.origin.x = 0;
   rect.origin.y = 0;
   rect.size.width = 32;
   rect.size.height = 32;
   view->table.obj = call(alloc("FixTableView"), "initWithFrame:", id, (NSRect), rect);
   ivar(view->table.obj, "table_view", void *) = view;
   view->obj = call(alloc("NSScrollView"), "initWithFrame:", id, (NSRect), rect);
   call(view->obj, "setBorderType:", void, (int), 2);
   call(view->obj, "setHasHorizontalScroller:", void, (BOOL), 1);
   call(view->obj, "setHasVerticalScroller:", void, (BOOL), 1);
   call(view->obj, "setAutohidesScrollers:", void, (BOOL), 1);
   call(view->obj, "setAutoresizingMask:", void, (int), NSViewWidthSizable | NSViewHeightSizable);
   call(view->obj, "setDocumentView:", void, (id), view->table.obj);

   view->table.data_obj = init(alloc("FixTableData"));
   ivar(view->table.data_obj, "table_view", void *) = view;
   call(view->table.obj, "setDataSource:", void, (id), view->table.data_obj);
   call(view->table.obj, "setDelegate:", void, (id), view->table.data_obj);
   call(view->table.obj, "setAllowsColumnReordering:", void, (BOOL), 0);
   call(view->table.obj, "setTarget:", void, (id), view->table.data_obj);
   call(view->table.obj, "setDoubleAction:", void, (SEL), sel("clickAction:"));

   rect = call_stret(call(view->table.obj, "headerView", id, ()), "frame", NSRect, ());
   header = call(alloc("FixTableHeaderView"), "initWithFrame:", id, (NSRect), rect);
   ivar(header, "table_view", void *) = view;
   call(view->table.obj, "setHeaderView:", void, (id), header);

   return view;
}


void table_set_columns(View *view, int num_columns, plat_char **titles)
{
   id str, column, cell, array;
   char buf[32];
   int i;

   array = call(view->table.obj, "tableColumns", id, ());
   for (;;) {
      column = call(array, "lastObject", id, ());
      if (!column) break;
      call(view->table.obj, "removeTableColumn:", void, (id), column);
      release(column);
   }

   for (i=0; i<num_columns; i++) {
      snprintf(buf, sizeof(buf), "column%d", i);
      str = string(buf);
      column = call(alloc("NSTableColumn"), "initWithIdentifier:", id, (id), str);
      release(str);

      cell = call(column, "headerCell", id, ());
      str = string(titles[i]);
      call(cell, "setStringValue:", void, (id), str);
      release(str);

      call(column, "setEditable:", void, (BOOL), 0);

      call(view->table.obj, "addTableColumn:", void, (id), column);
   }

   for (i=0; i<view->table.num_columns*view->table.num_rows; i++) {
      free(view->table.data[i]);
   }
   free(view->table.data);

   view->table.num_columns = num_columns;
   view->table.num_rows = 0;
   view->table.data = NULL;

   call(view->table.obj, "reloadData", void, ());
}


int table_get_column_width(View *view, int idx)
{
   id array, column;
   float scale;

   if (idx < 0 || idx >= view->table.num_columns) {
      return 0;
   }

   array = call(view->table.obj, "tableColumns", id, ());
   column = call(array, "objectAtIndex:", id, (NSUInteger), idx);
   scale = view_get_scale(view);
   return call(column, "width", CGFloat, ()) * scale;
}


void table_set_column_width(View *view, int idx, int width)
{
   id array, column;
   float scale;

   if (idx < 0 || idx >= view->table.num_columns) {
      return;
   }

   array = call(view->table.obj, "tableColumns", id, ());
   column = call(array, "objectAtIndex:", id, (NSUInteger), idx);
   scale = view_get_scale(view);
   call(column, "setWidth:", void, (CGFloat), width / scale);

   call(view->table.obj, "setNeedsDisplay:", void, (BOOL), 1);
}


void table_clear(View *view)
{
   int i;

   for (i=0; i<view->table.num_columns*view->table.num_rows; i++) {
      free(view->table.data[i]);
   }
   view->table.num_rows = 0;

   call(view->table.obj, "reloadData", void, ());
}


void table_insert_row(View *view, int row, int num_columns, plat_char **values)
{
   char **new_data, **new_values;
   int i, off;

   if (INT_MAX - view->table.num_columns * view->table.num_rows * sizeof(char *) < view->table.num_columns * sizeof(char *)) {
      return;
   }

   if (row > view->table.num_rows) {
      return;
   }
   if (row < 0) {
      row = view->table.num_rows;
   }

   new_data = realloc(view->table.data, view->table.num_columns * (view->table.num_rows+1) * sizeof(char *));
   if (!new_data) {
      return;
   }
   view->table.data = new_data;

   new_values = calloc(num_columns, sizeof(char *));
   if (!new_values) {
      return;
   }

   for (i=0; i<num_columns; i++) {
      new_values[i] = strdup(values[i]);
      if (!new_values[i]) {
         while (--i >= 0) {
            free(new_values[i]);
         }
         free(new_values);
         return;
      }
   }

   off = view->table.num_columns * row;
   memmove(&view->table.data[off + view->table.num_columns], &view->table.data[off], view->table.num_columns * sizeof(char *) * (view->table.num_rows - row));
   for (i=0; i<num_columns; i++) {
      view->table.data[off + i] = new_values[i];
   }
   view->table.num_rows++;
   free(new_values);

   call(view->table.obj, "reloadData", void, ());
}


int table_get_selected_row(View *view)
{
   return call(view->table.obj, "selectedRow", NSInteger, ());
}


void table_set_selected_row(View *view, int row)
{
   id set;

   if (row >= view->table.num_rows) {
      row = -1;
   }

   if (row >= 0) {
      set = call((id)class("NSIndexSet"), "indexSetWithIndex:", id, (NSUInteger), row);
   }
   else {
      set = call((id)class("NSIndexSet"), "indexSet", id, ());
   }
   call(view->table.obj, "selectRowIndexes:byExtendingSelection:", void, (id, BOOL), set, NO);
}


static NSInteger table_number_of_rows_method(id self, SEL selector, id table_view)
{
   View *view = ivar(self, "table_view", void *);

   return view->table.num_rows;
}


static id table_object_value_method(id self, SEL selector, id table_view, id table_column, NSInteger _row)
{
   View *view = ivar(self, "table_view", void *);
   id array;
   int column, row = _row;

   array = call(view->table.obj, "tableColumns", id, ());
   column = call(array, "indexOfObject:", NSUInteger, (id), table_column);

   if (column >= view->table.num_columns || row >= view->table.num_rows) {
      return string("");
   }

   return string(view->table.data[row*view->table.num_columns+column]);
}


static void table_click_action(id self, SEL selector, id sender)
{
   View *view = ivar(self, "table_view", void *);
   int column, row;

   column = call(view->table.obj, "clickedColumn", NSInteger, ());
   row = call(view->table.obj, "clickedRow", NSInteger, ());

   call_table_action_callback(view, CALLBACK_TABLE_CLICK_ACTION, column, row, 0, 0);
}


static void table_handle_header_mouse_event(id self, SEL selector, id event)
{
   View *view = ivar(self, "table_view", void *);
   NSPoint point;
   int type, column;

   #ifdef __LP64__
      point = call(event, "locationInWindow", NSPoint, ());
      point = call(self, "convertPoint:fromView:", NSPoint, (NSPoint, id), point, NULL);
   #else
      point = call_stret(event, "locationInWindow", NSPoint, ());
      point = call_stret(self, "convertPoint:fromView:", NSPoint, (NSPoint, id), point, NULL);
   #endif

   /*
   TODO:
   type = call(event, "type", NSUInteger, ());
         fprintf(stderr, "type = %d\n", type);
         fflush(stderr);
   switch (type) {
      case NSEventTypeLeftMouseDown:
         view->table.dragged_column = 0;
         fprintf(stderr, "dragged = 0\n");
         fflush(stderr);
         call_super(self, class("NSTableHeaderView"), selector, void, (id), event);
         return;

      case NSEventTypeLeftMouseDragged:
         view->table.dragged_column = 1;
         fprintf(stderr, "dragged = 1\n");
         fflush(stderr);
         call_super(self, class("NSTableHeaderView"), selector, void, (id), event);
         return;

      case NSEventTypeLeftMouseUp:
         fprintf(stderr, "left up\n");
         fflush(stderr);
         if (!view->table.dragged_column) {
            column = call(self, "columnAtPoint:", NSInteger, (NSPoint), point);
            fprintf(stderr, "left click %d\n", column);
            fflush(stderr);
            call_super(self, class("NSTableHeaderView"), selector, void, (id), event);
            call_table_action_callback(view, CALLBACK_TABLE_SORT_ACTION, column, 0, 0, 0);
         }
         else {
            call_super(self, class("NSTableHeaderView"), selector, void, (id), event);
         }
         return;
   }
   */
   column = call(self, "columnAtPoint:", NSInteger, (NSPoint), point);
   call_super(self, class("NSTableHeaderView"), selector, void, (id), event);
   call_table_action_callback(view, CALLBACK_TABLE_SORT_ACTION, column, 0, 0, 0);
}


static void table_handle_right_mouse_event(id self, SEL selector, id event)
{
   View *view = ivar(self, "table_view", void *);
   NSPoint point, point_scroll;
   float scale;
   int column, row, x, y;

   #ifdef __LP64__
      point = call(event, "locationInWindow", NSPoint, ());
      point_scroll = call(view->obj, "convertPoint:fromView:", NSPoint, (NSPoint, id), point, NULL);
      point = call(self, "convertPoint:fromView:", NSPoint, (NSPoint, id), point, NULL);
   #else
      point = call_stret(event, "locationInWindow", NSPoint, ());
      point_scroll = call_stret(view->obj, "convertPoint:fromView:", NSPoint, (NSPoint, id), point, NULL);
      point = call_stret(self, "convertPoint:fromView:", NSPoint, (NSPoint, id), point, NULL);
   #endif

   column = call(self, "columnAtPoint:", NSInteger, (NSPoint), point);
   row = call(self, "rowAtPoint:", NSInteger, (NSPoint), point);

   scale = view_get_scale(view);
   x = point_scroll.x * scale;
   y = point_scroll.y * scale;

   if (call_table_action_callback(view, CALLBACK_TABLE_RIGHT_CLICK_ACTION, column, row, x, y)) return;

   call_super(self, class("NSTableView"), selector, void, (id), event);
}


static void table_handle_key_event(id self, SEL selector, id event)
{
   View *view = ivar(self, "table_view", void *);
   int code, row;

   code = call(event, "keyCode", unsigned short, ());
   if (code == 49) {
      row = call(view->table.obj, "selectedRow", NSInteger, ());

      if (call_table_action_callback(view, CALLBACK_TABLE_SPACE_KEY_ACTION, 0, row, 0, 0)) return;
   }

   call_super(self, class("NSTableView"), selector, void, (id), event);
}


static BOOL table_accepts_preview_panel_method(id self, SEL selector, id preview_panel)
{
   return preview_panel_path? YES : NO;
}


static void table_begin_preview_panel_method(id self, SEL selector, id preview_panel)
{
   call(preview_panel, "setDataSource:", void, (id), preview_data_source);
   call(preview_panel, "setDelegate:", void, (id), preview_data_source);
}


static void table_end_preview_panel_method(id self, SEL selector, id preview_panel)
{
   call(preview_panel, "setDataSource:", void, (id), NULL);
   call(preview_panel, "setDelegate:", void, (id), NULL);
}


View *canvas_create(int flags)
{
   NSRect rect;
   NSSize size;
   View *view;
   id tracking_area;
   
   view = calloc(1, sizeof(View));
   if (!view) return NULL;

   view->canvas.flags = flags;

   rect.origin.x = 0;
   rect.origin.y = 0;
   rect.size.width = 32;
   rect.size.height = 32;

   if (flags & CANVAS_SCROLLABLE) {
      view->obj = call(alloc("NSScrollView"), "initWithFrame:", id, (NSRect), rect);
      if ((flags & CANVAS_BORDER) == 0) {
         call(view->obj, "setBorderType:", void, (int), 0);
      }
      call(view->obj, "setHasHorizontalScroller:", void, (BOOL), 0);
      call(view->obj, "setHasVerticalScroller:", void, (BOOL), 0);

      rect.size.width = size.width;
      rect.size.height = size.height;
      view->canvas.wrapper = call(alloc("FixCanvasWrapper"), "initWithFrame:", id, (NSRect), rect);
      view->canvas.obj = call(alloc("FixCanvas"), "initWithFrame:", id, (NSRect), rect);
      ivar(view->canvas.obj, "canvas_view", void *) = view;
      call(view->canvas.wrapper, "addSubview:", void, (id), view->canvas.obj);
      call(view->obj, "setDocumentView:", void, (id), view->canvas.wrapper);
      view->canvas.scroll_view = call(view->canvas.wrapper, "superview", id, ());
   }
   else {
      view->canvas.obj = call(alloc("FixCanvas"), "initWithFrame:", id, (NSRect), rect);
      ivar(view->canvas.obj, "canvas_view", void *) = view;
      view->obj = view->canvas.obj;
   }

   tracking_area = call(alloc("NSTrackingArea"), "initWithRect:options:owner:userInfo:", id, (NSRect, NSUInteger, id, id),
      rect,
      NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved | NSTrackingActiveAlways | NSTrackingInVisibleRect,
      view->canvas.obj,
      NULL
   );
   call(view->canvas.obj, "addTrackingArea:", void, (id), tracking_area);
   release(tracking_area);
   
   return view;
}


void canvas_set_scroll_state(View *view, int type, int pos, int max, int page_size, int always_show)
{
   Rect rect;

   if ((view->canvas.flags & CANVAS_SCROLLABLE) == 0) return;

   view->canvas.placed = 0;
   view->canvas.scroll[type].pos = pos;
   view->canvas.scroll[type].max = max;
   view->canvas.scroll[type].always_show = always_show;

   if (view->common.parent) {
      view_get_rect(view, &rect);
      view_set_rect(view, &rect);
   }
}


void canvas_set_scroll_position(View *view, int type, int pos)
{
   NSPoint point;
   NSRect bounds;
   Rect rect;
   float scale;

   if ((view->canvas.flags & CANVAS_SCROLLABLE) == 0) return;

   scale = view_get_scale(view);
   bounds = call_stret(view->canvas.scroll_view, "bounds", NSRect, ());
   view->canvas.scroll[0].pos = roundf(bounds.origin.x * scale);
   view->canvas.scroll[1].pos = roundf(bounds.origin.y * scale);
   view->canvas.scroll[type].pos = pos;

   if (view->canvas.placed) {
      point.x = view->canvas.scroll[0].pos / scale;
      point.y = view->canvas.scroll[1].pos / scale;
      call(view->canvas.wrapper, "scrollPoint:", void, (NSPoint), point);
   }
   else {
      view_get_rect(view, &rect);
      view_set_rect(view, &rect);
   }
}


int canvas_get_scroll_position(View *view, int type)
{
   NSRect bounds;
   CGFloat pos;
   float scale;

   if ((view->canvas.flags & CANVAS_SCROLLABLE) == 0) return 0;

   scale = view_get_scale(view);
   bounds = call_stret(view->canvas.scroll_view, "bounds", NSRect, ());
   pos = type == 0? bounds.origin.x : bounds.origin.y;
   return roundf(pos * scale);
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
   view->canvas.focusable = enable;
}


int canvas_is_focusable(View *view)
{
   return view->canvas.focusable;
}


void canvas_repaint(View *view, Rect *rect)
{
   NSRect frame, nsrect;
   
   if (rect) {
      frame = call_stret(view->canvas.obj, "frame", NSRect, ());
      nsrect = to_nsrect(rect, view_get_scale(view));
      flip_rect(&nsrect, frame.size.height);
      call(view->canvas.obj, "setNeedsDisplayInRect:", void, (NSRect), nsrect);
   }
   else {
      call(view->canvas.obj, "setNeedsDisplay:", void, (BOOL), 1);
   }
}


Menu *menu_create()
{
   Menu *menu;
   id str;
   
   menu = calloc(1, sizeof(Menu));
   if (!menu) return NULL;

   str = string("");
   menu->obj = call(alloc("NSMenu"), "initWithTitle:", id, (id), str);
   release(str);

   return menu;
}


static void remove_ampersand(plat_char *title)
{
   plat_char *s;

   for (s=title; *s; s++) {
      if (*s == '&') {
         memmove(s, s+1, strlen(s));
         s--;
      }
   }
}


void menu_insert_item(Menu *menu, int idx, plat_char *title, MenuItem *menu_item)
{
   id str1, str2, item, data;

   remove_ampersand(title);
   str1 = string(title);
   str2 = string("");
   if (idx == -1) {
      idx = call(menu->obj, "numberOfItems", NSInteger, ());
      item = call(menu->obj, "addItemWithTitle:action:keyEquivalent:", id, (id, SEL, id),
         str1, sel("clickAction:"), str2
      );
   }
   else {
      item = call(menu->obj, "insertItemWithTitle:action:keyEquivalent:atIndex:", id, (id, SEL, id, NSInteger),
         str1, sel("clickAction:"), str2, idx
      );
   }
   release(str1);
   release(str2);

   data = init(alloc("FixMenuItemData"));
   ivar(data, "menu", void *) = menu;
   ivar(data, "pos", int) = idx;
   call(item, "setRepresentedObject:", void, (id), data);
   call(item, "setTarget:", void, (id), data);
   release(data);
}


void menu_insert_separator(Menu *menu, int idx)
{
   id item;

   item = call((id)class("NSMenuItem"), "separatorItem", id, ());
   
   if (idx == -1) {
      call(menu->obj, "addItem:", void, (id), item);
   }
   else {
      call(menu->obj, "insertItem:atIndex:", void, (id, NSInteger), item, idx);
   }
}


int menu_insert_submenu(Menu *menu, int idx, plat_char *title, Menu *submenu)
{
   id str1, str2, item;

   remove_ampersand(title);
   str1 = string(title);
   str2 = string("");

   if (idx == -1) {
      item = call(menu->obj, "addItemWithTitle:action:keyEquivalent:", id, (id, SEL, id),
         str1, NULL, str2
      );
   }
   else {
      item = call(menu->obj, "insertItemWithTitle:action:keyEquivalent:atIndex:", id, (id, SEL, id, NSInteger),
         str1, NULL, str2, idx
      );
   }

   call(submenu->obj, "setTitle:", void, (id), str1);
   call(item, "setSubmenu:", void, (id), submenu->obj);

   release(str1);
   release(str2);
   return 1;
}


void menu_remove_item(Menu *menu, int idx, MenuItem *item)
{
   // TODO
}


void menu_show(Menu *menu, View *view, int x, int y)
{
   NSPoint point;
   id view_obj;
   float scale;

   view_obj = view->obj;
   if (view->common.type == TYPE_CANVAS) {
      view_obj = view->canvas.obj;
   }

   scale = view_get_scale(view);
   point.x = x / scale;
   point.y = y / scale;
   
   call(menu->obj, "popUpMenuPositioningItem:atLocation:inView:", BOOL, (id, NSPoint, id),
      NULL, point, view_obj
   );
}


static id create_message_window(int type, char *title, char *msg)
{
   id alert, str;
   int style;

   alert = init(alloc("NSAlert"));

   switch (type & 0xFF) {
      default:
      case MSG_OK:
         str = string("OK");
         call(alert, "addButtonWithTitle:", id, (id), str);
         release(str);
         break;

      case MSG_OK_CANCEL:
         str = string("OK");
         call(alert, "addButtonWithTitle:", id, (id), str);
         release(str);
         str = string("Cancel");
         call(alert, "addButtonWithTitle:", id, (id), str);
         release(str);
         break;

      case MSG_YES_NO:
         str = string("Yes");
         call(alert, "addButtonWithTitle:", id, (id), str);
         release(str);
         str = string("No");
         call(alert, "addButtonWithTitle:", id, (id), str);
         release(str);
         break;

      case MSG_YES_NO_CANCEL:
         str = string("Yes");
         call(alert, "addButtonWithTitle:", id, (id), str);
         release(str);
         str = string("No");
         call(alert, "addButtonWithTitle:", id, (id), str);
         release(str);
         str = string("Cancel");
         call(alert, "addButtonWithTitle:", id, (id), str);
         release(str);
         break;
   }

   switch (type & 0xFF00) {
      default:
      case MSG_ICON_INFO:     style = NSAlertStyleInformational; break;
      case MSG_ICON_QUESTION: style = NSAlertStyleInformational; break;
      case MSG_ICON_ERROR:    style = NSAlertStyleCritical; break;
      case MSG_ICON_WARNING:  style = NSAlertStyleWarning; break;
   }

   call(alert, "setAlertStyle:", void, (int), style);

   str = string(title);
   call(alert, "setMessageText:", void, (id), str);
   release(str);
   
   str = string(msg);
   call(alert, "setInformativeText:", void, (id), str);
   release(str);

   return alert;
}


static int get_message_return_code(int type, int ret)
{
   switch (type & 0xFF) {
      case MSG_OK:
         if (ret == 1000) return MSG_BTN_OK;
         break;

      case MSG_OK_CANCEL:
         if (ret == 1000) return MSG_BTN_OK;
         if (ret == 1001) return MSG_BTN_CANCEL;
         break;

      case MSG_YES_NO:
         if (ret == 1000) return MSG_BTN_YES;
         if (ret == 1001) return MSG_BTN_NO;
         break;

      case MSG_YES_NO_CANCEL:
         if (ret == 1000) return MSG_BTN_YES;
         if (ret == 1001) return MSG_BTN_NO;
         if (ret == 1002) return MSG_BTN_CANCEL;
         break;
   }
   return MSG_BTN_CANCEL;
}


int show_message(View *window, int type, plat_char *title, plat_char *msg)
{
   int ret;
   id alert;

   alert = create_message_window(type, title, msg);
   ret = call(alert, "runModal", int, ());
   release(alert);
   return get_message_return_code(type, ret);
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
   struct timeval tv;

   if (gettimeofday(&tv, NULL) != 0) {
      tv.tv_sec = 0;
      tv.tv_usec = 0;
   }

   return tv.tv_sec * 1000LL + tv.tv_usec / 1000;
}


uint32_t timer_get_micro_time()
{
   struct timeval tv;

   if (gettimeofday(&tv, NULL) != 0) {
      tv.tv_sec = 0;
      tv.tv_usec = 0;
   }

   return tv.tv_sec * 1000000LL + tv.tv_usec;
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


void timer_start(Heap *heap, Value instance, int interval, int restart)
{
   Timer *timer, **prev = &active_timers;
   id data;
   
   for (timer = active_timers; timer; timer = timer->next) {
      if (timer->heap == heap && timer->instance.value == instance.value && timer->instance.is_array == instance.is_array) {
         break;
      }
      prev = &timer->next;
   }

   if (timer) {
      if (restart) {
         call(timer->timer, "invalidate", void, ());
         fixscript_unref(timer->heap, timer->instance);
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

   data = init(alloc("FixTimerData"));
   ivar(data, "timer", void *) = timer;
   timer->timer = call((id)class("NSTimer"), "scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:", id, (double, id, SEL, id, BOOL), interval/1000.0, data, sel("timerFireMethod:"), NULL, 1);
   release(data);
}


void timer_stop(Heap *heap, Value instance)
{
   Timer *timer, **prev = &active_timers;
   
   for (timer = active_timers; timer; timer = timer->next) {
      if (timer->heap == heap && timer->instance.value == instance.value && timer->instance.is_array == instance.is_array) {
         call(timer->timer, "invalidate", void, ());
         fixscript_unref(timer->heap, timer->instance);
         *prev = timer->next;
         break;
      }
      prev = &timer->next;
   }
}


static void timer_fire_method(id self, SEL selector, id timer_obj)
{
   Timer *timer = ivar(self, "timer", void *);

   timer_run(timer->heap, timer->instance);
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
   id str;
   
   font = calloc(1, sizeof(SystemFont));
   if (!font) return NULL;

   str = string(family);
   font->font = call((id)class("NSFont"), "fontWithName:size:", id, (id, CGFloat), str, size);
   release(str);
   return font;
}


void system_font_destroy(SystemFont *font)
{
   release(font->font);
   free(font);
}


plat_char **system_font_get_list()
{
   return NULL;
}


int system_font_get_size(SystemFont *font)
{
   return (int)roundf(call(font->font, "pointSize", CGFloat, ()));
}


int system_font_get_ascent(SystemFont *font)
{
   return (int)roundf(call(font->font, "ascender", CGFloat, ()));
}


int system_font_get_descent(SystemFont *font)
{
   return -(int)roundf(call(font->font, "descender", CGFloat, ()));
}


int system_font_get_height(SystemFont *font)
{
   return system_font_get_ascent(font) + system_font_get_descent(font);
}


int system_font_get_string_advance(SystemFont *font, plat_char *text)
{
   CGSize size;
   id dict, str;

   retain(font->font);
   dict = call((id)class("NSDictionary"), "dictionaryWithObjectsAndKeys:", id, (id, id, id),
      font->font, NSFontAttributeName,
      NULL
   );

   str = string(text);
   size = call(str, "sizeWithAttributes:", CGSize, (id), dict);
   release(str);

   return (int)roundf(size.width);
}


float system_font_get_string_position(SystemFont *font, plat_char *text, int x)
{
   char *s;
   int width;
   int min, max, middle, w, w1, w2, pos;
   
   if (x < 0) return 0.0f;
   width = system_font_get_string_advance(font, text);
   if (x >= width) return (float)strlen(text);

   s = malloc(strlen(text)+1);

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
   NSPoint point;
   CGColorSpaceRef space;
   CGContextRef ctx;
   id nsctx_cls, nsctx, str, color_obj, dict;
   int ca, cr, cg, cb;

   space = CGColorSpaceCreateDeviceRGB();
   ctx = CGBitmapContextCreate(pixels, width, height, 8, stride*4, space, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);

   nsctx_cls = (id)class("NSGraphicsContext");
   if (call(nsctx_cls, "respondsToSelector:", BOOL, (SEL), sel("graphicsContextWithCGContext:flipped:"))) {
      nsctx = call(nsctx_cls, "graphicsContextWithCGContext:flipped:", id, (CGContextRef, BOOL), ctx, 0);
   }
   else {
      nsctx = call(nsctx_cls, "graphicsContextWithGraphicsPort:flipped:", id, (void *, BOOL), ctx, 0);
   }
   call(nsctx_cls, "saveGraphicsState", void, ());
   call(nsctx_cls, "setCurrentContext:", void, (id), nsctx);

   ca = (color >> 24) & 0xFF;
   cr = (color >> 16) & 0xFF;
   cg = (color >>  8) & 0xFF;
   cb = (color >>  0) & 0xFF;
   
   if (ca != 0) {
      cr = cr * 255 / ca;
      cg = cg * 255 / ca;
      cb = cb * 255 / ca;
      if (cr > 255) cr = 255;
      if (cg > 255) cg = 255;
      if (cb > 255) cb = 255;
   }

   color_obj = call((id)class("NSColor"), "colorWithCalibratedRed:green:blue:alpha:", id, (CGFloat, CGFloat, CGFloat, CGFloat),
      cr / 255.0, cg / 255.0, cb / 255.0, ca / 255.0
   );

   retain(font->font);
   dict = call((id)class("NSDictionary"), "dictionaryWithObjectsAndKeys:", id, (id, id, id, id, id),
      font->font, NSFontAttributeName,
      color_obj, NSForegroundColorAttributeName,
      NULL
   );

   str = string(text);
   point.x = x;
   point.y = height - y + call(font->font, "descender", CGFloat, ());
   call(str, "drawAtPoint:withAttributes:", void, (NSPoint, id), point, dict);
   release(str);

   call(nsctx_cls, "restoreGraphicsState", void, ());

   CGContextRelease(ctx);
   CGColorSpaceRelease(space);
}


NotifyIcon *notify_icon_create(Heap *heap, Value *images, int num_images, char **error_msg)
{
   NotifyIcon *icon;
   ImageData *idat;
   NSSize size;
   uint32_t *pixels;
   int i, j, width, height, stride;
   int icon_width=0, icon_height=0;
   id rep;
   id statusbar, button;

   for (i=0; i<num_images; i++) {
      if (!fiximage_get_data(heap, images[i], &width, &height, NULL, NULL, NULL, NULL)) {
         return NULL;
      }
      if (height == 21 && height > icon_height) {
         icon_width = width;
         icon_height = height;
      }
      else if (height == 42 && height/2 > icon_height) {
         icon_width = width/2;
         icon_height = height/2;
      }
      else if (height > icon_height) {
         icon_width = roundf(width/(float)height*18);
         icon_height = 18;
      }
   }

   if (num_images > 0 && (icon_width == 0 || icon_height == 0)) {
      return NULL;
   }
   
   icon = calloc(1, sizeof(NotifyIcon));
   if (!icon) return NULL;

   size.width = icon_width;
   size.height = icon_height;
   if (num_images > 0) {
      icon->image = call(alloc("NSImage"), "initWithSize:", id, (NSSize), size);
      icon->space = CGColorSpaceCreateDeviceRGB();
   }
   icon->images = calloc(num_images, sizeof(ImageData));
   icon->num_images = num_images;

   for (i=0; i<num_images; i++) {
      idat = &icon->images[i];
      fiximage_get_data(heap, images[i], &width, &height, &stride, &pixels, NULL, NULL);

      idat->pixels = malloc(width*height*sizeof(uint32_t));
      for (j=0; j<height; j++) {
         memcpy(&idat->pixels[j*width], &pixels[j*stride], width*sizeof(uint32_t));
      }

      idat->provider = CGDataProviderCreateWithData(NULL, idat->pixels, width*height*4, NULL);
      idat->img = CGImageCreate(width, height, 8, 32, width*4, icon->space, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little, idat->provider, NULL, 0, kCGRenderingIntentDefault);

      rep = call(alloc("NSBitmapImageRep"), "initWithCGImage:", id, (CGImageRef), idat->img);
      call(icon->image, "addRepresentation:", void, (id), rep);
   }
   
   statusbar = call((id)class("NSStatusBar"), "systemStatusBar", id, ());
   icon->item = call(statusbar, "statusItemWithLength:", id, (CGFloat), -1);
   retain(icon->item);
   if (num_images > 0) {
      call(icon->image, "setTemplate:", void, (BOOL), 1);
      if (call(icon->item, "respondsToSelector:", BOOL, (SEL), sel("button"))) {
         button = call(icon->item, "button", id, ());
         call(button, "setImage:", void, (id), icon->image);
         call(button, "setImagePosition:", void, (NSUInteger), 2); // NSImageLeft
      }
      else {
         call(icon->item, "setImage:", void, (id), icon->image);
         call(icon->item, "setHighlightMode:", void, (BOOL), 1);
      }
   }

   return icon;
}


void notify_icon_get_sizes(int **sizes, int *cnt)
{
   *cnt = 2;
   *sizes = calloc(*cnt, sizeof(int));
   (*sizes)[0] = 18;
   (*sizes)[1] = 36;
}


void notify_icon_destroy(NotifyIcon *icon)
{
}


int notify_icon_set_menu(NotifyIcon *icon, Menu *menu)
{
   call(icon->item, "setMenu:", void, (id), menu? menu->obj : 0);

   if (icon->menu) {
      fixscript_unref(icon->menu->common.heap, icon->menu->common.instance);
   }
   icon->menu = menu;
   if (icon->menu) {
      fixscript_ref(icon->menu->common.heap, icon->menu->common.instance);
   }
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
   id app = call((id)class("NSApplication"), "sharedApplication", id, ());
   call(app, "terminate:", void, (id), NULL);
}


typedef struct {
   int type;
   Heap *heap;
   Value func;
   Value data;
} AsyncMessage;

static void call_message_handler(AsyncMessage *amsg, int ret)
{
   Value error;

   if (amsg->func.value) {
      fixscript_call(amsg->heap, amsg->func, 2, &error, amsg->data, fixscript_int(get_message_return_code(amsg->type, ret)));
      if (error.value) {
         fprintf(stderr, "error while running async message callback:\n");
         fixscript_dump_value(amsg->heap, error, 1);
      }
   }
   fixscript_unref(amsg->heap, amsg->data);

   free(amsg);
}

static void alert_completion(Block *block, int ret)
{
   call_message_handler(block->data, ret);
   release_block(block);
}

static void alert_did_end(id self, SEL selector, id alert, int ret, void *data)
{
   call_message_handler(data, ret);
   release(self);
}

static Value func_common_show_async_message(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *window = NULL;
   char *title = NULL, *msg = NULL;
   int err, type;
   id alert, delegate;
   AsyncMessage *amsg;

   if (!params[0].value) {
      *error = fixscript_create_string(heap, "must provide window to show async message", -1);
      return fixscript_int(0);
   }
   
   window = view_get_native(heap, error, params[0], TYPE_WINDOW);
   if (!window) {
      return fixscript_int(0);
   }

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

   alert = create_message_window(type, title, msg);

   amsg = calloc(1, sizeof(AsyncMessage));
   amsg->type = type;
   amsg->heap = heap;
   amsg->func = params[4];
   amsg->data = params[5];
   fixscript_ref(heap, amsg->data);

   if (call(alert, "respondsToSelector:", BOOL, (SEL), sel("beginSheetModalForWindow:completionHandler:"))) {
      call(alert, "beginSheetModalForWindow:completionHandler:", void, (id, void *),
         window->obj,
         get_block((BlockFunc)alert_completion, amsg)
      );
   }
   else {
      delegate = init(alloc("MessageSheetDelegate"));
      call(alert, "beginSheetModalForWindow:modalDelegate:didEndSelector:contextInfo:", void, (id, id, SEL, void *),
         window->obj,
         delegate,
         sel("alertDidEnd:returnCode:contextInfo:"),
         amsg
      );
   }
   release(alert);

error:
   free(title);
   free(msg);
   return fixscript_int(0);
}


static Value func_cocoa_is_present(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   return fixscript_int(1);
}


static Value func_cocoa_set_bezel_style(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;
   Rect rect;
   int style = fixscript_get_int(params[1]);
   
   view = view_get_native(heap, error, params[0], TYPE_BUTTON);
   if (!view) {
      return fixscript_int(0);
   }

   switch (style) {
      case NSBezelStyleRounded:
      case NSBezelStyleRegularSquare:
      case NSBezelStyleDisclosure:
      case NSBezelStyleShadowlessSquare:
      case NSBezelStyleCircular:
      case NSBezelStyleTexturedSquare:
      case NSBezelStyleHelpButton:
      case NSBezelStyleSmallSquare:
      case NSBezelStyleTexturedRounded:
      case NSBezelStyleRoundRect:
      case NSBezelStyleRecessed:
      case NSBezelStyleRoundedDisclosure:
         break;

      case NSBezelStyleInline:
         if (NSAppKitVersionNumber < NSAppKitVersionNumber10_7) {
            style = NSBezelStyleRecessed; // NSBezelStyleTexturedRounded
         }
         break;

      default:
         style = NSBezelStyleRegularSquare;
         break;
   }

   call(view->obj, "setBezelStyle:", void, (int), style);
   view_get_rect(view, &rect);
   view_set_rect(view, &rect);
   return fixscript_int(0);
}


static Value func_cocoa_set_control_size(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;
   Rect rect;
   int size = fixscript_get_int(params[1]);
   int font_size;
   id font, cell;
   
   view = view_get_native(heap, error, params[0], TYPE_BUTTON);
   if (!view) {
      return fixscript_int(0);
   }

   switch (size) {
      case NSControlSizeRegular: font_size = 13; break;
      case NSControlSizeSmall:   font_size = 11; break;
      case NSControlSizeMini:    font_size = 9; break;
         break;

      default:
         size = NSControlSizeRegular;
         font_size = 13;
         break;
   }

   if (call(view->obj, "respondsToSelector:", BOOL, (SEL), sel("setControlSize:"))) {
      call(view->obj, "setControlSize:", void, (int), size);
   }
   else {
      cell = call(view->obj, "cell", id, ());
      call(cell, "setControlSize:", void, (int), size);
   }

   font = call((id)class("NSFont"), "systemFontOfSize:", id, (CGFloat), font_size);
   call(view->obj, "setFont:", void, (id), font);
   view_get_rect(view, &rect);
   view_set_rect(view, &rect);
   return fixscript_int(0);
}


static Value func_cocoa_set_menu_handler(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   int idx = (intptr_t)data;

   if (menu_handlers[idx].heap) {
      fixscript_unref(heap, menu_handlers[idx].data);
   }
   menu_handlers[idx].heap = heap;
   menu_handlers[idx].func = params[0];
   menu_handlers[idx].data = params[1];
   fixscript_ref(heap, menu_handlers[idx].data);
   return fixscript_int(0);
}


static Value func_cocoa_set_menubar(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Menu *menu;
   id app;

   menu = menu_get_native(heap, error, params[0]);
   if (!menu) {
      return fixscript_int(0);
   }

   app = call((id)class("NSApplication"), "sharedApplication", id, ());

   if (!menu->has_app_menu) {
      insert_app_menu(menu->obj);
      menu->has_app_menu = 1;
   }

   call(app, "setMainMenu:", void, (id), menu->obj);
   menubar_set = 1;

   if (main_menubar) {
      fixscript_unref(main_menubar->common.heap, main_menubar->common.instance);
   }
   main_menubar = menu;
   fixscript_ref(main_menubar->common.heap, main_menubar->common.instance);
   return fixscript_int(0);
}


static Value func_cocoa_create_notify_icon_with_template(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   NotifyIcon *icon;
   id statusbar, button, name_str;
   char *name;
   int err;

   err = fixscript_get_string(heap, params[0], 0, -1, &name, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   icon = calloc(1, sizeof(NotifyIcon));
   if (!icon) {
      free(name);
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   
   statusbar = call((id)class("NSStatusBar"), "systemStatusBar", id, ());
   icon->item = call(statusbar, "statusItemWithLength:", id, (CGFloat), -1);
   retain(icon->item);

   name_str = string(name);
   icon->image = call((id)class("NSImage"), "imageNamed:", id, (id), name_str);
   release(name_str);
   call(icon->image, "setTemplate:", void, (BOOL), 1);
   free(name);

   if (call(icon->item, "respondsToSelector:", BOOL, (SEL), sel("button"))) {
      button = call(icon->item, "button", id, ());
      call(button, "setImage:", void, (id), icon->image);
      call(button, "setImagePosition:", void, (NSUInteger), 2); // NSImageLeft
   }
   else {
      call(icon->item, "setImage:", void, (id), icon->image);
      call(icon->item, "setHighlightMode:", void, (BOOL), 1);
   }

   return notify_icon_create_handle(heap, error, icon);
}


static Value func_cocoa_set_notify_icon_color(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   NotifyIcon *icon;

   icon = notify_icon_get_native(heap, error, params[0]);
   if (!icon) return fixscript_int(0);
  
   if (icon->num_images > 0) {
      call(icon->image, "setTemplate:", void, (BOOL), params[1].value == 0);
   }
   return fixscript_int(0);
}


static Value func_cocoa_set_notify_icon_text(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   NotifyIcon *icon;
   char *text;
   int err;
   float size = fixscript_get_float(params[2]);
   id str, font, dict, attr, button;

   icon = notify_icon_get_native(heap, error, params[0]);
   if (!icon) return fixscript_int(0);

   err = fixscript_get_string(heap, params[1], 0, -1, &text, NULL);
   if (err) return fixscript_error(heap, error, err);

   if (size > 0) {
      font = call((id)class("NSFont"), "menuBarFontOfSize:", id, (CGFloat), size);
      if (call(icon->item, "respondsToSelector:", BOOL, (SEL), sel("button"))) {
         button = call(icon->item, "button", id, ());
         call(button, "setFont:", void, (id), font);
         str = string(text);
         call(button, "setTitle:", void, (id), str);
         release(str);
      }
      else {
         str = string(text);
         dict = call((id)class("NSDictionary"), "dictionaryWithObjectsAndKeys:", id, (id, id, id),
            font, NSFontAttributeName,
            NULL
         );
         attr = call(alloc("NSAttributedString"), "initWithString:attributes:", id, (id, id), str, dict);
         release(str);
         call(icon->item, "setAttributedTitle:", void, (id), attr);
      }
   }
   else {
      if (call(icon->item, "respondsToSelector:", BOOL, (SEL), sel("button"))) {
         font = call((id)class("NSFont"), "menuBarFontOfSize:", id, (CGFloat), 0);
         button = call(icon->item, "button", id, ());
         call(button, "setFont:", void, (id), font);
         str = string(text);
         call(button, "setTitle:", void, (id), str);
         release(str);
      }
      else {
         str = string(text);
         call(icon->item, "setTitle:", void, (id), str);
         release(str);
      }
   }

   free(text);
   return fixscript_int(0);
}


static Value func_cocoa_set_text_color(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   NSRange range;
   id storage, background_obj, foreground_obj;
   int from, to, length;
   uint32_t background, foreground;
   int ba, br, bg, bb;
   int fa, fr, fg, fb;
   View *view;
   
   view = view_get_native(heap, error, params[0], TYPE_TEXT_AREA);
   if (!view) {
      return fixscript_int(0);
   }

   from = params[1].value;
   to = params[2].value;
   background = params[3].value;
   foreground = params[4].value;
   if (from >= to) {
      return fixscript_int(0);
   }

   storage = call(view->text_area.obj, "textStorage", id, ());
   length = call(storage, "length", NSUInteger, ());
   if (from < 0) from = 0;
   if (from >= length) {
      return fixscript_int(0);
   }
   if (to < 0) to = 0;
   if (to > length) {
      to = length;
   }
   range.location = from;
   range.length = to - from;

   ba = (background >> 24) & 0xFF;
   br = (background >> 16) & 0xFF;
   bg = (background >>  8) & 0xFF;
   bb = (background >>  0) & 0xFF;

   fa = (foreground >> 24) & 0xFF;
   fr = (foreground >> 16) & 0xFF;
   fg = (foreground >>  8) & 0xFF;
   fb = (foreground >>  0) & 0xFF;

   if (ba != 0) {
      br = br * 255 / ba;
      bg = bg * 255 / ba;
      bb = bb * 255 / ba;
      if (br > 255) br = 255;
      if (bg > 255) bg = 255;
      if (bb > 255) bb = 255;
   }

   if (fa != 0) {
      fr = fr * 255 / fa;
      fg = fg * 255 / fa;
      fb = fb * 255 / fa;
      if (fr > 255) fr = 255;
      if (fg > 255) fg = 255;
      if (fb > 255) fb = 255;
   }

   background_obj = call((id)class("NSColor"), "colorWithCalibratedRed:green:blue:alpha:", id, (CGFloat, CGFloat, CGFloat, CGFloat),
      br / 255.0, bg / 255.0, bb / 255.0, ba / 255.0
   );

   foreground_obj = call((id)class("NSColor"), "colorWithCalibratedRed:green:blue:alpha:", id, (CGFloat, CGFloat, CGFloat, CGFloat),
      fr / 255.0, fg / 255.0, fb / 255.0, fa / 255.0
   );

   call(storage, "addAttribute:value:range:", void, (id, id, NSRange), NSBackgroundColorAttributeName, background_obj, range);
   call(storage, "addAttribute:value:range:", void, (id, id, NSRange), NSForegroundColorAttributeName, foreground_obj, range);

   return fixscript_int(0);
}


static Value func_cocoa_create_search_field(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   NativeFunc func;
   void *func_data;
   Value ret;

   create_search_field = 1;
   func = fixscript_get_native_func(heap, "text_field_create#0", &func_data);
   ret = func(heap, error, 0, NULL, func_data);
   create_search_field = 0;
   return ret;
}


static Value func_cocoa_show_file_preview(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   View *view;
   char *path = NULL;
   int err;

   if (!params[1].value) {
      if (preview_panel && call(preview_panel, "isVisible", BOOL, ())) {
         call(preview_panel, "orderOut:", void, (id), NULL);
      }
      return fixscript_int(0);
   }

   view = view_get_native(heap, error, params[0], TYPE_TABLE);
   if (!view) {
      return fixscript_int(0);
   }

   err = fixscript_get_string(heap, params[1], 0, -1, &path, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   free(preview_panel_path);
   preview_panel_path = path;

   if (!preview_panel) {
      dlopen("/System/Library/Frameworks/Quartz.framework/Versions/Current/Quartz", RTLD_LAZY);

      preview_data_source = init(alloc("FixPreviewPanelDataSource"));
      preview_panel = call((id)class("QLPreviewPanel"), "sharedPreviewPanel", id, ());
   }

   ivar(preview_data_source, "table_view", void *) = view;

   if (call(preview_panel, "isVisible", BOOL, ())) {
      call(preview_panel, "reloadData", void, ());
   }
   else {
      call(preview_panel, "makeKeyAndOrderFront:", void, (id), NULL);
   }

   return fixscript_int(0);
}


static NSInteger preview_panel_number_of_items_method(id self, SEL selector, id preview_panel)
{
   return preview_panel_path? 1 : 0;
}


static id preview_panel_item_at_index_method(id self, SEL selector, id preview_panel, NSInteger index)
{
   id str, url;

   if (preview_panel_path && index == 0) {
      str = string(preview_panel_path);
      url = call((id)class("NSURL"), "fileURLWithPath:", id, (id), str);
      release(str);
      return url;
   }
   return NULL;
}


static BOOL preview_panel_handle_event_method(id self, SEL selector, id preview_panel, id event)
{
   View *view = ivar(self, "table_view", void *);
   int type, row;

   type = call(event, "type", NSUInteger, ());
   if (type == NSEventTypeKeyDown && view) {
      call(view->table.obj, "keyDown:", void, (id), event);
      row = call(view->table.obj, "selectedRow", NSInteger, ());
      call_table_action_callback(view, CALLBACK_TABLE_SPACE_KEY_ACTION, 0, row, 0, 0);
      return YES;
   }

   return NO;
}


void register_platform_gui_functions(Heap *heap)
{
   fixscript_register_native_func(heap, "common_show_async_message#6", func_common_show_async_message, NULL);

   fixscript_register_native_func(heap, "cocoa_is_present#0", func_cocoa_is_present, NULL);
   fixscript_register_native_func(heap, "cocoa_set_bezel_style#2", func_cocoa_set_bezel_style, NULL);
   fixscript_register_native_func(heap, "cocoa_set_control_size#2", func_cocoa_set_control_size, NULL);
   fixscript_register_native_func(heap, "cocoa_set_about_handler#2", func_cocoa_set_menu_handler, (void *)MH_ABOUT);
   fixscript_register_native_func(heap, "cocoa_set_preferences_handler#2", func_cocoa_set_menu_handler, (void *)MH_PREFERENCES);
   fixscript_register_native_func(heap, "cocoa_set_menubar#1", func_cocoa_set_menubar, NULL);
   fixscript_register_native_func(heap, "cocoa_create_notify_icon_with_template#1", func_cocoa_create_notify_icon_with_template, NULL);
   fixscript_register_native_func(heap, "cocoa_set_notify_icon_color#2", func_cocoa_set_notify_icon_color, NULL);
   fixscript_register_native_func(heap, "cocoa_set_notify_icon_text#3", func_cocoa_set_notify_icon_text, NULL);
   fixscript_register_native_func(heap, "cocoa_set_text_color#5", func_cocoa_set_text_color, NULL);
   fixscript_register_native_func(heap, "cocoa_create_search_field#0", func_cocoa_create_search_field, NULL);
   fixscript_register_native_func(heap, "cocoa_show_file_preview#2", func_cocoa_show_file_preview, NULL);
}


static BOOL window_should_close(id self, SEL selector, id sender)
{
   View *view = ivar(self, "window_view", void *);
   int close = 0;

   view->window.close_requested = 1;
   call_view_callback(view, CALLBACK_WINDOW_CLOSE);
   if (view->window.close_requested == 2) {
      close = 1;
   }
   view->window.close_requested = 0;
   return close;
}


static void window_will_close(id self, SEL selector, id notification)
{
   View *view = ivar(self, "window_view", void *);
   id app;
   
   call(view->obj, "setDelegate:", void, (id), NULL);
   release(self);
   view->obj = NULL;

   call_view_callback(view, CALLBACK_WINDOW_DESTROY);

   if (!main_menubar) {
      app = call((id)class("NSApplication"), "sharedApplication", id, ());

      if (view->window.menu) {
         call(app, "setMainMenu:", void, (id), default_menubar);
      }
   }
}


static void window_did_resize(id self, SEL selector, id notification)
{
   View *v, *view = ivar(self, "window_view", void *);
   Rect rect;
   id content;

   if (!view->common.heap) {
      return;
   }

   call_view_callback(view, CALLBACK_WINDOW_RESIZE);

   for (v=view->common.first_child; v; v=v->common.next) {
      view_get_rect(v, &rect);
      view_set_rect(v, &rect);
   }

   content = call(view->obj, "contentView", id, ());
   call(content, "setNeedsDisplay:", void, (BOOL), 1);
}


static void window_did_change_backing(id self, SEL selector, id notification)
{
   View *v, *view = ivar(self, "window_view", void *);
   Rect rect;
   id content;

   if (!view->common.heap) {
      return;
   }

   call_view_callback(view, CALLBACK_WINDOW_RESIZE);

   for (v=view->common.first_child; v; v=v->common.next) {
      view_get_rect(v, &rect);
      view_set_rect(v, &rect);
   }

   content = call(view->obj, "contentView", id, ());
   call(content, "setNeedsDisplay:", void, (BOOL), 1);
}


static void window_did_become_key(id self, SEL selector, id notification)
{
   View *view = ivar(self, "window_view", void *);
   id app;

   if (view->window.menu && !main_menubar) {
      app = call((id)class("NSApplication"), "sharedApplication", id, ());
      call(app, "setMainMenu:", void, (id), view->window.menu->obj);
      menubar_set = 1;
   }
}


static void button_action(id self, SEL selector)
{
   View *view = ivar(self, "button_view", void *);

   call_action_callback(view, CALLBACK_BUTTON_ACTION);
}


static BOOL canvas_wrapper_is_flipped(id self, SEL selector)
{
   return 1;
}


static BOOL canvas_is_opaque(id self, SEL selector)
{
   return 1;
}


static BOOL canvas_accepts_first_responder(id self, SEL selector)
{
   View *view = ivar(self, "canvas_view", void *);

   return view->canvas.focusable;
}


static void draw_image(int x, int y, uint32_t *pixels, int width, int height, int stride, float scale, CGFloat parent_height, CGColorSpaceRef space)
{
   NSSize size;
   NSRect rect, src;
   CGDataProviderRef provider;
   CGImageRef img;
   id image;

   provider = CGDataProviderCreateWithData(NULL, pixels, (stride*(height-1)+width)*4, NULL);
   img = CGImageCreate(width, height, 8, 32, stride*4, space, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little, provider, NULL, 0, kCGRenderingIntentDefault);
   size.width = width;
   size.height = height;
   image = call(alloc("NSImage"), "initWithCGImage:size:", id, (void *, NSSize), img, size);
   rect.origin.x = x / scale;
   rect.origin.y = y / scale;
   rect.size.width = width / scale;
   rect.size.height = height / scale;
   flip_rect(&rect, parent_height);
   src.origin.x = 0;
   src.origin.y = 0;
   src.size.width = width;
   src.size.height = height;
   call(image, "drawInRect:fromRect:operation:fraction:", void, (NSRect, NSRect, NSUInteger, CGFloat),
      rect, src, NSCompositeCopy, 1.0 
   );
   release(image);
   CGImageRelease(img);
   CGDataProviderRelease(provider);
}


static void canvas_draw_rect(id self, SEL selector, NSRect dirty_rect)
{
   View *view = ivar(self, "canvas_view", void *);
   Heap *heap = view->common.heap;
   uint32_t *pixels;
   Value image, painter, error = fixscript_int(0);
   NSRect frame, rep_rect;
   id rep, nsspace;
   CGColorSpaceRef space;
   Rect rect;
   int width, height;
   float scale;

   scale = view_get_scale(view);
   frame = call_stret(self, "frame", NSRect, ());
   flip_rect(&dirty_rect, frame.size.height);
   from_nsrect(&rect, dirty_rect, scale);

   width = rect.x2 - rect.x1;
   height = rect.y2 - rect.y1;
   if (width < 1 || height < 1) return;

   pixels = calloc(height, width*4);
   if (!pixels) return;

   image = fiximage_create_from_pixels(heap, width, height, width, pixels, free, pixels, -1);
   if (!image.value) {
      fprintf(stderr, "error while painting:\n");
      fixscript_error(heap, &error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      fixscript_dump_value(heap, error, 1);
      return;
   }

   painter = fiximage_create_painter(heap, image, -rect.x1, -rect.y1);
   if (!painter.value) {
      fprintf(stderr, "error while painting:\n");
      fixscript_error(heap, &error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      fixscript_dump_value(heap, error, 1);
      return;
   }

   call_view_callback_with_value(view, CALLBACK_CANVAS_PAINT, painter);

   rep_rect.origin.x = 0;
   rep_rect.origin.y = 0;
   rep_rect.size.width = 1;
   rep_rect.size.height = 1;
   rep = call(self, "bitmapImageRepForCachingDisplayInRect:", id, (NSRect), rep_rect);
   nsspace = call(rep, "colorSpace", id, ());
   space = call(nsspace, "CGColorSpace", CGColorSpaceRef, ());
   draw_image(rect.x1, rect.y1, pixels, width, height, width, scale, frame.size.height, space);
}


static void canvas_handle_mouse_event(id self, SEL selector, id event)
{
   View *view = ivar(self, "canvas_view", void *);
   NSPoint point;
   NSRect rect;
   int x, y, width, height, type, mod, click_count;
   float scale;
   int mod_flags;
   float wheel_x, wheel_y;
   int scroll_x, scroll_y;

   type = call(event, "type", NSUInteger, ());

   #ifdef __LP64__
      point = call(event, "locationInWindow", NSPoint, ());
      point = call(self, "convertPoint:fromView:", NSPoint, (NSPoint, id), point, NULL);
   #else
      point = call_stret(event, "locationInWindow", NSPoint, ());
      point = call_stret(self, "convertPoint:fromView:", NSPoint, (NSPoint, id), point, NULL);
   #endif

   scale = view_get_scale(view);
   rect = call_stret(view->canvas.obj, "frame", NSRect, ());
   point.y = rect.size.height - point.y;
   x = roundf(point.x * scale);
   y = roundf(point.y * scale);
   width = roundf(rect.size.width * scale);
   height = roundf(rect.size.height * scale);
   switch (type) {
      case NSEventTypeLeftMouseDragged:
      case NSEventTypeRightMouseDragged:
      case NSEventTypeOtherMouseDragged:
         break;

      default:
         if (x < 0) x = 0;
         if (y < 0) y = 0;
         if (x > width-1) x = width-1;
         if (y > height-1) y = height-1;
   }

   mod = 0;

   mod_flags = call((id)class("NSEvent"), "modifierFlags", NSUInteger, ());
   if (mod_flags & NSEventModifierFlagControl) mod |= SCRIPT_MOD_CTRL;
   if (mod_flags & NSEventModifierFlagShift) mod |= SCRIPT_MOD_SHIFT;
   if (mod_flags & NSEventModifierFlagOption) mod |= SCRIPT_MOD_ALT;
   if (mod_flags & NSEventModifierFlagCommand) mod |= SCRIPT_MOD_CMD;

   mod_flags = call((id)class("NSEvent"), "pressedMouseButtons", NSUInteger, ());
   if (mod_flags & (1 << 0)) mod |= SCRIPT_MOD_LBUTTON;
   if (mod_flags & (1 << 1)) mod |= SCRIPT_MOD_RBUTTON;
   if (mod_flags & (1 << 2)) mod |= SCRIPT_MOD_MBUTTON;

   switch (type) {
      case NSEventTypeLeftMouseDown:
         click_count = call(event, "clickCount", NSInteger, ());
         if (call_mouse_event_callback(view, EVENT_MOUSE_DOWN, x, y, MOUSE_BUTTON_LEFT, mod, click_count, 0)) return;
         call(cursors[view->canvas.cursor], "set", void, ());
         break;

      case NSEventTypeRightMouseDown:
         click_count = call(event, "clickCount", NSInteger, ());
         if (call_mouse_event_callback(view, EVENT_MOUSE_DOWN, x, y, MOUSE_BUTTON_RIGHT, mod, click_count, 0)) return;
         call(cursors[view->canvas.cursor], "set", void, ());
         break;

      case NSEventTypeOtherMouseDown:
         if (call(event, "buttonNumber", NSInteger, ()) == 2) {
            click_count = call(event, "clickCount", NSInteger, ());
            if (call_mouse_event_callback(view, EVENT_MOUSE_DOWN, x, y, MOUSE_BUTTON_MIDDLE, mod, click_count, 0)) return;
         }
         call(cursors[view->canvas.cursor], "set", void, ());
         break;

      case NSEventTypeLeftMouseUp:
         if (call_mouse_event_callback(view, EVENT_MOUSE_UP, x, y, MOUSE_BUTTON_LEFT, mod, 0, 0)) return;
         if (view->canvas.send_leave) {
            call_mouse_event_callback(view, EVENT_MOUSE_LEAVE, 0, 0, 0, 0, 0, 0);
            view->canvas.send_leave = 0;
         }
         break;

      case NSEventTypeRightMouseUp:
         if (call_mouse_event_callback(view, EVENT_MOUSE_UP, x, y, MOUSE_BUTTON_RIGHT, mod, 0, 0)) return;
         if (view->canvas.send_leave) {
            call_mouse_event_callback(view, EVENT_MOUSE_LEAVE, 0, 0, 0, 0, 0, 0);
            view->canvas.send_leave = 0;
         }
         break;

      case NSEventTypeOtherMouseUp:
         if (call(event, "buttonNumber", NSInteger, ()) == 2) {
            if (call_mouse_event_callback(view, EVENT_MOUSE_UP, x, y, MOUSE_BUTTON_MIDDLE, mod, 0, 0)) return;
            if (view->canvas.send_leave) {
               call_mouse_event_callback(view, EVENT_MOUSE_LEAVE, 0, 0, 0, 0, 0, 0);
               view->canvas.send_leave = 0;
            }
         }
         break;

      case NSEventTypeLeftMouseDragged:
      case NSEventTypeRightMouseDragged:
         if (call_mouse_event_callback(view, EVENT_MOUSE_DRAG, x, y, -1, mod, 0, 0)) return;
         break;

      case NSEventTypeOtherMouseDragged:
         if (call(event, "buttonNumber", NSInteger, ()) == 2) {
            if (call_mouse_event_callback(view, EVENT_MOUSE_DRAG, x, y, -1, mod, 0, 0)) return;
         }
         break;

      case NSEventTypeMouseEntered:
         if ((mod & SCRIPT_MOD_MOUSE_BUTTONS) == 0) {
            call_mouse_event_callback(view, EVENT_MOUSE_ENTER, x, y, -1, mod, 0, 0);
         }
         view->canvas.send_leave = 0;
         break;

      case NSEventTypeMouseExited:
         if ((mod & SCRIPT_MOD_MOUSE_BUTTONS) == 0) {
            call_mouse_event_callback(view, EVENT_MOUSE_LEAVE, 0, 0, 0, 0, 0, 0);
            view->canvas.send_leave = 0;
         }
         else {
            view->canvas.send_leave = 1;
         }
         break;

      case NSEventTypeMouseMoved:
         if (call_mouse_event_callback(view, EVENT_MOUSE_MOVE, x, y, -1, mod, 0, 0)) return;
         call(cursors[view->canvas.cursor], "set", void, ());
         break;

      case NSEventTypeScrollWheel:
         wheel_x = -call(event, "deltaX", CGFloat, ());
         wheel_y = -call(event, "deltaY", CGFloat, ());
         scroll_x = 0;
         scroll_y = 0;
         if (NSAppKitVersionNumber >= NSAppKitVersionNumber10_7) {
            if (call(event, "hasPreciseScrollingDeltas", BOOL, ())) {
               scroll_x = roundf(-call(event, "scrollingDeltaX", CGFloat, ()) * scale);
               scroll_y = roundf(-call(event, "scrollingDeltaY", CGFloat, ()) * scale);
            }
         }
         if (wheel_x == -0.0f) wheel_x = 0.0f;
         if (wheel_y == -0.0f) wheel_y = 0.0f;
         if (call_mouse_wheel_callback(view, x, y, mod, wheel_x, wheel_y, scroll_x, scroll_y)) return;
         break;
   }

   call_super(self, class("NSView"), selector, void, (id), event);
}


static void canvas_handle_key_event(id self, SEL selector, id event)
{
   View *view = ivar(self, "canvas_view", void *);
   int type, key=0, code, mod=0, mod_flags;
   id arr;

   type = call(event, "type", NSUInteger, ());
   code = call(event, "keyCode", unsigned short, ());

   switch (code) {
      case 53:  key = KEY_ESCAPE; break;
      case 122: key = KEY_F1; break;
      case 120: key = KEY_F2; break;
      case 99:  key = KEY_F3; break;
      case 118: key = KEY_F4; break;
      case 96:  key = KEY_F5; break;
      case 97:  key = KEY_F6; break;
      case 98:  key = KEY_F7; break;
      case 100: key = KEY_F8; break;
      case 101: key = KEY_F9; break;
      case 109: key = KEY_F10; break;
      case 103: key = KEY_F11; break;
      case 111: key = KEY_F12; break;
      //case :  key = KEY_PRINT_SCREEN; break;
      //case :  key = KEY_SCROLL_LOCK; break;
      //case :  key = KEY_PAUSE; break;
      case 50:  key = KEY_GRAVE; break;
      case 18:  key = KEY_NUM1; break;
      case 19:  key = KEY_NUM2; break;
      case 20:  key = KEY_NUM3; break;
      case 21:  key = KEY_NUM4; break;
      case 23:  key = KEY_NUM5; break;
      case 22:  key = KEY_NUM6; break;
      case 26:  key = KEY_NUM7; break;
      case 28:  key = KEY_NUM8; break;
      case 25:  key = KEY_NUM9; break;
      case 29:  key = KEY_NUM0; break;
      case 27:  key = KEY_MINUS; break;
      case 24:  key = KEY_EQUAL; break;
      case 51:  key = KEY_BACKSPACE; break;
      case 48:  key = KEY_TAB; break;
      case 12:  key = KEY_Q; break;
      case 13:  key = KEY_W; break;
      case 14:  key = KEY_E; break;
      case 15:  key = KEY_R; break;
      case 17:  key = KEY_T; break;
      case 16:  key = KEY_Y; break;
      case 32:  key = KEY_U; break;
      case 34:  key = KEY_I; break;
      case 31:  key = KEY_O; break;
      case 35:  key = KEY_P; break;
      case 33:  key = KEY_LBRACKET; break;
      case 30:  key = KEY_RBRACKET; break;
      case 42:  key = KEY_BACKSLASH; break;
      //case :  key = KEY_CAPS_LOCK; break;
      case 0:   key = KEY_A; break;
      case 1:   key = KEY_S; break;
      case 2:   key = KEY_D; break;
      case 3:   key = KEY_F; break;
      case 5:   key = KEY_G; break;
      case 4:   key = KEY_H; break;
      case 38:  key = KEY_J; break;
      case 40:  key = KEY_K; break;
      case 37:  key = KEY_L; break;
      case 41:  key = KEY_SEMICOLON; break;
      case 39:  key = KEY_APOSTROPHE; break;
      case 36:  key = KEY_ENTER; break;
      //case :  key = KEY_LSHIFT; break;
      case 6:   key = KEY_Z; break;
      case 7:   key = KEY_X; break;
      case 8:   key = KEY_C; break;
      case 9:   key = KEY_V; break;
      case 11:  key = KEY_B; break;
      case 45:  key = KEY_N; break;
      case 46:  key = KEY_M; break;
      case 43:  key = KEY_COMMA; break;
      case 47:  key = KEY_PERIOD; break;
      case 44:  key = KEY_SLASH; break;
      //case :  key = KEY_RSHIFT; break;
      //case :  key = KEY_LCONTROL; break;
      //case :  key = KEY_LMETA; break;
      //case :  key = KEY_LALT; break;
      case 49:  key = KEY_SPACE; break;
      //case :  key = KEY_RALT; break;
      //case :  key = KEY_RMETA; break;
      //case :  key = KEY_RMENU; break;
      //case :  key = KEY_RCONTROL; break;
      //case :  key = KEY_INSERT; break;
      case 117: key = KEY_DELETE; break;
      case 115: key = KEY_HOME; break;
      case 119: key = KEY_END; break;
      case 116: key = KEY_PAGE_UP; break;
      case 121: key = KEY_PAGE_DOWN; break;
      case 123: key = KEY_LEFT; break;
      case 126: key = KEY_UP; break;
      case 124: key = KEY_RIGHT; break;
      case 125: key = KEY_DOWN; break;
      //case :  key = KEY_NUM_LOCK; break;
      case 75:  key = KEY_NUMPAD_SLASH; break;
      case 67:  key = KEY_NUMPAD_STAR; break;
      case 78:  key = KEY_NUMPAD_MINUS; break;
      case 69:  key = KEY_NUMPAD_PLUS; break;
      case 76:  key = KEY_NUMPAD_ENTER; break;
      case 65:  key = KEY_NUMPAD_DOT; break;
      case 82:  key = KEY_NUMPAD0; break;
      case 83:  key = KEY_NUMPAD1; break;
      case 84:  key = KEY_NUMPAD2; break;
      case 85:  key = KEY_NUMPAD3; break;
      case 86:  key = KEY_NUMPAD4; break;
      case 87:  key = KEY_NUMPAD5; break;
      case 88:  key = KEY_NUMPAD6; break;
      case 89:  key = KEY_NUMPAD7; break;
      case 91:  key = KEY_NUMPAD8; break;
      case 92:  key = KEY_NUMPAD9; break;
   }

   mod_flags = call((id)class("NSEvent"), "modifierFlags", NSUInteger, ());
   if (mod_flags & NSEventModifierFlagControl) mod |= SCRIPT_MOD_CTRL;
   if (mod_flags & NSEventModifierFlagShift) mod |= SCRIPT_MOD_SHIFT;
   if (mod_flags & NSEventModifierFlagOption) mod |= SCRIPT_MOD_ALT;
   if (mod_flags & NSEventModifierFlagCommand) mod |= SCRIPT_MOD_CMD;

   if (call_key_event_callback(view, type == NSEventTypeKeyDown? EVENT_KEY_DOWN : EVENT_KEY_UP, key, mod)) return;

   if (type == NSEventTypeKeyDown) {
      arr = call((id)class("NSArray"), "arrayWithObject:", id, (id), event);
      call(self, "interpretKeyEvents:", void, (id), arr);
   }

   call_super(self, class("NSView"), selector, void, (id), event);
}


static void canvas_insert_text(id self, SEL selector, id str)
{
   View *view = ivar(self, "canvas_view", void *);
   const char *chars;
   int mod=0, mod_flags;

   mod_flags = call((id)class("NSEvent"), "modifierFlags", NSUInteger, ());
   if (mod_flags & NSEventModifierFlagControl) mod |= SCRIPT_MOD_CTRL;
   if (mod_flags & NSEventModifierFlagShift) mod |= SCRIPT_MOD_SHIFT;
   if (mod_flags & NSEventModifierFlagOption) mod |= SCRIPT_MOD_ALT;
   if (mod_flags & NSEventModifierFlagCommand) mod |= SCRIPT_MOD_CMD;

   chars = call(str, "UTF8String", const char *, ());
   if (call_key_typed_event_callback(view, chars, mod)) return;
}


static void app_did_finish_launching(id self, SEL selector, id notification)
{
   id app;

   app = call((id)class("NSApplication"), "sharedApplication", id, ());

   call(app, "activateIgnoringOtherApps:", void, (BOOL), 1);
   
   if (!app_main(main_argc, main_argv)) {
      call(app, "terminate:", void, (id), NULL);
   }

   create_default_menubar();
}


static void app_did_become_active(id self, SEL selector, id notification)
{
}


static void app_show_about_dialog(id self, SEL selector, id sender)
{
   Heap *heap;
   Value error;
   id app;

   if (menu_handlers[MH_ABOUT].func.value) {
      heap = menu_handlers[MH_ABOUT].heap;
      fixscript_call(heap, menu_handlers[MH_ABOUT].func, 1, &error, menu_handlers[MH_ABOUT].data);
      if (error.value) {
         fprintf(stderr, "error while running about callback:\n");
         fixscript_dump_value(heap, error, 1);
      }
      return;
   }

   app = call((id)class("NSApplication"), "sharedApplication", id, ());
   call(app, "orderFrontStandardAboutPanel:", void, (id), sender);
}


static void app_show_preferences_dialog(id self, SEL selector, id sender)
{
   Heap *heap;
   Value error;

   if (menu_handlers[MH_PREFERENCES].func.value) {
      heap = menu_handlers[MH_PREFERENCES].heap;
      fixscript_call(heap, menu_handlers[MH_PREFERENCES].func, 1, &error, menu_handlers[MH_PREFERENCES].data);
      if (error.value) {
         fprintf(stderr, "error while running preferences callback:\n");
         fixscript_dump_value(heap, error, 1);
      }
   }
}


static void menu_click_action(id self, SEL selector, id sender)
{
   Menu *menu = ivar(self, "menu", void *);
   int pos = ivar(self, "pos", int);

   call_menu_callback(menu, pos);
}


static char *get_info_key(id bundle, const char *name)
{
   id str, value;
   char *s = NULL;

   str = string(name);
   value = call(bundle, "objectForInfoDictionaryKey:", id, (id), str);
   release(str);

   if (value) {
      s = strdup(call(value, "UTF8String", const char *, ()));
   }
   return s;
}


char *fixgui__get_cocoa_exec_path()
{
   return exec_path;
}


int main(int argc, char **argv)
{
   Class cls;
   NSPoint point;
   NSSize size;
   id pool, bundle, str, cursor_cls, img, empty_cursor, app, delegate;

   fixgui__tls_init();
   main_argc = argc;
   main_argv = argv;

   pool = init(alloc("NSAutoreleasePool"));

   bundle = call((id)class("NSBundle"), "mainBundle", id, ());

   str = call(bundle, "resourcePath", id, ());
   chdir(call(str, "UTF8String", const char *, ()));
   release(str);

   str = call(bundle, "executablePath", id, ());
   exec_path = strdup(call(str, "UTF8String", const char *, ()));
   release(str);

   app_name = get_info_key(bundle, "CFBundleDisplayName");
   if (!app_name) {
      app_name = get_info_key(bundle, "CFBundleName");
   }

   app = call((id)class("NSApplication"), "sharedApplication", id, ());

   size.width = 16;
   size.height = 16;
   img = call(alloc("NSImage"), "initWithSize:", id, (NSSize), size);
   point.x = 0;
   point.y = 0;
   empty_cursor = call(alloc("NSCursor"), "initWithImage:hotSpot:", id, (id, NSPoint), img, point);
   release(img);
   
   cursor_cls = (id)class("NSCursor");
   cursors[CURSOR_DEFAULT] = call(cursor_cls, "arrowCursor", id, ());
   cursors[CURSOR_ARROW] = call(cursor_cls, "arrowCursor", id, ());
   cursors[CURSOR_EMPTY] = empty_cursor;
   cursors[CURSOR_TEXT] = call(cursor_cls, "IBeamCursor", id, ());
   cursors[CURSOR_CROSS] = call(cursor_cls, "crosshairCursor", id, ());
   cursors[CURSOR_HAND] = call(cursor_cls, "pointingHandCursor", id, ());
   cursors[CURSOR_MOVE] = call(cursor_cls, "openHandCursor", id, ());
   cursors[CURSOR_RESIZE_N] = call(cursor_cls, "resizeUpDownCursor", id, ());
   cursors[CURSOR_RESIZE_NE] = call(cursor_cls, "openHandCursor", id, ());
   cursors[CURSOR_RESIZE_E] = call(cursor_cls, "resizeLeftRightCursor", id, ());
   cursors[CURSOR_RESIZE_SE] = call(cursor_cls, "openHandCursor", id, ());
   cursors[CURSOR_RESIZE_S] = call(cursor_cls, "resizeUpDownCursor", id, ());
   cursors[CURSOR_RESIZE_SW] = call(cursor_cls, "openHandCursor", id, ());
   cursors[CURSOR_RESIZE_W] = call(cursor_cls, "resizeLeftRightCursor", id, ());
   cursors[CURSOR_RESIZE_NW] = call(cursor_cls, "openHandCursor", id, ());
   cursors[CURSOR_WAIT] = call(cursor_cls, "arrowCursor", id, ());

   cls = objc_allocateClassPair((Class)objc_getClass("NSObject"), "AppDelegate", 0);
   class_addMethod(cls, sel("applicationDidFinishLaunching:"), (IMP)app_did_finish_launching, "v@:@");
   class_addMethod(cls, sel("applicationDidBecomeActive:"), (IMP)app_did_become_active, "v@:@");
   class_addMethod(cls, sel("showAboutDialog:"), (IMP)app_show_about_dialog, "v@:@");
   class_addMethod(cls, sel("showPreferencesDialog:"), (IMP)app_show_preferences_dialog, "v@:@");
   objc_registerClassPair(cls);

   cls = objc_allocateClassPair((Class)objc_getClass("NSObject"), "WindowDelegate", 0);
   class_addMethod(cls, sel("windowShouldClose:"), (IMP)window_should_close, "c@:@");
   class_addMethod(cls, sel("windowWillClose:"), (IMP)window_will_close, "v@:@");
   class_addMethod(cls, sel("windowDidResize:"), (IMP)window_did_resize, "v@:@");
   class_addMethod(cls, sel("windowDidChangeBackingProperties:"), (IMP)window_did_change_backing, "v@:@");
   class_addMethod(cls, sel("windowDidBecomeKey:"), (IMP)window_did_become_key, "v@:@");
   class_addIvar(cls, "window_view", sizeof(View *), sizeof(View *), "^v");
   objc_registerClassPair(cls);

   cls = objc_allocateClassPair((Class)objc_getClass("NSObject"), "MessageSheetDelegate", 0);
   class_addMethod(cls, sel("alertDidEnd:returnCode:contextInfo:"), (IMP)alert_did_end, "v@:@i^v");
   objc_registerClassPair(cls);

   cls = objc_allocateClassPair((Class)objc_getClass("NSButton"), "FixButton", 0);
   class_addMethod(cls, sel("buttonAction"), (IMP)button_action, "v@:");
   class_addIvar(cls, "button_view", sizeof(View *), sizeof(View *), "^v");
   objc_registerClassPair(cls);

   cls = objc_allocateClassPair((Class)objc_getClass("NSView"), "FixCanvasWrapper", 0);
   class_addMethod(cls, sel("isFlipped"), (IMP)canvas_wrapper_is_flipped, "c@:");
   objc_registerClassPair(cls);

   cls = objc_allocateClassPair((Class)objc_getClass("NSView"), "FixCanvas", 0);
   class_addMethod(cls, sel("isOpaque"), (IMP)canvas_is_opaque, "c@:");
   class_addMethod(cls, sel("acceptsFirstResponder"), (IMP)canvas_accepts_first_responder, "c@:");
   if (sizeof(CGFloat) == sizeof(float)) {
      class_addMethod(cls, sel("drawRect:"), (IMP)canvas_draw_rect, "v@:{NSRect=ffff}");
   }
   else {
      class_addMethod(cls, sel("drawRect:"), (IMP)canvas_draw_rect, "v@:{NSRect=dddd}");
   }
   class_addMethod(cls, sel("mouseDown:"), (IMP)canvas_handle_mouse_event, "v@:@");
   class_addMethod(cls, sel("mouseDragged:"), (IMP)canvas_handle_mouse_event, "v@:@");
   class_addMethod(cls, sel("mouseUp:"), (IMP)canvas_handle_mouse_event, "v@:@");
   class_addMethod(cls, sel("rightMouseDown:"), (IMP)canvas_handle_mouse_event, "v@:@");
   class_addMethod(cls, sel("rightMouseDragged:"), (IMP)canvas_handle_mouse_event, "v@:@");
   class_addMethod(cls, sel("rightMouseUp:"), (IMP)canvas_handle_mouse_event, "v@:@");
   class_addMethod(cls, sel("otherMouseDown:"), (IMP)canvas_handle_mouse_event, "v@:@");
   class_addMethod(cls, sel("otherMouseDragged:"), (IMP)canvas_handle_mouse_event, "v@:@");
   class_addMethod(cls, sel("otherMouseUp:"), (IMP)canvas_handle_mouse_event, "v@:@");
   class_addMethod(cls, sel("mouseMoved:"), (IMP)canvas_handle_mouse_event, "v@:@");
   class_addMethod(cls, sel("mouseEntered:"), (IMP)canvas_handle_mouse_event, "v@:@");
   class_addMethod(cls, sel("mouseExited:"), (IMP)canvas_handle_mouse_event, "v@:@");
   class_addMethod(cls, sel("scrollWheel:"), (IMP)canvas_handle_mouse_event, "v@:@");
   class_addMethod(cls, sel("keyDown:"), (IMP)canvas_handle_key_event, "v@:@");
   class_addMethod(cls, sel("keyUp:"), (IMP)canvas_handle_key_event, "v@:@");
   class_addMethod(cls, sel("insertText:"), (IMP)canvas_insert_text, "v@:@");
   class_addIvar(cls, "canvas_view", sizeof(View *), sizeof(View *), "^v");
   objc_registerClassPair(cls);

   cls = objc_allocateClassPair((Class)objc_getClass("NSObject"), "FixMenuItemData", 0);
   class_addMethod(cls, sel("clickAction:"), (IMP)menu_click_action, "v@:@");
   class_addIvar(cls, "menu", sizeof(Menu *), sizeof(Menu *), "^v");
   class_addIvar(cls, "pos", sizeof(int), sizeof(int), "i");
   objc_registerClassPair(cls);

   cls = objc_allocateClassPair((Class)objc_getClass("NSObject"), "FixTimerData", 0);
   class_addMethod(cls, sel("timerFireMethod:"), (IMP)timer_fire_method, "v@:@");
   class_addIvar(cls, "timer", sizeof(Timer *), sizeof(Timer *), "^v");
   objc_registerClassPair(cls);

   cls = objc_allocateClassPair((Class)objc_getClass("NSTableView"), "FixTableView", 0);
   class_addMethod(cls, sel("rightMouseDown:"), (IMP)table_handle_right_mouse_event, "v@:@");
   class_addMethod(cls, sel("keyDown:"), (IMP)table_handle_key_event, "v@:@");
   class_addMethod(cls, sel("acceptsPreviewPanelControl:"), (IMP)table_accepts_preview_panel_method, "c@:@");
   class_addMethod(cls, sel("beginPreviewPanelControl:"), (IMP)table_begin_preview_panel_method, "v@:@");
   class_addMethod(cls, sel("endPreviewPanelControl:"), (IMP)table_end_preview_panel_method, "v@:@");
   class_addIvar(cls, "table_view", sizeof(View *), sizeof(View *), "^v");
   objc_registerClassPair(cls);

   cls = objc_allocateClassPair((Class)objc_getClass("NSTableHeaderView"), "FixTableHeaderView", 0);
   class_addMethod(cls, sel("mouseDown:"), (IMP)table_handle_header_mouse_event, "v@:@");
   //class_addMethod(cls, sel("mouseDragged:"), (IMP)table_handle_header_mouse_event, "v@:@");
   //class_addMethod(cls, sel("mouseUp:"), (IMP)table_handle_header_mouse_event, "v@:@");
   class_addIvar(cls, "table_view", sizeof(View *), sizeof(View *), "^v");
   objc_registerClassPair(cls);

   cls = objc_allocateClassPair((Class)objc_getClass("NSObject"), "FixTableData", 0);
   class_addMethod(cls, sel("numberOfRowsInTableView:"), (IMP)table_number_of_rows_method, "l@:@");
   class_addMethod(cls, sel("tableView:objectValueForTableColumn:row:"), (IMP)table_object_value_method, "@@:@@l");
   class_addMethod(cls, sel("clickAction:"), (IMP)table_click_action, "v@:@");
   class_addIvar(cls, "table_view", sizeof(View *), sizeof(View *), "^v");
   objc_registerClassPair(cls);

   cls = objc_allocateClassPair((Class)objc_getClass("NSObject"), "FixPreviewPanelDataSource", 0);
   class_addMethod(cls, sel("numberOfPreviewItemsInPreviewPanel:"), (IMP)preview_panel_number_of_items_method, "l@:@");
   class_addMethod(cls, sel("previewPanel:previewItemAtIndex:"), (IMP)preview_panel_item_at_index_method, "@@:@l");
   class_addMethod(cls, sel("previewPanel:handleEvent:"), (IMP)preview_panel_handle_event_method, "c@:@@");
   class_addIvar(cls, "table_view", sizeof(View *), sizeof(View *), "^v");
   objc_registerClassPair(cls);

   delegate = init(alloc("AppDelegate"));
   call(app, "setDelegate:", void, (id), delegate);

   call(app, "run", void, ());
   return 0;
}
