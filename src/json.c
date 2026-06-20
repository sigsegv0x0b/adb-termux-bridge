#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <math.h>

static json_value_t *json_parse_value(const char **p);

static void skip_ws(const char **p) {
    while (**p && (unsigned char)**p <= ' ') (*p)++;
}

static json_value_t *new_val(json_type_t t) {
    json_value_t *v = calloc(1, sizeof(json_value_t));
    if (v) v->type = t;
    return v;
}

static char *parse_string_raw(const char **p) {
    if (**p != '"') return NULL;
    (*p)++;
    size_t cap = 64, len = 0;
    char *s = malloc(cap);
    if (!s) return NULL;
    while (**p && **p != '"') {
        if (**p == '\\') {
            (*p)++;
            char c = 0;
            switch (**p) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u': {
                    unsigned int u = 0;
                    for (int i = 0; i < 4; i++) {
                        (*p)++;
                        char h = **p;
                        u <<= 4;
                        if (h >= '0' && h <= '9') u |= h - '0';
                        else if (h >= 'a' && h <= 'f') u |= h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') u |= h - 'A' + 10;
                    }
                    if (u < 0x80) c = (char)u;
                    else if (u < 0x800) {
                        s[len++] = (char)(0xC0 | (u >> 6));
                        c = (char)(0x80 | (u & 0x3F));
                    } else {
                        s[len++] = (char)(0xE0 | (u >> 12));
                        s[len++] = (char)(0x80 | ((u >> 6) & 0x3F));
                        c = (char)(0x80 | (u & 0x3F));
                    }
                    break;
                }
                default: c = **p; break;
            }
            if (c) {
                if (len + 1 >= cap) { cap *= 2; s = realloc(s, cap); }
                s[len++] = c;
            }
        } else {
            if (len + 1 >= cap) { cap *= 2; s = realloc(s, cap); }
            s[len++] = **p;
        }
        (*p)++;
    }
    if (**p == '"') (*p)++;
    s[len] = '\0';
    return s;
}

static json_value_t *parse_object(const char **p) {
    json_value_t *obj = new_val(JSON_OBJECT);
    if (!obj) return NULL;
    (*p)++;
    skip_ws(p);
    if (**p == '}') { (*p)++; return obj; }
    while (1) {
        skip_ws(p);
        char *key = parse_string_raw(p);
        if (!key) break;
        skip_ws(p);
        if (**p != ':') { free(key); break; }
        (*p)++;
        skip_ws(p);
        json_value_t *val = json_parse_value(p);
        if (!val) { free(key); break; }
        val->key = key;
        val->parent = obj;
        if (!obj->child) obj->child = val;
        else {
            json_value_t *last = obj->child;
            while (last->next) last = last->next;
            last->next = val;
        }
        skip_ws(p);
        if (**p == ',') { (*p)++; continue; }
        if (**p == '}') break;
    }
    if (**p == '}') (*p)++;
    return obj;
}

static json_value_t *parse_array(const char **p) {
    json_value_t *arr = new_val(JSON_ARRAY);
    if (!arr) return NULL;
    (*p)++;
    skip_ws(p);
    if (**p == ']') { (*p)++; return arr; }
    while (1) {
        skip_ws(p);
        json_value_t *val = json_parse_value(p);
        if (!val) break;
        val->parent = arr;
        if (!arr->child) arr->child = val;
        else {
            json_value_t *last = arr->child;
            while (last->next) last = last->next;
            last->next = val;
        }
        skip_ws(p);
        if (**p == ',') { (*p)++; continue; }
        if (**p == ']') break;
    }
    if (**p == ']') (*p)++;
    return arr;
}

static json_value_t *parse_number(const char **p) {
    char *end;
    double n = strtod(*p, &end);
    if (end == *p) return NULL;
    json_value_t *v = new_val(JSON_NUMBER);
    if (v) v->number_value = n;
    *p = end;
    return v;
}

static json_value_t *parse_raw_string(const char **p) {
    char *s = parse_string_raw(p);
    if (!s) return NULL;
    json_value_t *v = new_val(JSON_STRING);
    if (v) v->string_value = s;
    else free(s);
    return v;
}

