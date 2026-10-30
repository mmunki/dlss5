/*
 * A very small JSON reader.  The daemon only has to read the flat config
 * object the app writes, so this is deliberately tiny: objects, arrays,
 * strings, numbers and booleans, no \u escapes beyond the BMP basics.
 */
#include "dlss5.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct D5Json {
    D5JsonType type;
    int boolean;
    double number;
    char *string;
    D5Json **items;
    char **keys;
    size_t count;
};

typedef struct {
    const char *p;
    char *err;
    size_t errlen;
    int failed;
} Parser;

static void fail(Parser *ps, const char *msg) {
    if (!ps->failed && ps->err && ps->errlen) {
        snprintf(ps->err, ps->errlen, "%s", msg);
    }
    ps->failed = 1;
}

static void skip_ws(Parser *ps) {
    while (*ps->p && isspace((unsigned char)*ps->p)) ps->p++;
}

static D5Json *node_new(D5JsonType type) {
    D5Json *v = calloc(1, sizeof(*v));
    if (v) v->type = type;
    return v;
}

static int append_item(D5Json *parent, char *key, D5Json *item) {
    D5Json **items = realloc(parent->items, (parent->count + 1) * sizeof(*items));
    if (!items) return 0;
    parent->items = items;
    if (parent->type == D5_JSON_OBJECT) {
        char **keys = realloc(parent->keys, (parent->count + 1) * sizeof(*keys));
        if (!keys) return 0;
        parent->keys = keys;
        parent->keys[parent->count] = key;
    }
    parent->items[parent->count++] = item;
    return 1;
}

static char *parse_string(Parser *ps) {
    if (*ps->p != '"') {
        fail(ps, "expected string");
        return NULL;
    }
    ps->p++;
    size_t cap = 32, len = 0;
    char *out = malloc(cap);
    if (!out) {
        fail(ps, "out of memory");
        return NULL;
    }
    while (*ps->p && *ps->p != '"') {
        unsigned char c = (unsigned char)*ps->p++;
        if (c == '\\') {
            char esc = *ps->p++;
            switch (esc) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u': {
                    unsigned int cp = 0;
                    for (int i = 0; i < 4; i++) {
                        char h = *ps->p++;
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= (unsigned int)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (unsigned int)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (unsigned int)(h - 'A' + 10);
                        else { fail(ps, "bad \\u escape"); free(out); return NULL; }
                    }
                    if (cp < 0x80) {
                        c = (unsigned char)cp;
                    } else {
                        /* Encode as UTF-8; the config never needs more. */
                        if (len + 4 >= cap) {
                            cap *= 2;
                            char *n = realloc(out, cap);
                            if (!n) { fail(ps, "out of memory"); free(out); return NULL; }
                            out = n;
                        }
                        if (cp < 0x800) {
                            out[len++] = (char)(0xC0 | (cp >> 6));
                            out[len++] = (char)(0x80 | (cp & 0x3F));
                        } else {
                            out[len++] = (char)(0xE0 | (cp >> 12));
                            out[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            out[len++] = (char)(0x80 | (cp & 0x3F));
                        }
                        continue;
                    }
                    break;
                }
                default:
                    fail(ps, "bad escape");
                    free(out);
                    return NULL;
            }
        }
        if (len + 2 >= cap) {
            cap *= 2;
            char *n = realloc(out, cap);
            if (!n) { fail(ps, "out of memory"); free(out); return NULL; }
            out = n;
        }
        out[len++] = (char)c;
    }
    if (*ps->p != '"') {
        fail(ps, "unterminated string");
        free(out);
        return NULL;
    }
    ps->p++;
    out[len] = '\0';
    return out;
}

static D5Json *parse_value(Parser *ps, int depth);

static D5Json *parse_array(Parser *ps, int depth) {
    D5Json *arr = node_new(D5_JSON_ARRAY);
    if (!arr) { fail(ps, "out of memory"); return NULL; }
    ps->p++; /* [ */
    skip_ws(ps);
    if (*ps->p == ']') { ps->p++; return arr; }
    for (;;) {
        D5Json *item = parse_value(ps, depth + 1);
        if (!item) { d5_json_free(arr); return NULL; }
        if (!append_item(arr, NULL, item)) {
            d5_json_free(item);
            d5_json_free(arr);
            fail(ps, "out of memory");
            return NULL;
        }
        skip_ws(ps);
        if (*ps->p == ',') { ps->p++; skip_ws(ps); continue; }
        if (*ps->p == ']') { ps->p++; return arr; }
        fail(ps, "expected , or ]");
        d5_json_free(arr);
        return NULL;
    }
}

