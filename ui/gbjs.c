#include "gbjs.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    gbjs_t *j;
    const unsigned char *p, *end;
} rd_t;

static void ws(rd_t *r)
{
    while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' || *r->p == '\r' || *r->p == '\n')) r->p++;
}

static int node(gbjs_t *j, int type)
{
    if (j->count >= GBJS_NODES) return -1;
    gbjs_node_t *n = &j->n[j->count];
    memset(n, 0, sizeof *n);
    n->type = type, n->child = n->next = -1;
    return j->count++;
}

/* the decoded text area: code points in, UTF-8 out, cut cleanly when full */
typedef struct { gbjs_t *j; int start; bool full; } sb_t;

static void sb_cp(sb_t *b, unsigned cp)
{
    if (b->full) return;
    unsigned char u[4];
    int k;
    if (cp < 0x80) u[0] = (unsigned char)cp, k = 1;
    else if (cp < 0x800) u[0] = 0xC0 | cp >> 6, u[1] = 0x80 | (cp & 63), k = 2;
    else if (cp < 0x10000) u[0] = 0xE0 | cp >> 12, u[1] = 0x80 | (cp >> 6 & 63), u[2] = 0x80 | (cp & 63), k = 3;
    else u[0] = 0xF0 | cp >> 18, u[1] = 0x80 | (cp >> 12 & 63), u[2] = 0x80 | (cp >> 6 & 63), u[3] = 0x80 | (cp & 63), k = 4;
    if (b->j->used + k + 1 > GBJS_TEXT) { b->full = true; return; }
    memcpy(b->j->text + b->j->used, u, (size_t)k);
    b->j->used += k;
}

static const char *sb_end(sb_t *b)
{
    if (b->j->used >= GBJS_TEXT) b->j->used = GBJS_TEXT - 1;
    b->j->text[b->j->used++] = 0;
    return b->j->text + b->start;
}

static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static bool hexn(rd_t *r, int n, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < n; i++) {
        if (r->p >= r->end || hexv(*r->p) < 0) return false;
        v = v << 4 | (unsigned)hexv(*r->p++);
    }
    *out = v;
    return true;
}

/* a quoted string; raw bytes are Latin-1 (Gadgetbridge sends ISO-8859-1), escapes as JavaScript */
static const char *string(rd_t *r)
{
    unsigned char q = *r->p++;
    sb_t b = {r->j, r->j->used, false};
    unsigned hi = 0; /* a pending high surrogate */
    while (r->p < r->end && *r->p != q) {
        unsigned cp = *r->p++;
        if (cp == '\\' && r->p < r->end) {
            unsigned c = *r->p++;
            switch (c) {
            case 'n': cp = '\n'; break;
            case 't': cp = '\t'; break;
            case 'r': cp = '\r'; break;
            case 'b': cp = '\b'; break;
            case 'f': cp = '\f'; break;
            case 'v': cp = '\v'; break;
            case 'x': if (!hexn(r, 2, &cp)) return NULL; break;
            case 'u': if (!hexn(r, 4, &cp)) return NULL; break;
            default:
                if (c >= '0' && c <= '7') { /* \0..\7, maybe a second octal digit: \20 is DLE */
                    cp = c - '0';
                    if (r->p < r->end && *r->p >= '0' && *r->p <= '7') cp = cp * 8 + (unsigned)(*r->p++ - '0');
                } else cp = c; /* \" \\ \' \/ */
            }
        }
        if (cp >= 0xD800 && cp < 0xDC00) { hi = cp; continue; }
        if (cp >= 0xDC00 && cp < 0xE000 && hi) cp = 0x10000 + ((hi - 0xD800) << 10) + (cp - 0xDC00);
        hi = 0;
        sb_cp(&b, cp);
    }
    if (r->p >= r->end) return NULL;
    r->p++; /* the closing quote */
    return sb_end(&b);
}

static int b64v(int c)
{
    return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
}

/* atob("...."): the bytes are Latin-1 code points, as with raw string bytes */
static const char *atob(rd_t *r)
{
    r->p += 5; /* atob( */
    ws(r);
    if (r->p >= r->end || (*r->p != '"' && *r->p != '\'')) return NULL;
    unsigned char q = *r->p++;
    sb_t b = {r->j, r->j->used, false};
    unsigned acc = 0;
    int bits = 0;
    while (r->p < r->end && *r->p != q) {
        int v = b64v(*r->p++);
        if (v < 0) continue; /* '=' padding, stray whitespace */
        acc = acc << 6 | (unsigned)v, bits += 6;
        if (bits >= 8) bits -= 8, sb_cp(&b, acc >> bits & 255);
    }
    if (r->p >= r->end) return NULL;
    r->p++;
    ws(r);
    if (r->p < r->end && *r->p == ')') r->p++;
    return sb_end(&b);
}

static bool word(rd_t *r, const char *w)
{
    size_t n = strlen(w);
    if ((size_t)(r->end - r->p) < n || memcmp(r->p, w, n)) return false;
    r->p += n;
    return true;
}

static int value(rd_t *r, int depth);

