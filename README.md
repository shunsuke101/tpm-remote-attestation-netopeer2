# tpm-remote-attestation-netopeer2
RISC-V QEMU guest（swtpm が TPM 2.0 として見える）を Attester、WSL2 の host を Verifier として、
NETCONF（netopeer2 + sysrepo）と RFC 9684 の YANG `ietf-tpm-remote-attestation` で challenge-response remote attestation を行う研究環境。
- Verifier（host）は nonce と PCR selection を RPC `tpm20-challenge-response-attestation` で送る。
- Attester（guest）は sysrepo-plugind の plugin `rats-attester.so` で RPC を受け、TPM2_Quote の結果（`quote-data`、`quote-signature`、`unsigned-pcr-values`）を返す。
- Verifier は署名、nonce、PCR digest、Reference Value を照合して合否を判定する。

全体の構成、確定パラメータ、手順 ①〜⑤ の対応は [doc/design/README.md](doc/design/README.md)。

## 構成
| パス | 内容 |
|---|---|
| `doc/design/` | 設計書（README、01〜06）。§9 にこのリポジトリのディレクトリ構成 |
| `doc/notes/` | 研究ノート（日付付き）。`doc/*.pdf` は RFC 6241/6242/7950/8348/9334/9683/9684 と TCG TPM 2.0 Library Part 2/3 |
| `attester/` | guest 側。plugin のソース（`src/`）、YANG（`yang/`）、初期設定と NACM（`config/`）、`provision_ak.sh`、systemd の drop-in、導入スクリプト |
| `verifier/` | host 側。C の Verifier `src/rats-verify.c`、`src/make_challenge.sh`、`templates/`。ビルドすると `build/rats-verify` と作業ディレクトリ `build/verifier/`（`enroll/`、`ref-values/`、`state/`、`out/`）ができる。詳細は [verifier/README.md](verifier/README.md) |
| `tests/` | yanglint による検証と、テスト計画への参照 |
| `CMakeLists.txt`、`CMakeModules/` | attester（guest 向け plugin）と verifier（host 向け `rats-verify`）のビルド。Find モジュールは Netopeer2 から流用（BSD-3） |

## 依存関係
libyang
libnetconf2
sysrepo
netopeer2
build-essential
cmake
libssl-dev
libcurl4-openssl-dev
libpcre2-dev
libpam0g-dev
libsystemd-dev
libtss-dev
tpm2-tools
libexpat1-dev
libjson-c-dev

## 使い方（概要）
client(verifier):

```bash
git clone tpm-remote-attestation-netopeer2
cd tpm-remote-attestation-netopeer2
tests/check_yang.sh                     # YANG と初期設定の検証（yanglint）これは消したい
cmake -S . -B build
cmake --build build
cd build
sudo make install
cd verifier
./make_challenge out
netopeer2-cli
>connect --host localhost --login ubuntu -p 8300
>status
>user-rpc --content ./out/challenge.xml --out ./out/response.xml

```
server(attester):

```bash
git clone tpm-remote-attestation-netopeer2 
cd tpm-remote-attestation-netopeer2
sudo attester/scripts/install_yang.sh        # 04 §4（済み。再実行しても登録済みは飛ばす）これは消したい
sudo attester/scripts/install_config.sh      # 04 §5、§7.3（済み）これは消したい
cmake -S . -B build
cmake --build build
cd build
sudo make install
cd attester
./provision_ak.sh                            # 05 §2.2。sudo なし。EK と AK の公開情報を build/keys/ に書き出す
sudo systemctl daemon-reload && sudo systemctl enable --now sysrepo-plugind
```

## 状態（2026-10-14）
- 手順 ⑤: attester（`attester/src/`）は guest の sysrepo-plugind で動いており、host の netopeer2-cli から quote が返る（2026-10-14、テスト N-1）。
  Verifier は C の `rats-verify`（`verifier/src/rats-verify.c`）で、同じ応答の検査 1〜5、7、8 が通った。golden（`build/verifier/ref-values/pcrs-sha256.json`）は未作成なので
  結果はまだ INCOMPLETE。設計は [doc/design/05_challenge_response.md](doc/design/05_challenge_response.md)。
