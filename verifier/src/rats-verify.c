/*
 * rats-verify.c — tpm20-challenge-response-attestation の応答（reply.xml）を検証する Verifier（host 側）。
 * doc/design/05_challenge_response.md §5.2（検査 1〜8）、§5.3（--dump）
 *
 * 使い方:
 *   rats-verify --reply out/reply.xml --nonce out/nonce.bin \
 *       --ak enroll/ak-cert-0.pem --ak-qname enroll/ak-cert-0.qname \
 *       --golden ref-values/pcrs-sha256.json --state state/ak-cert-0.json [--dump out/]
 *   終了コード: 0 = PASS、1 = FAIL（golden が無く PASS にできない INCOMPLETE も 1）、2 = 入力の誤り
 *
 * 送った challenge.xml は入力にしない。nonce は --nonce で渡し、PCR selection の期待値は golden（ref-values）から取る。
 *
 * 依存: expat（XML）、json-c（golden、state、結果の出力）、tss2-mu（TPM2B_ATTEST / TPMS_ATTEST / TPMT_SIGNATURE の unmarshal）、
 *       libcrypto（署名の検証、ハッシュ）。
 * 参考: ~/github/tpm2-tools/tools/misc/tpm2_checkquote.c（署名の検証、nonce、magic、pcrDigest の照合の手順）
 *
 * 検査（1 つでも失敗したら FAIL。途中で失敗しても全項目の結果を出してから終了する）:
 *   1 signature        TPMS_ATTEST（TPM2B_ATTEST の size を除いた部分）のハッシュを AK の公開鍵で検証する
 *   2 magic_type       magic == TPM2_GENERATED_VALUE、type == TPM2_ST_ATTEST_QUOTE
 *   3 nonce            extraData == nonce.bin
 *   4 pcr_select       pcrSelect が unsigned-pcr-values と一致し、golden が要求する PCR をすべて含む
 *   5 pcr_digest       unsigned-pcr-values を selection の順に連結したハッシュ == pcrDigest
 *   6 golden           5 が通った PCR 値 == golden（golden に無い PCR は「未評価」。golden が無ければ PASS にしない）
 *   7 clock            safe == 1。前回の state と比べて resetCount / clock が巻き戻っていない（resetCount の増加は「再起動を検出」）
 *   8 qualified_signer qualifiedSigner == ak-cert-0.qname
 *
 * openssh-portable/auth-options.c の verify() にあった欠陥（署名の失敗で続行、pcrDigest の未照合、golden の TOFU）は繰り返さない（§5.4）。
 */
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <expat.h>
#include <json-c/json.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <tss2/tss2_mu.h>
#include <tss2/tss2_tpm2_types.h>

#define NS_TPM  "urn:ietf:params:xml:ns:yang:ietf-tpm-remote-attestation"
#define NS_TAA  "urn:ietf:params:xml:ns:yang:ietf-tcg-algs"
#define NS_NC   "urn:ietf:params:xml:ns:netconf:base:1.0"

#define EXIT_PASS   0
#define EXIT_FAIL   1
#define EXIT_USAGE  2

#define MAX_FILE    (16u * 1024 * 1024)     /* 読み込むファイルの上限 */
#define MAX_PCR_VALUES (TPM2_NUM_PCR_BANKS * TPM2_MAX_PCRS)

/* ------------------------------------------------------------------------------------------------------------------
 * 共通のユーティリティ
 * ---------------------------------------------------------------------------------------------------------------- */

static const char *progname = "rats-verify";

static void
die(int code, const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "%s: ", progname);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(code);
}

static void *
xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);

    if (!p) {
        die(EXIT_USAGE, "out of memory");
    }
    return p;
}

static void *
xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n ? n : 1, sz ? sz : 1);

    if (!p) {
        die(EXIT_USAGE, "out of memory");
    }
    return p;
}

static char *
xstrdup(const char *s)
{
    char *p = xmalloc(strlen(s) + 1);

    return strcpy(p, s);
}

/* ファイル全体を読む（上限 MAX_FILE）。失敗は入力の誤りとして終了する */
static uint8_t *
read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    size_t cap = 65536, n = 0, r;

    if (!f) {
        die(EXIT_USAGE, "cannot open %s: %s", path, strerror(errno));
    }
    buf = xmalloc(cap);
    while ((r = fread(buf + n, 1, cap - n, f)) > 0) {
        n += r;
        if (n == cap) {
            if (cap >= MAX_FILE) {
                die(EXIT_USAGE, "%s: larger than %u bytes", path, MAX_FILE);
            }
            cap *= 2;
            buf = realloc(buf, cap);
            if (!buf) {
                die(EXIT_USAGE, "out of memory");
            }
        }
    }
    if (ferror(f)) {
        die(EXIT_USAGE, "read %s: %s", path, strerror(errno));
    }
    fclose(f);
    *len = n;
    return buf;
}

/* 書き込みは一時ファイルに書いてから rename する（途中で止まっても元のファイルが壊れないように） */
static int
write_file(const char *path, const void *data, size_t len, char *err, size_t errlen)
{
    char tmp[4096];
    FILE *f;

    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) {
        snprintf(err, errlen, "path too long");
        return -1;
    }
    f = fopen(tmp, "wb");
    if (!f) {
        snprintf(err, errlen, "cannot create %s: %s", tmp, strerror(errno));
        return -1;
    }
    if ((len && fwrite(data, 1, len, f) != len) || fflush(f) || fsync(fileno(f)) || fclose(f)) {
        snprintf(err, errlen, "write %s: %s", tmp, strerror(errno));
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path)) {
        snprintf(err, errlen, "rename %s -> %s: %s", tmp, path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

static void
to_hex(const uint8_t *data, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < len; i++) {
        out[2 * i] = digits[data[i] >> 4];
        out[2 * i + 1] = digits[data[i] & 0x0f];
    }
    out[2 * len] = '\0';
}

static int
hex_val(int c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* 偶数桁の hex を復号する。成功なら書き込んだ長さ、失敗なら -1 */
static int
from_hex(const char *s, uint8_t *out, size_t outmax)
{
    size_t n = strlen(s), i;

    if (n % 2 || n / 2 > outmax) {
        return -1;
    }
    for (i = 0; i < n / 2; i++) {
        int hi = hex_val(s[2 * i]), lo = hex_val(s[2 * i + 1]);

        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return (int)(n / 2);
}

/* RFC 4648 §4 の base64 を復号する（YANG の binary の XML 表現。RFC 7950 §9.8.2）。空白は無視し、それ以外の不正な文字と
 * 不正な padding は拒否する。成功なら書き込んだ長さ、失敗なら -1 */
static int
base64_decode(const char *s, uint8_t *out, size_t outmax)
{
    uint32_t acc = 0;
    int bits = 0, pad = 0;
    size_t n = 0;

    for (; *s; s++) {
        int v;
        unsigned char c = (unsigned char)*s;

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            continue;
        }
        if (c == '=') {
            pad++;
            continue;
        }
        if (pad) {
            return -1;      /* '=' の後に文字がある */
        }
        if (c >= 'A' && c <= 'Z') {
            v = c - 'A';
        } else if (c >= 'a' && c <= 'z') {
            v = c - 'a' + 26;
        } else if (c >= '0' && c <= '9') {
            v = c - '0' + 52;
        } else if (c == '+') {
            v = 62;
        } else if (c == '/') {
            v = 63;
        } else {
            return -1;
        }
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= outmax) {
                return -1;
            }
            out[n++] = (uint8_t)(acc >> bits);
            acc &= (1u << bits) - 1;
        }
    }
    /* 文字数 (mod 4) と padding の整合: 残りビットは 0 で、残り 2 ビットなら '=' が 1 つ、4 ビットなら 2 つ */
    if (pad > 2 || (bits == 0 && pad != 0) || (bits == 2 && pad != 1) || (bits == 4 && pad != 2) || acc != 0) {
        return -1;
    }
    return (int)n;
}

/* 前後の空白を取り除いた複製を返す */
static char *
trimmed(const char *s)
{
    const char *e;
    char *out;

    if (!s) {
        s = "";
    }
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) {
        e--;
    }
    out = xmalloc((size_t)(e - s) + 1);
    memcpy(out, s, (size_t)(e - s));
    out[e - s] = '\0';
    return out;
}

