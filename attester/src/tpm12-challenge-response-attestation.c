/*
 * tpm12-challenge-response-attestation.c — RPC /ietf-tpm-remote-attestation:tpm12-challenge-response-attestation
 *
 * TPM 1.2 は対象外（doc/design/06_future_extensions.md §6、04 §1.2: feature ietf-tcg-algs:tpm12 は有効にしない）。
 * 購読しておいて operation-not-supported を返す。購読者がいないときの operation-failed より意図が伝わる。
 * ubuntu（verifiers）には NACM の rats-deny-others（exec deny）が先に効くので、通常はここまで届かない。
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
            "TPM 1.2 attestation is not supported by this attester (TPM 2.0 only)");
    return SR_ERR_UNSUPPORTED;
}

int
rats_tpm12_cra_subscribe(struct rats_attester *app, sr_session_ctx_t *sess)
{
    /* RPC 自体が if-feature で消えている構成では、購読先のノードが無いので何もしない */
    if (!rats_schema_has_node(sess, RATS_XPATH_TPM12_RPC)) {
        SRPLG_LOG_WRN(RATS_PLG_NAME, "%s is not in the schema (feature disabled); not subscribing", RATS_XPATH_TPM12_RPC);
        return SR_ERR_OK;
    }
    return sr_rpc_subscribe_tree(sess, RATS_XPATH_TPM12_RPC, rpc_cb, app, 0, 0, &app->sub);
}
