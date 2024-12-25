#!/bin/sh
set -e
gcc -DFIXBUILD_BINCOMPAT -DFIXSCRIPT_NO_JIT -Wall -O3 -g -o fixscript.o -c fixscript.c
gcc -Wall -O3 -o fixembed fixembed.c -lm -lrt
gcc -Wall -O3 -o gencharsets gencharsets.c
gcc -Wall -O3 -g -o monocypher.o -c monocypher.c
gcc -Wall -O3 -g -o crypto_aes.o -c crypto_aes.c
gcc -DFIXBUILD_BINCOMPAT -Wall -O3 -g -o fixio.o -c fixio.c
gcc -DFIXBUILD_BINCOMPAT -Wall -O3 -g -msse2 -mstackrealign -o fiximage.o -c fiximage.c
gcc -Wall -O3 -g -o fixgui.o -c fixgui.c
gcc -Wall -O3 -g -o fixgui_gtk.o -c fixgui_gtk.c
gcc -Wall -O3 -g -o fixtask.o -c fixtask.c
./fixembed -ex scripts . embed_scripts.h embed_scripts
./fixembed -bin res embed_resources.h embed_resources
./fixembed -bin res_proxy embed_resources_proxy.h embed_resources_proxy
./gencharsets charsets embed_charsets.h embed_charsets
gcc -Wall -O3 -g -o fixbrowser bigint.c crypto.c browser.c script.c util.c image.c css.c fixscript.o crypto_aes.o monocypher.o fixio.o fiximage.o fixgui.o fixgui_gtk.o fixtask.o -lm -lrt -lpthread -ldl
gcc -Wall -O3 -g -o fixproxy bigint.c crypto.c proxy.c script.c util.c css.c fixscript.o crypto_aes.o monocypher.o fixio.o fixtask.o -lm -lrt -lpthread
strip fixbrowser
strip fixproxy