/* buf（大きさ size、今の長さ *len）に printf 形式で追記する。あふれた分は切り捨てる */
static void
sappend(char *buf, size_t size, size_t *len, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (*len >= size) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(buf + *len, size - *len, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    *len = (size_t)n >= size - *len ? size - 1 : *len + (size_t)n;
}

/* ハッシュアルゴリズム（TPM2_ALG_ID ↔ 名前、digest の長さ、OpenSSL の EVP_MD） */
static const struct alg {
    TPM2_ALG_ID id;
    const char *name;        /* golden の "hash"、tpm2_checkquote の -l / -g で使う名前 */
    const char *yang;        /* ietf-tcg-algs の identity 名 */
    uint16_t size;
    const EVP_MD *(*md)(void);
} algs[] = {
    { TPM2_ALG_SHA1,   "sha1",   "TPM_ALG_SHA1",   TPM2_SHA1_DIGEST_SIZE,   EVP_sha1 },
    { TPM2_ALG_SHA256, "sha256", "TPM_ALG_SHA256", TPM2_SHA256_DIGEST_SIZE, EVP_sha256 },
    { TPM2_ALG_SHA384, "sha384", "TPM_ALG_SHA384", TPM2_SHA384_DIGEST_SIZE, EVP_sha384 },
    { TPM2_ALG_SHA512, "sha512", "TPM_ALG_SHA512", TPM2_SHA512_DIGEST_SIZE, EVP_sha512 },
};

static const struct alg *
alg_by_id(TPM2_ALG_ID id)
{
    size_t i;

    for (i = 0; i < sizeof algs / sizeof algs[0]; i++) {
        if (algs[i].id == id) {
            return &algs[i];
        }
    }
    return NULL;
}

static const struct alg *
alg_by_name(const char *name)
{
    size_t i;

    for (i = 0; i < sizeof algs / sizeof algs[0]; i++) {
        if (!strcmp(algs[i].name, name)) {
            return &algs[i];
        }
    }
    return NULL;
}

static const struct alg *
alg_by_yang(const char *identity)
{
    size_t i;

    for (i = 0; i < sizeof algs / sizeof algs[0]; i++) {
        if (!strcmp(algs[i].yang, identity)) {
            return &algs[i];
        }
    }
    return NULL;
}

static const char *
alg_name(TPM2_ALG_ID id, char *buf, size_t buflen)
{
    const struct alg *a = alg_by_id(id);

    if (a) {
        return a->name;
    }
    snprintf(buf, buflen, "0x%04x", id);
    return buf;
}

/* ------------------------------------------------------------------------------------------------------------------
 * XML: expat で小さな木を作る（名前空間付き）。識別子の prefix を解決するために xmlns の宣言も要素に残す
 * ---------------------------------------------------------------------------------------------------------------- */

#define NS_SEP ' '      /* XML_ParserCreateNS の区切り。URI に空白は現れない */

struct nsdecl {
    char *prefix;           /* NULL = 既定の名前空間 */
    char *uri;
    struct nsdecl *next;
};

struct xnode {
    char *ns;               /* 名前空間 URI（無ければ "" ） */
    char *name;             /* ローカル名 */
    char *text;             /* 文字データ（連結したもの） */
    size_t textlen;
    struct nsdecl *decls;
    struct xnode *parent, *first_child, *last_child, *next;
};

struct xml_parse {
    XML_Parser parser;
    struct xnode *root, *cur;
    struct nsdecl *pending;             /* 次の要素に付ける xmlns の宣言 */
    char error[256];
};

static void
xnode_free(struct xnode *n)
{
    struct xnode *c, *next;
    struct nsdecl *d, *dn;

    if (!n) {
        return;
    }
    for (c = n->first_child; c; c = next) {
        next = c->next;
        xnode_free(c);
    }
    for (d = n->decls; d; d = dn) {
        dn = d->next;
        free(d->prefix);
        free(d->uri);
        free(d);
    }
    free(n->ns);
    free(n->name);
    free(n->text);
    free(n);
}

static void XMLCALL
xml_ns_start(void *ud, const XML_Char *prefix, const XML_Char *uri)
{
    struct xml_parse *xp = ud;
    struct nsdecl *d = xcalloc(1, sizeof *d);

    d->prefix = prefix ? xstrdup(prefix) : NULL;
    d->uri = xstrdup(uri ? uri : "");
    d->next = xp->pending;
    xp->pending = d;
}

static void XMLCALL
xml_start(void *ud, const XML_Char *name, const XML_Char **atts)
{
    struct xml_parse *xp = ud;
    struct xnode *n = xcalloc(1, sizeof *n);
    const char *sep = strchr(name, NS_SEP);

    (void)atts;
    if (sep) {
        n->ns = xmalloc((size_t)(sep - name) + 1);
        memcpy(n->ns, name, (size_t)(sep - name));
        n->ns[sep - name] = '\0';
        n->name = xstrdup(sep + 1);
    } else {
        n->ns = xstrdup("");
        n->name = xstrdup(name);
    }
    n->decls = xp->pending;
    xp->pending = NULL;
    n->parent = xp->cur;
    if (xp->cur) {
        if (xp->cur->last_child) {
            xp->cur->last_child->next = n;
        } else {
            xp->cur->first_child = n;
        }
        xp->cur->last_child = n;
    } else {
        xp->root = n;
    }
    xp->cur = n;
}

static void XMLCALL
xml_end(void *ud, const XML_Char *name)
{
    struct xml_parse *xp = ud;

    (void)name;
    if (xp->cur) {
        xp->cur = xp->cur->parent;
    }
}

static void XMLCALL
xml_chars(void *ud, const XML_Char *s, int len)
{
    struct xml_parse *xp = ud;
    struct xnode *n = xp->cur;

    if (!n || len <= 0) {
        return;
    }
    n->text = realloc(n->text, n->textlen + (size_t)len + 1);
    if (!n->text) {
        die(EXIT_USAGE, "out of memory");
    }
    memcpy(n->text + n->textlen, s, (size_t)len);
    n->textlen += (size_t)len;
    n->text[n->textlen] = '\0';
}

/* NETCONF の XML に DOCTYPE は現れない。実体の展開による攻撃を避けるため、あれば拒否する */
static void XMLCALL
xml_doctype(void *ud, const XML_Char *doctype_name, const XML_Char *sysid, const XML_Char *pubid, int has_internal_subset)
{
    struct xml_parse *xp = ud;

    (void)doctype_name; (void)sysid; (void)pubid; (void)has_internal_subset;
    snprintf(xp->error, sizeof xp->error, "DOCTYPE is not allowed");
    XML_StopParser(xp->parser, XML_FALSE);
}

/* XML 全体を木にする。失敗なら NULL を返し、err に理由を書く */
static struct xnode *
xml_parse(const uint8_t *data, size_t len, char *err, size_t errlen)
{
    struct xml_parse xp;
    struct xnode *root;

    memset(&xp, 0, sizeof xp);
    xp.parser = XML_ParserCreateNS(NULL, NS_SEP);
    if (!xp.parser) {
        die(EXIT_USAGE, "XML_ParserCreateNS failed");
    }
    XML_SetUserData(xp.parser, &xp);
    XML_SetElementHandler(xp.parser, xml_start, xml_end);
    XML_SetCharacterDataHandler(xp.parser, xml_chars);
    XML_SetNamespaceDeclHandler(xp.parser, xml_ns_start, NULL);
    XML_SetStartDoctypeDeclHandler(xp.parser, xml_doctype);

    if (len > (size_t)INT32_MAX || XML_Parse(xp.parser, (const char *)data, (int)len, XML_TRUE) != XML_STATUS_OK) {
        if (xp.error[0]) {
            snprintf(err, errlen, "%s", xp.error);
        } else {
            snprintf(err, errlen, "line %lu: %s", XML_GetCurrentLineNumber(xp.parser),
                     XML_ErrorString(XML_GetErrorCode(xp.parser)));
        }
        XML_ParserFree(xp.parser);
        xnode_free(xp.root);
        return NULL;
    }
    XML_ParserFree(xp.parser);
    root = xp.root;
    if (!root) {
        snprintf(err, errlen, "no root element");
    }
    return root;
}

/* 直下の子要素を探す（ns が NULL なら名前空間を問わない） */
static struct xnode *
xml_child(const struct xnode *n, const char *ns, const char *name)
{
    struct xnode *c;

    for (c = n ? n->first_child : NULL; c; c = c->next) {
        if (!strcmp(c->name, name) && (!ns || !strcmp(c->ns, ns))) {
            return c;
        }
    }
    return NULL;
}

/* 同じ名前の次の兄弟要素 */
static struct xnode *
xml_next(const struct xnode *n, const char *ns, const char *name)
{
    struct xnode *c;

    for (c = n ? n->next : NULL; c; c = c->next) {
        if (!strcmp(c->name, name) && (!ns || !strcmp(c->ns, ns))) {
            return c;
        }
    }
    return NULL;
}

/* 深さを問わず、最初に見つかった要素（深さ優先） */
static struct xnode *
xml_find(struct xnode *n, const char *ns, const char *name)
{
    struct xnode *c, *r;

    if (!n) {
        return NULL;
    }
    if (!strcmp(n->name, name) && (!ns || !strcmp(n->ns, ns))) {
        return n;
    }
    for (c = n->first_child; c; c = c->next) {
        r = xml_find(c, ns, name);
        if (r) {
            return r;
        }
    }
    return NULL;
}

/* 子要素の文字データ（前後の空白を除く）。無ければ NULL。返り値は free すること */
static char *
xml_child_text(const struct xnode *n, const char *name)
{
    struct xnode *c = xml_child(n, NS_TPM, name);

    return c ? trimmed(c->text) : NULL;
}

/* prefix（NULL なら既定の名前空間）を、その要素と祖先の xmlns 宣言で解決する */
static const char *
xml_resolve_prefix(const struct xnode *n, const char *prefix)
{
    for (; n; n = n->parent) {
        const struct nsdecl *d;

        for (d = n->decls; d; d = d->next) {
            if ((!prefix && !d->prefix) || (prefix && d->prefix && !strcmp(prefix, d->prefix))) {
                return d->uri;
            }
        }
    }
    return NULL;
}

/* identityref（RFC 7950 §9.10.3: "prefix:identity"。prefix が無ければ既定の名前空間）を ietf-tcg-algs の identity として読む。
 * 成功なら algs[] の項目、失敗なら NULL（err に理由） */
static const struct alg *
xml_hash_algo(const struct xnode *leaf, char *err, size_t errlen)
{
    char *v = trimmed(leaf->text), *colon = strchr(v, ':');
    const char *ns, *local = v;
    const struct alg *a = NULL;

    if (colon) {
        *colon = '\0';
        local = colon + 1;
        ns = xml_resolve_prefix(leaf, v);
    } else {
        ns = xml_resolve_prefix(leaf, NULL);
    }
    if (!ns) {
        snprintf(err, errlen, "tpm20-hash-algo \"%s\": prefix not declared", leaf->text ? leaf->text : "");
    } else if (strcmp(ns, NS_TAA)) {
        snprintf(err, errlen, "tpm20-hash-algo \"%s\": namespace %s is not ietf-tcg-algs", local, ns);
    } else if (!(a = alg_by_yang(local))) {
        snprintf(err, errlen, "tpm20-hash-algo \"%s\": unsupported identity", local);
    }
    free(v);
    return a;
}

/* ------------------------------------------------------------------------------------------------------------------
 * 検証の文脈と結果
 * ---------------------------------------------------------------------------------------------------------------- */

enum check_id {
    CK_SIGNATURE, CK_MAGIC_TYPE, CK_NONCE, CK_PCR_SELECT, CK_PCR_DIGEST, CK_GOLDEN, CK_CLOCK, CK_QUALIFIED_SIGNER, CK_COUNT
};

static const char *check_names[CK_COUNT] = {
    "signature", "magic_type", "nonce", "pcr_select", "pcr_digest", "golden", "clock", "qualified_signer"
};

enum check_state { CK_PENDING, CK_OK, CK_FAIL, CK_SKIP };

struct check {
    enum check_state state;
    char text[512];
};

/* unsigned-pcr-values の 1 つの値 */
struct pcr_value {
    TPM2_ALG_ID alg;
    uint8_t index;
    uint8_t value[sizeof(TPMU_HA)];
    uint16_t len;
};

struct opts {
    const char *reply, *nonce, *ak, *ak_qname, *golden, *state, *dump;
};

struct verify_ctx {
    struct opts opt;
    struct check checks[CK_COUNT];

    /* Verifier 側の入力 */
    uint8_t nonce[sizeof(TPMU_HA)];
    size_t nonce_len;
    EVP_PKEY *ak;
    uint8_t qname[sizeof(TPMU_NAME)];
    size_t qname_len;
    struct json_object *golden;         /* NULL なら無し */
    struct json_object *prev_state;     /* NULL なら初回（または --state 無し） */

    /* 応答から取り出したもの */
    char *certificate_name;
    uint32_t up_time;
    int have_up_time;
    uint8_t *quote_data;                /* TPM2B_ATTEST（marshal されたバイト列） */
    size_t quote_data_len;
    uint8_t *quote_sig;                 /* TPMT_SIGNATURE（marshal されたバイト列） */
    size_t quote_sig_len;
    struct pcr_value values[MAX_PCR_VALUES];
    size_t nvalues;

    /* unmarshal の結果 */
    TPM2B_ATTEST attest2b;
    TPMS_ATTEST attest;
    TPMT_SIGNATURE sig;
    const struct alg *sig_alg;          /* 署名のハッシュ（pcrDigest もこれで計算される） */
    int structures_ok;                  /* quote-data と quote-signature を unmarshal できた */

    /* 検査の途中で作る値 */
    uint8_t pcr_concat[MAX_PCR_VALUES * sizeof(TPMU_HA)];
    size_t pcr_concat_len;
    int values_complete;                /* selection の各 PCR の値がそろっている（検査 4 で確認） */
    char pcr_select_str[1024];          /* "sha256:0,1,2,..." */
};

static void
set_check(struct verify_ctx *ctx, enum check_id id, enum check_state state, const char *fmt, ...)
{
    va_list ap;

    ctx->checks[id].state = state;
    va_start(ap, fmt);
    vsnprintf(ctx->checks[id].text, sizeof ctx->checks[id].text, fmt, ap);
    va_end(ap);
}

#define ck_ok(ctx, id, ...)   set_check(ctx, id, CK_OK, "ok " __VA_ARGS__)
#define ck_fail(ctx, id, ...) set_check(ctx, id, CK_FAIL, "FAIL " __VA_ARGS__)
#define ck_skip(ctx, id, ...) set_check(ctx, id, CK_SKIP, "not evaluated " __VA_ARGS__)

/* ------------------------------------------------------------------------------------------------------------------
 * 応答の読み取り
 * ---------------------------------------------------------------------------------------------------------------- */

/* base64 の leaf を復号して malloc した領域に返す。失敗なら NULL（err に理由） */
static uint8_t *
decode_binary_leaf(const struct xnode *parent, const char *name, size_t *len, char *err, size_t errlen)
{
    struct xnode *leaf = xml_child(parent, NS_TPM, name);
    uint8_t *buf;
    int n;

    if (!leaf) {
        snprintf(err, errlen, "%s is missing", name);
        return NULL;
    }
    buf = xmalloc(leaf->textlen + 1);
    n = base64_decode(leaf->text ? leaf->text : "", buf, leaf->textlen + 1);
    if (n < 0) {
        free(buf);
        snprintf(err, errlen, "%s is not valid base64", name);
        return NULL;
    }
    *len = (size_t)n;
    return buf;
}

/* <rpc-error> なら、その内容を err に書いて 1 を返す */
static int
reply_is_rpc_error(struct xnode *root, char *err, size_t errlen)
{
    struct xnode *e = xml_find(root, NS_NC, "rpc-error");
    struct xnode *tag, *msg, *path;

    if (!e) {
        return 0;
    }
    tag = xml_child(e, NS_NC, "error-tag");
    msg = xml_child(e, NS_NC, "error-message");
    path = xml_child(e, NS_NC, "error-path");
    snprintf(err, errlen, "rpc-error: tag=%s message=%s path=%s",
             tag && tag->text ? tag->text : "?", msg && msg->text ? msg->text : "", path && path->text ? path->text : "");
    return 1;
}

/* reply.xml から tpm20-attestation-response を取り出す。<rpc-reply> 付き、libyang の reply 形式
 * （<tpm20-challenge-response-attestation> の下）、netopeer2-cli の --out（応答の要素だけ）のどれでもよい。
 * 応答の形が悪いときは err に理由を書いて -1 を返す（検証は FAIL になる）。入力の誤りは終了する */
static int
load_reply(struct verify_ctx *ctx, struct xnode **root_out, char *err, size_t errlen)
{
    size_t len;
    uint8_t *data = read_file(ctx->opt.reply, &len);
    struct xnode *root, *resp, *upv;
    char *s;

    root = xml_parse(data, len, err, errlen);
    free(data);
    if (!root) {
        die(EXIT_USAGE, "%s: XML parse error: %s", ctx->opt.reply, err);
    }
    *root_out = root;

    if (reply_is_rpc_error(root, err, errlen)) {
        return -1;
    }
    resp = xml_find(root, NS_TPM, "tpm20-attestation-response");
    if (!resp) {
        die(EXIT_USAGE, "%s: no tpm20-attestation-response element (namespace %s)", ctx->opt.reply, NS_TPM);
    }
    if (xml_next(resp, NS_TPM, "tpm20-attestation-response")) {
        die(EXIT_USAGE, "%s: more than one tpm20-attestation-response (feature mtpm is not supported)", ctx->opt.reply);
    }

    ctx->certificate_name = xml_child_text(resp, "certificate-name");
    if (!ctx->certificate_name) {
        snprintf(err, errlen, "certificate-name is missing");
        return -1;
    }
    s = xml_child_text(resp, "up-time");
    if (s) {
        char *end;
        unsigned long v = strtoul(s, &end, 10);

        if (*s && !*end && v <= UINT32_MAX) {
            ctx->up_time = (uint32_t)v;
            ctx->have_up_time = 1;
        }
        free(s);
    }
    ctx->quote_data = decode_binary_leaf(resp, "quote-data", &ctx->quote_data_len, err, errlen);
    if (!ctx->quote_data) {
        return -1;
    }
    ctx->quote_sig = decode_binary_leaf(resp, "quote-signature", &ctx->quote_sig_len, err, errlen);
    if (!ctx->quote_sig) {
        return -1;
    }

    /* unsigned-pcr-values（bank ごと）→ pcr-values（PCR ごと） */
    for (upv = xml_child(resp, NS_TPM, "unsigned-pcr-values"); upv; upv = xml_next(upv, NS_TPM, "unsigned-pcr-values")) {
        struct xnode *algo = xml_child(upv, NS_TPM, "tpm20-hash-algo"), *pv;
        const struct alg *a;

        if (!algo) {
            snprintf(err, errlen, "unsigned-pcr-values without tpm20-hash-algo");
            return -1;
        }
        a = xml_hash_algo(algo, err, errlen);
        if (!a) {
            return -1;
        }
        for (pv = xml_child(upv, NS_TPM, "pcr-values"); pv; pv = xml_next(pv, NS_TPM, "pcr-values")) {
            struct pcr_value *v;
            char *idx = xml_child_text(pv, "pcr-index"), *end;
            unsigned long i;
            struct xnode *val = xml_child(pv, NS_TPM, "pcr-value");
            int n;

            if (!idx) {
                snprintf(err, errlen, "pcr-values without pcr-index");
                return -1;
            }
            i = strtoul(idx, &end, 10);
            if (!*idx || *end || i >= TPM2_MAX_PCRS) {
                snprintf(err, errlen, "pcr-index \"%s\" is not 0..%d", idx, TPM2_MAX_PCRS - 1);
                free(idx);
                return -1;
            }
            free(idx);
            if (!val) {
                snprintf(err, errlen, "PCR %lu (%s): pcr-value is missing", i, a->name);
                return -1;
            }
            if (ctx->nvalues >= MAX_PCR_VALUES) {
                snprintf(err, errlen, "too many pcr-values");
                return -1;
            }
            v = &ctx->values[ctx->nvalues];
            v->alg = a->id;
            v->index = (uint8_t)i;
            n = base64_decode(val->text ? val->text : "", v->value, sizeof v->value);
            if (n < 0) {
                snprintf(err, errlen, "PCR %lu (%s): pcr-value is not valid base64 or longer than %zu bytes", i, a->name,
                         sizeof v->value);
                return -1;
            }
            v->len = (uint16_t)n;
            ctx->nvalues++;
        }
    }
    return 0;
}

/* quote-data → TPM2B_ATTEST → TPMS_ATTEST、quote-signature → TPMT_SIGNATURE（tpm2_checkquote の
 * files_tpm2b_attest_to_tpms_attest / tpm2_convert_sig_load_plain と同じ unmarshal）。余りのバイトは拒否する */
static int
unmarshal_structures(struct verify_ctx *ctx, char *err, size_t errlen)
{
    size_t off = 0;
    TSS2_RC rc;

    rc = Tss2_MU_TPM2B_ATTEST_Unmarshal(ctx->quote_data, ctx->quote_data_len, &off, &ctx->attest2b);
    if (rc) {
        snprintf(err, errlen, "quote-data: TPM2B_ATTEST unmarshal failed (0x%08" PRIx32 ")", rc);
        return -1;
    }
    if (off != ctx->quote_data_len) {
        snprintf(err, errlen, "quote-data: %zu trailing bytes after TPM2B_ATTEST", ctx->quote_data_len - off);
        return -1;
    }
    off = 0;
    rc = Tss2_MU_TPMS_ATTEST_Unmarshal(ctx->attest2b.attestationData, ctx->attest2b.size, &off, &ctx->attest);
    if (rc) {
        snprintf(err, errlen, "quote-data: TPMS_ATTEST unmarshal failed (0x%08" PRIx32 ")", rc);
        return -1;
    }
    if (off != ctx->attest2b.size) {
        snprintf(err, errlen, "quote-data: %zu trailing bytes in TPMS_ATTEST", (size_t)ctx->attest2b.size - off);
        return -1;
    }

    off = 0;
    rc = Tss2_MU_TPMT_SIGNATURE_Unmarshal(ctx->quote_sig, ctx->quote_sig_len, &off, &ctx->sig);
    if (rc) {
        snprintf(err, errlen, "quote-signature: TPMT_SIGNATURE unmarshal failed (0x%08" PRIx32 ")", rc);
        return -1;
    }
    if (off != ctx->quote_sig_len) {
        snprintf(err, errlen, "quote-signature: %zu trailing bytes after TPMT_SIGNATURE", ctx->quote_sig_len - off);
        return -1;
    }
    switch (ctx->sig.sigAlg) {
    case TPM2_ALG_ECDSA:
        ctx->sig_alg = alg_by_id(ctx->sig.signature.ecdsa.hash);
        break;
    case TPM2_ALG_RSASSA:
        ctx->sig_alg = alg_by_id(ctx->sig.signature.rsassa.hash);
        break;
    case TPM2_ALG_RSAPSS:
        ctx->sig_alg = alg_by_id(ctx->sig.signature.rsapss.hash);
        break;
    default:
        snprintf(err, errlen, "quote-signature: unsupported sigAlg 0x%04x (ecdsa, rsassa, rsapss)", ctx->sig.sigAlg);
        return -1;
    }
    if (!ctx->sig_alg) {
        snprintf(err, errlen, "quote-signature: unsupported hash algorithm");
        return -1;
    }
    ctx->structures_ok = 1;
    return 0;
}

/* ------------------------------------------------------------------------------------------------------------------
 * 検査 1〜8
 * ---------------------------------------------------------------------------------------------------------------- */

/* 1 署名。tpm2_checkquote の verify() と同じく、TPMS_ATTEST のハッシュに対して EVP_PKEY_verify を行う。
 * padding は推測せず、TPMT_SIGNATURE の sigAlg に従う */
static void
check_signature(struct verify_ctx *ctx)
{
    const EVP_MD *md = ctx->sig_alg->md();
    uint8_t dgst[EVP_MAX_MD_SIZE];
    unsigned dlen = 0;
    EVP_PKEY_CTX *pctx = NULL;
    const uint8_t *sig = NULL;
    size_t siglen = 0;
    uint8_t *der = NULL;
    int rc, key_id = EVP_PKEY_base_id(ctx->ak);
    const char *sig_name = "?";

    if (!EVP_Digest(ctx->attest2b.attestationData, ctx->attest2b.size, dgst, &dlen, md, NULL)) {
        ck_fail(ctx, CK_SIGNATURE, "(EVP_Digest: %s)", ERR_error_string(ERR_get_error(), NULL));
        return;
    }
    pctx = EVP_PKEY_CTX_new(ctx->ak, NULL);
    if (!pctx || EVP_PKEY_verify_init(pctx) <= 0 || EVP_PKEY_CTX_set_signature_md(pctx, md) <= 0) {
        ck_fail(ctx, CK_SIGNATURE, "(EVP_PKEY_CTX setup: %s)", ERR_error_string(ERR_get_error(), NULL));
        goto out;
    }

    switch (ctx->sig.sigAlg) {
    case TPM2_ALG_ECDSA: {
        const TPMS_SIGNATURE_ECC *e = &ctx->sig.signature.ecdsa;
        ECDSA_SIG *es;
        BIGNUM *r, *s;
        int derlen;

        sig_name = "ECDSA";
        if (key_id != EVP_PKEY_EC) {
            ck_fail(ctx, CK_SIGNATURE, "(signature is ECDSA but the AK public key is not an EC key)");
            goto out;
        }
        es = ECDSA_SIG_new();
        r = BN_bin2bn(e->signatureR.buffer, e->signatureR.size, NULL);
        s = BN_bin2bn(e->signatureS.buffer, e->signatureS.size, NULL);
        if (!es || !r || !s || !ECDSA_SIG_set0(es, r, s)) {
            ck_fail(ctx, CK_SIGNATURE, "(ECDSA_SIG: %s)", ERR_error_string(ERR_get_error(), NULL));
            ECDSA_SIG_free(es);
            goto out;
        }
        derlen = i2d_ECDSA_SIG(es, &der);      /* (r, s) を DER の SEQUENCE にする */
        ECDSA_SIG_free(es);
        if (derlen <= 0) {
            ck_fail(ctx, CK_SIGNATURE, "(i2d_ECDSA_SIG: %s)", ERR_error_string(ERR_get_error(), NULL));
            goto out;
        }
        sig = der;
        siglen = (size_t)derlen;
        break;
    }
    case TPM2_ALG_RSASSA:
    case TPM2_ALG_RSAPSS: {
        const TPMS_SIGNATURE_RSA *rs = &ctx->sig.signature.rsassa;
        int pss = ctx->sig.sigAlg == TPM2_ALG_RSAPSS;

        sig_name = pss ? "RSAPSS" : "RSASSA";
        if (key_id != EVP_PKEY_RSA) {
            ck_fail(ctx, CK_SIGNATURE, "(signature is %s but the AK public key is not an RSA key)", sig_name);
            goto out;
        }
        if (EVP_PKEY_CTX_set_rsa_padding(pctx, pss ? RSA_PKCS1_PSS_PADDING : RSA_PKCS1_PADDING) <= 0 ||
            (pss && EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_AUTO) <= 0)) {
            ck_fail(ctx, CK_SIGNATURE, "(RSA padding: %s)", ERR_error_string(ERR_get_error(), NULL));
            goto out;
        }
        sig = rs->sig.buffer;
        siglen = rs->sig.size;
        break;
    }
    default:
        ck_fail(ctx, CK_SIGNATURE, "(unsupported sigAlg 0x%04x)", ctx->sig.sigAlg);
        goto out;
    }

    rc = EVP_PKEY_verify(pctx, sig, siglen, dgst, dlen);
    if (rc == 1) {
        ck_ok(ctx, CK_SIGNATURE, "(%s/%s over TPMS_ATTEST, %u bytes, with %s)", sig_name, ctx->sig_alg->name,
              ctx->attest2b.size, ctx->opt.ak);
    } else if (rc == 0) {
        ck_fail(ctx, CK_SIGNATURE, "(%s/%s signature does not verify with %s)", sig_name, ctx->sig_alg->name, ctx->opt.ak);
    } else {
        ck_fail(ctx, CK_SIGNATURE, "(EVP_PKEY_verify: %s)", ERR_error_string(ERR_get_error(), NULL));
    }
out:
    OPENSSL_free(der);
    EVP_PKEY_CTX_free(pctx);
}

