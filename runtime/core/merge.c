#include <buffer.h>
#include <cpuid.h>
#include <debug.h>
#include <granule.h>
#include <measurement.h>
#include <merge.h>
#include <realm.h>
#include <rmm_el3_ifc.h>
#include <rsi-handler.h>
#include <smc-rsi.h>
#include <smc.h>
#include <string.h>
#include <utils_def.h>

static void *
myalloc(uint64_t len) {
	static char buf[0x3c00000];
	static char *ptr = buf;

	if (ptr + len >= buf + sizeof(buf)) {
		return NULL;
	}

	char *ret = ptr;
	ptr += len;
	return ret;
}

spinlock_t log_lock;

static struct ctx {
	struct rb_node *mergeable_pages;
	struct rb_node *pending_pages;
	struct rb_node *reclaimed_pages; // for debugging
	spinlock_t lock;
} _ctx;

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
get_time_ns(void)
{
	uint64_t ms;
	asm volatile("mrs %0, cntvct_el0" : "=r"(ms));
	return ms;
}

static inline struct page_item *
rb2item(struct rb_node *node) {
	if (node) {
		return (struct page_item *)(
			(char *)node - __builtin_offsetof(struct page_item, rb));
	} else {
		return NULL;
	}
}

static uint32_t
rand(void)
{
	static uint32_t x = 0x51810bd5;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return x;
}

static void
set_page_mergeable(struct ctx *ctx, struct rec *rec, uint64_t ipa)
{
	struct s2tt_walk wi;
	struct s2tt_context *s2_ctx = &rec->realm_info.s2_ctx;
	granule_lock(s2_ctx->g_rtt, GRANULE_STATE_RTT);
	s2tt_walk_lock_unlock(s2_ctx, ipa, S2TT_PAGE_LEVEL, &wi);

	uint64_t *s2tt = buffer_granule_map(wi.g_llt, SLOT_RTT);
	uint64_t s2tte = s2tte_read(&s2tt[wi.index]);
	uint64_t pa = s2tte_pa(s2_ctx, s2tte, wi.last_level);

	const uint64_t ap_mask = 3ull << 6;
	const uint64_t ap_ro = 1ull << 6;
	s2tte = (s2tte & ~ap_mask) | ap_ro;
	s2tte_write(&s2tt[wi.index], s2tte);
	s2tt_invalidate_page(s2_ctx, ipa);

	buffer_unmap(s2tt);
	granule_unlock(wi.g_llt);

	struct page_ref *new_ref = myalloc(sizeof(*new_ref));
	struct page_item *new_item = myalloc(sizeof(*new_item));
	rb_node_init(&new_item->rb);
	if (!new_ref || !new_item) {
		NOTICE("OUT OF MEMORY\n");
		panic();
	}

	new_ref->ipa = ipa;
	new_ref->item = new_item;
	new_ref->g_rec = rec->g_rec;
	new_item->refs = new_ref;
	new_item->pa = pa;

	uint64_t time_thr = 30 + 30 * (uint64_t)rand() / (uint64_t)UINT32_MAX;
	time_thr *= 1000000000;
	new_item->ns = new_item->rb.key = get_time_ns() + time_thr;
	// NOTICE("new_item->ns=%ld\n", new_item->ns);
	rb_insert(&ctx->pending_pages, &new_item->rb);
}

void
handle_rsi_set_pages_mergeable(struct rec *rec, struct rsi_result *res)
{
	struct ctx *ctx = &_ctx;
	spinlock_acquire(&ctx->lock);

	uint64_t ipa_start = rec->regs[1];
	uint64_t len = rec->regs[2];

	uint64_t start_ns = get_time_ns();

	for (uint64_t ipa = ipa_start; ipa < ipa_start + len; ipa += 4096) {
		set_page_mergeable(ctx, rec, ipa);
	}

	res->action = UPDATE_REC_RETURN_TO_REALM;
	res->smc_res.x[0] = RSI_SUCCESS;

	while (get_time_ns() - start_ns < 500000 * (len / 4096));
	// NOTICE("handle_rsi_set_pages_mergeable():ns,len= %ld %ld\n", get_time_ns() - start_ns, len / 4096);

	spinlock_release(&ctx->lock);
}

