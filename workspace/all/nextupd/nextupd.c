#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include "update_manifest.h"
#include "update_signature.h"
#include "update_transaction.h"
#include "update_zip.h"
#include "sha256_lite.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

typedef struct
{
    const char* package;
    const char* manifest;
    const char* signature;
    const char* trusted_key;
    const char* platform;
    const char* system_version;
    const char* staging_root;
    const char* id;
    const char* kind;
    const char* version;
    const char* min_system;
    const char* out;
    const char* staged_dir;
    const char* target_root;
    int integrity_only;
    int replace;
    int rollback;
    int json;
} Options;

typedef struct
{
    UpdManifest manifest;
    char key_id[UPD_MAX_KEY_ID + 1];
    int signature_verified;
} VerifiedPackage;

static void usage(FILE* out)
{
    fprintf(out,
            "usage: nextupd <command> [options]\n"
            "\n"
            "  check     verify a package against a signed manifest; write nothing\n"
            "  stage     verify and publish a package into a staging directory\n"
            "  install   apply a staged system package to an explicit target root\n"
            "  recover   resolve an interrupted install in an explicit target root\n"
            "  manifest  build a canonical manifest from a package\n"
            "\n"
            "check:\n"
            "  --package <file> --manifest <file> --platform <platform>\n"
            "  [--signature <file> --trusted-key <file>] [--integrity-only]\n"
            "  [--system-version <vX.Y.Z>] [--json]\n"
            "\n"
            "stage:\n"
            "  --package <file> --manifest <file> --signature <file>\n"
            "  --trusted-key <file> --platform <platform>\n"
            "  [--staging-root <dir>] [--replace] [--system-version <vX.Y.Z>] [--json]\n"
            "\n"
            "install:\n"
            "  --staged-dir <dir> --target-root <dir> --trusted-key <file>\n"
            "  --platform <platform> --system-version <vX.Y.Z> [--json]\n"
            "\n"
            "recover:\n"
            "  --target-root <dir> [--rollback] [--json]\n"
            "\n"
            "manifest:\n"
            "  --package <file> --id <id> --kind <system|app> --version <vX.Y.Z>\n"
            "  --platform <platform> [--min-system <vX.Y.Z>] [--out <file>]\n"
            "\n"
            "check without --integrity-only fails when no signature and trusted key are\n"
            "provided. stage always requires both. SHA-256 detects damage; it does not\n"
            "establish who produced a manifest. install writes only inside an explicit\n"
            "--target-root that already contains .system; there is no default root and no\n"
            "boot or launch activation. recover is the offline arbitration entry point.\n");
}

static const char* path_basename(const char* path)
{
    const char* slash = strrchr(path, '/');
    return slash == NULL ? path : slash + 1;
}

static void print_json_string(const char* text)
{
    const unsigned char* cursor = (const unsigned char*)text;

    fputc('"', stdout);
    for (; *cursor != '\0'; cursor++)
    {
        switch (*cursor)
        {
        case '"':
            fputs("\\\"", stdout);
            break;
        case '\\':
            fputs("\\\\", stdout);
            break;
        case '\n':
            fputs("\\n", stdout);
            break;
        case '\r':
            fputs("\\r", stdout);
            break;
        case '\t':
            fputs("\\t", stdout);
            break;
        default:
            if (*cursor < 0x20u)
                fprintf(stdout, "\\u%04x", (unsigned)*cursor);
            else
                fputc(*cursor, stdout);
            break;
        }
    }
    fputc('"', stdout);
}

static int report_error(const UpdError* error, int json)
{
    const char* reason = error->reason[0] == '\0' ? "error" : error->reason;
    const char* detail = error->detail[0] == '\0' ? reason : error->detail;

    if (json)
    {
        fputs("{\"ok\":false,\"result\":\"error\",\"code\":", stdout);
        fprintf(stdout, "%d", (int)error->code);
        fputs(",\"reason\":", stdout);
        print_json_string(reason);
        fputs(",\"detail\":", stdout);
        print_json_string(detail);
        fputs("}\n", stdout);
    }
    else
    {
        fprintf(stderr, "nextupd: %s: %s\n", reason, detail);
    }
    return (int)error->code;
}

static const char* resolved_platform(const Options* options)
{
    const char* platform = options->platform;
    if (platform == NULL || platform[0] == '\0') platform = getenv("PLATFORM");
    return platform;
}

static const char* resolved_system_version(const Options* options)
{
    const char* version = options->system_version;
    if (version == NULL || version[0] == '\0') version = getenv("NEXTUI_SYSTEM_VERSION");
    return version;
}

static int check_extra_files(const UpdManifest* manifest, const UpdZip* zip, UpdError* error)
{
    size_t manifest_index = 0;
    size_t zip_index;

    for (zip_index = 0; zip_index < zip->file_count; zip_index++)
    {
        while (manifest_index < manifest->file_count
               && strcmp(manifest->files[manifest_index].path, zip->files[zip_index].name) < 0)
            manifest_index++;
        if (manifest_index >= manifest->file_count
            || strcmp(manifest->files[manifest_index].path, zip->files[zip_index].name) != 0)
        {
            upd_error_set(error, UPD_E_INVALID, "unlisted_file",
                          "archive contains an unlisted file: %s",
                          zip->files[zip_index].name);
            return 0;
        }
    }
    return 1;
}

static int check_directories(const UpdManifest* manifest, const UpdZip* zip, UpdError* error)
{
    size_t directory_index;
    size_t file_index;

    for (directory_index = 0; directory_index < zip->directory_count; directory_index++)
    {
        const char* directory = zip->directories[directory_index];
        size_t length = strlen(directory);
        int implied = 0;

        for (file_index = 0; file_index < manifest->file_count; file_index++)
        {
            if (strncmp(manifest->files[file_index].path, directory, length) == 0
                && manifest->files[file_index].path[length] == '/')
            {
                implied = 1;
                break;
            }
        }
        if (!implied)
        {
            upd_error_set(error, UPD_E_INVALID, "unlisted_entry",
                          "archive contains an unlisted directory: %s", directory);
            return 0;
        }
    }
    return 1;
}

