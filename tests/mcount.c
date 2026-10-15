/* LD_PRELOAD malloc counter: exact bytes allocated between reset and
 * read - used to size the reference codec2_create() heap footprint. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stddef.h>

static size_t total = 0;
static void *(*real_malloc)(size_t);

void *malloc(size_t n)
{
    if (!real_malloc)
        real_malloc = dlsym(RTLD_NEXT, "malloc");
    void *p = real_malloc(n);
    if (p)
        total += n;
    return p;
}

void __malloc_reset(void) { total = 0; }
size_t __malloc_total(void) { return total; }
