# verifier（host 側）

Verifier の役割（RFC 9334）を host で担う。設計は [doc/design/05_challenge_response.md](../doc/design/05_challenge_response.md) §2.3、§2.5、§4、§5。

| パス | 内容 |
|---|---|
| `src/make_challenge.sh` | nonce（32 バイト、CSPRNG）を作り、`templates/challenge.xml.tmpl` から `challenge.xml` を作る。cmake の構成時に作業ディレクトリへコピーされ、そこで実行する |
| `src/rats-verify.c` | C の Verifier。応答（`reply.xml`）の検査 1〜8（§5.2）と `--dump`（§5.3）。結果は JSON。終了コード 0 = PASS、1 = FAIL / INCOMPLETE、2 = 入力の誤り |
| `CMakeLists.txt` | `rats-verify` のビルドと、作業ディレクトリの作成。ルートの CMake から入るほか、このディレクトリだけでも構成できる |
| `templates/challenge.xml.tmpl` | RPC の要素だけを書いた challenge。`@NONCE_B64@` を置き換えて使う。作業ディレクトリへコピーされる |

## 作業ディレクトリ `build/verifier/`（§5.1）

cmake の構成時（`cmake ..`）に作られる。`rats-verify` は 1 つ上の `build/rats-verify`。

| パス | 内容 |
|---|---|
| `make_challenge.sh`、`templates/` | ソースからコピーしたもの。ソースを変えると `make` でコピーし直される |
| `enroll/` | **Endorsement**。AK の公開情報 `ak-cert-0.pem`（SubjectPublicKeyInfo の PEM）、`ak-cert-0.name`、`ak-cert-0.qname`、`ENROLLED.sha256`（§2.3） |
| `ref-values/` | **Reference Value**（golden）。`pcrs-sha256.json`（§2.5） |
| `state/` | 前回の clockInfo（`<certificate-name>.json`。`rats-verify --state` が作る） |
| `out/` | `nonce.bin`、`challenge.xml`、`reply.xml`、`--dump` の出力 |

`build/` は git の管理外なので、どれも commit されない。**`build/` を消すと `enroll/`、`ref-values/`、`state/` も消える。**
`enroll/` と `ref-values/` は既知の正常状態でしか作り直せないので、`build/` を作り直す前に控えを取る（§5.1 に手順がある）。

## ビルド（host）

依存: `libexpat1-dev`（XML）、`libjson-c-dev`（JSON）、`libtss2-dev`（tss2-mu）、`libssl-dev`（libcrypto）。

```bash
cd ~/github/tpm-remote-attestation-netopeer2
mkdir -p build && cd build
cmake .. && make           # build/rats-verify と build/verifier/（attester の plugin も一緒）
```

## 流れ

```bash
cd ~/github/tpm-remote-attestation-netopeer2/build/verifier

./make_challenge.sh                        # out/nonce.bin、out/challenge.xml
netopeer2-cli                              # このディレクトリで起動する（--content と --out はここからの相対パス）
> connect --host localhost --login ubuntu -p 8300
> user-rpc --content out/challenge.xml --out out/reply.xml
> disconnect

../rats-verify --reply out/reply.xml --nonce out/nonce.bin \
    --ak enroll/ak-cert-0.pem --ak-qname enroll/ak-cert-0.qname \
    --golden ref-values/pcrs-sha256.json --state state/ak-cert-0.json --dump out/
```

- 送った `challenge.xml` は検証の入力にしない。nonce は `--nonce`、期待する PCR selection は golden から取る。
- `--golden` を省くと検査 6 は「未評価」になり、結果は PASS ではなく INCOMPLETE（終了コード 1）になる。golden を作る前の動作確認に使える。
- `--state` は、結果が FAIL でなければ更新される（初回は作られる）。
- 二重確認（§5.3）: `--dump out/` が `out/quote.msg`、`out/quote.sig`、`out/pcrs.bin` を書き、結果の JSON の `dump.tpm2_checkquote` に
  そのまま実行できるコマンドが入る。apt の tpm2-tools 5.6 は PCR を 8 個以上選ぶと失敗するので、`-l` と `-f` を外して署名と nonce だけを確かめる。

## 前提と注意

- **enrollment（§2.3）**: AK の公開情報は、既知の正常状態のときに、管理者の資格情報で認証された SSH 経由で guest の
  `~/tpm-remote-attestation-netopeer2/build/keys/`（`provision_ak.sh` の出力）から取り込む。
  NETCONF の応答から AK を取り込むこと（TOFU）はしない。
- **Reference Value（§2.5）**: `ref-values/pcrs-sha256.json` は、既知の正常状態で 1 回 attestation を行い、検査 1〜5 がすべて通った応答の
  `unsigned-pcr-values` を保存したもの。**「保存した時点の状態を信頼する」という前提**に立っている。event log の replay による期待値の算出は
  [06](../doc/design/06_future_extensions.md) §1 で扱う。golden が無いときは検査 6 を「未評価」とし、PASS にはしない。
  `rats-verify` の結果の JSON の `pcrs.sha256` が、そのまま golden の `pcrs` に使える形（PCR 番号 → 64 桁の hex）になっている。
- swtpm の状態（`/tmp/emulated_tpm`）が消えると AK が変わり、署名検証が失敗する（§2.4）。`provision_ak.sh` を実行し直して再登録する。