static json_value_t *json_parse_value(const char **p) {
    skip_ws(p);
    switch (**p) {
        case '"': return parse_raw_string(p);
        case '{': return parse_object(p);
        case '[': return parse_array(p);
        case 't': if (strncmp(*p, "true", 4) == 0) { *p += 4; json_value_t *v = new_val(JSON_BOOL); if (v) v->bool_value = 1; return v; } return NULL;
        case 'f': if (strncmp(*p, "false", 5) == 0) { *p += 5; json_value_t *v = new_val(JSON_BOOL); if (v) v->bool_value = 0; return v; } return NULL;
        case 'n': if (strncmp(*p, "null", 4) == 0) { *p += 4; return new_val(JSON_NULL); } return NULL;
        default: return parse_number(p);
    }
}

json_value_t *json_parse(const char *input) {
    const char *p = input;
    json_value_t *v = json_parse_value(&p);
    return v;
}

const char *json_get_string(const json_value_t *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    for (json_value_t *c = obj->child; c; c = c->next) {
        if (c->key && strcmp(c->key, key) == 0 && c->type == JSON_STRING)
            return c->string_value;
    }
    return NULL;
}

double json_get_number(const json_value_t *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT) return 0;
    for (json_value_t *c = obj->child; c; c = c->next) {
        if (c->key && strcmp(c->key, key) == 0 && c->type == JSON_NUMBER)
            return c->number_value;
    }
    return 0;
}

int json_get_bool(const json_value_t *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT) return 0;
    for (json_value_t *c = obj->child; c; c = c->next) {
        if (c->key && strcmp(c->key, key) == 0 && c->type == JSON_BOOL)
            return c->bool_value;
    }
    return 0;
}

void json_free(json_value_t *val) {
    if (!val) return;
    if (val->type == JSON_STRING) free(val->string_value);
    json_free(val->child);
    json_free(val->next);
    free(val->key);
    free(val);
}

json_value_t *json_new_object(void) {
    return new_val(JSON_OBJECT);
}

json_value_t *json_new_string(const char *s) {
    json_value_t *v = new_val(JSON_STRING);
    if (v && s) v->string_value = strdup(s);
    return v;
}

json_value_t *json_new_number(double n) {
    json_value_t *v = new_val(JSON_NUMBER);
    if (v) v->number_value = n;
    return v;
}

json_value_t *json_new_bool(int b) {
    json_value_t *v = new_val(JSON_BOOL);
    if (v) v->bool_value = b;
    return v;
}

void json_add(json_value_t *obj, const char *key, json_value_t *val) {
    if (!obj || !val || obj->type != JSON_OBJECT) return;
    if (key) val->key = strdup(key);
    val->parent = obj;
    if (!obj->child) obj->child = val;
    else {
        json_value_t *last = obj->child;
        while (last->next) last = last->next;
        last->next = val;
    }
}

void json_add_string(json_value_t *obj, const char *key, const char *val) {
    json_add(obj, key, json_new_string(val));
}

void json_add_number(json_value_t *obj, const char *key, double val) {
    json_add(obj, key, json_new_number(val));
}

void json_add_bool(json_value_t *obj, const char *key, int val) {
    json_add(obj, key, json_new_bool(val));
}

