#ifndef MERGE_H
#define MERGE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct rb_node {
	struct rb_node *left;
	struct rb_node *right;
	struct rb_node *parent;
	/* duplicate chain: nodes with equal key are chained here (not part of
	   binary links). Head node is the one embedded in the tree; its
	   `dup` points to the first duplicate, which points to the next, etc.
	 */
	struct rb_node *dup;
	size_t size;	     /* total nodes in subtree rooted here */
	unsigned char color; /* 0 = red, 1 = black */
	uint64_t key;        /* key stored in the node */
};

#define RB_RED 0
#define RB_BLACK 1

#define rb_entry(ptr, type, member)                                            \
	((type *)((char *)(ptr) - offsetof(type, member)))

static inline size_t
rb_size(const struct rb_node *n)
{
	return n ? n->size : 0;
}

/* Initialize a standalone node (no links). Color unspecified until inserted. */
static inline void
rb_node_init(struct rb_node *n)
{
	n->left = n->right = n->parent = NULL;
	n->dup = NULL;
	n->color = RB_RED;
	n->size = 1;
}

/* Insert node into tree rooted at *root.
	Caller must set `node->key` before calling. Keys are compared using the
	embedded `node->key` (uint64_t).
*/
void rb_insert(struct rb_node **root, struct rb_node *node);

/* Erase node from tree rooted at *root. Node must be already in tree. */
void rb_erase(struct rb_node **root, struct rb_node *node);

/* In-order selection by 0-based index. Returns NULL if index >= size(root). */
struct rb_node *rb_select(struct rb_node *root, size_t index);

/* In-order rank (0-based) of node in tree rooted at root. */
size_t rb_rank(struct rb_node *root, struct rb_node *node);

/* Iterator helpers */
struct rb_node *rb_first(struct rb_node *root);
struct rb_node *rb_next(struct rb_node *node);

/* Find the tree head node with the given key. Returns NULL if not found.
	If found, the returned node is the head in the binary links; duplicates
	(if any) are available via the returned node's `dup` chain.
*/
struct rb_node *rb_find(struct rb_node *root, uint64_t key);

struct page_ref {
	uint64_t ipa;
	struct page_item *item;
	struct granule *g_rec;
	struct page_ref *next;
};

struct page_item {
	struct page_ref *refs;
	uint64_t pa;
	// TODO revoke rec on realm destruction
	struct rb_node rb;
};

struct rec;
struct rsi_result;
struct smc_result;

void handle_rsi_set_pages_mergeable(struct rec *rec, struct rsi_result *res);

void smc_reclaim_mergeable_page(unsigned long index, struct smc_result *res);

bool merge_handle_data_destroy(uint64_t pa);

#endif
