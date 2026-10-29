# tests

| パス | 内容 |
|---|---|
| `check_yang.sh` | host で実行。[04](../doc/design/04_rfc9684_yang.md) §3 の yanglint による検証（スキーマ、初期設定、RPC 入力、あれば RPC 出力） |
| `samples/challenge-sample.xml` | 固定 nonce の challenge（yanglint 用） |
| `samples/reply-sample.xml` | ⑤ で実際の応答から作る（libyang の YANG reply 形式: `<tpm20-challenge-response-attestation>` の下に `<tpm20-attestation-response>`） |

## テスト計画
正常系 N-1〜N-6 と異常系 E-1〜E-16 は [05](../doc/design/05_challenge_response.md) §6 にある。
各テストで記録するもの（同 §6.3）:
- `reply.xml`、`rats-verify` の JSON（標準出力）
- netopeer2-server と sysrepo-plugind のログ（`journalctl -u netopeer2-server -u sysrepo-plugind --since ...`）
- Quote と PCR_Read の所要時間（plugin のログに ms 単位で出す）

記録は `tests/results/<日付>/<テスト ID>/` に置く（⑤ の実装時に作る）。
