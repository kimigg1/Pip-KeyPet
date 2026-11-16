/* puglik: a reader for what Gadgetbridge sends a "Bangle.js": not JSON but JavaScript literals, e.g.
     \x10GB({"t":"notify","id":5,"src":"Telegram","body":"Привет"})
   Strings may hold \xHH, \uHHHH, \n-style and octal escapes, raw Latin-1 bytes, or come as atob("base64").
   Everything is decoded to UTF-8. Pure C, no allocation: a fixed pool per parse. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

enum { GBJS_NULL, GBJS_BOOL, GBJS_NUM, GBJS_STR, GBJS_ARR, GBJS_OBJ };

typedef struct gbjs_node {
    int type;
    const char *key;   /* inside an object, else NULL */
    const char *str;   /* GBJS_STR: UTF-8, zero-terminated */
    double num;        /* GBJS_NUM; GBJS_BOOL: 0/1 */
    int child, next;   /* indexes into the pool, -1: none */
} gbjs_node_t;

#define GBJS_NODES 96
#define GBJS_TEXT 2048

typedef struct {
    gbjs_node_t n[GBJS_NODES];
    int count;
    char text[GBJS_TEXT]; /* decoded strings and keys */
    int used;
} gbjs_t;

/* Parses one value starting at s (n bytes). Returns the root node index, or -1 if it is not a value we understand.
   Strings longer than what fits are cut (on a UTF-8 character boundary). */
int gbjs_parse(gbjs_t *j, const char *s, size_t n);

/* Lookups on an object node (-1 / defaults when missing or of another type) */
int gbjs_get(const gbjs_t *j, int obj, const char *key);
const char *gbjs_str(const gbjs_t *j, int obj, const char *key, const char *def);
double gbjs_num(const gbjs_t *j, int obj, const char *key, double def);
bool gbjs_bool(const gbjs_t *j, int obj, const char *key, bool def);

/* Writes s into out as a JSON string literal (quotes included), escaping for Gadgetbridge's reader. */
size_t gbjs_quote(char *out, size_t cap, const char *s);
