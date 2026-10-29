# NETCONF / RFC 9684 による Challenge-Response Remote Attestation 設計

- 作成日: 2026-10-05
- 更新日: 2026-10-14
- 対象: `~/github/tpm-remote-attestation-netopeer2`（host: WSL2 Ubuntu 24.04 x86_64、guest: QEMU riscv64 Ubuntu 24.04.4）
- 状態: ①〜④ 完了。⑤ は attester（`attester/src/rats-attester.so`）と Verifier（`verifier/src/rats-verify.c`）を実装済み。host の netopeer2-cli から quote が返り、`rats-verify` で検査 1〜5、7、8 が通った（2026-10-14、テスト N-1）。golden は未作成なので結果は INCOMPLETE

## 1. 目的

RISC-V QEMU guest では swtpm が TPM として見えている。この guest で netopeer2-server を動かし、
RFC 9684 の YANG モジュール `ietf-tpm-remote-attestation` に定義された RPC
`tpm20-challenge-response-attestation` を NETCONF で呼び出せるようにする。

- Verifier（host）は nonce と PCR selection を送る。
- Attester（guest）は TPM2_Quote の結果（`quote-data`、`quote-signature`、`unsigned-pcr-values`）を返す。
- Verifier は署名、nonce、PCR digest、Reference Value を照合して合否を判定する。

## 2. 全体構成

```
+---------------------------- host: WSL2 Ubuntu 24.04 (x86_64) ----------------------------+
|                                                                                          |
|  Verifier                                                                                |
|   netopeer2-cli ── user-rpc ──┐     rats-verify (C) ── enroll/ak-cert-0.pem              |
|   (--content challenge.xml)    │                     └─ ref-values/pcrs-sha256.json      |
|                               │ NETCONF over SSH (RFC 6241 / RFC 6242)                   |
|                       localhost:8300                                                     |
|                               │                                                          |
|  swtpm (bin/tpm_emulator.sh)  │        QEMU (bin/run_qemu_measuredboot.sh)               |
|   /tmp/emulated_tpm/swtpm-sock◄─────── -chardev socket / -tpmdev emulator                |
|                               │        -netdev user,hostfwd=tcp::8300-:830               |
+-------------------------------│----------------------------------------------------------+
                                ▼  slirp
+---------------------------- guest: QEMU riscv64 Ubuntu 24.04 ----------------------------+
|  Attester                                                                                |
|   netopeer2-server :830 ──► sysrepo (/etc/sysrepo, /dev/shm/sr_*) ──► rats-attester.so   |
|     (libnetconf2/libssh)        ▲ YANG 検証 / NACM                     │ sysrepo-plugind |
|                                 │                                      ▼ (tss2-esys)     |
|                  ietf-tpm-remote-attestation                    /dev/tpmrm0              |
|                  ietf-tcg-algs (feature tpm20)                  (tpm-tis-device → swtpm) |
+------------------------------------------------------------------------------------------+
```

ポイント:

- netopeer2-server は RPC の中身を実装しない。RPC は sysrepo を経由して、購読しているプロセスに渡される。
  今回は sysrepo-plugind が読み込む plugin **`rats-attester.so`** が購読し、ESAPI で `TPM2_Quote` を実行して応答を組み立てる（[05](05_challenge_response.md) §3）。
  sysrepo の標準の仕組みなので、`main()`、シグナル処理、systemd の unit を自前で持たずに済む。
- swtpm は host で動き、QEMU の `tpm-tis-device` を通して guest の `/dev/tpmrm0` として見える。
  TPM の操作はすべて guest の中で行う。

## 3. RATS の役割との対応（RFC 9334）

| RATS の役割 | 本設計での実体 | 置き場所 |
|---|---|---|
| Attester | netopeer2-server + sysrepo + sysrepo-plugind（`rats-attester.so`）+ TPM（AK `0x81010002`） | guest |
| Verifier | netopeer2-cli + `rats-verify`（C。`verifier/src/rats-verify.c`） | host |
| Relying Party | Verifier の判定結果を使う利用者（今回は人手。将来はスクリプト） | host |
| Endorser / Reference Value Provider | 既知の正常状態で取得した golden PCR と、登録した AK 公開鍵 | host の `build/verifier/ref-values/`（golden）と `build/verifier/enroll/`（AK） |

構成は RFC 9683（RIV）の付録 A.3 にある「RFC 9684 → NETCONF → SSH」という積み方と同じになる。

## 4. 手順と設計書の対応

