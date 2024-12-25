/*
 * This file was written in 2024 by Martin Dvorak <jezek2@advel.cz>
 * You can download latest version at http://public-domain.advel.cz/
 *
 * To the extent possible under law, the author(s) have dedicated all
 * copyright and related and neighboring rights to this file to the
 * public domain worldwide. This software is distributed without any
 * warranty.
 *
 * You should have received a copy of the CC0 Public Domain Dedication
 * along with this software. If not, see:
 * http://creativecommons.org/publicdomain/zero/1.0/ 
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <math.h>

// Binary diff/patch code available at http://public-domain.advel.cz/ under CC0 license

typedef struct Entry {
   uint32_t key;
   int idx;
   struct Entry *next;
} Entry;

typedef struct Span {
   int start, end;
   int src_idx;
   struct Span *next;
} Span;


int read_file(const char *fname, unsigned char **data_out, int *len_out)
{
   FILE *f = NULL;
   unsigned char *data = NULL, test;
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


int write_file(const char *fname, const unsigned char *data, int len)
{
   FILE *f;

   f = fopen(fname, "wb");
   if (!f) {
      return 0;
   }

   if (len > 0 && fwrite(data, len, 1, f) != 1) {
      fclose(f);
      return 0;
   }

   if (fclose(f) != 0) {
      return 0;
   }
   return 1;
}


static int apply(unsigned char *src, int src_len, unsigned char *patch, int patch_len, unsigned char **result_out, int *result_len_out)
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


static int diff(unsigned char *src, int src_len, unsigned char *dest, int dest_len, int match_size, int threshold, int max_len, unsigned char **patch_out, int *patch_len_out)
{
   Entry **hash = NULL, *entry, *next_entry;
   Span *first_span = NULL, *next_span, *span, *new_span;
   unsigned char *window = NULL, *accum_window = NULL, *patch = NULL;
   int hash_size = 256, value;
   uint32_t sum;
   int i, j, k, m, pos, accum_pos, repeat, accum_value, start, end, best_len, best_dest_start = 0, best_src_start = 0, patch_len, ret=0;

   if (dest_len < match_size || src_len < match_size) {
      if (dest_len > INT_MAX - 8) {
         return 0;
      }
      patch_len = 8 + dest_len;
      patch = malloc(patch_len);
      if (!patch) {
         return 0;
      }
      memcpy(patch + 0, &dest_len, 4);
      value = 0;
      memcpy(patch + 4, &value, 4);
      memcpy(patch + 8, dest, dest_len);
      *patch_out = patch;
      *patch_len_out = patch_len;
      return 1;
   }

   threshold *= match_size;

   while (hash_size < src_len / match_size) {
      if (hash_size > INT_MAX/2) {
         goto error;
      }
      hash_size *= 2;
   }

   hash = calloc(hash_size, sizeof(Entry *));
   window = calloc(1, match_size);
   accum_window = calloc(1, match_size);
   if (!hash || !window || !accum_window) {
      goto error;
   }

   for (i=0; i<=src_len-match_size; i+=match_size) {
      sum = 0;
      for (j=0; j<match_size; j++) {
         sum += src[i+j];
      }
      entry = malloc(sizeof(Entry));
      if (!entry) {
         goto error;
      }
      entry->key = sum;
      entry->idx = i;
      entry->next = hash[sum & (hash_size-1)];
      hash[sum & (hash_size-1)] = entry;
   }

   span = malloc(sizeof(Span));
   if (!span) {
      goto error;
   }
   span->start = 0;
   span->end = dest_len;
   span->src_idx = -1;
   span->next = NULL;
   first_span = span;

   for (;;) {
      repeat = 0;
      span = first_span;
      while (span) {
         if (span->src_idx == -1 && span->end - span->start >= match_size) {
            sum = 0;
            pos = 0;
            for (i=span->start; i<span->start + match_size; i++) {
               sum += window[pos] = dest[i];
               pos = (pos+1) % match_size;
            }
            best_len = 0;
            for (i=span->start + match_size; i<span->end; i++) {
               entry = hash[sum & (hash_size-1)];
               while (entry) {
                  if (entry->key == sum && memcmp(dest + (i-match_size), src + entry->idx, match_size) == 0) {
                     memset(accum_window, 0, match_size);
                     accum_pos = 0;
                     accum_value = 0;
                     end = i;
                     for (k=i, m=entry->idx+match_size; k<span->end && m<src_len; k++, m++) {
                        accum_value -= accum_window[accum_pos];
                        accum_value += accum_window[accum_pos] = abs((int)dest[k] - (int)src[m]);
                        accum_pos = (accum_pos+1) % match_size;
                        end = k;
                        if (accum_value >= threshold) {
                           break;
                        }
                     }
                     memset(accum_window, 0, match_size);
                     accum_pos = 0;
                     accum_value = 0;
                     start = span->start;
                     for (k=i-match_size-1, m=entry->idx-1; k>=span->start && m>=0; k--, m--) {
                        accum_value -= accum_window[accum_pos];
                        accum_value += accum_window[accum_pos] = abs((int)dest[k] - (int)src[m]);
                        accum_pos = (accum_pos+1) % match_size;
                        if (accum_value >= threshold) {
                           start = k+1;
                           break;
                        }
                     }
                     if (end-start > best_len) {
                        best_len = end-start;
                        best_dest_start = start;
                        best_src_start = entry->idx - ((i-match_size)-start);
                        if (best_dest_start + best_len > dest_len) {
                           goto error;
                        }
                        if (best_src_start + best_len > src_len) {
                           goto error;
                        }
                        if (best_len >= max_len) {
                           goto has_match;
                        }
                     }
                  }
                  entry = entry->next;
               }
               sum -= window[pos];
               sum += window[pos] = dest[i];
               pos = (pos+1) % match_size;
            }
            has_match:;
            if (best_len > 0) {
               if (best_len == span->end - span->start && best_dest_start == span->start) {
                  span->src_idx = best_src_start;
                  repeat = 1;
               }
               else if (best_dest_start == span->start) {
                  new_span = malloc(sizeof(Span));
                  if (!new_span) {
                     goto error;
                  }
                  new_span->start = best_dest_start + best_len;
                  new_span->end = span->end;
                  new_span->src_idx = -1;
                  new_span->next = span->next;
                  span->next = new_span;

                  span->end = best_dest_start + best_len;
                  span->src_idx = best_src_start;

                  repeat = 1;
               }
               else if (best_dest_start + best_len == span->end) {
                  new_span = malloc(sizeof(Span));
                  if (!new_span) {
                     goto error;
                  }
                  new_span->start = best_dest_start;
                  new_span->end = span->end;
                  new_span->src_idx = best_src_start;
                  new_span->next = span->next;
                  span->next = new_span;

                  span->end = best_dest_start;

                  repeat = 1;
               }
               else {
                  new_span = malloc(sizeof(Span));
                  if (!new_span) {
                     goto error;
                  }
                  new_span->start = best_dest_start + best_len;
                  new_span->end = span->end;
                  new_span->src_idx = -1;
                  new_span->next = span->next;
                  span->next = new_span;

                  new_span = malloc(sizeof(Span));
                  if (!new_span) {
                     goto error;
                  }
                  new_span->start = best_dest_start;
                  new_span->end = best_dest_start + best_len;
                  new_span->src_idx = best_src_start;
                  new_span->next = span->next;
                  span->next = new_span;

                  span->end = best_dest_start;

                  repeat = 1;
               }
            }
            else {
               span->src_idx = -2;
            }
         }
         else if (span->src_idx == -1) {
            span->src_idx = -2;
         }
         span = span->next;
      }
      if (!repeat) break;
   }

   if (first_span->src_idx < 0) {
      first_span->src_idx = 0;
   }

   for (span = first_span; span; span = span->next) {
      if (span->next && span->next->src_idx < 0) {
         next_span = span->next;
         span->next = next_span->next;
         span->end = next_span->end;
         free(next_span);

         if (span->src_idx + (span->end - span->start) > src_len) {
            new_span = malloc(sizeof(Span));
            if (!new_span) {
               goto error;
            }
            new_span->start = span->start + (src_len - span->src_idx);
            new_span->end = span->end;
            new_span->src_idx = src_len - (span->end - span->start);
            if (new_span->src_idx < 0) {
               new_span->src_idx = 0;
            }
            new_span->next = span->next;
            span->next = new_span;
            span->end = new_span->start;
         }
      }
   }

   for (span = first_span; span; ) {
      if (span->next && span->end == span->next->start && span->src_idx + (span->end - span->start) == span->next->src_idx) {
         next_span = span->next;
         span->next = next_span->next;
         span->end = next_span->end;
         free(next_span);
      }
      else {
         span = span->next;
      }
   }

   for (span = first_span; span; span = span->next) {
      if (span->end - span->start < 1) {
         goto error;
      }
      if (span == first_span && span->start != 0) {
         goto error;
      }
      if (span->next && span->end != span->next->start) {
         goto error;
      }
      if (span->end > dest_len) {
         goto error;
      }
      if (span->src_idx >= 0 && span->src_idx + (span->end - span->start) > src_len) {
         goto error;
      }
      if (!span->next && span->end != dest_len) {
         goto error;
      }
      if (span->src_idx < 0) {
         goto error;
      }
   }

   i = 0;
   patch_len = 8;
   for (span = first_span; span; span = span->next) {
      i++;
      if (patch_len > INT_MAX - 2*4) {
         goto error;
      }
      patch_len += 2*4;
   }
   if (patch_len > INT_MAX - dest_len) {
      goto error;
   }
   patch_len += dest_len;
   patch = malloc(patch_len);
   if (!patch) {
      goto error;
   }

   memcpy(patch + 0, &dest_len, 4);
   value = i;
   memcpy(patch + 4, &value, 4);
   i = 8;
   for (span = first_span; span; span = span->next) {
      value = span->src_idx;
      memcpy(patch + i, &value, 4); i += 4;
      value = span->end - span->start;
      memcpy(patch + i, &value, 4); i += 4;
   }

   j = i;
   for (span = first_span; span; span = span->next) {
      memcpy(patch + j, src + span->src_idx, span->end - span->start);
      j += span->end - span->start;
   }

   for (j=0; j<dest_len; j++) {
      patch[i+j] ^= dest[j];
   }

   *patch_out = patch;
   *patch_len_out = patch_len;
   patch = NULL;
   ret = 1;

error:
   for (i=0; i<hash_size; i++) {
      entry = hash[i];
      while (entry) {
         next_entry = entry->next;
         free(entry);
         entry = next_entry;
      }
   }
   for (span = first_span; span; span = next_span) {
      next_span = span->next;
      free(span);
   }
   free(hash);
   free(window);
   free(accum_window);
   free(patch);
   return ret;
}


static int dump(unsigned char *patch, int patch_len)
{
   int i, off, cnt, result_len, result_len_header, dest_start = 0, src_start, len, min_src_len = 0, max_src_start = 0, max_len = 0;
   int cnt_decimals, dest_decimals, src_decimals, len_decimals;

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

   printf("destination size    = %d\n", result_len);

   off = 8;
   for (i=0; i<cnt; i++) {
      memcpy(&src_start, patch + off, 4); off += 4;
      memcpy(&len, patch + off, 4); off += 4;

      if (src_start < 0 || len <= 0 || src_start > INT_MAX - len) {
         goto error;
      }
      if (dest_start > result_len - len) {
         goto error;
      }

      if (src_start + len > min_src_len) {
         min_src_len = src_start + len;
      }
      if (src_start > max_src_start) {
         max_src_start = src_start;
      }
      if (len > max_len) {
         max_len = len;
      }
      dest_start += len;
   }

   if (cnt != 0) {
      if (dest_start != result_len) {
         goto error;
      }
   }

   printf("minimum source size = %d\n", min_src_len);
   printf("number of entries   = %d\n", cnt);
   if (cnt > 0) {
      printf("\n");
   }

   cnt_decimals = ceil(log10(cnt));
   dest_decimals = ceil(log10(dest_start));
   src_decimals = ceil(log10(max_src_start+1));
   len_decimals = ceil(log10(max_len+1));

   off = 8;
   dest_start = 0;
   for (i=0; i<cnt; i++) {
      memcpy(&src_start, patch + off, 4); off += 4;
      memcpy(&len, patch + off, 4); off += 4;
      printf("entry[%*d] dest=%*d src=%*d len=%*d\n", cnt_decimals, i, dest_decimals, dest_start, src_decimals, src_start, len_decimals, len);
      dest_start += len;
   }

   return 1;

error:
   return 0;
}


int main(int argc, char **argv)
{
   unsigned char *src, *dest, *patch, *target;
   int src_len, dest_len, patch_len, target_len;
   int match_size, threshold, max_len;
   int show_help = 0;

   match_size = 64;
   threshold = 1;
   max_len = 256;

   if (argc < 2) {
      show_help = 1;
   }
   else if (strcmp(argv[1], "dump") == 0) {
      if (argc < 3) {
         show_help = 1;
      }
   }
   else if (argc < 5) {
      show_help = 1;
   }

   if (show_help) {
      fprintf(stderr, "Usage: %s diff      <src> <dest> <patch>\n", argv[0]);
      fprintf(stderr, "   or: %s diff-fast <src> <dest> <patch>\n", argv[0]);
      fprintf(stderr, "   or: %s patch     <src> <patch> <dest>\n", argv[0]);
      fprintf(stderr, "   or: %s dump      <patch>\n", argv[0]);
      return 1;
   }

   if (strcmp(argv[1], "diff-fast") == 0) {
      match_size = 256;
      argv[1] = "diff";
   }

   if (strcmp(argv[1], "diff") == 0) {
      if (!read_file(argv[2], &src, &src_len)) {
         fprintf(stderr, "error: can't read source file\n");
         return 1;
      }
      if (!read_file(argv[3], &dest, &dest_len)) {
         fprintf(stderr, "error: can't read destination file\n");
         return 1;
      }

      if (!diff(src, src_len, dest, dest_len, match_size, threshold, max_len, &patch, &patch_len)) {
         fprintf(stderr, "internal error: generating of patch failed\n");
         return 1;
      }

      if (!apply(src, src_len, patch, patch_len, &target, &target_len)) {
         fprintf(stderr, "internal error: verifying of patch failed (patch)\n");
         return 1;
      }
      if (target_len != dest_len || memcmp(target, dest, target_len) != 0) {
         fprintf(stderr, "internal error: verifying of patch failed (compare)\n");
         return 1;
      }

      if (!write_file(argv[4], patch, patch_len)) {
         fprintf(stderr, "error: can't write patch file\n");
         return 1;
      }
   }
   else if (strcmp(argv[1], "patch") == 0) {
      if (!read_file(argv[2], &src, &src_len)) {
         fprintf(stderr, "error: can't read source file\n");
         return 1;
      }
      if (!read_file(argv[3], &patch, &patch_len)) {
         fprintf(stderr, "error: can't read patch file\n");
         return 1;
      }

      if (!apply(src, src_len, patch, patch_len, &dest, &dest_len)) {
         fprintf(stderr, "error: failed to apply the patch\n");
         return 1;
      }

      if (!write_file(argv[4], dest, dest_len)) {
         fprintf(stderr, "error: can't write destination file\n");
         return 1;
      }
   }
   else if (strcmp(argv[1], "dump") == 0) {
      if (!read_file(argv[2], &patch, &patch_len)) {
         fprintf(stderr, "error: can't read patch file\n");
         return 1;
      }
      if (!dump(patch, patch_len)) {
         fprintf(stderr, "error: invalid patch\n");
         return 1;
      }
   }
   else {
      fprintf(stderr, "error: unknown command '%s'\n", argv[1]);
      return 1;
   }
	return 0;
}
