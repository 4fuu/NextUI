#ifndef NEXTUPD_ZIP_H
#define NEXTUPD_ZIP_H

#include "update_util.h"

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Read-only ZIP access for update packages.
 *
 * The parser reads the central directory first, validates every local header,
 * and never extracts to the filesystem. Symlinks and special entries are
 * rejected; directory entries are kept separately so callers can reject
 * directories that the manifest does not imply.
 */

#define UPD_MAX_ZIP_ENTRIES 16384

typedef struct
{
    char name[UPD_MAX_REL_PATH + 1];
    uint16_t flags;
    uint16_t method;
    uint16_t version_made_by;
    uint32_t crc32;
    uint64_t compressed_size;
    uint64_t uncompressed_size;
    uint64_t data_offset;
} UpdZipEntry;

typedef struct
{
    UpdZipEntry* files;
    size_t file_count;
    char (*directories)[UPD_MAX_REL_PATH + 1];
    size_t directory_count;
    uint64_t archive_size;
    uint64_t central_offset;
    FILE* stream;
} UpdZip;

void upd_zip_init(UpdZip* zip);
void upd_zip_close(UpdZip* zip);

/* Open and validate the central directory and every local header. */
int upd_zip_open(const char* path, UpdZip* zip, UpdError* error);

/*
 * Stream one entry through SHA-256 and CRC32 without writing it anywhere.
 * expected_size is an upper bound: output beyond it fails immediately. At the
 * end actual_size must equal expected_size when expected_size is non-zero.
 */
int upd_zip_hash_entry(const UpdZip* zip, const UpdZipEntry* entry, uint64_t expected_size,
                       unsigned char digest[UPD_SHA256_BYTES], uint64_t* actual_size,
                       UpdError* error);

/* Binary search by exact path; returns NULL when absent. */
const UpdZipEntry* upd_zip_find(const UpdZip* zip, const char* path);

#ifdef __cplusplus
}
#endif

#endif
