#ifndef SHA256_LITE_H
#define SHA256_LITE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Small, dependency-free SHA-256 used by the device identity scanner. This is
 * content-change detection, not signing: it proves nothing about who built a
 * pak. Keeping it in-tree avoids adding a libcrypto runtime dependency to the
 * device service.
 */

#define SHA256_LITE_DIGEST_SIZE 32
#define SHA256_LITE_HEX_SIZE 65

typedef struct
{
    uint32_t state[8];
    uint64_t total_bytes;
    unsigned char buffer[64];
    size_t buffer_length;
} Sha256Lite;

void sha256_lite_init(Sha256Lite* context);
void sha256_lite_update(Sha256Lite* context, const void* data, size_t length);
void sha256_lite_final(Sha256Lite* context, unsigned char digest[SHA256_LITE_DIGEST_SIZE]);

/* Lowercase hex plus NUL. */
void sha256_lite_hex(const unsigned char digest[SHA256_LITE_DIGEST_SIZE],
                     char out[SHA256_LITE_HEX_SIZE]);

#ifdef __cplusplus
}
#endif

#endif
