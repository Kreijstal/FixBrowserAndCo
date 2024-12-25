You can find more information about the library on the website:

  https://www.fixscript.org/

Or use the official mirror:

  http://fixscript.advel.cz/

To use in your project, simply copy these files to your project:

  fixio.c
  fixio.h
  macros.fix
  classes.fix
  object.fix
  io/
  util/

On Windows you need to link against these libraries:
  -lws2_32 -lmswsock -lwinmm

On Linux you need to link against these libraries:
  -lm -lrt -lpthread

On Mac OS X you need to link against these libraries:
  -lm -lpthread

To enable SQLite support use this compilation flag:
  -DFIXIO_SQLITE
