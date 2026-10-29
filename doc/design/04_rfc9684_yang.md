# ④ RFC 9684 の YANG モジュールの抽出と導入

[README](README.md) の手順 ④ の詳細。RFC から YANG を抽出し、host で検証してから guest の sysrepo に導入する。
続けて、初期設定（`rats-support-structures`）と NACM を入れる。

## 1. 導入するモジュール

| モジュール | リビジョン | 出典 | 役割 | 導入のしかた |
|---|---|---|---|---|
| `ietf-tcg-algs` | 2024-12-05 | RFC 9684 | TPM のアルゴリズムの identity（`TPM_ALG_SHA256`、`TPM_ALG_ECDSA`、`tpm20` など）と feature | `sysrepoctl -i` で implemented にし、`-e tpm20` |
| `ietf-tpm-remote-attestation` | 2024-12-05 | RFC 9684 | RPC と `rats-support-structures` | `sysrepoctl -i`（feature は有効にしない） |
| `ietf-hardware` | 2018-03-13 | RFC 8348 | RFC 9684 が import している（`hw:entity-mib` の参照だけ） | `-s` の検索先に置き、import として自動で入れる |
| `iana-hardware` | 2018-03-13 | RFC 8348 | ietf-hardware が import している | 同上 |
| `ietf-keystore` | 2024-10-10 | RFC 9642 | `keystore-ref` の参照先 | netopeer2 の `setup.sh` で導入済み。`central-keystore-supported` と `asymmetric-keys` が有効 |
| `ietf-yang-types` | 2025-12-22 | libyang に内蔵 | `yang:date-and-time` など | 作業不要 |
| `ietf-inet-types` | 2025-12-22 | libyang に内蔵 | ietf-hardware が import している | 作業不要 |

RFC 9684 の `import` には `revision-date` が付いていないので、sysrepo に入っているリビジョンがそのまま使われる。

### 1.1 依存関係

```
ietf-tpm-remote-attestation@2024-12-05
 ├── import ietf-tcg-algs        (prefix taa)  ← implemented、feature tpm20
 ├── import ietf-keystore        (prefix ks)   ← netopeer2 が導入済み
 │     └── ietf-crypto-types, ietf-netconf-acm
 ├── import ietf-hardware        (prefix hw)   ← import only
 │     ├── iana-hardware                       ← import only
 │     ├── ietf-inet-types                     ← libyang 内蔵
 │     └── ietf-yang-types                     ← libyang 内蔵
 └── import ietf-yang-types      (prefix yang) ← libyang 内蔵
```

### 1.2 feature を有効にする・しない理由

| feature | 有効化 | 理由 |
|---|---|---|
| `ietf-tcg-algs:tpm20` | **する** | RPC `tpm20-challenge-response-attestation` に `if-feature "taa:tpm20"` がある。identity の `TPM_ALG_SHA256`、`TPM_ALG_ECDSA`、`tpm20` と、`tpm20-pcr-bank` にも同じ if-feature がある |
| `ietf-tcg-algs:tpm12` | しない | TPM 1.2 は無い |
| `mtpm` | しない | TPM は 1 つ。無効にすると、RPC の入力に `certificate-name` が現れず、`compute-node` も無くなる |
| `bios`, `ima`, `netequip_boot` | しない | log-retrieval は今回実装しない（[06](06_future_extensions.md) §1） |
| `ietf-hardware:entity-mib` | しない（import only なので自動的に無効） | ENTITY-MIB は使わない。`physical-index` が現れなくなる |

注意: libyang では、implemented でない（import only の）モジュールの feature は常に無効になる。
`ietf-tcg-algs` は data node を持たないが、feature `tpm20` を有効にするために implemented にする必要がある。

## 2. 抽出

`doc/` の PDF は、ページのヘッダやフッタ、改ページが本文に混じるので、YANG の抽出には向かない。
RFC のテキスト版を取得し、`xym` で `<CODE BEGINS> file "..."` から `<CODE ENDS>` までを切り出す。

