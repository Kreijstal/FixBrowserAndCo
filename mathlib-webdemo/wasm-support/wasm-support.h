#ifndef WASM_SUPPORT_H
#define WASM_SUPPORT_H

#include <stdint.h>

#ifndef __wasi__
#define WASM_BROWSER
#endif

#define IMPORT(module, name) __attribute__ ((import_module(module), import_name(name))) 
#define EXPORT(name) __attribute__ ((export_name(name))) 

#ifdef __wasi__

enum {
   WASI_EVENTTYPE_CLOCK    = 0,
   WASI_EVENTTYPE_FD_READ  = 1,
   WASI_EVENTTYPE_FD_WRITE = 2
};

enum {
   WASI_CLOCKID_MONOTONIC = 1
};

enum {
   WASI_ERRNO_SUCCESS = 0,
   WASI_ERRNO_BADF    = 8,
   WASI_ERRNO_EXIST   = 20,
   WASI_ERRNO_IO      = 29,
   WASI_ERRNO_NOENT   = 44,
   WASI_ERRNO_NOTDIR  = 54,
   WASI_ERRNO_PERM    = 63
};

enum {
   WASI_PREOPENTYPE_DIR = 0
};

enum {
   WASI_LOOKUPFLAGS_SYMLINK_FOLLOW = 1 << 0
};

enum {
   WASI_OFLAGS_CREAT     = 1 << 0,
   WASI_OFLAGS_DIRECTORY = 1 << 1,
   WASI_OFLAGS_EXCL      = 1 << 2,
   WASI_OFLAGS_TRUNC     = 1 << 3
};

enum {
   WASI_FDFLAGS_APPEND   = 1 << 0,
   WASI_FDFLAGS_DSYNC    = 1 << 1,
   WASI_FDFLAGS_NONBLOCK = 1 << 2,
   WASI_FDFLAGS_RSYNC    = 1 << 3,
   WASI_FDFLAGS_SYNC     = 1 << 4
};

enum {
   WASI_RIGHTS_FD_DATASYNC             = 1 << 0,
   WASI_RIGHTS_FD_READ                 = 1 << 1,
   WASI_RIGHTS_FD_SEEK                 = 1 << 2,
   WASI_RIGHTS_FD_FDSTAT_SET_FLAGS     = 1 << 3,
   WASI_RIGHTS_FD_SYNC                 = 1 << 4,
   WASI_RIGHTS_FD_TELL                 = 1 << 5,
   WASI_RIGHTS_FD_WRITE                = 1 << 6,
   WASI_RIGHTS_FD_ADVISE               = 1 << 7,
   WASI_RIGHTS_FD_ALLOCATE             = 1 << 8,
   WASI_RIGHTS_PATH_CREATE_DIRECTORY   = 1 << 9,
   WASI_RIGHTS_PATH_CREATE_FILE        = 1 << 10,
   WASI_RIGHTS_PATH_LINK_SOURCE        = 1 << 11,
   WASI_RIGHTS_PATH_LINK_TARGET        = 1 << 12,
   WASI_RIGHTS_PATH_OPEN               = 1 << 13,
   WASI_RIGHTS_FD_READDIR              = 1 << 14,
   WASI_RIGHTS_PATH_READLINK           = 1 << 15,
   WASI_RIGHTS_PATH_RENAME_SOURCE      = 1 << 16,
   WASI_RIGHTS_PATH_RENAME_TARGET      = 1 << 17,
   WASI_RIGHTS_PATH_FILESTAT_GET       = 1 << 18,
   WASI_RIGHTS_PATH_FILESTAT_SET_SIZE  = 1 << 19,
   WASI_RIGHTS_PATH_FILESTAT_SET_TIMES = 1 << 20,
   WASI_RIGHTS_FD_FILESTAT_GET         = 1 << 21,
   WASI_RIGHTS_FD_FILESTAT_SET_SIZE    = 1 << 22,
   WASI_RIGHTS_FD_FILESTAT_SET_TIMES   = 1 << 23,
   WASI_RIGHTS_PATH_SYMLINK            = 1 << 24,
   WASI_RIGHTS_PATH_REMOVE_DIRECTORY   = 1 << 25,
   WASI_RIGHTS_PATH_UNLINK_FILE        = 1 << 26,
   WASI_RIGHTS_POLL_FD_READWRITE       = 1 << 27,
   WASI_RIGHTS_SOCK_SHUTDOWN           = 1 << 28,
   WASI_RIGHTS_SOCK_ACCEPT             = 1 << 29
};

