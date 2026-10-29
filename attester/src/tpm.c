/*
 * tpm.c — ESAPI（tss2-esys）のラッパー。doc/design/05_challenge_response.md §3.4、§3.5、§3.7
 *
 * 参考: tpm2-tools の lib/pcr.c（pcr_read_pcr_values の繰り返し）、lib/tpm2_capability.c、tools/tpm2_getcap.c
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <tss2/tss2_esys.h>
#include <tss2/tss2_rc.h>
#include <tss2/tss2_tctildr.h>

#include "tpm.h"

static int
fail_rc(char *errmsg, size_t errlen, const char *what, TSS2_RC rc)
{
    snprintf(errmsg, errlen, "%s: %s", what, Tss2_RC_Decode(rc));
    return -1;
}

/* TPM2_PT_FAMILY_INDICATOR や TPM2_PT_MANUFACTURER の値（4 文字をビッグエンディアンで詰めたもの）を文字列にする */
static void
u32_to_chars(uint32_t v, char out[5])
{
    int i;

    out[0] = (char)(v >> 24);
    out[1] = (char)(v >> 16);
    out[2] = (char)(v >> 8);
    out[3] = (char)v;
    out[4] = '\0';
    for (i = 3; i >= 0 && (out[i] == '\0' || out[i] == ' '); i--) {
        out[i] = '\0';
    }
}

/* §3.4 の 3、4: AK を読み込み、属性と方式を確かめる */
static int
load_ak(rats_tpm_ctx *ctx, uint32_t ak_handle, char *errmsg, size_t errlen)
{
    TPM2B_PUBLIC *pub = NULL;
    TPM2B_NAME *name = NULL, *qname = NULL;
    const TPMT_PUBLIC *p;
    const TPMA_OBJECT need = TPMA_OBJECT_RESTRICTED | TPMA_OBJECT_SIGN_ENCRYPT | TPMA_OBJECT_FIXEDTPM;
    TSS2_RC rc;
    int ret = -1;

    rc = Esys_TR_FromTPMPublic(ctx->esys_ctx, ak_handle, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE, &ctx->ak);
    if (rc) {
        char what[64];

        snprintf(what, sizeof what, "AK 0x%08" PRIx32 " not available (Esys_TR_FromTPMPublic)", ak_handle);
        return fail_rc(errmsg, errlen, what, rc);
    }

    rc = Esys_ReadPublic(ctx->esys_ctx, ctx->ak, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE, &pub, &name, &qname);
    if (rc) {
        return fail_rc(errmsg, errlen, "Esys_ReadPublic(AK)", rc);
    }
    p = &pub->publicArea;

    if ((p->objectAttributes & need) != need) {
        snprintf(errmsg, errlen, "AK attributes 0x%08" PRIx32 " lack restricted|sign|fixedTPM", (uint32_t)p->objectAttributes);
        goto cleanup;
    }
    if ((p->type != TPM2_ALG_ECC) || (p->parameters.eccDetail.scheme.scheme != TPM2_ALG_ECDSA) ||
            (p->parameters.eccDetail.scheme.details.ecdsa.hashAlg != TPM2_ALG_SHA256)) {
        snprintf(errmsg, errlen, "AK is not an ECC/ECDSA/SHA-256 key (type 0x%04x, scheme 0x%04x)",
                p->type, (p->type == TPM2_ALG_ECC) ? p->parameters.eccDetail.scheme.scheme : 0);
        goto cleanup;
    }

    ctx->sig_hash = p->parameters.eccDetail.scheme.details.ecdsa.hashAlg;
    ctx->ak_name = *name;
    ctx->ak_qname = *qname;
    ret = 0;

cleanup:
    Esys_Free(pub);
    Esys_Free(name);
    Esys_Free(qname);
    return ret;
}

