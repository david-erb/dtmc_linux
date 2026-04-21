/*
 * dtsha1 -- Standalone SHA-1 digest with incremental and one-shot APIs.
 *
 * Provides init, update, and final functions over a small context struct for
 * incremental hashing of arbitrarily sized inputs, plus a convenience one-shot
 * helper for single-buffer inputs. Produces a 20-byte digest. Written without
 * external dependencies for portability across embedded and hosted targets;
 * used internally for WebSocket handshake key computation.
 *
 * cdox v1.0.2
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct dtsha1_ctx_t
{
    uint32_t state[5];
    uint64_t total_len_bytes;
    uint8_t buffer[64];
    size_t buffer_len;
} dtsha1_ctx_t;

// Initialize SHA-1 context
void
dtsha1_init(dtsha1_ctx_t* ctx);

// Feed bytes into the hash
void
dtsha1_update(dtsha1_ctx_t* ctx, const void* data, size_t len);

// Finalize and write 20-byte digest
void
dtsha1_final(dtsha1_ctx_t* ctx, uint8_t out_digest[20]);

// Convenience one-shot helper
void
dtsha1_compute(const void* data, size_t len, uint8_t out_digest[20]);
