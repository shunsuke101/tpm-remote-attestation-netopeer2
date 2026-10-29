# RFC 9684 の RPC が rats-attesterd に届いて返るまで / rpc_cb() とは何か

- 日付: 2026-10-08
- 分類: NETCONF / netopeer2 / sysrepo の仕組み、rats-attesterd の設計
- 関連: `doc/design/05_challenge_response.md` §3・§4、`doc/notes/netopeer2-status-get-getconfig-2026-10-07.md`、`doc/notes/sysrepo-shared-memory-2026-10-08.md`

この日の 2 つの質問への回答をまとめた。

---

## Q1. user-rpc を送ったあと、netopeer2-server が受け付け、sysrepo が動作し、rats-attesterd が処理し、sysrepo に戻り、netopeer2-server が rpc-reply として送り返すのか

> 設計書 05 §3 の rats-attesterd の設計で、`user-rpc --content challenge.xml --out reply.xml` を送ったあと、
> netopeer2-server が受け付けて、sysrepo が動作して、rats-attesterd が処理して、sysrepo に送って、
> netopeer2-server に処理されて rpc-reply として送り返されるのですか。

### 回答

その流れで合っている。ソース（netopeer2 v2.8.7、sysrepo v5.1.0）で確認したところ、その間で「パースと YANG の検証」「NACM の確認」「出力の検証」も行われている。
以下は ⑤ を実装したときの流れ。

```
host: netopeer2-cli                     guest: netopeer2-server        sysrepo（共有メモリ）            rats-attesterd
  user-rpc --content challenge.xml
  ① <rpc> を作って SSH で送る ───────► ② 受信・パース
                                        ③ ietf-netconf 以外の RPC なので
                                           sr_rpc_send_tree() へ ─────► ④ NACM の確認（exec）
                                                                        ⑤ 入力の YANG 検証
                                                                        ⑥ 購読者に通知 ─────────────► ⑦ rpc_cb() が呼ばれる
                                                                                                         TPM2_Quote / PCR_Read
                                                                                                         output の木を作る
                                                                        ⑧ 出力を受け取る ◄────────────── SR_ERR_OK を返す
                                                                        ⑨ 出力の YANG 検証
                                        ⑩ <rpc-reply> を作る ◄─────────
  ⑪ 受信して reply.xml に書く ◄──────── SSH で送り返す
```

#### 各段階で起きていること

| # | 担当 | 処理 | ソース |
|---|---|---|---|
| ① | netopeer2-cli（libnetconf2） | `challenge.xml` の中身を `<rpc message-id="N">` で包み、SSH の netconf チャネルに送る | — |
| ② | netopeer2-server（libnetconf2） | 受信した XML を libyang でパースする。スキーマに無い要素や型の誤り（base64 でない nonce など）はここで弾かれる | — |
| ③ | netopeer2-server | RPC のモジュールが ietf-netconf ではないので、自分では処理せずに `sr_rpc_send_tree()` で sysrepo に渡す | `Netopeer2/src/main.c:409` |
| ④ | sysrepo | セッションに設定された NACM のユーザー（ubuntu）で、exec の権限を確認する。`verifiers` の `permit-tpm20-attestation` に一致して許可される | `sysrepo/src/sysrepo.c` の `sr_rpc_send_tree_internal`（「check NACM」） |
| ⑤ | sysrepo | 入力を YANG で検証する。running のデータも参照するので、`tpm20-hash-algo` の must（`attester-supported-algos/tpm20-hash` に含まれるか）はここで確認される | `_sr_rpc_send_tree` の `sr_modinfo_op_validate(..., 0)` |
| ⑥ | sysrepo | 共有メモリ `/dev/shm/srsub_ietf-tpm-remote-attestation.rpc…` に入力を書き、rats-attesterd のイベントパイプ（`repository/sr_evpipe<番号>`）に書き込んで起こす。netopeer2 のスレッドは応答を待つ | `sr_shmsub_rpc_notify()` |
| ⑦ | rats-attesterd | 購読のスレッドが起きて `rpc_cb(session, ..., input, ..., output, ...)` を呼ぶ。nonce と PCR の範囲を検査し、TPM2_Quote と PCR_Read を行い、`output` に `tpm20-attestation-response` を組み立てて `SR_ERR_OK` を返す | 設計書 05 §3.5 |
| ⑧ | sysrepo | rats-attesterd が作った output を、共有メモリを通して netopeer2 側で受け取る | — |
| ⑨ | sysrepo | 出力を YANG で検証する。`certificate-name` の leafref（running に `ak-cert-0` があるか）はここで確認される | `sr_modinfo_op_validate(..., 1)` |
| ⑩ | netopeer2-server（libnetconf2） | output を `<rpc-reply message-id="N">` にして送り返す。RPC の出力は `<data>` で包まれず、`<tpm20-attestation-response>` が `<rpc-reply>` の直下に来る | — |
| ⑪ | netopeer2-cli | 受け取った応答を、`--out` で指定した `reply.xml` に書く | — |