static struct page_item *
get_scanned_item(struct ctx *ctx, char **content)
{
	struct page_item *item = rb2item(rb_first(ctx->pending_pages));
	if (item && get_time_ns() >= item->ns) {
		rb_erase(&ctx->pending_pages, &item->rb);

		struct granule *grn = find_granule(item->pa);
		*content = buffer_granule_map(grn, SLOT_RSI_CALL);
		item->rb.key = hash_page((uint64_t *)*content);

		rb_insert(&ctx->mergeable_pages, &item->rb);
		return item;
	}
	return NULL;
}

static struct page_item *
find_dup(struct rb_node *merged_map, struct page_item *item, char *content)
{
	uint64_t now = get_time_ns();

	for (struct rb_node *node = rb_find(merged_map, item->rb.key);
			node; node = node->dup) {
		struct page_item *dup_item = rb2item(node);
		if (dup_item == item) {
			continue;
		}
		if (now < dup_item->ns) {
			continue;
		}
		struct granule *grn = find_granule(dup_item->pa);
		char *mapped_page = buffer_granule_map(grn, SLOT_REC);
		bool eq = memcmp(mapped_page, content, 4096) == 0;
		buffer_unmap(mapped_page);
		if (eq) {
			return dup_item;
		}
	}

	return NULL;
}

static void
copy_page(struct page_item *dst, struct page_item *src)
{
	struct granule *g_dst_item = find_granule(dst->pa);
	struct granule *g_src_item = find_granule(src->pa);
	void *map_dst = buffer_granule_map(g_dst_item, SLOT_RSI_CALL);
	void *map_src = buffer_granule_map(g_src_item, SLOT_REC);
	memcpy(map_dst, map_src, 4096);
	buffer_unmap(map_dst);
	buffer_unmap(map_src);
}

static void
remap_page(struct page_item *dst, struct page_item *src)
{
	for (struct page_ref *ref = src->refs; ref; ref = ref->next) {
		// NOTICE("remapping ipa:%lx -> pa:%lx\n", ref->ipa, dst->pa);
		struct rec *rec = buffer_granule_map(ref->g_rec, SLOT_REC);
		struct s2tt_context *s2_ctx = &rec->realm_info.s2_ctx;

		struct s2tt_walk wi;
		granule_lock(s2_ctx->g_rtt, GRANULE_STATE_RTT);
		s2tt_walk_lock_unlock(s2_ctx, ref->ipa, S2TT_PAGE_LEVEL, &wi);
		uint64_t *s2tt = buffer_granule_map(wi.g_llt, SLOT_RTT);

		uint64_t s2tte = s2tt[wi.index];

		const uint64_t ap_mask = 3ull << 6;
		const uint64_t ap_ro = 1ull << 6;
		const uint64_t pa_mask = BIT_MASK_ULL(48, 12);
		if ((s2tte & ap_mask) != ap_ro || (s2tte & pa_mask) != src->pa) {
			NOTICE("invalid s2tte=%lx\n", s2tte);
			panic();
		}

		s2tte = (s2tte & ~pa_mask) | dst->pa;
		s2tte_write(&s2tt[wi.index], s2tte);
		s2tt_invalidate_page(s2_ctx, ref->ipa);

		ref->item = dst;

		granule_unlock(wi.g_llt);
		buffer_unmap(s2tt);
		buffer_unmap(rec);
	}

	struct page_ref **tail = &dst->refs;
	while (*tail) {
		tail = &(*tail)->next;
	}
	*tail = src->refs;
	src->refs = NULL;
}

static uint64_t
max(uint64_t a, uint64_t b) {
	return (a > b) ? a : b;
}

