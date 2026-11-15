#define _GNU_SOURCE

#include "app_state.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define APP_STATE_META_NAME "resume.meta"
#define APP_STATE_PAYLOAD_NAME "state"
#define APP_STATE_HEADER "nextui-app-state 1"
#define APP_STATE_HASH_HEX 64
#define APP_STATE_HASH_BYTES 32
#define APP_STATE_NAME_MAX 128

typedef struct
{
    char path[APP_STATE_REL_PATH_MAX];
    unsigned long long size;
    unsigned char hash[APP_STATE_HASH_BYTES];
} StateFile;

typedef struct
{
    StateFile* items;
    size_t count;
    size_t capacity;
    unsigned long long total_bytes;
} StateFileList;

typedef struct
{
    uint32_t state[8];
    uint64_t total_bytes;
    unsigned char block[64];
    size_t block_used;
} Sha256;

/* ------------------------------------------------------------------------- */
/* SHA-256 (small, self-contained; metadata only, no crypto dependency)        */
/* ------------------------------------------------------------------------- */

static uint32_t rotr32(uint32_t value, unsigned shift)
{
    return (value >> shift) | (value << (32u - shift));
}

static void sha256_init(Sha256* ctx)
{
    ctx->state[0] = UINT32_C(0x6a09e667);
    ctx->state[1] = UINT32_C(0xbb67ae85);
    ctx->state[2] = UINT32_C(0x3c6ef372);
    ctx->state[3] = UINT32_C(0xa54ff53a);
    ctx->state[4] = UINT32_C(0x510e527f);
    ctx->state[5] = UINT32_C(0x9b05688c);
    ctx->state[6] = UINT32_C(0x1f83d9ab);
    ctx->state[7] = UINT32_C(0x5be0cd19);
    ctx->total_bytes = 0;
    ctx->block_used = 0;
}

