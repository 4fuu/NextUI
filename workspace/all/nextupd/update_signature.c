#define _GNU_SOURCE

#include "update_signature.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "third_party/tweetnacl/tweetnacl.h"

#define SIGNATURE_HEADER "nextui-signature-v1"
#define PUBLIC_KEY_HEADER "nextui-public-key-v1"
#define ED25519_SIGNATURE_BYTES 64
#define ED25519_PUBLIC_KEY_BYTES 32

/*
 * TweetNaCl's signing entry points reference randombytes, but nextupd only
 * links the verify path and never calls key generation. Keep the symbol
 * defined so the vendored file links, and fail loudly if that assumption is
 * ever broken.
 */
void randombytes(unsigned char* data, unsigned long long length)
{
    (void)data;
    (void)length;
    abort();
}

static int base64_value(char value)
{
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return value - 'a' + 26;
    if (value >= '0' && value <= '9') return value - '0' + 52;
    if (value == '+') return 62;
    if (value == '/') return 63;
    return -1;
}

static int base64_decode(const char* text, unsigned char* out, size_t out_size)
{
    size_t length = strlen(text);
    size_t i;
    size_t written = 0;

    if (length == 0 || length % 4 != 0) return 0;
    for (i = 0; i < length; i += 4)
    {
        int values[4];
        int padding = 0;
        int j;

        for (j = 0; j < 4; j++)
        {
            char value = text[i + (size_t)j];
            if (value == '=')
            {
                if (i + 4 != length || j < 2) return 0;
                values[j] = 0;
                padding++;
            }
            else
            {
                if (padding != 0) return 0;
                values[j] = base64_value(value);
                if (values[j] < 0) return 0;
            }
        }
        if (padding > 2) return 0;
        if (written >= out_size) return 0;
        out[written++] = (unsigned char)((values[0] << 2) | (values[1] >> 4));
        if (padding < 2)
        {
            if (written >= out_size) return 0;
            out[written++] = (unsigned char)((values[1] << 4) | (values[2] >> 2));
        }
        if (padding < 1)
        {
            if (written >= out_size) return 0;
            out[written++] = (unsigned char)((values[2] << 6) | values[3]);
        }
    }
    return written == out_size;
}

static int split_document(unsigned char* data, size_t length, char** lines, size_t max_lines,
                          size_t* line_count, UpdError* error)
{
    size_t count = 0;
    size_t start = 0;
    size_t i;

    if (memchr(data, '\0', length) != NULL)
    {
        upd_error_set(error, UPD_E_SIGNATURE, "signature_format",
                      "signature document contains NUL");
        return 0;
    }
    for (i = 0; i <= length; i++)
    {
        if (i == length || data[i] == '\n')
        {
            size_t end = i;
            if (end > start && data[end - 1] == '\r') end--;
            if (end == start)
            {
                if (i == length) break;
                upd_error_set(error, UPD_E_SIGNATURE, "signature_format",
                              "blank line in signature document");
                return 0;
            }
            if (count >= max_lines)
            {
                upd_error_set(error, UPD_E_SIGNATURE, "signature_format",
                              "too many lines in signature document");
                return 0;
            }
            data[end] = '\0';
            lines[count++] = (char*)data + start;
            start = i + 1;
        }
    }
    *line_count = count;
    return 1;
}

static int parse_document(unsigned char* data, size_t length, const char* expected_header,
                          char* key_id, size_t key_id_size, unsigned char* payload,
                          size_t payload_size, UpdError* error)
{
    char* lines[4];
    size_t line_count = 0;

    if (!split_document(data, length, lines, sizeof lines / sizeof lines[0], &line_count,
                        error))
        return 0;
    if (line_count != 3)
    {
        upd_error_set(error, UPD_E_SIGNATURE, "signature_format",
                      "expected three lines, got %zu", line_count);
        return 0;
    }
    if (strcmp(lines[0], expected_header) != 0)
    {
        upd_error_set(error, UPD_E_SIGNATURE, "signature_format",
                      "unexpected document header");
        return 0;
    }
    if (!upd_is_safe_key_id(lines[1]))
    {
        upd_error_set(error, UPD_E_SIGNATURE, "signature_format", "invalid key id");
        return 0;
    }
    if (strlen(lines[1]) >= key_id_size)
    {
        upd_error_set(error, UPD_E_SIGNATURE, "signature_format", "key id is too long");
        return 0;
    }
    if (!base64_decode(lines[2], payload, payload_size))
    {
        upd_error_set(error, UPD_E_SIGNATURE, "signature_format",
                      "invalid base64 payload");
        return 0;
    }
    snprintf(key_id, key_id_size, "%s", lines[1]);
    return 1;
}

