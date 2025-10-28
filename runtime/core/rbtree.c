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
#include <utils_def.h>

static void
rb_rotate_left(struct rb_tree *map, struct rb_node *node)
{
	struct rb_node *right = node->h.right;
	node->h.right = right->h.left;
	if (right->h.left) {
		right->h.left->h.parent = node;
	}
	right->h.parent = node->h.parent;
	if (!node->h.parent) {
		map->root = right;
	} else if (node == node->h.parent->h.left) {
		node->h.parent->h.left = right;
	} else {
		node->h.parent->h.right = right;
	}
	right->h.left = node;
	node->h.parent = right;
}

static void
rb_rotate_right(struct rb_tree *map, struct rb_node *node)
{
	struct rb_node *left = node->h.left;
	node->h.left = left->h.right;
	if (left->h.right) {
		left->h.right->h.parent = node;
	}
	left->h.parent = node->h.parent;
	if (!node->h.parent) {
		map->root = left;
	} else if (node == node->h.parent->h.right) {
		node->h.parent->h.right = left;
	} else {
		node->h.parent->h.left = left;
	}
	left->h.right = node;
	node->h.parent = left;
}

static void
rb_insert_fixup(struct rb_tree *map, struct rb_node *node)
{
	while (node->h.parent && node->h.parent->red) {
		if (node->h.parent == node->h.parent->h.parent->h.left) {
			struct rb_node *uncle =
				node->h.parent->h.parent->h.right;
			if (uncle && uncle->red) {
				node->h.parent->red = false;
				uncle->red = false;
				node->h.parent->h.parent->red = true;
				node = node->h.parent->h.parent;
			} else {
				if (node == node->h.parent->h.right) {
					node = node->h.parent;
					rb_rotate_left(map, node);
				}
				node->h.parent->red = false;
				node->h.parent->h.parent->red = true;
				rb_rotate_right(map, node->h.parent->h.parent);
			}
		} else {
			struct rb_node *uncle =
				node->h.parent->h.parent->h.left;
			if (uncle && uncle->red) {
				node->h.parent->red = false;
				uncle->red = false;
				node->h.parent->h.parent->red = true;
				node = node->h.parent->h.parent;
			} else {
				if (node == node->h.parent->h.left) {
					node = node->h.parent;
					rb_rotate_right(map, node);
				}
				node->h.parent->red = false;
				node->h.parent->h.parent->red = true;
				rb_rotate_left(map, node->h.parent->h.parent);
			}
		}
	}
	map->root->red = false;
}

static void
list_insert(struct rb_node *head, struct rb_node *node)
{
	struct rb_node **tail_link = &head->next;
	while (*tail_link) {
		tail_link = &(*tail_link)->next;
	}
	*tail_link = node;
}

static void
list_delete(struct rb_node *item)
{
	struct rb_node **link = &item->c.head->next;
	while (*link) {
		if (*link == item) {
			*link = item->next;
			item->next = NULL;
			return;
		}
		link = &(*link)->next;
	}
}

void
rb_insert(struct rb_tree *map, struct rb_node *new_item)
{
	struct rb_node *parent = NULL;
	struct rb_node **link = &map->root;
	while (*link) {
		parent = *link;
		if (new_item->hash < parent->hash) {
			link = &parent->h.left;
		} else if (new_item->hash > parent->hash) {
			link = &parent->h.right;
		} else {
			new_item->chained = true;
			new_item->c.head = parent;
			map->size++;
			list_insert(parent, new_item);
			return;
		}
	}
	new_item->h.parent = parent;
	new_item->h.left = NULL;
	new_item->h.right = NULL;
	new_item->red = true;
	*link = new_item;
	map->size++;
	rb_insert_fixup(map, new_item);
}

struct rb_node *
rb_find(struct rb_tree *map, uint64_t hash)
{
	struct rb_node *node = map->root;
	while (node) {
		if (hash < node->hash) {
			node = node->h.left;
		} else if (hash > node->hash) {
			node = node->h.right;
		} else {
			return node;
		}
	}
	return NULL;
}

struct rb_node *
rb_min(struct rb_node *node)
{
	if (!node) {
		return NULL;
	}
	while (node->h.left) {
		node = node->h.left;
	}
	return node;
}

static void
rb_transplant(struct rb_tree *map, struct rb_node *u, struct rb_node *v)
{
	if (!u->h.parent) {
		map->root = v;
	} else if (u == u->h.parent->h.left) {
		u->h.parent->h.left = v;
	} else {
		u->h.parent->h.right = v;
	}
	if (v) {
		v->h.parent = u->h.parent;
	}
}

