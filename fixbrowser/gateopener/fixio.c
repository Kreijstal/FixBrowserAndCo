/*
 * FixScript IO v0.8 (modified) - https://www.fixscript.org/
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

// ZLIB code available at http://public-domain.advel.cz/ under CC0 license

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
   #error "FixIO supports little endian CPUs only"
#endif

#if defined(FIXBUILD_BINCOMPAT) && defined(__linux__)
#define fcntl fcntl__64
#endif

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <wchar.h>
#ifdef _WIN32
#define UNICODE
#define _UNICODE
#include <winsock2.h>
#include <mswsock.h>
#include <windows.h>
#else
#ifndef __wasm__
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <sys/file.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>
#endif
#include <errno.h>
#ifndef __wasm__
#include <dirent.h>
#endif
#include <pthread.h>
#ifndef __wasm__
#include <signal.h>
#ifdef __linux__
#include <sys/epoll.h>
#else
#include <sys/poll.h>
#endif
#endif
#include <time.h>
#ifndef __wasm__
#include <sys/time.h>
#include <sys/wait.h>
#endif
#include <locale.h>
#ifndef __wasm__
#include <sys/ioctl.h>
#include <termios.h>
#endif
#endif
#ifdef __APPLE__
#include <mach/mach_time.h>
#include <xlocale.h>
#endif
#if defined(__APPLE__) || defined(__HAIKU__)
#include <sched.h>
#endif
#ifdef __wasm__
#include <wasm-support.h>
#endif
#ifdef FIXIO_SQLITE
#include "sqlite3.h"
#endif
#include "fixio.h"

#if defined(FIXBUILD_BINCOMPAT) && defined(__linux__)
   #undef fcntl
   extern int fcntl(int fd, int cmd, ...);

   #if defined(__i386__)
      asm(".symver fcntl,fcntl@GLIBC_2.0");
   #elif defined(__x86_64__)
      asm(".symver memcpy,memcpy@GLIBC_2.2.5");
   #elif defined(__arm__)
   #endif
#endif

enum {
   ASYNC_READ  = 1 << 0,
   ASYNC_WRITE = 1 << 1
};

#if defined(__linux__)
#define USE_EPOLL
#elif defined(__wasm__)
#elif !defined(_WIN32)
#define USE_POLL
#endif

#if defined(USE_EPOLL)

typedef struct {
   int epoll_fd;
   int pipe_read_fd;
   int pipe_write_fd;
   struct epoll_event events[32];
   int cur, cnt;
} Poll;

static Poll *poll_create()
{
   Poll *poll;
   struct epoll_event event;
   int fds[2], flags;

   poll = calloc(1, sizeof(Poll));
   poll->epoll_fd = epoll_create(4);
   if (poll->epoll_fd == -1) {
      free(poll);
      return NULL;
   }

   if (pipe(fds) != 0) {
      close(poll->epoll_fd);
      free(poll);
      return NULL;
   }
   poll->pipe_read_fd = fds[0];
   poll->pipe_write_fd = fds[1];
   flags = fcntl(poll->pipe_read_fd, F_GETFL);
   flags |= O_NONBLOCK;
   fcntl(poll->pipe_read_fd, F_SETFL, flags);
   
   event.events = EPOLLIN;
   event.data.ptr = NULL;
   if (epoll_ctl(poll->epoll_fd, EPOLL_CTL_ADD, poll->pipe_read_fd, &event) != 0) {
      close(poll->pipe_read_fd);
      close(poll->pipe_write_fd);
      close(poll->epoll_fd);
      free(poll);
      return NULL;
   }
   return poll;
}

static void poll_destroy(Poll *poll)
{
   close(poll->pipe_read_fd);
   close(poll->pipe_write_fd);
   close(poll->epoll_fd);
   free(poll);
}

static int poll_add_socket(Poll *poll, int fd, void *data, int mode)
{
   struct epoll_event event;
   
   event.events = 0;
   if (mode & ASYNC_READ) event.events |= EPOLLIN;
   if (mode & ASYNC_WRITE) event.events |= EPOLLOUT;
   event.data.ptr = data;
   return epoll_ctl(poll->epoll_fd, EPOLL_CTL_ADD, fd, &event) == 0;
}

static int poll_remove_socket(Poll *poll, int fd)
{
   struct epoll_event event;
   
   return epoll_ctl(poll->epoll_fd, EPOLL_CTL_DEL, fd, &event) == 0;
}

static int poll_update_socket(Poll *poll, int fd, void *data, int mode)
{
   struct epoll_event event;
   
   event.events = 0;
   if (mode & ASYNC_READ) event.events |= EPOLLIN;
   if (mode & ASYNC_WRITE) event.events |= EPOLLOUT;
   event.data.ptr = data;
   return epoll_ctl(poll->epoll_fd, EPOLL_CTL_MOD, fd, &event) == 0;
}

static void poll_interrupt(Poll *poll)
{
   char c;

   while (write(poll->pipe_write_fd, &c, 1) == 0);
}

static void poll_wait(Poll *poll, int timeout)
{
   if (poll->cur < poll->cnt) return;

   poll->cur = 0;
   poll->cnt = epoll_wait(poll->epoll_fd, poll->events, sizeof(poll->events)/sizeof(struct epoll_event), timeout);
   if (poll->cnt < 0) {
      poll->cnt = 0;
   }
}

static void *poll_get_event(Poll *poll, int *flags)
{
   struct epoll_event *event;
   char c;
   
   for (;;) {
      if (poll->cur >= poll->cnt) return NULL;

      event = &poll->events[poll->cur++];
      if (!event->data.ptr && (event->events & EPOLLIN)) {
         while (read(poll->pipe_read_fd, &c, 1) == 1);
         continue;
      }

      *flags = 0;
      if (event->events & EPOLLIN) *flags |= ASYNC_READ;
      if (event->events & EPOLLOUT) *flags |= ASYNC_WRITE;
      return event->data.ptr;
   }
}

#elif defined(USE_POLL)

typedef struct {
   int epoll_fd;
   int pipe_read_fd;
   int pipe_write_fd;
   struct pollfd *fds;
   void **data;
   int cap, cnt;
   int pending;
} Poll;

static Poll *poll_create()
{
   Poll *poll;
   int fds[2], flags;

   poll = calloc(1, sizeof(Poll));
   poll->cap = 4;
   poll->fds = malloc(poll->cap * sizeof(struct pollfd));
   poll->data = malloc(poll->cap * sizeof(void *));
   if (!poll->fds || !poll->data) {
      free(poll->fds);
      free(poll->data);
      free(poll);
      return NULL;
   }

   if (pipe(fds) != 0) {
      free(poll->fds);
      free(poll->data);
      free(poll);
      return NULL;
   }
   poll->pipe_read_fd = fds[0];
   poll->pipe_write_fd = fds[1];
   flags = fcntl(poll->pipe_read_fd, F_GETFL);
   flags |= O_NONBLOCK;
   fcntl(poll->pipe_read_fd, F_SETFL, flags);

   poll->cnt = 1;
   poll->fds[0].fd = poll->pipe_read_fd;
   poll->fds[0].events = POLLIN;
   poll->data[0] = NULL;
   return poll;
}

static void poll_destroy(Poll *poll)
{
   close(poll->pipe_read_fd);
   close(poll->pipe_write_fd);
   free(poll->fds);
   free(poll->data);
   free(poll);
}

static int poll_add_socket(Poll *poll, int fd, void *data, int mode)
{
   struct pollfd *new_fds;
   void **new_data;
   int i, new_cap;

   for (i=0; i<poll->cnt; i++) {
      if (poll->fds[i].fd == fd) return 0;
   }

   if (poll->cnt == poll->cap) {
      if (poll->cap > (1<<26)) {
         return 0;
      }
      new_cap = poll->cap << 1;
      new_fds = realloc(poll->fds, new_cap * sizeof(struct pollfd));
      if (!new_fds) {
         return 0;
      }
      poll->fds = new_fds;
      new_data = realloc(poll->data, new_cap * sizeof(void *));
      if (!new_data) {
         return 0;
      }
      poll->data = new_data;
      poll->cap = new_cap;
   }

   poll->fds[poll->cnt].fd = fd;
   poll->fds[poll->cnt].events = 0;
   if (mode & ASYNC_READ) poll->fds[poll->cnt].events |= POLLIN;
   if (mode & ASYNC_WRITE) poll->fds[poll->cnt].events |= POLLOUT;
   poll->data[poll->cnt] = data;
   poll->cnt++;
   return 1;
}

static int poll_remove_socket(Poll *poll, int fd)
{
   int i;

   for (i=0; i<poll->cnt; i++) {
      if (poll->fds[i].fd == fd) {
         poll->fds[i] = poll->fds[poll->cnt-1];
         poll->data[i] = poll->data[poll->cnt-1];
         poll->cnt--;
         return 1;
      }
   }
   return 0;
}

static int poll_update_socket(Poll *poll, int fd, void *data, int mode)
{
   int i;

   for (i=0; i<poll->cnt; i++) {
      if (poll->fds[i].fd == fd) {
         poll->fds[i].events = 0;
         if (mode & ASYNC_READ) poll->fds[i].events |= POLLIN;
         if (mode & ASYNC_WRITE) poll->fds[i].events |= POLLOUT;
         poll->data[i] = data;
         return 1;
      }
   }
   return 0;
}

static void poll_interrupt(Poll *poll)
{
   char c;

   while (write(poll->pipe_write_fd, &c, 1) == 0);
}

static void poll_wait(Poll *_poll, int timeout)
{
   if (_poll->pending > 0) return;

   _poll->pending = poll(_poll->fds, _poll->cnt, timeout);
   if (_poll->pending < 0) {
      _poll->pending = 0;
   }
}

static void *poll_get_event(Poll *poll, int *flags)
{
   int i;
   char c;
   
   if (poll->pending == 0) return NULL;

   for (i=0; i<poll->cnt; i++) {
      if (!poll->data[i] && (poll->fds[i].revents & POLLIN)) {
         while (read(poll->pipe_read_fd, &c, 1) == 1);
         if (--poll->pending == 0) return NULL;
         continue;
      }

      if (poll->fds[i].revents & (POLLIN | POLLOUT)) {
         *flags = 0;
         if (poll->fds[i].revents & POLLIN) *flags |= ASYNC_READ;
         if (poll->fds[i].revents & POLLOUT) *flags |= ASYNC_WRITE;
         poll->pending--;
         return poll->data[i];
      }
   }

   return NULL;
}

#endif

#ifdef __wasm__
typedef int pid_t;
#endif

#ifdef _WIN32
#define ETIMEDOUT -1000
typedef CRITICAL_SECTION pthread_mutex_t;
typedef HANDLE pthread_cond_t;
#endif

enum {
   ZC_GZIP     = 1 << 0,
   ZC_COMPRESS = 1 << 1
};

enum {
   GZIP_FTEXT    = 1 << 0,
   GZIP_FHCRC    = 1 << 1,
   GZIP_FEXTRA   = 1 << 2,
   GZIP_FNAME    = 1 << 3,
   GZIP_FCOMMENT = 1 << 4
};

enum {
   COMP_ERROR = -1,
   COMP_DONE  = 0,
   COMP_FLUSH = 1,
   COMP_MORE  = 2
};

enum {
   TYPE_FILE      = 0,
   TYPE_DIRECTORY = 1,
   TYPE_SPECIAL   = 2,
   TYPE_SYMLINK   = 0x80
};

enum {
   SCRIPT_FILE_READ     = 0x01,
   SCRIPT_FILE_WRITE    = 0x02,
   SCRIPT_FILE_CREATE   = 0x04,
   SCRIPT_FILE_TRUNCATE = 0x08,
   SCRIPT_FILE_APPEND   = 0x10
};

enum {
   EVENT_READ  = 0x01,
   EVENT_WRITE = 0x02
};

enum {
   ASYNC_TCP_CONNECTION,
   ASYNC_TCP_SERVER
};

enum {
   REDIR_IN        = 0x01,
   REDIR_OUT       = 0x02,
   REDIR_ERR       = 0x04,
   REDIR_MERGE_ERR = 0x08
};

enum {
   PRINT_NORMAL,
   PRINT_LOG,
   PRINT_PROMPT,
   PRINT_PROGRESS
};

enum {
   EVENT_NONE,
   EVENT_CHAR_TYPED,
   EVENT_KEY_PRESSED,
   EVENT_CONSOLE_RESIZED
};

#ifdef _WIN32
#undef MOD_SHIFT
#undef MOD_ALT
#endif

enum {
   MOD_CTRL  = 0x01,
   MOD_SHIFT = 0x02,
   MOD_ALT   = 0x04
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

#define KEY_PRESSED(key, mod) ((EVENT_KEY_PRESSED << 28) | ((mod) << 24) | (key))

#ifdef FIXIO_SQLITE
enum {
   SQL_TYPE_UNKNOWN = -1,
   SQL_TYPE_INTEGER,
   SQL_TYPE_LONG,
   SQL_TYPE_FLOAT,
   SQL_TYPE_DOUBLE,
   SQL_TYPE_STRING,
   SQL_TYPE_BINARY
};
#endif

typedef struct {
   const unsigned char *src, *src_end;
   unsigned char *dest, *dest_end;
} ZCommon;

// note: the source buffer (not the current pointers) must be able to store at least 258 bytes
#define ZCOMP_NUM_BUCKETS 4096 // 4096*8*2 = 64KB
#define ZCOMP_NUM_SLOTS   8
typedef struct {
   const unsigned char *src, *src_end;
   unsigned char *dest, *dest_end;
   int src_final, src_flush;
   int flushable;

   int state;
   uint32_t bits;
   int num_bits;
   unsigned char extra[7];
   int extra_len;

   unsigned char circular[32768];
   int circular_pos, circular_written;
   unsigned short hash[ZCOMP_NUM_BUCKETS * ZCOMP_NUM_SLOTS];
} ZCompress;

// note: the source buffer (not the current pointers) must be able to store at least 570 bytes
typedef struct {
   const unsigned char *src, *src_end;
   unsigned char *dest, *dest_end;

   int state;
   uint32_t bits;
   int num_bits;
   int final_block;
   int remaining, dist;

   uint16_t lit_symbols[288], lit_counts[16];
   uint8_t dist_symbols[32], dist_counts[16];

   char circular[32768];
   int circular_pos, circular_written;
} ZUncompress;

typedef struct {
   ZCompress z;
   int state;
   unsigned char extra[10];
   int extra_len;
   uint32_t crc;
   uint32_t in_size;
} GZipCompress;

typedef struct {
   ZUncompress z;
   int state;
   int flags;
   int remaining;
   uint32_t crc;
   uint32_t out_size;
} GZipUncompress;

typedef struct {
   Heap *heap;
   void *state;
   int size;
   int read, written;
} CompressHandle;

typedef struct {
   volatile int refcnt;
   volatile int closed;
   volatile int locked;
#if defined(_WIN32)
   HANDLE handle;
#elif defined(__wasm__)
   wasm_file_t *file;
#else
   int fd;
#endif
   int mode;
} FileHandle;

typedef struct {
   volatile int refcnt;
   volatile int closed;
#if defined(_WIN32)
   SOCKET socket;
#else
   int fd;
#endif
   int in_nonblocking, want_nonblocking;
} TCPConnectionHandle;

typedef struct {
   volatile int refcnt;
   volatile int closed;
#if defined(_WIN32)
   SOCKET socket;
#else
   int fd;
#endif
} TCPServerHandle;

typedef struct AsyncThreadResult {
   int type;
   Value callback;
   Value data;
#if defined(_WIN32)
   SOCKET socket;
#else
   int fd;
#endif
   struct AsyncThreadResult *next;
} AsyncThreadResult;

typedef struct AsyncTimer {
   int immediate;
   uint32_t time;
   Value callback;
   Value data;
   struct AsyncTimer *next;
} AsyncTimer;

#ifdef _WIN32
typedef struct {
   DWORD transferred;
   ULONG_PTR key;
   void *overlapped;
} CompletedIO;
#endif

typedef struct {
   volatile int refcnt;
   pthread_mutex_t mutex;
   AsyncThreadResult *thread_results;
#if defined(_WIN32)
   HANDLE iocp;
   CompletedIO completed_ios[32];
   int num_events;
#elif defined(__wasm__)
   void *poll;
#else
   Poll *poll;
#endif
   int quit;
   Value quit_value;
   AsyncTimer *timers;

   pthread_mutex_t foreign_mutex;
   pthread_cond_t foreign_cond;
   IOEventNotifyFunc foreign_notify_func;
   void *foreign_notify_data;
   int foreign_processed;
} AsyncProcess;

typedef struct {
   Value callback;
   Value data;
   Value array;
   int off, len;
#if defined(_WIN32)
   char buf[1024];
   WSAOVERLAPPED overlapped;
#endif
} AsyncRead;

typedef struct {
   Value callback;
   Value data;
#if defined(_WIN32)
   char buf[1024];
   WSAOVERLAPPED overlapped;
#else
   int result;
#endif
} AsyncWrite;

typedef struct {
   AsyncProcess *proc;
   int type;
#if defined(_WIN32)
   SOCKET socket;
#else
   int fd;
   int last_active;
#endif
   int active;
   AsyncRead read;
   AsyncWrite write;
} AsyncHandle;

typedef struct {
   AsyncProcess *proc;
   int type;
#if defined(_WIN32)
   SOCKET socket;
   SOCKET accept_socket;
   char buf[(sizeof(struct sockaddr_in)+16)*2];
   OVERLAPPED overlapped;
#else
   int fd;
   int last_active;
#endif
   int active;
   Value callback;
   Value data;
} AsyncServerHandle;

typedef struct {
   volatile int refcnt;
   int flags;
#ifdef _WIN32
   HANDLE process;
   HANDLE in;
   HANDLE out;
   HANDLE err;
#else
   volatile pid_t pid;
   volatile int ret_value;
   int in_fd;
   int out_fd;
   int err_fd;
#endif
} ProcessHandle;

typedef struct {
   NativeFunc func;
   void *data;
   Value custom_func;
   int recursive;
#ifdef __wasm__
   ContinuationResultFunc cont_func;
   void *cont_data;
#endif
} LogFunc;

#ifdef FIXIO_SQLITE
typedef struct {
   sqlite3 *db;
} SQLiteHandle;

typedef struct {
   sqlite3_stmt *stmt;
   int done;
   int ignore_step;
} SQLiteStatementHandle;
#endif

typedef int (*CompressFunc)(void *st);

#define NUM_HANDLE_TYPES 11
#define HANDLE_TYPE_ZCOMPRESS       (handles_offset+0)
#define HANDLE_TYPE_ZUNCOMPRESS     (handles_offset+1)
#define HANDLE_TYPE_GZIP_COMPRESS   (handles_offset+2)
#define HANDLE_TYPE_GZIP_UNCOMPRESS (handles_offset+3)
#define HANDLE_TYPE_FILE            (handles_offset+4)
#define HANDLE_TYPE_TCP_CONNECTION  (handles_offset+5)
#define HANDLE_TYPE_TCP_SERVER      (handles_offset+6)
#define HANDLE_TYPE_ASYNC           (handles_offset+7)
#define HANDLE_TYPE_PROCESS         (handles_offset+8)
#define HANDLE_TYPE_SQLITE          (handles_offset+9)
#define HANDLE_TYPE_SQLITE_STMT     (handles_offset+10)

static volatile int handles_offset;
static volatile int async_process_key;
static volatile int global_initialized;

typedef void (*ThreadFunc)(void *data);

typedef struct Thread {
   pthread_mutex_t mutex;
   pthread_cond_t cond;
   ThreadFunc func;
   void *data;
   struct Thread *next;
} Thread;

#ifndef __wasm__
static volatile pthread_mutex_t *threads_mutex;
static Thread *threads_pool;
#endif

static volatile pthread_mutex_t *console_mutex;
#ifndef __wasm__
static int console_initialized = 0;
static char *console_clear_line;
#endif
#if !defined(_WIN32) && !defined(__wasm__)
static volatile pthread_mutex_t *console_auto_flush_mutex;
static volatile pthread_cond_t *console_auto_flush_cond;
static pthread_t console_auto_flush_thread;
static int console_auto_flush_trigger = 0;
static int console_auto_flush_active = 0;
static uint8_t console_input_buf[256];
static int console_input_len = 0;
#endif

#ifdef __APPLE__
extern const char **environ;
#endif

#ifdef _WIN32
static volatile int gui_app = -1;
#endif

#if !defined(_WIN32) && !defined(__HAIKU__)
static volatile locale_t cur_locale = NULL;
#endif

static volatile int console_is_present = -1;
#ifndef __wasm__
static volatile int console_size = -1;
static volatile int console_send_size_event = 0;
#endif
static int console_active = 0;
#ifdef _WIN32
static WORD console_cur_attr;
static CONSOLE_SCREEN_BUFFER_INFO last_csbi;
#else
static volatile int native_charset_utf8 = 0;
#endif


#if defined(_WIN32)
static int pthread_mutex_init(pthread_mutex_t *mutex, void *attr)
{
   InitializeCriticalSection(mutex);
   return 0;
}

static int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
   DeleteCriticalSection(mutex);
   return 0;
}

static int pthread_mutex_lock(pthread_mutex_t *mutex)
{
   EnterCriticalSection(mutex);
   return 0;
}

static int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
   LeaveCriticalSection(mutex);
   return 0;
}

static int pthread_cond_init(pthread_cond_t *cond, void *attr)
{
   *cond = CreateEvent(NULL, FALSE, FALSE, NULL);
   if (!(*cond)) {
      return -1;
   }
   return 0;
}

static int pthread_cond_destroy(pthread_cond_t *cond)
{
   CloseHandle(*cond);
   *cond = NULL;
   return 0;
}

static int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
   LeaveCriticalSection(mutex);
   WaitForSingleObject(*cond, INFINITE);
   EnterCriticalSection(mutex);
   return 0;
}

static int pthread_cond_timedwait_relative(pthread_cond_t *cond, pthread_mutex_t *mutex, int64_t timeout)
{
   int ret = 0;
   LeaveCriticalSection(mutex);
   if (WaitForSingleObject(*cond, timeout/1000000) == WAIT_TIMEOUT) {
      ret = ETIMEDOUT;
   }
   EnterCriticalSection(mutex);
   return ret;
}

static int pthread_cond_signal(pthread_cond_t *cond)
{
   SetEvent(*cond);
   return 0;
}

#elif !defined(__wasm__)

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
   ts.tv_nsec += timeout % 1000000;
   ts.tv_sec += ts.tv_nsec / 1000000 + timeout / 1000000;
   ts.tv_nsec %= 1000000;
   return pthread_cond_timedwait(cond, mutex, &ts);
}
#endif


static int zcompress(ZCompress *st)
{
   #define PUT_BYTE(val)                                              \
   {                                                                  \
      if (st->dest == st->dest_end) {                                 \
         st->extra[st->extra_len++] = val;                            \
      }                                                               \
      else {                                                          \
         *st->dest++ = val;                                           \
      }                                                               \
   }

   #define PUT_BITS(val, nb)                                          \
   {                                                                  \
      st->bits |= (val) << st->num_bits;                              \
      st->num_bits += nb;                                             \
      while (st->num_bits >= 8) {                                     \
         PUT_BYTE(st->bits);                                          \
         st->bits >>= 8;                                              \
         st->num_bits -= 8;                                           \
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

   #define PUT_CIRCULAR(value)                                        \
   {                                                                  \
      int val = value;                                                \
      st->circular[st->circular_pos++] = val;                         \
      st->circular_pos &= 32767;                                      \
      if (st->circular_written < 32768) st->circular_written++;       \
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
      bucket = st->hash + (idx & (num_buckets-1)) * num_slots;        \
   }

   #define GET_DIST(val)                                              \
   (                                                                  \
      st->circular_pos - (val) + ((val) >= st->circular_pos? 32768:0) \
   )

   #define CIRCULAR_MATCH(dist, c1, c2, c3)                           \
   (                                                                  \
      (c1)==st->circular[(st->circular_pos+32768-(dist)+0)&32767] &&  \
      (c2)==st->circular[(st->circular_pos+32768-(dist)+1)&32767] &&  \
      (c3)==st->circular[(st->circular_pos+32768-(dist)+2)&32767]     \
   )

   enum {
      STATE_INIT,
      STATE_MAIN,
      STATE_FLUSH,
      STATE_END,
      STATE_FINISH
   };

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

   int num_buckets = ZCOMP_NUM_BUCKETS;
   int num_slots = ZCOMP_NUM_SLOTS;

   int i, j, k, dist, len, best_len, best_dist=0, slot, worst_slot, worst_dist;
   unsigned short *bucket;
   unsigned char c;

   if (st->extra_len > 0) {
      len = st->dest_end - st->dest;
      if (len > st->extra_len) {
         len = st->extra_len;
      }
      memcpy(st->dest, st->extra, len);
      st->dest += len;
      memmove(st->extra, st->extra + len, st->extra_len - len);
      st->extra_len -= len;
      if (st->dest == st->dest_end) {
         return COMP_FLUSH;
      }
   }

   // max extra bytes:
   // init: 1 byte
   // output literal: 9 bits = 2 bytes
   // output repeat: (8+5)+(5+13) = 31 bits = 4 bytes
   // output trail: 2*9 = 18 bits = 3 bytes
   // flush: 7+3+7+4*8+3 = 52 bits = 7 bytes
   // ending: 7+7 = 14 bits = 2 bytes
   // ending (flushable): 7+3+7+7 = 24 bits = 3 bytes

again:
   switch (st->state) {
      case STATE_INIT:
         if (st->dest == st->dest_end) {
            return COMP_FLUSH;
         }
         PUT_BITS(st->flushable? 0 : 1, 1); // final block
         PUT_BITS(1, 2); // fixed Huffman codes
         st->state = STATE_MAIN;
         goto again;

      case STATE_MAIN:
         if (st->dest == st->dest_end) {
            return COMP_FLUSH;
         }
         if (!st->src_final && !st->src_flush && st->src_end - st->src < 258) {
            return COMP_MORE;
         }
         if (st->src_end - st->src < 3) {
            while (st->src < st->src_end) {
               c = *st->src++;
               PUT_SYM(c);
               PUT_CIRCULAR(c);
            }
            st->state = (st->src_flush && !st->src_final? STATE_FLUSH : STATE_END);
            st->src_flush = 0;
            goto again;
         }

         SELECT_BUCKET(st->src[0], st->src[1], st->src[2]);
         best_len = 0;
         slot = -1;
         worst_slot = 0;
         worst_dist = 0;
         for (i=0; i<num_slots; i++) {
            dist = GET_DIST(bucket[i]);
            if (dist <= st->circular_written && dist >= 3 && CIRCULAR_MATCH(dist, st->src[0], st->src[1], st->src[2])) {
               len = 3;
               for (j=3, k=dist-3; st->src+j < st->src_end && j<258; j++, k--) {
                  if (st->src[j] != (k > 0? st->circular[(st->circular_pos+32768-k) & 32767] : st->src[-k])) break;
                  len++;
               }
               if (len > best_len || (len == best_len && dist < best_dist)) {
                  best_len = len;
                  best_dist = dist;
               }
               if (dist > worst_dist) {
                  worst_slot = i;
                  worst_dist = dist;
               }
            }
            else if (slot < 0) {
               slot = i;
            }
         }

         if (slot < 0) {
            slot = worst_slot;
         }
         bucket[slot] = st->circular_pos;

         if (best_len >= 3) {
            PUT_LEN(best_len);
            PUT_DIST(best_dist);

            while (best_len > 0) {
               PUT_CIRCULAR(*st->src++);
               best_len--;
            }
         }
         else {
            c = *st->src++;
            PUT_SYM(c);
            PUT_CIRCULAR(c);
         }
         goto again;

      case STATE_FLUSH:
         if (!st->flushable) {
            goto error;
         }
         if (st->dest == st->dest_end) {
            return COMP_FLUSH;
         }
         PUT_SYM(256); // end of block
         PUT_BITS(0, 1); // not final block
         PUT_BITS(0, 2); // no compression
         if (st->num_bits > 0) {
            PUT_BITS(0, 8 - st->num_bits);
         }
         PUT_BYTE(0x00);
         PUT_BYTE(0x00);
         PUT_BYTE(0xFF);
         PUT_BYTE(0xFF);
         PUT_BITS(0, 1); // not final block
         PUT_BITS(1, 2); // fixed Huffman codes
         st->state = STATE_MAIN;
         return COMP_FLUSH;

      case STATE_END:
         if (st->dest == st->dest_end) {
            return COMP_FLUSH;
         }
         PUT_SYM(256); // end of block
         if (st->flushable) {
            PUT_BITS(1, 1); // final block
            PUT_BITS(1, 2); // fixed Huffman codes
            PUT_SYM(256); // end of block
         }
         if (st->num_bits > 0) {
            PUT_BITS(0, 8 - st->num_bits);
         }
         st->state = STATE_FINISH;
         return COMP_FLUSH;

      case STATE_FINISH:
         return COMP_DONE;
   }

error:
   return COMP_ERROR;

   #undef PUT_BYTE
   #undef PUT_BITS
   #undef PUT_SYM
   #undef PUT_LEN
   #undef PUT_DIST
   #undef PUT_CIRCULAR
   #undef SELECT_BUCKET
   #undef GET_DIST
   #undef CIRCULAR_MATCH
}


static int zcompress_memory(const unsigned char *src, int src_len, unsigned char **dest_out, int *dest_len_out)
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

static int zuncompress(ZUncompress *st)
{
   #define GET_BITS(dest, nb)                                         \
   {                                                                  \
      while (st->num_bits < nb) {                                     \
         if (st->src == st->src_end) goto retry;                      \
         st->bits |= (*st->src++) << st->num_bits;                    \
         st->num_bits += 8;                                           \
      }                                                               \
      dest = st->bits & ((1 << (nb))-1);                              \
      st->bits >>= nb;                                                \
      st->num_bits -= nb;                                             \
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
      int value = val;                                                \
      *st->dest++ = value;                                            \
      st->circular[st->circular_pos++] = value;                       \
      st->circular_pos &= 32767;                                      \
      if (st->circular_written < 32768) st->circular_written++;       \
   }

   enum {
      STATE_READ_HEADER,
      STATE_UNCOMPRESSED,
      STATE_COMPRESSED,
      STATE_REPEAT,
      STATE_FINISH
   };

   static const char prelength_reorder[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
   static const uint16_t len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
   static const uint8_t  len_bits[29] = { 0, 0, 0, 0, 0, 0, 0,  0,  1,  1,  1,  1,  2,  2,  2,  2,  3,  3,  3,  3,  4,  4,  4,   4,   5,   5,   5,   5,   0 };
   static const uint16_t dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
   static const uint8_t  dist_bits[30] = { 0, 0, 0, 0, 1, 1, 2,  2,  3,  3,  4,  4,  5,  5,   6,   6,   7,   7,   8,   8,    9,    9,   10,   10,   11,   11,   12,    12,    13,    13 };
   
   int type;
   int len, nlen;
   int hlit, hdist, hclen, pos, limit;

   uint8_t prelengths[19], precounts[8], presymbols[19];
   uint8_t lengths[320];

   int i, sym;

   const unsigned char *start_src;
   uint32_t start_bits;
   int start_num_bits;
   int start_final_block;

   // maximum input sizes:
   // block header: 1 byte
   // no compression: 1+4 bytes
   // dynamic: 5+5+4+19*3+320*(7+7)=4551 bits = 1+569 bytes
   // limit=257+31+1+31=320
   // decode: 15+5+15+13=48 bits = 6 bytes

again:
   start_src = st->src;
   start_bits = st->bits;
   start_num_bits = st->num_bits;
   start_final_block = st->final_block;

   switch (st->state) {
      case STATE_READ_HEADER:
         if (st->final_block) {
            st->state = STATE_FINISH;
            return COMP_FLUSH;
         }
         if (st->dest == st->dest_end) {
            return COMP_FLUSH;
         }
         GET_BITS(st->final_block, 1);
         GET_BITS(type, 2);
         if (type == 3) goto error;

         if (type == 0) {
            // no compression:

            st->bits = 0;
            st->num_bits = 0;

            if (st->src_end - st->src < 4) goto retry;
            len = st->src[0] | (st->src[1] << 8);
            nlen = st->src[2] | (st->src[3] << 8);
            if (len != ((~nlen) & 0xFFFF)) goto error;
            st->src += 4;

            st->state = STATE_UNCOMPRESSED;
            st->remaining = len;
            goto again;
         }

         if (type == 2) {
            // dynamic tree:

            GET_BITS(hlit, 5);
            GET_BITS(hdist, 5);
            GET_BITS(hclen, 4);

            limit = 257 + hlit + 1 + hdist;

            for (i=0; i<4+hclen; i++) {
               GET_BITS(prelengths[(int)prelength_reorder[i]], 3);
            }
            for (; i<19; i++) {
               prelengths[(int)prelength_reorder[i]] = 0;
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

         HUFF_BUILD(lengths, 257+hlit, 16, st->lit_symbols, st->lit_counts);
         HUFF_BUILD(lengths+(257+hlit), 1+hdist, 16, st->dist_symbols, st->dist_counts);

         st->state = STATE_COMPRESSED;
         goto again;

      case STATE_UNCOMPRESSED:
         if (st->src == st->src_end) {
            return COMP_MORE;
         }
         if (st->dest == st->dest_end) {
            return COMP_FLUSH;
         }

         len = st->remaining;
         if (len > st->dest_end - st->dest) {
            len = st->dest_end - st->dest;
         }

         for (i=0; i<len; i++) {
            PUT_BYTE(*st->src++);
         }
         st->remaining -= len;
         
         if (st->remaining == 0) {
            st->state = STATE_READ_HEADER;
         }
         goto again;

      case STATE_COMPRESSED:
         if (st->dest == st->dest_end) {
            return COMP_FLUSH;
         }

         HUFF_DECODE(sym, st->lit_symbols, st->lit_counts, 16);
         if (sym < 256) {
            PUT_BYTE(sym);
            goto again;
         }
         if (sym == 256) {
            st->state = STATE_READ_HEADER;
            goto again;
         }
         if (sym > 285) {
            goto error;
         }

         GET_BITS(len, len_bits[sym-257]);
         len += len_base[sym-257];

         HUFF_DECODE(sym, st->dist_symbols, st->dist_counts, 16);
         if (sym > 29) goto error;

         GET_BITS(st->dist, dist_bits[sym]);
         st->dist += dist_base[sym];

         if (st->dist > st->circular_written) goto error;

         st->state = STATE_REPEAT;
         st->remaining = len;
         goto again;

      case STATE_REPEAT:
         if (st->dest == st->dest_end) {
            return COMP_FLUSH;
         }

         len = st->remaining;
         if (len > st->dest_end - st->dest) {
            len = st->dest_end - st->dest;
         }

         for (i=0; i<len; i++) {
            PUT_BYTE(st->circular[(st->circular_pos + 32768 - st->dist) & 32767]);
         }
         st->remaining -= len;

         if (st->remaining == 0) {
            st->state = STATE_COMPRESSED;
         }
         goto again;

      case STATE_FINISH:
         return COMP_DONE;
   }

error:
   return COMP_ERROR;

retry:
   st->src = start_src;
   st->bits = start_bits;
   st->num_bits = start_num_bits;
   st->final_block = start_final_block;
   return COMP_MORE;

   #undef GET_BITS
   #undef HUFF_BUILD
   #undef HUFF_DECODE
   #undef PUT_BYTE
}


static int zuncompress_memory(const unsigned char *src, int src_len, unsigned char **dest_out, int *dest_len_out)
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

   #define PUT_BYTE(value)                                            \
   {                                                                  \
      int val = value;                                                \
      if (out_len == out_cap) {                                       \
         if (out_cap >= (1<<29)) goto error;                          \
         out_cap <<= 1;                                               \
         new_out = realloc(out, out_cap);                             \
         if (!new_out) goto error;                                    \
         out = new_out;                                               \
      }                                                               \
      out[out_len++] = val;                                           \
   }

   static const uint8_t  prelength_reorder[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
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

   out_cap = 4096;
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
            PUT_BYTE(out[out_len-dist]);
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


static uint32_t calc_crc32(uint32_t crc, const unsigned char *buf, int len)
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
   int i;
   
   for (i=0; i<len; i++) {
      crc = table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
   }
   return crc;
}


static int gzip_compress(GZipCompress *st)
{
   #define PUT_BYTE(val)                                              \
   {                                                                  \
      if (st->z.dest == st->z.dest_end) {                             \
         st->extra[st->extra_len++] = val;                            \
      }                                                               \
      else {                                                          \
         *st->z.dest++ = val;                                         \
      }                                                               \
   }

   enum {
      STATE_HEADER,
      STATE_MAIN,
      STATE_FOOTER,
      STATE_FINISH
   };

   const unsigned char *prev_src;
   int len, ret;

   if (st->extra_len > 0) {
      len = st->z.dest_end - st->z.dest;
      if (len > st->extra_len) {
         len = st->extra_len;
      }
      memcpy(st->z.dest, st->extra, len);
      st->z.dest += len;
      memmove(st->extra, st->extra + len, st->extra_len - len);
      st->extra_len -= len;
      if (st->z.dest == st->z.dest_end) {
         return COMP_FLUSH;
      }
   }

again:
   switch (st->state) {
      case STATE_HEADER:
         if (st->z.dest == st->z.dest_end) {
            return COMP_FLUSH;
         }
         PUT_BYTE(0x1f);
         PUT_BYTE(0x8b);
         PUT_BYTE(8); // deflate
         PUT_BYTE(0); // flags
         PUT_BYTE(0); // mtime
         PUT_BYTE(0);
         PUT_BYTE(0);
         PUT_BYTE(0);
         PUT_BYTE(0); // medium=0 fastest=4
         PUT_BYTE(3); // unix
         st->state = STATE_MAIN;
         st->crc = 0xFFFFFFFF;
         if (st->z.dest == st->z.dest_end) {
            return COMP_FLUSH;
         }
         goto again;

      case STATE_MAIN:
         prev_src = st->z.src;
         ret = zcompress(&st->z);
         st->crc = calc_crc32(st->crc, prev_src, st->z.src - prev_src);
         st->in_size += st->z.src - prev_src;
         if (ret == COMP_DONE) {
            st->state = STATE_FOOTER;
            goto again;
         }
         return ret;

      case STATE_FOOTER:
         if (st->z.dest == st->z.dest_end) {
            return COMP_FLUSH;
         }
         st->crc ^= 0xFFFFFFFF;
         PUT_BYTE(st->crc);
         PUT_BYTE(st->crc >> 8);
         PUT_BYTE(st->crc >> 16);
         PUT_BYTE(st->crc >> 24);
         PUT_BYTE(st->in_size);
         PUT_BYTE(st->in_size >> 8);
         PUT_BYTE(st->in_size >> 16);
         PUT_BYTE(st->in_size >> 24);
         st->state = STATE_FINISH;
         return COMP_FLUSH;

      case STATE_FINISH:
         return COMP_DONE;
   }

   return COMP_ERROR;

   #undef PUT_BYTE
}


static int gzip_compress_memory(const unsigned char *src, int src_len, unsigned char **dest_out, int *dest_len_out)
{
   unsigned char *dest = NULL, *comp = NULL, *p;
   uint32_t crc;
   int dest_len, comp_len, retval=0;

   if (!zcompress_memory(src, src_len, &comp, &comp_len)) goto error;

   dest_len = 10 + comp_len + 8;
   dest = malloc(dest_len);
   if (!dest) goto error;

   dest[0] = 0x1f;
   dest[1] = 0x8b;
   dest[2] = 8; // deflate
   dest[3] = 0; // flags
   dest[4] = 0; // mtime
   dest[5] = 0;
   dest[6] = 0;
   dest[7] = 0;
   dest[8] = 0; // medium=0 fastest=4
   dest[9] = 3; // unix
   memcpy(dest+10, comp, comp_len);

   p = dest+10+comp_len;
   crc = calc_crc32(0xFFFFFFFF, src, src_len) ^ 0xFFFFFFFF;
   *p++ = crc;
   *p++ = crc >> 8;
   *p++ = crc >> 16;
   *p++ = crc >> 24;
   *p++ = src_len;
   *p++ = src_len >> 8;
   *p++ = src_len >> 16;
   *p++ = src_len >> 24;

   *dest_out = dest;
   *dest_len_out = dest_len;
   dest = NULL;
   retval = 1;
   
error:
   free(comp);
   free(dest);
   return retval;
}


static int gzip_uncompress(GZipUncompress *st)
{
   enum {
      STATE_HEADER,
      STATE_FEXTRA,
      STATE_FEXTRA_CONTENT,
      STATE_FNAME,
      STATE_FCOMMENT,
      STATE_FHCRC,
      STATE_UNCOMPRESS,
      STATE_FOOTER,
      STATE_FINISH
   };

   int len, ret;
   uint32_t crc32, isize;
   const unsigned char *prev_dest, *start_src;

again:
   start_src = st->z.src;

   switch (st->state) {
      case STATE_HEADER:
         if (st->z.src_end - st->z.src < 10) {
            goto retry;
         }

         if (st->z.src[0] != 0x1f || st->z.src[1] != 0x8b || st->z.src[2] != 8) {
            goto error;
         }
         st->flags = st->z.src[3];
         st->z.src += 10;

         if (st->flags & GZIP_FEXTRA) {
            st->state = STATE_FEXTRA;
         }
         else if (st->flags & GZIP_FNAME) {
            st->state = STATE_FNAME;
         }
         else if (st->flags & GZIP_FCOMMENT) {
            st->state = STATE_FCOMMENT;
         }
         else if (st->flags & GZIP_FHCRC) {
            st->state = STATE_FHCRC;
         }
         else {
            st->state = STATE_UNCOMPRESS;
         }
         st->crc = 0xFFFFFFFF;
         goto again;

      case STATE_FEXTRA:
         if (st->z.src_end - st->z.src < 2) {
            goto retry;
         }
         st->state = STATE_FEXTRA_CONTENT;
         st->remaining = st->z.src[0] | (st->z.src[1] << 8);
         st->z.src += 2;
         goto again;

      case STATE_FEXTRA_CONTENT:
         if (st->z.src == st->z.src_end) {
            return COMP_MORE;
         }

         len = st->remaining;
         if (len > st->z.dest_end - st->z.dest) {
            len = st->z.dest_end - st->z.dest;
         }

         st->z.src += len;
         st->remaining -= len;
         
         if (st->remaining == 0) {
            if (st->flags & GZIP_FNAME) {
               st->state = STATE_FNAME;
            }
            else if (st->flags & GZIP_FCOMMENT) {
               st->state = STATE_FCOMMENT;
            }
            else if (st->flags & GZIP_FHCRC) {
               st->state = STATE_FHCRC;
            }
            else {
               st->state = STATE_UNCOMPRESS;
            }
         }
         goto again;

      case STATE_FNAME:
      case STATE_FCOMMENT:
         if (st->z.src == st->z.src_end) {
            return COMP_MORE;
         }
         while (st->z.src < st->z.src_end) {
            if (*st->z.src++ == 0) {
               if (st->state == STATE_FNAME && (st->flags & GZIP_FCOMMENT)) {
                  st->state = STATE_FCOMMENT;
               }
               else if (st->flags & GZIP_FHCRC) {
                  st->state = STATE_FHCRC;
               }
               else {
                  st->state = STATE_UNCOMPRESS;
               }
               goto again;
            }
         }
         goto again;

      case STATE_FHCRC:
         if (st->z.src_end - st->z.src < 2) {
            goto retry;
         }
         st->state = STATE_UNCOMPRESS;
         st->z.src += 2;
         goto again;

      case STATE_UNCOMPRESS:
         prev_dest = st->z.dest;
         ret = zuncompress(&st->z);
         st->crc = calc_crc32(st->crc, prev_dest, st->z.dest - prev_dest);
         st->out_size += st->z.dest - prev_dest;
         if (ret == COMP_DONE) {
            st->state = STATE_FOOTER;
            goto again;
         }
         return ret;

      case STATE_FOOTER:
         if (st->z.src_end - st->z.src < 8) {
            goto retry;
         }
         
         crc32 = st->z.src[0] | (st->z.src[1] << 8) | (st->z.src[2] << 16) | (st->z.src[3] << 24);
         isize = st->z.src[4] | (st->z.src[5] << 8) | (st->z.src[6] << 16) | (st->z.src[7] << 24);

         if (isize != st->out_size || crc32 != (st->crc ^ 0xFFFFFFFF)) {
            goto error;
         }

         st->z.src += 8;
         st->state = STATE_FINISH;
         return COMP_FLUSH;

      case STATE_FINISH:
         return COMP_DONE;
   }

error:
   return COMP_ERROR;

retry:
   st->z.src = start_src;
   return COMP_MORE;
}


static int gzip_uncompress_memory(const unsigned char *src, int src_len, unsigned char **dest_out, int *dest_len_out)
{
   unsigned char *dest = NULL;
   int dest_len;
   int flags;
   int extra_len;
   uint32_t crc32, isize;
   
   if (src_len < 10) {
      goto error;
   }
   
   if (src[0] != 0x1f || src[1] != 0x8b || src[2] != 8) {
      goto error;
   }

   flags = src[3];

   src += 10;
   src_len -= 10;

   if (flags & GZIP_FEXTRA) {
      if (src_len < 2) {
         goto error;
      }
      extra_len = src[0] | (src[1] << 8);
      if (src_len < 2+extra_len) {
         goto error;
      }

      src += 2+extra_len;
      src_len -= 2+extra_len;
   }

   if (flags & GZIP_FNAME) {
      for (;;) {
         if (src_len == 0) {
            goto error;
         }
         src_len--;
         if (*src++ == 0) {
            break;
         }
      }
   }

   if (flags & GZIP_FCOMMENT) {
      for (;;) {
         if (src_len == 0) {
            goto error;
         }
         src_len--;
         if (*src++ == 0) {
            break;
         }
      }
   }

   if (flags & GZIP_FHCRC) {
      if (src_len < 2) {
         goto error;
      }
      src += 2;
      src_len -= 2;
   }

   if (src_len < 8) {
      goto error;
   }
   
   if (!zuncompress_memory(src, src_len-8, &dest, &dest_len)) {
      goto error;
   }

   src += src_len-8;
   src_len = 8;
   
   crc32 = src[0] | (src[1] << 8) | (src[2] << 16) | (src[3] << 24);
   isize = src[4] | (src[5] << 8) | (src[6] << 16) | (src[7] << 24);

   if (isize != dest_len || crc32 != (calc_crc32(0xFFFFFFFF, dest, dest_len) ^ 0xFFFFFFFF)) {
      goto error;
   }

   *dest_out = dest;
   *dest_len_out = dest_len;
   return 1;

error:
   free(dest);
   return 0;
}


static Value native_zcompress_memory(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   int flags = (intptr_t)data;
   Value ret;
   int err, ok, off, len, out_len;
   unsigned char *in, *out;

   if (num_params == 3) {
      off = fixscript_get_int(params[1]);
      len = fixscript_get_int(params[2]);
   }
   else {
      off = 0;
      err = fixscript_get_array_length(heap, params[0], &len);
      if (err) {
         return fixscript_error(heap, error, err);
      }
   }

   err = fixscript_lock_array(heap, params[0], off, len, (void **)&in, 1, ACCESS_READ_ONLY);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   if (flags & ZC_COMPRESS) {
      if (flags & ZC_GZIP) {
         ok = gzip_compress_memory(in, len, &out, &out_len);
      }
      else {
         ok = zcompress_memory(in, len, &out, &out_len);
      }
   }
   else {
      if (flags & ZC_GZIP) {
         ok = gzip_uncompress_memory(in, len, &out, &out_len);
      }
      else {
         ok = zuncompress_memory(in, len, &out, &out_len);
      }
   }
   fixscript_unlock_array(heap, params[0], off, len, (void **)&in, 1, ACCESS_READ_ONLY);

   if (!ok) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   ret = fixscript_create_or_get_shared_array(heap, -1, out, out_len, 1, free, out, NULL);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return ret;
}


static void free_compress_handle(void *ptr)
{
   CompressHandle *ch = ptr;
   
   fixscript_adjust_heap_size(ch->heap, -ch->size);
   free(ch->state);
   free(ch);
}


static Value native_zcompress_create(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   int flags = (intptr_t)data;
   CompressHandle *ch;
   Value ret;
   int size, type;
   
   if (flags & ZC_COMPRESS) {
      if (flags & ZC_GZIP) {
         type = HANDLE_TYPE_GZIP_COMPRESS;
         size = sizeof(GZipCompress);
      }
      else {
         type = HANDLE_TYPE_ZCOMPRESS;
         size = sizeof(ZCompress);
      }
   }
   else {
      if (flags & ZC_GZIP) {
         type = HANDLE_TYPE_GZIP_UNCOMPRESS;
         size = sizeof(GZipUncompress);
      }
      else {
         type = HANDLE_TYPE_ZUNCOMPRESS;
         size = sizeof(ZUncompress);
      }
   }

   ch = calloc(1, sizeof(CompressHandle));
   if (!ch) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   ch->state = calloc(1, size);
   if (!ch->state) {
      free(ch);
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   
   if (flags & ZC_COMPRESS) {
      ((ZCompress *)ch->state)->flushable = params[0].value;
   }

   ch->heap = heap;
   ch->size = sizeof(CompressHandle) + size;
   fixscript_adjust_heap_size(ch->heap, ch->size);

   ret = fixscript_create_handle(heap, type, ch, free_compress_handle);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return ret;
}


static Value native_zcompress_process(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   CompressHandle *ch;
   CompressFunc func;
   ZCommon *common;
   Value src_val, dest_val;
   int err, ret, type;
   int src_off, src_len, dest_off, dest_len, final;
   unsigned char *src, *dest;

   ch = fixscript_get_handle(heap, params[0], -1, &type);
   if (!ch) {
      *error = fixscript_create_error_string(heap, "invalid handle");
      return fixscript_int(0);
   }

   if (type == HANDLE_TYPE_ZCOMPRESS) {
      func = (CompressFunc)zcompress;
   }
   else if (type == HANDLE_TYPE_ZUNCOMPRESS) {
      func = (CompressFunc)zuncompress;
   }
   else if (type == HANDLE_TYPE_GZIP_COMPRESS) {
      func = (CompressFunc)gzip_compress;
   }
   else if (type == HANDLE_TYPE_GZIP_UNCOMPRESS) {
      func = (CompressFunc)gzip_uncompress;
   }
   else {
      *error = fixscript_create_error_string(heap, "invalid handle");
      return fixscript_int(0);
   }

   src_val = params[1];
   src_off = params[2].value;
   src_len = params[3].value;
   dest_val = params[4];
   dest_off = params[5].value;
   dest_len = params[6].value;
   final = params[7].value;

   err = fixscript_lock_array(heap, src_val, src_off, src_len, (void **)&src, 1, ACCESS_READ_ONLY);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   err = fixscript_lock_array(heap, dest_val, dest_off, dest_len, (void **)&dest, 1, ACCESS_WRITE_ONLY);
   if (err) {
      fixscript_unlock_array(heap, src_val, src_off, src_len, (void **)&src, 1, ACCESS_READ_ONLY);
      return fixscript_error(heap, error, err);
   }

   common = ch->state;
   common->src = src;
   common->src_end = src + src_len;
   common->dest = dest;
   common->dest_end = dest + dest_len;
   if (type == HANDLE_TYPE_ZCOMPRESS || type == HANDLE_TYPE_GZIP_COMPRESS) {
      ((ZCompress *)common)->src_final = (final == 1);
      ((ZCompress *)common)->src_flush = (final == 2);
   }

   ret = func(ch->state);

   ch->read = common->src - src;
   ch->written = common->dest - dest;

   fixscript_unlock_array(heap, src_val, src_off, src_len, (void **)&src, 1, ACCESS_READ_ONLY);
   fixscript_unlock_array(heap, dest_val, dest_off, dest_len, (void **)&dest, 1, ACCESS_WRITE_ONLY);

   if (ret < 0) {
      if (type == HANDLE_TYPE_ZCOMPRESS || type == HANDLE_TYPE_GZIP_COMPRESS) {
         *error = fixscript_create_error_string(heap, "compression error");
      }
      else {
         *error = fixscript_create_error_string(heap, "decompression error");
      }
      return fixscript_int(0);
   }
   return fixscript_int(ret);
}


static Value native_zcompress_get_info(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   CompressHandle *ch;
   int type;

   ch = fixscript_get_handle(heap, params[0], -1, &type);
   if (!ch) {
      *error = fixscript_create_error_string(heap, "invalid handle");
      return fixscript_int(0);
   }

   if (type != HANDLE_TYPE_ZCOMPRESS && type != HANDLE_TYPE_ZUNCOMPRESS && type != HANDLE_TYPE_GZIP_COMPRESS && type != HANDLE_TYPE_GZIP_UNCOMPRESS) {
      *error = fixscript_create_error_string(heap, "invalid handle");
      return fixscript_int(0);
   }

   if ((intptr_t)data == 0) {
      return fixscript_int(ch->read);
   }
   else {
      return fixscript_int(ch->written);
   }
}


static Value native_crc32(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   uint32_t crc = 0xFFFFFFFF;
   Value arr;
   uint8_t *buf;
   int err, off = 0, len = 0;

   if (num_params == 2 || num_params == 4) {
      crc = params[0].value;
   }

   if (num_params == 1 || num_params == 3) {
      arr = params[0];
      if (num_params == 3) {
         off = params[1].value;
         len = params[2].value;
      }
   }
   else {
      arr = params[1];
      if (num_params == 4) {
         off = params[2].value;
         len = params[3].value;
      }
   }

   if (num_params == 1 || num_params == 2) {
      off = 0;
      err = fixscript_get_array_length(heap, arr, &len);
      if (err) {
         return fixscript_error(heap, error, err);
      }
   }

   err = fixscript_lock_array(heap, arr, off, len, (void **)&buf, 1, ACCESS_READ_ONLY);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   crc = calc_crc32(crc, buf, len);

   fixscript_unlock_array(heap, arr, off, len, (void **)&buf, 1, ACCESS_READ_ONLY);

   if (num_params == 1 || num_params == 3) {
      crc ^= 0xFFFFFFFF;
   }
   return fixscript_int(crc);
}


static Value native_path_get_separator(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(_WIN32)
   return fixscript_int('\\');
#else
   return fixscript_int('/');
#endif
}


static Value native_path_get_prefix_length(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(_WIN32)
   Value val[3];
   int err, len;

   err = fixscript_get_array_length(heap, params[0], &len);
   if (!err) {
      if (len > 3) len = 3;
      err = fixscript_get_array_range(heap, params[0], 0, len, val);
   }
   if (err) {
      return fixscript_error(heap, error, err);
   }
   if (len >= 3 && ((val[0].value >= 'A' && val[0].value <= 'Z') || (val[0].value >= 'a' && val[0].value <= 'z')) && val[1].value == ':' && val[2].value == '\\') {
      return fixscript_int(3);
   }
   return fixscript_int(0);
#else
   Value val;
   int err;

   err = fixscript_get_array_elem(heap, params[0], 0, &val);
   if (err == FIXSCRIPT_ERR_OUT_OF_BOUNDS) {
      return fixscript_int(0);
   }
   if (err) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   return fixscript_int(val.value == '/'? 1 : 0);
#endif
}


static Value native_path_is_valid_name(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   return fixscript_int(1);
}


static Value native_path_get_current(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(_WIN32)
   DWORD len;
   uint16_t *s = NULL;
#else
   char *s = NULL;
#endif
   Value ret;

#if defined(_WIN32)
   len = GetCurrentDirectory(0, NULL);
   if (len <= 0 || len > 16*1024) {
      *error = fixscript_create_error_string(heap, "I/O error");
      goto error;
   }
   s = malloc((len+1)*sizeof(uint16_t));
   if (!s) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }
   if (GetCurrentDirectory(len, s) != len-1) {
      *error = fixscript_create_error_string(heap, "I/O error");
      goto error;
   }
   ret = fixscript_create_string_utf16(heap, s, -1);
   if (!ret.value) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }
#else
   #ifdef __wasm__
      s = strdup(".");
   #else
      s = getcwd(NULL, 0);
   #endif
   if (!s) {
      *error = fixscript_create_error_string(heap, "I/O error");
      goto error;
   }

   ret = fixscript_create_string(heap, s, -1);
   if (!ret.value) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }
#endif

   return ret;

error:
   free(s);
   return fixscript_int(0);
}


static Value native_path_get_roots(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value arr, str;
#if defined(_WIN32)
   DWORD len;
   uint16_t *buf = NULL, *p, *s;
#endif
   int err;

   arr = fixscript_create_array(heap, 0);
   if (!arr.value) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

#if defined(_WIN32)
   len = GetLogicalDriveStrings(0, NULL);
   if (len <= 0 || len > 16*1024) {
      *error = fixscript_create_error_string(heap, "I/O error");
      goto error;
   }
   buf = malloc((len+1)*sizeof(uint16_t));
   if (!buf) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }
   if (GetLogicalDriveStrings(len, buf) != len-1) {
      *error = fixscript_create_error_string(heap, "I/O error");
      goto error;
   }
   s = buf;
   for (p=s; ; p++) {
      if (*p != 0) continue;
      if (p == s) break;

      str = fixscript_create_string_utf16(heap, s, p-s);
      if (!str.value) {
         fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         goto error;
      }
      err = fixscript_append_array_elem(heap, arr, str);
      if (err) {
         fixscript_error(heap, error, err);
         goto error;
      }
      s = p+1;
   }
#else
   str = fixscript_create_string(heap, "/", -1);
   if (!str.value) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   err = fixscript_append_array_elem(heap, arr, str);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }
#endif

   return arr;

error:
#if defined(_WIN32)
   free(buf);
#endif
   return fixscript_int(0);
}


#ifdef __wasm__
static int compare(const void *d1, const void *d2)
{
   const char **name1 = (void *)d1;
   const char **name2 = (void *)d2;
   return strcmp(*name1, *name2);
}
#else
#if defined(_WIN32)
static int compare(const void *d1, const void *d2)
{
   const uint16_t **name1 = (void *)d1;
   const uint16_t **name2 = (void *)d2;
   return wcscmp(*name1, *name2);
}
#else
static int filter(const struct dirent *dirent)
{
   if (dirent->d_name[0] == '.') {
      if (strcmp(dirent->d_name, ".") == 0) return 0;
      if (strcmp(dirent->d_name, "..") == 0) return 0;
   }
   return 1;
}

#if defined(__APPLE__)
static int compare(const struct dirent **d1, const struct dirent **d2)
#else
static int compare(const void *d1, const void *d2)
#endif
{
   const struct dirent **e1 = (void *)d1;
   const struct dirent **e2 = (void *)d2;
   return strcmp((*e1)->d_name, (*e2)->d_name);
}
#endif
#endif /* __wasm__ */


#ifdef __wasm__
typedef struct {
   Heap *heap;
   char *dirname;
   ContinuationResultFunc cont_func;
   void *cont_data;
} PathGetFilesCont;

void path_get_files_cont(int num_files, char **files, int wasm_error, void *data)
{
   PathGetFilesCont *cont = data;
   Heap *heap = cont->heap;
   Value ret = fixscript_int(0), error = fixscript_int(0), arr, val;
   ContinuationResultFunc cont_func;
   void *cont_data;
   char buf[256];
   int i, j, err, num_files_filtered;

   if (wasm_error) {
      if (wasm_error == WASM_ERROR_NOT_SUPPORTED) {
         error = fixscript_create_error_string(heap, "not supported");
      }
      else if (wasm_error == WASM_ERROR_FILE_NOT_FOUND) {
         snprintf(buf, sizeof(buf), "path '%s' does not exist", cont->dirname);
         error = fixscript_create_error_string(heap, buf);
      }
      else if (wasm_error == WASM_ERROR_NOT_DIRECTORY) {
         snprintf(buf, sizeof(buf), "path '%s' is not a directory", cont->dirname);
         error = fixscript_create_error_string(heap, buf);
      }
      else {
         error = fixscript_create_error_string(heap, "I/O error");
      }
      goto error;
   }

   qsort(files, num_files, sizeof(char *), compare);
   num_files_filtered = num_files;
   for (i=0; i<num_files; i++) {
      if (strcmp(files[i], ".") == 0 || strcmp(files[i], "..") == 0) {
         num_files_filtered--;
      }
   }

   arr = fixscript_create_array(heap, num_files_filtered);
   if (!arr.value) {
      fixscript_error(heap, &error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   for (i=0, j=0; i<num_files; i++) {
      if (strcmp(files[i], ".") == 0 || strcmp(files[i], "..") == 0) {
         continue;
      }

      val = fixscript_create_string(heap, files[i], -1);
      if (!val.value) {
         fixscript_error(heap, &error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         goto error;
      }

      err = fixscript_set_array_elem(heap, arr, j++, val);
      if (err) {
         fixscript_error(heap, &error, err);
         goto error;
      }
   }

   ret = arr;

error:
   free(cont->dirname);
   if (files) {
      for (i=0; i<num_files; i++) {
         free(files[i]);
      }
      free(files);
   }

   cont_func = cont->cont_func;
   cont_data = cont->cont_data;
   free(cont);

   cont_func(heap, ret, error, cont_data);
}
#endif /* __wasm__ */


static Value native_path_get_files(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(_WIN32)
   HANDLE handle = INVALID_HANDLE_VALUE;
   WIN32_FIND_DATA fd;
   uint16_t **files = NULL, **new_files;
   Value arr, val, retval = fixscript_int(0);
   int i, cnt=0, cap=0;
   uint16_t *dirname = NULL, *dirname2 = NULL;
   uint16_t buf[256];
   int err;

   err = fixscript_get_string_utf16(heap, params[0], 0, -1, &dirname, NULL);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   dirname2 = malloc((wcslen(dirname)+2+1)*sizeof(uint16_t));
   if (!dirname2) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   wcscpy(dirname2, dirname);
   wcscat(dirname2, L"\\*");

   handle = FindFirstFile(dirname2, &fd);
   if (handle == INVALID_HANDLE_VALUE) {
      err = GetLastError();
      if (err == ERROR_PATH_NOT_FOUND) {
         snwprintf(buf, sizeof(buf)/sizeof(uint16_t)-1, L"path '%s' does not exist", dirname);
         *error = fixscript_create_error(heap, fixscript_create_string_utf16(heap, buf, -1));
      }
      else if (err == ERROR_DIRECTORY) {
         snwprintf(buf, sizeof(buf)/sizeof(uint16_t)-1, L"path '%s' is not a directory", dirname);
         *error = fixscript_create_error(heap, fixscript_create_string_utf16(heap, buf, -1));
      }
      else {
         *error = fixscript_create_error_string(heap, "I/O error");
      }
      goto error;
   }

   do {
      if (fd.cFileName[0] == '.') {
         if (fd.cFileName[1] == 0) continue;
         if (fd.cFileName[1] == '.' && fd.cFileName[2] == 0) continue;
      }

      if (cnt == cap) {
         cap = cap? cap*2 : 8;
         new_files = realloc(files, cap * sizeof(Value));
         if (!new_files) {
            fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
            goto error;
         }
         files = new_files;
      }

      files[cnt] = wcsdup(fd.cFileName);
      if (!files[cnt]) {
         fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         goto error;
      }
      cnt++;
   }
   while (FindNextFile(handle, &fd));

   qsort(files, cnt, sizeof(uint16_t *), compare);

   arr = fixscript_create_array(heap, cnt);
   if (!arr.value) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   for (i=0; i<cnt; i++) {
      val = fixscript_create_string_utf16(heap, files[i], -1);
      if (!val.value) {
         fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         goto error;
      }

      err = fixscript_set_array_elem(heap, arr, i, val);
      if (err) {
         fixscript_error(heap, error, err);
         goto error;
      }
   }

   retval = arr;

error:
   free(dirname);
   free(dirname2);
   if (handle != INVALID_HANDLE_VALUE) {
      FindClose(handle);
   }
   for (i=0; i<cnt; i++) {
      free(files[i]);
   }
   free(files);
   return retval;
#elif defined(__wasm__)
   Value arr, val, retval = fixscript_int(0);
   PathGetFilesCont *cont;
   char *dirname = NULL;
   char **files = NULL;
   char buf[256];
   int i, j, err, num_files = 0, num_files_filtered;
   
   err = fixscript_get_string(heap, params[0], 0, -1, &dirname, NULL);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   if (wasm_path_get_files_sync(dirname, &num_files, &files, &err)) {
      if (err) {
         if (err == WASM_ERROR_NOT_SUPPORTED) {
            *error = fixscript_create_error_string(heap, "not supported");
         }
         else if (err == WASM_ERROR_FILE_NOT_FOUND) {
            snprintf(buf, sizeof(buf), "path '%s' does not exist", dirname);
            *error = fixscript_create_error_string(heap, buf);
         }
         else if (err == WASM_ERROR_NOT_DIRECTORY) {
            snprintf(buf, sizeof(buf), "path '%s' is not a directory", dirname);
            *error = fixscript_create_error_string(heap, buf);
         }
         else {
            *error = fixscript_create_error_string(heap, "I/O error");
         }
         goto error;
      }

      qsort(files, num_files, sizeof(char *), compare);
      num_files_filtered = num_files;
      for (i=0; i<num_files; i++) {
         if (strcmp(files[i], ".") == 0 || strcmp(files[i], "..") == 0) {
            num_files_filtered--;
         }
      }

      arr = fixscript_create_array(heap, num_files_filtered);
      if (!arr.value) {
         fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         goto error;
      }

      for (i=0, j=0; i<num_files; i++) {
         if (strcmp(files[i], ".") == 0 || strcmp(files[i], "..") == 0) {
            continue;
         }

         val = fixscript_create_string(heap, files[i], -1);
         if (!val.value) {
            fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
            goto error;
         }

         err = fixscript_set_array_elem(heap, arr, j++, val);
         if (err) {
            fixscript_error(heap, error, err);
            goto error;
         }
      }

      retval = arr;
   }
   else {
      cont = malloc(sizeof(PathGetFilesCont));
      cont->heap = heap;
      cont->dirname = dirname;
      fixscript_suspend(heap, &cont->cont_func, &cont->cont_data);
      wasm_path_get_files(dirname, path_get_files_cont, cont);
      return fixscript_int(0);
   }

error:
   free(dirname);
   if (files) {
      for (i=0; i<num_files; i++) {
         free(files[i]);
      }
      free(files);
   }
   return retval;
#else
   struct dirent **namelist = NULL;
   Value arr, val, retval = fixscript_int(0);
   char *dirname = NULL;
   int err, i, cnt=0;
   char buf[256];
   
   err = fixscript_get_string(heap, params[0], 0, -1, &dirname, NULL);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   cnt = scandir(dirname, &namelist, filter, compare);
   if (cnt < 0) {
      if (errno == ENOENT) {
         snprintf(buf, sizeof(buf), "path '%s' does not exist", dirname);
         *error = fixscript_create_error_string(heap, buf);
      }
      else if (errno == ENOTDIR) {
         snprintf(buf, sizeof(buf), "path '%s' is not a directory", dirname);
         *error = fixscript_create_error_string(heap, buf);
      }
      else {
         *error = fixscript_create_error_string(heap, "I/O error");
      }
      goto error;
   }

   arr = fixscript_create_array(heap, cnt);
   if (!arr.value) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   for (i=0; i<cnt; i++) {
      val = fixscript_create_string(heap, namelist[i]->d_name, -1);
      if (!val.value) {
         fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         goto error;
      }

      err = fixscript_set_array_elem(heap, arr, i, val);
      if (err) {
         fixscript_error(heap, error, err);
         goto error;
      }
   }

   retval = arr;

error:
   free(dirname);
   if (namelist) {
      for (i=0; i<cnt; i++) {
         free(namelist[i]);
      }
      free(namelist);
   }
   return retval;
#endif
}


#ifdef __wasm__
typedef struct {
   Heap *heap;
   char *path;
   ContinuationResultFunc cont_func;
   void *cont_data;
} PathExistsCont;

void path_exists_cont(int exists, int wasm_error, void *data)
{
   PathExistsCont *cont = data;
   Heap *heap = cont->heap;
   Value ret = fixscript_int(0), error = fixscript_int(0);
   ContinuationResultFunc cont_func;
   void *cont_data;
   char buf[256];

   if (wasm_error) {
      if (wasm_error == WASM_ERROR_ACCESS_DENIED) {
         snprintf(buf, sizeof(buf), "access denied to '%s'", cont->path);
         error = fixscript_create_error_string(heap, buf);
      }
      else {
         error = fixscript_create_error_string(heap, "I/O error");
      }
   }
   else {
      ret = fixscript_int(exists);
   }

   free(cont->path);

   cont_func = cont->cont_func;
   cont_data = cont->cont_data;
   free(cont);

   cont_func(heap, ret, error, cont_data);
}
#endif /* __wasm__ */


static Value native_path_exists(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(_WIN32)
   uint16_t *path = NULL;
   Value retval = fixscript_int(0);
   uint16_t buf[256];
   int err;

   err = fixscript_get_string_utf16(heap, params[0], 0, -1, &path, NULL);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }
   
   if (GetFileAttributes(path) == INVALID_FILE_ATTRIBUTES) {
      err = GetLastError();
      if (err == ERROR_ACCESS_DENIED) {
         snwprintf(buf, sizeof(buf)/sizeof(uint16_t)-1, L"access denied to '%s'", path);
         *error = fixscript_create_error(heap, fixscript_create_string_utf16(heap, buf, -1));
      }
      else if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
         goto error;
      }
      else {
         *error = fixscript_create_error_string(heap, "I/O error");
      }
      goto error;
   }

   retval = fixscript_int(1);

error:
   free(path);
   return retval;
#else
   char *path = NULL;
   Value retval = fixscript_int(0);
   char buf[256];
   int err;
#ifdef __wasm__
   PathExistsCont *cont;
   int exists;
#else
   struct stat st;
#endif

   err = fixscript_get_string(heap, params[0], 0, -1, &path, NULL);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

#if defined(__wasm__)
   if (wasm_path_exists_sync(path, &exists, &err)) {
      if (err) {
         if (err == WASM_ERROR_ACCESS_DENIED) {
            snprintf(buf, sizeof(buf), "access denied to '%s'", path);
            *error = fixscript_create_error_string(heap, buf);
         }
         else {
            *error = fixscript_create_error_string(heap, "I/O error");
         }
         goto error;
      }
      if (!exists) {
         goto error;
      }
   }
   else {
      cont = malloc(sizeof(PathExistsCont));
      cont->heap = heap;
      cont->path = path;
      fixscript_suspend(heap, &cont->cont_func, &cont->cont_data);
      wasm_path_exists(path, path_exists_cont, cont);
      return fixscript_int(0);
   }
#else
   if (stat(path, &st) != 0) {
      if (errno == EACCES) {
         snprintf(buf, sizeof(buf), "access denied to '%s'", path);
         *error = fixscript_create_error_string(heap, buf);
      }
      else if (errno != ENOENT && errno != ENOTDIR) {
         *error = fixscript_create_error_string(heap, "I/O error");
      }
      goto error;
   }
#endif

   retval = fixscript_int(1);

error:
   free(path);
   return retval;
#endif
}


#ifdef __wasm__
typedef struct {
   Heap *heap;
   char *path;
   ContinuationResultFunc cont_func;
   void *cont_data;
} PathGetTypeCont;

void path_get_type_cont(int type, int64_t length, int64_t mtime, int wasm_error, void *data)
{
   PathGetTypeCont *cont = data;
   Heap *heap = cont->heap;
   Value ret = fixscript_int(0), error = fixscript_int(0);
   ContinuationResultFunc cont_func;
   void *cont_data;
   char buf[256];

   if (wasm_error) {
      if (wasm_error == WASM_ERROR_ACCESS_DENIED) {
         snprintf(buf, sizeof(buf), "access denied to '%s'", cont->path);
         error = fixscript_create_error_string(heap, buf);
      }
      else if (wasm_error == WASM_ERROR_FILE_NOT_FOUND) {
         snprintf(buf, sizeof(buf), "path '%s' not found", cont->path);
         error = fixscript_create_error_string(heap, buf);
      }
      else {
         error = fixscript_create_error_string(heap, "I/O error");
      }
   }
   else {
      ret = fixscript_int(type);
   }

   free(cont->path);

   cont_func = cont->cont_func;
   cont_data = cont->cont_data;
   free(cont);

   cont_func(heap, ret, error, cont_data);
}
#endif /* __wasm__ */


static Value native_path_get_type(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(_WIN32)
   WIN32_FILE_ATTRIBUTE_DATA fad;
   uint16_t *path = NULL;
   Value retval = fixscript_int(0);
   uint16_t buf[256];
   int ret, err;

   err = fixscript_get_string_utf16(heap, params[0], 0, -1, &path, NULL);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }
   
   if (GetFileAttributesEx(path, GetFileExInfoStandard, &fad) == 0) {
      err = GetLastError();
      if (err == ERROR_ACCESS_DENIED) {
         snwprintf(buf, sizeof(buf)/sizeof(uint16_t)-1, L"access denied to '%s'", path);
         *error = fixscript_create_error(heap, fixscript_create_string_utf16(heap, buf, -1));
      }
      else if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
         snwprintf(buf, sizeof(buf)/sizeof(uint16_t)-1, L"path '%s' not found", path);
         *error = fixscript_create_error(heap, fixscript_create_string_utf16(heap, buf, -1));
      }
      else {
         *error = fixscript_create_error_string(heap, "I/O error");
      }
      goto error;
   }

   if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      ret = TYPE_DIRECTORY;
   }
   else if (fad.dwFileAttributes & FILE_ATTRIBUTE_DEVICE) {
      ret = TYPE_SPECIAL;
   }
   else {
      ret = TYPE_FILE;
   }

   retval = fixscript_int(ret);

error:
   free(path);
   return retval;
#else
   char *path = NULL;
   Value retval = fixscript_int(0);
#ifdef __wasm__
   PathGetTypeCont *cont;
   int type;
#else
   struct stat st, st2;
   int stat_ret, lstat_ret=0;
#endif
   char buf[256];
   int ret = 0, err;
   
   err = fixscript_get_string(heap, params[0], 0, -1, &path, NULL);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

#ifdef __wasm__
   if (wasm_path_get_info_sync(path, &type, NULL, NULL, &err)) {
      if (err) {
         if (err == WASM_ERROR_ACCESS_DENIED) {
            snprintf(buf, sizeof(buf), "access denied to '%s'", path);
            *error = fixscript_create_error_string(heap, buf);
         }
         else if (err == WASM_ERROR_FILE_NOT_FOUND) {
            snprintf(buf, sizeof(buf), "path '%s' not found", path);
            *error = fixscript_create_error_string(heap, buf);
         }
         else {
            *error = fixscript_create_error_string(heap, "I/O error");
         }
         goto error;
      }
      ret = type;
   }
   else {
      cont = malloc(sizeof(PathGetTypeCont));
      cont->heap = heap;
      cont->path = path;
      fixscript_suspend(heap, &cont->cont_func, &cont->cont_data);
      wasm_path_get_info(path, path_get_type_cont, cont);
      return fixscript_int(0);
   }
#else
   stat_ret = stat(path, &st);
   lstat_ret = lstat(path, &st2);
   if (stat_ret != 0 || lstat_ret != 0) {
      if (errno == EACCES) {
         snprintf(buf, sizeof(buf), "access denied to '%s'", path);
         *error = fixscript_create_error_string(heap, buf);
      }
      else if (errno == ENOENT || errno == ENOTDIR) {
         if (stat_ret != 0 && lstat_ret == 0) {
            retval = fixscript_int(TYPE_SPECIAL | TYPE_SYMLINK);
            goto error;
         }
         snprintf(buf, sizeof(buf), "path '%s' not found", path);
         *error = fixscript_create_error_string(heap, buf);
      }
      else {
         *error = fixscript_create_error_string(heap, "I/O error");
      }
      goto error;
   }

   if (S_ISDIR(st.st_mode)) {
      ret = TYPE_DIRECTORY;
   }
   else if (S_ISREG(st.st_mode)) {
      ret = TYPE_FILE;
   }
   else {
      ret = TYPE_SPECIAL;
   }

   if (S_ISLNK(st2.st_mode)) {
      ret |= TYPE_SYMLINK;
   }
#endif

   retval = fixscript_int(ret);

error:
   free(path);
   return retval;
#endif
}


#ifdef __wasm__
typedef struct {
   Heap *heap;
   char *path;
   ContinuationResultFunc cont_func;
   void *cont_data;
} PathGetLengthCont;

void path_get_length_cont(int type, int64_t length, int64_t mtime, int wasm_error, void *data)
{
   PathGetLengthCont *cont = data;
   Heap *heap = cont->heap;
   Value ret = fixscript_int(0), error = fixscript_int(0);
   ContinuationResultFunc cont_func;
   void *cont_data;
   char buf[256];

   if (wasm_error) {
      if (wasm_error == WASM_ERROR_ACCESS_DENIED) {
         snprintf(buf, sizeof(buf), "access denied to '%s'", cont->path);
         error = fixscript_create_error_string(heap, buf);
      }
      else if (wasm_error == WASM_ERROR_FILE_NOT_FOUND) {
         snprintf(buf, sizeof(buf), "path '%s' not found", cont->path);
         error = fixscript_create_error_string(heap, buf);
      }
      else {
         error = fixscript_create_error_string(heap, "I/O error");
      }
   }
   else {
      error = fixscript_int(length >> 32);
      ret = fixscript_int(length);
   }

   free(cont->path);

   cont_func = cont->cont_func;
   cont_data = cont->cont_data;
   free(cont);

   cont_func(heap, ret, error, cont_data);
}
#endif /* __wasm__ */


static Value native_path_get_length(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(_WIN32)
   WIN32_FILE_ATTRIBUTE_DATA fad;
   uint16_t *path = NULL;
   Value retval = fixscript_int(0);
   uint16_t buf[256];
   int err;

   err = fixscript_get_string_utf16(heap, params[0], 0, -1, &path, NULL);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }
   
   if (GetFileAttributesEx(path, GetFileExInfoStandard, &fad) == 0) {
      err = GetLastError();
      if (err == ERROR_ACCESS_DENIED) {
         snwprintf(buf, sizeof(buf)/sizeof(uint16_t)-1, L"access denied to '%s'", path);
         *error = fixscript_create_error(heap, fixscript_create_string_utf16(heap, buf, -1));
      }
      else if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
         snwprintf(buf, sizeof(buf)/sizeof(uint16_t)-1, L"path '%s' not found", path);
         *error = fixscript_create_error(heap, fixscript_create_string_utf16(heap, buf, -1));
      }
      else {
         *error = fixscript_create_error_string(heap, "I/O error");
      }
      goto error;
   }

   *error = fixscript_int(fad.nFileSizeHigh);
   retval = fixscript_int(fad.nFileSizeLow);

error:
   free(path);
   return retval;
#else
   char *path = NULL;
   Value retval = fixscript_int(0);
#ifdef __wasm__
   PathGetLengthCont *cont;
   int64_t length;
#else
   struct stat st;
#endif
   char buf[256];
   int err;
   
   err = fixscript_get_string(heap, params[0], 0, -1, &path, NULL);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

#ifdef __wasm__
   if (wasm_path_get_info_sync(path, NULL, &length, NULL, &err)) {
      if (err) {
         if (err == WASM_ERROR_ACCESS_DENIED) {
            snprintf(buf, sizeof(buf), "access denied to '%s'", path);
            *error = fixscript_create_error_string(heap, buf);
         }
         else if (err == WASM_ERROR_FILE_NOT_FOUND) {
            snprintf(buf, sizeof(buf), "path '%s' not found", path);
            *error = fixscript_create_error_string(heap, buf);
         }
         else {
            *error = fixscript_create_error_string(heap, "I/O error");
         }
         goto error;
      }
      *error = fixscript_int(length >> 32);
      retval = fixscript_int(length);
   }
   else {
      cont = malloc(sizeof(PathGetLengthCont));
      cont->heap = heap;
      cont->path = path;
      fixscript_suspend(heap, &cont->cont_func, &cont->cont_data);
      wasm_path_get_info(path, path_get_length_cont, cont);
      return fixscript_int(0);
   }
#else
   if (stat(path, &st) != 0) {
      if (errno == EACCES) {
         snprintf(buf, sizeof(buf), "access denied to '%s'", path);
         *error = fixscript_create_error_string(heap, buf);
      }
      else if (errno == ENOENT || errno == ENOTDIR) {
         if (lstat(path, &st) == 0) {
            goto error;
         }
         snprintf(buf, sizeof(buf), "path '%s' not found", path);
         *error = fixscript_create_error_string(heap, buf);
      }
      else {
         *error = fixscript_create_error_string(heap, "I/O error");
      }
      goto error;
   }

   if (S_ISREG(st.st_mode)) {
      *error = fixscript_int(((int64_t)st.st_size) >> 32);
      retval = fixscript_int((int64_t)st.st_size);
   }
#endif

error:
   free(path);
   return retval;
#endif
}


#ifdef __wasm__
typedef struct {
   Heap *heap;
   char *path;
   ContinuationResultFunc cont_func;
   void *cont_data;
} PathGetModTimeCont;

void path_get_mtime_cont(int type, int64_t length, int64_t mtime, int wasm_error, void *data)
{
   PathGetModTimeCont *cont = data;
   Heap *heap = cont->heap;
   Value ret = fixscript_int(0), error = fixscript_int(0);
   ContinuationResultFunc cont_func;
   void *cont_data;
   char buf[256];

   if (wasm_error) {
      if (wasm_error == WASM_ERROR_ACCESS_DENIED) {
         snprintf(buf, sizeof(buf), "access denied to '%s'", cont->path);
         error = fixscript_create_error_string(heap, buf);
      }
      else if (wasm_error == WASM_ERROR_FILE_NOT_FOUND) {
         snprintf(buf, sizeof(buf), "path '%s' not found", cont->path);
         error = fixscript_create_error_string(heap, buf);
      }
      else {
         error = fixscript_create_error_string(heap, "I/O error");
      }
   }
   else {
      error = fixscript_int(mtime >> 32);
      ret = fixscript_int(mtime);
   }

   free(cont->path);

   cont_func = cont->cont_func;
   cont_data = cont->cont_data;
   free(cont);

   cont_func(heap, ret, error, cont_data);
}
#endif /* __wasm__ */


static Value native_path_get_modification_time(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(_WIN32)
   WIN32_FILE_ATTRIBUTE_DATA fad;
   uint16_t *path = NULL;
   Value retval = fixscript_int(0);
   uint16_t buf[256];
   int64_t time;
   int err;

   err = fixscript_get_string_utf16(heap, params[0], 0, -1, &path, NULL);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }
   
   if (GetFileAttributesEx(path, GetFileExInfoStandard, &fad) == 0) {
      err = GetLastError();
      if (err == ERROR_ACCESS_DENIED) {
         snwprintf(buf, sizeof(buf)/sizeof(uint16_t)-1, L"access denied to '%s'", path);
         *error = fixscript_create_error(heap, fixscript_create_string_utf16(heap, buf, -1));
      }
      else if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
         snwprintf(buf, sizeof(buf)/sizeof(uint16_t)-1, L"path '%s' not found", path);
         *error = fixscript_create_error(heap, fixscript_create_string_utf16(heap, buf, -1));
      }
      else {
         *error = fixscript_create_error_string(heap, "I/O error");
      }
      goto error;
   }

   time = (((uint64_t)fad.ftLastWriteTime.dwHighDateTime) << 32) | ((uint32_t)fad.ftLastWriteTime.dwLowDateTime);
   time = time / 10000000LL - 11644473600LL;

   *error = fixscript_int(((uint64_t)time) >> 32);
   retval = fixscript_int(time);

error:
   free(path);
   return retval;
#else
   char *path = NULL;
   Value retval = fixscript_int(0);
#ifdef __wasm__
   PathGetModTimeCont *cont;
   int64_t mtime;
#else
   struct stat st;
#endif
   char buf[256];
   int err;
   
   err = fixscript_get_string(heap, params[0], 0, -1, &path, NULL);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

#ifdef __wasm__
   if (wasm_path_get_info_sync(path, NULL, NULL, &mtime, &err)) {
      if (err) {
         if (err == WASM_ERROR_ACCESS_DENIED) {
            snprintf(buf, sizeof(buf), "access denied to '%s'", path);
            *error = fixscript_create_error_string(heap, buf);
         }
         else if (err == WASM_ERROR_FILE_NOT_FOUND) {
            snprintf(buf, sizeof(buf), "path '%s' not found", path);
            *error = fixscript_create_error_string(heap, buf);
         }
         else {
            *error = fixscript_create_error_string(heap, "I/O error");
         }
         goto error;
      }
      *error = fixscript_int(mtime >> 32);
      retval = fixscript_int(mtime);
   }
   else {
      cont = malloc(sizeof(PathGetModTimeCont));
      cont->heap = heap;
      cont->path = path;
      fixscript_suspend(heap, &cont->cont_func, &cont->cont_data);
      wasm_path_get_info(path, path_get_mtime_cont, cont);
      return fixscript_int(0);
   }
#else
   if (stat(path, &st) != 0) {
      if (errno == EACCES) {
         snprintf(buf, sizeof(buf), "access denied to '%s'", path);
         *error = fixscript_create_error_string(heap, buf);
      }
      else if (errno == ENOENT || errno == ENOTDIR) {
         if (lstat(path, &st) == 0) {
            goto error;
         }
         snprintf(buf, sizeof(buf), "path '%s' not found", path);
         *error = fixscript_create_error_string(heap, buf);
      }
      else {
         *error = fixscript_create_error_string(heap, "I/O error");
      }
      goto error;
   }

   if (sizeof(time_t) == 4) {
      // treat time_t as unsigned to give chance for possible unsigned hack once year 2038 happens:
      *error = fixscript_int(0);
      retval = fixscript_int(st.st_mtime);
   }
   else {
      *error = fixscript_int(((int64_t)st.st_mtime) >> 32);
      retval = fixscript_int((int64_t)st.st_mtime);
   }
#endif

error:
   free(path);
   return retval;
#endif
}


#ifdef __wasm__
typedef struct {
   Heap *heap;
   char *path;
   ContinuationResultFunc cont_func;
   void *cont_data;
} PathGetSymlinkCont;

void path_get_symlink_cont(char *orig_path, int wasm_error, void *data)
{
   PathGetSymlinkCont *cont = data;
   Heap *heap = cont->heap;
   Value ret = fixscript_int(0), error = fixscript_int(0);
   ContinuationResultFunc cont_func;
   void *cont_data;
   char buf[256];

   if (wasm_error) {
      if (wasm_error == WASM_ERROR_ACCESS_DENIED) {
         snprintf(buf, sizeof(buf), "access denied to '%s'", cont->path);
         error = fixscript_create_error_string(heap, buf);
      }
      else if (wasm_error == WASM_ERROR_FILE_NOT_FOUND) {
         snprintf(buf, sizeof(buf), "path '%s' not found", cont->path);
         error = fixscript_create_error_string(heap, buf);
      }
      else {
         error = fixscript_create_error_string(heap, "I/O error");
      }
   }
   else {
      ret = fixscript_create_string(heap, orig_path, -1);
      free(orig_path);
      if (!ret.value) {
         fixscript_error(heap, &error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
   }

   free(cont->path);

   cont_func = cont->cont_func;
   cont_data = cont->cont_data;
   free(cont);

   cont_func(heap, ret, error, cont_data);
}
#endif /* __wasm__ */


static Value native_path_get_symlink(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(_WIN32)
   return fixscript_int(0);
#else
   char *path = NULL, *link = NULL;
   Value retval = fixscript_int(0);
   char buf[256];
   int err, len = 0;
#ifdef __wasm__
   PathGetSymlinkCont *cont;
#else
   int new_len;
#endif
   
   err = fixscript_get_string(heap, params[0], 0, -1, &path, NULL);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

#ifdef __wasm__
   if (wasm_path_resolve_symlink_sync(path, &link, &err)) {
      if (err) {
         if (err == WASM_ERROR_ACCESS_DENIED) {
            snprintf(buf, sizeof(buf), "access denied to '%s'", path);
            *error = fixscript_create_error_string(heap, buf);
         }
         else if (err == WASM_ERROR_FILE_NOT_FOUND) {
            snprintf(buf, sizeof(buf), "path '%s' not found", path);
            *error = fixscript_create_error_string(heap, buf);
         }
         else {
            *error = fixscript_create_error_string(heap, "I/O error");
         }
         goto error;
      }
      if (link) {
         len = strlen(link);
      }
      else {
         len = 0;
      }
   }
   else {
      cont = malloc(sizeof(PathGetSymlinkCont));
      cont->heap = heap;
      cont->path = path;
      fixscript_suspend(heap, &cont->cont_func, &cont->cont_data);
      wasm_path_resolve_symlink(path, path_get_symlink_cont, cont);
      return fixscript_int(0);
   }
#else
   len = 1;
again:
   link = malloc(len);
   if (!link) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }
   new_len = readlink(path, link, len);
   if (new_len == len) {
      free(link);
      len *= 2;
      goto again;
   }
   len = new_len;

   if (len < 0) {
      if (errno == EACCES) {
         snprintf(buf, sizeof(buf), "access denied to '%s'", path);
         *error = fixscript_create_error_string(heap, buf);
      }
      else if (errno == ENOENT || errno == ENOTDIR) {
         snprintf(buf, sizeof(buf), "path '%s' not found", path);
         *error = fixscript_create_error_string(heap, buf);
      }
      else if (errno == EINVAL) {
         goto error;
      }
      else {
         *error = fixscript_create_error_string(heap, "I/O error");
      }
      goto error;
   }
#endif

   retval = fixscript_create_string(heap, link, len);
   if (!retval.value) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

error:
   free(path);
   free(link);
   return retval;
#endif
}


#ifdef __wasm__
typedef struct {
   Heap *heap;
   char *path;
   ContinuationResultFunc cont_func;
   void *cont_data;
} PathCreateDirectoryCont;

void path_create_directory_cont(int wasm_error, void *data)
{
   PathCreateDirectoryCont *cont = data;
   Heap *heap = cont->heap;
   Value error = fixscript_int(0);
   ContinuationResultFunc cont_func;
   void *cont_data;
   char buf[256];
      
   if (wasm_error == WASM_ERROR_NOT_SUPPORTED) {
      error = fixscript_create_error_string(heap, "not supported");
   }
   else if (wasm_error) {
      snprintf(buf, sizeof(buf), "can't create directory '%s'", cont->path);
      error = fixscript_create_error_string(heap, buf);
   }
   free(cont->path);

   cont_func = cont->cont_func;
   cont_data = cont->cont_data;
   free(cont);

   cont_func(heap, fixscript_int(0), error, cont_data);
}
#endif /* __wasm__ */


static Value native_path_create_directory(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#ifdef _WIN32
   uint16_t *path;
   uint16_t buf[256];
   int err;
   
   err = fixscript_get_string_utf16(heap, params[0], 0, -1, &path, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   if (!CreateDirectory(path, NULL)) {
      snwprintf(buf, sizeof(buf)/sizeof(uint16_t)-1, L"can't create directory '%s'", path);
      *error = fixscript_create_error(heap, fixscript_create_string_utf16(heap, buf, -1));
   }
   free(path);
   return fixscript_int(0);
#else
   char *path;
   char buf[256];
   int err;
#if defined(__wasm__)
   PathCreateDirectoryCont *cont;
#endif
   
   err = fixscript_get_string(heap, params[0], 0, -1, &path, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }

#if defined(__wasm__)
   if (wasm_path_create_directory_sync(path, &err)) {
      if (err == WASM_ERROR_NOT_SUPPORTED) {
         *error = fixscript_create_error_string(heap, "not supported");
      }
      else if (err) {
         snprintf(buf, sizeof(buf), "can't create directory '%s'", path);
         *error = fixscript_create_error_string(heap, buf);
      }
   }
   else {
      cont = malloc(sizeof(PathCreateDirectoryCont));
      cont->heap = heap;
      cont->path = path;
      fixscript_suspend(heap, &cont->cont_func, &cont->cont_data);
      wasm_path_create_directory(path, path_create_directory_cont, cont);
      return fixscript_int(0);
   }
#else
   if (mkdir(path, 0777) != 0) {
      snprintf(buf, sizeof(buf), "can't create directory '%s'", path);
      *error = fixscript_create_error_string(heap, buf);
   }
#endif

   free(path);
   return fixscript_int(0);
#endif
}


#ifdef __wasm__
typedef struct {
   Heap *heap;
   char *path;
   ContinuationResultFunc cont_func;
   void *cont_data;
} PathDeleteFileCont;

void path_delete_file_cont(int wasm_error, void *data)
{
   PathDeleteFileCont *cont = data;
   Heap *heap = cont->heap;
   Value error = fixscript_int(0);
   ContinuationResultFunc cont_func;
   void *cont_data;
   char buf[256];
      
   if (wasm_error == WASM_ERROR_NOT_SUPPORTED) {
      error = fixscript_create_error_string(heap, "not supported");
   }
   else if (wasm_error) {
      snprintf(buf, sizeof(buf), "can't delete file '%s'", cont->path);
      error = fixscript_create_error_string(heap, buf);
   }
   free(cont->path);

   cont_func = cont->cont_func;
   cont_data = cont->cont_data;
   free(cont);

   cont_func(heap, fixscript_int(0), error, cont_data);
}
#endif /* __wasm__ */


static Value native_path_delete_file(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#ifdef _WIN32
   uint16_t *path;
   uint16_t buf[256];
   int err;
   
   err = fixscript_get_string_utf16(heap, params[0], 0, -1, &path, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   if (!DeleteFile(path)) {
      snwprintf(buf, sizeof(buf)/sizeof(uint16_t)-1, L"can't delete file '%s'", path);
      *error = fixscript_create_error(heap, fixscript_create_string_utf16(heap, buf, -1));
   }
   free(path);
   return fixscript_int(0);
#else
   char *path;
   char buf[256];
   int err;
#if defined(__wasm__)
   PathDeleteFileCont *cont;
#endif
   
   err = fixscript_get_string(heap, params[0], 0, -1, &path, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }

#if defined(__wasm__)
   if (wasm_path_delete_file_sync(path, &err)) {
      if (err == WASM_ERROR_NOT_SUPPORTED) {
         *error = fixscript_create_error_string(heap, "not supported");
      }
      else if (err) {
         snprintf(buf, sizeof(buf), "can't delete file '%s'", path);
         *error = fixscript_create_error_string(heap, buf);
      }
   }
   else {
      cont = malloc(sizeof(PathDeleteFileCont));
      cont->heap = heap;
      cont->path = path;
      fixscript_suspend(heap, &cont->cont_func, &cont->cont_data);
      wasm_path_delete_file(path, path_delete_file_cont, cont);
      return fixscript_int(0);
   }
#else
   if (unlink(path) != 0) {
      snprintf(buf, sizeof(buf), "can't delete file '%s'", path);
      *error = fixscript_create_error_string(heap, buf);
   }
#endif

   free(path);
   return fixscript_int(0);
#endif
}


#ifdef __wasm__
typedef struct {
   Heap *heap;
   char *path;
   ContinuationResultFunc cont_func;
   void *cont_data;
} PathDeleteDirectoryCont;

void path_delete_directory_cont(int wasm_error, void *data)
{
   PathDeleteDirectoryCont *cont = data;
   Heap *heap = cont->heap;
   Value error = fixscript_int(0);
   ContinuationResultFunc cont_func;
   void *cont_data;
   char buf[256];
      
   if (wasm_error == WASM_ERROR_NOT_SUPPORTED) {
      error = fixscript_create_error_string(heap, "not supported");
   }
   else if (wasm_error) {
      snprintf(buf, sizeof(buf), "can't delete directory '%s'", cont->path);
      error = fixscript_create_error_string(heap, buf);
   }
   free(cont->path);

   cont_func = cont->cont_func;
   cont_data = cont->cont_data;
   free(cont);

   cont_func(heap, fixscript_int(0), error, cont_data);
}
#endif /* __wasm__ */


static Value native_path_delete_directory(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#ifdef _WIN32
   uint16_t *path;
   uint16_t buf[256];
   int err;
   
   err = fixscript_get_string_utf16(heap, params[0], 0, -1, &path, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   if (!RemoveDirectory(path)) {
      snwprintf(buf, sizeof(buf)/sizeof(uint16_t)-1, L"can't delete directory '%s'", path);
      *error = fixscript_create_error(heap, fixscript_create_string_utf16(heap, buf, -1));
   }
   free(path);
   return fixscript_int(0);
#else
   char *path;
   char buf[256];
   int err;
#if defined(__wasm__)
   PathDeleteDirectoryCont *cont;
#endif
   
   err = fixscript_get_string(heap, params[0], 0, -1, &path, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }

#if defined(__wasm__)
   if (wasm_path_delete_directory_sync(path, &err)) {
      if (err == WASM_ERROR_NOT_SUPPORTED) {
         *error = fixscript_create_error_string(heap, "not supported");
      }
      else if (err) {
         snprintf(buf, sizeof(buf), "can't delete directory '%s'", path);
         *error = fixscript_create_error_string(heap, buf);
      }
   }
   else {
      cont = malloc(sizeof(PathDeleteDirectoryCont));
      cont->heap = heap;
      cont->path = path;
      fixscript_suspend(heap, &cont->cont_func, &cont->cont_data);
      wasm_path_delete_directory(path, path_delete_directory_cont, cont);
      return fixscript_int(0);
   }
#else
   if (rmdir(path) != 0) {
      snprintf(buf, sizeof(buf), "can't delete directory '%s'", path);
      *error = fixscript_create_error_string(heap, buf);
   }
#endif

   free(path);
   return fixscript_int(0);
#endif
}


static void *file_handle_func(Heap *heap, int op, void *p1, void *p2)
{
   FileHandle *handle = p1;
   switch (op) {
      case HANDLE_OP_FREE:
         if (__sync_sub_and_fetch(&handle->refcnt, 1) == 0) {
            if (!handle->closed) {
               #if defined(_WIN32)
                  CloseHandle(handle->handle);
               #elif defined(__wasm__)
                  wasm_file_close_sync(handle->file, NULL);
               #else
                  close(handle->fd);
               #endif
            }
            free(handle);
         }
         break;

      case HANDLE_OP_COPY:
         __sync_add_and_fetch(&handle->refcnt, 1);
         return handle;
   }
   return NULL;
}


#ifdef __wasm__
typedef struct {
   Heap *heap;
   char *fname;
   FileHandle *handle;
   ContinuationResultFunc cont_func;
   void *cont_data;
} FileOpenCont;

static void file_open_cont(wasm_file_t *file, int file_error, void *data)
{
   FileOpenCont *cont = data;
   Heap *heap = cont->heap;
   Value ret = fixscript_int(0), error = fixscript_int(0);
   ContinuationResultFunc cont_func;
   void *cont_data;
   char *s;
   int len;

   if (file_error == WASM_ERROR_NOT_SUPPORTED) {
      error = fixscript_create_error_string(heap, "not supported");
      goto error;
   }
   if (file_error) {
      len = strlen(cont->fname);
      if (len > INT_MAX-64) {
         fixscript_error(heap, &error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         goto error;
      }
      len += 64;
      s = malloc(len);
      if (!s) {
         fixscript_error(heap, &error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         goto error;
      }
      snprintf(s, len, "can't open file '%s'", cont->fname);
      error = fixscript_create_error_string(heap, s);
      free(s);
      goto error;
   }
   else {
      cont->handle->file = file;
   }

   ret = fixscript_create_value_handle(heap, HANDLE_TYPE_FILE, cont->handle, file_handle_func);
   cont->handle = NULL;
   if (!ret.value) {
      fixscript_error(heap, &error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

error:
   free(cont->fname);
   if (cont->handle && cont->handle->file) {
      wasm_file_close_sync(cont->handle->file, NULL);
   }
   free(cont->handle);
   cont_func = cont->cont_func;
   cont_data = cont->cont_data;
   free(cont);

   cont_func(heap, ret, error, cont_data);
}
#endif /* __wasm__ */


static Value native_file_open(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   FileHandle *handle = NULL;
   Value retval = fixscript_int(0);
#if defined(_WIN32)
   uint16_t *fname_utf16 = NULL;
   DWORD creation;
#elif defined(__wasm__)
   FileOpenCont *cont;
#endif
   char *fname = NULL, *s;
   int mode = fixscript_get_int(params[1]);
   size_t len;
   int err, flags;

   if ((mode & (SCRIPT_FILE_READ | SCRIPT_FILE_WRITE)) == 0) {
      *error = fixscript_create_error_string(heap, "file must be opened for reading and/or writing");
      goto error;
   }

   if ((mode & (SCRIPT_FILE_CREATE | SCRIPT_FILE_TRUNCATE | SCRIPT_FILE_APPEND)) && (mode & SCRIPT_FILE_WRITE) == 0) {
      *error = fixscript_create_error_string(heap, "file must be opened for writing when create, truncate and/or append mode is requested");
      goto error;
   }

   if ((mode & SCRIPT_FILE_APPEND) && (mode & SCRIPT_FILE_READ)) {
      *error = fixscript_create_error_string(heap, "file must be opened in write only mode when appending");
      goto error;
   }

#if defined(_WIN32)
   err = fixscript_get_string_utf16(heap, params[0], 0, -1, &fname_utf16, NULL);
#else
   err = 0;
#endif
   if (!err) {
      err = fixscript_get_string(heap, params[0], 0, -1, &fname, NULL);
   }
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   handle = calloc(1, sizeof(FileHandle));
   if (!handle) goto error;
   handle->refcnt = 1;
   handle->mode = mode;

#if defined(_WIN32)
   flags = 0;
   if (mode & SCRIPT_FILE_APPEND) {
      flags = FILE_APPEND_DATA;
   }
   else {
      if (mode & SCRIPT_FILE_READ) flags |= GENERIC_READ;
      if (mode & SCRIPT_FILE_WRITE) flags |= GENERIC_WRITE;
   }
   switch (mode & (SCRIPT_FILE_CREATE | SCRIPT_FILE_TRUNCATE)) {
      case 0:                                         creation = OPEN_EXISTING; break;
      case SCRIPT_FILE_CREATE:                        creation = OPEN_ALWAYS; break;
      case SCRIPT_FILE_TRUNCATE:                      creation = TRUNCATE_EXISTING; break;
      case SCRIPT_FILE_CREATE | SCRIPT_FILE_TRUNCATE: creation = CREATE_ALWAYS; break;
   }
   handle->handle = CreateFile(fname_utf16, flags, FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, creation, FILE_ATTRIBUTE_NORMAL, NULL);
   if (handle->handle == INVALID_HANDLE_VALUE) {
      len = strlen(fname);
      if (len > INT_MAX-64) {
         fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         goto error;
      }
      len += 64;
      s = malloc(len);
      if (!s) {
         fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         goto error;
      }
      snprintf(s, len, "can't open file '%s'", fname);
      *error = fixscript_create_error_string(heap, s);
      free(s);
      goto error;
   }
#elif defined(__wasm__)
   flags = 0;
   if (mode & SCRIPT_FILE_READ) {
      flags |= WASM_FILE_READ;
   }
   if (mode & SCRIPT_FILE_WRITE) {
      flags |= WASM_FILE_WRITE;
   }
   if (mode & SCRIPT_FILE_CREATE) {
      flags |= WASM_FILE_CREATE;
   }
   if (mode & SCRIPT_FILE_TRUNCATE) {
      flags |= WASM_FILE_TRUNCATE;
   }
   if (mode & SCRIPT_FILE_APPEND) {
      flags |= WASM_FILE_APPEND;
   }

   if (wasm_file_open_sync(fname, flags, &handle->file, &err)) {
      if (err == WASM_ERROR_NOT_SUPPORTED) {
         *error = fixscript_create_error_string(heap, "not supported");
         goto error;
      }
      if (err) {
         len = strlen(fname);
         if (len > INT_MAX-64) {
            fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
            goto error;
         }
         len += 64;
         s = malloc(len);
         if (!s) {
            fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
            goto error;
         }
         snprintf(s, len, "can't open file '%s'", fname);
         *error = fixscript_create_error_string(heap, s);
         free(s);
         goto error;
      }
   }
   else {
      cont = malloc(sizeof(FileOpenCont));
      cont->heap = heap;
      cont->fname = fname;
      cont->handle = handle;
      fixscript_suspend(heap, &cont->cont_func, &cont->cont_data);
      wasm_file_open(fname, flags, file_open_cont, cont);
      fname = NULL;
      handle = NULL;
      goto error;
   }
#else
   switch (mode & (SCRIPT_FILE_READ | SCRIPT_FILE_WRITE)) {
      case SCRIPT_FILE_READ: flags = O_RDONLY; break;
      case SCRIPT_FILE_WRITE: flags = O_WRONLY; break;
      default: flags = O_RDWR; break;
   }
   if (mode & SCRIPT_FILE_CREATE) {
      flags |= O_CREAT;
   }
   if (mode & SCRIPT_FILE_TRUNCATE) {
      flags |= O_TRUNC;
   }
   if (mode & SCRIPT_FILE_APPEND) {
      flags |= O_APPEND;
   }
   
   handle->fd = open(fname, flags, 0666);
   if (handle->fd == -1) {
      len = strlen(fname);
      if (len > INT_MAX-64) {
         fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         goto error;
      }
      len += 64;
      s = malloc(len);
      if (!s) {
         fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         goto error;
      }
      snprintf(s, len, "can't open file '%s'", fname);
      *error = fixscript_create_error_string(heap, s);
      free(s);
      goto error;
   }
#endif

   retval = fixscript_create_value_handle(heap, HANDLE_TYPE_FILE, handle, file_handle_func);
   handle = NULL;
   if (!retval.value) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

error:
   free(fname);
#if defined(_WIN32)
   free(fname_utf16);
   if (handle && handle->handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle->handle);
   }
#elif defined(__wasm__)
   if (handle && handle->file) {
      wasm_file_close_sync(handle->file, NULL);
   }
#else
   if (handle && handle->fd != -1) {
      close(handle->fd);
   }
#endif
   free(handle);
   return retval;
}


static FileHandle *get_file_handle(Heap *heap, Value *error, Value handle_val)
{
   FileHandle *handle;

   handle = fixscript_get_handle(heap, handle_val, HANDLE_TYPE_FILE, NULL);
   if (!handle) {
      *error = fixscript_create_error_string(heap, "invalid file handle");
      return NULL;
   }

   if (handle->closed) {
      *error = fixscript_create_error_string(heap, "file is already closed");
      return NULL;
   }

   return handle;
}


#ifdef __wasm__
typedef struct {
   Heap *heap;
   ContinuationResultFunc cont_func;
   void *cont_data;
} FileCloseCont;

void file_close_cont(int file_error, void *data)
{
   FileCloseCont *cont = data;
   Heap *heap = cont->heap;
   Value error = fixscript_int(0);
   ContinuationResultFunc cont_func;
   void *cont_data;
      
   if (file_error) {
      error = fixscript_create_error_string(heap, "I/O error");
   }

   cont_func = cont->cont_func;
   cont_data = cont->cont_data;
   free(cont);

   cont_func(heap, fixscript_int(0), error, cont_data);
}
#endif /* __wasm__ */


static Value native_file_close(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   FileHandle *handle;
#ifdef __wasm__
   FileCloseCont *cont;
   int err;
#endif

   handle = get_file_handle(heap, error, params[0]);
   if (!handle) {
      return fixscript_int(0);
   }

   handle->closed = 1;
#if defined(_WIN32)
   if (!CloseHandle(handle->handle)) {
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }
#elif defined(__wasm__)
   if (wasm_file_close_sync(handle->file, &err)) {
      if (err) {
         *error = fixscript_create_error_string(heap, "I/O error");
         return fixscript_int(0);
      }
   }
   else {
      cont = malloc(sizeof(FileCloseCont));
      cont->heap = heap;
      fixscript_suspend(heap, &cont->cont_func, &cont->cont_data);
      wasm_file_close(handle->file, file_close_cont, cont);
      return fixscript_int(0);
   }
#else
   if (close(handle->fd) == -1) {
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }
#endif
   return fixscript_int(0);
}


#ifdef __wasm__
typedef struct {
   Heap *heap;
   Value param1, param2;
   char *buf;
   int len;
   ContinuationResultFunc cont_func;
   void *cont_data;
} FileReadCont;

void file_read_cont(int read_bytes, int file_error, void *data)
{
   FileReadCont *cont = data;
   Heap *heap = cont->heap;
   Value ret = fixscript_int(0), error = fixscript_int(0);
   ContinuationResultFunc cont_func;
   void *cont_data;

   if (!file_error && read_bytes < cont->len) {
      cont->len = read_bytes;
   }

   fixscript_unlock_array(heap, cont->param1, cont->param2.value, cont->len, (void **)&cont->buf, 1, ACCESS_WRITE_ONLY);

   if (file_error) {
      error = fixscript_create_error_string(heap, "I/O error");
   }
   else {
      if (read_bytes == 0) {
         ret = fixscript_int(-1);
      }
      else {
         ret = fixscript_int(read_bytes);
      }
   }

   cont_func = cont->cont_func;
   cont_data = cont->cont_data;
   free(cont);

   cont_func(heap, ret, error, cont_data);
}
#endif /* __wasm__ */


static Value native_file_read(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   FileHandle *handle;
   char *buf;
#if defined(_WIN32)
   DWORD ret;
#elif defined(__wasm__)
   FileReadCont *cont;
   int ret = 0;
#else
   ssize_t ret;
#endif
   int err, len;

   handle = get_file_handle(heap, error, params[0]);
   if (!handle) {
      return fixscript_int(0);
   }

   if ((handle->mode & SCRIPT_FILE_READ) == 0) {
      *error = fixscript_create_error_string(heap, "file not opened for reading");
      return fixscript_int(0);
   }

   len = params[3].value;
   err = fixscript_lock_array(heap, params[1], params[2].value, len, (void **)&buf, 1, ACCESS_WRITE_ONLY);
   if (err) {
      fixscript_error(heap, error, err);
      return fixscript_int(0);
   }

#if defined(_WIN32)
   if (!ReadFile(handle->handle, buf, params[3].value, &ret, NULL)) {
      ret = -1;
   }
   else if (ret < len) {
      len = ret;
   }
#elif defined(__wasm__)
   if (wasm_file_read_sync(handle->file, buf, params[3].value, &ret, &err)) {
      if (err) {
         ret = -1;
      }
      else if (ret < len) {
         len = ret;
      }
   }
   else {
      cont = malloc(sizeof(FileReadCont));
      cont->heap = heap;
      cont->param1 = params[1];
      cont->param2 = params[2];
      cont->buf = buf;
      cont->len = len;
      fixscript_suspend(heap, &cont->cont_func, &cont->cont_data);
      wasm_file_read(handle->file, buf, params[3].value, file_read_cont, cont);
      return fixscript_int(0);
   }
#else
   ret = read(handle->fd, buf, params[3].value);
   if (ret < 0) {
      len = 0;
   }
   else if (ret < len) {
      len = ret;
   }
#endif

   fixscript_unlock_array(heap, params[1], params[2].value, len, (void **)&buf, 1, ACCESS_WRITE_ONLY);

   if (ret == 0) {
      return fixscript_int(-1);
   }
   if (ret < 0) {
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }
   return fixscript_int((int)ret);
}


#ifdef __wasm__
typedef struct {
   Heap *heap;
   Value param1, param2, param3;
   char *buf;
   ContinuationResultFunc cont_func;
   void *cont_data;
} FileWriteCont;

void file_write_cont(int written_bytes, int file_error, void *data)
{
   FileWriteCont *cont = data;
   Heap *heap = cont->heap;
   Value ret = fixscript_int(0), error = fixscript_int(0);
   ContinuationResultFunc cont_func;
   void *cont_data;

   fixscript_unlock_array(heap, cont->param1, cont->param2.value, cont->param3.value, (void **)&cont->buf, 1, ACCESS_READ_ONLY);

   if (file_error) {
      error = fixscript_create_error_string(heap, "I/O error");
   }
   else {
      ret = fixscript_int(written_bytes);
   }

   cont_func = cont->cont_func;
   cont_data = cont->cont_data;
   free(cont);

   cont_func(heap, ret, error, cont_data);
}
#endif /* __wasm__ */


static Value native_file_write(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   FileHandle *handle;
   char *buf;
#if defined(_WIN32)
   DWORD ret;
#elif defined(__wasm__)
   FileWriteCont *cont;
   int ret;
#else
   ssize_t ret = 0;
#endif
   int err;

   handle = get_file_handle(heap, error, params[0]);
   if (!handle) {
      return fixscript_int(0);
   }

   if ((handle->mode & SCRIPT_FILE_WRITE) == 0) {
      *error = fixscript_create_error_string(heap, "file not opened for writing");
      return fixscript_int(0);
   }

   err = fixscript_lock_array(heap, params[1], params[2].value, params[3].value, (void **)&buf, 1, ACCESS_READ_ONLY);
   if (err) {
      fixscript_error(heap, error, err);
      return fixscript_int(0);
   }

#if defined(_WIN32)
   if (!WriteFile(handle->handle, buf, params[3].value, &ret, NULL)) {
      ret = -1;
   }
#elif defined(__wasm__)
   if (wasm_file_write_sync(handle->file, buf, params[3].value, &ret, &err)) {
      if (err) {
         ret = -1;
      }
   }
   else {
      cont = malloc(sizeof(FileWriteCont));
      cont->heap = heap;
      cont->param1 = params[1];
      cont->param2 = params[2];
      cont->param3 = params[3];
      cont->buf = buf;
      fixscript_suspend(heap, &cont->cont_func, &cont->cont_data);
      wasm_file_write(handle->file, buf, params[3].value, file_write_cont, cont);
      return fixscript_int(0);
   }
#else
   ret = write(handle->fd, buf, params[3].value);
   if (ret < 0 && errno == EAGAIN) {
      ret = 0;
   }
#endif

   fixscript_unlock_array(heap, params[1], params[2].value, params[3].value, (void **)&buf, 1, ACCESS_READ_ONLY);

   if (ret < 0) {
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }
   return fixscript_int(ret);
}


#ifdef __wasm__
typedef struct {
   Heap *heap;
   ContinuationResultFunc cont_func;
   void *cont_data;
} FileGetLengthCont;

void file_get_length_cont(int64_t length, int file_error, void *data)
{
   FileGetLengthCont *cont = data;
   Heap *heap = cont->heap;
   Value ret = fixscript_int(0), error = fixscript_int(0);
   ContinuationResultFunc cont_func;
   void *cont_data;

   if (file_error) {
      error = fixscript_create_error_string(heap, "I/O error");
   }
   else {
      error = fixscript_int(length >> 32);
      ret = fixscript_int(length);
   }

   cont_func = cont->cont_func;
   cont_data = cont->cont_data;
   free(cont);

   cont_func(heap, ret, error, cont_data);
}
#endif /* __wasm__ */


static Value native_file_get_length(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   FileHandle *handle;
#if defined(_WIN32)
   LARGE_INTEGER size;
#elif defined(__wasm__)
   FileGetLengthCont *cont;
   int64_t length;
   int err;
#else
   off_t cur_pos, end_pos;
#endif

   handle = get_file_handle(heap, error, params[0]);
   if (!handle) {
      return fixscript_int(0);
   }

   if (handle->mode & SCRIPT_FILE_APPEND) {
      *error = fixscript_create_error_string(heap, "not supported in append mode");
      return fixscript_int(0);
   }

#if defined(_WIN32)
   if (!GetFileSizeEx(handle->handle, &size)) {
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }

   *error = fixscript_int(size.HighPart);
   return fixscript_int(size.LowPart);
#elif defined(__wasm__)
   if (wasm_file_get_length_sync(handle->file, &length, &err)) {
      if (err) {
         *error = fixscript_create_error_string(heap, "I/O error");
         return fixscript_int(0);
      }
      *error = fixscript_int(length >> 32);
      return fixscript_int(length);
   }
   else {
      cont = malloc(sizeof(FileGetLengthCont));
      cont->heap = heap;
      fixscript_suspend(heap, &cont->cont_func, &cont->cont_data);
      wasm_file_get_length(handle->file, file_get_length_cont, cont);
      return fixscript_int(0);
   }
#else
   cur_pos = lseek(handle->fd, 0, SEEK_CUR);
   if (cur_pos < 0) {
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }

   end_pos = lseek(handle->fd, 0, SEEK_END);
   if (end_pos < 0) {
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }

   if (lseek(handle->fd, cur_pos, SEEK_SET) < 0) {
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }

   *error = fixscript_int(((int64_t)end_pos) >> 32);
   return fixscript_int((int64_t)end_pos);
#endif
}


#ifdef __wasm__
typedef struct {
   Heap *heap;
   ContinuationResultFunc cont_func;
   void *cont_data;
} FileSetLengthCont;

void file_set_length_cont(int file_error, void *data)
{
   FileSetLengthCont *cont = data;
   Heap *heap = cont->heap;
   Value error = fixscript_int(0);
   ContinuationResultFunc cont_func;
   void *cont_data;

   if (file_error) {
      error = fixscript_create_error_string(heap, "I/O error");
   }

   cont_func = cont->cont_func;
   cont_data = cont->cont_data;
   free(cont);

   cont_func(heap, fixscript_int(0), error, cont_data);
}
#endif /* __wasm__ */


static Value native_file_set_length(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   FileHandle *handle;
#if defined(_WIN32)
   LARGE_INTEGER pos, old_pos;
#elif defined(__wasm__)
   FileSetLengthCont *cont;
   int err;
#else
   off_t pos;
#endif

   handle = get_file_handle(heap, error, params[0]);
   if (!handle) {
      return fixscript_int(0);
   }

   if (handle->mode & SCRIPT_FILE_APPEND) {
      *error = fixscript_create_error_string(heap, "not supported in append mode");
      return fixscript_int(0);
   }

#if defined(_WIN32)
   pos.LowPart = 0;
   pos.HighPart = 0;
   if (!SetFilePointerEx(handle->handle, pos, &old_pos, FILE_CURRENT)) {
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }
   pos.LowPart = fixscript_get_int(params[1]);
   pos.HighPart = fixscript_get_int(params[2]);
   if (!SetFilePointerEx(handle->handle, pos, NULL, FILE_BEGIN)) {
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }
   if (!SetEndOfFile(handle->handle)) {
      SetFilePointerEx(handle->handle, old_pos, NULL, FILE_BEGIN);
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }
   if (!SetFilePointerEx(handle->handle, old_pos, NULL, FILE_BEGIN)) {
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }
#elif defined(__wasm__)
   if (wasm_file_set_length_sync(handle->file, ((uint32_t)params[1].value) | (((uint64_t)params[2].value) << 32), &err)) {
      if (err) {
         *error = fixscript_create_error_string(heap, "I/O error");
         return fixscript_int(0);
      }
   }
   else {
      cont = malloc(sizeof(FileSetLengthCont));
      cont->heap = heap;
      fixscript_suspend(heap, &cont->cont_func, &cont->cont_data);
      wasm_file_set_length(handle->file, ((uint32_t)params[1].value) | (((uint64_t)params[2].value) << 32), file_set_length_cont, cont);
      return fixscript_int(0);
   }
#else
   pos = ((uint32_t)params[1].value) | (((uint64_t)params[2].value) << 32);

   if (ftruncate(handle->fd, pos) < 0) {
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }
#endif
   
   return fixscript_int(0);
}


#ifdef __wasm__
typedef struct {
   Heap *heap;
   ContinuationResultFunc cont_func;
   void *cont_data;
} FileGetPositionCont;

void file_get_position_cont(int64_t position, int file_error, void *data)
{
   FileGetPositionCont *cont = data;
   Heap *heap = cont->heap;
   Value ret = fixscript_int(0), error = fixscript_int(0);
   ContinuationResultFunc cont_func;
   void *cont_data;

   if (file_error) {
      error = fixscript_create_error_string(heap, "I/O error");
   }
   else {
      error = fixscript_int(position >> 32);
      ret = fixscript_int(position);
   }

   cont_func = cont->cont_func;
   cont_data = cont->cont_data;
   free(cont);

   cont_func(heap, ret, error, cont_data);
}
#endif /* __wasm__ */


static Value native_file_get_position(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   FileHandle *handle;
#if defined(_WIN32)
   LARGE_INTEGER zero, pos;
#elif defined(__wasm__)
   FileGetPositionCont *cont;
   int64_t position;
   int err;
#else
   off_t pos;
#endif

   handle = get_file_handle(heap, error, params[0]);
   if (!handle) {
      return fixscript_int(0);
   }

   if (handle->mode & SCRIPT_FILE_APPEND) {
      *error = fixscript_create_error_string(heap, "not supported in append mode");
      return fixscript_int(0);
   }

#if defined(_WIN32)
   zero.LowPart = 0;
   zero.HighPart = 0;
   if (!SetFilePointerEx(handle->handle, zero, &pos, FILE_CURRENT)) {
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }

   *error = fixscript_int(pos.HighPart);
   return fixscript_int(pos.LowPart);
#elif defined(__wasm__)
   if (wasm_file_get_position_sync(handle->file, &position, &err)) {
      if (err) {
         *error = fixscript_create_error_string(heap, "I/O error");
         return fixscript_int(0);
      }
      *error = fixscript_int(position >> 32);
      return fixscript_int(position);
   }
   else {
      cont = malloc(sizeof(FileGetPositionCont));
      cont->heap = heap;
      fixscript_suspend(heap, &cont->cont_func, &cont->cont_data);
      wasm_file_get_position(handle->file, file_get_position_cont, cont);
      return fixscript_int(0);
   }
#else
   pos = lseek(handle->fd, 0, SEEK_CUR);
   if (pos < 0) {
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }

   *error = fixscript_int(((int64_t)pos) >> 32);
   return fixscript_int((int64_t)pos);
#endif
}


#ifdef __wasm__
typedef struct {
   Heap *heap;
   ContinuationResultFunc cont_func;
   void *cont_data;
} FileSetPositionCont;

void file_set_position_cont(int file_error, void *data)
{
   FileSetPositionCont *cont = data;
   Heap *heap = cont->heap;
   Value error = fixscript_int(0);
   ContinuationResultFunc cont_func;
   void *cont_data;

   if (file_error) {
      error = fixscript_create_error_string(heap, "I/O error");
   }

   cont_func = cont->cont_func;
   cont_data = cont->cont_data;
   free(cont);

   cont_func(heap, fixscript_int(0), error, cont_data);
}
#endif /* __wasm__ */


static Value native_file_set_position(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   FileHandle *handle;
#if defined(_WIN32)
   LARGE_INTEGER pos;
#elif defined(__wasm__)
   FileSetPositionCont *cont;
   int err;
#else
   off_t pos;
#endif

   handle = get_file_handle(heap, error, params[0]);
   if (!handle) {
      return fixscript_int(0);
   }

   if (handle->mode & SCRIPT_FILE_APPEND) {
      *error = fixscript_create_error_string(heap, "not supported in append mode");
      return fixscript_int(0);
   }

#if defined(_WIN32)
   pos.LowPart = fixscript_get_int(params[1]);
   pos.HighPart = fixscript_get_int(params[2]);
   if (!SetFilePointerEx(handle->handle, pos, NULL, FILE_BEGIN)) {
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }
#elif defined(__wasm__)
   if (wasm_file_set_position_sync(handle->file, ((uint32_t)params[1].value) | (((uint64_t)params[2].value) << 32), &err)) {
      if (err) {
         *error = fixscript_create_error_string(heap, "I/O error");
         return fixscript_int(0);
      }
   }
   else {
      cont = malloc(sizeof(FileSetPositionCont));
      cont->heap = heap;
      fixscript_suspend(heap, &cont->cont_func, &cont->cont_data);
      wasm_file_set_position(handle->file, ((uint32_t)params[1].value) | (((uint64_t)params[2].value) << 32), file_set_position_cont, cont);
      return fixscript_int(0);
   }
#else
   pos = ((uint32_t)params[1].value) | (((uint64_t)params[2].value) << 32);

   if (lseek(handle->fd, pos, SEEK_SET) < 0) {
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }
#endif
   
   return fixscript_int(0);
}


#ifdef __wasm__
typedef struct {
   Heap *heap;
   ContinuationResultFunc cont_func;
   void *cont_data;
} FileSyncCont;

void file_sync_cont(int file_error, void *data)
{
   FileSyncCont *cont = data;
   Heap *heap = cont->heap;
   Value error = fixscript_int(0);
   ContinuationResultFunc cont_func;
   void *cont_data;

   if (file_error) {
      error = fixscript_create_error_string(heap, "I/O error");
   }

   cont_func = cont->cont_func;
   cont_data = cont->cont_data;
   free(cont);

   cont_func(heap, fixscript_int(0), error, cont_data);
}
#endif /* __wasm__ */


static Value native_file_sync(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   FileHandle *handle;
#ifdef __wasm__
   FileSyncCont *cont;
   int err;
#endif

   handle = get_file_handle(heap, error, params[0]);
   if (!handle) {
      return fixscript_int(0);
   }

#ifdef __wasm__
   if (wasm_file_sync_sync(handle->file, &err)) {
      if (err) {
         *error = fixscript_create_error_string(heap, "I/O error");
         return fixscript_int(0);
      }
   }
   else {
      cont = malloc(sizeof(FileSyncCont));
      cont->heap = heap;
      fixscript_suspend(heap, &cont->cont_func, &cont->cont_data);
      wasm_file_sync(handle->file, file_sync_cont, cont);
      return fixscript_int(0);
   }
#else
#if defined(_WIN32)
   if (!FlushFileBuffers(handle->handle)) {
#elif defined(__APPLE__)
   if (fcntl(handle->fd, F_FULLFSYNC) != 0) {
#elif defined(__linux__)
   if (fdatasync(handle->fd) != 0) {
#else
   if (fsync(handle->fd) != 0) {
#endif
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }
#endif /* __wasm__ */
   
   return fixscript_int(0);
}


#ifdef __wasm__
typedef struct {
   Heap *heap;
   FileHandle *handle;
   ContinuationResultFunc cont_func;
   void *cont_data;
} FileLockCont;

void file_lock_cont(int locked, int file_error, void *data)
{
   FileLockCont *cont = data;
   Heap *heap = cont->heap;
   Value ret = fixscript_int(0), error = fixscript_int(0);
   ContinuationResultFunc cont_func;
   void *cont_data;

   if (file_error) {
      error = fixscript_create_error_string(heap, "I/O error");
      __sync_val_compare_and_swap(&cont->handle->locked, 1, 0);
   }
   else if (locked) {
      ret = fixscript_int(1);
   }
   else {
      __sync_val_compare_and_swap(&cont->handle->locked, 1, 0);
   }

   cont_func = cont->cont_func;
   cont_data = cont->cont_data;
   free(cont);

   cont_func(heap, ret, error, cont_data);
}
#endif /* __wasm__ */


static Value native_monotonic_get_time(Heap *heap, Value *error, int num_params, Value *params, void *data);

static Value native_file_lock(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   FileHandle *handle;
   int exclusive, timeout;
#if defined(_WIN32)
   DWORD flags;
   OVERLAPPED overlapped;
#elif defined(__wasm__)
   FileLockCont *cont;
   int locked, err;
#else
   int ret, flags;
#endif
#ifndef __wasm__
   uint32_t start_time=0, cur_time;
#endif

   handle = get_file_handle(heap, error, params[0]);
   if (!handle) {
      return fixscript_int(0);
   }

   if (__sync_val_compare_and_swap(&handle->locked, 0, 1)) {
      *error = fixscript_create_error_string(heap, "file is already locked");
      return fixscript_int(0);
   }

   exclusive = params[1].value;
   timeout = params[2].value;

#if defined(_WIN32)
   flags = exclusive? LOCKFILE_EXCLUSIVE_LOCK : 0;
   if (timeout >= 0) {
      flags |= LOCKFILE_FAIL_IMMEDIATELY;
      if (timeout > 0) {
         start_time = native_monotonic_get_time(heap, error, 0, NULL, (void *)0).value;
      }
   }
   for (;;) {
      memset(&overlapped, 0, sizeof(OVERLAPPED));
      overlapped.Offset = 0xFFFFFFFE;
      overlapped.OffsetHigh = 0xFFFFFFFF;
      if (LockFileEx(handle->handle, flags, 0, 1, 0, &overlapped)) {
         return fixscript_int(1);
      }
      if (timeout < 0) {
         *error = fixscript_create_error_string(heap, "I/O error");
         break;
      }
      if (timeout == 0) break;
      if (timeout > 0) {
         cur_time = native_monotonic_get_time(heap, error, 0, NULL, (void *)0).value;
         if ((int32_t)(cur_time - start_time) > timeout) {
            break;
         }
         Sleep(1);
      }
   }
#elif defined(__wasm__)
   if (wasm_file_lock_sync(handle->file, exclusive, timeout, &locked, &err)) {
      if (err) {
         *error = fixscript_create_error_string(heap, "I/O error");
      }
      else if (locked) {
         return fixscript_int(1);
      }
   }
   else {
      cont = malloc(sizeof(FileLockCont));
      cont->heap = heap;
      cont->handle = handle;
      fixscript_suspend(heap, &cont->cont_func, &cont->cont_data);
      wasm_file_lock(handle->file, exclusive, timeout, file_lock_cont, cont);
      return fixscript_int(0);
   }
#else
   flags = exclusive? LOCK_EX : LOCK_SH;
   if (timeout >= 0) {
      flags |= LOCK_NB;
      if (timeout > 0) {
         start_time = native_monotonic_get_time(heap, error, 0, NULL, (void *)0).value;
      }
   }
   for (;;) {
      ret = flock(handle->fd, flags);
      if (ret == 0) {
         return fixscript_int(1);
      }
      if (errno == EINTR) {
         continue;
      }
      else if (errno == EWOULDBLOCK) {
         if (timeout == 0) {
            break;
         }
         if (timeout > 0) {
            cur_time = native_monotonic_get_time(heap, error, 0, NULL, (void *)0).value;
            if ((int32_t)(cur_time - start_time) > timeout) {
               break;
            }
            usleep(1000);
         }
      }
      else {
         *error = fixscript_create_error_string(heap, "I/O error");
         break;
      }
   }
#endif

   __sync_val_compare_and_swap(&handle->locked, 1, 0);
   return fixscript_int(0);
}


#ifdef __wasm__
typedef struct {
   Heap *heap;
   FileHandle *handle;
   ContinuationResultFunc cont_func;
   void *cont_data;
} FileUnlockCont;

void file_unlock_cont(int file_error, void *data)
{
   FileUnlockCont *cont = data;
   Heap *heap = cont->heap;
   Value error = fixscript_int(0);
   ContinuationResultFunc cont_func;
   void *cont_data;

   if (file_error) {
      error = fixscript_create_error_string(heap, "I/O error");
   }
   else {
      __sync_val_compare_and_swap(&cont->handle->locked, 1, 0);
   }

   cont_func = cont->cont_func;
   cont_data = cont->cont_data;
   free(cont);

   cont_func(heap, fixscript_int(0), error, cont_data);
}
#endif /* __wasm__ */


static Value native_file_unlock(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   FileHandle *handle;
#if defined(_WIN32)
   OVERLAPPED overlapped;
#elif defined(__wasm__)
   FileUnlockCont *cont;
   int err;
#else
   int ret;
#endif

   handle = get_file_handle(heap, error, params[0]);
   if (!handle) {
      return fixscript_int(0);
   }

   if (!handle->locked) {
      *error = fixscript_create_error_string(heap, "file is not locked");
      return fixscript_int(0);
   }

#if defined(_WIN32)
   memset(&overlapped, 0, sizeof(OVERLAPPED));
   overlapped.Offset = 0xFFFFFFFE;
   overlapped.OffsetHigh = 0xFFFFFFFF;
   if (!UnlockFileEx(handle->handle, 0, 1, 0, &overlapped)) {
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }
   __sync_val_compare_and_swap(&handle->locked, 1, 0);
#elif defined(__wasm__)
   if (wasm_file_unlock_sync(handle->file, &err)) {
      if (err) {
         *error = fixscript_create_error_string(heap, "I/O error");
      }
      else {
         __sync_val_compare_and_swap(&handle->locked, 1, 0);
      }
   }
   else {
      cont = malloc(sizeof(FileUnlockCont));
      cont->heap = heap;
      cont->handle = handle;
      fixscript_suspend(heap, &cont->cont_func, &cont->cont_data);
      wasm_file_unlock(handle->file, file_unlock_cont, cont);
      return fixscript_int(0);
   }
#else
   for (;;) {
      ret = flock(handle->fd, LOCK_UN);
      if (ret == 0) {
         __sync_val_compare_and_swap(&handle->locked, 1, 0);
         return fixscript_int(0);
      }
      if (errno == EINTR) {
         continue;
      }
      else {
         *error = fixscript_create_error_string(heap, "I/O error");
         return fixscript_int(0);
      }
   }
#endif
   
   return fixscript_int(0);
}


static Value native_file_get_native_descriptor(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   FileHandle *handle;

   handle = get_file_handle(heap, error, params[0]);
   if (!handle) {
      return fixscript_int(0);
   }

#if defined(_WIN32) || defined(__wasm__)
   *error = fixscript_create_error_string(heap, "not available on this platform");
   return fixscript_int(0);
#else
   return fixscript_int(handle->fd);
#endif
}


static Value native_file_get_native_handle(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   FileHandle *handle;
#if defined(_WIN32)
   uint64_t value;
#endif

   handle = get_file_handle(heap, error, params[0]);
   if (!handle) {
      return fixscript_int(0);
   }

#if !defined(_WIN32)
   *error = fixscript_create_error_string(heap, "not available on this platform");
   return fixscript_int(0);
#else
   value = (uintptr_t)handle->handle;
   *error = fixscript_int(value >> 32);
   return fixscript_int(value);
#endif
}


static Value native_file_exists(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   *error = fixscript_create_error_string(heap, "not implemented yet");
   return fixscript_int(0);
}


#ifndef __wasm__
static void *tcp_connection_handle_func(Heap *heap, int op, void *p1, void *p2)
{
   TCPConnectionHandle *handle = p1;
   switch (op) {
      case HANDLE_OP_FREE:
         if (__sync_sub_and_fetch(&handle->refcnt, 1) == 0) {
            if (!handle->closed) {
               #if defined(_WIN32)
                  closesocket(handle->socket);
               #elif defined(__wasm__)
               #else
                  close(handle->fd);
               #endif
            }
            free(handle);
         }
         break;

      case HANDLE_OP_COPY:
         __sync_add_and_fetch(&handle->refcnt, 1);
         return handle;
   }
   return NULL;
}
#endif


static TCPConnectionHandle *get_tcp_connection_handle(Heap *heap, Value *error, Value handle_val)
{
   TCPConnectionHandle *handle;

   handle = fixscript_get_handle(heap, handle_val, HANDLE_TYPE_TCP_CONNECTION, NULL);
   if (!handle) {
      *error = fixscript_create_error_string(heap, "invalid TCP connection handle");
      return NULL;
   }

   if (handle->closed) {
      *error = fixscript_create_error_string(heap, "TCP connection is already closed");
      return NULL;
   }

   return handle;
}


#ifndef __wasm__
static int update_nonblocking(TCPConnectionHandle *handle)
{
#if defined(_WIN32)
   u_long arg;
#elif defined(__wasm__)
#else
   int flags;
#endif

   if (handle->in_nonblocking != handle->want_nonblocking) {
      #if defined(_WIN32)
         arg = handle->want_nonblocking;
         if (ioctlsocket(handle->socket, FIONBIO, &arg) != 0) {
            return 0;
         }
      #elif defined(__wasm__)
      #else
         flags = fcntl(handle->fd, F_GETFL);
         if (flags == -1) {
            return 0;
         }
         if (handle->want_nonblocking) {
            flags |= O_NONBLOCK;
         }
         else {
            flags &= ~O_NONBLOCK;
         }
         if (fcntl(handle->fd, F_SETFL, flags) == -1) {
            return 0;
         }
      #endif
      handle->in_nonblocking = handle->want_nonblocking;
   }
   return 1;
}
#endif


#ifndef __wasm__
#if defined(_WIN32)
static int tcp_connect(const char *hostname, int port, SOCKET *ret, int overlapped)
#else
static int tcp_connect(const char *hostname, int port, int *ret)
#endif
{
#if defined(_WIN32)
   LPHOSTENT host_ent;
   SOCKADDR_IN sock_addr;
   SOCKET sock = INVALID_SOCKET;
#else
   struct addrinfo *addrinfo = NULL, *ai, hints;
   int fd = -1;
   char buf[16];
   int err;
#endif
   int flag;

#if defined(_WIN32)
   host_ent = gethostbyname(hostname);
   if (!host_ent || !host_ent->h_addr_list[0]) {
      goto error;
   }

   if (overlapped) {
      sock = WSASocket(PF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
   }
   else {
      sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
   }
   if (sock == INVALID_SOCKET) {
      goto error;
   }

   memset(&sock_addr, 0, sizeof(SOCKADDR_IN));
   sock_addr.sin_family = PF_INET;
   sock_addr.sin_addr = *((LPIN_ADDR)host_ent->h_addr_list[0]);
   sock_addr.sin_port = htons(port);

   if (connect(sock, (LPSOCKADDR)&sock_addr, sizeof(SOCKADDR_IN)) != 0) {
      goto error;
   }

   flag = 1;
   setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));

   *ret = sock;
#else
   memset(&hints, 0, sizeof(struct addrinfo));
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;

   snprintf(buf, sizeof(buf), "%d", port);
   err = getaddrinfo(hostname, buf, &hints, &addrinfo);
   if (err != 0) {
      goto error;
   }

   for (ai = addrinfo; ai; ai = ai->ai_next) {
      fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
      if (fd == -1) {
         continue;
      }

      if (connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
         close(fd);
         fd = -1;
         continue;
      }

      break;
   }

   if (fd == -1) {
      goto error;
   }

   flag = 1;
   setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));

   *ret = fd;
#endif

   return 1;

error:
#if defined(_WIN32)
   if (sock != INVALID_SOCKET) {
      closesocket(sock);
   }
#else
   if (addrinfo) {
      freeaddrinfo(addrinfo);
   }
   if (fd != -1) {
      close(fd);
   }
#endif
   return 0;
}
#endif /* __wasm__ */
 

static Value native_tcp_connection_open(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(__wasm__)
   *error = fixscript_create_error_string(heap, "not supported");
   return fixscript_int(0);
#else
   TCPConnectionHandle *handle;
   char *hostname = NULL;
   char buf[128];
   int err;
   Value retval = fixscript_int(0);
#if defined(_WIN32)
   SOCKET sock = INVALID_SOCKET;
#else
   int fd = -1;
#endif

   err = fixscript_get_string(heap, params[0], 0, -1, &hostname, NULL);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

#if defined(_WIN32)
   if (!tcp_connect(hostname, params[1].value, &sock, 0))
#else
   if (!tcp_connect(hostname, params[1].value, &fd))
#endif
   {
      snprintf(buf, sizeof(buf), "can't connect to %s:%d", hostname, params[1].value);
      *error = fixscript_create_error_string(heap, buf);
      goto error;
   }

   handle = calloc(1, sizeof(TCPConnectionHandle));
   if (!handle) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   handle->refcnt = 1;
#if defined(_WIN32)
   handle->socket = sock;
#else
   handle->fd = fd;
#endif

   retval = fixscript_create_value_handle(heap, HANDLE_TYPE_TCP_CONNECTION, handle, tcp_connection_handle_func);
#if defined(_WIN32)
   sock = INVALID_SOCKET;
#else
   fd = -1;
#endif
   if (!retval.value) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

error:
   free(hostname);
#if defined(_WIN32)
   if (!retval.value && sock != INVALID_SOCKET) {
      closesocket(sock);
   }
#else
   if (!retval.value && fd != -1) {
      close(fd);
   }
#endif
   return retval;
#endif /* __wasm__ */
}


static Value native_tcp_connection_close(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   TCPConnectionHandle *handle;

   handle = get_tcp_connection_handle(heap, error, params[0]);
   if (!handle) {
      return fixscript_int(0);
   }

   handle->closed = 1;
#if defined(_WIN32)
   if (closesocket(handle->socket) != 0) {
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }
#elif defined(__wasm__)
   *error = fixscript_create_error_string(heap, "not supported");
#else
   if (close(handle->fd) == -1) {
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }
#endif
   return fixscript_int(0);
}


static Value native_tcp_connection_read(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(__wasm__)
   *error = fixscript_create_error_string(heap, "not supported");
   return fixscript_int(0);
#else
   TCPConnectionHandle *handle;
   char *buf;
   int timeout = fixscript_get_int(params[4]);
#if defined(_WIN32)
   fd_set readfds;
   TIMEVAL timeval;
   int ret;
#else
   struct pollfd pfd;
   ssize_t ret;
#endif
   int err, len;

   handle = get_tcp_connection_handle(heap, error, params[0]);
   if (!handle) {
      return fixscript_int(0);
   }

#if defined(_WIN32)
   if (timeout >= 0) {
      FD_ZERO(&readfds);
      FD_SET(handle->socket, &readfds);
      timeval.tv_sec = timeout / 1000;
      timeval.tv_usec = (timeout % 1000) * 1000;
      ret = select(1, &readfds, NULL, NULL, &timeval);
      if (ret == SOCKET_ERROR) {
         *error = fixscript_create_error_string(heap, "I/O error");
         return fixscript_int(0);
      }
      if (ret == 0) {
         return fixscript_int(0);
      }
      handle->want_nonblocking = 1;
   }
#else
   if (timeout >= 0) {
      pfd.fd = handle->fd;
      pfd.events = POLLIN;
      ret = poll(&pfd, 1, timeout);
      if (ret < 0) {
         *error = fixscript_create_error_string(heap, "I/O error");
         return fixscript_int(0);
      }
      if (ret == 1) {
         ret = 0;
         if (pfd.revents & POLLIN) ret = 1;
      }
      if (ret == 0) {
         return fixscript_int(0);
      }
      handle->want_nonblocking = 1;
   }
#endif

   if (!update_nonblocking(handle)) {
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }
   handle->want_nonblocking = 0;

   len = params[3].value;
   err = fixscript_lock_array(heap, params[1], params[2].value, len, (void **)&buf, 1, ACCESS_WRITE_ONLY);
   if (err) {
      fixscript_error(heap, error, err);
      return fixscript_int(0);
   }

#if defined(_WIN32)
   ret = recv(handle->socket, buf, len, 0);
   if (ret == SOCKET_ERROR) {
      len = 0;
      ret = -1;
   }
   else if (ret < len) {
      len = ret;
   }
#else
   ret = read(handle->fd, buf, len);
   if (ret < 0) {
      len = 0;
   }
   else if (ret < len) {
      len = ret;
   }
#endif

   fixscript_unlock_array(heap, params[1], params[2].value, len, (void **)&buf, 1, ACCESS_WRITE_ONLY);

   if (ret == 0) {
      return fixscript_int(-1);
   }
   if (ret < 0) {
      #if !defined(_WIN32)
      if (errno == EAGAIN) {
         return fixscript_int(0);
      }
      #endif
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }
   return fixscript_int((int)ret);
#endif /* __wasm__ */
}


static Value native_tcp_connection_write(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(__wasm__)
   *error = fixscript_create_error_string(heap, "not supported");
   return fixscript_int(0);
#else
   TCPConnectionHandle *handle;
   char *buf;
   int timeout = fixscript_get_int(params[4]);
#if defined(_WIN32)
   fd_set writefds;
   TIMEVAL timeval;
   int ret;
#else
   struct pollfd pfd;
   ssize_t ret;
#endif
   int err;

   handle = get_tcp_connection_handle(heap, error, params[0]);
   if (!handle) {
      return fixscript_int(0);
   }

#if defined(_WIN32)
   if (timeout >= 0) {
      FD_ZERO(&writefds);
      FD_SET(handle->socket, &writefds);
      timeval.tv_sec = timeout / 1000;
      timeval.tv_usec = (timeout % 1000) * 1000;
      ret = select(1, NULL, &writefds, NULL, &timeval);
      if (ret == SOCKET_ERROR) {
         *error = fixscript_create_error_string(heap, "I/O error");
         return fixscript_int(0);
      }
      if (ret == 0) {
         return fixscript_int(0);
      }
      handle->want_nonblocking = 1;
   }
#else
   if (timeout >= 0) {
      pfd.fd = handle->fd;
      pfd.events = POLLOUT;
      ret = poll(&pfd, 1, timeout);
      if (ret < 0) {
         *error = fixscript_create_error_string(heap, "I/O error");
         return fixscript_int(0);
      }
      if (ret == 1) {
         ret = 0;
         if (pfd.revents & POLLOUT) ret = 1;
      }
      if (ret == 0) {
         return fixscript_int(0);
      }
      handle->want_nonblocking = 1;
   }
#endif

   if (!update_nonblocking(handle)) {
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }
   handle->want_nonblocking = 0;

   err = fixscript_lock_array(heap, params[1], params[2].value, params[3].value, (void **)&buf, 1, ACCESS_READ_ONLY);
   if (err) {
      fixscript_error(heap, error, err);
      return fixscript_int(0);
   }

#if defined(_WIN32)
   ret = send(handle->socket, buf, params[3].value, 0);
   if (ret == SOCKET_ERROR) {
      if (WSAGetLastError() == WSAEWOULDBLOCK) {
         ret = 0;
      }
      else {
         ret = -1;
      }
   }
#else
   ret = write(handle->fd, buf, params[3].value);
   if (ret < 0 && errno == EAGAIN) {
      ret = 0;
   }
#endif

   fixscript_unlock_array(heap, params[1], params[2].value, params[3].value, (void **)&buf, 1, ACCESS_READ_ONLY);

   if (ret < 0) {
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }
   return fixscript_int(ret);
#endif /* __wasm__ */
}


#ifndef __wasm__
static void *tcp_server_handle_func(Heap *heap, int op, void *p1, void *p2)
{
   TCPServerHandle *handle = p1;
   switch (op) {
      case HANDLE_OP_FREE:
         if (__sync_sub_and_fetch(&handle->refcnt, 1) == 0) {
            if (!handle->closed) {
               #if defined(_WIN32)
                  closesocket(handle->socket);
               #elif defined(__wasm__)
               #else
                  close(handle->fd);
               #endif
            }
            free(handle);
         }
         break;

      case HANDLE_OP_COPY:
         __sync_add_and_fetch(&handle->refcnt, 1);
         return handle;
   }
   return NULL;
}
#endif


static TCPServerHandle *get_tcp_server_handle(Heap *heap, Value *error, Value handle_val)
{
   TCPServerHandle *handle;

   handle = fixscript_get_handle(heap, handle_val, HANDLE_TYPE_TCP_SERVER, NULL);
   if (!handle) {
      *error = fixscript_create_error_string(heap, "invalid TCP server handle");
      return NULL;
   }

   if (handle->closed) {
      *error = fixscript_create_error_string(heap, "TCP server is already closed");
      return NULL;
   }

   return handle;
}


static Value native_tcp_server_create(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(__wasm__)
   *error = fixscript_create_error_string(heap, "not supported");
   return fixscript_int(0);
#else
   int local_only = data == (void *)1;
   TCPServerHandle *handle;
   struct sockaddr_in server;
#if defined(_WIN32)
   SOCKET sock = INVALID_SOCKET;
#else
   int fd = -1;
#endif
   int reuse;
   Value retval = fixscript_int(0);

#if defined(_WIN32)
   sock = socket(AF_INET, SOCK_STREAM, 0);
   if (sock == INVALID_SOCKET) {
      goto io_error;
   }

   reuse = 1;
   if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse)) < 0) {
      goto io_error;
   }

   server.sin_family = AF_INET;
   server.sin_addr.s_addr = htonl(local_only? INADDR_LOOPBACK : INADDR_ANY);
   server.sin_port = htons(params[0].value);

   if (bind(sock, (struct sockaddr *)&server, sizeof(server)) == SOCKET_ERROR) {
      goto io_error;
   }

   if (listen(sock, 5) == SOCKET_ERROR) {
      goto io_error;
   }
#else
   fd = socket(AF_INET, SOCK_STREAM, 0);
   if (fd == -1) {
      goto io_error;
   }

   reuse = 1;
   if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse)) < 0) {
      goto io_error;
   }

   server.sin_family = AF_INET;
   server.sin_addr.s_addr = htonl(local_only? INADDR_LOOPBACK : INADDR_ANY);
   server.sin_port = htons(params[0].value);

   if (bind(fd, (struct sockaddr *)&server, sizeof(server)) < 0) {
      goto io_error;
   }

   if (listen(fd, 5) < 0) {
      goto io_error;
   }
#endif

   handle = calloc(1, sizeof(TCPServerHandle));
   if (!handle) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   handle->refcnt = 1;
#if defined(_WIN32)
   handle->socket = sock;
#else
   handle->fd = fd;
#endif

   retval = fixscript_create_value_handle(heap, HANDLE_TYPE_TCP_SERVER, handle, tcp_server_handle_func);
#if defined(_WIN32)
   sock = INVALID_SOCKET;
#else
   fd = -1;
#endif
   if (!retval.value) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

error:
#if defined(_WIN32)
   if (!retval.value && sock != INVALID_SOCKET) {
      closesocket(sock);
   }
#else
   if (!retval.value && fd != -1) {
      close(fd);
   }
#endif
   return retval;

io_error:
   *error = fixscript_create_error_string(heap, "I/O error");
   goto error;
#endif /* __wasm__ */
}


static Value native_tcp_server_close(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   TCPServerHandle *handle;

   handle = get_tcp_server_handle(heap, error, params[0]);
   if (!handle) {
      return fixscript_int(0);
   }

   handle->closed = 1;
#if defined(_WIN32)
   if (closesocket(handle->socket) != 0) {
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }
#elif defined(__wasm__)
   *error = fixscript_create_error_string(heap, "not supported");
#else
   if (close(handle->fd) == -1) {
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }
#endif
   return fixscript_int(0);
}


static Value native_tcp_server_accept(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(__wasm__)
   *error = fixscript_create_error_string(heap, "not supported");
   return fixscript_int(0);
#else
   TCPServerHandle *handle;
   TCPConnectionHandle *conn_handle;
   Value retval;
   int timeout = fixscript_get_int(params[1]);
#if defined(_WIN32)
   fd_set readfds;
   TIMEVAL timeval;
   SOCKET sock;
#else
   struct pollfd pfd;
   int fd;
#endif
   int ret, flag;

   handle = get_tcp_server_handle(heap, error, params[0]);
   if (!handle) {
      return fixscript_int(0);
   }

#if defined(_WIN32)
   if (timeout >= 0) {
      FD_ZERO(&readfds);
      FD_SET(handle->socket, &readfds);
      timeval.tv_sec = timeout / 1000;
      timeval.tv_usec = (timeout % 1000) * 1000;
      ret = select(1, &readfds, NULL, NULL, &timeval);
      if (ret == SOCKET_ERROR) {
         *error = fixscript_create_error_string(heap, "I/O error");
         return fixscript_int(0);
      }
      if (ret == 0) {
         return fixscript_int(0);
      }
   }
   sock = accept(handle->socket, NULL, NULL);
   if (sock == INVALID_SOCKET) {
      if (WSAGetLastError() == WSAEWOULDBLOCK) {
         return fixscript_int(0);
      }
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }

   flag = 1;
   setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));
#else
   if (timeout >= 0) {
      pfd.fd = handle->fd;
      pfd.events = POLLIN;
      ret = poll(&pfd, 1, timeout);
      if (ret < 0) {
         *error = fixscript_create_error_string(heap, "I/O error");
         return fixscript_int(0);
      }
      if (ret == 1) {
         ret = 0;
         if (pfd.revents & POLLIN) ret = 1;
      }
      if (ret == 0) {
         return fixscript_int(0);
      }
   }
   fd = accept(handle->fd, NULL, NULL);
   if (fd < 0) {
      if (errno == EAGAIN) {
         return fixscript_int(0);
      }
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }

   flag = 1;
   setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));
#endif

   conn_handle = calloc(1, sizeof(TCPConnectionHandle));
   if (!conn_handle) {
      #if defined(_WIN32)
         closesocket(sock);
      #else
         close(fd);
      #endif
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      return fixscript_int(0);
   }

   conn_handle->refcnt = 1;
#if defined(_WIN32)
   conn_handle->socket = sock;
#else
   conn_handle->fd = fd;
#endif

   retval = fixscript_create_value_handle(heap, HANDLE_TYPE_TCP_CONNECTION, conn_handle, tcp_connection_handle_func);
   if (!retval.value) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      return fixscript_int(0);
   }

   return retval;
#endif /* __wasm__ */
}


#ifndef __wasm__
static void async_process_ref(AsyncProcess *proc)
{
   __sync_add_and_fetch(&proc->refcnt, 1);
}
#endif


#ifndef __wasm__
static void async_process_unref(AsyncProcess *proc)
{
   AsyncThreadResult *atr, *atr_next;
   AsyncTimer *timer, *timer_next;
   
   if (__sync_sub_and_fetch(&proc->refcnt, 1) == 0) {
      for (atr = proc->thread_results; atr; atr = atr_next) {
         #if defined(_WIN32)
            if (atr->socket != INVALID_SOCKET) {
               closesocket(atr->socket);
            }
         #elif defined(__wasm__)
         #else
            if (atr->fd != -1) {
               close(atr->fd);
            }
         #endif
         atr_next = atr->next;
         free(atr);
      }
      #if defined(_WIN32)
         CloseHandle(proc->iocp);
      #elif defined(__wasm__)
      #else
         poll_destroy(proc->poll);
      #endif
      for (timer = proc->timers; timer; timer = timer_next) {
         timer_next = timer->next;
         free(timer);
      }
      pthread_mutex_destroy(&proc->mutex);
      free(proc);
   }
}
#endif


#ifndef __wasm__
static AsyncProcess *get_async_process(Heap *heap, Value *error)
{
   AsyncProcess *proc;
   int err;

   proc = fixscript_get_heap_data(heap, async_process_key);
   if (!proc) {
      proc = calloc(1, sizeof(AsyncProcess));
      proc->refcnt = 1;
      if (pthread_mutex_init(&proc->mutex, NULL) != 0) {
         free(proc);
         *error = fixscript_create_error_string(heap, "can't create mutex");
         return NULL;
      }

#if defined(_WIN32)
      proc->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
      if (!proc->iocp) {
         pthread_mutex_destroy(&proc->mutex);
         free(proc);
         *error = fixscript_create_error_string(heap, "can't create IO completion port");
         return NULL;
      }
#else
      #ifndef __wasm__
         proc->poll = poll_create();
      #endif
      if (!proc->poll) {
         pthread_mutex_destroy(&proc->mutex);
         free(proc);
         *error = fixscript_create_error_string(heap, "can't create poll file descriptor");
         return NULL;
      }
#endif

      err = fixscript_set_heap_data(heap, async_process_key, proc, (HandleFreeFunc)async_process_unref);
      if (err) {
         fixscript_error(heap, error, err);
         return NULL;
      }
   }
   return proc;
}
#endif


#ifndef __wasm__
static void async_process_notify(AsyncProcess *proc)
{
#if defined(_WIN32)
   PostQueuedCompletionStatus(proc->iocp, 0, 0, (OVERLAPPED *)proc);
#elif defined(__wasm__)
#else
   poll_interrupt(proc->poll);
#endif
}
#endif


static pthread_mutex_t *get_mutex(volatile pthread_mutex_t **mutex_ptr)
{
   pthread_mutex_t *mutex, *cur_mutex;

   mutex = (pthread_mutex_t *)*mutex_ptr;
   if (!mutex) {
      mutex = calloc(1, sizeof(pthread_mutex_t));
      if (!mutex) {
         return NULL;
      }
      if (pthread_mutex_init(mutex, NULL) != 0) {
         free(mutex);
         return NULL;
      }
      cur_mutex = (pthread_mutex_t *)__sync_val_compare_and_swap(mutex_ptr, NULL, mutex);
      if (cur_mutex) {
         pthread_mutex_destroy(mutex);
         free(mutex);
         mutex = cur_mutex;
      }
   }
   return mutex;
}


#if !defined(_WIN32) && !defined(__wasm__)
static pthread_cond_t *get_cond(volatile pthread_cond_t **cond_ptr)
{
   pthread_cond_t *cond, *cur_cond;

   cond = (pthread_cond_t *)*cond_ptr;
   if (!cond) {
      cond = calloc(1, sizeof(pthread_cond_t));
      if (!cond) {
         return NULL;
      }
      if (pthread_cond_init(cond, NULL) != 0) {
         free(cond);
         return NULL;
      }
      cur_cond = (pthread_cond_t *)__sync_val_compare_and_swap(cond_ptr, NULL, cond);
      if (cur_cond) {
         pthread_cond_destroy(cond);
         free(cond);
         cond = cur_cond;
      }
   }
   return cond;
}
#endif


#ifndef __wasm__
#if defined(_WIN32)
static DWORD WINAPI thread_main(void *data)
#else
static void *thread_main(void *data)
#endif
{
   Thread *thread = data;
   pthread_mutex_t *global_mutex;
   
   global_mutex = get_mutex(&threads_mutex);
   if (!global_mutex) {
#if defined(_WIN32)
      return 0;
#else
      return NULL;
#endif
   }

   for (;;) {
      pthread_mutex_lock(&thread->mutex);
      while (!thread->func) {
         if (pthread_cond_timedwait_relative(&thread->cond, &thread->mutex, 5000*1000000LL) == ETIMEDOUT && !thread->func) {
            pthread_mutex_unlock(&thread->mutex);
            goto end;
         }
      }

      thread->func(thread->data);
      thread->func = NULL;
      pthread_mutex_unlock(&thread->mutex);
      
      pthread_mutex_lock(global_mutex);
      thread->next = threads_pool;
      threads_pool = thread;
      pthread_mutex_unlock(global_mutex);
   }

end:
   pthread_cond_destroy(&thread->cond);
   pthread_mutex_destroy(&thread->mutex);
   free(thread);

#if defined(_WIN32)
   return 0;
#else
   return NULL;
#endif
}
#endif


#ifndef __wasm__
static int async_run_thread(ThreadFunc func, void *data)
{
   pthread_mutex_t *mutex;
   Thread *thread;
#if defined(_WIN32)
   HANDLE handle;
#else
   pthread_t handle;
#endif

   mutex = get_mutex(&threads_mutex);
   if (!mutex) {
      return 0;
   }
   
   pthread_mutex_lock(mutex);

   if (threads_pool) {
      thread = threads_pool;
      threads_pool = thread->next;
   }
   else {
      thread = calloc(1, sizeof(Thread));
      if (!thread) {
         pthread_mutex_unlock(mutex);
         return 0;
      }

      if (pthread_mutex_init(&thread->mutex, NULL) != 0) {
         free(thread);
         pthread_mutex_unlock(mutex);
         return 0;
      }

      if (pthread_cond_init(&thread->cond, NULL) != 0) {
         pthread_mutex_destroy(&thread->mutex);
         free(thread);
         pthread_mutex_unlock(mutex);
         return 0;
      }

#if defined(_WIN32)
      handle = CreateThread(NULL, 0, thread_main, thread, 0, NULL);
      if (!handle) {
         pthread_cond_destroy(&thread->cond);
         pthread_mutex_destroy(&thread->mutex);
         free(thread);
         pthread_mutex_unlock(mutex);
         return 0;
      }
      CloseHandle(handle);
#else
      if (pthread_create(&handle, NULL, thread_main, thread) != 0) {
         pthread_cond_destroy(&thread->cond);
         pthread_mutex_destroy(&thread->mutex);
         free(thread);
         pthread_mutex_unlock(mutex);
         return 0;
      }
      pthread_detach(handle);
#endif
   }

   pthread_mutex_unlock(mutex);
   
   pthread_mutex_lock(&thread->mutex);
   thread->func = func;
   thread->data = data;
   pthread_cond_signal(&thread->cond);
   pthread_mutex_unlock(&thread->mutex);
   return 1;
}
#endif


#ifndef __wasm__
static void free_async_handle(void *data)
{
   AsyncHandle *handle = data;
#if !defined(_WIN32) && !defined(__wasm__)
   AsyncProcess *proc = handle->proc;
#endif

#if !defined(_WIN32) && !defined(__wasm__)
   poll_remove_socket(proc->poll, handle->fd);
#endif
   async_process_unref(handle->proc);

   if (handle->type == ASYNC_TCP_CONNECTION || handle->type == ASYNC_TCP_SERVER) {
      #if defined(_WIN32)
      if (handle->type == ASYNC_TCP_SERVER) {
         closesocket(((AsyncServerHandle *)handle)->accept_socket);
      }
      closesocket(handle->socket);
      #elif defined(__wasm__)
      #else
      close(handle->fd);
      #endif
   }
   free(handle);
}
#endif


#if !defined(_WIN32) && !defined(__wasm__)
static void update_poll_ctl(AsyncHandle *handle)
{
   AsyncServerHandle *server_handle;

   if (handle->type == ASYNC_TCP_CONNECTION) {
      if (handle->active != handle->last_active) {
         poll_update_socket(handle->proc->poll, handle->fd, handle, handle->active);
         handle->last_active = handle->active;
      }
   }
   else if (handle->type == ASYNC_TCP_SERVER) {
      server_handle = (AsyncServerHandle *)handle;
      if (server_handle->active != server_handle->last_active) {
         poll_update_socket(server_handle->proc->poll, server_handle->fd, server_handle, server_handle->active? ASYNC_READ : 0);
         server_handle->last_active = server_handle->active;
      }
   }
}
#endif


#ifndef __wasm__
typedef struct {
   AsyncProcess *proc;
   char *hostname;
   int port;
   Value callback;
   Value data;
   AsyncThreadResult *atr;
} TCPOpenData;

static void tcp_open_func(void *data)
{
   TCPOpenData *tod = data;
   AsyncProcess *proc = tod->proc;
   AsyncThreadResult *atr = tod->atr;
#if defined(_WIN32)
   u_long arg;
#else
   int flags;
#endif

   atr->type = ASYNC_TCP_CONNECTION;
   atr->callback = tod->callback;
   atr->data = tod->data;

#if defined(_WIN32)
   if (tcp_connect(tod->hostname, tod->port, &atr->socket, 1)) {
      arg = 1;
      if (ioctlsocket(atr->socket, FIONBIO, &arg) != 0) {
         closesocket(atr->socket);
         atr->socket = INVALID_SOCKET;
      }
   }
   else {
      atr->socket = INVALID_SOCKET;
   }
#else
   if (tcp_connect(tod->hostname, tod->port, &atr->fd)) {
      flags = fcntl(atr->fd, F_GETFL);
      if (flags != -1) {
         flags |= O_NONBLOCK;
         if (fcntl(atr->fd, F_SETFL, flags) == -1) {
            close(atr->fd);
            atr->fd = -1;
         }
      }
      else {
         close(atr->fd);
         atr->fd = -1;
      }
   }
   else {
      atr->fd = -1;
   }
#endif

   pthread_mutex_lock(&proc->mutex);
   atr->next = proc->thread_results;
   proc->thread_results = atr;
   async_process_notify(proc);
   pthread_mutex_unlock(&proc->mutex);

   async_process_unref(tod->proc);
   free(tod->hostname);
   free(tod);
}
#endif /* __wasm__ */

static Value native_async_tcp_connection_open(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(__wasm__)
   *error = fixscript_create_error_string(heap, "not supported");
   return fixscript_int(0);
#else
   AsyncProcess *proc;
   TCPOpenData *tod;
   int err;
   
   proc = get_async_process(heap, error);
   if (!proc) return fixscript_int(0);

   tod = calloc(1, sizeof(TCPOpenData));
   if (!tod) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   
   tod->atr = calloc(1, sizeof(AsyncThreadResult));
   if (!tod->atr) {
      free(tod);
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   err = fixscript_get_string(heap, params[0], 0, -1, &tod->hostname, NULL);
   if (err) {
      free(tod->atr);
      free(tod);
      return fixscript_error(heap, error, err);
   }

   tod->proc = proc;
   tod->port = fixscript_get_int(params[1]);
   tod->callback = params[2];
   tod->data = params[3];
   async_process_ref(proc);
   fixscript_ref(heap, tod->data);

   if (!async_run_thread(tcp_open_func, tod)) {
      async_process_unref(proc);
      fixscript_unref(heap, tod->data);
      free(tod->hostname);
      free(tod->atr);
      free(tod);
      *error = fixscript_create_error_string(heap, "can't create thread");
      return fixscript_int(0);
   }

   return fixscript_int(0);
#endif /* __wasm__ */
}


static Value native_async_tcp_connection_read(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(__wasm__)
   *error = fixscript_create_error_string(heap, "not supported");
   return fixscript_int(0);
#else
   AsyncHandle *handle;
#ifdef _WIN32
   WSABUF wsabuf;
   DWORD flags;
   DWORD read;
#endif

   handle = fixscript_get_handle(heap, params[0], HANDLE_TYPE_ASYNC, NULL);
   if (!handle || handle->type != ASYNC_TCP_CONNECTION) {
      *error = fixscript_create_error_string(heap, "invalid async stream handle");
      return fixscript_int(0);
   }
   
   if (handle->active & ASYNC_READ) {
      *error = fixscript_create_error_string(heap, "only one read operation can be active at a time");
      return fixscript_int(0);
   }

#ifdef _WIN32
   if (params[3].value > sizeof(handle->read.buf)) {
      params[3].value = sizeof(handle->read.buf);
   }
   if (params[3].value < 0) {
      *error = fixscript_create_error_string(heap, "negative length");
      return fixscript_int(0);
   }
#endif

   handle->active |= ASYNC_READ;
   handle->read.callback = params[4];
   handle->read.data = params[5];
   handle->read.array = params[1];
   handle->read.off = params[2].value;
   handle->read.len = params[3].value;
   fixscript_ref(heap, handle->read.data);
   fixscript_ref(heap, handle->read.array);

#ifdef _WIN32
   memset(&handle->read.overlapped, 0, sizeof(WSAOVERLAPPED));

   wsabuf.len = params[3].value;
   wsabuf.buf = handle->read.buf;
   flags = 0;
   WSARecv(handle->socket, &wsabuf, 1, &read, &flags, &handle->read.overlapped, NULL);
#else
   update_poll_ctl(handle);
#endif

   return fixscript_int(0);
#endif /* __wasm__ */
}


#if defined(_WIN32)
static Value native_async_tcp_connection_write(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   AsyncHandle *handle;
   WSABUF wsabuf;
   DWORD written;
   int err;

   handle = fixscript_get_handle(heap, params[0], HANDLE_TYPE_ASYNC, NULL);
   if (!handle || handle->type != ASYNC_TCP_CONNECTION) {
      *error = fixscript_create_error_string(heap, "invalid async stream handle");
      return fixscript_int(0);
   }
   
   if (handle->active & ASYNC_WRITE) {
      *error = fixscript_create_error_string(heap, "only one write operation can be active at a time");
      return fixscript_int(0);
   }

   if (params[3].value > sizeof(handle->write.buf)) {
      params[3].value = sizeof(handle->write.buf);
   }

   err = fixscript_get_array_bytes(heap, params[1], params[2].value, params[3].value, handle->write.buf);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   handle->active |= ASYNC_WRITE;
   handle->write.callback = params[4];
   handle->write.data = params[5];
   fixscript_ref(heap, handle->write.data);

   memset(&handle->write.overlapped, 0, sizeof(WSAOVERLAPPED));

   wsabuf.len = params[3].value;
   wsabuf.buf = handle->write.buf;
   WSASend(handle->socket, &wsabuf, 1, &written, 0, &handle->write.overlapped, NULL);

   return fixscript_int(0);
}
#elif defined(__wasm__)
static Value native_async_tcp_connection_write(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   *error = fixscript_create_error_string(heap, "not supported");
   return fixscript_int(0);
}
#else
static Value native_async_tcp_connection_write(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   AsyncHandle *handle;
   char *buf;
   int err;
   int written;

   handle = fixscript_get_handle(heap, params[0], HANDLE_TYPE_ASYNC, NULL);
   if (!handle || handle->type != ASYNC_TCP_CONNECTION) {
      *error = fixscript_create_error_string(heap, "invalid async stream handle");
      return fixscript_int(0);
   }
   
   if (handle->active & ASYNC_WRITE) {
      *error = fixscript_create_error_string(heap, "only one write operation can be active at a time");
      return fixscript_int(0);
   }

   err = fixscript_lock_array(heap, params[1], params[2].value, params[3].value, (void **)&buf, 1, ACCESS_READ_ONLY);
   if (err) {
      fixscript_error(heap, error, err);
      return fixscript_int(0);
   }

   handle->active |= ASYNC_WRITE;
   handle->write.callback = params[4];
   handle->write.data = params[5];
   fixscript_ref(heap, handle->write.data);

   written = write(handle->fd, buf, params[3].value);
   if (written < 0 && errno == EAGAIN) {
      written = 0;
   }
   handle->write.result = written;
   update_poll_ctl(handle);

   fixscript_unlock_array(heap, params[1], params[2].value, params[3].value, (void **)&buf, 1, ACCESS_READ_ONLY);

   return fixscript_int(0);
}
#endif


static Value native_async_tcp_connection_close(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   AsyncHandle *handle;

   handle = fixscript_get_handle(heap, params[0], HANDLE_TYPE_ASYNC, NULL);
   if (!handle || handle->type != ASYNC_TCP_CONNECTION) {
      *error = fixscript_create_error_string(heap, "invalid async stream handle");
      return fixscript_int(0);
   }

#if defined(_WIN32)
   closesocket(handle->socket);
   handle->socket = INVALID_SOCKET;
#elif defined(__wasm__)
#else
   poll_remove_socket(handle->proc->poll, handle->fd);
   close(handle->fd);
   handle->fd = -1;
#endif
   return fixscript_int(0);
}


static Value native_async_tcp_server_create(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(__wasm__)
   *error = fixscript_create_error_string(heap, "not supported");
   return fixscript_int(0);
#else
   int local_only = data == (void *)1;
   AsyncProcess *proc;
   AsyncServerHandle *handle;
   struct sockaddr_in server;
#if defined(_WIN32)
   SOCKET sock = INVALID_SOCKET;
#else
   int fd = -1;
#endif
   int reuse;
   Value retval = fixscript_int(0);
   
   proc = get_async_process(heap, error);
   if (!proc) return fixscript_int(0);

#if defined(_WIN32)
   sock = socket(AF_INET, SOCK_STREAM, 0);
   if (sock == INVALID_SOCKET) {
      goto io_error;
   }

   reuse = 1;
   if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse)) < 0) {
      goto io_error;
   }

   server.sin_family = AF_INET;
   server.sin_addr.s_addr = htonl(local_only? INADDR_LOOPBACK : INADDR_ANY);
   server.sin_port = htons(params[0].value);

   if (bind(sock, (struct sockaddr *)&server, sizeof(server)) == SOCKET_ERROR) {
      goto io_error;
   }

   if (listen(sock, 5) == SOCKET_ERROR) {
      goto io_error;
   }
#else
   fd = socket(AF_INET, SOCK_STREAM, 0);
   if (fd == -1) {
      goto io_error;
   }

   reuse = 1;
   if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse)) < 0) {
      goto io_error;
   }

   server.sin_family = AF_INET;
   server.sin_addr.s_addr = htonl(local_only? INADDR_LOOPBACK : INADDR_ANY);
   server.sin_port = htons(params[0].value);

   if (bind(fd, (struct sockaddr *)&server, sizeof(server)) < 0) {
      goto io_error;
   }

   if (listen(fd, 5) < 0) {
      goto io_error;
   }
#endif

   handle = calloc(1, sizeof(AsyncServerHandle));
   if (!handle) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   handle->proc = proc;
   async_process_ref(handle->proc);
   handle->type = ASYNC_TCP_SERVER;
#if defined(_WIN32)
   handle->socket = sock;
   handle->accept_socket = INVALID_SOCKET;
   if (!CreateIoCompletionPort((HANDLE)handle->socket, proc->iocp, (ULONG_PTR)handle, 0)) {
      async_process_unref(handle->proc);
      free(handle);
      goto io_error;
   }
#else
   handle->fd = fd;
   if (!poll_add_socket(proc->poll, fd, handle, 0)) {
      async_process_unref(handle->proc);
      free(handle);
      goto io_error;
   }
#endif

   retval = fixscript_create_handle(heap, HANDLE_TYPE_ASYNC, handle, free_async_handle);
#if defined(_WIN32)
   sock = INVALID_SOCKET;
#else
   fd = -1;
#endif
   if (!retval.value) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

error:
#if defined(_WIN32)
   if (!retval.value && sock != INVALID_SOCKET) {
      closesocket(sock);
   }
#else
   if (!retval.value && fd != -1) {
      close(fd);
   }
#endif
   return retval;

io_error:
   *error = fixscript_create_error_string(heap, "I/O error");
   goto error;

   return fixscript_int(0);
#endif /* __wasm__ */
}


static Value native_async_tcp_server_close(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(__wasm__)
   *error = fixscript_create_error_string(heap, "not supported");
   return fixscript_int(0);
#else
   AsyncServerHandle *handle;

   handle = fixscript_get_handle(heap, params[0], HANDLE_TYPE_ASYNC, NULL);
   if (!handle || handle->type != ASYNC_TCP_SERVER) {
      *error = fixscript_create_error_string(heap, "invalid async TCP server handle");
      return fixscript_int(0);
   }

#ifdef _WIN32
   closesocket(handle->socket);
   handle->socket = INVALID_SOCKET;
#else
   poll_remove_socket(handle->proc->poll, handle->fd);
   close(handle->fd);
   handle->fd = -1;
#endif
   return fixscript_int(0);
#endif /* __wasm__ */
}


static Value native_async_tcp_server_accept(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(__wasm__)
   *error = fixscript_create_error_string(heap, "not supported");
   return fixscript_int(0);
#else
   AsyncServerHandle *handle;
#ifdef _WIN32
   BOOL ret;
   int flag;
#endif

   handle = fixscript_get_handle(heap, params[0], HANDLE_TYPE_ASYNC, NULL);
   if (!handle || handle->type != ASYNC_TCP_SERVER) {
      *error = fixscript_create_error_string(heap, "invalid async TCP server handle");
      return fixscript_int(0);
   }
   
   if (handle->active) {
      *error = fixscript_create_error_string(heap, "only one accept operation can be active at a time");
      return fixscript_int(0);
   }

#ifdef _WIN32
   handle->accept_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
   if (handle->accept_socket == INVALID_SOCKET) {
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }
   flag = 1;
   setsockopt(handle->accept_socket, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));
#endif

   handle->active = 1;
   handle->callback = params[1];
   handle->data = params[2];
   fixscript_ref(heap, handle->data);

#ifdef _WIN32
   memset(&handle->overlapped, 0, sizeof(OVERLAPPED));
   ret = AcceptEx(handle->socket, handle->accept_socket, handle->buf, 0, sizeof(struct sockaddr_in) + 16, sizeof(struct sockaddr_in) + 16, NULL, &handle->overlapped);
   if (ret != 0 || WSAGetLastError() != ERROR_IO_PENDING) {
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }
#else
   update_poll_ctl((AsyncHandle *)handle);
#endif
   return fixscript_int(0);
#endif /* __wasm__ */
}


#ifndef __wasm__
static uint32_t get_time()
{
#if defined(_WIN32)
   return timeGetTime();
#elif defined(__APPLE__)
   uint64_t time;
   mach_timebase_info_data_t info;
   time = mach_absolute_time();
   mach_timebase_info(&info);
   return time * info.numer / info.denom / 1000000;
#else
   struct timespec ts;
   if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
      return 0;
   }
   return ((uint64_t)ts.tv_sec) * 1000 + ((uint64_t)ts.tv_nsec / 1000000L);
#endif
}
#endif


#ifndef __wasm__
static void wait_events(AsyncProcess *proc, int timeout)
{
   AsyncTimer *timer;
   int32_t diff;
#ifdef _WIN32
   CompletedIO *cio;
#endif

   pthread_mutex_lock(&proc->mutex);
   timer = proc->timers;
   if (timer) {
      if (timer->immediate) {
         timeout = 0;
      }
      else {
         diff = (int32_t)(timer->time - get_time());
         if (timeout < 0 || diff < timeout) {
            timeout = diff;
            if (timeout < 0) timeout = 0;
         }
      }
   }
   pthread_mutex_unlock(&proc->mutex);

#if defined(_WIN32)
   if (timeout < 0) {
      timeout = INFINITE;
   }
   proc->num_events = 0;
   cio = &proc->completed_ios[0];

   while (proc->num_events < sizeof(proc->completed_ios)/sizeof(CompletedIO)) {
      GetQueuedCompletionStatus(proc->iocp, &cio->transferred, &cio->key, (OVERLAPPED **)&cio->overlapped, timeout);
      if (!cio->overlapped) break;

      cio = &proc->completed_ios[++proc->num_events];
      timeout = 0;
   }
#elif defined(__wasm__)
#else
   if (timeout < 0) {
      timeout = -1;
   }
   poll_wait(proc->poll, timeout);
#endif
}
#endif


#ifndef __wasm__
static int process_events(AsyncProcess *proc, Heap *heap, Value *error)
{
   AsyncThreadResult *atr = NULL, *atr_next;
   AsyncTimer *timer;
   AsyncHandle *handle;
   Value handle_val, callback_error;
   uint32_t time = 0;
   int32_t diff;
   int time_obtained;
#if defined(_WIN32)
   CompletedIO *cio;
   int i;
#else
   int flags;
#endif

#if defined(_WIN32)
   for (i=0; i<proc->num_events; i++) {
      cio = &proc->completed_ios[i];
      if (cio->overlapped == proc) {
         continue;
      }

      handle = (AsyncHandle *)(uintptr_t)cio->key;
      if (handle->type == ASYNC_TCP_CONNECTION) {
         if ((void *)cio->overlapped == &handle->read.overlapped) {
            if (handle->active & ASYNC_READ) {
               Value callback, data;
               int result = cio->transferred;

               callback = handle->read.callback;
               data = handle->read.data;
               handle->active &= ~ASYNC_READ;

               if (fixscript_set_array_bytes(heap, handle->read.array, handle->read.off, result, handle->read.buf) != 0) {
                  result = -1;
               }
               fixscript_unref(heap, handle->read.array);

               fixscript_call(heap, callback, 2, &callback_error, data, fixscript_int(result));
               if (callback_error.value) {
                  fixscript_dump_value(heap, callback_error, 1);
               }
               fixscript_unref(heap, data);
            }
         }
         if ((void *)cio->overlapped == &handle->write.overlapped) {
            if (handle->active & ASYNC_WRITE) {
               Value callback, data;
               int result = cio->transferred;

               callback = handle->write.callback;
               data = handle->write.data;
               handle->active &= ~ASYNC_WRITE;

               fixscript_call(heap, callback, 2, &callback_error, data, fixscript_int(result));
               if (callback_error.value) {
                  fixscript_dump_value(heap, callback_error, 1);
               }
               fixscript_unref(heap, data);
            }
         }
      }
      else if (handle->type == ASYNC_TCP_SERVER) {
         AsyncServerHandle *server_handle = (AsyncServerHandle *)handle;
         if ((void *)cio->overlapped == &server_handle->overlapped) {
            AsyncHandle *new_handle;
            Value callback, data;
            Value result = fixscript_int(0);
            SOCKET socket;

            socket = server_handle->accept_socket;
            server_handle->accept_socket = INVALID_SOCKET;

            new_handle = calloc(1, sizeof(AsyncHandle));
            if (!new_handle) {
               closesocket(socket);
            }
            else {
               new_handle->proc = proc;
               async_process_ref(new_handle->proc);
               new_handle->type = ASYNC_TCP_CONNECTION;
               new_handle->socket = socket;
               if (!CreateIoCompletionPort((HANDLE)new_handle->socket, proc->iocp, (ULONG_PTR)new_handle, 0)) {
                  async_process_unref(new_handle->proc);
                  closesocket(new_handle->socket);
                  free(new_handle);
               }
               else {
                  result = fixscript_create_handle(heap, HANDLE_TYPE_ASYNC, new_handle, free_async_handle);
               }
            }

            callback = server_handle->callback;
            data = server_handle->data;
            server_handle->active = 0;

            fixscript_call(heap, callback, 2, &callback_error, data, result);
            if (callback_error.value) {
               fixscript_dump_value(heap, callback_error, 1);
            }
            fixscript_unref(heap, data);
         }
      }
   }
#else
   for (;;) {
      handle = poll_get_event(proc->poll, &flags);
      if (!handle) break;

      if (handle->type == ASYNC_TCP_CONNECTION) {
         if (flags & ASYNC_READ) {
            if (handle->active & ASYNC_READ) {
               Value callback, data;
               char *buf;
               int err = 0, result = -1;

               callback = handle->read.callback;
               data = handle->read.data;
               handle->active &= ~ASYNC_READ;

               err = fixscript_lock_array(heap, handle->read.array, handle->read.off, handle->read.len, (void **)&buf, 1, ACCESS_WRITE_ONLY);
               if (!err) {
                  result = read(handle->fd, buf, handle->read.len);
                  handle->read.len = result;
                  if (handle->read.len < 0) handle->read.len = 0;
                  fixscript_unlock_array(heap, handle->read.array, handle->read.off, handle->read.len, (void **)&buf, 1, ACCESS_WRITE_ONLY);
               }
               fixscript_unref(heap, handle->read.array);

               fixscript_call(heap, callback, 2, &callback_error, data, fixscript_int(result));
               if (callback_error.value) {
                  fixscript_dump_value(heap, callback_error, 1);
               }
               fixscript_unref(heap, data);
            }
         }
         if (flags & ASYNC_WRITE) {
            if (handle->active & ASYNC_WRITE) {
               Value callback, data;

               callback = handle->write.callback;
               data = handle->write.data;
               handle->active &= ~ASYNC_WRITE;

               fixscript_call(heap, callback, 2, &callback_error, data, fixscript_int(handle->write.result));
               if (callback_error.value) {
                  fixscript_dump_value(heap, callback_error, 1);
               }
               fixscript_unref(heap, data);
            }
         }
         update_poll_ctl(handle);
      }
      else if (handle->type == ASYNC_TCP_SERVER) {
         AsyncServerHandle *server_handle = (AsyncServerHandle *)handle;
         AsyncHandle *new_handle;
         if (flags & ASYNC_READ) {
            Value callback, data;
            Value result = fixscript_int(0);
            int fd, flag;
            fd = accept(server_handle->fd, NULL, NULL);
            if (fd >= 0) {
               flag = 1;
               setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));
               flag = fcntl(fd, F_GETFL);
               flag |= O_NONBLOCK;
               fcntl(fd, F_SETFL, flag);

               new_handle = calloc(1, sizeof(AsyncHandle));
               if (!new_handle) {
                  close(fd);
               }
               else {
                  new_handle->proc = proc;
                  async_process_ref(new_handle->proc);
                  new_handle->type = ASYNC_TCP_CONNECTION;
                  new_handle->fd = fd;
                  if (!poll_add_socket(proc->poll, fd, new_handle, 0)) {
                     async_process_unref(new_handle->proc);
                     close(new_handle->fd);
                     free(new_handle);
                  }
                  else {
                     result = fixscript_create_handle(heap, HANDLE_TYPE_ASYNC, new_handle, free_async_handle);
                  }
               }
            }

            callback = server_handle->callback;
            data = server_handle->data;
            server_handle->active = 0;

            fixscript_call(heap, callback, 2, &callback_error, data, result);
            if (callback_error.value) {
               fixscript_dump_value(heap, callback_error, 1);
            }
            fixscript_unref(heap, data);
         }
         update_poll_ctl(handle);
      }
   }
#endif

   pthread_mutex_lock(&proc->mutex);
   atr = proc->thread_results;
   proc->thread_results = NULL;
   pthread_mutex_unlock(&proc->mutex);

   while (atr) {
      if (atr->type == ASYNC_TCP_CONNECTION) {
         #ifdef _WIN32
         if (atr->socket != INVALID_SOCKET)
         #else
         if (atr->fd != -1)
         #endif
         {
            handle = calloc(1, sizeof(AsyncHandle));
            if (!handle) {
               #ifdef _WIN32
               closesocket(handle->socket);
               #else
               close(handle->fd);
               #endif
               fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
               goto error;
            }
            handle->proc = proc;
            async_process_ref(handle->proc);
            handle->type = ASYNC_TCP_CONNECTION;
            #ifdef _WIN32
            handle->socket = atr->socket;
            if (!CreateIoCompletionPort((HANDLE)handle->socket, proc->iocp, (ULONG_PTR)handle, 0)) {
               async_process_unref(handle->proc);
               closesocket(handle->socket);
               free(handle);
               *error = fixscript_create_error_string(heap, "can't add socket to IO completion port");
               goto error;
            }
            #else
            handle->fd = atr->fd;
            if (!poll_add_socket(proc->poll, atr->fd, handle, 0)) {
               async_process_unref(handle->proc);
               close(handle->fd);
               free(handle);
               *error = fixscript_create_error_string(heap, "can't add socket to poll");
               goto error;
            }
            #endif

            handle_val = fixscript_create_handle(heap, HANDLE_TYPE_ASYNC, handle, free_async_handle);
            if (!handle_val.value) {
               fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
               goto error;
            }
         }
         else {
            handle_val = fixscript_int(0);
         }

         fixscript_call(heap, atr->callback, 2, &callback_error, atr->data, handle_val);
         if (callback_error.value) {
            fixscript_dump_value(heap, callback_error, 1);
         }
         fixscript_unref(heap, atr->data);
      }
      atr_next = atr->next;
      free(atr);
      atr = atr_next;
   }

   pthread_mutex_lock(&proc->mutex);
   time_obtained = 0;
   while (proc->timers) {
      timer = proc->timers;
      if (!timer->immediate) {
         if (!time_obtained) {
            time = get_time();
            time_obtained = 1;
         }
         diff = (int32_t)(timer->time - time);
         if (diff > 0) break;
      }

      proc->timers = timer->next;
      pthread_mutex_unlock(&proc->mutex);

      fixscript_call(heap, timer->callback, 1, &callback_error, timer->data);
      if (callback_error.value) {
         fixscript_dump_value(heap, callback_error, 1);
      }
      fixscript_unref(heap, timer->data);

      free(timer);

      pthread_mutex_lock(&proc->mutex);
   }
   pthread_mutex_unlock(&proc->mutex);
   return 1;

error:
   while (atr) {
      atr_next = atr->next;
      free(atr);
      atr = atr_next;
   }
   return 0;
}
#endif /* __wasm__ */


static Value native_async_process(Heap *heap, Value *error, int num_params, Value *params, void *func_data)
{
#if defined(__wasm__)
   *error = fixscript_create_error_string(heap, "not supported");
   return fixscript_int(0);
#else
   AsyncProcess *proc;
   int timeout = -1, infinite;
   Value quit_value;
   
   proc = get_async_process(heap, error);
   if (!proc) return fixscript_int(0);
   
   if (proc->foreign_notify_func) {
      *error = fixscript_create_error_string(heap, "can't use async_process when integrated with other event loop");
      return fixscript_int(0);
   }

   if (num_params == 1) {
      timeout = fixscript_get_int(params[0]);
   }

   fixscript_unref(heap, proc->quit_value);
   infinite = (timeout < 0);
   proc->quit = !infinite;
   proc->quit_value = fixscript_int(0);

   if (infinite) {
      timeout = -1;
   }

   do {
      wait_events(proc, timeout);
      if (!process_events(proc, heap, error)) {
         return fixscript_int(0);
      }
   }
   while (!proc->quit);

   quit_value = proc->quit_value;
   proc->quit_value = fixscript_int(0);
   fixscript_unref(heap, quit_value);
   return quit_value;
#endif /* __wasm__ */
}


static Value native_async_run_later(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(__wasm__)
   *error = fixscript_create_error_string(heap, "not supported");
   return fixscript_int(0);
#else
   AsyncProcess *proc;
   AsyncTimer *timer, *t, **p;
   int32_t diff;

   proc = get_async_process(heap, error);
   if (!proc) return fixscript_int(0);

   timer = calloc(1, sizeof(AsyncTimer));
   if (!timer) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   if (params[0].value == 0) {
      timer->immediate = 1;
   }
   else {
      timer->time = get_time() + (uint32_t)params[0].value;
   }

   timer->callback = params[1];
   timer->data = params[2];
   fixscript_ref(heap, timer->data);

   pthread_mutex_lock(&proc->mutex);
   if (timer->immediate || !proc->timers) {
      timer->next = proc->timers;
      proc->timers = timer;
   }
   else {
      p = &proc->timers;
      for (t=proc->timers; t; t=t->next) {
         diff = (int32_t)(timer->time - t->time);
         if (!t->immediate && diff <= 0) {
            timer->next = t;
            *p = timer;
            break;
         }
         if (!t->next) {
            t->next = timer;
            break;
         }
         p = &t->next;
      }
   }
   pthread_mutex_unlock(&proc->mutex);

   if (proc->foreign_notify_func) {
      async_process_notify(proc);
   }
   return fixscript_int(0);
#endif /* __wasm__ */
}


static Value native_async_quit(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(__wasm__)
   *error = fixscript_create_error_string(heap, "not supported");
   return fixscript_int(0);
#else
   AsyncProcess *proc;
   Value quit_value = fixscript_int(0);
   
   proc = get_async_process(heap, error);
   if (!proc) return fixscript_int(0);
   
   if (proc->foreign_notify_func) {
      *error = fixscript_create_error_string(heap, "can't use async_quit when integrated with other event loop");
      return fixscript_int(0);
   }

   if (num_params == 1) {
      quit_value = params[0];
   }

   fixscript_unref(heap, proc->quit_value);
   proc->quit = 1;
   proc->quit_value = quit_value;
   fixscript_ref(heap, proc->quit_value);

   return fixscript_int(0);
#endif /* __wasm__ */
}


#ifndef __wasm__
static void *process_handle_func(Heap *heap, int op, void *p1, void *p2)
{
   ProcessHandle *handle = p1;
   switch (op) {
      case HANDLE_OP_FREE:
         if (__sync_sub_and_fetch(&handle->refcnt, 1) == 0) {
            #if defined(_WIN32)
               if (handle->flags & REDIR_IN) CloseHandle(handle->in);
               if (handle->flags & REDIR_OUT) CloseHandle(handle->out);
               if (handle->flags & REDIR_ERR) CloseHandle(handle->err);
               if (handle->process != ERROR_INVALID_HANDLE) {
                  CloseHandle(handle->process);
               }
            #elif defined(__wasm__)
            #else
               if (handle->flags & REDIR_IN) close(handle->in_fd);
               if (handle->flags & REDIR_OUT) close(handle->out_fd);
               if (handle->flags & REDIR_ERR) close(handle->err_fd);
            #endif
            free(handle);
         }
         break;

      case HANDLE_OP_COPY:
         __sync_add_and_fetch(&handle->refcnt, 1);
         return handle;
   }
   return NULL;
}
#endif


static Value native_process_create(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(_WIN32)
   ProcessHandle *handle;
   Value *arg_values = NULL, key_val, value_val, handle_val, ret = fixscript_int(0);
   uint16_t **args = NULL, *p, *cmdline = NULL, **envs = NULL, *key, *value, *envblock = NULL, *path = NULL;
   DWORD proc_flags = 0;
   STARTUPINFO si;
   PROCESS_INFORMATION pi;
   SECURITY_ATTRIBUTES sa;
   HANDLE pipes[6];
   HANDLE in_other, out_other, err_other;
   int flags = params[3].value;
   int num_args=0, total_len, len, num_envs=0, num_pipes;
   int i, j, err, needs_quotes, pos, idx;

   if (flags & REDIR_MERGE_ERR) {
      if ((flags & (REDIR_OUT | REDIR_ERR)) != REDIR_OUT) {
         *error = fixscript_create_error_string(heap, "incompatible flags");
         goto error;
      }
   }

   err = fixscript_get_array_length(heap, params[0], &num_args);
   if (!err) {
      if (num_args < 1) {
         *error = fixscript_create_error_string(heap, "must provide executable argument");
         goto error;
      }
      arg_values = calloc(num_args, sizeof(Value));
      if (!arg_values) {
         err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
   }
   if (!err) {
      err = fixscript_get_array_range(heap, params[0], 0, num_args, arg_values);
   }
   if (!err) {
      args = calloc(num_args, sizeof(uint8_t *));
      if (!args) {
         err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
   }
   if (!err) {
      for (i=0; i<num_args; i++) {
         err = fixscript_get_string_utf16(heap, arg_values[i], 0, -1, &args[i], NULL);
         if (err) break;
      }
   }
   if (!err) {
      total_len = 0;
      for (i=0; i<num_args; i++) {
         if (i > 0) total_len++;
         needs_quotes = 0;
         for (p = args[i]; *p; p++) {
            if (*p == ' ' || *p == '"') {
               needs_quotes = 1;
               break;
            }
         }
         if (needs_quotes) {
            total_len += 2;
         }
         for (p = args[i]; *p; p++) {
            if (*p == '"') {
               total_len += 3;
               continue;
            }
            total_len++;
         }
      }
      cmdline = calloc(total_len+1, sizeof(uint16_t));
      if (!cmdline) {
         err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
   }
   if (!err) {
      for (i=0, j=0; i<num_args; i++) {
         if (i > 0) {
            cmdline[j++] = ' ';
         }
         needs_quotes = 0;
         for (p = args[i]; *p; p++) {
            if (*p == ' ' || *p == '"') {
               needs_quotes = 1;
               break;
            }
         }
         if (needs_quotes) {
            cmdline[j++] = '"';
         }
         for (p = args[i]; *p; p++) {
            if (*p == '"') {
               cmdline[j++] = '"';
               cmdline[j++] = '"';
               cmdline[j++] = '"';
               continue;
            }
            cmdline[j++] = *p;
         }
         if (needs_quotes) {
            cmdline[j++] = '"';
         }
      }
      cmdline[j] = 0;
      if (j != total_len) {
         err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
   }
   if (params[1].value) {
      if (!err) {
         err = fixscript_get_array_length(heap, params[1], &num_envs);
      }
      if (!err) {
         envs = calloc(num_envs*2, sizeof(uint16_t *));
         if (!envs) {
            err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
         }
      }
      if (!err) {
         pos = 0;
         idx = 0;
         while (fixscript_iter_hash(heap, params[1], &key_val, &value_val, &pos)) {
            err = fixscript_get_string_utf16(heap, key_val, 0, -1, &key, NULL);
            if (err) {
               break;
            }
            err = fixscript_get_string_utf16(heap, value_val, 0, -1, &value, NULL);
            if (err) {
               free(key);
               break;
            }
            envs[idx++] = key;
            envs[idx++] = value;
         }
         if (!err && idx != num_envs*2) {
            err = FIXSCRIPT_ERR_INVALID_ACCESS;
         }
      }
      if (!err) {
         total_len = 0;
         for (i=0; i<num_envs; i++) {
            total_len += wcslen(envs[i*2+0]);
            total_len++;
            total_len += wcslen(envs[i*2+1]);
            total_len++;
         }
         envblock = calloc(total_len+1, sizeof(uint16_t));
         if (!envblock) {
            err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
         }
      }
      if (!err) {
         for (i=0, j=0; i<num_envs; i++) {
            len = wcslen(envs[i*2+0]);
            memcpy(envblock+j, envs[i*2+0], len*2);
            j += len;
            envblock[j++] = '=';
            
            len = wcslen(envs[i*2+1]);
            memcpy(envblock+j, envs[i*2+1], len*2);
            j += len;
            envblock[j++] = 0;
         }
         envblock[j] = 0;
         if (j != total_len) {
            err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
         }
      }
      proc_flags |= CREATE_UNICODE_ENVIRONMENT;
   }
   if (params[2].value) {
      if (!err) {
         err = fixscript_get_string_utf16(heap, params[2], 0, -1, &path, NULL);
      }
   }
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   handle = calloc(1, sizeof(ProcessHandle));
   if (!handle) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   memset(&si, 0, sizeof(STARTUPINFO));
   si.cb = sizeof(STARTUPINFO);
   memset(&pi, 0, sizeof(PROCESS_INFORMATION));

   if (flags & (REDIR_IN | REDIR_OUT | REDIR_ERR)) {
      si.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
      si.wShowWindow = SW_HIDE;

      memset(&sa, 0, sizeof(SECURITY_ATTRIBUTES));
      sa.nLength = sizeof(SECURITY_ATTRIBUTES);
      sa.bInheritHandle = 1;

      num_pipes = 0;
      if (flags & REDIR_IN) num_pipes++;
      if (flags & REDIR_OUT) num_pipes++;
      if (flags & REDIR_ERR) num_pipes++;
      for (i=0; i<num_pipes; i++) {
         if (!CreatePipe(&pipes[i*2+0], &pipes[i*2+1], &sa, 0)) {
            while (i > 0) {
               i--;
               CloseHandle(pipes[i*2+0]);
               CloseHandle(pipes[i*2+1]);
            }
            *error = fixscript_create_error_string(heap, "can't create pipe");
            free(handle);
            goto error;
         }
      }

      if (flags & REDIR_IN) {
         num_pipes--;
         handle->in = pipes[num_pipes*2+1];
         in_other = pipes[num_pipes*2+0];
         si.hStdInput = pipes[num_pipes*2+0];
         SetHandleInformation(handle->in, HANDLE_FLAG_INHERIT, 0);
      }
      else {
         si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
      }

      if (flags & REDIR_OUT) {
         num_pipes--;
         handle->out = pipes[num_pipes*2+0];
         out_other = pipes[num_pipes*2+1];
         si.hStdOutput = pipes[num_pipes*2+1];
         if (flags & REDIR_MERGE_ERR) {
            si.hStdError = pipes[num_pipes*2+1];
         }
         SetHandleInformation(handle->out, HANDLE_FLAG_INHERIT, 0);
      }
      else {
         si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
      }

      if (flags & REDIR_ERR) {
         num_pipes--;
         handle->err = pipes[num_pipes*2+0];
         err_other = pipes[num_pipes*2+1];
         si.hStdError = pipes[num_pipes*2+1];
         SetHandleInformation(handle->err, HANDLE_FLAG_INHERIT, 0);
      }
      else {
         si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
      }
   }

   handle->refcnt = 1;
   handle->flags = flags;
   handle->process = ERROR_INVALID_HANDLE;

   handle_val = fixscript_create_value_handle(heap, HANDLE_TYPE_PROCESS, handle, process_handle_func);
   if (!handle_val.value) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   if (!CreateProcess(NULL, cmdline, NULL, NULL, TRUE, proc_flags, envblock, path, &si, &pi)) {
      *error = fixscript_create_error_string(heap, "can't start process");
      goto error;
   }
   
   if (flags & REDIR_IN) {
      CloseHandle(in_other);
   }
   if (flags & REDIR_OUT) {
      CloseHandle(out_other);
   }
   if (flags & REDIR_ERR) {
      CloseHandle(err_other);
   }

   CloseHandle(pi.hThread);
   handle->process = pi.hProcess;

   ret = handle_val;

error:
   free(arg_values);
   if (args) {
      for (i=0; i<num_args; i++) {
         free(args[i]);
      }
      free(args);
   }
   free(cmdline);
   if (envs) {
      for (i=0; i<num_envs*2; i++) {
         free(envs[i]);
      }
      free(envs);
   }
   free(envblock);
   free(path);
   return ret;
#elif defined(__wasm__)
   *error = fixscript_create_error_string(heap, "not supported");
   return fixscript_int(0);
#else
   ProcessHandle *handle = NULL;
   Value *arg_values = NULL;
   Value key_val, value_val, handle_val, ret = fixscript_int(0);
   int flags = params[3].value;
   char **args = NULL, **envs = NULL, *path = NULL, *key, *value;
   int num_args=0, num_envs=0;
   int i, err, pos, idx, own_handle=1;
   int in_fds[2], out_fds[2], err_fds[2];

   in_fds[0] = -1;
   in_fds[1] = -1;
   out_fds[0] = -1;
   out_fds[1] = -1;
   err_fds[0] = -1;
   err_fds[1] = -1;

   if (flags & REDIR_MERGE_ERR) {
      if ((flags & (REDIR_OUT | REDIR_ERR)) != REDIR_OUT) {
         *error = fixscript_create_error_string(heap, "incompatible flags");
         goto error;
      }
   }

   err = fixscript_get_array_length(heap, params[0], &num_args);
   if (!err) {
      if (num_args < 1) {
         *error = fixscript_create_error_string(heap, "must provide executable argument");
         goto error;
      }
      arg_values = calloc(num_args, sizeof(Value));
      if (!arg_values) {
         err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
   }
   if (!err) {
      err = fixscript_get_array_range(heap, params[0], 0, num_args, arg_values);
   }
   if (!err) {
      args = calloc(num_args+1, sizeof(char *));
      if (!args) {
         err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
   }
   if (!err) {
      for (i=0; i<num_args; i++) {
         err = fixscript_get_string(heap, arg_values[i], 0, -1, &args[i], NULL);
         if (err) break;
      }
   }
   if (params[1].value) {
      if (!err) {
         err = fixscript_get_array_length(heap, params[1], &num_envs);
      }
      if (!err) {
         envs = calloc(num_envs*2, sizeof(char *));
         if (!envs) {
            err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
         }
      }
      if (!err) {
         pos = 0;
         idx = 0;
         while (fixscript_iter_hash(heap, params[1], &key_val, &value_val, &pos)) {
            err = fixscript_get_string(heap, key_val, 0, -1, &key, NULL);
            if (err) {
               break;
            }
            err = fixscript_get_string(heap, value_val, 0, -1, &value, NULL);
            if (err) {
               free(key);
               break;
            }
            envs[idx++] = key;
            envs[idx++] = value;
         }
         if (!err && idx != num_envs*2) {
            err = FIXSCRIPT_ERR_INVALID_ACCESS;
         }
      }
   }
   if (params[2].value) {
      if (!err) {
         err = fixscript_get_string(heap, params[2], 0, -1, &path, NULL);
      }
   }
   if (!err) {
      handle = calloc(1, sizeof(ProcessHandle));
      if (!handle) {
         err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
   }
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   handle->refcnt = 1;
   handle->flags = flags;
   handle->ret_value = -1;
   handle->in_fd = -1;
   handle->out_fd = -1;
   handle->err_fd = -1;

   if (flags & REDIR_IN) {
      if (pipe(in_fds) < 0) {
         *error = fixscript_create_error_string(heap, "can't create pipe");
         goto error;
      }
   }

   if (flags & REDIR_OUT) {
      if (pipe(out_fds) < 0) {
         *error = fixscript_create_error_string(heap, "can't create pipe");
         goto error;
      }
   }

   if (flags & REDIR_ERR) {
      if (pipe(err_fds) < 0) {
         *error = fixscript_create_error_string(heap, "can't create pipe");
         goto error;
      }
   }

   handle->pid = fork();

   if (handle->pid < 0) {
      *error = fixscript_create_error_string(heap, "can't fork process");
      goto error;
   }

   handle_val = fixscript_create_value_handle(heap, HANDLE_TYPE_PROCESS, handle, process_handle_func);
   own_handle = 0;
   if (!handle_val.value) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   if (handle->pid == 0) {
      if (flags & REDIR_IN) {
         dup2(in_fds[0], 0);
         close(in_fds[0]);
         close(in_fds[1]);
      }
      if (flags & REDIR_OUT) {
         dup2(out_fds[1], 1);
         if (flags & REDIR_MERGE_ERR) {
            dup2(out_fds[1], 2);
         }
         close(out_fds[0]);
         close(out_fds[1]);
      }
      if (flags & REDIR_ERR) {
         dup2(err_fds[1], 2);
         close(err_fds[0]);
         close(err_fds[1]);
      }
      if (path) {
         chdir(path);
      }
      if (envs) {
#ifdef __linux__
         clearenv();
#else
         environ = NULL;
#endif
         for (i=0; i<num_envs; i++) {
            setenv(envs[i*2+0], envs[i*2+1], 0);
         }
      }
      execvp(args[0], args);
      exit(1);
   }

   if (flags & REDIR_IN) {
      handle->in_fd = in_fds[1];
      close(in_fds[0]);
      in_fds[0] = -1;
      in_fds[1] = -1;
   }

   if (flags & REDIR_OUT) {
      handle->out_fd = out_fds[0];
      close(out_fds[1]);
      out_fds[0] = -1;
      out_fds[1] = -1;
   }

   if (flags & REDIR_ERR) {
      handle->err_fd = err_fds[0];
      close(err_fds[1]);
      err_fds[0] = -1;
      err_fds[1] = -1;
   }

   ret = handle_val;

error:
   free(arg_values);
   if (args) {
      for (i=0; i<num_args; i++) {
         free(args[i]);
      }
      free(args);
   }
   if (envs) {
      for (i=0; i<num_envs*2; i++) {
         free(envs[i]);
      }
      free(envs);
   }
   free(path);
   if (own_handle) {
      free(handle);
   }
   if (in_fds[0] != -1) close(in_fds[0]);
   if (in_fds[1] != -1) close(in_fds[1]);
   if (out_fds[0] != -1) close(out_fds[0]);
   if (out_fds[1] != -1) close(out_fds[1]);
   if (err_fds[0] != -1) close(err_fds[0]);
   if (err_fds[1] != -1) close(err_fds[1]);
   return ret;
#endif
}


static Value native_process_read(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(__wasm__)
   *error = fixscript_create_error_string(heap, "not supported");
   return fixscript_int(0);
#else
   ProcessHandle *handle = NULL;
   char *buf;
   int timeout;
   int err;
#ifdef _WIN32
   DWORD ret;
   HANDLE h;
#else
   int ret;
   int fd = (int)(intptr_t)data;
#endif

   if (num_params == 5) {
      handle = fixscript_get_handle(heap, params[0], HANDLE_TYPE_PROCESS, NULL);
      if (!handle) {
         *error = fixscript_create_error_string(heap, "invalid process handle");
         return fixscript_int(0);
      }
      params++;

#ifdef _WIN32
      switch ((int)(intptr_t)data) {
         case 1: h = (handle->flags & REDIR_OUT)? handle->out : INVALID_HANDLE_VALUE; break;
         case 2: h = (handle->flags & REDIR_ERR)? handle->err : INVALID_HANDLE_VALUE; break;
         default: h = INVALID_HANDLE_VALUE;
      }

      if (h == INVALID_HANDLE_VALUE) {
         *error = fixscript_create_error_string(heap, "stream is not redirected");
         return fixscript_int(0);
      }
#else
      switch (fd) {
         case 1: fd = (handle->flags & REDIR_OUT)? handle->out_fd : -1; break;
         case 2: fd = (handle->flags & REDIR_ERR)? handle->err_fd : -1; break;
         default: fd = -1;
      }

      if (fd == -1) {
         *error = fixscript_create_error_string(heap, "stream is not redirected");
         return fixscript_int(0);
      }
#endif
   }
   else {
#ifdef _WIN32
      switch ((int)(intptr_t)data) {
         case 0: h = GetStdHandle(STD_INPUT_HANDLE); break;
         default: h = INVALID_HANDLE_VALUE;
      }
#endif
   }

   timeout = params[3].value;

   err = fixscript_lock_array(heap, params[0], params[1].value, params[2].value, (void **)&buf, 1, ACCESS_WRITE_ONLY);
   if (err) {
      return fixscript_error(heap, error, err);
   }

#ifdef _WIN32
   if (!ReadFile(h, buf, params[2].value, &ret, NULL)) {
      ret = -1;
   }
#else
   ret = read(fd, buf, params[2].value);
   if (ret < params[2].value) {
      params[2].value = ret >= 0? ret : 0;
   }
#endif

   fixscript_unlock_array(heap, params[0], params[1].value, params[2].value, (void **)&buf, 1, ACCESS_WRITE_ONLY);
   if (ret < 0) {
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }
   if (ret == 0) ret = -1;
   return fixscript_int(ret);
#endif /* __wasm__ */
}


static Value native_process_write(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(__wasm__)
   *error = fixscript_create_error_string(heap, "not supported");
   return fixscript_int(0);
#else
   ProcessHandle *handle = NULL;
   char *buf;
   int timeout;
   int err;
#ifdef _WIN32
   DWORD ret;
   HANDLE h;
#else
   int ret;
   int fd = (int)(intptr_t)data;
#endif

   if (num_params == 5) {
      handle = fixscript_get_handle(heap, params[0], HANDLE_TYPE_PROCESS, NULL);
      if (!handle) {
         *error = fixscript_create_error_string(heap, "invalid process handle");
         return fixscript_int(0);
      }
      params++;

#ifdef _WIN32
      switch ((int)(intptr_t)data) {
         case 0: h = (handle->flags & REDIR_IN)? handle->in : INVALID_HANDLE_VALUE; break;
         default: h = INVALID_HANDLE_VALUE;
      }

      if (h == INVALID_HANDLE_VALUE) {
         *error = fixscript_create_error_string(heap, "stream is not redirected");
         return fixscript_int(0);
      }
#else
      switch (fd) {
         case 0: fd = (handle->flags & REDIR_IN)? handle->in_fd : -1; break;
         default: fd = -1;
      }

      if (fd == -1) {
         *error = fixscript_create_error_string(heap, "stream is not redirected");
         return fixscript_int(0);
      }
#endif
   }
   else {
#ifdef _WIN32
      switch ((int)(intptr_t)data) {
         case 1: h = GetStdHandle(STD_OUTPUT_HANDLE); break;
         case 2: h = GetStdHandle(STD_ERROR_HANDLE); break;
         default: h = INVALID_HANDLE_VALUE;
      }
#endif
   }

   timeout = params[3].value;

   err = fixscript_lock_array(heap, params[0], params[1].value, params[2].value, (void **)&buf, 1, ACCESS_READ_ONLY);
   if (err) {
      return fixscript_error(heap, error, err);
   }

#ifdef _WIN32
   if (!WriteFile(h, buf, params[2].value, &ret, NULL)) {
      ret = -1;
   }
#else
   ret = write(fd, buf, params[2].value);
#endif

   fixscript_unlock_array(heap, params[0], params[1].value, params[2].value, (void **)&buf, 1, ACCESS_READ_ONLY);

   if (ret < 0) {
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }
   return fixscript_int(ret);
#endif /* __wasm__ */
}


static Value native_process_close(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(__wasm__)
   *error = fixscript_create_error_string(heap, "not supported");
   return fixscript_int(0);
#else
   ProcessHandle *handle = NULL;
#ifdef _WIN32
   DWORD ret;
   HANDLE h;
#else
   int ret;
   int fd = (int)(intptr_t)data;
#endif

   handle = fixscript_get_handle(heap, params[0], HANDLE_TYPE_PROCESS, NULL);
   if (!handle) {
      *error = fixscript_create_error_string(heap, "invalid process handle");
      return fixscript_int(0);
   }

#ifdef _WIN32
   switch ((int)(intptr_t)data) {
      case 0: h = (handle->flags & REDIR_IN)? handle->in : INVALID_HANDLE_VALUE; break;
      case 1: h = (handle->flags & REDIR_OUT)? handle->out : INVALID_HANDLE_VALUE; break;
      case 2: h = (handle->flags & REDIR_ERR)? handle->err : INVALID_HANDLE_VALUE; break;
      default: h = INVALID_HANDLE_VALUE;
   }

   if (h == INVALID_HANDLE_VALUE) {
      *error = fixscript_create_error_string(heap, "stream is not redirected");
      return fixscript_int(0);
   }
#else
   switch (fd) {
      case 0: fd = (handle->flags & REDIR_IN)? handle->in_fd : -1; break;
      case 1: fd = (handle->flags & REDIR_OUT)? handle->out_fd : -1; break;
      case 2: fd = (handle->flags & REDIR_ERR)? handle->err_fd : -1; break;
      default: fd = -1;
   }

   if (fd == -1) {
      *error = fixscript_create_error_string(heap, "stream is not redirected");
      return fixscript_int(0);
   }
#endif

#ifdef _WIN32
   ret = 0;
   if (!CloseHandle(h)) {
      ret = -1;
   }
#else
   ret = close(fd);
#endif

   if (ret < 0) {
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }
   return fixscript_int(0);
#endif /* __wasm__ */
}


static Value native_process_wait(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(__wasm__)
   *error = fixscript_create_error_string(heap, "not supported");
   return fixscript_int(0);
#else
   ProcessHandle *handle;
#ifdef _WIN32
   DWORD result = 0xFFFFFFFF;
#else
   int status;
#endif

   handle = fixscript_get_handle(heap, params[0], HANDLE_TYPE_PROCESS, NULL);
   if (!handle) {
      *error = fixscript_create_error_string(heap, "invalid process handle");
      return fixscript_int(0);
   }

#ifdef _WIN32
   WaitForSingleObject(handle->process, INFINITE);
   GetExitCodeProcess(handle->process, &result);
   return fixscript_int(result);
#else
   if (handle->pid == 0) {
      return fixscript_int(handle->ret_value);
   }

   if (waitpid(handle->pid, &status, 0) == handle->pid) {
      handle->pid = 0;
      if (WIFEXITED(status)) {
         handle->ret_value = WEXITSTATUS(status);
      }
      return fixscript_int(handle->ret_value);
   }

   handle->pid = 0;
   *error = fixscript_create_error_string(heap, "I/O error");
   return fixscript_int(0);
#endif
#endif /* __wasm__ */
}


static Value native_process_kill(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(__wasm__)
   *error = fixscript_create_error_string(heap, "not supported");
   return fixscript_int(0);
#else
   ProcessHandle *handle;
   int force = 0;

   handle = fixscript_get_handle(heap, params[0], HANDLE_TYPE_PROCESS, NULL);
   if (!handle) {
      *error = fixscript_create_error_string(heap, "invalid process handle");
      return fixscript_int(0);
   }

#ifdef _WIN32
   // TODO: send a message to GUI applications for non-forceful killing
   if (handle->process != INVALID_HANDLE_VALUE) {
      TerminateProcess(handle->process, 1);
   }
   return fixscript_int(0);
#else
   if (handle->pid == 0) {
      return fixscript_int(0);
   }

   if (num_params == 2) {
      force = params[1].value;
   }

   kill(handle->pid, force? SIGKILL : SIGTERM);
   return fixscript_int(0);
#endif
#endif /* __wasm__ */
}


static Value native_process_is_running(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(__wasm__)
   *error = fixscript_create_error_string(heap, "not supported");
   return fixscript_int(0);
#else
   ProcessHandle *handle;
#ifdef _WIN32
   DWORD result;
#else
   pid_t ret;
   int status;
#endif

   handle = fixscript_get_handle(heap, params[0], HANDLE_TYPE_PROCESS, NULL);
   if (!handle) {
      *error = fixscript_create_error_string(heap, "invalid process handle");
      return fixscript_int(0);
   }

#ifdef _WIN32
   if (GetExitCodeProcess(handle->process, &result) && result == STILL_ACTIVE) {
      return fixscript_int(1);
   }
   return fixscript_int(0);
#else
   if (handle->pid == 0) {
      return fixscript_int(0);
   }

   ret = waitpid(handle->pid, &status, WNOHANG);
   if (ret == 0) {
      return fixscript_int(1);
   }
   if (ret == handle->pid) {
      handle->pid = 0;
      if (WIFEXITED(status)) {
         handle->ret_value = WEXITSTATUS(status);
      }
      return fixscript_int(0);
   }

   handle->pid = 0;
   *error = fixscript_create_error_string(heap, "I/O error");
   return fixscript_int(0);
#endif
#endif /* __wasm__ */
}


static Value native_process_get_id(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(__wasm__)
   *error = fixscript_create_error_string(heap, "not supported");
   return fixscript_int(0);
#else
   ProcessHandle *handle;
#ifdef _WIN32
   DWORD ret;
#endif

   handle = fixscript_get_handle(heap, params[0], HANDLE_TYPE_PROCESS, NULL);
   if (!handle) {
      *error = fixscript_create_error_string(heap, "invalid process handle");
      return fixscript_int(0);
   }

#ifdef _WIN32
   if (handle->process != ERROR_INVALID_HANDLE) {
      // TODO: add proper support for Windows 2000, removed for now because it's not used in FixBrowser
      //ret = GetProcessId(handle->process);
      ret = 0;
      *error = fixscript_create_error_string(heap, "not supported");
      return fixscript_int(ret);
   }
   return fixscript_int(0);
#else
   return fixscript_int(handle->pid);
#endif
#endif /* __wasm__ */
}


static Value native_process_get_current_environment(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(_WIN32)
   Value map, key_val, value_val;
   uint16_t *env, *p, *key, *value;
   int err;

   map = fixscript_create_hash(heap);
   if (!map.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   env = GetEnvironmentStrings();
   for (p=env; *p; p++) {
      key = p;
      value = NULL;
      for (; *p; p++) {
         if (*p == '=') {
            value = p+1;
            break;
         }
      }
      while (*p) {
         p++;
      }
      if (value) {
         key_val = fixscript_create_string_utf16(heap, key, value - key - 1);
         value_val = fixscript_create_string_utf16(heap, value, -1);
      }
      else {
         key_val = fixscript_create_string_utf16(heap, key, -1);
         value_val = fixscript_create_string_utf16(heap, L"", -1);
      }
      if (!key_val.value || !value_val.value) {
         FreeEnvironmentStrings(env);
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
      err = fixscript_set_hash_elem(heap, map, key_val, value_val);
      if (err) {
         FreeEnvironmentStrings(env);
         return fixscript_error(heap, error, err);
      }
   }
   FreeEnvironmentStrings(env);
   return map;
#elif defined(__wasm__)
   *error = fixscript_create_error_string(heap, "not supported");
   return fixscript_int(0);
#else
   Value map, key_val, value_val;
   const char **p, *s, *key, *value;
   int err;

   map = fixscript_create_hash(heap);
   if (!map.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   for (p=(const char **)environ; *p; p++) {
      s = *p;
      key = s;
      value = NULL;
      for (; *s; s++) {
         if (*s == '=') {
            value = s+1;
            break;
         }
      }
      if (value) {
         key_val = fixscript_create_string(heap, key, value - key - 1);
         value_val = fixscript_create_string(heap, value, -1);
      }
      else {
         key_val = fixscript_create_string(heap, key, -1);
         value_val = fixscript_create_string(heap, "", -1);
      }
      if (!key_val.value || !value_val.value) {
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
      err = fixscript_set_hash_elem(heap, map, key_val, value_val);
      if (err) {
         return fixscript_error(heap, error, err);
      }
   }
   return map;
#endif
}


static Value native_clock_get_time(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(__wasm__)
   // TODO
   return fixscript_int(0);
#else
   int micro = ((intptr_t)data == 1);
   union {
      int64_t time;
      int32_t i[2];
   } u;

#ifdef _WIN32
   FILETIME ft;

   GetSystemTimeAsFileTime(&ft);
   u.i[0] = ft.dwLowDateTime;
   u.i[1] = ft.dwHighDateTime;
   u.time = ((u.time + 5) / 10LL) - 11644473600000000LL;
#else
   struct timeval tv;

   if (gettimeofday(&tv, NULL) != 0) {
      tv.tv_sec = 0;
      tv.tv_usec = 0;
   }

   if (sizeof(time_t) == 4) {
      // treat time_t as unsigned to give chance for possible unsigned hack once year 2038 happens:
      u.time = ((uint32_t)tv.tv_sec) * 1000000LL + tv.tv_usec;
   }
   else {
      u.time = tv.tv_sec * 1000000LL + tv.tv_usec;
   }
#endif

   if (!micro) {
      u.time = (u.time + 500) / 1000;
   }

   *error = fixscript_int(u.i[1]);
   return fixscript_int(u.i[0]);
#endif /* __wasm__ */
}


static Value native_monotonic_get_time(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(__wasm__)
   // TODO
   return fixscript_int(0);
#else
   int micro = ((intptr_t)data == 1);
   uint64_t time;

#if defined(_WIN32)
   uint64_t freq, cnt;
   QueryPerformanceFrequency((LARGE_INTEGER *)&freq);
   QueryPerformanceCounter((LARGE_INTEGER *)&cnt);
   time = cnt * 1000000 / freq;
#elif defined(__linux__)
   struct timespec ts;
   
   if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
      ts.tv_sec = 0;
      ts.tv_nsec = 0;
   }

   time = ts.tv_sec * 1000000LL + (ts.tv_nsec + 500) / 1000;
#else
   struct timeval tv;

   if (gettimeofday(&tv, NULL) != 0) {
      tv.tv_sec = 0;
      tv.tv_usec = 0;
   }

   time = tv.tv_sec * 1000000LL + tv.tv_usec;
#endif

   if (micro) {
      return fixscript_int(time);
   }
   else {
      return fixscript_int((time + 500) / 1000);
   }
#endif /* __wasm__ */
}


static Value native_array_create_view(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   SharedArrayHandle *sah;
   Value value;
   char *ptr;
   int arr_len, arr_elem_size;
   int off, len, elem_size;
   int created;
   
   sah = fixscript_get_shared_array_handle(heap, params[0], -1, NULL);
   if (!sah) {
      *error = fixscript_create_error_string(heap, "invalid shared array");
      return fixscript_int(0);
   }

   ptr = fixscript_get_shared_array_handle_data(sah, &arr_len, &arr_elem_size, NULL, -1, NULL);

   if (num_params >= 3) {
      off = params[1].value;
      len = params[2].value;
   }
   else {
      off = 0;
      len = arr_len;
   }

   elem_size = num_params == 2? params[1].value : num_params == 4? params[3].value : arr_elem_size;
   if (elem_size != 1 && elem_size != 2 && elem_size != 4) {
      *error = fixscript_create_error_string(heap, "element size must be 1, 2 or 4");
      return fixscript_int(0);
   }

   if (off < 0 || off >= arr_len || len < 0 || (int64_t)off + (int64_t)len > arr_len) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_BOUNDS);
   }

   ptr += (intptr_t)off * arr_elem_size;
   if ((((intptr_t)ptr) & (elem_size-1)) != 0) {
      *error = fixscript_create_error_string(heap, "unaligned offset");
      return fixscript_int(0);
   }

   value = fixscript_get_shared_array(heap, -1, ptr, len, elem_size, sah);
   if (value.value) {
      return value;
   }

   fixscript_ref_shared_array(sah);
   value = fixscript_create_or_get_shared_array(heap, -1, ptr, (intptr_t)len * arr_elem_size / elem_size, elem_size, (HandleFreeFunc)fixscript_unref_shared_array, sah, &created);
   if (!value.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   if (!created) {
      fixscript_unref_shared_array(sah);
   }
   return value;
}


typedef struct {
   char *start;
   char *cur, *end;
} Buffer;


static int buffer_expand(Buffer *buf)
{
   char *new_data;
   int new_size, pos;

   new_size = buf->end - buf->start;
   if (new_size >= (1<<30)) {
      return 0;
   }

   new_size <<= 1;
   pos = buf->cur - buf->start;
   new_data = realloc(buf->start, new_size);
   if (!new_data) {
      return 0;
   }

   buf->start = new_data;
   buf->cur = new_data + pos;
   buf->end = new_data + new_size;
   return 1;
}


static inline int buffer_append_byte(Buffer *buf, uint8_t value)
{
   if (buf->cur == buf->end) {
      if (!buffer_expand(buf)) return 0;
   }
   *buf->cur++ = value;
   return 1;
}


static inline int buffer_append_short(Buffer *buf, uint16_t value)
{
   if (buf->end - buf->cur < 2) {
      if (!buffer_expand(buf)) return 0;
   }
   memcpy(buf->cur, &value, 2);
   buf->cur += 2;
   return 1;
}


static inline int buffer_append_int(Buffer *buf, int value)
{
   if (buf->end - buf->cur < 4) {
      if (!buffer_expand(buf)) return 0;
   }
   memcpy(buf->cur, &value, 4);
   buf->cur += 4;
   return 1;
}


static inline int buffer_append_length(Buffer *buf, int type, int len)
{
   if (len <= 12) {
      if (!buffer_append_byte(buf, type | (len << 4))) return 0;
   }
   else if (len <= 0xFF) {
      if (!buffer_append_byte(buf, type | (13 << 4))) return 0;
      if (!buffer_append_byte(buf, len)) return 0;
   }
   else if (len <= 0xFFFF) {
      if (!buffer_append_byte(buf, type | (14 << 4))) return 0;
      if (!buffer_append_short(buf, len)) return 0;
   }
   else {
      if (!buffer_append_byte(buf, type | (15 << 4))) return 0;
      if (!buffer_append_int(buf, len)) return 0;
   }
   return 1;
}


static inline int buffer_read_byte(Buffer *buf, uint8_t *value)
{
   if (buf->cur == buf->end) {
      return 0;
   }
   *value = *buf->cur++;
   return 1;
}


static inline int buffer_read_short(Buffer *buf, uint16_t *value)
{
   if (buf->end - buf->cur < 2) {
      return 0;
   }
   memcpy(value, buf->cur, 2);
   buf->cur += 2;
   return 1;
}


static inline int buffer_read_int(Buffer *buf, int *value)
{
   if (buf->end - buf->cur < 4) {
      return 0;
   }
   memcpy(value, buf->cur, 4);
   buf->cur += 4;
   return 1;
}


static inline int buffer_read_length(Buffer *buf, uint8_t type, int *len)
{
   uint8_t len8;
   uint16_t len16;

   if ((type >> 4) <= 12) {
      *len = type >> 4;
      return 1;
   }
   if ((type >> 4) == 13) {
      if (!buffer_read_byte(buf, &len8)) return 0;
      *len = len8;
      return 1;
   }
   if ((type >> 4) == 14) {
      if (!buffer_read_short(buf, &len16)) return 0;
      *len = len16;
      return 1;
   }
   return buffer_read_int(buf, len);
}


enum {
   SER_ZERO         = 0,
   SER_BYTE         = 1,
   SER_SHORT        = 2,
   SER_INT          = 3,
   SER_FLOAT        = 4,
   SER_FLOAT_ZERO   = 5,
   SER_REF          = 6,
   SER_REF_SHORT    = 7,
   SER_ARRAY        = 8,
   SER_ARRAY_BYTE   = 9,
   SER_ARRAY_SHORT  = 10,
   SER_ARRAY_INT    = 11,
   SER_STRING_BYTE  = 12,
   SER_STRING_SHORT = 13,
   SER_STRING_INT   = 14,
   SER_HASH         = 15
};

typedef struct {
   char *key, *value;
   int key_len, value_len;
} SortedEntry;


static int compare_serialized_values(Buffer *buf1, Buffer *buf2, int remaining)
{
   uint8_t type1, type2;

   while (remaining-- > 0) {
      if (buf1->cur == buf1->end) return buf2->cur == buf2->end? 0 : -1;
      if (buf2->cur == buf2->end) return +1;

      type1 = *(buf1->cur++);
      type2 = *(buf2->cur++);

      switch (type1 & 0x0F) {
         case SER_ZERO:
            switch (type2 & 0x0F) {
               case SER_ZERO:
                  continue;

               case SER_BYTE:
               case SER_SHORT:
                  return -1;

               case SER_INT:
                  if (buf2->end - buf2->cur < 4) goto error;
                  if (buf2->cur[3] & 0x80) return +1;
                  return -1;

               case SER_FLOAT:
               case SER_FLOAT_ZERO:
               case SER_ARRAY:
               case SER_ARRAY_BYTE:
               case SER_ARRAY_SHORT:
               case SER_ARRAY_INT:
               case SER_STRING_BYTE:
               case SER_STRING_SHORT:
               case SER_STRING_INT:
               case SER_HASH:
                  return -1;
            }
            goto error;

         case SER_BYTE:
            switch (type2 & 0x0F) {
               case SER_ZERO:
                  return +1;

               case SER_BYTE: {
                  uint8_t val1, val2;
                  if (!buffer_read_byte(buf1, &val1)) goto error;
                  if (!buffer_read_byte(buf2, &val2)) goto error;
                  if (val1 == val2) continue;
                  return (int)val1 - (int)val2;
               }

               case SER_SHORT:
                  return -1;

               case SER_INT:
                  if (buf2->end - buf2->cur < 4) goto error;
                  if (buf2->cur[3] & 0x80) return +1;
                  return -1;

               case SER_FLOAT:
               case SER_FLOAT_ZERO:
               case SER_ARRAY:
               case SER_ARRAY_BYTE:
               case SER_ARRAY_SHORT:
               case SER_ARRAY_INT:
               case SER_STRING_BYTE:
               case SER_STRING_SHORT:
               case SER_STRING_INT:
               case SER_HASH:
                  return -1;
            }
            goto error;

         case SER_SHORT:
            switch (type2 & 0x0F) {
               case SER_ZERO:
               case SER_BYTE:
                  return +1;

               case SER_SHORT: {
                  uint16_t val1, val2;
                  if (!buffer_read_short(buf1, &val1)) goto error;
                  if (!buffer_read_short(buf2, &val2)) goto error;
                  if (val1 == val2) continue;
                  return (int)val1 - (int)val2;
               }

               case SER_INT:
                  if (buf2->end - buf2->cur < 4) goto error;
                  if (buf2->cur[3] & 0x80) return +1;
                  return -1;

               case SER_FLOAT:
               case SER_FLOAT_ZERO:
               case SER_ARRAY:
               case SER_ARRAY_BYTE:
               case SER_ARRAY_SHORT:
               case SER_ARRAY_INT:
               case SER_STRING_BYTE:
               case SER_STRING_SHORT:
               case SER_STRING_INT:
               case SER_HASH:
                  return -1;
            }
            goto error;

         case SER_INT:
            switch (type2 & 0x0F) {
               case SER_ZERO:
               case SER_BYTE:
               case SER_SHORT:
                  if (buf1->end - buf1->cur < 4) goto error;
                  if (buf1->cur[3] & 0x80) return -1;
                  return +1;

               case SER_INT: {
                  int val1, val2;
                  if (!buffer_read_int(buf1, &val1)) goto error;
                  if (!buffer_read_int(buf2, &val2)) goto error;
                  if (val1 == val2) continue;
                  return val1 < val2? -1 : +1;
               }

               case SER_FLOAT:
               case SER_FLOAT_ZERO:
               case SER_ARRAY:
               case SER_ARRAY_BYTE:
               case SER_ARRAY_SHORT:
               case SER_ARRAY_INT:
               case SER_STRING_BYTE:
               case SER_STRING_SHORT:
               case SER_STRING_INT:
               case SER_HASH:
                  return -1;
            }
            goto error;

         case SER_FLOAT:
            switch (type2 & 0x0F) {
               case SER_ZERO:
               case SER_BYTE:
               case SER_SHORT:
               case SER_INT:
                  return +1;

               case SER_FLOAT: {
                  int val1, val2;
                  if (!buffer_read_int(buf1, &val1)) goto error;
                  if (!buffer_read_int(buf2, &val2)) goto error;
                  if (val1 == val2) continue;
                  if (val1 & 0x80000000) {
                     if (val2 & 0x80000000) {
                        return (val2 & 0x7FFFFFFF) - (val1 & 0x7FFFFFFF);
                     }
                     return -1;
                  }
                  if (val2 & 0x80000000) {
                     return +1;
                  }
                  return (val1 & 0x7FFFFFFF) - (val2 & 0x7FFFFFFF);
               }

               case SER_FLOAT_ZERO: {
                  if (buf1->end - buf1->cur < 4) goto error;
                  if (buf1->cur[3] & 0x80) return -1;
                  return +1;
               }

               case SER_ARRAY:
               case SER_ARRAY_BYTE:
               case SER_ARRAY_SHORT:
               case SER_ARRAY_INT:
               case SER_STRING_BYTE:
               case SER_STRING_SHORT:
               case SER_STRING_INT:
               case SER_HASH:
                  return -1;
            }
            goto error;

         case SER_FLOAT_ZERO:
            switch (type2 & 0x0F) {
               case SER_ZERO:
               case SER_BYTE:
               case SER_SHORT:
               case SER_INT:
                  return +1;

               case SER_FLOAT: {
                  if (buf2->end - buf2->cur < 4) goto error;
                  if (buf2->cur[3] & 0x80) return +1;
                  return -1;
               }

               case SER_FLOAT_ZERO:
                  continue;

               case SER_ARRAY:
               case SER_ARRAY_BYTE:
               case SER_ARRAY_SHORT:
               case SER_ARRAY_INT:
               case SER_STRING_BYTE:
               case SER_STRING_SHORT:
               case SER_STRING_INT:
               case SER_HASH:
                  return -1;
            }
            goto error;

         case SER_ARRAY:
            switch (type2 & 0x0F) {
               case SER_ZERO:
               case SER_BYTE:
               case SER_SHORT:
               case SER_INT:
               case SER_FLOAT:
               case SER_FLOAT_ZERO:
                  return +1;

               case SER_ARRAY: {
                  int len1, len2, ret;

                  if (!buffer_read_length(buf1, type1, &len1)) goto error;
                  if (!buffer_read_length(buf2, type2, &len2)) goto error;

                  ret = compare_serialized_values(buf1, buf2, len1 < len2? len1 : len2);
                  if (ret != 0) return ret;
                  if (len1 == len2) {
                     continue;
                  }
                  return len1 - len2;
               }

               case SER_ARRAY_BYTE: {
                  uint8_t val2;
                  int i, len1, len2, len;

                  if (!buffer_read_length(buf1, type1, &len1)) goto error;
                  if (!buffer_read_length(buf2, type2, &len2)) goto error;
                  len = len1 < len2? len1 : len2;

                  for (i=0; i<len; i++) {
                     if (!buffer_read_byte(buf1, &type1)) goto error;
                     if (!buffer_read_byte(buf2, &val2)) goto error;
                     switch (type1 & 0x0F) {
                        case SER_ZERO:
                           if (val2 > 0) return -1;
                           continue;

                        case SER_BYTE: {
                           uint8_t val1;
                           if (!buffer_read_byte(buf1, &val1)) goto error;
                           if (val1 != val2) {
                              return (int)val1 - (int)val2;
                           }
                           continue;
                        }

                        case SER_SHORT: {
                           uint16_t val1;
                           if (!buffer_read_short(buf1, &val1)) goto error;
                           if (val1 != val2) {
                              return (int)val1 - (int)val2;
                           }
                           continue;
                        }
                        
                        case SER_INT: {
                           int val1;
                           if (!buffer_read_int(buf1, &val1)) goto error;
                           if (val1 != val2) {
                              return val1 < val2? -1 : +1;
                           }
                           continue;
                        }

                        case SER_FLOAT:
                        case SER_FLOAT_ZERO:
                        case SER_ARRAY:
                        case SER_ARRAY_BYTE:
                        case SER_ARRAY_SHORT:
                        case SER_ARRAY_INT:
                        case SER_STRING_BYTE:
                        case SER_STRING_SHORT:
                        case SER_STRING_INT:
                        case SER_HASH:
                           return +1;
                     }
                     goto error;
                  }
                  if (len1 == len2) {
                     continue;
                  }
                  return len1 - len2;
               }

               case SER_ARRAY_SHORT: {
                  uint16_t val2;
                  int i, len1, len2, len;

                  if (!buffer_read_length(buf1, type1, &len1)) goto error;
                  if (!buffer_read_length(buf2, type2, &len2)) goto error;
                  len = len1 < len2? len1 : len2;

                  for (i=0; i<len; i++) {
                     if (!buffer_read_byte(buf1, &type1)) goto error;
                     if (!buffer_read_short(buf2, &val2)) goto error;
                     switch (type1 & 0x0F) {
                        case SER_ZERO:
                           if (val2 > 0) return -1;
                           continue;

                        case SER_BYTE: {
                           uint8_t val1;
                           if (!buffer_read_byte(buf1, &val1)) goto error;
                           if (val1 != val2) {
                              return (int)val1 - (int)val2;
                           }
                           continue;
                        }

                        case SER_SHORT: {
                           uint16_t val1;
                           if (!buffer_read_short(buf1, &val1)) goto error;
                           if (val1 != val2) {
                              return (int)val1 - (int)val2;
                           }
                           continue;
                        }
                        
                        case SER_INT: {
                           int val1;
                           if (!buffer_read_int(buf1, &val1)) goto error;
                           if (val1 != val2) {
                              return val1 < val2? -1 : +1;
                           }
                           continue;
                        }

                        case SER_FLOAT:
                        case SER_FLOAT_ZERO:
                        case SER_ARRAY:
                        case SER_ARRAY_BYTE:
                        case SER_ARRAY_SHORT:
                        case SER_ARRAY_INT:
                        case SER_STRING_BYTE:
                        case SER_STRING_SHORT:
                        case SER_STRING_INT:
                        case SER_HASH:
                           return +1;
                     }
                     goto error;
                  }
                  if (len1 == len2) {
                     continue;
                  }
                  return len1 - len2;
               }

               case SER_ARRAY_INT: {
                  int val2;
                  int i, len1, len2, len;

                  if (!buffer_read_length(buf1, type1, &len1)) goto error;
                  if (!buffer_read_length(buf2, type2, &len2)) goto error;
                  len = len1 < len2? len1 : len2;

                  for (i=0; i<len; i++) {
                     if (!buffer_read_byte(buf1, &type1)) goto error;
                     if (!buffer_read_int(buf2, &val2)) goto error;
                     switch (type1 & 0x0F) {
                        case SER_ZERO:
                           if (val2 < 0) return +1;
                           if (val2 > 0) return -1;
                           continue;

                        case SER_BYTE: {
                           uint8_t val1;
                           if (!buffer_read_byte(buf1, &val1)) goto error;
                           if (val1 != val2) {
                              return val1 < val2? -1 : +1;
                           }
                           continue;
                        }

                        case SER_SHORT: {
                           uint16_t val1;
                           if (!buffer_read_short(buf1, &val1)) goto error;
                           if (val1 != val2) {
                              return val1 < val2? -1 : +1;
                           }
                           continue;
                        }
                        
                        case SER_INT: {
                           int val1;
                           if (!buffer_read_int(buf1, &val1)) goto error;
                           if (val1 != val2) {
                              return val1 < val2? -1 : +1;
                           }
                           continue;
                        }

                        case SER_FLOAT:
                        case SER_FLOAT_ZERO:
                        case SER_ARRAY:
                        case SER_ARRAY_BYTE:
                        case SER_ARRAY_SHORT:
                        case SER_ARRAY_INT:
                        case SER_STRING_BYTE:
                        case SER_STRING_SHORT:
                        case SER_STRING_INT:
                        case SER_HASH:
                           return +1;
                     }
                     goto error;
                  }
                  if (len1 == len2) {
                     continue;
                  }
                  return len1 - len2;
               }

               case SER_STRING_BYTE:
               case SER_STRING_SHORT:
               case SER_STRING_INT:
               case SER_HASH:
                  return -1;
            }
            goto error;

         case SER_ARRAY_BYTE:
            switch (type2 & 0x0F) {
               case SER_ZERO:
               case SER_BYTE:
               case SER_SHORT:
               case SER_INT:
               case SER_FLOAT:
               case SER_FLOAT_ZERO:
                  return +1;

               case SER_ARRAY: {
                  uint8_t val1;
                  int i, len1, len2, len;

                  if (!buffer_read_length(buf1, type1, &len1)) goto error;
                  if (!buffer_read_length(buf2, type2, &len2)) goto error;
                  len = len1 < len2? len1 : len2;

                  for (i=0; i<len; i++) {
                     if (!buffer_read_byte(buf1, &val1)) goto error;
                     if (!buffer_read_byte(buf2, &type2)) goto error;
                     switch (type2 & 0x0F) {
                        case SER_ZERO:
                           if (val1 > 0) return +1;
                           continue;

                        case SER_BYTE: {
                           uint8_t val2;
                           if (!buffer_read_byte(buf2, &val2)) goto error;
                           if (val1 != val2) {
                              return (int)val1 - (int)val2;
                           }
                           continue;
                        }

                        case SER_SHORT: {
                           uint16_t val2;
                           if (!buffer_read_short(buf2, &val2)) goto error;
                           if (val1 != val2) {
                              return (int)val1 - (int)val2;
                           }
                           continue;
                        }

                        case SER_INT: {
                           int val2;
                           if (!buffer_read_int(buf2, &val2)) goto error;
                           if (val1 != val2) {
                              return val1 < val2? -1 : +1;
                           }
                           continue;
                        }

                        case SER_FLOAT:
                        case SER_FLOAT_ZERO:
                        case SER_ARRAY:
                        case SER_ARRAY_BYTE:
                        case SER_ARRAY_SHORT:
                        case SER_ARRAY_INT:
                        case SER_STRING_BYTE:
                        case SER_STRING_SHORT:
                        case SER_STRING_INT:
                        case SER_HASH:
                           return -1;
                     }
                     goto error;
                  }
                  if (len1 == len2) {
                     continue;
                  }
                  return len1 - len2;
               }

               case SER_ARRAY_BYTE: {
                  uint8_t val1, val2;
                  int i, len1, len2, len;

                  if (!buffer_read_length(buf1, type1, &len1)) goto error;
                  if (!buffer_read_length(buf2, type2, &len2)) goto error;
                  len = len1 < len2? len1 : len2;

                  for (i=0; i<len; i++) {
                     if (!buffer_read_byte(buf1, &val1)) goto error;
                     if (!buffer_read_byte(buf2, &val2)) goto error;
                     if (val1 != val2) {
                        return (int)val1 - (int)val2;
                     }
                  }
                  if (len1 == len2) {
                     continue;
                  }
                  return len1 - len2;
               }

               case SER_ARRAY_SHORT: {
                  uint8_t val1;
                  uint16_t val2;
                  int i, len1, len2, len;

                  if (!buffer_read_length(buf1, type1, &len1)) goto error;
                  if (!buffer_read_length(buf2, type2, &len2)) goto error;
                  len = len1 < len2? len1 : len2;

                  for (i=0; i<len; i++) {
                     if (!buffer_read_byte(buf1, &val1)) goto error;
                     if (!buffer_read_short(buf2, &val2)) goto error;
                     if (val1 != val2) {
                        return (int)val1 - (int)val2;
                     }
                  }
                  if (len1 == len2) {
                     continue;
                  }
                  return len1 - len2;
               }

               case SER_ARRAY_INT: {
                  uint8_t val1;
                  int val2;
                  int i, len1, len2, len;

                  if (!buffer_read_length(buf1, type1, &len1)) goto error;
                  if (!buffer_read_length(buf2, type2, &len2)) goto error;
                  len = len1 < len2? len1 : len2;

                  for (i=0; i<len; i++) {
                     if (!buffer_read_byte(buf1, &val1)) goto error;
                     if (!buffer_read_int(buf2, &val2)) goto error;
                     if (val1 != val2) {
                        return val1 < val2? -1 : +1;
                     }
                  }
                  if (len1 == len2) {
                     continue;
                  }
                  return len1 - len2;
               }

               case SER_STRING_BYTE:
               case SER_STRING_SHORT:
               case SER_STRING_INT:
               case SER_HASH:
                  return -1;
            }
            goto error;

         case SER_ARRAY_SHORT:
            switch (type2 & 0x0F) {
               case SER_ZERO:
               case SER_BYTE:
               case SER_SHORT:
               case SER_INT:
               case SER_FLOAT:
               case SER_FLOAT_ZERO:
                  return +1;

               case SER_ARRAY: {
                  uint16_t val1;
                  int i, len1, len2, len;

                  if (!buffer_read_length(buf1, type1, &len1)) goto error;
                  if (!buffer_read_length(buf2, type2, &len2)) goto error;
                  len = len1 < len2? len1 : len2;

                  for (i=0; i<len; i++) {
                     if (!buffer_read_short(buf1, &val1)) goto error;
                     if (!buffer_read_byte(buf2, &type2)) goto error;
                     switch (type2 & 0x0F) {
                        case SER_ZERO:
                           if (val1 > 0) return +1;
                           continue;

                        case SER_BYTE: {
                           uint8_t val2;
                           if (!buffer_read_byte(buf2, &val2)) goto error;
                           if (val1 != val2) {
                              return (int)val1 - (int)val2;
                           }
                           continue;
                        }

                        case SER_SHORT: {
                           uint16_t val2;
                           if (!buffer_read_short(buf2, &val2)) goto error;
                           if (val1 != val2) {
                              return (int)val1 - (int)val2;
                           }
                           continue;
                        }

                        case SER_INT: {
                           int val2;
                           if (!buffer_read_int(buf2, &val2)) goto error;
                           if (val1 != val2) {
                              return val1 < val2? -1 : +1;
                           }
                           continue;
                        }

                        case SER_FLOAT:
                        case SER_FLOAT_ZERO:
                        case SER_ARRAY:
                        case SER_ARRAY_BYTE:
                        case SER_ARRAY_SHORT:
                        case SER_ARRAY_INT:
                        case SER_STRING_BYTE:
                        case SER_STRING_SHORT:
                        case SER_STRING_INT:
                        case SER_HASH:
                           return -1;
                     }
                     goto error;
                  }
                  if (len1 == len2) {
                     continue;
                  }
                  return len1 - len2;
               }

               case SER_ARRAY_BYTE: {
                  uint16_t val1;
                  uint8_t val2;
                  int i, len1, len2, len;

                  if (!buffer_read_length(buf1, type1, &len1)) goto error;
                  if (!buffer_read_length(buf2, type2, &len2)) goto error;
                  len = len1 < len2? len1 : len2;

                  for (i=0; i<len; i++) {
                     if (!buffer_read_short(buf1, &val1)) goto error;
                     if (!buffer_read_byte(buf2, &val2)) goto error;
                     if (val1 != val2) {
                        return (int)val1 - (int)val2;
                     }
                  }
                  if (len1 == len2) {
                     continue;
                  }
                  return len1 - len2;
               }

               case SER_ARRAY_SHORT: {
                  uint16_t val1;
                  uint16_t val2;
                  int i, len1, len2, len;

                  if (!buffer_read_length(buf1, type1, &len1)) goto error;
                  if (!buffer_read_length(buf2, type2, &len2)) goto error;
                  len = len1 < len2? len1 : len2;

                  for (i=0; i<len; i++) {
                     if (!buffer_read_short(buf1, &val1)) goto error;
                     if (!buffer_read_short(buf2, &val2)) goto error;
                     if (val1 != val2) {
                        return (int)val1 - (int)val2;
                     }
                  }
                  if (len1 == len2) {
                     continue;
                  }
                  return len1 - len2;
               }

               case SER_ARRAY_INT: {
                  uint16_t val1;
                  int val2;
                  int i, len1, len2, len;

                  if (!buffer_read_length(buf1, type1, &len1)) goto error;
                  if (!buffer_read_length(buf2, type2, &len2)) goto error;
                  len = len1 < len2? len1 : len2;

                  for (i=0; i<len; i++) {
                     if (!buffer_read_short(buf1, &val1)) goto error;
                     if (!buffer_read_int(buf2, &val2)) goto error;
                     if (val1 != val2) {
                        return val1 < val2? -1 : +1;
                     }
                  }
                  if (len1 == len2) {
                     continue;
                  }
                  return len1 - len2;
               }

               case SER_STRING_BYTE:
               case SER_STRING_SHORT:
               case SER_STRING_INT:
               case SER_HASH:
                  return -1;
            }
            goto error;

         case SER_ARRAY_INT:
            switch (type2 & 0x0F) {
               case SER_ZERO:
               case SER_BYTE:
               case SER_SHORT:
               case SER_INT:
               case SER_FLOAT:
               case SER_FLOAT_ZERO:
                  return +1;

               case SER_ARRAY: {
                  int val1;
                  int i, len1, len2, len;

                  if (!buffer_read_length(buf1, type1, &len1)) goto error;
                  if (!buffer_read_length(buf2, type2, &len2)) goto error;
                  len = len1 < len2? len1 : len2;

                  for (i=0; i<len; i++) {
                     if (!buffer_read_int(buf1, &val1)) goto error;
                     if (!buffer_read_byte(buf2, &type2)) goto error;
                     switch (type2 & 0x0F) {
                        case SER_ZERO:
                           if (val1 < 0) return -1;
                           if (val1 > 0) return +1;
                           continue;

                        case SER_BYTE: {
                           uint8_t val2;
                           if (!buffer_read_byte(buf2, &val2)) goto error;
                           if (val1 != val2) {
                              return val1 < val2? -1 : +1;
                           }
                           continue;
                        }

                        case SER_SHORT: {
                           uint16_t val2;
                           if (!buffer_read_short(buf2, &val2)) goto error;
                           if (val1 != val2) {
                              return val1 < val2? -1 : +1;
                           }
                           continue;
                        }

                        case SER_INT: {
                           int val2;
                           if (!buffer_read_int(buf2, &val2)) goto error;
                           if (val1 != val2) {
                              return val1 < val2? -1 : +1;
                           }
                           continue;
                        }

                        case SER_FLOAT:
                        case SER_FLOAT_ZERO:
                        case SER_ARRAY:
                        case SER_ARRAY_BYTE:
                        case SER_ARRAY_SHORT:
                        case SER_ARRAY_INT:
                        case SER_STRING_BYTE:
                        case SER_STRING_SHORT:
                        case SER_STRING_INT:
                        case SER_HASH:
                           return -1;
                     }
                     goto error;
                  }
                  if (len1 == len2) {
                     continue;
                  }
                  return len1 - len2;
               }

               case SER_ARRAY_BYTE: {
                  int val1;
                  uint8_t val2;
                  int i, len1, len2, len;

                  if (!buffer_read_length(buf1, type1, &len1)) goto error;
                  if (!buffer_read_length(buf2, type2, &len2)) goto error;
                  len = len1 < len2? len1 : len2;

                  for (i=0; i<len; i++) {
                     if (!buffer_read_int(buf1, &val1)) goto error;
                     if (!buffer_read_byte(buf2, &val2)) goto error;
                     if (val1 != val2) {
                        return val1 < val2? -1 : +1;
                     }
                  }
                  if (len1 == len2) {
                     continue;
                  }
                  return len1 - len2;
               }

               case SER_ARRAY_SHORT: {
                  int val1;
                  uint16_t val2;
                  int i, len1, len2, len;

                  if (!buffer_read_length(buf1, type1, &len1)) goto error;
                  if (!buffer_read_length(buf2, type2, &len2)) goto error;
                  len = len1 < len2? len1 : len2;

                  for (i=0; i<len; i++) {
                     if (!buffer_read_int(buf1, &val1)) goto error;
                     if (!buffer_read_short(buf2, &val2)) goto error;
                     if (val1 != val2) {
                        return val1 < val2? -1 : +1;
                     }
                  }
                  if (len1 == len2) {
                     continue;
                  }
                  return len1 - len2;
               }

               case SER_ARRAY_INT: {
                  int val1;
                  int val2;
                  int i, len1, len2, len;

                  if (!buffer_read_length(buf1, type1, &len1)) goto error;
                  if (!buffer_read_length(buf2, type2, &len2)) goto error;
                  len = len1 < len2? len1 : len2;

                  for (i=0; i<len; i++) {
                     if (!buffer_read_int(buf1, &val1)) goto error;
                     if (!buffer_read_int(buf2, &val2)) goto error;
                     if (val1 != val2) {
                        return val1 < val2? -1 : +1;
                     }
                  }
                  if (len1 == len2) {
                     continue;
                  }
                  return len1 - len2;
               }

               case SER_STRING_BYTE:
               case SER_STRING_SHORT:
               case SER_STRING_INT:
               case SER_HASH:
                  return -1;
            }
            goto error;

         case SER_STRING_BYTE:
            switch (type2 & 0x0F) {
               case SER_ZERO:
               case SER_BYTE:
               case SER_SHORT:
               case SER_INT:
               case SER_FLOAT:
               case SER_FLOAT_ZERO:
               case SER_ARRAY:
               case SER_ARRAY_BYTE:
               case SER_ARRAY_SHORT:
               case SER_ARRAY_INT:
                  return +1;

               case SER_STRING_BYTE: {
                  uint8_t val1, val2;
                  int i, len1, len2, len;

                  if (!buffer_read_length(buf1, type1, &len1)) goto error;
                  if (!buffer_read_length(buf2, type2, &len2)) goto error;
                  len = len1 < len2? len1 : len2;

                  for (i=0; i<len; i++) {
                     if (!buffer_read_byte(buf1, &val1)) goto error;
                     if (!buffer_read_byte(buf2, &val2)) goto error;
                     if (val1 != val2) {
                        return (int)val1 - (int)val2;
                     }
                  }
                  if (len1 == len2) {
                     continue;
                  }
                  return len1 - len2;
               }

               case SER_STRING_SHORT: {
                  uint8_t val1;
                  uint16_t val2;
                  int i, len1, len2, len;

                  if (!buffer_read_length(buf1, type1, &len1)) goto error;
                  if (!buffer_read_length(buf2, type2, &len2)) goto error;
                  len = len1 < len2? len1 : len2;

                  for (i=0; i<len; i++) {
                     if (!buffer_read_byte(buf1, &val1)) goto error;
                     if (!buffer_read_short(buf2, &val2)) goto error;
                     if (val1 != val2) {
                        return (int)val1 - (int)val2;
                     }
                  }
                  if (len1 == len2) {
                     continue;
                  }
                  return len1 - len2;
               }

               case SER_STRING_INT: {
                  uint8_t val1;
                  int val2;
                  int i, len1, len2, len;

                  if (!buffer_read_length(buf1, type1, &len1)) goto error;
                  if (!buffer_read_length(buf2, type2, &len2)) goto error;
                  len = len1 < len2? len1 : len2;

                  for (i=0; i<len; i++) {
                     if (!buffer_read_byte(buf1, &val1)) goto error;
                     if (!buffer_read_int(buf2, &val2)) goto error;
                     if (val1 != val2) {
                        return val1 < val2? -1 : +1;
                     }
                  }
                  if (len1 == len2) {
                     continue;
                  }
                  return len1 - len2;
               }

               case SER_HASH:
                  return -1;
            }
            goto error;

         case SER_STRING_SHORT:
            switch (type2 & 0x0F) {
               case SER_ZERO:
               case SER_BYTE:
               case SER_SHORT:
               case SER_INT:
               case SER_FLOAT:
               case SER_FLOAT_ZERO:
               case SER_ARRAY:
               case SER_ARRAY_BYTE:
               case SER_ARRAY_SHORT:
               case SER_ARRAY_INT:
                  return +1;

               case SER_STRING_BYTE: {
                  uint16_t val1;
                  uint8_t val2;
                  int i, len1, len2, len;

                  if (!buffer_read_length(buf1, type1, &len1)) goto error;
                  if (!buffer_read_length(buf2, type2, &len2)) goto error;
                  len = len1 < len2? len1 : len2;

                  for (i=0; i<len; i++) {
                     if (!buffer_read_short(buf1, &val1)) goto error;
                     if (!buffer_read_byte(buf2, &val2)) goto error;
                     if (val1 != val2) {
                        return (int)val1 - (int)val2;
                     }
                  }
                  if (len1 == len2) {
                     continue;
                  }
                  return len1 - len2;
               }

               case SER_STRING_SHORT: {
                  uint16_t val1, val2;
                  int i, len1, len2, len;

                  if (!buffer_read_length(buf1, type1, &len1)) goto error;
                  if (!buffer_read_length(buf2, type2, &len2)) goto error;
                  len = len1 < len2? len1 : len2;

                  for (i=0; i<len; i++) {
                     if (!buffer_read_short(buf1, &val1)) goto error;
                     if (!buffer_read_short(buf2, &val2)) goto error;
                     if (val1 != val2) {
                        return (int)val1 - (int)val2;
                     }
                  }
                  if (len1 == len2) {
                     continue;
                  }
                  return len1 - len2;
               }

               case SER_STRING_INT: {
                  uint16_t val1;
                  int val2;
                  int i, len1, len2, len;

                  if (!buffer_read_length(buf1, type1, &len1)) goto error;
                  if (!buffer_read_length(buf2, type2, &len2)) goto error;
                  len = len1 < len2? len1 : len2;

                  for (i=0; i<len; i++) {
                     if (!buffer_read_short(buf1, &val1)) goto error;
                     if (!buffer_read_int(buf2, &val2)) goto error;
                     if (val1 != val2) {
                        return val1 < val2? -1 : +1;
                     }
                  }
                  if (len1 == len2) {
                     continue;
                  }
                  return len1 - len2;
               }

               case SER_HASH:
                  return -1;
            }
            goto error;

         case SER_STRING_INT:
            switch (type2 & 0x0F) {
               case SER_ZERO:
               case SER_BYTE:
               case SER_SHORT:
               case SER_INT:
               case SER_FLOAT:
               case SER_FLOAT_ZERO:
               case SER_ARRAY:
               case SER_ARRAY_BYTE:
               case SER_ARRAY_SHORT:
               case SER_ARRAY_INT:
                  return +1;

               case SER_STRING_BYTE: {
                  int val1;
                  uint8_t val2;
                  int i, len1, len2, len;

                  if (!buffer_read_length(buf1, type1, &len1)) goto error;
                  if (!buffer_read_length(buf2, type2, &len2)) goto error;
                  len = len1 < len2? len1 : len2;

                  for (i=0; i<len; i++) {
                     if (!buffer_read_int(buf1, &val1)) goto error;
                     if (!buffer_read_byte(buf2, &val2)) goto error;
                     if (val1 != val2) {
                        return val1 < val2? -1 : +1;
                     }
                  }
                  if (len1 == len2) {
                     continue;
                  }
                  return len1 - len2;
               }

               case SER_STRING_SHORT: {
                  int val1;
                  uint16_t val2;
                  int i, len1, len2, len;

                  if (!buffer_read_length(buf1, type1, &len1)) goto error;
                  if (!buffer_read_length(buf2, type2, &len2)) goto error;
                  len = len1 < len2? len1 : len2;

                  for (i=0; i<len; i++) {
                     if (!buffer_read_int(buf1, &val1)) goto error;
                     if (!buffer_read_short(buf2, &val2)) goto error;
                     if (val1 != val2) {
                        return val1 < val2? -1 : +1;
                     }
                  }
                  if (len1 == len2) {
                     continue;
                  }
                  return len1 - len2;
               }

               case SER_STRING_INT: {
                  int val1, val2;
                  int i, len1, len2, len;

                  if (!buffer_read_length(buf1, type1, &len1)) goto error;
                  if (!buffer_read_length(buf2, type2, &len2)) goto error;
                  len = len1 < len2? len1 : len2;

                  for (i=0; i<len; i++) {
                     if (!buffer_read_int(buf1, &val1)) goto error;
                     if (!buffer_read_int(buf2, &val2)) goto error;
                     if (val1 != val2) {
                        return val1 < val2? -1 : +1;
                     }
                  }
                  if (len1 == len2) {
                     continue;
                  }
                  return len1 - len2;
               }

               case SER_HASH:
                  return -1;
            }
            goto error;

         case SER_HASH:
            switch (type2 & 0x0F) {
               case SER_ZERO:
               case SER_BYTE:
               case SER_SHORT:
               case SER_INT:
               case SER_FLOAT:
               case SER_FLOAT_ZERO:
               case SER_ARRAY:
               case SER_ARRAY_BYTE:
               case SER_ARRAY_SHORT:
               case SER_ARRAY_INT:
               case SER_STRING_BYTE:
               case SER_STRING_SHORT:
               case SER_STRING_INT:
                  return +1;

               case SER_HASH: {
                  int len1, len2, len, ret;

                  if (!buffer_read_length(buf1, type1, &len1)) goto error;
                  if (!buffer_read_length(buf2, type2, &len2)) goto error;
                  len = len1 < len2? len1 : len2;
                  if (len >= ((1<<30)-1)) goto error;

                  ret = compare_serialized_values(buf1, buf2, len*2);
                  if (ret != 0) return ret;
                  if (len1 == len2) {
                     continue;
                  }
                  return len1 - len2;
               }
            }
            goto error;

         default:
            goto error;
      }
   }
   return 0;

error:
   return 0x80000000;
}


static int compare_sorted_entries(const void *e1, const void *e2)
{
   const SortedEntry *entry1 = (void *)e1;
   const SortedEntry *entry2 = (void *)e2;
   Buffer buf1, buf2;

   buf1.cur = entry1->key;
   buf1.end = entry1->key + entry1->key_len;
   buf2.cur = entry2->key;
   buf2.end = entry2->key + entry2->key_len;
   return compare_serialized_values(&buf1, &buf2, 1);
}


static int serialize_key(Heap *heap, Value value, Buffer *buf, int recursion_limit);

static int serialize_subkey(Heap *heap, Value value, char **out_data, int *out_len, int recursion_limit)
{
   Buffer buf;
   int err;

   buf.start = malloc(8);
   if (!buf.start) {
      return FIXSCRIPT_ERR_OUT_OF_MEMORY;
   }
   buf.cur = buf.start;
   buf.end = buf.start + 8;

   err = serialize_key(heap, value, &buf, recursion_limit);
   if (err) {
      free(buf.start);
      return err;
   }

   *out_data = buf.start;
   *out_len = buf.cur - buf.start;
   return FIXSCRIPT_SUCCESS;
}


static int serialize_key(Heap *heap, Value value, Buffer *buf, int recursion_limit)
{
   Value key, elem;
   SortedEntry *entries;
   int i, err, val, max_val, has_refs, len, elem_size, is_string, pos;
   void *data;

   if (recursion_limit <= 0) {
      return FIXSCRIPT_ERR_RECURSION_LIMIT;
   }

   if (fixscript_is_int(value)) {
      if (value.value == 0) {
         if (!buffer_append_byte(buf, SER_ZERO)) goto error;
         return FIXSCRIPT_SUCCESS;
      }
      if ((value.value & ~0xFF) == 0) {
         if (!buffer_append_byte(buf, SER_BYTE)) goto error;
         if (!buffer_append_byte(buf, value.value)) goto error;
         return FIXSCRIPT_SUCCESS;
      }
      if ((value.value & ~0xFFFF) == 0) {
         if (!buffer_append_byte(buf, SER_SHORT)) goto error;
         if (!buffer_append_short(buf, value.value)) goto error;
         return FIXSCRIPT_SUCCESS;
      }
      if (!buffer_append_byte(buf, SER_INT)) goto error;
      if (!buffer_append_int(buf, value.value)) goto error;
      return FIXSCRIPT_SUCCESS;
   }

   if (fixscript_is_float(value)) {
      if (value.value == 0) {
         if (!buffer_append_byte(buf, SER_FLOAT_ZERO)) goto error;
         return FIXSCRIPT_SUCCESS;
      }
      // normalize NaNs:
      val = value.value;
      if (((val >> 23) & 0xFF) == 0xFF && (val & ((1<<23)-1))) {
         val = (val & ~((1<<23)-1)) | (1 << 22);
      }
      if (!buffer_append_byte(buf, SER_FLOAT)) goto error;
      if (!buffer_append_int(buf, val)) goto error;
      return FIXSCRIPT_SUCCESS;
   }

   if (fixscript_is_array(heap, value)) {
      is_string = fixscript_is_string(heap, value);

      err = fixscript_get_array_length(heap, value, &len);
      if (err) return err;
      err = fixscript_has_array_references(heap, value, 0, len, 1, &has_refs);
      if (err) return err;

      if (has_refs) {
         if (is_string) {
            return FIXSCRIPT_ERR_UNSERIALIZABLE_REF;
         }

         if (!buffer_append_length(buf, SER_ARRAY, len)) goto error;
         for (i=0; i<len; i++) {
            err = fixscript_get_array_elem(heap, value, i, &elem);
            if (err) return err;
            err = serialize_key(heap, elem, buf, recursion_limit-1);
            if (err) return err;
         }
         return FIXSCRIPT_SUCCESS;
      }

      err = fixscript_get_array_element_size(heap, value, &elem_size);
      if (err) return err;
      err = fixscript_lock_array(heap, value, 0, len, &data, elem_size, ACCESS_READ_ONLY);
      if (err) return err;

      if (elem_size == 1) {
         if (!buffer_append_length(buf, is_string? SER_STRING_BYTE : SER_ARRAY_BYTE, len)) goto array_memory_error;
         while (buf->end - buf->cur < len) {
            if (!buffer_expand(buf)) goto array_memory_error;
         }
         memcpy(buf->cur, data, len);
         buf->cur += len;
      }
      else if (elem_size == 2) {
         max_val = 0;
         for (i=0; i<len; i++) {
            max_val |= ((uint16_t *)data)[i];
         }
         if ((max_val & ~0xFF) == 0) {
            if (!buffer_append_length(buf, is_string? SER_STRING_BYTE : SER_ARRAY_BYTE, len)) goto array_memory_error;
            while (buf->end - buf->cur < len) {
               if (!buffer_expand(buf)) goto array_memory_error;
            }
            for (i=0; i<len; i++) {
               *buf->cur++ = ((uint16_t *)data)[i];
            }
         }
         else {
            if (!buffer_append_length(buf, is_string? SER_STRING_SHORT : SER_ARRAY_SHORT, len)) goto array_memory_error;
            if (len >= ((1<<30)-1)) goto array_memory_error;
            while (buf->end - buf->cur < len*2) {
               if (!buffer_expand(buf)) goto array_memory_error;
            }
            for (i=0; i<len; i++) {
               val = ((uint16_t *)data)[i];
               *buf->cur++ = val & 0xFF;
               *buf->cur++ = val >> 8;
            }
         }
      }
      else {
         max_val = 0;
         for (i=0; i<len; i++) {
            max_val |= ((uint32_t *)data)[i];
         }
         if ((max_val & ~0xFF) == 0) {
            if (!buffer_append_length(buf, is_string? SER_STRING_BYTE : SER_ARRAY_BYTE, len)) goto array_memory_error;
            while (buf->end - buf->cur < len) {
               if (!buffer_expand(buf)) goto array_memory_error;
            }
            for (i=0; i<len; i++) {
               *buf->cur++ = ((uint32_t *)data)[i];
            }
         }
         else if ((max_val & ~0xFFFF) == 0) {
            if (!buffer_append_length(buf, is_string? SER_STRING_SHORT : SER_ARRAY_SHORT, len)) goto array_memory_error;
            if (len >= ((1<<30)-1)) goto array_memory_error;
            while (buf->end - buf->cur < len*2) {
               if (!buffer_expand(buf)) goto array_memory_error;
            }
            for (i=0; i<len; i++) {
               val = ((uint32_t *)data)[i];
               *buf->cur++ = val & 0xFF;
               *buf->cur++ = val >> 8;
            }
         }
         else {
            if (!buffer_append_length(buf, is_string? SER_STRING_INT : SER_ARRAY_INT, len)) goto array_memory_error;
            if (len >= ((1<<29)-1)) goto array_memory_error;
            while (buf->end - buf->cur < len*4) {
               if (!buffer_expand(buf)) goto array_memory_error;
            }
            for (i=0; i<len; i++) {
               val = ((uint32_t *)data)[i];
               *buf->cur++ = val & 0xFF;
               *buf->cur++ = (val >> 8) & 0xFF;
               *buf->cur++ = (val >> 16) & 0xFF;
               *buf->cur++ = (val >> 24) & 0xFF;
            }
         }
      }

array_error:
      fixscript_unlock_array(heap, value, 0, len, &data, elem_size, ACCESS_READ_ONLY);
      return err;

array_memory_error:
      err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
      goto array_error;
   }

   if (fixscript_is_hash(heap, value)) {
      err = fixscript_get_array_length(heap, value, &len);
      if (err) return err;

      if (!buffer_append_length(buf, SER_HASH, len)) goto error;

      entries = calloc(len, sizeof(SortedEntry));
      if (!entries) goto error;

      i = 0;
      pos = 0;
      while (fixscript_iter_hash(heap, value, &key, &elem, &pos)) {
         err = serialize_subkey(heap, key, &entries[i].key, &entries[i].key_len, recursion_limit-1);
         if (err) goto hash_error;
         err = serialize_subkey(heap, elem, &entries[i].value, &entries[i].value_len, recursion_limit-1);
         if (err) goto hash_error;
         i++;
      }

      qsort(entries, len, sizeof(SortedEntry), compare_sorted_entries);

      for (i=0; i<len; i++) {
         while (buf->end - buf->cur < entries[i].key_len + entries[i].value_len) {
            if (!buffer_expand(buf)) goto hash_memory_error;
         }
         memcpy(buf->cur, entries[i].key, entries[i].key_len);
         buf->cur += entries[i].key_len;
         memcpy(buf->cur, entries[i].value, entries[i].value_len);
         buf->cur += entries[i].value_len;
      }

hash_error:
      for (i=0; i<len; i++) {
         free(entries[i].key);
         free(entries[i].value);
      }
      free(entries);
      return err;

hash_memory_error:
      err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
      goto hash_error;
   }

   return FIXSCRIPT_ERR_UNSERIALIZABLE_REF;

error:
   return FIXSCRIPT_ERR_OUT_OF_MEMORY;
}


static Value native_serialize_key(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Buffer buf;
   Value ret = fixscript_int(0);
   int64_t new_len;
   int err, len;

   buf.start = malloc(64);
   if (!buf.start) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   buf.cur = buf.start;
   buf.end = buf.start + 64;

   err = serialize_key(heap, params[num_params-1], &buf, 50);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   if (num_params == 2) {
      err = fixscript_get_array_length(heap, params[0], &len);
      if (!err) {
         new_len = (int64_t)len + (int64_t)(buf.cur - buf.start);
         if (new_len > INT_MAX) err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
      if (!err) {
         err = fixscript_set_array_length(heap, params[0], (int)new_len);
      }
      if (!err) {
         err = fixscript_set_array_bytes(heap, params[0], len, buf.cur - buf.start, buf.start);
      }
      if (err) {
         fixscript_error(heap, error, err);
         goto error;
      }
   }
   else {
      ret = fixscript_create_byte_array(heap, buf.start, buf.cur - buf.start);
      if (!ret.value) {
         fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         goto error;
      }
   }

error:
   free(buf.start);
   return ret;
}


static Value native_serialize_compare(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value arr1, arr2;
   Buffer buf1, buf2;
   char *data1 = NULL, *data2 = NULL;
   int err, off1, len1, off2, len2, ret=0;

   if (num_params == 6) {
      arr1 = params[0];
      off1 = params[1].value;
      len1 = params[2].value;
      arr2 = params[3];
      off2 = params[4].value;
      len2 = params[5].value;
   }
   else {
      arr1 = params[0];
      arr2 = params[1];
      off1 = 0;
      off2 = 0;
      err = fixscript_get_array_length(heap, arr1, &len1);
      if (!err) {
         err = fixscript_get_array_length(heap, arr2, &len2);
      }
      if (err) {
         return fixscript_error(heap, error, err);
      }
   }

   err = fixscript_lock_array(heap, arr1, off1, len1, (void **)&data1, 1, ACCESS_READ_ONLY);
   if (!err) {
      err = fixscript_lock_array(heap, arr2, off2, len2, (void **)&data2, 1, ACCESS_READ_ONLY);
   }
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   buf1.cur = data1;
   buf1.end = data1 + len1;
   buf2.cur = data2;
   buf2.end = data2 + len2;
   ret = compare_serialized_values(&buf1, &buf2, 1);
   if (ret == 0x80000000) {
      ret = 0;
      fixscript_error(heap, error, FIXSCRIPT_ERR_BAD_FORMAT);
      goto error;
   }
   if (ret < 0) ret = -1;
   if (ret > 0) ret = +1;

error:
   if (data1) {
      fixscript_unlock_array(heap, arr1, off1, len1, (void **)&data1, 1, ACCESS_READ_ONLY);
   }
   if (data2) {
      fixscript_unlock_array(heap, arr2, off2, len2, (void **)&data2, 1, ACCESS_READ_ONLY);
   }
   return fixscript_int(ret);
}


#ifndef _WIN32
typedef struct {
   char buf[8];
   int cnt;
#ifndef __wasm__
   mbstate_t mbstate;
#endif
} NativeConvertState;

static void convert_from_native(wchar_t *dest, int dest_len, char *src, int src_len, NativeConvertState *state, int *processed_dest, int *processed_src)
{
#ifndef __wasm__
   mbstate_t mbstate_sav;
#endif
   wchar_t *orig_dest = dest;
   char *orig_src = src;
   unsigned int c;
   unsigned char c2, c3, c4;
   int parsed;

   for (;;) {
      while (src_len > 0 && state->cnt < sizeof(state->buf)) {
         state->buf[state->cnt++] = *src++;
         src_len--;
      }
      if (state->cnt <= 0 || dest_len <= 0) break;

      if (state->buf[0] == 0) {
         *dest++ = 0;
         dest_len--;
         memmove(state->buf, state->buf+1, --state->cnt);
         continue;
      }

      if (native_charset_utf8) {
         c = (unsigned char)state->buf[0];
         if ((c & 0x80) == 0) {
            parsed = 1;
         }
         else if ((c & 0xE0) == 0xC0) {
            if (state->cnt < 2) break;
            c2 = state->buf[1];
            c = ((c & 0x1F) << 6) | (c2 & 0x3F);
            if (c < 0x80) {
               c = 0xFFFD;
            }
            parsed = 2;
         }
         else if ((c & 0xF0) == 0xE0) {
            if (state->cnt < 3) break;
            c2 = state->buf[1];
            c3 = state->buf[2];
            c = ((c & 0x0F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
            if (c < 0x800) {
               c = 0xFFFD;
            }
            parsed = 3;
         }
         else if ((c & 0xF8) == 0xF0) {
            if (state->cnt < 4) break;
            c2 = state->buf[1];
            c3 = state->buf[2];
            c4 = state->buf[3];
            c = ((c & 0x07) << 18) | ((c2 & 0x3F) << 12) | ((c3 & 0x3F) << 6) | (c4 & 0x3F);
            if (c < 0x10000 || c > 0x10FFFF) {
               c = 0xFFFD;
            }
            parsed = 4;
         }
         else {
            c = 0xFFFD;
            parsed = 1;
         }

         if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
         }

         #if WCHAR_MAX > 0xFFFF
            *dest = c;
         #elif WCHAR_MAX > 0xFF
            if (c > 0xFFFF) {
               c = 0xFFFD;
            }
            *dest = c;
         #else
            if (c > 0xFF) {
               c = '?';
            }
            *dest = c;
         #endif
      }
#ifndef __wasm__
      else {
         memcpy(&mbstate_sav, &state->mbstate, sizeof(mbstate_t));
         parsed = (int)mbrtowc(dest, state->buf, state->cnt, &state->mbstate);
         if (parsed == -2) {
            memcpy(&state->mbstate, &mbstate_sav, sizeof(mbstate_t));
            break;
         }
         if (parsed < 0) {
            memcpy(&state->mbstate, &mbstate_sav, sizeof(mbstate_t));
            #if WCHAR_MAX > 0xFF
               *dest++ = 0xFFFD;
            #else
               *dest++ = '?';
            #endif
            dest_len--;
            memmove(state->buf, state->buf+1, --state->cnt);
            continue;
         }
      }
#endif

      dest++;
      dest_len--;
      state->cnt -= parsed;
      memmove(state->buf, state->buf+parsed, state->cnt);
   }

   *processed_dest = dest - orig_dest;
   *processed_src = src - orig_src;
}

static void convert_to_native(char *dest, int dest_len, wchar_t *src, int src_len, NativeConvertState *state, int *processed_dest, int *processed_src)
{
#ifndef __wasm__
   mbstate_t mbstate_sav;
#endif
   char *orig_dest = dest;
   wchar_t *orig_src = src;
   int len, c;

   for (;;) {
      if (state->cnt == 0) {
         if (src_len <= 0) break;
         
         if (native_charset_utf8) {
            #if WCHAR_MAX > 0xFFFF
               c = *src++;
            #elif WCHAR_MAX > 0xFF
               c = (uint16_t)*src++;
            #else
               c = (uint8_t)*src++;
            #endif
            src_len--;
            if (c < 0 || c > 0x10FFFF) {
               c = 0xFFFD;
            }
            if (c >= 0xD800 && c <= 0xDFFF) {
               c = 0xFFFD;
            }
            if (c >= 0x10000) {
               state->buf[0] = (c >> 18) | 0xF0;
               state->buf[1] = ((c >> 12) & 0x3F) | 0x80;
               state->buf[2] = ((c >> 6) & 0x3F) | 0x80;
               state->buf[3] = (c & 0x3F) | 0x80;
               state->cnt = 4;
            }
            else if (c >= 0x800) {
               state->buf[0] = (c >> 12) | 0xE0;
               state->buf[1] = ((c >> 6) & 0x3F) | 0x80;
               state->buf[2] = (c & 0x3F) | 0x80;
               state->cnt = 3;
            }
            else if (c >= 0x80) {
               state->buf[0] = (c >> 6) | 0xC0;
               state->buf[1] = (c & 0x3F) | 0x80;
               state->cnt = 2;
            }
            else {
               state->buf[0] = c;
               state->cnt = 1;
            }
         }
#ifndef __wasm__
         else if (*src == 0) {
            src++;
            src_len--;
            state->buf[0] = 0;
            state->cnt = 1;
         }
         else {
            memcpy(&mbstate_sav, &state->mbstate, sizeof(mbstate_t));
            len = (int)wcrtomb(state->buf, *src, &state->mbstate);
            if (len <= 0) {
               memcpy(&state->mbstate, &mbstate_sav, sizeof(mbstate_t));
               src++;
               src_len--;
               state->buf[0] = '?';
               state->cnt = 1;
            }
            else {
               src++;
               src_len--;
               state->cnt = len;
            }
         }
#endif
      }

      if (dest_len <= 0) break;

      len = state->cnt;
      if (len > dest_len) {
         len = dest_len;
      }
      memcpy(dest, state->buf, len);
      dest += len;
      dest_len -= len;
      state->cnt -= len;
      memmove(state->buf, state->buf + len, state->cnt);
   }

   *processed_dest = dest - orig_dest;
   *processed_src = src - orig_src;
}
#endif


static Value native_string_from_native(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value arr, off, len, result;
   char *bytes;
#ifdef _WIN32
   Value str;
   unsigned short *chars;
   int64_t len64;
   int chars_len, result_len, real_chars_len;
#else
   NativeConvertState state;
   wchar_t chars[128];
   Value values[128];
   int processed_dest, processed_src;
   int i, j, result_len=0;
#if !defined(__HAIKU__)
   locale_t prev_locale = (locale_t)0;
#endif
#endif
   int err;

   if (num_params == 2 || num_params == 4) {
      result = *params++;
   }
   else {
      result = fixscript_int(0);
   }
   arr = *params++;
   if (num_params == 3 || num_params == 4) {
      off = *params++;
      len = *params++;

      if (off.value < 0) {
         *error = fixscript_create_error_string(heap, "negative offset");
         return fixscript_int(0);
      }
      if (len.value < 0) {
         *error = fixscript_create_error_string(heap, "negative length");
         return fixscript_int(0);
      }
   }
   else {
      off.value = 0;
      err = fixscript_get_array_length(heap, arr, &len.value);
      if (err) {
         return fixscript_error(heap, error, err);
      }
   }

   if (!result.value) {
      result = fixscript_create_string(heap, NULL, 0);
      if (!result.value) {
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
   }

   err = fixscript_lock_array(heap, arr, off.value, len.value, (void **)&bytes, 1, ACCESS_READ_ONLY);
   if (err) {
      return fixscript_error(heap, error, err);
   }

#ifdef _WIN32
   if (len.value > 0) {
      chars_len = MultiByteToWideChar(CP_ACP, 0, bytes, len.value, NULL, 0);
      if (chars_len <= 0) {
         fixscript_unlock_array(heap, arr, off.value, len.value, (void **)&bytes, 1, ACCESS_READ_ONLY);
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }

      len64 = (int64_t)chars_len * sizeof(uint16_t);
      if (len64 > INT_MAX) {
         fixscript_unlock_array(heap, arr, off.value, len.value, (void **)&bytes, 1, ACCESS_READ_ONLY);
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }

      chars = malloc(len64);
      if (!chars) {
         fixscript_unlock_array(heap, arr, off.value, len.value, (void **)&bytes, 1, ACCESS_READ_ONLY);
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }

      if (MultiByteToWideChar(CP_ACP, 0, bytes, len.value, chars, chars_len) != chars_len) {
         free(chars);
         fixscript_unlock_array(heap, arr, off.value, len.value, (void **)&bytes, 1, ACCESS_READ_ONLY);
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }

      err = fixscript_get_array_length(heap, result, &result_len);
      if (!err) {
         if (result_len >= INT_MAX - chars_len) {
            err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
         }
      }
      if (!err) {
         str = fixscript_create_string_utf16(heap, chars, chars_len);
         if (!str.value) {
            err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
         }
      }
      if (!err) {
         err = fixscript_get_array_length(heap, str, &real_chars_len);
      }
      if (!err) {
         err = fixscript_set_array_length(heap, result, result_len + real_chars_len);
      }
      if (!err) {
         err = fixscript_copy_array(heap, result, result_len, str, 0, real_chars_len);
      }
      if (err) {
         free(chars);
         fixscript_unlock_array(heap, arr, off.value, len.value, (void **)&bytes, 1, ACCESS_READ_ONLY);
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }

      free(chars);
   }
#else
   #if !defined(__HAIKU__)
      if (!native_charset_utf8) {
         prev_locale = uselocale(cur_locale);
      }
   #endif
   memset(&state, 0, sizeof(NativeConvertState));

   i = 0;
   for (;;) {
      convert_from_native(chars, sizeof(chars)/sizeof(wchar_t), bytes + i, len.value - i, &state, &processed_dest, &processed_src);
      if (processed_dest == 0) {
         if (state.cnt > 0) {
            state.cnt = 0;
            chars[0] = 0xFFFD;
            processed_dest = 1;
         }
         else break;
      }

      if (result_len >= INT_MAX - processed_dest) {
         err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
      if (!err) {
         err = fixscript_set_array_length(heap, result, result_len + processed_dest);
      }
      if (!err) {
         for (j=0; j<processed_dest; j++) {
            #if WCHAR_MAX > 0xFFFF
               values[j] = fixscript_int(chars[j]);
            #elif WCHAR_MAX > 0xFF
               values[j] = fixscript_int((uint16_t)chars[j]);
            #else
               values[j] = fixscript_int((uint8_t)chars[j]);
            #endif
         }
         err = fixscript_set_array_range(heap, result, result_len, processed_dest, values);
         result_len += processed_dest;
      }
      if (err) {
         #if !defined(__HAIKU__)
            if (!native_charset_utf8) {
               uselocale(prev_locale);
            }
         #endif
         fixscript_unlock_array(heap, arr, off.value, len.value, (void **)&bytes, 1, ACCESS_READ_ONLY);
         return fixscript_error(heap, error, err);
      }
      i += processed_src;
   }

   #if !defined(__HAIKU__)
      if (!native_charset_utf8) {
         uselocale(prev_locale);
      }
   #endif
#endif

   fixscript_unlock_array(heap, arr, off.value, len.value, (void **)&bytes, 1, ACCESS_READ_ONLY);
   return result;
}


static Value native_string_to_native(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value str, off, len, result;
#ifdef _WIN32
   unsigned short *chars;
   char *bytes;
   int chars_len, bytes_len, result_len;
#else
   NativeConvertState state;
   wchar_t chars[128];
   char bytes[128*4];
   int chars_cap = sizeof(chars) / sizeof(wchar_t) - 1;
   int processed_dest, processed_src;
   int i, j, elem_size, cnt, result_len=0, ch;
   union {
      uint8_t *u8;
      uint16_t *u16;
      int32_t *s32;
   } u;
#if !defined(__HAIKU__)
   locale_t prev_locale = (locale_t)0;
#endif
#endif
   int err;

   if (num_params == 2 || num_params == 4) {
      result = *params++;
   }
   else {
      result = fixscript_int(0);
   }
   str = *params++;
   if (num_params == 3 || num_params == 4) {
      off = *params++;
      len = *params++;

      if (off.value < 0) {
         *error = fixscript_create_error_string(heap, "negative offset");
         return fixscript_int(0);
      }
      if (len.value < 0) {
         *error = fixscript_create_error_string(heap, "negative length");
         return fixscript_int(0);
      }
   }
   else {
      off.value = 0;
      err = fixscript_get_array_length(heap, str, &len.value);
      if (err) {
         return fixscript_error(heap, error, err);
      }
   }

   if (!result.value) {
      result = fixscript_create_array(heap, 0);
      if (!result.value) {
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
   }

#ifdef _WIN32
   err = fixscript_get_string_utf16(heap, str, off.value, len.value, &chars, &chars_len);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   if (chars_len > 0) {
      bytes_len = WideCharToMultiByte(CP_ACP, 0, chars, chars_len, NULL, 0, NULL, NULL);
      if (bytes_len <= 0) {
         free(chars);
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }

      bytes = malloc(bytes_len);
      if (!bytes) {
         free(chars);
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }

      if (WideCharToMultiByte(CP_ACP, 0, chars, chars_len, bytes, bytes_len, NULL, NULL) != bytes_len) {
         free(chars);
         free(bytes);
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }

      err = fixscript_get_array_length(heap, result, &result_len);
      if (!err) {
         if (result_len >= INT_MAX - bytes_len) {
            err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
         }
      }
      if (!err) {
         err = fixscript_set_array_length(heap, result, result_len + bytes_len);
      }
      if (!err) {
         err = fixscript_set_array_bytes(heap, result, result_len, bytes_len, bytes);
      }
      if (err) {
         free(chars);
         free(bytes);
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }

      free(bytes);
   }

   free(chars);
#else
   err = fixscript_get_array_element_size(heap, str, &elem_size);
   if (!err) {
      err = fixscript_lock_array(heap, str, off.value, len.value, (void **)&u.u8, elem_size, ACCESS_READ_ONLY);
   }
   if (err) {
      return fixscript_error(heap, error, err);
   }

   #if !defined(__HAIKU__)
      if (!native_charset_utf8) {
         prev_locale = uselocale(cur_locale);
      }
   #endif
   memset(&state, 0, sizeof(NativeConvertState));

   i = 0;
   while (i < len.value) {
      cnt = len.value - i;
      if (cnt > chars_cap) {
         cnt = chars_cap;
      }

      switch (elem_size) {
         default:
         case 1:
            for (j=0; j<cnt; j++) {
               chars[j] = u.u8[i++];
            }
            break;

         case 2:
            for (j=0; j<cnt; j++) {
               ch = u.u16[i++];
               #if WCHAR_MAX > 0xFF
               if (ch >= 0xD800 && ch <= 0xDFFF) {
                  ch = 0xFFFD;
               }
               #else
               if (ch > 0xFF) {
                  ch = '?';
               }
               #endif
               chars[j] = ch;
            }
            break;

         case 4:
            for (j=0; j<cnt; j++) {
               ch = u.s32[i++];
               if ((ch >= 0xD800 && ch <= 0xDFFF) || ch < 0 || ch > 0x10FFFF) {
                  ch = 0xFFFD;
               }
               #if WCHAR_MAX <= 0xFF
               if (ch > 0xFF) {
                  ch = '?';
               }
               #elif WCHAR_MAX <= 0xFFFF
               if (ch > 0xFFFF) {
                  ch = 0xFFFD;
               }
               #endif
               chars[j] = ch;
            }
            break;
      }

      for (j=0; j<cnt; ) {
         convert_to_native(bytes, sizeof(bytes)-1, chars + j, cnt - j, &state, &processed_dest, &processed_src);
         if (processed_dest > 0) {
            if (result_len >= INT_MAX - processed_dest) {
               err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
            }
            if (!err) {
               err = fixscript_set_array_length(heap, result, result_len + processed_dest);
            }
            if (!err) {
               err = fixscript_set_array_bytes(heap, result, result_len, processed_dest, bytes);
               result_len += processed_dest;
            }
            if (err) {
               #if !defined(__HAIKU__)
                  if (!native_charset_utf8) {
                     uselocale(prev_locale);
                  }
               #endif
               fixscript_unlock_array(heap, str, off.value, len.value, (void **)&u.u8, elem_size, ACCESS_READ_ONLY);
               return fixscript_error(heap, error, err);
            }
         }
         j += processed_src;
      }
   }

   #if !defined(__HAIKU__)
      if (!native_charset_utf8) {
         uselocale(prev_locale);
      }
   #endif
   fixscript_unlock_array(heap, str, off.value, len.value, (void **)&u.u8, elem_size, ACCESS_READ_ONLY);
#endif

   return result;
}


#ifndef __wasm__
static int clear_console_line()
{
   if (console_clear_line) {
      fputc('\r', stdout);
      fputs(console_clear_line, stdout);
      fputc('\r', stdout);
      free(console_clear_line);
      console_clear_line = NULL;
      return 1;
   }
   return 0;
}
#endif


static Value native_console_is_present(Heap *heap, Value *error, int num_params, Value *params, void *data);

static Value native_print(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(__wasm__)
   int mode = (intptr_t)data;
   NativeFunc log_func;
   void *log_data;

   if (mode == PRINT_NORMAL || mode == PRINT_PROGRESS) {
      log_func = fixscript_get_native_func(heap, "log#1", &log_data);
      return log_func(heap, error, 1, params, log_data);
   }
   return fixscript_int(0);
#else
   int mode = (intptr_t)data;
   Script *script;
#ifdef _WIN32
   HANDLE in, out;
   DWORD console_mode, ret;
   Value prompt_str, tmp_str, val1, val2;
   pthread_mutex_t *mutex;
   uint16_t *s, buf[256];
   char *bytes;
   int i, j, err, len, len2, bytes_len;
#else
   pthread_mutex_t *mutex;
#if !defined(__HAIKU__)
   locale_t prev_locale = (locale_t)0;
#endif
   NativeConvertState state;
   char *bytes;
   wchar_t buf[128];
   char charbuf[128], *cp;
   Value prompt_str;
   int buf_cap = sizeof(buf) / sizeof(wchar_t);
   int i, j, err, len, elem_size, cnt, processed_dest, processed_src;
   union {
      uint8_t *u8;
      uint16_t *u16;
      int32_t *s32;
   } u;
#endif

#ifdef _WIN32
   if (gui_app) {
      NativeFunc log_func;
      void *log_data;

      if (mode == PRINT_NORMAL || mode == PRINT_PROGRESS) {
         log_func = fixscript_get_native_func(heap, "log#1", &log_data);
         return log_func(heap, error, 1, params, log_data);
      }
      return fixscript_int(0);
   }
#endif

   if (mode == PRINT_PROMPT) {
      script = fixscript_get(heap, "io/console.fix");
      if (!script) {
         *error = fixscript_create_error_string(heap, "script io/console must be imported");
         return fixscript_int(0);
      }

      if (native_console_is_present(heap, error, 0, NULL, NULL).value) {
         return fixscript_run(heap, script, "prompt#2", error, params[0], fixscript_int(0));
      }
   }

   mutex = get_mutex(&console_mutex);
   if (!mutex) {
      *error = fixscript_create_error_string(heap, "can't lock console");
      return fixscript_int(0);
   }

   pthread_mutex_lock(mutex);
   
   if (console_active) {
      pthread_mutex_unlock(mutex);
      return fixscript_int(0);
   }

#ifdef _WIN32
   if ((mode == PRINT_NORMAL || mode == PRINT_LOG) && !fixscript_is_string(heap, params[0])) {
      err = fixscript_to_string(heap, params[0], 0, &bytes, &bytes_len);
      if (err) {
         pthread_mutex_unlock(mutex);
         return fixscript_error(heap, error, err);
      }
      
      params[0] = fixscript_create_string(heap, bytes, bytes_len);
      free(bytes);
      if (!params[0].value) {
         pthread_mutex_unlock(mutex);
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
   }

   err = fixscript_get_string_utf16(heap, params[0], 0, -1, &s, &len);
   if (err) {
      pthread_mutex_unlock(mutex);
      return fixscript_error(heap, error, err);
   }

   for (i=0; i<len; i++) {
      if (s[i] >= 0 && s[i] < 32) {
         switch (s[i]) {
            case '\t':
               break;

            case '\n':
               if (mode == PRINT_PROGRESS) {
                  s[i] = '?';
               }
               break;

            default:
               s[i] = '?';
         }
      }
   }

   out = GetStdHandle(mode == PRINT_LOG? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
   if (GetConsoleMode(out, &console_mode)) {
      if (mode == PRINT_PROGRESS) {
         if (!clear_console_line()) {
            WriteConsole(out, L"\r", 1, NULL, NULL);
         }

         console_clear_line = malloc(len+1);
         if (console_clear_line) {
            for (i=0, j=0; i<len; i++) {
               if (s[i] >= 0xD800 && s[i] <= 0xDBFF) {
                  continue;
               }
               console_clear_line[j++] = s[i] == '\t'? '\t' : ' ';
            }
            console_clear_line[j] = 0;
         }
      }
      else {
         clear_console_line();
      }
      WriteConsole(out, s, len, NULL, NULL);
      if (mode == PRINT_NORMAL || mode == PRINT_LOG) {
         WriteConsole(out, L"\n", 1, NULL, NULL);
      }
   }
   else {
      if (len > 0) {
         bytes_len = WideCharToMultiByte(CP_ACP, 0, s, len, NULL, 0, NULL, NULL);
         if (bytes_len <= 0) {
            free(s);
            pthread_mutex_unlock(mutex);
            return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         }

         if (INT_MAX - bytes_len < 2) {
            free(s);
            pthread_mutex_unlock(mutex);
            return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         }
      }
      else {
         bytes_len = 0;
      }

      bytes = malloc(bytes_len+2);
      if (!bytes) {
         free(s);
         pthread_mutex_unlock(mutex);
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }

      if (len > 0) {
         if (WideCharToMultiByte(CP_ACP, 0, (mode == PRINT_PROGRESS)? s+1 : s, len, bytes, bytes_len, NULL, NULL) != bytes_len) {
            free(bytes);
            free(s);
            pthread_mutex_unlock(mutex);
            return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         }
      }

      if (mode == PRINT_PROGRESS) {
         bytes[0] = '\r';
         bytes_len++;
      }

      if (mode == PRINT_NORMAL || mode == PRINT_LOG) {
         bytes[bytes_len++] = '\n';
      }

      clear_console_line();
      WriteFile(out, bytes, bytes_len, &ret, NULL);
      free(bytes);
   }

   free(s);

   if (mode == PRINT_PROMPT) {
      prompt_str = fixscript_create_string(heap, NULL, 0);
      if (!prompt_str.value) {
         pthread_mutex_unlock(mutex);
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }

      in = GetStdHandle(STD_INPUT_HANDLE);
      if (GetConsoleMode(in, &console_mode)) {
         SetConsoleMode(in, console_mode | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);

         for (;;) {
            if (!ReadConsole(in, buf, sizeof(buf)/sizeof(uint16_t), &ret, 0)) {
               break;
            }
            if (ret <= 0) continue;
            
            tmp_str = fixscript_create_string_utf16(heap, buf, ret);
            if (!tmp_str.value) {
               err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
            }
            if (!err) {
               err = fixscript_get_array_length(heap, prompt_str, &len);
            }
            if (!err) {
               err = fixscript_get_array_length(heap, tmp_str, &len2);
            }
            if (len > INT_MAX - len2) {
               err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
            }
            if (!err) {
               err = fixscript_set_array_length(heap, prompt_str, len + len2);
            }
            if (!err) {
               err = fixscript_copy_array(heap, prompt_str, len, tmp_str, 0, len2);
            }
            if (err) {
               SetConsoleMode(in, console_mode);
               pthread_mutex_unlock(mutex);
               return fixscript_error(heap, error, err);
            }

            if (buf[ret-1] == '\n') break;
         }

         SetConsoleMode(in, console_mode);
      }

      err = fixscript_get_array_length(heap, prompt_str, &len);
      if (!err && len >= 2) {
         err = fixscript_get_array_elem(heap, prompt_str, len-2, &val1);
         if (!err) {
            err = fixscript_get_array_elem(heap, prompt_str, len-1, &val2);
         }
         if (!err && val1.value == '\r' && val2.value == '\n') {
            err = fixscript_set_array_length(heap, prompt_str, len-2);
         }
      }
      pthread_mutex_unlock(mutex);
      if (err) {
         return fixscript_error(heap, error, err);
      }
      return prompt_str;
   }
   else {
      // TODO
      pthread_mutex_unlock(mutex);
      return fixscript_create_string(heap, NULL, 0);
   }

   pthread_mutex_unlock(mutex);
#else
   if ((mode == PRINT_NORMAL || mode == PRINT_LOG) && !fixscript_is_string(heap, params[0])) {
      err = fixscript_to_string(heap, params[0], 0, &bytes, &len);
      if (err) {
         pthread_mutex_unlock(mutex);
         return fixscript_error(heap, error, err);
      }
      
      params[0] = fixscript_create_string(heap, bytes, len);
      free(bytes);
      if (!params[0].value) {
         pthread_mutex_unlock(mutex);
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
   }

   err = fixscript_get_array_length(heap, params[0], &len);
   if (!err) {
      err = fixscript_get_array_element_size(heap, params[0], &elem_size);
   }
   if (!err) {
      err = fixscript_lock_array(heap, params[0], 0, len, (void **)&u.u8, elem_size, ACCESS_READ_ONLY);
   }
   if (err) {
      pthread_mutex_unlock(mutex);
      return fixscript_error(heap, error, err);
   }

   #if !defined(__HAIKU__)
      if (!native_charset_utf8) {
         prev_locale = uselocale(cur_locale);
      }
   #endif
   memset(&state, 0, sizeof(NativeConvertState));

   if (mode == PRINT_PROGRESS) {
      if (!clear_console_line()) {
         fputc('\r', stdout);
      }

      console_clear_line = malloc(len+1);
      if (console_clear_line) {
         if (elem_size == 1) {
            for (i=0; i<len; i++) {
               console_clear_line[i] = u.u8[i] == '\t'? '\t' : ' ';
            }
         }
         else if (elem_size == 2) {
            for (i=0; i<len; i++) {
               console_clear_line[i] = u.u16[i] == '\t'? '\t' : ' ';
            }
         }
         else if (elem_size == 4) {
            for (i=0; i<len; i++) {
               console_clear_line[i] = u.s32[i] == '\t'? '\t' : ' ';
            }
         }
         console_clear_line[len] = 0;
      }
   }
   else {
      clear_console_line();
   }

   i = 0;
   for (;;) {
      cnt = len - i;
      if (cnt > buf_cap) {
         cnt = buf_cap;
      }

      if (elem_size == 1) {
         for (j=0; j<cnt; j++) {
            buf[j] = u.u8[i+j];
         }
      }
      else if (elem_size == 2) {
         for (j=0; j<cnt; j++) {
            buf[j] = u.u16[i+j];
         }
      }
      else if (elem_size == 4) {
         for (j=0; j<cnt; j++) {
            #if WCHAR_MAX <= 0xFFFF
               buf[j] = (u.s32[i+j] < 0 || u.s32[i+j] > 0xFFFF)? 0xFFFD : u.s32[i+j];
            #else
               buf[j] = u.s32[i+j];
            #endif
         }
      }

      for (j=0; j<cnt; j++) {
         if (buf[j] >= 0 && buf[j] < 32) {
            switch (buf[j]) {
               case '\t':
                  break;

               case '\n':
                  if (mode == PRINT_PROGRESS) {
                     buf[j] = '?';
                  }
                  break;

               default:
                  buf[j] = '?';
            }
         }
      }

      convert_to_native(charbuf, sizeof(charbuf)-1, buf, cnt, &state, &processed_dest, &processed_src);
      if (processed_dest == 0 && processed_src == 0) break;
      charbuf[processed_dest] = 0;
      fputs(charbuf, mode == PRINT_LOG? stderr : stdout);
      i += processed_src;
   }

   if (mode == PRINT_NORMAL) {
      fputc('\n', stdout);
      fflush(stdout);
   }
   else if (mode == PRINT_LOG) {
      fputc('\n', stderr);
      fflush(stderr);
   }
   else {
      fflush(stdout);
   }
   
   fixscript_unlock_array(heap, params[0], 0, len, (void **)&u.u8, elem_size, ACCESS_READ_ONLY);

   if (mode == PRINT_PROMPT) {
      prompt_str = fixscript_create_string(heap, NULL, 0);
      if (!prompt_str.value) {
         #if !defined(__HAIKU__)
            if (!native_charset_utf8) {
               uselocale(prev_locale);
            }
         #endif
         pthread_mutex_unlock(mutex);
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
      memset(&state, 0, sizeof(NativeConvertState));
      for (;;) {
         if (!fgets(charbuf, sizeof(charbuf), stdin)) {
            break;
         }
         cp = charbuf;
         len = strlen(charbuf);
         for (;;) {
            convert_from_native(buf, sizeof(buf)/sizeof(wchar_t), cp, len, &state, &processed_dest, &processed_src);
            if (processed_dest == 0 && processed_src == 0) break;
            for (i=0; i<processed_dest; i++) {
               if (buf[i] == '\n') {
                  goto end;
               }
               err = fixscript_append_array_elem(heap, prompt_str, fixscript_int(buf[i]));
               if (err) {
                  #if !defined(__HAIKU__)
                     if (!native_charset_utf8) {
                        uselocale(prev_locale);
                     }
                  #endif
                  pthread_mutex_unlock(mutex);
                  return fixscript_error(heap, error, err);
               }
            }
            cp += processed_src;
            len -= processed_src;
         }
      }
      end:;
   }

   #if !defined(__HAIKU__)
      if (!native_charset_utf8) {
         uselocale(prev_locale);
      }
   #endif
   pthread_mutex_unlock(mutex);
   
   if (mode == PRINT_PROMPT) {
      return prompt_str;
   }
#endif

   return fixscript_int(0);
#endif /* __wasm__ */
}


static Value native_beep(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(_WIN32)
   DWORD console_mode;

   if (!gui_app && GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &console_mode)) {
      MessageBeep(-1);
   }
#elif defined(__wasm__)
#else
   if (isatty(STDOUT_FILENO)) {
      fprintf(stdout, "\a");
      fflush(stdout);
   }
#endif

   return fixscript_int(0);
}


#ifdef __wasm__
static void call_log_function_cont(Heap *heap, Value result, Value error, void *data)
{
   LogFunc *log_func = data;

   if (error.value) {
      fixscript_dump_value(heap, error, 1);
   }
   log_func->recursive = 0;
   log_func->cont_func(heap, fixscript_int(0), fixscript_int(0), log_func->cont_data);
}
#endif


static Value call_log_function(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   LogFunc *log_func = data;
#ifndef __wasm__
   Value func_error;
#endif

   if (log_func->recursive) {
      return log_func->func(heap, error, num_params, params, log_func->data);
   }

   log_func->recursive = 1;
#ifdef __wasm__
   fixscript_suspend(heap, &log_func->cont_func, &log_func->cont_data);
   fixscript_call_async(heap, log_func->custom_func, 1, &params[0], call_log_function_cont, log_func);
#else
   fixscript_call(heap, log_func->custom_func, 1, &func_error, params[0]);
   if (func_error.value) {
      fixscript_dump_value(heap, func_error, 1);
   }
   log_func->recursive = 0;
#endif
   return fixscript_int(0);
}


static Value set_log_function(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   LogFunc *log_func = data;
   int err;

   if (!params[0].value) {
      if (log_func) {
         fixscript_register_native_func(heap, "log#1", log_func->func, log_func->data);
      }
      return fixscript_int(0);
   }

   err = fixscript_get_function_name(heap, params[0], NULL, NULL, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   if (!log_func) {
      log_func = calloc(1, sizeof(LogFunc));
      if (!log_func) {
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
      log_func->func = fixscript_get_native_func(heap, "log#1", &log_func->data);

      fixscript_register_native_func(heap, "set_log_function#1", set_log_function, log_func);
      fixscript_register_cleanup(heap, free, log_func);
   }

   log_func->custom_func = params[0];
   fixscript_register_native_func(heap, "log#1", call_log_function, log_func);
   return fixscript_int(0);
}


#if !defined(_WIN32) && !defined(__wasm__)
static void console_interrupt_signal_handler(int signum)
{
   struct termios tios;
   const char restore[] = "\e[0m\e[7h\e[?7h\e[?25h";
   ssize_t ret, pos=0;

   while (pos < sizeof(restore)) {
      ret = write(STDOUT_FILENO, restore + pos, sizeof(restore) - pos);
      if (ret < 0) break;
      pos += ret;
   }

   if (tcgetattr(STDOUT_FILENO, &tios) == 0) {
      tios.c_iflag |= IXON;
      tios.c_lflag |= ECHO | ICANON | ISIG | IEXTEN;
      tios.c_cc[VMIN] = 1;
      tios.c_cc[VTIME] = 0;
      tcsetattr(STDOUT_FILENO, TCSAFLUSH, &tios);
   }

   signal(SIGINT, SIG_DFL);
   kill(getpid(), SIGINT);
}
#endif


#if !defined(_WIN32) && !defined(__wasm__)
static void *console_auto_flush_thread_main(void *data)
{
   pthread_mutex_t *mutex;
   pthread_cond_t *cond;

   mutex = get_mutex(&console_auto_flush_mutex);
   cond = get_cond(&console_auto_flush_cond);

   pthread_mutex_lock(mutex);
   while (console_auto_flush_active) {
      pthread_cond_wait(cond, mutex);
      if (!console_auto_flush_active) break;
      if (console_auto_flush_trigger) {
         pthread_mutex_unlock(mutex);
         usleep(10*1000);
         pthread_mutex_lock(mutex);
         if (console_auto_flush_active) {
            fflush(stdout);
         }
         console_auto_flush_trigger = 0;
      }
   }
   pthread_mutex_unlock(mutex);

   return NULL;
}
#endif


#if !defined(_WIN32) && !defined(__wasm__)
static void console_flush()
{
   pthread_mutex_t *mutex;
   pthread_cond_t *cond;

   mutex = get_mutex(&console_auto_flush_mutex);
   cond = get_cond(&console_auto_flush_cond);

   pthread_mutex_lock(mutex);
   console_auto_flush_trigger = 1;
   pthread_cond_signal(cond);
   pthread_mutex_unlock(mutex);
}
#endif


#if !defined(_WIN32) && !defined(__wasm__)
enum {
   CSSE_OCCURED     = 1 << 0,
   CSSE_WITH_RESET  = 1 << 1,
   CSSE_NEEDS_RESET = 1 << 2,
   CSSE_WAS_RESET   = 1 << 3
};

static void console_size_signal_handler(int signum)
{
   struct termios tios;
   int csse;

   console_size = -2;

   for (;;) {
      csse = console_send_size_event;
      if (csse & CSSE_NEEDS_RESET) {
         if (!__sync_bool_compare_and_swap(&console_send_size_event, csse, csse | CSSE_OCCURED | CSSE_WITH_RESET)) continue;
         if (tcgetattr(STDOUT_FILENO, &tios) == 0) {
            tios.c_cc[VMIN] = 0;
            tios.c_cc[VTIME] = 0;
            tcsetattr(STDOUT_FILENO, TCSANOW, &tios);
         }
         __sync_fetch_and_or(&console_send_size_event, CSSE_WAS_RESET);
         break;
      }
      else {
         if (!__sync_bool_compare_and_swap(&console_send_size_event, csse, csse | CSSE_OCCURED)) continue;
         break;
      }
   }
}
#endif


#ifndef __wasm__
static void activate_console()
{
#if defined(_WIN32)
   HANDLE in, out;
   DWORD console_mode;
   
   in = GetStdHandle(STD_INPUT_HANDLE);
   out = GetStdHandle(STD_OUTPUT_HANDLE);
   if (GetConsoleMode(in, &console_mode)) {
      SetConsoleMode(in, console_mode & ~ENABLE_PROCESSED_INPUT);
   }
   if (GetConsoleMode(out, &console_mode)) {
      SetConsoleMode(out, console_mode & ~ENABLE_WRAP_AT_EOL_OUTPUT);
   }
   console_cur_attr = 0x07;
   GetConsoleScreenBufferInfo(out, &last_csbi);
#elif defined(__wasm__)
#else
   struct termios tios;
   pthread_mutex_t *mutex;

   if (tcgetattr(STDOUT_FILENO, &tios) == 0) {
      tios.c_iflag &= ~(IXON);
      tios.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
      tios.c_cc[VMIN] = 1;
      tios.c_cc[VTIME] = 0;
      tcsetattr(STDOUT_FILENO, TCSAFLUSH, &tios);
   }

   fprintf(stdout, "\e[0m"); // reset color
   fprintf(stdout, "\e[7l"); // disable line wrap (vt100)
   fprintf(stdout, "\e[?7l"); // disable line wrap (other)
   fflush(stdout);

   signal(SIGINT, console_interrupt_signal_handler);
   if (console_size == -1) {
      signal(SIGWINCH, console_size_signal_handler);
   }

   mutex = get_mutex(&console_auto_flush_mutex);
   pthread_mutex_lock(mutex);
   console_auto_flush_active = 1;
   pthread_mutex_unlock(mutex);
   pthread_create(&console_auto_flush_thread, NULL, console_auto_flush_thread_main, NULL);

   setvbuf(stdin, NULL, _IONBF, 0);
#endif
}
#endif


#ifndef __wasm__
static void deactivate_console()
{
#if defined(_WIN32)
   HANDLE in, out;
   DWORD console_mode;
   CONSOLE_CURSOR_INFO cci;
   
   in = GetStdHandle(STD_INPUT_HANDLE);
   out = GetStdHandle(STD_OUTPUT_HANDLE);
   if (GetConsoleMode(in, &console_mode)) {
      SetConsoleMode(in, console_mode | ENABLE_PROCESSED_INPUT);
   }
   if (GetConsoleMode(out, &console_mode)) {
      SetConsoleMode(out, console_mode | ENABLE_WRAP_AT_EOL_OUTPUT);
   }
   if (GetConsoleCursorInfo(out, &cci)) {
      cci.bVisible = 1;
      SetConsoleCursorInfo(out, &cci);
   }
#elif defined(__wasm__)
#else
   struct termios tios;
   pthread_mutex_t *mutex;
   pthread_cond_t *cond;

   mutex = get_mutex(&console_auto_flush_mutex);
   cond = get_cond(&console_auto_flush_cond);

   pthread_mutex_lock(mutex);
   console_auto_flush_active = 0;
   pthread_cond_signal(cond);
   pthread_mutex_unlock(mutex);
   pthread_join(console_auto_flush_thread, NULL);

   if (tcgetattr(STDOUT_FILENO, &tios) == 0) {
      tios.c_iflag |= IXON;
      tios.c_lflag |= ECHO | ICANON | ISIG | IEXTEN;
      tios.c_cc[VMIN] = 1;
      tios.c_cc[VTIME] = 0;
      tcsetattr(STDOUT_FILENO, TCSAFLUSH, &tios);
   }
   fprintf(stdout, "\e[0m"); // reset color
   fprintf(stdout, "\e[7h"); // enable line wrap (vt100)
   fprintf(stdout, "\e[?7h"); // enable line wrap (other)
   fprintf(stdout, "\e[?25h"); // show cursor
   fflush(stdout);

   signal(SIGINT, SIG_DFL);

   setvbuf(stdin, NULL, _IOLBF, 0);
#endif
}
#endif


#ifndef __wasm__
static void handle_console_cleanup()
{
   pthread_mutex_t *mutex;

   mutex = get_mutex(&console_mutex);
   if (mutex) {
      pthread_mutex_lock(mutex);
      if (clear_console_line()) {
         fflush(stdout);
      }
      if (console_active) {
         deactivate_console();
         console_active = 0;
      }
      pthread_mutex_unlock(mutex);
   }
}
#endif


static Value native_console_is_present(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   int is_present = console_is_present;
#ifdef _WIN32
   DWORD console_mode;
#endif

   if (is_present < 0) {
      #if defined(_WIN32)
         if (gui_app) {
            is_present = 0;
         }
         else {
            is_present = GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &console_mode) && GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &console_mode);
         }
      #elif defined(__wasm__)
         is_present = 0;
      #else
         is_present = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
      #endif
      console_is_present = is_present;
   }
   return fixscript_int(is_present);
}


static Value native_console_get_size(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(__wasm__)
   return fixscript_int(0);
#else
   int which = (intptr_t)data;
   int size;
#ifdef _WIN32
   CONSOLE_SCREEN_BUFFER_INFO csbi;
#else
   struct {
      unsigned short ws_row;
      unsigned short ws_col;
      unsigned short ws_xpixel;
      unsigned short ws_ypixel;
   } ws;
#endif

   size = console_size;
   if (size < 0) {
      #ifdef _WIN32
         if (gui_app) {
            size = 0;
         }
         else {
            if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
               size = ((uint16_t)csbi.dwSize.X) | (((int)(uint16_t)(csbi.srWindow.Bottom - csbi.srWindow.Top + 1)) << 16);
            }
            else {
               size = 0;
            }
         }
      #else
         if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0) {
            return fixscript_int(0);
         }
         if (size == -1) {
            signal(SIGWINCH, console_size_signal_handler);
         }
         size = ws.ws_col | (((int)ws.ws_row) << 16);
      #endif
      console_size = size;
   }

   return fixscript_int(which == 0? (size & 0xFFFF) : ((size >> 16) & 0xFFFF));
#endif /* __wasm__ */
}


static Value native_console_set_active(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(__wasm__)
   *error = fixscript_create_error_string(heap, "not supported");
   return fixscript_int(0);
#else
   pthread_mutex_t *mutex;

   if (!native_console_is_present(heap, error, 0, NULL, NULL).value) {
      *error = fixscript_create_error_string(heap, "console is not present");
      return fixscript_int(0);
   }

   mutex = get_mutex(&console_mutex);
   if (!mutex) {
      *error = fixscript_create_error_string(heap, "can't lock console");
      return fixscript_int(0);
   }

   pthread_mutex_lock(mutex);
   if (clear_console_line()) {
      fflush(stdout);
   }

   if (!console_active && params[0].value) {
      activate_console();
      console_active = 1;
   }
   else if (console_active && !params[0].value) {
      deactivate_console();
      console_active = 0;
   }

   pthread_mutex_unlock(mutex);
   return fixscript_int(0);
#endif /* __wasm__ */
}


static Value native_console_is_active(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   pthread_mutex_t *mutex;
   int active;

   mutex = get_mutex(&console_mutex);
   if (!mutex) {
      *error = fixscript_create_error_string(heap, "can't lock console");
      return fixscript_int(0);
   }

   pthread_mutex_lock(mutex);
   active = console_active;
   pthread_mutex_unlock(mutex);
   
   return fixscript_int(active);
}


static Value native_console_get_cursor(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(__wasm__)
   *error = fixscript_create_error_string(heap, "not supported");
   return fixscript_int(0);
#else
   pthread_mutex_t *mutex;
   Value cursor[2], ret;
   int err;
#ifdef _WIN32
   HANDLE out;
   CONSOLE_SCREEN_BUFFER_INFO csbi;
   int base_y;
#else
   char c;
   int i, ch, x, y, start;
#endif

   mutex = get_mutex(&console_mutex);
   if (!mutex) {
      *error = fixscript_create_error_string(heap, "can't lock console");
      return fixscript_int(0);
   }

   pthread_mutex_lock(mutex);
   if (!console_active) {
      pthread_mutex_unlock(mutex);
      *error = fixscript_create_error_string(heap, "console is not active");
      return fixscript_int(0);
   }

   cursor[0] = fixscript_int(0);
   cursor[1] = fixscript_int(0);

#ifdef _WIN32
   out = GetStdHandle(STD_OUTPUT_HANDLE);
   if (GetConsoleScreenBufferInfo(out, &csbi)) {
      base_y = csbi.dwSize.Y - (csbi.srWindow.Bottom - csbi.srWindow.Top + 1);

      cursor[0] = fixscript_int(csbi.dwCursorPosition.X);
      cursor[1] = fixscript_int(csbi.dwCursorPosition.Y - base_y);
   }
#else
   fprintf(stdout, "\e[6n");
   fflush(stdout);
   for (;;) {
      if (read(STDIN_FILENO, &c, 1) == 1) {
         ch = c;
      }
      else {
         ch = EOF;
      }
      if (ch != EOF) {
         if (console_input_len == sizeof(console_input_buf)) {
            memmove(console_input_buf, console_input_buf+1, --console_input_len);
         }
         console_input_buf[console_input_len++] = ch;

         if (ch == 'R' && console_input_len >= 6) {
            for (i=console_input_len-2; i>=0; i--) {
               if (console_input_buf[i] == '\e' && console_input_buf[i+1] == '[') {
                  start = i;
                  i += 2;
                  y = 0;
                  while (console_input_buf[i] >= '0' && console_input_buf[i] <= '9') {
                     y = y*10 + (console_input_buf[i++] - '0');
                  }
                  if (console_input_buf[i++] == ';') {
                     x = 0;
                     while (console_input_buf[i] >= '0' && console_input_buf[i] <= '9') {
                        x = x*10 + (console_input_buf[i++] - '0');
                     }
                     if (i == console_input_len-1) {
                        cursor[0] = fixscript_int(x-1);
                        cursor[1] = fixscript_int(y-1);
                        console_input_len = start;
                        goto end;
                     }
                  }
                  break;
               }
            }
         }
      }
   }
   end:;
#endif

   pthread_mutex_unlock(mutex);

   ret = fixscript_create_array(heap, 2);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   err = fixscript_set_array_range(heap, ret, 0, 2, cursor);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return ret;
#endif /* __wasm__ */
}


static Value native_console_set_cursor(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(__wasm__)
   *error = fixscript_create_error_string(heap, "not supported");
   return fixscript_int(0);
#else
   int relative = (intptr_t)data;
   pthread_mutex_t *mutex;
#ifdef _WIN32
   HANDLE out;
   CONSOLE_SCREEN_BUFFER_INFO csbi;
   int x, y, w, h, base_y;
#endif

   mutex = get_mutex(&console_mutex);
   if (!mutex) {
      *error = fixscript_create_error_string(heap, "can't lock console");
      return fixscript_int(0);
   }

   pthread_mutex_lock(mutex);
   if (!console_active) {
      pthread_mutex_unlock(mutex);
      *error = fixscript_create_error_string(heap, "console is not active");
      return fixscript_int(0);
   }

#ifdef _WIN32
   out = GetStdHandle(STD_OUTPUT_HANDLE);
   if (GetConsoleScreenBufferInfo(out, &csbi)) {
      base_y = csbi.dwSize.Y - (csbi.srWindow.Bottom - csbi.srWindow.Top + 1);

      if (relative) {
         x = csbi.dwCursorPosition.X;
         y = csbi.dwCursorPosition.Y - base_y;
         x += params[0].value;
         y += params[1].value;
      }
      else {
         x = params[0].value;
         y = params[1].value;
      }

      w = csbi.dwSize.X;
      h = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;

      if (x < 0) x = 0;
      if (y < -base_y) y = -base_y;
      if (x >= w) x = w-1;
      if (y >= h) y = h-1;

      csbi.dwCursorPosition.X = x;
      csbi.dwCursorPosition.Y = y + base_y;
      SetConsoleCursorPosition(out, csbi.dwCursorPosition);
   }
#else
   if (relative) {
      if (params[0].value < 0) {
         fprintf(stdout, "\e[%dD", -params[0].value);
      }
      if (params[0].value > 0) {
         fprintf(stdout, "\e[%dC", params[0].value);
      }
      if (params[1].value < 0) {
         fprintf(stdout, "\e[%dA", -params[1].value);
      }
      if (params[1].value > 0) {
         fprintf(stdout, "\e[%dB", params[1].value);
      }
   }
   else {
      fprintf(stdout, "\e[%d;%dH", params[1].value+1, params[0].value+1);
   }
   console_flush();
#endif

   pthread_mutex_unlock(mutex);
   return fixscript_int(0);
#endif /* __wasm__ */
}


static Value native_console_show_cursor(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(__wasm__)
   *error = fixscript_create_error_string(heap, "not supported");
   return fixscript_int(0);
#else
   int show = (intptr_t)data;
   pthread_mutex_t *mutex;
#ifdef _WIN32
   HANDLE out;
   CONSOLE_CURSOR_INFO cci;
#endif

   mutex = get_mutex(&console_mutex);
   if (!mutex) {
      *error = fixscript_create_error_string(heap, "can't lock console");
      return fixscript_int(0);
   }

   pthread_mutex_lock(mutex);
   if (!console_active) {
      pthread_mutex_unlock(mutex);
      *error = fixscript_create_error_string(heap, "console is not active");
      return fixscript_int(0);
   }

#ifdef _WIN32
   out = GetStdHandle(STD_OUTPUT_HANDLE);
   if (GetConsoleCursorInfo(out, &cci)) {
      cci.bVisible = show;
      SetConsoleCursorInfo(out, &cci);
   }
#else
   if (show) {
      fprintf(stdout, "\e[?25h");
   }
   else {
      fprintf(stdout, "\e[?25l");
   }
   console_flush();
#endif

   pthread_mutex_unlock(mutex);
   return fixscript_int(0);
#endif /* __wasm__ */
}


static Value native_console_clear(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(__wasm__)
   *error = fixscript_create_error_string(heap, "not supported");
   return fixscript_int(0);
#else
   pthread_mutex_t *mutex;
#ifdef _WIN32
   HANDLE out;
   CONSOLE_SCREEN_BUFFER_INFO csbi;
   COORD size, coord;
   SMALL_RECT rect;
   CHAR_INFO *chars;
   int64_t len64;
   int i;
#endif

   mutex = get_mutex(&console_mutex);
   if (!mutex) {
      *error = fixscript_create_error_string(heap, "can't lock console");
      return fixscript_int(0);
   }

   pthread_mutex_lock(mutex);
   if (!console_active) {
      pthread_mutex_unlock(mutex);
      *error = fixscript_create_error_string(heap, "console is not active");
      return fixscript_int(0);
   }

#ifdef _WIN32
   out = GetStdHandle(STD_OUTPUT_HANDLE);
   if (GetConsoleScreenBufferInfo(out, &csbi)) {
      size.X = csbi.dwSize.X;
      size.Y = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;

      len64 = ((int64_t)size.X) * ((int64_t)size.Y) * sizeof(CHAR_INFO);
      chars = len64 > 0 && len64 <= INT_MAX? malloc(len64) : NULL;
      if (chars) {
         for (i=0; i<size.X*size.Y; i++) {
            chars[i].Char.UnicodeChar = ' ';
            chars[i].Attributes = 0x07;
         }
         coord.X = 0;
         coord.Y = 0;
         rect.Left = 0;
         rect.Top = csbi.dwSize.Y - (csbi.srWindow.Bottom - csbi.srWindow.Top + 1);
         rect.Right = rect.Left+size.X-1;
         rect.Bottom = rect.Top+size.Y-1;
         csbi.dwCursorPosition.X = rect.Left;
         csbi.dwCursorPosition.Y = rect.Top;
         WriteConsoleOutput(out, chars, size, coord, &rect);
         SetConsoleCursorPosition(out, csbi.dwCursorPosition);
         free(chars);
      }
   }
#else
   fprintf(stdout, "\e[1;1H\e[2J");
   console_flush();
#endif

   pthread_mutex_unlock(mutex);
   return fixscript_int(0);
#endif /* __wasm__ */
}


static Value native_console_put_text(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(__wasm__)
   *error = fixscript_create_error_string(heap, "not supported");
   return fixscript_int(0);
#else
   pthread_mutex_t *mutex;
   union {
      uint8_t *u8;
      uint16_t *u16;
      int32_t *s32;
   } u;
#ifdef _WIN32
   CHAR_INFO *chars;
   HANDLE out;
   CONSOLE_SCREEN_BUFFER_INFO csbi;
   COORD size, coord;
   SMALL_RECT rect;
   int64_t len64;
   int ch;
#else
#if !defined(__HAIKU__)
   locale_t prev_locale = (locale_t)0;
#endif
   NativeConvertState state;
   wchar_t buf[128];
   char charbuf[128*4];
   int buf_cap = sizeof(buf) / sizeof(wchar_t) - 1;
   int processed_dest, processed_src;
   int j, cnt, bp;
#endif
   int i, err, off, len, elem_size;

   mutex = get_mutex(&console_mutex);
   if (!mutex) {
      *error = fixscript_create_error_string(heap, "can't lock console");
      return fixscript_int(0);
   }

   pthread_mutex_lock(mutex);
   if (!console_active) {
      pthread_mutex_unlock(mutex);
      *error = fixscript_create_error_string(heap, "console is not active");
      return fixscript_int(0);
   }

   if (num_params == 1) {
      off = 0;
      err = fixscript_get_array_length(heap, params[0], &len);
      if (err) {
         pthread_mutex_unlock(mutex);
         return fixscript_error(heap, error, err);
      }
   }
   else {
      off = params[1].value;
      len = params[2].value;
   }

#ifdef _WIN32
   err = fixscript_get_array_element_size(heap, params[0], &elem_size);
   if (!err) {
      err = fixscript_lock_array(heap, params[0], off, len, (void **)&u.u8, elem_size, ACCESS_READ_ONLY);
   }
   if (err) {
      pthread_mutex_unlock(mutex);
      return fixscript_error(heap, error, err);
   }

   if (len > 0) {
      len64 = ((int64_t)len) * sizeof(CHAR_INFO);
      chars = len64 <= INT_MAX? malloc(len64) : NULL;
      if (!chars) {
         fixscript_unlock_array(heap, params[0], off, len, (void **)&u.u8, elem_size, ACCESS_READ_ONLY);
         pthread_mutex_unlock(mutex);
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }

      if (elem_size == 1) {
         for (i=0; i<len; i++) {
            chars[i].Char.UnicodeChar = u.u8[i];
            chars[i].Attributes = console_cur_attr;
         }
      }
      else if (elem_size == 2) {
         for (i=0; i<len; i++) {
            chars[i].Char.UnicodeChar = u.u16[i];
            chars[i].Attributes = console_cur_attr;
         }
      }
      else {
         for (i=0; i<len; i++) {
            ch = u.s32[i];
            if (ch < 0 || ch > 0xFFFF) {
               ch = 0xFFFD;
            }
            chars[i].Char.UnicodeChar = ch;
            chars[i].Attributes = console_cur_attr;
         }
      }
      
      out = GetStdHandle(STD_OUTPUT_HANDLE);
      if (GetConsoleScreenBufferInfo(out, &csbi)) {
         size.X = len;
         size.Y = 1;
         coord.X = 0;
         coord.Y = 0;
         rect.Left = csbi.dwCursorPosition.X;
         rect.Top = csbi.dwCursorPosition.Y;
         rect.Right = rect.Left+len-1;
         rect.Bottom = rect.Top;
         WriteConsoleOutput(out, chars, size, coord, &rect);

         csbi.dwCursorPosition.X += len;
         if (csbi.dwCursorPosition.X >= csbi.dwSize.X) {
            csbi.dwCursorPosition.X = csbi.dwSize.X-1;
         }
         SetConsoleCursorPosition(out, csbi.dwCursorPosition);
      }

      free(chars);
   }

   fixscript_unlock_array(heap, params[0], off, len, (void **)&u.u8, elem_size, ACCESS_READ_ONLY);
#else
   err = fixscript_get_array_element_size(heap, params[0], &elem_size);
   if (!err) {
      err = fixscript_lock_array(heap, params[0], off, len, (void **)&u.u8, elem_size, ACCESS_READ_ONLY);
   }
   if (err) {
      pthread_mutex_unlock(mutex);
      return fixscript_error(heap, error, err);
   }

   #if !defined(__HAIKU__)
      if (!native_charset_utf8) {
         prev_locale = uselocale(cur_locale);
      }
   #endif
   memset(&state, 0, sizeof(NativeConvertState));

   for (i=0; i<len; ) {
      cnt = len - i;
      if (cnt > buf_cap) {
         cnt = buf_cap;
      }

      if (elem_size == 1) {
         for (j=0; j<cnt; j++) {
            buf[j] = u.u8[i+j];
         }
      }
      else if (elem_size == 2) {
         for (j=0; j<cnt; j++) {
            buf[j] = u.u16[i+j];
         }
      }
      else if (elem_size == 4) {
         for (j=0; j<cnt; j++) {
            #if WCHAR_MAX <= 0xFFFF
               buf[j] = (u.s32[i+j] < 0 || u.s32[i+j] > 0xFFFF)? 0xFFFD : u.s32[i+j];
            #else
               buf[j] = u.s32[i+j];
            #endif
         }
      }

      for (j=0; j<cnt; j++) {
         if (buf[j] >= 0 && buf[j] < 32) {
            buf[j] = '?';
         }
      }

      bp = 0;
      for (;;) {
         convert_to_native(charbuf, sizeof(charbuf)-1, buf + bp, cnt - bp, &state, &processed_dest, &processed_src);
         if (processed_dest == 0 && processed_src == 0) break;
         charbuf[processed_dest] = 0;
         fputs(charbuf, stdout);
         bp += processed_src;
      }
      i += cnt;
   }
   
   #if !defined(__HAIKU__)
      if (!native_charset_utf8) {
         uselocale(prev_locale);
      }
   #endif
   fixscript_unlock_array(heap, params[0], off, len, (void **)&u.u8, elem_size, ACCESS_READ_ONLY);
   console_flush();
#endif

   pthread_mutex_unlock(mutex);
   return fixscript_int(0);
#endif /* __wasm__ */
}


static Value native_console_set_color(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(__wasm__)
   *error = fixscript_create_error_string(heap, "not supported");
   return fixscript_int(0);
#else
   int reset = (intptr_t)data;
   pthread_mutex_t *mutex;
   int background = 0, foreground = 0;

   if (!reset) {
      background = params[0].value;
      foreground = params[1].value;
      if (background < 0 || background > 15 || foreground < 0 || foreground > 15) {
         *error = fixscript_create_error_string(heap, "invalid color value");
         return fixscript_int(0);
      }
   }

   mutex = get_mutex(&console_mutex);
   if (!mutex) {
      *error = fixscript_create_error_string(heap, "can't lock console");
      return fixscript_int(0);
   }

   pthread_mutex_lock(mutex);
   if (!console_active) {
      pthread_mutex_unlock(mutex);
      *error = fixscript_create_error_string(heap, "console is not active");
      return fixscript_int(0);
   }

#ifdef _WIN32
   if (reset) {
      console_cur_attr = 0x07;
   }
   else {
      console_cur_attr = (background << 4) | foreground;
   }
#else
   if (reset) {
      fprintf(stdout, "\e[0m");
   }
   else {
      switch (background) {
         case  0: background = 40; break;
         case  1: background = 44; break;
         case  2: background = 42; break;
         case  3: background = 46; break;
         case  4: background = 41; break;
         case  5: background = 45; break;
         case  6: background = 43; break;
         case  7: background = 47; break;
         case  8: background = 100; break;
         case  9: background = 104; break;
         case 10: background = 102; break;
         case 11: background = 106; break;
         case 12: background = 101; break;
         case 13: background = 105; break;
         case 14: background = 103; break;
         case 15: background = 107; break;
      }
      switch (foreground) {
         case  0: foreground = 30; break;
         case  1: foreground = 34; break;
         case  2: foreground = 32; break;
         case  3: foreground = 36; break;
         case  4: foreground = 31; break;
         case  5: foreground = 35; break;
         case  6: foreground = 33; break;
         case  7: foreground = 37; break;
         case  8: foreground = 90; break;
         case  9: foreground = 94; break;
         case 10: foreground = 92; break;
         case 11: foreground = 96; break;
         case 12: foreground = 91; break;
         case 13: foreground = 95; break;
         case 14: foreground = 93; break;
         case 15: foreground = 97; break;
      }
      fprintf(stdout, "\e[%d;%dm", foreground, background);
   }
   console_flush();
#endif

   pthread_mutex_unlock(mutex);
   return fixscript_int(0);
#endif /* __wasm__ */
}


static Value native_console_scroll(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(__wasm__)
   *error = fixscript_create_error_string(heap, "not supported");
   return fixscript_int(0);
#else
   int rel_y = params[0].value;
   pthread_mutex_t *mutex;
#ifdef _WIN32
   HANDLE out;
   CONSOLE_SCREEN_BUFFER_INFO csbi;
   SMALL_RECT rect;
   COORD origin;
   CHAR_INFO fill;
   int x, y, w, h, base_y;
#endif

   mutex = get_mutex(&console_mutex);
   if (!mutex) {
      *error = fixscript_create_error_string(heap, "can't lock console");
      return fixscript_int(0);
   }

   pthread_mutex_lock(mutex);
   if (!console_active) {
      pthread_mutex_unlock(mutex);
      *error = fixscript_create_error_string(heap, "console is not active");
      return fixscript_int(0);
   }

#ifdef _WIN32
   out = GetStdHandle(STD_OUTPUT_HANDLE);

   fill.Char.UnicodeChar = ' ';
   fill.Attributes = console_cur_attr;

   if (GetConsoleScreenBufferInfo(out, &csbi)) {
      base_y = csbi.dwSize.Y - (csbi.srWindow.Bottom - csbi.srWindow.Top + 1);

      x = csbi.dwCursorPosition.X;
      y = csbi.dwCursorPosition.Y - base_y;
      y -= rel_y;

      w = csbi.dwSize.X;
      h = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;

      if (x < 0) x = 0;
      if (y < -base_y) y = -base_y;
      if (x >= w) x = w-1;
      if (y >= h) y = h-1;

      if (rel_y > 0) {
         rect.Left = 0;
         rect.Top = 0;
         rect.Right = csbi.dwSize.X;
         rect.Bottom = csbi.dwSize.Y;
         origin.X = 0;
         origin.Y = -rel_y;
         ScrollConsoleScreenBuffer(out, &rect, NULL, origin, &fill);
      }
      else {
         rect.Left = 0;
         rect.Top = base_y;
         rect.Right = csbi.dwSize.X;
         rect.Bottom = base_y+h;
         origin.X = 0;
         origin.Y = base_y-rel_y;
         ScrollConsoleScreenBuffer(out, &rect, &rect, origin, &fill);
      }

      csbi.dwCursorPosition.X = x;
      csbi.dwCursorPosition.Y = y + base_y;
      SetConsoleCursorPosition(out, csbi.dwCursorPosition);
   }
#else
   if (rel_y > 0) {
      fprintf(stdout, "\e[%dS\e[%dA", rel_y, rel_y);
   }
   else if (rel_y < 0) {
      fprintf(stdout, "\e[%dT\e[%dB", -rel_y, -rel_y);
   }
   console_flush();
#endif

   pthread_mutex_unlock(mutex);
   return fixscript_int(0);
#endif /* __wasm__ */
}


#if !defined(_WIN32) && !defined(__wasm__)
static int get_char()
{
   int ch;
   char c;
   
   if (console_input_len > 0) {
      ch = console_input_buf[0];
      memmove(console_input_buf, console_input_buf+1, --console_input_len);
      return ch;
   }

   if (read(STDIN_FILENO, &c, 1) == 1) {
      return c;
   }
   return EOF;
}

static int get_modifiers(int ch)
{
   int mod = 0;
   if (ch >= '2' && ch <= '8') {
      ch -= '1';
      if (ch & 1) mod |= MOD_SHIFT;
      if (ch & 2) mod |= MOD_ALT;
      if (ch & 4) mod |= MOD_CTRL;
   }
   return mod;
}

static int decode_ext_key(int ch, int key, uint32_t *event)
{
   int mod;
   switch (ch) {
      case '~':
         if (key) {
            *event = KEY_PRESSED(key, 0);
            ch = EOF;
         }
         break;
      case ';':
         mod = get_modifiers(get_char());
         ch = get_char();
         if (ch == '~') {
            if (key) {
               *event = KEY_PRESSED(key, mod);
               ch = EOF;
            }
         }
         else if (key == 0) {
            switch (ch) {
               case 'A': *event = KEY_PRESSED(KEY_UP, mod); ch = EOF; break;
               case 'B': *event = KEY_PRESSED(KEY_DOWN, mod); ch = EOF; break;
               case 'C': *event = KEY_PRESSED(KEY_RIGHT, mod); ch = EOF; break;
               case 'D': *event = KEY_PRESSED(KEY_LEFT, mod); ch = EOF; break;
               case 'H': *event = KEY_PRESSED(KEY_HOME, mod); ch = EOF; break;
               case 'F': *event = KEY_PRESSED(KEY_END, mod); ch = EOF; break;
            }
         }
         break;
   }
   return ch;
}

static Value handle_console_resize_event(Heap *heap, Value *error, int wait)
{
   struct termios tios;
   int size, width, height;

   do {
      native_console_get_size(heap, error, 0, NULL, (void *)0);
      size = console_size;
   }
   while (size == -2);

   if (wait) {
      if (__sync_fetch_and_and(&console_send_size_event, ~CSSE_NEEDS_RESET) & CSSE_WITH_RESET) {
         while ((console_send_size_event & CSSE_WAS_RESET) == 0) {
            #if defined(__APPLE__) || defined(__HAIKU__)
               sched_yield();
            #else
               pthread_yield();
            #endif
         }
      }
      __sync_fetch_and_and(&console_send_size_event, CSSE_OCCURED);
   }

   if (tcgetattr(STDOUT_FILENO, &tios) == 0) {
      tios.c_cc[VMIN] = 1;
      tios.c_cc[VTIME] = 0;
      tcsetattr(STDOUT_FILENO, TCSANOW, &tios);
   }

   width = size & 0xFFFF;
   height = (size >> 16) & 0xFFFF;
   if (width > 0x3FFF) width = 0x3FFF;
   if (height > 0x3FFF) height = 0x3FFF;
   return fixscript_int((EVENT_CONSOLE_RESIZED << 28) | (width << 14) | height);
}
#endif


static Value native_console_get_event(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(__wasm__)
   *error = fixscript_create_error_string(heap, "not supported");
   return fixscript_int(0);
#else
   pthread_mutex_t *mutex;
   uint32_t event = 0;
   int timeout = -1;
#ifdef _WIN32
   HANDLE in, out;
   CONSOLE_SCREEN_BUFFER_INFO csbi;
   INPUT_RECORD in_rec;
   DWORD num_events = 0;
   BOOL success;
   int key, mod, override, width, height, last_width=0, last_height=0;
#else
   struct termios tios;
   wchar_t chars[1];
   char bytes[1];
   NativeConvertState state;
   int processed_dest, processed_src;
   int ch, mod, csse;
#if !defined(__HAIKU__)
   locale_t prev_locale = (locale_t)0;
#endif
#endif

   if (num_params == 1) {
      timeout = params[0].value;
   }

   mutex = get_mutex(&console_mutex);
   if (!mutex) {
      *error = fixscript_create_error_string(heap, "can't lock console");
      return fixscript_int(0);
   }

   pthread_mutex_lock(mutex);
   if (!console_active) {
      pthread_mutex_unlock(mutex);
      *error = fixscript_create_error_string(heap, "console is not active");
      return fixscript_int(0);
   }

#ifdef _WIN32
   in = GetStdHandle(STD_INPUT_HANDLE);
   out = GetStdHandle(STD_OUTPUT_HANDLE);
again:
   for (;;) {
      if (GetConsoleScreenBufferInfo(out, &csbi)) {
         width = csbi.dwSize.X;
         height = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
         last_width = last_csbi.dwSize.X;
         last_height = last_csbi.srWindow.Bottom - last_csbi.srWindow.Top + 1;
         last_csbi = csbi;
      }
      else {
         width = 0;
         height = 0;
      }
      if (width != last_width || height != last_height) {
         if (width < 0) width = 0;
         if (height < 0) height = 0;
         if (width > 0x3FFF) width = 0x3FFF;
         if (height > 0x3FFF) height = 0x3FFF;
         pthread_mutex_unlock(mutex);
         return fixscript_int((EVENT_CONSOLE_RESIZED << 28) | (width << 14) | height);
      }

      success = PeekConsoleInput(in, &in_rec, 1, &num_events);
      if (!success || num_events == 1) break;
      if (timeout >= 0) {
         if (timeout < 10) break;
         Sleep(10);
         timeout -= 10;
      }
   }

   if (success && num_events == 1) {
      ReadConsoleInput(in, &in_rec, 1, &num_events);
      if (in_rec.EventType == KEY_EVENT) {
         if (in_rec.Event.KeyEvent.bKeyDown) {
            override = 0;
            key = KEY_NONE;
            switch (in_rec.Event.KeyEvent.wVirtualKeyCode) {
               case VK_ESCAPE:             key = KEY_ESCAPE; override = 1; break;
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
               //case VK_SNAPSHOT:           key = KEY_PRINT_SCREEN; break;
               //case VK_SCROLL:             key = KEY_SCROLL_LOCK; break;
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
               case VK_BACK:               key = KEY_BACKSPACE; override = 1; break;
               case VK_TAB:                key = KEY_TAB; override = 1; break;
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
               //case VK_CAPITAL:            key = KEY_CAPS_LOCK; break;
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
               case VK_RETURN:             key = KEY_ENTER; override = 1; break;
               //case VK_SHIFT:              key = KEY_LSHIFT; break;
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
               //case VK_CONTROL:            key = KEY_LCONTROL; break;
               case VK_LWIN:               key = KEY_LMETA; break;
               //case VK_MENU:               key = KEY_LALT; break;
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
               //case VK_NUMLOCK:            key = KEY_NUM_LOCK; break;
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
            mod = 0;
            if (in_rec.Event.KeyEvent.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) mod |= MOD_CTRL;
            if (in_rec.Event.KeyEvent.dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) mod |= MOD_ALT;
            if (in_rec.Event.KeyEvent.dwControlKeyState & SHIFT_PRESSED) mod |= MOD_SHIFT;
            if (in_rec.Event.KeyEvent.uChar.UnicodeChar != 0 && (mod & (MOD_CTRL | MOD_ALT)) == 0 && !override) {
               event = (EVENT_CHAR_TYPED << 28) | (in_rec.Event.KeyEvent.uChar.UnicodeChar & 0x0FFFFFFF);
            }
            else if (key) {
               event = KEY_PRESSED(key, mod);
            }
            else if (timeout != 0) {
               goto again;
            }
         }
         else if (timeout != 0) {
            goto again;
         }
      }
      else if (timeout != 0) {
         goto again;
      }
   }
#else
   fflush(stdout);

   if (__sync_fetch_and_and(&console_send_size_event, ~CSSE_OCCURED) & CSSE_OCCURED) {
      pthread_mutex_unlock(mutex);
      return handle_console_resize_event(heap, error, 0);
   }

   if (tcgetattr(STDOUT_FILENO, &tios) != 0) {
      pthread_mutex_unlock(mutex);
      *error = fixscript_create_error_string(heap, "I/O error");
      return fixscript_int(0);
   }

   if (timeout > 0 && timeout < 10) {
      timeout = 10;
   }

   if (timeout >= 0) {
      tios.c_cc[VMIN] = 0;
      tios.c_cc[VTIME] = timeout / 100;
      tcsetattr(STDOUT_FILENO, TCSANOW, &tios);
   }

   for (;;) {
      csse = console_send_size_event;
      if (csse & CSSE_OCCURED) {
         if (!__sync_bool_compare_and_swap(&console_send_size_event, csse, csse & ~CSSE_OCCURED)) continue;
         pthread_mutex_unlock(mutex);
         return handle_console_resize_event(heap, error, 0);
      }
      else {
         if (!__sync_bool_compare_and_swap(&console_send_size_event, csse, csse | CSSE_NEEDS_RESET)) continue;
         break;
      }
   }

   ch = get_char();
   if (ch == EOF && __sync_fetch_and_and(&console_send_size_event, ~CSSE_OCCURED) & CSSE_OCCURED) {
      pthread_mutex_unlock(mutex);
      return handle_console_resize_event(heap, error, 1);
   }
   if (ch == EOF && timeout > 0 && timeout < 100) {
      while (timeout >= 10) {
         usleep(10*1000);
         timeout -= 10;
         ch = get_char();
         if (ch == EOF && __sync_fetch_and_and(&console_send_size_event, ~CSSE_OCCURED) & CSSE_OCCURED) {
            pthread_mutex_unlock(mutex);
            return handle_console_resize_event(heap, error, 1);
         }
         if (ch != EOF) break;
      }
   }

   if (ch == 27) {
      tios.c_cc[VMIN] = 0;
      tios.c_cc[VTIME] = 0;
      tcsetattr(STDOUT_FILENO, TCSANOW, &tios);
      ch = get_char();
      if (ch == EOF) {
         if (__sync_fetch_and_and(&console_send_size_event, ~CSSE_OCCURED) & CSSE_OCCURED) {
            pthread_mutex_unlock(mutex);
            return handle_console_resize_event(heap, error, 1);
         }
         usleep(10*1000);
         ch = get_char();
         tios.c_cc[VMIN] = 1;
         tios.c_cc[VTIME] = 0;
         tcsetattr(STDOUT_FILENO, TCSANOW, &tios);
         if (ch == EOF) {
            if (__sync_fetch_and_and(&console_send_size_event, ~CSSE_OCCURED) & CSSE_OCCURED) {
               pthread_mutex_unlock(mutex);
               return handle_console_resize_event(heap, error, 1);
            }
            ch = 27;
         }
      }
      else {
         tios.c_cc[VMIN] = 1;
         tios.c_cc[VTIME] = 0;
         tcsetattr(STDOUT_FILENO, TCSANOW, &tios);
      }
      switch (ch) {
         case 'O':
            switch ((ch = get_char())) {
               case 'P': event = KEY_PRESSED(KEY_F1, 0); ch = EOF; break;
               case 'Q': event = KEY_PRESSED(KEY_F2, 0); ch = EOF; break;
               case 'R': event = KEY_PRESSED(KEY_F3, 0); ch = EOF; break;
               case 'S': event = KEY_PRESSED(KEY_F4, 0); ch = EOF; break;

               case '2': case '3': case '4': case '5': case '6': case '7': case '8':
                  mod = get_modifiers(ch);
                  switch ((ch = get_char())) {
                     case 'P': event = KEY_PRESSED(KEY_F1, mod); ch = EOF; break;
                     case 'Q': event = KEY_PRESSED(KEY_F2, mod); ch = EOF; break;
                     case 'R': event = KEY_PRESSED(KEY_F3, mod); ch = EOF; break;
                     case 'S': event = KEY_PRESSED(KEY_F4, mod); ch = EOF; break;
                  }
                  break;

               case 'M': event = KEY_PRESSED(KEY_ENTER, MOD_SHIFT); ch = EOF; break;

               case EOF: ch = 'O'; break;
            }
            break;

         case '[':
            switch ((ch = get_char())) {
               case '1':
                  switch ((ch = get_char())) {
                     case '1': ch = decode_ext_key(get_char(), KEY_F1, &event); break;
                     case '2': ch = decode_ext_key(get_char(), KEY_F2, &event); break;
                     case '3': ch = decode_ext_key(get_char(), KEY_F3, &event); break;
                     case '4': ch = decode_ext_key(get_char(), KEY_F4, &event); break;
                     case '5': ch = decode_ext_key(get_char(), KEY_F5, &event); break;
                     case '7': ch = decode_ext_key(get_char(), KEY_F6, &event); break;
                     case '8': ch = decode_ext_key(get_char(), KEY_F7, &event); break;
                     case '9': ch = decode_ext_key(get_char(), KEY_F8, &event); break;
                     case '~': case ';': ch = decode_ext_key(ch, KEY_HOME, &event); break;
                  }
                  break;
               case '2':
                  switch ((ch = get_char())) {
                     case '0': ch = decode_ext_key(get_char(), KEY_F9, &event); break;
                     case '1': ch = decode_ext_key(get_char(), KEY_F10, &event); break;
                     case '3': ch = decode_ext_key(get_char(), KEY_F11, &event); break;
                     case '4': ch = decode_ext_key(get_char(), KEY_F12, &event); break;
                     case '~': case ';': ch = decode_ext_key(ch, KEY_INSERT, &event); break;
                  }
                  break;
               case '3': ch = decode_ext_key(get_char(), KEY_DELETE, &event); break;
               case '4':
                  switch ((ch = get_char())) {
                     case '~': case ';': ch = decode_ext_key(ch, KEY_END, &event); break;
                  }
                  break;
               case '5': ch = decode_ext_key(get_char(), KEY_PAGE_UP, &event); break;
               case '6': ch = decode_ext_key(get_char(), KEY_PAGE_DOWN, &event); break;
               case 'A': event = KEY_PRESSED(KEY_UP, 0); ch = EOF; break;
               case 'B': event = KEY_PRESSED(KEY_DOWN, 0); ch = EOF; break;
               case 'C': event = KEY_PRESSED(KEY_RIGHT, 0); ch = EOF; break;
               case 'D': event = KEY_PRESSED(KEY_LEFT, 0); ch = EOF; break;
               case 'H': event = KEY_PRESSED(KEY_HOME, 0); ch = EOF; break;
               case 'F': event = KEY_PRESSED(KEY_END, 0); ch = EOF; break;
               case 'Z': event = KEY_PRESSED(KEY_TAB, MOD_SHIFT); ch = EOF; break;
               case EOF: ch = '['; break;
            }
            break;

         case '\n': event = KEY_PRESSED(KEY_ENTER, MOD_ALT); ch = EOF; break;
      }
      if (event == 0) {
         switch (ch) {
            case 'q':  event = KEY_PRESSED(KEY_Q, MOD_ALT); ch = EOF; break;
            case 'w':  event = KEY_PRESSED(KEY_W, MOD_ALT); ch = EOF; break;
            case 'e':  event = KEY_PRESSED(KEY_E, MOD_ALT); ch = EOF; break;
            case 'r':  event = KEY_PRESSED(KEY_R, MOD_ALT); ch = EOF; break;
            case 't':  event = KEY_PRESSED(KEY_T, MOD_ALT); ch = EOF; break;
            case 'y':  event = KEY_PRESSED(KEY_Y, MOD_ALT); ch = EOF; break;
            case 'u':  event = KEY_PRESSED(KEY_U, MOD_ALT); ch = EOF; break;
            case 'i':  event = KEY_PRESSED(KEY_I, MOD_ALT); ch = EOF; break;
            case 'o':  event = KEY_PRESSED(KEY_O, MOD_ALT); ch = EOF; break;
            case 'p':  event = KEY_PRESSED(KEY_P, MOD_ALT); ch = EOF; break;
            case '[':  event = KEY_PRESSED(KEY_LBRACKET, MOD_ALT); ch = EOF; break;
            case ']':  event = KEY_PRESSED(KEY_RBRACKET, MOD_ALT); ch = EOF; break;
            case '\\': event = KEY_PRESSED(KEY_BACKSLASH, MOD_ALT); ch = EOF; break;
            case 'a':  event = KEY_PRESSED(KEY_A, MOD_ALT); ch = EOF; break;
            case 's':  event = KEY_PRESSED(KEY_S, MOD_ALT); ch = EOF; break;
            case 'd':  event = KEY_PRESSED(KEY_D, MOD_ALT); ch = EOF; break;
            case 'f':  event = KEY_PRESSED(KEY_F, MOD_ALT); ch = EOF; break;
            case 'g':  event = KEY_PRESSED(KEY_G, MOD_ALT); ch = EOF; break;
            case 'h':  event = KEY_PRESSED(KEY_H, MOD_ALT); ch = EOF; break;
            case 'j':  event = KEY_PRESSED(KEY_J, MOD_ALT); ch = EOF; break;
            case 'k':  event = KEY_PRESSED(KEY_K, MOD_ALT); ch = EOF; break;
            case 'l':  event = KEY_PRESSED(KEY_L, MOD_ALT); ch = EOF; break;
            case ';':  event = KEY_PRESSED(KEY_SEMICOLON, MOD_ALT); ch = EOF; break;
            case '\'': event = KEY_PRESSED(KEY_APOSTROPHE, MOD_ALT); ch = EOF; break;
            case 'z':  event = KEY_PRESSED(KEY_Z, MOD_ALT); ch = EOF; break;
            case 'x':  event = KEY_PRESSED(KEY_X, MOD_ALT); ch = EOF; break;
            case 'c':  event = KEY_PRESSED(KEY_C, MOD_ALT); ch = EOF; break;
            case 'v':  event = KEY_PRESSED(KEY_V, MOD_ALT); ch = EOF; break;
            case 'b':  event = KEY_PRESSED(KEY_B, MOD_ALT); ch = EOF; break;
            case 'n':  event = KEY_PRESSED(KEY_N, MOD_ALT); ch = EOF; break;
            case 'm':  event = KEY_PRESSED(KEY_M, MOD_ALT); ch = EOF; break;
            case ',':  event = KEY_PRESSED(KEY_COMMA, MOD_ALT); ch = EOF; break;
            case '.':  event = KEY_PRESSED(KEY_PERIOD, MOD_ALT); ch = EOF; break;
            case '/':  event = KEY_PRESSED(KEY_SLASH, MOD_ALT); ch = EOF; break;

            case 'Q':  event = KEY_PRESSED(KEY_Q, MOD_ALT | MOD_SHIFT); ch = EOF; break;
            case 'W':  event = KEY_PRESSED(KEY_W, MOD_ALT | MOD_SHIFT); ch = EOF; break;
            case 'E':  event = KEY_PRESSED(KEY_E, MOD_ALT | MOD_SHIFT); ch = EOF; break;
            case 'R':  event = KEY_PRESSED(KEY_R, MOD_ALT | MOD_SHIFT); ch = EOF; break;
            case 'T':  event = KEY_PRESSED(KEY_T, MOD_ALT | MOD_SHIFT); ch = EOF; break;
            case 'Y':  event = KEY_PRESSED(KEY_Y, MOD_ALT | MOD_SHIFT); ch = EOF; break;
            case 'U':  event = KEY_PRESSED(KEY_U, MOD_ALT | MOD_SHIFT); ch = EOF; break;
            case 'I':  event = KEY_PRESSED(KEY_I, MOD_ALT | MOD_SHIFT); ch = EOF; break;
            case 'O':  event = KEY_PRESSED(KEY_O, MOD_ALT | MOD_SHIFT); ch = EOF; break;
            case 'P':  event = KEY_PRESSED(KEY_P, MOD_ALT | MOD_SHIFT); ch = EOF; break;
            case '{':  event = KEY_PRESSED(KEY_LBRACKET, MOD_ALT | MOD_SHIFT); ch = EOF; break;
            case '}':  event = KEY_PRESSED(KEY_RBRACKET, MOD_ALT | MOD_SHIFT); ch = EOF; break;
            case '|':  event = KEY_PRESSED(KEY_BACKSLASH, MOD_ALT | MOD_SHIFT); ch = EOF; break;
            case 'A':  event = KEY_PRESSED(KEY_A, MOD_ALT | MOD_SHIFT); ch = EOF; break;
            case 'S':  event = KEY_PRESSED(KEY_S, MOD_ALT | MOD_SHIFT); ch = EOF; break;
            case 'D':  event = KEY_PRESSED(KEY_D, MOD_ALT | MOD_SHIFT); ch = EOF; break;
            case 'F':  event = KEY_PRESSED(KEY_F, MOD_ALT | MOD_SHIFT); ch = EOF; break;
            case 'G':  event = KEY_PRESSED(KEY_G, MOD_ALT | MOD_SHIFT); ch = EOF; break;
            case 'H':  event = KEY_PRESSED(KEY_H, MOD_ALT | MOD_SHIFT); ch = EOF; break;
            case 'J':  event = KEY_PRESSED(KEY_J, MOD_ALT | MOD_SHIFT); ch = EOF; break;
            case 'K':  event = KEY_PRESSED(KEY_K, MOD_ALT | MOD_SHIFT); ch = EOF; break;
            case 'L':  event = KEY_PRESSED(KEY_L, MOD_ALT | MOD_SHIFT); ch = EOF; break;
            case ':':  event = KEY_PRESSED(KEY_SEMICOLON, MOD_ALT | MOD_SHIFT); ch = EOF; break;
            case '"':  event = KEY_PRESSED(KEY_APOSTROPHE, MOD_ALT | MOD_SHIFT); ch = EOF; break;
            case 'Z':  event = KEY_PRESSED(KEY_Z, MOD_ALT | MOD_SHIFT); ch = EOF; break;
            case 'X':  event = KEY_PRESSED(KEY_X, MOD_ALT | MOD_SHIFT); ch = EOF; break;
            case 'C':  event = KEY_PRESSED(KEY_C, MOD_ALT | MOD_SHIFT); ch = EOF; break;
            case 'V':  event = KEY_PRESSED(KEY_V, MOD_ALT | MOD_SHIFT); ch = EOF; break;
            case 'B':  event = KEY_PRESSED(KEY_B, MOD_ALT | MOD_SHIFT); ch = EOF; break;
            case 'N':  event = KEY_PRESSED(KEY_N, MOD_ALT | MOD_SHIFT); ch = EOF; break;
            case 'M':  event = KEY_PRESSED(KEY_M, MOD_ALT | MOD_SHIFT); ch = EOF; break;
            case '<':  event = KEY_PRESSED(KEY_COMMA, MOD_ALT | MOD_SHIFT); ch = EOF; break;
            case '>':  event = KEY_PRESSED(KEY_PERIOD, MOD_ALT | MOD_SHIFT); ch = EOF; break;
            case '?':  event = KEY_PRESSED(KEY_SLASH, MOD_ALT | MOD_SHIFT); ch = EOF; break;
         }
      }
   }

   if (__sync_fetch_and_and(&console_send_size_event, ~CSSE_NEEDS_RESET) & CSSE_WITH_RESET) {
      while ((console_send_size_event & CSSE_WAS_RESET) == 0) {
         #if defined(__APPLE__) || defined(__HAIKU__)
            sched_yield();
         #else
            pthread_yield();
         #endif
      }
   }
   __sync_fetch_and_and(&console_send_size_event, CSSE_OCCURED);

   tios.c_cc[VMIN] = 1;
   tios.c_cc[VTIME] = 0;
   tcsetattr(STDOUT_FILENO, TCSANOW, &tios);

   if (ch != EOF) {
      switch (ch) {
         case 27:   event = KEY_PRESSED(KEY_ESCAPE, 0); break;
         case 127:  event = KEY_PRESSED(KEY_BACKSPACE, 0); break;

         case '\t': event = KEY_PRESSED(KEY_TAB, 0); break;
         case 17:   event = KEY_PRESSED(KEY_Q, MOD_CTRL); break;
         case 23:   event = KEY_PRESSED(KEY_W, MOD_CTRL); break;
         case 5:    event = KEY_PRESSED(KEY_E, MOD_CTRL); break;
         case 18:   event = KEY_PRESSED(KEY_R, MOD_CTRL); break;
         case 20:   event = KEY_PRESSED(KEY_T, MOD_CTRL); break;
         case 25:   event = KEY_PRESSED(KEY_Y, MOD_CTRL); break;
         case 21:   event = KEY_PRESSED(KEY_U, MOD_CTRL); break;
         case 15:   event = KEY_PRESSED(KEY_O, MOD_CTRL); break;
         case 16:   event = KEY_PRESSED(KEY_P, MOD_CTRL); break;
         case 29:   event = KEY_PRESSED(KEY_RBRACKET, MOD_CTRL); break;
         case 28:   event = KEY_PRESSED(KEY_BACKSLASH, MOD_CTRL); break;

         case 1:    event = KEY_PRESSED(KEY_A, MOD_CTRL); break;
         case 19:   event = KEY_PRESSED(KEY_S, MOD_CTRL); break;
         case 4:    event = KEY_PRESSED(KEY_D, MOD_CTRL); break;
         case 6:    event = KEY_PRESSED(KEY_F, MOD_CTRL); break;
         case 7:    event = KEY_PRESSED(KEY_G, MOD_CTRL); break;
         case 8:    event = KEY_PRESSED(KEY_H, MOD_CTRL); break;
         case 11:   event = KEY_PRESSED(KEY_K, MOD_CTRL); break;
         case 12:   event = KEY_PRESSED(KEY_L, MOD_CTRL); break;
         case '\n': event = KEY_PRESSED(KEY_ENTER, 0); break;

         case 26:   event = KEY_PRESSED(KEY_Z, MOD_CTRL); break;
         case 24:   event = KEY_PRESSED(KEY_X, MOD_CTRL); break;
         case 3:    event = KEY_PRESSED(KEY_C, MOD_CTRL); break;
         case 22:   event = KEY_PRESSED(KEY_V, MOD_CTRL); break;
         case 2:    event = KEY_PRESSED(KEY_B, MOD_CTRL); break;
         case 14:   event = KEY_PRESSED(KEY_N, MOD_CTRL); break;
         case 31:   event = KEY_PRESSED(KEY_SLASH, MOD_CTRL); break;

         case 0:    event = KEY_PRESSED(KEY_SPACE, MOD_CTRL); break;

         default:
            #if !defined(__HAIKU__)
               if (!native_charset_utf8) {
                  prev_locale = uselocale(cur_locale);
               }
            #endif
            memset(&state, 0, sizeof(NativeConvertState));
            for (;;) {
               bytes[0] = ch;
               convert_from_native(chars, 1, bytes, 1, &state, &processed_dest, &processed_src);
               if (processed_dest == 1 || processed_src != 1) break;
               ch = get_char();
            }
            if (chars[0]) {
               event = (EVENT_CHAR_TYPED << 28) | (chars[0] & 0x0FFFFFFF);
            }
            #if !defined(__HAIKU__)
               if (!native_charset_utf8) {
                  uselocale(prev_locale);
               }
            #endif
      }
   }
#endif

   pthread_mutex_unlock(mutex);
   return fixscript_int(event);
#endif /* __wasm__ */
}


#ifdef FIXIO_SQLITE

static void *sqlite_handle_func(Heap *heap, int op, void *p1, void *p2)
{
   SQLiteHandle *handle = p1;
   switch (op) {
      case HANDLE_OP_FREE:
         if (handle->db) {
            sqlite3_close_v2(handle->db);
         }
         free(handle);
         break;
   }
   return NULL;
}


static void *sqlite_stmt_handle_func(Heap *heap, int op, void *p1, void *p2)
{
   SQLiteStatementHandle *handle = p1;
   switch (op) {
      case HANDLE_OP_FREE:
         if (handle->stmt) {
            sqlite3_finalize(handle->stmt);
         }
         free(handle);
         break;
   }
   return NULL;
}


static SQLiteHandle *get_sqlite_handle(Heap *heap, Value *error, Value handle_val)
{
   SQLiteHandle *handle;

   handle = fixscript_get_handle(heap, handle_val, HANDLE_TYPE_SQLITE, NULL);
   if (!handle) {
      *error = fixscript_create_error_string(heap, "invalid SQLite database handle");
      return NULL;
   }

   if (!handle->db) {
      *error = fixscript_create_error_string(heap, "SQLite database is already closed");
      return NULL;
   }
   return handle;
}


static SQLiteStatementHandle *get_sqlite_stmt_handle(Heap *heap, Value *error, Value handle_val)
{
   SQLiteStatementHandle *handle;

   handle = fixscript_get_handle(heap, handle_val, HANDLE_TYPE_SQLITE_STMT, NULL);
   if (!handle) {
      *error = fixscript_create_error_string(heap, "invalid SQLite statement handle");
      return NULL;
   }

   if (!handle->stmt) {
      *error = fixscript_create_error_string(heap, "SQLite statement is already closed");
      return NULL;
   }
   return handle;
}


static Value native_sqlite_is_present(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   return fixscript_int(1);
}


static Value native_sqlite_open(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   SQLiteHandle *handle = NULL;
   Value ret = fixscript_int(0);
   char *fname = NULL;
   int err;

   handle = calloc(1, sizeof(SQLiteHandle));
   if (!handle) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   err = fixscript_get_string(heap, params[0], 0, -1, &fname, NULL);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   err = sqlite3_open(fname, &handle->db);
   if (err) {
      *error = fixscript_create_error_string(heap, sqlite3_errmsg(handle->db));
      sqlite3_close_v2(handle->db);
      goto error;
   }
   
   ret = fixscript_create_value_handle(heap, HANDLE_TYPE_SQLITE, handle, sqlite_handle_func);
   handle = NULL;
   if (!ret.value) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

error:
   free(handle);
   free(fname);
   return ret;
}


static Value native_sqlite_close(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   SQLiteHandle *handle;
   int err;

   handle = get_sqlite_handle(heap, error, params[0]);
   if (!handle) {
      return fixscript_int(0);
   }

   err = sqlite3_close_v2(handle->db);
   handle->db = NULL;
   if (err) {
      *error = fixscript_create_error_string(heap, sqlite3_errmsg(handle->db));
      return fixscript_int(0);
   }
   return fixscript_int(0);
}


static Value native_sqlite_exec(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   SQLiteHandle *handle;
   SQLiteStatementHandle *stmt_handle;
   Value ret = fixscript_int(0);
   sqlite3_stmt *stmt = NULL;
   Value *sql_params = NULL, long_val[2];
   char *sql = NULL, *sig = NULL, *text;
   void *bin_data;
   int i, err, len, num_sql_params=0, num_binds;
   union {
      uint64_t i;
      double f;
   } u;

   handle = get_sqlite_handle(heap, error, params[0]);
   if (!handle) {
      goto error;
   }

   err = fixscript_get_string(heap, params[1], 0, -1, &sql, NULL);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   if (params[2].value) {
      err = fixscript_get_string(heap, params[2], 0, -1, &sig, NULL);
      if (err) {
         fixscript_error(heap, error, err);
         goto error;
      }
      
      if (!params[3].value) {
         *error = fixscript_create_error_string(heap, "must provide parameters");
         goto error;
      }

      err = fixscript_get_array_length(heap, params[3], &num_sql_params);
      if (!err) {
         sql_params = calloc(num_sql_params, sizeof(Value));
         if (!sql_params) {
            err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
         }
      }
      if (!err) {
         err = fixscript_get_array_range(heap, params[3], 0, num_sql_params, sql_params);
      }
      if (err) {
         fixscript_error(heap, error, err);
         goto error;
      }

      if (strlen(sig) != num_sql_params) {
         *error = fixscript_create_error_string(heap, "parameter count mismatch");
         goto error;
      }
   }
   else {
      if (params[3].value) {
         *error = fixscript_create_error_string(heap, "must provide signature");
         goto error;
      }
   }

   err = sqlite3_prepare_v2(handle->db, sql, -1, &stmt, NULL);
   if (err) {
      *error = fixscript_create_error_string(heap, sqlite3_errmsg(handle->db));
      goto error;
   }
   
   num_binds = sqlite3_bind_parameter_count(stmt);
   if (num_binds != num_sql_params) {
      *error = fixscript_create_error_string(heap, "parameter count mismatch");
      goto error;
   }

   for (i=0; i<num_sql_params; i++) {
      switch (sig[i]) {
         case 'i':
            err = sqlite3_bind_int(stmt, i+1, fixscript_get_int(sql_params[i]));
            break;

         case 'l':
            if (sql_params[i].value) {
               err = fixscript_get_array_range(heap, sql_params[i], 0, 2, long_val);
               if (err) {
                  fixscript_error(heap, error, err);
                  goto error;
               }
               u.i = ((uint64_t)(uint32_t)long_val[0].value) | (((uint64_t)(uint32_t)long_val[1].value) << 32);
               err = sqlite3_bind_int64(stmt, i+1, u.i);
            }
            else {
               err = sqlite3_bind_null(stmt, i+1);
            }
            break;

         case 'f':
            err = sqlite3_bind_double(stmt, i+1, fixscript_get_float(sql_params[i]));
            break;

         case 'd':
            if (sql_params[i].value) {
               err = fixscript_get_array_range(heap, sql_params[i], 0, 2, long_val);
               if (err) {
                  fixscript_error(heap, error, err);
                  goto error;
               }
               u.i = ((uint64_t)(uint32_t)long_val[0].value) | (((uint64_t)(uint32_t)long_val[1].value) << 32);
               err = sqlite3_bind_double(stmt, i+1, u.f);
            }
            else {
               err = sqlite3_bind_null(stmt, i+1);
            }
            break;

         case 's':
            if (sql_params[i].value) {
               err = fixscript_get_string(heap, sql_params[i], 0, -1, &text, NULL);
               if (err) {
                  fixscript_error(heap, error, err);
                  goto error;
               }
               err = sqlite3_bind_text(stmt, i+1, text, -1, free);
            }
            else {
               err = sqlite3_bind_null(stmt, i+1);
            }
            break;

         case 'b':
            if (sql_params[i].value) {
               err = fixscript_get_array_length(heap, sql_params[i], &len);
               if (!err) {
                  err = fixscript_lock_array(heap, sql_params[i], 0, len, &bin_data, 1, ACCESS_READ_ONLY);
               }
               if (err) {
                  fixscript_error(heap, error, err);
                  goto error;
               }
               err = sqlite3_bind_blob(stmt, i+1, bin_data, len, SQLITE_TRANSIENT);
               fixscript_unlock_array(heap, sql_params[i], 0, len, &bin_data, 1, ACCESS_READ_ONLY);
            }
            else {
               err = sqlite3_bind_null(stmt, i+1);
            }
            break;

         default:
            *error = fixscript_create_error_string(heap, "invalid signature");
            goto error;
      }

      if (err) {
         *error = fixscript_create_error_string(heap, sqlite3_errmsg(handle->db));
         goto error;
      }
   }

   err = sqlite3_step(stmt);
   if (err == SQLITE_DONE) {
      if (sqlite3_column_count(stmt) == 0) {
         err = sqlite3_finalize(stmt);
         if (err) {
            *error = fixscript_create_error_string(heap, sqlite3_errmsg(handle->db));
         }
         goto error;
      }
   }
   else if (err != SQLITE_ROW) {
      *error = fixscript_create_error_string(heap, sqlite3_errmsg(handle->db));
      sqlite3_finalize(stmt);
      goto error;
   }

   stmt_handle = calloc(1, sizeof(SQLiteStatementHandle));
   if (!stmt_handle) {
      sqlite3_finalize(stmt);
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   stmt_handle->stmt = stmt;
   stmt_handle->done = (err == SQLITE_DONE);
   stmt_handle->ignore_step = 1;
   
   ret = fixscript_create_value_handle(heap, HANDLE_TYPE_SQLITE_STMT, stmt_handle, sqlite_stmt_handle_func);
   if (!ret.value) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

error:
   free(sql);
   free(sig);
   free(sql_params);
   return ret;
}


static Value native_sqlite_step(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   SQLiteStatementHandle *stmt_handle;
   int err;

   stmt_handle = get_sqlite_stmt_handle(heap, error, params[0]);
   if (!stmt_handle) {
      return fixscript_int(0);
   }

   if (stmt_handle->done) {
      return fixscript_int(0);
   }

   if (stmt_handle->ignore_step) {
      stmt_handle->ignore_step = 0;
      return fixscript_int(1);
   }

   err = sqlite3_step(stmt_handle->stmt);
   if (err == SQLITE_ROW) {
      return fixscript_int(1);
   }
   if (err != SQLITE_DONE) {
      *error = fixscript_create_error_string(heap, sqlite3_errstr(err));
      return fixscript_int(0);
   }
   stmt_handle->done = 1;
   return fixscript_int(0);
}


static Value native_sqlite_finalize(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   SQLiteStatementHandle *stmt_handle;
   int err;

   stmt_handle = get_sqlite_stmt_handle(heap, error, params[0]);
   if (!stmt_handle) {
      return fixscript_int(0);
   }

   err = sqlite3_finalize(stmt_handle->stmt);
   stmt_handle->stmt = NULL;
   if (err) {
      *error = fixscript_create_error_string(heap, sqlite3_errstr(err));
      return fixscript_int(0);
   }
   return fixscript_int(0);
}


static Value native_sqlite_column_count(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   SQLiteStatementHandle *stmt_handle;

   stmt_handle = get_sqlite_stmt_handle(heap, error, params[0]);
   if (!stmt_handle) {
      return fixscript_int(0);
   }

   return fixscript_int(sqlite3_column_count(stmt_handle->stmt));
}


static Value native_sqlite_column_name(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   SQLiteStatementHandle *stmt_handle;
   Value ret;
   const char *name;

   stmt_handle = get_sqlite_stmt_handle(heap, error, params[0]);
   if (!stmt_handle) {
      return fixscript_int(0);
   }

   name = sqlite3_column_name(stmt_handle->stmt, params[1].value);
   if (!name) {
      if (params[1].value < 0 || params[1].value >= sqlite3_column_count(stmt_handle->stmt)) {
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_BOUNDS);
      }
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   ret = fixscript_create_string(heap, name, -1);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return ret;
}


static Value native_sqlite_column_type(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   SQLiteStatementHandle *stmt_handle;
   const char *name;

   stmt_handle = get_sqlite_stmt_handle(heap, error, params[0]);
   if (!stmt_handle) {
      return fixscript_int(0);
   }

   name = sqlite3_column_decltype(stmt_handle->stmt, params[1].value);
   if (name) {
      if (strcmp(name, "INTEGER") == 0) return fixscript_int(SQL_TYPE_LONG);
      if (strcmp(name, "REAL") == 0)    return fixscript_int(SQL_TYPE_DOUBLE);
      if (strcmp(name, "TEXT") == 0)    return fixscript_int(SQL_TYPE_STRING);
      if (strcmp(name, "BLOB") == 0)    return fixscript_int(SQL_TYPE_BINARY);
   }

   if (params[1].value < 0 || params[1].value >= sqlite3_column_count(stmt_handle->stmt)) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_BOUNDS);
   }
   return fixscript_int(SQL_TYPE_UNKNOWN);
}


static Value native_sqlite_data_type(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   SQLiteStatementHandle *stmt_handle;
   int type;

   stmt_handle = get_sqlite_stmt_handle(heap, error, params[0]);
   if (!stmt_handle) {
      return fixscript_int(0);
   }

   type = sqlite3_column_type(stmt_handle->stmt, params[1].value);
   if (type == SQLITE_NULL) {
      if (params[1].value < 0 || params[1].value >= sqlite3_column_count(stmt_handle->stmt)) {
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_BOUNDS);
      }
      return fixscript_int(SQL_TYPE_UNKNOWN);
   }

   switch (type) {
      case SQLITE_INTEGER: return fixscript_int(SQL_TYPE_LONG);
      case SQLITE_FLOAT:   return fixscript_int(SQL_TYPE_DOUBLE);
      case SQLITE_TEXT:    return fixscript_int(SQL_TYPE_STRING);
      case SQLITE_BLOB:    return fixscript_int(SQL_TYPE_BINARY);
   }
   return fixscript_int(SQL_TYPE_UNKNOWN);
}


static Value native_sqlite_is_null(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   SQLiteStatementHandle *stmt_handle;

   stmt_handle = get_sqlite_stmt_handle(heap, error, params[0]);
   if (!stmt_handle) {
      return fixscript_int(0);
   }

   if (sqlite3_column_type(stmt_handle->stmt, params[1].value) == SQLITE_NULL) {
      if (params[1].value < 0 || params[1].value >= sqlite3_column_count(stmt_handle->stmt)) {
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_BOUNDS);
      }
      return fixscript_int(1);
   }
   return fixscript_int(0);
}


static Value native_sqlite_get_int(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   SQLiteStatementHandle *stmt_handle;
   int64_t value;

   stmt_handle = get_sqlite_stmt_handle(heap, error, params[0]);
   if (!stmt_handle) {
      return fixscript_int(0);
   }

   value = sqlite3_column_int64(stmt_handle->stmt, params[1].value);
   if (value < INT_MIN || value > INT_MAX) {
      *error = fixscript_create_error_string(heap, "integer overflow");
      return fixscript_int(0);
   }
   if (value == 0) {
      if (sqlite3_column_type(stmt_handle->stmt, params[1].value) == SQLITE_NULL) {
         if (params[1].value < 0 || params[1].value >= sqlite3_column_count(stmt_handle->stmt)) {
            return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_BOUNDS);
         }
         return params[2];
      }
   }
   return fixscript_int(value);
}


static Value native_sqlite_get_long(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   SQLiteStatementHandle *stmt_handle;
   uint64_t value;

   stmt_handle = get_sqlite_stmt_handle(heap, error, params[0]);
   if (!stmt_handle) {
      return fixscript_int(0);
   }

   value = sqlite3_column_int64(stmt_handle->stmt, params[1].value);
   if (value == 0) {
      if (sqlite3_column_type(stmt_handle->stmt, params[1].value) == SQLITE_NULL) {
         if (params[1].value < 0 || params[1].value >= sqlite3_column_count(stmt_handle->stmt)) {
            return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_BOUNDS);
         }
         return params[2];
      }
   }
   *error = fixscript_int(value >> 32);
   return fixscript_int(value);
}


static Value native_sqlite_get_float(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   int use_double = (data == (void *)1);
   SQLiteStatementHandle *stmt_handle;
   double value;
   union {
      double f;
      uint64_t i;
   } u;

   stmt_handle = get_sqlite_stmt_handle(heap, error, params[0]);
   if (!stmt_handle) {
      return fixscript_int(0);
   }

   value = sqlite3_column_double(stmt_handle->stmt, params[1].value);
   if (value == 0) {
      if (sqlite3_column_type(stmt_handle->stmt, params[1].value) == SQLITE_NULL) {
         if (params[1].value < 0 || params[1].value >= sqlite3_column_count(stmt_handle->stmt)) {
            return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_BOUNDS);
         }
         return params[2];
      }
   }
   if (use_double) {
      u.f = value;
      *error = fixscript_int(u.i >> 32);
      return fixscript_int(u.i);
   }
   return fixscript_float(value);
}


static Value native_sqlite_get_string(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   SQLiteStatementHandle *stmt_handle;
   const unsigned char *text;
   Value ret;

   stmt_handle = get_sqlite_stmt_handle(heap, error, params[0]);
   if (!stmt_handle) {
      return fixscript_int(0);
   }

   text = sqlite3_column_text(stmt_handle->stmt, params[1].value);
   if (!text) {
      if (params[1].value < 0 || params[1].value >= sqlite3_column_count(stmt_handle->stmt)) {
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_BOUNDS);
      }
      return fixscript_int(0);
   }

   ret = fixscript_create_string(heap, (const char *)text, -1);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return ret;
}


static Value native_sqlite_get_binary(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   SQLiteStatementHandle *stmt_handle;
   const void *blob;
   Value ret;

   stmt_handle = get_sqlite_stmt_handle(heap, error, params[0]);
   if (!stmt_handle) {
      return fixscript_int(0);
   }

   blob = sqlite3_column_blob(stmt_handle->stmt, params[1].value);
   if (!blob) {
      if (params[1].value < 0 || params[1].value >= sqlite3_column_count(stmt_handle->stmt)) {
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_BOUNDS);
      }
      return fixscript_int(0);
   }

   ret = fixscript_create_byte_array(heap, (const char *)blob, sqlite3_column_bytes(stmt_handle->stmt, params[1].value));
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return ret;
}


static Value native_sqlite_last_insert_rowid(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   SQLiteHandle *handle;
   uint64_t ret;

   handle = get_sqlite_handle(heap, error, params[0]);
   if (!handle) {
      return fixscript_int(0);
   }

   ret = sqlite3_last_insert_rowid(handle->db);
   *error = fixscript_int(ret >> 32);
   return fixscript_int(ret);
}

#endif /* FIXIO_SQLITE */


void fixio_register_functions(Heap *heap)
{
#ifndef __wasm__
   pthread_mutex_t *mutex;
   union {
      uint32_t i32;
      uint8_t i8[4];
   } u;
#endif
#if defined(_WIN32)
   uint16_t filename[256];
   uint32_t dwret;
   HANDLE self_file;
   LARGE_INTEGER pos;
   DWORD pe_header, read_bytes;
   uint16_t subsystem;
   int new_gui_app_value;
#elif !defined(__wasm__)
   NativeConvertState state;
   wchar_t wsrc[2];
   char wdest[8];
   int processed_dest, processed_src;
#if !defined(__HAIKU__)
   locale_t prev_locale;
#endif
#endif

#ifndef __wasm__
   u.i32 = 0x04030201;
   if (u.i8[0] != 0x01) {
      fprintf(stderr, "fatal error: FixIO supports little endian CPUs only\n");
      fflush(stderr);
      abort();
   }
#endif

#ifdef _WIN32
   if (gui_app == -1) {
      new_gui_app_value = 0;
      dwret = GetModuleFileName(NULL, filename, 255);
      if (dwret > 0 && dwret < 254) {
         self_file = CreateFile(filename, GENERIC_READ, FILE_SHARE_DELETE | FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
         if (self_file != INVALID_HANDLE_VALUE) {
            pos.LowPart = 0x3C;
            pos.HighPart = 0;
            if (SetFilePointerEx(self_file, pos, NULL, FILE_BEGIN) && ReadFile(self_file, &pe_header, 4, &read_bytes, NULL) && read_bytes == 4) {
               pos.LowPart = pe_header + 6*4 + 17*4;
               pos.HighPart = 0;
               if (SetFilePointerEx(self_file, pos, NULL, FILE_BEGIN) && ReadFile(self_file, &subsystem, 2, &read_bytes, NULL) && read_bytes == 2) {
                  if (subsystem == 2) {
                     new_gui_app_value = 1;
                  }
               }
            }
            CloseHandle(self_file);
         }
      }
      gui_app = new_gui_app_value;
   }
#endif

#if defined(__HAIKU__) || defined(__wasm__)
   native_charset_utf8 = 1;
#elif !defined(_WIN32)
   if (!cur_locale) {
      cur_locale = newlocale(LC_ALL_MASK, "", NULL);
   }
   if (!native_charset_utf8) {
      prev_locale = uselocale(cur_locale);
      if (MB_CUR_MAX > sizeof(((NativeConvertState *)NULL)->buf)) {
         native_charset_utf8 = 1;
      }
      else {
         memset(&state, 0, sizeof(NativeConvertState));
         wsrc[0] = 'A';
         wsrc[1] = 0x1234;
         convert_to_native(wdest, sizeof(wdest), wsrc, 2, &state, &processed_dest, &processed_src);
         if (processed_dest == 4 && processed_src == 2 && memcmp(wdest, "\101\341\210\264", 4) == 0) {
            native_charset_utf8 = 1;
         }
      }
      uselocale(prev_locale);
   }
#endif

   fixscript_register_handle_types(&handles_offset, NUM_HANDLE_TYPES);
   fixscript_register_heap_key(&async_process_key);

   if (__sync_val_compare_and_swap(&global_initialized, 0, 1) == 0) {
#if defined(_WIN32)
      WSADATA wsa_data;

      WSAStartup(MAKEWORD(2,2), &wsa_data);
#elif defined(__wasm__)
#else
      signal(SIGPIPE, SIG_IGN);
#endif
   }

#ifndef __wasm__
   mutex = get_mutex(&console_mutex);
   if (mutex) {
      pthread_mutex_lock(mutex);
      if (!console_initialized) {
         atexit(handle_console_cleanup);
         console_initialized = 1;
      }
      pthread_mutex_unlock(mutex);
   }
#endif
   
   fixscript_register_native_func(heap, "zcompress#1", native_zcompress_memory, (void *)(ZC_COMPRESS));
   fixscript_register_native_func(heap, "zcompress#3", native_zcompress_memory, (void *)(ZC_COMPRESS));
   fixscript_register_native_func(heap, "zuncompress#1", native_zcompress_memory, (void *)(0));
   fixscript_register_native_func(heap, "zuncompress#3", native_zcompress_memory, (void *)(0));
   fixscript_register_native_func(heap, "gzip_compress#1", native_zcompress_memory, (void *)(ZC_COMPRESS | ZC_GZIP));
   fixscript_register_native_func(heap, "gzip_compress#3", native_zcompress_memory, (void *)(ZC_COMPRESS | ZC_GZIP));
   fixscript_register_native_func(heap, "gzip_uncompress#1", native_zcompress_memory, (void *)(ZC_GZIP));
   fixscript_register_native_func(heap, "gzip_uncompress#3", native_zcompress_memory, (void *)(ZC_GZIP));

   fixscript_register_native_func(heap, "zcompress_create#1", native_zcompress_create, (void *)(ZC_COMPRESS));
   fixscript_register_native_func(heap, "zuncompress_create#0", native_zcompress_create, (void *)(0));
   fixscript_register_native_func(heap, "gzip_compress_create#1", native_zcompress_create, (void *)(ZC_COMPRESS | ZC_GZIP));
   fixscript_register_native_func(heap, "gzip_uncompress_create#0", native_zcompress_create, (void *)(ZC_GZIP));
   fixscript_register_native_func(heap, "zcompress_process#8", native_zcompress_process, NULL);
   fixscript_register_native_func(heap, "zcompress_get_read#1", native_zcompress_get_info, (void *)0);
   fixscript_register_native_func(heap, "zcompress_get_written#1", native_zcompress_get_info, (void *)1);

   fixscript_register_native_func(heap, "crc32#1", native_crc32, NULL);
   fixscript_register_native_func(heap, "crc32#2", native_crc32, NULL);
   fixscript_register_native_func(heap, "crc32#3", native_crc32, NULL);
   fixscript_register_native_func(heap, "crc32#4", native_crc32, NULL);

   fixscript_register_native_func(heap, "path_get_separator#0", native_path_get_separator, NULL);
   fixscript_register_native_func(heap, "path_get_prefix_length#1", native_path_get_prefix_length, NULL);
   fixscript_register_native_func(heap, "path_is_valid_name#1", native_path_is_valid_name, NULL);
   fixscript_register_native_func(heap, "path_get_current#0", native_path_get_current, NULL);
   fixscript_register_native_func(heap, "path_get_roots#0", native_path_get_roots, NULL);
   fixscript_register_native_func(heap, "path_get_files#1", native_path_get_files, NULL);
   fixscript_register_native_func(heap, "path_exists#1", native_path_exists, NULL);
   fixscript_register_native_func(heap, "path_get_type#1", native_path_get_type, NULL);
   fixscript_register_native_func(heap, "path_get_length#1", native_path_get_length, NULL);
   fixscript_register_native_func(heap, "path_get_modification_time#1", native_path_get_modification_time, NULL);
   fixscript_register_native_func(heap, "path_get_symlink#1", native_path_get_symlink, NULL);
   fixscript_register_native_func(heap, "path_create_directory#1", native_path_create_directory, NULL);
   fixscript_register_native_func(heap, "path_delete_file#1", native_path_delete_file, NULL);
   fixscript_register_native_func(heap, "path_delete_directory#1", native_path_delete_directory, NULL);

   fixscript_register_native_func(heap, "file_open#2", native_file_open, NULL);
   fixscript_register_native_func(heap, "file_close#1", native_file_close, NULL);
   fixscript_register_native_func(heap, "file_read#4", native_file_read, NULL);
   fixscript_register_native_func(heap, "file_write#4", native_file_write, NULL);
   fixscript_register_native_func(heap, "file_get_length#1", native_file_get_length, NULL);
   fixscript_register_native_func(heap, "file_set_length#3", native_file_set_length, NULL);
   fixscript_register_native_func(heap, "file_get_position#1", native_file_get_position, NULL);
   fixscript_register_native_func(heap, "file_set_position#3", native_file_set_position, NULL);
   fixscript_register_native_func(heap, "file_sync#1", native_file_sync, NULL);
   fixscript_register_native_func(heap, "file_lock#3", native_file_lock, NULL);
   fixscript_register_native_func(heap, "file_unlock#1", native_file_unlock, NULL);
   fixscript_register_native_func(heap, "file_get_native_descriptor#1", native_file_get_native_descriptor, NULL);
   fixscript_register_native_func(heap, "file_get_native_handle#1", native_file_get_native_handle, NULL);
   fixscript_register_native_func(heap, "file_exists#1", native_file_exists, NULL);

   fixscript_register_native_func(heap, "tcp_connection_open#2", native_tcp_connection_open, NULL);
   fixscript_register_native_func(heap, "tcp_connection_close#1", native_tcp_connection_close, NULL);
   fixscript_register_native_func(heap, "tcp_connection_read#5", native_tcp_connection_read, NULL);
   fixscript_register_native_func(heap, "tcp_connection_write#5", native_tcp_connection_write, NULL);

   fixscript_register_native_func(heap, "tcp_server_create#1", native_tcp_server_create, (void *)0);
   fixscript_register_native_func(heap, "tcp_server_create_local#1", native_tcp_server_create, (void *)1);
   fixscript_register_native_func(heap, "tcp_server_close#1", native_tcp_server_close, NULL);
   fixscript_register_native_func(heap, "tcp_server_accept#2", native_tcp_server_accept, NULL);

   fixscript_register_native_func(heap, "async_tcp_connection_open#4", native_async_tcp_connection_open, NULL);
   fixscript_register_native_func(heap, "async_tcp_connection_read#6", native_async_tcp_connection_read, NULL);
   fixscript_register_native_func(heap, "async_tcp_connection_write#6", native_async_tcp_connection_write, NULL);
   fixscript_register_native_func(heap, "async_tcp_connection_close#1", native_async_tcp_connection_close, NULL);
   fixscript_register_native_func(heap, "async_tcp_server_create#1", native_async_tcp_server_create, (void *)0);
   fixscript_register_native_func(heap, "async_tcp_server_create_local#1", native_async_tcp_server_create, (void *)1);
   fixscript_register_native_func(heap, "async_tcp_server_close#1", native_async_tcp_server_close, NULL);
   fixscript_register_native_func(heap, "async_tcp_server_accept#3", native_async_tcp_server_accept, NULL);
   fixscript_register_native_func(heap, "async_process#0", native_async_process, NULL);
   fixscript_register_native_func(heap, "async_process#1", native_async_process, NULL);
   fixscript_register_native_func(heap, "async_run_later#3", native_async_run_later, NULL);
   fixscript_register_native_func(heap, "async_quit#0", native_async_quit, NULL);
   fixscript_register_native_func(heap, "async_quit#1", native_async_quit, NULL);

   fixscript_register_native_func(heap, "process_create#4", native_process_create, NULL);
   fixscript_register_native_func(heap, "process_in_write#5", native_process_write, (void *)0);
   fixscript_register_native_func(heap, "process_out_read#5", native_process_read, (void *)1);
   fixscript_register_native_func(heap, "process_err_read#5", native_process_read, (void *)2);
   fixscript_register_native_func(heap, "process_in_read#4", native_process_read, (void *)0);
   fixscript_register_native_func(heap, "process_out_write#4", native_process_write, (void *)1);
   fixscript_register_native_func(heap, "process_err_write#4", native_process_write, (void *)2);
   fixscript_register_native_func(heap, "process_close_in#1", native_process_close, (void *)0);
   fixscript_register_native_func(heap, "process_close_out#1", native_process_close, (void *)1);
   fixscript_register_native_func(heap, "process_close_err#1", native_process_close, (void *)2);
   fixscript_register_native_func(heap, "process_wait#1", native_process_wait, NULL);
   fixscript_register_native_func(heap, "process_kill#1", native_process_kill, NULL);
   fixscript_register_native_func(heap, "process_kill#2", native_process_kill, NULL);
   fixscript_register_native_func(heap, "process_is_running#1", native_process_is_running, NULL);
   fixscript_register_native_func(heap, "process_get_id#1", native_process_get_id, NULL);
   fixscript_register_native_func(heap, "process_get_current_environment#0", native_process_get_current_environment, NULL);

   fixscript_register_native_func(heap, "clock_get_time#0", native_clock_get_time, (void *)0);
   fixscript_register_native_func(heap, "clock_get_micro_time#0", native_clock_get_time, (void *)1);
   fixscript_register_native_func(heap, "monotonic_get_time#0", native_monotonic_get_time, (void *)0);
   fixscript_register_native_func(heap, "monotonic_get_micro_time#0", native_monotonic_get_time, (void *)1);

   fixscript_register_native_func(heap, "array_create_view#2", native_array_create_view, NULL);
   fixscript_register_native_func(heap, "array_create_view#3", native_array_create_view, NULL);
   fixscript_register_native_func(heap, "array_create_view#4", native_array_create_view, NULL);

   fixscript_register_native_func(heap, "serialize_key#1", native_serialize_key, NULL);
   fixscript_register_native_func(heap, "serialize_key#2", native_serialize_key, NULL);
   fixscript_register_native_func(heap, "serialize_compare#2", native_serialize_compare, NULL);
   fixscript_register_native_func(heap, "serialize_compare#6", native_serialize_compare, NULL);

   fixscript_register_native_func(heap, "string_from_native#1", native_string_from_native, NULL);
   fixscript_register_native_func(heap, "string_from_native#2", native_string_from_native, NULL);
   fixscript_register_native_func(heap, "string_from_native#3", native_string_from_native, NULL);
   fixscript_register_native_func(heap, "string_from_native#4", native_string_from_native, NULL);
   fixscript_register_native_func(heap, "string_to_native#1", native_string_to_native, NULL);
   fixscript_register_native_func(heap, "string_to_native#2", native_string_to_native, NULL);
   fixscript_register_native_func(heap, "string_to_native#3", native_string_to_native, NULL);
   fixscript_register_native_func(heap, "string_to_native#4", native_string_to_native, NULL);

   fixscript_register_native_func(heap, "print#1", native_print, (void *)PRINT_NORMAL);
#ifdef _WIN32
   if (!gui_app)
#endif
   {
#ifndef __wasm__
      fixscript_register_native_func(heap, "log#1", native_print, (void *)PRINT_LOG);
#endif
   }
   fixscript_register_native_func(heap, "prompt#1", native_print, (void *)PRINT_PROMPT);
   fixscript_register_native_func(heap, "progress#1", native_print, (void *)PRINT_PROGRESS);
   fixscript_register_native_func(heap, "beep#0", native_beep, NULL);
   fixscript_register_native_func(heap, "set_log_function#1", set_log_function, NULL);

   fixscript_register_native_func(heap, "console_is_present#0", native_console_is_present, NULL);
   fixscript_register_native_func(heap, "console_get_width#0", native_console_get_size, (void *)0);
   fixscript_register_native_func(heap, "console_get_height#0", native_console_get_size, (void *)1);
   fixscript_register_native_func(heap, "console_set_active#1", native_console_set_active, NULL);
   fixscript_register_native_func(heap, "console_is_active#0", native_console_is_active, NULL);
   fixscript_register_native_func(heap, "console_get_cursor#0", native_console_get_cursor, NULL);
   fixscript_register_native_func(heap, "console_set_cursor#2", native_console_set_cursor, (void *)0);
   fixscript_register_native_func(heap, "console_move_cursor#2", native_console_set_cursor, (void *)1);
   fixscript_register_native_func(heap, "console_show_cursor#0", native_console_show_cursor, (void *)1);
   fixscript_register_native_func(heap, "console_hide_cursor#0", native_console_show_cursor, (void *)0);
   fixscript_register_native_func(heap, "console_clear#0", native_console_clear, NULL);
   fixscript_register_native_func(heap, "console_put_text#1", native_console_put_text, NULL);
   fixscript_register_native_func(heap, "console_put_text#3", native_console_put_text, NULL);
   fixscript_register_native_func(heap, "console_reset_color#0", native_console_set_color, (void *)1);
   fixscript_register_native_func(heap, "console_set_color#2", native_console_set_color, (void *)0);
   fixscript_register_native_func(heap, "console_scroll#1", native_console_scroll, NULL);
   fixscript_register_native_func(heap, "console_get_event#0", native_console_get_event, NULL);
   fixscript_register_native_func(heap, "console_get_event#1", native_console_get_event, NULL);

#ifdef FIXIO_SQLITE
   fixscript_register_native_func(heap, "sqlite_is_present#0", native_sqlite_is_present, NULL);
   fixscript_register_native_func(heap, "sqlite_open#1", native_sqlite_open, NULL);
   fixscript_register_native_func(heap, "sqlite_close#1", native_sqlite_close, NULL);
   fixscript_register_native_func(heap, "sqlite_exec#4", native_sqlite_exec, NULL);
   fixscript_register_native_func(heap, "sqlite_step#1", native_sqlite_step, NULL);
   fixscript_register_native_func(heap, "sqlite_finalize#1", native_sqlite_finalize, NULL);
   fixscript_register_native_func(heap, "sqlite_column_count#1", native_sqlite_column_count, NULL);
   fixscript_register_native_func(heap, "sqlite_column_name#2", native_sqlite_column_name, NULL);
   fixscript_register_native_func(heap, "sqlite_column_type#2", native_sqlite_column_type, NULL);
   fixscript_register_native_func(heap, "sqlite_data_type#2", native_sqlite_data_type, NULL);
   fixscript_register_native_func(heap, "sqlite_is_null#2", native_sqlite_is_null, NULL);
   fixscript_register_native_func(heap, "sqlite_get_int#3", native_sqlite_get_int, NULL);
   fixscript_register_native_func(heap, "sqlite_get_long#3", native_sqlite_get_long, NULL);
   fixscript_register_native_func(heap, "sqlite_get_float#3", native_sqlite_get_float, (void *)0);
   fixscript_register_native_func(heap, "sqlite_get_double#3", native_sqlite_get_float, (void *)1);
   fixscript_register_native_func(heap, "sqlite_get_string#2", native_sqlite_get_string, NULL);
   fixscript_register_native_func(heap, "sqlite_get_binary#2", native_sqlite_get_binary, NULL);
   fixscript_register_native_func(heap, "sqlite_last_insert_rowid#1", native_sqlite_last_insert_rowid, NULL);
#endif
}


void *fixio_get_console_mutex()
{
   return get_mutex(&console_mutex);
}


int fixio_is_console_active()
{
   return console_active;
}


void fixio_flush_console()
{
#if !defined(_WIN32) && !defined(__wasm__)
   if (console_active) {
      console_flush();
   }
   else {
      fflush(stdout);
   }
#endif
}


#ifndef __wasm__
static void event_thread(void *data)
{
   AsyncProcess *proc = data;

   for (;;) {
      wait_events(proc, -1);

      pthread_mutex_lock(&proc->foreign_mutex);
      proc->foreign_processed = 0;
      proc->foreign_notify_func(proc->foreign_notify_data);
      while (!proc->foreign_processed) {
         pthread_cond_wait(&proc->foreign_cond, &proc->foreign_mutex);
      }
      pthread_mutex_unlock(&proc->foreign_mutex);
   }
}
#endif


void fixio_integrate_event_loop(Heap *heap, IOEventNotifyFunc notify_func, void *notify_data)
{
#ifndef __wasm__
   AsyncProcess *proc;
   Value error;

   proc = get_async_process(heap, &error);
   if (!proc || proc->foreign_notify_func) {
      fprintf(stderr, "foreign event loop already registered!\n");
      fflush(stderr);
      abort();
      return;
   }

   async_process_ref(proc);
   proc->foreign_notify_func = notify_func;
   proc->foreign_notify_data = notify_data;

   if (pthread_mutex_init(&proc->foreign_mutex, NULL) != 0) {
      fprintf(stderr, "can't initialize mutex for foreign event loop integration!\n");
      fflush(stderr);
      abort();
      return;
   }
   
   if (pthread_cond_init(&proc->foreign_cond, NULL) != 0) {
      fprintf(stderr, "can't initialize condition for foreign event loop integration!\n");
      fflush(stderr);
      abort();
      return;
   }

   if (!async_run_thread(event_thread, proc)) {
      fprintf(stderr, "can't create thread for foreign event loop integration!\n");
      fflush(stderr);
      abort();
      return;
   }
#endif
}


void fixio_process_events(Heap *heap)
{
#ifndef __wasm__
   AsyncProcess *proc;
   Value error;

   proc = get_async_process(heap, &error);
   if (!proc || !proc->foreign_notify_func) {
      fprintf(stderr, "foreign event loop not registered!\n");
      fflush(stderr);
      abort();
      return;
   }
   
   if (!process_events(proc, heap, &error)) {
      fixscript_dump_value(heap, error, 1);
   }

   pthread_mutex_lock(&proc->foreign_mutex);
   proc->foreign_processed = 1;
   pthread_cond_signal(&proc->foreign_cond);
   pthread_mutex_unlock(&proc->foreign_mutex);
#endif
}
