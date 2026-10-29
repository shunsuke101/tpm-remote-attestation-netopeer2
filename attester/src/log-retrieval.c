/*
 * log-retrieval.c — RPC /ietf-tpm-remote-attestation:log-retrieval
 *
 * event log の取得は将来の拡張（doc/design/06_future_extensions.md §1）。feature bios / ima を有効にしていないので、
 * log-type の identity も無い。購読しておいて operation-not-supported を返す。
 * 実装するときは、/sys/kernel/security/tpm0/binary_bios_measurements（TCG_PCR_EVENT2）と
 * /sys/kernel/security/ima/ascii_runtime_measurements（ima-ng）を 06 §1.3 の対応で bios-event-entry / ima-event-entry にする。
 */
#include <libyang/libyang.h>
#include <sysrepo.h>

#include "attester.h"

static int
rpc_cb(sr_session_ctx_t *session, uint32_t sub_id, const char *op_path, const struct lyd_node *input,
        sr_event_t event, uint32_t operation_id, struct lyd_node *output, void *private_data)
{
    (void)sub_id; (void)op_path; (void)input; (void)event; (void)operation_id; (void)output; (void)private_data;

    rats_nc_error(session, "operation-not-supported", NULL,
            "log-retrieval is not implemented (features bios/ima are disabled on this attester)");
    return SR_ERR_UNSUPPORTED;
}

int
rats_log_retrieval_subscribe(struct rats_attester *app, sr_session_ctx_t *sess)
{
    if (!rats_schema_has_node(sess, RATS_XPATH_LOG_RPC)) {
        SRPLG_LOG_WRN(RATS_PLG_NAME, "%s is not in the schema (feature disabled); not subscribing", RATS_XPATH_LOG_RPC);
        return SR_ERR_OK;
    }
    return sr_rpc_subscribe_tree(sess, RATS_XPATH_LOG_RPC, rpc_cb, app, 0, 0, &app->sub);
}
