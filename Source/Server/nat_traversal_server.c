/*
 * Mupen MPN NAT traversal + index server
 *
 * UDP 9290 (default):
 *   N02TRAV1  host codes (REGISTER, LOOKUP, KEEP, UNREGISTER)
 *   N02IDX1   key/value index (SET, GET, DEL, LIST) — values as B64:...
 *
 * HTTP 9191 (default, --http-port):
 *   GET  /              HTML index of keys
 *   GET  /rooms         JSON list of active traversal rooms
 *   GET  /index/{key}   read value
 *   PUT  /index/{key}   store body (also POST)
 *   GET  /turn/ice-servers  short-lived Cloudflare TURN credentials (broker)
 *
 * Cloudflare TURN broker (server-side only):
 *   turn_secrets.h (compiled in) or RMG_TURN_KEY_ID / RMG_TURN_API_TOKEN env overrides
 *
 * Build:
 *   gcc -O2 -Wall -o nat_traversal_server nat_traversal_server.c -lws2_32
 *   make            # links libcurl when available for /turn/ice-servers
 */

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_t;
#define SOCKET_INVALID INVALID_SOCKET
#define socket_close closesocket
#define socket_errno WSAGetLastError()
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int socket_t;
#define SOCKET_INVALID (-1)
#define socket_close close
#define socket_errno errno
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef HAS_TURN_SECRETS_H
#include "turn_secrets.h"
#else
#define RMG_TURN_KEY_ID_EMBED ""
#define RMG_TURN_API_TOKEN_EMBED ""
#define RMG_TURN_CREDENTIAL_TTL_EMBED 0
#endif

#define TRAV_PROTOCOL "N02TRAV1"
#define INDEX_PROTOCOL "N02IDX1"
#define DEFAULT_UDP_PORT 9150
#define DEFAULT_HTTP_PORT 9151
#define MAX_HOSTS 4096
#define MAX_INDEX_ENTRIES 512
#define HOST_TTL_SEC 300
#define INDEX_TTL_SEC 300
#define MAX_HOST_CODE 0x0FFFFFFF
#define MAX_KEY_LEN 128
#define MAX_VALUE_LEN 8192
#define MAX_UDP_PACKET 8192
#define MAX_HTTP_REQUEST 16384
#define MAX_TURN_RESPONSE 32768
#define DEFAULT_TURN_CREDENTIAL_TTL_SEC 86400
#define TURN_CACHE_REFRESH_SKEW_SEC 300

#ifdef WITH_LIBCURL
#include <curl/curl.h>
#endif

typedef struct {
    uint32_t code;
    uint32_t host_ip;
    uint16_t signaling_port;
    uint16_t traversal_port;
    uint64_t last_seen;
    int in_use;
    int list_in_browser;
} host_entry_t;

typedef struct {
    char key[MAX_KEY_LEN];
    char value[MAX_VALUE_LEN];
    size_t value_len;
    uint64_t last_seen;
    int in_use;
} index_entry_t;

static host_entry_t g_hosts[MAX_HOSTS];
static int g_host_count = 0;
static index_entry_t g_index[MAX_INDEX_ENTRIES];
static int g_index_count = 0;

static char g_turn_cache[MAX_TURN_RESPONSE];
static size_t g_turn_cache_len = 0;
static uint64_t g_turn_cache_expires = 0;
static int g_turn_credential_ttl_sec = DEFAULT_TURN_CREDENTIAL_TTL_SEC;

static uint64_t now_seconds(void)
{
    return (uint64_t)time(NULL);
}

static void send_reply(socket_t sock, const struct sockaddr* addr, socklen_t addr_len, const char* message)
{
    sendto(sock, message, (int)strlen(message), 0, addr, addr_len);
}

static int is_valid_key(const char* key)
{
    size_t len;
    if (key == NULL) {
        return 0;
    }
    len = strlen(key);
    if (len == 0 || len >= MAX_KEY_LEN) {
        return 0;
    }
    for (size_t i = 0; i < len; ++i) {
        char c = key[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
              c == '_' || c == '-' || c == '/')) {
            return 0;
        }
    }
    return 1;
}

/* ---- base64 (encode only for replies; decode for SET) ---- */

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_decode_char(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static size_t b64_decode(const char* in, unsigned char* out, size_t out_max)
{
    size_t len = strlen(in);
    size_t o = 0;
    int val = 0;
    int valb = -8;

    for (size_t i = 0; i < len; ++i) {
        int c = in[i];
        if (c == '=') {
            break;
        }
        int d = b64_decode_char(c);
        if (d < 0) {
            continue;
        }
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            if (o < out_max) {
                out[o++] = (unsigned char)((val >> valb) & 0xFF);
            }
            valb -= 8;
        }
    }
    return o;
}

static size_t b64_encode(const unsigned char* in, size_t in_len, char* out, size_t out_max)
{
    size_t o = 0;
    for (size_t i = 0; i < in_len; i += 3) {
        uint32_t triple = (uint32_t)in[i] << 16;
        if (i + 1 < in_len) {
            triple |= (uint32_t)in[i + 1] << 8;
        }
        if (i + 2 < in_len) {
            triple |= (uint32_t)in[i + 2];
        }
        if (o + 4 >= out_max) {
            break;
        }
        out[o++] = b64_table[(triple >> 18) & 0x3F];
        out[o++] = b64_table[(triple >> 12) & 0x3F];
        out[o++] = (i + 1 < in_len) ? b64_table[(triple >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < in_len) ? b64_table[triple & 0x3F] : '=';
    }
    if (o < out_max) {
        out[o] = '\0';
    }
    return o;
}

/* ---- NAT traversal (N02TRAV1) ---- */

static int format_host_code(uint32_t code, char* out, size_t out_size)
{
    if (out_size < 8) {
        return 0;
    }
    snprintf(out, out_size, "%07X", code & MAX_HOST_CODE);
    return 1;
}

static int parse_host_code(const char* text, uint32_t* code_out)
{
    size_t len;
    if (text == NULL || code_out == NULL || strlen(text) != 7) {
        return 0;
    }
    len = strlen(text);
    for (size_t i = 0; i < len; ++i) {
        char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
            return 0;
        }
    }
    {
        char* end = NULL;
        unsigned long value = strtoul(text, &end, 16);
        if (end == NULL || *end != '\0' || value > MAX_HOST_CODE) {
            return 0;
        }
        *code_out = (uint32_t)value;
    }
    return 1;
}

static host_entry_t* find_host(uint32_t code)
{
    for (int i = 0; i < g_host_count; ++i) {
        if (g_hosts[i].in_use && g_hosts[i].code == code) {
            return &g_hosts[i];
        }
    }
    return NULL;
}

static host_entry_t* find_host_by_traversal_endpoint(uint32_t host_ip, uint16_t traversal_port)
{
    const uint64_t now = now_seconds();
    for (int i = 0; i < g_host_count; ++i) {
        if (g_hosts[i].in_use && g_hosts[i].host_ip == host_ip && g_hosts[i].traversal_port == traversal_port &&
            now - g_hosts[i].last_seen <= HOST_TTL_SEC) {
            return &g_hosts[i];
        }
    }
    return NULL;
}

static void index_del(const char* key);

static void retire_hosts_at_signaling_endpoint(uint32_t host_ip, uint16_t signaling_port, uint32_t except_code)
{
    for (int i = 0; i < g_host_count; ++i) {
        if (!g_hosts[i].in_use || g_hosts[i].host_ip != host_ip || g_hosts[i].signaling_port != signaling_port ||
            g_hosts[i].code == except_code) {
            continue;
        }

        char code_text[8];
        g_hosts[i].in_use = 0;
        if (format_host_code(g_hosts[i].code, code_text, sizeof(code_text))) {
            char session_key[MAX_KEY_LEN];
            snprintf(session_key, sizeof(session_key), "session/%s", code_text);
            index_del(session_key);
            {
                struct in_addr addr;
                addr.s_addr = host_ip;
                printf("[trav] RETIRE %s (superseded at %s:%u)\n", code_text, inet_ntoa(addr),
                       (unsigned)signaling_port);
            }
        }
    }
}

