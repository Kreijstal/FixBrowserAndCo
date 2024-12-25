#ifndef STDLIB_H
#define STDLIB_H

#include <locale.h>

#define NULL ((void *)0)

typedef unsigned long size_t;

void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

double strtod(const char *nptr, char **endptr);
double strtod_l(const char *nptr, char **endptr, locale_t locale);
long int strtol(const char *nptr, char **endptr, int base);
long long int strtoll(const char *nptr, char **endptr, int base);
unsigned long int strtoul(const char *nptr, char **endptr, int base);
int atoi(const char *nptr);

void qsort(void *base, size_t nmemb, size_t size, int(*compar)(const void *, const void *));

int abs(int j);

void abort();

#endif /* STDLIB_H */