/* 2 magic と type */
static void
check_magic_type(struct verify_ctx *ctx)
{
    if (ctx->attest.magic != TPM2_GENERATED_VALUE) {
        ck_fail(ctx, CK_MAGIC_TYPE, "(magic 0x%08" PRIx32 " != TPM_GENERATED_VALUE 0x%08" PRIx32 ")", ctx->attest.magic,
                (uint32_t)TPM2_GENERATED_VALUE);
    } else if (ctx->attest.type != TPM2_ST_ATTEST_QUOTE) {
        ck_fail(ctx, CK_MAGIC_TYPE, "(type 0x%04x != TPM_ST_ATTEST_QUOTE 0x%04x)", ctx->attest.type, TPM2_ST_ATTEST_QUOTE);
    } else {
        ck_ok(ctx, CK_MAGIC_TYPE, "(magic 0x%08" PRIx32 ", type 0x%04x quote)", ctx->attest.magic, ctx->attest.type);
    }
}

/* 3 nonce（extraData） */
static void
check_nonce(struct verify_ctx *ctx)
{
    const TPM2B_DATA *x = &ctx->attest.extraData;

    if (x->size != ctx->nonce_len || memcmp(x->buffer, ctx->nonce, ctx->nonce_len)) {
        char got[2 * sizeof(TPMU_HA) + 1];

        to_hex(x->buffer, x->size, got);
        ck_fail(ctx, CK_NONCE, "(extraData %s (%u bytes) != %s (%zu bytes): replay or wrong nonce file)", got, x->size,
                ctx->opt.nonce, ctx->nonce_len);
    } else {
        ck_ok(ctx, CK_NONCE, "(extraData == %s, %zu bytes)", ctx->opt.nonce, ctx->nonce_len);
    }
}