static int verify_archive(const Options* options, const UpdManifest* manifest,
                          UpdError* error)
{
    UpdZip zip;
    unsigned char archive_digest[UPD_SHA256_BYTES];
    unsigned char file_digest[UPD_SHA256_BYTES];
    uint64_t archive_size = 0;
    uint64_t file_size = 0;
    size_t i;

    if (strcmp(path_basename(options->package), manifest->archive_name) != 0)
    {
        upd_error_set(error, UPD_E_INVALID, "archive_name_mismatch",
                      "package is named %s but the manifest expects %s",
                      path_basename(options->package), manifest->archive_name);
        return 0;
    }
    if (!upd_file_size(options->package, &archive_size, error))
    {
        if (strcmp(error->reason, "file_missing") == 0)
            upd_error_set(error, UPD_E_MISSING, "archive_missing", "%s", error->detail);
        return 0;
    }
    if (archive_size != manifest->archive_size)
    {
        upd_error_set(error, UPD_E_INTEGRITY, "archive_size_mismatch",
                      "archive is %llu bytes, manifest says %llu",
                      (unsigned long long)archive_size,
                      (unsigned long long)manifest->archive_size);
        return 0;
    }
    if (!upd_hash_file(options->package, archive_digest, &archive_size,
                       UPD_MAX_ARCHIVE_BYTES, error))
        return 0;
    if (memcmp(archive_digest, manifest->archive_sha256, UPD_SHA256_BYTES) != 0)
    {
        char expected[UPD_SHA256_HEX];
        char actual[UPD_SHA256_HEX];
        upd_hex_encode(manifest->archive_sha256, UPD_SHA256_BYTES, expected);
        upd_hex_encode(archive_digest, UPD_SHA256_BYTES, actual);
        upd_error_set(error, UPD_E_INTEGRITY, "archive_hash_mismatch",
                      "archive sha256 is %s, manifest says %s", actual, expected);
        return 0;
    }

    upd_zip_init(&zip);
    if (!upd_zip_open(options->package, &zip, error)) return 0;
    for (i = 0; i < manifest->file_count; i++)
    {
        const UpdZipEntry* entry = upd_zip_find(&zip, manifest->files[i].path);
        if (entry == NULL)
        {
            upd_error_set(error, UPD_E_MISSING, "missing_file",
                          "manifest file is missing from archive: %s",
                          manifest->files[i].path);
            upd_zip_close(&zip);
            return 0;
        }
        if (!upd_zip_hash_entry(&zip, entry, manifest->files[i].size, file_digest,
                                &file_size, error))
        {
            upd_zip_close(&zip);
            return 0;
        }
        if (file_size != manifest->files[i].size)
        {
            upd_error_set(error, UPD_E_INTEGRITY, "file_size_mismatch",
                          "%s: got %llu bytes, manifest says %llu",
                          manifest->files[i].path, (unsigned long long)file_size,
                          (unsigned long long)manifest->files[i].size);
            upd_zip_close(&zip);
            return 0;
        }
        if (memcmp(file_digest, manifest->files[i].sha256, UPD_SHA256_BYTES) != 0)
        {
            upd_error_set(error, UPD_E_INTEGRITY, "file_hash_mismatch",
                          "%s: SHA-256 mismatch", manifest->files[i].path);
            upd_zip_close(&zip);
            return 0;
        }
    }
    if (!check_extra_files(manifest, &zip, error))
    {
        upd_zip_close(&zip);
        return 0;
    }
    if (!check_directories(manifest, &zip, error))
    {
        upd_zip_close(&zip);
        return 0;
    }
    upd_zip_close(&zip);
    return 1;
}

static int verify_package(const Options* options, int require_signature,
                          VerifiedPackage* verified, UpdError* error)
{
    const char* platform;
    const char* system_version;

    memset(verified, 0, sizeof *verified);
    upd_manifest_init(&verified->manifest);
    if (!upd_manifest_load(options->manifest, &verified->manifest, error)) return 0;

    if (options->signature != NULL || options->trusted_key != NULL)
    {
        if (!upd_signature_verify(verified->manifest.raw, verified->manifest.raw_length,
                                  options->signature, options->trusted_key,
                                  verified->key_id, sizeof verified->key_id, error))
            return 0;
        verified->signature_verified = 1;
    }
    else if (require_signature)
    {
        upd_error_set(error, UPD_E_SIGNATURE, "signature_missing",
                      "check/stage requires --signature and --trusted-key");
        return 0;
    }

    platform = resolved_platform(options);
    if (platform == NULL || platform[0] == '\0')
    {
        upd_error_set(error, UPD_E_USAGE, "platform_missing",
                      "--platform or PLATFORM is required");
        return 0;
    }
    if (!upd_manifest_platform_matches(&verified->manifest, platform))
    {
        upd_error_set(error, UPD_E_PLATFORM, "platform_mismatch",
                      "package is for %s, target is %s", verified->manifest.platform,
                      platform);
        return 0;
    }
    system_version = resolved_system_version(options);
    if (!upd_manifest_check_min_system(&verified->manifest, system_version, error)) return 0;
    return verify_archive(options, &verified->manifest, error);
}

static void print_check_result(const Options* options, const VerifiedPackage* verified)
{
    char sha[UPD_SHA256_HEX];
    const char* platform = resolved_platform(options);

    upd_hex_encode(verified->manifest.archive_sha256, UPD_SHA256_BYTES, sha);
    if (!verified->signature_verified)
        fprintf(stderr,
                "nextupd: warning: authenticity is NOT verified; SHA-256 is not a signature\n");
    if (options->json)
    {
        fputs("{\"ok\":true,\"result\":\"ok\",\"id\":", stdout);
        print_json_string(verified->manifest.id);
        fputs(",\"kind\":", stdout);
        print_json_string(verified->manifest.kind);
        fputs(",\"version\":", stdout);
        print_json_string(verified->manifest.version);
        fputs(",\"platform\":", stdout);
        print_json_string(verified->manifest.platform);
        fputs(",\"target\":", stdout);
        print_json_string(platform);
        fputs(",\"archive\":", stdout);
        print_json_string(verified->manifest.archive_name);
        fprintf(stdout,
                ",\"archive_size\":%llu,\"archive_sha256\":\"%s\",\"files\":%zu,"
                "\"bytes\":%llu,\"signature_verified\":%s,\"signature_key_id\":",
                (unsigned long long)verified->manifest.archive_size, sha,
                verified->manifest.file_count,
                (unsigned long long)verified->manifest.total_bytes,
                verified->signature_verified ? "true" : "false");
        print_json_string(verified->key_id);
        fputs("}\n", stdout);
        return;
    }

    printf("package: %s %s (%s)\n", verified->manifest.id, verified->manifest.version,
           verified->manifest.kind);
    printf("platform: %s -> %s\n", verified->manifest.platform, platform);
    printf("archive: %s (%llu bytes, sha256 %s)\n", verified->manifest.archive_name,
           (unsigned long long)verified->manifest.archive_size, sha);
    printf("files: %zu (%llu bytes) verified\n", verified->manifest.file_count,
           (unsigned long long)verified->manifest.total_bytes);
    if (verified->signature_verified)
        printf("signature: verified (key %s)\n", verified->key_id);
    else
        printf("signature: unverified (integrity only)\n");
    printf("result: ok\n");
}