static int container(rd_t *r, int depth, bool obj)
{
    int me = node(r->j, obj ? GBJS_OBJ : GBJS_ARR), last = -1;
    if (me < 0) return -1;
    r->p++; /* { or [ */
    for (;;) {
        ws(r);
        if (r->p >= r->end) return -1;
        if (*r->p == (obj ? '}' : ']')) { r->p++; return me; }
        const char *key = NULL;
        if (obj) {
            if (*r->p == '"' || *r->p == '\'') key = string(r);
            else { /* a bare identifier */
                sb_t b = {r->j, r->j->used, false};
                while (r->p < r->end && (*r->p == '_' || *r->p == '$' || ((*r->p | 32) >= 'a' && (*r->p | 32) <= 'z') || (*r->p >= '0' && *r->p <= '9')))
                    sb_cp(&b, *r->p++);
                key = sb_end(&b);
            }
            ws(r);
            if (!key || r->p >= r->end || *r->p != ':') return -1;
            r->p++;
        }
        int v = value(r, depth + 1);
        if (v < 0) return -1;
        r->j->n[v].key = key;
        if (last < 0) r->j->n[me].child = v;
        else r->j->n[last].next = v;
        last = v;
        ws(r);
        if (r->p < r->end && *r->p == ',') r->p++;
    }
}

static int value(rd_t *r, int depth)
{
    ws(r);
    if (r->p >= r->end || depth > 8) return -1;
    unsigned char c = *r->p;
    if (c == '{' || c == '[') return container(r, depth, c == '{');
    if (c == '"' || c == '\'') {
        const char *s = string(r);
        int n = s ? node(r->j, GBJS_STR) : -1;
        if (n >= 0) r->j->n[n].str = s;
        return n;
    }
    if (word(r, "atob(") ? (r->p -= 5, true) : false) {
        const char *s = atob(r);
        int n = s ? node(r->j, GBJS_STR) : -1;
        if (n >= 0) r->j->n[n].str = s;
        return n;
    }
    if (word(r, "true") || word(r, "false")) {
        int n = node(r->j, GBJS_BOOL);
        if (n >= 0) r->j->n[n].num = r->p[-1] == 'e' && r->p[-2] == 'u';
        return n;
    }
    if (word(r, "null") || word(r, "undefined")) return node(r->j, GBJS_NULL);
    if (c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9')) {
        char tmp[40];
        size_t k = 0;
        while (r->p < r->end && k < sizeof tmp - 1 && strchr("+-.eE0123456789", *r->p)) tmp[k++] = (char)*r->p++;
        tmp[k] = 0;
        int n = node(r->j, GBJS_NUM);
        if (n >= 0) r->j->n[n].num = strtod(tmp, NULL);
        return n;
    }
    return -1;
}

int gbjs_parse(gbjs_t *j, const char *s, size_t n)
{
    j->count = 0, j->used = 0;
    rd_t r = {j, (const unsigned char *)s, (const unsigned char *)s + n};
    return value(&r, 0);
}

int gbjs_get(const gbjs_t *j, int obj, const char *key)
{
    if (obj < 0 || obj >= j->count || j->n[obj].type != GBJS_OBJ) return -1;
    for (int c = j->n[obj].child; c >= 0; c = j->n[c].next)
        if (j->n[c].key && !strcmp(j->n[c].key, key)) return c;
    return -1;
}

const char *gbjs_str(const gbjs_t *j, int obj, const char *key, const char *def)
{
    int c = gbjs_get(j, obj, key);
    return c >= 0 && j->n[c].type == GBJS_STR ? j->n[c].str : def;
}

double gbjs_num(const gbjs_t *j, int obj, const char *key, double def)
{
    int c = gbjs_get(j, obj, key);
    return c >= 0 && (j->n[c].type == GBJS_NUM || j->n[c].type == GBJS_BOOL) ? j->n[c].num : def;
}

bool gbjs_bool(const gbjs_t *j, int obj, const char *key, bool def)
{
    int c = gbjs_get(j, obj, key);
    return c >= 0 && (j->n[c].type == GBJS_BOOL || j->n[c].type == GBJS_NUM) ? j->n[c].num != 0 : def;
}

size_t gbjs_quote(char *out, size_t cap, const char *s)
{
    size_t k = 0;
#define PUT(ch) (k + 1 < cap ? (void)(out[k++] = (char)(ch)) : (void)0)
    PUT('"');
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        unsigned cp = *p;
        int len = cp < 0x80 ? 1 : cp >> 5 == 6 ? 2 : cp >> 4 == 14 ? 3 : cp >> 3 == 30 ? 4 : 1;
        if (len > 1) { /* UTF-8 to a \uXXXX escape (Java's JSONObject reads those) */
            cp &= 0xFF >> (len + 1);
            for (int i = 1; i < len && p[i]; i++) cp = cp << 6 | (p[i] & 63);
            for (int i = 0; i < len && *p; i++) p++;
            if (cp > 0xFFFF) cp = '?';
            char u[8];
            static const char hx[] = "0123456789abcdef";
            u[0] = '\\', u[1] = 'u', u[2] = hx[cp >> 12 & 15], u[3] = hx[cp >> 8 & 15], u[4] = hx[cp >> 4 & 15], u[5] = hx[cp & 15];
            for (int i = 0; i < 6; i++) PUT(u[i]);
            continue;
        }
        p++;
        if (cp == '"' || cp == '\\') PUT('\\'), PUT(cp);
        else if (cp == '\n') PUT('\\'), PUT('n');
        else if (cp < 32) PUT(' ');
        else PUT(cp);
    }
    PUT('"');
#undef PUT
    if (cap) out[k < cap ? k : cap - 1] = 0;
    return k;
}
