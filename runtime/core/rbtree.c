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

static inline size_t
_size_of(const struct rb_node *n)
{
	return n ? n->size : 0;
}

/* count nodes in duplicate chain including head */
static size_t
_group_count(const struct rb_node *n)
{
	size_t c = 1;
	const struct rb_node *p = n->dup;
	while (p) {
		c++;
		p = p->dup;
	}
	return c;
}

static void
_recompute_size_up(struct rb_node *n)
{
	while (n) {
		n->size = _group_count(n) + _size_of(n->left)
			+ _size_of(n->right);
		n = n->parent;
	}
}

static void
_left_rotate(struct rb_node **root, struct rb_node *x)
{
	struct rb_node *y = x->right;
	x->right = y->left;
	if (y->left)
		y->left->parent = x;
	y->parent = x->parent;
	if (!x->parent)
		*root = y;
	else if (x == x->parent->left)
		x->parent->left = y;
	else
		x->parent->right = y;
	y->left = x;
	x->parent = y;
	/* update sizes: include duplicate counts for heads */
	x->size = _group_count(x) + _size_of(x->left) + _size_of(x->right);
	y->size = _group_count(y) + _size_of(y->left) + _size_of(y->right);
}

static void
_right_rotate(struct rb_node **root, struct rb_node *y)
{
	struct rb_node *x = y->left;
	y->left = x->right;
	if (x->right)
		x->right->parent = y;
	x->parent = y->parent;
	if (!y->parent)
		*root = x;
	else if (y == y->parent->right)
		y->parent->right = x;
	else
		y->parent->left = x;
	x->right = y;
	y->parent = x;
	/* update sizes */
	y->size = _group_count(y) + _size_of(y->left) + _size_of(y->right);
	x->size = _group_count(x) + _size_of(x->left) + _size_of(x->right);
}

void
rb_insert(struct rb_node **root, struct rb_node *z)
{
	struct rb_node *y = NULL;
	struct rb_node *x = *root;
	/* standard BST insert */
	while (x) {
		y = x;
		x->size += 1; /* increment size along the insertion path */
		if (z->key < x->key) {
			x = x->left;
		} else if (z->key > x->key) {
			x = x->right;
		} else {
			/* duplicate: chain onto x->dup (append to tail) */
			struct rb_node *t = x;
			while (t->dup)
				t = t->dup;
			t->dup = z;
			z->dup = NULL;
			z->left = z->right = NULL;
			z->parent = x; /* parent points to head */
			z->color = RB_RED;
			z->size = 1;
			return;
		}
	}
	z->parent = y;
	z->left = z->right = NULL;
	z->dup = NULL;
	z->color = RB_RED;
	z->size = 1;
	if (!y) {
		*root = z;
	} else if (z->key < y->key) {
		y->left = z;
	} else {
		y->right = z;
	}

	/* fixup */
	while (z->parent && z->parent->color == RB_RED) {
		if (z->parent == z->parent->parent->left) {
			struct rb_node *y = z->parent->parent->right;
			if (y && y->color == RB_RED) {
				z->parent->color = RB_BLACK;
				y->color = RB_BLACK;
				z->parent->parent->color = RB_RED;
				z = z->parent->parent;
			} else {
				if (z == z->parent->right) {
					z = z->parent;
					_left_rotate(root, z);
				}
				z->parent->color = RB_BLACK;
				z->parent->parent->color = RB_RED;
				_right_rotate(root, z->parent->parent);
			}
		} else {
			struct rb_node *y = z->parent->parent->left;
			if (y && y->color == RB_RED) {
				z->parent->color = RB_BLACK;
				y->color = RB_BLACK;
				z->parent->parent->color = RB_RED;
				z = z->parent->parent;
			} else {
				if (z == z->parent->left) {
					z = z->parent;
					_right_rotate(root, z);
				}
				z->parent->color = RB_BLACK;
				z->parent->parent->color = RB_RED;
				_left_rotate(root, z->parent->parent);
			}
		}
	}
	(*root)->color = RB_BLACK;
}

/* transplant u with v */
static void
_transplant(struct rb_node **root, struct rb_node *u, struct rb_node *v)
{
	if (!u->parent)
		*root = v;
	else if (u == u->parent->left)
		u->parent->left = v;
	else
		u->parent->right = v;
	if (v)
		v->parent = u->parent;
}

static struct rb_node *
_minimum(struct rb_node *x)
{
	while (x->left)
		x = x->left;
	return x;
}

