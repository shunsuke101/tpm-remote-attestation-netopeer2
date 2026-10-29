# ② netopeer2-server を systemd で常駐させる

[README](README.md) の手順 ② の詳細。guest 側の作業。

## 1. unit ファイル

### 1.1 自動でインストールされる unit

ビルド時に `libsystemd-dev` があれば、netopeer2 の `make install` が `service/netopeer2-server.service.in` から unit を生成し、
`/usr/local/lib/systemd/system/netopeer2-server.service` に置く（pkg-config があると、unit の置き場所をインストール先のプレフィックスから決めるため。2026-10-05 に host で確認。systemd はこのディレクトリも読む）。
プレフィックスが `/usr/local` のときの展開後の内容は次のとおり。

```ini
[Unit]
Description=netopeer2 NETCONF server
#After=

[Service]
Type=notify
ExecStart=/usr/local/sbin/netopeer2-server -d -v2
Restart=always
SystemCallArchitectures=native
KillMode=control-group
SupplementaryGroups=

[Install]
WantedBy=multi-user.target
```

- `-d`: デーモン化せずフォアグラウンドで動き、ログを stderr に出す。systemd 経由なので journald に入る。
- `-v2`: ログのレベル（0=error、1=warning、2=verbose、3=debug）。
- `Type=notify`: 起動が終わると sd_notify で通知する。libsystemd をリンクしてビルドしたときだけ有効。
- `SupplementaryGroups=` が空なのは、sysrepo の `SYSREPO_GROUP` が既定で空のため。今回は root で動かすので問題ない。

### 1.2 libsystemd 無しでビルドした場合の代わりの unit

unit が生成されなかったときは、`/etc/systemd/system/netopeer2-server.service` を手で作る。
sd_notify が使えないので `Type=simple` にする。

```ini
[Unit]
Description=netopeer2 NETCONF server (manual unit)
After=network.target

[Service]
Type=simple
ExecStart=/usr/local/sbin/netopeer2-server -d -v2
Restart=always
KillMode=control-group

[Install]
WantedBy=multi-user.target
```

### 1.3 sysrepo-plugind の unit（⑤ で使う）

⑤ の attester（`rats-attester.so`）は sysrepo-plugind が読み込む plugin なので、sysrepo-plugind の unit も使う。
sysrepo の `make install` も、libsystemd が見つかれば `src/executables/sysrepo-plugind.service.in` から unit を生成し、
netopeer2 と同じ決め方で `/usr/local/lib/systemd/system/sysrepo-plugind.service` に置く（`ENABLE_SYSREPO_PLUGIND=ON` が既定）。

```ini
[Unit]
Description=Sysrepo plugin daemon
#After=

[Service]
Type=notify
ExecStart=/usr/local/bin/sysrepo-plugind -d -v2
Restart=always
SystemCallArchitectures=native
KillMode=control-group

[Install]
WantedBy=multi-user.target
```

- 起動すると、plugin ディレクトリ `/usr/local/lib/sysrepo-plugind/plugins/`（環境変数 `SRPD_PLUGINS_PATH` で変えられる）にある `.so` をすべて読み込み、
  それぞれの `sr_plugin_init_cb()` を呼ぶ。`rats-attester.so` は ⑤ の `make install` でここに入る。
- `-v` のレベルは netopeer2-server と違う: 0=none、1=error、2=warning、3=info、4=verbose、5=debug（`sysrepo-plugind -h`）。
  既定の `-v2` では、plugin が INF で出すログ（監査ログ、Quote の所要時間）は出ない（§4.1）。
- `-f` を付けると、plugin の初期化が 1 つでも失敗したときに終了する。付けなければ、失敗した plugin を外したまま動き続ける（[05](05_challenge_response.md) §3.4）。
- この unit は変更せず、TPM との依存は drop-in で足す（§3）。有効化は ⑤ で行う（[05](05_challenge_response.md) §3.8）。
- **要確認**: guest で `systemctl cat sysrepo-plugind` を実行して、unit が入っていることを確かめる。無ければ §1.2 と同じ形で手で作る（`Type=simple`）。

## 2. 有効化

```bash
# guest
sudo systemctl daemon-reload
sudo systemctl enable --now netopeer2-server
systemctl status netopeer2-server --no-pager
journalctl -u netopeer2-server -f          # ログを追う
sudo ss -ltnp | grep ':830'                # netopeer2-server が 0.0.0.0:830 で待ち受けていること
```

## 3. 起動順と依存関係

sysrepo には常駐プロセスが無い。netopeer2-server と sysrepo-plugind（⑤ の plugin `rats-attester.so` を読み込む）は、どちらも共有メモリ（`/dev/shm/sr_*`）と `/etc/sysrepo` を通して sysrepo を使う。

```
          (udev)                         (systemd)
  /dev/tpmrm0 出現 ──► dev-tpmrm0.device ──┐
                                           ├──► sysrepo-plugind.service   ← drop-in で依存を足す（⑤）
  netopeer2-server.service ────────────────┘      (After= で順序だけ付ける)
```

| unit | 依存の書き方 | 理由 |
|---|---|---|
| `netopeer2-server.service` | 変更しない | sysrepo の初期化と NETCONF の待ち受けを担う |
| `sysrepo-plugind.service` | sysrepo が入れる unit（§1.3）はそのまま使い、drop-in で `Requires=dev-tpmrm0.device`、`After=dev-tpmrm0.device netopeer2-server.service`、`Wants=netopeer2-server.service` を足す | TPM が無ければ plugin を読み込んでも意味がない。sysrepo の初期化は netopeer2-server に任せる |