static int run_check(const Options* options)
{
    VerifiedPackage verified;
    UpdError error;
    int result;

    upd_error_clear(&error);
    if (!verify_package(options, !options->integrity_only, &verified, &error))
    {
        result = report_error(&error, options->json);
        upd_manifest_free(&verified.manifest);
        return result;
    }
    print_check_result(options, &verified);
    upd_manifest_free(&verified.manifest);
    return 0;
}

static int copy_option_text(char* out, size_t out_size, const char* value,
                            const char* field, UpdError* error)
{
    size_t length = value == NULL ? 0 : strlen(value);
    if (value == NULL || length >= out_size)
    {
        upd_error_set(error, UPD_E_INVALID, "field_too_long", "%s is too long", field);
        return 0;
    }
    memcpy(out, value, length + 1);
    return 1;
}

static int run_manifest(const Options* options)
{
    UpdManifest manifest;
    UpdZip zip;
    UpdError error;
    unsigned char digest[UPD_SHA256_BYTES];
    uint64_t archive_size = 0;
    char* rendered = NULL;
    size_t rendered_length = 0;
    size_t i;
    int result;

    upd_error_clear(&error);
    upd_manifest_init(&manifest);
    if (!copy_option_text(manifest.id, sizeof manifest.id, options->id, "id", &error)
        || !copy_option_text(manifest.kind, sizeof manifest.kind, options->kind, "kind",
                             &error)
        || !copy_option_text(manifest.version, sizeof manifest.version, options->version,
                             "version", &error)
        || !copy_option_text(manifest.platform, sizeof manifest.platform, options->platform,
                             "platform", &error)
        || !copy_option_text(manifest.archive_name, sizeof manifest.archive_name,
                             path_basename(options->package), "archive name", &error))
    {
        result = report_error(&error, 0);
        goto done;
    }
    if (options->min_system != NULL)
    {
        if (!copy_option_text(manifest.min_system, sizeof manifest.min_system,
                              options->min_system, "min-system", &error))
        {
            result = report_error(&error, 0);
            goto done;
        }
        manifest.has_min_system = 1;
    }

    upd_zip_init(&zip);
    if (!upd_file_size(options->package, &archive_size, &error))
    {
        if (strcmp(error.reason, "file_missing") == 0)
            upd_error_set(&error, UPD_E_MISSING, "archive_missing", "%s", error.detail);
        result = report_error(&error, 0);
        goto done;
    }
    manifest.archive_size = archive_size;
    if (!upd_hash_file(options->package, manifest.archive_sha256, &archive_size,
                       UPD_MAX_ARCHIVE_BYTES, &error))
    {
        result = report_error(&error, 0);
        goto done;
    }
    if (!upd_manifest_check_identity(&manifest, &error))
    {
        result = report_error(&error, 0);
        goto done;
    }
    if (!upd_zip_open(options->package, &zip, &error))
    {
        result = report_error(&error, 0);
        goto done;
    }
    for (i = 0; i < zip.file_count; i++)
    {
        uint64_t actual_size = 0;
        if (!upd_zip_hash_entry(&zip, &zip.files[i], zip.files[i].uncompressed_size, digest,
                                &actual_size, &error))
        {
            result = report_error(&error, 0);
            upd_zip_close(&zip);
            goto done;
        }
        if (!upd_manifest_add_file(&manifest, zip.files[i].name, actual_size, digest, &error))
        {
            result = report_error(&error, 0);
            upd_zip_close(&zip);
            goto done;
        }
    }
    if (!check_directories(&manifest, &zip, &error))
    {
        result = report_error(&error, 0);
        upd_zip_close(&zip);
        goto done;
    }
    upd_zip_close(&zip);
    if (!upd_manifest_finalize(&manifest, &error))
    {
        result = report_error(&error, 0);
        goto done;
    }
    if (!upd_manifest_render(&manifest, &rendered, &rendered_length, &error))
    {
        result = report_error(&error, 0);
        goto done;
    }
    if (options->out != NULL)
    {
        if (!upd_manifest_write(&manifest, options->out, &error))
        {
            result = report_error(&error, 0);
            goto done;
        }
        printf("wrote %s: %s %s %s, %zu files, %llu bytes\n", options->out, manifest.id,
               manifest.version, manifest.platform, manifest.file_count,
               (unsigned long long)manifest.total_bytes);
    }
    else if (fwrite(rendered, 1, rendered_length, stdout) != rendered_length)
    {
        upd_error_set(&error, UPD_E_IO, "io_error", "cannot write manifest to stdout");
        result = report_error(&error, 0);
        goto done;
    }
    result = 0;

done:
    free(rendered);
    upd_manifest_free(&manifest);
    return result;
}

#ifdef NEXTUPD_TESTING
static int test_limit(const char* name, long long* value)
{
    const char* text = getenv(name);
    char* end = NULL;
    long long parsed;

    if (text == NULL || text[0] == '\0') return 0;
    parsed = strtoll(text, &end, 10);
    if (end == NULL || *end != '\0' || parsed < 0) return 0;
    *value = parsed;
    return 1;
}
#endif

