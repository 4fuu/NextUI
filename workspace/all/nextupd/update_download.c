#define _GNU_SOURCE

#include "update_download.h"
#include "update_feed.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define HTTP_HEADER_LIMIT 64
#define HTTP_LINE_LIMIT 4096
#define HTTP_READ_BUFFER 8192
#define HTTP_REDIRECT_LIMIT 5

typedef struct
{
    int tls;
    char host[256];
    char port[8];
    char host_header[300];
    char path[UPD_MAX_URL + 1];
} ParsedUrl;

typedef struct
{
    int fd;
    int timeout_ms;
    unsigned char buffer[HTTP_READ_BUFFER];
    size_t position;
    size_t length;
    int eof;
} HttpReader;

typedef struct
{
    long long content_length;
    int chunked;
    int status;
    char location[UPD_MAX_URL + 1];
} HttpHead;

static int wait_fd(int fd, short events, int timeout_ms)
{
    struct pollfd descriptor;
    int result;

    descriptor.fd = fd;
    descriptor.events = events;
    descriptor.revents = 0;
    do
    {
        result = poll(&descriptor, 1, timeout_ms);
    } while (result < 0 && errno == EINTR);

    if (result < 0) return -1;
    if (result == 0) return 0;
    /* Let the caller observe a half-closed peer as EOF or a write error
     * instead of reporting a generic poll failure. */
    if ((descriptor.revents & (events | POLLHUP | POLLERR | POLLNVAL)) != 0) return 1;
    return -1;
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0) return 0;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static int map_write_error(UpdError* error, const char* destination)
{
    if (errno == ENOSPC)
        upd_error_set(error, UPD_E_SPACE, "insufficient_space",
                      "storage full while writing %s", destination);
    else
        upd_error_set(error, UPD_E_IO, "io_error", "write %s: %s", destination,
                      strerror(errno));
    return 0;
}

static int write_all(int fd, const unsigned char* data, size_t length, int timeout_ms,
                     const char* destination, UpdError* error)
{
    size_t written = 0;

    while (written < length)
    {
        ssize_t count;

        if (wait_fd(fd, POLLOUT, timeout_ms) <= 0)
        {
            upd_error_set(error, UPD_E_IO, "download_timeout",
                          "timed out writing %s", destination);
            return 0;
        }
        count = send(fd, data + written, length - written, MSG_NOSIGNAL);
        if (count < 0)
        {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return map_write_error(error, destination);
        }
        if (count == 0)
        {
            upd_error_set(error, UPD_E_IO, "download_failed",
                          "connection closed while writing %s", destination);
            return 0;
        }
        written += (size_t)count;
    }
    return 1;
}

static int write_file_bytes(int fd, const unsigned char* data, size_t length, int timeout_ms,
                            const char* destination, UpdError* error)
{
    size_t written = 0;

    (void)timeout_ms;
    while (written < length)
    {
        ssize_t count = write(fd, data + written, length - written);
        if (count < 0)
        {
            if (errno == EINTR) continue;
            return map_write_error(error, destination);
        }
        if (count == 0)
        {
            errno = EIO;
            return map_write_error(error, destination);
        }
        written += (size_t)count;
    }
    return 1;
}

static int reader_fill(HttpReader* reader, UpdError* error)
{
    ssize_t count;

    if (reader->position < reader->length) return 1;
    if (reader->eof) return 0;
    for (;;)
    {
        int ready = wait_fd(reader->fd, POLLIN, reader->timeout_ms);

        if (ready == 0)
        {
            upd_error_set(error, UPD_E_IO, "download_timeout",
                          "timed out waiting for response data");
            return -1;
        }
        if (ready < 0)
        {
            upd_error_set(error, UPD_E_IO, "download_failed",
                          "cannot read response: %s", strerror(errno));
            return -1;
        }
        count = recv(reader->fd, reader->buffer, sizeof reader->buffer, 0);
        if (count < 0)
        {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            upd_error_set(error, UPD_E_IO, "download_failed",
                          "read response: %s", strerror(errno));
            return -1;
        }
        reader->position = 0;
        reader->length = (size_t)count;
        if (count == 0)
        {
            reader->eof = 1;
            return 0;
        }
        return 1;
    }
}