static void json_serialize_rec(const json_value_t *val, char **out, size_t *len, size_t *cap) {
    if (!val) return;
    size_t needed = 0;

    switch (val->type) {
        case JSON_NULL:
            needed = 4;
            if (*len + needed >= *cap) { *cap = *cap ? *cap * 2 : 64; *out = realloc(*out, *cap); }
            memcpy(*out + *len, "null", 4); *len += 4;
            break;
        case JSON_BOOL:
            needed = val->bool_value ? 4 : 5;
            if (*len + needed >= *cap) { *cap = *cap ? *cap * 2 : 64; *out = realloc(*out, *cap); }
            if (val->bool_value) { memcpy(*out + *len, "true", 4); *len += 4; }
            else { memcpy(*out + *len, "false", 5); *len += 5; }
            break;
        case JSON_NUMBER: {
            char buf[64];
            if (val->number_value == (long long)val->number_value)
                snprintf(buf, sizeof(buf), "%lld", (long long)val->number_value);
            else
                snprintf(buf, sizeof(buf), "%g", val->number_value);
            size_t slen = strlen(buf);
            if (*len + slen >= *cap) { while (*len + slen >= *cap) *cap = *cap ? *cap * 2 : 64; *out = realloc(*out, *cap); }
            memcpy(*out + *len, buf, slen); *len += slen;
            break;
        }
        case JSON_STRING: {
            size_t slen = strlen(val->string_value);
            size_t extra = 2;
            for (size_t i = 0; i < slen; i++) {
                char c = val->string_value[i];
                if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t' || c == '\b' || c == '\f')
                    extra++;
                else if ((unsigned char)c < 0x20)
                    extra += 5;
            }
            if (*len + slen + extra >= *cap) { while (*len + slen + extra >= *cap) *cap = *cap ? *cap * 2 : 64; *out = realloc(*out, *cap); }
            (*out)[(*len)++] = '"';
            for (size_t i = 0; i < slen; i++) {
                char c = val->string_value[i];
                switch (c) {
                    case '"': (*out)[(*len)++] = '\\'; (*out)[(*len)++] = '"'; break;
                    case '\\': (*out)[(*len)++] = '\\'; (*out)[(*len)++] = '\\'; break;
                    case '\n': (*out)[(*len)++] = '\\'; (*out)[(*len)++] = 'n'; break;
                    case '\r': (*out)[(*len)++] = '\\'; (*out)[(*len)++] = 'r'; break;
                    case '\t': (*out)[(*len)++] = '\\'; (*out)[(*len)++] = 't'; break;
                    case '\b': (*out)[(*len)++] = '\\'; (*out)[(*len)++] = 'b'; break;
                    case '\f': (*out)[(*len)++] = '\\'; (*out)[(*len)++] = 'f'; break;
                    default:
                        if ((unsigned char)c < 0x20) {
                            *len += snprintf(*out + *len, *cap - *len, "\\u%04x", (unsigned char)c);
                        } else {
                            (*out)[(*len)++] = c;
                        }
                        break;
                }
            }
            (*out)[(*len)++] = '"';
            break;
        }
        case JSON_OBJECT: {
            if (*len + 1 >= *cap) { *cap = *cap ? *cap * 2 : 64; *out = realloc(*out, *cap); }
            (*out)[(*len)++] = '{';
            int first = 1;
            for (json_value_t *c = val->child; c; c = c->next) {
                if (!first) { if (*len + 1 >= *cap) { *cap *= 2; *out = realloc(*out, *cap); } (*out)[(*len)++] = ','; }
                first = 0;
                if (c->key) {
                    json_value_t tmp = { .type = JSON_STRING, .string_value = c->key };
                    json_serialize_rec(&tmp, out, len, cap);
                }
                if (*len + 1 >= *cap) { *cap *= 2; *out = realloc(*out, *cap); }
                (*out)[(*len)++] = ':';
                json_serialize_rec(c, out, len, cap);
            }
            if (*len + 1 >= *cap) { *cap *= 2; *out = realloc(*out, *cap); }
            (*out)[(*len)++] = '}';
            break;
        }
        case JSON_ARRAY: {
            if (*len + 1 >= *cap) { *cap = *cap ? *cap * 2 : 64; *out = realloc(*out, *cap); }
            (*out)[(*len)++] = '[';
            int first = 1;
            for (json_value_t *c = val->child; c; c = c->next) {
                if (!first) { if (*len + 1 >= *cap) { *cap *= 2; *out = realloc(*out, *cap); } (*out)[(*len)++] = ','; }
                first = 0;
                json_serialize_rec(c, out, len, cap);
            }
            if (*len + 1 >= *cap) { *cap *= 2; *out = realloc(*out, *cap); }
            (*out)[(*len)++] = ']';
            break;
        }
    }
}

char *json_serialize(const json_value_t *val) {
    char *out = NULL;
    size_t len = 0, cap = 0;
    json_serialize_rec(val, &out, &len, &cap);
    if (out) { out[len] = '\0'; }
    return out;
}
