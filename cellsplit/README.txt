You can download the latest version from the website:

  https://www.cellsplit.org/

To build you need to download FixScript SDK 0.6 from:

  https://www.fixscript.org/download

Then build it using this command:

  fixbuild

To build for other platforms use:

  fixbuild dist

Or you can build a specific target by using this command:

  fixbuild --target <target> <client|server>

Native library
==============

To build the native library you need to have a working installation of GCC
(Mingw32 on Windows). Use this command to build:

  fixbuild native

The native library contains JPG/PNG loader/writer and therefore is not required
to be built every time. It is recommended to just use the already compiled
version from the binary distribution.
