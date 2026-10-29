# RFC 9684 の feature bios と ima を有効にしない理由

- 日付: 2026-10-06
- 分類: 設計（RFC 9684 の YANG）
- 関連: `doc/design/04_rfc9684_yang.md` §1.2、`doc/design/06_future_extensions.md` §1

## 質問

> 設計書 04 の YANG の導入手順で、feature は `bios` と `ima` を入れるのではないのですか。
>
> ```bash
> sudo sysrepoctl -s . \
>     -i ietf-tcg-algs@2024-12-05.yang -e tpm20 \
>     -i ietf-tpm-remote-attestation@2024-12-05.yang
> ```

## 回答

設計書 04 では、意図して `bios` と `ima` を有効にしていない。最初に log-retrieval を「将来拡張として章だけ」にすると決めたため。
ただ、今入れても動作に悪い影響は無いので、入れるかどうかは方針次第。

### feature bios / ima が実際に有効にするもの

`attester/yang/ietf-tpm-remote-attestation@2024-12-05.yang` で確認した。

| 対象 | feature との関係 |
|---|---|
| `rpc log-retrieval`（777 行目） | if-feature は付いていない。feature が無くてもスキーマには存在する |
| identity `bios` / `ima`（156〜166 行目） | if-feature は付いていない |
| 出力の choice `attested_event_log_type` の case `bios`（640 行目）/ `ima`（650 行目） | **ここだけが** feature で有効になる |

この choice は `mandatory true`。そのため、feature を 1 つも有効にしないと、log-retrieval の出力に入れられるものがなくなり、RPC として使えない。
逆に言えば、feature を入れても変わるのは log-retrieval だけで、`tpm20-challenge-response-attestation` には影響しない。

### 今は有効にしなかった理由

1. **応答するものが無い**: rats-attesterd が購読するのは `tpm20-challenge-response-attestation` だけ。log-retrieval を呼んでも、購読者がいないので `operation-failed` になる。
2. **提供できるログが確認できていない**:
   - `bios`: u-boot の event log が Linux の `/sys/kernel/security/tpm0/binary_bios_measurements` に引き継がれているか、まだ確かめていない。
   - `ima`: 今使っているのは measured boot 用のイメージ（`run_qemu_measuredboot.sh`）。IMA 用のイメージ（`run_qemu_ima.sh`）は別にある。
   - 実際に返せないログの種類を、feature として公開することになる。
3. **後からでも 1 行で足せる**: `sudo sysrepoctl -c ietf-tpm-remote-attestation -e bios -e ima` を実行し、netopeer2-server を再起動するだけ。

なお、設計書 04 の NACM ルールでは、`verifiers` に許可しているのは `tpm20-challenge-response-attestation` だけ。
feature を有効にしても、log-retrieval は ubuntu を含む全員に拒否される。

### 今入れる場合

```bash
sudo sysrepoctl -s . \
    -i ietf-tcg-algs@2024-12-05.yang -e tpm20 \
    -i ietf-tpm-remote-attestation@2024-12-05.yang -e bios -e ima
```

本当に log-retrieval まで実装するなら、設計の変更が必要になる。04 の feature の表と NACM ルール、05 の rats-attesterd（log-retrieval の購読と event log の解析）、
Verifier の replay を変え、06 §1 の内容を本編に移す。

おすすめは、challenge-response が動くまでは無効のままにし、log-retrieval に取りかかるときに有効にすること。
