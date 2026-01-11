/*
 * Tank Game - Networking Helpers
 */

#include "pz_net.h"

#include "../core/pz_mem.h"
#include "../core/pz_str.h"
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char pz_net_b64_table[]
    = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char
pz_net_b64_value(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char)(c - 'A');
    if (c >= 'a' && c <= 'z')
        return (char)(c - 'a' + 26);
    if (c >= '0' && c <= '9')
        return (char)(c - '0' + 52);
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

static char *
pz_net_base64_url_encode(const uint8_t *data, size_t len)
{
    if (data == NULL && len > 0)
        return NULL;

    size_t out_len = ((len + 2) / 3) * 4;
    char *out = pz_alloc(out_len + 1);
    if (!out)
        return NULL;

    size_t i = 0;
    size_t j = 0;

    while (i < len) {
        size_t remaining = len - i;
        uint32_t octet_a = data[i++];
        uint32_t octet_b = remaining > 1 ? data[i++] : 0;
        uint32_t octet_c = remaining > 2 ? data[i++] : 0;

        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        out[j++] = pz_net_b64_table[(triple >> 18) & 0x3F];
        out[j++] = pz_net_b64_table[(triple >> 12) & 0x3F];
        out[j++] = remaining > 1 ? pz_net_b64_table[(triple >> 6) & 0x3F] : '=';
        out[j++] = remaining > 2 ? pz_net_b64_table[triple & 0x3F] : '=';
    }

    out[j] = '\0';

    for (size_t k = 0; k < j; k++) {
        if (out[k] == '+')
            out[k] = '-';
        else if (out[k] == '/')
            out[k] = '_';
    }

    while (j > 0 && out[j - 1] == '=') {
        out[j - 1] = '\0';
        j--;
    }

    return out;
}

static bool
pz_net_base64_url_decode(const char *input, uint8_t **out_data, size_t *out_len)
{
    if (input == NULL || out_data == NULL || out_len == NULL)
        return false;

    size_t len = strlen(input);
    if (len == 0)
        return false;
    if (len % 4 == 1)
        return false;

    size_t pad = (4 - (len % 4)) % 4;
    size_t padded_len = len + pad;

    char *temp = pz_alloc(padded_len + 1);
    if (!temp)
        return false;

    for (size_t i = 0; i < len; i++) {
        char c = input[i];
        if (c == '-')
            c = '+';
        else if (c == '_')
            c = '/';
        temp[i] = c;
    }
    for (size_t i = 0; i < pad; i++) {
        temp[len + i] = '=';
    }
    temp[padded_len] = '\0';

    size_t decoded_len = (padded_len / 4) * 3;
    if (pad > 0)
        decoded_len -= pad;

    uint8_t *decoded = pz_alloc(decoded_len + 1);
    if (!decoded) {
        pz_free(temp);
        return false;
    }

    size_t i = 0;
    size_t j = 0;

    while (i < padded_len) {
        char c0 = temp[i++];
        char c1 = temp[i++];
        char c2 = temp[i++];
        char c3 = temp[i++];

        char v0 = pz_net_b64_value(c0);
        char v1 = pz_net_b64_value(c1);
        char v2 = (c2 == '=') ? 0 : pz_net_b64_value(c2);
        char v3 = (c3 == '=') ? 0 : pz_net_b64_value(c3);

        if (v0 < 0 || v1 < 0 || (c2 != '=' && v2 < 0)
            || (c3 != '=' && v3 < 0)) {
            pz_free(decoded);
            pz_free(temp);
            return false;
        }

        uint32_t triple = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12)
            | ((uint32_t)v2 << 6) | (uint32_t)v3;

        if (j < decoded_len)
            decoded[j++] = (uint8_t)((triple >> 16) & 0xFF);
        if (c2 != '=' && j < decoded_len)
            decoded[j++] = (uint8_t)((triple >> 8) & 0xFF);
        if (c3 != '=' && j < decoded_len)
            decoded[j++] = (uint8_t)(triple & 0xFF);
    }

    decoded[decoded_len] = '\0';
    *out_data = decoded;
    *out_len = decoded_len;

    pz_free(temp);
    return true;
}

