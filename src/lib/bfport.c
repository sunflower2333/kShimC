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