static void prune_hosts(void)
{
    const uint64_t now = now_seconds();
    for (int i = 0; i < g_host_count; ++i) {
        if (g_hosts[i].in_use && now - g_hosts[i].last_seen > HOST_TTL_SEC) {
            g_hosts[i].in_use = 0;

            // Fix: Clean up index data if the room drops due to TTL timeout
            char code_text[8];
            if (format_host_code(g_hosts[i].code, code_text, sizeof(code_text))) {
                char session_key[MAX_KEY_LEN];
                snprintf(session_key, sizeof(session_key), "session/%s", code_text);
                index_del(session_key);
            }
        }
    }
}

static uint32_t allocate_code(void)
{
    for (int attempt = 0; attempt < 100000; ++attempt) {
        uint32_t code = ((uint32_t)rand() << 12) ^ (uint32_t)rand();
        code &= MAX_HOST_CODE;
        if (code != 0 && find_host(code) == NULL) {
            return code;
        }
    }
    return 0;
}

static host_entry_t* upsert_host(uint32_t code, uint32_t host_ip, uint16_t port, uint16_t traversal_port,
                                 uint64_t now, int list_in_browser)
{
    host_entry_t* entry = find_host(code);
    if (entry != NULL) {
        entry->host_ip = host_ip;
        entry->signaling_port = port;
        entry->traversal_port = traversal_port;
        entry->last_seen = now;
        entry->list_in_browser = list_in_browser;
        return entry;
    }
    for (int i = 0; i < g_host_count; ++i) {
        if (!g_hosts[i].in_use) {
            entry = &g_hosts[i];
            entry->in_use = 1;
            entry->code = code;
            entry->host_ip = host_ip;
            entry->signaling_port = port;
            entry->traversal_port = traversal_port;
            entry->last_seen = now;
            entry->list_in_browser = list_in_browser;
            return entry;
        }
    }
    if (g_host_count >= MAX_HOSTS) {
        return NULL;
    }
    entry = &g_hosts[g_host_count++];
    memset(entry, 0, sizeof(*entry));
    entry->in_use = 1;
    entry->code = code;
    entry->host_ip = host_ip;
    entry->signaling_port = port;
    entry->traversal_port = traversal_port;
    entry->last_seen = now;
    entry->list_in_browser = list_in_browser;
    return entry;
}

static void trav_register(socket_t sock, const struct sockaddr_in* client, socklen_t client_len, const char* port_text,
                          int list_in_browser)
{
    char code_text[8];
    char reply[96];
    int parsed_port = atoi(port_text);
    uint16_t port;
    uint16_t traversal_port;
    uint32_t code;

    if (parsed_port < 1024 || parsed_port > 65535) {
        send_reply(sock, (const struct sockaddr*)client, client_len, TRAV_PROTOCOL "|ERR|BADPORT");
        return;
    }
    port = (uint16_t)parsed_port;
    traversal_port = ntohs(client->sin_port);

    {
        host_entry_t* existing = find_host_by_traversal_endpoint(client->sin_addr.s_addr, traversal_port);
        if (existing != NULL) {
            if (!format_host_code(existing->code, code_text, sizeof(code_text))) {
                send_reply(sock, (const struct sockaddr*)client, client_len, TRAV_PROTOCOL "|ERR|NOCODE");
                return;
            }

            existing->host_ip = client->sin_addr.s_addr;
            existing->signaling_port = port;
            existing->traversal_port = traversal_port;
            existing->last_seen = now_seconds();
            existing->list_in_browser = list_in_browser;

            if (!list_in_browser) {
                char session_key[MAX_KEY_LEN];
                snprintf(session_key, sizeof(session_key), "session/%s", code_text);
                index_del(session_key);
            }

            snprintf(reply, sizeof(reply), TRAV_PROTOCOL "|REGISTEROK|%s|%s|%u", code_text,
                     inet_ntoa(client->sin_addr), (unsigned)port);
            for (int i = 0; i < 3; ++i) {
                send_reply(sock, (const struct sockaddr*)client, client_len, reply);
            }
            printf("[trav] RE-REGISTER %s -> %s:%u (signaling port %u, list=%d)\n", code_text,
                   inet_ntoa(client->sin_addr), (unsigned)ntohs(client->sin_port), (unsigned)port, list_in_browser);
            return;
        }
    }

    code = allocate_code();
    if (code == 0 || !format_host_code(code, code_text, sizeof(code_text))) {
        send_reply(sock, (const struct sockaddr*)client, client_len, TRAV_PROTOCOL "|ERR|NOCODE");
        return;
    }
    if (upsert_host(code, client->sin_addr.s_addr, port, traversal_port, now_seconds(), list_in_browser) == NULL) {
        send_reply(sock, (const struct sockaddr*)client, client_len, TRAV_PROTOCOL "|ERR|FULL");
        return;
    }

    retire_hosts_at_signaling_endpoint(client->sin_addr.s_addr, port, code);

    snprintf(reply, sizeof(reply), TRAV_PROTOCOL "|REGISTEROK|%s|%s|%u", code_text,
             inet_ntoa(client->sin_addr), (unsigned)port);
    for (int i = 0; i < 3; ++i) {
        send_reply(sock, (const struct sockaddr*)client, client_len, reply);
    }
    printf("[trav] REGISTER %s -> %s:%u (signaling port %u, list=%d)\n", code_text, inet_ntoa(client->sin_addr),
           (unsigned)ntohs(client->sin_port), (unsigned)port, list_in_browser);

    if (!list_in_browser) {
        char session_key[MAX_KEY_LEN];
        snprintf(session_key, sizeof(session_key), "session/%s", code_text);
        index_del(session_key);
    }
}

static void trav_send_punch(socket_t sock, const struct sockaddr_in* target, socklen_t target_len, const char* ip_text,
                            unsigned target_port)
{
    char reply[96];
    snprintf(reply, sizeof(reply), TRAV_PROTOCOL "|PUNCH|%s|%u", ip_text, target_port);
    for (int i = 0; i < 10; ++i) {
        send_reply(sock, (const struct sockaddr*)target, target_len, reply);
    }
}

static void trav_lookup(socket_t sock, const struct sockaddr_in* client, socklen_t client_len, const char* code_text)
{
    char reply[96];
    uint32_t code = 0;
    struct in_addr addr;

    if (!parse_host_code(code_text, &code)) {
        send_reply(sock, (const struct sockaddr*)client, client_len, TRAV_PROTOCOL "|LOOKUPFAIL|0000000|INVALID");
        return;
    }

    host_entry_t* entry = find_host(code);
    if (entry == NULL || now_seconds() - entry->last_seen > HOST_TTL_SEC) {
        snprintf(reply, sizeof(reply), TRAV_PROTOCOL "|LOOKUPFAIL|%s|NOTFOUND", code_text);
        send_reply(sock, (const struct sockaddr*)client, client_len, reply);
        return;
    }

    addr.s_addr = entry->host_ip;
    snprintf(reply, sizeof(reply), TRAV_PROTOCOL "|LOOKUPOK|%s|%s|%u", code_text, inet_ntoa(addr),
             (unsigned)entry->signaling_port);
    send_reply(sock, (const struct sockaddr*)client, client_len, reply);

    /* Coordinate UDP hole punching for port-restricted cone NAT (Dolphin-style). */
    {
        struct sockaddr_in host_addr;
        memset(&host_addr, 0, sizeof(host_addr));
        host_addr.sin_family = AF_INET;
        host_addr.sin_addr.s_addr = entry->host_ip;
        host_addr.sin_port = htons(entry->traversal_port > 0 ? entry->traversal_port : entry->signaling_port);

        trav_send_punch(sock, &host_addr, sizeof(host_addr), inet_ntoa(client->sin_addr),
                        (unsigned)ntohs(client->sin_port));
        trav_send_punch(sock, client, client_len, inet_ntoa(addr), (unsigned)entry->signaling_port);
    }
}