| 手順 | 内容 | 設計書 |
|---|---|---|
| ① | libyang、libnetconf2、sysrepo、netopeer2 の導入（guest は全部、host は CLI 用） | [01_build_netconf_stack.md](01_build_netconf_stack.md) |
| ② | netopeer2-server を systemd で常駐させる | [02_netopeer2_systemd.md](02_netopeer2_systemd.md) |
| ③ | host の netopeer2-cli から `connect` / `status` / `get` / `get-config` で疎通確認 | [03_connectivity_check.md](03_connectivity_check.md) |
| ④ | RFC 9684 の YANG を抽出・検証・導入し、初期設定と NACM を入れる | [04_rfc9684_yang.md](04_rfc9684_yang.md) |
| ⑤ | nonce と pcr-selection を送って Quote を受け取り、検証する | [05_challenge_response.md](05_challenge_response.md) |
| 将来 | log-retrieval、credential activation、keystore-ref、mtpm など | [06_future_extensions.md](06_future_extensions.md) |

## 5. 確定パラメータ

以下の値は全ファイルで共通に使う。変えるときは全ファイルを直すこと。

| 項目 | 値 |
|---|---|
| リポジトリ | host: `~/github/tpm-remote-attestation-netopeer2/`、guest: `~/tpm-remote-attestation-netopeer2/`（§9） |
| Attester の実装 | sysrepo-plugind の plugin `rats-attester.so`（`/usr/local/lib/sysrepo-plugind/plugins/`） |
| Attester の設定 | `/etc/rats-attester.conf` |
| Verifier の実装 | C の `rats-verify`（`verifier/src/rats-verify.c`。host でビルドし、作業ディレクトリ `build/verifier/` から `../rats-verify` を実行する。[05](05_challenge_response.md) §5.2） |
| Verifier のファイル | 作業ディレクトリ `build/verifier/`（cmake の構成時にできる。[05](05_challenge_response.md) §5.1）の `enroll/`（AK の公開情報）、`ref-values/pcrs-sha256.json`（golden）、`state/<certificate-name>.json`（前回の clockInfo）、`out/`（nonce、challenge、reply）。`build/` を消すと一緒に消える |
| EK と AK の公開情報 | guest の `~/tpm-remote-attestation-netopeer2/build/keys/`（cmake の構成時にコピーされる `build/provision_ak.sh` を sudo なしで実行して書き出す。[05](05_challenge_response.md) §2.2） |
| guest の NETCONF ポート | 830（netopeer2 の既定。`merge_config.sh` はポートを指定せず、ietf-netconf-server の既定値 830 が使われる） |
| host から見たポート | `localhost:8300`（QEMU の `hostfwd=tcp::8300-:830`） |
| SSH（既存） | `localhost:2222` → guest:22（`ssh -p 2222 ubuntu@localhost`） |
| NETCONF のログインユーザー | `ubuntu` |
| YANG モジュール | `ietf-tpm-remote-attestation@2024-12-05`、`ietf-tcg-algs@2024-12-05`（feature `tpm20`） |
| 依存モジュール | `ietf-hardware@2018-03-13`、`iana-hardware@2018-03-13`（RFC 8348）、`ietf-keystore@2024-10-10`（RFC 9642、libnetconf2 が導入する）、`ietf-yang-types`（libyang に内蔵） |
| 有効にしない feature | `mtpm`、`bios`、`ima`、`netequip_boot`、`tpm12`、`ietf-hardware:entity-mib` |
| TPM 名 | `tpm0` |
| EK | RSA2048、永続ハンドル `0x81010001` |
| AK | EK 配下の ECC NIST P-256 / ECDSA / SHA-256 の restricted signing key。永続ハンドル `0x81010002` |
| certificate-name | `ak-cert-0`（type `local-attestation-certificate`）。X.509 証明書は作らず、AK を指す識別子として使う |
| PCR bank | sha256。`tpm20-pcr-bank` には PCR 0〜10 を設定する |
| 例の challenge | sha256 の PCR 0〜10 |
| nonce | Verifier が CSPRNG で生成する 32 バイト |
| `quote-data` | marshal した TPM2B_ATTEST（先頭 2 バイトがサイズ、続いて TPMS_ATTEST） |
| `quote-signature` | marshal した TPMT_SIGNATURE |
| YANG の binary 型 | base64 で表す（RFC 7950 §9.8.2） |