static D5Json *parse_object(Parser *ps, int depth) {
    D5Json *obj = node_new(D5_JSON_OBJECT);
    if (!obj) { fail(ps, "out of memory"); return NULL; }
    ps->p++; /* { */
    skip_ws(ps);
    if (*ps->p == '}') { ps->p++; return obj; }
    for (;;) {
        skip_ws(ps);
        char *key = parse_string(ps);
        if (!key) { d5_json_free(obj); return NULL; }
        skip_ws(ps);
        if (*ps->p != ':') {
            free(key);
            fail(ps, "expected :");
            d5_json_free(obj);
            return NULL;
        }
        ps->p++;
        D5Json *item = parse_value(ps, depth + 1);
        if (!item) { free(key); d5_json_free(obj); return NULL; }
        if (!append_item(obj, key, item)) {
            free(key);
            d5_json_free(item);
            d5_json_free(obj);
            fail(ps, "out of memory");
            return NULL;
        }
        skip_ws(ps);
        if (*ps->p == ',') { ps->p++; continue; }
        if (*ps->p == '}') { ps->p++; return obj; }
        fail(ps, "expected , or }");
        d5_json_free(obj);
        return NULL;
    }
}

static D5Json *parse_value(Parser *ps, int depth) {
    if (depth > 32) { fail(ps, "nesting too deep"); return NULL; }
    skip_ws(ps);
    switch (*ps->p) {
        case '{': return parse_object(ps, depth);
        case '[': return parse_array(ps, depth);
        case '"': {
            D5Json *v = node_new(D5_JSON_STRING);
            if (!v) { fail(ps, "out of memory"); return NULL; }
            v->string = parse_string(ps);
            if (!v->string) { free(v); return NULL; }
            return v;
        }
        case 't':
            if (strncmp(ps->p, "true", 4) == 0) {
                D5Json *v = node_new(D5_JSON_BOOL);
                if (!v) { fail(ps, "out of memory"); return NULL; }
                v->boolean = 1;
                ps->p += 4;
                return v;
            }
            fail(ps, "bad literal");
            return NULL;
        case 'f':
            if (strncmp(ps->p, "false", 5) == 0) {
                D5Json *v = node_new(D5_JSON_BOOL);
                if (!v) { fail(ps, "out of memory"); return NULL; }
                v->boolean = 0;
                ps->p += 5;
                return v;
            }
            fail(ps, "bad literal");
            return NULL;
        case 'n':
            if (strncmp(ps->p, "null", 4) == 0) {
                ps->p += 4;
                return node_new(D5_JSON_NULL);
            }
            fail(ps, "bad literal");
            return NULL;
        default: {
            char *end = NULL;
            double num = strtod(ps->p, &end);
            if (end == ps->p) { fail(ps, "bad value"); return NULL; }
            D5Json *v = node_new(D5_JSON_NUMBER);
            if (!v) { fail(ps, "out of memory"); return NULL; }
            v->number = num;
            ps->p = end;
            return v;
        }
    }
}

D5Json *d5_json_parse(const char *text, char *err, size_t errlen) {
    Parser ps = {text, err, errlen, 0};
    if (err && errlen) err[0] = '\0';
    if (!text) {
        if (err && errlen) snprintf(err, errlen, "empty input");
        return NULL;
    }
    D5Json *root = parse_value(&ps, 0);
    if (!root) return NULL;
    skip_ws(&ps);
    if (*ps.p != '\0') {
        d5_json_free(root);
        if (err && errlen) snprintf(err, errlen, "trailing data");
        return NULL;
    }
    return root;
}

void d5_json_free(D5Json *v) {
    if (!v) return;
    for (size_t i = 0; i < v->count; i++) {
        d5_json_free(v->items[i]);
        if (v->keys) free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v->string);
    free(v);
}

D5JsonType d5_json_type(const D5Json *v) { return v ? v->type : D5_JSON_NULL; }

const D5Json *d5_json_get(const D5Json *obj, const char *key) {
    if (!obj || obj->type != D5_JSON_OBJECT) return NULL;
    for (size_t i = 0; i < obj->count; i++) {
        if (strcmp(obj->keys[i], key) == 0) return obj->items[i];
    }
    return NULL;
}

const D5Json *d5_json_at(const D5Json *arr, size_t index) {
    if (!arr || arr->type != D5_JSON_ARRAY || index >= arr->count) return NULL;
    return arr->items[index];
}

size_t d5_json_len(const D5Json *v) { return v ? v->count : 0; }

int d5_json_bool(const D5Json *v, int fallback) {
    if (!v) return fallback;
    if (v->type == D5_JSON_BOOL) return v->boolean;
    if (v->type == D5_JSON_NUMBER) return v->number != 0.0;
    return fallback;
}

double d5_json_num(const D5Json *v, double fallback) {
    if (!v) return fallback;
    if (v->type == D5_JSON_NUMBER) return v->number;
    if (v->type == D5_JSON_BOOL) return v->boolean ? 1.0 : 0.0;
    return fallback;
}

const char *d5_json_str(const D5Json *v, const char *fallback) {
    if (!v || v->type != D5_JSON_STRING || !v->string) return fallback;
    return v->string;
}
