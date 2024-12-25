You can find more information about the library on the website:

  https://www.fixscript.org/

Or use the official mirror:

  http://fixscript.advel.cz/

To use in your project, simply copy these files to your project:

  fiximage.c
  fiximage.h
  macros.fix
  classes.fix
  object.fix
  image/

Compile with these flags:
  -msse2 -mstackrealign

On Linux you need to link against these libraries:
  -lm -lrt -lpthread

On Mac OS X you need to link against these libraries:
  -lm -lpthread
