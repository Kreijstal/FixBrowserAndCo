CFLAGS=-Wall -O3
LIBS=-lm

OS=$(shell uname)

ifeq ($(OS), Linux)
LIBS += -lrt
endif

CLANG=clang-14
WASM_CFLAGS=-Wall -Os -Iwasm-support --target=wasm32 -mcpu=mvp
WASM_LIBS=-nostdlib -Wl,-z,stack-size=262144 -Wl,--no-entry -Wl,--export-table -Wl,--strip-all

.PHONY: scripts

all: scripts webdemo.wasm

scripts: fixembed
	./fixembed -ex classes.fix -ex macros.fix scripts scripts.h scripts

fixembed: fixembed.c fixscript.c fixscript.h
	gcc $(CFLAGS) -o fixembed fixembed.c $(LIBS)

webdemo.wasm: main.c scripts.h fixscript.o wasm-support.o
	$(CLANG) $(WASM_CFLAGS) -o webdemo.wasm main.c fixscript.o wasm-support.o $(WASM_LIBS)
	@chmod -x webdemo.wasm

fixscript.o: fixscript.c fixscript.h
	$(CLANG) $(WASM_CFLAGS) -o fixscript.o -c fixscript.c

wasm-support.o: wasm-support.c
	$(CLANG) $(WASM_CFLAGS) -o wasm-support.o -c wasm-support.c
