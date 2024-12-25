/*
 * FixScript v0.9 - https://www.fixscript.org/
 * Copyright (c) 2018-2024 Martin Dvorak <jezek2@advel.cz>
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

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <errno.h>

#define FIXEMBED_TOKEN_DUMP
#include "fixscript.c"

typedef struct DirEntry {
   char *name;
   int dir;
   struct DirEntry *next;
} DirEntry;

typedef struct Exclude {
   const char *path;
   struct Exclude *next;
} Exclude;

Heap *heap;
FILE *out;
int verbose = 0;
int use_raw_scripts = 0;
int use_compression = 1;
int total_uncompressed = 0;
int total_compressed = 0;
Exclude *excludes = NULL;
int binary_mode = 0;

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
      out[out_len++] = value;                                         \
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


#ifdef _WIN32

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


void embed_file(const char *fname, const char *script_name)
{
   char *src, *compressed;
   int i, c, len, compressed_len;
   unsigned char buf[4];

   if (verbose) {
      if (binary_mode) {
         fprintf(stderr, "processing %s...", script_name);
      }
      else {
         fprintf(stderr, "processing %s.fix...", script_name);
      }
      fflush(stderr);
   }

   if (!read_file(fname, &src, &len)) {
      perror("can't read file");
      exit(1);
   }

   fprintf(out, "   \"");
   if (binary_mode) {
      memcpy(buf, &len, 4);
      fprintf(out, "\\%03o\\%03o\\%03o\\%03o\"\n   \"", buf[0], buf[1], buf[2], buf[3]);
      for (i=0; i<len; i++) {
         c = (unsigned char)src[i];
         fprintf(out, "\\%03o", c);
         if ((i % 32) == 31) {
            fprintf(out, "\"\n   \"");
         }
      }
      if (verbose) {
         fprintf(stderr, "\rprocessing %s   \n", script_name);
         fflush(stderr);
      }
   }
   else if (use_compression) {
      if (!compress_script(src, len, &compressed, &compressed_len)) abort();
      for (i=0; i<compressed_len; i++) {
         c = (unsigned char)compressed[i];
         fprintf(out, "\\%03o", c);
         if ((i % 32) == 31) {
            fprintf(out, "\"\n   \"");
         }
      }
      if (verbose) {
         fprintf(stderr, "\rprocessing %s.fix (compressed %d bytes to %d, %0.2fx)\n", script_name, len, compressed_len, (double)len/compressed_len);
         fflush(stderr);
      }
      total_uncompressed += len;
      total_compressed += compressed_len;
      free(compressed);
   }
   else {
      for (i=0; i<len; i++) {
         c = (unsigned char)src[i];
         if (c == '\n') {
            fprintf(out, "\\n\"\n   \"");
         }
         else if (c == '\\') {
            fprintf(out, "\\\\");
         }
         else if (c == '\"') {
            fprintf(out, "\\\"");
         }
         else if (c == '\t') {
            fprintf(out, "\\t");
         }
         else if (c >= 32 && c < 128) {
            fprintf(out, "%c", c);
         }
         else {
            fprintf(out, "\\%03o", c);
         }
      }
      if (verbose) {
         fprintf(stderr, "\rprocessing %s.fix   \n", script_name);
         fflush(stderr);
      }
   }
   fprintf(out, "\",\n\n");

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


void fixembed_native_function_used(const char *name)
{
}


void fixembed_dump_tokens(const char *fname, Tokenizer *tok)
{
   Tokenizer tok_sav = *tok;
   String str;
   Value line_adjusts;
   int i, c, new_line=1, last_line=1, suppress, prev_type=-1, cur_type, num_tokens=1, len, compressed_len;
   const char *prefix;
   char prev_symbol[5], symbol[5], *ser, *script_code, *compressed;

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

   if (use_compression) {
      prefix = "use \"__fixlines\";";
   }
   else {
      prefix = "use \\\"__fixlines\\\";";
   }
   if (!string_append(&str, prefix)) abort();

   fprintf(out, "   \"%s\",\n", fname);
   fprintf(out, "   \"");

   while (next_token(tok)) {
      if (tok->line != last_line) {
         if (tok->line > last_line && tok->line - last_line < 100) {
            while (tok->line > last_line+1) {
               if (!string_append(&str, use_compression? "\n" : "\\n")) abort();
               last_line++;
            }
         }
         if (!string_append(&str, use_compression? "\n" : "\\n\"\n   \"")) abort();
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

      if (use_compression) {
         if (!string_append(&str, "%.*s", tok->len, tok->value)) abort();
      }
      else {
         for (i=0; i<tok->len; i++) {
            c = (unsigned char)tok->value[i];
            if (c == '\\') {
               if (!string_append(&str, "\\\\")) abort();
            }
            else if (c == '\"') {
               if (!string_append(&str, "\\\"")) abort();
            }
            else if (c == '\t') {
               if (!string_append(&str, "\\t")) abort();
            }
            else if (c >= 32 && c < 127) {
               if (!string_append(&str, "%c", c)) abort();
            }
            else {
               if (!string_append(&str, "\\%03o", c)) abort();
            }
         }
      }
      num_tokens++;
   }

   fixscript_get_array_length(heap, line_adjusts, &len);
   if (len > 0) {
      if (fixscript_append_array_elem(heap, line_adjusts, fixscript_int(num_tokens)) != 0) abort();
      if (!string_append(&str, use_compression? "\n\"" : "\\n\"\n   \"\\\"")) abort();

      if (fixscript_serialize_to_array(heap, &ser, &len, line_adjusts) != 0) abort();
      for (i=0; i<len; i++) {
         c = (unsigned char)ser[i];
         if (c == '\\') {
            if (!string_append(&str, use_compression? "\\\\" : "\\\\\\\\")) abort();
         }
         else if (c == '\"') {
            if (!string_append(&str, use_compression? "\\\"" : "\\\\\\\"")) abort();
         }
         else if (c == '\t') {
            if (!string_append(&str, use_compression? "\\t" : "\\\\t")) abort();
         }
         else if (c == '\r') {
            if (!string_append(&str, use_compression? "\\r" : "\\\\r")) abort();
         }
         else if (c == '\n') {
            if (!string_append(&str, use_compression? "\\n" : "\\\\n")) abort();
         }
         else if (c >= 32 && c < 127) {
            if (!string_append(&str, "%c", c)) abort();
         }
         else {
            if (!string_append(&str, use_compression? "\\%02x" : "\\\\%02x", c)) abort();
         }
      }

      if (!string_append(&str, use_compression? "\"" : "\\\"")) abort();
      free(ser);

      script_code = str.data;
   }
   else {
      script_code = str.data + strlen(prefix);
   }

   if (use_compression) {
      if (!compress_script(script_code, strlen(script_code), &compressed, &compressed_len)) abort();
      for (i=0; i<compressed_len; i++) {
         c = (unsigned char)compressed[i];
         fprintf(out, "\\%03o", c);
         if ((i % 32) == 31) {
            fprintf(out, "\"\n   \"");
         }
      }
      if (verbose) {
         fprintf(stderr, "\rprocessing %s (compressed %d bytes to %d, %0.2fx)\n", fname, (int)strlen(script_code), compressed_len, (double)strlen(script_code)/compressed_len);
         fflush(stderr);
      }
      total_uncompressed += strlen(script_code);
      total_compressed += compressed_len;
      free(compressed);
   }
   else {
      fprintf(out, "%s", script_code);
      if (verbose) {
         fprintf(stderr, "\rprocessing %s   \n", fname);
         fflush(stderr);
      }
   }

   fprintf(out, "\",\n\n");

   free(str.data);
   fixscript_unref(heap, line_adjusts);
   *tok = tok_sav;
}


void traverse_dir(const char *dirname, const char *orig_dirname)
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
         fprintf(out, "   \"%s\",\n", tmp2);
         embed_file(tmp, tmp2);
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
         *strrchr(tmp2, '.') = '\0';
         if (use_raw_scripts) {
            fprintf(out, "   \"%s%s%s\",\n", prefix, prefix[0]? "/" : "", e->name);
            embed_file(tmp, tmp2);
         }
         else {
            if (!fixscript_load_file(heap, tmp2, &error, orig_dirname)) {
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
         traverse_dir(tmp, orig_dirname);
      }
   }

   free_dir_entries(entries);
}


#if 0
void test_combinations()
{
   const char *sym[55] = {
      "(", ")", "{", "}", "[", "]", ",", ";", "~", ":", "@", "?", "+", "-", "*", "/", "%", "&", "|", "^", "=", "!", "<", ">", "#", "$", ".", "\\", "`", //29
      "+=", "++", "-=", "--", "->", "*=", "/=", "%=", "&=", "&&", "|=", "||", "^=", "<=", "<<", ">=", ">>", "==", "!=", "..", //20
      "===", "!==", "<<=", ">>=", ">>>", //5
      ">>>=" //1
   };
   const char *s1, *s2, *s3, *s4;
   char buf[256];
   int i, j, k, l, m, n, found;

   for (i=0; i<55; i++) {
      for (j=0; j<55; j++) {
         s1 = sym[i];
         s2 = sym[j];
         found = 0;
         for (k=0; k<55; k++) {
            for (l=0; l<55; l++) {
               s3 = sym[k];
               s4 = sym[l];
               sprintf(buf, "%s%s%s%s", s1, s2, s3, s4);
               for (m=strlen(s1)+1; m<=strlen(buf); m++) {
                  for (n=0; n<55; n++) {
                     if (strncmp(buf, sym[n], m) == 0) {
                        found = 2;
                        goto found2_end;
                     }
                  }
               }
               found2_end:;
            }
         }
         sprintf(buf, "%s%s", s1, s2);
         for (k=0; k<55; k++) {
            if (strcmp(buf, sym[k]) == 0) {
               found = 1;
               break;
            }
         }
         if (found) {
            printf("collision %s %s%s\n", s1, s2, found == 2? " (2)":"");
            if (!symbols_require_whitespace(s1, s2)) {
               printf("ERR1!\n");
            }
         }
         else {
            if (symbols_require_whitespace(s1, s2)) {
               printf("ERR2!\n");
            }
         }
      }
   }
}
#endif


int main(int argc, char **argv)
{
   Exclude *exc;
   Value error;
   int argp = 1;
   int show_help = 0;

   for (;;) {
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
            exc = malloc(sizeof(Exclude));
            exc->path = strdup(argv[argp+1]);
            exc->next = excludes;
            excludes = exc;
            argp += 2;
            continue;
         }
         else {
            fprintf(stderr, "error: parameter %s requires value\n", argv[argp]);
            show_help = 1;
            break;
         }
      }

      if (argp < argc && strcmp(argv[argp], "-bin") == 0) {
         binary_mode = 1;
         argp++;
         continue;
      }

      break;
   }

   if (!show_help && argp < argc && strncmp(argv[argp], "-", 1) == 0) {
      fprintf(stderr, "error: unknown parameter %s\n", argv[argp]);
      show_help = 1;
   }

   if (argc - argp < 3) {
      show_help = 1;
   }

   if (show_help) {
      fprintf(stderr, "Usage: %s [options] <dir> <out-file> <var-name>\n", argv[0]);
      fprintf(stderr, "\n");
      fprintf(stderr, "    -v          verbose mode\n");
      fprintf(stderr, "    -np         do not run token processors\n");
      fprintf(stderr, "    -nc         do not compress scripts\n");
      fprintf(stderr, "    -ex <name>  exclude file name or directory\n");
      fprintf(stderr, "    -bin        binary mode (stores files instead of scripts)\n");
      fprintf(stderr, "\n");
      return 1;
   }

   heap = fixscript_create_heap();
   heap->token_dump_mode = 1;
   heap->token_heap = fixscript_create_heap();
   heap->token_heap->script_heap = heap;

   out = fopen(argv[argp+1], "w");
   if (!out) {
      perror("can't write to out file");
      return 1;
   }

   if (binary_mode) {
      fprintf(out, "#include <string.h>\n");
      fprintf(out, "#ifdef FIXSCRIPT_H\n");
      fprintf(out, "#include <stdlib.h>\n");
      fprintf(out, "#endif\n\n");
   }

   fprintf(out, "static const char * const %s[] = {\n", argv[argp+2]);
   if (!use_raw_scripts && !binary_mode) {
      if (!fixscript_load(heap, fixup_script, "__fixlines.fix", &error, NULL, NULL)) {
         fprintf(stderr, "%s\n", fixscript_get_compiler_error(heap->token_heap, error));
         exit(1);
      }
   }
   traverse_dir(argv[argp+0], argv[argp+0]);
   fprintf(out, "   (void *)0, (void *)0\n");
   fprintf(out, "};\n");

   if (binary_mode) {
      fprintf(out, "\nstatic inline const char *%s_get(const char *fname, int *len)\n", argv[argp+2]);
      fprintf(out, "{\n");
      fprintf(out, "   const char * const * s = %s;\n", argv[argp+2]);
      fprintf(out, "   for (; s[0]; s+=2) {\n");
      fprintf(out, "      if (strcmp(fname, s[0]) == 0) {\n");
      fprintf(out, "         memcpy(len, s[1], 4);\n");
      fprintf(out, "         return s[1] + 4;\n");
      fprintf(out, "      }\n");
      fprintf(out, "   }\n");
      fprintf(out, "   return (void *)0;\n");
      fprintf(out, "}\n");

      fprintf(out, "\n#ifdef FIXSCRIPT_H\n");
      fprintf(out, "\nstatic inline Value %s_get_func(Heap *heap, Value *error, int num_params, Value *params, void *data)\n", argv[argp+2]);
      fprintf(out, "{\n");
      fprintf(out, "   const char *bin;\n");
      fprintf(out, "   char *fname, *copy, buf[256];\n");
      fprintf(out, "   int err, len;\n");
      fprintf(out, "   Value ret;\n");
      fprintf(out, "   err = fixscript_get_string(heap, params[0], 0, -1, &fname, NULL);\n");
      fprintf(out, "   if (err) return fixscript_error(heap, error, err);\n");
      fprintf(out, "   bin = %s_get(fname, &len);\n", argv[argp+2]);
      fprintf(out, "   if (!bin) {\n");
      fprintf(out, "      snprintf(buf, sizeof(buf), \"resource '%%s' not found\", fname);\n");
      fprintf(out, "      free(fname);\n");
      fprintf(out, "      *error = fixscript_create_error_string(heap, buf);\n");
      fprintf(out, "      return fixscript_int(0);\n");
      fprintf(out, "   }\n");
      fprintf(out, "   free(fname);\n");
      fprintf(out, "   copy = malloc(len);\n");
      fprintf(out, "   if (!copy) return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);\n");
      fprintf(out, "   memcpy(copy, bin, len);\n");
      fprintf(out, "   ret = fixscript_create_or_get_shared_array(heap, -1, copy, len, 1, free, copy, NULL);\n");
      fprintf(out, "   if (!ret.value) return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);\n");
      fprintf(out, "   return ret;\n");
      fprintf(out, "}\n");
      fprintf(out, "\n#endif\n");
   }

   fclose(out);

   if (use_compression && verbose && !binary_mode) {
      fprintf(stderr, "\ntotal compressed %d bytes to %d (%0.2fx)\n", total_uncompressed, total_compressed, (double)total_uncompressed/total_compressed);
      fflush(stderr);
   }
   return 0;
}