static int reader_getline(HttpReader* reader, char* line, size_t line_size, UpdError* error)
{
    size_t used = 0;

    for (;;)
    {
        int result = reader_fill(reader, error);
        unsigned char value;

        if (result < 0) return -1;
        if (result == 0)
        {
            if (used == 0) return 0;
            upd_error_set(error, UPD_E_IO, "download_truncated",
                          "response ended inside a header line");
            return -1;
        }
        value = reader->buffer[reader->position++];
        if (value == '\n')
        {
            if (used > 0 && line[used - 1] == '\r') used--;
            line[used] = '\0';
            return 1;
        }
        if (used + 1 >= line_size)
        {
            upd_error_set(error, UPD_E_IO, "http_invalid", "response line is too long");
            return -1;
        }
        line[used++] = (char)value;
    }
}

static int reader_read(HttpReader* reader, unsigned char* out, size_t wanted, size_t* got,
                       UpdError* error)
{
    size_t copied = 0;

    while (copied < wanted)
    {
        size_t available;
        int result = reader_fill(reader, error);

        if (result < 0) return 0;
        if (result == 0)
        {
            *got = copied;
            return 1;
        }
        available = reader->length - reader->position;
        if (available > wanted - copied) available = wanted - copied;
        memcpy(out + copied, reader->buffer + reader->position, available);
        reader->position += available;
        copied += available;
    }
    *got = copied;
    return 1;
}

static int parse_positive_long_long(const char* text, long long* value)
{
    long long parsed = 0;
    const unsigned char* cursor;

    if (text == NULL || text[0] == '\0') return 0;
    for (cursor = (const unsigned char*)text; *cursor != '\0'; cursor++)
    {
        unsigned digit;
        if (*cursor < '0' || *cursor > '9') return 0;
        digit = (unsigned)(*cursor - '0');
        if (parsed > (LLONG_MAX - (long long)digit) / 10) return 0;
        parsed = parsed * 10 + (long long)digit;
    }
    *value = parsed;
    return 1;
}

