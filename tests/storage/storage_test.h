/* SPDX-License-Identifier: MIT */
#pragma once
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <kshim/block.h>

#define SKIP_CODE 77

static int test_failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            test_failures++;                                                     \
        }                                                                        \
    } while (0)

#define CHECK_EQ(actual, expected)                                               \
    do {                                                                         \
        long long a_ = (long long)(actual), e_ = (long long)(expected);          \
        if (a_ != e_) {                                                          \
            fprintf(stderr, "%s:%d: %s = %lld (%s), expected %lld (%s)\n",       \
                    __FILE__, __LINE__, #actual, a_,                             \
                    kshim_storage_strerror((int)a_), e_,                         \
                    kshim_storage_strerror((int)e_));                            \
            test_failures++;                                                     \
        }                                                                        \
    } while (0)

static inline int test_finish(const char *name)
{
    if (test_failures) {
        fprintf(stderr, "FAIL %s: %d check(s)\n", name, test_failures);
        return 1;
    }
    printf("PASS %s\n", name);
    return 0;
}

/* Private, writable mapping of a fixture: tests may corrupt it in memory. */
struct image {
    uint8_t *data;
    size_t size;
};

static inline int image_map(struct image *img, const char *dir, const char *name)
{
    char path[1024];
    struct stat st;
    int fd;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) || !st.st_size) {
        close(fd);
        return -1;
    }
    img->size = (size_t)st.st_size;
    img->data = mmap(NULL, img->size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    close(fd);
    if (img->data == MAP_FAILED) {
        img->data = NULL;
        return -1;
    }
    return 0;
}

static inline void image_unmap(struct image *img)
{
    if (img->data)
        munmap(img->data, img->size);
    img->data = NULL;
}

/* Read a host reference file into a malloc buffer. */
static inline uint8_t *host_file(const char *dir, const char *rel, size_t *size)
{
    char path[1024];
    FILE *f;
    uint8_t *data;
    long n;

    snprintf(path, sizeof(path), "%s/%s", dir, rel);
    f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc(n ? (size_t)n : 1);
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return data;
}

static inline const char *fixture_dir(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : getenv("KSHIM_STORAGE_FIXTURES");
    char path[1024];

    if (!dir)
        return NULL;
    snprintf(path, sizeof(path), "%s/manifest.txt", dir);
    return access(path, R_OK) ? NULL : dir;
}
