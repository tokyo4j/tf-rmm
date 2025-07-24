#ifndef MERGE_H
#define MERGE_H

#include <stdint.h>
#include <stdbool.h>

struct page_item {
	uint64_t ipa;
	uint64_t pa;
	uint64_t hash;
	uint64_t ms;
	// TODO revoke rec on realm destruction
	struct granule *g_rec;
	struct page_item *prev, *next;
	bool merged;
};

struct rec;
struct rsi_result;
struct smc_result;

void handle_rsi_set_pages_mergeable(struct rec *rec, struct rsi_result *res);

void smc_reclaim_mergeable_page(unsigned long index, struct smc_result *res);

bool merge_handle_data_destroy(uint64_t pa);

#endif
