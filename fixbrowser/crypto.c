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
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "browser.h"
#include "util.h"
#include "monocypher.h"

void register_aes_functions(Heap *heap);


static inline uint32_t rol32(uint32_t a, uint32_t b)
{
   return (a << b) | (a >> (32-b));
}


static inline uint32_t ror32(uint32_t a, uint32_t b)
{
   return (a >> b) | (a << (32-b));
}


static inline uint64_t rol64(uint64_t a, uint64_t b)
{
   return (a << b) | (a >> (64-b));
}


static inline uint64_t ror64(uint64_t a, uint64_t b)
{
   return (a >> b) | (a << (64-b));
}


static Value crypto_random(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value ret = fixscript_int(0), result;
   int len, err;
#ifdef _WIN32
   HCRYPTPROV prov;
#else
   int fd = -1;
   int pos, r;
#endif
   char *tmp = NULL;

   len = fixscript_get_int(params[0]);
   if (!fixscript_is_int(params[0]) || len < 0) {
      *error = fixscript_create_error_string(heap, "length must be a positive integer");
      goto error;
   }

   tmp = malloc(len);
   if (!tmp) {
      *error = fixscript_create_error_string(heap, "out of memory");
      goto error;
   }

#ifdef _WIN32
   if (!CryptAcquireContext(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
      *error = fixscript_create_error_string(heap, "can't acquire crypto context");
      goto error;
   }
   if (!CryptGenRandom(prov, len, (BYTE *)tmp)) {
      CryptReleaseContext(prov, 0);
      *error = fixscript_create_error_string(heap, "can't get random data");
      goto error;
   }
   CryptReleaseContext(prov, 0);
#else
   fd = open("/dev/urandom", O_RDONLY);
   if (fd == -1) {
      *error = fixscript_create_error_string(heap, "can't open /dev/urandom device file");
      goto error;
   }

   pos = 0;
   while (len - pos > 0) {
      r = read(fd, tmp + pos, len - pos);
      if (r <= 0) {
         *error = fixscript_create_error_string(heap, "I/O error while reading from /dev/urandom");
         goto error;
      }
      pos += r;
   }
#endif

   result = fixscript_create_array(heap, len);
   if (!result.value) {
      *error = fixscript_create_error_string(heap, "out of memory");
      goto error;
   }

   err = fixscript_set_array_bytes(heap, result, 0, len, tmp);
   if (err != FIXSCRIPT_SUCCESS) {
      *error = fixscript_create_error_string(heap, fixscript_get_error_msg(err));
      goto error;
   }

   ret = result;

error:
#ifndef _WIN32
   if (fd != -1) {
      close(fd);
   }
#endif
   free(tmp);
   return ret;
}


static Value crypto_md5(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   static const uint32_t s[64] = {
      7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,
      5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,
      4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,
      6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21
   };
   static const uint32_t K[64] = {
      0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
      0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
      0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
      0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
      0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
      0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
      0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
      0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
      0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
      0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
      0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
      0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
      0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
      0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
      0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
      0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
   };

   Value state[4];
   unsigned char buf[64];
   uint32_t M[16], a, b, c, d, f, g;
   int i, err;

   err = fixscript_get_array_range(heap, params[0], 0, 4, state);
   if (err != FIXSCRIPT_SUCCESS) {
      return fixscript_error(heap, error, err);
   }

   err = fixscript_get_array_bytes(heap, params[1], params[2].value, 64, (char *)buf);
   if (err != FIXSCRIPT_SUCCESS) {
      return fixscript_error(heap, error, err);
   }

   for (i=0; i<16; i++) {
      M[i] = buf[i*4+0] | (buf[i*4+1] << 8) | (buf[i*4+2] << 16) | (buf[i*4+3] << 24);
   }

   a = state[0].value;
   b = state[1].value;
   c = state[2].value;
   d = state[3].value;

   for (i=0; i<16; i++) {
      f = (b & c) | ((~b) & d);
      g = i;
      f += a + K[i] + M[g];
      a = d;
      d = c;
      c = b;
      b += rol32(f, s[i]);
   }

   for (i=16; i<32; i++) {
      f = (d & b) | ((~d) & c);
      g = (5*i + 1) & 15;
      f += a + K[i] + M[g];
      a = d;
      d = c;
      c = b;
      b += rol32(f, s[i]);
   }

   for (i=32; i<48; i++) {
      f = b ^ c ^ d;
      g = (3*i + 5) & 15;
      f += a + K[i] + M[g];
      a = d;
      d = c;
      c = b;
      b += rol32(f, s[i]);
   }

   for (i=48; i<64; i++) {
      f = c ^ (b | (~d));
      g = (7*i) & 15;
      f += a + K[i] + M[g];
      a = d;
      d = c;
      c = b;
      b += rol32(f, s[i]);
   }

   state[0].value = ((uint32_t)state[0].value) + a;
   state[1].value = ((uint32_t)state[1].value) + b;
   state[2].value = ((uint32_t)state[2].value) + c;
   state[3].value = ((uint32_t)state[3].value) + d;

   err = fixscript_set_array_range(heap, params[0], 0, 4, state);
   if (err != FIXSCRIPT_SUCCESS) {
      return fixscript_error(heap, error, err);
   }
   return fixscript_int(0);
}


static Value crypto_sha1(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Value state[5];
   unsigned char buf[64];
   uint32_t w[80], a, b, c, d, e, f, tmp;
   int i, err;

   err = fixscript_get_array_range(heap, params[0], 0, 5, state);
   if (err != FIXSCRIPT_SUCCESS) {
      return fixscript_error(heap, error, err);
   }

   err = fixscript_get_array_bytes(heap, params[1], params[2].value, 64, (char *)buf);
   if (err != FIXSCRIPT_SUCCESS) {
      return fixscript_error(heap, error, err);
   }

   for (i=0; i<16; i++) {
      w[i] = (buf[i*4+0] << 24) | (buf[i*4+1] << 16) | (buf[i*4+2] << 8) | buf[i*4+3];
   }

   for (i=16; i<80; i++) {
      w[i] = rol32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
   }

   a = state[0].value;
   b = state[1].value;
   c = state[2].value;
   d = state[3].value;
   e = state[4].value;

   for (i=0; i<20; i++) {
      f = (b & c) | ((~b) & d);
      tmp = rol32(a, 5) + f + e + 0x5A827999 + w[i];
      e = d;
      d = c;
      c = rol32(b, 30);
      b = a;
      a = tmp;
   }

   for (i=20; i<40; i++) {
      f = b ^ c ^ d;
      tmp = rol32(a, 5) + f + e + 0x6ED9EBA1 + w[i];
      e = d;
      d = c;
      c = rol32(b, 30);
      b = a;
      a = tmp;
   }

   for (i=40; i<60; i++) {
      f = (b & c) | (b & d) | (c & d);
      tmp = rol32(a, 5) + f + e + 0x8F1BBCDC + w[i];
      e = d;
      d = c;
      c = rol32(b, 30);
      b = a;
      a = tmp;
   }

   for (i=60; i<80; i++) {
      f = b ^ c ^ d;
      tmp = rol32(a, 5) + f + e + 0xCA62C1D6 + w[i];
      e = d;
      d = c;
      c = rol32(b, 30);
      b = a;
      a = tmp;
   }

   state[0].value = ((uint32_t)state[0].value) + a;
   state[1].value = ((uint32_t)state[1].value) + b;
   state[2].value = ((uint32_t)state[2].value) + c;
   state[3].value = ((uint32_t)state[3].value) + d;
   state[4].value = ((uint32_t)state[4].value) + e;

   err = fixscript_set_array_range(heap, params[0], 0, 5, state);
   if (err != FIXSCRIPT_SUCCESS) {
      return fixscript_error(heap, error, err);
   }
   return fixscript_int(0);
}


static Value crypto_sha256(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   static const uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
   };
   
   Value state[8];
   unsigned char buf[64];
   uint32_t w[64], s0, s1, a, b, c, d, e, f, g, h, ch, tmp1, tmp2, maj;
   int i, err;

   err = fixscript_get_array_range(heap, params[0], 0, 8, state);
   if (err != FIXSCRIPT_SUCCESS) {
      return fixscript_error(heap, error, err);
   }

   err = fixscript_get_array_bytes(heap, params[1], params[2].value, 64, (char *)buf);
   if (err != FIXSCRIPT_SUCCESS) {
      return fixscript_error(heap, error, err);
   }

   for (i=0; i<16; i++) {
      w[i] = (buf[i*4+0] << 24) | (buf[i*4+1] << 16) | (buf[i*4+2] << 8) | buf[i*4+3];
   }

   for (i=16; i<64; i++) {
      s0 = ror32(w[i-15], 7) ^ ror32(w[i-15], 18) ^ (w[i-15] >> 3);
      s1 = ror32(w[i-2], 17) ^ ror32(w[i-2], 19) ^ (w[i-2] >> 10);
      w[i] = w[i-16] + s0 + w[i-7] + s1;
   }

   a = state[0].value;
   b = state[1].value;
   c = state[2].value;
   d = state[3].value;
   e = state[4].value;
   f = state[5].value;
   g = state[6].value;
   h = state[7].value;

   for (i=0; i<64; i++) {
      s1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
      ch = (e & f) ^ ((~e) & g);
      tmp1 = h + s1 + ch + k[i] + w[i];
      s0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
      maj = (a & b) ^ (a & c) ^ (b & c);
      tmp2 = s0 + maj;

      h = g;
      g = f;
      f = e;
      e = d + tmp1;
      d = c;
      c = b;
      b = a;
      a = tmp1 + tmp2;
   }

   state[0].value = ((uint32_t)state[0].value) + a;
   state[1].value = ((uint32_t)state[1].value) + b;
   state[2].value = ((uint32_t)state[2].value) + c;
   state[3].value = ((uint32_t)state[3].value) + d;
   state[4].value = ((uint32_t)state[4].value) + e;
   state[5].value = ((uint32_t)state[5].value) + f;
   state[6].value = ((uint32_t)state[6].value) + g;
   state[7].value = ((uint32_t)state[7].value) + h;

   err = fixscript_set_array_range(heap, params[0], 0, 8, state);
   if (err != FIXSCRIPT_SUCCESS) {
      return fixscript_error(heap, error, err);
   }
   return fixscript_int(0);
}


