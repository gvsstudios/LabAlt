/**
 * llm_client.ino
 * OpenRouter LLM client - ported from llm_client.cpp
 * Classic ESP32 compatible. No changes needed from original
 * except removing PlatformIO includes.
 */

#include "config.h"
#include <LittleFS.h>  /* ensure File/LittleFS visible in C-mode compile */
// ============================================================
// LlmMessage constructor helpers
// (regular functions - older compilers dislike inline + struct
//  return values combined with NULL pointer members)
// ============================================================

LlmMessage llmMsg(const char *role, const char *content) {
    LlmMessage m;
    m.type = LLM_MSG_NORMAL;
    m.role = role;
    m.content = content;
    m.tool_call_id = NULL;
    m.tool_calls_json = NULL;
    return m;
}

LlmMessage llmToolResult(const char *tool_call_id, const char *content) {
    LlmMessage m;
    m.type = LLM_MSG_TOOL_RESULT;
    m.role = "tool";
    m.content = content;
    m.tool_call_id = tool_call_id;
    m.tool_calls_json = NULL;
    return m;
}

LlmMessage llmToolCallMsg(const char *content, const char *tool_calls_json) {
    LlmMessage m;
    m.type = LLM_MSG_TOOL_CALL;
    m.role = "assistant";
    m.content = content;
    m.tool_call_id = NULL;
    m.tool_calls_json = tool_calls_json;
    return m;
}

// ============================================================
// Portable memmem replacement (memmem is a GNU extension and
// may not be available on all toolchains)
// ============================================================
static const void *wc_memmem(const void *haystack, size_t haystacklen,
                              const void *needle, size_t needlelen) {
    if (needlelen == 0) return haystack;
    if (haystacklen < needlelen) return NULL;
    const char *h = (const char *)haystack;
    const char *n = (const char *)needle;
    size_t last = haystacklen - needlelen;
    for (size_t i = 0; i <= last; i++) {
        if (h[i] == n[0] && memcmp(h + i, n, needlelen) == 0)
            return h + i;
    }
    return NULL;
}

// ============================================================
// JSON helpers (file-scope, no header needed in .ino tabs)
// ============================================================

static int llm_json_escape(char *dst, int dst_len, const char *src) {
    int w = 0;
    for (int i = 0; src[i] != '\0'; i++) {
        char c = src[i];
        const char *esc = NULL;
        switch (c) {
            case '\\': esc = "\\\\"; break;
            case '"':  esc = "\\\""; break;
            case '\n': esc = "\\n";  break;
            case '\r': esc = "\\r";  break;
            case '\t': esc = "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) continue;
                if (w + 1 >= dst_len) return -1;
                dst[w++] = c;
                continue;
        }
        int elen = strlen(esc);
        if (w + elen >= dst_len) return -1;
        memcpy(dst + w, esc, elen);
        w += elen;
    }
    if (w >= dst_len) return -1;
    dst[w] = '\0';
    return w;
}

static const char *llm_json_find_string(const char *json, int json_len,
                                         const char *key, int *out_len) {
    char pattern[128];
    int plen = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (plen < 0 || plen >= (int)sizeof(pattern)) return NULL;

    const char *end = json + json_len;
    const char *p = json;

    while (p < end - plen) {
        const char *found = (const char *)wc_memmem(p, end - p, pattern, plen);
        if (!found) return NULL;

        const char *after_key = found + plen;
        while (after_key < end && (*after_key == ' ' || *after_key == ':'))
            after_key++;

        if (after_key >= end || *after_key != '"') {
            p = after_key;
            continue;
        }

        const char *val_start = after_key + 1;
        const char *q = val_start;
        while (q < end) {
            if (*q == '\\' && q + 1 < end) { q += 2; continue; }
            if (*q == '"') break;
            q++;
        }
        *out_len = q - val_start;
        return val_start;
    }
    return NULL;
}

static int llm_json_find_int(const char *json, int json_len,
                              const char *key, int default_val) {
    char pattern[128];
    int plen = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (plen < 0 || plen >= (int)sizeof(pattern)) return default_val;

    const char *found = (const char *)wc_memmem(json, json_len, pattern, plen);
    if (!found) return default_val;

    const char *after_key = found + plen;
    const char *end = json + json_len;
    while (after_key < end && (*after_key == ' ' || *after_key == ':'))
        after_key++;
    if (after_key >= end) return default_val;

    return atoi(after_key);
}

