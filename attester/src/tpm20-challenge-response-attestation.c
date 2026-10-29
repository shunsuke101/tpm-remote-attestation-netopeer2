/*
 * tpm20-challenge-response-attestation.c — RPC /ietf-tpm-remote-attestation:tpm20-challenge-response-attestation
 * doc/design/05_challenge_response.md §3.5（処理の手順 0〜10）、§3.6（エラーの返し方）
 *
 * input / output は RPC の要素自身（tpm20-challenge-response-attestation）。
 * ここに届く前に、sysrepo が NACM（exec）と入力の YANG 検証（tpm20-hash-algo の must、pcr の範囲 0..31）を済ませている
 * （doc/notes/rats-attesterd-rpc-flow-2026-10-08.md）。返した後、sysrepo が出力を検証する（certificate-name の leafref）。
 *
 * pcrDigest の再計算（SHA-256）と binary 値の base64 には libcrypto（OpenSSL）を使う。tpm2-tss 自身の依存でもある。
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/sysinfo.h>

#include <libyang/libyang.h>
#include <openssl/evp.h>
#include <sysrepo.h>
#include <tss2/tss2_mu.h>

#include "attester.h"

#define XPATH_NONCE     "tpm20-attestation-challenge/nonce-value"
#define XPATH_SELECTION "tpm20-attestation-challenge/tpm20-pcr-selection"

/* 手順 0: 監査ログ。netopeer2 は orig data に NETCONF のセッション ID（uint32）とユーザー名を入れる（Netopeer2/src/common.c） */
static void
audit_log(sr_session_ctx_t *session)
{
    const char *orig = sr_session_get_orig_name(session), *user = "?";
    const void *data = NULL;
    uint32_t size = 0, nc_sid = 0;

    if (!sr_session_get_orig_data(session, 0, &size, &data) && (size == sizeof nc_sid)) {
        memcpy(&nc_sid, data, sizeof nc_sid);
    }
    if (!sr_session_get_orig_data(session, 1, &size, &data) && size && (((const char *)data)[size - 1] == '\0')) {
        user = data;
    }
    SRPLG_LOG_INF(RATS_PLG_NAME, "tpm20-challenge-response-attestation from %s (NETCONF session %" PRIu32 ", user %s)",
            orig ? orig : "?", nc_sid, user);
}

/* 手順 2: nonce。libyang が base64 を復号済みなので、生のバイト列を直接取る */
static int
get_nonce(const struct lyd_node *input, const struct rats_conf *conf, const uint8_t **nonce, size_t *nonce_len,
        char *err, size_t errlen)
{
    struct lyd_node *node;
    struct lyd_value_binary *bin;

    if (lyd_find_path(input, XPATH_NONCE, 0, &node)) {
        snprintf(err, errlen, "nonce-value is missing");
        return SR_ERR_INVAL_ARG;
    }
    LYD_VALUE_GET(&((struct lyd_node_term *)node)->value, bin);
    *nonce = bin->data;
    *nonce_len = bin->size;

    /* RFC 9684 は長すぎる nonce を TPM が切り詰めるとしているが、Verifier が extraData と照合できなくなるので拒否する */
    if ((*nonce_len < conf->nonce_min) || (*nonce_len > conf->nonce_max)) {
        snprintf(err, errlen, "nonce-value must be %zu..%zu bytes (got %zu)", conf->nonce_min, conf->nonce_max, *nonce_len);
        return SR_ERR_INVAL_ARG;
    }
    return SR_ERR_OK;
}

static const struct rats_bank *
find_bank(const struct rats_banks *banks, TPM2_ALG_ID hash)
{
    uint32_t i;

    for (i = 0; i < banks->count; i++) {
        if (banks->bank[i].hash == hash) {
            return &banks->bank[i];
        }
    }
    return NULL;
}

/* bitmap の中で、TPM に無い PCR（pcr_count 以上）が選ばれていれば、その番号を返す。無ければ -1 */
static int
first_pcr_beyond(const uint8_t *select, uint8_t sizeof_select, uint32_t pcr_count)
{
    unsigned idx;

    for (idx = pcr_count; idx < 8u * sizeof_select && idx < TPM2_MAX_PCRS; idx++) {
        if (select[idx / 8] & (1u << (idx % 8))) {
            return (int)idx;
        }
    }
    return -1;
}