static int
pcr_selected(const TPMS_PCR_SELECTION *s, unsigned i)
{
    return i / 8 < s->sizeofSelect && (s->pcrSelect[i / 8] & (1u << (i % 8)));
}

static const struct pcr_value *
find_value(const struct verify_ctx *ctx, TPM2_ALG_ID alg, unsigned index)
{
    size_t k;

    for (k = 0; k < ctx->nvalues; k++) {
        if (ctx->values[k].alg == alg && ctx->values[k].index == index) {
            return &ctx->values[k];
        }
    }
    return NULL;
}

/* golden の "pcrs" から PCR index の値（hex 文字列）を返す。無ければ NULL */
static const char *
golden_pcr(const struct verify_ctx *ctx, unsigned index)
{
    struct json_object *pcrs, *v;
    char key[8];

    if (!ctx->golden || !json_object_object_get_ex(ctx->golden, "pcrs", &pcrs)) {
        return NULL;
    }
    snprintf(key, sizeof key, "%u", index);
    if (!json_object_object_get_ex(pcrs, key, &v) || !json_object_is_type(v, json_type_string)) {
        return NULL;
    }
    return json_object_get_string(v);
}

static const struct alg *
golden_alg(const struct verify_ctx *ctx)
{
    struct json_object *h;

    if (!ctx->golden || !json_object_object_get_ex(ctx->golden, "hash", &h)) {
        return NULL;
    }
    return alg_by_name(json_object_get_string(h));
}