```bash
# host
cd ~/github/tpm-remote-attestation-netopeer2/attester/yang
curl -LO https://www.rfc-editor.org/rfc/rfc9684.txt
curl -LO https://www.rfc-editor.org/rfc/rfc8348.txt
xym --strict rfc9684.txt
xym --strict rfc8348.txt
ls -1 *.yang
# 期待される結果:
#   iana-hardware@2018-03-13.yang
#   ietf-hardware@2018-03-13.yang
#   ietf-tcg-algs@2024-12-05.yang
#   ietf-tpm-remote-attestation@2024-12-05.yang
sha256sum *.yang > SHA256SUMS
rm rfc9684.txt rfc8348.txt          # リポジトリには YANG と SHA256SUMS だけを置く
```

- 抽出済みのファイルが変わっていないことは `sha256sum -c SHA256SUMS` で確かめられる。
- `--strict` を付けると、ファイル名の付いた CODE BEGINS だけを抽出する（例として載っているコード片は除かれる）。
- 照合したいときは、YangModels/yang リポジトリの `standard/ietf/RFC/` にある同名のファイルと sha256 を比べる。
- `iana-hardware` は IANA の registry で更新されている可能性があるが、RFC 8348 の版（2018-03-13）を使う。

## 3. 検証（host）

guest に入れる前に、host の `yanglint` でスキーマとデータを確かめる。
(1)〜(5) は `tests/check_yang.sh` でまとめて実行できる（(5) は `tests/samples/reply-sample.xml` があるときだけ）。
個々のコマンドは次のとおり。

yanglint は、`-F` を 1 つでも指定すると、指定しなかったモジュールの feature をすべて無効にする。
guest と同じ状態にするため、`ietf-keystore` の feature も指定する。

```bash
cd ~/github/tpm-remote-attestation-netopeer2/attester/yang
YL="yanglint -p . -p $HOME/github/libnetconf2/modules \
    -F ietf-tcg-algs:tpm20 \
    -F ietf-keystore:central-keystore-supported,asymmetric-keys"

# (1) スキーマの検証（エラーが出ないこと）
$YL ietf-tcg-algs@2024-12-05.yang ietf-tpm-remote-attestation@2024-12-05.yang

# (2) ツリーを表示し、RFC の tree diagram と見比べる
$YL -f tree ietf-tpm-remote-attestation@2024-12-05.yang

# (3) 初期設定 XML（§5）を config として検証
$YL -t config ietf-tpm-remote-attestation@2024-12-05.yang ../config/rats-init.xml

# (4) RPC の入力（05 §4.1 の challenge.xml）。must の参照先として初期設定を -O で与える
$YL -t rpc -O ../config/rats-init.xml ietf-tpm-remote-attestation@2024-12-05.yang ../../tests/samples/challenge-sample.xml

# (5) RPC の出力（05 §4.3 の形）。leafref certificate-name の参照先として初期設定を -O で与える
$YL -t reply -O ../config/rats-init.xml ietf-tpm-remote-attestation@2024-12-05.yang ../../tests/samples/reply-sample.xml

# (6) 任意: IETF の書式の検査
pyang --ietf -p . -p ~/github/libnetconf2/modules ietf-tpm-remote-attestation@2024-12-05.yang
```

- `-t reply` に渡すファイルは、NETCONF の `<rpc-reply>` ではなく、libyang の YANG reply 形式にする。
  RPC の要素（`<tpm20-challenge-response-attestation>`）の下に、出力のノード（`<tpm20-attestation-response>`）を置く。
  `<rpc-reply>` のままで検証したいときは、`-t nc-reply -R <request>` を使う。
- (4) の `challenge-sample.xml` は、[05](05_challenge_response.md) §4.1 の challenge に固定の nonce を入れたもの。
- (5) の `reply-sample.xml` は、⑤ で実際の応答から作る（[05](05_challenge_response.md) §4.3）。

## 4. 導入（guest）

リポジトリを [README](README.md) §9.1 の方法で guest の `~/tpm-remote-attestation-netopeer2/` に置いてから、`attester/scripts/install_yang.sh` を実行する。
登録済みのモジュールは飛ばすので、何度実行してもよい。