sysrepo は常駐するプロセスではない。④⑤⑥⑧⑨は、netopeer2-server のプロセスの中で、sysrepo のライブラリとして実行されている。
rats-attesterd とのやり取りは、共有メモリとイベントパイプだけで行われる（`doc/notes/sysrepo-shared-memory-2026-10-08.md`）。

ietf-netconf の RPC（`get`、`get-config` など）の場合は、③で netopeer2 自身が処理する。そのときは netopeer2 が
`lyd_validate_op()` で検証し、`sr_nacm_check_operation()` で NACM を確認する（`Netopeer2/src/main.c:373-381`）。

#### 途中で失敗した場合に返るもの

| 失敗する場所 | 例 | 返る rpc-error |
|---|---|---|
| ② パース | nonce が base64 でない | invalid-value など |
| ④ NACM | `verifiers` に属さないユーザー | access-denied |
| ⑤ 入力の検証 | `TPM_ALG_SHA384` を指定した（must の違反）、PCR が 32 以上 | invalid-value など（YANG の error-message 付き） |
| ⑥ 購読者がいない | rats-attesterd が止まっている | operation-failed |
| ⑦ rats-attesterd の判断 | bank に無い PCR、nonce の長さが不正、TPM のエラー | `sr_session_set_netconf_error()` で設定したもの（設計書 05 §3.6） |
| ⑨ 出力の検証 | running に `ak-cert-0` が無い | operation-failed など |
| 時間切れ | Quote に時間がかかりすぎた | CLI の `--rpc-timeout`（既定 5 秒）か、sysrepo の待ち時間を超えるとエラー |

---

## Q2. rpc_cb() は何かのライブラリの関数？それとも自作？

### 回答

`rpc_cb()` は **自作の関数**。設計書 05 §3.5 で、rats-attesterd の `src/rpc_cb.c` に書く予定のもの（2026-10-08 時点では未実装で、設計書の中にだけある）。
名前も設計で付けたもので、`tpm20_attestation_cb` など何でもよい。

ただし、**引数と戻り値の形は sysrepo が決めている**。自作の関数を sysrepo に登録しておき、RPC が届いたときに sysrepo のほうから呼んでもらう「コールバック関数」という仕組み。

#### 役割の分担

| もの | 誰が作るか | 誰が呼ぶか |
|---|---|---|
| `sr_rpc_tree_cb`（コールバックの型） | sysrepo（`sysrepo/src/sysrepo_types.h:594`） | — |
| `sr_rpc_subscribe_tree()`（登録する関数） | sysrepo のライブラリ（`sysrepo/src/sysrepo.h:1763`） | rats-attesterd が起動時に呼ぶ |
| `rpc_cb()`（処理の中身） | **自作**（rats-attesterd） | RPC が届いたときに sysrepo が呼ぶ |

sysrepo が決めている型:

```c
typedef int (*sr_rpc_tree_cb)(sr_session_ctx_t *session, uint32_t sub_id, const char *op_path,
        const struct lyd_node *input, sr_event_t event, uint32_t operation_id,
        struct lyd_node *output, void *private_data);
```

自作の関数をこの形に合わせて書き、登録する:

```c
/* 自作: 形は sr_rpc_tree_cb に合わせる */
static int
rpc_cb(sr_session_ctx_t *session, uint32_t sub_id, const char *op_path,
       const struct lyd_node *input, sr_event_t event, uint32_t operation_id,
       struct lyd_node *output, void *private_data)
{
    /* input から nonce と PCR selection を取り出す
     * → Esys_Quote()、Esys_PCR_Read()（tpm2-tss のライブラリ）
     * → lyd_new_list() / lyd_new_term()（libyang のライブラリ）で output を組み立てる */
    return SR_ERR_OK;
}

/* 起動時: ライブラリの関数で登録する（rpc_cb を引数として渡す） */
sr_rpc_subscribe_tree(sess, "/ietf-tpm-remote-attestation:tpm20-challenge-response-attestation",
                      rpc_cb, &app, 0, 0, &sub);
```

Q1 の流れの図に当てはめると、⑥で sysrepo が rats-attesterd のスレッドを起こし、⑦でこの `rpc_cb()` を呼ぶ。
rats-attesterd の中身はほとんどがライブラリの関数を組み合わせたもので、sysrepo、libyang、tpm2-tss（`Esys_Quote`、`Tss2_MU_*`）の関数を、
自作のコールバックの中で順番に呼ぶ。

#### 紛らわしい名前の関数

netopeer2 にも `np2srv_rpc_cb()`（`Netopeer2/src/main.c:279`）という似た名前の関数がある。
こちらは netopeer2 の内部の関数で、libnetconf2 から呼ばれ、届いた RPC の振り分けをしている（Q1 の③）。rats-attesterd の `rpc_cb()` とは別物。
