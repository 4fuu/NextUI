#ifndef NEXTUPD_SIGNATURE_H
#define NEXTUPD_SIGNATURE_H

#include "update_util.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Detached manifest signature, nextui-signature-v1:
 *
 *   nextui-signature-v1
 *   <key-id>
 *   <base64 Ed25519 signature of the exact manifest bytes>
 *
 * Trusted public key, nextui-public-key-v1:
 *
 *   nextui-public-key-v1
 *   <key-id>
 *   <base64 raw 32-byte Ed25519 public key>
 *
 * This is Ed25519 over the manifest bytes, not a minisign container and not a
 * SHA-256 checksum. A checksum can detect damage but cannot establish who
 * produced the manifest.
 */

int upd_signature_verify(const unsigned char* manifest, size_t manifest_length,
                         const char* signature_path, const char* public_key_path,
                         char* key_id_out, size_t key_id_size, UpdError* error);

#ifdef __cplusplus
}
#endif

#endif