```bash
# guest
cd ~/tpm-remote-attestation-netopeer2
sudo attester/scripts/install_yang.sh                     # 引数で YANG のディレクトリを指定することもできる
sudo systemctl restart netopeer2-server sysrepo-plugind   # sysrepo-plugind は ⑤ 以降
```

スクリプトが行うことは次のとおり。

```bash
cd ~/tpm-remote-attestation-netopeer2/attester/yang
sudo sysrepoctl -s . -i ietf-tcg-algs@2024-12-05.yang -e tpm20
sudo sysrepoctl -s . -i ietf-tpm-remote-attestation@2024-12-05.yang
sudo sysrepoctl -l | grep -E 'hardware|tcg-algs|tpm-remote|keystore'
```

- sysrepoctl の help には「All the options will affect the module of the last operation」とある。
  `-e tpm20` は、直前の `-i ietf-tcg-algs` だけに効く。
- `-s .` を付けると、`ietf-hardware` と `iana-hardware` が同じディレクトリから見つかり、import only として入る。
  見つからないというエラーになったときは、`-i iana-hardware@2018-03-13.yang -i ietf-hardware@2018-03-13.yang` を先に並べて、明示的に入れる。
- `sysrepoctl -l` の期待結果: `ietf-tcg-algs` と `ietf-tpm-remote-attestation` が implemented（`I`）になり、
  `ietf-tcg-algs` の feature に `tpm20` がある。`ietf-hardware` と `iana-hardware` は import only（`i`）になる。
- モジュールのファイルの所有者は root、権限は sysrepoctl の既定になる。sysrepo-plugind（`rats-attester.so`）も root で動くので変更しなくてよい。
- 後から feature を変えるには `sudo sysrepoctl -c ietf-tcg-algs -e tpm20` のようにする（変えたら再起動。[02](02_netopeer2_systemd.md) §4.3）。
- 外すには `sudo sysrepoctl -u ietf-tpm-remote-attestation -u ietf-tcg-algs` を実行する。

> **チェックポイント（2026-10-07）: §4 完了。** `attester/yang/` の YANG を guest の sysrepo に登録した。
> `sysrepoctl -l` で、`ietf-tcg-algs`（`I`、feature `tpm20`）、`ietf-tpm-remote-attestation`（`I`）、
> `ietf-hardware` と `iana-hardware`（`i`）になっていることを確認した。次は §5。

## 5. 初期設定（rats-support-structures）

`attester/config/rats-init.xml`:

```xml
<rats-support-structures xmlns="urn:ietf:params:xml:ns:yang:ietf-tpm-remote-attestation"
                         xmlns:taa="urn:ietf:params:xml:ns:yang:ietf-tcg-algs">
  <tpms>
    <tpm>
      <name>tpm0</name>
      <firmware-version>taa:tpm20</firmware-version>
      <tpm20-pcr-bank>
        <tpm20-hash-algo>taa:TPM_ALG_SHA256</tpm20-hash-algo>
        <pcr-index>0</pcr-index>
        <pcr-index>1</pcr-index>
        <pcr-index>2</pcr-index>
        <pcr-index>3</pcr-index>
        <pcr-index>4</pcr-index>
        <pcr-index>5</pcr-index>
        <pcr-index>6</pcr-index>
        <pcr-index>7</pcr-index>
        <pcr-index>8</pcr-index>
        <pcr-index>9</pcr-index>
        <pcr-index>10</pcr-index>
      </tpm20-pcr-bank>
      <certificates>
        <certificate>
          <name>ak-cert-0</name>
          <type>local-attestation-certificate</type>
        </certificate>
      </certificates>
    </tpm>
  </tpms>
  <attester-supported-algos>
    <tpm20-asymmetric-signing>taa:TPM_ALG_ECDSA</tpm20-asymmetric-signing>
    <tpm20-hash>taa:TPM_ALG_SHA256</tpm20-hash>
  </attester-supported-algos>
</rats-support-structures>
```

投入する。`attester/scripts/install_config.sh` が、この §5 と §7.3 をまとめて行う。

```bash
# guest
cd ~/tpm-remote-attestation-netopeer2
sudo attester/scripts/install_config.sh      # 引数で config のディレクトリを指定することもできる
```

