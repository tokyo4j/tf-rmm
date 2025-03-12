#ifndef MYALLOC_H
#define MYALLOC_H

#include <stdint.h>
#include <stddef.h>

void *myalloc_alloc(size_t n, size_t size);
void myalloc_free(void *ptr);

#endif
