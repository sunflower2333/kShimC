/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <pl011_uart.h>
#include <kshim_console.h>
#include <kshim/irq.h>
#include <config.h>

extern char __heap_start[];
extern char __heap_end[];

static char *heap_break = __heap_start;

static int picoport_putc(char character, FILE *stream)
{
    (void)stream;
    kshim_console_write(&character, 1);
    return (unsigned char)character;
}

static FILE picoport_stdio =
    FDEV_SETUP_STREAM(picoport_putc, NULL, NULL, _FDEV_SETUP_WRITE);

FILE *const stdout = &picoport_stdio;
FILE *const stderr = &picoport_stdio;

__attribute__((noreturn)) void _exit(int status)
{
    (void)status;
    char message[] = "kShimC: runtime aborted\n";
    kshim_console_write(message, sizeof(message) - 1);
    __asm__ volatile("msr daifset, #0xf" ::: "memory");
    for (;;)
        __asm__ volatile("wfe" ::: "memory");
}

void *sbrk(ptrdiff_t increment)
{
    char *previous_break = heap_break;

    if (increment < 0) {
        if ((size_t)(heap_break - __heap_start) < (size_t)(-increment)) {
            errno = ENOMEM;
            return (void *)-1;
        }
    } else if ((size_t)(__heap_end - heap_break) < (size_t)increment) {
        errno = ENOMEM;
        return (void *)-1;
    }

    heap_break += increment;
    return previous_break;
}

/* Picolibc is built single-threaded, so its allocator takes no lock. The
 * link wraps the allocator entry points (CMakeLists.txt) so a handler that
 * allocates cannot interleave with an allocation it interrupted. */
void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *pointer, size_t size);
void __real_free(void *pointer);

void *__wrap_malloc(size_t size)
{
    unsigned long flags = kshim_irq_save();
    void *pointer = __real_malloc(size);
    kshim_irq_restore(flags);
    return pointer;
}

void *__wrap_calloc(size_t count, size_t size)
{
    unsigned long flags = kshim_irq_save();
    void *pointer = __real_calloc(count, size);
    kshim_irq_restore(flags);
    return pointer;
}

void *__wrap_realloc(void *pointer, size_t size)
{
    unsigned long flags = kshim_irq_save();
    void *result = __real_realloc(pointer, size);
    kshim_irq_restore(flags);
    return result;
}

void __wrap_free(void *pointer)
{
    unsigned long flags = kshim_irq_save();
    __real_free(pointer);
    kshim_irq_restore(flags);
}
