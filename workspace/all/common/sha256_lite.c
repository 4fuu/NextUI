#include "sha256_lite.h"

#include <string.h>

static const uint32_t round_constants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t rotate_right(uint32_t value, unsigned count)
{
    return (value >> count) | (value << (32u - count));
}

static void transform(Sha256Lite* context, const unsigned char block[64])
{
    uint32_t words[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    int i;

    for (i = 0; i < 16; i++)
    {
        words[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16)
                   | ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }
    for (i = 16; i < 64; i++)
    {
        uint32_t s0 = rotate_right(words[i - 15], 7) ^ rotate_right(words[i - 15], 18)
                      ^ (words[i - 15] >> 3);
        uint32_t s1 = rotate_right(words[i - 2], 17) ^ rotate_right(words[i - 2], 19)
                      ^ (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    a = context->state[0];
    b = context->state[1];
    c = context->state[2];
    d = context->state[3];
    e = context->state[4];
    f = context->state[5];
    g = context->state[6];
    h = context->state[7];

    for (i = 0; i < 64; i++)
    {
        uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        uint32_t choice = (e & f) ^ (~e & g);
        uint32_t temp1 = h + s1 + choice + round_constants[i] + words[i];
        uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

void sha256_lite_init(Sha256Lite* context)
{
    context->state[0] = 0x6a09e667u;
    context->state[1] = 0xbb67ae85u;
    context->state[2] = 0x3c6ef372u;
    context->state[3] = 0xa54ff53au;
    context->state[4] = 0x510e527fu;
    context->state[5] = 0x9b05688cu;
    context->state[6] = 0x1f83d9abu;
    context->state[7] = 0x5be0cd19u;
    context->total_bytes = 0;
    context->buffer_length = 0;
}

void sha256_lite_update(Sha256Lite* context, const void* data, size_t length)
{
    const unsigned char* bytes = (const unsigned char*)data;

    context->total_bytes += (uint64_t)length;

    while (length > 0)
    {
        size_t room = sizeof context->buffer - context->buffer_length;
        size_t take = length < room ? length : room;

        memcpy(context->buffer + context->buffer_length, bytes, take);
        context->buffer_length += take;
        bytes += take;
        length -= take;

        if (context->buffer_length == sizeof context->buffer)
        {
            transform(context, context->buffer);
            context->buffer_length = 0;
        }
    }
}

static void append_length(Sha256Lite* context, uint64_t bits)
{
    unsigned char length_bytes[8];
    int i;

    for (i = 0; i < 8; i++)
        length_bytes[i] = (unsigned char)(bits >> (56u - (unsigned)i * 8u));
    sha256_lite_update(context, length_bytes, sizeof length_bytes);
}

void sha256_lite_final(Sha256Lite* context, unsigned char digest[SHA256_LITE_DIGEST_SIZE])
{
    uint64_t bits = context->total_bytes * 8u;
    unsigned char padding = 0x80;
    unsigned char zero = 0;
    int i;

    sha256_lite_update(context, &padding, 1);
    while (context->buffer_length != 56) sha256_lite_update(context, &zero, 1);
    append_length(context, bits);

    for (i = 0; i < 8; i++)
    {
        digest[i * 4] = (unsigned char)(context->state[i] >> 24);
        digest[i * 4 + 1] = (unsigned char)(context->state[i] >> 16);
        digest[i * 4 + 2] = (unsigned char)(context->state[i] >> 8);
        digest[i * 4 + 3] = (unsigned char)context->state[i];
    }
}

void sha256_lite_hex(const unsigned char digest[SHA256_LITE_DIGEST_SIZE],
                     char out[SHA256_LITE_HEX_SIZE])
{
    static const char hex[] = "0123456789abcdef";
    int i;

    for (i = 0; i < SHA256_LITE_DIGEST_SIZE; i++)
    {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0Fu];
    }
    out[SHA256_LITE_HEX_SIZE - 1] = '\0';
}
