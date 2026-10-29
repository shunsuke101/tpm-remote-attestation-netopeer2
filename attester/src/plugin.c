/*
 * plugin.c — sysrepo-plugind の入口。doc/design/05_challenge_response.md §3.4（初期化と後始末）と共通の補助関数。
 *
 * sysrepo-plugind は rats-attester.so を dlopen し、sr_plugin_init_cb() と sr_plugin_cleanup_cb() を dlsym で探して呼ぶ
 * （sysrepo/src/executables/sysrepo-plugind.c）。セッション（running）は sysrepo-plugind が作って渡してくるので、
 * 自分では sr_connect も sr_disconnect もしない（設計書 §3.4 の 6、9）。
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libyang/libyang.h>
#include <sysrepo.h>
#include <sysrepo/error_format.h>   /* sr_session_set_netconf_error()（well-known error format "NETCONF" の helper） */

#include "attester.h"

#ifndef RATS_ATTESTER_CONF
#define RATS_ATTESTER_CONF "/etc/rats-attester.conf"
#endif

static const struct {
    const char *name;
    TPM2_ALG_ID alg;
} hash_algs[] = {
    { "TPM_ALG_SHA1",   TPM2_ALG_SHA1 },
    { "TPM_ALG_SHA256", TPM2_ALG_SHA256 },
    { "TPM_ALG_SHA384", TPM2_ALG_SHA384 },
    { "TPM_ALG_SHA512", TPM2_ALG_SHA512 },
};

TPM2_ALG_ID
rats_alg_from_identity(const char *identity)
{
    size_t i;
    const char *colon;

    if (!identity) {
        return TPM2_ALG_NULL;
    }
    /* libyang の lyd_get_value() は identityref を "module:identity" の形で返す */
    colon = strchr(identity, ':');
    if (colon) {
        identity = colon + 1;
    }
    for (i = 0; i < sizeof hash_algs / sizeof hash_algs[0]; i++) {
        if (!strcmp(identity, hash_algs[i].name)) {
            return hash_algs[i].alg;
        }
    }
    return TPM2_ALG_NULL;
}

const char *
rats_identity_from_alg(TPM2_ALG_ID alg)
{
    /* lyd_new_term() に渡す JSON 形式（module:identity）の文字列 */
    static const char *const identities[] = {
        "ietf-tcg-algs:TPM_ALG_SHA1", "ietf-tcg-algs:TPM_ALG_SHA256", "ietf-tcg-algs:TPM_ALG_SHA384", "ietf-tcg-algs:TPM_ALG_SHA512",
    };
    size_t i;

    for (i = 0; i < sizeof hash_algs / sizeof hash_algs[0]; i++) {
        if (hash_algs[i].alg == alg) {
            return identities[i];
        }
    }
    return NULL;
}

void
rats_nc_error(sr_session_ctx_t *sess, const char *error_tag, const char *error_path, const char *msg)
{
    /* §3.6: error-type は常に application。error-info は付けない */
    sr_session_set_netconf_error(sess, "application", error_tag, NULL, error_path, msg, 0);
}

int
rats_schema_has_node(sr_session_ctx_t *sess, const char *xpath)
{
    const struct ly_ctx *ctx;
    const struct lysc_node *node;

    ctx = sr_session_acquire_context(sess);
    node = lys_find_path(ctx, NULL, xpath, 0);
    sr_session_release_context(sess);
    return node != NULL;
}

