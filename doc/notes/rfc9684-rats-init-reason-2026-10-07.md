# rats-init.xml（rats-support-structures の初期設定）が必要な理由

- 日付: 2026-10-07
- 分類: 設計（RFC 9684 の YANG）
- 関連: `doc/design/04_rfc9684_yang.md` §5・§6、`attester/config/rats-init.xml`

## 質問

> 設計書 04 の §5 に進むにあたって、`attester/config/rats-init.xml` を定義する理由がわからない。

## 回答

RFC 9684 のモジュールは、2 つの部分でできている。

- **RPC**（`tpm20-challenge-response-attestation`）: attestation を行う「操作」
- **`rats-support-structures`**: この装置にどんな TPM や AK があり、どのアルゴリズムが使えるかを表す「設定データ」

RPC はこの設定データを 3 か所で参照している。そのため、設定データが空のままだと、RPC は YANG の検証の段階で失敗する。

| 参照している箇所 | 設定が無いとどうなるか | rats-init.xml の対応する部分 |
|---|---|---|
| RPC の**出力**の `certificate-name` は leafref で、`…/certificates/certificate/name` を参照している | sysrepo は rats-attesterd が返した応答を検証する。`ak-cert-0` が running に無いと、quote を取得できても応答全体がエラーになる | `<certificate><name>ak-cert-0</name>…` |
| RPC の**入力**の `tpm20-hash-algo` に `must '. = …/attester-supported-algos/tpm20-hash'` がある | Verifier が `TPM_ALG_SHA256` を指定すると、rats-attesterd に届く前に YANG の検証で拒否される | `<attester-supported-algos><tpm20-hash>taa:TPM_ALG_SHA256</tpm20-hash>` |
| `attester-supported-algos` に `when "…/tpm[firmware-version='taa:tpm20']"` がある | TPM 2.0 の tpm が設定されていないと、上の algos の設定自体が無効になる | `<tpm><name>tpm0</name><firmware-version>taa:tpm20</firmware-version>` |

さらに、`tpm20-pcr-bank`（sha256 の PCR 0〜9）は、Verifier が要求してよい PCR の範囲を決めている。
RFC 9684 は pcr-index の説明で「要求された PCR が、その TPM で使える PCR の部分集合であることを確認しなければならない（MUST）」としている。
rats-attesterd はこの値を、その確認の基準として使う。

まとめると、rats-init.xml は「この Attester は TPM 2.0 を 1 つ持ち、AK の名前は `ak-cert-0`、SHA-256 と ECDSA が使え、PCR 0〜9 を見せてよい」と宣言するもの。
これが無いと、⑤ で RPC を呼んでも必ず失敗する。Verifier が `get-config` でこの Attester の構成を知るための情報にもなる。

### ファイルにして sysrepocfg で入れる理由

- **NETCONF 経由では書き込めない**: ubuntu は NACM の `write-default deny` で書き込みを拒否される。guest で root として `sysrepocfg` を使うと、NACM を通らずに入れられる。
- **後から再現できる**: sysrepo を作り直したときに、同じ設定をもう一度入れられる。
- **事前に検証できる**: guest に入れる前に、yanglint で正しい設定か確かめられる。

### 用語の補足

モジュールを登録するコマンドは `sysrepoctl`、データ（設定）を出し入れするコマンドは `sysrepocfg`。

## その後

2026-10-07: `attester/config/rats-init.xml` を作成し、host の yanglint で検証してから guest の running と startup に投入した。
host から `get-config` で内容を確認した（設計書 04 §8 のチェックポイント）。
