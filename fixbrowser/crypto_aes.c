/*
 * FixBrowser v0.4 - https://www.fixbrowser.org/
 * Copyright (c) 2018-2025 Martin Dvorak <jezek2@advel.cz>
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

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "browser.h"

// based on tiny-AES-c (revision 61fa5c1d28d8caaf430f92d15fce1e945404508f)
// https://github.com/kokke/tiny-AES-c (released to public domain)

// GCM code based on c-crumbs (revision 740b341c97653517e61b08384a91ca2b8b9eb16c)
// https://github.com/andrebdo/c-crumbs (released to public domain using unlicense.org)
 
#define Nb 4

enum {
   TYPE_AES128,
   TYPE_AES192,
   TYPE_AES256
};

typedef struct {
   int type;
   int Nk, Nr;
   uint8_t round_key[240];
   uint8_t iv[16];
} AESState;

typedef uint8_t Matrix[4][4];

static const uint8_t sbox[256] = {
   0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
   0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
   0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
   0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
   0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
   0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
   0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
   0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
   0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
   0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
   0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
   0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
   0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
   0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
   0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
   0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
};

static const uint8_t rsbox[256] = {
   0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
   0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
   0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
   0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
   0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
   0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
   0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
   0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
   0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
   0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
   0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
   0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
   0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
   0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
   0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
   0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d
};

static const uint8_t Rcon[11] = {
   0x8d, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
};


static void key_expansion(AESState *state, uint8_t *round_key, const uint8_t *key)
{
   uint8_t tmp[4];
   int i, j, k;

   for (i=0; i<state->Nk; i++) {
      round_key[(i * 4) + 0] = key[(i * 4) + 0];
      round_key[(i * 4) + 1] = key[(i * 4) + 1];
      round_key[(i * 4) + 2] = key[(i * 4) + 2];
      round_key[(i * 4) + 3] = key[(i * 4) + 3];
   }

   for (i=state->Nk; i < Nb * (state->Nr + 1); i++) {
      k = (i-1)*4;
      tmp[0] = round_key[k+0];
      tmp[1] = round_key[k+1];
      tmp[2] = round_key[k+2];
      tmp[3] = round_key[k+3];

      if (i % state->Nk == 0) {
         // Function RotWord()
         k = tmp[0];
         tmp[0] = tmp[1];
         tmp[1] = tmp[2];
         tmp[2] = tmp[3];
         tmp[3] = k;

         // Function Subword()
         tmp[0] = sbox[tmp[0]];
         tmp[1] = sbox[tmp[1]];
         tmp[2] = sbox[tmp[2]];
         tmp[3] = sbox[tmp[3]];

         tmp[0] = tmp[0] ^ Rcon[i / state->Nk];
      }
      if (state->type == TYPE_AES256 && i % state->Nk == 4) {
         // Function Subword()
         tmp[0] = sbox[tmp[0]];
         tmp[1] = sbox[tmp[1]];
         tmp[2] = sbox[tmp[2]];
         tmp[3] = sbox[tmp[3]];
      }

      j = i*4;
      k = (i - state->Nk) * 4;
      round_key[j+0] = round_key[k+0] ^ tmp[0];
      round_key[j+1] = round_key[k+1] ^ tmp[1];
      round_key[j+2] = round_key[k+2] ^ tmp[2];
      round_key[j+3] = round_key[k+3] ^ tmp[3];
   }
}


static void AES_init_state_iv(AESState *state, int type, const uint8_t *key, const uint8_t *iv)
{
   state->type = type;
   switch (type) {
      case TYPE_AES128: state->Nk = 4; state->Nr = 10; break;
      case TYPE_AES192: state->Nk = 6; state->Nr = 12; break;
      case TYPE_AES256: state->Nk = 8; state->Nr = 14; break;
   }
   key_expansion(state, state->round_key, key);
   if (iv) {
      memcpy(state->iv, iv, 16);
   }
}


static void add_round_key(int round, Matrix *mat, uint8_t *round_key)
{
   int i, j;

   for (i=0; i<4; i++) {
      for (j=0; j<4; j++) {
         (*mat)[i][j] ^= round_key[(round * Nb * 4) + (i * Nb) + j];
      }
   }
}


static void sub_bytes(Matrix *mat)
{
   int i, j;

   for (i=0; i<4; i++) {
      for (j=0; j<4; j++) {
         (*mat)[j][i] = sbox[(*mat)[j][i]];
      }
   }
}


static void shift_rows(Matrix *mat)
{
   uint8_t tmp;

   // rotate first row 1 columns to left:
   tmp             = (*mat)[0][1];
   (*mat)[0][1] = (*mat)[1][1];
   (*mat)[1][1] = (*mat)[2][1];
   (*mat)[2][1] = (*mat)[3][1];
   (*mat)[3][1] = tmp;

   // rotate second row 2 columns to left:
   tmp             = (*mat)[0][2];
   (*mat)[0][2] = (*mat)[2][2];
   (*mat)[2][2] = tmp;

   tmp             = (*mat)[1][2];
   (*mat)[1][2] = (*mat)[3][2];
   (*mat)[3][2] = tmp;

   // rotate third row 3 columns to left:
   tmp             = (*mat)[0][3];
   (*mat)[0][3] = (*mat)[3][3];
   (*mat)[3][3] = (*mat)[2][3];
   (*mat)[2][3] = (*mat)[1][3];
   (*mat)[1][3] = tmp;
}


static uint8_t xtime(uint8_t x)
{
   return (x<<1) ^ (((x>>7) & 1) * 0x1b);
}


static void mix_columns(Matrix *mat)
{
   uint8_t tmp0, tmp1, tmp2;
   int i;

   for (i=0; i<4; i++) {
      tmp0 = (*mat)[i][0];
      tmp1 = (*mat)[i][0] ^ (*mat)[i][1] ^ (*mat)[i][2] ^ (*mat)[i][3] ;
      tmp2 = (*mat)[i][0] ^ (*mat)[i][1]; tmp2 = xtime(tmp2); (*mat)[i][0] ^= tmp2 ^ tmp1;
      tmp2 = (*mat)[i][1] ^ (*mat)[i][2]; tmp2 = xtime(tmp2); (*mat)[i][1] ^= tmp2 ^ tmp1;
      tmp2 = (*mat)[i][2] ^ (*mat)[i][3]; tmp2 = xtime(tmp2); (*mat)[i][2] ^= tmp2 ^ tmp1;
      tmp2 = (*mat)[i][3] ^ tmp0;            tmp2 = xtime(tmp2); (*mat)[i][3] ^= tmp2 ^ tmp1;
   }
}


static uint8_t multiply(uint8_t x, uint8_t y)
{
   return (((y & 1) * x) ^
          ((y>>1 & 1) * xtime(x)) ^
          ((y>>2 & 1) * xtime(xtime(x))) ^
          ((y>>3 & 1) * xtime(xtime(xtime(x)))) ^
          ((y>>4 & 1) * xtime(xtime(xtime(xtime(x))))));
}


static void inv_mix_columns(Matrix *mat)
{
   uint8_t a, b, c, d;
   int i;

   for (i=0; i<4; i++) {
      a = (*mat)[i][0];
      b = (*mat)[i][1];
      c = (*mat)[i][2];
      d = (*mat)[i][3];

      (*mat)[i][0] = multiply(a, 0x0e) ^ multiply(b, 0x0b) ^ multiply(c, 0x0d) ^ multiply(d, 0x09);
      (*mat)[i][1] = multiply(a, 0x09) ^ multiply(b, 0x0e) ^ multiply(c, 0x0b) ^ multiply(d, 0x0d);
      (*mat)[i][2] = multiply(a, 0x0d) ^ multiply(b, 0x09) ^ multiply(c, 0x0e) ^ multiply(d, 0x0b);
      (*mat)[i][3] = multiply(a, 0x0b) ^ multiply(b, 0x0d) ^ multiply(c, 0x09) ^ multiply(d, 0x0e);
   }
}


static void inv_sub_bytes(Matrix *mat)
{
   int i, j;

   for (i=0; i<4; i++) {
      for (j=0; j<4; j++) {
         (*mat)[j][i] = rsbox[(*mat)[j][i]];
      }
   }
}


static void inv_shift_rows(Matrix *mat)
{
   uint8_t tmp;

   // rotate first row 1 columns to right:
   tmp = (*mat)[3][1];
   (*mat)[3][1] = (*mat)[2][1];
   (*mat)[2][1] = (*mat)[1][1];
   (*mat)[1][1] = (*mat)[0][1];
   (*mat)[0][1] = tmp;

   // rotate second row 2 columns to right:
   tmp = (*mat)[0][2];
   (*mat)[0][2] = (*mat)[2][2];
   (*mat)[2][2] = tmp;

   tmp = (*mat)[1][2];
   (*mat)[1][2] = (*mat)[3][2];
   (*mat)[3][2] = tmp;

   // rotate third row 3 columns to right:
   tmp = (*mat)[0][3];
   (*mat)[0][3] = (*mat)[1][3];
   (*mat)[1][3] = (*mat)[2][3];
   (*mat)[2][3] = (*mat)[3][3];
   (*mat)[3][3] = tmp;
}


static void cipher(AESState *state, Matrix *mat, uint8_t *round_key)
{
   int round;

   add_round_key(0, mat, round_key);

   for (round = 1; round < state->Nr; round++) {
      sub_bytes(mat);
      shift_rows(mat);
      mix_columns(mat);
      add_round_key(round, mat, round_key);
   }

   sub_bytes(mat);
   shift_rows(mat);
   add_round_key(state->Nr, mat, round_key);
}


static void inv_cipher(AESState *state, Matrix *mat, uint8_t *round_key)
{
   int round;

   add_round_key(state->Nr, mat, round_key);

   for (round = (state->Nr - 1); round > 0; round--) {
      inv_shift_rows(mat);
      inv_sub_bytes(mat);
      add_round_key(round, mat, round_key);
      inv_mix_columns(mat);
   }

   inv_shift_rows(mat);
   inv_sub_bytes(mat);
   add_round_key(0, mat, round_key);
}


static void xor_with_iv(uint8_t *buf, uint8_t *iv)
{
   int i;
   
   for (i=0; i<16; i++) {
      buf[i] ^= iv[i];
   }
}


// Implements the AES-GCM authenticated encryption and decryption functions
// for 128-bit keys.
//
// References:
// [GCM] Recommendation for Block Cipher Modes of Operation:
//       Galois/Counter Mode (GCM) and GMAC,
//       NIST Special Publication 800-38D, November 2007
//       http://csrc.nist.gov/publications/nistpubs/800-38D/SP-800-38D.pdf


static void aes_encrypt_128(void *output, const void *input, const void *key)
{
   AESState state;

   AES_init_state_iv(&state, TYPE_AES128, key, NULL);
   memcpy(output, input, 16);
   cipher(&state, (Matrix *)output, state.round_key);
}


// Computes the multiplication of blocks X and Y and stores the result in X.
// x: pointer to 16 bytes (128 bits) of memory with X
// y: pointer to 16 bytes (128 bits) of memory with Y
//
// [GCM] 6.3 Multiplication Operation on Blocks
static void aes_gcm_mul(void *x, const void *y) {
  unsigned char z[16];
  unsigned char v[16];
  unsigned char lsb1;
  int i, j;

  /* Step 2. Z0 = 0^128 and V0 = Y */
  for (i = 0; i < 16; i++) {
    z[i] = 0;
    v[i] = ((unsigned char *)y)[i];
  }

  /*
   * Step 3. For (bit) i = 0 to 127, calculate blocks Zi+1 and Vi+1 as follows:
   * Zi+1 = Zi            if xi = 0
   * Zi+1 = Zi ^ Vi       if xi = 1
   * Vi+1 = Vi >> 1       if LSB1(Vi) = 0
   * Vi+1 = (Vi >> 1) ^ R if LSB1(Vi) = 1
   */
  for (i = 0; i < 128; i++) {
    if (((unsigned char *)x)[i >> 3] & (0x80 >> (i & 7))) {
      for (j = 0; j < 16; j++) {
        z[j] ^= v[j];
      }
    }
    lsb1 = v[15] & 1;
    for (j = 15; j > 0; j--) {
      v[j] = (v[j] >> 1) | (v[j-1] << 7);
    }
    v[0] >>= 1;
    if (lsb1) {
      v[0] ^= 0xe1; /* R = 11100001 || 0^128 */
    }
  }

  /* Step 4. Return Z_128 */
  for (i = 0; i < 16; i++) {
    ((unsigned char *)x)[i] = z[i];
  }
}