§5 の部分でスクリプトが行うことは次のとおり。

```bash
cd ~/tpm-remote-attestation-netopeer2/attester/config
sudo sysrepocfg --import=rats-init.xml -d running -m ietf-tpm-remote-attestation
sudo sysrepocfg -C running -d startup -m ietf-tpm-remote-attestation   # running を startup にコピー
sudo sysrepocfg -X -d startup -m ietf-tpm-remote-attestation           # 確認
```

各ノードの意味:

| ノード | 値 | 説明 |
|---|---|---|
| `tpm/name` | `tpm0` | list のキー。`/etc/rats-attester.conf` の `tpm-name` と一致させる |
| `firmware-version` | `taa:tpm20` | config true かつ mandatory。YANG の説明には「装置が自動で設定する」とあるが、今回は導入時に入れる。`rats-attester.so` は初期化のときに、TPM の `TPM_PT_FAMILY_INDICATOR`（"2.0"）と矛盾しないか確かめる |
| `tpm20-pcr-bank` | sha256: PCR 0〜10 | Verifier が要求できる PCR の範囲。`rats-attester.so` は、要求がこの範囲に含まれるかを検査する（RFC は pcr-index の説明で「subset であることを MUST で確認する」としている） |
| `certificates/certificate` | `ak-cert-0`、`local-attestation-certificate` | RPC の出力 `certificate-name` の leafref の参照先。X.509 証明書は作らないので、AK を指す名前として使う。`keystore-ref` は付けない（[06](06_future_extensions.md) §3） |
| `attester-supported-algos` | ECDSA と SHA-256 | AK の署名方式と、使える PCR bank のハッシュ |

**要確認**: swtpm の既定の PCR bank。u-boot は `CONFIG_TPM_PCR_ALLOCATE` で、有効なハッシュ（SHA-1/256/384/512）の bank を割り当てる。
guest で `tpm2_getcap pcrs` を実行して、実際に有効な bank を確かめる。sha256 以外も使うなら、
その bank を `tpm20-pcr-bank` と `attester-supported-algos/tpm20-hash` の両方に追加する。

## 6. 制約と落とし穴

| # | 内容 | 影響 | 対処 |
|---|---|---|---|
| 1 | RPC 出力の `certificate-name` は leafref（`/rats-support-structures/tpms/tpm/certificates/certificate/name`） | sysrepo は `rats-attester.so` が返した出力を検証する。参照先が running に無いと RPC 全体が失敗する | §5 の初期設定で `ak-cert-0` を入れる。`rats-attester.so` は初期化のときにその存在を確かめる |
| 2 | `tpm20-hash-algo`（grouping `tpm20-hash-algo`）に `must '. = /tpm:rats-support-structures/tpm:attester-supported-algos/tpm:tpm20-hash'` がある | config の `tpm20-pcr-bank` だけでなく、**RPC 入力**の `tpm20-pcr-selection` も、`attester-supported-algos/tpm20-hash` に載っていないハッシュを指定すると YANG の検証で拒否される | §5 で `tpm20-hash` に `TPM_ALG_SHA256` を入れる |
| 3 | `attester-supported-algos` の leaf-list には `when "../../tpm:tpms/tpm:tpm[tpm:firmware-version='taa:tpm20']"` がある | tpm の設定が無いと、algos の設定が無効になる | 同じ XML で tpm と algos を一緒に入れる。§3 (3) の yanglint で事前に確かめる |
| 4 | `hardware-based` と `status` は mandatory な config false | config としては検証されない（libyang は config の検証で状態データを無視する）。`get` で見えるようにするには誰かが提供する必要がある | `rats-attester.so` が operational データとして提供する（[05](05_challenge_response.md) §3.7） |
| 5 | identity（`TPM_ALG_SHA256`、`tpm20` など）に `if-feature "tpm20"` がある | feature を有効にしないと identity 自体が存在しない。XML の `taa:TPM_ALG_SHA256` が不正な値になる | `-e tpm20`（§4） |
| 6 | `tpm20-hash-algo` には YANG の `default` が無い（default が SHA-256 であることは description に書かれているだけ） | 入力で省略されたときに、YANG としては値が無い | `rats-attester.so` は、省略された場合に `TPM_ALG_SHA256` として扱う |
| 7 | RFC 本文中にある `tpm20-challenge-response-attestation` の XML の例は YANG と食い違っている | 例のとおりに送ると検証で拒否される | [05](05_challenge_response.md) §4.1 の正しい形を使う（`<nonce>` ではなく `<nonce-value>` にし、値は hex ではなく base64 にする。RPC 要素 `<tpm20-challenge-response-attestation>` で包む） |
| 8 | `tpm20-attestation-response` と `unsigned-pcr-values` はキーの無い list | libyang 5 では、`lyd_new_inner()` は container、notification、RPC、action しか作れない | `lyd_new_list()` をキー無しで使う（[05](05_challenge_response.md) §3.5） |
| 9 | YANG の `pcr` 型は 0..31 だが、TPM の PCR は通常 24 個（0..23） | 24〜31 は YANG では正しい値になる | `rats-attester.so` が `TPM_PT_PCR_COUNT` で検査し、`invalid-value` を返す |