/* 4 pcrSelect。challenge.xml は入力にしないので、quote の selection を unsigned-pcr-values と golden の要求と照合する。
 * あわせて、検査 5 のための連結バイト列（selection の順: bank の順、各 bank の中は PCR 番号の昇順）と、
 * tpm2_checkquote の -l に渡す文字列を作る */
static void
check_pcr_select(struct verify_ctx *ctx)
{
    const TPML_PCR_SELECTION *sel = &ctx->attest.attested.quote.pcrSelect;
    char problems[400] = "", namebuf[16];
    size_t plen = 0, k, pos = 0;
    uint32_t b;
    unsigned i, nsel = 0;

#define PROBLEM(...) do { sappend(problems, sizeof problems, &plen, "%s", plen ? "; " : ""); \
                          sappend(problems, sizeof problems, &plen, __VA_ARGS__); } while (0)

    ctx->values_complete = 1;
    if (sel->count == 0 || sel->count > TPM2_NUM_PCR_BANKS) {
        ck_fail(ctx, CK_PCR_SELECT, "(pcrSelect has %" PRIu32 " banks)", sel->count);
        ctx->values_complete = 0;
        return;
    }
    for (b = 0; b < sel->count; b++) {
        const TPMS_PCR_SELECTION *s = &sel->pcrSelections[b];
        const struct alg *a = alg_by_id(s->hash);
        int first = 1;

        sappend(ctx->pcr_select_str, sizeof ctx->pcr_select_str, &pos, "%s%s:", b ? "+" : "",
                alg_name(s->hash, namebuf, sizeof namebuf));
        if (!a) {
            PROBLEM("bank %" PRIu32 ": unsupported hash 0x%04x", b, s->hash);
            ctx->values_complete = 0;
        }
        if (s->sizeofSelect > TPM2_PCR_SELECT_MAX) {
            PROBLEM("bank %s: sizeofSelect %u", alg_name(s->hash, namebuf, sizeof namebuf), s->sizeofSelect);
            ctx->values_complete = 0;
            continue;
        }
        for (i = 0; i < 8u * s->sizeofSelect; i++) {
            const struct pcr_value *v;

            if (!pcr_selected(s, i)) {
                continue;
            }
            nsel++;
            sappend(ctx->pcr_select_str, sizeof ctx->pcr_select_str, &pos, "%s%u", first ? "" : ",", i);
            first = 0;
            v = find_value(ctx, s->hash, i);
            if (!v) {
                PROBLEM("PCR %u (%s) selected but missing in unsigned-pcr-values", i, alg_name(s->hash, namebuf, sizeof namebuf));
                ctx->values_complete = 0;
            } else if (a && v->len != a->size) {
                PROBLEM("PCR %u (%s) value is %u bytes, expected %u", i, a->name, v->len, a->size);
                ctx->values_complete = 0;
            } else if (ctx->values_complete) {
                memcpy(ctx->pcr_concat + ctx->pcr_concat_len, v->value, v->len);
                ctx->pcr_concat_len += v->len;
            }
        }
        if (first) {
            PROBLEM("bank %s selects no PCR", alg_name(s->hash, namebuf, sizeof namebuf));
        }
    }

    /* unsigned-pcr-values の側に余計な PCR や重複が無いこと */
    for (k = 0; k < ctx->nvalues; k++) {
        const struct pcr_value *v = &ctx->values[k];
        int in_sel = 0;
        size_t m;

        for (b = 0; b < sel->count; b++) {
            if (sel->pcrSelections[b].hash == v->alg && pcr_selected(&sel->pcrSelections[b], v->index)) {
                in_sel = 1;
            }
        }
        if (!in_sel) {
            PROBLEM("unsigned-pcr-values has PCR %u (%s) that is not in pcrSelect", v->index, alg_name(v->alg, namebuf, sizeof namebuf));
        }
        for (m = 0; m < k; m++) {
            if (ctx->values[m].alg == v->alg && ctx->values[m].index == v->index) {
                PROBLEM("PCR %u (%s) listed twice in unsigned-pcr-values", v->index, alg_name(v->alg, namebuf, sizeof namebuf));
                ctx->values_complete = 0;
                break;
            }
        }
    }

    /* golden が要求する bank と PCR がすべて quote に含まれていること（Verifier の期待する selection） */
    if (ctx->golden) {
        const struct alg *ga = golden_alg(ctx);
        const TPMS_PCR_SELECTION *gs = NULL;
        struct json_object *pcrs;

        for (b = 0; ga && b < sel->count; b++) {
            if (sel->pcrSelections[b].hash == ga->id) {
                gs = &sel->pcrSelections[b];
            }
        }
        if (!gs) {
            PROBLEM("golden bank %s is not in pcrSelect", ga ? ga->name : "?");
        } else if (json_object_object_get_ex(ctx->golden, "pcrs", &pcrs)) {
            json_object_object_foreach(pcrs, key, val) {
                char *end;
                unsigned long gi = strtoul(key, &end, 10);

                (void)val;
                if (!*key || *end || gi >= TPM2_MAX_PCRS) {
                    PROBLEM("golden has an invalid PCR key \"%s\"", key);
                } else if (!pcr_selected(gs, (unsigned)gi)) {
                    PROBLEM("golden expects PCR %lu (%s) but it is not quoted", gi, ga->name);
                }
            }
        }
    }

    if (plen) {
        ck_fail(ctx, CK_PCR_SELECT, "(%s)", problems);
    } else {
        ck_ok(ctx, CK_PCR_SELECT, "(%s, %u PCRs, all present in unsigned-pcr-values%s)", ctx->pcr_select_str, nsel,
              ctx->golden ? ", covers golden" : "");
    }