// Calculates an authentication tag.
// tag: pointer to 16 bytes (128 bits) of memory to store the calculated tag
// iv: pointer to the initialization vector (12 bytes (96 bits))
// aad: pointer to the additional authenticated data
// aad_length: number of bytes of the additional authenticated data
// text: pointer to the text (plaintext or ciphertext)
// text_length: number of bytes of the text
// key: pointer to the encryption key (16 bytes (128 bits))
//
// Used internally by the aes_gcm_encrypt and aes_gcm_decrypt functions.
// Can also be called externally to calculate just a GMAC:
// aes_gcm_tag(gmac, iv, aad, aad_length, NULL, 0, key)
//
// [GCM] 6.4 GHASH Function
// [GCM] 6.5 GCTR Function
// [GCM] 7.1 Algorithm for the Authenticated Encryption Function
static void aes_gcm_tag(void *tag, const void *iv, const void *aad, int aad_length, const void *text, int text_length, const void *key) {
  unsigned char h[16];  /* the hash subkey */
  unsigned char j0[16];  /* the pre-counter block */
  int i, j;

  /* [GCM] 7.1 Step 1. H = CIPH_K(0^128) */
  for (i = 0; i < 16; i++) {
    h[i] = 0;
  }
  aes_encrypt_128(h, h, key);

  /* [GCM] 7.1 Step 5. S = GHASH_H(A || 0^v || C || 0^u || len(A)64 || len(C)64) */
  for (i = 0; i < 16; i++) {
    ((unsigned char *)tag)[i] = 0;
  }
  for (i = 0; i < aad_length; i += 16) {
    for (j = 0; j < 16 && i + j < aad_length; j++) {
      ((unsigned char *)tag)[j] ^= ((unsigned char *)aad)[i + j];
    }
    aes_gcm_mul(tag, h);
  }
  for (i = 0; i < text_length; i += 16) {
    for (j = 0; j < 16 && i + j < text_length; j++) {
      ((unsigned char *)tag)[j] ^= ((unsigned char *)text)[i + j];
    }
    aes_gcm_mul(tag, h);
  }
  /*
  ((unsigned char *)tag)[0] ^= aad_length >> 53;
  ((unsigned char *)tag)[1] ^= aad_length >> 45;
  ((unsigned char *)tag)[2] ^= aad_length >> 37;
  */
  ((unsigned char *)tag)[3] ^= aad_length >> 29;
  ((unsigned char *)tag)[4] ^= aad_length >> 21;
  ((unsigned char *)tag)[5] ^= aad_length >> 13;
  ((unsigned char *)tag)[6] ^= aad_length >> 5;
  ((unsigned char *)tag)[7] ^= aad_length << 3;
  /*
  ((unsigned char *)tag)[8] ^= text_length >> 53;
  ((unsigned char *)tag)[9] ^= text_length >> 45;
  ((unsigned char *)tag)[10] ^= text_length >> 37;
  */
  ((unsigned char *)tag)[11] ^= text_length >> 29;
  ((unsigned char *)tag)[12] ^= text_length >> 21;
  ((unsigned char *)tag)[13] ^= text_length >> 13;
  ((unsigned char *)tag)[14] ^= text_length >> 5;
  ((unsigned char *)tag)[15] ^= text_length << 3;
  aes_gcm_mul(tag, h);

  /* [GCM] 7.1 Step 6. T = MSBt(GCTRk(J0,S)) */
  for (i = 0; i < 12; i++) {
    j0[i] = ((unsigned char *)iv)[i];
  }
  j0[12] = 0;
  j0[13] = 0;
  j0[14] = 0;
  j0[15] = 1;
  aes_encrypt_128(j0, j0, key);
  for (i = 0; i < 16; i++) {
    ((unsigned char *)tag)[i] ^= j0[i];
  }
}


