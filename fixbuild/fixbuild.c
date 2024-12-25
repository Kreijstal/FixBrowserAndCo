/*
 * FixBuild v0.6 - https://www.fixscript.org/
 * Copyright (c) 2020-2024 Martin Dvorak <jezek2@advel.cz>
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

// ZLIB/PNG & binary diff/patch code available at http://public-domain.advel.cz/ under CC0 license

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define UNICODE
#define _UNICODE
#include <windows.h>
#else
#ifdef __wasm
#include <wasm-support.h>
#else
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#endif
#include <unistd.h>
#endif
#include <errno.h>
#include "classes.h"
#include "build.h"
#include "scripts.h"
#include "binaries.h"

#define FIXEMBED_TOKEN_DUMP
#include "fixscript.c"

#include "fixio.h"
#include "fixtask.h"
#include "fiximage.h"
#include "fixnative.h"
#include "fixutil.h"

#include "icon.h"

#ifndef FIXBUILD_VERSION
#define FIXBUILD_VERSION 0.0-dev
#endif

#define STR2(x) #x
#define STR(x) STR2(x)

#define MAGIC1 0x093C1FBE
#define MAGIC2 0x8404665A

#if defined(_WIN32)
   #if defined(__i386__)
      #define DEFAULT_TARGET "windows32"
   #elif defined(__x86_64__)
      #define DEFAULT_TARGET "windows64"
   #endif
#elif defined(__APPLE__)
   #define DEFAULT_TARGET "macos"
#elif defined(__linux__)
   #if defined(__i386__)
      #define DEFAULT_TARGET "linux32"
   #elif defined(__x86_64__)
      #define DEFAULT_TARGET "linux64"
   #elif defined(__arm__)
      #define DEFAULT_TARGET "raspi32"
   #endif
#elif defined(__HAIKU__)
   #if defined(__i386__)
      #define DEFAULT_TARGET "haiku32"
   #elif defined(__x86_64__)
      #define DEFAULT_TARGET "haiku64"
   #endif
#elif defined(__wasm__)
   #define DEFAULT_TARGET "wasm"
#endif

#ifndef DEFAULT_TARGET
   #error "unknown system"
#endif

#ifdef __wasm__
#define PATH_MAX 256
#define SEEK_SET 1
#define SEEK_END 2
#define FILE wasm_file_t
#define fopen emul_fopen
#define fclose emul_fclose
#define fseek emul_fseek
#define ftell emul_ftell
#define fread emul_fread
#define fwrite emul_fwrite
#define ferror emul_ferror
#define feof emul_feof
#define printf(p...) fprintf(stderr, p)

static FILE *emul_fopen(const char *path, const char *mode)
{
   FILE *file;
   int err, flags = 0;

   if (strcmp(mode, "rb") == 0) {
      flags = WASM_FILE_READ;
   }
   else if (strcmp(mode, "wb") == 0) {
      flags = WASM_FILE_WRITE | WASM_FILE_CREATE | WASM_FILE_TRUNCATE;
   }
   else if (strcmp(mode, "w+b") == 0) {
      flags = WASM_FILE_READ | WASM_FILE_WRITE | WASM_FILE_CREATE | WASM_FILE_TRUNCATE;
   }
   else {
      fprintf(stderr, "internal error: unknown mode '%s'\n", mode);
      return NULL;
   }

   if (!wasm_file_open_sync(path, flags, &file, &err)) {
      return NULL;
   }
   if (err) {
      return NULL;
   }
   return file;
}

static int emul_fclose(FILE *file)
{
   int err;
   
   wasm_file_close_sync(file, &err);
   return err;
}

static int emul_fseek(FILE *file, int offset, int whence)
{
   int64_t pos;
   int err = 0;

   if (whence == SEEK_SET) {
      pos = offset;
   }
   else if (whence == SEEK_END) {
      if (!wasm_file_get_length_sync(file, &pos, &err)) {
         return WASM_ERROR_NOT_SUPPORTED;
      }
      if (err) {
         return err;
      }
      pos += offset;
   }
   else {
      return WASM_ERROR_NOT_SUPPORTED;
   }
   wasm_file_set_position_sync(file, pos, &err);
   return err;
}

static int emul_ftell(FILE *file)
{
   int64_t pos = 0;
   int err;

   if (!wasm_file_get_position_sync(file, &pos, &err)) {
      return WASM_ERROR_NOT_SUPPORTED;
   }
   if (err) {
      return err;
   }
   return (int)pos;
}

static int emul_fread(void *ptr, int size, int nmemb, FILE *file)
{
   int64_t remaining, processed = 0;
   int err=0, read_bytes;

   remaining = size * nmemb;
   while (remaining > 0) {
      if (!wasm_file_read_sync(file, ptr, remaining, &read_bytes, &err)) {
         return 0;
      }
      if (err) {
         return processed / size;
      }
      if (read_bytes == 0) break;
      ptr += read_bytes;
      remaining -= read_bytes;
      processed += read_bytes;
   }
   return processed / size;
}

static int emul_fwrite(const void *ptr, int size, int nmemb, FILE *file)
{
   int64_t remaining, processed = 0;
   int err=0, written_bytes;

   remaining = size * nmemb;
   while (remaining > 0) {
      if (!wasm_file_write_sync(file, (void *)ptr, remaining, &written_bytes, &err)) {
         return 0;
      }
      if (err) {
         return processed / size;
      }
      if (written_bytes == 0) break;
      ptr += written_bytes;
      remaining -= written_bytes;
      processed += written_bytes;
   }
   return processed / size;
}

static int emul_ferror(FILE *file)
{
   return 0;
}

static int emul_feof(FILE *file)
{
   return 1;
}

static void exit(int code)
{
   wasi_proc_exit(code);
}

static void perror(const char *msg)
{
   fprintf(stderr, "error: %s\n", msg);
}

#endif /* __wasm__ */

enum {
   GZIP_FTEXT    = 1 << 0,
   GZIP_FHCRC    = 1 << 1,
   GZIP_FEXTRA   = 1 << 2,
   GZIP_FNAME    = 1 << 3,
   GZIP_FCOMMENT = 1 << 4
};

typedef struct DirEntry {
   char *name;
   int dir;
   struct DirEntry *next;
} DirEntry;

typedef struct Exclude {
   const char *path;
   struct Exclude *next;
} Exclude;

typedef struct Icon {
   const char *fname;
   struct Icon *next;
} Icon;

typedef struct File {
   char *name;
   int off, len;
   int binary;
   int compressed;
   struct File *next;
} File;

Heap *heap;
int verbose = 0;
int use_raw_scripts = 0;
int use_compression = 1;
int total_uncompressed = 0;
int total_compressed = 0;
Exclude *excludes = NULL;
Icon *icons = NULL;
char *icon_pixels[6];
const char *sources_dir = ".";
const char *resources_dir = NULL;
File *out_files = NULL;
char *out_buf = NULL;
int out_cap = 0;
int out_len = 0;
#if defined(__wasm__)
const char *fixbuild_path = "<internal-fixbuild>";
#elif !defined(_WIN32)
char fixbuild_path[PATH_MAX];
#endif
DynArray extra_scripts = { NULL, 0, 0 };
int use_sqlite = 0;
int use_wasm_raw = 0;
#ifdef __wasm__
int wasm_exitcode = -1;
#endif

const char *fixup_script =
   "const {\n"
      "TOK_type,\n"
      "TOK_off,\n"
      "TOK_len,\n"
      "TOK_line,\n"
      "TOK_SIZE\n"
   "};\n"
   "\n"
   "function process_tokens(fname, tokens, src)\n"
   "{\n"
      "var idx = length(tokens) - TOK_SIZE;\n"
      "var lines = unserialize(token_parse_string(src, tokens[idx+TOK_off], tokens[idx+TOK_len]));\n"
      "array_set_length(tokens, idx);\n"
   "\n"
      "if (lines[length(lines)-1]*TOK_SIZE != idx) {\n"
         "return 0, error(\"token count mismatch (bug in fixembed)\");\n"
      "}\n"
   "\n"
      "var next_idx = lines[2] * TOK_SIZE;\n"
      "var adj = lines[1] - 32768;\n"
   "\n"
      "for (var i=lines[0]*TOK_SIZE,j=2,len=length(tokens); i<len; i+=TOK_SIZE) {\n"
         "if (i == next_idx) {\n"
            "adj += lines[++j] - 32768;\n"
            "next_idx = lines[++j] * TOK_SIZE;\n"
         "}\n"
         "tokens[i+TOK_line] += adj;\n"
      "}\n"
   "}\n";