static int copy_file_verified(const char* source, const char* destination,
                              uint64_t expected_size,
                              const unsigned char expected_digest[UPD_SHA256_BYTES],
                              UpdError* error)
{
    unsigned char buffer[64 * 1024];
    unsigned char digest[UPD_SHA256_BYTES];
    Sha256Lite context;
    uint64_t total = 0;
    uint64_t source_size = 0;
    int input = -1;
    int output = -1;

    if (!upd_file_size(source, &source_size, error)) return 0;
    if (source_size != expected_size)
    {
        upd_error_set(error, UPD_E_INTEGRITY, "file_size_mismatch",
                      "source is %llu bytes, expected %llu",
                      (unsigned long long)source_size, (unsigned long long)expected_size);
        return 0;
    }
    input = open(source, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    output = open(destination, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (input < 0 || output < 0)
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot copy %s: %s", source,
                      strerror(errno));
        if (input >= 0) close(input);
        if (output >= 0) close(output);
        unlink(destination);
        return 0;
    }
    sha256_lite_init(&context);
    for (;;)
    {
        ssize_t got = read(input, buffer, sizeof buffer);
        if (got < 0)
        {
            if (errno == EINTR) continue;
            upd_error_set(error, UPD_E_IO, "io_error", "read %s: %s", source,
                          strerror(errno));
            goto failed;
        }
        if (got == 0) break;
        {
            size_t to_write = (size_t)got;
            int fail_after_write = 0;
            int crash_after_write = 0;
#ifdef NEXTUPD_TESTING
            {
                long long fail_after;
                long long crash_after;
                int fail_set = test_limit("NEXTUPD_TEST_FAIL_AFTER_BYTES", &fail_after);
                int crash_set = test_limit("NEXTUPD_TEST_CRASH_AFTER_BYTES", &crash_after);
                if (fail_set && total >= (uint64_t)fail_after)
                {
                    upd_error_set(error, UPD_E_INTERRUPTED, "interrupted",
                                  "test hook stopped after %llu bytes",
                                  (unsigned long long)total);
                    goto failed;
                }
                if (crash_set && total >= (uint64_t)crash_after) _exit(70);
                if (fail_set && total + to_write > (uint64_t)fail_after)
                {
                    to_write = (size_t)((uint64_t)fail_after - total);
                    fail_after_write = 1;
                }
                else if (crash_set && total + to_write > (uint64_t)crash_after)
                {
                    to_write = (size_t)((uint64_t)crash_after - total);
                    crash_after_write = 1;
                }
            }
#endif
            {
                size_t written = 0;
                while (written < to_write)
                {
                    ssize_t count = write(output, buffer + written, to_write - written);
                    if (count < 0)
                    {
                        if (errno == EINTR) continue;
                        if (errno == ENOSPC)
                            upd_error_set(error, UPD_E_SPACE, "insufficient_space",
                                          "storage full while writing %s", destination);
                        else
                            upd_error_set(error, UPD_E_IO, "io_error", "write %s: %s",
                                          destination, strerror(errno));
                        goto failed;
                    }
                    written += (size_t)count;
                }
            }
            total += to_write;
            sha256_lite_update(&context, buffer, to_write);
            if (fail_after_write)
            {
                upd_error_set(error, UPD_E_INTERRUPTED, "interrupted",
                              "test hook stopped after %llu bytes",
                              (unsigned long long)total);
                goto failed;
            }
            if (crash_after_write) _exit(70);
        }
    }
    if (fsync(output) != 0)
    {
        if (errno == ENOSPC)
            upd_error_set(error, UPD_E_SPACE, "insufficient_space",
                          "storage full while syncing %s", destination);
        else
            upd_error_set(error, UPD_E_IO, "io_error", "sync %s: %s", destination,
                          strerror(errno));
        goto failed;
    }
    if (close(output) != 0)
    {
        output = -1;
        upd_error_set(error, UPD_E_IO, "io_error", "close %s: %s", destination,
                      strerror(errno));
        goto failed;
    }
    output = -1;
    close(input);
    input = -1;

    sha256_lite_final(&context, digest);
    if (total != expected_size || memcmp(digest, expected_digest, UPD_SHA256_BYTES) != 0)
    {
        upd_error_set(error, UPD_E_INTEGRITY, "archive_hash_mismatch",
                      "copied archive does not match the manifest");
        unlink(destination);
        return 0;
    }
    return 1;

failed:
    if (input >= 0) close(input);
    if (output >= 0) close(output);
    unlink(destination);
    return 0;
}

static int storage_free_bytes(const char* root, uint64_t* free_bytes, UpdError* error)
{
    struct statvfs status;
    uint64_t block_size;

    if (statvfs(root, &status) != 0)
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot stat staging filesystem: %s",
                      strerror(errno));
        return 0;
    }
    block_size = status.f_frsize != 0 ? status.f_frsize : status.f_bsize;
    *free_bytes = (uint64_t)status.f_bavail * block_size;
    return 1;
}

static void cleanup_stage(const char* stage_dir, const char* zip_path,
                          const char* manifest_path, const char* signature_path,
                          const char* marker_path)
{
    char part[4096];
    const char* paths[4];
    size_t i;

    paths[0] = zip_path;
    paths[1] = manifest_path;
    paths[2] = signature_path;
    paths[3] = marker_path;
    for (i = 0; i < 4; i++)
    {
        if (paths[i] == NULL) continue;
        unlink(paths[i]);
        if (snprintf(part, sizeof part, "%s.part", paths[i]) < (int)sizeof part)
            unlink(part);
    }
    if (stage_dir != NULL) rmdir(stage_dir);
}

