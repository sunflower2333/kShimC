#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <bfdev.h>

/* Alias to stdc functions of bfdev */
int vsnprintf(char *buffer, size_t size, const char *format, va_list args);
void *memset(void *s, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);
void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
// int printf(const char *fmt, ...);
#define printf(...) bfdev_log_info(__VA_ARGS__)
#define printf_e(...) bfdev_log_emerg(__VA_ARGS__)
