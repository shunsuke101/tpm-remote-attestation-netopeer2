# sysrepo のデータストアはどこに、どう保存されているか

- 日付: 2026-10-07
- 分類: sysrepo の仕組み
- 関連: `doc/notes/sysrepo-shared-memory-2026-10-08.md`、`doc/notes/netopeer2-status-get-getconfig-2026-10-07.md`

この日の 3 つの質問への回答をまとめた。

---

## Q1. /dev/shm には running がある。operational はどこのファイルに保存されているのか

### 回答

operational は、running のように 1 つのファイルとして保存されてはいない。**読み出すたびに組み立てられるデータストア**。
sysrepo のソース（`sysrepo/src/plugins/common_json.c`、`common.c`）では、次の 3 つを合わせて作っている。

| 材料 | 置き場所 | 寿命 |
|---|---|---|
| ① running の設定 | `/dev/shm/sr_<モジュール>.running` | `/dev/shm` が消えるまで |
| ② push 型の状態データ | `/dev/shm/sr_<モジュール>.operational.<接続ID>-<セッションID>` | 書き込んだセッションが終わるまで |
| ③ pull 型の状態データ | **保存されない**（読み出しのたびにコールバックで作る） | その場限り |

- **① running の設定**: operational には config も含まれる。netopeer2 の `get` は、running を自分で読んだうえで、operational には
  「config 以外」を指定して読む（`SR_OPER_NO_CONFIG`）。そのため、netopeer2 の `get` に限っては、operational からは ②③ だけを受け取っている。
- **② push 型**: プロセスが operational に `sr_set_item()` などで状態データを書き込むと、セッションごとのファイルに保存される
  （`srpjson_get_oper_path()`、`common_json.c:661`）。書き込んだプロセスの接続（cid）とセッション（sid）に結び付いていて、そのセッションが終わると消える。
- **③ pull 型**: `sr_oper_get_subscribe()` でコールバックを登録しておくと、誰かが operational を読むたびに呼び出される。結果はどこにも保存されない。
  やり取りには `/dev/shm` のファイルを「受け渡し用の郵便受け」として使う（`sr_path_sub_shm()` と `sr_path_sub_data_shm()`、`common.c:1148`、`common.c:1167`）。
  - `/dev/shm/srsub_<mod>.oper.<ハッシュ>`: 要求と応答のイベントの通知
  - `/dev/shm/srsub_data_<mod>.oper.<ハッシュ>`: データの受け渡し

  guest で見た `srsub_ietf-subscribed-notifications.oper.45c2376e`（184 バイト）などがこれ。データの保管場所ではなく、netopeer2 が登録したコールバックの窓口。

### 各データストアの置き場所（この guest の場合）

guest の sysrepo は Debug ビルドなので、リポジトリは `/home/ubuntu/github/sysrepo/build/repository` にある。

| データストア | ファイル | 残るか |
|---|---|---|
| startup | `…/repository/data/<mod>.startup` | 残る（再起動しても消えない） |
| factory-default | `…/repository/data/<mod>.factory-default` | 残る |
| running | `/dev/shm/sr_<mod>.running` | 消える。共有メモリを作り直すときに startup から再作成される |
| candidate | `/dev/shm/sr_<mod>.candidate` | 変更したときだけ作られる |
| operational | 上の ①②③ を合わせたもの（② だけが `/dev/shm/sr_<mod>.operational.<cid>-<sid>`） | ② はセッションの間だけ、③ は残らない |
| 権限の情報 | `…/repository/data/<mod>.<データストア>.perm` | 残る |

ファイル名の先頭の `sr` は、共有メモリの接頭辞（既定値 `sr`）。

### 今回の設計との関係

- rats-attesterd は、`rats-support-structures/tpms/tpm` の `hardware-based`、`status`、`manufacturer` などを **pull 型（③）** で提供する設計（設計書 05 §3.7）。
  host から `get` されたときに初めてコールバックが呼ばれ、TPM の状態をその場で読んで返す。ファイルとしては残らない。
- rats-attesterd が止まっていると、これらの状態データは `get` の結果に出てこなくなる。running にある設定（`tpm0` や `ak-cert-0`）は出てくる。
- 追記（2026-10-07、設計書 04 §8 の確認で分かったこと）: sysrepo の operational には、そのモジュールの変更を「所有者」として購読しているプロセス
  （`sr_module_change_subscribe` に `SR_SUBSCR_PASSIVE` を付けないもの）がいる間だけ、running の設定が入る（`sysrepo/src/sysrepo_types.h:386-388`）。
  rats-attesterd が無い今は、`sysrepocfg -X -d operational` で `rats-support-structures` が空になるのが正しい。

---

## Q2. XPath の `/ietf-netconf-server:netconf-server/listen/endpoints/endpoint/name` は、`/dev/shm/sr_ietf-netconf-server.running` の内容のことか

### 回答

半分正しい。XPath が表しているのは「データの構造（どの項目か）」で、`sr_ietf-netconf-server.running` に入っているのは「その構造に沿った実際の値」。役割が分かれている。

#### XPath の各部分は YANG の定義を指している

XPath はファイルのパスではない。YANG モジュール `ietf-netconf-server` が定義しているデータの木の中の位置。
host の `~/github/libnetconf2/modules/ietf-netconf-server@2025-04-24.yang` では次のように定義されている。