static int parse_url(const char* url, ParsedUrl* parsed, UpdError* error)
{
    const char* cursor;
    const char* authority;
    const char* end;
    const char* host;
    const char* host_end;
    const char* port = NULL;
    size_t length;

    memset(parsed, 0, sizeof *parsed);
    if (strchr(url, ' ') != NULL || strchr(url, '\t') != NULL
        || strchr(url, '\r') != NULL || strchr(url, '\n') != NULL)
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_url", "URL contains whitespace");
        return 0;
    }
    if (strncmp(url, "http://", 7) == 0)
    {
        parsed->tls = 0;
        cursor = url + 7;
    }
    else if (strncmp(url, "https://", 8) == 0)
    {
        parsed->tls = 1;
        cursor = url + 8;
    }
    else
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_url", "unsupported URL scheme");
        return 0;
    }
    authority = cursor;
    while (*cursor != '\0' && *cursor != '/' && *cursor != '?' && *cursor != '#') cursor++;
    end = cursor;
    if (authority == end)
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_url", "URL has no host");
        return 0;
    }
    if (memchr(authority, '@', (size_t)(end - authority)) != NULL)
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_url", "URL userinfo is not accepted");
        return 0;
    }
    if (*authority == '[')
    {
        const char* closing = memchr(authority, ']', (size_t)(end - authority));
        if (closing == NULL)
        {
            upd_error_set(error, UPD_E_INVALID, "invalid_url", "invalid IPv6 host");
            return 0;
        }
        host = authority + 1;
        host_end = closing;
        if (closing + 1 < end)
        {
            if (closing[1] != ':')
            {
                upd_error_set(error, UPD_E_INVALID, "invalid_url",
                              "unexpected data after IPv6 host");
                return 0;
            }
            port = closing + 2;
        }
    }
    else
    {
        const char* colon = memchr(authority, ':', (size_t)(end - authority));
        host = authority;
        host_end = colon == NULL ? end : colon;
        if (colon != NULL) port = colon + 1;
    }
    length = (size_t)(host_end - host);
    if (length == 0 || length >= sizeof parsed->host)
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_url", "host is empty or too long");
        return 0;
    }
    memcpy(parsed->host, host, length);
    parsed->host[length] = '\0';
    if (port != NULL)
    {
        length = (size_t)(end - port);
        if (length == 0 || length >= sizeof parsed->port)
        {
            upd_error_set(error, UPD_E_INVALID, "invalid_url", "port is empty or too long");
            return 0;
        }
        memcpy(parsed->port, port, length);
        parsed->port[length] = '\0';
    }
    else
    {
        snprintf(parsed->port, sizeof parsed->port, "%d", parsed->tls ? 443 : 80);
    }
    {
        long long port_number;
        if (!parse_positive_long_long(parsed->port, &port_number) || port_number <= 0
            || port_number > 65535)
        {
            upd_error_set(error, UPD_E_INVALID, "invalid_url", "invalid port");
            return 0;
        }
    }
    if (strchr(parsed->host, ':') != NULL)
        snprintf(parsed->host_header, sizeof parsed->host_header, "[%s]", parsed->host);
    else
        snprintf(parsed->host_header, sizeof parsed->host_header, "%s", parsed->host);
    if ((parsed->tls && strcmp(parsed->port, "443") != 0)
        || (!parsed->tls && strcmp(parsed->port, "80") != 0))
    {
        size_t used = strlen(parsed->host_header);
        snprintf(parsed->host_header + used, sizeof parsed->host_header - used, ":%s",
                 parsed->port);
    }
    if (*cursor == '\0')
        snprintf(parsed->path, sizeof parsed->path, "/");
    else if (*cursor == '?')
        snprintf(parsed->path, sizeof parsed->path, "/%s", cursor);
    else
    {
        if (strlen(cursor) >= sizeof parsed->path)
        {
            upd_error_set(error, UPD_E_INVALID, "invalid_url", "URL path is too long");
            return 0;
        }
        snprintf(parsed->path, sizeof parsed->path, "%s", cursor);
    }
    return 1;
}

static int connect_url(const ParsedUrl* parsed, int timeout_ms, UpdError* error)
{
    struct addrinfo hints;
    struct addrinfo* results = NULL;
    struct addrinfo* candidate;
    int fd = -1;
    int last_error = 0;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(parsed->host, parsed->port, &hints, &results) != 0)
    {
        upd_error_set(error, UPD_E_IO, "download_failed", "cannot resolve %s", parsed->host);
        return -1;
    }
    for (candidate = results; candidate != NULL; candidate = candidate->ai_next)
    {
        int so_error = 0;
        socklen_t so_length = sizeof so_error;

        fd = socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (fd < 0)
        {
            last_error = errno;
            continue;
        }
        (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
        {
            int enabled = 1;
            (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled);
        }
#endif
        if (!set_nonblocking(fd))
        {
            last_error = errno;
            close(fd);
            fd = -1;
            continue;
        }
        if (connect(fd, candidate->ai_addr, candidate->ai_addrlen) != 0)
        {
            if (errno != EINPROGRESS)
            {
                last_error = errno;
                close(fd);
                fd = -1;
                continue;
            }
            if (wait_fd(fd, POLLOUT, timeout_ms) <= 0)
            {
                last_error = ETIMEDOUT;
                close(fd);
                fd = -1;
                continue;
            }
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_length) != 0
                || so_error != 0)
            {
                last_error = so_error != 0 ? so_error : errno;
                close(fd);
                fd = -1;
                continue;
            }
        }
        break;
    }
    freeaddrinfo(results);
    if (fd < 0)
    {
        if (last_error == ETIMEDOUT)
            upd_error_set(error, UPD_E_IO, "download_timeout", "connection to %s timed out",
                          parsed->host);
        else
            upd_error_set(error, UPD_E_IO, "download_failed", "cannot connect to %s: %s",
                          parsed->host, strerror(last_error == 0 ? ECONNREFUSED : last_error));
    }
    return fd;
}