`quote-data` を TPM2B_ATTEST にした根拠: RFC 9684 は出力を
"an attestation structure (TPMS_ATTEST), including a length, and a signature (TPMT_SIGNATURE)" と説明している。
`tpm2_checkquote` に渡すときは先頭の 2 バイトを取り除く（[05](05_challenge_response.md) §5.3）。

## 6. ソースのバージョン

2026-10-05 の時点で host の `~/github` に clone してあるものを基準にする。いずれも `master` で、最新リリースのタグと一致している。

| リポジトリ | パス | タグ | commit | 日付 |
|---|---|---|---|---|
| CESNET/libyang | `~/github/libyang` | v5.8.6 | `47351e59e` | 2026-06-19 |
| CESNET/libnetconf2 | `~/github/libnetconf2` | v4.4.10 | `2add91c` | 2026-06-04 |
| sysrepo/sysrepo | `~/github/sysrepo` | v5.1.0 | `e92dd4fc` | 2026-06-04 |
| CESNET/netopeer2 | `~/github/Netopeer2` | v2.8.7 | `5be28a9` | 2026-06-22 |

guest にも同じ commit を入れる（[01](01_build_netconf_stack.md) §3）。

## 7. これまでの方式との関係

### 7.1 経緯

| 時期 | 方式 | Attester | 状態 |
|---|---|---|---|
| 〜2026-09 | SSH ユーザー証明書の拡張に quote、sig、nonce を入れる（`bin/create_quote_certificate.sh`、`openssh-portable/auth-options.c` の `verify()`） | ssh クライアント側 | 実装済み |
| 2026-09-17 | 役割を反転し、sshd を Attester にする。EXT_INFO で Evidence を送る案 | sshd | 設計のみ |
| 2026-09-18 | connection 層の独自 subsystem を採用。NETCONF はコストが高いとして見送った（`~/.claude/plans/ssh-attestation-ssh-transport-protocol-elegant-truffle.md`） | sshd | 設計のみ |
| **2026-10-05（本設計）** | **NETCONF + RFC 9684 の YANG で行う** | **netopeer2-server + sysrepo-plugind（guest）** | **①〜④ 完了、⑤ は attester と Verifier を実装済み（N-1 が通った。golden は未作成）** |

### 7.2 NETCONF にする理由

- RFC 9684 と RFC 9683 に沿った標準どおりの形で、CHARRA に準拠した Verifier と相互運用できる。
- nonce は Verifier が生成して送る。RFC 9684 の nonce grouping は「Attester の外のエントロピーから作る」ことを MUST としている。
  これで旧方式の課題だった freshness（nonce を自分で作るのでリプレイできる）が解ける。
- YANG によるメッセージの型付けと検証、NACM によるアクセス制御、何度でもできる再 attestation を、自前で作らずに済む。

### 7.3 受け入れる制約

- netopeer2-server は libssh による独自の SSH サーバで、830 番で待ち受ける。そのため、ログインに使う sshd（22 番）とは別の接続・別のホスト鍵になる。
  attestation で示せるのは「AK を持つ TPM のプラットフォーム状態」までで、ログインセッションとは結び付かない。
- 中継攻撃（Asokan 攻撃。RFC 9683 §5.2）への耐性は今回の範囲外とする。対策の案は [06](06_future_extensions.md) §5 に書いた。
- event log による PCR 値の再計算（replay）は今回は行わない。PCR ごとの golden 値と照合するだけにする（[06](06_future_extensions.md) §1）。

### 7.4 旧方式から引き継ぐもの・直すもの

- 引き継ぐ: AK の鍵種（ECC P-256 / ECDSA / SHA-256、restricted|sign|fixedtpm）、sha256 bank、u-boot の measured boot の PCR 構成。
- 直す: 既存の `verify()` には次の欠陥がある。Verifier の設計（[05](05_challenge_response.md) §5.4）では同じ欠陥を作らない。
  - 署名の検証に失敗しても処理が続く。
  - PCR 値を quote の `pcrDigest` と照合していない。
  - 最初の接続で golden を自動的に書き込んでしまう（TOFU）。

## 8. 前提と既存の資産

