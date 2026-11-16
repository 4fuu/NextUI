#include "toolrunner.hpp"

// json_lite is a C scanner with no extern "C" guard in its header; this is its
// first C++ consumer, so wrap the include to keep the linkage C.
extern "C"
{
#include "json_lite.h"
}

#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

#include <fstream>
#include <sstream>
#include <cstring>
#include <cerrno>

extern char** environ;

int runToolProcess(const std::vector<std::string>& argv, std::string& stdout_text)
{
    stdout_text.clear();
    if (argv.empty())
        return -1;

    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const std::string& arg : argv)
        cargv.push_back(const_cast<char*>(arg.c_str()));
    cargv.push_back(nullptr);

    int pipefd[2];
    if (pipe(pipefd) != 0)
        return -1;
    // Close-on-exec on the read end so the child never inherits it; the write end
    // is dup2'd onto stdout and the original closed by the file actions below.
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    posix_spawn_file_actions_addclose(&actions, pipefd[1]);

    pid_t pid = -1;
    int spawn_rc = posix_spawnp(&pid, cargv[0], &actions, nullptr, cargv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);

    if (spawn_rc != 0)
    {
        close(pipefd[0]);
        return -1;
    }

    char buffer[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buffer, sizeof(buffer))) > 0)
        stdout_text.append(buffer, (size_t)n);
    close(pipefd[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    {
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}

bool parseToolResultJson(const std::string& text, std::map<std::string, std::string>& out)
{
    // Read one JSON string value into out: validate and measure it, then re-read
    // it into a right-sized buffer. Always advances the cursor past the closing
    // quote, including for the empty string "".
    auto readString = [](JsonLiteCursor* c, std::string& value) -> bool {
        const char* start = c->p;
        size_t len = 0;
        if (!json_lite_parse_string(c, nullptr, 0, &len))
            return false;
        value.assign(len, '\0');
        c->p = start;
        size_t got = 0;
        if (!json_lite_parse_string(c, len ? &value[0] : nullptr, len + 1, &got))
            return false;
        value.resize(got);
        return true;
    };

    JsonLiteCursor cursor;
    json_lite_init(&cursor, text.c_str(), text.size());
    json_lite_skip_ws(&cursor);
    if (cursor.p >= cursor.end || *cursor.p != '{')
        return false;
    cursor.p++;
    json_lite_skip_ws(&cursor);

    for (;;)
    {
        std::string key;
        if (!readString(&cursor, key))
            return false;

        json_lite_skip_ws(&cursor);
        if (cursor.p >= cursor.end || *cursor.p != ':')
            return false;
        cursor.p++;
        json_lite_skip_ws(&cursor);
        if (cursor.p >= cursor.end)
            return false;

        std::string value;
        char lead = *cursor.p;
        if (lead == '"')
        {
            if (!readString(&cursor, value))
                return false;
        }
        else if (lead == 't')
        {
            if (cursor.p + 4 <= cursor.end && strncmp(cursor.p, "true", 4) == 0)
            {
                cursor.p += 4;
                value = "true";
            }
            else
                return false;
        }
        else if (lead == 'f')
        {
            if (cursor.p + 5 <= cursor.end && strncmp(cursor.p, "false", 5) == 0)
            {
                cursor.p += 5;
                value = "false";
            }
            else
                return false;
        }
        else if (lead == '-' || (lead >= '0' && lead <= '9'))
        {
            long number = 0;
            if (!json_lite_parse_integer(&cursor, &number))
                return false;
            value = std::to_string(number);
        }
        else
        {
            // A nested object or array is skipped, not stored. Whitespace is
            // skipped first: json_lite_skip_value expects the caller to have
            // consumed it, and a pretty-printed document (a manifest sidecar)
            // has some after the colon.
            json_lite_skip_ws(&cursor);
            if (!json_lite_skip_value(&cursor, 2))
                return false;
        }

        out[key] = value;

        json_lite_skip_ws(&cursor);
        if (cursor.p >= cursor.end)
            return false;
        if (*cursor.p == '}')
        {
            cursor.p++;
            break;
        }
        if (*cursor.p != ',')
            return false;
        cursor.p++;
        json_lite_skip_ws(&cursor);
    }

    json_lite_skip_ws(&cursor);
    return cursor.p == cursor.end;
}

std::string readSystemVersion(const std::string& version_path)
{
    std::ifstream file(version_path.c_str());
    if (!file)
        return "";
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string text = buffer.str();
    // Trim trailing whitespace, matching trim_text() in update_transaction.c, so a
    // version.txt written with a trailing newline still yields a clean vX.Y.Z.
    while (!text.empty())
    {
        char c = text.back();
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t')
            text.pop_back();
        else
            break;
    }
    return text;
}