static int ascii_equal(const char* left, size_t left_length, const char* right)
{
    size_t i;

    if (strlen(right) != left_length) return 0;
    for (i = 0; i < left_length; i++)
    {
        if (tolower((unsigned char)left[i]) != tolower((unsigned char)right[i])) return 0;
    }
    return 1;
}

static const char* trim_http_value(const char* value, size_t* length)
{
    size_t removed = 0;

    while (*value == ' ' || *value == '\t')
    {
        value++;
        removed++;
    }
    if (removed > *length) *length = 0;
    else *length -= removed;
    while (*length > 0 && (value[*length - 1] == ' ' || value[*length - 1] == '\t'))
        (*length)--;
    return value;
}

static int parse_http_head(HttpReader* reader, HttpHead* head, UpdError* error)
{
    char line[HTTP_LINE_LIMIT];
    int header_count = 0;
    int result;

    memset(head, 0, sizeof *head);
    head->content_length = -1;
    result = reader_getline(reader, line, sizeof line, error);
    if (result <= 0)
    {
        if (result == 0)
            upd_error_set(error, UPD_E_IO, "download_truncated",
                          "server closed before a response");
        return 0;
    }
    if (strlen(line) < 12 || strncmp(line, "HTTP/1.", 7) != 0 || line[7] < '0'
        || line[7] > '9' || line[8] != ' ' || !isdigit((unsigned char)line[9])
        || !isdigit((unsigned char)line[10]) || !isdigit((unsigned char)line[11])
        || line[12] != ' ')
    {
        upd_error_set(error, UPD_E_IO, "http_invalid", "invalid HTTP status line");
        return 0;
    }
    head->status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');
    if (head->status < 100 || head->status > 999)
    {
        upd_error_set(error, UPD_E_IO, "http_invalid", "invalid HTTP status code");
        return 0;
    }
    for (;;)
    {
        const char* value;
        size_t value_length;
        char name[32];
        size_t name_length;

        result = reader_getline(reader, line, sizeof line, error);
        if (result <= 0)
        {
            if (result == 0)
                upd_error_set(error, UPD_E_IO, "download_truncated",
                              "server closed inside response headers");
            return 0;
        }
        if (line[0] == '\0') break;
        if (++header_count > HTTP_HEADER_LIMIT)
        {
            upd_error_set(error, UPD_E_IO, "http_invalid", "too many response headers");
            return 0;
        }
        value = strchr(line, ':');
        if (value == NULL)
        {
            upd_error_set(error, UPD_E_IO, "http_invalid", "invalid response header");
            return 0;
        }
        name_length = (size_t)(value - line);
        if (name_length == 0 || name_length >= sizeof name)
        {
            upd_error_set(error, UPD_E_IO, "http_invalid", "invalid response header name");
            return 0;
        }
        memcpy(name, line, name_length);
        name[name_length] = '\0';
        value_length = strlen(value + 1);
        value = trim_http_value(value + 1, &value_length);

        if (ascii_equal(name, name_length, "content-length"))
        {
            long long parsed;
            if (!parse_positive_long_long(value, &parsed))
            {
                upd_error_set(error, UPD_E_IO, "http_invalid", "invalid Content-Length");
                return 0;
            }
            head->content_length = parsed;
        }
        else if (ascii_equal(name, name_length, "transfer-encoding"))
        {
            if (ascii_equal(value, value_length, "chunked"))
                head->chunked = 1;
            else
            {
                upd_error_set(error, UPD_E_IO, "http_invalid",
                              "unsupported Transfer-Encoding");
                return 0;
            }
        }
        else if (ascii_equal(name, name_length, "content-encoding"))
        {
            if (!ascii_equal(value, value_length, "identity"))
            {
                upd_error_set(error, UPD_E_IO, "http_invalid",
                              "compressed response bodies are not supported");
                return 0;
            }
        }
        else if (ascii_equal(name, name_length, "location"))
        {
            if (value_length >= sizeof head->location)
            {
                upd_error_set(error, UPD_E_IO, "http_invalid", "redirect location is too long");
                return 0;
            }
            memcpy(head->location, value, value_length);
            head->location[value_length] = '\0';
        }
    }
    return 1;
}

