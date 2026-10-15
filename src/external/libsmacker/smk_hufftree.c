/**
	libsmacker - A C library for decoding .smk Smacker Video files
	Copyright (C) 2012-2017 Greg Kennedy

	See smacker.h for more information.

	smk_hufftree.c
		Implementation of Smacker Huffman coding trees.
*/

#include "smk_hufftree.h"

/* malloc and friends */
#include "smk_malloc.h"

/**
	8-bit Tree node structure.
	If b0 is non-null, this is a branch, and b1 from the union should be used.
	If b0 is null, this is a leaf, and val / escape code from union should be used.
*/
struct smk_huff8_t
{
	struct smk_huff8_t* b0;
	union
	{
		struct smk_huff8_t* b1;
		struct
		{
			uint16_t value;
			uint8_t escapecode;
		} leaf;
	} u;
};

/**
	16-bit Tree root struct: holds a huff8_t structure,
	as well as a cache of three 16-bit values.
*/
struct smk_huff16_t
{
	struct smk_huff8_t* t;
	uint16_t cache[3];
	struct smk_node_pool* pool; /* Added: where t's nodes come from */
	uint32_t* table; /* Added: lookup tables (see smk_huff16_table) */
	uint32_t table_len;
};

/* Added: a 16-bit tree is decoded through multi-level lookup tables instead
   of a walk down the tree one bit at a time: the root table is indexed by
   the next SMK_ROOT_BITS bits of the stream (LSB first, the order Smacker
   reads them), and codes longer than that continue in SMK_SUB_BITS bit
   subtables. Entries:
     leaf:     value (bits 0-15) | code length within the level (16-20)
               | escape code (21-22; 3 = none)
     subtable: SMK_SUB_FLAG | index of the subtable in `table`
   The walk took most of a frame's decode time on an ESP32-P4, where the
   ~300k nodes of a movie's trees live in PSRAM. */
#define SMK_ROOT_BITS 12
#define SMK_SUB_BITS 3
#define SMK_SUB_FLAG 0x80000000u
#define SMK_ESC_NONE 3u

/* Added: a 16-bit tree's nodes come from chunks rather than a calloc() each:
   a movie's trees hold some 300k nodes, and a heap allocation apiece took
   ~10 s to build them on an ESP32-P4 (PSRAM heap); a chunk also has no
   per-node heap header. */
#define SMK_POOL_NODES 8192
struct smk_node_pool
{
	struct smk_node_pool* next;
	uint32_t used;
	struct smk_huff8_t nodes[SMK_POOL_NODES];
};
static struct smk_huff8_t* smk_pool_node(struct smk_node_pool** pp)
{
	struct smk_node_pool* p = *pp;
	if (!p || p->used == SMK_POOL_NODES)
	{
		smk_malloc(p, sizeof(struct smk_node_pool)); /* zeroed */
		p->next = *pp;
		*pp = p;
	}
	return &p->nodes[p->used++];
}
static void smk_pool_free(struct smk_node_pool* p)
{
	while (p)
	{
		struct smk_node_pool* next = p->next;
		free(p);
		p = next;
	}
}

/* Added: the next 25+ bits of the stream, LSB first (zeros past the end) */
static inline uint32_t smk_bs_peek(const struct smk_bit_t* bs)
{
	const uint8_t* b = bs->buffer + bs->byte_num;
	uint32_t v;
	if (bs->byte_num + 4 <= bs->size)
	{
		v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
	}
	else
	{
		uint32_t i;
		v = 0;
		for (i = 0; bs->byte_num + i < bs->size && i < 4; i ++)
			v |= (uint32_t)b[i] << (8 * i);
	}
	return v >> bs->bit_num;
}

static inline void smk_bs_skip(struct smk_bit_t* bs, uint32_t n)
{
	uint32_t bit = bs->bit_num + n;
	bs->byte_num += bit >> 3;
	bs->bit_num = bit & 7;
}

