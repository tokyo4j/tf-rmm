/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The Mbed TLS Contributors
 * SPDX-FileCopyrightText: Copyright TF-RMM Contributors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <arch_helpers.h>
#include <assert.h>
#include <cpuid.h>
#include <debug.h>
#include <mbedtls/memory_buffer_alloc.h>
#include <memory_alloc.h>
#include <sizes.h>
#include <string.h>
#include <merge.h>

#define MAGIC1 UL(0xFF00AA55)
#define MAGIC2 UL(0xEE119966)
#define MAX_BT 20

static struct buffer_alloc_ctx myctx;

static int
verify_header(struct memory_header_s *hdr)
{
	if (hdr->magic1 != MAGIC1) {
		return 1;
	}

	if (hdr->magic2 != MAGIC2) {
		return 1;
	}

	if (hdr->alloc > 1UL) {
		return 1;
	}

	if ((hdr->prev != NULL) && (hdr->prev == hdr->next)) {
		return 1;
	}

	if ((hdr->prev_free != NULL) && (hdr->prev_free == hdr->next_free)) {
		return 1;
	}

	return 0;
}

static int
verify_chain(struct buffer_alloc_ctx *heap)
{
	struct memory_header_s *prv = heap->first;
	struct memory_header_s *cur;

	if ((prv == NULL) || (verify_header(prv) != 0)) {
		return 1;
	}

	if (heap->first->prev != NULL) {
		return 1;
	}

	cur = heap->first->next;

	while (cur != NULL) {
		if (verify_header(cur) != 0) {
			return 1;
		}

		if (cur->prev != prv) {
			return 1;
		}

		prv = cur;
		cur = cur->next;
	}

	return 0;
}

static void *
buffer_alloc_calloc_with_heap(
	struct buffer_alloc_ctx *heap, size_t n, size_t size)
{
	struct memory_header_s *new;
	struct memory_header_s *cur = heap->first_free;
	uintptr_t p;
	void *ret;
	size_t original_len;
	size_t len;

	if ((heap->buf == NULL) || (heap->first == NULL)) {
		return NULL;
	}

	original_len = n * size;
	len = original_len;

	if ((n == 0UL) || (size == 0UL) || ((len / n) != size)) {
		return NULL;
	} else if (len > ((size_t)-MBEDTLS_MEMORY_ALIGN_MULTIPLE)) {
		return NULL;
	}

	if ((len % U(MBEDTLS_MEMORY_ALIGN_MULTIPLE)) != 0U) {
		len -= len % U(MBEDTLS_MEMORY_ALIGN_MULTIPLE);
		len += U(MBEDTLS_MEMORY_ALIGN_MULTIPLE);
	}

	/* Find block that fits */
	while (cur != NULL) {
		if (cur->size >= len) {
			break;
		}
		cur = cur->next_free;
	}

	if (cur == NULL) {
		return NULL;
	}

	if (cur->alloc != 0UL) {
		assert(false);
	}

	/* Found location, split block if > memory_header + 4 room left */
	if ((cur->size - len) < (sizeof(struct memory_header_s)
				 + U(MBEDTLS_MEMORY_ALIGN_MULTIPLE))) {
		cur->alloc = 1UL;

		/* Remove from free_list */
		if (cur->prev_free != NULL) {
			cur->prev_free->next_free = cur->next_free;
		} else {
			heap->first_free = cur->next_free;
		}

		if (cur->next_free != NULL) {
			cur->next_free->prev_free = cur->prev_free;
		}

		cur->prev_free = NULL;
		cur->next_free = NULL;

		/* cppcheck-suppress misra-c2012-10.1 */
		if ((heap->verify & U(MBEDTLS_MEMORY_VERIFY_ALLOC)) != 0U) {
			assert(verify_chain(heap) == 0);
		}

		ret = (void *)((uintptr_t)cur + sizeof(struct memory_header_s));
		(void)memset(ret, 0, original_len);

		return ret;
	}

	p = ((uintptr_t)cur) + sizeof(struct memory_header_s) + len;
	new = (struct memory_header_s *)p;

	new->size = cur->size - len - sizeof(struct memory_header_s);
	new->alloc = 0;
	new->prev = cur;
	new->next = cur->next;
	new->magic1 = MAGIC1;
	new->magic2 = MAGIC2;

	if (new->next != NULL) {
		new->next->prev = new;
	}

	/* Replace cur with new in free_list */
	new->prev_free = cur->prev_free;
	new->next_free = cur->next_free;
	if (new->prev_free != NULL) {
		new->prev_free->next_free = new;
	} else {
		heap->first_free = new;
	}