static Value crypto_sha512(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   static const uint64_t k[80] = {
      0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL,
      0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
      0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL, 0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
      0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
      0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL, 0x983e5152ee66dfabULL,
      0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
      0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL,
      0x53380d139d95b3dfULL, 0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
      0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
      0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL, 0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
      0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL,
      0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
      0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL, 0xca273eceea26619cULL,
      0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
      0x113f9804bef90daeULL, 0x1b710b35131c471bULL, 0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
      0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
   };
   
   Value state_values[16];
   uint64_t state[8];
   unsigned char buf[128];
   uint32_t hi, lo;
   uint64_t w[80], s0, s1, a, b, c, d, e, f, g, h, ch, tmp1, tmp2, maj;
   int i, err;

   err = fixscript_get_array_range(heap, params[0], 0, 16, state_values);
   if (err != FIXSCRIPT_SUCCESS) {
      return fixscript_error(heap, error, err);
   }

   err = fixscript_get_array_bytes(heap, params[1], params[2].value, 128, (char *)buf);
   if (err != FIXSCRIPT_SUCCESS) {
      return fixscript_error(heap, error, err);
   }

   for (i=0; i<16; i++) {
      hi = (buf[i*8+0] << 24) | (buf[i*8+1] << 16) | (buf[i*8+2] << 8) | buf[i*8+3];
      lo = (buf[i*8+4] << 24) | (buf[i*8+5] << 16) | (buf[i*8+6] << 8) | buf[i*8+7];
      w[i] = (((uint64_t)hi) << 32) | lo;
   }

   for (i=16; i<80; i++) {
      s0 = ror64(w[i-15], 1) ^ ror64(w[i-15], 8) ^ (w[i-15] >> 7);
      s1 = ror64(w[i-2], 19) ^ ror64(w[i-2], 61) ^ (w[i-2] >> 6);
      w[i] = w[i-16] + s0 + w[i-7] + s1;
   }

   for (i=0; i<8; i++) {
      state[i] = (((uint64_t)(uint32_t)state_values[i*2+0].value) << 32) | ((uint32_t)state_values[i*2+1].value);
   }

   a = state[0];
   b = state[1];
   c = state[2];
   d = state[3];
   e = state[4];
   f = state[5];
   g = state[6];
   h = state[7];

   for (i=0; i<80; i++) {
      s1 = ror64(e, 14) ^ ror64(e, 18) ^ ror64(e, 41);
      ch = (e & f) ^ ((~e) & g);
      tmp1 = h + s1 + ch + k[i] + w[i];
      s0 = ror64(a, 28) ^ ror64(a, 34) ^ ror64(a, 39);
      maj = (a & b) ^ (a & c) ^ (b & c);
      tmp2 = s0 + maj;

      h = g;
      g = f;
      f = e;
      e = d + tmp1;
      d = c;
      c = b;
      b = a;
      a = tmp1 + tmp2;
   }

   state[0] += a;
   state[1] += b;
   state[2] += c;
   state[3] += d;
   state[4] += e;
   state[5] += f;
   state[6] += g;
   state[7] += h;

   for (i=0; i<8; i++) {
      state_values[i*2+0].value = state[i] >> 32;
      state_values[i*2+1].value = state[i];
   }

   err = fixscript_set_array_range(heap, params[0], 0, 16, state_values);
   if (err != FIXSCRIPT_SUCCESS) {
      return fixscript_error(heap, error, err);
   }
   return fixscript_int(0);
}