/* ---- Index (N02IDX1) ---- */

static index_entry_t* find_index(const char* key)
{
    for (int i = 0; i < g_index_count; ++i) {
        if (g_index[i].in_use && strcmp(g_index[i].key, key) == 0) {
            return &g_index[i];
        }
    }
    return NULL;
}

static void prune_index(void)
{
    const uint64_t now = now_seconds();
    for (int i = 0; i < g_index_count; ++i) {
        if (g_index[i].in_use && now - g_index[i].last_seen > INDEX_TTL_SEC) {
            g_index[i].in_use = 0;
        }
    }
}

static index_entry_t* upsert_index(const char* key, const unsigned char* value, size_t value_len, uint64_t now)
{
    index_entry_t* entry = find_index(key);
    if (value_len >= MAX_VALUE_LEN) {
        return NULL;
    }

    if (entry != NULL) {
        memcpy(entry->value, value, value_len);
        entry->value[value_len] = '\0';
        entry->value_len = value_len;
        entry->last_seen = now;
        return entry;
    }

    for (int i = 0; i < g_index_count; ++i) {
        if (!g_index[i].in_use) {
            entry = &g_index[i];
            strncpy(entry->key, key, MAX_KEY_LEN - 1);
            entry->key[MAX_KEY_LEN - 1] = '\0';
            memcpy(entry->value, value, value_len);
            entry->value[value_len] = '\0';
            entry->value_len = value_len;
            entry->last_seen = now;
            entry->in_use = 1;
            return entry;
        }
    }

    if (g_index_count >= MAX_INDEX_ENTRIES) {
        return NULL;
    }

    entry = &g_index[g_index_count++];
    memset(entry, 0, sizeof(*entry));
    strncpy(entry->key, key, MAX_KEY_LEN - 1);
    entry->key[MAX_KEY_LEN - 1] = '\0';
    memcpy(entry->value, value, value_len);
    entry->value[value_len] = '\0';
    entry->value_len = value_len;
    entry->last_seen = now;
    entry->in_use = 1;
    return entry;
}

static void index_set(socket_t sock, const struct sockaddr_in* client, socklen_t client_len, const char* key,
                      const char* value_field)
{
    char reply[128];
    unsigned char decoded[MAX_VALUE_LEN];
    size_t decoded_len = 0;

    if (!is_valid_key(key)) {
        printf("[idx] SET BADKEY '%s' from %s:%u\n", key, inet_ntoa(client->sin_addr), (unsigned)ntohs(client->sin_port));
        send_reply(sock, (const struct sockaddr*)client, client_len, INDEX_PROTOCOL "|ERR|BADKEY");
        return;
    }

    if (strncmp(value_field, "B64:", 4) == 0) {
        decoded_len = b64_decode(value_field + 4, decoded, sizeof(decoded));
        printf("[idx] Decoded B64 length=%zu for key=%s from %s:%u\n", decoded_len, key, inet_ntoa(client->sin_addr), (unsigned)ntohs(client->sin_port));
    } else {
        decoded_len = strlen(value_field);
        if (decoded_len >= sizeof(decoded)) {
            printf("[idx] SET TOO LARGE %zu bytes for key=%s from %s:%u\n", decoded_len, key, inet_ntoa(client->sin_addr), (unsigned)ntohs(client->sin_port));
            send_reply(sock, (const struct sockaddr*)client, client_len, INDEX_PROTOCOL "|ERR|TOOLARGE");
            return;
        }
        memcpy(decoded, value_field, decoded_len);
    }

    if (upsert_index(key, decoded, decoded_len, now_seconds()) == NULL) {
        printf("[idx] SET FAILED for key=%s (index full or error) from %s:%u\n", key, inet_ntoa(client->sin_addr), (unsigned)ntohs(client->sin_port));
        send_reply(sock, (const struct sockaddr*)client, client_len, INDEX_PROTOCOL "|ERR|FULL");
        return;
    }

    snprintf(reply, sizeof(reply), INDEX_PROTOCOL "|SETOK|%s", key);
    send_reply(sock, (const struct sockaddr*)client, client_len, reply);
    printf("[idx] SET %s (%zu bytes)\n", key, decoded_len);
}

static void index_get(socket_t sock, const struct sockaddr_in* client, socklen_t client_len, const char* key)
{
    char reply[MAX_UDP_PACKET];
    char b64[MAX_VALUE_LEN * 2];
    index_entry_t* entry;

    if (!is_valid_key(key)) {
        send_reply(sock, (const struct sockaddr*)client, client_len, INDEX_PROTOCOL "|ERR|BADKEY");
        return;
    }

    entry = find_index(key);
    if (entry == NULL || now_seconds() - entry->last_seen > INDEX_TTL_SEC) {
        snprintf(reply, sizeof(reply), INDEX_PROTOCOL "|GETFAIL|%s|NOTFOUND", key);
        send_reply(sock, (const struct sockaddr*)client, client_len, reply);
        return;
    }

    b64_encode((const unsigned char*)entry->value, entry->value_len, b64, sizeof(b64));
    snprintf(reply, sizeof(reply), INDEX_PROTOCOL "|GETOK|%s|B64:%s", key, b64);
    send_reply(sock, (const struct sockaddr*)client, client_len, reply);
}

static void index_del(const char* key)
{
    index_entry_t* entry = find_index(key);
    if (entry != NULL) {
        entry->in_use = 0;
        printf("[idx] DEL %s\n", key);
    }
}

static void index_list(socket_t sock, const struct sockaddr_in* client, socklen_t client_len)
{
    char reply[MAX_UDP_PACKET];
    size_t offset = 0;
    const uint64_t now = now_seconds();

    offset += (size_t)snprintf(reply, sizeof(reply), "%s|LISTOK|", INDEX_PROTOCOL);

    for (int i = 0; i < g_index_count; ++i) {
        if (!g_index[i].in_use || now - g_index[i].last_seen > INDEX_TTL_SEC) {
            continue;
        }
        if (offset > strlen(INDEX_PROTOCOL "|LISTOK|")) {
            if (offset + 1 < sizeof(reply)) {
                reply[offset++] = ',';
            }
        }
        offset += (size_t)snprintf(reply + offset, sizeof(reply) - offset, "%s", g_index[i].key);
        if (offset >= sizeof(reply) - 16) {
            break;
        }
    }

    send_reply(sock, (const struct sockaddr*)client, client_len, reply);
}

static int split_parts(char* message, int length, char* parts[], int max_parts)
{
    int count = 0;
    if (message == NULL || max_parts <= 0 || length <= 0) {
        return 0;
    }
    parts[count++] = message;
    for (int i = 0; i < length && count < max_parts; ++i) {
        if (message[i] == '|') {
            message[i] = '\0';
            if (i + 1 < length) {
                parts[count++] = &message[i + 1];
            }
        }
    }
    return count;
}

static const char* field_after_key(const char* message, const char* op, const char* key)
{
    char pattern[160];
    snprintf(pattern, sizeof(pattern), "%s|%s|%s|", INDEX_PROTOCOL, op, key);
    {
        const char* pos = strstr(message, pattern);
        if (pos == NULL) {
            return NULL;
        }
        return pos + strlen(pattern);
    }
}