| XPath の部分 | 意味 | YANG の定義 |
|---|---|---|
| `ietf-netconf-server:` | モジュール名（ファイル名やディレクトリではない） | `module ietf-netconf-server` |
| `netconf-server` | 最上位の container | 677 行目 `container netconf-server`（中身は grouping で定義） |
| `listen` | container | 444 行目 |
| `endpoints` | container | 463 行目 |
| `endpoint` | list（キーは `name`） | 466〜467 行目 |
| `name` | leaf | 471 行目 |

sysrepo は、導入したモジュールの YANG をリポジトリの `yang/` にコピーして持っている（guest では
`/home/ubuntu/github/sysrepo/build/repository/yang/`。ietf-yang-library の `location` でも確認できる）。

#### 値は sr_ietf-netconf-server.running に JSON で入っている

`get-config --source running` で返る値は、`/dev/shm/sr_ietf-netconf-server.running` から読まれている。
sysrepo は、running の値をモジュールごとに 1 ファイルずつ、**JSON 形式（RFC 7951）** で保存している。
書き込みは `ds_json.c:104` の `lyd_print_all(..., LYD_JSON, ...)`、読み込みは `ds_json.c:494` の `lyd_parse_data_fd(..., LYD_JSON, ...)`。

ファイルは改行の無い 1 行の JSON。`merge_config.sh` が作った設定なら、整形すると次のような形になるはず（例として書いたもので、実物は見ていない）。

```json
{
  "ietf-netconf-server:netconf-server": {
    "listen": {
      "endpoints": {
        "endpoint": [
          {
            "name": "default-ssh",
            "ssh": {
              "tcp-server-parameters": { "local-bind": [ { "local-address": "0.0.0.0" } ] },
              "ssh-server-parameters": { ... }
            }
          }
        ]
      }
    }
  }
}
```

XPath とこの JSON の対応:

- 最上位のキー `"ietf-netconf-server:netconf-server"` は、XPath の先頭と同じ「モジュール名:ノード名」の書き方。
- `listen` と `endpoints` は、入れ子のオブジェクト。
- list の `endpoint` は JSON の配列になり、各要素に `name` が入っている。

`get-config` を受け取ると、sysrepo はこのファイルを読んで libyang のデータの木に戻し、その木に XPath を当てて `name` を選び、
親の要素と一緒に XML にして返す。`--source startup` にすると、読む先が `…/repository/data/ietf-netconf-server.startup` に変わる。

#### vi で開かない

- 2026-10-07 時点では、このファイルが存在しない（`doc/notes/troubleshoot-sysrepo-running-removeipc-2026-10-06.md`）。
  `vi` で開くと新しい空のファイルとして開かれ、保存すると中身の無いファイルが作られ、sysrepo が読めなくなる。
- ファイルがある場合でも直接書き換えない。sysrepo を通らないので、YANG による検証、NACM、netopeer2 への変更の通知が行われない。
- 中身を見るとき: `sudo sysrepocfg -X -d running -m ietf-netconf-server -f json`（生のファイルなら `sudo cat`）。
- 変更するとき: `sudo sysrepocfg --edit` か、NETCONF の `edit-config`。

---

## Q3. sysrepocfg で、XML の内容を書き込んでいるという感じか

### 回答

ほぼその理解で合っている。ただし、XML をそのままファイルに書き込むのではなく、間に「YANG で検証して、データの木に変換する」段階が入る。

`sysrepocfg --import=rats-init.xml -d running -m ietf-tpm-remote-attestation` の流れ:

```
rats-init.xml（XML）
   │ ① libyang が YANG の定義に沿ってパースする（データの木になる）
   ▼
データの木（メモリ上）
   │ ② sysrepo が検証する（型、mandatory、must、when、leafref など）
   │    → ここで失敗したら何も書かれない
   ▼
   │ ③ 変更を購読しているプロセスに通知する（netopeer2 や、将来の rats-attesterd）
   ▼
JSON DS プラグインが JSON にして保存する
   ├─ -d running → /dev/shm/sr_ietf-tpm-remote-attestation.running
   └─ -d startup → …/repository/data/ietf-tpm-remote-attestation.startup
```

- **XML は入力の形式で、保存の形式は JSON**: 同じ内容を JSON で書いたファイルでも import できる（拡張子か `-f json` で判断される）。
- **YANG に合わない XML は書き込まれない**: 例えば `taa:TPM_ALG_SHA256` を書き間違えたり、feature `tpm20` が無効だったりすると、②で拒否される。`vi` で直接書き換えるのとの一番の違い。
- **`-m` で指定したモジュールの分だけが対象**: `--import` は、そのモジュールの既存のデータを XML の内容で**丸ごと置き換える**。
  `--edit` は既存のデータに**追加・変更（merge）**する。設計書 04 の §5 で `--import`、§7 の NACM で `--edit` を使っているのはこの違いのため（NACM の既存のルールを消さないため）。
- **読み出しは逆の流れ**: `sysrepocfg -X`（export）や NETCONF の `get-config` では、JSON ファイルを読んでデータの木に戻し、XML（または JSON）にして出力する。