// Implements the steps that are common to the encryption and decryption:
// steps 2 and 3 of the authenticated encryption function and
// steps 3 and 4 of the authenticated decryption function.
// 
// output: pointer to input_length bytes of memory to store the ciphertext/plaintext
// iv: pointer to the 12-byte (96-bit) initialization vector
// input: pointer to the plaintext/ciphertext
// input_length: number of bytes of the input
// aad: pointer to the additional authenticated data
// aad_length: number of bytes of the additional authenticated data
// key: pointer to the 16-byte (128-bit) key
//
// [GCM] 7.1 Algorithm for the Authenticated Encryption Function
// [GCM] 7.2 Algorithm for the Authenticated Decryption Function
static void aes_gcm_encrypt_or_decrypt(void *output, const void *iv, const void *input, int input_length, const void *key) {
  unsigned char cb[16];  /* the counter block CBi */
  unsigned counter;
  int i, m;

  /* J0 = IV || 0^31 || 1 */
  for (i = 0; i < 12; i++) {
    cb[i] = ((unsigned char *)iv)[i];
  }
  counter = 1;

  /* C = GCTR_K(inc32(J0), P) */
  for (m = 0; m < input_length - 16; m += 16) {
    /* [GCM] 6.5 GCTR Function, 5. For i = 2 to n, let CBi = inc32(CBi-1) */
    counter++;
    cb[12] = counter >> 24;
    cb[13] = counter >> 16;
    cb[14] = counter >> 8;
    cb[15] = counter;
    /* [GCM] 6.5 GCTR Function, 6. For i = 1 to n - 1, let Yi = Xi ^ CIPHk(CBi) */
    aes_encrypt_128((unsigned char *)output + m, cb, key);
    for (i = 0; i < 16; i++) {
      ((unsigned char *)output)[m + i] ^= ((unsigned char *)input)[m + i];
    }
  }
  /* [GCM] 6.5 GCTR Function, Step 7. Let Yn = Xn ^ MSBlen(Xn)(CIPHk(CBn)) */
  counter++;
  cb[12] = counter >> 24;
  cb[13] = counter >> 16;
  cb[14] = counter >> 8;
  cb[15] = counter;
  aes_encrypt_128(cb, cb, key);
  for (i = 0; i < input_length - m; i++) {
    ((unsigned char *)output)[m + i] = ((unsigned char *)input)[m + i] ^ cb[i];
  }
}