static void sha256_block(Sha256* ctx, const unsigned char* block)
{
    static const uint32_t k[64] = {
        UINT32_C(0x428a2f98), UINT32_C(0x71374491), UINT32_C(0xb5c0fbcf), UINT32_C(0xe9b5dba5),
        UINT32_C(0x3956c25b), UINT32_C(0x59f111f1), UINT32_C(0x923f82a4), UINT32_C(0xab1c5ed5),
        UINT32_C(0xd807aa98), UINT32_C(0x12835b01), UINT32_C(0x243185be), UINT32_C(0x550c7dc3),
        UINT32_C(0x72be5d74), UINT32_C(0x80deb1fe), UINT32_C(0x9bdc06a7), UINT32_C(0xc19bf174),
        UINT32_C(0xe49b69c1), UINT32_C(0xefbe4786), UINT32_C(0x0fc19dc6), UINT32_C(0x240ca1cc),
        UINT32_C(0x2de92c6f), UINT32_C(0x4a7484aa), UINT32_C(0x5cb0a9dc), UINT32_C(0x76f988da),
        UINT32_C(0x983e5152), UINT32_C(0xa831c66d), UINT32_C(0xb00327c8), UINT32_C(0xbf597fc7),
        UINT32_C(0xc6e00bf3), UINT32_C(0xd5a79147), UINT32_C(0x06ca6351), UINT32_C(0x14292967),
        UINT32_C(0x27b70a85), UINT32_C(0x2e1b2138), UINT32_C(0x4d2c6dfc), UINT32_C(0x53380d13),
        UINT32_C(0x650a7354), UINT32_C(0x766a0abb), UINT32_C(0x81c2c92e), UINT32_C(0x92722c85),
        UINT32_C(0xa2bfe8a1), UINT32_C(0xa81a664b), UINT32_C(0xc24b8b70), UINT32_C(0xc76c51a3),
        UINT32_C(0xd192e819), UINT32_C(0xd6990624), UINT32_C(0xf40e3585), UINT32_C(0x106aa070),
        UINT32_C(0x19a4c116), UINT32_C(0x1e376c08), UINT32_C(0x2748774c), UINT32_C(0x34b0bcb5),
        UINT32_C(0x391c0cb3), UINT32_C(0x4ed8aa4a), UINT32_C(0x5b9cca4f), UINT32_C(0x682e6ff3),
        UINT32_C(0x748f82ee), UINT32_C(0x78a5636f), UINT32_C(0x84c87814), UINT32_C(0x8cc70208),
        UINT32_C(0x90befffa), UINT32_C(0xa4506ceb), UINT32_C(0xbef9a3f7), UINT32_C(0xc67178f2),
    };
    uint32_t w[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    unsigned i;

    for (i = 0; i < 16; i++)
    {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16)
               | ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }
    for (i = 16; i < 64; i++)
    {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (i = 0; i < 64; i++)
    {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + ch + k[i] + w[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_update(Sha256* ctx, const unsigned char* data, size_t length)
{
    size_t offset = 0;

    ctx->total_bytes += (uint64_t)length;
    while (offset < length)
    {
        size_t room = sizeof ctx->block - ctx->block_used;
        size_t take = length - offset < room ? length - offset : room;
        memcpy(ctx->block + ctx->block_used, data + offset, take);
        ctx->block_used += take;
        offset += take;
        if (ctx->block_used == sizeof ctx->block)
        {
            sha256_block(ctx, ctx->block);
            ctx->block_used = 0;
        }
    }
}

static void sha256_final(Sha256* ctx, unsigned char digest[APP_STATE_HASH_BYTES])
{
    uint64_t bits = ctx->total_bytes * UINT64_C(8);
    unsigned i;

    ctx->block[ctx->block_used++] = 0x80;
    if (ctx->block_used > 56)
    {
        memset(ctx->block + ctx->block_used, 0, sizeof ctx->block - ctx->block_used);
        sha256_block(ctx, ctx->block);
        ctx->block_used = 0;
    }
    memset(ctx->block + ctx->block_used, 0, 56 - ctx->block_used);
    for (i = 0; i < 8; i++)
        ctx->block[56 + i] = (unsigned char)(bits >> (56u - i * 8u));
    sha256_block(ctx, ctx->block);

    for (i = 0; i < 8; i++)
    {
        digest[i * 4] = (unsigned char)(ctx->state[i] >> 24);
        digest[i * 4 + 1] = (unsigned char)(ctx->state[i] >> 16);
        digest[i * 4 + 2] = (unsigned char)(ctx->state[i] >> 8);
        digest[i * 4 + 3] = (unsigned char)ctx->state[i];
    }
}

/* ------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* ------------------------------------------------------------------------- */

static void set_reason(char* reason, size_t reason_size, const char* text)
{
    if (reason == NULL || reason_size == 0) return;
    snprintf(reason, reason_size, "%s", text != NULL ? text : "");
}

static int copy_string(char* out, size_t out_size, const char* value)
{
    size_t length;

    if (out == NULL || out_size == 0 || value == NULL) return 0;
    length = strlen(value);
    if (length >= out_size) return 0;
    memcpy(out, value, length + 1);
    return 1;
}

static int join_path(char* out, size_t out_size, const char* base, const char* child)
{
    size_t length;
    int written;

    if (out == NULL || out_size == 0 || base == NULL || child == NULL) return 0;
    length = strlen(base);
    while (length > 1 && base[length - 1] == '/') length--;
    if (child[0] == '\0')
    {
        if (length >= out_size) return 0;
        memcpy(out, base, length);
        out[length] = '\0';
        return 1;
    }
    written = snprintf(out, out_size, "%.*s/%s", (int)length, base, child);
    return written >= 0 && (size_t)written < out_size;
}

static int append_component(char* out, size_t out_size, const char* component)
{
    size_t used = strlen(out);
    int written;

    if (used >= out_size) return 0;
    if (used > 0 && out[used - 1] != '/')
    {
        if (used + 1 >= out_size) return 0;
        out[used++] = '/';
        out[used] = '\0';
    }
    written = snprintf(out + used, out_size - used, "%s", component);
    return written >= 0 && (size_t)written < out_size - used;
}

static int is_safe_name(const char* name)
{
    const unsigned char* p;
    size_t length;

    if (name == NULL) return 0;
    length = strlen(name);
    if (length == 0 || length > APP_STATE_NAME_MAX) return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
    if (name[0] == ' ' || name[length - 1] == ' ') return 0;

    for (p = (const unsigned char*)name; *p != '\0'; p++)
    {
        if (*p < 0x20u || *p == 0x7Fu) return 0;
        if (*p == '/' || *p == '\\') return 0;
    }
    return 1;
}

static int is_safe_rel_path(const char* path)
{
    const char* p;
    size_t length;
    unsigned depth = 0;

    if (path == NULL) return 0;
    length = strlen(path);
    if (length == 0 || length >= APP_STATE_REL_PATH_MAX) return 0;
    if (path[0] == '/' || path[length - 1] == '/') return 0;
    if (strstr(path, "//") != NULL) return 0;

    p = path;
    for (;;)
    {
        const char* slash = strchr(p, '/');
        size_t part_length = slash != NULL ? (size_t)(slash - p) : strlen(p);
        char part[APP_STATE_REL_PATH_MAX];

        if (part_length == 0 || part_length >= sizeof part) return 0;
        if (part_length > APP_STATE_NAME_MAX) return 0;
        memcpy(part, p, part_length);
        part[part_length] = '\0';
        if (!is_safe_name(part)) return 0;
        depth++;
        if (depth > APP_STATE_MAX_DEPTH) return 0;
        if (slash == NULL) break;
        p = slash + 1;
    }
    return 1;
}

static int is_safe_id(const char* id)
{
    const unsigned char* p;
    size_t length;

    if (id == NULL) return 0;
    length = strlen(id);
    if (length == 0 || length >= APP_STATE_ID_MAX) return 0;
    if (strcmp(id, ".") == 0 || strcmp(id, "..") == 0) return 0;
    if (strstr(id, "..") != NULL) return 0;
    for (p = (const unsigned char*)id; *p != '\0'; p++)
    {
        if (*p < 0x20u || *p == 0x7Fu) return 0;
        if (*p == '/' || *p == '\\') return 0;
    }
    return 1;
}

static uint64_t fnv1a64(const char* text)
{
    const unsigned char* p;
    uint64_t value = UINT64_C(1469598103934665603);

    for (p = (const unsigned char*)text; *p != '\0'; p++)
    {
        value ^= (uint64_t)*p;
        value *= UINT64_C(1099511628211);
    }
    return value;
}

static void slot_name_for_id(const char* id, char* out, size_t out_size)
{
    uint64_t hash = fnv1a64(id);
    char prefix[33];
    size_t used = 0;
    size_t i;

    for (i = 0; id[i] != '\0' && used < sizeof prefix - 1 && used < 32; i++)
    {
        unsigned char ch = (unsigned char)id[i];
        int safe = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
                   || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
        if (used == 0 && (ch == '.' || ch == '-')) safe = 0;
        prefix[used++] = safe ? (char)ch : '_';
    }
    while (used > 0 && (prefix[used - 1] == '.' || prefix[used - 1] == '-')) used--;
    if (used == 0)
    {
        prefix[used++] = 'a';
        prefix[used++] = 'p';
        prefix[used++] = 'p';
    }
    prefix[used] = '\0';
    snprintf(out, out_size, "%s-%016llx", prefix, (unsigned long long)hash);
}

static int derive_paths(const char* root, const PakManifest* manifest,
                        char* app_id, size_t app_id_size, char* slot, size_t slot_size,
                        char* payload, size_t payload_size)
{
    char name[80];

    if (root == NULL || root[0] == '\0' || strlen(root) >= APP_STATE_PATH_MAX) return 0;
    if (manifest == NULL || manifest->status != PAK_MANIFEST_OK) return 0;
    if (!is_safe_id(manifest->id)) return 0;

    slot_name_for_id(manifest->id, name, sizeof name);
    if (!join_path(slot, slot_size, root, name)) return 0;
    if (!join_path(payload, payload_size, slot, APP_STATE_PAYLOAD_NAME)) return 0;
    if (!copy_string(app_id, app_id_size, manifest->id)) return 0;
    return 1;
}

static int metadata_path(const char* slot, char* out, size_t out_size)
{
    return join_path(out, out_size, slot, APP_STATE_META_NAME);
}

/* ------------------------------------------------------------------------- */
/* Directory and file validation                                              */
/* ------------------------------------------------------------------------- */

static int ensure_dir(const char* path)
{
    char work[APP_STATE_PATH_MAX];
    size_t length;
    size_t i;
    struct stat st;

    if (path == NULL || path[0] == '\0' || strlen(path) >= sizeof work) return 0;
    copy_string(work, sizeof work, path);
    length = strlen(work);
    while (length > 1 && work[length - 1] == '/') work[--length] = '\0';

    for (i = 1; i < length; i++)
    {
        if (work[i] != '/') continue;
        work[i] = '\0';
        if (mkdir(work, 0755) != 0 && errno != EEXIST) return 0;
        work[i] = '/';
    }
    if (mkdir(work, 0755) != 0 && errno != EEXIST) return 0;
    if (lstat(work, &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) return 0;
    return 1;
}

static int ensure_owned_dir(const char* path)
{
    struct stat st;

    if (!ensure_dir(path)) return 0;
    if (lstat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode);
}

static int safe_relative_file(const char* root, const char* relative, char* out,
                              size_t out_size, struct stat* st)
{
    char current[APP_STATE_PATH_MAX];
    const char* p;

    if (!is_safe_rel_path(relative)) return -1;
    if (!copy_string(current, sizeof current, root)) return -1;

    p = relative;
    for (;;)
    {
        const char* slash = strchr(p, '/');
        size_t length = slash != NULL ? (size_t)(slash - p) : strlen(p);
        char part[APP_STATE_NAME_MAX + 1];

        if (length == 0 || length > APP_STATE_NAME_MAX) return -1;
        memcpy(part, p, length);
        part[length] = '\0';
        if (!append_component(current, sizeof current, part)) return -1;
        if (lstat(current, st) != 0) return 0;
        if (S_ISLNK(st->st_mode)) return -1;
        if (slash == NULL)
        {
            if (out != NULL && !copy_string(out, out_size, current)) return -1;
            return 1;
        }
        if (!S_ISDIR(st->st_mode)) return -1;
        p = slash + 1;
    }
}

static int hash_file(const char* path, unsigned long long* size,
                     unsigned char digest[APP_STATE_HASH_BYTES], char* reason,
                     size_t reason_size)
{
    unsigned char buffer[64 * 1024];
    struct stat st;
    Sha256 ctx;
    int fd;
    ssize_t got;
    unsigned long long total = 0;

    fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
    {
        set_reason(reason, reason_size, "io_error");
        return 0;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
    {
        close(fd);
        set_reason(reason, reason_size, "unsafe_path");
        return 0;
    }
    if ((unsigned long long)st.st_size > APP_STATE_MAX_FILE_BYTES)
    {
        close(fd);
        set_reason(reason, reason_size, "too_large");
        return 0;
    }

    sha256_init(&ctx);
    while ((got = read(fd, buffer, sizeof buffer)) > 0)
    {
        total += (unsigned long long)got;
        if (total > APP_STATE_MAX_FILE_BYTES)
        {
            close(fd);
            set_reason(reason, reason_size, "too_large");
            return 0;
        }
        sha256_update(&ctx, buffer, (size_t)got);
    }
    if (got < 0)
    {
        close(fd);
        set_reason(reason, reason_size, "io_error");
        return 0;
    }
    close(fd);
    sha256_final(&ctx, digest);
    if (size != NULL) *size = total;
    return 1;
}

static void file_list_free(StateFileList* list)
{
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
    list->total_bytes = 0;
}

static int file_list_add(StateFileList* list, const StateFile* file, char* reason,
                         size_t reason_size)
{
    size_t i;

    for (i = 0; i < list->count; i++)
    {
        if (strcmp(list->items[i].path, file->path) == 0)
        {
            set_reason(reason, reason_size, "duplicate_path");
            return 0;
        }
    }
    if (list->count >= APP_STATE_MAX_FILES)
    {
        set_reason(reason, reason_size, "too_large");
        return 0;
    }
    if (list->count == list->capacity)
    {
        size_t capacity = list->capacity == 0 ? 16 : list->capacity * 2;
        StateFile* items;
        if (capacity > APP_STATE_MAX_FILES) capacity = APP_STATE_MAX_FILES;
        items = (StateFile*)realloc(list->items, capacity * sizeof *items);
        if (items == NULL)
        {
            set_reason(reason, reason_size, "io_error");
            return 0;
        }
        list->items = items;
        list->capacity = capacity;
    }
    list->items[list->count++] = *file;
    list->total_bytes += file->size;
    return 1;
}

static int compare_files(const void* a, const void* b)
{
    const StateFile* left = (const StateFile*)a;
    const StateFile* right = (const StateFile*)b;
    return strcmp(left->path, right->path);
}

static int collect_dir(const char* root, const char* relative, unsigned depth,
                       StateFileList* list, char* reason, size_t reason_size)
{
    char directory[APP_STATE_PATH_MAX];
    DIR* dir;
    struct dirent* entry;

    if (!join_path(directory, sizeof directory, root, relative))
    {
        set_reason(reason, reason_size, "unsafe_path");
        return 0;
    }
    dir = opendir(directory);
    if (dir == NULL)
    {
        set_reason(reason, reason_size, "io_error");
        return 0;
    }

    while ((entry = readdir(dir)) != NULL)
    {
        char child[APP_STATE_REL_PATH_MAX];
        char full[APP_STATE_PATH_MAX];
        struct stat st;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (!is_safe_name(entry->d_name))
        {
            closedir(dir);
            set_reason(reason, reason_size, "unsafe_path");
            return 0;
        }
        if (relative[0] == '\0')
            copy_string(child, sizeof child, entry->d_name);
        else if (snprintf(child, sizeof child, "%s/%s", relative, entry->d_name) >= (int)sizeof child)
        {
            closedir(dir);
            set_reason(reason, reason_size, "unsafe_path");
            return 0;
        }
        if (!is_safe_rel_path(child))
        {
            closedir(dir);
            set_reason(reason, reason_size, "unsafe_path");
            return 0;
        }
        if (!join_path(full, sizeof full, root, child))
        {
            closedir(dir);
            set_reason(reason, reason_size, "unsafe_path");
            return 0;
        }
        if (lstat(full, &st) != 0)
        {
            closedir(dir);
            set_reason(reason, reason_size, "io_error");
            return 0;
        }
        if (S_ISLNK(st.st_mode))
        {
            closedir(dir);
            set_reason(reason, reason_size, "unsafe_path");
            return 0;
        }
        if (S_ISDIR(st.st_mode))
        {
            if (depth + 1 > APP_STATE_MAX_DEPTH)
            {
                closedir(dir);
                set_reason(reason, reason_size, "unsafe_path");
                return 0;
            }
            if (!collect_dir(root, child, depth + 1, list, reason, reason_size))
            {
                closedir(dir);
                return 0;
            }
        }
        else if (S_ISREG(st.st_mode))
        {
            StateFile file;
            if (!is_safe_rel_path(child))
            {
                closedir(dir);
                set_reason(reason, reason_size, "unsafe_path");
                return 0;
            }
            memset(&file, 0, sizeof file);
            copy_string(file.path, sizeof file.path, child);
            if (!hash_file(full, &file.size, file.hash, reason, reason_size))
            {
                closedir(dir);
                return 0;
            }
            if (file.size > APP_STATE_MAX_FILE_BYTES
                || list->total_bytes + file.size > APP_STATE_MAX_TOTAL_BYTES)
            {
                closedir(dir);
                set_reason(reason, reason_size, "too_large");
                return 0;
            }
            if (!file_list_add(list, &file, reason, reason_size))
            {
                closedir(dir);
                return 0;
            }
        }
        else
        {
            closedir(dir);
            set_reason(reason, reason_size, "unsafe_path");
            return 0;
        }
    }
    closedir(dir);
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Metadata                                                                   */
/* ------------------------------------------------------------------------- */

static void hex_encode(const unsigned char* bytes, size_t length, char* out)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < length; i++)
    {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 0x0Fu];
    }
    out[length * 2] = '\0';
}

static int hex_digit(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static int hex_decode(const char* text, unsigned char* out, size_t length)
{
    size_t i;

    if (strlen(text) != length * 2) return 0;
    for (i = 0; i < length; i++)
    {
        int high = hex_digit(text[i * 2]);
        int low = hex_digit(text[i * 2 + 1]);
        if (high < 0 || low < 0) return 0;
        out[i] = (unsigned char)((high << 4) | low);
    }
    return 1;
}

static int parse_unsigned(const char* text, unsigned long long* out)
{
    unsigned long long value = 0;
    const char* p;

    if (text == NULL || text[0] == '\0') return 0;
    for (p = text; *p != '\0'; p++)
    {
        if (*p < '0' || *p > '9') return 0;
        if (value > (UINT64_MAX - (unsigned long long)(*p - '0')) / 10) return 0;
        value = value * 10 + (unsigned long long)(*p - '0');
    }
    *out = value;
    return 1;
}

static int parse_signed_time(const char* text, long long* out)
{
    unsigned long long value;
    if (!parse_unsigned(text, &value)) return 0;
    if (value > (unsigned long long)INT64_MAX) return 0;
    *out = (long long)value;
    return 1;
}

static int parse_file_line(char* line, StateFileList* list, char* reason, size_t reason_size)
{
    char* hash_text;
    char* size_text;
    unsigned long long size;
    StateFile file;

    hash_text = strrchr(line, ' ');
    if (hash_text == NULL) return 0;
    *hash_text++ = '\0';
    size_text = strrchr(line, ' ');
    if (size_text == NULL) return 0;
    *size_text++ = '\0';

    if (!is_safe_rel_path(line))
    {
        set_reason(reason, reason_size, "unsafe_path");
        return 0;
    }
    if (!parse_unsigned(size_text, &size) || size > APP_STATE_MAX_FILE_BYTES)
    {
        set_reason(reason, reason_size, "invalid_metadata");
        return 0;
    }
    memset(&file, 0, sizeof file);
    copy_string(file.path, sizeof file.path, line);
    file.size = size;
    if (!hex_decode(hash_text, file.hash, APP_STATE_HASH_BYTES))
    {
        set_reason(reason, reason_size, "invalid_metadata");
        return 0;
    }
    if (!file_list_add(list, &file, reason, reason_size)) return 0;
    return 1;
}

static int read_metadata(const char* path, const char* expected_id, StateFileList* list,
                         char* app_version, size_t app_version_size, long long* saved_at,
                         unsigned long long* total_bytes, char* reason, size_t reason_size)
{
    struct stat st;
    char* buffer;
    char* line;
    char* end;
    unsigned seen = 0;
    unsigned long long declared_files = 0;
    unsigned long long declared_total = 0;
    int fd;
    ssize_t got;
    size_t length;
    int first = 1;

    memset(list, 0, sizeof *list);
    if (app_version != NULL && app_version_size > 0) app_version[0] = '\0';

    fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
    {
        set_reason(reason, reason_size, errno == ENOENT ? "absent" : "io_error");
        return 0;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
    {
        close(fd);
        set_reason(reason, reason_size, "unsafe_metadata");
        return 0;
    }
    if (st.st_size <= 0)
    {
        close(fd);
        set_reason(reason, reason_size, "invalid_metadata");
        return 0;
    }
    if ((unsigned long long)st.st_size > APP_STATE_MAX_METADATA_BYTES)
    {
        close(fd);
        set_reason(reason, reason_size, "metadata_too_large");
        return 0;
    }

    buffer = (char*)malloc((size_t)st.st_size + 1);
    if (buffer == NULL)
    {
        close(fd);
        set_reason(reason, reason_size, "io_error");
        return 0;
    }
    length = 0;
    while (length < (size_t)st.st_size)
    {
        got = read(fd, buffer + length, (size_t)st.st_size - length);
        if (got <= 0)
        {
            free(buffer);
            close(fd);
            set_reason(reason, reason_size, "io_error");
            return 0;
        }
        length += (size_t)got;
    }
    close(fd);
    buffer[length] = '\0';
    if (memchr(buffer, '\0', length) != NULL)
    {
        free(buffer);
        set_reason(reason, reason_size, "invalid_metadata");
        return 0;
    }

    line = buffer;
    end = buffer + length;
    while (line < end)
    {
        char* newline = memchr(line, '\n', (size_t)(end - line));
        char* current_end = newline != NULL ? newline : end;
        *current_end = '\0';
        if (current_end > line && current_end[-1] == '\r') current_end[-1] = '\0';

        if (first)
        {
            if (strcmp(line, APP_STATE_HEADER) != 0)
            {
                free(buffer);
                set_reason(reason, reason_size, "invalid_metadata");
                return 0;
            }
            first = 0;
        }
        else if (strncmp(line, "app ", 4) == 0)
        {
            if (seen & 1u) goto invalid;
            seen |= 1u;
            if (strcmp(line + 4, expected_id) != 0) goto invalid;
        }
        else if (strncmp(line, "app_version ", 12) == 0)
        {
            if (seen & 2u) goto invalid;
            seen |= 2u;
            if (strcmp(line + 12, "-") != 0)
            {
                if (app_version != NULL && !copy_string(app_version, app_version_size, line + 12))
                    goto invalid;
            }
        }
        else if (strncmp(line, "saved_at ", 9) == 0)
        {
            if (seen & 4u) goto invalid;
            seen |= 4u;
            if (!parse_signed_time(line + 9, saved_at)) goto invalid;
        }
        else if (strncmp(line, "files ", 6) == 0)
        {
            if (seen & 8u) goto invalid;
            seen |= 8u;
            if (!parse_unsigned(line + 6, &declared_files) || declared_files == 0
                || declared_files > APP_STATE_MAX_FILES)
                goto invalid;
        }
        else if (strncmp(line, "total_bytes ", 12) == 0)
        {
            if (seen & 16u) goto invalid;
            seen |= 16u;
            if (!parse_unsigned(line + 12, &declared_total)
                || declared_total > APP_STATE_MAX_TOTAL_BYTES)
                goto invalid;
        }
        else if (strncmp(line, "file ", 5) == 0)
        {
            if (!parse_file_line(line + 5, list, reason, reason_size))
            {
                free(buffer);
                return 0;
            }
        }
        else
        {
            goto invalid;
        }

        line = current_end + 1;
    }

    if ((seen & 1u) == 0 || (seen & 4u) == 0 || (seen & 8u) == 0 || (seen & 16u) == 0
        || declared_files != list->count || declared_total != list->total_bytes
        || list->count == 0)
    {
        goto invalid;
    }

    *total_bytes = declared_total;
    free(buffer);
    return 1;

invalid:
    free(buffer);
    file_list_free(list);
    set_reason(reason, reason_size, "invalid_metadata");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Removal                                                                    */
/* ------------------------------------------------------------------------- */

static int remove_tree(const char* path, unsigned depth)
{
    struct stat st;
    DIR* dir;
    struct dirent* entry;
    int ok = 1;

    if (depth > 32) return 0;
    if (lstat(path, &st) != 0) return errno == ENOENT;
    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) return unlink(path) == 0;

    dir = opendir(path);
    if (dir == NULL) return 0;
    while ((entry = readdir(dir)) != NULL)
    {
        char child[APP_STATE_PATH_MAX];
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (!join_path(child, sizeof child, path, entry->d_name)) ok = 0;
        else if (!remove_tree(child, depth + 1)) ok = 0;
    }
    closedir(dir);
    if (rmdir(path) != 0) ok = 0;
    return ok;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                 */
/* ------------------------------------------------------------------------- */

const char* app_state_status_name(AppStateStatus status)
{
    switch (status)
    {
    case APP_STATE_ABSENT: return "absent";
    case APP_STATE_VALID: return "valid";
    case APP_STATE_INVALID: return "invalid";
    case APP_STATE_UNSUPPORTED: return "unsupported";
    case APP_STATE_ERROR: return "error";
    }
    return "unknown";
}

const char* app_state_commit_name(AppStateCommitResult result)
{
    switch (result)
    {
    case APP_STATE_COMMIT_OK: return "ok";
    case APP_STATE_COMMIT_UNSUPPORTED: return "unsupported_capability";
    case APP_STATE_COMMIT_NO_PAYLOAD: return "no_payload";
    case APP_STATE_COMMIT_UNSAFE_PATH: return "unsafe_path";
    case APP_STATE_COMMIT_TOO_LARGE: return "too_large";
    case APP_STATE_COMMIT_IO: return "io_error";
    }
    return "unknown";
}

int app_state_prepare(const char* root, const char* pak_dir,
                      const PakManifest* manifest, AppStateInfo* info)
{
    char app_id[APP_STATE_ID_MAX];
    char slot[APP_STATE_PATH_MAX];
    char payload[APP_STATE_PATH_MAX];

    (void)pak_dir;
    if (info != NULL) memset(info, 0, sizeof *info);
    if (manifest == NULL || manifest->status != PAK_MANIFEST_OK
        || !pak_manifest_has_capability(manifest, PAK_CAPABILITY_SAVES))
        return 0;
    if (!derive_paths(root, manifest, app_id, sizeof app_id, slot, sizeof slot,
                      payload, sizeof payload))
        return 0;
    if (!ensure_dir(root)) return 0;
    if (!ensure_owned_dir(slot)) return 0;
    if (!ensure_owned_dir(payload)) return 0;
    if (info != NULL)
    {
        info->status = APP_STATE_ABSENT;
        copy_string(info->app_id, sizeof info->app_id, app_id);
        copy_string(info->app_version, sizeof info->app_version,
                    manifest->version_valid ? manifest->version : "");
        copy_string(info->slot_dir, sizeof info->slot_dir, slot);
        copy_string(info->payload_dir, sizeof info->payload_dir, payload);
        set_reason(info->reason, sizeof info->reason, "absent");
    }
    return 1;
}

AppStateStatus app_state_inspect(const char* root, const char* pak_dir,
                                 const PakManifest* manifest, AppStateInfo* info)
{
    AppStateInfo local;
    char app_id[APP_STATE_ID_MAX];
    char slot[APP_STATE_PATH_MAX];
    char payload[APP_STATE_PATH_MAX];
    char meta[APP_STATE_PATH_MAX];
    StateFileList files;
    unsigned long long total = 0;
    long long saved_at = 0;
    struct stat st;
    size_t i;

    memset(&local, 0, sizeof local);
    if (info == NULL) info = &local;
    memset(info, 0, sizeof *info);

    if (root == NULL || root[0] == '\0' || pak_dir == NULL || manifest == NULL)
    {
        info->status = APP_STATE_ERROR;
        set_reason(info->reason, sizeof info->reason, "invalid_arguments");
        return info->status;
    }
    if (manifest->status != PAK_MANIFEST_OK
        || !pak_manifest_has_capability(manifest, PAK_CAPABILITY_SAVES))
    {
        info->status = APP_STATE_UNSUPPORTED;
        set_reason(info->reason, sizeof info->reason, "capability_not_declared");
        return info->status;
    }
    if (!derive_paths(root, manifest, app_id, sizeof app_id, slot, sizeof slot,
                      payload, sizeof payload))
    {
        info->status = APP_STATE_UNSUPPORTED;
        set_reason(info->reason, sizeof info->reason, "invalid_app_id");
        return info->status;
    }

    copy_string(info->app_id, sizeof info->app_id, app_id);
    copy_string(info->app_version, sizeof info->app_version,
                manifest->version_valid ? manifest->version : "");
    copy_string(info->slot_dir, sizeof info->slot_dir, slot);
    copy_string(info->payload_dir, sizeof info->payload_dir, payload);

    if (lstat(slot, &st) != 0)
    {
        if (errno == ENOENT)
        {
            info->status = APP_STATE_ABSENT;
            set_reason(info->reason, sizeof info->reason, "absent");
        }
        else
        {
            info->status = APP_STATE_ERROR;
            set_reason(info->reason, sizeof info->reason, "io_error");
        }
        return info->status;
    }
    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))
    {
        info->status = APP_STATE_INVALID;
        set_reason(info->reason, sizeof info->reason, "unsafe_slot");
        return info->status;
    }

    if (!metadata_path(slot, meta, sizeof meta))
    {
        info->status = APP_STATE_ERROR;
        set_reason(info->reason, sizeof info->reason, "invalid_arguments");
        return info->status;
    }
    if (lstat(meta, &st) != 0)
    {
        if (errno == ENOENT)
        {
            info->status = APP_STATE_ABSENT;
            set_reason(info->reason, sizeof info->reason, "absent");
        }
        else
        {
            info->status = APP_STATE_ERROR;
            set_reason(info->reason, sizeof info->reason, "io_error");
        }
        return info->status;
    }
    if (!S_ISREG(st.st_mode) || S_ISLNK(st.st_mode))
    {
        info->status = APP_STATE_INVALID;
        set_reason(info->reason, sizeof info->reason, "unsafe_metadata");
        return info->status;
    }

    if (!read_metadata(meta, app_id, &files, info->app_version, sizeof info->app_version,
                       &saved_at, &total, info->reason, sizeof info->reason))
    {
        info->status = strcmp(info->reason, "absent") == 0 ? APP_STATE_ABSENT : APP_STATE_INVALID;
        file_list_free(&files);
        return info->status;
    }

    if (lstat(payload, &st) != 0)
    {
        file_list_free(&files);
        info->status = APP_STATE_INVALID;
        set_reason(info->reason, sizeof info->reason,
                   errno == ENOENT ? "missing_payload" : "io_error");
        return info->status;
    }
    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))
    {
        file_list_free(&files);
        info->status = APP_STATE_INVALID;
        set_reason(info->reason, sizeof info->reason, "unsafe_payload");
        return info->status;
    }

    for (i = 0; i < files.count; i++)
    {
        char full[APP_STATE_PATH_MAX];
        int found = safe_relative_file(payload, files.items[i].path, full, sizeof full, &st);
        unsigned char digest[APP_STATE_HASH_BYTES];
        unsigned long long actual_size = 0;

        if (found < 0)
        {
            file_list_free(&files);
            info->status = APP_STATE_INVALID;
            set_reason(info->reason, sizeof info->reason, "unsafe_path");
            return info->status;
        }
        if (found == 0)
        {
            file_list_free(&files);
            info->status = APP_STATE_INVALID;
            set_reason(info->reason, sizeof info->reason,
                       errno == ENOENT ? "missing_payload" : "io_error");
            return info->status;
        }
        if (!S_ISREG(st.st_mode) || (unsigned long long)st.st_size != files.items[i].size)
        {
            file_list_free(&files);
            info->status = APP_STATE_INVALID;
            set_reason(info->reason, sizeof info->reason, "payload_size");
            return info->status;
        }
        if (!hash_file(full, &actual_size, digest, info->reason, sizeof info->reason))
        {
            file_list_free(&files);
            info->status = APP_STATE_INVALID;
            return info->status;
        }
        if (actual_size != files.items[i].size
            || memcmp(digest, files.items[i].hash, APP_STATE_HASH_BYTES) != 0)
        {
            file_list_free(&files);
            info->status = APP_STATE_INVALID;
            set_reason(info->reason, sizeof info->reason, "payload_hash");
            return info->status;
        }
    }

    info->status = APP_STATE_VALID;
    info->saved_at = saved_at;
    info->file_count = (unsigned)files.count;
    info->total_bytes = total;
    set_reason(info->reason, sizeof info->reason, "ok");
    file_list_free(&files);
    return info->status;
}

int app_state_is_resumable(const char* root, const char* pak_dir,
                           const PakManifest* manifest)
{
    return app_state_inspect(root, pak_dir, manifest, NULL) == APP_STATE_VALID;
}

AppStateCommitResult app_state_commit(const char* root, const char* pak_dir,
                                      const PakManifest* manifest,
                                      char* error, size_t error_size)
{
    char app_id[APP_STATE_ID_MAX];
    char slot[APP_STATE_PATH_MAX];
    char payload[APP_STATE_PATH_MAX];
    char meta[APP_STATE_PATH_MAX];
    char temp[APP_STATE_PATH_MAX];
    char version[APP_STATE_VERSION_MAX];
    StateFileList files;
    char reason[APP_STATE_REASON_MAX];
    struct stat st;
    FILE* file = NULL;
    int fd = -1;
    int ok = 0;
    size_t i;

    set_reason(error, error_size, "ok");
    memset(&files, 0, sizeof files);
    memset(reason, 0, sizeof reason);

    if (root == NULL || pak_dir == NULL || manifest == NULL
        || manifest->status != PAK_MANIFEST_OK
        || !pak_manifest_has_capability(manifest, PAK_CAPABILITY_SAVES))
    {
        set_reason(error, error_size, "unsupported_capability");
        return APP_STATE_COMMIT_UNSUPPORTED;
    }
    if (!derive_paths(root, manifest, app_id, sizeof app_id, slot, sizeof slot,
                      payload, sizeof payload))
    {
        set_reason(error, error_size, "unsupported_capability");
        return APP_STATE_COMMIT_UNSUPPORTED;
    }
    copy_string(version, sizeof version, manifest->version_valid ? manifest->version : "-");

    if (!ensure_dir(root) || !ensure_owned_dir(slot) || !ensure_owned_dir(payload))
    {
        set_reason(error, error_size, "io_error");
        return APP_STATE_COMMIT_IO;
    }
    if (!metadata_path(slot, meta, sizeof meta))
    {
        set_reason(error, error_size, "io_error");
        return APP_STATE_COMMIT_IO;
    }

    if (!collect_dir(payload, "", 1, &files, reason, sizeof reason))
    {
        AppStateCommitResult result = APP_STATE_COMMIT_IO;
        if (strcmp(reason, "unsafe_path") == 0) result = APP_STATE_COMMIT_UNSAFE_PATH;
        else if (strcmp(reason, "too_large") == 0) result = APP_STATE_COMMIT_TOO_LARGE;
        else if (strcmp(reason, "no_payload") == 0) result = APP_STATE_COMMIT_NO_PAYLOAD;
        set_reason(error, error_size, reason);
        file_list_free(&files);
        return result;
    }
    if (files.count == 0)
    {
        set_reason(error, error_size, "no_payload");
        file_list_free(&files);
        return APP_STATE_COMMIT_NO_PAYLOAD;
    }
    qsort(files.items, files.count, sizeof files.items[0], compare_files);

    if (snprintf(temp, sizeof temp, "%s/.resume.meta.tmp.XXXXXX", slot) >= (int)sizeof temp)
    {
        set_reason(error, error_size, "io_error");
        file_list_free(&files);
        return APP_STATE_COMMIT_IO;
    }
    fd = mkstemp(temp);
    if (fd < 0)
    {
        set_reason(error, error_size, "io_error");
        file_list_free(&files);
        return APP_STATE_COMMIT_IO;
    }
    file = fdopen(fd, "w");
    if (file == NULL)
    {
        close(fd);
        unlink(temp);
        set_reason(error, error_size, "io_error");
        file_list_free(&files);
        return APP_STATE_COMMIT_IO;
    }

    fprintf(file, "%s\n", APP_STATE_HEADER);
    fprintf(file, "app %s\n", app_id);
    fprintf(file, "app_version %s\n", version);
    fprintf(file, "saved_at %lld\n", (long long)time(NULL));
    fprintf(file, "files %u\n", (unsigned)files.count);
    fprintf(file, "total_bytes %llu\n", files.total_bytes);
    for (i = 0; i < files.count; i++)
    {
        char hex[APP_STATE_HASH_HEX + 1];
        hex_encode(files.items[i].hash, APP_STATE_HASH_BYTES, hex);
        fprintf(file, "file %s %llu %s\n", files.items[i].path, files.items[i].size, hex);
    }
    if (fflush(file) != 0 || fsync(fileno(file)) != 0)
    {
        fclose(file);
        unlink(temp);
        set_reason(error, error_size, "io_error");
        file_list_free(&files);
        return APP_STATE_COMMIT_IO;
    }
    if (fclose(file) != 0)
    {
        unlink(temp);
        set_reason(error, error_size, "io_error");
        file_list_free(&files);
        return APP_STATE_COMMIT_IO;
    }

    if (rename(temp, meta) != 0)
    {
        unlink(temp);
        set_reason(error, error_size, "io_error");
        file_list_free(&files);
        return APP_STATE_COMMIT_IO;
    }
    fd = open(slot, O_RDONLY | O_DIRECTORY);
    if (fd >= 0)
    {
        fsync(fd);
        close(fd);
    }

    ok = (lstat(meta, &st) == 0 && S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode));
    if (!ok) set_reason(error, error_size, "io_error");
    file_list_free(&files);
    return ok ? APP_STATE_COMMIT_OK : APP_STATE_COMMIT_IO;
}

int app_state_clear(const char* root, const char* pak_dir,
                    const PakManifest* manifest, char* error, size_t error_size)
{
    char app_id[APP_STATE_ID_MAX];
    char slot[APP_STATE_PATH_MAX];
    char payload[APP_STATE_PATH_MAX];
    char meta[APP_STATE_PATH_MAX];
    struct stat st;

    (void)pak_dir;
    set_reason(error, error_size, "ok");
    if (!derive_paths(root, manifest, app_id, sizeof app_id, slot, sizeof slot,
                      payload, sizeof payload))
    {
        set_reason(error, error_size, "invalid_manifest");
        return 0;
    }
    if (lstat(slot, &st) != 0)
    {
        if (errno == ENOENT) return 1;
        set_reason(error, error_size, "io_error");
        return 0;
    }
    if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode))
    {
        if (unlink(slot) != 0 && errno != ENOENT)
        {
            set_reason(error, error_size, "io_error");
            return 0;
        }
        return 1;
    }

    if (!metadata_path(slot, meta, sizeof meta))
    {
        set_reason(error, error_size, "io_error");
        return 0;
    }
    if (unlink(meta) != 0 && errno != ENOENT && errno != EISDIR)
    {
        set_reason(error, error_size, "io_error");
        return 0;
    }
    if (!remove_tree(slot, 0))
    {
        set_reason(error, error_size, "io_error");
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Launch command                                                             */
/* ------------------------------------------------------------------------- */

static int append_text(char* out, size_t out_size, size_t* used, const char* text)
{
    size_t length = strlen(text);

    if (*used + length >= out_size) return 0;
    memcpy(out + *used, text, length);
    *used += length;
    out[*used] = '\0';
    return 1;
}

static int append_quoted(char* out, size_t out_size, size_t* used, const char* value)
{
    const char* p;

    if (!append_text(out, out_size, used, "'")) return 0;
    for (p = value; *p != '\0'; p++)
    {
        if (*p == '\'')
        {
            if (!append_text(out, out_size, used, "'\\''")) return 0;
        }
        else
        {
            char one[2];
            one[0] = *p;
            one[1] = '\0';
            if (!append_text(out, out_size, used, one)) return 0;
        }
    }
    return append_text(out, out_size, used, "'");
}

static int append_env(char* out, size_t out_size, size_t* used,
                      const char* name, const char* value)
{
    return append_text(out, out_size, used, name) && append_text(out, out_size, used, "=")
           && append_quoted(out, out_size, used, value)
           && append_text(out, out_size, used, " ");
}

int app_state_build_launch(const char* root, const char* pak_dir,
                           const PakManifest* manifest, AppLaunchMode mode,
                           char* out, size_t out_size)
{
    AppStateInfo info;
    char launch_path[APP_STATE_PATH_MAX];
    size_t used = 0;
    int declared;

    if (out == NULL || out_size == 0 || pak_dir == NULL || pak_dir[0] == '\0') return 0;
    out[0] = '\0';
    if (!join_path(launch_path, sizeof launch_path, pak_dir, "launch.sh")) return 0;

    declared = manifest != NULL && manifest->status == PAK_MANIFEST_OK
               && pak_manifest_has_capability(manifest, PAK_CAPABILITY_SAVES);

    if (!declared)
    {
        if (mode == APP_LAUNCH_RESUME) return 0;
        return append_quoted(out, out_size, &used, launch_path);
    }

    if (mode == APP_LAUNCH_RESUME)
    {
        if (app_state_inspect(root, pak_dir, manifest, &info) != APP_STATE_VALID) return 0;
    }
    else if (!app_state_prepare(root, pak_dir, manifest, &info))
    {
        /* State is unavailable: still launch, but without claiming support. */
        return append_quoted(out, out_size, &used, launch_path);
    }

    if (!append_env(out, out_size, &used, "NEXTUI_APP_ID", info.app_id)
        || !append_env(out, out_size, &used, "NEXTUI_APP_PAK_DIR", pak_dir)
        || !append_env(out, out_size, &used, "NEXTUI_APP_STATE_DIR", info.payload_dir)
        || !append_text(out, out_size, &used,
                        mode == APP_LAUNCH_RESUME ? "NEXTUI_APP_RESUME=1 " : "NEXTUI_APP_RESUME=0 ")
        || !append_quoted(out, out_size, &used, launch_path))
        return 0;

    return 1;
}
