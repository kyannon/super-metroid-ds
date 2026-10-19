/* Rewinding: save states pushed every few frames, kept as differences from
 * one to the next (a few kilobytes each instead of the state's 800), the
 * oldest dropped past a memory budget; popped newest first. */
#ifndef SMDS_REWIND_H
#define SMDS_REWIND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Rewind Rewind;

Rewind* rewind_create(size_t stateSize, size_t budgetBytes);
void rewind_destroy(Rewind* r);
/* Forget everything (after loading a state or a reset). */
void rewind_clear(Rewind* r);
/* The state now (stateSize bytes). */
void rewind_push(Rewind* r, const void* state);
/* The state pushed before the newest one, into state; false when there's
 * nothing further back. It becomes the newest. */
bool rewind_pop(Rewind* r, void* state);
/* The newest state (what a failed pop leaves): false before any push. */
bool rewind_current(const Rewind* r, void* state);
size_t rewind_count(const Rewind* r);
size_t rewind_bytes(const Rewind* r);

#endif
