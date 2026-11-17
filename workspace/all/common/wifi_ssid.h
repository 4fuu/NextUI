#ifndef WIFI_SSID_H
#define WIFI_SSID_H

// Helpers for the text wpa_supplicant hands back for SSIDs.
//
// wpa_cli prints an SSID with printf_encode(): printable ASCII (0x20-0x7e)
// goes through unchanged and every other byte, which includes every byte of a
// UTF-8 name, becomes an escape sequence such as \xe4. The helpers here turn
// that text back into the raw bytes an SSID actually is, and turn those bytes
// into valid UTF-8 for drawing.
//
// Kept header-only so the platform wifi backend and the settings UI (and their
// host tests) share one implementation without extra build wiring.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C"
{
#endif
#include "api.h" // SSID_MAX, struct WIFI_network, WifiSecurityType
#ifdef __cplusplus
}
#endif

static inline int wifi_ssidHexDigit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// Decodes wpa_supplicant's printf-escaped text (\\, \", \xNN and the single
// character C escapes) into raw bytes. The result is always NUL-terminated and
// never longer than out_size - 1 bytes.
static inline size_t wifi_ssidDecode(const char *encoded, char *out, size_t out_size)
{
    if (!out || out_size == 0)
        return 0;

    size_t o = 0;
    const char *p = encoded ? encoded : "";
    while (*p && o + 1 < out_size)
    {
        unsigned char c = (unsigned char)*p;
        if (c != '\\')
        {
            if (c == 0)
                break;
            out[o++] = (char)c;
            p++;
            continue;
        }

        char esc = p[1];
        if (esc == '\0')
        {
            out[o++] = '\\';
            break;
        }

        char decoded = 0;
        size_t consumed = 2;
        switch (esc)
        {
        case '\\':
        case '"':
        case '\'':
            decoded = esc;
            break;
        case 'n':
            decoded = '\n';
            break;
        case 'r':
            decoded = '\r';
            break;
        case 't':
            decoded = '\t';
            break;
        case 'e':
            decoded = 0x1b;
            break;
        case 'x':
        {
            int hi = wifi_ssidHexDigit(p[2]);
            int lo = wifi_ssidHexDigit(p[3]);
            if (hi < 0 || lo < 0)
                break; // not an \xNN escape: keep the backslash literally
            decoded = (char)((hi << 4) | lo);
            consumed = 4;
            break;
        }
        default:
            break;
        }

        if (consumed == 2 && decoded == 0)
        {
            // Unknown escape: wpa_supplicant only emits the forms above, so
            // preserve the backslash instead of silently dropping data.
            out[o++] = '\\';
            p++;
            continue;
        }

        if (decoded == '\0')
            break; // an SSID cannot be carried in a C string past a NUL byte
        out[o++] = decoded;
        p += consumed;
    }

    out[o] = '\0';
    return o;
}

