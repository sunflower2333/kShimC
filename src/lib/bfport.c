#include <lib/bfport.h>
// Implement nessesary functions for bfdev
uint8_t HEAP_BASE[HEAP_SIZE] = {0};

static bfdev_memalloc_head_t *bfport_mempool = (bfdev_memalloc_head_t *)HEAP_BASE;

void *init_bfdev_heap()
{
    /*
     *    Heap layout:
     *    |-----------------------|
     *    | bfdev_memalloc_head_t |  <- heap header
     *    |-----------------------|
     *    |          pool         |  <- heap memory pool
     *    |-----------------------|
     */
    void *memory;
    if (!bfport_mempool)
        return BFDEV_ERR_PTR(-12);
    memory = (void *)bfport_mempool + sizeof(*bfport_mempool);
    bfdev_memalloc_init(bfport_mempool, bfdev_memalloc_first_fit, memory, HEAP_SIZE);
}

__bfdev_malloc void *
bfport_malloc(bfdev_size_t size)
{
    void *ptr = bfdev_memalloc_alloc(bfport_mempool, size);
    if (!ptr)
    {
        return BFDEV_NULL;
    }
    return ptr;
}

__bfdev_malloc void *
bfport_calloc(bfdev_size_t nmemb, bfdev_size_t size)
{
    void *ptr = bfport_malloc(nmemb * size);
    if (!ptr)
    {
        return BFDEV_NULL;
    }
    /* Initialize with 0 */
    bfdev_memset(ptr, 0, nmemb * size);
    return ptr;
}

void bfport_free(void *ptr)
{
    if (!ptr)
    {
        return;
    }
    bfdev_memalloc_free(bfport_mempool, ptr);
}

__bfdev_malloc void *
bfport_realloc(void *ptr, bfdev_size_t size)
{
    if (!ptr)
    {
        return bfport_malloc(size);
    }
    void *new_ptr = bfport_malloc(size);
    if (!new_ptr)
    {
        return BFDEV_NULL;
    }
    bfdev_memcpy(new_ptr, ptr, size);
    /* Free old pointer */
    bfport_free(ptr);
    return new_ptr;
}


static char *itoa(int value, char *buffer, int base)
{
    char *ptr = buffer;
    char *ptr1 = buffer;
    char tmp_char;
    int tmp_value;

    if (value < 0 && base == 10)
    {
        *ptr++ = '-';
        value = -value;
    }

    do
    {
        tmp_value = value;
        value /= base;
        *ptr++ = "0123456789abcdef"[tmp_value - value * base];
    } while (value);

    *ptr-- = '\0';

    while (ptr1 < ptr)
    {
        tmp_char = *ptr;
        *ptr = *ptr1;
        *ptr1 = tmp_char;
        ptr--;
        ptr1++;
    }

    return buffer;
}

static size_t format_integer(char *buf, size_t remaining, long value, int base, 
                           int width, int zero_pad)
{
    char tmp_buffer[32];
    char *buf_start = buf;
    itoa(value, tmp_buffer, base);
    
    int len = 0;
    for (char *p = tmp_buffer; *p; p++) 
        len++;
    
    // Add padding if needed
    if (width > len && zero_pad) {
        int pad_len = width - len;
        for (int i = 0; i < pad_len && remaining > 1; i++) {
            *buf++ = '0';
            remaining--;
        }
    }
    
    // Copy the number
    for (char *p = tmp_buffer; *p && remaining > 1; p++) {
        *buf++ = *p;
        remaining--;
    }
    
    return buf - buf_start;
}