static int run_stage(const Options* options)
{
    VerifiedPackage verified;
    UpdError error;
    char root[4096];
    char lock_path[4096];
    char base[256];
    char stage_dir[4096];
    char zip_path[4096];
    char manifest_path[4096];
    char signature_path[4096];
    char marker_path[4096];
    char zip_part[4096];
    unsigned char* signature_bytes = NULL;
    size_t signature_length = 0;
    unsigned char manifest_digest[UPD_SHA256_BYTES];
    char manifest_sha[UPD_SHA256_HEX];
    uint64_t signature_size = 0;
    uint64_t required = 0;
    uint64_t free_bytes;
    int lock_fd = -1;
    int marker_exists = 0;
    int renamed = 0;
    int result;
    char descriptor[2048];
    int descriptor_length;

    upd_error_clear(&error);
    if (!verify_package(options, 1, &verified, &error))
    {
        result = report_error(&error, options->json);
        upd_manifest_free(&verified.manifest);
        return result;
    }
    if (verified.manifest.raw_length > UPD_MAX_MANIFEST_BYTES
        || options->signature == NULL || options->trusted_key == NULL)
    {
        upd_error_set(&error, UPD_E_SIGNATURE, "signature_missing",
                      "stage requires a signed manifest and trusted key");
        result = report_error(&error, options->json);
        upd_manifest_free(&verified.manifest);
        return result;
    }
    if (options->staging_root != NULL)
        snprintf(root, sizeof root, "%s", options->staging_root);
    else
    {
        const char* sdcard = getenv("SDCARD_PATH");
        if (sdcard != NULL && sdcard[0] != '\0')
            snprintf(root, sizeof root, "%s/.tmp_update/staging", sdcard);
        else
            snprintf(root, sizeof root, ".nextupd-staging");
    }
    if (strlen(root) + 64 >= sizeof root)
    {
        upd_error_set(&error, UPD_E_USAGE, "path_too_long", "staging root is too long");
        result = report_error(&error, options->json);
        upd_manifest_free(&verified.manifest);
        return result;
    }
    if (!upd_mkdir_p(root, &error))
    {
        result = report_error(&error, options->json);
        upd_manifest_free(&verified.manifest);
        return result;
    }
    if (snprintf(lock_path, sizeof lock_path, "%s/.nextupd.lock", root) >= (int)sizeof lock_path)
    {
        upd_error_set(&error, UPD_E_USAGE, "path_too_long", "lock path is too long");
        result = report_error(&error, options->json);
        upd_manifest_free(&verified.manifest);
        return result;
    }
    lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lock_fd < 0)
    {
        upd_error_set(&error, UPD_E_IO, "io_error", "cannot open %s: %s", lock_path,
                      strerror(errno));
        result = report_error(&error, options->json);
        upd_manifest_free(&verified.manifest);
        return result;
    }
    if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0)
    {
        upd_error_set(&error, UPD_E_BUSY, "busy",
                      errno == EWOULDBLOCK ? "another stage is running"
                                           : "cannot lock staging directory");
        result = report_error(&error, options->json);
        close(lock_fd);
        upd_manifest_free(&verified.manifest);
        return result;
    }

    if (snprintf(base, sizeof base, "%s-%s-%s", verified.manifest.id,
                 verified.manifest.version, verified.manifest.platform)
        >= (int)sizeof base)
    {
        upd_error_set(&error, UPD_E_USAGE, "path_too_long", "package name is too long");
        result = report_error(&error, options->json);
        goto done;
    }
    if (snprintf(stage_dir, sizeof stage_dir, "%s/%s", root, base)
        >= (int)sizeof stage_dir)
    {
        upd_error_set(&error, UPD_E_USAGE, "path_too_long", "staging directory is too long");
        result = report_error(&error, options->json);
        goto done;
    }
    if (snprintf(zip_path, sizeof zip_path, "%s/%s", stage_dir,
                 verified.manifest.archive_name)
            >= (int)sizeof zip_path
        || snprintf(manifest_path, sizeof manifest_path, "%s/manifest.json", stage_dir)
               >= (int)sizeof manifest_path
        || snprintf(signature_path, sizeof signature_path, "%s/manifest.sig", stage_dir)
               >= (int)sizeof signature_path
        || snprintf(marker_path, sizeof marker_path, "%s/staged.json", stage_dir)
               >= (int)sizeof marker_path)
    {
        upd_error_set(&error, UPD_E_USAGE, "path_too_long", "staging path is too long");
        result = report_error(&error, options->json);
        goto done;
    }
    if (!upd_mkdir_p(stage_dir, &error))
    {
        result = report_error(&error, options->json);
        goto done;
    }
    marker_exists = access(marker_path, F_OK) == 0;
    if (marker_exists && !options->replace)
    {
        upd_error_set(&error, UPD_E_ALREADY_STAGED, "already_staged",
                      "%s %s is already staged; pass --replace to replace it",
                      verified.manifest.id, verified.manifest.version);
        result = report_error(&error, options->json);
        goto done;
    }
    if (marker_exists) unlink(marker_path);
    cleanup_stage(stage_dir, zip_path, manifest_path, signature_path, marker_path);
    if (!upd_mkdir_p(stage_dir, &error))
    {
        result = report_error(&error, options->json);
        goto done;
    }

    if (!upd_file_size(options->signature, &signature_size, &error))
    {
        if (strcmp(error.reason, "file_missing") == 0)
            upd_error_set(&error, UPD_E_SIGNATURE, "signature_missing", "%s", error.detail);
        result = report_error(&error, options->json);
        goto done;
    }
    if (verified.manifest.archive_size > UINT64_MAX - verified.manifest.raw_length
        || verified.manifest.archive_size + verified.manifest.raw_length
               > UINT64_MAX - signature_size
        || verified.manifest.archive_size + verified.manifest.raw_length + signature_size
               > UINT64_MAX - 64 * 1024)
    {
        upd_error_set(&error, UPD_E_INVALID, "too_large", "staging requirement overflows");
        result = report_error(&error, options->json);
        goto done;
    }
    required = verified.manifest.archive_size + verified.manifest.raw_length + signature_size
               + 64 * 1024;
    if (!storage_free_bytes(root, &free_bytes, &error))
    {
        result = report_error(&error, options->json);
        goto done;
    }
#ifdef NEXTUPD_TESTING
    {
        long long override;
        if (test_limit("NEXTUPD_TEST_FREE_BYTES", &override))
            free_bytes = (uint64_t)override;
    }