static void handle_udp(socket_t sock, const char* buffer, int length, const struct sockaddr_in* client,
                       socklen_t client_len)
{
    char message[MAX_UDP_PACKET];
    char* parts[8];
    int part_count;

    if (length <= 0 || length >= (int)sizeof(message)) {
        return;
    }

    memcpy(message, buffer, (size_t)length);
    message[length] = '\0';
    for (int i = 0; i < length; ++i) {
        if (message[i] == '\0') {
            message[i] = '|';
        }
    }

    part_count = split_parts(message, length, parts, 8);
    if (part_count < 2) {
        return;
    }

    if (strcmp(parts[0], TRAV_PROTOCOL) == 0) {
        prune_hosts();
        if (strcmp(parts[1], "REGISTER") == 0 && part_count >= 3) {
            int list_in_browser = 1;
            if (part_count >= 4) {
                list_in_browser = (strcmp(parts[3], "0") != 0);
            }
            trav_register(sock, client, client_len, parts[2], list_in_browser);
        } else if (strcmp(parts[1], "KEEP") == 0 && part_count >= 3) {
            uint32_t code = 0;
            if (parse_host_code(parts[2], &code)) {
                host_entry_t* entry = find_host(code);
                if (entry != NULL) {
                    entry->last_seen = now_seconds();
                    if (part_count >= 4) {
                        entry->list_in_browser = (strcmp(parts[3], "0") != 0);
                        if (!entry->list_in_browser) {
                            char session_key[MAX_KEY_LEN];
                            snprintf(session_key, sizeof(session_key), "session/%s", parts[2]);
                            index_del(session_key);
                        }
                    }
                }
            }
        } else if (strcmp(parts[1], "LOOKUP") == 0 && part_count >= 3) {
            for (int i = 0; parts[2][i] != '\0'; i++) {
                if (parts[2][i] >= 'a' && parts[2][i] <= 'f') {
                    parts[2][i] = parts[2][i] - 'a' + 'A';
                }
            }
            trav_lookup(sock, client, client_len, parts[2]);
        } else if (strcmp(parts[1], "UNREGISTER") == 0 && part_count >= 3) {
            uint32_t code = 0;
            if (parse_host_code(parts[2], &code)) {
                host_entry_t* entry = find_host(code);
                if (entry != NULL) {
                    entry->in_use = 0;
                    
                    // Fix: Explicitly remove the corresponding session details from index
                    char session_key[MAX_KEY_LEN];
                    snprintf(session_key, sizeof(session_key), "session/%s", parts[2]);
                    index_del(session_key);
                }
            }
        }
        return;
    }

    if (strcmp(parts[0], INDEX_PROTOCOL) == 0) {
        /* Diagnostic log for incoming index requests: show length and hex of first bytes */
        {
            char ip_text[INET_ADDRSTRLEN];
            struct in_addr caddr = client->sin_addr;
            const char* ip_string = inet_ntop(AF_INET, &caddr, ip_text, sizeof(ip_text));
            if (!ip_string) ip_string = "0.0.0.0";
            /* length is the original datagram length available in this scope */
            printf("[idx] RX from %s:%u len=%d data=", ip_string, (unsigned)ntohs(client->sin_port), length);
            for (int i = 0; i < length && i < 64; ++i) {
                unsigned char b = (unsigned char)message[i];
                printf("%02X", b);
            }
            if (length > 64) {
                printf("...");
            }
            printf("\n");
        }

        prune_index();
        if (strcmp(parts[1], "SET") == 0 && part_count >= 4) {
            const char* value = field_after_key(message, "SET", parts[2]);
            if (value == NULL) {
                value = parts[3];
            }
            index_set(sock, client, client_len, parts[2], value);
        } else if (strcmp(parts[1], "GET") == 0 && part_count >= 3) {
            index_get(sock, client, client_len, parts[2]);
        } else if (strcmp(parts[1], "DEL") == 0 && part_count >= 3) {
            index_del(parts[2]);
        } else if (strcmp(parts[1], "LIST") == 0) {
            index_list(sock, client, client_len);
        }
        return;
    }
}

/* ---- HTTP index ---- */

static void http_send_all(socket_t client, const char* data)
{
    size_t total = strlen(data);
    size_t sent = 0;
    while (sent < total) {
        int n = send(client, data + sent, (int)(total - sent), 0);
        if (n <= 0) {
            break;
        }
        sent += (size_t)n;
    }
}

static void http_reply_json(socket_t client, int status_code, const char* status_text, const char* json_body)
{
    char header[256];
    const size_t body_len = json_body != NULL ? strlen(json_body) : 0;

    snprintf(header, sizeof(header),
             "HTTP/1.1 %d %s\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: %zu\r\n"
             "Connection: close\r\n\r\n",
             status_code, status_text, body_len);
    http_send_all(client, header);
    if (body_len > 0) {
        http_send_all(client, json_body);
    }
}

static int http_parse_content_length(const char* headers)
{
    const char* cl = strstr(headers, "Content-Length:");
    if (cl == NULL) {
        cl = strstr(headers, "content-length:");
    }
    if (cl == NULL) {
        return -1;
    }

    cl += 15;
    while (*cl == ' ' || *cl == '\t') {
        ++cl;
    }
    return atoi(cl);
}

static int http_recv_request(socket_t client, char* buffer, size_t buffer_size, char** body_out, size_t* body_len_out)
{
    size_t total = 0;
    char* header_end = NULL;
    size_t header_len = 0;
    int content_length = -1;

    if (body_out) {
        *body_out = NULL;
    }
    if (body_len_out) {
        *body_len_out = 0;
    }

    while (total < buffer_size - 1) {
        int received = recv(client, buffer + total, (int)(buffer_size - 1 - total), 0);
        if (received <= 0) {
            break;
        }

        total += (size_t)received;
        buffer[total] = '\0';

        header_end = strstr(buffer, "\r\n\r\n");
        if (header_end == NULL) {
            continue;
        }

        header_len = (size_t)(header_end - buffer) + 4;
        content_length = http_parse_content_length(buffer);
        if (content_length < 0) {
            break;
        }

        while (total < header_len + (size_t)content_length && total < buffer_size - 1) {
            received = recv(client, buffer + total, (int)(buffer_size - 1 - total), 0);
            if (received <= 0) {
                break;
            }
            total += (size_t)received;
        }

        buffer[total < buffer_size ? total : buffer_size - 1] = '\0';
        break;
    }

    if (header_end == NULL) {
        return total > 0 ? (int)total : -1;
    }

    if (body_out) {
        *body_out = buffer + header_len;
    }
    if (body_len_out) {
        if (content_length >= 0) {
            size_t available = total > header_len ? total - header_len : 0;
            *body_len_out = available < (size_t)content_length ? available : (size_t)content_length;
        } else {
            *body_len_out = total > header_len ? total - header_len : 0;
        }
    }

    return (int)total;
}

static void http_reply_index_page(socket_t client)
{
    char body[65536];
    size_t offset = 0;
    const uint64_t now = now_seconds();

    offset += (size_t)snprintf(body + offset, sizeof(body) - offset,
                               "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
                               "<title>Mupen MPN Index</title></head><body><h1>Mupen MPN Index</h1><ul>");

    for (int i = 0; i < g_index_count; ++i) {
        if (!g_index[i].in_use || now - g_index[i].last_seen > INDEX_TTL_SEC) {
            continue;
        }
        offset += (size_t)snprintf(body + offset, sizeof(body) - offset,
                                   "<li><a href=\"/index/%s\">%s</a> (%zu bytes)</li>", g_index[i].key,
                                   g_index[i].key, g_index[i].value_len);
        if (offset >= sizeof(body) - 256) {
            break;
        }
    }

    offset += (size_t)snprintf(body + offset, sizeof(body) - offset, "</ul></body></html>");

    {
        char header[256];
        snprintf(header, sizeof(header),
                 "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %zu\r\n"
                 "Connection: close\r\n\r\n",
                 offset);
        http_send_all(client, header);
        http_send_all(client, body);
    }
}

