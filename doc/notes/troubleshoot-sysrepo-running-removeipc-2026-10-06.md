# get が「JSON DS file: Opening ... .running failed」で失敗する

- 日付: 2026-10-06
- 分類: トラブルシュート（sysrepo）
- 関連: `~/github/CLAUDE.md` の「未解決」、`doc/notes/sysrepo-shared-memory-2026-10-08.md`

## 質問

> host から netopeer2-cli で `connect --host localhost --login ubuntu --port 8300` して、`status` はできた。
> `get` を実行すると次のエラーになる。
>
> ```
> ERROR
>         message:  JSON DS file: Opening "/dev/shm/sr_ietf-subscribed-notifications.running" failed (No such file or directory).
> ```
>
> guest に `/dev/shm/sr_ietf-subscribed-notifications.running` が無い。

## 回答

### 原因: ubuntu 所有の running ファイルを systemd-logind が消していた

guest で確認した事実:

- `/dev/shm` に残っている running ファイルは、root 所有の 3 つだけだった（`ietf-netconf-acm`、`sysrepo-plugind`、`yang`）。
  `ietf-subscribed-notifications` だけでなく、`ietf-netconf-server` と `ietf-keystore` の running も無く、`sudo sysrepocfg -X -d running` でも同じエラーになる。
- 無くなったモジュールは、どれも running の所有者が `ubuntu` だった（`repository/data/<mod>.running.perm` の所有者。
  sysrepo は running ファイルをこの所有者で `/dev/shm` に作る）。
  - 所有者が ubuntu なのは、guest で `cmake` を ubuntu ユーザーで実行したため。netopeer2 の `MODULES_OWNER` が
    「cmake を実行したユーザー」（`id -un`）になり、`setup.sh` が 31 個のモジュールを `ubuntu:ubuntu` で登録した。
- guest の logind は既定の `RemoveIPC=yes` のまま。この設定では、ubuntu の最後のログインセッションが閉じたときに、
  ubuntu が所有する `/dev/shm` のファイルがすべて消される。

つまり、netopeer2-server の起動時（00:03 UTC）には running ファイルがあったが、その後 ssh のセッションがすべて閉じたときに消された。
1 コマンドずつ ssh で実行した分もきっかけになり得る。

### status が通って get が失敗した理由

- `status` は、connect のときに保存した情報を表示するだけで、サーバと通信しない。
- netopeer2 は、起動時に自分の設定（listen など）をメモリに読み込んでいるので、接続はできる。
- `get` は全モジュールの running を読みに行くので、ファイルが無いモジュールのところで失敗する。

startup のデータ（`~/github/sysrepo/build/repository/data/*.startup`）はディスク上にあるので消えていない。

### sysrepo の仕組み（ソースで確認）

- running ファイルは、sysrepo の共有メモリを作るときに、データを持つモジュールの分だけ作られる（`sysrepo/src/plugins/ds_json.c` の `srpds_json_init`）。
- running を読むときにファイルが無く、そのモジュールがデータを持つ場合はエラーになる（同 `srpds_json_load`）。

### 直し方（guest）

```bash
# 1. ubuntu 所有のモジュールを root に変える（-D 省略で全データストアに適用）
for m in $(sudo sysrepoctl -l | awk -F'|' '$4 ~ /ubuntu/ {gsub(/ /,"",$1); print $1}'); do
    sudo sysrepoctl -c "$m" -o root -g root
done

# 2. sysrepo の共有メモリを作り直して、running を startup から復元する
sudo systemctl stop netopeer2-server
sudo rm -f /dev/shm/sr_* /dev/shm/srsub_*
sudo systemctl start netopeer2-server       # 代わりに guest を再起動してもよい

# 3. 再発を防ぐ: 次に netopeer2 をインストールし直すときに root 所有で登録されるようにする
cd ~/github/Netopeer2/build && cmake -DMODULES_OWNER=root -DMODULES_GROUP=root ..
```

別の方法として、`/etc/systemd/logind.conf.d/` で `RemoveIPC=no` にすることもできる。

## その後

- 2026-10-07: guest で `RemoveIPC=no` が設定されていることを確認した（再発はしない）。
- 2026-10-07 時点では、消えたファイルはまだ作り直されておらず、`sudo sysrepocfg -X -d running -m ietf-netconf-server` が失敗する。
  31 個のモジュールは ubuntu 所有のまま。
- `ietf-tpm-remote-attestation` は root 所有なので、この問題の影響を受けない。
- 2026-10-14: **解消した。** sysrepo のリポジトリが消えたため、Release（`/etc/sysrepo`）で作り直し、netopeer2 を `MODULES_OWNER=root` で入れ直した
  （`doc/notes/troubleshoot-sysrepo-debug-repository-lost-2026-10-14.md`）。モジュールはすべて `root:root` になり、共有メモリも作り直された。
  `sudo sysrepocfg -X -d running -m ietf-netconf-server` が通り、host からの `get` も通る。