#endif
    if (free_bytes < required)
    {
        upd_error_set(&error, UPD_E_SPACE, "insufficient_space",
                      "need %llu bytes, %llu available", (unsigned long long)required,
                      (unsigned long long)free_bytes);
        result = report_error(&error, options->json);
        goto done;
    }
    if (snprintf(zip_part, sizeof zip_part, "%s.part", zip_path) >= (int)sizeof zip_part)
    {
        upd_error_set(&error, UPD_E_USAGE, "path_too_long", "staging path is too long");
        result = report_error(&error, options->json);
        goto done;
    }
    if (!copy_file_verified(options->package, zip_part, verified.manifest.archive_size,
                            verified.manifest.archive_sha256, &error))
    {
        result = report_error(&error, options->json);
        cleanup_stage(stage_dir, zip_path, manifest_path, signature_path, marker_path);
        goto done;
    }
    if (rename(zip_part, zip_path) != 0)
    {
        upd_error_set(&error, UPD_E_IO, "io_error", "publish %s: %s", zip_path,
                      strerror(errno));
        result = report_error(&error, options->json);
        cleanup_stage(stage_dir, zip_path, manifest_path, signature_path, marker_path);
        goto done;
    }
    renamed = 1;
    if (!upd_write_file_atomic(manifest_path, verified.manifest.raw,
                               verified.manifest.raw_length, &error))
    {
        result = report_error(&error, options->json);
        goto failed_stage;
    }
    if (!upd_read_file(options->signature, UPD_MAX_SIGNATURE_BYTES, &signature_bytes,
                       &signature_length, &error))
    {
        result = report_error(&error, options->json);
        goto failed_stage;
    }
    if (!upd_write_file_atomic(signature_path, signature_bytes, signature_length, &error))
    {
        result = report_error(&error, options->json);
        goto failed_stage;
    }
    {
        char copied_key_id[UPD_MAX_KEY_ID + 1];
        if (!upd_signature_verify(verified.manifest.raw, verified.manifest.raw_length,
                                  signature_path, options->trusted_key, copied_key_id,
                                  sizeof copied_key_id, &error))
        {
            result = report_error(&error, options->json);
            goto failed_stage;
        }
        if (strcmp(copied_key_id, verified.key_id) != 0)
        {
            upd_error_set(&error, UPD_E_SIGNATURE, "signature_key_mismatch",
                          "staged signature changed key id during staging");
            result = report_error(&error, options->json);
            goto failed_stage;
        }
    }
    {
        Sha256Lite context;
        sha256_lite_init(&context);
        sha256_lite_update(&context, verified.manifest.raw, verified.manifest.raw_length);
        sha256_lite_final(&context, manifest_digest);
        upd_hex_encode(manifest_digest, UPD_SHA256_BYTES, manifest_sha);
    }
    descriptor_length = snprintf(
        descriptor, sizeof descriptor,
        "{\n"
        "  \"schema\": \"nextui-staged-update-v1\",\n"
        "  \"status\": \"staged\",\n"
        "  \"id\": \"%s\",\n"
        "  \"kind\": \"%s\",\n"
        "  \"version\": \"%s\",\n"
        "  \"platform\": \"%s\",\n"
        "  \"archive\": \"%s\",\n"
        "  \"archive_sha256\": \"",
        verified.manifest.id, verified.manifest.kind, verified.manifest.version,
        verified.manifest.platform, verified.manifest.archive_name);
    if (descriptor_length < 0 || descriptor_length >= (int)sizeof descriptor)
    {
        upd_error_set(&error, UPD_E_IO, "io_error", "cannot render staging descriptor");
        result = report_error(&error, options->json);
        goto failed_stage;
    }
    {
        char archive_sha[UPD_SHA256_HEX];
        upd_hex_encode(verified.manifest.archive_sha256, UPD_SHA256_BYTES, archive_sha);
        descriptor_length += snprintf(
            descriptor + descriptor_length, sizeof descriptor - (size_t)descriptor_length,
            "%s\",\n"
            "  \"manifest\": \"manifest.json\",\n"
            "  \"manifest_sha256\": \"%s\",\n"
            "  \"signature\": \"manifest.sig\",\n"
            "  \"signature_key_id\": \"%s\",\n"
            "  \"staged_at\": %lld\n"
            "}\n",
            archive_sha, manifest_sha, verified.key_id,
            (long long)time(NULL));
    }
    if (descriptor_length < 0 || descriptor_length >= (int)sizeof descriptor)
    {
        upd_error_set(&error, UPD_E_IO, "io_error", "cannot render staging descriptor");
        result = report_error(&error, options->json);
        goto failed_stage;
    }
    if (!upd_write_file_atomic(marker_path, descriptor, (size_t)descriptor_length, &error))
    {
        result = report_error(&error, options->json);
        goto failed_stage;
    }
    if (!upd_fsync_dir(root))
        fprintf(stderr, "nextupd: warning: could not sync staging directory\n");
    if (options->json)
    {
        fputs("{\"ok\":true,\"result\":\"staged\",\"id\":", stdout);
        print_json_string(verified.manifest.id);
        fputs(",\"version\":", stdout);
        print_json_string(verified.manifest.version);
        fputs(",\"platform\":", stdout);
        print_json_string(verified.manifest.platform);
        fputs(",\"staging_root\":", stdout);
        print_json_string(root);
        fputs(",\"archive_path\":", stdout);
        print_json_string(zip_path);
        fputs(",\"manifest_path\":", stdout);
        print_json_string(manifest_path);
        fputs(",\"signature_path\":", stdout);
        print_json_string(signature_path);
        fputs(",\"marker_path\":", stdout);
        print_json_string(marker_path);
        fputs("}\n", stdout);
    }
    else
    {
        printf("staged %s %s at %s\n", verified.manifest.id, verified.manifest.version, root);
        printf("archive: %s\n", zip_path);
        printf("manifest: %s\n", manifest_path);
        printf("signature: %s (key %s)\n", signature_path, verified.key_id);
        printf("marker: %s\n", marker_path);
    }
    result = 0;
    goto done;

failed_stage:
    if (renamed) cleanup_stage(stage_dir, zip_path, manifest_path, signature_path, marker_path);
done:
    free(signature_bytes);
    if (lock_fd >= 0) close(lock_fd);
    upd_manifest_free(&verified.manifest);
    return result;
}

