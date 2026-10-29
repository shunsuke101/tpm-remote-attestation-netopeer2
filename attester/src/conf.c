/*
 * conf.c — /etc/rats-attester.conf の解析。doc/design/05_challenge_response.md §3.3
 * rats-attester.confの形式: key = value。'#' 以降はコメント。前後の空白は無視する。
 * 未知の key と、値の形式の誤り（ak-handle が 16 進数でない、nonce-max が TPM2B_DATA を超えるなど）はエラーにする。
 */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tss2/tss2_tpm2_types.h>

#include "conf.h"

/* TPM2B_DATA.buffer に収まる nonce の上限（64 バイト） */
#define NONCE_LIMIT sizeof(((TPM2B_DATA *)0)->buffer)

/* 永続ハンドルの範囲（TPM 2.0 Library Part 2 §7.4 TPM_HT_PERSISTENT） */
#define PERSISTENT_FIRST 0x81000000u
#define PERSISTENT_LAST  0x81FFFFFFu

/*これはいらない、rats-attester.confの内容のハードコードだ*/
void
rats_conf_defaults(struct rats_conf *c)
{
    memset(c, 0, sizeof *c);
    c->tcti = strdup("device:/dev/tpmrm0");
    c->tpm_name = strdup("tpm0");
    c->certificate_name = strdup("ak-cert-0");
    c->ak_handle = 0x81010002;
    c->hardware_based = 0;
    c->nonce_min = 16;
    c->nonce_max = 64;
    c->quote_retry = 3;
}

static char *
trim(char *s)
{
    char *end;

    while (isspace((unsigned char)*s)) {
        s++;
    }
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return s;
}

static int
set_str(char **dst, const char *val)
{
    char *dup = strdup(val);

    if (!dup) {
        return -1;
    }
    free(*dst);
    *dst = dup;
    return 0;
}

/* 10 進（base 10）か、0x 付きの 16 進（base 16）。全体が数字でなければ失敗 */
static int
parse_ul(const char *val, int base, unsigned long *out)
{
    char *end = NULL;

    errno = 0;
    *out = strtoul(val, &end, base);
    if (errno || (end == val) || *end) {
        return -1;
    }
    return 0;
}

static int
parse_bool(const char *val, int *out)
{
    if (!strcmp(val, "true") || !strcmp(val, "yes") || !strcmp(val, "1")) {
        *out = 1;
    } else if (!strcmp(val, "false") || !strcmp(val, "no") || !strcmp(val, "0")) {
        *out = 0;
    } else {
        return -1;
    }
    return 0;
}

/* 1 つの key = value を c に入れる。0 で成功、-1 で値の誤り、-2 で未知の key、-3 でメモリ不足 */
static int
set_key(struct rats_conf *c, const char *key, const char *val)
{
    unsigned long ul;

    if (!strcmp(key, "tcti")) {
        return set_str(&c->tcti, val) ? -3 : 0;
    }
    if (!strcmp(key, "tpm-name")) {
        return set_str(&c->tpm_name, val) ? -3 : 0;
    }
    if (!strcmp(key, "certificate-name")) {
        return set_str(&c->certificate_name, val) ? -3 : 0;
    }
    if (!strcmp(key, "ak-handle")) {
        if (strncmp(val, "0x", 2) || parse_ul(val, 16, &ul) || (ul > 0xFFFFFFFFul)) {
            return -1;
        }
        c->ak_handle = (uint32_t)ul;
        return 0;
    }
    if (!strcmp(key, "hardware-based")) {
        return parse_bool(val, &c->hardware_based);
    }
    if (!strcmp(key, "nonce-min")) {
        if (parse_ul(val, 10, &ul)) {
            return -1;
        }
        c->nonce_min = ul;
        return 0;
    }
    if (!strcmp(key, "nonce-max")) {
        if (parse_ul(val, 10, &ul)) {
            return -1;
        }
        c->nonce_max = ul;
        return 0;
    }
    if (!strcmp(key, "quote-retry")) {
        if (parse_ul(val, 10, &ul) || (ul > 100)) {
            return -1;
        }
        c->quote_retry = (unsigned)ul;
        return 0;
    }
    return -2;
}