static char *
pz_net_json_escape(const char *str)
{
    if (!str)
        return pz_str_dup("");

    size_t len = 0;
    for (const char *p = str; *p; p++) {
        switch (*p) {
        case '\\':
        case '"':
            len += 2;
            break;
        case '\n':
        case '\r':
        case '\t':
            len += 2;
            break;
        default:
            len++;
            break;
        }
    }

    char *out = pz_alloc(len + 1);
    if (!out)
        return NULL;

    char *dst = out;
    for (const char *p = str; *p; p++) {
        switch (*p) {
        case '\\':
            *dst++ = '\\';
            *dst++ = '\\';
            break;
        case '"':
            *dst++ = '\\';
            *dst++ = '"';
            break;
        case '\n':
            *dst++ = '\\';
            *dst++ = 'n';
            break;
        case '\r':
            *dst++ = '\\';
            *dst++ = 'r';
            break;
        case '\t':
            *dst++ = '\\';
            *dst++ = 't';
            break;
        default:
            *dst++ = *p;
            break;
        }
    }
    *dst = '\0';

    return out;
}

static char *
pz_net_json_unescape(const char *str, size_t len)
{
    char *out = pz_alloc(len + 1);
    if (!out)
        return NULL;

    size_t i = 0;
    size_t j = 0;

    while (i < len) {
        char c = str[i++];
        if (c == '\\' && i < len) {
            char next = str[i++];
            switch (next) {
            case 'n':
                out[j++] = '\n';
                break;
            case 'r':
                out[j++] = '\r';
                break;
            case 't':
                out[j++] = '\t';
                break;
            case '\\':
                out[j++] = '\\';
                break;
            case '"':
                out[j++] = '"';
                break;
            default:
                out[j++] = next;
                break;
            }
        } else {
            out[j++] = c;
        }
    }

    out[j] = '\0';
    return out;
}

static bool
pz_net_json_find_uint(const char *json, const char *key, uint32_t *out_value)
{
    if (!json || !key || !out_value)
        return false;

    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *start = strstr(json, needle);
    if (!start)
        return false;

    start += strlen(needle);
    while (*start == ' ')
        start++;

    char *end = NULL;
    unsigned long value = strtoul(start, &end, 10);
    if (start == end)
        return false;

    *out_value = (uint32_t)value;
    return true;
}

static bool
pz_net_json_find_string(const char *json, const char *key, char **out_value)
{
    if (!json || !key || !out_value)
        return false;

    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char *start = strstr(json, needle);
    if (!start)
        return false;

    start += strlen(needle);
    const char *p = start;
    bool escaped = false;

    while (*p) {
        if (escaped) {
            escaped = false;
        } else if (*p == '\\') {
            escaped = true;
        } else if (*p == '"') {
            break;
        }
        p++;
    }

    if (*p != '"')
        return false;

    size_t len = (size_t)(p - start);
    char *unescaped = pz_net_json_unescape(start, len);
    if (!unescaped)
        return false;

    *out_value = unescaped;
    return true;
}

pz_net_offer *
pz_net_offer_create(uint32_t version, const char *host_name,
    const char *map_name, const char *sdp)
{
    pz_net_offer *offer = pz_alloc(sizeof(pz_net_offer));
    if (!offer)
        return NULL;

    offer->version = version;
    offer->host_name = pz_str_dup(host_name ? host_name : "");
    offer->map_name = pz_str_dup(map_name ? map_name : "");
    offer->sdp = pz_str_dup(sdp ? sdp : "");

    if (!offer->host_name || !offer->map_name || !offer->sdp) {
        pz_net_offer_free(offer);
        return NULL;
    }

    return offer;
}

void
pz_net_offer_free(pz_net_offer *offer)
{
    if (!offer)
        return;

    pz_free(offer->host_name);
    pz_free(offer->map_name);
    pz_free(offer->sdp);
    pz_free(offer);
}