// Implements the AES-GCM authenticated encryption algorithm.
//
// Outputs:
// ciphertext: pointer to plaintext_length bytes of memory to store the ciphertext
// tag: pointer to 16 bytes (128 bits) of memory to store the authentication tag
//
// Inputs:
// iv: pointer to the initialization vector (12 bytes (96 bits))
// plaintext: pointer to the plaintext
// plaintext_length: number of bytes of the plaintext
// aad: pointer to the additional authenticated data
// aad_length: number of bytes of the additional authenticated data
// key: pointer to the 16-byte (128-bit) key
//
// [GCM] 7.1 Algorithm for the Authenticated Encryption Function
static void aes_gcm_encrypt(void *ciphertext, void *tag, const void *iv, const void *plaintext, int plaintext_length, const void *aad, int aad_length, const void *key) {
  /* Encrypt the plaintext */
  aes_gcm_encrypt_or_decrypt(ciphertext, iv, plaintext, plaintext_length, key);
  /* Calculate the tag */
  aes_gcm_tag(tag, iv, aad, aad_length, ciphertext, plaintext_length, key);
}

// Implements the AES-GCM authenticated decryption algorithm.
//
// Outputs:
// plaintext: pointer to ciphertext_length bytes of memory to store the plaintext
//
// Inputs:
// iv: pointer to the initialization vector (12 bytes (96 bits))
// ciphertext: pointer to the ciphertext
// ciphertext_length: number of bytes of the ciphertext
// aad: pointer to the additional authenticated data
// aad_length: number of bytes of the additional authenticated data
// tag: pointer to the authentication tag
// tag_length: number of bytes of the authentication tag
// key: pointer to the 16-byte (128-bit () key
//
// Returns 0 on success, or -1 if the verification of the tag fails.
//
// [GCM] 7.2 Algorithm for the Authenticated Decryption Function
static int aes_gcm_decrypt(void *plaintext, const void *iv, const void *ciphertext, int ciphertext_length, const void *aad, int aad_length, const void *tag, int tag_length, const void *key) {
  unsigned char t[16];  /* the calculated tag */
  int i;

  /* Check the tag */
  aes_gcm_tag(t, iv, aad, aad_length, ciphertext, ciphertext_length, key);
  for (i = 0; i < tag_length; i++) {
    if (t[i] != ((unsigned char *)tag)[i]) {
      return -1;
    }
  }

  /* Decrypt the ciphertext */
  aes_gcm_encrypt_or_decrypt(plaintext, iv, ciphertext, ciphertext_length, key);

  return 0;
}


