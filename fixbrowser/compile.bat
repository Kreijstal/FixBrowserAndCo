@echo off
set PATH=c:\mingw32\bin;%PATH%
windres browser.rc browser_rc.o || exit /b
gcc -DFIXSCRIPT_NO_JIT -Wall -O3 -g -o fixscript.o -c fixscript.c || exit /b
gcc -Wall -O3 -o fixembed.exe fixembed.c || exit /b
gcc -Wall -O3 -o gencharsets.exe gencharsets.c || exit /b
gcc -Wall -O3 -g -o monocypher.o -c monocypher.c || exit /b
gcc -Wall -O3 -g -o crypto_aes.o -c crypto_aes.c || exit /b
gcc -Wall -O3 -g -o fixio.o -c fixio.c || exit /b
gcc -Wall -O3 -g -msse2 -mstackrealign -o fiximage.o -c fiximage.c || exit /b
gcc -Wall -O3 -g -o fixgui.o -c fixgui.c || exit /b
gcc -Wall -O3 -g -o fixgui_win32.o -c fixgui_win32.c || exit /b
gcc -Wall -O3 -g -o fixtask.o -c fixtask.c || exit /b
fixembed.exe -ex scripts . embed_scripts.h embed_scripts || exit /b
fixembed.exe -bin res embed_resources.h embed_resources || exit /b
fixembed.exe -bin res_proxy embed_resources_proxy.h embed_resources_proxy || exit /b
gencharsets.exe charsets embed_charsets.h embed_charsets || exit /b
gcc -Wall -O3 -g -o fixbrowser.exe bigint.c crypto.c browser.c script.c util.c image.c css.c fixscript.o crypto_aes.o monocypher.o fixio.o fiximage.o fixgui.o fixgui_win32.o fixtask.o browser_rc.o -lgdi32 -lcomctl32 -lwinmm -lws2_32 -lmswsock -mwindows || exit /b
gcc -Wall -O3 -g -o fixproxy.exe bigint.c crypto.c proxy.c script.c util.c css.c fixscript.o crypto_aes.o monocypher.o fixio.o fixtask.o -lwinmm -lws2_32 -lmswsock -mconsole || exit /b
strip fixbrowser.exe || exit /b
strip fixproxy.exe || exit /b
