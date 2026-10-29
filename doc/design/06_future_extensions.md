# 将来の拡張

[README](README.md) の範囲外とした項目について、概要と課題をまとめる。今回は設計の方向だけを示し、詳細は着手するときに詰める。

## 1. log-retrieval（event log の取得）

### 1.1 目的

本設計の Verifier は、PCR ごとの golden 値と照合するだけで、PCR 値が「何を測定した結果か」は説明できない。
RFC 9683 は、Verifier が event log を取得し、replay して PCR 値を再計算することを求めている。
RFC 9684 では、これを RPC `log-retrieval` で行う。

### 1.2 YANG の要点

| 項目 | 内容 |
|---|---|
| 必要な feature | `bios`（BIOS/UEFI の event log）、`ima`（Linux IMA）。`netequip_boot` は使わない |
| 入力 | `log-type`（identityref。`bios`、`ima` など）、`log-selector`（`name`、`last-entry-value` / `last-index-number` / `timestamp` の choice、`log-entry-quantity`） |
| 出力 | `system-event-logs/node-data/log-result` の choice に、`bios-event-logs/bios-event-entry` または `ima-event-logs/ima-event-entry` |
| NACM | RFC 9684 §4 は `log-retrieval` を sensitive としている。digest からソフトウェアの版が分かってしまうため |

feature を有効にする:

```bash
sudo sysrepoctl -c ietf-tpm-remote-attestation -e bios -e ima
sudo systemctl restart netopeer2-server sysrepo-plugind
```

- RPC `log-retrieval` 自体には `if-feature` が無いので、今の構成でもスキーマにある。`rats-attester.so` は購読しており、`attester/src/log-retrieval.c` が `operation-not-supported` を返している（[05](05_challenge_response.md) §3.6）。実装はこのファイルに書く。
- 今の NACM（`attester/config/nacm-rats.xml`）では、`verifiers` にも `log-retrieval` の exec を許可していない（`rats-deny-others` で拒否される）。使うときは、`rats-verifiers` に `log-retrieval` を許可する rule を足す。

### 1.3 データの対応

**bios**（`/sys/kernel/security/tpm0/binary_bios_measurements`、TCG PC Client の crypto-agile 形式、TCG_PCR_EVENT2）:

| event log | `bios-event-entry` |
|---|---|
| 先頭からの番号 | `event-number`（key） |
| eventType | `event-type` |
| pcrIndex | `pcr-index` |
| digests[]（algId、digest） | `digest-list`（`hash-algo`、`digest`） |
| eventSize | `event-size` |
| event | `event-data` |

**ima**（`/sys/kernel/security/ima/ascii_runtime_measurements`、テンプレート `ima-ng`）:

| 列 | `ima-event-entry` |
|---|---|
| 行の番号 | `event-number`（key） |
| PCR（通常 10） | `pcr-index` |
| template-hash | `template-hash`（`template-hash-algorithm` は sha1 か sha256） |
| template-name | `ima-template` |
| filedata-hash（`sha256:...`） | `filedata-hash-algorithm`、`filedata-hash` |
| filename-hint | `filename-hint` |

### 1.4 課題

- u-boot の EFI TCG2 が作った event log が、Linux の `binary_bios_measurements` に引き継がれているかを guest で確認する。
  引き継がれていなければ、u-boot から Linux への event log の受け渡しを調べる必要がある。
- PCR 8〜9 は GRUB によるものとみられる。GRUB の event が event log に含まれるかを確認する。
- IMA は、既存の `run_qemu_ima.sh` と IMA 用のイメージを使う。PCR 10 を `tpm20-pcr-bank` に追加する（2026-10-22 に追加済み。challenge の例も PCR 0〜10 を要求する）。
- event log は大きくなる。`log-selector` の `last-index-number` と `log-entry-quantity` で分割して取得する。
  Verifier は前回の位置を覚えておき、差分だけを取る。
- Verifier に replay を実装する。bank ごとに初期値（通常は 0。PCR 0 は locality の影響を受ける場合がある）から extend を順にやり直し、
  quote の PCR 値（検査 5 を通ったもの）と一致することを確かめる。そのうえで、event ごとに Reference Value と照合する。
- log の取得と quote の取得の間に PCR が変わる競合への対処（log を取ってから quote を取り、quote が log の範囲に収まるかを見る）。

## 2. credential activation（AK と EK が同じ TPM にあることの証明）

本設計の enrollment（[05](05_challenge_response.md) §2.3）は、信頼できる経路で公開鍵を受け取ることに頼っている。
これを暗号的な手順に置き換える。

1. Verifier は EK の公開鍵と AK の name を受け取る。
2. Verifier は `tpm2_makecredential` で、ランダムな秘密を EK と AK の name に束縛して暗号化する。
3. Attester は `tpm2_activatecredential` で復号する。同じ TPM の中に EK と AK が両方あるときだけ復号できる。
4. Attester は復号した秘密を返す。Verifier は送った秘密と一致すれば、AK を登録する。

課題:

- EK が正当な TPM のものであることを示すには、EK 証明書が必要。swtpm は `swtpm_setup --create-ek-cert` に自前の CA を指定すると、EK 証明書を作れる。
  ただし、`bin/tpm_emulator.sh` は今のところ swtpm_setup を使っていない。