static Value ecdh_calc_public_key(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   uint8_t secret_key[32], public_key[32];
   Value ret;
   int err;

   err = fixscript_get_array_bytes(heap, params[0], 0, 32, (char *)secret_key);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   crypto_x25519_public_key(public_key, secret_key);

   ret = fixscript_create_byte_array(heap, (char *)public_key, 32);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return ret;
}


static Value ecdh_calc_secret(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   uint8_t shared_secret[32], secret_key[32], other_public_key[32];
   Value ret;
   int err;

   err = fixscript_get_array_bytes(heap, params[0], 0, 32, (char *)secret_key);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   err = fixscript_get_array_bytes(heap, params[1], 0, 32, (char *)other_public_key);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   crypto_x25519(shared_secret, secret_key, other_public_key);

   ret = fixscript_create_byte_array(heap, (char *)shared_secret, 32);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return ret;
}


void register_crypto_functions(Heap *heap)
{
   fixscript_register_native_func(heap, "crypto_random#1", crypto_random, NULL);
   fixscript_register_native_func(heap, "crypto_md5#3", crypto_md5, NULL);
   fixscript_register_native_func(heap, "crypto_sha1#3", crypto_sha1, NULL);
   fixscript_register_native_func(heap, "crypto_sha256#3", crypto_sha256, NULL);
   fixscript_register_native_func(heap, "crypto_sha512#3", crypto_sha512, NULL);
   fixscript_register_native_func(heap, "ecdh_calc_public_key_x25519#1", ecdh_calc_public_key, NULL);
   fixscript_register_native_func(heap, "ecdh_calc_secret_x25519#2", ecdh_calc_secret, NULL);
   register_aes_functions(heap);
}