## 7. NACM

### 7.1 既定の状態

| 項目 | 既定値 | 意味 |
|---|---|---|
| `enable-nacm` | true | NACM が有効 |
| `read-default` | permit | 読み出しは許可 |
| `write-default` | deny | 書き込みは拒否（recovery user の root を除く） |
| `exec-default` | permit | RPC の実行は許可 |
| recovery user | root（sysrepo の `NACM_RECOVERY_USER`） | NACM の制限を受けない |

`ietf-tpm-remote-attestation` には `nacm:default-deny-all` が付いていないので、既定のままでは**どのユーザーでも** RPC を実行でき、
`rats-support-structures` を読める。RFC 9684 §4 は「RPC は NACM で保護し、既定を deny-all にする」ことを推奨している。

### 7.2 ルール（`attester/config/nacm-rats.xml`）

- グループ `verifiers` に `ubuntu` を入れる。
- このグループに、`tpm20-challenge-response-attestation` の exec と `rats-support-structures` の read を許可する。
- それ以外のユーザーには、同じモジュールの RPC と read をすべて拒否する。

```xml
<nacm xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-acm">
  <groups>
    <group>
      <name>verifiers</name>
      <user-name>ubuntu</user-name>
    </group>
  </groups>
  <rule-list>
    <name>rats-verifiers</name>
    <group>verifiers</group>
    <rule>
      <name>permit-tpm20-attestation</name>
      <module-name>ietf-tpm-remote-attestation</module-name>
      <rpc-name>tpm20-challenge-response-attestation</rpc-name>
      <access-operations>exec</access-operations>
      <action>permit</action>
    </rule>
    <rule>
      <name>permit-read-rats-support-structures</name>
      <module-name>ietf-tpm-remote-attestation</module-name>
      <path xmlns:tpm="urn:ietf:params:xml:ns:yang:ietf-tpm-remote-attestation">/tpm:rats-support-structures</path>
      <access-operations>read</access-operations>
      <action>permit</action>
    </rule>
  </rule-list>
  <rule-list>
    <name>rats-deny-others</name>
    <group>*</group>
    <rule>
      <name>deny-rats-rpc</name>
      <module-name>ietf-tpm-remote-attestation</module-name>
      <rpc-name>*</rpc-name>
      <access-operations>exec</access-operations>
      <action>deny</action>
    </rule>
    <rule>
      <name>deny-rats-read</name>
      <module-name>ietf-tpm-remote-attestation</module-name>
      <access-operations>read</access-operations>
      <action>deny</action>
    </rule>
  </rule-list>
</nacm>
```

- rule-list は設定に現れる順に評価され、最初に一致した rule が使われる（RFC 8341）。
  `ubuntu` は `rats-verifiers` で許可され、それ以外のユーザーは `rats-deny-others` で拒否される。
- 書き込みは `write-default deny` のままにする。`rats-support-structures` の設定は、root が guest で `sysrepocfg` を使って変える
  （sysrepocfg は NETCONF を通らないので NACM の対象外）。