static int llm_json_unescape(char *buf, int len) {
    int r = 0, w = 0;
    while (r < len) {
        if (buf[r] == '\\' && r + 1 < len) {
            r++;
            switch (buf[r]) {
                case 'n':  buf[w++] = '\n'; break;
                case 'r':  buf[w++] = '\r'; break;
                case 't':  buf[w++] = '\t'; break;
                case '\\': buf[w++] = '\\'; break;
                case '"':  buf[w++] = '"';  break;
                case '/':  buf[w++] = '/';  break;
                default:   buf[w++] = buf[r]; break;
            }
            r++;
        } else {
            buf[w++] = buf[r++];
        }
    }
    buf[w] = '\0';
    return w;
}

static const char *llm_json_skip_value(const char *p, const char *end) {
    if (p >= end) return NULL;
    if (*p == '"') {
        p++;
        while (p < end) {
            if (*p == '\\' && p + 1 < end) { p += 2; continue; }
            if (*p == '"') return p + 1;
            p++;
        }
        return NULL;
    }
    if (*p == '{' || *p == '[') {
        char open = *p;
        char close = (open == '{') ? '}' : ']';
        int depth = 1;
        p++;
        while (p < end && depth > 0) {
            if (*p == '"') {
                p = llm_json_skip_value(p, end);
                if (!p) return NULL;
                continue;
            }
            if (*p == open) depth++;
            else if (*p == close) depth--;
            p++;
        }
        return (depth == 0) ? p : NULL;
    }
    while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != '\n')
        p++;
    return p;
}

// ============================================================
// LlmClient class implementation
// ============================================================

// Static members
// FIX: In ESP32 Arduino core 3.x, WiFiClientSecure is NetworkClientSecure which
// does not directly inherit Client in a way the compiler accepts for raw pointer
// assignment.  We store the objects and derive BOTH a Client* (for connect/write)
// AND a Stream* (for readStringUntil / readBytes) via explicit casts.
static WiFiClientSecure _llm_secure_client;
static WiFiClient       _llm_plain_client;
static Client          *_llm_client = NULL;   // used for connect / write / stop
static Stream          *_llm_stream = NULL;   // used for readStringUntil / readBytes
static const char      *_llm_api_key = NULL;
static const char      *_llm_model = NULL;
static char             _llm_host[64];
static int              _llm_port = 443;
static char             _llm_path[64];
static bool             _llm_use_tls = true;
static char             _llm_error[128];

void llmBegin(const char *api_key, const char *model, const char *base_url) {
    _llm_api_key = api_key;
    _llm_model   = model;

    if (base_url && base_url[0]) {
        const char *p = base_url;
        if (strncmp(p, "https://", 8) == 0) {
            _llm_use_tls = true; _llm_port = 443; p += 8;
        } else if (strncmp(p, "http://", 7) == 0) {
            _llm_use_tls = false; _llm_port = 80; p += 7;
        } else {
            _llm_use_tls = true; _llm_port = 443;
        }
        const char *slash = strchr(p, '/');
        const char *colon = strchr(p, ':');
        if (colon && (!slash || colon < slash)) {
            int hlen = colon - p;
            if (hlen >= (int)sizeof(_llm_host)) hlen = sizeof(_llm_host) - 1;
            memcpy(_llm_host, p, hlen); _llm_host[hlen] = '\0';
            _llm_port = atoi(colon + 1);
            if (slash) strncpy(_llm_path, slash, sizeof(_llm_path) - 1);
            else strncpy(_llm_path, "/", sizeof(_llm_path));
        } else if (slash) {
            int hlen = slash - p;
            if (hlen >= (int)sizeof(_llm_host)) hlen = sizeof(_llm_host) - 1;
            memcpy(_llm_host, p, hlen); _llm_host[hlen] = '\0';
            strncpy(_llm_path, slash, sizeof(_llm_path) - 1);
        } else {
            strncpy(_llm_host, p, sizeof(_llm_host) - 1);
            strncpy(_llm_path, "/", sizeof(_llm_path));
        }
    } else {
        _llm_use_tls = true;
        strncpy(_llm_host, "openrouter.ai", sizeof(_llm_host));
        _llm_port = 443;
        strncpy(_llm_path, "/api/v1/chat/completions", sizeof(_llm_path));
    }

    if (_llm_use_tls) {
        _llm_secure_client.setInsecure();
        _llm_secure_client.setTimeout(LLM_READ_TIMEOUT_MS / 1000);
        // FIX: explicit casts for ESP32 core 3.x where the inheritance chain
        // is not directly compatible with raw pointer assignment.
        /* FIX: reinterpret_cast is C++ only. Use C-style cast which works in
           both C and C++ compilation modes used by ArduinoDroid.
           The cast is safe: WiFiClientSecure IS-A Client IS-A Stream. */
        _llm_client = (Client*)(&_llm_secure_client);
        _llm_stream = (Stream*)(&_llm_secure_client);
    } else {
        _llm_plain_client.setTimeout(LLM_READ_TIMEOUT_MS / 1000);
        _llm_client = (Client*)(&_llm_plain_client);
        _llm_stream = (Stream*)(&_llm_plain_client);
    }

    Serial.printf("LLM: %s://%s:%d%s\n",
                  _llm_use_tls ? "https" : "http",
                  _llm_host, _llm_port, _llm_path);
}

