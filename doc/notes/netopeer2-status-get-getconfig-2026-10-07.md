# netopeer2-cli の status / get / get-config はどう動いているか

- 日付: 2026-10-07
- 分類: NETCONF / netopeer2 / sysrepo の仕組み
- 関連: `doc/notes/netopeer2-cli-getconfig-options-2026-10-07.md`、`doc/notes/sysrepo-datastore-storage-2026-10-07.md`

## 質問

> `netopeer2-cli` の `status`、`get`、`get-config --source running --filter-xpath /ietf-netconf-server:netconf-server/listen/endpoints/endpoint/name`
> は、どのように動いているか知りたい。

## 回答

ソース（netopeer2 v2.8.7、libnetconf2 v4.4.10、sysrepo v5.1.0）を読んで追った。

### 共通: connect の時点で準備されるもの

```
netopeer2-cli ─ TCP ─► localhost:8300 ─(QEMU hostfwd)─► guest:830 netopeer2-server
   libnetconf2 + libssh             SSH（公開鍵認証）→ subsystem "netconf" → <hello> の交換
```

1. **SSH 接続**: libssh で SSH 接続し、`~/.netopeer2-cli/config.xml` に書いた鍵で公開鍵認証を行う。認証が通ったら、SSH の subsystem `netconf` を開く（RFC 6242）。
2. **`<hello>` の交換**: クライアントとサーバが、使える capability の一覧を送り合う。サーバの `<hello>` には session-id が入っている。
   両方が base:1.1 に対応しているので、以降のメッセージは chunked framing でやり取りされる。
3. **サーバ側の準備**: netopeer2-server は、この NETCONF セッション専用の sysrepo セッションを作る（`Netopeer2/src/common.c:732-740`）。
   - `orig_name = "netopeer2"`
   - NETCONF のセッション ID と、ユーザー名 `ubuntu`
   - `sr_nacm_set_user(sess, "ubuntu")`: 以降、このセッションのデータの読み書きには、すべて ubuntu として NACM が適用される

### status: サーバとは通信しない

`status` は RPC を送らない。connect のときに保存しておいた情報を表示するだけ（`Netopeer2/cli/commands.c:2839` 付近）。

| 表示 | 中身 |
|---|---|
| ID | サーバの `<hello>` に入っていた session-id |
| Host / Port | 接続先（localhost / 8300） |
| Transport | SSH |
| Capabilities | サーバの `<hello>` に入っていた capability の一覧 |

YANG 1.1 のモジュール（`ietf-tpm-remote-attestation` など）は、capability の一覧に出てこない。
libnetconf2 は、hello を作るときに YANG 1.1 のモジュールを飛ばしている（`libnetconf2/src/session.c:1446`）。

### get: running の設定と状態データを合わせて返す

**クライアント**: `<get/>` を作り、`<rpc message-id="N">` で包んで送る（`libnetconf2/src/session_client.c:2911`）。

```xml
<rpc message-id="2" xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"><get/></rpc>
```

**サーバ**:

1. libnetconf2 が受け取った XML を libyang でパースし、netopeer2 のコールバックに渡す。`get` と `get-config` は `np2srv_rpc_get_cb` が処理する（`Netopeer2/src/main.c:298, 308`）。
2. フィルタが無いので、XPath は `"/*"`（全部）になる（`netconf.c:172`）。
3. `np_get_rpc_data()`（`netconf.c:53`）が、データを 2 回に分けて集めて合わせる。
   - **running** から、全モジュールの設定を読む（`SR_DS_RUNNING`）
   - **operational** から、状態データ（config false）だけを読んで追加する（`SR_DS_OPERATIONAL` + `SR_OPER_NO_CONFIG`）。状態データを提供するのは次のもの。
     - sysrepo 自身（ietf-yang-library など）
     - netopeer2（netconf-state のセッション一覧など）
     - 将来は rats-attesterd（`rats-support-structures` の `status` など）
4. 実際の読み出しは `sr_get_data()` で行う（`common.c:1269`）。セッションに NACM のユーザーが設定されているので、
   ubuntu に読む権限が無いノード（鍵の秘密部分など）は、エラーにならずに結果から外される。
5. 結果を `<rpc-reply><data>…</data></rpc-reply>` で返す。

2026-10-06 に `get` だけが失敗したのは、手順 3 で**全モジュールの running を読みに行く**から。
`/dev/shm/sr_ietf-subscribed-notifications.running` が無かったので、そこでエラーになった。`status` は通信しないので影響を受けなかった。

### get-config --source running --filter-xpath …: running の設定だけを、絞り込んで返す

**クライアント**: `--filter-xpath` の値が `/` で始まるので、XPath フィルタとして送る。

```xml
<rpc message-id="3" xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
  <get-config>
    <source><running/></source>
    <filter type="xpath" select="/ietf-netconf-server:netconf-server/listen/endpoints/endpoint/name"/>
  </get-config>
</rpc>
```

- XPath フィルタを使えるのは、サーバが `:xpath` capability を出しているから。
- `select` の接頭辞には、モジュール名（`ietf-netconf-server:`）をそのまま書いている。RFC 6241 の規定では XML の名前空間接頭辞を使うことになっているが、
  libnetconf2 と netopeer2 はモジュール名の形でも受け付ける（libyang の JSON 形式の XPath として扱うため）。

**サーバ**:

1. `np2srv_rpc_get_cb` が `<source>` を見て、対象を `SR_DS_RUNNING` に決める（`netconf.c:126-137`）。
2. `filter` の `type="xpath"` を確認し、`select` の文字列をそのまま XPath として使う（`netconf.c:141-167`）。
3. `get` と違い、operational（状態データ）とは合わせない。`np_op_filter_data_get()` で running だけを読む（`netconf.c:185` → `common.c:1269` の `sr_get_data()`）。NACM の適用は `get` と同じ。
4. `sr_get_data()` は、選ばれたノードを親ノードとリストのキーを付けて返す。

```xml
<data xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
  <netconf-server xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-server">
    <listen><endpoints><endpoint><name>default-ssh</name></endpoint></endpoints></listen>
  </netconf-server>
</data>
```

`default-ssh` は、netopeer2 のインストール時に `merge_config.sh` が作った endpoint の名前。

### まとめ

| コマンド | 通信 | 読むデータストア | NACM | running ファイルが無いと |
|---|---|---|---|---|
| `status` | しない | なし（hello の情報を表示） | 関係なし | 影響なし |
| `get` | `<get>` | running + operational（状態） | 適用 | 全モジュールが対象なので失敗 |
| `get-config --source running --filter-xpath X` | `<get-config>` | running だけ | 適用 | X に含まれるモジュールの分が無いと失敗 |

### ⑤ の RFC 9684 の RPC との違い

RFC 9684 の RPC は ietf-netconf の RPC ではないので、`np2srv_rpc_get_cb` などは通らない。
`sr_rpc_send_tree()`（`main.c:409`）で sysrepo に渡され、そこから購読している rats-attesterd に届く。
