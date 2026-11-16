#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include "update_zip.h"
#include "sha256_lite.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <zlib.h>

#define EOCD_SIGNATURE 0x06054b50u
#define CENTRAL_SIGNATURE 0x02014b50u
#define LOCAL_SIGNATURE 0x04034b50u
#define EOCD_MIN_SIZE 22u
#define EOCD_MAX_COMMENT 65535u
#define CENTRAL_HEADER_SIZE 46u
#define LOCAL_HEADER_SIZE 30u

static uint16_t read_le16(const unsigned char* data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static uint32_t read_le32(const unsigned char* data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16)
           | ((uint32_t)data[3] << 24);
}

static int read_at(FILE* stream, uint64_t offset, void* data, size_t length)
{
    if (offset > (uint64_t)INT64_MAX) return 0;
    if (fseeko(stream, (off_t)offset, SEEK_SET) != 0) return 0;
    return fread(data, 1, length, stream) == length;
}

void upd_zip_init(UpdZip* zip)
{
    memset(zip, 0, sizeof *zip);
}

void upd_zip_close(UpdZip* zip)
{
    if (zip == NULL) return;
    if (zip->stream != NULL) fclose(zip->stream);
    free(zip->files);
    free(zip->directories);
    memset(zip, 0, sizeof *zip);
}

static int add_file(UpdZip* zip, const UpdZipEntry* entry, UpdError* error)
{
    UpdZipEntry* files;
    if (zip->file_count >= UPD_MAX_FILES)
    {
        upd_error_set(error, UPD_E_INVALID, "too_many_files", "archive has more than %u files",
                      UPD_MAX_FILES);
        return 0;
    }
    files = (UpdZipEntry*)realloc(zip->files, (zip->file_count + 1) * sizeof *files);
    if (files == NULL)
    {
        upd_error_set(error, UPD_E_IO, "io_error", "out of memory reading ZIP");
        return 0;
    }
    zip->files = files;
    zip->files[zip->file_count++] = *entry;
    return 1;
}

static int add_directory(UpdZip* zip, const char* name, UpdError* error)
{
    char (*directories)[UPD_MAX_REL_PATH + 1];
    if (zip->directory_count >= UPD_MAX_ZIP_ENTRIES)
    {
        upd_error_set(error, UPD_E_INVALID, "too_many_files", "too many ZIP directories");
        return 0;
    }
    directories = (char(*)[UPD_MAX_REL_PATH + 1])realloc(
        zip->directories, (zip->directory_count + 1) * (UPD_MAX_REL_PATH + 1));
    if (directories == NULL)
    {
        upd_error_set(error, UPD_E_IO, "io_error", "out of memory reading ZIP");
        return 0;
    }
    zip->directories = directories;
    snprintf(zip->directories[zip->directory_count], UPD_MAX_REL_PATH + 1, "%s", name);
    zip->directory_count++;
    return 1;
}

static int compare_entries_exact(const void* left, const void* right)
{
    const UpdZipEntry* a = (const UpdZipEntry*)left;
    const UpdZipEntry* b = (const UpdZipEntry*)right;
    return strcmp(a->name, b->name);
}

static int compare_string_pointers_ci(const void* left, const void* right)
{
    const char* const* a = (const char* const*)left;
    const char* const* b = (const char* const*)right;
    return strcasecmp(*a, *b);
}

static int compare_directory_names(const void* left, const void* right)
{
    const char* a = (const char*)left;
    const char* b = (const char*)right;
    return strcmp(a, b);
}

static int check_case_duplicates(UpdZip* zip, UpdError* error)
{
    const char** names;
    size_t total = zip->file_count + zip->directory_count;
    size_t i;

    if (total < 2) return 1;
    names = (const char**)malloc(total * sizeof *names);
    if (names == NULL)
    {
        upd_error_set(error, UPD_E_IO, "io_error", "out of memory checking ZIP");
        return 0;
    }
    for (i = 0; i < zip->file_count; i++) names[i] = zip->files[i].name;
    for (i = 0; i < zip->directory_count; i++)
        names[zip->file_count + i] = zip->directories[i];
    qsort(names, total, sizeof *names, compare_string_pointers_ci);
    for (i = 1; i < total; i++)
    {
        if (strcasecmp(names[i - 1], names[i]) == 0)
        {
            upd_error_set(error, UPD_E_INVALID, "duplicate_entry",
                          "case-colliding ZIP entries: %s and %s", names[i - 1], names[i]);
            free(names);
            return 0;
        }
    }
    free(names);
    return 1;
}

