LIBS=-lm

OS=$(shell uname)

ifeq ($(OS), Linux)
LIBS += -lrt
endif

all: test fixembed

test: test.c fixscript.o
	gcc -g -Wall -O3 -o test test.c fixscript.o $(LIBS)

fixembed: fixembed.c fixscript.c fixscript.h
	gcc -g -Wall -O3 -o fixembed fixembed.c $(LIBS)

fixscript.o: fixscript.c fixscript.h
	gcc -g -Wall -O3 -o fixscript.o -c fixscript.c