static void http_reply_value(socket_t client, const char* key, int found)
{
    index_entry_t* entry = find_index(key);
    if (!found || entry == NULL || now_seconds() - entry->last_seen > INDEX_TTL_SEC) {
        http_send_all(client, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        return;
    }

    {
        char header[256];
        snprintf(header, sizeof(header),
                 "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %zu\r\n"
                 "Connection: close\r\n\r\n",
                 entry->value_len);
        http_send_all(client, header);
        {
            size_t sent = 0;
            while (sent < entry->value_len) {
                int n = send(client, entry->value + sent, (int)(entry->value_len - sent), 0);
                if (n <= 0) {
                    break;
                }
                sent += (size_t)n;
            }
        }
    }
}

static const char* json_find_value(const char* json, const char* key)
{
    char pattern[160];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    return strstr(json, pattern) ? strstr(json, pattern) + strlen(pattern) : NULL;
}

static int json_get_bool_field(const char* json, const char* key, int* out)
{
    const char* value = json_find_value(json, key);
    if (value == NULL || out == NULL) {
        return 0;
    }
    if (strncmp(value, "true", 4) == 0) {
        *out = 1;
        return 1;
    }
    if (strncmp(value, "false", 5) == 0) {
        *out = 0;
        return 1;
    }
    return 0;
}

static int json_get_int_field(const char* json, const char* key, int* out)
{
    const char* value = json_find_value(json, key);
    char* end = NULL;
    long parsed;

    if (value == NULL || out == NULL) {
        return 0;
    }

    parsed = strtol(value, &end, 10);
    if (end == value) {
        return 0;
    }

    *out = (int)parsed;
    return 1;
}

static int json_get_string_field(const char* json, const char* key, char* out, size_t out_size)
{
    const char* value = json_find_value(json, key);
    size_t offset = 0;

    if (value == NULL || out == NULL || out_size == 0 || *value != '"') {
        return 0;
    }

    ++value;
    while (*value != '\0' && *value != '"') {
        if (*value == '\\' && value[1] != '\0') {
            ++value;
        }
        if (offset + 1 < out_size) {
            out[offset++] = *value;
        }
        ++value;
    }

    if (*value != '"') {
        return 0;
    }

    out[offset] = '\0';
    return 1;
}

static int json_copy_array_field(const char* json, const char* key, char* out, size_t out_size)
{
    const char* value = json_find_value(json, key);
    size_t offset = 0;
    int depth = 0;
    int in_string = 0;

    if (value == NULL || out == NULL || out_size == 0 || *value != '[') {
        return 0;
    }

    while (*value != '\0') {
        if (offset + 1 >= out_size) {
            return 0;
        }

        out[offset++] = *value;

        if (in_string) {
            if (*value == '\\' && value[1] != '\0') {
                ++value;
                if (offset + 1 >= out_size) {
                    return 0;
                }
                out[offset++] = *value;
            } else if (*value == '"') {
                in_string = 0;
            }
        } else {
            if (*value == '"') {
                in_string = 1;
            } else if (*value == '[') {
                ++depth;
            } else if (*value == ']') {
                --depth;
                if (depth == 0) {
                    out[offset] = '\0';
                    return 1;
                }
            }
        }

        ++value;
    }

    return 0;
}

static int is_public_ipv4_text(const char* text)
{
    struct in_addr addr;
    uint32_t ip;
    uint8_t b0;
    uint8_t b1;

    if (text == NULL || text[0] == '\0' || inet_pton(AF_INET, text, &addr) != 1) {
        return 0;
    }

    ip = ntohl(addr.s_addr);
    b0 = (uint8_t)((ip >> 24) & 0xFF);
    b1 = (uint8_t)((ip >> 16) & 0xFF);
    if (b0 == 0 || b0 == 10 || b0 == 127) {
        return 0;
    }
    if (b0 == 192 && b1 == 168) {
        return 0;
    }
    if (b0 == 172 && b1 >= 16 && b1 <= 31) {
        return 0;
    }
    return 1;
}

static int room_index_is_browsable(const char* session_json)
{
    char game_name[128] = {0};
    char connect_addr[INET_ADDRSTRLEN] = {0};
    int show_in_browser = 1;
    int use_nat = 1;

    if (session_json == NULL) {
        return 0;
    }

    if (json_get_bool_field(session_json, "show_in_browser", &show_in_browser) && !show_in_browser) {
        return 0;
    }

    if (!json_get_string_field(session_json, "game_name", game_name, sizeof(game_name)) || game_name[0] == '\0') {
        return 0;
    }

    if (json_get_bool_field(session_json, "use_nat_traversal", &use_nat) && !use_nat) {
        if (!json_get_string_field(session_json, "connect_address", connect_addr, sizeof(connect_addr)) ||
            !is_public_ipv4_text(connect_addr)) {
            if (!json_get_string_field(session_json, "public_address", connect_addr, sizeof(connect_addr)) ||
                !is_public_ipv4_text(connect_addr)) {
                return 0;
            }
        }
    }

    return 1;
}

static void json_room_connect_endpoint(const char* session_json, const char* fallback_ip, uint16_t fallback_port,
                                       char* out_ip, size_t out_ip_size, uint16_t* out_port)
{
    int use_nat = 1;
    char connect_addr[INET_ADDRSTRLEN] = {0};
    int connect_port = 0;

    strncpy(out_ip, fallback_ip, out_ip_size - 1);
    out_ip[out_ip_size - 1] = '\0';
    *out_port = fallback_port;

    if (session_json == NULL) {
        return;
    }

    if (!json_get_bool_field(session_json, "use_nat_traversal", &use_nat) || use_nat) {
        return;
    }

    if (!json_get_string_field(session_json, "connect_address", connect_addr, sizeof(connect_addr)) ||
        connect_addr[0] == '\0') {
        if (!json_get_string_field(session_json, "public_address", connect_addr, sizeof(connect_addr)) ||
            connect_addr[0] == '\0') {
            return;
        }
    }

    strncpy(out_ip, connect_addr, out_ip_size - 1);
    out_ip[out_ip_size - 1] = '\0';

    if (json_get_int_field(session_json, "connect_port", &connect_port) && connect_port >= 1024 &&
        connect_port <= 65535) {
        *out_port = (uint16_t)connect_port;
    } else if (json_get_int_field(session_json, "public_port", &connect_port) && connect_port >= 1024 &&
               connect_port <= 65535) {
        *out_port = (uint16_t)connect_port;
    }
}

static void json_escape_string(const char* input, char* output, size_t output_size)
{
    size_t offset = 0;

    if (output_size == 0) {
        return;
    }

    for (const unsigned char* p = (const unsigned char*)input; p != NULL && *p != '\0'; ++p) {
        const char* replacement = NULL;
        char tmp[3] = {0, 0, 0};

        switch (*p) {
        case '"': replacement = "\\\""; break;
        case '\\': replacement = "\\\\"; break;
        case '\b': replacement = "\\b"; break;
        case '\f': replacement = "\\f"; break;
        case '\n': replacement = "\\n"; break;
        case '\r': replacement = "\\r"; break;
        case '\t': replacement = "\\t"; break;
        default:
            if (*p < 0x20) {
                snprintf(tmp, sizeof(tmp), "%c", *p);
                replacement = tmp;
            }
            break;
        }

        if (replacement != NULL) {
            size_t len = strlen(replacement);
            if (offset + len >= output_size) {
                break;
            }
            memcpy(output + offset, replacement, len);
            offset += len;
        } else {
            if (offset + 1 >= output_size) {
                break;
            }
            output[offset++] = (char)*p;
        }
    }

    output[offset] = '\0';
}

static int http_query_bool(const char* path, const char* key, int default_value)
{
    char pattern[64];
    const char* query;
    const char* value;

    snprintf(pattern, sizeof(pattern), "%s=", key);
    query = strchr(path, '?');
    if (query == NULL) {
        return default_value;
    }

    value = strstr(query + 1, pattern);
    if (value == NULL) {
        return default_value;
    }

    value += strlen(pattern);
    if (strncmp(value, "true", 4) == 0 || strncmp(value, "1", 1) == 0 || strncmp(value, "yes", 3) == 0) {
        return 1;
    }
    if (strncmp(value, "false", 5) == 0 || strncmp(value, "0", 1) == 0 || strncmp(value, "no", 2) == 0) {
        return 0;
    }

    return default_value;
}

static void http_reply_rooms_html(socket_t client)
{
    char body[65536];
    size_t offset = 0;
    const uint64_t now = now_seconds();

    offset += (size_t)snprintf(body + offset, sizeof(body) - offset,
                               "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
                               "<title>Mupen MPN Active Rooms</title></head><body><h1>Active Rooms</h1><ul>");

    for (int i = 0; i < g_host_count; ++i) {
        if (!g_hosts[i].in_use || now - g_hosts[i].last_seen > HOST_TTL_SEC || !g_hosts[i].list_in_browser) {
            continue;
        }

        char code_text[8];
        char ip_text[INET_ADDRSTRLEN];
        struct in_addr addr;

        if (!format_host_code(g_hosts[i].code, code_text, sizeof(code_text))) {
            continue;
        }

        addr.s_addr = g_hosts[i].host_ip;
        const char* ip_string = inet_ntop(AF_INET, &addr, ip_text, sizeof(ip_text));
        if (ip_string == NULL) {
            ip_string = "0.0.0.0";
        }

        offset += (size_t)snprintf(body + offset, sizeof(body) - offset,
                                   "<li><b>%s</b> - %s:%u (last seen %llu sec ago)</li>", code_text,
                                   ip_string, (unsigned)g_hosts[i].signaling_port,
                                   (unsigned long long)(now - g_hosts[i].last_seen));
        if (offset >= sizeof(body) - 256) {
            break;
        }
    }

    if (offset == 0) {
        offset += (size_t)snprintf(body + offset, sizeof(body) - offset, "<li>No active rooms</li>");
    }

    offset += (size_t)snprintf(body + offset, sizeof(body) - offset, "</ul></body></html>");

    {
        char header[256];
        snprintf(header, sizeof(header),
                 "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %zu\r\n"
                 "Connection: close\r\n\r\n",
                 offset);
        http_send_all(client, header);
        http_send_all(client, body);
    }
}

static void http_reply_rooms_json(socket_t client, int waitingOnly)
{
    char body[65536];
    size_t offset = 0;
    const uint64_t now = now_seconds();
    int wrote_room = 0;

    body[0] = '\0';
    offset += (size_t)snprintf(body + offset, sizeof(body) - offset, "{\"rooms\":[");

    for (int i = 0; i < g_host_count; ++i) {
        if (!g_hosts[i].in_use || now - g_hosts[i].last_seen > HOST_TTL_SEC || !g_hosts[i].list_in_browser) {
            continue;
        }

        char code_text[8];
        char ip_text[INET_ADDRSTRLEN];
        char key[MAX_KEY_LEN];
        char host_name[128] = {0};
        char game_name[128] = {0};
        char players_json[MAX_VALUE_LEN] = {0};
        char host_name_json[256];
        char game_name_json[256];
        char lobby_size[32];
        char* session_json = NULL;
        struct in_addr addr;
        int started = 0;
        int player_count = 1;
        int max_players = 4;

        if (!format_host_code(g_hosts[i].code, code_text, sizeof(code_text))) {
            continue;
        }

        snprintf(key, sizeof(key), "session/%s", code_text);
        {
            index_entry_t* entry = find_index(key);
            if (entry != NULL && now - entry->last_seen <= INDEX_TTL_SEC) {
                session_json = entry->value;
                json_get_bool_field(session_json, "started", &started);
                json_get_int_field(session_json, "player_count", &player_count);
                json_get_int_field(session_json, "max_players", &max_players);
                if (!json_get_string_field(session_json, "host_name", host_name, sizeof(host_name))) {
                    if (!json_get_string_field(session_json, "player_name", host_name, sizeof(host_name))) {
                        if (!json_get_string_field(session_json, "room_name", host_name, sizeof(host_name))) {
                            strncpy(host_name, code_text, sizeof(host_name) - 1);
                        }
                    }
                }
                if (!json_get_string_field(session_json, "game_name", game_name, sizeof(game_name))) {
                    strncpy(game_name, "", sizeof(game_name) - 1);
                }
                if (!json_copy_array_field(session_json, "players", players_json, sizeof(players_json))) {
                    char escaped_host[256];
                    json_escape_string(host_name, escaped_host, sizeof(escaped_host));
                    snprintf(players_json, sizeof(players_json), "[{\"name\":\"%s\",\"slotIndex\":0}]", escaped_host);
                }
            }
        }

        if (!room_index_is_browsable(session_json)) {
            continue;
        }

        if (waitingOnly) {
            if (started) {
                continue;
            }
        } else {
            if (!started) {
                continue;
            }
        }

        if (host_name[0] == '\0') {
            strncpy(host_name, code_text, sizeof(host_name) - 1);
        }

        json_escape_string(host_name, host_name_json, sizeof(host_name_json));
        json_escape_string(game_name, game_name_json, sizeof(game_name_json));
        snprintf(lobby_size, sizeof(lobby_size), "%d/%d", player_count, max_players > 0 ? max_players : 4);

        /* Ensure players_json is valid JSON even if no session/index entry exists */
        if (players_json[0] == '\0') {
            strncpy(players_json, "[]", sizeof(players_json) - 1);
            players_json[sizeof(players_json) - 1] = '\0';
        }

        addr.s_addr = g_hosts[i].host_ip;
        if (inet_ntop(AF_INET, &addr, ip_text, sizeof(ip_text)) == NULL) {
            strncpy(ip_text, "0.0.0.0", sizeof(ip_text) - 1);
        }

        {
            char listing_ip[INET_ADDRSTRLEN];
            uint16_t listing_port = g_hosts[i].signaling_port;
            json_room_connect_endpoint(session_json, ip_text, g_hosts[i].signaling_port, listing_ip,
                                       sizeof(listing_ip), &listing_port);
            strncpy(ip_text, listing_ip, sizeof(ip_text) - 1);
            ip_text[sizeof(ip_text) - 1] = '\0';

            if (wrote_room) {
                if (offset + 1 < sizeof(body)) {
                    body[offset++] = ',';
                    body[offset] = '\0';
                }
            }

            offset += (size_t)snprintf(body + offset, sizeof(body) - offset,
                                       "{\"hostCode\":\"%s\",\"hostName\":\"%s\",\"gameName\":\"%s\","
                                       "\"playerCount\":%d,\"maxPlayers\":%d,\"lobbySize\":\"%s\","
                                       "\"started\":%s,\"players\":%s,\"address\":\"%s\",\"port\":%u}",
                                       code_text, host_name_json, game_name_json, player_count,
                                       (max_players > 0 ? max_players : 4), lobby_size,
                                       started ? "true" : "false", players_json, ip_text,
                                       (unsigned)listing_port);
        }
        wrote_room = 1;

        if (offset >= sizeof(body) - 256) {
            break;
        }
    }

    offset += (size_t)snprintf(body + offset, sizeof(body) - offset, "]}");

    {
        char header[256];
        snprintf(header, sizeof(header),
                 "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: %zu\r\n"
                 "Connection: close\r\n\r\n",
                 offset);
        http_send_all(client, header);
        http_send_all(client, body);
    }
}

#ifdef WITH_LIBCURL

typedef struct {
    char* data;
    size_t size;
    size_t capacity;
} http_buffer_t;

static size_t turn_curl_write_cb(void* contents, size_t size, size_t nmemb, void* userp)
{
    size_t total = size * nmemb;
    http_buffer_t* buffer = (http_buffer_t*)userp;
    size_t needed = buffer->size + total + 1;

    if (needed > buffer->capacity) {
        size_t new_capacity = buffer->capacity == 0 ? 4096 : buffer->capacity;
        while (new_capacity < needed) {
            new_capacity *= 2;
        }
        if (new_capacity > MAX_TURN_RESPONSE) {
            return 0;
        }
        {
            char* resized = (char*)realloc(buffer->data, new_capacity);
            if (resized == NULL) {
                return 0;
            }
            buffer->data = resized;
            buffer->capacity = new_capacity;
        }
    }

    memcpy(buffer->data + buffer->size, contents, total);
    buffer->size += total;
    buffer->data[buffer->size] = '\0';
    return total;
}

static void turn_load_config_from_env(void)
{
    const char* ttl = getenv("RMG_TURN_CREDENTIAL_TTL");
    if (ttl != NULL && ttl[0] != '\0') {
        int parsed = atoi(ttl);
        if (parsed > 0) {
            g_turn_credential_ttl_sec = parsed;
            return;
        }
    }

#if RMG_TURN_CREDENTIAL_TTL_EMBED > 0
    g_turn_credential_ttl_sec = RMG_TURN_CREDENTIAL_TTL_EMBED;
#endif
}

static const char* turn_key_id_config(void)
{
    const char* env = getenv("RMG_TURN_KEY_ID");
    if (env != NULL && env[0] != '\0') {
        return env;
    }
    if (RMG_TURN_KEY_ID_EMBED[0] != '\0') {
        return RMG_TURN_KEY_ID_EMBED;
    }
    return NULL;
}

static const char* turn_api_token_config(void)
{
    const char* env = getenv("RMG_TURN_API_TOKEN");
    if (env != NULL && env[0] != '\0') {
        return env;
    }
    if (RMG_TURN_API_TOKEN_EMBED[0] != '\0') {
        return RMG_TURN_API_TOKEN_EMBED;
    }
    return NULL;
}

static int turn_is_configured(void)
{
    const char* turn_key_id = turn_key_id_config();
    const char* api_token = turn_api_token_config();
    return turn_key_id != NULL && turn_key_id[0] != '\0' && api_token != NULL && api_token[0] != '\0';
}

static int turn_fetch_cloudflare_credentials(char* out, size_t out_size, size_t* out_len)
{
    const char* turn_key_id = turn_key_id_config();
    const char* api_token = turn_api_token_config();
    CURL* curl = NULL;
    struct curl_slist* headers = NULL;
    http_buffer_t response = {0};
    char url[512];
    char auth_header[1024];
    char body[64];
    long http_code = 0;
    int success = 0;

    if (!turn_is_configured()) {
        return 0;
    }

    turn_load_config_from_env();

    snprintf(url, sizeof(url),
             "https://rtc.live.cloudflare.com/v1/turn/keys/%s/credentials/generate-ice-servers", turn_key_id);
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_token);
    snprintf(body, sizeof(body), "{\"ttl\":%d}", g_turn_credential_ttl_sec);

    curl = curl_easy_init();
    if (curl == NULL) {
        return 0;
    }

    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth_header);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, turn_curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "RMG-NAT-Traversal-Server/1.0");

    CURLcode result = curl_easy_perform(curl);
    if (result == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code == 201 && response.data != NULL && response.size > 0 && strstr(response.data, "\"iceServers\"") != NULL &&
            response.size < out_size) {
            memcpy(out, response.data, response.size);
            out[response.size] = '\0';
            *out_len = response.size;
            success = 1;
        } else {
            fprintf(stderr, "[turn] Cloudflare credential request failed (HTTP %ld)\n", http_code);
        }
    } else {
        fprintf(stderr, "[turn] Cloudflare credential request failed: %s\n", curl_easy_strerror(result));
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(response.data);
    return success;
}