| 項目 | 内容 |
|---|---|
| swtpm の起動 | `~/github/bin/tpm_emulator.sh`（状態とソケットは `/tmp/emulated_tpm`） |
| QEMU の起動 | `~/github/bin/run_qemu_measuredboot.sh`（メモにある `run_qemu_measureboot.sh` は、実際にはこのファイル名） |
| guest へのログイン | `ssh -p 2222 ubuntu@localhost` |
| QEMU | `~/qemu/build/qemu-system-riscv64`（11.0.50。`-machine virt -m 4G -smp 2`） |
| u-boot | v2025.10。`CONFIG_EFI_TCG2_PROTOCOL=y`。PCR 0、1、2、4、5、7 を extend し、separator は PCR 0〜7 に入る。PCR 8〜9 は GRUB によるものとみられる |
| host の TPM ライブラリ | libtss2 4.0.1（esys、mu、rc、tctildr）、swtpm 0.7.3、tpm2-tools 5.6（apt。PCR を 8 個以上選ぶと tpm2_checkquote が失敗する。[05](05_challenge_response.md) §5.3）。ソースは `~/github/tpm2-tools`（5.8-rc1） |
| host の Verifier のライブラリ | expat 2.6.1、json-c 0.17、OpenSSL 3.0.13（libcrypto）、tss2-mu 4.0.1。golden を作るときに jq 1.7 を使う |
| guest の TPM ツール | tpm2-tools（`bin/create_quote_certificate.sh` で使っていた） |
| 依存パッケージのスクリプト | `~/github/bin/libnetconf2_need_package.sh`（libyang と libnetconf2 のビルドに必要なもの） |
| RFC | `doc/` に RFC 6241、6242、7950、9334、9683、9684 の PDF がある |

## 9. リポジトリの構成

設計書・YANG・初期設定・研究ノート・実装は、git で管理するリポジトリ `~/github/tpm-remote-attestation-netopeer2/` にまとめてある。

```
tpm-remote-attestation-netopeer2/
├── README.md
├── CMakeLists.txt                # ルート。option(BUILD_ATTESTER)、option(BUILD_VERIFIER)、add_subdirectory(attester / verifier)、uninstall
├── CMakeModules/                 # FindLibYANG / FindSysrepo / uninstall（Netopeer2 から流用）、FindTSS2（pkg-config のラッパー）
├── doc/
│   ├── *.pdf                     # RFC 6241/6242/7950/8348/9334/9683/9684、TCG TPM 2.0 Library Part 2/3
│   ├── design/                   # この設計書（README、01〜06）
│   └── notes/                    # 研究ノート（日付付き）
├── attester/                     # guest 側（05）。sysrepo-plugind の plugin
│   ├── CMakeLists.txt            # add_library(rats-attester MODULE ...) → SRPD plugins path に install
│   ├── src/
│   │   ├── plugin.c              # sr_plugin_init_cb / sr_plugin_cleanup_cb
│   │   ├── attester.h            # 共有する構造体と各ファイルの登録関数
│   │   ├── conf.c / conf.h       # /etc/rats-attester.conf
│   │   ├── tpm.c / tpm.h         # ESAPI のラッパー
│   │   ├── rats-support-structures.c              # 状態データ（05 §3.7）
│   │   ├── tpm20-challenge-response-attestation.c # RPC 本体（05 §3.5）
│   │   ├── tpm12-challenge-response-attestation.c # スタブ（operation-not-supported）
│   │   └── log-retrieval.c                        # スタブ（06 §1）
│   ├── yang/                     # RFC から抽出した YANG（04）+ SHA256SUMS
│   ├── config/                   # rats-init.xml、nacm-rats.xml（04）
│   ├── systemd/sysrepo-plugind.service.d/10-rats-attester.conf   # drop-in（02 §3）
│   ├── scripts/                  # install_yang.sh、install_config.sh（04 §4/§5/§7.3）
│   ├── rats-attester.conf        # 05 §3.3
│   └── provision_ak.sh           # 05 §2.2。cmake の構成時に build/ にコピーされ、build/keys/ に公開情報を書き出す
├── verifier/                     # host 側（05）
│   ├── README.md                 # 使い方
│   ├── CMakeLists.txt            # add_executable(rats-verify) → build/rats-verify。作業ディレクトリ build/verifier/ を作る
│   ├── src/                      # make_challenge.sh、rats-verify.c（C の Verifier。検査 1〜8 と --dump）
│   └── templates/challenge.xml.tmpl
└── tests/
    ├── README.md                 # テスト計画（05 §6）への参照と記録の置き方
    ├── samples/                  # yanglint 用のサンプル XML（04 §3）
    └── check_yang.sh             # 04 §3 (1)〜(5) を実行する（(5) は reply-sample.xml があるとき）
```

`build/`（git の管理外）。`mkdir -p build && cd build && cmake .. && make` でできる。

