#include "bps.h"

#include <stdlib.h>
#include <string.h>

uint32_t bps_crc32(const uint8_t* d, size_t n) {
	static uint32_t table[256];
	if (!table[1]) {
		for (uint32_t i = 0; i < 256; ++i) {
			uint32_t c = i;
			for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
	}
	uint32_t c = 0xFFFFFFFFu;
	for (size_t i = 0; i < n; ++i) c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
	return c ^ 0xFFFFFFFFu;
}

static bool num(const uint8_t* p, size_t end, size_t* i, uint64_t* v) {
	uint64_t r = 0, shift = 1;
	for (;;) {
		if (*i >= end || shift > ((uint64_t) 1 << 56)) return false;
		uint8_t x = p[(*i)++];
		r += (x & 0x7F) * shift;
		if (x & 0x80) break;
		shift <<= 7;
		r += shift;
	}
	*v = r;
	return true;
}

static uint32_t le32(const uint8_t* p) {
	return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}

bool bps_source_crc(const uint8_t* patch, size_t plen, uint32_t* crc) {
	if (plen < 16 || memcmp(patch, "BPS1", 4)) return false;
	*crc = le32(patch + plen - 12);
	return true;
}

uint8_t* bps_apply(const uint8_t* src, size_t srcLen, const uint8_t* p, size_t plen, size_t* outLen, const char** why) {
	*why = "not a BPS patch";
	if (plen < 16 || memcmp(p, "BPS1", 4)) return NULL;
	size_t end = plen - 12, i = 4;
	uint64_t ss, ts, ms;
	if (!num(p, end, &i, &ss) || !num(p, end, &i, &ts) || !num(p, end, &i, &ms) || i + ms > end) return NULL;
	i += ms;
	*why = "the patch is for another ROM";
	if (ss != srcLen || le32(p + end) != bps_crc32(src, srcLen)) return NULL;
	*why = "a damaged patch";
	if (le32(p + plen - 4) != bps_crc32(p, plen - 4) || ts > (64u << 20)) return NULL;
	uint8_t* out = calloc(1, ts ? ts : 1);
	if (!out) {
		*why = "out of memory";
		return NULL;
	}
	size_t o = 0;
	int64_t sr = 0, tr = 0;
	while (i < end) {
		uint64_t d;
		if (!num(p, end, &i, &d)) goto bad;
		uint64_t len = (d >> 2) + 1;
		if (o + len > ts) goto bad;
		switch (d & 3) {
		case 0: /* the source's bytes, same place */
			if (o + len > srcLen) goto bad;
			memcpy(out + o, src + o, len);
			break;
		case 1: /* bytes in the patch */
			if (i + len > end) goto bad;
			memcpy(out + o, p + i, len);
			i += len;
			break;
		default: { /* from elsewhere in the source / the output */
			uint64_t off;
			if (!num(p, end, &i, &off)) goto bad;
			int64_t delta = (int64_t) (off >> 1) * (off & 1 ? -1 : 1);
			if ((d & 3) == 2) {
				sr += delta;
				if (sr < 0 || (uint64_t) sr + len > srcLen) goto bad;
				memcpy(out + o, src + sr, len);
				sr += (int64_t) len;
			} else {
				tr += delta;
				if (tr < 0) goto bad;
				for (uint64_t k = 0; k < len; ++k) {
					if ((uint64_t) tr >= o + k) goto bad;
					out[o + k] = out[tr++];
				}
			}
			break;
		}
		}
		o += len;
	}
	if (o != ts || le32(p + end + 4) != bps_crc32(out, ts)) goto bad;
	*outLen = ts;
	*why = NULL;
	return out;
bad:
	free(out);
	*why = "a damaged patch";
	return NULL;
}