static int parse_central_entry(UpdZip* zip, uint64_t cd_offset, uint64_t cd_end,
                               uint64_t* cursor, UpdError* error)
{
    unsigned char header[CENTRAL_HEADER_SIZE];
    unsigned char name_bytes[UPD_MAX_REL_PATH + 2];
    unsigned char local_header[LOCAL_HEADER_SIZE];
    unsigned char local_name[UPD_MAX_REL_PATH + 2];
    uint16_t name_length;
    uint16_t extra_length;
    uint16_t comment_length;
    uint16_t flags;
    uint16_t method;
    uint16_t version_made_by;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t external_attributes;
    uint32_t local_offset;
    uint16_t local_name_length;
    uint16_t local_extra_length;
    uint16_t local_flags;
    uint16_t local_method;
    uint64_t entry_end;
    uint64_t local_data;
    uint32_t mode;
    int is_directory = 0;
    int is_symlink = 0;
    char name[UPD_MAX_REL_PATH + 1];
    UpdZipEntry entry;

    name[0] = '\0';
    if (*cursor + CENTRAL_HEADER_SIZE > cd_end) goto invalid;
    if (!read_at(zip->stream, *cursor, header, sizeof header)) goto io;
    if (read_le32(header) != CENTRAL_SIGNATURE) goto invalid;

    version_made_by = read_le16(header + 4);
    flags = read_le16(header + 8);
    method = read_le16(header + 10);
    compressed_size = read_le32(header + 20);
    uncompressed_size = read_le32(header + 24);
    name_length = read_le16(header + 28);
    extra_length = read_le16(header + 30);
    comment_length = read_le16(header + 32);
    external_attributes = read_le32(header + 38);
    local_offset = read_le32(header + 42);

    if (name_length == 0 || name_length > UPD_MAX_REL_PATH) goto unsafe_path;
    entry_end = *cursor + CENTRAL_HEADER_SIZE + name_length + extra_length + comment_length;
    if (entry_end > cd_end) goto invalid;
    if (!read_at(zip->stream, *cursor + CENTRAL_HEADER_SIZE, name_bytes, name_length)) goto io;
    if (memchr(name_bytes, '\0', name_length) != NULL) goto unsafe_path;
    memcpy(name, name_bytes, name_length);
    name[name_length] = '\0';

    if (name[name_length - 1] == '/') is_directory = 1;
    if (name_length >= 2 && name[name_length - 2] == '/' && name[name_length - 1] == '/')
        goto unsafe_path;

    mode = external_attributes >> 16;
    if ((mode & 0170000u) == 0120000u) is_symlink = 1;
    if ((mode & 0170000u) == 0040000u) is_directory = 1;
    if ((external_attributes & 0x10u) != 0) is_directory = 1;

    if (is_symlink)
    {
        upd_error_set(error, UPD_E_UNSAFE, "unsafe_link",
                      "ZIP entry is a symbolic link: %s", name);
        return 0;
    }
    if ((mode & 0170000u) != 0 && (mode & 0170000u) != 0100000u
        && (mode & 0170000u) != 0040000u)
    {
        upd_error_set(error, UPD_E_UNSAFE, "unsafe_link",
                      "ZIP entry is not a regular file or directory: %s", name);
        return 0;
    }

    if (is_directory)
    {
        size_t directory_length = name_length;
        while (directory_length > 0 && name[directory_length - 1] == '/') directory_length--;
        name[directory_length] = '\0';
        if (directory_length == 0 || !upd_is_safe_rel_path(name, UPD_MAX_REL_PATH))
            goto unsafe_path;
        if (compressed_size != 0 || uncompressed_size != 0) goto invalid;
        if (!add_directory(zip, name, error)) return 0;
        *cursor = entry_end;
        return 1;
    }

    if (!upd_is_safe_rel_path(name, UPD_MAX_REL_PATH)) goto unsafe_path;
    if ((flags & 0x0001u) != 0 || (flags & 0x0040u) != 0)
    {
        upd_error_set(error, UPD_E_INVALID, "zip_encrypted", "encrypted ZIP entry: %s", name);
        return 0;
    }
    if ((flags & 0x2000u) != 0)
    {
        upd_error_set(error, UPD_E_INVALID, "zip_unsupported", "masked ZIP entry: %s", name);
        return 0;
    }
    if (method != 0 && method != 8)
    {
        upd_error_set(error, UPD_E_INVALID, "zip_unsupported",
                      "unsupported compression method %u for %s", (unsigned)method, name);
        return 0;
    }
    /* The central directory stores 32-bit sizes; ZIP64 is rejected above, so
     * these values are inherently below the 4 GiB per-file limit. */
    if (method == 0 && compressed_size != uncompressed_size) goto invalid;

    if (local_offset >= cd_offset) goto invalid;
    if (!read_at(zip->stream, local_offset, local_header, sizeof local_header)) goto io;
    if (read_le32(local_header) != LOCAL_SIGNATURE) goto invalid;
    local_flags = read_le16(local_header + 6);
    local_method = read_le16(local_header + 8);
    local_name_length = read_le16(local_header + 26);
    local_extra_length = read_le16(local_header + 28);
    if (local_flags != flags || local_method != method) goto invalid;
    if (local_name_length != name_length || local_name_length > UPD_MAX_REL_PATH + 1) goto invalid;
    if (!read_at(zip->stream, local_offset + LOCAL_HEADER_SIZE, local_name, local_name_length))
        goto io;
    if (memcmp(local_name, name_bytes, name_length) != 0) goto invalid;
    local_data = local_offset + LOCAL_HEADER_SIZE + local_name_length + local_extra_length;
    if (local_data > cd_offset || compressed_size > cd_offset - local_data) goto invalid;

    memset(&entry, 0, sizeof entry);
    snprintf(entry.name, sizeof entry.name, "%s", name);
    entry.flags = flags;
    entry.method = method;
    entry.version_made_by = version_made_by;
    entry.crc32 = read_le32(header + 16);
    entry.compressed_size = compressed_size;
    entry.uncompressed_size = uncompressed_size;
    entry.data_offset = local_data;
    if (!add_file(zip, &entry, error)) return 0;
    *cursor = entry_end;
    return 1;

unsafe_path:
    upd_error_set(error, UPD_E_UNSAFE, "unsafe_path", "unsafe ZIP path: %s", name);
    return 0;
invalid:
    upd_error_set(error, UPD_E_INVALID, "zip_invalid", "invalid ZIP central directory");
    return 0;
io:
    upd_error_set(error, UPD_E_IO, "io_error", "cannot read ZIP");
    return 0;
}