enum {
   WASI_WHENCE_SET = 0,
   WASI_WHENCE_CUR = 1,
   WASI_WHENCE_END = 2
};

enum {
   WASI_FILETYPE_UNKNOWN          = 0,
   WASI_FILETYPE_BLOCK_DEVICE     = 1,
   WASI_FILETYPE_CHARACTER_DEVICE = 2,
   WASI_FILETYPE_DIRECTORY        = 3,
   WASI_FILETYPE_REGULAR_FILE     = 4,
   WASI_FILETYPE_SOCKET_DGRAM     = 5,
   WASI_FILETYPE_SOCKET_STREAM    = 6,
   WASI_FILETYPE_SYMBOLIC_LINK    = 7
};

typedef uint16_t wasi_errno_t;

typedef struct {
   char *buf;
   int len;
} wasi_iovec_t;

typedef struct {
   uint64_t userdata;
   uint8_t tag;
   union {
      struct {
         uint32_t id;
         uint64_t timeout;
         uint64_t precision;
         uint16_t flags;
      } clock;
      struct {
         int fd;
      } fd_read;
      struct {
         int fd;
      } fd_write;
   };
} wasi_subscription_t;

typedef struct {
   uint64_t userdata;
   wasi_errno_t error;
   uint8_t type;
   struct {
      uint64_t nbytes;
      uint16_t flags;
   } fd_readwrite;
} wasi_event_t;

typedef struct {
   uint8_t tag;
   union {
      struct {
         int pr_name_len;
      } dir;
   };
} wasi_prestat_t;

typedef struct {
   uint64_t dev;
   uint64_t ino;
   uint8_t filetype;
   uint64_t nlink;
   uint64_t size;
   uint64_t atim;
   uint64_t mtim;
   uint64_t ctim;
} wasi_filestat_t;

typedef struct {
   uint64_t d_next;
   uint64_t d_ino;
   uint32_t d_namlen;
   uint8_t d_type;
} wasi_dirent_t;

extern wasi_errno_t wasi_fd_read(int fd, wasi_iovec_t *iovecs, int len, int *ret) IMPORT("wasi_snapshot_preview1", "fd_read");
extern wasi_errno_t wasi_fd_write(int fd, wasi_iovec_t *iovecs, int len, int *ret) IMPORT("wasi_snapshot_preview1", "fd_write");
extern wasi_errno_t wasi_poll_oneoff(wasi_subscription_t *in, wasi_event_t *out, int num, int *ret) IMPORT("wasi_snapshot_preview1", "poll_oneoff");
extern wasi_errno_t wasi_clock_time_get(uint32_t id, uint64_t precision, uint64_t *ret) IMPORT("wasi_snapshot_preview1", "clock_time_get");
extern wasi_errno_t wasi_args_get(uint8_t **argv, uint8_t *argv_buf) IMPORT("wasi_snapshot_preview1", "args_get");
extern wasi_errno_t wasi_args_sizes_get(int *num_args, int *buf_size) IMPORT("wasi_snapshot_preview1", "args_sizes_get");
extern wasi_errno_t wasi_fd_prestat_get(int fd, wasi_prestat_t *ret) IMPORT("wasi_snapshot_preview1", "fd_prestat_get");
extern wasi_errno_t wasi_fd_prestat_dir_name(int fd, uint8_t *path, int path_len) IMPORT("wasi_snapshot_preview1", "fd_prestat_dir_name");
extern wasi_errno_t wasi_path_open(int fd, uint32_t dirflags, const char *path, int path_len, uint16_t oflags, uint64_t fs_rights_base, uint64_t fs_rights_inheriting, uint16_t fdflags, int *ret) IMPORT("wasi_snapshot_preview1", "path_open");
extern wasi_errno_t wasi_fd_close(int fd) IMPORT("wasi_snapshot_preview1", "fd_close");
extern wasi_errno_t wasi_fd_filestat_get(int fd, wasi_filestat_t *ret) IMPORT("wasi_snapshot_preview1", "fd_filestat_get");
extern wasi_errno_t wasi_fd_tell(int fd, uint64_t *ret) IMPORT("wasi_snapshot_preview1", "fd_tell");
extern wasi_errno_t wasi_fd_filestat_set_size(int fd, uint64_t size) IMPORT("wasi_snapshot_preview1", "fd_filestat_set_size");
extern wasi_errno_t wasi_fd_seek(int fd, int64_t offset, uint8_t whence, uint64_t *ret) IMPORT("wasi_snapshot_preview1", "fd_seek");
extern wasi_errno_t wasi_fd_datasync(int fd) IMPORT("wasi_snapshot_preview1", "fd_datasync");
extern wasi_errno_t wasi_path_create_directory(int fd, const char *path, int path_len) IMPORT("wasi_snapshot_preview1", "path_create_directory");
extern wasi_errno_t wasi_path_unlink_file(int fd, const char *path, int path_len) IMPORT("wasi_snapshot_preview1", "path_unlink_file");
extern wasi_errno_t wasi_path_remove_directory(int fd, const char *path, int path_len) IMPORT("wasi_snapshot_preview1", "path_remove_directory");
extern wasi_errno_t wasi_fd_readdir(int fd, uint8_t *buf, int buf_len, uint64_t cookie, int *ret) IMPORT("wasi_snapshot_preview1", "fd_readdir");
extern wasi_errno_t wasi_path_filestat_get(int fd, uint32_t dirflags, const char *path, int path_len, wasi_filestat_t *ret) IMPORT("wasi_snapshot_preview1", "path_filestat_get");
extern wasi_errno_t wasi_path_readlink(int fd, const char *path, int path_len, uint8_t *buf, int buf_len, int *ret) IMPORT("wasi_snapshot_preview1", "path_readlink");
extern void wasi_proc_exit(uint32_t code) IMPORT("wasi_snapshot_preview1", "proc_exit");