void
rb_erase(struct rb_node **root, struct rb_node *z)
{
	/* If z is a duplicate (not part of the binary links), unlink it */
	if (z->parent && z != z->parent->left && z != z->parent->right) {
		struct rb_node *head = z->parent;
		/* find previous in dup list */
		if (head->dup == z) {
			head->dup = z->dup;
		} else {
			struct rb_node *prev = head->dup;
			while (prev && prev->dup != z)
				prev = prev->dup;
			if (prev)
				prev->dup = z->dup;
		}
		if (z->dup)
			z->dup->parent = head;
		/* decrement sizes from head upward */
		struct rb_node *p = head;
		while (p) {
			p->size -= 1;
			p = p->parent;
		}
		return;
	}

	struct rb_node *y = z;
	struct rb_node *x = NULL;
	unsigned char y_original_color = y->color;

	/* Decrement sizes on path from z up to root */
	struct rb_node *p = z;
	while (p) {
		p->size -= 1;
		p = p->parent;
	}

	/* If head has duplicates, promote first duplicate into the tree slot */
	if (z->dup) {
		struct rb_node *d = z->dup; /* first duplicate */
		/* unlink d from z's dup list */
		z->dup = d->dup;
		/* attach remaining dups to d */
		d->dup = z->dup;
		if (d->dup) {
			struct rb_node *q = d->dup;
			while (q) {
				q->parent = d;
				q = q->dup;
			}
		}
		/* transplant d into z's tree position */
		d->parent = z->parent;
		d->left = z->left;
		d->right = z->right;
		d->color = z->color;
		if (d->left)
			d->left->parent = d;
		if (d->right)
			d->right->parent = d;
		if (!z->parent)
			*root = d;
		else if (z == z->parent->left)
			z->parent->left = d;
		else
			z->parent->right = d;
		/* d already has size decreased (we decremented z earlier) so
		 * set to z->size */
		d->size = z->size;
		_recompute_size_up(d);
		return;
	}

	if (!z->left) {
		x = z->right;
		_transplant(root, z, z->right);
	} else if (!z->right) {
		x = z->left;
		_transplant(root, z, z->left);
	} else {
		y = _minimum(z->right);
		y_original_color = y->color;
		x = y->right;
		if (y->parent == z) {
			if (x)
				x->parent = y;
		} else {
			_transplant(root, y, y->right);
			y->right = z->right;
			if (y->right)
				y->right->parent = y;
		}
		_transplant(root, z, y);
		y->left = z->left;
		if (y->left)
			y->left->parent = y;
		y->color = z->color;
		/* fix sizes: y replaces z, give y z's size decremented already
		   but we'll recompute sizes upward from y */
		y->size = y->size; /* placeholder; recompute next */
		_recompute_size_up(y);
	}

	if (y_original_color == RB_BLACK) {
		struct rb_node *w;
		while (x != *root && (!x || x->color == RB_BLACK)) {
			if (x && x->parent && x == x->parent->left) {
				w = x->parent->right;
				if (w && w->color == RB_RED) {
					w->color = RB_BLACK;
					x->parent->color = RB_RED;
					_left_rotate(root, x->parent);
					w = x->parent->right;
				}
				if ((!w->left || w->left->color == RB_BLACK)
					&& (!w->right
						|| w->right->color
							== RB_BLACK)) {
					w->color = RB_RED;
					x = x->parent;
				} else {
					if (!w->right
						|| w->right->color
							== RB_BLACK) {
						if (w->left)
							w->left->color =
								RB_BLACK;
						w->color = RB_RED;
						_right_rotate(root, w);
						w = x->parent->right;
					}
					w->color = x->parent->color;
					x->parent->color = RB_BLACK;
					if (w->right)
						w->right->color = RB_BLACK;
					_left_rotate(root, x->parent);
					x = *root;
				}
			} else if (x && x->parent) {
				/* symmetric */
				w = x->parent->left;
				if (w && w->color == RB_RED) {
					w->color = RB_BLACK;
					x->parent->color = RB_RED;
					_right_rotate(root, x->parent);
					w = x->parent->left;
				}
				if ((!w->right || w->right->color == RB_BLACK)
					&& (!w->left
						|| w->left->color
							== RB_BLACK)) {
					w->color = RB_RED;
					x = x->parent;
				} else {
					if (!w->left
						|| w->left->color == RB_BLACK) {
						if (w->right)
							w->right->color =
								RB_BLACK;
						w->color = RB_RED;
						_left_rotate(root, w);
						w = x->parent->left;
					}
					w->color = x->parent->color;
					x->parent->color = RB_BLACK;
					if (w->left)
						w->left->color = RB_BLACK;
					_right_rotate(root, x->parent);
					x = *root;
				}
			} else {
				break;
			}
		}
		if (x)
			x->color = RB_BLACK;
	}
}