static int read_chunk_line(HttpReader* reader, char* line, size_t line_size, UpdError* error)
{
    return reader_getline(reader, line, line_size, error);
}

static int write_response_body(HttpReader* reader, const HttpHead* head, int out_fd,
                               uint64_t max_bytes, int timeout_ms, const char* destination,
                               UpdError* error)
{
    unsigned char buffer[64 * 1024];
    uint64_t total = 0;

    if (head->chunked)
    {
        for (;;)
        {
            char line[128];
            char* end = NULL;
            unsigned long long chunk;
            int result = read_chunk_line(reader, line, sizeof line, error);
            size_t remaining;
            size_t got;

            if (result <= 0)
            {
                if (result == 0)
                    upd_error_set(error, UPD_E_IO, "download_truncated",
                                  "chunked response ended before the final chunk");
                return 0;
            }
            errno = 0;
            chunk = strtoull(line, &end, 16);
            if (errno != 0 || end == line)
            {
                upd_error_set(error, UPD_E_IO, "http_invalid", "invalid chunk size");
                return 0;
            }
            if (chunk == 0)
            {
                for (;;)
                {
                    result = reader_getline(reader, line, sizeof line, error);
                    if (result <= 0)
                    {
                        if (result == 0)
                            upd_error_set(error, UPD_E_IO, "download_truncated",
                                          "chunked response ended inside trailers");
                        return 0;
                    }
                    if (line[0] == '\0') break;
                }
                return 1;
            }
            if (total > max_bytes || chunk > max_bytes - total)
            {
                upd_error_set(error, UPD_E_INVALID, "download_too_large",
                              "%s exceeds %llu bytes", destination,
                              (unsigned long long)max_bytes);
                return 0;
            }
            remaining = (size_t)chunk;
            while (remaining > 0)
            {
                size_t want = remaining < sizeof buffer ? remaining : sizeof buffer;
                if (!reader_read(reader, buffer, want, &got, error)) return 0;
                if (got != want)
                {
                    upd_error_set(error, UPD_E_IO, "download_truncated",
                                  "response ended inside a chunk");
                    return 0;
                }
                if (!write_file_bytes(out_fd, buffer, got, timeout_ms, destination, error))
                    return 0;
                total += (uint64_t)got;
                remaining -= got;
            }
            result = read_chunk_line(reader, line, sizeof line, error);
            if (result <= 0 || line[0] != '\0')
            {
                upd_error_set(error, UPD_E_IO, "http_invalid", "missing chunk terminator");
                return 0;
            }
        }
    }

    if (head->content_length >= 0)
    {
        uint64_t expected = (uint64_t)head->content_length;
        if (expected > max_bytes)
        {
            upd_error_set(error, UPD_E_INVALID, "download_too_large",
                          "%s exceeds %llu bytes", destination,
                          (unsigned long long)max_bytes);
            return 0;
        }
        while (total < expected)
        {
            size_t want = (size_t)(expected - total);
            size_t got;
            if (want > sizeof buffer) want = sizeof buffer;
            if (!reader_read(reader, buffer, want, &got, error)) return 0;
            if (got != want)
            {
                upd_error_set(error, UPD_E_IO, "download_truncated",
                              "%s is shorter than Content-Length", destination);
                return 0;
            }
            if (!write_file_bytes(out_fd, buffer, got, timeout_ms, destination, error))
                return 0;
            total += (uint64_t)got;
        }
        return 1;
    }

    for (;;)
    {
        size_t got;
        int result = reader_read(reader, buffer, sizeof buffer, &got, error);
        if (result == 0) return 0;
        if (got == 0) break;
        if (total > max_bytes || got > max_bytes - total)
        {
            upd_error_set(error, UPD_E_INVALID, "download_too_large",
                          "%s exceeds %llu bytes", destination,
                          (unsigned long long)max_bytes);
            return 0;
        }
        if (!write_file_bytes(out_fd, buffer, got, timeout_ms, destination, error)) return 0;
        total += (uint64_t)got;
    }
    return 1;
}