static int
has_any_pcr(const uint8_t *select, uint8_t sizeof_select)
{
    return first_pcr_beyond(select, sizeof_select, 0) >= 0;
}

static int
selection_has_hash(const TPML_PCR_SELECTION *sel, TPM2_ALG_ID hash)
{
    uint32_t i;

    for (i = 0; i < sel->count; i++) {
        if (sel->pcrSelections[i].hash == hash) {
            return 1;
        }
    }
    return 0;
}

/* 手順 3、4: tpm20-pcr-selection → TPML_PCR_SELECTION。省略時の扱いと範囲の検査もここで行う */
static int
build_selection(const struct lyd_node *input, const struct rats_banks *banks, uint32_t pcr_count,
        TPML_PCR_SELECTION *sel, char *err, size_t errlen)
{
    struct ly_set *set = NULL;
    uint8_t sizeof_select = (uint8_t)((pcr_count + 7) / 8);
    uint32_t i;
    int rc = SR_ERR_OK;

    if (sizeof_select > TPM2_PCR_SELECT_MAX) {
        sizeof_select = TPM2_PCR_SELECT_MAX;
    }
    memset(sel, 0, sizeof *sel);

    if (lyd_find_xpath(input, XPATH_SELECTION, &set)) {
        snprintf(err, errlen, "lyd_find_xpath(%s): %s", XPATH_SELECTION, ly_last_logmsg());
        return SR_ERR_LY;
    }

    if (set->count == 0) {
        /* selection が無い: tpm20-pcr-bank のすべての bank とすべての PCR */
        for (i = 0; i < banks->count; i++) {
            TPMS_PCR_SELECTION *s = &sel->pcrSelections[sel->count];
            int beyond = first_pcr_beyond(banks->bank[i].select, sizeof_select, pcr_count);

            if (beyond >= 0) {
                snprintf(err, errlen, "PCR %d (%s) in tpm20-pcr-bank is not implemented by the TPM (TPM2_PT_PCR_COUNT = %" PRIu32 ")",
                        beyond, rats_identity_from_alg(banks->bank[i].hash), pcr_count);
                rc = SR_ERR_INVAL_ARG;
                goto cleanup;
            }
            s->hash = banks->bank[i].hash;
            s->sizeofSelect = sizeof_select;
            memcpy(s->pcrSelect, banks->bank[i].select, sizeof_select);
            sel->count++;
        }
        goto cleanup;
    }

    for (i = 0; i < set->count; i++) {
        const struct lyd_node *entry = set->dnodes[i], *child;
        struct lyd_node *node;
        const struct rats_bank *bank;
        TPMS_PCR_SELECTION *s;
        TPM2_ALG_ID hash;
        const char *id;
        int have_index = 0, beyond;

        if (sel->count >= TPM2_NUM_PCR_BANKS) {
            snprintf(err, errlen, "more than %d tpm20-pcr-selection entries", TPM2_NUM_PCR_BANKS);
            rc = SR_ERR_INVAL_ARG;
            goto cleanup;
        }

        /* tpm20-hash-algo が無ければ SHA-256（04 §6 の 6） */
        if (lyd_find_path(entry, "tpm20-hash-algo", 0, &node)) {
            hash = TPM2_ALG_SHA256;
        } else {
            hash = rats_alg_from_identity(lyd_get_value(node));
            if (hash == TPM2_ALG_NULL) {
                snprintf(err, errlen, "tpm20-hash-algo %s is not supported by this attester", lyd_get_value(node));
                rc = SR_ERR_INVAL_ARG;
                goto cleanup;
            }
        }
        id = rats_identity_from_alg(hash);
        bank = find_bank(banks, hash);
        if (!bank) {
            snprintf(err, errlen, "%s bank is not configured in tpm20-pcr-bank", id);
            rc = SR_ERR_INVAL_ARG;
            goto cleanup;
        }
        /* YANG の unique は tpm20-hash-algo を省略した項目には効かないので、ここで重複を弾く */
        if (selection_has_hash(sel, hash)) {
            snprintf(err, errlen, "%s bank is selected twice", id);
            rc = SR_ERR_INVAL_ARG;
            goto cleanup;
        }

        s = &sel->pcrSelections[sel->count];
        s->hash = hash;
        s->sizeofSelect = sizeof_select;
        for (child = lyd_child(entry); child; child = child->next) {
            unsigned long idx;

            if (strcmp(LYD_NAME(child), "pcr-index")) {
                continue;
            }
            have_index = 1;
            idx = strtoul(lyd_get_value(child), NULL, 10);   /* 型 pcr は 0..31 を YANG が保証している */
            if (idx >= pcr_count) {
                snprintf(err, errlen, "PCR %lu is not implemented by the TPM (TPM2_PT_PCR_COUNT = %" PRIu32 ")", idx, pcr_count);
                rc = SR_ERR_INVAL_ARG;
                goto cleanup;
            }
            if (!(bank->select[idx / 8] & (1u << (idx % 8)))) {
                snprintf(err, errlen, "PCR %lu (%s) is not in tpm20-pcr-bank", idx, id);
                rc = SR_ERR_INVAL_ARG;
                goto cleanup;
            }
            s->pcrSelect[idx / 8] |= (uint8_t)(1u << (idx % 8));
        }
        if (!have_index) {
            /* エントリはあるが pcr-index が無い: その bank の設定済み PCR すべて */
            beyond = first_pcr_beyond(bank->select, sizeof_select, pcr_count);
            if (beyond >= 0) {
                snprintf(err, errlen, "PCR %d (%s) in tpm20-pcr-bank is not implemented by the TPM (TPM2_PT_PCR_COUNT = %" PRIu32 ")",
                        beyond, id, pcr_count);
                rc = SR_ERR_INVAL_ARG;
                goto cleanup;
            }
            memcpy(s->pcrSelect, bank->select, sizeof_select);
        }
        if (!has_any_pcr(s->pcrSelect, sizeof_select)) {
            snprintf(err, errlen, "no PCR selected for %s", id);
            rc = SR_ERR_INVAL_ARG;
            goto cleanup;
        }
        sel->count++;
    }

cleanup:
    ly_set_free(set, NULL);
    return rc;
}

