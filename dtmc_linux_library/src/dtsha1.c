#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <dtmc/dtsha1.h>

// --------------------------------------------------------------------------------------------

static uint32_t
_dtsha1_rotl32(uint32_t x, uint32_t n)
{
    return (x << n) | (x >> (32 - n));
}

// --------------------------------------------------------------------------------------------

static uint32_t
_dtsha1_load_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | ((uint32_t)p[3]);
}

// --------------------------------------------------------------------------------------------

static void
_dtsha1_store_be64(uint8_t* p, uint64_t x)
{
    p[0] = (uint8_t)((x >> 56) & 0xFF);
    p[1] = (uint8_t)((x >> 48) & 0xFF);
    p[2] = (uint8_t)((x >> 40) & 0xFF);
    p[3] = (uint8_t)((x >> 32) & 0xFF);
    p[4] = (uint8_t)((x >> 24) & 0xFF);
    p[5] = (uint8_t)((x >> 16) & 0xFF);
    p[6] = (uint8_t)((x >> 8) & 0xFF);
    p[7] = (uint8_t)(x & 0xFF);
}

// --------------------------------------------------------------------------------------------

static void
_dtsha1_store_be32(uint8_t* p, uint32_t x)
{
    p[0] = (uint8_t)((x >> 24) & 0xFF);
    p[1] = (uint8_t)((x >> 16) & 0xFF);
    p[2] = (uint8_t)((x >> 8) & 0xFF);
    p[3] = (uint8_t)(x & 0xFF);
}

// --------------------------------------------------------------------------------------------

static void
_dtsha1_process_block(dtsha1_ctx_t* ctx, const uint8_t block[64])
{
    uint32_t w[80];
    uint32_t a, b, c, d, e;
    uint32_t f, k, temp;
    int t;

    for (t = 0; t < 16; ++t)
    {
        w[t] = _dtsha1_load_be32(block + (t * 4));
    }

    for (t = 16; t < 80; ++t)
    {
        w[t] = _dtsha1_rotl32(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];

    for (t = 0; t < 80; ++t)
    {
        if (t < 20)
        {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        }
        else if (t < 40)
        {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        }
        else if (t < 60)
        {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        }
        else
        {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }

        temp = _dtsha1_rotl32(a, 5) + f + e + k + w[t];
        e = d;
        d = c;
        c = _dtsha1_rotl32(b, 30);
        b = a;
        a = temp;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
}

// --------------------------------------------------------------------------------------------

void
dtsha1_init(dtsha1_ctx_t* ctx)
{
    if (!ctx)
        return;

    ctx->state[0] = 0x67452301u;
    ctx->state[1] = 0xEFCDAB89u;
    ctx->state[2] = 0x98BADCFEu;
    ctx->state[3] = 0x10325476u;
    ctx->state[4] = 0xC3D2E1F0u;
    ctx->total_len_bytes = 0;
    ctx->buffer_len = 0;
}

// --------------------------------------------------------------------------------------------

void
dtsha1_update(dtsha1_ctx_t* ctx, const void* data, size_t len)
{
    const uint8_t* p = (const uint8_t*)data;

    if (!ctx || (!data && len > 0))
        return;

    ctx->total_len_bytes += (uint64_t)len;

    while (len > 0)
    {
        size_t to_copy = sizeof(ctx->buffer) - ctx->buffer_len;
        if (to_copy > len)
            to_copy = len;

        memcpy(ctx->buffer + ctx->buffer_len, p, to_copy);
        ctx->buffer_len += to_copy;
        p += to_copy;
        len -= to_copy;

        if (ctx->buffer_len == sizeof(ctx->buffer))
        {
            _dtsha1_process_block(ctx, ctx->buffer);
            ctx->buffer_len = 0;
        }
    }
}

// --------------------------------------------------------------------------------------------

void
dtsha1_final(dtsha1_ctx_t* ctx, uint8_t out_digest[20])
{
    uint64_t total_bits;
    uint8_t len_block[8];
    uint8_t pad_byte = 0x80;
    uint8_t zero = 0x00;
    int i;

    if (!ctx || !out_digest)
        return;

    total_bits = ctx->total_len_bytes * 8u;
    _dtsha1_store_be64(len_block, total_bits);

    // append 0x80
    dtsha1_update(ctx, &pad_byte, 1);

    // append zero bytes until buffer length is 56 mod 64
    while (ctx->buffer_len != 56)
    {
        dtsha1_update(ctx, &zero, 1);
    }

    // append 64-bit big-endian length
    dtsha1_update(ctx, len_block, sizeof(len_block));

    for (i = 0; i < 5; ++i)
    {
        _dtsha1_store_be32(out_digest + (i * 4), ctx->state[i]);
    }
}

// --------------------------------------------------------------------------------------------

void
dtsha1_compute(const void* data, size_t len, uint8_t out_digest[20])
{
    dtsha1_ctx_t ctx;
    dtsha1_init(&ctx);
    dtsha1_update(&ctx, data, len);
    dtsha1_final(&ctx, out_digest);
}