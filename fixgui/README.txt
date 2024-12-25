You can find more information about the library on the website:

  https://www.fixscript.org/

Or use the official mirror:

  http://fixscript.advel.cz/

To use in your project, simply copy these files to your project:

  fixgui.c
  fixgui.h
  fixgui_common.h
  fixgui_win32.c
  fixgui_cocoa.c
  fixgui_gtk.c
  fixgui_haiku.c
  macros.fix
  classes.fix
  object.fix
  optional.fix
  gui/

Compile fiximage.c with these flags:
  -msse2 -mstackrealign

On Windows you need to link against these libraries:
  -lgdi32 -lcomdlg32 -lshlwapi -lcomctl32 -lwinmm -mwindows

On Mac OS X you need to link against these libraries:
  -lm -lobjc -lpthread -framework Cocoa