const char *llmLastError() { return _llm_error; }

static int llmBuildRequest(char *buf, int buf_len,
                            const LlmMessage *messages, int count,
                            const char *tools_json) {
    int w = 0;
    w += snprintf(buf + w, buf_len - w,
        "{\"model\":\"%s\",\"messages\":[", _llm_model);
    if (w >= buf_len) return -1;

    for (int i = 0; i < count; i++) {
        if (i > 0) { if (w + 1 >= buf_len) return -1; buf[w++] = ','; }

        const LlmMessage *msg = &messages[i];

        if (msg->type == LLM_MSG_TOOL_CALL) {
            w += snprintf(buf + w, buf_len - w, "{\"role\":\"assistant\"");
            if (w >= buf_len) return -1;
            if (msg->content && msg->content[0]) {
                w += snprintf(buf + w, buf_len - w, ",\"content\":\"");
                int esc = llm_json_escape(buf + w, buf_len - w, msg->content);
                if (esc < 0) return -1;
                w += esc;
                w += snprintf(buf + w, buf_len - w, "\"");
            } else {
                w += snprintf(buf + w, buf_len - w, ",\"content\":null");
            }
            if (msg->tool_calls_json)
                w += snprintf(buf + w, buf_len - w,
                    ",\"tool_calls\":%s", msg->tool_calls_json);
            if (w >= buf_len) return -1;
            w += snprintf(buf + w, buf_len - w, "}");

        } else if (msg->type == LLM_MSG_TOOL_RESULT) {
            w += snprintf(buf + w, buf_len - w,
                "{\"role\":\"tool\",\"tool_call_id\":\"%s\",\"content\":\"",
                msg->tool_call_id ? msg->tool_call_id : "");
            if (w >= buf_len) return -1;
            int esc = llm_json_escape(buf + w, buf_len - w,
                                      msg->content ? msg->content : "");
            if (esc < 0) return -1;
            w += esc;
            w += snprintf(buf + w, buf_len - w, "\"}");

        } else {
            w += snprintf(buf + w, buf_len - w,
                "{\"role\":\"%s\",\"content\":\"", msg->role);
            if (w >= buf_len) return -1;
            int esc = llm_json_escape(buf + w, buf_len - w,
                                      msg->content ? msg->content : "");
            if (esc < 0) return -1;
            w += esc;
            w += snprintf(buf + w, buf_len - w, "\"}");
        }
        if (w >= buf_len) return -1;
    }

    w += snprintf(buf + w, buf_len - w, "]");
    if (w >= buf_len) return -1;

    if (tools_json && tools_json[0]) {
        w += snprintf(buf + w, buf_len - w,
            ",\"tools\":%s,\"tool_choice\":\"auto\"", tools_json);
        if (w >= buf_len) return -1;
    }

    w += snprintf(buf + w, buf_len - w,
        ",\"max_tokens\":2048,\"temperature\":0.7}");
    if (w >= buf_len) return -1;
    return w;
}

