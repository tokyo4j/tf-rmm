#include <buffer.h>
#include <debug.h>
#include <granule.h>
#include <measurement.h>
#include <merge.h>
#include <realm.h>
#include <rsi-handler.h>
#include <smc-rsi.h>
#include <smc.h>
#include <string.h>
#include <utils_def.h>

#include <cpuid.h>
#include <myalloc.h>

struct page_item {
	uint64_t ipa;
	uint64_t pa;
	uint64_t hash;
	// TODO revoke rec on realm destruction
	struct granule *g_rec;
	struct page_item *prev, *next;
};

struct page_item mergeable = {.prev = &mergeable, .next = &mergeable};

static inline void
page_list_add(struct page_item *list, struct page_item *item)
{
	item->prev = list->prev;
	item->next = list;
	list->prev->next = item;
	list->prev = item;
}

static inline void
page_list_remove(struct page_item *item)
{
	item->prev->next = item->next;
	item->next->prev = item->prev;
}

static uint64_t
hash_page(uint64_t *p)
{
	uint64_t *end = (uint64_t *)((char *)p + 4096);
	uint64_t hash = 0;
	for (; p < end; p++) {
		hash ^= *p;
	}
	return hash;
}

#define PAGE_LIST_FOR_EACH(list, item)                                         \
	for (struct page_item * (item) = (list)->next; (item) != (list);       \
		(item) = (item)->next)

static void
set_page_mergeable(struct rec *rec, uint64_t ipa)
{
	struct s2_walk_result r;
	enum s2_walk_status walk_stat = realm_ipa_to_pa(rec, ipa, &r);
	granule_unlock(r.llt);

	assert(walk_stat == WALK_SUCCESS);
	struct granule *grn = find_granule(r.pa);

	char *mapped_page = buffer_granule_map(grn, SLOT_RSI_CALL);
	uint64_t hash = hash_page((uint64_t *)mapped_page);

	// NOTICE("set_page_mergeabe(): ipa=%lx pa=%lx\n", ipa, r.pa);

	struct page_item *insert_before = &mergeable;
	PAGE_LIST_FOR_EACH(&mergeable, item)
	{
		// NOTICE("item->hash=%lx hash=%lx\n", item->hash, hash);
		if (item->hash >= hash) {
			insert_before = item;
			break;
		}
	}

	struct page_item *new_item = myalloc_alloc(1, sizeof(*new_item));
	if (!new_item) {
		NOTICE("OUT OF MEMORY\n");
		panic();
	}
	new_item->ipa = ipa;
	new_item->pa = r.pa;
	new_item->hash = hash;
	new_item->g_rec = rec->g_rec;
	// insert before the first item with larger hash
	page_list_add(insert_before, new_item);

	buffer_unmap(mapped_page);
}

void
handle_rsi_set_pages_mergeable(struct rec *rec, struct rsi_result *res)
{
	// NOTICE("handle_rsi_set_pages_mergeable start\n");

	uint64_t ipa_start = rec->regs[1];
	uint64_t len = rec->regs[2];
	for (uint64_t ipa = ipa_start; ipa < ipa_start + len; ipa += 4096) {
		set_page_mergeable(rec, ipa);
	}

	res->action = UPDATE_REC_RETURN_TO_REALM;
	res->smc_res.x[0] = RSI_SUCCESS;
}

static bool
items_identical(struct page_item *item1, struct page_item *item2)
{
	if (item1->hash != item2->hash) {
		return false;
	}

	/* TODO: lock granule? */
	struct granule *granule1 = find_granule(item1->pa);
	unsigned char *map1 = buffer_granule_map(granule1, SLOT_RSI_CALL);
	unsigned char buf1[4096];
	memcpy(buf1, map1, sizeof(buf1));
	buffer_unmap(map1);
	map1 = NULL;

	struct granule *granule2 = find_granule(item2->pa);
	unsigned char *map2 = buffer_granule_map(granule2, SLOT_RSI_CALL);
	bool equal = memcmp(map2, buf1, sizeof(buf1)) == 0;
	buffer_unmap(map2);
	map2 = NULL;

	if (!equal) {
		NOTICE("Hash matched but not identical pages\n");
	}

	return equal;
}

static bool
find_duplicated_items(struct page_item **item1, struct page_item **item2)
{
	struct page_item *prev_item = NULL;
	PAGE_LIST_FOR_EACH(&mergeable, item)
	{
		// NOTICE("find_duplicated_items item->hash=%lx\n", item->hash);

		if (!prev_item) {
			prev_item = item;
			continue;
		}

		if (items_identical(prev_item, item)) {
			*item1 = prev_item;
			*item2 = item;
			return true;
		}

		prev_item = item;
	}
	return false;
}

void
smc_reclaim_mergeable_page(unsigned long index, struct smc_result *res)
{
	res->x[0] = RMI_SUCCESS;

	struct page_item *fixed_item, *item;
	if (!find_duplicated_items(&fixed_item, &item)) {
		// NOTICE("Duplicated page not found\n");
		return;
	}

	struct rec *rec = buffer_granule_map(item->g_rec, SLOT_REC);

	NOTICE("Duplicated page found (ipa=%lx pa=%lx)\n", item->ipa, item->pa);

	struct s2tt_walk wi;
	struct s2tt_context *s2_ctx = &rec->realm_info.s2_ctx;
	granule_lock(s2_ctx->g_rtt, GRANULE_STATE_RTT);
	s2tt_walk_lock_unlock(s2_ctx, item->ipa, S2TT_PAGE_LEVEL, &wi);
	uint64_t *s2tt = buffer_granule_map(wi.g_llt, SLOT_RTT);
	uint64_t s2tte =
		s2tte_create_assigned_ram(s2_ctx, item->pa, S2TT_PAGE_LEVEL);
	s2tte_write(&s2tt[wi.index], s2tte);

	res->x[1] = item->pa;
	page_list_remove(item);
	myalloc_free(item);

	granule_unlock(wi.g_llt);
	buffer_unmap(s2tt);
	s2tt_invalidate_page(s2_ctx, item->ipa);
	buffer_unmap(rec);
}