static Value crypto_aes_init(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   uint8_t key[32];
   uint8_t iv[16];
   AESState *state;
   Value state_val;
   int err;
   int type = (int)data;
   int key_len = -1;

   switch (type) {
      case TYPE_AES128: key_len = 16; break;
      case TYPE_AES192: key_len = 24; break;
      case TYPE_AES256: key_len = 32; break;
   }

   err = fixscript_get_array_bytes(heap, params[0], 0, key_len, (char *)key);
   if (err != FIXSCRIPT_SUCCESS) return fixscript_error(heap, error, err);

   err = fixscript_get_array_bytes(heap, params[1], 0, 16, (char *)iv);
   if (err != FIXSCRIPT_SUCCESS) return fixscript_error(heap, error, err);

   state = malloc(sizeof(AESState));
   if (!state) return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);

   state_val = fixscript_create_handle(heap, HANDLE_TYPE_AES_STATE, state, free);
   if (!state_val.value) return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);

   AES_init_state_iv(state, type, key, iv);
   return state_val;
}


static Value crypto_aes_cbc(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   AESState *state;
   uint8_t buf[16], next_iv[16];
   int err;

   state = fixscript_get_handle(heap, params[0], HANDLE_TYPE_AES_STATE, NULL);
   if (!state) {
      *error = fixscript_create_error_string(heap, "invalid AES handle");
      return fixscript_int(0);
   }

   err = fixscript_get_array_bytes(heap, params[1], params[2].value, 16, (char *)buf);
   if (err != FIXSCRIPT_SUCCESS) return fixscript_error(heap, error, err);

   if ((int)data == 0) {
      xor_with_iv(buf, state->iv);
      cipher(state, (Matrix *)buf, state->round_key);
      memcpy(state->iv, buf, 16);
   }
   else {
      memcpy(next_iv, buf, 16);
      inv_cipher(state, (Matrix *)buf, state->round_key);
      xor_with_iv(buf, state->iv);
      memcpy(state->iv, next_iv, 16);
   }

   err = fixscript_set_array_bytes(heap, params[1], params[2].value, 16, (char *)buf);
   if (err != FIXSCRIPT_SUCCESS) return fixscript_error(heap, error, err);

   return fixscript_int(0);
}