struct rb_node *
rb_first(struct rb_node *root)
{
	if (!root)
		return NULL;
	while (root->left)
		root = root->left;
	return root;
}

struct rb_node *
rb_next(struct rb_node *n)
{
	if (!n)
		return NULL;
	/* if this node is a duplicate (not in binary links) */
	if (n->parent && n != n->parent->left && n != n->parent->right) {
		/* duplicates chain first */
		if (n->dup)
			return n->dup;
		/* otherwise successor is successor of the head in the binary
		 * tree */
		struct rb_node *head = n->parent;
		/* find successor of head in tree (ignore dup lists) */
		struct rb_node *x = head;
		if (x->right) {
			x = x->right;
			while (x->left)
				x = x->left;
			return x;
		}
		struct rb_node *p = x->parent;
		while (p && x == p->right) {
			x = p;
			p = p->parent;
		}
		return p;
	}

	/* duplicates first for tree heads */
	if (n->dup)
		return n->dup;

	/* helper: successor in the binary tree ignoring dup chains */
	struct rb_node *x = n;
	if (x->right) {
		x = x->right;
		while (x->left)
			x = x->left;
		return x;
	}
	struct rb_node *p = x->parent;
	while (p && x == p->right) {
		x = p;
		p = p->parent;
	}
	return p;
}

struct rb_node *
rb_select(struct rb_node *root, size_t index)
{
	struct rb_node *x = root;
	while (x) {
		size_t left_size = _size_of(x->left);
		size_t group = _group_count(x);
		if (index < left_size)
			x = x->left;
		else if (index < left_size + group) {
			/* pick within duplicates */
			size_t off = index - left_size;
			if (off == 0)
				return x;
			struct rb_node *d = x->dup;
			while (off > 1 && d) {
				d = d->dup;
				off--;
			}
			return d;
		} else {
			index = index - left_size - group;
			x = x->right;
		}
	}
	return NULL;
}

size_t
rb_rank(struct rb_node *root, struct rb_node *x)
{
	/* if x is in a duplicate chain (not the head), compute offset */
	size_t offset = 0;
	if (x->parent && x != x->parent->left && x != x->parent->right) {
		struct rb_node *head = x->parent;
		struct rb_node *d = head->dup;
		offset = 1; /* first dup has offset 1 */
		while (d && d != x) {
			d = d->dup;
			offset++;
		}
		/* rank of head + offset */
		size_t r = 0;
		/* compute rank of head (tree node) */
		r = _size_of(head->left);
		struct rb_node *t = head;
		while (t != root) {
			if (t->parent && t == t->parent->right)
				r += _group_count(t->parent) + _size_of(t->parent->left);
			t = t->parent;
		}
		return r + offset;
	}

	size_t r = _size_of(x->left);
	struct rb_node *y = x;
	while (y != root) {
		if (y->parent && y == y->parent->right)
			r += _group_count(y->parent) + _size_of(y->parent->left);
		y = y->parent;
	}
	return r;
}

/* helper: return a uniformly distributed index in [0, n). Uses rand()
   and combines calls to get more bits when needed. Caller must seed RNG.
*/
/* xorshift64 PRNG state (file-local). Use a fixed initial seed for
   deterministic sequences across runs. */
static uint64_t _xorshift_state = 88172645463325252ULL;

static inline uint64_t
_xorshift64(void)
{
	uint64_t x = _xorshift_state;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	_xorshift_state = x;
	return x;
}

static size_t
_rand_index(size_t n)
{
	if (n == 0)
		return 0;
	uint64_t r = _xorshift64();
	return (size_t)(r % n);
}

struct rb_node *
rb_random(struct rb_node *root)
{
	if (!root)
		return NULL;
	size_t total = root->size;
	size_t idx = _rand_index(total);
	return rb_select(root, idx);
}

struct rb_node *
rb_find(struct rb_node *root, uint64_t key)
{
	struct rb_node *x = root;
	while (x) {
		if (key < x->key)
			x = x->left;
		else if (key > x->key)
			x = x->right;
		else
			return x; /* head node; duplicates on x->dup */
	}
	return NULL;
}
