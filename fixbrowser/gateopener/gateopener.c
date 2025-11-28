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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <X11/extensions/XTest.h>
#include "fixio.h"
#include "fixtask.h"
#include "fiximage.h"
#include "embed_scripts.h"

static int test_scripts = 0;
static Display *display = NULL;


static Value native_mouse_connect(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   if (display) {
      *error = fixscript_create_error_string(heap, "already connected");
      return fixscript_int(0);
   }

   display = XOpenDisplay(NULL);
   if (!display) {
      *error = fixscript_create_error_string(heap, "connect failed");
   }
   return fixscript_int(0);
}


static Value native_mouse_disconnect(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   if (!display) {
      *error = fixscript_create_error_string(heap, "not connected");
      return fixscript_int(0);
   }

   XCloseDisplay(display);
   display = NULL;
   return fixscript_int(0);
}


static Value native_mouse_move_event(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   if (!XTestFakeMotionEvent(display, -1, params[0].value, params[1].value, CurrentTime)) {
      *error = fixscript_create_error_string(heap, "request failed");
   }
   XFlush(display);
   return fixscript_int(0);
}


static Value native_mouse_button_event(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   if (!XTestFakeButtonEvent(display, params[0].value, params[1].value, CurrentTime)) {
      *error = fixscript_create_error_string(heap, "request failed");
   }
   XFlush(display);
   return fixscript_int(0);
}


static inline uint32_t ror32(uint32_t a, uint32_t b)
{
   return (a >> b) | (a << (32-b));
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


static Script *load_script(Heap *heap, const char *fname, Value *error, void *data)
{
   if (test_scripts) {
      return fixscript_load_file(heap, fname, error, ".");
   }
   else {
      return fixscript_load_embed(heap, fname, error, embed_scripts);
   }
}


static Heap *create_heap(void *data)
{
   Heap *heap;

   heap = fixscript_create_heap();
   fixio_register_functions(heap);
   fixtask_register_functions(heap, create_heap, NULL, load_script, NULL);
   fiximage_register_functions(heap);
   fixscript_register_native_func(heap, "native_mouse_connect#0", native_mouse_connect, NULL);
   fixscript_register_native_func(heap, "native_mouse_disconnect#0", native_mouse_disconnect, NULL);
   fixscript_register_native_func(heap, "native_mouse_move_event#2", native_mouse_move_event, NULL);
   fixscript_register_native_func(heap, "native_mouse_button_event#2", native_mouse_button_event, NULL);
   fixscript_register_native_func(heap, "crypto_sha256#3", crypto_sha256, NULL);
   return heap;
}


int main(int argc, char **argv)
{
   Heap *heap;
   Script *script;
   Value error;
   int i;

   signal(SIGPIPE, SIG_IGN);
   
   for (i=1; i<argc; i++) {
      if (strcmp(argv[i], "-t") == 0) {
         test_scripts = 1;
      }
   }

   heap = create_heap(NULL);

   script = load_script(heap, "gateopener/main", &error, NULL);
   if (!script) {
      fprintf(stderr, "%s\n", fixscript_get_compiler_error(heap, error));
      fflush(stderr);
      return 1;
   }
   
   fixscript_run(heap, script, "main#0", &error);
   if (error.value) {
      fixscript_dump_value(heap, error, 1);
      return 1;
   }

   return 0;
}