#undef PROBLEM
}

/* 5 pcrDigest = H(selection の順に連結した PCR 値)。H は署名のハッシュ（TPM2_Quote は AK の scheme のハッシュで pcrDigest を作る。
 * tpm2_checkquote も同じ halg で tpm2_openssl_hash_pcr_banks を呼ぶ） */
static void
check_pcr_digest(struct verify_ctx *ctx)
{
    const TPM2B_DIGEST *d = &ctx->attest.attested.quote.pcrDigest;
    uint8_t calc[EVP_MAX_MD_SIZE];
    unsigned clen = 0;
    char want[2 * sizeof(TPMU_HA) + 1], got[2 * sizeof(TPMU_HA) + 1];

    if (!ctx->values_complete) {
        ck_fail(ctx, CK_PCR_DIGEST, "(not computed: unsigned-pcr-values do not match pcrSelect)");
        return;
    }
    if (!EVP_Digest(ctx->pcr_concat, ctx->pcr_concat_len, calc, &clen, ctx->sig_alg->md(), NULL)) {
        ck_fail(ctx, CK_PCR_DIGEST, "(EVP_Digest: %s)", ERR_error_string(ERR_get_error(), NULL));
        return;
    }
    if (d->size != clen || memcmp(d->buffer, calc, clen)) {
        to_hex(d->buffer, d->size, want);
        to_hex(calc, clen, got);
        ck_fail(ctx, CK_PCR_DIGEST, "(%s(unsigned-pcr-values) %s != pcrDigest %s: unsigned-pcr-values were altered)",
                ctx->sig_alg->name, got, want);
    } else {
        ck_ok(ctx, CK_PCR_DIGEST, "(%s over %zu bytes of PCR values == pcrDigest)", ctx->sig_alg->name, ctx->pcr_concat_len);
    }
}

/* 6 Reference Value（golden）。検査 5 が通った値だけを比べる。golden に無い PCR は「未評価」として報告する */
static void
check_golden(struct verify_ctx *ctx)
{
    const struct alg *ga;
    const TPML_PCR_SELECTION *sel = &ctx->attest.attested.quote.pcrSelect;
    struct json_object *cn;
    char mism[256] = "", uneval[256] = "";
    size_t ml = 0, ul = 0;
    unsigned compared = 0, i;
    uint32_t b;

    if (!ctx->golden) {
        ck_skip(ctx, CK_GOLDEN, "(no --golden: reference values are required for PASS)");
        return;
    }
    if (ctx->checks[CK_PCR_DIGEST].state != CK_OK) {
        ck_skip(ctx, CK_GOLDEN, "(pcr_digest did not pass; unsigned-pcr-values are not trusted)");
        return;
    }
    ga = golden_alg(ctx);
    if (!ga) {
        ck_fail(ctx, CK_GOLDEN, "(golden \"hash\" is missing or unsupported)");
        return;
    }
    if (json_object_object_get_ex(ctx->golden, "certificate-name", &cn) &&
        strcmp(json_object_get_string(cn), ctx->certificate_name)) {
        ck_fail(ctx, CK_GOLDEN, "(golden is for certificate-name \"%s\", reply has \"%s\")", json_object_get_string(cn),
                ctx->certificate_name);
        return;
    }
    for (b = 0; b < sel->count; b++) {
        const TPMS_PCR_SELECTION *s = &sel->pcrSelections[b];

        if (s->hash != ga->id) {
            continue;
        }
        for (i = 0; i < 8u * s->sizeofSelect; i++) {
            const char *g;
            const struct pcr_value *v;
            uint8_t gbin[sizeof(TPMU_HA)];
            int glen;

            if (!pcr_selected(s, i)) {
                continue;
            }
            g = golden_pcr(ctx, i);
            v = find_value(ctx, ga->id, i);
            if (!g) {
                sappend(uneval, sizeof uneval, &ul, "%s%u", ul ? "," : "", i);
                continue;
            }
            glen = from_hex(g, gbin, sizeof gbin);
            compared++;
            if (glen != (int)ga->size) {
                sappend(mism, sizeof mism, &ml, "%sPCR %u (golden value is not %u-byte hex)", ml ? ", " : "", i, ga->size);
            } else if (!v || v->len != glen || memcmp(v->value, gbin, (size_t)glen)) {
                sappend(mism, sizeof mism, &ml, "%sPCR %u", ml ? ", " : "", i);
            }
        }
    }
    if (ml) {
        ck_fail(ctx, CK_GOLDEN, "(mismatch with %s: %s%s%s)", ctx->opt.golden, mism, ul ? "; not evaluated: " : "", uneval);
    } else if (!compared) {
        ck_skip(ctx, CK_GOLDEN, "(%s has none of the quoted PCRs; not evaluated: %s)", ctx->opt.golden, uneval);
    } else {
        ck_ok(ctx, CK_GOLDEN, "(%u PCRs (%s) match %s%s%s)", compared, ga->name, ctx->opt.golden,
              ul ? "; not evaluated: " : "", uneval);
    }
}

/* 7 clockInfo。safe == 1 と、前回の state からの巻き戻しの検出 */
static void
check_clock(struct verify_ctx *ctx)
{
    const TPMS_CLOCK_INFO *c = &ctx->attest.clockInfo;
    struct json_object *j;
    uint64_t pclock;
    uint32_t preset, prestart;

    if (c->safe != 1) {
        ck_fail(ctx, CK_CLOCK, "(safe = %u: clock may have been rolled back inside the TPM)", c->safe);
        return;
    }
    if (!ctx->opt.state) {
        ck_ok(ctx, CK_CLOCK, "(safe=1; no --state, rollback against a previous quote not checked)");
        return;
    }
    if (!ctx->prev_state) {
        ck_ok(ctx, CK_CLOCK, "(safe=1; first run, %s will be created)", ctx->opt.state);
        return;
    }
    if (!json_object_object_get_ex(ctx->prev_state, "clock", &j) || !json_object_is_type(j, json_type_int)) {
        die(EXIT_USAGE, "%s: \"clock\" is missing", ctx->opt.state);
    }
    pclock = (uint64_t)json_object_get_int64(j);
    if (!json_object_object_get_ex(ctx->prev_state, "resetCount", &j) || !json_object_is_type(j, json_type_int)) {
        die(EXIT_USAGE, "%s: \"resetCount\" is missing", ctx->opt.state);
    }
    preset = (uint32_t)json_object_get_int64(j);
    if (!json_object_object_get_ex(ctx->prev_state, "restartCount", &j) || !json_object_is_type(j, json_type_int)) {
        die(EXIT_USAGE, "%s: \"restartCount\" is missing", ctx->opt.state);
    }
    prestart = (uint32_t)json_object_get_int64(j);

    if (c->resetCount < preset) {
        ck_fail(ctx, CK_CLOCK, "(resetCount %" PRIu32 " < previous %" PRIu32 ": TPM state rolled back or replaced)",
                c->resetCount, preset);
    } else if (c->resetCount > preset) {
        ck_ok(ctx, CK_CLOCK, "(reboot detected: resetCount %" PRIu32 " -> %" PRIu32 ")", preset, c->resetCount);
    } else if (c->clock < pclock) {
        ck_fail(ctx, CK_CLOCK, "(clock %" PRIu64 " < previous %" PRIu64 " with the same resetCount: rollback)", c->clock, pclock);
    } else if (c->restartCount < prestart) {
        ck_fail(ctx, CK_CLOCK, "(restartCount %" PRIu32 " < previous %" PRIu32 " with the same resetCount: rollback)",
                c->restartCount, prestart);
    } else {
        ck_ok(ctx, CK_CLOCK, "(safe=1; clock advanced %" PRIu64 " ms since previous quote, resetCount %" PRIu32 ")",
              c->clock - pclock, c->resetCount);
    }
}