	if (new->next_free != NULL) {
		new->next_free->prev_free = new;
	}

	cur->alloc = 1;
	cur->size = len;
	cur->next = new;
	cur->prev_free = NULL;
	cur->next_free = NULL;

	/* cppcheck-suppress misra-c2012-10.1 */
	if ((heap->verify & U(MBEDTLS_MEMORY_VERIFY_ALLOC)) != 0U) {
		assert(verify_chain(heap) == 0);
	}

	ret = (void *)((uintptr_t)cur + sizeof(struct memory_header_s));
	(void)memset(ret, 0, original_len);

	return ret;
}

static void
mymalloc_init(unsigned char *buf, size_t len)
{
	myctx = (struct buffer_alloc_ctx){0};

	(void)memset(buf, 0, len);

	myctx.buf = buf;
	myctx.len = len;

	myctx.first = (struct memory_header_s *)buf;
	myctx.first->size = len - sizeof(struct memory_header_s);
	myctx.first->magic1 = MAGIC1;
	myctx.first->magic2 = MAGIC2;
	myctx.first_free = myctx.first;
}

#define MAX_MERGEABLE_SIZE (3ull * 1024 * 1024 * 1024)
#define REQUIRED_ALLOC_SIZE (MAX_MERGEABLE_SIZE * sizeof(struct page_item) / 4096)

void *
myalloc_alloc(size_t n, size_t size)
{
	static bool inited = false;
	if (!inited) {
		static unsigned char buf[REQUIRED_ALLOC_SIZE];
		mymalloc_init(buf, sizeof(buf));
		inited = true;
	}

	return buffer_alloc_calloc_with_heap(&myctx, n, size);
}

static void
buffer_alloc_free_with_heap(struct buffer_alloc_ctx *heap, void *ptr)
{
	struct memory_header_s *hdr;
	struct memory_header_s *old = NULL;
	uintptr_t p = (uintptr_t)ptr;

	if ((ptr == NULL) || (heap->buf == NULL) || (heap->first == NULL)) {
		return;
	}

	if ((p < (uintptr_t)heap->buf)
		|| (p >= ((uintptr_t)heap->buf + heap->len))) {
		assert(0);
	}

	p -= sizeof(struct memory_header_s);
	hdr = (struct memory_header_s *)p;

	assert(verify_header(hdr) == 0);

	if (hdr->alloc != 1U) {
		assert(0);
	}

	hdr->alloc = 0;

	/* Regroup with block before */
	if ((hdr->prev != NULL) && (hdr->prev->alloc == 0UL)) {
		hdr->prev->size += sizeof(struct memory_header_s) + hdr->size;
		hdr->prev->next = hdr->next;
		old = hdr;
		hdr = hdr->prev;

		if (hdr->next != NULL) {
			hdr->next->prev = hdr;
		}

		(void)memset(old, 0, sizeof(struct memory_header_s));
	}

	/* Regroup with block after */
	if ((hdr->next != NULL) && (hdr->next->alloc == 0UL)) {
		hdr->size += sizeof(struct memory_header_s) + hdr->next->size;
		old = hdr->next;
		hdr->next = hdr->next->next;

		if ((hdr->prev_free != NULL) || (hdr->next_free != NULL)) {
			if (hdr->prev_free != NULL) {
				hdr->prev_free->next_free = hdr->next_free;
			} else {
				heap->first_free = hdr->next_free;
			}
			if (hdr->next_free != NULL) {
				hdr->next_free->prev_free = hdr->prev_free;
			}
		}

		hdr->prev_free = old->prev_free;
		hdr->next_free = old->next_free;

		if (hdr->prev_free != NULL) {
			hdr->prev_free->next_free = hdr;
		} else {
			heap->first_free = hdr;
		}

		if (hdr->next_free != NULL) {
			hdr->next_free->prev_free = hdr;
		}

		if (hdr->next != NULL) {
			hdr->next->prev = hdr;
		}

		(void)memset(old, 0, sizeof(struct memory_header_s));
	}

	/*
	 * Prepend to free_list if we have not merged
	 * (Does not have to stay in same order as prev / next list)
	 */
	if (old == NULL) {
		hdr->next_free = heap->first_free;
		if (heap->first_free != NULL) {
			heap->first_free->prev_free = hdr;
		}
		heap->first_free = hdr;
	}

	/* cppcheck-suppress misra-c2012-10.1 */
	if ((heap->verify & U(MBEDTLS_MEMORY_VERIFY_FREE)) != 0U) {
		assert(verify_chain(heap));
	}
}

void
myalloc_free(void *ptr)
{
	buffer_alloc_free_with_heap(&myctx, ptr);
}