### 7.3 投入する

既存のルールを消さないよう、`--import`（置き換え）ではなく `--edit`（merge）を使う。
`attester/scripts/install_config.sh`（§5）を実行すれば、この §7.3 も行われる。行うことは次のとおり。

```bash
# guest
cd ~/tpm-remote-attestation-netopeer2/attester/config
sudo sysrepocfg -X -d running -m ietf-netconf-acm                 # 既存のルールを確認する
sudo sysrepocfg --edit=nacm-rats.xml -d running -m ietf-netconf-acm
sudo sysrepocfg -C running -d startup -m ietf-netconf-acm
sudo sysrepocfg -X -d running -m ietf-netconf-acm                 # 反映されたか確認する
```

既存の rule-list があると、merge したリストはその後ろに追加される（rule-list は ordered-by user）。
既存のリストに広い permit があると、そちらが先に一致してしまうので、上の最初の `-X` で順序を見ておく。

## 8. 確認（host の netopeer2-cli）

```text
> connect --host localhost --login ubuntu -p 8300

# モジュールが入っているか（YANG 1.1 なので status の capabilities には出ない。03 §5.1）
> get --filter-xpath "/ietf-yang-library:yang-library/module-set/module[name='ietf-tpm-remote-attestation']"
> get --filter-xpath "/ietf-yang-library:yang-library/module-set/module[name='ietf-tcg-algs']"
    → <feature>tpm20</feature> があること

# スキーマを取り出し、抽出したファイルと同じか確かめる
> get-schema --model ietf-tpm-remote-attestation --version 2024-12-05 --out /tmp/tpm-ra.yang

# 初期設定
> get-config --source running --filter-xpath /ietf-tpm-remote-attestation:rats-support-structures

# 状態データ（rats-attester.so が読み込まれていれば hardware-based、status、path、manufacturer が加わる）
> get --filter-xpath /ietf-tpm-remote-attestation:rats-support-structures
```

guest の中で確認する場合:

```bash
sudo sysrepocfg -X -d running -m ietf-tpm-remote-attestation       # 設定
sudo sysrepocfg -X -d operational -x /ietf-tpm-remote-attestation:rats-support-structures   # ⑤ 以降
```

注意: sysrepo の operational には、そのモジュールの変更を「所有者」として購読しているプロセス（`sr_module_change_subscribe` に
`SR_SUBSCR_PASSIVE` を付けないもの）がいる間だけ、running の設定が入る（`sysrepo/src/sysrepo_types.h:386-388`）。
`rats-attester.so` が読み込まれる前は operational が空になるのが正しい。netopeer2 の `get` は running を別に読んでいるので、設定は表示される。
読み込まれた後は、plugin がモジュールの変更を購読して所有者になるので（[05](05_challenge_response.md) §3.4 の 8）、
operational に running の設定と状態データ（`hardware-based`、`path`、`manufacturer`、`status`）が一緒に入る。

`get-schema` で取り出したファイルと `attester/yang/ietf-tpm-remote-attestation@2024-12-05.yang` を `diff` で比べ、差が無いことを確かめる。

> **チェックポイント（2026-10-07）: §8 完了。** host の netopeer2-cli から次を確認した。
> - ietf-yang-library に `ietf-tpm-remote-attestation@2024-12-05` があり、`ietf-tcg-algs@2024-12-05` には `<feature>tpm20</feature>` がある
> - `get-schema` で取り出したファイルが `attester/yang/ietf-tpm-remote-attestation@2024-12-05.yang` と完全に一致する
> - `get-config --source running` と `get` の結果が `attester/config/rats-init.xml` と同じ（状態データは attester がまだ無いので無い）
> - guest の operational は空（上の注意のとおり、今の段階では正しい）
>
> 追記（2026-10-07）: `rats-init.xml` の startup へのコピー（§5）と NACM の投入（§7）も完了した。
> どちらも running と startup の内容が一致することを確認した。host から ubuntu で `rats-support-structures` を読めること、
> `/ietf-netconf-acm:nacm` が空で返ること（NACM が ubuntu に適用されている）も確認した。**④ はすべて完了。**