/* 8 qualifiedSigner */
static void
check_qualified_signer(struct verify_ctx *ctx)
{
    const TPM2B_NAME *q = &ctx->attest.qualifiedSigner;
    char got[2 * sizeof(TPMU_NAME) + 1];

    if (!ctx->opt.ak_qname) {
        ck_skip(ctx, CK_QUALIFIED_SIGNER, "(no --ak-qname)");
        return;
    }
    if (q->size != ctx->qname_len || memcmp(q->name, ctx->qname, ctx->qname_len)) {
        to_hex(q->name, q->size, got);
        ck_fail(ctx, CK_QUALIFIED_SIGNER, "(qualifiedSigner %s != %s: quote by a key that is not enrolled)", got, ctx->opt.ak_qname);
    } else {
        ck_ok(ctx, CK_QUALIFIED_SIGNER, "(qualifiedSigner == %s)", ctx->opt.ak_qname);
    }
}

/* ------------------------------------------------------------------------------------------------------------------
 * Verifier 側の入力（nonce、AK、qualified name、golden、前回の state）
 * ---------------------------------------------------------------------------------------------------------------- */

static struct json_object *
load_json(const char *path, const char *what)
{
    struct json_object *j = json_object_from_file(path);

    if (!j) {
        die(EXIT_USAGE, "%s %s: %s", what, path, json_util_get_last_err());
    }
    if (!json_object_is_type(j, json_type_object)) {
        die(EXIT_USAGE, "%s %s: not a JSON object", what, path);
    }
    return j;
}

static void
load_inputs(struct verify_ctx *ctx)
{
    size_t len;
    uint8_t *buf;
    BIO *bio;
    struct json_object *pcrs;

    /* nonce: TPM2B_DATA に入る長さ（1〜64 バイト） */
    buf = read_file(ctx->opt.nonce, &len);
    if (len == 0 || len > sizeof ctx->nonce) {
        die(EXIT_USAGE, "%s: nonce must be 1..%zu bytes, got %zu", ctx->opt.nonce, sizeof ctx->nonce, len);
    }
    memcpy(ctx->nonce, buf, len);
    ctx->nonce_len = len;
    free(buf);

    /* AK の公開鍵: SubjectPublicKeyInfo の PEM（tpm2_readpublic -f pem） */
    buf = read_file(ctx->opt.ak, &len);
    bio = BIO_new_mem_buf(buf, (int)len);
    ctx->ak = bio ? PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL) : NULL;
    BIO_free(bio);
    free(buf);
    if (!ctx->ak) {
        die(EXIT_USAGE, "%s: not a PEM public key (SubjectPublicKeyInfo): %s", ctx->opt.ak,
            ERR_error_string(ERR_get_error(), NULL));
    }

    /* qualified name: tpm2_readpublic -q の生のバイト列（TPM2B_NAME の name の部分） */
    if (ctx->opt.ak_qname) {
        buf = read_file(ctx->opt.ak_qname, &len);
        if (len < 4 || len > sizeof ctx->qname) {
            die(EXIT_USAGE, "%s: qualified name must be 4..%zu bytes, got %zu", ctx->opt.ak_qname, sizeof ctx->qname, len);
        }
        memcpy(ctx->qname, buf, len);
        ctx->qname_len = len;
        free(buf);
    }

    /* golden（05 §2.5）: {"certificate-name", "hash", "captured", "note", "pcrs": {"0": "<hex>", ...}} */
    if (ctx->opt.golden) {
        ctx->golden = load_json(ctx->opt.golden, "golden");
        if (!golden_alg(ctx)) {
            die(EXIT_USAGE, "%s: \"hash\" must be one of sha1, sha256, sha384, sha512", ctx->opt.golden);
        }
        if (!json_object_object_get_ex(ctx->golden, "pcrs", &pcrs) || !json_object_is_type(pcrs, json_type_object)) {
            die(EXIT_USAGE, "%s: \"pcrs\" object is missing", ctx->opt.golden);
        }
    }

    /* 前回の state。無ければ初回 */
    if (ctx->opt.state && access(ctx->opt.state, F_OK) == 0) {
        ctx->prev_state = load_json(ctx->opt.state, "state");
    }
}

/* ------------------------------------------------------------------------------------------------------------------
 * 出力: 結果の JSON、--dump、state の更新
 * ---------------------------------------------------------------------------------------------------------------- */

static const char *
result_of(const struct verify_ctx *ctx)
{
    int i, skipped = 0;

    for (i = 0; i < CK_COUNT; i++) {
        if (ctx->checks[i].state == CK_FAIL || ctx->checks[i].state == CK_PENDING) {
            return "FAIL";
        }
        if (ctx->checks[i].state == CK_SKIP) {
            skipped = 1;
        }
    }
    /* golden や qualified name を与えず、検査が行えなかったときは PASS にしない（05 §5.4） */
    return skipped ? "INCOMPLETE" : "PASS";
}

