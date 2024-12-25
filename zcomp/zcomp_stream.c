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

enum {
   COMP_ERROR = -1,
   COMP_DONE  = 0,
   COMP_FLUSH = 1,
   COMP_MORE  = 2
};

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


int zcompress(ZCompress *st)
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

int zuncompress(ZUncompress *st)
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


int gzip_compress(GZipCompress *st)
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


int gzip_uncompress(GZipUncompress *st)
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


int main(int argc, char **argv)
{
   FILE *fin, *fout;
   unsigned char in[4096], out[4096];
   int read;
   int result;
   int remaining;
   int compress, flush=0;

   if (argc < 4) {
      printf("Usage: %s [-c|-d] <infile> <outfile>\n", argv[0]);
      printf("  -c  compress to GZIP file\n");
      printf("  -f  compress to GZIP file and flush after every block\n");
      printf("  -d  decompress from GZIP file\n");
      return 0;
   }

   if (strcmp(argv[1], "-c") == 0) {
      compress = 1;
   }
   else if (strcmp(argv[1], "-f") == 0) {
      compress = 1;
      flush = 1;
   }
   else if (strcmp(argv[1], "-d") == 0) {
      compress = 0;
   }
   else {
      fprintf(stderr, "first parameter must be -c or -d\n");
      return 1;
   }

   fin = fopen(argv[2], "rb");
   if (!fin) {
      fprintf(stderr, "can't read file '%s'\n", argv[2]);
      return 1;
   }

   fout = fopen(argv[3], "wb");
   if (!fout) {
      fclose(fin);
      fprintf(stderr, "can't write file '%s'\n", argv[3]);
      return 1;
   }

   if (compress) {
      GZipCompress *gzip = calloc(1, sizeof(GZipCompress));
      gzip->z.dest = out;
      gzip->z.dest_end = out + sizeof(out);
      gzip->z.flushable = flush;

      for (;;) {
         result = gzip_compress(gzip);
         if (result < 0) {
            free(gzip);
            fclose(fin);
            fclose(fout);
            fprintf(stderr, "compression error\n");
            return 1;
         }
         if (result == COMP_DONE) break;

         if (result == COMP_FLUSH) {
            if (gzip->z.dest - out > 0) {
               if (fwrite(out, gzip->z.dest - out, 1, fout) != 1) {
                  free(gzip);
                  fclose(fin);
                  fclose(fout);
                  fprintf(stderr, "can't write file '%s'\n", argv[3]);
                  return 1;
               }
            }
            gzip->z.dest = out;
            gzip->z.dest_end = out + sizeof(out);
         }
         else {
            remaining = gzip->z.src_end - gzip->z.src;
            memmove(in, gzip->z.src, remaining);
            if (sizeof(in) - remaining > 100) {
               read = fread(in + remaining, 1, 100, fin);
            }
            else {
               read = fread(in + remaining, 1, sizeof(in) - remaining, fin);
            }
            if (ferror(fin)) {
               free(gzip);
               fclose(fin);
               fclose(fout);
               fprintf(stderr, "can't read file '%s'\n", argv[2]);
               return 1;
            }
            gzip->z.src = in;
            gzip->z.src_end = in + remaining + read;
            gzip->z.src_flush = flush;
            gzip->z.src_final = feof(fin);
         }
      }

      free(gzip);
   }
   else {
      GZipUncompress *gunzip = calloc(1, sizeof(GZipUncompress));
      gunzip->z.dest = out;
      gunzip->z.dest_end = out + sizeof(out);

      for (;;) {
         result = gzip_uncompress(gunzip);
         if (result < 0) {
            free(gunzip);
            fclose(fin);
            fclose(fout);
            fprintf(stderr, "decompression error\n");
            return 1;
         }
         if (result == COMP_DONE) break;

         if (result == COMP_FLUSH) {
            if (gunzip->z.dest - out > 0) {
               if (fwrite(out, gunzip->z.dest - out, 1, fout) != 1) {
                  free(gunzip);
                  fclose(fin);
                  fclose(fout);
                  fprintf(stderr, "can't write file '%s'\n", argv[3]);
                  return 1;
               }
            }
            gunzip->z.dest = out;
            gunzip->z.dest_end = out + sizeof(out);
         }
         else {
            remaining = gunzip->z.src_end - gunzip->z.src;
            memmove(in, gunzip->z.src, remaining);
            if (sizeof(in) - remaining > 100) {
               read = fread(in + remaining, 1, 100, fin);
            }
            else {
               read = fread(in + remaining, 1, sizeof(in) - remaining, fin);
            }
            if (ferror(fin)) {
               free(gunzip);
               fclose(fin);
               fclose(fout);
               fprintf(stderr, "can't read file '%s'\n", argv[2]);
               return 1;
            }
            gunzip->z.src = in;
            gunzip->z.src_end = in + remaining + read;
         }
      }

      free(gunzip);
   }

   fclose(fin);
   fclose(fout);
   return 0;
}