static int http_request_once(const char* url, int out_fd, uint64_t max_bytes, int timeout_ms,
                             const char* destination, HttpHead* head, UpdError* error)
{
    ParsedUrl parsed;
    HttpReader reader;
    char request[UPD_MAX_URL + 512];
    int request_length;
    int fd;

    if (!parse_url(url, &parsed, error)) return 0;
    if (parsed.tls)
    {
        upd_error_set(error, UPD_E_IO, "transport_unavailable",
                      "HTTPS is handled by the external curl transport");
        return 0;
    }
    fd = connect_url(&parsed, timeout_ms, error);
    if (fd < 0) return 0;
    request_length = snprintf(
        request, sizeof request,
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: nextupd/1\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n"
        "\r\n",
        parsed.path, parsed.host_header);
    if (request_length < 0 || request_length >= (int)sizeof request)
    {
        upd_error_set(error, UPD_E_USAGE, "url_too_long", "request is too long");
        close(fd);
        return 0;
    }
    if (!write_all(fd, (const unsigned char*)request, (size_t)request_length, timeout_ms,
                   destination, error))
    {
        close(fd);
        return 0;
    }
    memset(&reader, 0, sizeof reader);
    reader.fd = fd;
    reader.timeout_ms = timeout_ms;
    if (!parse_http_head(&reader, head, error))
    {
        close(fd);
        return 0;
    }
    if (head->status == 301 || head->status == 302 || head->status == 303
        || head->status == 307 || head->status == 308)
    {
        close(fd);
        return 2;
    }
    if (head->status < 200 || head->status > 299)
    {
        upd_error_set(error, UPD_E_IO, "http_status", "server returned HTTP %d", head->status);
        close(fd);
        return 0;
    }
    if (!write_response_body(&reader, head, out_fd, max_bytes, timeout_ms, destination, error))
    {
        close(fd);
        return 0;
    }
    close(fd);
    return 1;
}

static int open_destination(const char* destination, UpdError* error)
{
    int fd = open(destination, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);

    if (fd < 0)
    {
        if (errno == ENOSPC)
            upd_error_set(error, UPD_E_SPACE, "insufficient_space",
                          "cannot create %s: no space left", destination);
        else
            upd_error_set(error, UPD_E_IO, "io_error", "create %s: %s", destination,
                          strerror(errno));
    }
    return fd;
}