/* §3.4 の 5: TPM2_CAP_TPM_PROPERTIES の fixed グループから family、manufacturer、PCR 数を読む */
static int
read_properties(rats_tpm_ctx *ctx, char *errmsg, size_t errlen)
{
    TPM2_PT prop = TPM2_PT_FIXED;
    TPMI_YES_NO more = 1;
    char family[5] = "";
    int have_manufacturer = 0, have_pcr_count = 0;
    TSS2_RC rc;

    while (more) {
        TPMS_CAPABILITY_DATA *cap = NULL;
        const TPML_TAGGED_TPM_PROPERTY *props;
        uint32_t i;

        rc = Esys_GetCapability(ctx->esys_ctx, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE, TPM2_CAP_TPM_PROPERTIES, prop,
                TPM2_MAX_TPM_PROPERTIES, &more, &cap);
        if (rc) {
            return fail_rc(errmsg, errlen, "Esys_GetCapability(TPM_PROPERTIES)", rc);
        }
        props = &cap->data.tpmProperties;
        if (props->count == 0) {
            Esys_Free(cap);
            break;
        }
        for (i = 0; i < props->count; i++) {
            const TPMS_TAGGED_PROPERTY *tp = &props->tpmProperty[i];

            if (tp->property >= TPM2_PT_FIXED + TPM2_PT_GROUP) {
                more = 0;   /* fixed グループを出た */
                break;
            }
            switch (tp->property) {
            case TPM2_PT_FAMILY_INDICATOR:
                u32_to_chars(tp->value, family);
                break;
            case TPM2_PT_MANUFACTURER:
                u32_to_chars(tp->value, ctx->manufacturer);
                have_manufacturer = 1;
                break;
            case TPM2_PT_PCR_COUNT:
                ctx->pcr_count = tp->value;
                have_pcr_count = 1;
                break;
            default:
                break;
            }
            prop = tp->property + 1;
        }
        Esys_Free(cap);
    }

    if (strcmp(family, "2.0")) {
        snprintf(errmsg, errlen, "TPM2_PT_FAMILY_INDICATOR is \"%s\", expected \"2.0\"", family);
        return -1;
    }
    if (!have_manufacturer || !have_pcr_count) {
        snprintf(errmsg, errlen, "TPM did not report TPM2_PT_MANUFACTURER or TPM2_PT_PCR_COUNT");
        return -1;
    }
    if ((ctx->pcr_count == 0) || (ctx->pcr_count > TPM2_MAX_PCRS)) {
        snprintf(errmsg, errlen, "TPM2_PT_PCR_COUNT = %" PRIu32 " is out of range (1..%d)", ctx->pcr_count, TPM2_MAX_PCRS);
        return -1;
    }
    return 0;
}

/* §3.4 の 5: TPM2_CAP_PCRS で有効な bank を読む */
static int
read_banks(rats_tpm_ctx *ctx, char *errmsg, size_t errlen)
{
    TPMS_CAPABILITY_DATA *cap = NULL;
    TPMI_YES_NO more = 0;
    TSS2_RC rc;

    rc = Esys_GetCapability(ctx->esys_ctx, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE, TPM2_CAP_PCRS, 0, TPM2_NUM_PCR_BANKS,
            &more, &cap);
    if (rc) {
        return fail_rc(errmsg, errlen, "Esys_GetCapability(PCRS)", rc);
    }
    ctx->banks = cap->data.assignedPCR;
    Esys_Free(cap);
    return 0;
}

/*tctiのcontextをinitilizeして、esysのcontextをinitilizeして、attestation keyをloadする関数*/
int
rats_tpm_init(rats_tpm_ctx *ctx, const char *tcti_conf, uint32_t ak_handle, char *errmsg, size_t errlen)
{
    TSS2_RC rc;

    memset(ctx, 0, sizeof *ctx);
    ctx->ak = ESYS_TR_NONE;

    rc = Tss2_TctiLdr_Initialize(tcti_conf, &ctx->tcti_ctx);
    if (rc) {
        char what[160];

        snprintf(what, sizeof what, "Tss2_TctiLdr_Initialize(%s)", tcti_conf);
        fail_rc(errmsg, errlen, what, rc);
        goto fail;
    }
    rc = Esys_Initialize(&ctx->esys_ctx, ctx->tcti_ctx, NULL);
    if (rc) {
        fail_rc(errmsg, errlen, "Esys_Initialize", rc);
        goto fail;
    }
    if (load_ak(ctx, ak_handle, errmsg, errlen) || read_properties(ctx, errmsg, errlen) || read_banks(ctx, errmsg, errlen)) {
        goto fail;
    }
    return 0;

fail:
    rats_tpm_finalize(ctx);
    return -1;
}

void
rats_tpm_finalize(rats_tpm_ctx *ctx)
{
    if (!ctx) {
        return;
    }
    /* Esys_Finalize は TCTI を解放しない（Esys_Initialize に渡した側が所有する）ので、両方を順に片付ける。
     * AK の ESYS_TR は永続ハンドルのメタデータなので、Esys_Finalize が一緒に捨てる */
    if (ctx->esys_ctx) {
        Esys_Finalize(&ctx->esys_ctx);
    }
    if (ctx->tcti_ctx) {
        Tss2_TctiLdr_Finalize(&ctx->tcti_ctx);
    }
    memset(ctx, 0, sizeof *ctx);
}

static int
selection_has_bits(const TPMS_PCR_SELECTION *s)
{
    uint8_t i;

    for (i = 0; i < s->sizeofSelect && i < TPM2_PCR_SELECT_MAX; i++) {
        if (s->pcrSelect[i]) {
            return 1;
        }
    }
    return 0;
}

int
rats_tpm_has_bank(const rats_tpm_ctx *ctx, TPM2_ALG_ID hash)
{
    uint32_t i;

    for (i = 0; i < ctx->banks.count && i < TPM2_NUM_PCR_BANKS; i++) {
        if ((ctx->banks.pcrSelections[i].hash == hash) && selection_has_bits(&ctx->banks.pcrSelections[i])) {
            return 1;
        }
    }
    return 0;
}