static Value crypto_aes_gcm(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   int encrypt = ((int)data == 0);
   char iv[12];
   char key[16];
   char tag[16];
   char *input = NULL, *aad = NULL, *output = NULL;
   Value ret = fixscript_int(0);
   int input_len, aad_len, tag_len=0;
   int err, decrypt_ret;

   err = fixscript_get_array_bytes(heap, params[1], 0, 12, iv);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   err = fixscript_get_array_bytes(heap, params[2], 0, 16, key);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   if (!encrypt) {
      err = fixscript_get_array_length(heap, params[4], &tag_len);
      if (err) {
         fixscript_error(heap, error, err);
         goto error;
      }

      if (tag_len > 16) {
         *error = fixscript_create_error_string(heap, "tag length is bigger than 16");
         goto error;
      }

      err = fixscript_get_array_bytes(heap, params[4], 0, tag_len, tag);
      if (err) {
         fixscript_error(heap, error, err);
         goto error;
      }
   }

   err = fixscript_get_array_length(heap, params[0], &input_len);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   input = malloc(input_len);
   if (!input) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   err = fixscript_get_array_bytes(heap, params[0], 0, input_len, input);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   err = fixscript_get_array_length(heap, params[3], &aad_len);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   aad = malloc(aad_len);
   if (!aad) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   err = fixscript_get_array_bytes(heap, params[3], 0, aad_len, aad);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   output = malloc(input_len);
   if (!output) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   if (encrypt) {
      aes_gcm_encrypt(output, tag, iv, input, input_len, aad, aad_len, key);

      err = fixscript_set_array_bytes(heap, params[0], 0, input_len, output);
      if (err) {
         fixscript_error(heap, error, err);
         goto error;
      }

      err = fixscript_set_array_bytes(heap, params[4], 0, 16, tag);
      if (err) {
         fixscript_error(heap, error, err);
         goto error;
      }
   }
   else {
      decrypt_ret = aes_gcm_decrypt(output, iv, input, input_len, aad, aad_len, tag, tag_len, key);

      err = fixscript_set_array_bytes(heap, params[0], 0, input_len, output);
      if (err) {
         fixscript_error(heap, error, err);
         goto error;
      }

      ret = fixscript_int(decrypt_ret == 0);
   }

error:
   free(input);
   free(aad);
   free(output);
   return ret;
}


void register_aes_functions(Heap *heap)
{
   fixscript_register_native_func(heap, "crypto_aes128_init#2", crypto_aes_init, (void *)TYPE_AES128);
   fixscript_register_native_func(heap, "crypto_aes192_init#2", crypto_aes_init, (void *)TYPE_AES192);
   fixscript_register_native_func(heap, "crypto_aes256_init#2", crypto_aes_init, (void *)TYPE_AES256);
   fixscript_register_native_func(heap, "crypto_aes_cbc_encrypt#3", crypto_aes_cbc, (void *)0);
   fixscript_register_native_func(heap, "crypto_aes_cbc_decrypt#3", crypto_aes_cbc, (void *)1);
   fixscript_register_native_func(heap, "crypto_aes_gcm_encrypt#5", crypto_aes_gcm, (void *)0);
   fixscript_register_native_func(heap, "crypto_aes_gcm_decrypt#5", crypto_aes_gcm, (void *)1);
}
