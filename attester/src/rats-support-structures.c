/*
 * rats-support-structures.c — /ietf-tpm-remote-attestation:rats-support-structures/tpms/tpm の状態データ（config false）。
 * doc/design/05_challenge_response.md §3.7
 *
 * tpm[name=conf.tpm_name] の下に、running の config と合わせて次を返す。
 *   hardware-based  conf.hardware_based（swtpm なので false）
 *   path            conf.tcti の "device:" 以降（/dev/tpmrm0）
 *   manufacturer    TPM2_PT_MANUFACTURER の 4 文字（libtpms なら IBM）
 *   status          rats_tpm_is_operational() ? operational : non-operational
 */
#include <stdio.h>
#include <string.h>

#include <libyang/libyang.h>
#include <sysrepo.h>

#include "attester.h"

static int
oper_cb(sr_session_ctx_t *session, uint32_t sub_id, const char *module_name, const char *path,
        const char *request_xpath, uint32_t operation_id, struct lyd_node **parent, void *private_data)
{
    struct rats_attester *app = private_data;
    const char *prefix, *dev;
    char xpath[RATS_ERRLEN];
    int operational;
    size_t i;
    struct {
        const char *leaf;
        const char *value;
    } leaves[4];

    (void)sub_id; (void)module_name; (void)request_xpath; (void)operation_id;

    if (!*parent) {
        /* 購読したパスは入れ子なので、普通は tpms（か rats-support-structures）が渡される。無ければ作る */
        const struct ly_ctx *ctx = sr_session_acquire_context(session);
        LY_ERR lrc = lyd_new_path(NULL, ctx, RATS_XPATH_SUPPORT "/tpms", NULL, 0, parent);

        sr_session_release_context(session);
        if (lrc) {
            SRPLG_LOG_ERR(RATS_PLG_NAME, "oper_cb %s: lyd_new_path: %s", path, ly_last_logmsg());
            return SR_ERR_LY;
        }
    }
    /* 渡された parent が tpms なら "tpm[...]"、rats-support-structures なら "tpms/tpm[...]" から書く */
    prefix = strcmp(LYD_NAME(*parent), "tpms") ? "tpms/" : "";

    dev = app->conf.tcti;
    if (!strncmp(dev, "device:", 7)) {
        dev += 7;
    }

    pthread_mutex_lock(&app->tpm_lock);
    operational = rats_tpm_is_operational(&app->tpm);
    pthread_mutex_unlock(&app->tpm_lock);

    leaves[0].leaf = "hardware-based"; leaves[0].value = app->conf.hardware_based ? "true" : "false";
    leaves[1].leaf = "path";           leaves[1].value = dev;
    leaves[2].leaf = "manufacturer";   leaves[2].value = app->tpm.manufacturer;
    leaves[3].leaf = "status";         leaves[3].value = operational ? "operational" : "non-operational";

    for (i = 0; i < sizeof leaves / sizeof leaves[0]; i++) {
        snprintf(xpath, sizeof xpath, "%stpm[name='%s']/%s", prefix, app->conf.tpm_name, leaves[i].leaf);
        if (lyd_new_path(*parent, NULL, xpath, leaves[i].value, 0, NULL)) {
            SRPLG_LOG_ERR(RATS_PLG_NAME, "oper_cb %s: lyd_new_path(%s): %s", path, xpath, ly_last_logmsg());
            return SR_ERR_LY;
        }
    }
    return SR_ERR_OK;
}

int
rats_support_structures_subscribe(struct rats_attester *app, sr_session_ctx_t *sess)
{
    /* SR_SUBSCR_OPER_MERGE が必要。無いと sysrepo は、購読したパス（tpms/tpm）に既にあるデータ、つまり running から来た
     * tpm[name='tpm0'] の設定（firmware-version、tpm20-pcr-bank、certificates）を消してから、このコールバックの結果で置き換える
     * （sysrepo/src/modinfo.c の sr_lyd_xpath_complement）。すると operational から certificate[name='ak-cert-0'] が消え、
     * operational に対して行われる RPC 出力の検証で、certificate-name（leafref）の参照先が無いというエラーになる */
    return sr_oper_get_subscribe(sess, RATS_MOD_NAME, RATS_XPATH_TPM_OPER, oper_cb, app, SR_SUBSCR_OPER_MERGE, &app->sub);
}