int
rats_load_pcr_banks(sr_session_ctx_t *sess, const struct rats_conf *conf, struct rats_banks *banks,
        char *errmsg, size_t errlen)
{
    sr_data_t *data = NULL;
    struct ly_set *set = NULL;
    struct lyd_node *node;
    char xpath[RATS_ERRLEN];
    uint32_t i;
    LY_ERR lrc;
    int rc;

    memset(banks, 0, sizeof *banks);

    snprintf(xpath, sizeof xpath, RATS_XPATH_SUPPORT "/tpms/tpm[name='%s']", conf->tpm_name);
    rc = sr_get_data(sess, xpath, 0, 0, 0, &data);
    if (rc) {
        snprintf(errmsg, errlen, "sr_get_data(%s): %s", xpath, sr_strerror(rc));
        return rc;
    }
    if (!data || !data->tree) {
        snprintf(errmsg, errlen, "tpm[name='%s'] is not configured in running (doc/design/04 §5)", conf->tpm_name);
        rc = SR_ERR_NOT_FOUND;
        goto cleanup;
    }

    /* RPC 出力の certificate-name（leafref）の参照先があること */
    snprintf(xpath, sizeof xpath, RATS_XPATH_SUPPORT "/tpms/tpm[name='%s']/certificates/certificate[name='%s']",
            conf->tpm_name, conf->certificate_name);
    if (lyd_find_path(data->tree, xpath, 0, &node)) {
        snprintf(errmsg, errlen, "certificate[name='%s'] is not configured under tpm[name='%s'] (doc/design/04 §5)",
                conf->certificate_name, conf->tpm_name);
        rc = SR_ERR_NOT_FOUND;
        goto cleanup;
    }

    snprintf(xpath, sizeof xpath, RATS_XPATH_SUPPORT "/tpms/tpm[name='%s']/tpm20-pcr-bank", conf->tpm_name);
    lrc = lyd_find_xpath(data->tree, xpath, &set);
    if (lrc) {
        snprintf(errmsg, errlen, "lyd_find_xpath(%s): %s", xpath, ly_last_logmsg());
        rc = SR_ERR_LY;
        goto cleanup;
    }
    for (i = 0; i < set->count; i++) {
        const struct lyd_node *entry = set->dnodes[i], *child;
        struct rats_bank *b;
        const char *id;

        if (banks->count >= TPM2_NUM_PCR_BANKS) {
            snprintf(errmsg, errlen, "more than %d tpm20-pcr-bank entries", TPM2_NUM_PCR_BANKS);
            rc = SR_ERR_INVAL_ARG;
            goto cleanup;
        }
        b = &banks->bank[banks->count];

        /* tpm20-hash-algo は list のキーなので必ずある */
        if (lyd_find_path(entry, "tpm20-hash-algo", 0, &node)) {
            snprintf(errmsg, errlen, "tpm20-pcr-bank without tpm20-hash-algo");
            rc = SR_ERR_LY;
            goto cleanup;
        }
        id = lyd_get_value(node);
        b->hash = rats_alg_from_identity(id);
        if (b->hash == TPM2_ALG_NULL) {
            snprintf(errmsg, errlen, "tpm20-pcr-bank %s: hash algorithm not supported by this attester", id);
            rc = SR_ERR_UNSUPPORTED;
            goto cleanup;
        }
        for (child = lyd_child(entry); child; child = child->next) {
            unsigned long idx;

            if (strcmp(LYD_NAME(child), "pcr-index")) {
                continue;
            }
            idx = strtoul(lyd_get_value(child), NULL, 10);   /* 型 pcr は 0..31 を YANG が保証している */
            if (idx >= TPM2_MAX_PCRS) {
                snprintf(errmsg, errlen, "tpm20-pcr-bank %s: pcr-index %lu out of range", id, idx);
                rc = SR_ERR_INVAL_ARG;
                goto cleanup;
            }
            b->select[idx / 8] |= (uint8_t)(1u << (idx % 8));
        }
        banks->count++;
    }
    if (banks->count == 0) {
        snprintf(errmsg, errlen, "tpm[name='%s'] has no tpm20-pcr-bank (doc/design/04 §5)", conf->tpm_name);
        rc = SR_ERR_NOT_FOUND;
        goto cleanup;
    }
    rc = SR_ERR_OK;

cleanup:
    ly_set_free(set, NULL);
    sr_release_data(data);
    return rc;
}