static uint64_t
reclaim_page(struct ctx *ctx)
{
	char *content = NULL;
	struct page_item *scan_item = get_scanned_item(ctx, &content);
	if (!scan_item) {
		// NOTICE("scan_item not found\n");
		return 0;
	}
	// NOTICE("scan_item=%8lx(%lx)\n", (uint64_t)&scan_item->rb, scan_item->rb.hash);
	struct page_item *dup_item = find_dup(ctx->mergeable_pages, scan_item, content);
	buffer_unmap(content);
	if (!dup_item) {
		// NOTICE("dup_item not found\n");
		return 0;
	}
	// NOTICE("dup_item=%8lx(%lx)\n", (uint64_t)&dup_item->rb, dup_item->rb.key);

	struct rb_node **rand_tree = NULL;
	struct page_item *rand_item = NULL;
	for (int i = 0; i < 5; i++) {
		size_t idx = rand() % (rb_size(ctx->pending_pages)
					+ rb_size(ctx->mergeable_pages));
		if (idx < rb_size(ctx->pending_pages)) {
			rand_tree = &ctx->pending_pages;
		} else {
			rand_tree = &ctx->mergeable_pages;
			idx -= rb_size(ctx->pending_pages);
		}
		rand_item = rb2item(rb_select(*rand_tree, idx));

		if (rand_item == scan_item || rand_item == dup_item) {
			rand_item = NULL;
		}
		if (rand_item) {
			break;
		}
	}
	if (!rand_item) {
		// NOTICE("rand_item not found\n");
		return 0;
	}
	// NOTICE("p1->ipa=%lx, p2->ipa=%lx, ret->pa=%lx, ret->ipa=%lx\n",
	// 	scan_item->refs->ipa, dup_item->refs->ipa, rand_item->pa, rand_item->refs->ipa);
	// NOTICE("rand_item=%8lx(%lx)\n", (uint64_t)&rand_item->rb, rand_item->rb.hash);

	rb_erase(&ctx->mergeable_pages, &dup_item->rb);
	rb_erase(rand_tree, &rand_item->rb);

	scan_item->ns = max(scan_item->ns, dup_item->ns);
	dup_item->ns = rand_item->ns;
	dup_item->rb.key = rand_item->rb.key;
	copy_page(dup_item, rand_item);
	remap_page(scan_item, dup_item);
	remap_page(dup_item, rand_item);
	rb_insert(rand_tree, &dup_item->rb);

	struct granule *granule = find_granule(rand_item->pa);
	granule_lock(granule, GRANULE_STATE_DATA);
	rmm_el3_ifc_gtsi_undelegate(rand_item->pa);
	granule_unlock_transition(granule, GRANULE_STATE_NS);

	uint64_t pa = rand_item->pa;
	rb_insert(&ctx->reclaimed_pages, &rand_item->rb);
	// myalloc_free(copied_from_item);

	return pa;
}

void
smc_reclaim_mergeable_page(unsigned long pa_array_addr, struct smc_result *res)
{
	struct ctx *ctx = &_ctx;
	spinlock_acquire(&ctx->lock);
	// NOTICE("smc_reclaim_mergeable_page(): pa=%lx\n", pa_array_addr);

	// uint64_t start_ns = get_time_ns();

	static uint64_t pa_array[512];
	memset(pa_array, 0, sizeof(pa_array));

	bool success = false;
	int i = 0;
	for (int tries = 0; tries < 512; tries++) {
		uint64_t pa = reclaim_page(ctx);
		if (!pa) {
			continue;
		}
		success = true;
		pa_array[i++] = pa;
		res->x[1] = pa;
	}
	if (!success) {
		res->x[0] = 1;
		goto out;
	}

	struct granule *g = find_granule(pa_array_addr);
	ns_buffer_write(SLOT_NS, g, 0, 4096, pa_array);
	res->x[0] = RMI_SUCCESS;

	// while (get_time_ns() - start_ns < (uint64_t)i * 800000);
	// NOTICE("smc_reclaim_mergeable_page():ns,i= %ld %d\n", get_time_ns() - start_ns, i);

out:
	spinlock_release(&ctx->lock);
}

bool
merge_handle_data_destroy(uint64_t ipa) {
	struct ctx *ctx = &_ctx;
	spinlock_acquire(&ctx->lock);

	for (struct rb_node *node = rb_first(ctx->mergeable_pages);
			node; node = rb_next(node)) {
		struct page_item *item = rb2item(node);
			for (struct page_ref *ref = item->refs; ref; ref = ref->next) {
				if (ref->ipa == ipa) {
					NOTICE("Tried to destroy mergeable page: ipa=%lx, pa=%lx\n",
						ref->ipa, item->pa);
			}
		}
	}

	for (struct rb_node *node = rb_first(ctx->reclaimed_pages);
			node; node = rb_next(node)) {
		struct page_item *item = rb2item(node);
			for (struct page_ref *ref = item->refs; ref; ref = ref->next) {
				if (ref->ipa == ipa) {
					NOTICE("Tried to destroy reclaimed page: ipa=%lx, pa=%lx\n",
						ref->ipa, item->pa);
			}
		}
	}

	spinlock_release(&ctx->lock);
	return false;
}
