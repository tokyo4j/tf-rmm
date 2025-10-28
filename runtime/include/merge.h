#ifndef MERGE_H
#define MERGE_H

#include <stdint.h>
#include <stdbool.h>

struct rb_node {
	uint64_t hash;
	union {
		struct {
			struct rb_node *left;
			struct rb_node *right;
			struct rb_node *parent;
		} h;
		struct {
			struct rb_node *head;
		} c;
	};
	struct rb_node *next;
	bool red;
	bool chained;
};

struct rb_tree {
	struct rb_node *root;
	uint64_t size;
};

void rb_insert(struct rb_tree *map, struct rb_node *new_item);
struct rb_node *rb_find(struct rb_tree *map, uint64_t hash);
void rb_delete(struct rb_tree *map, struct rb_node *z);
struct rb_node *rb_get_next(struct rb_node *iter);
struct rb_node *rb_min(struct rb_node *node);
void rb_print_node(struct rb_node *root, int space);

struct page_item {
	uint64_t ipa;
	uint64_t pa;
	uint64_t ns;
	// TODO revoke rec on realm destruction
	struct granule *g_rec;
	struct rb_node rb;
	bool merged;
};

struct rec;
struct rsi_result;
struct smc_result;

void handle_rsi_set_pages_mergeable(struct rec *rec, struct rsi_result *res);

void smc_reclaim_mergeable_page(unsigned long index, struct smc_result *res);

bool merge_handle_data_destroy(uint64_t pa);

#endif