int
rats_tpm_quote(rats_tpm_ctx *ctx, const uint8_t *nonce, size_t nonce_len, const TPML_PCR_SELECTION *sel,
        TPM2B_ATTEST **quoted, TPMT_SIGNATURE **sig, char *errmsg, size_t errlen)
{
    TPM2B_DATA qd = { .size = 0 };
    TPMT_SIG_SCHEME scheme = { .scheme = TPM2_ALG_NULL };   /* AK に設定された方式（ECDSA/SHA-256）を使う */
    TSS2_RC rc;

    if (nonce_len > sizeof qd.buffer) {
        snprintf(errmsg, errlen, "nonce of %zu bytes does not fit in TPM2B_DATA (%zu)", nonce_len, sizeof qd.buffer);
        return -1;
    }
    qd.size = (UINT16)nonce_len;
    memcpy(qd.buffer, nonce, nonce_len);

    /* AK の認可値は空なので、Esys_TR_SetAuth は不要。パスワードセッションで空の認可を送る */
    rc = Esys_Quote(ctx->esys_ctx, ctx->ak, ESYS_TR_PASSWORD, ESYS_TR_NONE, ESYS_TR_NONE, &qd, &scheme, sel, quoted, sig);
    if (rc) {
        return fail_rc(errmsg, errlen, "Esys_Quote", rc);
    }
    return 0;
}

/* remaining から、TPM が返した分（out）のビットを消す。tpm2-tools の pcr_update_pcr_selections と同じ */
static void
selection_clear(TPML_PCR_SELECTION *remaining, const TPML_PCR_SELECTION *out)
{
    uint32_t i, j;
    uint8_t k;

    for (i = 0; i < out->count && i < TPM2_NUM_PCR_BANKS; i++) {
        for (j = 0; j < remaining->count && j < TPM2_NUM_PCR_BANKS; j++) {
            if (remaining->pcrSelections[j].hash != out->pcrSelections[i].hash) {
                continue;
            }
            for (k = 0; k < remaining->pcrSelections[j].sizeofSelect && k < TPM2_PCR_SELECT_MAX; k++) {
                remaining->pcrSelections[j].pcrSelect[k] &= (uint8_t)~out->pcrSelections[i].pcrSelect[k];
            }
        }
    }
}

static int
selection_is_empty(const TPML_PCR_SELECTION *sel)
{
    uint32_t i;

    for (i = 0; i < sel->count && i < TPM2_NUM_PCR_BANKS; i++) {
        if (selection_has_bits(&sel->pcrSelections[i])) {
            return 0;
        }
    }
    return 1;
}

int
rats_tpm_pcr_read(rats_tpm_ctx *ctx, const TPML_PCR_SELECTION *sel, struct rats_pcr_values *values,
        char *errmsg, size_t errlen)
{
    TPML_PCR_SELECTION remaining = *sel;
    TSS2_RC rc;

    values->count = 0;
    while (!selection_is_empty(&remaining)) {
        UINT32 update_counter = 0;
        TPML_PCR_SELECTION *out = NULL;
        TPML_DIGEST *vals = NULL;
        uint32_t i;

        rc = Esys_PCR_Read(ctx->esys_ctx, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE, &remaining, &update_counter, &out, &vals);
        if (rc) {
            return fail_rc(errmsg, errlen, "Esys_PCR_Read", rc);
        }
        /* 1 回で返るのは最大 8 個。返ってきた順に積む（selection の順 = bank の順、PCR 番号の昇順） */
        if ((vals->count == 0) || selection_is_empty(out)) {
            Esys_Free(out);
            Esys_Free(vals);
            snprintf(errmsg, errlen, "Esys_PCR_Read returned no values for the remaining selection");
            return -1;
        }
        for (i = 0; i < vals->count && i < 8; i++) {
            if (values->count >= RATS_MAX_PCR_VALUES) {
                Esys_Free(out);
                Esys_Free(vals);
                snprintf(errmsg, errlen, "too many PCR values (more than %d)", RATS_MAX_PCR_VALUES);
                return -1;
            }
            values->digest[values->count++] = vals->digests[i];
        }
        selection_clear(&remaining, out);
        Esys_Free(out);
        Esys_Free(vals);
    }
    return 0;
}

int
rats_tpm_is_operational(rats_tpm_ctx *ctx)
{
    TPM2B_MAX_BUFFER *out = NULL;
    TPM2_RC test_result = TPM2_RC_FAILURE;
    TSS2_RC rc;

    if (!ctx->esys_ctx) {
        return 0;
    }
    rc = Esys_GetTestResult(ctx->esys_ctx, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE, &out, &test_result);
    Esys_Free(out);
    return (rc == TSS2_RC_SUCCESS) && (test_result == TPM2_RC_SUCCESS);
}
