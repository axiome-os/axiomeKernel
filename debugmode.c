#include "debugmode.h"

static int g_debug;
static int g_splash = 1;
static int g_force_fb;

static int token_is(const char *s, const char *a, const char *b,
                    const char *c)
{
    /* Compare a NUL/space-terminated token against up to three spellings. */
    const char *alts[3] = { a, b, c };
    for (int i = 0; i < 3; i++) {
        const char *p = s;
        const char *q = alts[i];
        if (!q)
            continue;
        while (*p && *p != ' ' && *p != '\t' && *p == *q) {
            p++;
            q++;
        }
        if ((!*p || *p == ' ' || *p == '\t') && *q == '\0')
            return 1;
    }
    return 0;
}

static int bool_value(const char *value, int def)
{
    if (!value || !*value)
        return def;
    if (value[0] == '1' || value[0] == 'y' || value[0] == 'Y' ||
        value[0] == 't' || value[0] == 'T')
        return 1;
    if (value[0] == '0' || value[0] == 'n' || value[0] == 'N' ||
        value[0] == 'f' || value[0] == 'F')
        return 0;
    return def;
}

void kernel_debug_init(const char *cmdline)
{
    const char *p = cmdline;

    g_debug = 0;
    g_splash = 1;
    g_force_fb = 0;
    if (!p)
        return;

    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (!*p)
            break;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
            p++;
        const char *end = p;

        char tok[96];
        int n = 0;
        for (const char *q = start; q < end && n < (int)sizeof(tok) - 1; q++)
            tok[n++] = *q;
        tok[n] = '\0';

        char *eq = 0;
        for (char *q = tok; *q; q++)
            if (*q == '=') { eq = q; break; }

        if (eq) {
            *eq = '\0';
            const char *value = eq + 1;
            if (token_is(tok, "debug", 0, 0))
                g_debug = bool_value(value, 1);
            else if (token_is(tok, "splash", 0, 0))
                g_splash = bool_value(value, 1);
        } else {
            if (token_is(tok, "debug", "debugon", 0))
                g_debug = 1;
            else if (token_is(tok, "nodebug", "debugoff", 0))
                g_debug = 0;
            else if (token_is(tok, "nosplash", "nospash", 0))
                g_splash = 0;
        }
    }
}

int kernel_debug_mode(void)
{
    return g_debug;
}

int kernel_debug_fb(void)
{
    return g_debug || g_force_fb;
}

void kernel_debug_force_fb(int on)
{
    g_force_fb = on != 0;
}

int kernel_splash_enabled(void)
{
    return g_splash;
}