static const EVP_MD *
md_from_alg(TPM2_ALG_ID alg)
{
    switch (alg) {
    case TPM2_ALG_SHA1:
        return EVP_sha1();
    case TPM2_ALG_SHA256:
        return EVP_sha256();
    case TPM2_ALG_SHA384:
        return EVP_sha384();
    case TPM2_ALG_SHA512:
        return EVP_sha512();
    default:
        return NULL;
    }
}

/* 手順 6 の自己検査: TPM が返した TPMS_ATTEST の magic、type、extraData（= nonce）を確かめる */
static int
check_attest(const TPM2B_ATTEST *quoted, const uint8_t *nonce, size_t nonce_len, TPMS_ATTEST *attest, char *err, size_t errlen)
{
    size_t off = 0;
    TSS2_RC rc;

    rc = Tss2_MU_TPMS_ATTEST_Unmarshal(quoted->attestationData, quoted->size, &off, attest);
    if (rc) {
        snprintf(err, errlen, "Tss2_MU_TPMS_ATTEST_Unmarshal failed (0x%08" PRIx32 ")", rc);
        return -1;
    }
    if ((attest->magic != TPM2_GENERATED_VALUE) || (attest->type != TPM2_ST_ATTEST_QUOTE)) {
        snprintf(err, errlen, "TPM returned an unexpected attestation structure (magic 0x%08" PRIx32 ", type 0x%04x)",
                attest->magic, attest->type);
        return -1;
    }
    if ((attest->extraData.size != nonce_len) || memcmp(attest->extraData.buffer, nonce, nonce_len)) {
        snprintf(err, errlen, "extraData of the quote does not match the nonce");
        return -1;
    }
    return 0;
}