- RFC 9684 の YANG には、credential activation の RPC が無い。独自の RPC を定義する（別モジュールで augment するなど）か、SSH 経由のスクリプトで行う。
- RFC 9683 §2.2 が想定する IAK / IDevID の証明書（製造者の発行するもの）は、swtpm では作れない。
  研究用の CA で代用し、`certificate/type` を `initial-attestation-certificate` にする。

## 3. keystore-ref と ietf-keystore

`certificates/certificate/keystore-ref` は、`/ks:keystore/asymmetric-keys/asymmetric-key/name` への leafref。
AK を keystore に登録すれば、Verifier は NETCONF の `get-config` で AK の公開鍵を取得できる。

- 登録の形: `asymmetric-key` に `public-key-format` と `public-key`（SubjectPublicKeyInfo）を入れる。秘密鍵は TPM の外に出せないので、
  ietf-crypto-types の `hidden-private-key`（「ハードウェアの中に隠された秘密鍵」を表す）を使う。
- 課題 1: libnetconf2 が有効にしている ietf-crypto-types の feature は `cleartext-passwords` と `cleartext-private-keys` だけ。
  `hidden-private-key` を使うには、feature `hidden-private-keys` を有効にする必要がある。
- 課題 2: libnetconf2 は `ietf-keystore` の設定を監視して、SSH のホスト鍵として読み込む。秘密鍵が hidden の鍵を見つけたときに
  エラーになるか、無視されるかは未確認。netopeer2 の動作を壊さないか、事前に試す必要がある。
- 課題 3: get-config で取得した公開鍵は、登録の経路としては NETCONF 接続に頼ることになる。
  credential activation（§2）と組み合わせないと、TOFU と同じになる。

## 4. mtpm / compute-node（複数の TPM）

- feature `mtpm` を有効にすると、`compute-nodes/compute-node` が現れ、RPC の入力で `certificate-name` を指定できるようになる。
  `certificate-name` には、ファームウェアが `tpm20` の TPM の証明書であることを確かめる `must` が付く。
- 出力の `tpm20-attestation-response` は、TPM ごとに 1 つずつ並ぶ（`unique "certificate-name"`）。
  入力で `certificate-name` を指定しなければ、すべての TPM 2.0 が応答する。
- 本環境の swtpm は 1 つなので、試すには QEMU に 2 つ目の TPM（swtpm をもう 1 つ）を付ける必要がある。
  `tpm-tis-device` を 2 つ付けられるかは要確認。
- `/etc/rats-attester.conf`（`attester/src/conf.c`）は、certificate-name ごとのセクションに広げる（[05](05_challenge_response.md) §3.3）。

## 5. NETCONF のホスト鍵と AK の結び付け（中継攻撃への対策）

本設計では、「NETCONF で接続した相手」と「quote に署名した TPM」が同じ装置であることを示せない
（[README](README.md) §7.3、RFC 9683 §5.2）。

| 案 | 概要 | 課題 |
|---|---|---|
| A. ホスト鍵の公開鍵を PCR に extend する | 起動時（netopeer2-server の起動より前）に、systemd の oneshot unit が `genkey` の公開鍵の SHA-256 を PCR（例: 15）に extend する。Verifier は SSH のハンドシェイクで見たホスト鍵のハッシュから期待値を作り、quote の PCR 15 と照合する | extend の前に root を奪われると、偽の値を extend されうる。起動の早い段階で測定する必要がある。ホスト鍵を作り直すと期待値が変わる |
| B. ホスト鍵を TPM の中に置く | libssh の PKCS#11 対応と tpm2-pkcs11 を使い、ホスト鍵の秘密鍵を TPM の外に出さない。AK でホスト鍵を `TPM2_Certify` する | libnetconf2 は keystore の平文の秘密鍵を前提にしている。改修が必要になる可能性が高い |
| C. 旧方式（subsystem）と組み合わせる | ログイン用の sshd の接続の上で attestation を行い、NETCONF は管理用に使い分ける | 9/18 の計画（[README](README.md) §7.1）に戻ることになる |

研究としては、まず案 A を試すのが手軽。

## 6. その他

- **定期的な attestation**: 現在の RPC は challenge-response（Verifier が呼び出す）だけ。定期的に行うには Verifier 側で cron などを使う。
  RFC 9684 の範囲外だが、YANG-push（RFC 8641）による通知型の方式も検討できる。
- **Verifier のサービス化・自動化**: netopeer2-cli にはバッチモードが無いので、nonce の生成から判定までを 1 コマンドで行うには、
  libnetconf2 を使った C のクライアント（`rats-verify` の検査を呼ぶ）を作るか、netopeer2-cli を expect などで駆動する。
  複数の Attester は登録表（certificate-name → AK、golden）で管理する（05 §5.5）。
- **性能の測定**: swtpm と TCG（RISC-V エミュレーション）の環境で、Quote、PCR_Read、NETCONF の往復、検証にかかる時間を測る。
  旧方式（SSH 証明書）や subsystem 案と比べる材料にする。
- **TPM 1.2 対応**（`tpm12-challenge-response-attestation`）: 対象外。swtpm は TPM 1.2 も扱えるが、研究上の必要が無い。
  この RPC には `if-feature "taa:tpm12"` があり、feature `tpm12` を有効にしていないのでスキーマに無い。`attester/src/tpm12-challenge-response-attestation.c` は、RPC がスキーマにあるときだけ購読して `operation-not-supported` を返す。
