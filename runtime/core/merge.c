#include <buffer.h>
#include <cpuid.h>
#include <debug.h>
#include <granule.h>
#include <measurement.h>
#include <merge.h>
#include <myalloc.h>
#include <realm.h>
#include <rmm_el3_ifc.h>
#include <rsi-handler.h>
#include <smc-rsi.h>
#include <smc.h>
#include <string.h>
#include <utils_def.h>

static spinlock_t lock;

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

static uint64_t
get_time_ms(void)
{
	uint64_t ms;
	asm volatile("mrs %0, cntvct_el0" : "=r"(ms));
	return ms;
}

#define PAGE_LIST_FOR_EACH(list, item)                                         \
	for (struct page_item * (item) = (list)->next; (item) != (list);       \
		(item) = (item)->next)

static void
set_page_mergeable(struct rec *rec, uint64_t ipa)
{
	struct s2tt_walk wi;
	struct s2tt_context *s2_ctx = &rec->realm_info.s2_ctx;
	granule_lock(s2_ctx->g_rtt, GRANULE_STATE_RTT);
	s2tt_walk_lock_unlock(s2_ctx, ipa, S2TT_PAGE_LEVEL, &wi);

	uint64_t *s2tt = buffer_granule_map(wi.g_llt, SLOT_RTT);
	uint64_t s2tte = s2tte_read(&s2tt[wi.index]);
	uint64_t pa = s2tte_pa(s2_ctx, s2tte, wi.last_level);
	struct granule *grn = find_granule(pa);

	char *mapped_page = buffer_granule_map(grn, SLOT_RSI_CALL);
	uint64_t hash = hash_page((uint64_t *)mapped_page);
	buffer_unmap(mapped_page);

	const uint64_t ap_mask = 3ull << 6;
	const uint64_t ap_ro = 1ull << 6;
	s2tte = (s2tte & ~ap_mask) | ap_ro;
	s2tte_write(&s2tt[wi.index], s2tte);
	s2tt_invalidate_page(s2_ctx, ipa);

	buffer_unmap(s2tt);
	granule_unlock(wi.g_llt);

	struct page_item *insert_before = &mergeable;
	PAGE_LIST_FOR_EACH(&mergeable, item) {
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
	new_item->pa = pa;
	new_item->hash = hash;
	new_item->g_rec = rec->g_rec;
	new_item->ms = get_time_ms();

	// insert before the first item with larger hash
	page_list_add(insert_before, new_item);
}

void
handle_rsi_set_pages_mergeable(struct rec *rec, struct rsi_result *res)
{
	spinlock_acquire(&lock);
	// NOTICE("handle_rsi_set_pages_mergeable start\n");

	uint64_t ipa_start = rec->regs[1];
	uint64_t len = rec->regs[2];
	for (uint64_t ipa = ipa_start; ipa < ipa_start + len; ipa += 4096) {
		set_page_mergeable(rec, ipa);
	}

	res->action = UPDATE_REC_RETURN_TO_REALM;
	res->smc_res.x[0] = RSI_SUCCESS;

	spinlock_release(&lock);
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

static uint32_t
rand(void)
{
	static uint32_t x = 123456;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return x;
}

static bool
find_duplicated_items(
	struct page_item **copied_to_item, struct page_item **merged_item)
{
	uint64_t now = get_time_ms();
	const uint64_t second = 1000000000;
	uint64_t time_threshold = 5 * second + 5 * second * (uint64_t)rand() / UINT32_MAX;

	struct page_item *prev_item = NULL;
	PAGE_LIST_FOR_EACH(&mergeable, item) {
		if (!prev_item) {
			prev_item = item;
			continue;
		}

		if ((!prev_item->merged || !item->merged)
			&& items_identical(prev_item, item)
			&& now - prev_item->ms > time_threshold
			&& now - item->ms > time_threshold) {
			if (!prev_item->merged) {
				*copied_to_item = prev_item;
				*merged_item = item;
			} else {
				*copied_to_item = item;
				*merged_item = prev_item;
			}
			return true;
		}

		prev_item = item;
	}
	return false;
}

static struct page_item *
find_copied_from_item(
	struct page_item *ignored_item1, struct page_item *ignored_item2)
{
	uint32_t i = 0;
	PAGE_LIST_FOR_EACH(&mergeable, item) {
		if (item == ignored_item1 || item == ignored_item2) {
			continue;
		}
		i++;
	}

	if (i == 0) {
		return NULL;
	}

	uint32_t migrated_item_idx = rand() % i;

	i = 0;
	PAGE_LIST_FOR_EACH(&mergeable, item) {
		if (item == ignored_item1 || item == ignored_item2) {
			continue;
		}
		if (migrated_item_idx == i) {
			return item;
		}
		i++;
	}

	panic();
	return NULL;
}

static void
copy_and_eject_item(struct page_item *dst, struct page_item *src)
{
	struct granule *g_dst_item = find_granule(dst->pa);
	struct granule *g_src_item = find_granule(src->pa);
	void *map_dst = buffer_granule_map(g_dst_item, SLOT_RSI_CALL);
	void *map_src = buffer_granule_map(g_src_item, SLOT_REC);
	memcpy(map_dst, map_src, 4096);
	buffer_unmap(map_dst);
	buffer_unmap(map_src);

	dst->hash = src->hash;

	struct page_item *src_next = src->next;
	if (src_next == dst) {
		src_next = src_next->next;
	}
	if (src_next == src || src_next == dst) {
		NOTICE("failed to copy item\n");
		panic();
	}
	page_list_remove(src);
	page_list_remove(dst);
	page_list_add(src_next, dst);
}

static void
remap_page(struct page_item *item, uint64_t pa)
{
	struct rec *rec = buffer_granule_map(item->g_rec, SLOT_REC);
	struct s2tt_context *s2_ctx = &rec->realm_info.s2_ctx;

	struct s2tt_walk wi;
	granule_lock(s2_ctx->g_rtt, GRANULE_STATE_RTT);
	s2tt_walk_lock_unlock(s2_ctx, item->ipa, S2TT_PAGE_LEVEL, &wi);
	uint64_t *s2tt = buffer_granule_map(wi.g_llt, SLOT_RTT);

	uint64_t s2tte = s2tt[wi.index];
	const uint64_t pa_mask = BIT_MASK_ULL(48, 12);
	s2tte = (s2tte & ~pa_mask) | pa;
	s2tte_write(&s2tt[wi.index], s2tte);

	s2tt_invalidate_page(s2_ctx, item->ipa);

	granule_unlock(wi.g_llt);
	buffer_unmap(s2tt);
	buffer_unmap(rec);
}

__attribute__((unused)) static void
debug_state(void)
{
	PAGE_LIST_FOR_EACH(&mergeable, item) {
		NOTICE("---- ipa=%lx pa=%lx hash=%lx merged=%d\n",
			item->ipa, item->pa, item->hash, item->merged);
	}
}

void
smc_reclaim_mergeable_page(unsigned long index, struct smc_result *res)
{
	spinlock_acquire(&lock);
	res->x[0] = 1;

	struct page_item *copied_to_item, *merged_item;
	if (!find_duplicated_items(&copied_to_item, &merged_item)) {
		goto out;
	}
	struct page_item *copied_from_item =
		find_copied_from_item(copied_to_item, merged_item);
	if (!copied_from_item) {
		goto out;
	}

	copy_and_eject_item(copied_to_item, copied_from_item);

	remap_page(copied_to_item, merged_item->pa);
	remap_page(copied_from_item, copied_to_item->pa);
	copied_to_item->ipa = copied_from_item->ipa;
	merged_item->merged = true;

	struct granule *granule = find_granule(copied_from_item->pa);
	granule_lock(granule, GRANULE_STATE_DATA);
	rmm_el3_ifc_gtsi_undelegate(copied_from_item->pa);
	granule_unlock_transition(granule, GRANULE_STATE_NS);

	res->x[1] = copied_from_item->pa;
	myalloc_free(copied_from_item);

	// debug_state();

	res->x[0] = RMI_SUCCESS;
out:
	spinlock_release(&lock);
}
