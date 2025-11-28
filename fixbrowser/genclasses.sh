#!/bin/sh
set -e
WORKDIR=scripts/__classes
mkdir $WORKDIR
cp scripts/classes.src.fix $WORKDIR/classes.fix
cp scripts/{macros,object}.fix $WORKDIR/
(cd $WORKDIR && ../../fixembed -nc . embed_scripts.h embed_scripts)
cat > $WORKDIR/extract.c <<EOF
#include <stdio.h>
#include <string.h>
#include "embed_scripts.h"

int main(int argc, char **argv)
{
	const char *src;
	int i=0;
	for (;;) {
		if (!embed_scripts[i]) break;
		if (strcmp(embed_scripts[i], "classes.fix") == 0) {
			src = strdup(embed_scripts[i+1]);
			src += 17;
			while (*src == '\n') src++;
			*(strrchr(src, '\n')+1) = '\0';
			printf("// THIS FILE WAS AUTOMATICALLY GENERATED FROM \"classes.src.fix\", DO NOT EDIT!\n\n%s", src);
			return 0;
		}
		i += 2;
	}
	return 1;
}
EOF
gcc -o $WORKDIR/extract $WORKDIR/extract.c
$WORKDIR/extract > scripts/classes.fix
rm $WORKDIR/*
rmdir $WORKDIR
