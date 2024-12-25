@echo off
set PATH=c:\mingw32\bin;%PATH%
gcc -g -Wall -O3 -o fixscript.o -c fixscript.c
gcc -g -Wall -O3 -o test.exe test.c fixscript.o
gcc -g -Wall -O3 -o fixembed.exe fixembed.c