static int turn_credentials_are_fresh(uint64_t now)
{
    return g_turn_cache_len > 0 && g_turn_cache_expires > now + TURN_CACHE_REFRESH_SKEW_SEC;
}

static int turn_refresh_credentials(uint64_t now)
{
    size_t fetched_len = 0;

    if (!turn_fetch_cloudflare_credentials(g_turn_cache, sizeof(g_turn_cache), &fetched_len)) {
        return 0;
    }

    g_turn_cache_len = fetched_len;
    g_turn_cache_expires = now + (uint64_t)g_turn_credential_ttl_sec;
    printf("[turn] refreshed Cloudflare ICE credentials (%zu bytes, ttl %d sec)\n", g_turn_cache_len,
           g_turn_credential_ttl_sec);
    return 1;
}

#endif /* WITH_LIBCURL */

static void http_reply_turn_ice_servers(socket_t client)
{
#ifdef WITH_LIBCURL
    const uint64_t now = now_seconds();

    if (!turn_is_configured()) {
        http_reply_json(client, 503, "Service Unavailable",
                        "{\"error\":\"Cloudflare TURN is not configured on this server\"}");
        return;
    }

    if (!turn_credentials_are_fresh(now) && !turn_refresh_credentials(now)) {
        if (g_turn_cache_len > 0) {
            /* Serve stale credentials if refresh failed but we still have a cache. */
        } else {
            http_reply_json(client, 502, "Bad Gateway",
                            "{\"error\":\"Failed to fetch Cloudflare TURN credentials\"}");
            return;
        }
    }

    {
        char header[256];
        snprintf(header, sizeof(header),
                 "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: %zu\r\n"
                 "Connection: close\r\n\r\n",
                 g_turn_cache_len);
        http_send_all(client, header);
        {
            size_t sent = 0;
            while (sent < g_turn_cache_len) {
                int n = send(client, g_turn_cache + sent, (int)(g_turn_cache_len - sent), 0);
                if (n <= 0) {
                    break;
                }
                sent += (size_t)n;
            }
        }
    }
#else
    http_reply_json(client, 503, "Service Unavailable",
                    "{\"error\":\"TURN broker requires server built with libcurl\"}");
#endif
}

