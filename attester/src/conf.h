/*
 * conf.h — /etc/rats-attester.conf（key = value 形式）の読み込み。doc/design/05_challenge_response.md §3.3
 *
 * AK のハンドルは YANG モデルに無いので、certificate-name との対応をこのファイルで持つ。
 * 将来 mtpm を使うときは、[certificate] のセクションを複数持てる形式に広げる（06 §4）。
 */
#ifndef RATS_CONF_H
#define RATS_CONF_H

#include <stddef.h>
#include <stdint.h>

struct rats_conf {
    char *tcti;              /* Tss2_TctiLdr_Initialize に渡す。例: device:/dev/tpmrm0 */
    char *tpm_name;          /* rats-support-structures/tpms/tpm[name] と一致させる。例: tpm0 */
    char *certificate_name;  /* RPC 出力の certificate-name（leafref）。例: ak-cert-0 */
    uint32_t ak_handle;      /* AK の永続ハンドル。例: 0x81010002 */
    int hardware_based;      /* 状態データ hardware-based。swtpm なら 0 */
    size_t nonce_min;        /* nonce の最小長（バイト） */
    size_t nonce_max;        /* nonce の最大長（バイト）。TPM2B_DATA に収まる上限は 64 */
    unsigned quote_retry;    /* Quote と PCR_Read の間に PCR が変わったときの再試行回数 */
};

/* 既定値（rats-attester.conf のサンプルと同じ値）を入れる */
void rats_conf_defaults(struct rats_conf *c);

/* path を読んで c を上書きする。0 で成功、-1 で失敗（errmsg に理由を書く） */
int rats_conf_load(struct rats_conf *c, const char *path, char *errmsg, size_t errlen);

void rats_conf_free(struct rats_conf *c);

#endif /* RATS_CONF_H */
