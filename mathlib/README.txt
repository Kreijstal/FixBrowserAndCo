A tiny public domain math library
=================================

You can find the latest version as well as other libraries on the website:

  http://public-domain.advel.cz/

To use in your project, simply include mathlib.c or it's portions into your
code.


The tables generator and a reference implementation
---------------------------------------------------

The gen.fix contains a generator that computes the required tables of powers
of ten used in the float/string conversion.

The functions.fix contains a much more accurate and precise reference
implementation for comparison purposes. You need to adjust the code to test
functions you're interested in.

Both are written in FixScript available on the website:

  https://www.fixscript.org/

The code was built using the SDK 0.6, building on newer versions may need
adjustments. To build, simply run "fixbuild".
