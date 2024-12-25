/*
 * FixBrowser v0.1 - https://www.fixbrowser.org/
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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
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

typedef struct DirEntry {
   char *name;
   int dir;
   struct DirEntry *next;
} DirEntry;

FILE *out;


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
         free_dir_entries(entries);
         closedir(dir);
         return NULL;
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


void embed_file(const char *fname)
{
   FILE *f;
   char buf[1024];
   int num;
   int first, second;
   union {
      uint16_t value;
      uint8_t c[2];
   } u;

   f = fopen(fname, "r");
   if (!f) {
      perror("fopen");
      exit(1);
   }
   fprintf(out, "   \"");
   while (fgets(buf, sizeof(buf), f)) {
      u.value = 0xFFFF;
      num = sscanf(buf, "0x%x\t0x%x", &first, &second);
      if (num == 2) {
         //printf("%x\n", second);
         u.value = second;
      }
      else if (num == 1) {
         //printf("0xFFFD (undefined)\n");
         u.value = 0xFFFD;
      }
      if (u.value != 0xFFFF) {
         fprintf(out, "\\%03o\\%03o", u.c[0], u.c[1]);
      }
   }
   if (ferror(f)) {
      errno = ferror(f);
      perror("fgets");
      exit(1);
   }
   fprintf(out, "\\%03o\\%03o", 0xFF, 0xFF);
   fprintf(out, "\",\n\n");
   fclose(f);
}


void traverse_dir(const char *dirname)
{
   DirEntry *entries, *e;
   char tmp[256], *prefix;
   
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

   for (e=entries; e; e=e->next) {
      if (e->name[0] == '.') continue;

      if (!e->dir && ends_with(e->name, ".txt")) {
         strcpy(tmp, e->name);
         tmp[strlen(tmp)-4] = 0;
         fprintf(out, "   \"%s\",\n", tmp);
         if (snprintf(tmp, sizeof(tmp), "%s/%s", dirname, e->name) >= sizeof(tmp)) {
            fprintf(stderr, "path too long");
            exit(1);
         }
         embed_file(tmp);
      }
   }

   free_dir_entries(entries);
}


int main(int argc, char **argv)
{
   if (argc < 4) {
      fprintf(stderr, "Usage: %s <dir> <out-file> <var-name>\n", argv[0]);
      return 1;
   }

   out = fopen(argv[2], "w");
   if (!out) {
      perror("can't write to out file");
      return 1;
   }

   fprintf(out, "static const char * const %s[] = {\n", argv[3]);
   traverse_dir(argv[1]);
   fprintf(out, "   (void *)0, (void *)0\n");
   fprintf(out, "};\n");

   fclose(out);
   return 0;
}
