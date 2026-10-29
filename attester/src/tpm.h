/*
 * tpm.h — ESAPI（tss2-esys）のラッパー。
 *
 * 設計書: doc/design/05_challenge_response.md
 *   §3.4 の 2〜5: 初期化（TctiLdr → Esys_Initialize → AK の読み込みと属性の確認 → GetCapability）
 *   §3.5 の 5〜7: TPM2_Quote、TPM2_PCR_Read（8 個ずつ）
 *   §3.7:         manufacturer（TPM2_PT_MANUFACTURER）、status（Esys_GetTestResult）
 *
 * ESYS_CONTEXT はスレッドセーフではない。呼び出し側（attester.h の tpm_lock）で直列化する。
 */
#ifndef RATS_TPM_H
#define RATS_TPM_H

#include <stddef.h>
#include <stdint.h>

#include <tss2/tss2_esys.h>
#include <tss2/tss2_tctildr.h>

/* 1 回の RPC で返せる PCR 値の上限（bank 数 × PCR 数）。struct rats_pcr_values は大きいので heap に置く */
#define RATS_MAX_PCR_VALUES (TPM2_NUM_PCR_BANKS * TPM2_MAX_PCRS)

typedef struct rats_tpm_ctx rats_tpm_ctx;
struct rats_tpm_ctx {
    TSS2_TCTI_CONTEXT *tcti_ctx;
    ESYS_CONTEXT *esys_ctx;
    ESYS_TR ak;                 /* Esys_TR_FromTPMPublic(ak_handle) */
    TPM2B_NAME ak_name;         /* 起動時に hex でログに出し、登録済みの値と照合できるようにする */
    TPM2B_NAME ak_qname;        /* TPMS_ATTEST.qualifiedSigner と同じもの（Verifier の検査 8） */
    TPM2_ALG_ID sig_hash;       /* AK の署名方式のハッシュ（ECDSA/SHA-256 なら TPM2_ALG_SHA256）。pcrDigest の再計算に使う */
    uint32_t pcr_count;         /* TPM2_PT_PCR_COUNT。PCR 番号の上限と sizeofSelect（(pcr_count + 7) / 8）に使う */
    char manufacturer[5];       /* TPM2_PT_MANUFACTURER の 4 文字 + NUL（swtpm/libtpms なら "IBM"） */
    TPML_PCR_SELECTION banks;   /* TPM2_CAP_PCRS。pcrSelect がすべて 0 の bank は PCR が割り当てられていない */
};

/* selection の順に並べた PCR 値（TPML_DIGEST は 1 回の PCR_Read 分 = 最大 8 個しか持てないので別に持つ） */
struct rats_pcr_values {
    uint32_t count;
    TPM2B_DIGEST digest[RATS_MAX_PCR_VALUES];
};

/* 初期化。0 で成功、-1 で失敗（errmsg に Tss2_RC_Decode の結果など）。失敗したときは中で後始末する tctiのcontextとesysのcontextをinitilizeする*/
int rats_tpm_init(rats_tpm_ctx *ctx, const char *tcti_conf, uint32_t ak_handle, char *errmsg, size_t errlen);
void rats_tpm_finalize(rats_tpm_ctx *ctx);

/* hash の bank に PCR が割り当てられていれば 1 */
int rats_tpm_has_bank(const rats_tpm_ctx *ctx, TPM2_ALG_ID hash);

/* TPM2_Quote（§3.5 の 5）。qualifyingData = nonce、scheme = TPM2_ALG_NULL（AK に設定された ECDSA/SHA-256 を使う）。
 * *quoted と *sig は Esys_Free で解放する */
int rats_tpm_quote(rats_tpm_ctx *ctx, const uint8_t *nonce, size_t nonce_len, const TPML_PCR_SELECTION *sel,
        TPM2B_ATTEST **quoted, TPMT_SIGNATURE **sig, char *errmsg, size_t errlen);

/* TPM2_PCR_Read を、残りが無くなるまで繰り返す（§3.5 の 7）。values は sel の順（bank の順、PCR 番号の昇順） */
int rats_tpm_pcr_read(rats_tpm_ctx *ctx, const TPML_PCR_SELECTION *sel, struct rats_pcr_values *values,
        char *errmsg, size_t errlen);

/* Esys_GetTestResult が TPM2_RC_SUCCESS なら 1（status = operational）、それ以外は 0 */
int rats_tpm_is_operational(rats_tpm_ctx *ctx);

#endif /* RATS_TPM_H */
