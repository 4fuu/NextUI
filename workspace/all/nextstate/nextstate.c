#define _GNU_SOURCE

#include "app_state.h"
#include "pak_manifest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum
{
    CLI_OK = 0,
    CLI_ABSENT = 2,
    CLI_INVALID = 3,
    CLI_UNSUPPORTED = 4,
    CLI_UNSAFE = 5,
    CLI_TOO_LARGE = 6,
    CLI_IO = 7,
    CLI_USAGE = 64,
};

static void usage(FILE* out)
{
    fprintf(out,
            "usage: nextstate <commit|clear|check> [--pak <pak-dir>] [--root <state-root>]\n"
            "\n"
            "  commit   validate the payload and publish resume.meta atomically\n"
            "  clear    remove this application's state slot\n"
            "  check    exit 0 only when the app declares saves and the state validates\n"
            "\n"
            "--pak defaults to $NEXTUI_APP_PAK_DIR.\n"
            "--root defaults to $SDCARD_PATH/.userdata/shared/.minui/apps.\n");
}

static int default_root(char* out, size_t out_size)
{
    const char* sdcard = getenv("SDCARD_PATH");
    int written;

    if (sdcard == NULL || sdcard[0] == '\0') sdcard = "/mnt/SDCARD";
    written = snprintf(out, out_size, "%s/.userdata/shared/.minui/apps", sdcard);
    return written >= 0 && (size_t)written < out_size;
}

static int parse_args(int argc, char** argv, const char* command,
                      const char** pak, const char** root)
{
    int i;

    *pak = NULL;
    *root = NULL;
    for (i = 0; i < argc; i++)
    {
        if (strcmp(argv[i], "--pak") == 0)
        {
            if (i + 1 >= argc) return 0;
            *pak = argv[++i];
        }
        else if (strcmp(argv[i], "--root") == 0)
        {
            if (i + 1 >= argc) return 0;
            *root = argv[++i];
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            usage(stdout);
            exit(CLI_OK);
        }
        else
        {
            fprintf(stderr, "nextstate: unknown argument '%s'\n", argv[i]);
            usage(stderr);
            return 0;
        }
    }
    (void)command;
    if (*pak == NULL || (*pak)[0] == '\0') *pak = getenv("NEXTUI_APP_PAK_DIR");
    if (*pak == NULL || (*pak)[0] == '\0')
    {
        fprintf(stderr, "nextstate: --pak or NEXTUI_APP_PAK_DIR is required\n");
        return 0;
    }
    return 1;
}

int main(int argc, char** argv)
{
    const char* command;
    const char* pak;
    const char* root_arg;
    char root[APP_STATE_PATH_MAX];
    PakManifest manifest;
    PakManifestStatus manifest_status;

    if (argc < 2)
    {
        usage(stderr);
        return CLI_USAGE;
    }
    command = argv[1];
    if (strcmp(command, "-h") == 0 || strcmp(command, "--help") == 0)
    {
        usage(stdout);
        return CLI_OK;
    }
    if (strcmp(command, "commit") != 0 && strcmp(command, "clear") != 0
        && strcmp(command, "check") != 0)
    {
        fprintf(stderr, "nextstate: unknown command '%s'\n", command);
        usage(stderr);
        return CLI_USAGE;
    }
    if (!parse_args(argc - 2, argv + 2, command, &pak, &root_arg)) return CLI_USAGE;
    if (root_arg == NULL)
    {
        if (!default_root(root, sizeof root))
        {
            fprintf(stderr, "nextstate: state root path is too long\n");
            return CLI_IO;
        }
        root_arg = root;
    }

    manifest_status = pak_manifest_load(pak, &manifest);
    if (manifest_status != PAK_MANIFEST_OK)
    {
        printf("unsupported %s\n",
               manifest_status == PAK_MANIFEST_ABSENT ? "manifest_absent" : "manifest_invalid");
        return CLI_UNSUPPORTED;
    }

    if (strcmp(command, "check") == 0)
    {
        AppStateInfo info;
        AppStateStatus status = app_state_inspect(root_arg, pak, &manifest, &info);

        switch (status)
        {
        case APP_STATE_VALID:
            printf("valid %u %llu\n", info.file_count, info.total_bytes);
            return CLI_OK;
        case APP_STATE_ABSENT:
            printf("absent\n");
            return CLI_ABSENT;
        case APP_STATE_INVALID:
            printf("invalid %s\n", info.reason);
            return strncmp(info.reason, "unsafe_", 7) == 0 ? CLI_UNSAFE : CLI_INVALID;
        case APP_STATE_UNSUPPORTED:
            printf("unsupported %s\n", info.reason);
            return CLI_UNSUPPORTED;
        case APP_STATE_ERROR:
            printf("error %s\n", info.reason);
            return CLI_IO;
        }
    }
    else if (strcmp(command, "commit") == 0)
    {
        char error[APP_STATE_REASON_MAX];
        AppStateCommitResult result = app_state_commit(root_arg, pak, &manifest,
                                                       error, sizeof error);

        switch (result)
        {
        case APP_STATE_COMMIT_OK:
        {
            AppStateInfo info;
            if (app_state_inspect(root_arg, pak, &manifest, &info) == APP_STATE_VALID)
                printf("ok %u %llu\n", info.file_count, info.total_bytes);
            else
                printf("ok\n");
            return CLI_OK;
        }
        case APP_STATE_COMMIT_UNSUPPORTED:
            printf("unsupported %s\n", error);
            return CLI_UNSUPPORTED;
        case APP_STATE_COMMIT_NO_PAYLOAD:
            printf("absent %s\n", error);
            return CLI_ABSENT;
        case APP_STATE_COMMIT_UNSAFE_PATH:
            printf("invalid %s\n", error);
            return CLI_UNSAFE;
        case APP_STATE_COMMIT_TOO_LARGE:
            printf("invalid %s\n", error);
            return CLI_TOO_LARGE;
        case APP_STATE_COMMIT_IO:
            printf("error %s\n", error);
            return CLI_IO;
        }
    }
    else
    {
        char error[APP_STATE_REASON_MAX];
        if (!app_state_clear(root_arg, pak, &manifest, error, sizeof error))
        {
            printf("error %s\n", error);
            return strcmp(error, "invalid_manifest") == 0 ? CLI_INVALID : CLI_IO;
        }
        printf("cleared\n");
        return CLI_OK;
    }

    return CLI_IO;
}
