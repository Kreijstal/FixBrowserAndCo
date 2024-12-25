#ifndef STDIO_H
#define STDIO_H

#include <stdlib.h>
#include <stdarg.h>

typedef void FILE;

#define stderr ((FILE *)NULL)

int fprintf(FILE *stream, const char *format, ...);
int sprintf(char *str, const char *format, ...);
int snprintf(char *str, size_t size, const char *format, ...);
int vsnprintf(char *str, size_t size, const char *format, va_list ap);

FILE *fopen(const char *path, const char *mode);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int fflush(FILE *stream);
int ferror(FILE *stream);
int fclose(FILE *fp);

int fputc(int c, FILE *stream);
int fputs(const char *s, FILE *stream);

#endif /* STDIO_H */
