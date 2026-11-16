#ifndef NEXTUPD_UTIL_H
#define NEXTUPD_UTIL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * nextupd validates and stages update packages. It deliberately does not
 * install anything: staging only publishes verified bytes for a later task.
 */

typedef enum
{
    UPD_OK = 0,
    UPD_E_MISSING = 2,
    UPD_E_INVALID = 3,
    UPD_E_PLATFORM = 4,
    UPD_E_UNSAFE = 5,
    UPD_E_INTEGRITY = 6,
    UPD_E_IO = 7,
    UPD_E_SIGNATURE = 8,
    UPD_E_SPACE = 9,
    UPD_E_INTERRUPTED = 10,
    UPD_E_ALREADY_STAGED = 11,
    UPD_E_BUSY = 12,
    UPD_E_STATE = 13,
    UPD_E_VERSION = 14,
    UPD_E_UNCONFIGURED = 15,
    UPD_E_USAGE = 64,
} UpdExitCode;

typedef struct
{
    UpdExitCode code;
    char reason[64];  /* stable machine-readable reason */
    char detail[256]; /* human-readable context; never contains key material */
} UpdError;

void upd_error_clear(UpdError* error);
void upd_error_set(UpdError* error, UpdExitCode code, const char* reason,
                   const char* format, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;

#define UPD_SHA256_BYTES 32
#define UPD_SHA256_HEX 65
#define UPD_MAX_REL_PATH 512
#define UPD_MAX_FILENAME 256
#define UPD_MAX_ID 128
#define UPD_MAX_VERSION 32
#define UPD_MAX_PLATFORM 16
#define UPD_MAX_KEY_ID 64

/*
 * Package identity limits. These are intentionally large enough for the
 * current system package but small enough to reject decompression bombs and
 * runaway manifests before allocating anything proportional to the archive.
 */
#define UPD_MAX_FILES 8192
#define UPD_MAX_ARCHIVE_BYTES (4ULL * 1024ULL * 1024ULL * 1024ULL)
#define UPD_MAX_FILE_BYTES (4ULL * 1024ULL * 1024ULL * 1024ULL)
#define UPD_MAX_TOTAL_BYTES (8ULL * 1024ULL * 1024ULL * 1024ULL)
#define UPD_MAX_MANIFEST_BYTES (16U * 1024U * 1024U)
#define UPD_MAX_SIGNATURE_BYTES 4096U

int upd_is_safe_rel_path(const char* path, size_t max_length);
int upd_is_safe_filename(const char* name, size_t max_length);
int upd_is_safe_app_id(const char* id);
int upd_is_safe_key_id(const char* key_id);

/*
 * Strict vMAJOR.MINOR.PATCH parsing. The v prefix is required when
 * require_v_prefix is non-zero. No prerelease or build suffix is accepted:
 * the release contract is the plan's semver tag format.
 */
int upd_parse_semver(const char* text, int require_v_prefix,
                     unsigned* major, unsigned* minor, unsigned* patch);
int upd_semver_compare(const char* left, const char* right, int* result);

int upd_parse_u64_text(const char* text, uint64_t* value);
int upd_hex_decode(const char* text, unsigned char* out, size_t bytes);
void upd_hex_encode(const unsigned char* bytes, size_t length, char* out);

int upd_read_file(const char* path, size_t max_bytes, unsigned char** out,
                  size_t* out_length, UpdError* error);
int upd_write_file_atomic(const char* path, const void* data, size_t length,
                          UpdError* error);
int upd_mkdir_p(const char* path, UpdError* error);
int upd_fsync_dir(const char* path);
int upd_file_size(const char* path, uint64_t* size, UpdError* error);
int upd_hash_file(const char* path, unsigned char digest[UPD_SHA256_BYTES],
                  uint64_t* size, uint64_t max_bytes, UpdError* error);

#ifdef __cplusplus
}
#endif

#endif
