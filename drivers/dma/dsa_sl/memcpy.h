#ifndef DSA_MEMCPY_H
#define DSA_MEMCPY_H

#include "dsa.h"

int dsa_memcpy_pp(uint64_t dst, uint64_t src, size_t n,
		  unsigned long (*copy)(void *, const void *, size_t));

int dsa_memcpy_no_pagefault(uint64_t dst, uint64_t src, size_t size);

int dsa_copy_from_user(void *dst, const void *src, size_t size);

int dsa_copy_to_user(void *dst, const void *src, size_t size);

int dsa_memcpy_user(void *dst, const void *src, size_t size);

#endif
