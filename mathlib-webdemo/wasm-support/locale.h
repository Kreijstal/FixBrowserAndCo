#ifndef LOCALE_H
#define LOCALE_H

typedef void *locale_t;

#define LC_ALL_MASK 0

locale_t newlocale(int category, const char *name, locale_t base);
void freelocale(locale_t locale);
locale_t uselocale(locale_t locale);

#endif /* LOCALE_H */