int compress_script(const char *src, int src_len, char **dest_out, int *dest_len_out)
{
   #define RESERVE(size)                                              \
   {                                                                  \
      while (out_len+size > out_cap) {                                \
         if (out_cap >= (1<<29)) goto error;                          \
         out_cap <<= 1;                                               \
         new_out = realloc(out, out_cap);                             \
         if (!new_out) goto error;                                    \
         out = new_out;                                               \
      }                                                               \
   }

   #define PUT_BYTE(value)                                            \
   {                                                                  \
      out[out_len++] = (char)value;                                   \
   }

   #define PUT_BIG_VALUE(value)                                       \
   {                                                                  \
      int big_value = (value) - 15;                                   \
      while (big_value >= 255) {                                      \
         RESERVE(1);                                                  \
         PUT_BYTE(255);                                               \
         big_value -= 255;                                            \
      }                                                               \
      RESERVE(1);                                                     \
      PUT_BYTE(big_value);                                            \
   }

   #define PUT_LITERAL(idx)                                           \
   {                                                                  \
      int literal_value = (idx) - last_literal;                       \
      RESERVE(1);                                                     \
      PUT_BYTE((literal_value >= 15? 15 : literal_value) << 4);       \
      if (literal_value >= 15) {                                      \
         PUT_BIG_VALUE(literal_value);                                \
      }                                                               \
      RESERVE(literal_value);                                         \
      memcpy(out + out_len, src + last_literal, literal_value);       \
      out_len += literal_value;                                       \
   }

   #define PUT_MATCH(idx, dist, len)                                  \
   {                                                                  \
      int literal_value = (idx) - last_literal;                       \
      uint16_t dist_value = dist;                                     \
      int len_value = (len) - 4;                                      \
      RESERVE(1);                                                     \
      PUT_BYTE(                                                       \
         ((literal_value >= 15? 15 : literal_value) << 4) |           \
         (len_value >= 15? 15 : len_value)                            \
      );                                                              \
      if (literal_value >= 15) {                                      \
         PUT_BIG_VALUE(literal_value);                                \
      }                                                               \
      RESERVE(literal_value+2);                                       \
      memcpy(out + out_len, src + last_literal, literal_value);       \
      out_len += literal_value;                                       \
      memcpy(out + out_len, &dist_value, 2);                          \
      out_len += 2;                                                   \
      if (len_value >= 15) {                                          \
         PUT_BIG_VALUE(len_value);                                    \
      }                                                               \
   }

   #define SELECT_BUCKET(c1, c2, c3, c4)                              \
   {                                                                  \
      uint32_t idx = ((c1)<<24) | ((c2)<<16) | ((c3)<<8) | (c4);      \
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
      ((i) & ~65535) + (val) - ((val) >= ((i) & 65535)? 65536 : 0)    \
   )

   int num_buckets = 8192; // 8192*64*2 = 1MB
   int num_slots = 64;
   unsigned short *hash = NULL, *bucket;

   char *out = NULL, *new_out;
   int out_len, out_cap;

   int i, j, k, idx, dist, len;
   int best_len, best_dist=0, slot, worst_slot, worst_dist;
   int last_literal=0;

   hash = calloc(num_buckets * num_slots, sizeof(unsigned short));
   if (!hash) goto error;

   out_cap = 4096;
   out_len = 9;
   out = malloc(out_cap);
   if (!out) goto error;

   for (i=0; i<src_len-4; i++) {
      SELECT_BUCKET((uint8_t)src[i], (uint8_t)src[i+1], (uint8_t)src[i+2], (uint8_t)src[i+3]);
      best_len = 0;
      slot = -1;
      worst_slot = 0;
      worst_dist = 0;
      for (j=0; j<num_slots; j++) {
         idx = GET_INDEX(i, bucket[j]);
         if (idx >= 0 && idx+3 < i && i-idx < 65536 && src[i+0] == src[idx+0] && src[i+1] == src[idx+1] && src[i+2] == src[idx+2] && src[i+3] == src[idx+3]) {
            len = 4;
            for (k=4; k<(src_len-i) && k<512; k++) {
               if (src[i+k] != src[idx+k]) break;
               len++;
            }
            dist = i - idx;
            if (len > best_len) {
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
      bucket[slot] = i & 65535;

      if (best_len >= 4) {
         PUT_MATCH(i, best_dist, best_len);
         i += best_len-1;
         last_literal = i+1;
      }
   }

   if (last_literal < src_len) {
      PUT_LITERAL(src_len);
   }
   
   *dest_out = out;
   *dest_len_out = out_len;

   out_len -= 9;
   out[0] = 0xFF;
   memcpy(&out[1], &out_len, sizeof(int));
   memcpy(&out[5], &src_len, sizeof(int));
   free(hash);
   return 1;

error:
   free(out);
   free(hash);
   return 0;

   #undef RESERVE
   #undef PUT_BYTE
   #undef PUT_BIG_VALUE
   #undef PUT_LITERAL
   #undef PUT_MATCH
   #undef SELECT_BUCKET
   #undef GET_INDEX
}


static int ends_with(const char *s, const char *suffix)
{
   int len1 = strlen(s);
   int len2 = strlen(suffix);
   if (len1 < len2) return 0;
   return !strcmp(s + len1 - len2, suffix);
}


static void insert_dir_entry(DirEntry **entries, DirEntry *entry)
{
   DirEntry *e = *entries;

   while (e) {
      if (strcmp(entry->name, e->name) < 0) {
         break;
      }
      entries = &e->next;
      e = e->next;
   }

   entry->next = *entries;
   *entries = entry;
}


static void free_dir_entries(DirEntry *entries)
{
   DirEntry *next;
   
   while (entries) {
      next = entries->next;
      free(entries->name);
      free(entries);
      entries = next;
   }
}


#if defined(_WIN32)

static DirEntry *list_directory(const char *dirname)
{
   HANDLE handle;
   WIN32_FIND_DATAA data;
   DirEntry *entries = NULL, *entry;
   char *s;
   
   s = malloc(strlen(dirname)+2+1);
   strcpy(s, dirname);
   strcat(s, "/*");
   handle = FindFirstFileA(s, &data);
   free(s);
   if (handle == INVALID_HANDLE_VALUE) {
      errno = ENOENT;
      return NULL;
   }

   for (;;) {
      entry = calloc(1, sizeof(DirEntry));
      entry->name = strdup(data.cFileName);
      entry->dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
      insert_dir_entry(&entries, entry);
      
      if (!FindNextFileA(handle, &data)) break;
   }

   FindClose(handle);
   return entries;
}

#elif defined(__wasm)

static DirEntry *list_directory(const char *dirname)
{
   DirEntry *entry, *entries = NULL;
   char **files, *tmp;
   int i, err=0, num_files, type=0;
   
   if (!wasm_path_get_files_sync(dirname, &num_files, &files, &err)) {
      return NULL;
   }
   if (err) {
      return NULL;
   }

   for (i=0; i<num_files; i++) {
      if (strcmp(files[i], ".") == 0) continue;
      if (strcmp(files[i], "..") == 0) continue;
      tmp = malloc(strlen(dirname)+1+strlen(files[i])+1);
      strcpy(tmp, dirname);
      strcat(tmp, "/");
      strcat(tmp, files[i]);
      if (!wasm_path_get_info_sync(tmp, &type, NULL, NULL, &err)) {
         free(tmp);
         continue;
      }
      free(tmp);
      if (err) {
         continue;
      }
      if ((type & 0x7F) != WASM_FILE_TYPE_FILE && (type & 0x7F) != WASM_FILE_TYPE_DIRECTORY) {
         continue;
      }

      entry = calloc(1, sizeof(DirEntry));
      entry->name = files[i];
      entry->dir = (type & 0x7F) == WASM_FILE_TYPE_DIRECTORY;
      insert_dir_entry(&entries, entry);
      files[i] = NULL;
   }

   for (i=0; i<num_files; i++) {
      free(files[i]);
   }
   free(files);

   return entries;
}

#else

static DirEntry *list_directory(const char *dirname)
{
   DIR *dir;
   struct dirent *ent;
   struct stat buf;
   DirEntry *entry, *entries = NULL;
   char *tmp;

   dir = opendir(dirname);
   if (!dir) {
      return NULL;
   }

   while ((ent = readdir(dir))) {
      tmp = malloc(strlen(dirname)+1+strlen(ent->d_name)+1);
      strcpy(tmp, dirname);
      strcat(tmp, "/");
      strcat(tmp, ent->d_name);
      if (stat(tmp, &buf) != 0) {
         free(tmp);
         continue;
      }
      free(tmp);

      if (!S_ISREG(buf.st_mode) && !S_ISDIR(buf.st_mode)) {
         continue;
      }

      entry = calloc(1, sizeof(DirEntry));
      entry->name = strdup(ent->d_name);
      entry->dir = S_ISDIR(buf.st_mode);
      insert_dir_entry(&entries, entry);
   }

   if (closedir(dir) != 0) {
      free_dir_entries(entries);
      return NULL;
   }
   return entries;
}

#endif


int read_file(const char *fname, char **data_out, int *len_out)
{
   FILE *f = NULL;
   char *data = NULL, test;
   long len;

   f = fopen(fname, "rb");
   if (!f) {
      goto error;
   }

   if (fseek(f, 0, SEEK_END) != 0) goto error;
   len = ftell(f);
   if (fseek(f, 0, SEEK_SET) != 0) goto error;
   if (len < 0 || len > INT_MAX) {
      goto error;
   }

   data = malloc((int)len);
   if (!data) {
      goto error;
   }

   if (len > 0 && fread(data, (int)len, 1, f) != 1) {
      goto error;
   }

   if (fread(&test, 1, 1, f) != 0 || !feof(f)) {
      goto error;
   }

   if (fclose(f) != 0) {
      f = NULL;
      goto error;
   }

   *data_out = data;
   *len_out = (int)len;
   return 1;

error:
   free(data);
   if (f) fclose(f);
   return 0;
}


void put_file(const char *fname, const char *data, int len, int binary, int compressed)
{
   File **prev = &out_files, *file;
   char *new_buf;

   file = *prev;
   while (file) {
      prev = &file->next;
      file = file->next;
   }

   file = calloc(1, sizeof(File));
   file->name = strdup(fname);
   file->off = out_len;
   file->len = len;
   file->binary = binary;
   file->compressed = compressed;
   *prev = file;

   if (out_len + len > out_cap) {
      if (out_cap == 0) out_cap = 4096;
      while (out_len + len > out_cap) {
         out_cap <<= 1;
      }
      new_buf = realloc(out_buf, out_cap);
      if (!new_buf) {
         fprintf(stderr, "error: out of memory\n");
         exit(1);
      }
      out_buf = new_buf;
   }
   
   memcpy(out_buf + out_len, data, len);
   out_len += len;
}


static int has_file(const char *fname)
{
   File *file = out_files;

   while (file) {
      if (strcmp(file->name, fname) == 0) return 1;
      file = file->next;
   }
   return 0;
}


static int encode_int(unsigned char *ptr, uint32_t value)
{
   int cnt = 0;
   uint8_t b;

   do {
      b = value & 0x7F;
      value >>= 7;
      if (value != 0) b |= 0x80;
      if (ptr) {
         *ptr++ = b;
      }
      cnt++;
   }
   while (value != 0);

   return cnt;
}


static int decode_int(unsigned char **ptr, unsigned char *end, int *result, int *num_bytes)
{
   uint32_t value = 0;
   int shift = 0, cnt = 0;
   uint8_t b;

   while (*ptr < end) {
      b = *(*ptr)++;
      if (++cnt > 5) {
         return 0;
      }
      value |= (((uint32_t)b) & 0x7F) << shift;
      if ((b & 0x80) == 0) {
         *result = (int)value;
         if (num_bytes) {
            *num_bytes = cnt;
         }
         return 1;
      }
      shift += 7;
   }
   return 0;
}


typedef struct Patch {
   int off, len;
   unsigned char *replacement_data;
   int replacement_len;
   struct Patch *next;
} Patch;


static Patch *add_patch(Patch **patches, int off, int len, unsigned char *replacement_data, int replacement_len)
{
   Patch *new_patch, *patch, **prev;

   new_patch = malloc(sizeof(Patch));
   if (!new_patch) {
      free(replacement_data);
      return NULL;
   }
   new_patch->off = off;
   new_patch->len = len;
   new_patch->replacement_data = replacement_data;
   new_patch->replacement_len = replacement_len;

   prev = patches;
   patch = *patches;
   for (;;) {
      if (!patch || new_patch->off > patch->off) {
         *prev = new_patch;
         new_patch->next = patch;
         break;
      }
      if (!patch->next) {
         patch->next = new_patch;
         new_patch->next = NULL;
         break;
      }
      prev = &patch->next;
      patch = patch->next;
   }
   return new_patch;
}


static int set_patch_int(Patch *patch, int value)
{
   free(patch->replacement_data);
   patch->replacement_len = encode_int(NULL, value);
   patch->replacement_data = malloc(patch->replacement_len);
   if (!patch->replacement_data) {
      return 0;
   }
   
   encode_int(patch->replacement_data, value);
   return 1;
}


static Patch *add_patch_int(Patch **patches, int off, int len, int value)
{
   Patch *patch;

   patch = add_patch(patches, off, len, NULL, 0);
   if (!patch) {
      return NULL;
   }
   if (!set_patch_int(patch, value)) {
      return NULL;
   }
   return patch;
}


static void free_patches(Patch **patches)
{
   Patch *patch, *next;

   for (patch = *patches; patch; patch = next) {
      free(patch->replacement_data);
      next = patch->next;
      free(patch);
   }
   *patches = NULL;
}


static int apply_patches(Patch **patches, unsigned char *input, int input_len, unsigned char **result_out, int *result_len_out)
{
   Patch *patch;
   unsigned char *result;
   int result_len, adj_len, cur_len, move_len;

   result_len = input_len;
   for (patch = *patches; patch; patch = patch->next) {
      adj_len = patch->replacement_len - patch->len;
      if (adj_len > 0 && result_len > INT_MAX - adj_len) return 0;
      result_len += adj_len;
   }

   result = malloc(result_len);
   if (!result) return 0;

   cur_len = input_len;
   memcpy(result, input, input_len);

   for (patch = *patches; patch; patch = patch->next) {
      adj_len = patch->replacement_len - patch->len;
      if (adj_len > 0) {
         move_len = cur_len - (patch->off + adj_len);
         if (move_len > 0) {
            memmove(result + (patch->off + adj_len), result + patch->off, move_len);
         }
      }
      else if (adj_len < 0) {
         move_len = cur_len - (patch->off - adj_len);
         if (move_len > 0) {
            memmove(result + patch->off, result + (patch->off - adj_len), move_len);
         }
      }
      memcpy(result + patch->off, patch->replacement_data, patch->replacement_len);
      cur_len += adj_len;
   }

   if (cur_len != result_len) {
      free(result);
      return 0;
   }
   
   *result_out = result;
   *result_len_out = result_len;
   return 1;
}


static int wasm_embed_data(unsigned char *input, int input_len, unsigned char *embed_data, int embed_data_len, unsigned char **result_out, int *result_len_out)
{
   unsigned char *ptr = input;
   unsigned char *end = input + input_len;
   unsigned char *section_ptr, *section_end;
   unsigned char *new_data;
   Patch *patches = NULL;
   Patch *memory_section_len_patch = NULL;
   Patch *memory_section_value_patch = NULL;
   Patch *data_section_len_patch = NULL;
   Patch *data_section_value_patch = NULL;
   Patch *new_data_section_patch = NULL;
   int memory_section_size = 0;
   int data_section_size = 0;
   int memory_size = 0;
   int section_id, section_len, cnt, value, data_len, heap_start=0;
   int i, off, len, ret=0, new_data_len, used_memory_bytes;

   if (ptr >= end - 8) goto error;
   if (memcmp(ptr, "\0asm\1\0\0\0", 8) != 0) {
      goto error;
   }
   ptr += 8;

   while (ptr < end) {
      if (ptr >= end - 1) goto error;
      section_id = *ptr++;
      if (section_id > 11) goto error;
      off = ptr - input;
      if (!decode_int(&ptr, end, &section_len, &len)) goto error;
      if (section_id == 5) {
         memory_section_len_patch = add_patch(&patches, off, len, NULL, 0);
         if (!memory_section_len_patch) {
            goto error;
         }
         memory_section_size = section_len;
      }
      else if (section_id == 11) {
         data_section_len_patch = add_patch(&patches, off, len, NULL, 0);
         if (!data_section_len_patch) {
            goto error;
         }
         data_section_size = section_len;
      }
      if (ptr > end - section_len) goto error;
      if (section_id == 5) { // memory
         section_ptr = ptr;
         section_end = ptr + section_len;
         if (!decode_int(&section_ptr, section_end, &cnt, NULL)) goto error;
         if (cnt != 1) goto error;
         if (!decode_int(&section_ptr, section_end, &value, NULL)) goto error;
         if (value != 0) goto error;
         off = section_ptr - input;
         if (!decode_int(&section_ptr, section_end, &memory_size, &len)) goto error;
         memory_section_value_patch = add_patch(&patches, off, len, NULL, 0);
         if (!memory_section_value_patch) {
            goto error;
         }
      }
      if (section_id == 6) { // global
         section_ptr = ptr;
         section_end = ptr + section_len;
         if (!decode_int(&section_ptr, section_end, &cnt, NULL)) goto error;
         if (cnt < 1) goto error;
         if (section_ptr >= section_end - 3) goto error;
         value = *section_ptr++;
         if (value != 0x7F) goto error; // i32
         value = *section_ptr++;
         if (value != 1) goto error; // mut
         value = *section_ptr++;
         if (value != 0x41) goto error; // i32.const
         if (!decode_int(&section_ptr, section_end, &heap_start, NULL)) goto error;
         if (section_ptr != section_end - 1) goto error;
         value = *section_ptr++;
         if (value != 0x0B) goto error; // end
      }
      if (section_id == 11) { // data
         section_ptr = ptr;
         section_end = ptr + section_len;
         off = section_ptr - input;
         if (!decode_int(&section_ptr, section_end, &cnt, &len)) goto error;
         data_section_value_patch = add_patch_int(&patches, off, len, cnt+1);
         if (!data_section_value_patch) {
            goto error;
         }
         if (cnt < 1) goto error;
         for (i=0; i<cnt; i++) {
            if (!decode_int(&section_ptr, section_end, &value, NULL)) goto error; // memidx
            if (value != 0) goto error;
            if (section_ptr >= section_end - 1) goto error;
            value = *section_ptr++;
            if (value != 0x41) goto error; // i32.const
            if (!decode_int(&section_ptr, section_end, &value, NULL)) goto error;
            if (section_ptr >= section_end - 1) goto error;
            value = *section_ptr++;
            if (value != 0x0B) goto error; // end
            if (!decode_int(&section_ptr, section_end, &data_len, NULL)) goto error;
            if (section_ptr > section_end - data_len) goto error;
            section_ptr += data_len;
         }
         if (section_ptr != section_end) goto error;
      }
      ptr += section_len;
   }

   if (!memory_section_len_patch || !memory_section_value_patch || !data_section_len_patch || !data_section_value_patch) {
      goto error;
   }

   new_data_len = 2+encode_int(NULL, heap_start)+1+encode_int(NULL, 4+embed_data_len)+4;
   if (embed_data_len > INT_MAX - new_data_len) {
      goto error;
   }
   new_data_len += embed_data_len;
   new_data = malloc(new_data_len);
   if (!new_data) {
      goto error;
   }

   ptr = new_data;
   *ptr++ = 0; // memidx
   *ptr++ = 0x41; // i32.const
   ptr += encode_int(ptr, heap_start);
   *ptr++ = 0x0B; // end
   ptr += encode_int(ptr, 4+embed_data_len);
   value = embed_data_len;
   memcpy(ptr, &value, 4);
   ptr += 4;
   memcpy(ptr, embed_data, embed_data_len);
   ptr += embed_data_len;
   if (ptr - new_data != new_data_len) {
      goto error;
   }

   new_data_section_patch = add_patch(&patches, input_len, 0, new_data, new_data_len);
   if (!new_data_section_patch) {
      goto error;
   }
   if (!set_patch_int(data_section_len_patch, data_section_size + new_data_len)) {
      goto error;
   }

   used_memory_bytes = ((heap_start + 4+embed_data_len) + 0xFFFF) & ~0xFFFF;
   if (memory_size < (used_memory_bytes >> 16)) {
      memory_size = used_memory_bytes >> 16;
   }

   if (!set_patch_int(memory_section_value_patch, memory_size)) {
      goto error;
   }
   if (!set_patch_int(memory_section_len_patch, memory_section_size + (memory_section_value_patch->replacement_len - memory_section_value_patch->len))) {
      goto error;
   }

   ret = apply_patches(&patches, input, input_len, result_out, result_len_out);

error:
   free_patches(&patches);
   return ret;
}


static int base64_encode(unsigned char *input, int input_len, char **output_out, int *output_len_out)
{
   const char *table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
   uint32_t value;
   char *output;
   int i, output_len, pos=0, b0, b1;

   output_len = (input_len+2)/3*4;
   output = malloc(output_len+1);
   if (!output) {
      return 0;
   }

   for (i=0; i+2<input_len; i+=3) {
      value = (((int)input[i]) << 16) | (((int)input[i+1]) << 8) | ((int)input[i+2]);
      output[pos++] = table[(value >> 18) & 0x3F];
      output[pos++] = table[(value >> 12) & 0x3F];
      output[pos++] = table[(value >>  6) & 0x3F];
      output[pos++] = table[(value >>  0) & 0x3F];
   }

   switch (input_len - i) {
      case 1:
         b0 = input[i];
         output[pos++] = table[b0 >> 2];
         output[pos++] = table[(b0 << 4) & 0x3F];
         output[pos++] = '=';
         output[pos++] = '=';
         break;

      case 2:
         b0 = input[i];
         b1 = input[i+1];
         output[pos++] = table[b0 >> 2];
         output[pos++] = table[((b0 << 4) | (b1 >> 4)) & 0x3F];
         output[pos++] = table[(b1 << 2) & 0x3F];
         output[pos++] = '=';
         break;
   }

   output[pos] = 0;

   *output_out = output;
   *output_len_out = output_len;
   return 1;
}


static void write_data(FILE *f, const void *data, int len)
{
   if (len > 0 && fwrite(data, len, 1, f) != 1) {
      fprintf(stderr, "error writing to output file\n");
      exit(1);
   }
}


static void write_string(FILE *f, const char *s)
{
   write_data(f, s, strlen(s));
}


typedef struct {
   char *data;
   int cap, len;
} Buffer;

static void append_data(Buffer *buf, const void *data, int len)
{
   char *new_data;
   int new_cap;
   
   while (buf->len + len > buf->cap) {
      new_cap = buf->cap == 0? 0x10000 : buf->cap*2;
      new_data = realloc(buf->data, new_cap);
      if (!new_data) {
         fprintf(stderr, "error writing to output file\n");
         exit(1);
      }
      buf->data = new_data;
      buf->cap = new_cap;
   }

   memcpy(buf->data + buf->len, data, len);
   buf->len += len;
}


void write_files(FILE *f, const char *exec, int exec_len, const char *main_script, const char *target, int use_gui)
{
   Buffer buf;
   File *file = out_files;
   unsigned char flag, *patched_data;
   const char *orig_html;
   char *html, *encoded, *s;
   int off, base_off, magic, patched_len, html_len=0, encoded_len=0;
   
   memset(&buf, 0, sizeof(Buffer));

   if (strcmp(target, "wasm") == 0) {
      base_off = 0;
   }
   else {
      base_off = exec_len;
   }

   append_data(&buf, out_buf, out_len);
   append_data(&buf, main_script, strlen(main_script)+1);

   while (file) {
      append_data(&buf, file->name, strlen(file->name)+1);
      off = base_off + file->off;
      append_data(&buf, &off, sizeof(int));
      append_data(&buf, &file->len, sizeof(int));
      flag = 0;
      if (file->binary) flag |= 1 << 0;
      if (file->compressed) flag |= 1 << 1;
      append_data(&buf, &flag, 1);
      file = file->next;
   }

   magic = MAGIC1;
   append_data(&buf, &magic, sizeof(int));
   magic = MAGIC2;
   append_data(&buf, &magic, sizeof(int));

   off = base_off + out_len;
   append_data(&buf, &off, sizeof(int));

   if (strcmp(target, "wasm") == 0) {
      if (!wasm_embed_data((unsigned char *)exec, exec_len, (unsigned char *)buf.data, buf.len, &patched_data, &patched_len)) {
         fprintf(stderr, "error embedding of WASM data\n");
         exit(1);
      }
      if (use_gui && !use_wasm_raw) {
         orig_html = exec_binaries_get("wasm.html", &html_len);
         if (!orig_html) {
            fprintf(stderr, "internal error: WASM HTML template is not present\n");
            exit(1);
         }
         if (!base64_encode(patched_data, patched_len, &encoded, &encoded_len)) {
            fprintf(stderr, "out of memory\n");
            exit(1);
         }
         html = malloc(html_len+encoded_len+1);
         if (!html) {
            fprintf(stderr, "out of memory\n");
            exit(1);
         }
         memcpy(html, orig_html, html_len);
         html[html_len] = 0;
         s = strstr(html, "@@WASM_DATA@@");
         if (!s) {
            fprintf(stderr, "internal error: invalid WASM HTML template\n");
            exit(1);
         }
         memmove(s+encoded_len, s+13, html_len-(int)(s-html+13)+1);
         memcpy(s, encoded, encoded_len);
         write_data(f, html, strlen(html));
         free(html);
         free(encoded);
      }
      else {
         write_data(f, patched_data, patched_len);
      }
      free(patched_data);
   }
   else {
      write_data(f, exec, exec_len);
      write_data(f, buf.data, buf.len);
   }

   free(buf.data);
}


void embed_file(const char *fname, const char *script_name, int binary_mode)
{
   char *src, *compressed;
   int len, compressed_len;

   if (verbose) {
      fprintf(stderr, "processing %s...", script_name);
      fflush(stderr);
   }

   if (!read_file(fname, &src, &len)) {
      perror("can't read file");
      exit(1);
   }

   if (binary_mode) {
      put_file(script_name, src, len, binary_mode, 0);
      if (verbose) {
         fprintf(stderr, "\rprocessing %s   \n", script_name);
         fflush(stderr);
      }
   }
   else if (use_compression) {
      if (!compress_script(src, len, &compressed, &compressed_len)) abort();
      put_file(script_name, compressed, compressed_len, binary_mode, 1);
      if (verbose) {
         fprintf(stderr, "\rprocessing %s (compressed %d bytes to %d, %.2fx)\n", script_name, len, compressed_len, (double)len/compressed_len);
         fflush(stderr);
      }
      total_uncompressed += len;
      total_compressed += compressed_len;
      free(compressed);
   }
   else {
      put_file(script_name, src, len, binary_mode, 0);
      if (verbose) {
         fprintf(stderr, "\rprocessing %s   \n", script_name);
         fflush(stderr);
      }
   }

   free(src);
}


int symbols_require_whitespace(const char *s1, const char *s2)
{
   int len = strlen(s1);
   if (len == 1) {
      switch (s1[0]) {
         case '+': 
            if (strcmp(s2, "+") == 0) return 1;
            if (strcmp(s2, "=") == 0) return 1;
            if (strcmp(s2, "+=") == 0) return 1;
            if (strcmp(s2, "++") == 0) return 1;
            if (strcmp(s2, "==") == 0) return 1;
            if (strcmp(s2, "===") == 0) return 1;
            break;
         case '-': 
            if (strcmp(s2, "-") == 0) return 1;
            if (strcmp(s2, "=") == 0) return 1;
            if (strcmp(s2, ">") == 0) return 1;
            if (strcmp(s2, "-=") == 0) return 1;
            if (strcmp(s2, "--") == 0) return 1;
            if (strcmp(s2, "->") == 0) return 1;
            if (strcmp(s2, ">=") == 0) return 1;
            if (strcmp(s2, ">>") == 0) return 1;
            if (strcmp(s2, "==") == 0) return 1;
            if (strcmp(s2, "===") == 0) return 1;
            if (strcmp(s2, ">>=") == 0) return 1;
            if (strcmp(s2, ">>>") == 0) return 1;
            if (strcmp(s2, ">>>=") == 0) return 1;
            break;
         case '*':
         case '/':
         case '%':
         case '^':
         case '=':
         case '!':
            if (strcmp(s2, "=") == 0) return 1;
            if (strcmp(s2, "==") == 0) return 1;
            if (strcmp(s2, "===") == 0) return 1;
            break;
         case '&':
            if (strcmp(s2, "&") == 0) return 1;
            if (strcmp(s2, "=") == 0) return 1;
            if (strcmp(s2, "&=") == 0) return 1;
            if (strcmp(s2, "&&") == 0) return 1;
            if (strcmp(s2, "==") == 0) return 1;
            if (strcmp(s2, "===") == 0) return 1;
            break;
         case '|':
            if (strcmp(s2, "|") == 0) return 1;
            if (strcmp(s2, "=") == 0) return 1;
            if (strcmp(s2, "|=") == 0) return 1;
            if (strcmp(s2, "||") == 0) return 1;
            if (strcmp(s2, "==") == 0) return 1;
            if (strcmp(s2, "===") == 0) return 1;
            break;
         case '<':
            if (strcmp(s2, "=") == 0) return 1;
            if (strcmp(s2, "<") == 0) return 1;
            if (strcmp(s2, "<=") == 0) return 1;
            if (strcmp(s2, "<<") == 0) return 1;
            if (strcmp(s2, "==") == 0) return 1;
            if (strcmp(s2, "===") == 0) return 1;
            if (strcmp(s2, "<<=") == 0) return 1;
            break;
         case '>':
            if (strcmp(s2, "=") == 0) return 1;
            if (strcmp(s2, ">") == 0) return 1;
            if (strcmp(s2, ">=") == 0) return 1;
            if (strcmp(s2, ">>") == 0) return 1;
            if (strcmp(s2, "==") == 0) return 1;
            if (strcmp(s2, "===") == 0) return 1;
            if (strcmp(s2, ">>=") == 0) return 1;
            if (strcmp(s2, ">>>") == 0) return 1;
            if (strcmp(s2, ">>>=") == 0) return 1;
            break;
         case '.':
            if (strcmp(s2, ".") == 0) return 1;
            if (strcmp(s2, "..") == 0) return 1;
            break;
      }
   }
   else if (len == 2) {
      if (strcmp(s1, "<<") == 0 || strcmp(s1, "==") == 0 || strcmp(s1, "!=") == 0 || strcmp(s1, ">>>") == 0) {
         if (strcmp(s2, "=") == 0) return 1;
         if (strcmp(s2, "==") == 0) return 1;
         if (strcmp(s2, "===") == 0) return 1;
      }
      else if (strcmp(s1, ">>") == 0) {
         if (strcmp(s2, "=") == 0) return 1;
         if (strcmp(s2, ">") == 0) return 1;
         if (strcmp(s2, ">=") == 0) return 1;
         if (strcmp(s2, ">>") == 0) return 1;
         if (strcmp(s2, "==") == 0) return 1;
         if (strcmp(s2, "===") == 0) return 1;
         if (strcmp(s2, ">>=") == 0) return 1;
         if (strcmp(s2, ">>>") == 0) return 1;
         if (strcmp(s2, ">>>=") == 0) return 1;
      }
   }
   else if (len == 3) {
      if (strcmp(s1, ">>>") == 0) {
         if (strcmp(s2, "=") == 0) return 1;
         if (strcmp(s2, "==") == 0) return 1;
         if (strcmp(s2, "===") == 0) return 1;
      }
   }
   return 0;
}


int is_path_excluded(const char *path)
{
   Exclude *exc;

   for (exc = excludes; exc; exc = exc->next) {
      if (strcmp(exc->path, path) == 0) return 1;
   }
   return 0;
}


int is_path_excluded_full(const char *path)
{
   char *s = strdup(path), *c;
   for (;;) {
      if (is_path_excluded(s)) return 1;
      c = strrchr(s, '/');
      if (!c) break;
      *c = '\0';
   }
   free(s);
   return 0;
}


Script *load_script_file(Heap *heap, const char *name, Value *error, const char *dirname)
{
   FILE *f;
   char *src = NULL;
   int src_size = 0;
   int buf_size = 4096;
   char *buf = NULL, *tmp;
   int read;
   Script *script = NULL;
   char *sname = NULL, *fname = NULL;

   if (!is_valid_path(name)) {
      if (error) {
         tmp = string_format("invalid script file name %s given", name);
         *error = fixscript_create_string(heap->token_heap? heap->token_heap : heap, tmp, -1);
         free(tmp);
      }
      return NULL;
   }

   sname = string_format("%s.fix", name);
   script = fixscript_get(heap, sname);
   if (script) {
      free(sname);
      return script;
   }

   fname = string_format("%s/%s.fix", dirname, name);
   f = fopen(fname, "rb");
   if (f) {
      buf = malloc(buf_size);

      while ((read = fread(buf, 1, buf_size, f)) > 0) {
         src = realloc(src, src_size + read + 1);
         memcpy(src + src_size, buf, read);
         src_size += read;
      }

      if (ferror(f)) {
         fclose(f);
         if (error) {
            tmp = string_format("reading of script %s failed", name);
            *error = fixscript_create_string(heap->token_heap? heap->token_heap : heap, tmp, -1);
            free(tmp);
         }
         goto error;
      }
      fclose(f);
   }
   else {
      if (strcmp(name, "classes") == 0) {
         const char * const *p = classes_script;
         while (*p) {
            if (strcmp(*p, "classes.fix") == 0) break;
            p += 2;
         }
         buf = (char *)*(++p);
         buf_size = strlen(buf);
      }
      else {
         buf = (char *)common_scripts_get(sname, &buf_size);
      }
      if (!buf) {
         if (error) {
            tmp = string_format("script %s not found", name);
            *error = fixscript_create_string(heap->token_heap? heap->token_heap : heap, tmp, -1);
            free(tmp);
         }
         goto error;
      }
      src_size = buf_size;
      src = malloc(src_size+1);
      memcpy(src, buf, src_size);
      buf = NULL;
   }

   if (src) {
      src[src_size] = 0;
   }
   else {
      src = strdup("");
   }
   
   script = fixscript_load(heap, src, sname, error, (LoadScriptFunc)load_script_file, (void *)dirname);

error:
   free(sname);
   free(fname);
   free(buf);
   free(src);
   return script;
}


void fixembed_native_function_used(const char *name)
{
   if (strncmp(name, "sqlite_", 7) == 0) {
      use_sqlite = 1;
   }
}


void fixembed_dump_tokens(const char *fname, Tokenizer *tok)
{
   Tokenizer tok_sav = *tok;
   String str;
   Value line_adjusts;
   int i, c, new_line=1, last_line=1, suppress, prev_type=-1, cur_type, num_tokens=1, len, compressed_len;
   const char *prefix;
   char prev_symbol[5], symbol[5], *ser, *script_code, *compressed;
   int state=0;

   if (is_path_excluded_full(fname)) {
      fprintf(stderr, "error: script %s excluded but it's required by other script\n", fname);
      fflush(stderr);
      exit(1);
   }

   if (verbose) {
      fprintf(stderr, "processing %s...", fname);
      fflush(stderr);
   }

   memset(&str, 0, sizeof(String));
   line_adjusts = fixscript_create_array(heap, 0);
   if (!line_adjusts.value) abort();
   fixscript_ref(heap, line_adjusts);
   prev_symbol[0] = '\0';
   symbol[0] = '\0';

   prefix = "use \"__fixlines\";";
   if (!string_append(&str, prefix)) abort();

   while (next_token(tok)) {
      if (tok->line != last_line) {
         if (tok->line > last_line && tok->line - last_line < 100) {
            while (tok->line > last_line+1) {
               if (!string_append(&str, "\n")) abort();
               last_line++;
            }
         }
         if (!string_append(&str, "\n")) abort();
         new_line = 1;
         if (tok->line != last_line+1) {
            if (fixscript_append_array_elem(heap, line_adjusts, fixscript_int(num_tokens)) != 0) abort();
            if (fixscript_append_array_elem(heap, line_adjusts, fixscript_int(32768+tok->line - last_line - 1)) != 0) abort();
         }
         last_line = tok->line;
      }

      suppress = 0;
      if (new_line) {
         suppress = 1;
         new_line = 0;
         prev_type = -1;
      }

      cur_type = tok->type;
      if (cur_type > TOK_UNKNOWN && cur_type < ' ') cur_type = TOK_IDENT;

      if (cur_type >= ' ') {
         snprintf(symbol, sizeof(symbol), "%.*s", tok->len, tok->value);
      }

      if (cur_type >= ' ' && prev_type < ' ') {
         if (prev_type != TOK_NUMBER || tok->len != 2 || strncmp(tok->value, "..", 2) != 0) {
            suppress = 1;
         }
      }

      if (cur_type < ' ' && prev_type >= ' ') {
         suppress = 1;
      }

      if (cur_type >= ' ' && prev_type >= ' ' && !symbols_require_whitespace(prev_symbol, symbol)) {
         suppress = 1;
      }

      prev_type = cur_type;
      strcpy(prev_symbol, symbol);
      if (!suppress) {
         if (!string_append(&str, " ")) abort();
      }

      if (!string_append(&str, "%.*s", tok->len, tok->value)) abort();
      num_tokens++;

      switch (state) {
         case 0:
            if (tok->type == TOK_IDENT) {
               if (tok->len == 11 && memcmp(tok->value, "task_create", 11) == 0) {
                  state = 1;
               }
               else if (tok->len == 13 && memcmp(tok->value, "worker_create", 13) == 0) {
                  state = 1;
               }
               else if (tok->len == 16 && memcmp(tok->value, "heap_load_script", 16) == 0) {
                  state = 1;
               }
            }
            break;

         case 1:
            if (tok->type == '(') {
               state = 2;
            }
            else {
               state = 0;
            }
            break;

         case 2:
            if (tok->type == TOK_STRING) {
               if (dynarray_add(&extra_scripts, get_token_string(tok)) != 0) {
                  fprintf(stderr, "error: out of memory\n");
                  exit(1);
               }
            }
            state = 0;
            break;
      }
   }

   fixscript_get_array_length(heap, line_adjusts, &len);
   if (len > 0) {
      if (fixscript_append_array_elem(heap, line_adjusts, fixscript_int(num_tokens)) != 0) abort();
      if (!string_append(&str, "\n\"")) abort();

      if (fixscript_serialize_to_array(heap, &ser, &len, line_adjusts) != 0) abort();
      for (i=0; i<len; i++) {
         c = (unsigned char)ser[i];
         if (c == '\\') {
            if (!string_append(&str, "\\\\")) abort();
         }
         else if (c == '\"') {
            if (!string_append(&str, "\\\"")) abort();
         }
         else if (c == '\t') {
            if (!string_append(&str, "\\t")) abort();
         }
         else if (c == '\r') {
            if (!string_append(&str, "\\r")) abort();
         }
         else if (c == '\n') {
            if (!string_append(&str, "\\n")) abort();
         }
         else if (c >= 32 && c < 127) {
            if (!string_append(&str, "%c", c)) abort();
         }
         else {
            #ifdef __wasm__
               if (!string_append(&str, "\\%c%c", "0123456789abcdef"[c >> 4], "0123456789abcdef"[c & 15])) abort();
            #else
               if (!string_append(&str, "\\%02x", c)) abort();
            #endif
         }
      }

      if (!string_append(&str, "\"")) abort();
      free(ser);

      script_code = str.data;
   }
   else {
      script_code = str.data + strlen(prefix);
   }

   if (use_compression) {
      if (!compress_script(script_code, strlen(script_code), &compressed, &compressed_len)) abort();
      put_file(fname, compressed, compressed_len, 0, 1);
      if (verbose) {
         fprintf(stderr, "\rprocessing %s (compressed %d bytes to %d, %.2fx)\n", fname, (int)strlen(script_code), compressed_len, (double)strlen(script_code)/compressed_len);
         fflush(stderr);
      }
      total_uncompressed += strlen(script_code);
      total_compressed += compressed_len;
      free(compressed);
   }
   else {
      put_file(fname, script_code, strlen(script_code), 0, 0);
      if (verbose) {
         fprintf(stderr, "\rprocessing %s   \n", fname);
         fflush(stderr);
      }
   }

   free(str.data);
   fixscript_unref(heap, line_adjusts);
   *tok = tok_sav;
}


void traverse_dir(const char *dirname, const char *orig_dirname, int binary_mode)
{
   DirEntry *entries, *e;
   Value error;
   char tmp[256], tmp2[256], *prefix;
   
   entries = list_directory(dirname);
   if (!entries) {
      perror("scandir");
      exit(1);
   }

   prefix = strchr(dirname, '/');
   if (prefix) {
      prefix++;
   }
   else {
      prefix = "";
   }

   if (is_path_excluded(prefix)) {
      return;
   }
   if (!binary_mode && resources_dir && strcmp(prefix, resources_dir) == 0) {
      return;
   }

   for (e=entries; e; e=e->next) {
      if (e->name[0] == '.') continue;
      if (!binary_mode) {
         if (!e->dir && !ends_with(e->name, ".fix")) {
            continue;
         }
      }

      if (binary_mode && !e->dir) {
         if (snprintf(tmp, sizeof(tmp), "%s/%s", dirname, e->name) >= sizeof(tmp)) {
            fprintf(stderr, "path too long");
            exit(1);
         }
         snprintf(tmp2, sizeof(tmp2), "%s%s%s", prefix, prefix[0]? "/" : "", e->name);
         if (is_path_excluded(tmp2)) {
            continue;
         }
         embed_file(tmp, tmp2, binary_mode);
      }
      else if (!e->dir && ends_with(e->name, ".fix") && !binary_mode) {
         if (snprintf(tmp, sizeof(tmp), "%s/%s", dirname, e->name) >= sizeof(tmp)) {
            fprintf(stderr, "path too long");
            exit(1);
         }
         snprintf(tmp2, sizeof(tmp2), "%s%s%s", prefix, prefix[0]? "/" : "", e->name);
         if (is_path_excluded(tmp2)) {
            continue;
         }
         if (use_raw_scripts) {
            embed_file(tmp, tmp2, binary_mode);
         }
         else {
            *strrchr(tmp2, '.') = '\0';
            if (!load_script_file(heap, tmp2, &error, orig_dirname)) {
               fprintf(stderr, "%s\n", fixscript_get_compiler_error(heap->token_heap, error));
               exit(1);
            }
         }
      }
      else if (e->dir) {
         if (snprintf(tmp, sizeof(tmp), "%s/%s", dirname, e->name) >= sizeof(tmp)) {
            fprintf(stderr, "path too long");
            exit(1);
         }
         traverse_dir(tmp, orig_dirname, binary_mode);
      }
   }

   free_dir_entries(entries);
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

int zuncompress_memory(const unsigned char *src, int src_len, unsigned char **dest_out, int *dest_len_out)
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


int gzip_uncompress_memory(const unsigned char *src, int src_len, unsigned char **dest_out, int *dest_len_out)
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


static int apply_patch(unsigned char *src, int src_len, unsigned char *patch, int patch_len, unsigned char **result_out, int *result_len_out)
{
   unsigned char *result = NULL;
   int i, off, cnt, result_len, result_len_header, dest_start = 0, src_start, len;

   if (patch_len < 8) {
      goto error;
   }
   
   memcpy(&result_len_header, patch + 0, 4);
   memcpy(&cnt, patch + 4, 4);
   if (cnt < 0 || cnt > (patch_len-8)/(2*4)) {
      goto error;
   }
   result_len = patch_len - 8 - cnt*(2*4);
   if (result_len < 0 || result_len != result_len_header) {
      goto error;
   }

   result = malloc(result_len);
   if (!result) {
      goto error;
   }

   off = 8;
   for (i=0; i<cnt; i++) {
      memcpy(&src_start, patch + off, 4); off += 4;
      memcpy(&len, patch + off, 4); off += 4;

      if (src_start < 0 || len <= 0 || len > src_len) {
         goto error;
      }
      if (dest_start > result_len - len) {
         goto error;
      }
      if (src_start > src_len - len) {
         goto error;
      }

      memcpy(result + dest_start, src + src_start, len);
      dest_start += len;
   }

   if (cnt == 0) {
      memcpy(result, patch + off, result_len);
   }
   else {
      if (dest_start != result_len) {
         goto error;
      }
      for (i=0; i<result_len; i++) {
         result[i] ^= patch[off+i];
      }
   }

   *result_out = result;
   *result_len_out = result_len;
   return 1;

error:
   free(result);
   return 0;
}


#ifndef __wasm__
static char *find_parent_file(const char *fname)
{
   FILE *f;
   char buf[256];
   int prefix_len;

   strcpy(buf, "");
   prefix_len = 0;

   for (;;) {
      if (snprintf(buf+prefix_len, sizeof(buf)-prefix_len, "%s", fname) >= (sizeof(buf)-prefix_len)-1) {
         return NULL;
      }
      f = fopen(buf, "rb");
      if (f) {
         fclose(f);
         buf[prefix_len] = 0;
         return strdup(buf);
      }
      if (prefix_len+4 > sizeof(buf)) {
         return NULL;
      }
      strcpy(buf+prefix_len, "../");
      prefix_len += 3;
   }
}
#endif


static Script *load_build_script(Heap *heap, const char *fname, Value *error, void *data)
{
   Script *script;
   char buf[256], *src, *tmp, *real_fname, *dirname = data;
   int len, ret;

   if (!dirname && strcmp(fname, "build") == 0) {
      fname = "build:build";
   }

   snprintf(buf, sizeof(buf), "%s.fix", fname);
   script = fixscript_get(heap, buf);
   if (script) {
      return script;
   }

   if (dirname) {
      real_fname = string_format("%s%s.fix", dirname, fname);
      ret = read_file(real_fname, &src, &len);
      free(real_fname);
      if (ret) {
         tmp = malloc(len+1);
         memcpy(tmp, src, len);
         tmp[len] = 0;
         script = fixscript_load(heap, tmp, buf, error, load_build_script, dirname);
         free(tmp);
         return script;
      }
   }

   if (strcmp(fname, "classes") == 0 || strcmp(fname, "__fixlines") == 0) {
      const char * const *p = classes_script;
      while (*p) {
         if (strcmp(*p, buf) == 0) break;
         p += 2;
      }
      src = (char *)*(++p);
      len = strlen(src);
   }
   else if (strncmp(fname, "build:", 6) == 0) {
      const char * const *p = build_scripts;
      src = NULL;
      while (*p) {
         if (strcmp(*p, buf+6) == 0) {
            src = (char *)*(++p);
            len = strlen(src);
            break;
         }
         p += 2;
      }
      if (!src) {
         snprintf(buf, sizeof(buf), "script %s not found", fname);
         *error = fixscript_create_string(heap, buf, -1);
         return NULL;
      }
   }
   else {
      tmp = strstr(fname, "/:");
      if (tmp) {
         if (strchr(tmp+2, ':')) {
            snprintf(buf, sizeof(buf), "script %s not found", fname);
            *error = fixscript_create_string(heap, buf, -1);
            return NULL;
         }
         dirname = strdup(fname);
         dirname[tmp - fname + 1] = 0;
         script = load_build_script(heap, tmp+2, error, dirname);
         free(dirname);
         return script;
      }
      else {
         src = (char *)common_scripts_get(buf, &len);
      }
   }

   if (src) {
      tmp = malloc(len+1);
      memcpy(tmp, src, len);
      tmp[len] = 0;
      script = fixscript_load(heap, tmp, buf, error, load_build_script, NULL);
      free(tmp);
      return script;
   }

   snprintf(buf, sizeof(buf), "script %s not found", fname);
   *error = fixscript_create_string(heap, buf, -1);
   return NULL;
}


static Value get_fixbuild_version(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value ret;

   ret = fixscript_create_string(heap, STR(FIXBUILD_VERSION), -1);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return ret;
}


static Value get_fixbuild_path(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#ifdef _WIN32
   Value ret;
   DWORD dwret;
   uint16_t filename[256];

   dwret = GetModuleFileName(NULL, filename, 255);
   if (dwret == 0 || dwret >= 254) {
      *error = fixscript_create_error_string(heap, "can't get fixbuild path");
      return fixscript_int(0);
   }
   ret = fixscript_create_string_utf16(heap, filename, -1);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return ret;
#else
   Value ret;

   ret = fixscript_create_string(heap, fixbuild_path, -1);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return ret;
#endif
}


static Value get_fixbuild_targets(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value ret, values[9];
   int i, err;

   ret = fixscript_create_array(heap, sizeof(values)/sizeof(Value));
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   values[0] = fixscript_create_string(heap, "windows32", -1);
   values[1] = fixscript_create_string(heap, "windows64", -1);
   values[2] = fixscript_create_string(heap, "linux32", -1);
   values[3] = fixscript_create_string(heap, "linux64", -1);
   values[4] = fixscript_create_string(heap, "macos", -1);
   values[5] = fixscript_create_string(heap, "haiku32", -1);
   values[6] = fixscript_create_string(heap, "haiku64", -1);
   values[7] = fixscript_create_string(heap, "raspi32", -1);
   values[8] = fixscript_create_string(heap, "wasm", -1);

   for (i=0; i<sizeof(values)/sizeof(Value); i++) {
      if (!values[i].value) {
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
   }

   err = fixscript_set_array_range(heap, ret, 0, sizeof(values)/sizeof(Value), values);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return ret;
}


static Value get_fixbuild_current_target(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value ret;

   ret = fixscript_create_string(heap, DEFAULT_TARGET, -1);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return ret;
}


static Value native_exit(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   int value = 0;
#ifdef __wasm__
   ContinuationResultFunc cont_func;
   void *cont_data;
#endif

   if (num_params == 1) {
      value = params[0].value;
   }
#ifdef __wasm__
   wasm_exitcode = value & 0xFF;
   fixscript_suspend(heap, &cont_func, &cont_data);
   wasi_exit_loop();
#else
   exit(value);
#endif
   return fixscript_int(0);
}


#ifdef __wasm__

int main(int argc, char **argv);

static Value emul_process_create(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value value;
   char *str;
   char **args = NULL;
   int i, err, num_args = 0;

   if (params[1].value != 0 || params[2].value != 0 || params[3].value != 0) {
      goto not_supported;
   }

   err = fixscript_get_array_length(heap, params[0], &num_args);
   if (!err) {
      if (num_args < 1) {
         goto not_supported;
      }
      err = fixscript_get_array_elem(heap, params[0], 0, &value);
   }
   if (!err) {
      err = fixscript_get_string(heap, value, 0, -1, &str, NULL);
   }
   if (!err) {
      if (strcmp(str, fixbuild_path) != 0) {
         free(str);
         goto not_supported;
      }
      free(str);
      args = calloc(num_args, sizeof(char *));
      if (!args) {
         err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
   }
   if (!err) {
      for (i=0; i<num_args; i++) {
         err = fixscript_get_array_elem(heap, params[0], i, &value);
         if (!err) {
            err = fixscript_get_string(heap, value, 0, -1, &str, NULL);
         }
         if (err) {
            break;
         }
         args[i] = str;
      }
   }
   if (err) {
      if (args) {
         for (i=0; i<num_args; i++) {
            free(args[i]);
         }
         free(args);
      }
      return fixscript_error(heap, error, err);
   }

   err = main(num_args, args);

   for (i=0; i<num_args; i++) {
      free(args[i]);
   }
   free(args);

   return fixscript_int(err);

not_supported:
   *error = fixscript_create_error_string(heap, "not supported");
   return fixscript_int(0);
}

static Value emul_process_wait(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   return params[0];
}

#endif /* __wasm__ */


static Heap *create_build_heap(void *data)
{
   Heap *heap;

   heap = fixscript_create_heap();
   fixio_register_functions(heap);
   fixtask_register_functions(heap, create_build_heap, NULL, load_build_script, NULL);
   fiximage_register_functions(heap);
   fixnative_register_functions(heap);
   fixutil_register_functions(heap);
   fixscript_register_native_func(heap, "get_fixbuild_version#0", get_fixbuild_version, NULL);
   fixscript_register_native_func(heap, "get_fixbuild_path#0", get_fixbuild_path, NULL);
   fixscript_register_native_func(heap, "get_fixbuild_targets#0", get_fixbuild_targets, NULL);
   fixscript_register_native_func(heap, "get_fixbuild_current_target#0", get_fixbuild_current_target, NULL);
   fixscript_register_native_func(heap, "exit#0", native_exit, NULL);
   fixscript_register_native_func(heap, "exit#1", native_exit, NULL);
   #ifdef __wasm__
      fixscript_register_native_func(heap, "process_create#4", emul_process_create, NULL);
      fixscript_register_native_func(heap, "process_wait#1", emul_process_wait, NULL);
      wasm_auto_suspend_heap(heap);
   #endif
   return heap;
}


#if !defined(_WIN32) && !defined(__wasm__)
static char *search_file_on_path(const char *fname)
{
   FILE *f;
   char *path;
   char *s, *p;
   char buf[256];

   if (strchr(fname, '/')) {
      return strdup(fname);
   }

   path = strdup(getenv("PATH"));
   s = path;
   do {
      p = strchr(s, ':');
      if (p) {
         *p = 0;
      }
      snprintf(buf, sizeof(buf), "%s/%s", s, fname);
      f = fopen(buf, "rb");
      if (f) {
         fclose(f);
         free(path);
         return strdup(buf);
      }
      if (p) {
         s = p+1;
      }
   }
   while (p);

   free(path);
   return NULL;
}
#endif


#ifdef __wasm__
static void run_build_script_cont(Heap *heap, Value result, Value error, void *data)
{
   if (error.value) {
      fixscript_dump_value(heap, error, 1);
      wasm_exitcode = 1;
   }
   else {
      wasm_exitcode = result.value & 0xFF;
   }
   wasi_exit_loop();
}
#endif


static void run_build_script(int argc, char **argv)
{
   Heap *heap;
   Script *script;
   Value error, args, func;
#ifndef __wasm__
   Value ret;
#endif
   char *prefix, *tmp;
   char buf[256], *src;
   int i, len;

#ifdef __wasm__
   prefix = strdup("");
#else
   prefix = find_parent_file("build.fix");
   if (!prefix) return;
#endif

   snprintf(buf, sizeof(buf), "%sbuild.fix", prefix);
   if (!read_file(buf, &src, &len)) {
      free(prefix);
      return;
   }

#if defined(_WIN32)
   SetCurrentDirectoryA(prefix);
#elif defined(__wasm__)
   // nothing
#else
   tmp = search_file_on_path(argv[0]);
   if (!tmp || !realpath(tmp, fixbuild_path)) {
      fprintf(stderr, "error: can't get absolute path for fixbuild executable\n");
      fflush(stderr);
      exit(1);
   }
   free(tmp);
   chdir(prefix);
#endif
   free(prefix);

   heap = create_build_heap(NULL);

   tmp = malloc(len+1);
   memcpy(tmp, src, len);
   tmp[len] = 0;
   script = fixscript_load(heap, tmp, "build.fix", &error, load_build_script, NULL);
   free(tmp);
   free(src);
   if (!script) {
      fprintf(stderr, "%s\n", fixscript_get_compiler_error(heap, error));
      fflush(stderr);
      exit(1);
   }

   args = fixscript_create_array(heap, 0);
   for (i=1; i<argc; i++) {
      fixscript_append_array_elem(heap, args, fixscript_create_string(heap, argv[i], -1));
   }
   
   func = fixscript_get_function(heap, script, "main#2");
   if (func.value) {
      #ifdef __wasm__
         fixscript_call_async(heap, func, 2, (Value[]) { fixscript_create_string(heap, argv[0], -1), args }, run_build_script_cont, NULL);
      #else
         ret = fixscript_call(heap, func, 2, &error, fixscript_create_string(heap, argv[0], -1), args);
      #endif
   }
   else {
      func = fixscript_get_function(heap, script, "main#1");
      if (func.value) {
         #ifdef __wasm__
            fixscript_call_async(heap, func, 1, (Value[]) { args }, run_build_script_cont, NULL);
         #else
            ret = fixscript_call(heap, func, 1, &error, args);
         #endif
      }
      else {
         func = fixscript_get_function(heap, script, "main#0");
         if (func.value) {
            #ifdef __wasm__
               fixscript_call_async(heap, func, 0, NULL, run_build_script_cont, NULL);
            #else
               ret = fixscript_call(heap, func, 0, &error);
            #endif
         }
      }
   }
   
   if (!func.value) {
      fprintf(stderr, "error: can't find main function\n");
      fflush(stderr);
      exit(1);
   }

#ifdef __wasm__
   wasi_run_loop();
   exit(wasm_exitcode);
#else
   if (error.value) {
      fixscript_dump_value(heap, error, 1);
      exit(1);
   }
   exit(ret.value & 0xFF);
#endif
}


static char *escape_string(const char *str)
{
   char *dest, *p;
   int i, len, dest_len;

   len = strlen(str);

   dest_len = 2;
   for (i=0; i<len; i++) {
      switch (str[i]) {
         case '\r':
         case '\n':
         case '\t':
         case '\\':
         case '\'':
         case '\"':
            dest_len += 2;
            break;
            
         default:
            if (str[i] >= 0 && str[i] < 32) {
               dest_len += 3;
            }
            else {
               dest_len++;
            }
            break;
      }
   }

   dest = malloc(dest_len+1);
   dest[dest_len] = 0;
   p = dest;
   *p++ = '\"';
   for (i=0; i<len; i++) {
      switch (str[i]) {
         case '\r': *p++ = '\\'; *p++ = 'r'; break;
         case '\n': *p++ = '\\'; *p++ = 'n'; break;
         case '\t': *p++ = '\\'; *p++ = 't'; break;
         case '\\': *p++ = '\\'; *p++ = '\\'; break;
         case '\'': *p++ = '\\'; *p++ = '\''; break;
         case '\"': *p++ = '\\'; *p++ = '\"'; break;
            
         default:
            if (str[i] >= 0 && str[i] < 32) {
               *p++ = '\\';
               *p++ = get_hex_char(str[i] >> 4);
               *p++ = get_hex_char(str[i] & 0xF);
            }
            else {
               *p++ = str[i];
            }
            break;
      }
   }
   *p++ = '\"';
   return dest;
}


static char *get_icon_pixels_32(Heap *heap, Value arr, int idx, int flip)
{
   Value img;
   uint32_t *pixels, p;
   int a, r, g, b;
   char *dest;
   int width, height, stride;
   int i, j, err;

   err = fixscript_get_array_elem(heap, arr, idx, &img);
   if (err) {
      return NULL;
   }

   if (!fiximage_get_data(heap, img, &width, &height, &stride, &pixels, NULL, NULL)) {
      return NULL;
   }

   dest = malloc(width*height*4);
   if (!dest) {
      return NULL;
   }

   for (i=0; i<height; i++) {
      for (j=0; j<width; j++) {
         p = pixels[i*stride+j];
         a = (p >> 24) & 0xFF;
         r = (p >> 16) & 0xFF;
         g = (p >>  8) & 0xFF;
         b = (p >>  0) & 0xFF;
         if (a != 0) {
            r = (r*255) / a;
            g = (g*255) / a;
            b = (b*255) / a;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
         }
         if (flip) {
            dest[((height-i-1)*width+j)*4+0] = b;
            dest[((height-i-1)*width+j)*4+1] = g;
            dest[((height-i-1)*width+j)*4+2] = r;
            dest[((height-i-1)*width+j)*4+3] = a;
         }
         else {
            dest[(i*width+j)*4+0] = b;
            dest[(i*width+j)*4+1] = g;
            dest[(i*width+j)*4+2] = r;
            dest[(i*width+j)*4+3] = a;
         }
      }
   }

   return dest;
}


static char *get_icon_pixels_24(Heap *heap, Value arr, int idx, int padding)
{
   Value img;
   uint32_t *pixels, p;
   int a, r, g, b;
   char *dest;
   int width, height, stride;
   int i, j, err;

   err = fixscript_get_array_elem(heap, arr, idx, &img);
   if (err) {
      return NULL;
   }

   if (!fiximage_get_data(heap, img, &width, &height, &stride, &pixels, NULL, NULL)) {
      return NULL;
   }

   dest = malloc(width*height*3 + width*height/8 * (padding? 2:1));
   if (!dest) {
      return NULL;
   }
   memset(dest + width*height*3, 0, width*height/8 * (padding? 2:1));

   for (i=0; i<height; i++) {
      for (j=0; j<width; j++) {
         p = pixels[i*stride+j];
         a = (p >> 24) & 0xFF;
         r = (p >> 16) & 0xFF;
         g = (p >>  8) & 0xFF;
         b = (p >>  0) & 0xFF;
         if (a != 0) {
            r = (r*255) / a;
            g = (g*255) / a;
            b = (b*255) / a;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
         }
         if (a < 128) {
            r = 0;
            g = 0;
            b = 0;
         }
         dest[((height-i-1)*width+j)*3+0] = b;
         dest[((height-i-1)*width+j)*3+1] = g;
         dest[((height-i-1)*width+j)*3+2] = r;
         if (a < 128) {
            if (padding) {
               if ((j & 15) < 8) {
                  dest[width*height*3 + ((((height-i-1)*width+j) >> 4) << 2)+0] |= 1 << (7-((j & 15) & 7));
               }
               else {
                  dest[width*height*3 + ((((height-i-1)*width+j) >> 4) << 2)+1] |= 1 << (7-(((j & 15)-8) & 7));
               }
            }
            else {
               dest[width*height*3 + (((height-i-1)*width+j) >> 3)] |= 1 << (7-(j & 7));
            }
         }
      }
   }

   return dest;
}


int load_icons(int flip)
{
   Heap *heap = NULL;
   Script *script = NULL;
   Icon *icon;
   Value error, arr;
   const void *ptr;
   char *src;
   int i, len=0;

   ptr = icon_files_get("icon.fix", &len);
   src = malloc(len+1);
   memcpy(src, ptr, len);
   src[len] = 0;

   heap = fixscript_create_heap();
   fixio_register_functions(heap);
   fiximage_register_functions(heap);
   fixscript_register_native_func(heap, "get_resource#1", icon_files_get_func, NULL);
   script = fixscript_load(heap, src, "icon.fix", &error, load_build_script, NULL);
   free(src);
   if (!script) {
      fprintf(stderr, "internal error: %s\n", fixscript_get_compiler_error(heap, error));
      return 0;
   }

   arr = fixscript_create_array(heap, 0);
   for (icon = icons; icon; icon = icon->next) {
      fixscript_append_array_elem(heap, arr, fixscript_create_string(heap, icon->fname, -1));
   }
   
   arr = fixscript_run(heap, script, "load_icons#1", &error, arr);
   if (error.value) {
      fprintf(stderr, "error: loading of icons failed\n");
      fixscript_dump_value(heap, error, 1);
      return 0;
   }

   icon_pixels[0] = get_icon_pixels_24(heap, arr, 0, 1);
   icon_pixels[1] = get_icon_pixels_32(heap, arr, 0, flip);
   icon_pixels[2] = get_icon_pixels_24(heap, arr, 1, 0);
   icon_pixels[3] = get_icon_pixels_32(heap, arr, 1, flip);
   icon_pixels[4] = get_icon_pixels_32(heap, arr, 2, flip);
   icon_pixels[5] = get_icon_pixels_32(heap, arr, 3, flip);

   for (i=0; i<6; i++) {
      if (!icon_pixels[i]) {
         fprintf(stderr, "error: loading of icons failed\n");
         return 0;
      }
   }
   return 1;
}


static void replace_pattern(unsigned char *bin_data, int bin_len, const uint8_t *pattern, int pattern_len, char *replacement, int len, int extra)
{
   int i, j, found;
   
   for (i=bin_len-pattern_len-1; i>=0; i--) {
      if (bin_data[i] == pattern[0]) {
         found = 1;
         for (j=1; j<pattern_len; j++) {
            if (bin_data[i+j] != pattern[j]) {
               found = 0;
               break;
            }
         }
         if (found) {
            memcpy(bin_data+(i+pattern_len-len), replacement, len+extra);
            return;
         }
      }
   }
}


static void replace_icons(unsigned char *bin_data, int bin_len)
{
   const uint8_t pattern16_24[12] = { 0xE5, 0xCC, 0x11, 0xF0, 0xB3, 0x5F, 0x49, 0xAC, 0x25, 0x6B, 0xA0, 0xAF };
   const uint8_t pattern16_32[16] = { 0xB0, 0xC6, 0xDE, 0xFF, 0x67, 0x70, 0xA6, 0xFF, 0x44, 0x46, 0x2B, 0xFF, 0x1F, 0xC5, 0x2E, 0xFF };
   const uint8_t pattern32_24[12] = { 0x97, 0x8B, 0x62, 0xD6, 0x34, 0xAC, 0x74, 0xDB, 0x56, 0x83, 0xD9, 0xF7 };
   const uint8_t pattern32_32[16] = { 0xF9, 0xA8, 0xB3, 0xFF, 0x56, 0xC7, 0x57, 0xFF, 0x88, 0x0C, 0xEA, 0xFF, 0x2F, 0x40, 0x82, 0xFF };
   const uint8_t pattern48_32[16] = { 0x5E, 0xD5, 0x45, 0xFF, 0x0E, 0x7D, 0xF8, 0xFF, 0x42, 0x22, 0x7C, 0xFF, 0x6E, 0x21, 0xB9, 0xFF };
   const uint8_t pattern256_32[16] = { 0xB0, 0x5A, 0x05, 0xFF, 0x06, 0x04, 0x1B, 0xFF, 0x4E, 0x4A, 0xAE, 0xFF, 0xBA, 0xEF, 0xCA, 0xFF };

   replace_pattern(bin_data, bin_len, pattern16_24, 12, icon_pixels[0], 16*16*3, 16*16/8*2);
   replace_pattern(bin_data, bin_len, pattern16_32, 16, icon_pixels[1], 16*16*4, 0);
   replace_pattern(bin_data, bin_len, pattern32_24, 12, icon_pixels[2], 32*32*3, 32*32/8);
   replace_pattern(bin_data, bin_len, pattern32_32, 16, icon_pixels[3], 32*32*4, 0);
   replace_pattern(bin_data, bin_len, pattern48_32, 16, icon_pixels[4], 48*48*4, 0);
   replace_pattern(bin_data, bin_len, pattern256_32, 16, icon_pixels[5], 256*256*4, 0);
}


static int compute_pe_checksum(FILE *f)
{
   int len, size;
   uint16_t *data = NULL, sum=0;
   uint32_t tmp, carry=0;
   int checksum_off = 0xD8;
   int i, ret=0;

   fseek(f, 0, SEEK_END);
   len = ftell(f);
   size = (len+1)/2;
   if (len < checksum_off+4) goto error;

   data = malloc(size*2);
   if (!data) goto error;
   data[size-1] = 0;
   
   fseek(f, 0, SEEK_SET);
   if (fread(data, len, 1, f) != 1) goto error;

   data[checksum_off/2+0] = 0;
   data[checksum_off/2+1] = 0;

   for (i=0; i<size; i++) {
      tmp = sum + data[i] + carry;
      sum = tmp;
      carry = tmp >> 16;
   }

   tmp = sum + len;
   fseek(f, checksum_off, SEEK_SET);
   if (fwrite(&tmp, 4, 1, f) != 1) goto error;

   ret = 1;

error:
   free(data);
   return ret;
}


static void uncompress_binary(unsigned char **data, int *len)
{
   if (!gzip_uncompress_memory(*data, *len, data, len)) {
      fprintf(stderr, "decompression error\n");
      exit(1);
   }
}


static char *get_binary(const char *target, const char *variant, int *len_out)
{
   unsigned char *bin, *patch, *result;
   char buf[256];
   int len, patch_len, result_len;

   snprintf(buf, sizeof(buf), "shell_%s.gz", target);
   bin = (unsigned char *)exec_binaries_get(buf, &len);
   if (!bin) {
      return NULL;
   }
   uncompress_binary(&bin, &len);

   snprintf(buf, sizeof(buf), "shell_%s%s.binpatch.gz", target, variant);
   patch = (unsigned char *)exec_binaries_get(buf, &patch_len);
   if (!patch) {
      free(bin);
      return NULL;
   }
   uncompress_binary(&patch, &patch_len);
   if (!apply_patch(bin, len, patch, patch_len, &result, &result_len)) {
      fprintf(stderr, "patching error\n");
      exit(1);
   }

   free(bin);
   free(patch);
   *len_out = result_len;
   return (char *)result;
}


int main(int argc, char **argv)
{
   FILE *out;
   Exclude *exc, *new_exc;
   Icon *icon, *new_icon;
   Value error;
   int argp = 1;
   int has_param;
   int show_help = 0;
   int create_build_script = 0;
   char *bin_data;
   const char *main_script = NULL;
   const char *param_script;
   const char *output_file = NULL;
   const char *target = DEFAULT_TARGET;
   const char *extension;
   int use_gui = 0;
   int bin_len;
#if !defined(_WIN32) && !defined(__wasm__)
   mode_t umask_value;
#endif
   const char *s, *p;
   char buf[256], *tmp, *extra_script;
   int i, first;

#ifdef __wasm__
   verbose = 0;
   use_raw_scripts = 0;
   use_compression = 1;
   total_uncompressed = 0;
   total_compressed = 0;
   excludes = NULL;
   icons = NULL;
   sources_dir = ".";
   resources_dir = NULL;
   out_files = NULL;
   out_buf = NULL;
   out_cap = 0;
   out_len = 0;
   extra_scripts.data = NULL;
   extra_scripts.size = 0;
   extra_scripts.len = 0;
   use_sqlite = 0;
   use_wasm_raw = 0;
   wasm_exitcode = -1;
#endif

#if !defined(_WIN32) && !defined(__wasm__)
   umask_value = umask(0);
   umask(umask_value);
#endif

   if (argc == 2 && strcmp(argv[1], "--fixbuild-version") == 0) {
      printf("%s\n", STR(FIXBUILD_VERSION));
      return 0;
   }

   if (argc == 2 && strcmp(argv[1], "--fixbuild-script-list") == 0) {
      const char * const * s = common_scripts;
      for (; s[0]; s+=2) {
         printf("%s\n", s[0]);
      }
      return 0;
   }

   if (argc == 3 && strcmp(argv[1], "--fixbuild-script-get") == 0) {
      const char *s;
      int len;

      s = common_scripts_get(argv[2], &len);
      if (!s) return 1;
      printf("%s", s);
      return 0;
   }

   if (argc >= 2 && strncmp(argv[1], "--fixbuild-", 11) == 0) {
      fprintf(stderr, "error: unknown backend command\n");
      return 1;
   }

   has_param = 0;
   for (i=1; i<argc; i++) {
      if (strlen(argv[i]) >= 2 && argv[i][0] == '-' && argv[i][1] != '-') {
         has_param = 1;
         break;
      }
   }
   if (!has_param) {
      run_build_script(argc, argv);
   }

   if (argc == 2 && strcmp(argv[1], "-v") == 0) {
      show_help = 1;
   }

   for (;;) {
      if (argp < argc && strcmp(argv[argp], "-h") == 0) {
         show_help = 1;
         argp++;
         break;
      }

      if (argp < argc && strcmp(argv[argp], "-v") == 0) {
         verbose = 1;
         argp++;
         continue;
      }

      if (argp < argc && strcmp(argv[argp], "-np") == 0) {
         use_raw_scripts = 1;
         argp++;
         continue;
      }

      if (argp < argc && strcmp(argv[argp], "-nc") == 0) {
         use_compression = 0;
         argp++;
         continue;
      }

      if (argp < argc && strcmp(argv[argp], "-ex") == 0) {
         if (argp+1 < argc) {
            new_exc = calloc(1, sizeof(Exclude));
            new_exc->path = strdup(argv[argp+1]);
            if (excludes) {
               for (exc = excludes; exc; exc = exc->next) {
                  if (!exc->next) {
                     exc->next = new_exc;
                     break;
                  }
               }
            }
            else {
               excludes = new_exc;
            }
            argp += 2;
            continue;
         }
         else {
            fprintf(stderr, "error: parameter %s requires value\n", argv[argp]);
            show_help = 1;
            break;
         }
      }

      if (argp < argc && strcmp(argv[argp], "-src") == 0) {
         if (argp+1 < argc) {
            sources_dir = argv[argp+1];
            argp += 2;
            continue;
         }
         else {
            fprintf(stderr, "error: parameter %s requires value\n", argv[argp]);
            show_help = 1;
            break;
         }
      }

      if (argp < argc && strcmp(argv[argp], "-res") == 0) {
         if (argp+1 < argc) {
            resources_dir = argv[argp+1];
            argp += 2;
            continue;
         }
         else {
            fprintf(stderr, "error: parameter %s requires value\n", argv[argp]);
            show_help = 1;
            break;
         }
      }

      if (argp < argc && strcmp(argv[argp], "-m") == 0) {
         if (argp+1 < argc) {
            param_script = argv[argp+1];
            #ifdef _WIN32
               for (s=param_script; *s; s++) {
                  if (*s == '\\') {
                     fprintf(stderr, "error: main script name %s must use forward slashes\n", param_script);
                     return 1;
                  }
               }
            #endif
            if (!main_script) {
               main_script = param_script;
            }
            else {
               if (dynarray_add(&extra_scripts, strdup(param_script)) != 0) {
                  fprintf(stderr, "error: out of memory\n");
                  exit(1);
               }
            }
            argp += 2;
            continue;
         }
         else {
            fprintf(stderr, "error: parameter %s requires value\n", argv[argp]);
            show_help = 1;
            break;
         }
      }

      if (argp < argc && strcmp(argv[argp], "-t") == 0) {
         if (argp+1 < argc) {
            target = argv[argp+1];
            argp += 2;
            continue;
         }
         else {
            fprintf(stderr, "error: parameter %s requires value\n", argv[argp]);
            show_help = 1;
            break;
         }
      }

      if (argp < argc && strcmp(argv[argp], "-o") == 0) {
         if (argp+1 < argc) {
            output_file = argv[argp+1];
            argp += 2;
            continue;
         }
         else {
            fprintf(stderr, "error: parameter %s requires value\n", argv[argp]);
            show_help = 1;
            break;
         }
      }

      if (argp < argc && strcmp(argv[argp], "-icon") == 0) {
         if (argp+1 < argc) {
            new_icon = calloc(1, sizeof(Icon));
            new_icon->fname = strdup(argv[argp+1]);
            if (icons) {
               for (icon = icons; icon; icon = icon->next) {
                  if (!icon->next) {
                     icon->next = new_icon;
                     break;
                  }
               }
            }
            else {
               icons = new_icon;
            }
            argp += 2;
            continue;
         }
         else {
            fprintf(stderr, "error: parameter %s requires value\n", argv[argp]);
            show_help = 1;
            break;
         }
      }

      if (argp < argc && strcmp(argv[argp], "-wasm_raw") == 0) {
         use_wasm_raw = 1;
         argp++;
         continue;
      }

      if (argp < argc && strcmp(argv[argp], "-g") == 0) {
         use_gui = 1;
         argp++;
         continue;
      }

      if (argp < argc && strcmp(argv[argp], "-c") == 0) {
         create_build_script = 1;
         argp++;
         continue;
      }

      break;
   }

   if (!show_help && argp < argc) {
      fprintf(stderr, "error: unknown parameter %s\n", argv[argp]);
      show_help = 1;
   }

   if (!output_file && main_script) {
      s = main_script;
      p = strrchr(s, '/');
      if (p) s = p+1;
      p = strrchr(s, '\\');
      if (p) s = p+1;
      output_file = s;
   }

   if (create_build_script) {
      out = fopen("build.fix", "rb");
      if (out) {
         fclose(out);
         fprintf(stderr, "error: 'build.fix' file already exists\n");
         return 1;
      }

      out = fopen("build.fix", "wb");
      if (!out) {
         fprintf(stderr, "error: can't write to 'build.fix' file\n");
         return 1;
      }

      if (!main_script) {
         main_script = "main";
         output_file = main_script;
      }

      write_string(out, "use \"build\";\n\n");
      write_string(out, "build {");
      first = 1;
      if (verbose) {
         write_string(out, first? "\n\t" : ",\n\t");
         write_string(out, "verbose: true");
         first = 0;
      }
      if (use_raw_scripts) {
         write_string(out, first? "\n\t" : ",\n\t");
         write_string(out, "processors: false");
         first = 0;
      }
      if (!use_compression) {
         write_string(out, first? "\n\t" : ",\n\t");
         write_string(out, "compress: false");
         first = 0;
      }
      if (excludes) {
         write_string(out, first? "\n\t" : ",\n\t");
         write_string(out, "exclude: [");
         for (exc=excludes; exc; exc = exc->next) {
            if (exc != excludes) {
               write_string(out, ", ");
            }
            tmp = escape_string(exc->path);
            write_string(out, tmp);
            free(tmp);
         }
         write_string(out, "]");
         first = 0;
      }
      if (strcmp(sources_dir, ".") != 0) {
         write_string(out, first? "\n\t" : ",\n\t");
         write_string(out, "sources: ");
         tmp = escape_string(sources_dir);
         write_string(out, tmp);
         free(tmp);
         first = 0;
      }
      if (resources_dir) {
         write_string(out, first? "\n\t" : ",\n\t");
         write_string(out, "resources: ");
         tmp = escape_string(resources_dir);
         write_string(out, tmp);
         free(tmp);
         first = 0;
      }
      if (main_script) {
         write_string(out, first? "\n\t" : ",\n\t");
         write_string(out, "main: ");
         if (extra_scripts.len > 0) {
            write_string(out, "[");
         }
         tmp = escape_string(main_script);
         write_string(out, tmp);
         free(tmp);
         if (extra_scripts.len > 0) {
            for (i=0; i<extra_scripts.len; i++) {
               write_string(out, ", ");
               tmp = escape_string(extra_scripts.data[i]);
               write_string(out, tmp);
               free(tmp);
            }
            write_string(out, "]");
         }
         first = 0;
      }
      if (output_file) {
         write_string(out, first? "\n\t" : ",\n\t");
         write_string(out, "binary: ");
         tmp = escape_string(output_file);
         write_string(out, tmp);
         free(tmp);
         first = 0;
      }
      if (use_gui) {
         write_string(out, first? "\n\t" : ",\n\t");
         write_string(out, "gui: true");
         first = 0;
      }
      if (icons) {
         write_string(out, first? "\n\t" : ",\n\t");
         write_string(out, "icon: [");
         for (icon=icons; icon; icon = icon->next) {
            if (icon != icons) {
               write_string(out, ", ");
            }
            tmp = escape_string(icon->fname);
            write_string(out, tmp);
            free(tmp);
         }
         write_string(out, "]");
         first = 0;
      }
      write_string(out, "\n};\n");
      fclose(out);

      snprintf(buf, sizeof(buf), "%s.fix", main_script);
      out = fopen(buf, "rb");
      if (out) {
         fclose(out);
      }
      else {
         out = fopen(buf, "wb");
         if (!out) {
            fprintf(stderr, "error: can't write to '%s' file\n", buf);
            return 1;
         }
         write_string(out, "use \"classes\";\n\n");
         if (use_gui) {
            write_string(out, "import \"gui/view\";\n\n");
            write_string(out, "class MainWindow: Window\n");
            write_string(out, "{\n");
            write_string(out, "\tconstructor create()\n");
            write_string(out, "\t{\n");
            write_string(out, "\t\tsuper::create(\"Main Window\", 800, 600, WIN_CENTER | WIN_RESIZABLE);\n");
            write_string(out, "\t\tset_visible(true);\n");
            write_string(out, "\t}\n");
            write_string(out, "}\n\n");
            write_string(out, "function init()\n");
            write_string(out, "{\n");
            write_string(out, "\tMainWindow::create();\n");
            write_string(out, "}\n");
         }
         else {
            write_string(out, "function main()\n");
            write_string(out, "{\n");
            write_string(out, "}\n");
         }
         fclose(out);
      }
      return 0;
   }

   if (!show_help && !main_script) {
      if (argc > 1) {
         fprintf(stderr, "error: must provide main script name\n");
      }
      show_help = 1;
   }

   if (show_help) {
      fprintf(stderr, "FixBuild v" STR(FIXBUILD_VERSION) " - https://www.fixscript.org/\n");
      fprintf(stderr, "\n");
      fprintf(stderr, "Usage: %s [options]\n", argv[0]);
      fprintf(stderr, "\n");
      fprintf(stderr, "    -h           shows this help\n");
      fprintf(stderr, "    -v           verbose mode\n");
      fprintf(stderr, "    -np          do not run token processors\n");
      fprintf(stderr, "    -nc          do not compress scripts\n");
      fprintf(stderr, "    -ex <name>   exclude file name or directory\n");
      fprintf(stderr, "    -src <dir>   directory with sources (defaults to current directory)\n");
      fprintf(stderr, "    -res <dir>   embed resources from given directory\n");
      fprintf(stderr, "    -m <script>  main script name (must be specified)\n");
      fprintf(stderr, "    -t <target>  select platform target (defaults to " DEFAULT_TARGET ")\n");
      fprintf(stderr, "    -o <file>    output executable file (defaults to same name as main script)\n");
      fprintf(stderr, "    -icon <file> provide icon as a PNG file (multiple sizes can be provided)\n");
      fprintf(stderr, "    -wasm_raw    produce raw WASM instead of HTML for GUI applications\n");
      fprintf(stderr, "    -g           build GUI application (experimental)\n");
      fprintf(stderr, "    -c           create build script (can be combined with other arguments)\n");
      fprintf(stderr, "\n");
      fprintf(stderr, "Supported targets:\n");
      fprintf(stderr, "    windows32    Windows 32bit\n");
      fprintf(stderr, "    windows64    Windows 64bit\n");
      fprintf(stderr, "    linux32      Linux 32bit\n");
      fprintf(stderr, "    linux64      Linux 64bit\n");
      fprintf(stderr, "    macos        Mac OS (Intel 64bit)\n");
      fprintf(stderr, "    haiku32      Haiku 32bit\n");
      fprintf(stderr, "    haiku64      Haiku 64bit\n");
      fprintf(stderr, "    raspi32      Raspberry Pi 32bit\n");
      fprintf(stderr, "    wasm         WebAssembly (WASI/HTML)\n");
      fprintf(stderr, "\n");
      fprintf(stderr, "When 'build.fix' is present in the current or any parent directory it is\n");
      fprintf(stderr, "executed unless a single dash parameter is used.\n");
      #ifndef _WIN32
      fprintf(stderr, "\n");
      #endif
      return 1;
   }

   if (use_gui && icons && strncmp(target, "windows", 7) == 0) {
      if (!load_icons(1)) return 1;
   }

   heap = fixscript_create_heap();
   heap->token_dump_mode = 1;
   heap->token_heap = fixscript_create_heap();
   heap->token_heap->script_heap = heap;

   if (use_raw_scripts) {
      if (strcmp(target, "wasm") != 0) {
         use_sqlite = 1;
      }
      traverse_dir(sources_dir, sources_dir, 0);
   }
   else {
      if (!fixscript_load(heap, fixup_script, "__fixlines.fix", &error, NULL, NULL)) {
         fprintf(stderr, "%s\n", fixscript_get_compiler_error(heap->token_heap, error));
         exit(1);
      }
      if (!fixscript_load(heap->token_heap, fixup_script, "__fixlines.fix", &error, NULL, NULL)) {
         fprintf(stderr, "%s\n", fixscript_get_compiler_error(heap->token_heap, error));
         exit(1);
      }
      if (!load_script_file(heap, main_script, &error, sources_dir)) {
         fprintf(stderr, "%s\n", fixscript_get_compiler_error(heap->token_heap, error));
         exit(1);
      }
      if (use_gui && strcmp(target, "wasm") == 0) {
         if (!load_script_file(heap, "gui/virtual/handler", &error, sources_dir)) {
            fprintf(stderr, "%s\n", fixscript_get_compiler_error(heap->token_heap, error));
            exit(1);
         }
      }
      while (extra_scripts.len > 0) {
         extra_script = extra_scripts.data[--extra_scripts.len];
         if (!load_script_file(heap, extra_script, &error, sources_dir)) {
            fprintf(stderr, "%s\n", fixscript_get_compiler_error(heap->token_heap, error));
            exit(1);
         }
         free(extra_script);
      }
      free(extra_scripts.data);
   }

   if (resources_dir) {
      traverse_dir(resources_dir, resources_dir, 1);
   }

   if (use_raw_scripts) {
      const char * const * s = common_scripts;
      const char *data;
      char *compressed;
      int len = 0, compressed_len;
      for (; s[0]; s+=2) {
         if (!has_file(s[0])) {
            data = common_scripts_get(s[0], &len);
            if (use_compression) {
               if (!compress_script(data, len, &compressed, &compressed_len)) abort();
               put_file(s[0], compressed, compressed_len, 0, 1);
               free(compressed);
            }
            else {
               put_file(s[0], data, len, 0, 0);
            }
         }
      }
   }

   if (strcmp(target, "wasm") == 0 && use_sqlite) {
      fprintf(stderr, "error: embedded SQLite is currently not supported in WebAssembly target\n");
      return 1;
   }

   if (strncmp(target, "windows", 7) == 0) {
      extension = ".exe";
   }
   else if (strcmp(target, "wasm") == 0) {
      if (use_gui && !use_wasm_raw) {
         extension = ".html";
      }
      else {
         extension = ".wasm";
      }
   }
   else {
      extension = "";
   }
   snprintf(buf, sizeof(buf), "%s%s", output_file, extension);
   out = fopen(buf, "w+b");
   if (!out) {
      perror("can't write to out file");
      return 1;
   }
   snprintf(buf, sizeof(buf), "%s%s", use_sqlite? "_sqlite":"", use_gui? (icons && strncmp(target, "windows", 7) == 0? "_gui_icon" : "_gui") : "");
   bin_data = get_binary(target, buf, &bin_len);
   if (!bin_data) {
      snprintf(buf, sizeof(buf), "%s%s", use_sqlite? "_sqlite":"", "");
      if (use_gui && get_binary(target, buf, &bin_len)) {
         fprintf(stderr, "missing GUI support for platform: %s\n", target);
      }
      else {
         fprintf(stderr, "unknown target platform: %s\n", target);
      }
      return 1;
   }
   if (use_gui && icons && strncmp(target, "windows", 7) == 0) {
      replace_icons((unsigned char *)bin_data, bin_len);
   }
   write_files(out, bin_data, bin_len, main_script, target, use_gui);
   if (strncmp(target, "windows", 7) == 0) {
      if (!compute_pe_checksum(out)) {
         fprintf(stderr, "PE checksum computation error\n");
         return 1;
      }
   }
   fclose(out);
   free(bin_data);

#if !defined(_WIN32) && !defined(__wasm__)
   if (strncmp(target, "windows", 7) != 0 && strcmp(target, "wasm") != 0) {
      chmod(output_file, 0777 & ~umask_value);
   }
#endif

   if (use_compression && verbose) {
      fprintf(stderr, "\ntotal compressed %d bytes to %d (%.2fx)\n", total_uncompressed, total_compressed, (double)total_uncompressed/total_compressed);
      fflush(stderr);
   }
   return 0;
}