static void print_transaction_result(const Options* options,
                                     const UpdTransactionResult* result, const char* id,
                                     const char* target_root, const char* staged_dir)
{
    if (options->json)
    {
        fputs("{\"ok\":true,\"result\":", stdout);
        print_json_string(result->outcome);
        fputs(",\"outcome\":", stdout);
        print_json_string(result->outcome);
        fputs(",\"state\":", stdout);
        print_json_string(result->state);
        fputs(",\"version\":", stdout);
        print_json_string(result->version);
        fputs(",\"previous_version\":", stdout);
        print_json_string(result->previous_version);
        if (id != NULL)
        {
            fputs(",\"id\":", stdout);
            print_json_string(id);
        }
        if (target_root != NULL)
        {
            fputs(",\"target_root\":", stdout);
            print_json_string(target_root);
        }
        if (staged_dir != NULL)
        {
            fputs(",\"staged_dir\":", stdout);
            print_json_string(staged_dir);
        }
        fputs("}\n", stdout);
        return;
    }
    printf("%s: %s\n", result->outcome, result->state);
    if (id != NULL) printf("package: %s %s\n", id, result->version);
    else if (result->version[0] != '\0')
        printf("version: %s\n", result->version);
    if (result->previous_version[0] != '\0')
        printf("previous: %s\n", result->previous_version);
    if (target_root != NULL) printf("target: %s\n", target_root);
    if (staged_dir != NULL) printf("staged: %s\n", staged_dir);
}

typedef struct
{
    int descriptor;
} TargetLock;

/* Serialize install and recover on one target root. The lock lives beside the
 * live tree, never inside it, and is advisory: a crashed holder releases it
 * when the process exits. A missing root is left to upd_install_execute() so
 * it can report target_missing instead of a misleading lock error. */
static int target_lock_acquire(const char* root, TargetLock* lock, UpdError* error)
{
    struct stat status;
    int descriptor;

    lock->descriptor = -1;
    if (root == NULL || root[0] == '\0') return 1;
    if (stat(root, &status) != 0 || !S_ISDIR(status.st_mode)) return 1;
    descriptor = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0)
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot open %s: %s", root,
                      strerror(errno));
        return 0;
    }
    if (flock(descriptor, LOCK_EX | LOCK_NB) != 0)
    {
        upd_error_set(error, UPD_E_BUSY, "busy",
                      errno == EWOULDBLOCK ? "another install or recovery is running"
                                           : "cannot lock target root");
        close(descriptor);
        return 0;
    }
#ifdef NEXTUPD_TESTING
    {
        const char* marker = getenv("NEXTUPD_TEST_LOCK_MARKER");
        const char* hold = getenv("NEXTUPD_TEST_HOLD_LOCK_MS");

        if (marker != NULL && marker[0] != '\0')
        {
            int marker_fd = open(marker, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
            if (marker_fd >= 0) close(marker_fd);
        }
        if (hold != NULL && hold[0] != '\0')
        {
            long milliseconds = strtol(hold, NULL, 10);
            if (milliseconds > 0) usleep((useconds_t)milliseconds * 1000);
        }
    }
#endif
    lock->descriptor = descriptor;
    return 1;
}

static void target_lock_release(TargetLock* lock)
{
    if (lock->descriptor >= 0)
    {
        close(lock->descriptor);
        lock->descriptor = -1;
    }
}

static int run_install(const Options* options)
{
    UpdStaged staged;
    VerifiedPackage verified;
    UpdInstallRequest request;
    UpdTransactionResult result;
    Options check_options;
    UpdError error;
    TargetLock target_lock;
    char archive[PATH_MAX];
    char manifest[PATH_MAX];
    char signature[PATH_MAX];
    int code;

    upd_error_clear(&error);
    if (!upd_staged_load(options->staged_dir, &staged, &error))
        return report_error(&error, options->json);
    if (snprintf(archive, sizeof archive, "%s/%s", options->staged_dir, staged.archive)
            >= (int)sizeof archive
        || snprintf(manifest, sizeof manifest, "%s/%s", options->staged_dir,
                    "manifest.json")
               >= (int)sizeof manifest
        || snprintf(signature, sizeof signature, "%s/%s", options->staged_dir,
                    "manifest.sig")
               >= (int)sizeof signature)
    {
        upd_error_set(&error, UPD_E_USAGE, "path_too_long",
                      "staged package path is too long");
        return report_error(&error, options->json);
    }

    check_options = *options;
    check_options.package = archive;
    check_options.manifest = manifest;
    check_options.signature = signature;
    if (!verify_package(&check_options, 1, &verified, &error))
    {
        code = report_error(&error, options->json);
        upd_manifest_free(&verified.manifest);
        return code;
    }
    if (!upd_staged_verify(options->staged_dir, &staged, &verified.manifest,
                           verified.key_id, &error))
    {
        code = report_error(&error, options->json);
        upd_manifest_free(&verified.manifest);
        return code;
    }
    memset(&request, 0, sizeof request);
    request.target_root = options->target_root;
    request.staged_dir = options->staged_dir;
    request.platform = options->platform;
    request.system_version = options->system_version;
    request.manifest = &verified.manifest;
    if (!target_lock_acquire(options->target_root, &target_lock, &error))
    {
        code = report_error(&error, options->json);
        upd_manifest_free(&verified.manifest);
        return code;
    }
    if (!upd_install_execute(&request, &result, &error))
    {
        target_lock_release(&target_lock);
        code = report_error(&error, options->json);
        upd_manifest_free(&verified.manifest);
        return code;
    }
    target_lock_release(&target_lock);
    print_transaction_result(options, &result, verified.manifest.id, options->target_root,
                             options->staged_dir);
    upd_manifest_free(&verified.manifest);
    return 0;
}

static int run_recover(const Options* options)
{
    UpdTransactionResult result;
    UpdError error;
    TargetLock target_lock;

    upd_error_clear(&error);
    if (!target_lock_acquire(options->target_root, &target_lock, &error))
        return report_error(&error, options->json);
    if (!upd_install_recover(options->target_root, options->rollback, &result, &error))
    {
        target_lock_release(&target_lock);
        return report_error(&error, options->json);
    }
    target_lock_release(&target_lock);
    print_transaction_result(options, &result, NULL, options->target_root, NULL);
    return 0;
}