char *
pz_net_offer_encode_json(const pz_net_offer *offer)
{
    if (!offer)
        return NULL;

    char *host_name = pz_net_json_escape(offer->host_name);
    char *map_name = pz_net_json_escape(offer->map_name);
    char *sdp = pz_net_json_escape(offer->sdp);

    if (!host_name || !map_name || !sdp) {
        pz_free(host_name);
        pz_free(map_name);
        pz_free(sdp);
        return NULL;
    }

    char *json
        = pz_str_fmt("{\"v\":%u,\"name\":\"%s\",\"map\":\"%s\",\"sdp\":\"%s\"}",
            offer->version, host_name, map_name, sdp);

    pz_free(host_name);
    pz_free(map_name);
    pz_free(sdp);

    return json;
}

char *
pz_net_offer_encode_url(const pz_net_offer *offer)
{
    if (!offer)
        return NULL;

    char *json = pz_net_offer_encode_json(offer);
    if (!json)
        return NULL;

    char *token = pz_net_base64_url_encode((const uint8_t *)json, strlen(json));
    pz_free(json);

    if (!token)
        return NULL;

    char *url = pz_str_fmt("%s%s", PZ_NET_JOIN_URL_PREFIX, token);
    pz_free(token);

    return url;
}

static const char *
pz_net_find_offer_token(const char *url)
{
    if (!url)
        return NULL;

    const char *join = strstr(url, "#join/");
    if (join)
        return join + strlen("#join/");

    size_t prefix_len = strlen(PZ_NET_JOIN_URL_PREFIX);
    if (strncmp(url, PZ_NET_JOIN_URL_PREFIX, prefix_len) == 0)
        return url + prefix_len;

    return url;
}

static pz_net_offer *
pz_net_offer_decode_json_internal(const char *json)
{
    if (!json)
        return NULL;

    uint32_t version = 0;
    char *name = NULL;
    char *map = NULL;
    char *sdp = NULL;

    bool ok = pz_net_json_find_uint(json, "v", &version)
        && pz_net_json_find_string(json, "name", &name)
        && pz_net_json_find_string(json, "map", &map)
        && pz_net_json_find_string(json, "sdp", &sdp);

    if (!ok) {
        pz_free(name);
        pz_free(map);
        pz_free(sdp);
        return NULL;
    }

    pz_net_offer *offer = pz_net_offer_create(version, name, map, sdp);

    pz_free(name);
    pz_free(map);
    pz_free(sdp);

    return offer;
}

pz_net_offer *
pz_net_offer_decode_json(const char *json)
{
    if (!json)
        return NULL;

    char *trimmed = pz_str_trim(json);
    if (!trimmed || trimmed[0] == '\0') {
        pz_free(trimmed);
        return NULL;
    }

    pz_net_offer *offer = pz_net_offer_decode_json_internal(trimmed);
    pz_free(trimmed);
    return offer;
}

pz_net_offer *
pz_net_offer_decode_url(const char *url)
{
    if (!url)
        return NULL;

    const char *token = pz_net_find_offer_token(url);
    if (!token || *token == '\0')
        return NULL;

    char *trimmed = pz_str_trim(token);
    if (!trimmed || trimmed[0] == '\0') {
        pz_free(trimmed);
        return NULL;
    }

    size_t trimmed_len = strlen(trimmed);
    char *clean = pz_alloc(trimmed_len + 1);
    if (!clean) {
        pz_free(trimmed);
        return NULL;
    }

    size_t clean_len = 0;
    for (size_t i = 0; i < trimmed_len; i++) {
        unsigned char c = (unsigned char)trimmed[i];
        if (!isspace(c)) {
            clean[clean_len++] = (char)c;
        }
    }
    clean[clean_len] = '\0';
    pz_free(trimmed);

    if (clean_len == 0) {
        pz_free(clean);
        return NULL;
    }

    uint8_t *decoded = NULL;
    size_t decoded_len = 0;
    if (!pz_net_base64_url_decode(clean, &decoded, &decoded_len)) {
        pz_free(clean);
        return NULL;
    }
    pz_free(clean);

    char *json = pz_str_ndup((const char *)decoded, decoded_len);
    pz_free(decoded);

    if (!json)
        return NULL;

    pz_net_offer *offer = pz_net_offer_decode_json_internal(json);
    pz_free(json);

    return offer;
}