int vsnprintf(char *buffer, size_t size, const char *format, va_list args)
{
    if (!buffer || !format || size == 0)
        return -1;

    char *buf_ptr = buffer;
    const char *fmt_ptr = format;
    size_t remaining = size;

    while (*fmt_ptr && remaining > 1)
    {
        if (*fmt_ptr != '%') {
            *buf_ptr++ = *fmt_ptr++;
            remaining--;
            continue;
        }

        // Handle format specifier
        fmt_ptr++; // Skip '%'
        
        // Parse format options
        int zero_pad = (*fmt_ptr == '0');
        if (zero_pad) fmt_ptr++;
        
        // Parse width
        int width = 0;
        while (*fmt_ptr >= '0' && *fmt_ptr <= '9') {
            width = width * 10 + (*fmt_ptr - '0');
            fmt_ptr++;
        }
        
        // Parse length modifier
        int is_long = (*fmt_ptr == 'l');
        if (is_long) fmt_ptr++;

        // Handle format specifier
        switch (*fmt_ptr) {
            case 'd': {
                long value = is_long ? va_arg(args, long) : va_arg(args, int);
                buf_ptr += format_integer(buf_ptr, remaining, value, 10, width, zero_pad);
                remaining = size - (buf_ptr - buffer);
                break;
            }
                
            case 'u': {
                unsigned long value = is_long ? va_arg(args, unsigned long) : va_arg(args, unsigned int);
                buf_ptr += format_integer(buf_ptr, remaining, value, 10, width, zero_pad);
                remaining = size - (buf_ptr - buffer);
                
                // Handle special case for "%lu.%lus" format
                if (is_long && fmt_ptr[1] == '.' && fmt_ptr[2] == '%' && 
                    fmt_ptr[3] == 'l' && fmt_ptr[4] == 'u' && fmt_ptr[5] == 's') {
                    if (remaining > 1) {
                        *buf_ptr++ = '.';
                        remaining--;
                    }
                    
                    unsigned long sec_value = va_arg(args, unsigned long);
                    buf_ptr += format_integer(buf_ptr, remaining, sec_value, 10, 0, 0);
                    remaining = size - (buf_ptr - buffer);
                    
                    if (remaining > 1) {
                        *buf_ptr++ = 's';
                        remaining--;
                    }
                    
                    fmt_ptr += 5;  // Skip ".%lus"
                }
                break;
            }
                
            case 'x': {
                unsigned long value = is_long ? va_arg(args, unsigned long) : va_arg(args, unsigned int);
                buf_ptr += format_integer(buf_ptr, remaining, value, 16, width, zero_pad);
                remaining = size - (buf_ptr - buffer);
                break;
            }
                
            case 's': {
                char *str = va_arg(args, char *);
                if (!str) str = "(null)";
                while (*str && remaining > 1) {
                    *buf_ptr++ = *str++;
                    remaining--;
                }
                break;
            }
                
            case '%': {
                if (remaining > 1) {
                    *buf_ptr++ = '%';
                    remaining--;
                }
                break;
            }
                
            default: {
                // Unknown format, output as-is with modifiers
                if (remaining > 1) {
                    *buf_ptr++ = '%';
                    remaining--;
                }
                
                if (zero_pad && width > 0 && remaining > 1) {
                    *buf_ptr++ = '0';
                    remaining--;
                    
                    char width_str[10];
                    itoa(width, width_str, 10);
                    for (char *p = width_str; *p && remaining > 1; p++) {
                        *buf_ptr++ = *p;
                        remaining--;
                    }
                }
                
                if (is_long && remaining > 1) {
                    *buf_ptr++ = 'l';
                    remaining--;
                }
                
                if (remaining > 1) {
                    *buf_ptr++ = *fmt_ptr;
                    remaining--;
                }
            }
        }
        
        fmt_ptr++;
    }

    // Ensure null termination
    if (remaining > 0)
        *buf_ptr = '\0';
    else if (size > 0)
        buffer[size - 1] = '\0';

    return buf_ptr - buffer;
}

int bfport_vsnprintf(char *s, bfdev_size_t maxlen, const char *fmt, bfdev_va_list arg)
{
    return vsnprintf(s, maxlen, fmt, arg);
}

int bfport_log_write(bfdev_log_message_t *msg)
{
    // geni_uart_write(0, msg->buff, msg->length);
    pl011_write(0x9000000, msg->buff, msg->length);
    return 0;
}

/* Alias */
void *memset(void *s, int c, size_t n) { return bfdev_memset(s, c, n); }
void *memcpy(void *dest, const void *src, size_t n) { return bfdev_memcpy(dest, src, n); }
int memcmp(const void *s1, const void *s2, size_t n) { return bfdev_memcmp(s1, s2, n); }
void *malloc(size_t size) { return bfport_malloc(size); }
void *calloc(size_t nmemb, size_t size) { return bfport_calloc(nmemb, size); }
void *realloc(void *ptr, size_t size) { return bfport_realloc(ptr, size); }
void free(void *ptr) { bfport_free(ptr); }