static int copy_local(const char* source, const char* destination, uint64_t max_bytes,
                      UpdError* error)
{
    unsigned char buffer[64 * 1024];
    struct stat status;
    uint64_t total = 0;
    int input = -1;
    int output = -1;
    int result = 0;

    input = open(source, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (input < 0)
    {
        if (errno == ENOENT)
            upd_error_set(error, UPD_E_MISSING, "download_missing", "cannot open %s: %s",
                          source, strerror(errno));
        else
            upd_error_set(error, UPD_E_IO, "io_error", "cannot open %s: %s", source,
                          strerror(errno));
        return 0;
    }
    if (fstat(input, &status) != 0 || !S_ISREG(status.st_mode))
    {
        upd_error_set(error, UPD_E_UNSAFE, "unsafe_path", "%s is not a regular file", source);
        goto done;
    }
    if (status.st_size < 0 || (uint64_t)status.st_size > max_bytes)
    {
        upd_error_set(error, UPD_E_INVALID, "download_too_large", "%s exceeds %llu bytes",
                      source, (unsigned long long)max_bytes);
        goto done;
    }
    output = open_destination(destination, error);
    if (output < 0)
    {
        if (errno == ENOSPC)
            upd_error_set(error, UPD_E_SPACE, "insufficient_space",
                          "cannot create %s: no space left", destination);
        goto done;
    }
    for (;;)
    {
        ssize_t count = read(input, buffer, sizeof buffer);
        if (count < 0)
        {
            if (errno == EINTR) continue;
            upd_error_set(error, UPD_E_IO, "io_error", "read %s: %s", source,
                          strerror(errno));
            goto done;
        }
        if (count == 0) break;
        if (total > max_bytes || (uint64_t)count > max_bytes - total)
        {
            upd_error_set(error, UPD_E_INVALID, "download_too_large", "%s exceeds %llu bytes",
                          source, (unsigned long long)max_bytes);
            goto done;
        }
        if (!write_file_bytes(output, buffer, (size_t)count, 0, destination, error))
            goto done;
        total += (uint64_t)count;
    }
    result = 1;

done:
    if (input >= 0) close(input);
    if (output >= 0) close(output);
    if (!result) unlink(destination);
    return result;
}

static int download_https(const char* source, const char* destination, uint64_t max_bytes,
                          int timeout_seconds, UpdError* error)
{
    char timeout_text[16];
    char max_text[32];
    pid_t child;
    int status = 0;
    struct stat file_status;

    if (!upd_url_is_remote(source) || strncmp(source, "https://", 8) != 0)
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_url", "HTTPS transport needs an HTTPS URL");
        return 0;
    }
    snprintf(timeout_text, sizeof timeout_text, "%d", timeout_seconds);
    snprintf(max_text, sizeof max_text, "%llu", (unsigned long long)max_bytes);
    child = fork();
    if (child < 0)
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot start HTTPS transport: %s",
                      strerror(errno));
        return 0;
    }
    if (child == 0)
    {
        execlp("curl", "curl", "--fail", "--silent", "--show-error", "--location",
               "--proto", "=https", "--proto-redir", "=https", "--connect-timeout",
               timeout_text, "--speed-limit", "1024", "--speed-time", timeout_text,
               "--max-time", "3600", "--max-filesize", max_text, "--output", destination,
               "--url", source, (char*)NULL);
        _exit(127);
    }
    for (;;)
    {
        pid_t waited = waitpid(child, &status, 0);
        if (waited == child) break;
        if (waited < 0 && errno == EINTR) continue;
        upd_error_set(error, UPD_E_IO, "io_error", "cannot wait for HTTPS transport: %s",
                      strerror(errno));
        return 0;
    }
    if (!WIFEXITED(status))
    {
        upd_error_set(error, UPD_E_IO, "download_failed", "HTTPS transport was terminated");
        unlink(destination);
        return 0;
    }
    if (WEXITSTATUS(status) == 127)
    {
        upd_error_set(error, UPD_E_IO, "transport_unavailable",
                      "HTTPS download requires curl, but curl is not available");
        unlink(destination);
        return 0;
    }
    if (WEXITSTATUS(status) != 0)
    {
        int code = WEXITSTATUS(status);
        if (code == 28)
            upd_error_set(error, UPD_E_IO, "download_timeout", "HTTPS download timed out");
        else if (code == 22)
            upd_error_set(error, UPD_E_IO, "http_status", "HTTPS server returned an error");
        else if (code == 63)
            upd_error_set(error, UPD_E_INVALID, "download_too_large",
                          "HTTPS download exceeds %llu bytes",
                          (unsigned long long)max_bytes);
        else
            upd_error_set(error, UPD_E_IO, "download_failed",
                          "HTTPS transport failed with exit code %d", code);
        unlink(destination);
        return 0;
    }
    if (stat(destination, &file_status) != 0 || !S_ISREG(file_status.st_mode))
    {
        upd_error_set(error, UPD_E_IO, "download_failed", "HTTPS transport wrote no file");
        unlink(destination);
        return 0;
    }
    if (file_status.st_size < 0 || (uint64_t)file_status.st_size > max_bytes)
    {
        upd_error_set(error, UPD_E_INVALID, "download_too_large",
                      "HTTPS download exceeds %llu bytes", (unsigned long long)max_bytes);
        unlink(destination);
        return 0;
    }
    return 1;
}

