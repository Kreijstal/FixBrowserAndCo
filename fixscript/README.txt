You can find more information about the language on the website:

  https://www.fixscript.org/

Or use the official mirror:

  http://fixscript.advel.cz/

To use in your project, simply copy fixscript.c and fixscript.h to your
project.

The 'wasm-test.html' file is released into public domain under the CC0 license.


WebAssembly
-----------

The test scripts must be made as part of the resulting binary:

  fixembed -np . test_scripts.h test_scripts

Then compile it using the Clang compiler like so:

  clang -Wall -Os -Iwasm-support --target=wasm32 -mcpu=mvp -nostdlib
        -Wl,-z,stack-size=262144 -Wl,--no-entry -Wl,--export-table -Wl,--strip-all \
        -o wasm-test.wasm test.c wasm-support.c fixscript.c

This compiles it with a minimal subset of the libc library providing just the
functions used by FixScript. The wasm-test.html provides an example how to
interact with it and also the implementation of the required imported functions.