/*********************** 8-BIT HUFF-TREE FUNCTIONS ***********************/
/** safe build with built-in error jump */
#define smk_huff8_build_rec(bs,p) \
{ \
	if (!(p = _smk_huff8_build_rec(bs))) \
	{ \
		fprintf(stderr, "libsmacker::smk_huff8_build_rec(" #bs ", " #p ") - ERROR (file: %s, line: %lu)\n", __FILE__, (unsigned long)__LINE__); \
		goto error; \
	} \
}
/** Recursive tree-building function. */
static struct smk_huff8_t* _smk_huff8_build_rec(struct smk_bit_t* bs)
{
	struct smk_huff8_t* ret = NULL;
	char bit;

	/* sanity check - removed: bs cannot be null, because it was checked at smk_huff8_build below */
	/* smk_assert(bs); */

	/* Read the bit */
	smk_bs_read_1(bs, bit);

	/* Malloc a structure. */
	smk_malloc(ret, sizeof(struct smk_huff8_t));

	if (bit)
	{
		/* Bit set: this forms a Branch node. */
		/* Recursively attempt to build the Left branch. */
		smk_huff8_build_rec(bs, ret->b0);

		/* Everything is still OK: attempt to build the Right branch. */
		smk_huff8_build_rec(bs, ret->u.b1);

		/* return branch pointer here */
		return ret;
	}

	/* Bit unset signifies a Leaf node. */
	/* Attempt to read value */
	smk_bs_read_8(bs, ret->u.leaf.value);

	/* smk_malloc sets entries to 0 by default */
	/* ret->b0 = NULL; */
	ret->u.leaf.escapecode = 0xFF;

	return ret;

error:
	/* In case of error, undo the subtree we were building, and return NULL. */
	smk_huff8_free(ret);
	return NULL;
}

/* Look up an 8-bit value from a basic huff tree.
	Return -1 on error.
	Added: walks with bits peeked from the stream, skipping them at the end,
	instead of reading (and recursing) one bit at a time. */
FAST_FUNC int16_t _smk_huff8_lookup(struct smk_bit_t* bs, const struct smk_huff8_t* t)
{
	uint32_t v = smk_bs_peek(bs);
	uint32_t n = 0;

	while (t->b0)
	{
		if (n == 25)
		{
			smk_bs_skip(bs, n);
			v = smk_bs_peek(bs);
			n = 0;
		}
		t = (v & 1) ? t->u.b1 : t->b0;
		v >>= 1;
		n ++;
	}
	smk_bs_skip(bs, n);
	return t->u.leaf.value;
}

/**
	Entry point for huff8 build. Basically just checks the start/end tags
	and calls smk_huff8_build_rec recursive function.
*/
struct smk_huff8_t* _smk_huff8_build(struct smk_bit_t* bs)
{
	struct smk_huff8_t* ret = NULL;
	char bit;

	/* sanity check */
	smk_assert(bs);

	/* Smacker huff trees begin with a set-bit. */
	smk_bs_read_1(bs, bit);

	if (!bit)
	{
		/* Got a bit, but it was not 1. In theory, there could be a smk file
			without this particular tree. */
		fputs("libsmacker::_smk_huff8_build(bs) - Warning: initial get_bit returned 0\n", stderr);
		goto error;
	}

	/* Begin parsing the tree data. */
	smk_huff8_build_rec(bs, ret);

	/* huff trees end with an unset-bit */
	smk_bs_read_1(bs, bit);

	if (bit)
	{
		fputs("libsmacker::_smk_huff8_build(bs) - ERROR: final get_bit returned 1\n", stderr);
		goto error;
	}

	return ret;

error:
	smk_huff8_free(ret);
	return NULL;
}

/* function to recursively delete a huffman tree */
void smk_huff8_free(struct smk_huff8_t* t)
{
	/* Sanity check: do not double-free */
	smk_assert(t);

	/* If this is not a leaf node, free child trees first */
	if (t->b0)
	{
		smk_huff8_free(t->b0);
		smk_huff8_free(t->u.b1);
	}

	/* Safe-delete tree node. */
	smk_free(t);

error: ;
}

/*********************** 16-BIT HUFF-TREE FUNCTIONS ***********************/
/* safe bigtree build with built-in error jump */
#define smk_huff16_build_rec(bs,cache,low8,hi8,pool,p) \
{ \
	if (!(p = _smk_huff16_build_rec(bs, cache, low8, hi8, pool))) \
	{ \
		fprintf(stderr, "libsmacker::smk_huff16_build_rec(" #bs ", " #cache ", " #low8 ", " #hi8 ", " #p ") - ERROR (file: %s, line: %lu)\n", __FILE__, (unsigned long)__LINE__); \
		goto error; \
	} \
}
/* Recursively builds a Big tree. */
static struct smk_huff8_t* _smk_huff16_build_rec(struct smk_bit_t* bs, const uint16_t cache[3], const struct smk_huff8_t* low8, const struct smk_huff8_t* hi8, struct smk_node_pool** pool)
{
	struct smk_huff8_t* ret = NULL;

	char bit;
	int16_t lowval;

	/* sanity check - removed: these cannot be null, because they were checked at smk_huff16_build below */
	/* smk_assert(bs);
	smk_assert(cache);
	smk_assert(low8);
	smk_assert(hi8); */

	/* Get the first bit */
	smk_bs_read_1(bs, bit);

	/* A node from the tree's pool (zeroed). */
	ret = smk_pool_node(pool);

	if (bit)
	{
		/* Recursively attempt to build the Left branch. */
		smk_huff16_build_rec(bs, cache, low8, hi8, pool, ret->b0);

		/* Recursively attempt to build the Left branch. */
		smk_huff16_build_rec(bs, cache, low8, hi8, pool, ret->u.b1);

		/* return branch pointer here */
		return ret;
	}

	/* Bit unset signifies a Leaf node. */
	smk_huff8_lookup(bs, low8, lowval);
	smk_huff8_lookup(bs, hi8, ret->u.leaf.value);

	/* Looks OK: we got low and hi values. Return a new LEAF */
	/* ret->b0 = NULL; */
	ret->u.leaf.value = lowval | (ret->u.leaf.value << 8);

	/* Last: when building the tree, some Values may correspond to cache positions.
		Identify these values and set the Escape code byte accordingly. */
	if (ret->u.leaf.value == cache[0])
	{
		ret->u.leaf.escapecode = 0;
	}
	else if (ret->u.leaf.value == cache[1])
	{
		ret->u.leaf.escapecode = 1;
	}
	else if (ret->u.leaf.value == cache[2])
	{
		ret->u.leaf.escapecode = 2;
	}
	else
	{
		ret->u.leaf.escapecode = 0xFF;
	}

	return ret;

error:
	/* (the pool's nodes go with the tree) */
	return NULL;
}

/* Added: the number of subtable entries the subtree `node` needs, reached
   at `depth` bits into a `bits` wide level */
static uint32_t smk_huff16_table_count(const struct smk_huff8_t* node, uint32_t bits, uint32_t depth)
{
	if (!node->b0)
		return 0;
	if (depth == bits)
		return (1u << SMK_SUB_BITS) + smk_huff16_table_count(node, SMK_SUB_BITS, 0);
	return smk_huff16_table_count(node->b0, bits, depth + 1) + smk_huff16_table_count(node->u.b1, bits, depth + 1);
}

/* Added: fill the level at `off` (`bits` wide) with the subtree `node`,
   reached by `code` (`depth` bits) within the level. */
static int smk_huff16_table_fill(struct smk_huff16_t* big, uint32_t off, uint32_t bits, const struct smk_huff8_t* node, uint32_t depth, uint32_t code)
{
	if (!node->b0)
	{
		const uint32_t esc = node->u.leaf.escapecode <= 2 ? node->u.leaf.escapecode : SMK_ESC_NONE;
		const uint32_t e = node->u.leaf.value | (depth << 16) | (esc << 21);
		uint32_t r;
		for (r = 0; r < (1u << (bits - depth)); r ++)
			big->table[off + (code | (r << depth))] = e;
		return 0;
	}
	if (depth == bits)
	{
		const uint32_t sub = big->table_len;
		big->table_len += 1u << SMK_SUB_BITS;
		big->table[off + code] = SMK_SUB_FLAG | sub;
		return smk_huff16_table_fill(big, sub, SMK_SUB_BITS, node, 0, 0);
	}
	if (smk_huff16_table_fill(big, off, bits, node->b0, depth + 1, code)) return -1;
	return smk_huff16_table_fill(big, off, bits, node->u.b1, depth + 1, code | (1u << depth));
}

/* Added: build the lookup tables from the tree, then drop the tree */
static int smk_huff16_table(struct smk_huff16_t* big)
{
	const uint32_t n = (1u << SMK_ROOT_BITS) + smk_huff16_table_count(big->t, SMK_ROOT_BITS, 0);
	big->table = (uint32_t*)malloc(n * sizeof(uint32_t));
	if (!big->table) return -1;
	big->table_len = 1u << SMK_ROOT_BITS;
	if (smk_huff16_table_fill(big, 0, SMK_ROOT_BITS, big->t, 0, 0)) return -1;
	smk_pool_free(big->pool);
	big->pool = NULL;
	big->t = NULL;
	return 0;
}

/* Entry point for building a big 16-bit tree. */
struct smk_huff16_t* _smk_huff16_build(struct smk_bit_t* bs)
{
	struct smk_huff16_t* big = NULL;

	struct smk_huff8_t* low8 = NULL;
	struct smk_huff8_t* hi8 = NULL;

	int16_t lowval;

	char bit;
	uint8_t i;

	/* sanity check */
	smk_assert(bs);

	/* Smacker huff trees begin with a set-bit. */
	smk_bs_read_1(bs, bit);

	if (!bit)
	{
		fputs("libsmacker::smk_huff16_build(bs) - ERROR: initial get_bit returned 0\n", stderr);
		goto error;
	}

	/* build low-8-bits tree */
	smk_huff8_build(bs, low8);
	/* build hi-8-bits tree */
	smk_huff8_build(bs, hi8);

	/* Everything looks OK so far. Time to malloc structure. */
	smk_malloc(big, sizeof(struct smk_huff16_t));

	/* Init the escape code cache. */
	for (i = 0; i < 3; i ++)
	{
		smk_bs_read_8(bs, lowval);
		smk_bs_read_8(bs, big->cache[i]);
		big->cache[i] = lowval | (big->cache[i] << 8);
	}

	/* Finally, call recursive function to retrieve the Bigtree. */
	smk_huff16_build_rec(bs, big->cache, low8, hi8, &big->pool, big->t);

	/* Done with 8-bit hufftrees, free them. */
	smk_huff8_free(hi8);
	smk_huff8_free(low8);

	/* Check final end tag. */
	smk_bs_read_1(bs, bit);

	if (bit)
	{
		fputs("libsmacker::smk_huff16_build(bs) - ERROR: final get_bit returned 1\n", stderr);
		goto error;
	}

	/* Added: decode through lookup tables */
	if (smk_huff16_table(big))
	{
		fputs("libsmacker::smk_huff16_build(bs) - ERROR: out of memory for the lookup table\n", stderr);
		goto error;
	}

	return big;

error:
	smk_huff16_free(big);
	smk_huff8_free(hi8);
	smk_huff8_free(low8);
	return NULL;
}

static uint16_t _smk_huff16_lookup_rec(struct smk_bit_t* bs, uint16_t cache[3], const struct smk_huff8_t* t)
{
	uint16_t val;
	char bit;

	/* sanity check */
	/* smk_assert(bs);
	smk_assert(cache);
	smk_assert(t); */

	/* Reached a Leaf node */
	if (!t->b0)
	{
		if (t->u.leaf.escapecode != 0xFF)
		{
			/* Found escape code. Retrieve value from Cache. */
			val = cache[t->u.leaf.escapecode];
		}
		else
		{
			/* Use value directly. */
			val = t->u.leaf.value;
		}

		if (cache[0] != val)
		{
			/* Update the cache, by moving val to the front of the queue,
				if it isn't already there. */
			cache[2] = cache[1];
			cache[1] = cache[0];
			cache[0] = val;
		}

		return val;
	}

	/* Read the next bit from bitstream to determine path */
	smk_bs_read_1(bs, bit);

	if (bit)
	{
		/* get_bit returned Set, follow Right branch. */
		return _smk_huff16_lookup_rec(bs, cache, t->u.b1);
	}

	return _smk_huff16_lookup_rec(bs, cache, t->b0);

error:
	return -1;
}

/* Bigtree lookup (Added: through the lookup tables) */
FAST_FUNC uint16_t _smk_huff16_lookup(struct smk_bit_t* bs, struct smk_huff16_t* big)
{
	const uint32_t* table = big->table;
	uint32_t bits = SMK_ROOT_BITS;
	uint32_t off = 0;
	uint32_t e, esc;
	uint16_t val;

	for (;;)
	{
		e = table[off + (smk_bs_peek(bs) & ((1u << bits) - 1))];
		if (!(e & SMK_SUB_FLAG))
			break;
		smk_bs_skip(bs, bits);
		off = e & ~SMK_SUB_FLAG;
		bits = SMK_SUB_BITS;
	}
	smk_bs_skip(bs, (e >> 16) & 0x1F);

	esc = (e >> 21) & 3;
	val = esc == SMK_ESC_NONE ? (uint16_t)e : big->cache[esc];
	if (big->cache[0] != val)
	{
		big->cache[2] = big->cache[1];
		big->cache[1] = big->cache[0];
		big->cache[0] = val;
	}
	return val;
}

/* Resets a Big hufftree cache */
void smk_huff16_reset(struct smk_huff16_t* big)
{
	/* sanity check */
	smk_assert(big);

	big->cache[0] = 0;
	big->cache[1] = 0;
	big->cache[2] = 0;

error: ;
}

/* delete a (big) huffman tree */
void smk_huff16_free(struct smk_huff16_t* big)
{
	/* Sanity check: do not double-free */
	smk_assert(big);
 
	/* free the subtree: its nodes are the pool's */
	smk_pool_free(big->pool);
	free(big->table);
	big->table = NULL;
	big->pool = NULL;
	big->t = NULL;

	/* free the bigtree */
	smk_free(big);

error: ;
};