/* 手順 7: PCR 値を selection の順に連結し、AK の署名方式のハッシュにかける（TPM2_Quote の pcrDigest と同じ計算） */
static int
compute_pcr_digest(TPM2_ALG_ID hash, const struct rats_pcr_values *values, TPM2B_DIGEST *digest, char *err, size_t errlen)
{
    const EVP_MD *md = md_from_alg(hash);
    EVP_MD_CTX *ctx;
    unsigned int len = 0;
    uint32_t i;
    int ok;

    if (!md) {
        snprintf(err, errlen, "AK signing hash 0x%04x is not supported for pcrDigest", hash);
        return -1;
    }
    ctx = EVP_MD_CTX_new();
    if (!ctx) {
        snprintf(err, errlen, "EVP_MD_CTX_new failed");
        return -1;
    }
    ok = EVP_DigestInit_ex(ctx, md, NULL);
    for (i = 0; ok && i < values->count; i++) {
        ok = EVP_DigestUpdate(ctx, values->digest[i].buffer, values->digest[i].size);
    }
    ok = ok && EVP_DigestFinal_ex(ctx, digest->buffer, &len);
    EVP_MD_CTX_free(ctx);
    if (!ok) {
        snprintf(err, errlen, "EVP digest of PCR values failed");
        return -1;
    }
    digest->size = (UINT16)len;
    return 0;
}

static double
ms_since(const struct timespec *t0)
{
    struct timespec t1;

    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (double)(t1.tv_sec - t0->tv_sec) * 1e3 + (double)(t1.tv_nsec - t0->tv_nsec) / 1e6;
}

/* 手順 5〜7: Quote → PCR_Read → pcrDigest の照合。不一致なら conf.quote_retry 回まで繰り返す。呼び出し側が tpm_lock を持つ */
static int
quote_with_retry(struct rats_attester *app, const uint8_t *nonce, size_t nonce_len, const TPML_PCR_SELECTION *sel,
        TPM2B_ATTEST **quoted, TPMT_SIGNATURE **sig, struct rats_pcr_values *pcrs, TPMS_ATTEST *attest,
        char *err, size_t errlen)
{
    unsigned attempt;

    for (attempt = 0; ; attempt++) {
        struct timespec t0;
        double quote_ms, read_ms;
        TPM2B_DIGEST digest;

        clock_gettime(CLOCK_MONOTONIC, &t0);
        if (rats_tpm_quote(&app->tpm, nonce, nonce_len, sel, quoted, sig, err, errlen)) {
            return -1;
        }
        quote_ms = ms_since(&t0);

        clock_gettime(CLOCK_MONOTONIC, &t0);
        if (rats_tpm_pcr_read(&app->tpm, sel, pcrs, err, errlen)) {
            goto fail;
        }
        read_ms = ms_since(&t0);
        SRPLG_LOG_INF(RATS_PLG_NAME, "TPM2_Quote %.1f ms, TPM2_PCR_Read %.1f ms (%" PRIu32 " PCRs)", quote_ms, read_ms, pcrs->count);

        if (check_attest(*quoted, nonce, nonce_len, attest, err, errlen) ||
                compute_pcr_digest(app->tpm.sig_hash, pcrs, &digest, err, errlen)) {
            goto fail;
        }
        if ((digest.size == attest->attested.quote.pcrDigest.size) &&
                !memcmp(digest.buffer, attest->attested.quote.pcrDigest.buffer, digest.size)) {
            return 0;
        }

        /* Quote と PCR_Read の間に PCR が extend された */
        Esys_Free(*quoted);
        Esys_Free(*sig);
        *quoted = NULL;
        *sig = NULL;
        if (attempt >= app->conf.quote_retry) {
            snprintf(err, errlen, "PCRs changed during quote (pcrDigest mismatch after %u attempts)", attempt + 1);
            return -1;
        }
        SRPLG_LOG_WRN(RATS_PLG_NAME, "pcrDigest does not match the PCR values read; retrying (%u/%u)", attempt + 1,
                app->conf.quote_retry);
    }

fail:
    Esys_Free(*quoted);
    Esys_Free(*sig);
    *quoted = NULL;
    *sig = NULL;
    return -1;
}

/* base64（改行無し）。malloc したものを返す */
static char *
b64_encode(const uint8_t *in, size_t len)
{
    char *out = malloc(4 * ((len + 2) / 3) + 1);

    if (out) {
        EVP_EncodeBlock((unsigned char *)out, in, (int)len);
    }
    return out;
}

