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
	struct page_item *prev, *next;
};

struct page_item merged = {.prev = &merged, .next = &merged};
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

void
handle_rsi_set_pages_mergeable(struct rec *rec, struct rsi_result *res)
{
	NOTICE("handle_rsi_set_pages_mergeable start\n");

	struct s2_walk_result r;
	uint64_t ipa = rec->regs[1];
	enum s2_walk_status walk_stat = realm_ipa_to_pa(rec, ipa, &r);
	granule_unlock(r.llt);

	assert(walk_stat == WALK_SUCCESS);
	struct granule *grn = find_granule(r.pa);

	char *mapped_page = buffer_granule_map(grn, SLOT_RSI_CALL);
	uint64_t hash = hash_page((uint64_t *)mapped_page);

	NOTICE("handle_rsi_set_pages_mergeable(): ipa=%lx pa=%lx\n", ipa, r.pa);

	PAGE_LIST_FOR_EACH(&mergeable, item)
	{
		NOTICE("item->hash=%lx hash=%lx\n", item->hash, hash);
		if (item->hash != hash) {
			continue;
		}

		/* TODO: lock granule? */
		unsigned char buf[4096];
		memcpy(buf, mapped_page, sizeof(buf));
		buffer_unmap(mapped_page);

		struct granule *target_granule = find_granule(item->pa);
		unsigned char *mapped_target_page =
			buffer_granule_map(target_granule, SLOT_RSI_CALL);
		bool equal = memcmp(mapped_target_page, buf, sizeof(buf)) == 0;

		buffer_unmap(mapped_target_page);
		mapped_page = buffer_granule_map(grn, SLOT_RSI_CALL);

		if (!equal) {
			NOTICE("Hash matched, but not an identical page\n");
			continue;
		}

		NOTICE("Duplicated page found (ipa=%lx pa=%lx)\n", item->ipa,
			item->pa);

		struct s2tt_walk wi;
		struct s2tt_context *s2_ctx = &rec->realm_info.s2_ctx;
		granule_lock(s2_ctx->g_rtt, GRANULE_STATE_RTT);
		s2tt_walk_lock_unlock(s2_ctx, ipa, S2TT_PAGE_LEVEL, &wi);
		uint64_t *s2tt = buffer_granule_map(wi.g_llt, SLOT_RTT);
		uint64_t s2tte = s2tte_create_assigned_ram(
			s2_ctx, item->pa, S2TT_PAGE_LEVEL);
		s2tte_write(&s2tt[wi.index], s2tte);
		granule_unlock(wi.g_llt);
		buffer_unmap(s2tt);
		s2tt_invalidate_page(s2_ctx, ipa);

		page_list_remove(item);
		page_list_add(&merged, item);

		goto done;
	}

	NOTICE("cpuid=%d\n", my_cpuid());

	struct page_item *new_item = myalloc_alloc(1, sizeof(*new_item));
	new_item->ipa = ipa;
	new_item->pa = r.pa;
	new_item->hash = hash;
	page_list_add(&mergeable, new_item);

done:
	buffer_unmap(mapped_page);

	res->action = UPDATE_REC_RETURN_TO_REALM;
	res->smc_res.x[0] = RSI_SUCCESS;
}

void
smc_reclaim_mergeable_page(unsigned long index, struct smc_result *res)
{
	if (merged.prev != &merged) {
		struct page_item *item = merged.prev;
		res->x[1] = item->pa;
		NOTICE("smc_reclaim_mergeable_page pa=%lx\n", item->pa);
		page_list_remove(item);
		myalloc_free(item);
	}
	res->x[0] = RMI_SUCCESS;
}
