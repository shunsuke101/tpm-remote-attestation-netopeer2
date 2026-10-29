# netopeer2-cli の get-config で指定できるもの（フィルタの書き方）

- 日付: 2026-10-07
- 分類: NETCONF / netopeer2-cli の使い方
- 関連: `doc/notes/netopeer2-status-get-getconfig-2026-10-07.md`

## 質問

> `get-config --source running --filter-xpath /ietf-netconf-server:netconf-server/listen/endpoints/endpoint` 以外に何か指定できますか。
> 他の指定方法を教えて。

## 回答

CLI の usage と処理（`Netopeer2/cli/commands.c` の `cmd_getconfig`）で確認した。以下の例は実行していない。

### get-config のオプション

```
get-config --source running|startup|candidate
           [--filter-subtree[=<file>] | --filter-xpath <XPath>]
           [--defaults report-all|report-all-tagged|trim|explicit]
           [--out <file>] [--rpc-timeout <seconds>]
```

| オプション | 意味 |
|---|---|
| `--source` | 必須。読むデータストア。`startup` と `candidate` は、サーバがその capability を出しているときだけ使える（netopeer2 は両方出している） |
| `--filter-xpath <XPath>` | XPath で絞り込む |
| `--filter-subtree[=<file>]` | XML（subtree フィルタ）で絞り込む。ファイルを省略すると、エディタが開いてその場で書ける |
| `--defaults` | 既定値の扱い（with-defaults、RFC 6243） |
| `--out <file>` | 応答を画面ではなくファイルに保存する |
| `--rpc-timeout <秒>` | 応答を待つ時間。既定は 5 秒 |

`--filter-xpath` と `--filter-subtree` は同時には使えない（「Mixing … is not allowed」というエラーになる）。また、1 回のコマンドで指定できるフィルタは 1 つ。

### XPath フィルタの書き方

先頭のノードには、モジュール名を接頭辞として付ける。同じモジュールの子ノードには付けなくてよい。
別のモジュールが augment したノードには、そのモジュール名を付ける。

```text
# リストのキーで 1 件に絞る
--filter-xpath "/ietf-netconf-server:netconf-server/listen/endpoints/endpoint[name='default-ssh']"

# その中の一部だけ（待ち受けアドレスなど）
--filter-xpath "/ietf-netconf-server:netconf-server/listen/endpoints/endpoint[name='default-ssh']/ssh/tcp-server-parameters"

# ログインできるユーザー名の一覧
--filter-xpath "/ietf-netconf-server:netconf-server/listen/endpoints/endpoint/ssh/ssh-server-parameters/client-authentication/users/user/name"

# 条件で絞る（0.0.0.0 で待ち受けている endpoint の名前）
--filter-xpath "/ietf-netconf-server:netconf-server/listen/endpoints/endpoint[ssh/tcp-server-parameters/local-bind/local-address='0.0.0.0']/name"

# 複数を一度に（| で和集合）
--filter-xpath "/ietf-netconf-server:netconf-server | /ietf-keystore:keystore/asymmetric-keys/asymmetric-key/name"

# 全部（フィルタ無しと同じ）
--filter-xpath "/*"
```

このプロジェクトで使いそうな例:

```text
--filter-xpath /ietf-tpm-remote-attestation:rats-support-structures
--filter-xpath "/ietf-tpm-remote-attestation:rats-support-structures/tpms/tpm[name='tpm0']/tpm20-pcr-bank"
--filter-xpath /ietf-tpm-remote-attestation:rats-support-structures/attester-supported-algos
```

- XPath は「ノードを選ぶ式」にする必要がある。`count(...)` のように数値を返す式は使えない。
- NACM で読む権限が無いノードは、エラーにならずに結果から外れる。たとえば `/ietf-netconf-acm:nacm` は `default-deny-all` なので、ubuntu には空で返る。

### subtree フィルタ（XML で指定する）

RFC 6241 §6 の方式。ファイルに書いて `--filter-subtree=ファイル名` で渡す。
netopeer2 は受け取った subtree フィルタを、内部で XPath に変換して処理している（`Netopeer2/src/netconf.c:155`）。

**名前だけを取り出す**（中身が空の要素 `<name/>` は「選択ノード」で、その項目だけを返す指定）:

```xml
<netconf-server xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-server">
  <listen><endpoints><endpoint><name/></endpoint></endpoints></listen>
</netconf-server>
```

**特定の endpoint を丸ごと取り出す**（値の入った要素は「内容一致ノード」で、一致したエントリ全体を返す指定）:

```xml
<netconf-server xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-server">
  <listen><endpoints><endpoint><name>default-ssh</name></endpoint></endpoints></listen>
</netconf-server>
```

```text
> get-config --source running --filter-subtree=endpoint-name.xml
> get-config --source running --filter-subtree        ← エディタが開くので、上の XML をその場で書く
```

トップレベルに要素を複数並べると、それぞれの結果をまとめて返す（和集合）。

### --defaults: 既定値も表示する

```text
> get-config --source running --filter-xpath /ietf-netconf-server:netconf-server --defaults report-all
```

`merge_config.sh` はポート番号を設定していないので、普通に読むと `local-port` は出てこない。
`report-all` を付けると YANG の既定値も表示される。ietf-netconf-server は `local-port` の既定値を 830 にしている
（`ietf-netconf-server@2025-04-24.yang:213-214`）ので、`<local-port>830</local-port>` が出るはず。
`report-all-tagged` にすると、既定値の項目に印が付く。

### 関連するコマンド

| コマンド | 違い |
|---|---|
| `get` | running の設定に状態データも加えて返す。使えるオプションは `--filter-xpath`、`--filter-subtree`、`--defaults`、`--out` で、`--source` は無い |
| `get-data --datastore <ds>` | NMDA（RFC 8526）の取得方法。`operational` や `factory-default` も指定できる。`--config false`（状態データだけ）、`--depth <n>`（深さを制限）、`--with-origin` / `--origin`（値の出どころ）なども使える |
| `outputformat json` | 表示を JSON に切り替える（`outputformat xml` で元に戻る） |
| `get-config --help` など | そのコマンドの usage を表示する |

例えば `get-data --datastore operational --config false --filter-xpath /ietf-tpm-remote-attestation:rats-support-structures` とすると、
rats-attesterd が提供する状態データ（`hardware-based`、`status` など）だけを見られる。

なお、`ietf-netconf-server` の running ファイルが `/dev/shm` に無い状態のままだと、ietf-netconf-server を対象にした例は
「JSON DS file: Opening … failed」になる（`doc/notes/troubleshoot-sysrepo-running-removeipc-2026-10-06.md`）。
