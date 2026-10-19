/* Rewinding: the game's recent past as a chain of save states, each kept
 * as its difference from the one after it (see rewind.h). */
#include "rewind.h"

#include <stdlib.h>
#include <string.h>

struct Rewind {
	size_t stateSize, budget, used;
	uint8_t* cur;  /* the newest state pushed (or the last one popped) */
	uint8_t* diff; /* scratch: cur XOR the next state */
	bool haveCur;
	/* the differences, oldest first, in a circular array */
	uint8_t** entries;
	size_t* sizes;
	size_t cap, head, count;
};

Rewind* rewind_create(size_t stateSize, size_t budgetBytes) {
	Rewind* r = calloc(1, sizeof(*r));
	if (!r) return NULL;
	r->stateSize = stateSize;
	r->budget = budgetBytes;
	r->cap = 1024;
	r->cur = malloc(stateSize);
	r->diff = malloc(stateSize);
	r->entries = malloc(r->cap * sizeof(*r->entries));
	r->sizes = malloc(r->cap * sizeof(*r->sizes));
	if (!r->cur || !r->diff || !r->entries || !r->sizes) {
		rewind_destroy(r);
		return NULL;
	}
	return r;
}

void rewind_clear(Rewind* r) {
	if (!r) return;
	for (size_t i = 0; i < r->count; ++i) free(r->entries[(r->head + i) % r->cap]);
	r->head = r->count = r->used = 0;
	r->haveCur = false;
}

void rewind_destroy(Rewind* r) {
	if (!r) return;
	rewind_clear(r);
	free(r->cur);
	free(r->diff);
	free(r->entries);
	free(r->sizes);
	free(r);
}

size_t rewind_count(const Rewind* r) {
	return r ? r->count : 0;
}

/* A difference (a XOR b) as runs: [u32 zeros][u32 n][n bytes]..., the
 * zeros being bytes where the two states agree (most of them). */
static uint8_t* encode(const uint8_t* x, size_t n, size_t* outSize) {
	/* worst case: everything differs, one run */
	size_t cap = n + 8 + 64, o = 0;
	uint8_t* out = malloc(cap);
	if (!out) return NULL;
	size_t i = 0;
	while (i < n) {
		size_t z = i;
		while (z + 8 <= n) {
			uint64_t w;
			memcpy(&w, x + z, 8);
			if (w) break;
			z += 8;
		}
		while (z < n && !x[z]) ++z;
		size_t l = z;
		/* a literal run ends at 16 zero bytes in a row (shorter gaps cost
		 * less kept inside it) */
		size_t zr = 0;
		while (l < n) {
			if (x[l]) zr = 0;
			else if (++zr >= 16) {
				l -= 15;
				break;
			}
			++l;
		}
		if (l > n) l = n;
		if (l == n) {
			while (l > z && !x[l - 1]) --l; /* trailing zeros aren't literal */
		}
		uint32_t zeros = (uint32_t) (z - i), lit = (uint32_t) (l - z);
		if (o + 8 + lit > cap) {
			cap = (o + 8 + lit) * 2;
			uint8_t* grown = realloc(out, cap);
			if (!grown) {
				free(out);
				return NULL;
			}
			out = grown;
		}
		memcpy(out + o, &zeros, 4);
		memcpy(out + o + 4, &lit, 4);
		memcpy(out + o + 8, x + z, lit);
		o += 8 + lit;
		i = l;
		if (!lit) break; /* only zeros left */
	}
	uint8_t* shrunk = realloc(out, o ? o : 1);
	*outSize = o;
	return shrunk ? shrunk : out;
}

/* cur ^= the difference */
static void apply(uint8_t* cur, size_t n, const uint8_t* d, size_t size) {
	size_t i = 0, o = 0;
	while (o + 8 <= size) {
		uint32_t zeros, lit;
		memcpy(&zeros, d + o, 4);
		memcpy(&lit, d + o + 4, 4);
		i += zeros;
		if (i + lit > n || o + 8 + lit > size) return;
		for (uint32_t k = 0; k < lit; ++k) cur[i + k] ^= d[o + 8 + k];
		i += lit;
		o += 8 + lit;
	}
}

static void drop_oldest(Rewind* r) {
	free(r->entries[r->head]);
	r->used -= r->sizes[r->head];
	r->head = (r->head + 1) % r->cap;
	--r->count;
}

void rewind_push(Rewind* r, const void* state) {
	if (!r) return;
	const uint8_t* s = state;
	if (r->haveCur) {
		const uint64_t* a = (const uint64_t*) r->cur;
		const uint64_t* b = (const uint64_t*) s;
		uint64_t* d = (uint64_t*) r->diff;
		size_t words = r->stateSize / 8;
		for (size_t i = 0; i < words; ++i) {
			uint64_t x, y;
			memcpy(&x, a + i, 8);
			memcpy(&y, b + i, 8);
			x ^= y;
			memcpy(d + i, &x, 8);
		}
		for (size_t i = words * 8; i < r->stateSize; ++i) r->diff[i] = r->cur[i] ^ s[i];
		size_t size;
		uint8_t* e = encode(r->diff, r->stateSize, &size);
		if (e) {
			if (r->count == r->cap) {
				/* grow the circular array, oldest first again */
				size_t nc = r->cap * 2;
				uint8_t** ne = malloc(nc * sizeof(*ne));
				size_t* ns = malloc(nc * sizeof(*ns));
				if (ne && ns) {
					for (size_t i = 0; i < r->count; ++i) {
						ne[i] = r->entries[(r->head + i) % r->cap];
						ns[i] = r->sizes[(r->head + i) % r->cap];
					}
					free(r->entries);
					free(r->sizes);
					r->entries = ne, r->sizes = ns, r->cap = nc, r->head = 0;
				} else {
					free(ne);
					free(ns);
					drop_oldest(r);
				}
			}
			size_t at = (r->head + r->count) % r->cap;
			r->entries[at] = e;
			r->sizes[at] = size;
			++r->count;
			r->used += size;
			while (r->count > 1 && r->used > r->budget) drop_oldest(r);
		} else {
			rewind_clear(r); /* out of memory: start again from here */
		}
	}
	memcpy(r->cur, s, r->stateSize);
	r->haveCur = true;
}

bool rewind_pop(Rewind* r, void* state) {
	if (!r || !r->count) return false;
	size_t at = (r->head + r->count - 1) % r->cap;
	apply(r->cur, r->stateSize, r->entries[at], r->sizes[at]);
	free(r->entries[at]);
	r->used -= r->sizes[at];
	--r->count;
	memcpy(state, r->cur, r->stateSize);
	return true;
}

size_t rewind_bytes(const Rewind* r) {
	return r ? r->used : 0;
}

bool rewind_current(const Rewind* r, void* state) {
	if (!r || !r->haveCur) return false;
	memcpy(state, r->cur, r->stateSize);
	return true;
}