/* §3.4 の 7: 設定が TPM と矛盾していないか */
static int
check_config_against_tpm(const struct rats_attester *app, const struct rats_banks *banks, char *errmsg, size_t errlen)
{
    uint32_t i;
    unsigned idx;

    for (i = 0; i < banks->count; i++) {
        const struct rats_bank *b = &banks->bank[i];
        const char *id = rats_identity_from_alg(b->hash);

        if (!rats_tpm_has_bank(&app->tpm, b->hash)) {
            snprintf(errmsg, errlen, "tpm20-pcr-bank %s is configured but the TPM has no PCRs in that bank", id);
            return SR_ERR_INVAL_ARG;
        }
        for (idx = 0; idx < TPM2_MAX_PCRS; idx++) {
            if ((b->select[idx / 8] & (1u << (idx % 8))) && (idx >= app->tpm.pcr_count)) {
                /* 設定としては許すが、その PCR を要求されると invalid-value になる（テスト E-8） */
                SRPLG_LOG_WRN(RATS_PLG_NAME, "tpm20-pcr-bank %s contains PCR %u but the TPM has only %" PRIu32 " PCRs",
                        id, idx, app->tpm.pcr_count);
            }
        }
    }
    return SR_ERR_OK;
}

static void
bytes_to_hex(const uint8_t *in, size_t len, char *out, size_t outlen)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < len && 2 * i + 2 < outlen; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 0x0f];
    }
    out[(2 * i < outlen) ? 2 * i : outlen - 1] = '\0';
}

/*
 * モジュールの変更の「所有者」になるための購読。処理は何もしない。
 * sysrepo は、変更を（SR_SUBSCR_PASSIVE 無しで）購読しているプロセスがいる間だけ、そのモジュールの running の設定を
 * operational に入れる（sysrepo/src/sysrepo_types.h:386-388）。これが無いと、§3.7 の状態データだけが operational に
 * 現れ、設定と並べて見られない。
 */
static int
change_cb(sr_session_ctx_t *session, uint32_t sub_id, const char *module_name, const char *xpath, sr_event_t event,
        uint32_t operation_id, void *private_data)
{
    (void)session; (void)sub_id; (void)xpath; (void)event; (void)operation_id; (void)private_data;

    SRPLG_LOG_INF(RATS_PLG_NAME, "configuration of %s changed; it is re-read on the next RPC", module_name);
    return SR_ERR_OK;
}

static void
app_destroy(struct rats_attester *app)
{
    if (!app) {
        return;
    }
    /* §3.4 の 9 の順: 購読を外す → セッションを閉じる → TPM を閉じる → 設定を解放する */
    if (app->sub) {
        sr_unsubscribe(app->sub);
    }
    if (app->run_sess) {
        sr_session_stop(app->run_sess);
    }
    rats_tpm_finalize(&app->tpm);
    rats_conf_free(&app->conf);
    pthread_mutex_destroy(&app->tpm_lock);
    free(app);
}