static void handle_http_client(socket_t client)
{
    char request[MAX_HTTP_REQUEST];
    char* body = NULL;
    size_t body_len = 0;
    const char* path;
    char key[MAX_KEY_LEN];

    if (http_recv_request(client, request, sizeof(request), &body, &body_len) <= 0) {
        return;
    }

    if (strncmp(request, "GET ", 4) != 0 && strncmp(request, "PUT ", 4) != 0 && strncmp(request, "POST ", 5) != 0) {
        http_send_all(client, "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        return;
    }

    path = strchr(request, ' ');
    if (path == NULL) {
        return;
    }
    ++path;
    {
        char* end = strchr(path, ' ');
        if (end != NULL) {
            *end = '\0';
        }
    }

    prune_index();

    if (strcmp(path, "/") == 0 || strcmp(path, "/index") == 0 || strcmp(path, "/index/") == 0) {
        http_reply_index_page(client);
        return;
    }

    if (strcmp(path, "/rooms.html") == 0 || strcmp(path, "/rooms/page") == 0) {
        http_reply_rooms_html(client);
        return;
    }

    if (strncmp(path, "/rooms", 6) == 0) {
        /* Show waiting (lobby) rooms by default to match client expectations. */
        http_reply_rooms_json(client, http_query_bool(path, "waiting", 1));
        return;
    }

    if (strcmp(path, "/turn/ice-servers") == 0) {
        if (strncmp(request, "GET ", 4) != 0) {
            http_send_all(client, "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            return;
        }
        http_reply_turn_ice_servers(client);
        return;
    }

    if (strcmp(path, "/debug_index") == 0) {
        /* Return raw index entries for debugging */
        char body[131072];
        size_t offset = 0;
        const uint64_t now = now_seconds();

        offset += (size_t)snprintf(body + offset, sizeof(body) - offset, "{\"index\":[");
        int wrote = 0;
        for (int i = 0; i < g_index_count; ++i) {
            if (!g_index[i].in_use || now - g_index[i].last_seen > INDEX_TTL_SEC) {
                continue;
            }
            char b64[MAX_VALUE_LEN * 2];
            b64_encode((const unsigned char*)g_index[i].value, g_index[i].value_len, b64, sizeof(b64));
            if (wrote) {
                offset += (size_t)snprintf(body + offset, sizeof(body) - offset, ",");
            }
            offset += (size_t)snprintf(body + offset, sizeof(body) - offset,
                                       "{\"key\":\"%s\",\"last_seen\":%llu,\"value_b64\":\"%s\"}",
                                       g_index[i].key, (unsigned long long)g_index[i].last_seen, b64);
            wrote = 1;
            if (offset >= sizeof(body) - 1024) break;
        }
        offset += (size_t)snprintf(body + offset, sizeof(body) - offset, "]");

        /* Append hosts list for easier debugging */
        offset += (size_t)snprintf(body + offset, sizeof(body) - offset, ",\"hosts\":[");
        int wrote_host = 0;
        for (int i = 0; i < g_host_count; ++i) {
            if (!g_hosts[i].in_use) continue;
            if (now - g_hosts[i].last_seen > HOST_TTL_SEC) continue;
            char code_text[8];
            char ip_text[INET_ADDRSTRLEN];
            struct in_addr addr;
            if (!format_host_code(g_hosts[i].code, code_text, sizeof(code_text))) {
                continue;
            }
            addr.s_addr = g_hosts[i].host_ip;
            if (inet_ntop(AF_INET, &addr, ip_text, sizeof(ip_text)) == NULL) {
                strncpy(ip_text, "0.0.0.0", sizeof(ip_text) - 1);
            }
            if (wrote_host) {
                offset += (size_t)snprintf(body + offset, sizeof(body) - offset, ",");
            }
            offset += (size_t)snprintf(body + offset, sizeof(body) - offset,
                                       "{\"hostCode\":\"%s\",\"address\":\"%s\",\"port\":%u,\"last_seen\":%llu}",
                                       code_text, ip_text, (unsigned)g_hosts[i].signaling_port,
                                       (unsigned long long)g_hosts[i].last_seen);
            wrote_host = 1;
            if (offset >= sizeof(body) - 256) break;
        }
        offset += (size_t)snprintf(body + offset, sizeof(body) - offset, "]}");

        char header[256];
        snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", offset);
        http_send_all(client, header);
        http_send_all(client, body);
        return;
    }

    if (strncmp(path, "/index/", 7) == 0) {
        strncpy(key, path + 7, MAX_KEY_LEN - 1);
        key[MAX_KEY_LEN - 1] = '\0';
        {
            char* q = strchr(key, '?');
            if (q != NULL) {
                *q = '\0';
            }
        }

        if (!is_valid_key(key)) {
            http_send_all(client, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            return;
        }

        if (strncmp(request, "GET ", 4) == 0) {
            http_reply_value(client, key, 1);
            return;
        }

        if (body != NULL && body_len > 0) {
            if (body_len >= MAX_VALUE_LEN) {
                http_send_all(client,
                              "HTTP/1.1 413 Payload Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                printf("[http] PUT /index/%s rejected: %zu bytes (max %d)\n", key, body_len, MAX_VALUE_LEN - 1);
                return;
            }
            if (upsert_index(key, (const unsigned char*)body, body_len, now_seconds()) != NULL) {
                http_send_all(client, "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n");
                printf("[http] PUT /index/%s (%zu bytes)\n", key, body_len);
                return;
            }
            printf("[http] PUT /index/%s failed: index store full\n", key);
        } else {
            printf("[http] PUT /index/%s failed: empty body (got %zu bytes)\n", key, body_len);
        }

        http_send_all(client, "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        return;
    }

    http_send_all(client, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
}

#ifdef _WIN32
static int net_init(void)
{
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0 ? 0 : -1;
}

static void net_shutdown(void)
{
    WSACleanup();
}
#else
static int net_init(void)
{
    return 0;
}

static void net_shutdown(void)
{
}
#endif

int main(int argc, char** argv)
{
    const char* bind_host = "0.0.0.0";
    int udp_port = DEFAULT_UDP_PORT;
    int http_port = DEFAULT_HTTP_PORT;
    int http_enabled = 1;
    socket_t udp_sock;
    socket_t http_sock = SOCKET_INVALID;
    struct sockaddr_in udp_addr;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            udp_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--http-port") == 0 && i + 1 < argc) {
            http_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            bind_host = argv[++i];
        } else if (strcmp(argv[i], "--no-http") == 0) {
            http_enabled = 0;
        }
    }

    srand((unsigned)time(NULL));

    if (net_init() != 0) {
        fprintf(stderr, "network init failed\n");
        return 1;
    }

    udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_sock == SOCKET_INVALID) {
        fprintf(stderr, "udp socket() failed\n");
        net_shutdown();
        return 1;
    }

    memset(&udp_addr, 0, sizeof(udp_addr));
    udp_addr.sin_family = AF_INET;
    udp_addr.sin_port = htons((uint16_t)udp_port);
    if (inet_pton(AF_INET, bind_host, &udp_addr.sin_addr) != 1) {
        fprintf(stderr, "invalid bind host\n");
        socket_close(udp_sock);
        net_shutdown();
        return 1;
    }

    if (bind(udp_sock, (struct sockaddr*)&udp_addr, sizeof(udp_addr)) != 0) {
        fprintf(stderr, "udp bind failed on %s:%d\n", bind_host, udp_port);
        socket_close(udp_sock);
        net_shutdown();
        return 1;
    }

    if (http_enabled) {
        struct sockaddr_in http_addr;
        http_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (http_sock != SOCKET_INVALID) {
            int opt = 1;
            setsockopt(http_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
            memset(&http_addr, 0, sizeof(http_addr));
            http_addr.sin_family = AF_INET;
            http_addr.sin_port = htons((uint16_t)http_port);
            inet_pton(AF_INET, bind_host, &http_addr.sin_addr);
            if (bind(http_sock, (struct sockaddr*)&http_addr, sizeof(http_addr)) == 0 &&
                listen(http_sock, 8) == 0) {
                printf("HTTP index on http://%s:%d/\n", bind_host, http_port);
            } else {
                fprintf(stderr, "http bind failed, continuing without HTTP\n");
                socket_close(http_sock);
                http_sock = SOCKET_INVALID;
            }
        }
    }

#ifdef WITH_LIBCURL
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        fprintf(stderr, "warning: libcurl init failed; /turn/ice-servers will be unavailable\n");
    }
#endif

    printf("UDP NAT on %s:%d (%s + %s)\n", bind_host, udp_port, TRAV_PROTOCOL, INDEX_PROTOCOL);

    for (;;) {
        fd_set readfds;
        socket_t max_fd = udp_sock;

        FD_ZERO(&readfds);
        FD_SET(udp_sock, &readfds);
        if (http_sock != SOCKET_INVALID) {
            FD_SET(http_sock, &readfds);
            if (http_sock > max_fd) {
                max_fd = http_sock;
            }
        }

        if (select((int)max_fd + 1, &readfds, NULL, NULL, NULL) <= 0) {
            continue;
        }

        if (FD_ISSET(udp_sock, &readfds)) {
            char buffer[MAX_UDP_PACKET];
            struct sockaddr_in client;
            socklen_t client_len = sizeof(client);
            int received =
                recvfrom(udp_sock, buffer, sizeof(buffer) - 1, 0, (struct sockaddr*)&client, &client_len);
            if (received > 0) {
                handle_udp(udp_sock, buffer, received, &client, client_len);
            }
        }

        if (http_sock != SOCKET_INVALID && FD_ISSET(http_sock, &readfds)) {
            struct sockaddr_in client;
            socklen_t client_len = sizeof(client);
            socket_t client_sock = accept(http_sock, (struct sockaddr*)&client, &client_len);
            if (client_sock != SOCKET_INVALID) {
                char peer_ip[INET_ADDRSTRLEN];
                if (inet_ntop(AF_INET, &client.sin_addr, peer_ip, sizeof(peer_ip)) == NULL) {
                    strncpy(peer_ip, "0.0.0.0", sizeof(peer_ip));
                }
                printf("[http] connection from %s:%u\n", peer_ip, (unsigned)ntohs(client.sin_port));
                handle_http_client(client_sock);
                socket_close(client_sock);
            }
        }
    }

    socket_close(udp_sock);
    if (http_sock != SOCKET_INVALID) {
        socket_close(http_sock);
    }
    net_shutdown();
    return 0;
}