static int download_http(const char* source, const char* destination, uint64_t max_bytes,
                         int timeout_seconds, UpdError* error)
{
    char current[UPD_MAX_URL + 1];
    char next[UPD_MAX_URL + 1];
    int redirects = 0;
    int fd;

    snprintf(current, sizeof current, "%s", source);
    for (;;)
    {
        HttpHead head;
        int result;

        fd = open_destination(destination, error);
        if (fd < 0) return 0;
        result = http_request_once(current, fd, max_bytes, timeout_seconds * 1000, destination,
                                   &head, error);
        close(fd);
        if (result == 1) return 1;
        if (result == 0)
        {
            unlink(destination);
            return 0;
        }
        unlink(destination);
        if (++redirects > HTTP_REDIRECT_LIMIT)
        {
            upd_error_set(error, UPD_E_IO, "too_many_redirects",
                          "server redirected more than %d times", HTTP_REDIRECT_LIMIT);
            return 0;
        }
        if (head.location[0] == '\0')
        {
            upd_error_set(error, UPD_E_IO, "http_invalid", "redirect has no Location");
            return 0;
        }
        if (!upd_url_resolve(current, head.location, next, sizeof next, error)) return 0;
        snprintf(current, sizeof current, "%s", next);
        if (strncmp(current, "https://", 8) == 0)
            return download_https(current, destination, max_bytes, timeout_seconds, error);
        if (strncmp(current, "http://", 7) != 0)
        {
            upd_error_set(error, UPD_E_INVALID, "invalid_url", "unsupported redirect scheme");
            return 0;
        }
    }
}

int upd_download_to_file(const char* source, const char* destination, uint64_t max_bytes,
                         int timeout_seconds, UpdError* error)
{
    if (source == NULL || source[0] == '\0' || destination == NULL
        || destination[0] == '\0')
    {
        upd_error_set(error, UPD_E_USAGE, "usage", "download needs a source and destination");
        return 0;
    }
    if (timeout_seconds <= 0 || timeout_seconds > 3600)
    {
        upd_error_set(error, UPD_E_USAGE, "invalid_timeout",
                      "timeout must be between 1 and 3600 seconds");
        return 0;
    }
    if (strncmp(source, "https://", 8) == 0)
        return download_https(source, destination, max_bytes, timeout_seconds, error);
    if (strncmp(source, "http://", 7) == 0)
        return download_http(source, destination, max_bytes, timeout_seconds, error);
    if (upd_url_is_remote(source))
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_url", "unsupported URL scheme");
        return 0;
    }
    return copy_local(source, destination, max_bytes, error);
}

int upd_download_temporary(const char* source, uint64_t max_bytes, int timeout_seconds,
                           char* out_path, size_t out_size, UpdError* error)
{
    const char* directory = getenv("TMPDIR");
    char path[PATH_MAX];
    int fd;

    if (directory == NULL || directory[0] == '\0') directory = "/tmp";
    if (snprintf(path, sizeof path, "%s/nextupd-download-XXXXXX", directory)
        >= (int)sizeof path)
    {
        upd_error_set(error, UPD_E_USAGE, "path_too_long", "temporary directory is too long");
        return 0;
    }
    fd = mkstemp(path);
    if (fd < 0)
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot create temporary file: %s",
                      strerror(errno));
        return 0;
    }
    close(fd);
    unlink(path);
    if (!upd_download_to_file(source, path, max_bytes, timeout_seconds, error))
    {
        unlink(path);
        return 0;
    }
    if (strlen(path) >= out_size)
    {
        unlink(path);
        upd_error_set(error, UPD_E_USAGE, "path_too_long", "temporary path is too long");
        return 0;
    }
    snprintf(out_path, out_size, "%s", path);
    return 1;
}