/* 全体の整合性（ファイルに無い項目は既定値のまま検査される） */
static int
validate(const struct rats_conf *c, char *errmsg, size_t errlen)
{
    if (!c->tcti || !*c->tcti) {
        snprintf(errmsg, errlen, "tcti must not be empty");
        return -1;
    }
    if (!c->tpm_name || !*c->tpm_name || !c->certificate_name || !*c->certificate_name) {
        snprintf(errmsg, errlen, "tpm-name and certificate-name must not be empty");
        return -1;
    }
    if ((c->ak_handle < PERSISTENT_FIRST) || (c->ak_handle > PERSISTENT_LAST)) {
        snprintf(errmsg, errlen, "ak-handle 0x%08x is not a persistent handle (0x%08x..0x%08x)", c->ak_handle,
                PERSISTENT_FIRST, PERSISTENT_LAST);
        return -1;
    }
    if ((c->nonce_min < 1) || (c->nonce_max > NONCE_LIMIT) || (c->nonce_min > c->nonce_max)) {
        snprintf(errmsg, errlen, "nonce-min..nonce-max = %zu..%zu is not within 1..%zu", c->nonce_min, c->nonce_max,
                (size_t)NONCE_LIMIT);
        return -1;
    }
    return 0;
}

/*/etc/rats-attester.confから内容を取得してrats_confの変数に値を入れる*/
int
rats_conf_load(struct rats_conf *c, const char *path, char *errmsg, size_t errlen)
{
    FILE *f;
    char line[512];
    unsigned lineno = 0;
    int ret = -1;

    f = fopen(path, "r");
    if (!f) {
        snprintf(errmsg, errlen, "%s: %s", path, strerror(errno));
        return -1;
    }

    while (fgets(line, sizeof line, f)) {
        char *s, *eq, *key, *val;
        int r;

        lineno++;
        if (!strchr(line, '\n') && !feof(f)) {
            snprintf(errmsg, errlen, "%s:%u: line too long (max %zu)", path, lineno, sizeof line - 2);
            goto cleanup;
        }
        if ((s = strchr(line, '#'))) {
            *s = '\0';
        }
        s = trim(line);
        if (!*s) {
            continue;
        }
        eq = strchr(s, '=');
        if (!eq) {
            snprintf(errmsg, errlen, "%s:%u: expected \"key = value\"", path, lineno);
            goto cleanup;
        }
        *eq = '\0';
        key = trim(s);
        val = trim(eq + 1);
        if (!*key || !*val) {
            snprintf(errmsg, errlen, "%s:%u: empty key or value", path, lineno);
            goto cleanup;
        }

        r = set_key(c, key, val);
        if (r == -1) {
            snprintf(errmsg, errlen, "%s:%u: invalid value \"%s\" for %s", path, lineno, val, key);
            goto cleanup;
        } else if (r == -2) {
            snprintf(errmsg, errlen, "%s:%u: unknown key \"%s\"", path, lineno, key);
            goto cleanup;
        } else if (r == -3) {
            snprintf(errmsg, errlen, "%s:%u: out of memory", path, lineno);
            goto cleanup;
        }
    }
    if (ferror(f)) {
        snprintf(errmsg, errlen, "%s: read error: %s", path, strerror(errno));
        goto cleanup;
    }

    ret = validate(c, errmsg, errlen);

cleanup:
    fclose(f);
    return ret;
}

void
rats_conf_free(struct rats_conf *c)
{
    if (!c) {
        return;
    }
    free(c->tcti);
    free(c->tpm_name);
    free(c->certificate_name);
    memset(c, 0, sizeof *c);
}
