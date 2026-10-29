/*
 * attester.h — rats-attester plugin で共有する構造体と、各ソースファイルが提供する登録関数。
 *
 * 設計書: doc/design/05_challenge_response.md §3
 *
 * ファイルの分担:
 *   plugin.c                                sr_plugin_init_cb / sr_plugin_cleanup_cb（§3.4）、共通の補助関数
 *   conf.c                                  /etc/rats-attester.conf の解析（§3.3）
 *   tpm.c                                   ESAPI のラッパー（§3.4、§3.5、§3.7）
 *   rats-support-structures.c               /rats-support-structures/tpms/tpm の状態データ（§3.7）
 *   tpm20-challenge-response-attestation.c  RPC 本体（§3.5、§3.6）
 *   tpm12-challenge-response-attestation.c  スタブ: operation-not-supported（06 §6）
 *   log-retrieval.c                         スタブ: operation-not-supported（06 §1）
 */
#ifndef RATS_ATTESTER_H
#define RATS_ATTESTER_H

#include <pthread.h>
#include <stddef.h>
#include <sysrepo.h>

#include "conf.h"
#include "tpm.h"

/* sysrepo-plugind はファイル名（rats-attester.so）から plugin 名を決める。ログの接頭辞に使う */
#define RATS_PLG_NAME "rats-attester"

#define RATS_MOD_NAME        "ietf-tpm-remote-attestation"
#define RATS_XPATH_TPM20_RPC "/ietf-tpm-remote-attestation:tpm20-challenge-response-attestation"
#define RATS_XPATH_TPM12_RPC "/ietf-tpm-remote-attestation:tpm12-challenge-response-attestation"
#define RATS_XPATH_LOG_RPC   "/ietf-tpm-remote-attestation:log-retrieval"
#define RATS_XPATH_TPM_OPER  "/ietf-tpm-remote-attestation:rats-support-structures/tpms/tpm"
#define RATS_XPATH_SUPPORT   "/ietf-tpm-remote-attestation:rats-support-structures"

/* エラーメッセージ用のバッファの長さ */
#define RATS_ERRLEN 512

struct rats_attester {
    struct rats_conf conf;
    rats_tpm_ctx tpm;
    pthread_mutex_t tpm_lock;   /* ESYS_CONTEXT はスレッドセーフではない。TPM を呼ぶ区間を守る */
    sr_subscription_ctx_t *sub; /* すべての購読を 1 つにまとめ、コールバックを 1 スレッドで直列に処理させる（§3.4 の 8） */
    sr_session_ctx_t *run_sess; /* コールバックから running を読むためのセッション。RPC に渡される implicit session は
                                   operational なので別に持つ。コールバックは直列なので共有してよい */
};

/* running の tpm20-pcr-bank（tpm[name=conf.tpm_name] の下）。hash ごとの PCR のビットマップ */
struct rats_bank {
    TPM2_ALG_ID hash;
    uint8_t select[TPM2_PCR_SELECT_MAX];
};

struct rats_banks {
    uint32_t count;
    struct rats_bank bank[TPM2_NUM_PCR_BANKS];
};

/*
 * 購読の登録。どれも app->sub を共有する。
 * tpm12 と log-retrieval は、スキーマに RPC のノードが無い（feature が無効）ときは警告を出して登録しない。
 */
int rats_tpm20_cra_subscribe(struct rats_attester *app, sr_session_ctx_t *sess);
int rats_tpm12_cra_subscribe(struct rats_attester *app, sr_session_ctx_t *sess);
int rats_log_retrieval_subscribe(struct rats_attester *app, sr_session_ctx_t *sess);
int rats_support_structures_subscribe(struct rats_attester *app, sr_session_ctx_t *sess);

/* --- plugin.c の共通の補助関数 --- */

/* sr_session_set_netconf_error() の薄いラッパー（§3.6）。error-type は "application"、error-path は NULL でもよい */
void rats_nc_error(sr_session_ctx_t *sess, const char *error_tag, const char *error_path, const char *msg);

/* RPC のノードがスキーマにあるか（feature で消えていないか） */
int rats_schema_has_node(sr_session_ctx_t *sess, const char *xpath);

/* running から tpm[name=conf.tpm_name] を読み、certificate[name=conf.certificate_name] があることと、
 * tpm20-pcr-bank を banks に取り出す。sr_error_t を返す（SR_ERR_OK 以外のとき errmsg に理由） */
int rats_load_pcr_banks(sr_session_ctx_t *sess, const struct rats_conf *conf, struct rats_banks *banks,
        char *errmsg, size_t errlen);

/* identityref の値（"ietf-tcg-algs:TPM_ALG_SHA256" または "TPM_ALG_SHA256"）と TPM2_ALG_ID の相互変換。
 * 対応していなければ TPM2_ALG_NULL / NULL */
TPM2_ALG_ID rats_alg_from_identity(const char *identity);
const char *rats_identity_from_alg(TPM2_ALG_ID alg);

#endif /* RATS_ATTESTER_H */
