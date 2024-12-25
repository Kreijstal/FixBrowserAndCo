/*
 * This file was written in 2019 by Martin Dvorak <jezek2@advel.cz>
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

enum {
   GZIP_FTEXT    = 1 << 0,
   GZIP_FHCRC    = 1 << 1,
   GZIP_FEXTRA   = 1 << 2,
   GZIP_FNAME    = 1 << 3,
   GZIP_FCOMMENT = 1 << 4
};


int zcompress0_memory(const unsigned char *src, int src_len, unsigned char **dest_out, int *dest_len_out)
{
   unsigned char *dest, *p;
   int dest_len, num_blocks, amount;

   num_blocks = (src_len+65534) / 65535;
   if (num_blocks == 0) {
      num_blocks = 1;
   }
   dest_len = num_blocks * 5 + src_len;
   dest = malloc(dest_len);
   if (!dest) return 0;

   p = dest;
   do {
      if (src_len > 65535) {
         *p++ = 0;
         amount = 65535;
      }
      else {
         *p++ = 1;
         amount = src_len;
      }
      *p++ = amount;
      *p++ = amount >> 8;
      *p++ = amount ^ 0xFF;
      *p++ = (amount >> 8) ^ 0xFF;
      memcpy(p, src, amount);
      p += amount;
      src += amount;
      src_len -= amount;
   }
   while (src_len > 0);

   *dest_out = dest;
   *dest_len_out = dest_len;
   return 1;
}


int zcompress1_memory(const unsigned char *src, int src_len, unsigned char **dest_out, int *dest_len_out)
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
   const uint8_t dists[4] = { 0x00, 0x10, 0x08, 0x18 };
   const uint16_t len_base[7] = { 3, 11, 19, 35, 67, 131, 258 };

   unsigned char *out = NULL, *new_out;
   int out_len=0, out_cap=0;

   uint32_t bits = 0;
   int num_bits = 0;

   int i, j, dist;
   int best_len, best_dist=0;

   out_cap = 4096;
   out = malloc(out_cap);
   if (!out) goto error;

   PUT_BITS(1, 1); // final block
   PUT_BITS(1, 2); // fixed Huffman codes

   for (i=0; i<src_len; i++) {
      PUT_SYM(src[i]);
      best_len = 0;
      for (dist=1; dist<=4; dist++) {
         if (dist > i+1) continue;
         for (j=i+1; j<src_len && j<i+259; j++) {
            if (src[j] != src[j-dist]) break;
         }
         j--;
         if (j - i > best_len) {
            best_len = j - i;
            best_dist = dist;
         }
      }
      if (best_len >= 3) {
         PUT_LEN(best_len); // length 3..258
         PUT_BITS(dists[best_dist-1], 5); // distance 1..4
         i += best_len;
      }
   }
   PUT_SYM(256); // end of block

   // flush last byte:
   if (num_bits > 0) {
      PUT_BITS(0, 8);
   }
   
   *dest_out = out;
   *dest_len_out = out_len;
   return 1;

error:
   free(out);
   return 0;

   #undef PUT_BYTE
   #undef PUT_BITS
   #undef PUT_SYM
   #undef PUT_LEN
}


int zcompress2_memory(const unsigned char *src, int src_len, unsigned char **dest_out, int *dest_len_out)
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


int gzip_compress_memory(const unsigned char *src, int src_len, unsigned char **dest_out, int *dest_len_out)
{
   unsigned char *dest = NULL, *comp = NULL, *p;
   uint32_t crc;
   int dest_len, comp_len, retval=0;

   //if (!zcompress0_memory(src, src_len, &comp, &comp_len)) goto error;
   //if (!zcompress1_memory(src, src_len, &comp, &comp_len)) goto error;
   if (!zcompress2_memory(src, src_len, &comp, &comp_len)) goto error;

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


void gen_static_huffman()
{
   int i, j;
   uint32_t value, rev;

   for (i=0; i<288; i++) {
      if (i < 144) {
         value = 0x30 + i;
      }
      else if (i < 256) {
         value = 0x90 + (i - 144);
      }
      else if (i < 280) {
         value = (i - 256) << 1;
      }
      else {
         value = 0xC0 + (i - 280);
      }
      rev = 0;
      for (j=0; j<8; j++) {
         if (value & (1 << j)) {
            rev |= 1 << (7-j);
         }
      }
      printf(" 0x%02x,", rev);
      if ((i % 16) == 15) printf("\n");
   }
   printf("\n");

   for (i=0; i<30; i++) {
      value = i;
      rev = 0;
      for (j=0; j<5; j++) {
         if (value & (1 << j)) {
            rev |= 1 << (4-j);
         }
      }
      printf(" 0x%02x,", rev);
      if ((i % 16) == 15) printf("\n");
   }
   printf("\n");
}


int read_file(const char *fname, unsigned char **data_out, int *len_out)
{
   FILE *f = NULL;
   unsigned char *data = NULL;
   int len;

   f = fopen(fname, "rb");
   if (!f) {
      goto error;
   }

   fseek(f, 0, SEEK_END);
   len = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (len < 0) {
      goto error;
   }

   data = malloc(len);
   if (!data) {
      goto error;
   }

   if (fread(data, len, 1, f) != 1) {
      goto error;
   }

   fclose(f);
   *data_out = data;
   *len_out = len;
   return 1;

error:
   free(data);
   if (f) fclose(f);
   return 0;
}


int write_file(const char *fname, unsigned char *data, int len)
{
   FILE *f = NULL;

   f = fopen(fname, "wb");
   if (!f) {
      goto error;
   }

   if (fwrite(data, len, 1, f) != 1) {
      goto error;
   }

   fclose(f);
   return 1;

error:
   if (f) fclose(f);
   return 0;
}


int main(int argc, char **argv)
{
   int compress;
   int ilen, olen;
   unsigned char *idata=NULL, *odata=NULL;

   if (argc < 4) {
      printf("Usage: %s [-c|-d] <infile> <outfile>\n", argv[0]);
      printf("  -c  compress to GZIP file\n");
      printf("  -d  decompress from GZIP file\n");
      return 0;
   }

   if (strcmp(argv[1], "-c") == 0) {
      compress = 1;
   }
   else if (strcmp(argv[1], "-d") == 0) {
      compress = 0;
   }
   else {
      fprintf(stderr, "first parameter must be -c or -d\n");
      return 1;
   }

   if (!read_file(argv[2], &idata, &ilen)) {
      fprintf(stderr, "can't read file '%s'\n", argv[2]);
      return 1;
   }
   if (compress) {
      if (!gzip_compress_memory(idata, ilen, &odata, &olen)) {
         free(idata);
         fprintf(stderr, "compression error\n");
         return 1;
      }
   }
   else {
      if (!gzip_uncompress_memory(idata, ilen, &odata, &olen)) {
         free(idata);
         fprintf(stderr, "decompression error\n");
         return 1;
      }
   }
   free(idata);
   if (!write_file(argv[3], odata, olen)) {
      free(odata);
      fprintf(stderr, "can't write file '%s'\n", argv[3]);
      return 1;
   }
   free(odata);
   return 0;
}