int upd_zip_open(const char* path, UpdZip* zip, UpdError* error)
{
    struct stat status;
    unsigned char* tail = NULL;
    size_t tail_length;
    ssize_t index;
    int found = -1;
    uint16_t disk_number;
    uint16_t central_disk;
    uint16_t entries_on_disk;
    uint16_t entries_total;
    uint32_t central_size;
    uint32_t central_offset;
    uint64_t eocd_offset;
    uint64_t cursor;
    uint64_t cd_end;
    uint64_t total_uncompressed = 0;
    size_t i;

    upd_zip_init(zip);
    if (stat(path, &status) != 0 || !S_ISREG(status.st_mode))
    {
        upd_error_set(error, UPD_E_MISSING, "archive_missing", "cannot stat %s", path);
        return 0;
    }
    if (status.st_size < 0)
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_size", "negative archive size");
        return 0;
    }
    zip->archive_size = (uint64_t)status.st_size;
    if (zip->archive_size < EOCD_MIN_SIZE)
    {
        upd_error_set(error, UPD_E_INVALID, "zip_invalid", "archive is too small for a ZIP");
        return 0;
    }
    {
        int descriptor = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (descriptor < 0)
        {
            upd_error_set(error, UPD_E_MISSING, "archive_missing", "cannot open %s: %s",
                          path, strerror(errno));
            return 0;
        }
        zip->stream = fdopen(descriptor, "rb");
        if (zip->stream == NULL)
        {
            upd_error_set(error, UPD_E_IO, "io_error", "cannot read %s: %s", path,
                          strerror(errno));
            close(descriptor);
            return 0;
        }
    }

    tail_length = zip->archive_size < EOCD_MIN_SIZE + EOCD_MAX_COMMENT
                      ? (size_t)zip->archive_size
                      : EOCD_MIN_SIZE + EOCD_MAX_COMMENT;
    tail = (unsigned char*)malloc(tail_length);
    if (tail == NULL)
    {
        upd_error_set(error, UPD_E_IO, "io_error", "out of memory reading ZIP");
        goto failed;
    }
    if (!read_at(zip->stream, zip->archive_size - tail_length, tail, tail_length)) goto io;
    for (index = (ssize_t)tail_length - (ssize_t)EOCD_MIN_SIZE; index >= 0; index--)
    {
        if (read_le32(tail + index) == EOCD_SIGNATURE
            && (uint64_t)index + EOCD_MIN_SIZE + read_le16(tail + index + 20) == tail_length)
        {
            found = (int)index;
            break;
        }
    }
    if (found < 0)
    {
        upd_error_set(error, UPD_E_INVALID, "zip_invalid", "ZIP end record not found");
        goto failed;
    }
    eocd_offset = zip->archive_size - tail_length + (uint64_t)found;
    disk_number = read_le16(tail + found + 4);
    central_disk = read_le16(tail + found + 6);
    entries_on_disk = read_le16(tail + found + 8);
    entries_total = read_le16(tail + found + 10);
    central_size = read_le32(tail + found + 12);
    central_offset = read_le32(tail + found + 16);
    free(tail);
    tail = NULL;

    if (disk_number != 0 || central_disk != 0 || entries_on_disk != entries_total)
    {
        upd_error_set(error, UPD_E_INVALID, "zip_unsupported",
                      "multi-disk ZIP archives are not supported");
        goto failed;
    }
    if (entries_total == 0xFFFFu || central_size == 0xFFFFFFFFu
        || central_offset == 0xFFFFFFFFu)
    {
        upd_error_set(error, UPD_E_INVALID, "zip64_unsupported",
                      "ZIP64 archives are not supported");
        goto failed;
    }
    if (entries_total > UPD_MAX_ZIP_ENTRIES)
    {
        upd_error_set(error, UPD_E_INVALID, "too_many_files", "ZIP has too many entries");
        goto failed;
    }
    if ((uint64_t)central_offset + central_size > eocd_offset)
    {
        upd_error_set(error, UPD_E_INVALID, "zip_invalid",
                      "ZIP central directory overlaps the end record");
        goto failed;
    }
    zip->central_offset = central_offset;
    cursor = central_offset;
    cd_end = (uint64_t)central_offset + central_size;

    for (i = 0; i < entries_total; i++)
    {
        size_t previous_count = zip->file_count;
        if (!parse_central_entry(zip, central_offset, cd_end, &cursor, error)) goto failed;
        if (zip->file_count > previous_count)
        {
            const UpdZipEntry* entry = &zip->files[zip->file_count - 1];
            if (total_uncompressed > UPD_MAX_TOTAL_BYTES - entry->uncompressed_size)
            {
                upd_error_set(error, UPD_E_INVALID, "too_large",
                              "uncompressed ZIP total exceeds %llu bytes",
                              (unsigned long long)UPD_MAX_TOTAL_BYTES);
                goto failed;
            }
            total_uncompressed += entry->uncompressed_size;
        }
    }
    if (cursor != cd_end)
    {
        upd_error_set(error, UPD_E_INVALID, "zip_invalid",
                      "ZIP central directory size does not match its entries");
        goto failed;
    }

    if (zip->file_count > 1)
    {
        qsort(zip->files, zip->file_count, sizeof *zip->files, compare_entries_exact);
        for (i = 1; i < zip->file_count; i++)
        {
            if (strcmp(zip->files[i - 1].name, zip->files[i].name) == 0)
            {
                upd_error_set(error, UPD_E_INVALID, "duplicate_entry",
                              "duplicate ZIP entry: %s", zip->files[i].name);
                goto failed;
            }
        }
    }
    if (zip->directory_count > 1)
        qsort(zip->directories, zip->directory_count, UPD_MAX_REL_PATH + 1,
              compare_directory_names);
    for (i = 1; i < zip->directory_count; i++)
    {
        if (strcmp(zip->directories[i - 1], zip->directories[i]) == 0)
        {
            upd_error_set(error, UPD_E_INVALID, "duplicate_entry",
                          "duplicate ZIP directory: %s", zip->directories[i]);
            goto failed;
        }
    }
    for (i = 0; i < zip->directory_count; i++)
    {
        if (upd_zip_find(zip, zip->directories[i]) != NULL)
        {
            upd_error_set(error, UPD_E_INVALID, "duplicate_entry",
                          "ZIP path is both a file and a directory: %s",
                          zip->directories[i]);
            goto failed;
        }
    }
    if (!check_case_duplicates(zip, error)) goto failed;
    return 1;