/*sysrepo-plugindがrats-attester.soをdlopenし、dlsymでこの関数を呼び出し実行する*/
int
sr_plugin_init_cb(sr_session_ctx_t *session, void **private_data)
{
    struct rats_attester *app;
    struct rats_banks banks;
    const char *conf_path;
    char err[RATS_ERRLEN], hex[2 * sizeof(((TPM2B_NAME *)0)->name) + 1];
    uint32_t i;
    int rc;

    app = calloc(1, sizeof *app);
    if (!app) {
        SRPLG_LOG_ERR(RATS_PLG_NAME, "calloc failed");
        return SR_ERR_NO_MEMORY;
    }
    pthread_mutex_init(&app->tpm_lock, NULL);
    rats_conf_defaults(&app->conf);

    /* 1. 設定ファイル */
    conf_path = getenv("RATS_ATTESTER_CONF");
    if (!conf_path) {
        conf_path = RATS_ATTESTER_CONF;
    }
    if (rats_conf_load(&app->conf, conf_path, err, sizeof err)) {
        SRPLG_LOG_ERR(RATS_PLG_NAME, "config: %s", err);
        rc = SR_ERR_INVAL_ARG;
        goto fail;
    }
    SRPLG_LOG_INF(RATS_PLG_NAME, "config %s: tcti=%s tpm-name=%s certificate-name=%s ak-handle=0x%08" PRIx32
            " hardware-based=%s nonce=%zu..%zu quote-retry=%u", conf_path, app->conf.tcti, app->conf.tpm_name,
            app->conf.certificate_name, app->conf.ak_handle, app->conf.hardware_based ? "true" : "false",
            app->conf.nonce_min, app->conf.nonce_max, app->conf.quote_retry);

    /* 2〜5. TPM と AK */
    if (rats_tpm_init(&app->tpm, app->conf.tcti, app->conf.ak_handle, err, sizeof err)) {
        SRPLG_LOG_ERR(RATS_PLG_NAME, "TPM: %s", err);
        rc = SR_ERR_OPERATION_FAILED;
        goto fail;
    }
    bytes_to_hex(app->tpm.ak_name.name, app->tpm.ak_name.size, hex, sizeof hex);
    SRPLG_LOG_INF(RATS_PLG_NAME, "TPM 2.0 manufacturer=%s pcr-count=%" PRIu32 "; AK 0x%08" PRIx32 " name=%s",
            app->tpm.manufacturer, app->tpm.pcr_count, app->conf.ak_handle, hex);
    bytes_to_hex(app->tpm.ak_qname.name, app->tpm.ak_qname.size, hex, sizeof hex);
    SRPLG_LOG_INF(RATS_PLG_NAME, "AK qualified name=%s (compare with build/verifier/enroll/ak-cert-0.qname)", hex);
    for (i = 0; i < app->tpm.banks.count && i < TPM2_NUM_PCR_BANKS; i++) {
        const TPMS_PCR_SELECTION *s = &app->tpm.banks.pcrSelections[i];
        const char *id = rats_identity_from_alg(s->hash);

        bytes_to_hex(s->pcrSelect, s->sizeofSelect, hex, sizeof hex);
        SRPLG_LOG_INF(RATS_PLG_NAME, "TPM bank %s (0x%04x): pcrSelect=%s", id ? id : "?", s->hash, hex);
    }

    /* 6. sysrepo には自分で接続しない。コールバック用に running のセッションだけ作る */
    rc = sr_session_start(sr_session_get_connection(session), SR_DS_RUNNING, &app->run_sess);
    if (rc) {
        SRPLG_LOG_ERR(RATS_PLG_NAME, "sr_session_start(running): %s", sr_strerror(rc));
        goto fail;
    }

    /* 7. running の設定を確かめる */
    rc = rats_load_pcr_banks(session, &app->conf, &banks, err, sizeof err);
    if (rc) {
        SRPLG_LOG_ERR(RATS_PLG_NAME, "running config: %s", err);
        goto fail;
    }
    rc = check_config_against_tpm(app, &banks, err, sizeof err);
    if (rc) {
        SRPLG_LOG_ERR(RATS_PLG_NAME, "running config: %s", err);
        goto fail;
    }
    for (i = 0; i < banks.count; i++) {
        bytes_to_hex(banks.bank[i].select, sizeof banks.bank[i].select, hex, sizeof hex);
        SRPLG_LOG_INF(RATS_PLG_NAME, "tpm20-pcr-bank %s: pcrSelect=%s", rats_identity_from_alg(banks.bank[i].hash), hex);
    }

    /* 8. 購読（すべて app->sub を共有し、1 スレッドで直列に処理される） */
    if ((rc = rats_tpm20_cra_subscribe(app, session)) || (rc = rats_tpm12_cra_subscribe(app, session)) ||
            (rc = rats_log_retrieval_subscribe(app, session)) || (rc = rats_support_structures_subscribe(app, session)) ||
            (rc = sr_module_change_subscribe(session, RATS_MOD_NAME, NULL, change_cb, app, 0, SR_SUBSCR_DONE_ONLY,
            &app->sub))) {
        SRPLG_LOG_ERR(RATS_PLG_NAME, "subscribe: %s", sr_strerror(rc));
        goto fail;
    }

    SRPLG_LOG_INF(RATS_PLG_NAME, "initialized: serving %s", RATS_XPATH_TPM20_RPC);
    *private_data = app;
    return SR_ERR_OK;

fail:
    /* 初期化に失敗した plugin の cleanup は呼ばれないので、ここで片付ける */
    app_destroy(app);
    return rc;
}

/*void sessionを書く必要があるのか*/
void
sr_plugin_cleanup_cb(sr_session_ctx_t *session, void *private_data)
{
    (void)session;

    app_destroy(private_data);
}