static int llmParseToolCalls(const char *body, int body_len, LlmResult *result) {
    result->tool_call_count = 0;
    result->tool_calls_json[0] = '\0';

    const char *tc_key = "\"tool_calls\"";
    const char *found = (const char *)wc_memmem(body, body_len, tc_key, strlen(tc_key));
    if (!found) return 0;

    const char *end = body + body_len;
    const char *p = found + strlen(tc_key);
    while (p < end && *p != '[') p++;
    if (p >= end) return 0;

    const char *arr_start = p;
    const char *arr_end = llm_json_skip_value(p, end);
    if (!arr_end) return 0;

    int tc_json_len = arr_end - arr_start;
    if (tc_json_len < (int)sizeof(result->tool_calls_json) - 1) {
        memcpy(result->tool_calls_json, arr_start, tc_json_len);
        result->tool_calls_json[tc_json_len] = '\0';
    }

    p = arr_start + 1;
    int count = 0;

    while (p < arr_end && count < LLM_MAX_TOOL_CALLS) {
        while (p < arr_end && *p != '{') p++;
        if (p >= arr_end) break;

        const char *obj_start = p;
        const char *obj_end = llm_json_skip_value(p, arr_end);
        if (!obj_end) break;

        int obj_len = obj_end - obj_start;
        LlmToolCall *tc = &result->tool_calls[count];

        int id_len = 0;
        const char *id = llm_json_find_string(obj_start, obj_len, "id", &id_len);
        if (id && id_len > 0) {
            int clen = id_len < (int)sizeof(tc->id) - 1 ? id_len : (int)sizeof(tc->id) - 1;
            memcpy(tc->id, id, clen); tc->id[clen] = '\0';
        } else { tc->id[0] = '\0'; }

        int name_len = 0;
        const char *name = llm_json_find_string(obj_start, obj_len, "name", &name_len);
        if (name && name_len > 0) {
            int clen = name_len < (int)sizeof(tc->name) - 1 ? name_len : (int)sizeof(tc->name) - 1;
            memcpy(tc->name, name, clen); tc->name[clen] = '\0';
        } else { tc->name[0] = '\0'; }

        int args_len = 0;
        const char *args = llm_json_find_string(obj_start, obj_len, "arguments", &args_len);
        if (args && args_len > 0) {
            int clen = args_len < (int)sizeof(tc->arguments) - 1 ? args_len : (int)sizeof(tc->arguments) - 1;
            memcpy(tc->arguments, args, clen); tc->arguments[clen] = '\0';
            llm_json_unescape(tc->arguments, clen);
        } else { tc->arguments[0] = '\0'; }

        count++;
        p = obj_end;
    }

    result->tool_call_count = count;
    return count;
}

static bool llmParseResponse(const char *body, int body_len, LlmResult *result) {
    result->ok = false;
    result->content[0] = '\0';
    result->content_len = 0;
    result->prompt_tokens = 0;
    result->completion_tokens = 0;
    result->tool_call_count = 0;
    result->tool_calls_json[0] = '\0';

    int tc_count = llmParseToolCalls(body, body_len, result);

    int clen = 0;
    const char *content = llm_json_find_string(body, body_len, "content", &clen);
    if (content && clen > 0) {
        int copy_len = clen < LLM_MAX_RESPONSE_LEN - 1 ? clen : LLM_MAX_RESPONSE_LEN - 1;
        memcpy(result->content, content, copy_len);
        result->content[copy_len] = '\0';
        result->content_len = llm_json_unescape(result->content, copy_len);
    }

    if (tc_count > 0) {
        result->ok = true;
        result->prompt_tokens     = llm_json_find_int(body, body_len, "prompt_tokens", 0);
        result->completion_tokens = llm_json_find_int(body, body_len, "completion_tokens", 0);
        return true;
    }

    if (result->content_len <= 0) {
        int elen = 0;
        const char *errmsg = llm_json_find_string(body, body_len, "message", &elen);
        if (errmsg && elen > 0) {
            int copy = elen < (int)sizeof(_llm_error) - 1 ? elen : (int)sizeof(_llm_error) - 1;
            memcpy(_llm_error, errmsg, copy); _llm_error[copy] = '\0';
        } else {
            snprintf(_llm_error, sizeof(_llm_error), "No content in response");
        }
        return false;
    }

    result->prompt_tokens     = llm_json_find_int(body, body_len, "prompt_tokens", 0);
    result->completion_tokens = llm_json_find_int(body, body_len, "completion_tokens", 0);
    result->ok = true;
    return true;
}

