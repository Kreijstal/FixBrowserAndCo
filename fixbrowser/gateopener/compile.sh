#!/bin/sh
set -e
gcc -DFIXSCRIPT_NO_JIT -Wall -O3 -g -o fixscript.o -c fixscript.c
gcc -Wall -O3 -o fixembed fixembed.c -lm -lrt
gcc -Wall -O3 -g -o fixio.o -c fixio.c
gcc -Wall -O3 -g -o fixtask.o -c fixtask.c
gcc -Wall -O3 -g -o fiximage.o -c fiximage.c
./fixembed -np . embed_scripts.h embed_scripts
gcc -Wall -O3 -g -o gateopener.bin gateopener.c fixscript.o fixio.o fixtask.o fiximage.o -lm -lrt -lpthread -lX11 -lXtst
strip gateopener.bin