// Decodes one UTF-8 sequence. Returns the sequence length (1-4) and stores the
// code point, or returns 0 when the bytes at s are not valid UTF-8.
static inline size_t wifi_utf8Next(const char *s, size_t len, uint32_t *cp)
{
    if (!s || !cp || len == 0)
        return 0;

    const unsigned char *p = (const unsigned char *)s;
    unsigned char c = p[0];
    if (c < 0x80)
    {
        *cp = c;
        return 1;
    }
    if (c >= 0xc2 && c <= 0xdf)
    {
        if (len < 2 || (p[1] & 0xc0) != 0x80)
            return 0;
        *cp = ((uint32_t)(c & 0x1f) << 6) | (p[1] & 0x3f);
        return 2;
    }
    if (c >= 0xe0 && c <= 0xef)
    {
        if (len < 3 || (p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80)
            return 0;
        if (c == 0xe0 && p[1] < 0xa0)
            return 0; // overlong
        if (c == 0xed && p[1] >= 0xa0)
            return 0; // UTF-16 surrogate
        *cp = ((uint32_t)(c & 0x0f) << 12) | ((uint32_t)(p[1] & 0x3f) << 6) | (p[2] & 0x3f);
        return 3;
    }
    if (c >= 0xf0 && c <= 0xf4)
    {
        if (len < 4 || (p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80 || (p[3] & 0xc0) != 0x80)
            return 0;
        if (c == 0xf0 && p[1] < 0x90)
            return 0; // overlong
        if (c == 0xf4 && p[1] >= 0x90)
            return 0; // above U+10FFFF
        *cp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(p[1] & 0x3f) << 16) |
              ((uint32_t)(p[2] & 0x3f) << 6) | (p[3] & 0x3f);
        return 4;
    }
    return 0;
}

// Copies raw SSID bytes into a valid UTF-8 string for rendering. Invalid
// sequences, control characters and characters outside the BMP (which the
// device's SDL_ttf 2.0 cannot draw) become '?'. The bundled fonts have no
// U+FFFD, so the replacement has to be a glyph every font carries. The result
// is always NUL-terminated and truncated on a character boundary.
static inline size_t wifi_ssidDisplay(const char *raw, char *out, size_t out_size)
{
    if (!out || out_size == 0)
        return 0;
    out[0] = '\0';
    if (!raw)
        return 0;

    size_t o = 0;
    const char *p = raw;
    size_t remaining = strlen(raw);
    while (remaining > 0 && o + 1 < out_size)
    {
        uint32_t cp = 0;
        size_t n = wifi_utf8Next(p, remaining, &cp);
        if (n == 0)
        {
            out[o++] = '?';
            p++;
            remaining--;
            continue;
        }

        if (cp > 0xffff || cp < 0x20 || cp == 0x7f)
        {
            out[o++] = '?';
            p += n;
            remaining -= n;
            continue;
        }

        if (o + n >= out_size)
            break;
        memcpy(out + o, p, n);
        o += n;
        p += n;
        remaining -= n;
    }

    out[o] = '\0';
    return o;
}

static inline bool wifi_ssidParseInt(const char *s, size_t len, int *out)
{
    char buf[16];
    if (len == 0 || len >= sizeof(buf))
        return false;
    memcpy(buf, s, len);
    buf[len] = '\0';
    char *end = NULL;
    long value = strtol(buf, &end, 10);
    if (end == buf)
        return false;
    *out = (int)value;
    return true;
}

// Parses wpa_cli scan_results text into networks, decoding the SSID column.
//
//   bssid / frequency / signal level / flags / ssid
//   04:b4:fe:32:f9:73	2462	-63	[WPA2-PSK-CCMP][WPS][ESS]	frynet
//
// Returns the number of usable rows. A 32-byte SSID can be 128 characters in
// its escaped form, so rows are decoded through a temporary buffer instead of
// being scanned straight into WIFI_network.ssid.
static inline int wifi_ssidParseScanResults(const char *results, struct WIFI_network *networks, int max)
{
    if (!results || !networks || max <= 0)
        return 0;

    const char *line = strchr(results, '\n'); // skip the header line
    if (!line)
        return 0;
    line++;

    int count = 0;
    while (*line && count < max)
    {
        const char *end = strchr(line, '\n');
        size_t line_len = end ? (size_t)(end - line) : strlen(line);
        const char *limit = line + line_len;

        const char *fields[4];
        size_t field_lens[4];
        const char *p = line;
        bool ok = true;
        for (int i = 0; i < 4; i++)
        {
            const char *tab = (const char *)memchr(p, '\t', (size_t)(limit - p));
            if (!tab)
            {
                ok = false;
                break;
            }
            fields[i] = p;
            field_lens[i] = (size_t)(tab - p);
            p = tab + 1;
        }

        if (ok)
        {
            int freq = -1;
            int rssi = -1;
            ok = wifi_ssidParseInt(fields[1], field_lens[1], &freq) &&
                 wifi_ssidParseInt(fields[2], field_lens[2], &rssi);
            if (ok)
            {
                const char *ssid_field = p;
                size_t ssid_len = (size_t)(limit - ssid_field);
                while (ssid_len > 0 && (ssid_field[ssid_len - 1] == ' ' || ssid_field[ssid_len - 1] == '\t' ||
                                        ssid_field[ssid_len - 1] == '\r'))
                    ssid_len--;

                if (ssid_len > 0)
                {
                    char encoded[SSID_MAX * 4 + 1];
                    if (ssid_len >= sizeof(encoded))
                        ssid_len = sizeof(encoded) - 1;
                    memcpy(encoded, ssid_field, ssid_len);
                    encoded[ssid_len] = '\0';

                    char flags[128];
                    size_t flags_len = field_lens[3] < sizeof(flags) - 1 ? field_lens[3] : sizeof(flags) - 1;
                    memcpy(flags, fields[3], flags_len);
                    flags[flags_len] = '\0';

                    struct WIFI_network *network = &networks[count];
                    memset(network, 0, sizeof(*network));

                    size_t bssid_len = field_lens[0] < sizeof(network->bssid) - 1 ? field_lens[0] : sizeof(network->bssid) - 1;
                    memcpy(network->bssid, fields[0], bssid_len);
                    network->bssid[bssid_len] = '\0';
                    network->freq = freq;
                    network->rssi = rssi;
                    network->security = SECURITY_NONE;
                    if (strstr(flags, "WPA2-PSK"))
                        network->security = SECURITY_WPA2_PSK;
                    else if (strstr(flags, "WPA-PSK"))
                        network->security = SECURITY_WPA_PSK;
                    else if (strstr(flags, "WEP"))
                        network->security = SECURITY_WEP;
                    else if (strstr(flags, "EAP"))
                        network->security = SECURITY_UNSUPPORTED;

                    wifi_ssidDecode(encoded, network->ssid, sizeof(network->ssid));
                    if (network->ssid[0] != '\0')
                        count++;
                }
            }
        }

        line = end ? end + 1 : limit;
    }

    return count;
}

#endif