static int llmReadResponse(char *buf, int buf_len) {
    int content_length = -1;
    bool chunked = false;

    // FIX: readStringUntil and readBytes are Stream methods.
    // Use _llm_stream (Stream*) for those; keep _llm_client (Client*) for
    // connected() / stop() which are Client/EthernetClient methods.
    String status_line = _llm_stream->readStringUntil('\n');
    if (status_line.length() < 12) {
        snprintf(_llm_error, sizeof(_llm_error), "Invalid HTTP response");
        return -1;
    }

    while (_llm_client->connected()) {
        String header = _llm_stream->readStringUntil('\n');
        header.trim();
        if (header.length() == 0) break;
        if (header.startsWith("Content-Length:") ||
            header.startsWith("content-length:")) {
            content_length = header.substring(15).toInt();
        }
        if (header.indexOf("chunked") >= 0) chunked = true;
    }

    int total = 0;
    int target = (content_length > 0 && !chunked)
                 ? (content_length < buf_len - 1 ? content_length : buf_len - 1)
                 : buf_len - 1;

    unsigned long last_data = millis();
    while (total < target) {
        int avail = _llm_client->available();
        if (avail > 0) {
            int to_read = avail < (target - total) ? avail : (target - total);
            int rd = _llm_stream->readBytes(buf + total, to_read);
            total += rd;
            last_data = millis();
        } else if (!_llm_client->connected()) {
            break;
        } else if (millis() - last_data > 10000) {
            break;
        } else {
            delay(10);
        }
    }

    buf[total] = '\0';
    return total;
}

bool llmChat(const LlmMessage *messages, int count,
             const char *tools_json, LlmResult *result) {
    result->ok = false;
    result->content[0] = '\0';
    result->content_len = 0;
    result->http_status = 0;
    result->tool_call_count = 0;

    /* request_buf: LLM_MAX_REQUEST_LEN now = 10240 (was 20480).
       Saves 10,240 B of .bss. Still fits typical LLM requests with
       tool definitions + a few conversation turns. */
    static char request_buf[LLM_MAX_REQUEST_LEN];
    int req_len = llmBuildRequest(request_buf, sizeof(request_buf),
                                   messages, count, tools_json);
    if (req_len < 0) {
        snprintf(_llm_error, sizeof(_llm_error), "Request too large for buffer");
        return false;
    }

    if (!_llm_client->connect(_llm_host, _llm_port)) {
        snprintf(_llm_error, sizeof(_llm_error), "%s connect failed",
                 _llm_use_tls ? "TLS" : "TCP");
        return false;
    }

    _llm_client->printf("POST %s HTTP/1.1\r\n", _llm_path);
    _llm_client->printf("Host: %s\r\n", _llm_host);
    if (_llm_api_key && _llm_api_key[0])
        _llm_client->printf("Authorization: Bearer %s\r\n", _llm_api_key);
    _llm_client->printf("Content-Type: application/json\r\n");
    _llm_client->printf("Content-Length: %d\r\n", req_len);
    _llm_client->printf("Connection: close\r\n");
    _llm_client->printf("\r\n");
    _llm_client->write((uint8_t *)request_buf, req_len);

    unsigned long wait_start = millis();
    while (!_llm_client->available()) {
        if (millis() - wait_start > LLM_READ_TIMEOUT_MS) {
            snprintf(_llm_error, sizeof(_llm_error), "Response timeout");
            _llm_client->stop();
            return false;
        }
        delay(50);
    }

    /* response_buf: LLM_MAX_RESPONSE_LEN now = 2048 (was 4096+2048=6144).
       Saves 4,096 B of .bss. The reader caps to buf_len-1 already. */
    static char response_buf[LLM_MAX_RESPONSE_LEN];
    int body_len = llmReadResponse(response_buf, sizeof(response_buf));
    _llm_client->stop();

    if (body_len <= 0) {
        snprintf(_llm_error, sizeof(_llm_error), "Empty response body");
        return false;
    }

    if (g_debug) {
        Serial.printf("[LLM] Response: %d bytes\n", body_len);
        Serial.printf("[LLM] Body: %.*s\n",
                      body_len < 500 ? body_len : 500, response_buf);
    }

    return llmParseResponse(response_buf, body_len, result);
}