static int parse_options(int argc, char** argv, const char* command, Options* options)
{
    int i;

    memset(options, 0, sizeof *options);
    for (i = 0; i < argc; i++)
    {
        const char* argument = argv[i];
        const char** target = NULL;

        if (strcmp(argument, "-h") == 0 || strcmp(argument, "--help") == 0)
        {
            usage(stdout);
            exit(0);
        }
        if (strcmp(argument, "--package") == 0) target = &options->package;
        else if (strcmp(argument, "--manifest") == 0) target = &options->manifest;
        else if (strcmp(argument, "--signature") == 0) target = &options->signature;
        else if (strcmp(argument, "--trusted-key") == 0) target = &options->trusted_key;
        else if (strcmp(argument, "--platform") == 0) target = &options->platform;
        else if (strcmp(argument, "--system-version") == 0) target = &options->system_version;
        else if (strcmp(argument, "--staging-root") == 0) target = &options->staging_root;
        else if (strcmp(argument, "--id") == 0) target = &options->id;
        else if (strcmp(argument, "--kind") == 0) target = &options->kind;
        else if (strcmp(argument, "--version") == 0) target = &options->version;
        else if (strcmp(argument, "--min-system") == 0) target = &options->min_system;
        else if (strcmp(argument, "--out") == 0) target = &options->out;
        else if (strcmp(argument, "--staged-dir") == 0) target = &options->staged_dir;
        else if (strcmp(argument, "--target-root") == 0) target = &options->target_root;
        else if (strcmp(argument, "--integrity-only") == 0)
        {
            options->integrity_only = 1;
            continue;
        }
        else if (strcmp(argument, "--replace") == 0)
        {
            options->replace = 1;
            continue;
        }
        else if (strcmp(argument, "--rollback") == 0)
        {
            options->rollback = 1;
            continue;
        }
        else if (strcmp(argument, "--json") == 0)
        {
            options->json = 1;
            continue;
        }
        else
        {
            fprintf(stderr, "nextupd: unknown argument '%s'\n", argument);
            usage(stderr);
            return 0;
        }
        if (i + 1 >= argc || argv[i + 1][0] == '\0')
        {
            fprintf(stderr, "nextupd: %s requires a value\n", argument);
            return 0;
        }
        *target = argv[++i];
    }
    if (strcmp(command, "manifest") == 0
        && (options->id == NULL || options->kind == NULL || options->version == NULL
            || options->platform == NULL || options->package == NULL))
    {
        fprintf(stderr,
                "nextupd: manifest needs --package, --id, --kind, --version and --platform\n");
        return 0;
    }
    if ((strcmp(command, "check") == 0 || strcmp(command, "stage") == 0)
        && (options->package == NULL || options->manifest == NULL))
    {
        fprintf(stderr, "nextupd: %s needs --package and --manifest\n", command);
        return 0;
    }
    if (strcmp(command, "install") == 0 && options->target_root == NULL)
    {
        fprintf(stderr,
                "nextupd: target_root_missing: --target-root is required; there is no "
                "default\n");
        return 0;
    }
    if (strcmp(command, "install") == 0
        && (options->staged_dir == NULL || options->trusted_key == NULL
            || options->platform == NULL || options->system_version == NULL))
    {
        fprintf(stderr,
                "nextupd: install needs --staged-dir, --target-root, --trusted-key, "
                "--platform and --system-version\n");
        return 0;
    }
    if (strcmp(command, "recover") == 0 && options->target_root == NULL)
    {
        fprintf(stderr, "nextupd: recover needs --target-root\n");
        return 0;
    }
    if (strcmp(command, "manifest") == 0
        && (options->signature != NULL || options->trusted_key != NULL
            || options->system_version != NULL || options->staging_root != NULL
            || options->staged_dir != NULL || options->target_root != NULL
            || options->integrity_only || options->replace || options->rollback
            || options->json))
    {
        fprintf(stderr, "nextupd: manifest does not accept check/stage options\n");
        return 0;
    }
    if (strcmp(command, "check") == 0
        && (options->id != NULL || options->kind != NULL || options->version != NULL
            || options->min_system != NULL || options->out != NULL
            || options->staging_root != NULL || options->staged_dir != NULL
            || options->target_root != NULL || options->replace || options->rollback))
    {
        fprintf(stderr, "nextupd: check does not accept manifest/stage/install options\n");
        return 0;
    }
    if (strcmp(command, "stage") == 0
        && (options->id != NULL || options->kind != NULL || options->version != NULL
            || options->min_system != NULL || options->out != NULL
            || options->staged_dir != NULL || options->target_root != NULL
            || options->integrity_only || options->rollback))
    {
        fprintf(stderr, "nextupd: stage does not accept manifest/check/install options\n");
        return 0;
    }
    if (strcmp(command, "install") == 0
        && (options->package != NULL || options->manifest != NULL
            || options->signature != NULL || options->id != NULL
            || options->kind != NULL || options->version != NULL
            || options->min_system != NULL || options->out != NULL
            || options->staging_root != NULL || options->integrity_only
            || options->replace || options->rollback))
    {
        fprintf(stderr, "nextupd: install does not accept package/manifest options\n");
        return 0;
    }
    if (strcmp(command, "recover") == 0
        && (options->package != NULL || options->manifest != NULL
            || options->signature != NULL || options->trusted_key != NULL
            || options->platform != NULL || options->system_version != NULL
            || options->staging_root != NULL || options->staged_dir != NULL
            || options->id != NULL || options->kind != NULL || options->version != NULL
            || options->min_system != NULL || options->out != NULL
            || options->integrity_only || options->replace))
    {
        fprintf(stderr, "nextupd: recover accepts only --target-root, --rollback and --json\n");
        return 0;
    }
    return 1;
}

int main(int argc, char** argv)
{
    const char* command;
    Options options;

    if (argc < 2)
    {
        usage(stderr);
        return UPD_E_USAGE;
    }
    command = argv[1];
    if (strcmp(command, "-h") == 0 || strcmp(command, "--help") == 0)
    {
        usage(stdout);
        return 0;
    }
    if (strcmp(command, "check") != 0 && strcmp(command, "stage") != 0
        && strcmp(command, "install") != 0 && strcmp(command, "recover") != 0
        && strcmp(command, "manifest") != 0)
    {
        fprintf(stderr, "nextupd: unknown command '%s'\n", command);
        usage(stderr);
        return UPD_E_USAGE;
    }
    if (!parse_options(argc - 2, argv + 2, command, &options)) return UPD_E_USAGE;
    if (strcmp(command, "check") == 0) return run_check(&options);
    if (strcmp(command, "stage") == 0) return run_stage(&options);
    if (strcmp(command, "install") == 0) return run_install(&options);
    if (strcmp(command, "recover") == 0) return run_recover(&options);
    return run_manifest(&options);
}