drop-in（リポジトリの `attester/systemd/sysrepo-plugind.service.d/10-rats-attester.conf`。⑤ の `make install` で `/etc/systemd/system/sysrepo-plugind.service.d/` に入る）:

```ini
[Unit]
Requires=dev-tpmrm0.device
After=dev-tpmrm0.device netopeer2-server.service
Wants=netopeer2-server.service
```

- plugin が RPC を購読する前に RPC が届くと、sysrepo は「購読者がいない」というエラーを返し、netopeer2 は `rpc-error`（operation-failed）にする。順序はベストエフォートでよい。
- `dev-tpmrm0.device` は、udev が `/dev/tpmrm0` に `TAG+="systemd"` を付けている場合にだけ存在する。Ubuntu 24.04（systemd 255）では付いている想定だが、
  **要確認**: guest で `systemctl status dev-tpmrm0.device` を実行して確かめる。無ければ `Requires=` をやめて、
  `After=tpm2.target` と `ConditionPathExists=/dev/tpmrm0` に置き換える。

## 4. 運用上の注意

### 4.1 ログのレベルを変える

unit を直接書き換えず、drop-in を使う。

```bash
sudo systemctl edit netopeer2-server
# 開いたエディタに次を書く:
# [Service]
# ExecStart=
# ExecStart=/usr/local/sbin/netopeer2-server -d -v3
sudo systemctl restart netopeer2-server
```

sysrepo-plugind も同じようにする。`rats-attester.so` の INF のログ（監査ログ、Quote の所要時間）を見るには `-v3` 以上にする。

```bash
sudo systemctl edit sysrepo-plugind
# [Service]
# ExecStart=
# ExecStart=/usr/local/bin/sysrepo-plugind -d -v3
sudo systemctl restart sysrepo-plugind
```

`systemctl edit` は `override.conf` を `/etc/systemd/system/sysrepo-plugind.service.d/` に作る。⑤ の drop-in `10-rats-attester.conf` と同じディレクトリで、両方が読まれる。

### 4.2 手で起動してデバッグする

netopeer2-server は PID ファイル（`/var/run/netopeer2-server.pid`）が衝突するので、先にサービスを止める。

```bash
sudo systemctl stop netopeer2-server
sudo /usr/local/sbin/netopeer2-server -d -v3     # Ctrl-C で終了
sudo systemctl start netopeer2-server
```

sysrepo-plugind（`rats-attester.so`）も、サービスを止めてから前面で動かす。

```bash
sudo systemctl stop sysrepo-plugind
sudo /usr/local/bin/sysrepo-plugind -d -v3 -f    # Ctrl-C で終了。-f: plugin の初期化が失敗したら終了する
sudo systemctl start sysrepo-plugind
```

別の設定ファイルで試すときは、`sudo RATS_ATTESTER_CONF=<path> /usr/local/bin/sysrepo-plugind -d -v3 -f` とする（[05](05_challenge_response.md) §3.3）。

### 4.3 YANG モジュールを導入・変更したあと

④ で `sysrepoctl -i` や `-c` を実行したら、netopeer2-server と sysrepo-plugind（⑤ 以降）を再起動する。

```bash
sudo systemctl restart netopeer2-server sysrepo-plugind
```

### 4.4 running と startup

再起動などで sysrepo の共有メモリが無い状態から立ち上がると、running データストアは startup から作り直される。
**設定を入れたら、必ず startup にもコピーする**（④ の `sysrepocfg -C running -d startup -m <module>`）。
`merge_config.sh` も、startup に書いてから running にコピーしている。

### 4.5 異常終了したあとの共有メモリ

sysrepo を使うプロセスが異常終了すると、`/dev/shm/sr_*` にロックが残ることがある。通常は sysrepo が次の接続のときに回復する。
回復しないときは、sysrepo を使う全プロセスを止めてから、`sudo rm -f /dev/shm/sr_*` を実行する（running の内容は startup から作り直される）。

### 4.6 swtpm と QEMU の起動順

guest の再起動とは別に、host 側の起動順は従来どおり。

```bash
~/github/bin/tpm_emulator.sh            # swtpm（QEMU より先）
~/github/bin/run_qemu_measuredboot.sh   # QEMU（hostfwd の追加は 03 を参照）
```

swtpm が動いていないと QEMU が起動しないか、`/dev/tpmrm0` が現れず sysrepo-plugind が起動しない（drop-in の `Requires=dev-tpmrm0.device`）。

## 5. 確認項目

| # | 確認内容 | コマンド | 期待結果 |
|---|---|---|---|
| 1 | 自動起動が有効 | `systemctl is-enabled netopeer2-server` | `enabled` |
| 2 | 動作中 | `systemctl is-active netopeer2-server` | `active` |
| 3 | 待ち受け | `sudo ss -ltnp \| grep ':830'` | `0.0.0.0:830` に `netopeer2-server` |
| 4 | guest の再起動後も上がる | `sudo reboot` のあとに 1〜3 | 同上 |
| 5 | ログにエラーが無い | `journalctl -u netopeer2-server -b --no-pager \| grep -i err` | 致命的なエラーが無い |
| 6 | TPM の device unit がある | `systemctl status dev-tpmrm0.device` | `active (plugged)`（無ければ §3 の代案にする） |
| 7 | sysrepo-plugind が動作中（⑤ 以降） | `systemctl is-active sysrepo-plugind` | `active` |
| 8 | plugin が読み込まれた（⑤ 以降） | `sudo sysrepocfg -X -d operational -x /sysrepo-plugind:sysrepo-plugind` | `loaded-plugins` に `rats-attester` がある（初期化に成功した plugin だけが載る） |
