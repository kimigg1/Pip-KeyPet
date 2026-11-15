/* plip: the Gadgetbridge (Bangle.js) literal reader on what Gadgetbridge really sends */
#include "gbjs.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(what, cond)                                                                                                   \
    do {                                                                                                                    \
        if (cond) printf("ok    %s\n", what);                                                                               \
        else printf("FAIL  %s\n", what), fails++;                                                                           \
    } while (0)

int main(void)
{
    static gbjs_t j;
    /* Russian text arrives as \u escapes, a Latin-1 byte as itself, a lead byte in the C2..F4 range as \x */
    const char *a = "{\"t\":\"notify\",\"id\":1700000123,\"src\":\"Telegram\",\"title\":\"\\u041c\\u0430\\u043c\\u0430\","
                    "\"body\":\"\\u041f\\u0440\\u0438\\u0432\\u0435\\u0442!\\n\\xc3 caf\xe9\",\"reply\":true}";
    int r = gbjs_parse(&j, a, strlen(a));
    CHECK("notify parses", r >= 0);
    CHECK("type", !strcmp(gbjs_str(&j, r, "t", ""), "notify"));
    CHECK("id is a big number", gbjs_num(&j, r, "id", 0) == 1700000123.0);
    CHECK("cyrillic title", !strcmp(gbjs_str(&j, r, "title", ""), "Мама"));
    CHECK("body: cyrillic, newline, \\x and a raw Latin-1 byte", !strcmp(gbjs_str(&j, r, "body", ""), "Привет!\nÃ café"));
    CHECK("bool", gbjs_bool(&j, r, "reply", false));
    CHECK("missing key -> default", !strcmp(gbjs_str(&j, r, "subject", "-"), "-"));

    /* base64 when it is shorter, single quotes, bare keys, an alarm array, negative and float numbers */
    const char *b = "{t:'alarm',d:[{h:7,m:30,rep:31,on:true},{h:9,m:0,rep:96,on:false}],x:-2.5,s:atob(\"SGVsbG8gd29ybGQ=\")}";
    r = gbjs_parse(&j, b, strlen(b));
    CHECK("alarm parses", r >= 0 && !strcmp(gbjs_str(&j, r, "t", ""), "alarm"));
    int d = gbjs_get(&j, r, "d");
    int n = 0, first = d >= 0 ? j.n[d].child : -1;
    for (int c = first; c >= 0; c = j.n[c].next) n++;
    CHECK("two alarms", d >= 0 && j.n[d].type == GBJS_ARR && n == 2);
    CHECK("first alarm 7:30 every weekday", first >= 0 && gbjs_num(&j, first, "h", -1) == 7 && gbjs_num(&j, first, "m", -1) == 30 &&
                                                gbjs_num(&j, first, "rep", -1) == 31 && gbjs_bool(&j, first, "on", false));
    CHECK("negative float", fabs(gbjs_num(&j, r, "x", 0) + 2.5) < 1e-9);
    CHECK("atob", !strcmp(gbjs_str(&j, r, "s", ""), "Hello world"));

    /* surrogate pairs (an emoji), escaped quote and backslash, \20 (DLE) */
    const char *c = "{\"t\":\"notify\",\"body\":\"ok \\ud83d\\ude00 \\\"q\\\" \\\\ \\20.\"}";
    r = gbjs_parse(&j, c, strlen(c));
    CHECK("emoji and escapes", r >= 0 && !strcmp(gbjs_str(&j, r, "body", ""), "ok \xF0\x9F\x98\x80 \"q\" \\ \x10."));

    /* something that is not a value */
    CHECK("garbage is refused", gbjs_parse(&j, "setTime(5)", 10) < 0);

    /* a very long body is cut, not overflowed, and stays valid UTF-8 */
    static char big[6000];
    strcpy(big, "{\"t\":\"notify\",\"body\":\"");
    for (int i = 0; i < 900; i++) strcat(big, "\\u0416");
    strcat(big, "\"}");
    r = gbjs_parse(&j, big, strlen(big));
    const char *body = r >= 0 ? gbjs_str(&j, r, "body", NULL) : NULL;
    bool utf8 = body && strlen(body) % 2 == 0 && strlen(body) > 1000;
    CHECK("long body cut on a character boundary", utf8);

    /* quoting back for Gadgetbridge: UTF-8 to \u, quotes and backslashes escaped */
    char q[128];
    gbjs_quote(q, sizeof q, "Пип \"say\"\\");
    CHECK("quote", !strcmp(q, "\"\\u041f\\u0438\\u043f \\\"say\\\"\\\\\""));
    return fails ? 1 : 0;
}
