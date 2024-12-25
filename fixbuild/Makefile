CFLAGS=-Wall -O3
LIBS=-lm

OS=$(shell uname)

ifeq ($(OS), Linux)
LIBS += -lrt -lpthread -ldl
endif

all: fixembed
	cd shell && make
	./fixembed -nc classes classes.h classes_script
	./fixembed -nc build build.h build_scripts
	./fixembed -bin common scripts.h common_scripts
	./fixembed -bin bin binaries.h exec_binaries
	./fixembed -bin icon icon.h icon_files
	gcc $(CFLAGS) -o fixbuild fixbuild.c shell/fixio.o shell/fixtask.o shell/fiximage.o shell/fixnative.o shell/fixutil.o $(LIBS)

fixembed: fixembed.c
	gcc $(CFLAGS) -o fixembed fixembed.c $(LIBS)