#endif

#ifdef __wasi__
#define WASM_TIMER_NULL NULL
typedef void *wasm_timer_t;
wasm_timer_t wasm_timer_start(int interval, int repeat, void (*func)(void *), void *data);
void wasm_timer_stop(wasm_timer_t timer);
void wasi_run_loop();
void wasi_exit_loop();
#else
#define WASM_TIMER_NULL 0
typedef double wasm_timer_t;
extern wasm_timer_t wasm_timer_start(int interval, int repeat, void (*func)(void *), void *data) IMPORT("env", "timer_start");
extern void wasm_timer_stop(wasm_timer_t timer) IMPORT("env", "timer_stop");
#endif

static inline wasm_timer_t wasm_sleep(int delay, void (*func)(void *), void *data)
{
   return wasm_timer_start(delay, 0, func, data);
}

void wasm_auto_suspend_set_num_instructions(int num_instructions);
int wasm_auto_suspend_get_num_instructions();
void wasm_auto_suspend_heap(void *heap);

int malloc_get_heap_used();
int malloc_get_heap_free();
int malloc_get_heap_size();

#ifdef WASM_EMBEDDED_DATA
const char *wasm_get_embedded_data(int *len);
#endif

enum {
   WASM_SUCCESS              = 0,
   WASM_ERROR_NOT_SUPPORTED  = -1,
   WASM_ERROR_OUT_OF_MEMORY  = -2,
   WASM_ERROR_FILE_NOT_FOUND = -3,
   WASM_ERROR_NOT_DIRECTORY  = -4,
   WASM_ERROR_ACCESS_DENIED  = -5,
   WASM_ERROR_IO_ERROR       = -6
};

const char *wasm_get_error_msg(int error);

enum {
   WASM_FILE_READ       = 0x01,
   WASM_FILE_WRITE      = 0x02,
   WASM_FILE_CREATE     = 0x04,
   WASM_FILE_TRUNCATE   = 0x08,
   WASM_FILE_APPEND     = 0x10
};

enum {
   WASM_FILE_TYPE_FILE      = 0,
   WASM_FILE_TYPE_DIRECTORY = 1,
   WASM_FILE_TYPE_SPECIAL   = 2,
   WASM_FILE_TYPE_SYMLINK   = 0x80
};

typedef struct wasm_file_t wasm_file_t;

typedef void (*wasm_path_create_directory_cont)(int error, void *data);
typedef void (*wasm_path_delete_file_cont)(int error, void *data);
typedef void (*wasm_path_delete_directory_cont)(int error, void *data);
typedef void (*wasm_path_get_files_cont)(int num_files, char **files, int error, void *data);
typedef void (*wasm_path_exists_cont)(int exists, int error, void *data);
typedef void (*wasm_path_get_info_cont)(int type, int64_t length, int64_t mtime, int error, void *data);
typedef void (*wasm_path_resolve_symlink_cont)(char *orig_path, int error, void *data);