/* 手順 6、8、9: marshal して output の木を組み立てる */
static int
build_output(const struct rats_attester *app, struct lyd_node *output, const TPML_PCR_SELECTION *sel,
        const TPM2B_ATTEST *quoted, const TPMT_SIGNATURE *sig, const struct rats_pcr_values *pcrs, char *err, size_t errlen)
{
    uint8_t abuf[sizeof(TPM2B_ATTEST)], sbuf[sizeof(TPMT_SIGNATURE)];
    size_t alen = 0, slen = 0;
    char *b64 = NULL, uptime[16];
    struct sysinfo si;
    struct lyd_node *resp, *upv, *pv;
    uint32_t i, k = 0;
    TSS2_RC trc;
    int rc = -1;

    trc = Tss2_MU_TPM2B_ATTEST_Marshal(quoted, abuf, sizeof abuf, &alen);
    if (!trc) {
        trc = Tss2_MU_TPMT_SIGNATURE_Marshal(sig, sbuf, sizeof sbuf, &slen);
    }
    if (trc) {
        snprintf(err, errlen, "marshalling the quote failed (0x%08" PRIx32 ")", trc);
        return -1;
    }
    if (sysinfo(&si)) {
        si.uptime = 0;
    }
    snprintf(uptime, sizeof uptime, "%" PRIu32, (uint32_t)si.uptime);

#define NEW_OR_FAIL(call, what) \
    do { if (call) { snprintf(err, errlen, "building output (%s): %s", what, ly_last_logmsg()); goto cleanup; } } while (0)
#define B64_TERM(parent, leaf, buf, len) \
    do { \
        free(b64); \
        b64 = b64_encode(buf, len); \
        if (!b64) { snprintf(err, errlen, "out of memory"); goto cleanup; } \
        NEW_OR_FAIL(lyd_new_term(parent, NULL, leaf, b64, LYD_NEW_VAL_OUTPUT, NULL), leaf); \
    } while (0)

    /* キーの無い list は lyd_new_list() をキー無しで呼ぶ（lyd_new_inner() では作れない） */
    NEW_OR_FAIL(lyd_new_list(output, NULL, "tpm20-attestation-response", LYD_NEW_VAL_OUTPUT, &resp), "tpm20-attestation-response");
    NEW_OR_FAIL(lyd_new_term(resp, NULL, "certificate-name", app->conf.certificate_name, LYD_NEW_VAL_OUTPUT, NULL), "certificate-name");
    B64_TERM(resp, "quote-data", abuf, alen);
    B64_TERM(resp, "quote-signature", sbuf, slen);
    NEW_OR_FAIL(lyd_new_term(resp, NULL, "up-time", uptime, LYD_NEW_VAL_OUTPUT, NULL), "up-time");

    /* unsigned-pcr-values: bank ごと、PCR 番号の昇順。pcrs は同じ順に並んでいる */
    for (i = 0; i < sel->count; i++) {
        const TPMS_PCR_SELECTION *s = &sel->pcrSelections[i];
        unsigned idx;

        NEW_OR_FAIL(lyd_new_list(resp, NULL, "unsigned-pcr-values", LYD_NEW_VAL_OUTPUT, &upv), "unsigned-pcr-values");
        NEW_OR_FAIL(lyd_new_term(upv, NULL, "tpm20-hash-algo", rats_identity_from_alg(s->hash), LYD_NEW_VAL_OUTPUT, NULL),
                "tpm20-hash-algo");
        for (idx = 0; idx < 8u * s->sizeofSelect && idx < TPM2_MAX_PCRS; idx++) {
            char key[4];

            if (!(s->pcrSelect[idx / 8] & (1u << (idx % 8)))) {
                continue;
            }
            if (k >= pcrs->count) {
                snprintf(err, errlen, "TPM returned fewer PCR values (%" PRIu32 ") than selected", pcrs->count);
                goto cleanup;
            }
            snprintf(key, sizeof key, "%u", idx);
            NEW_OR_FAIL(lyd_new_list(upv, NULL, "pcr-values", LYD_NEW_VAL_OUTPUT, &pv, key), "pcr-values");
            B64_TERM(pv, "pcr-value", pcrs->digest[k].buffer, pcrs->digest[k].size);
            k++;
        }
    }
    if (k != pcrs->count) {
        snprintf(err, errlen, "TPM returned %" PRIu32 " PCR values but %" PRIu32 " were selected", pcrs->count, k);
        goto cleanup;
    }
    rc = 0;

#undef B64_TERM
#undef NEW_OR_FAIL
cleanup:
    free(b64);
    return rc;
}