int upd_signature_verify(const unsigned char* manifest, size_t manifest_length,
                         const char* signature_path, const char* public_key_path,
                         char* key_id_out, size_t key_id_size, UpdError* error)
{
    unsigned char* signature_document = NULL;
    unsigned char* key_document = NULL;
    size_t signature_length = 0;
    size_t key_length = 0;
    unsigned char signature[ED25519_SIGNATURE_BYTES];
    unsigned char public_key[ED25519_PUBLIC_KEY_BYTES];
    char signature_key_id[UPD_MAX_KEY_ID + 1];
    char public_key_id[UPD_MAX_KEY_ID + 1];
    unsigned char* attached = NULL;
    unsigned char* message = NULL;
    unsigned long long message_length = 0;
    int result = 0;

    if (signature_path == NULL || signature_path[0] == '\0')
    {
        upd_error_set(error, UPD_E_SIGNATURE, "signature_missing",
                      "a detached signature is required");
        return 0;
    }
    if (public_key_path == NULL || public_key_path[0] == '\0')
    {
        upd_error_set(error, UPD_E_SIGNATURE, "trusted_key_missing",
                      "a trusted public key is required");
        return 0;
    }
    if (!upd_read_file(signature_path, UPD_MAX_SIGNATURE_BYTES, &signature_document,
                       &signature_length, error))
    {
        if (strcmp(error->reason, "file_missing") == 0)
            upd_error_set(error, UPD_E_SIGNATURE, "signature_missing", "%s", error->detail);
        return 0;
    }
    if (!upd_read_file(public_key_path, UPD_MAX_SIGNATURE_BYTES, &key_document, &key_length,
                       error))
    {
        if (strcmp(error->reason, "file_missing") == 0)
            upd_error_set(error, UPD_E_SIGNATURE, "trusted_key_missing", "%s",
                          error->detail);
        goto done;
    }
    if (!parse_document(signature_document, signature_length, SIGNATURE_HEADER,
                        signature_key_id, sizeof signature_key_id, signature,
                        sizeof signature, error))
        goto done;
    if (!parse_document(key_document, key_length, PUBLIC_KEY_HEADER, public_key_id,
                        sizeof public_key_id, public_key, sizeof public_key, error))
        goto done;
    if (strcmp(signature_key_id, public_key_id) != 0)
    {
        upd_error_set(error, UPD_E_SIGNATURE, "signature_key_mismatch",
                      "signature key id %s does not match trusted key id %s",
                      signature_key_id, public_key_id);
        goto done;
    }
    if (manifest_length > (size_t)-1 - ED25519_SIGNATURE_BYTES)
    {
        upd_error_set(error, UPD_E_SIGNATURE, "signature_invalid", "manifest is too large");
        goto done;
    }
    attached = (unsigned char*)malloc(manifest_length + ED25519_SIGNATURE_BYTES);
    /* crypto_sign_open copies the whole attached message (signature + payload)
     * into m before shifting it, so m needs room for signature + manifest. */
    message = (unsigned char*)malloc(manifest_length + ED25519_SIGNATURE_BYTES + 1);
    if (attached == NULL || message == NULL)
    {
        upd_error_set(error, UPD_E_IO, "io_error", "out of memory verifying signature");
        goto done;
    }
    memcpy(attached, signature, ED25519_SIGNATURE_BYTES);
    if (manifest_length > 0) memcpy(attached + ED25519_SIGNATURE_BYTES, manifest, manifest_length);
    if (crypto_sign_open(message, &message_length, attached,
                          (unsigned long long)manifest_length + ED25519_SIGNATURE_BYTES,
                          public_key)
        != 0)
    {
        upd_error_set(error, UPD_E_SIGNATURE, "signature_invalid",
                      "manifest signature does not verify");
        goto done;
    }
    if (message_length != (unsigned long long)manifest_length
        || (manifest_length > 0 && memcmp(message, manifest, manifest_length) != 0))
    {
        upd_error_set(error, UPD_E_SIGNATURE, "signature_invalid",
                      "signature message does not match manifest");
        goto done;
    }
    if (key_id_out != NULL && key_id_size > 0)
        snprintf(key_id_out, key_id_size, "%s", signature_key_id);
    result = 1;

done:
    free(attached);
    free(message);
    free(signature_document);
    free(key_document);
    return result;
}