/* tpm2_checkquote 用に quote.msg（TPMS_ATTEST）、quote.sig（TPMT_SIGNATURE）、pcrs.bin（連結した PCR 値）を書く（05 §5.3） */
static struct json_object *
write_dump(struct verify_ctx *ctx)
{
    struct json_object *d = json_object_new_object();
    char path[4096], err[512], nonce_hex[2 * sizeof(TPMU_HA) + 1], cmd[8192];
    const struct {
        const char *name;
        const void *data;
        size_t len;
    } files[] = {
        { "quote.msg", ctx->attest2b.attestationData, ctx->attest2b.size },
        { "quote.sig", ctx->quote_sig, ctx->quote_sig_len },
        { "pcrs.bin", ctx->pcr_concat, ctx->pcr_concat_len },
    };
    size_t i;

    if (mkdir(ctx->opt.dump, 0755) && errno != EEXIST) {
        die(EXIT_USAGE, "mkdir %s: %s", ctx->opt.dump, strerror(errno));
    }
    for (i = 0; i < sizeof files / sizeof files[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", ctx->opt.dump, files[i].name);
        if (write_file(path, files[i].data, files[i].len, err, sizeof err)) {
            die(EXIT_USAGE, "--dump: %s", err);
        }
        json_object_object_add(d, files[i].name, json_object_new_string(path));
    }
    to_hex(ctx->nonce, ctx->nonce_len, nonce_hex);
    snprintf(cmd, sizeof cmd, "tpm2_checkquote -u %s -g %s -m %s/quote.msg -s %s/quote.sig -q %s -l %s -f %s/pcrs.bin",
             ctx->opt.ak, ctx->sig_alg->name, ctx->opt.dump, ctx->opt.dump, nonce_hex, ctx->pcr_select_str, ctx->opt.dump);
    json_object_object_add(d, "tpm2_checkquote", json_object_new_string(cmd));
    return d;
}

static struct json_object *
clock_json(const TPMS_CLOCK_INFO *c)
{
    struct json_object *j = json_object_new_object();

    json_object_object_add(j, "clock", json_object_new_int64((int64_t)c->clock));
    json_object_object_add(j, "resetCount", json_object_new_int64(c->resetCount));
    json_object_object_add(j, "restartCount", json_object_new_int64(c->restartCount));
    json_object_object_add(j, "safe", json_object_new_int64(c->safe));
    return j;
}

/* 結果の JSON（05 §5.2 の形に、記録用の値を足したもの） */
static struct json_object *
result_json(struct verify_ctx *ctx, const char *result)
{
    struct json_object *r = json_object_new_object(), *checks = json_object_new_object(), *pcrs, *bank;
    char hex[2 * sizeof(TPMU_NAME) + 1], fw[32], namebuf[16];
    const TPML_PCR_SELECTION *sel;
    int i;
    uint32_t b;

    json_object_object_add(r, "result", json_object_new_string(result));
    json_object_object_add(r, "certificate-name",
                           ctx->certificate_name ? json_object_new_string(ctx->certificate_name) : NULL);
    for (i = 0; i < CK_COUNT; i++) {
        json_object_object_add(checks, check_names[i], json_object_new_string(ctx->checks[i].text));
    }
    json_object_object_add(r, "checks", checks);
    if (!ctx->structures_ok) {
        return r;
    }

    json_object_object_add(r, "clock", clock_json(&ctx->attest.clockInfo));
    json_object_object_add(r, "up_time", ctx->have_up_time ? json_object_new_int64(ctx->up_time) : NULL);
    snprintf(fw, sizeof fw, "0x%016" PRIx64, ctx->attest.firmwareVersion);
    json_object_object_add(r, "firmware_version", json_object_new_string(fw));
    to_hex(ctx->attest.qualifiedSigner.name, ctx->attest.qualifiedSigner.size, hex);
    json_object_object_add(r, "qualified_signer", json_object_new_string(hex));
    json_object_object_add(r, "pcr_select", json_object_new_string(ctx->pcr_select_str));
    to_hex(ctx->attest.attested.quote.pcrDigest.buffer, ctx->attest.attested.quote.pcrDigest.size, hex);
    json_object_object_add(r, "pcr_digest", json_object_new_string(hex));

    /* unsigned-pcr-values を bank → index の順で hex に */
    pcrs = json_object_new_object();
    sel = &ctx->attest.attested.quote.pcrSelect;
    for (b = 0; b < sel->count && b < TPM2_NUM_PCR_BANKS; b++) {
        size_t k;

        bank = json_object_new_object();
        for (k = 0; k < ctx->nvalues; k++) {
            char key[8];

            if (ctx->values[k].alg != sel->pcrSelections[b].hash) {
                continue;
            }
            snprintf(key, sizeof key, "%u", ctx->values[k].index);
            to_hex(ctx->values[k].value, ctx->values[k].len, hex);
            json_object_object_add(bank, key, json_object_new_string(hex));
        }
        json_object_object_add(pcrs, alg_name(sel->pcrSelections[b].hash, namebuf, sizeof namebuf), bank);
    }
    json_object_object_add(r, "pcrs", pcrs);
    return r;
}

/* state を更新する（検査 1〜3、7、8 が通った quote の clockInfo だけを記録する） */
static void
update_state(struct verify_ctx *ctx, const char *result)
{
    struct json_object *s = json_object_new_object();
    char hex[2 * sizeof(TPMU_NAME) + 1], fw[32], ts[32], err[512];
    time_t now = time(NULL);
    struct tm tm;
    const char *text;
    const TPMS_CLOCK_INFO *c = &ctx->attest.clockInfo;

    gmtime_r(&now, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm);
    json_object_object_add(s, "certificate-name", json_object_new_string(ctx->certificate_name));
    to_hex(ctx->attest.qualifiedSigner.name, ctx->attest.qualifiedSigner.size, hex);
    json_object_object_add(s, "qualified_signer", json_object_new_string(hex));
    json_object_object_add(s, "clock", json_object_new_int64((int64_t)c->clock));
    json_object_object_add(s, "resetCount", json_object_new_int64(c->resetCount));
    json_object_object_add(s, "restartCount", json_object_new_int64(c->restartCount));
    json_object_object_add(s, "safe", json_object_new_int64(c->safe));
    snprintf(fw, sizeof fw, "0x%016" PRIx64, ctx->attest.firmwareVersion);
    json_object_object_add(s, "firmware_version", json_object_new_string(fw));
    json_object_object_add(s, "up_time", ctx->have_up_time ? json_object_new_int64(ctx->up_time) : NULL);
    json_object_object_add(s, "updated", json_object_new_string(ts));
    json_object_object_add(s, "result", json_object_new_string(result));

    text = json_object_to_json_string_ext(s, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE);
    if (write_file(ctx->opt.state, text, strlen(text), err, sizeof err)) {
        json_object_put(s);
        die(EXIT_USAGE, "--state: %s", err);
    }
    json_object_put(s);
}

/* 応答の形が悪く、構造体を読めなかったとき: 読めなかった理由を各検査に書き、FAIL で終える */
static int
finish_malformed(struct verify_ctx *ctx, const char *reason)
{
    struct json_object *r;
    int i;

    for (i = 0; i < CK_COUNT; i++) {
        if (ctx->checks[i].state == CK_PENDING) {
            ck_fail(ctx, i, "(reply unusable: %s)", reason);
        }
    }
    r = result_json(ctx, "FAIL");
    json_object_object_add(r, "error", json_object_new_string(reason));
    puts(json_object_to_json_string_ext(r, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE));
    json_object_put(r);
    return EXIT_FAIL;
}

/* ------------------------------------------------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------------------------------------------------- */

static void
usage(FILE *out)
{
    fprintf(out,
        "usage: %s --reply REPLY.xml --nonce NONCE.bin --ak AK.pem [--ak-qname AK.qname]\n"
        "       [--golden PCRS.json] [--state STATE.json] [--dump DIR]\n"
        "\n"
        "Verify a tpm20-challenge-response-attestation reply (RFC 9684) against the nonce that was sent,\n"
        "the enrolled AK and the reference PCR values (doc/design/05_challenge_response.md §5.2).\n"
        "\n"
        "  --reply     reply.xml from netopeer2-cli --out (rpc-reply envelope optional)\n"
        "  --nonce     nonce.bin written by make_challenge.sh (1..64 bytes)\n"
        "  --ak        AK public key, PEM SubjectPublicKeyInfo (enroll/ak-cert-0.pem)\n"
        "  --ak-qname  AK qualified name, raw bytes (enroll/ak-cert-0.qname); check 8 is skipped without it\n"
        "  --golden    reference values ref-values/pcrs-sha256.json; check 6 is skipped without it (no PASS)\n"
        "  --state     previous clockInfo state/<certificate-name>.json; created or updated unless the result is FAIL\n"
        "  --dump DIR  write quote.msg, quote.sig and pcrs.bin for tpm2_checkquote (§5.3)\n"
        "\n"
        "exit status: 0 = PASS, 1 = FAIL or INCOMPLETE (a check was skipped), 2 = bad input\n", progname);
}

int
main(int argc, char **argv)
{
    static const struct option longopts[] = {
        { "reply",    required_argument, NULL, 'r' },
        { "nonce",    required_argument, NULL, 'n' },
        { "ak",       required_argument, NULL, 'a' },
        { "ak-qname", required_argument, NULL, 'q' },
        { "golden",   required_argument, NULL, 'g' },
        { "state",    required_argument, NULL, 's' },
        { "dump",     required_argument, NULL, 'd' },
        { "help",     no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    struct verify_ctx *ctx = xcalloc(1, sizeof *ctx);
    struct xnode *root = NULL;
    struct json_object *r;
    const char *result;
    char err[512];
    int c, rc;

    while ((c = getopt_long(argc, argv, "r:n:a:q:g:s:d:h", longopts, NULL)) != -1) {
        switch (c) {
        case 'r': ctx->opt.reply = optarg; break;
        case 'n': ctx->opt.nonce = optarg; break;
        case 'a': ctx->opt.ak = optarg; break;
        case 'q': ctx->opt.ak_qname = optarg; break;
        case 'g': ctx->opt.golden = optarg; break;
        case 's': ctx->opt.state = optarg; break;
        case 'd': ctx->opt.dump = optarg; break;
        case 'h': usage(stdout); return EXIT_PASS;
        default: usage(stderr); return EXIT_USAGE;
        }
    }
    if (optind != argc || !ctx->opt.reply || !ctx->opt.nonce || !ctx->opt.ak) {
        usage(stderr);
        return EXIT_USAGE;
    }

    load_inputs(ctx);

    if (load_reply(ctx, &root, err, sizeof err) || unmarshal_structures(ctx, err, sizeof err)) {
        rc = finish_malformed(ctx, err);
        goto out;
    }

    /* 検査の順は 05 §5.2 のとおり。6 は 5 の結果を、5 は 4 の結果を使う */
    check_signature(ctx);
    check_magic_type(ctx);
    check_nonce(ctx);
    check_pcr_select(ctx);
    check_pcr_digest(ctx);
    check_golden(ctx);
    check_clock(ctx);
    check_qualified_signer(ctx);

    result = result_of(ctx);
    r = result_json(ctx, result);
    if (ctx->opt.dump) {
        json_object_object_add(r, "dump", write_dump(ctx));
    }
    if (ctx->opt.state && strcmp(result, "FAIL")) {
        update_state(ctx, result);
    }
    puts(json_object_to_json_string_ext(r, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE));
    json_object_put(r);
    rc = strcmp(result, "PASS") ? EXIT_FAIL : EXIT_PASS;

out:
    xnode_free(root);
    EVP_PKEY_free(ctx->ak);
    json_object_put(ctx->golden);
    json_object_put(ctx->prev_state);
    free(ctx->certificate_name);
    free(ctx->quote_data);
    free(ctx->quote_sig);
    free(ctx);
    return rc;
}
