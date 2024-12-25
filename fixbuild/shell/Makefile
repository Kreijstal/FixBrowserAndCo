CFLAGS=-Wall -O3
LIBS=-lm

OS=$(shell uname)

ifeq ($(OS), Linux)
LIBS += -lrt -lpthread -ldl
endif

all: fixscript.o fixio.o fixtask.o fiximage.o fixnative.o fixutil.o fixgui.o fixgui_gtk.o
	gcc $(CFLAGS) -o shell_linux32 shell.c fixscript.o fixio.o fixtask.o fiximage.o fixnative.o fixutil.o $(LIBS)
	gcc $(CFLAGS) -DFIXBUILD_GUI -o shell_linux32_gui shell.c fixscript.o fixio.o fixtask.o fiximage.o fixnative.o fixutil.o fixgui.o fixgui_gtk.o $(LIBS)
	chmod -x shell_linux32 shell_linux32_gui
	gzip -f -9 shell_linux32 shell_linux32_gui

fixscript.o: fixscript.c fixscript.h
	gcc $(CFLAGS) -o fixscript.o -c fixscript.c

fixio.o: fixio.c fixio.h
	gcc $(CFLAGS) -o fixio.o -c fixio.c

fixtask.o: fixtask.c fixtask.h
	gcc $(CFLAGS) -o fixtask.o -c fixtask.c

fiximage.o: fiximage.c fiximage.h
	gcc $(CFLAGS) -msse2 -mstackrealign -o fiximage.o -c fiximage.c

fixnative.o: fixnative.c fixnative.h
	gcc $(CFLAGS) -o fixnative.o -c fixnative.c

fixutil.o: fixutil.c fixutil.h
	gcc $(CFLAGS) -o fixutil.o -c fixutil.c

fixgui.o: fixgui.c fixgui.h
	gcc $(CFLAGS) -o fixgui.o -c fixgui.c

fixgui_gtk.o: fixgui_gtk.c
	gcc $(CFLAGS) -o fixgui_gtk.o -c fixgui_gtk.c