```
build/
├── provision_ak.sh               # cmake .. の時点でソースからコピーされる（guest で実行する。05 §2.2）
├── keys/                         # provision_ak.sh の出力（guest。05 §2.2）
├── rats-attester.so              # make でできる。sudo make install で plugin のディレクトリに入る（05 §3.8）
├── rats-verify                   # make でできる（host。05 §5.2）
└── verifier/                     # Verifier の作業ディレクトリ（host。05 §5.1）。cmake .. の時点でできる
    ├── make_challenge.sh         # ソースからコピー（05 §4.2）
    ├── templates/challenge.xml.tmpl
    ├── enroll/                   # AK の公開情報 ak-cert-0.{pem,name,qname}（05 §2.3）
    ├── ref-values/               # Reference Value（golden）。pcrs-sha256.json（05 §2.5）
    ├── state/                    # clockInfo の履歴
    └── out/                      # nonce.bin、challenge.xml、reply.xml、--dump の出力
```

RATS の用語では、`ref-values/` が Reference Value（Reference Value Provider から得るもの）、`enroll/` が Endorsement（AK の公開鍵）にあたるので、分けて置く。
`enroll/` と `ref-values/` は既知の正常状態でしか作り直せないので、`build/` を作り直す前に控えを取る（05 §5.1）。

### 9.1 guest への配置

host のリポジトリを tar にまとめて送る（[01](01_build_netconf_stack.md) §3 と同じ方法）。host の `build/` と `verifier/build/` は x86_64 用で、CMake のキャッシュに host のパスが入っているので送らない。
`.git` は送るので、guest でも commit を確認できる。

```bash
# host
tar czf /tmp/tra-netopeer2.tgz -C ~/github --exclude='tpm-remote-attestation-netopeer2/build' \
    --exclude='tpm-remote-attestation-netopeer2/verifier/build' tpm-remote-attestation-netopeer2
scp -P 2222 /tmp/tra-netopeer2.tgz ubuntu@localhost:~

# guest
tar xzf ~/tra-netopeer2.tgz -C ~
git -C ~/tpm-remote-attestation-netopeer2 log -1 --oneline
```

guest でのビルドとインストールは [05](05_challenge_response.md) §3.8、YANG と設定の導入は [04](04_rfc9684_yang.md) §4、§5、§7.3。

## 10. 用語

| 用語 | 意味 |
|---|---|
| AK | Attestation Key。restricted signing key で、TPM2_Quote に署名する |
| EK | Endorsement Key。TPM ごとに固有の鍵 |
| LAK / IAK | Local / Initial Attestation Key（RFC 9683）。本設計の AK は guest 内で作るので LAK にあたる |
| TPMS_ATTEST | TPM2_Quote が署名する構造体。magic、type、qualifiedSigner、extraData（nonce）、clockInfo、firmwareVersion、TPMS_QUOTE_INFO（pcrSelect、pcrDigest）を含む |
| TPM2B_ATTEST | TPMS_ATTEST の前に 2 バイトのサイズを付けたもの |
| Reference Value / golden | 既知の正常状態で得た PCR の期待値 |
| NACM | NETCONF Access Control Model（RFC 8341） |
| sysrepo | YANG のデータストア。netopeer2 は RPC や設定を sysrepo に委ねる |
| sysrepo-plugind | sysrepo の plugin（`.so`）を読み込んで常駐するデーモン。plugin は `sr_plugin_init_cb()` と `sr_plugin_cleanup_cb()` を持ち、init で RPC などを購読する |

## 11. 参照

- RFC 6241 NETCONF、RFC 6242 NETCONF over SSH、RFC 7950 YANG 1.1（`doc/` に PDF あり）
- RFC 9334 RATS Architecture、RFC 9683 RIV、RFC 9684 CHARRA YANG（`doc/` に PDF あり）
- RFC 8341 NACM、RFC 8348 ietf-hardware、RFC 9640 ietf-crypto-types、RFC 9642 ietf-keystore
- TCG TPM 2.0 Library Part 2: Structures（TPMS_ATTEST §10.12.12、TPML_PCR_SELECTION §10.9.7）
- 上流のリポジトリ: <https://github.com/CESNET/libyang>、<https://github.com/CESNET/libnetconf2>、
  <https://github.com/sysrepo/sysrepo>、<https://github.com/CESNET/netopeer2>
- RFC のテキスト版（YANG の抽出に使う）: <https://www.rfc-editor.org/rfc/rfc9684.txt>、<https://www.rfc-editor.org/rfc/rfc8348.txt>