static void
rb_delete_fixup(struct rb_tree *map, struct rb_node *x, struct rb_node *xp)
{
	while ((x != map->root) && (!x || !x->red)) {
		if (x == (xp ? xp->h.left : NULL)) {
			struct rb_node *w = xp->h.right;
			if (w && w->red) {
				w->red = false;
				xp->red = true;
				rb_rotate_left(map, xp);
				w = xp->h.right;
			}
			if ((!w->h.left || !w->h.left->red)
				&& (!w->h.right || !w->h.right->red)) {
				w->red = true;
				x = xp;
				xp = xp->h.parent;
			} else {
				if (!w->h.right || !w->h.right->red) {
					if (w->h.left) {
						w->h.left->red = false;
					}
					w->red = true;
					rb_rotate_right(map, w);
					w = xp->h.right;
				}
				w->red = xp->red;
				xp->red = false;
				if (w->h.right) {
					w->h.right->red = false;
				}
				rb_rotate_left(map, xp);
				x = map->root;
				break;
			}
		} else {
			struct rb_node *w = xp->h.left;
			if (w && w->red) {
				w->red = false;
				xp->red = true;
				rb_rotate_right(map, xp);
				w = xp->h.left;
			}
			if ((!w->h.right || !w->h.right->red)
				&& (!w->h.left || !w->h.left->red)) {
				w->red = true;
				x = xp;
				xp = xp->h.parent;
			} else {
				if (!w->h.left || !w->h.left->red) {
					if (w->h.right) {
						w->h.right->red = false;
					}
					w->red = true;
					rb_rotate_left(map, w);
					w = xp->h.left;
				}
				w->red = xp->red;
				xp->red = false;
				if (w->h.left) {
					w->h.left->red = false;
				}
				rb_rotate_right(map, xp);
				x = map->root;
				break;
			}
		}
	}
	if (x) {
		x->red = false;
	}
}

static void
replace_ref(struct rb_node **ref, struct rb_node *old_node,
		struct rb_node *new_node)
{
	if (*ref == old_node) {
		*ref = new_node;
	}
}

void
rb_delete(struct rb_tree *map, struct rb_node *z)
{
	if (z->chained) {
		list_delete(z);
		*z = (struct rb_node) {.hash = z->hash};
		map->size--;
		return;
	} else if (z->next) {
		if (z->h.parent) {
			replace_ref(&z->h.parent->h.left, z, z->next);
			replace_ref(&z->h.parent->h.right, z, z->next);
		}
		if (z->h.left) {
			replace_ref(&z->h.left->h.parent, z, z->next);
		}
		if (z->h.right) {
			replace_ref(&z->h.right->h.parent, z, z->next);
		}
		for (struct rb_node *n = z->next->next; n; n = n->next) {
			n->c.head = z->next;
		}
		if (map->root == z) {
			map->root = z->next;
		}
		z->next->chained = false;
		z->next->h.parent = z->h.parent;
		z->next->h.left = z->h.left;
		z->next->h.right = z->h.right;
		z->next->red = z->red;
		*z = (struct rb_node) {.hash = z->hash};
		map->size--;
		return;
	}

	struct rb_node *y = z;
	struct rb_node *x;
	struct rb_node *xp;
	int y_red = y->red;
	if (!z->h.left) {
		x = z->h.right;
		xp = z->h.parent;
		rb_transplant(map, z, z->h.right);
	} else if (!z->h.right) {
		x = z->h.left;
		xp = z->h.parent;
		rb_transplant(map, z, z->h.left);
	} else {
		y = rb_min(z->h.right);
		y_red = y->red;
		x = y->h.right;
		if (y->h.parent == z) {
			xp = y;
		} else {
			rb_transplant(map, y, y->h.right);
			y->h.right = z->h.right;
			y->h.right->h.parent = y;
			xp = y->h.parent;
		}
		rb_transplant(map, z, y);
		y->h.left = z->h.left;
		y->h.left->h.parent = y;
		y->red = z->red;
	}
	*z = (struct rb_node) {.hash = z->hash};
	map->size--;
	if (!y_red) {
		rb_delete_fixup(map, x, xp);
	}
}

struct rb_node *
rb_get_next(struct rb_node *node)
{
	if (!node) {
		return NULL;
	}

	if (node->h.right) {
		node = node->h.right;
		while (node->h.left) {
			node = node->h.left;
		}
		return node;
	}

	struct rb_node *parent = node->h.parent;
	while (parent && node == parent->h.right) {
		node = parent;
		parent = parent->h.parent;
	}
	return parent;
}

void
rb_print_node(struct rb_node *root, int space)
{
	if (!root) {
		return;
	}
	space += 15;
	rb_print_node(root->h.right, space);
	for (int i = 15; i < space; i++) {
		NOTICE(" ");
	}
	NOTICE("%lx%c(%8lx)", root->hash, root->chained ? 'c' : 'h', (uint64_t)root);

	for (struct rb_node *chain = root->next; chain; chain = chain->next) {
		NOTICE("-%lx%c(%8lx)", chain->hash, chain->chained ? 'c' : 'h', (uint64_t)chain);
	}
	NOTICE("\n");

	rb_print_node(root->h.left, space);
}
