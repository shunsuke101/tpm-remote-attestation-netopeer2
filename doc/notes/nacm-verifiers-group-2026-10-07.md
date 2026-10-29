# NACM の verifiers グループに ubuntu を入れている理由

- 日付: 2026-10-07
- 分類: NACM / RATS の役割
- 関連: `doc/design/04_rfc9684_yang.md` §7、`attester/config/nacm-rats.xml`

この日の 2 つの質問への回答をまとめた。

---

## Q1. nacm-rats.xml が `<group><name>verifiers</name><user-name>ubuntu</user-name>` になっているのはなぜ

### 回答

NACM では、ルールを「ユーザー」に直接付けられず、必ず「グループ」に付ける仕組みになっているから（RFC 8341）。そのため 2 段構えにしている。

```
/nacm/groups/group                    /nacm/rule-list
┌──────────────────────────┐          ┌──────────────────────────────────┐
│ name: verifiers          │ ◄─────── │ group: verifiers                 │
│ user-name: ubuntu        │          │  rule: tpm20-… の exec を permit │
└──────────────────────────┘          │  rule: rats-… の read を permit  │
                                      └──────────────────────────────────┘
```

1. **`<groups>`**: 「どのユーザーがどのグループに属するか」を決める。ここで `ubuntu` を `verifiers` に入れている。
2. **`<rule-list>`**: 「どのグループに何を許可・拒否するか」を決める。`<group>verifiers</group>` と書いて、1 のグループを指している。

rule-list にはユーザー名を書く欄が無い。グループを経由するしかないので、この形になる。

#### グループ名が verifiers である理由

RATS の役割（Verifier）に合わせて付けた名前で、何でもよい。rule-list の `<group>` と同じ文字列になっていれば動く。役割ごとにグループにしておく利点:

- Verifier 用のアカウントを増やすときは、`<user-name>` を追加するだけで済み、ルールは触らなくてよい。
- 別の役割（監視用など）を作ったときも、グループを分けるだけで権限を分けられる。

#### user-name が ubuntu である理由

`<user-name>` には、**NETCONF でログインするときのユーザー名**を書く。

- host の netopeer2-cli は `--login ubuntu` で接続している。
- netopeer2-server は、SSH で認証したユーザー名を、そのまま NACM のユーザーとして sysrepo のセッションに設定する
  （`Netopeer2/src/common.c:739` の `sr_nacm_set_user(sr_sess, username)`）。
- netopeer2 のインストール時に `merge_config.sh` が許可したユーザーは `ubuntu` だけなので、今 NETCONF でログインできるのは ubuntu だけ。

もう一方の rule-list `rats-deny-others` の `<group>*</group>` は「全ユーザー」を表す。
ubuntu の要求は先に `rats-verifiers` に一致するので許可され、それ以外のユーザーの要求はこちらで拒否される。

---

## Q2. NETCONF サーバが Attester で、クライアント側が Verifier になっていると思うのですが

### 回答

その理解で合っている。NETCONF サーバ（guest の netopeer2-server）が Attester で、クライアント（host の netopeer2-cli）が Verifier。

`ubuntu` が guest のユーザー名なので紛らわしいが、`nacm-rats.xml` の `ubuntu` は Attester 自身ではない。
**Verifier が Attester にログインするときに使うアカウント名**。

```
host（Verifier）                                guest（Attester）
lab-student が netopeer2-cli を実行
  connect --login ubuntu  ───── SSH ─────►  netopeer2-server
  （鍵: host の ~/.ssh/id_ed25519）            │ ① 認証: ubuntu の authorized_keys にある鍵か
                                               │   → 接続してきた相手は「ubuntu」
                                               │ ② NACM: ubuntu は verifiers グループ
                                               │   → tpm20-challenge-response-attestation を許可
                                               ▼
                                            rats-attesterd が Quote を返す
```

- **NACM は Attester の側で動く**: 「接続してきたクライアントに何を許すか」を決めるのはサーバ（Attester）。そのため、ルールは guest の sysrepo に入れる。
- **クライアントは、サーバ側のアカウント名で識別される**: SSH でログインするときと同じで、Verifier は Attester 上のアカウント（ここでは `ubuntu`）として認証される。
  host のユーザー名（`lab-student`）は Attester からは見えない。
- **verifiers グループの意味**: 「Attester に Verifier として接続してくるアカウントの集まり」。Attester の側で、
  「このアカウントで来た相手は Verifier として扱い、attestation の RPC を許す」と宣言している。

---

## 改善の余地

今は `ubuntu` が、guest の管理用（SSH ログイン）と Verifier 用を兼ねているので紛らわしい。
権限を最小限にし、役割も読み取りやすくするなら、NETCONF 専用のアカウント（例: `verifier`）を作るのが望ましい。その場合は次の 2 か所を変える。

- `ietf-netconf-server` の users に、そのユーザーを追加する
- `nacm-rats.xml` の `<user-name>` を、そのユーザーに変える

研究用の PoC としては、今のままでも問題ない。

## その後

2026-10-07: `nacm-rats.xml` を guest の running と startup に投入した。host から ubuntu で `rats-support-structures` を読めることと、
`/ietf-netconf-acm:nacm` が空で返る（ubuntu のセッションに NACM が適用されている）ことを確認した。