static int
rpc_cb(sr_session_ctx_t *session, uint32_t sub_id, const char *op_path, const struct lyd_node *input,
        sr_event_t event, uint32_t operation_id, struct lyd_node *output, void *private_data)
{
    struct rats_attester *app = private_data;
    struct rats_banks banks;
    TPML_PCR_SELECTION sel;
    const uint8_t *nonce = NULL;
    size_t nonce_len = 0;
    TPM2B_ATTEST *quoted = NULL;
    TPMT_SIGNATURE *sig = NULL;
    struct rats_pcr_values *pcrs = NULL;
    TPMS_ATTEST attest;
    const char *tag = NULL;
    char err[RATS_ERRLEN];
    int rc;

    (void)sub_id; (void)event; (void)operation_id;

    /* 0 */
    audit_log(session);

    /* 1. 設定は RPC のたびに running から読み直す（古い設定を使わない） */
    if (rats_load_pcr_banks(app->run_sess, &app->conf, &banks, err, sizeof err)) {
        tag = "operation-failed";
        rc = SR_ERR_OPERATION_FAILED;
        goto done;
    }

    /* 2.yang moduleのinputのnonceを取得*/
    rc = get_nonce(input, &app->conf, &nonce, &nonce_len, err, sizeof err);
    if (rc) {
        tag = "invalid-value";
        goto done;
    }

    /*3.yang moduleのinputのPCR selectionを取得*/
    rc = build_selection(input, &banks, app->tpm.pcr_count, &sel, err, sizeof err);
    if (rc) {
        tag = (rc == SR_ERR_INVAL_ARG) ? "invalid-value" : "operation-failed";
        goto done;
    }

    /* Qouteを実行*/
    pcrs = calloc(1, sizeof *pcrs);
    if (!pcrs) {
        snprintf(err, sizeof err, "out of memory");
        tag = "operation-failed";
        rc = SR_ERR_NO_MEMORY;
        goto done;
    }
    pthread_mutex_lock(&app->tpm_lock);
    rc = quote_with_retry(app, nonce, nonce_len, &sel, &quoted, &sig, pcrs, &attest, err, sizeof err);
    pthread_mutex_unlock(&app->tpm_lock);
    if (rc) {
        tag = "operation-failed";
        rc = SR_ERR_OPERATION_FAILED;
        goto done;
    }

    /* 出力の組み立て */
    if (build_output(app, output, &sel, quoted, sig, pcrs, err, sizeof err)) {
        tag = "operation-failed";
        rc = SR_ERR_OPERATION_FAILED;
        goto done;
    }

    SRPLG_LOG_INF(RATS_PLG_NAME, "quote returned: certificate-name=%s resetCount=%" PRIu32 " clock=%" PRIu64 " %" PRIu32 " PCRs",
            app->conf.certificate_name, attest.clockInfo.resetCount, attest.clockInfo.clock, pcrs->count);
    rc = SR_ERR_OK;   /* 10. この後 sysrepo が出力を検証する（leafref の certificate-name など） */

done:
    if (tag) {
        SRPLG_LOG_ERR(RATS_PLG_NAME, "%s: %s", op_path, err);
        rats_nc_error(session, tag, NULL, err);
    }
    Esys_Free(quoted);
    Esys_Free(sig);
    free(pcrs);
    return rc;
}

int
rats_tpm20_cra_subscribe(struct rats_attester *app, sr_session_ctx_t *sess)
{
    return sr_rpc_subscribe_tree(sess, RATS_XPATH_TPM20_RPC, rpc_cb, app, 0, 0, &app->sub);
}