typedef void (*wasm_file_open_cont)(wasm_file_t *file, int error, void *data);
typedef void (*wasm_file_close_cont)(int error, void *data);
typedef void (*wasm_file_read_cont)(int read_bytes, int error, void *data);
typedef void (*wasm_file_write_cont)(int written_bytes, int error, void *data);
typedef void (*wasm_file_get_length_cont)(int64_t length, int error, void *data);
typedef void (*wasm_file_set_length_cont)(int error, void *data);
typedef void (*wasm_file_get_position_cont)(int64_t position, int error, void *data);
typedef void (*wasm_file_set_position_cont)(int error, void *data);
typedef void (*wasm_file_sync_cont)(int error, void *data);
typedef void (*wasm_file_lock_cont)(int locked, int error, void *data);
typedef void (*wasm_file_unlock_cont)(int error, void *data);

void wasm_path_create_directory(const char *path, wasm_path_create_directory_cont func, void *data);
void wasm_path_delete_file(const char *path, wasm_path_delete_file_cont func, void *data);
void wasm_path_delete_directory(const char *path, wasm_path_delete_directory_cont func, void *data);
void wasm_path_get_files(const char *path, wasm_path_get_files_cont func, void *data);
void wasm_path_exists(const char *path, wasm_path_exists_cont func, void *data);
void wasm_path_get_info(const char *path, wasm_path_get_info_cont func, void *data);
void wasm_path_resolve_symlink(const char *path, wasm_path_resolve_symlink_cont func, void *data);

int wasm_path_create_directory_sync(const char *path, int *error_out);
int wasm_path_delete_file_sync(const char *path, int *error_out);
int wasm_path_delete_directory_sync(const char *path, int *error_out);
int wasm_path_get_files_sync(const char *path, int *num_files_out, char ***files_out, int *error_out);
int wasm_path_exists_sync(const char *path, int *exists_out, int *error_out);
int wasm_path_get_info_sync(const char *path, int *type_out, int64_t *length_out, int64_t *mtime_out, int *error_out);
int wasm_path_resolve_symlink_sync(const char *path, char **orig_path_out, int *error_out);

void wasm_file_open(const char *path, int flags, wasm_file_open_cont func, void *data);
void wasm_file_close(wasm_file_t *file, wasm_file_close_cont func, void *data);
void wasm_file_read(wasm_file_t *file, void *buf, int len, wasm_file_read_cont func, void *data);
void wasm_file_write(wasm_file_t *file, void *buf, int len, wasm_file_write_cont func, void *data);
void wasm_file_get_length(wasm_file_t *file, wasm_file_get_length_cont func, void *data);
void wasm_file_set_length(wasm_file_t *file, int64_t new_length, wasm_file_set_length_cont func, void *data);
void wasm_file_get_position(wasm_file_t *file, wasm_file_get_position_cont func, void *data);
void wasm_file_set_position(wasm_file_t *file, int64_t new_position, wasm_file_set_position_cont func, void *data);
void wasm_file_sync(wasm_file_t *file, wasm_file_sync_cont func, void *data);
void wasm_file_lock(wasm_file_t *file, int exclusive, int timeout, wasm_file_lock_cont func, void *data);
void wasm_file_unlock(wasm_file_t *file, wasm_file_unlock_cont func, void *data);

int wasm_file_open_sync(const char *path, int flags, wasm_file_t **file_out, int *error_out);
int wasm_file_close_sync(wasm_file_t *file, int *error_out);
int wasm_file_read_sync(wasm_file_t *file, void *buf, int len, int *read_bytes_out, int *error_out);
int wasm_file_write_sync(wasm_file_t *file, void *buf, int len, int *written_bytes_out, int *error_out);
int wasm_file_get_length_sync(wasm_file_t *file, int64_t *length_out, int *error_out);
int wasm_file_set_length_sync(wasm_file_t *file, int64_t new_length, int *error_out);
int wasm_file_get_position_sync(wasm_file_t *file, int64_t *position_out, int *error_out);
int wasm_file_set_position_sync(wasm_file_t *file, int64_t new_position, int *error_out);
int wasm_file_sync_sync(wasm_file_t *file, int *error_out);
int wasm_file_lock_sync(wasm_file_t *file, int exclusive, int timeout, int *locked_out, int *error_out);
int wasm_file_unlock_sync(wasm_file_t *file, int *error_out);

#endif /* WASM_SUPPORT_H */
