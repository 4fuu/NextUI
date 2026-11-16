// Host-side regression tests for the About page version text.
//
// Missing or empty version data used to reach the renderer verbatim: an empty
// string makes TTF_RenderUTF8_Blended return NULL (SIGSEGV at the value blit),
// and a trailing newline renders as a missing-glyph box. These tests pin the
// readVersionText() contract the About row now relies on.
//
// Build and run from the repository root:
//   workspace/all/settings/tests/run_settings_version_test.sh

#include "version.hpp"

#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

static int failures = 0;

#define CHECK(condition, ...)                         \
    do                                                \
    {                                                 \
        if (!(condition))                             \
        {                                             \
            printf("FAIL: ");                         \
            printf(__VA_ARGS__);                      \
            printf("\n");                             \
            failures += 1;                            \
        }                                             \
    } while (0)

static const char *DIR = nullptr;

static std::string fixturePath(const char *name)
{
    return std::string(DIR) + "/" + name;
}

static void writeFile(const char *name, const std::string &content)
{
    std::ofstream out(fixturePath(name), std::ios::binary);
    out << content;
}

static void expectText(const char *name, const char *content, const char *expected)
{
    writeFile(name, content);
    std::string actual = readVersionText(fixturePath(name));
    CHECK(actual == expected, "%s: read '%s' as '%s', expected '%s'",
          name, content, actual.c_str(), expected);
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        printf("usage: %s <fixture-dir>\n", argv[0]);
        return 2;
    }
    DIR = argv[1];

    // A missing file must fall back instead of rendering an empty value.
    CHECK(readVersionText(fixturePath("does-not-exist.txt")) == "Unknown",
          "missing file must fall back to Unknown, got '%s'",
          readVersionText(fixturePath("does-not-exist.txt")).c_str());

    // Empty and whitespace-only files carry no version text.
    expectText("empty.txt", "", "Unknown");
    expectText("newline-only.txt", "\n", "Unknown");
    expectText("spaces-only.txt", "  \t\n", "Unknown");

    // The file is written with a trailing newline; it must not reach the
    // renderer (the newline glyph is a missing-glyph box in the UI fonts).
    expectText("trailing-newline.txt", "1.2.3\n", "1.2.3");
    expectText("trailing-crlf.txt", "1.2.3\r\n", "1.2.3");
    expectText("trailing-blank-line.txt", "1.2.3\n\n", "1.2.3");
    expectText("trailing-spaces.txt", "1.2.3  ", "1.2.3");

    // A well-formed file is returned unchanged, and a directory (an unreadable
    // path) falls back like a missing file does.
    expectText("valid.txt", "1.2.3", "1.2.3");
    CHECK(readVersionText(DIR) == "Unknown",
          "unreadable path must fall back to Unknown");

    if (failures == 0)
        printf("settings version tests passed\n");
    return failures == 0 ? 0 : 1;
}