io:
    upd_error_set(error, UPD_E_IO, "io_error", "cannot read ZIP: %s", strerror(errno));
failed:
    free(tail);
    upd_zip_close(zip);
    return 0;
}

const UpdZipEntry* upd_zip_find(const UpdZip* zip, const char* path)
{
    size_t low = 0;
    size_t high = zip->file_count;

    while (low < high)
    {
        size_t middle = low + (high - low) / 2;
        int comparison = strcmp(zip->files[middle].name, path);
        if (comparison == 0) return &zip->files[middle];
        if (comparison < 0)
            low = middle + 1;
        else
            high = middle;
    }
    return NULL;
}

static int update_digest(Sha256Lite* context, uLong* crc, const unsigned char* data,
                         size_t length, uint64_t* total, uint64_t max_output,
                         UpdError* error)
{
    if (length > max_output || *total > max_output - length)
    {
        upd_error_set(error, UPD_E_INTEGRITY, "file_size_mismatch",
                      "entry expands beyond %llu bytes", (unsigned long long)max_output);
        return 0;
    }
    *total += length;
    sha256_lite_update(context, data, length);
    *crc = crc32(*crc, data, (uInt)length);
    return 1;
}

int upd_zip_hash_entry(const UpdZip* zip, const UpdZipEntry* entry, uint64_t expected_size,
                       unsigned char digest[UPD_SHA256_BYTES], uint64_t* actual_size,
                       UpdError* error)
{
    unsigned char input[64 * 1024];
    unsigned char output[64 * 1024];
    Sha256Lite context;
    uLong crc = crc32(0L, Z_NULL, 0);
    uint64_t total = 0;
    uint64_t remaining;
    uint64_t max_output;
    int stream_ended = 0;

    if (expected_size > UPD_MAX_FILE_BYTES)
    {
        upd_error_set(error, UPD_E_INVALID, "too_large", "%s: expected size is too large",
                      entry->name);
        return 0;
    }
    max_output = expected_size;
    if (entry->uncompressed_size != expected_size)
    {
        upd_error_set(error, UPD_E_INTEGRITY, "file_size_mismatch", "%s: expected %llu bytes, ZIP says %llu",
                      entry->name, (unsigned long long)expected_size,
                      (unsigned long long)entry->uncompressed_size);
        return 0;
    }
    if (entry->data_offset > (uint64_t)INT64_MAX
        || fseeko(zip->stream, (off_t)entry->data_offset, SEEK_SET) != 0)
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot seek to %s", entry->name);
        return 0;
    }
    sha256_lite_init(&context);
    remaining = entry->compressed_size;

    if (entry->method == 0)
    {
        while (remaining > 0)
        {
            size_t chunk = remaining < sizeof input ? (size_t)remaining : sizeof input;
            if (fread(input, 1, chunk, zip->stream) != chunk)
            {
                upd_error_set(error, UPD_E_IO, "io_error", "short read from %s", entry->name);
                return 0;
            }
            if (!update_digest(&context, &crc, input, chunk, &total, max_output, error))
                return 0;
            remaining -= chunk;
        }
    }
    else
    {
        z_stream stream;
        int result;
        memset(&stream, 0, sizeof stream);
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
        {
            upd_error_set(error, UPD_E_IO, "io_error", "cannot initialize inflate");
            return 0;
        }
        while (remaining > 0)
        {
            size_t chunk = remaining < sizeof input ? (size_t)remaining : sizeof input;
            if (fread(input, 1, chunk, zip->stream) != chunk)
            {
                upd_error_set(error, UPD_E_IO, "io_error", "short read from %s", entry->name);
                inflateEnd(&stream);
                return 0;
            }
            remaining -= chunk;
            stream.next_in = input;
            stream.avail_in = (uInt)chunk;
            while (stream.avail_in > 0)
            {
                size_t produced;
                stream.next_out = output;
                stream.avail_out = sizeof output;
                result = inflate(&stream, Z_NO_FLUSH);
                if (result != Z_OK && result != Z_STREAM_END)
                {
                    upd_error_set(error, UPD_E_INVALID, "zip_invalid",
                                  "deflate failed for %s", entry->name);
                    inflateEnd(&stream);
                    return 0;
                }
                produced = sizeof output - stream.avail_out;
                if (produced > 0
                    && !update_digest(&context, &crc, output, produced, &total, max_output,
                                      error))
                {
                    inflateEnd(&stream);
                    return 0;
                }
                if (result == Z_STREAM_END)
                {
                    if (stream.avail_in != 0 || remaining != 0)
                    {
                        upd_error_set(error, UPD_E_INVALID, "zip_invalid",
                                      "trailing deflate data for %s", entry->name);
                        inflateEnd(&stream);
                        return 0;
                    }
                    stream_ended = 1;
                    break;
                }
            }
        }
        while (!stream_ended)
        {
            size_t produced;
            stream.next_out = output;
            stream.avail_out = sizeof output;
            result = inflate(&stream, Z_FINISH);
            if (result != Z_OK && result != Z_STREAM_END && result != Z_BUF_ERROR)
            {
                upd_error_set(error, UPD_E_INVALID, "zip_invalid",
                              "deflate did not finish for %s", entry->name);
                inflateEnd(&stream);
                return 0;
            }
            produced = sizeof output - stream.avail_out;
            if (produced > 0
                && !update_digest(&context, &crc, output, produced, &total, max_output, error))
            {
                inflateEnd(&stream);
                return 0;
            }
            if (result == Z_STREAM_END) break;
            if (produced == 0)
            {
                upd_error_set(error, UPD_E_INVALID, "zip_invalid",
                              "deflate stream ended early for %s", entry->name);
                inflateEnd(&stream);
                return 0;
            }
        }
        if (stream.total_in != entry->compressed_size)
        {
            upd_error_set(error, UPD_E_INVALID, "zip_invalid",
                          "deflate input size mismatch for %s", entry->name);
            inflateEnd(&stream);
            return 0;
        }
        inflateEnd(&stream);
    }

    if (total != entry->uncompressed_size || total != expected_size)
    {
        upd_error_set(error, UPD_E_INTEGRITY, "file_size_mismatch",
                      "%s: got %llu bytes, expected %llu", entry->name,
                      (unsigned long long)total, (unsigned long long)expected_size);
        return 0;
    }
    if ((uint32_t)crc != entry->crc32)
    {
        upd_error_set(error, UPD_E_INTEGRITY, "file_crc_mismatch",
                      "%s: CRC32 mismatch", entry->name);
        return 0;
    }
    sha256_lite_final(&context, digest);
    if (actual_size != NULL) *actual_size = total;
    return 1;
}
