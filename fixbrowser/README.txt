You can download the latest version from the website:

  https://www.fixbrowser.org/

To build you need to adjust and run compilation script:

  compile.bat (for Windows)
  compile.sh (for Linux)
  compile_haiku.sh (for Haiku OS)

Once compiled you can pass the -t parameter to load FixScript sources from
the files instead of using the compiled scripts in the executable.

You can pass the -p parameter to FixProxy with a port number to listen on
another port than the default 8080.

You can specify a default URL to open in FixBrowser:

  ./fixbrowser http://www.example.com/
