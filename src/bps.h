/* BPS patches (byuu's format, as bsnes-hd's widescreen patches come). */
#ifndef SMDS_BPS_H
#define SMDS_BPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

uint32_t bps_crc32(const uint8_t* d, size_t n);
/* The source's CRC32 the patch is for (false if it isn't a BPS patch). */
bool bps_source_crc(const uint8_t* patch, size_t plen, uint32_t* crc);
/* The patched data, malloc'd (NULL and why on a bad patch or source). */
uint8_t* bps_apply(const uint8_t* src, size_t srcLen, const uint8_t* patch, size_t plen, size_t* outLen, const char** why);

#endif
