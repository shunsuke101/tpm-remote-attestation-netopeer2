# ③ netopeer2-cli による疎通確認

[README](README.md) の手順 ③ の詳細。host の netopeer2-cli から guest の netopeer2-server に接続する。

## 1. 経路

```
host: netopeer2-cli ──TCP──► localhost:8300 ──(QEMU slirp hostfwd)──► guest: 0.0.0.0:830 netopeer2-server
                     SSH（libssh 同士）の上で NETCONF（RFC 6242、subsystem "netconf"）
```

- netopeer2-server の SSH は libssh によるもので、guest の OpenSSH の sshd（22 番、host からは 2222）とは別物。ホスト鍵も別（`ietf-keystore` の `genkey`）。
- netopeer2-cli は libnetconf2 の SSH クライアントを使う。known_hosts は OpenSSH と同じ `~/.ssh/known_hosts` を使い、
  `[localhost]:8300` として記録するので、`[localhost]:2222` とは混ざらない。

## 2. QEMU の起動スクリプトに hostfwd を追加する

既存の `bin/run_qemu_measuredboot.sh` の `-netdev` に `hostfwd=tcp::8300-:830` を足す。
本設計の段階では変更しない。実装時に適用する。

```diff
 $INSTALL/qemu-system-riscv64 \
   -machine virt -cpu rv64 -m 4G -smp 2 \
   -nographic \
   -kernel ~/github/u-boot/u-boot_measuredboot.elf \
-  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
+  -netdev user,id=net0,hostfwd=tcp::2222-:22,hostfwd=tcp::8300-:830 \
   -device virtio-net-device,netdev=net0 \
```

- `run_qemu_tpm.sh` や `run_qemu_ima.sh` も使うなら、同じ変更を入れる。
- 起動の手順は今までと変わらない。

```bash
~/github/bin/tpm_emulator.sh
~/github/bin/run_qemu_measuredboot.sh
ssh -p 2222 ubuntu@localhost        # 既存の SSH（guest の作業用）
```

- QEMU が起動した時点で、host 側の 8300 番は QEMU が待ち受ける。`ss -ltn | grep 8300` で確認できる。
  guest で netopeer2-server が動いていなければ、接続した直後に切られる。

## 3. guest の中での事前確認

host から試す前に、guest の中だけで動くことを確かめておく。問題を経路と切り分けられる。

```text
ubuntu@guest$ netopeer2-cli
> connect --host localhost --login ubuntu
> status
> disconnect
> exit
```

ポートを指定しなければ 830 に接続する。

## 4. 認証の準備（host）

[01](01_build_netconf_stack.md) §4.3 で決めた認証方式に合わせる。

### 4.1 公開鍵（guest に authorized_keys がある場合）

netopeer2-cli は鍵を自動で読まないので、明示的に登録する。登録した内容は `~/.netopeer2-cli/config.xml` に保存され、次回以降も使われる。

```text
$ netopeer2-cli
> auth keys add /home/lab-student/.ssh/id_ed25519.pub /home/lab-student/.ssh/id_ed25519
> auth keys                 # 登録された鍵の一覧
> auth pref                 # 認証方式の優先度（publickey が最優先になっているか確認する）
```

### 4.2 パスワード（keyboard-interactive）

`connect` のときに guest の `ubuntu` のパスワードを聞かれる。PAM の設定は [01](01_build_netconf_stack.md) §4.4 を参照。

### 4.3 ホスト鍵の確認

初めて接続するとき、`knownhosts --mode` が `ask`（既定）であれば、フィンガープリントを確認するよう求められる。
正しく照合したいときは、guest 側で `ietf-keystore` の `genkey` の公開鍵を取り出して比べる。

```bash
# guest
sudo sysrepocfg -X -d running -m ietf-keystore -x "/ietf-keystore:keystore/asymmetric-keys/asymmetric-key[name='genkey']/public-key"
```

## 5. host からの確認（手順 ③ のコマンド）

```text
$ netopeer2-cli
> connect --host localhost --login ubuntu -p 8300
> status
> get
> get-config --source running
> disconnect
```

- `-p` は `--port` の短縮形で、`-l` は `--login`、`-o` は `--host` の短縮形（`cli/commands.c` の `getopt_long(..., "so:p:l:")`）。
- `get-config` では `--source running|startup|candidate` が必須。メモでは省略されていたので補った。
- `get` の出力は大きい（ietf-yang-library、netconf-state、NACM など）。`--out get.xml` でファイルに保存するか、`--filter-xpath` で絞り込む。

### 5.1 期待される結果

**status**

```text
Current NETCONF session:
  ID          : 1
  Host        : localhost
  Port        : 8300
  Transport   : SSH
  Capabilities:
	urn:ietf:params:netconf:base:1.0
	urn:ietf:params:netconf:base:1.1
	urn:ietf:params:netconf:capability:writable-running:1.0
	...
	urn:ietf:params:netconf:capability:yang-library:1.1?revision=2019-01-04&content-id=...
```

注意: NETCONF 1.1 と yang-library 1.1 の組み合わせでは、YANG 1.1 のモジュールは hello の capabilities に載らない（RFC 7950 §5.6.4）。
④ で導入する `ietf-tpm-remote-attestation` も YANG 1.1 なので、`status` には出てこない。
導入したことは ietf-yang-library で確かめる（[04](04_rfc9684_yang.md) §8）。

**get**

`ietf-yang-library:yang-library`、`ietf-netconf-monitoring:netconf-state`、`ietf-netconf-acm:nacm` などの状態データが返る。

**get-config --source running**

`ietf-netconf-server:netconf-server`（endpoint `default-ssh`、`local-address 0.0.0.0`、ユーザー `ubuntu`）と `ietf-keystore:keystore` などが返る。
秘密鍵のような NACM で `default-deny-all` になっている部分は、recovery user（root）以外には返らない。

### 5.2 絞り込みの例

```text
> get --filter-xpath /ietf-netconf-monitoring:netconf-state/sessions
> get-config --source running --filter-xpath /ietf-netconf-server:netconf-server
> get --filter-xpath "/ietf-yang-library:yang-library/module-set/module[name='ietf-netconf-acm']"
```

## 6. よくある失敗

| 症状 | 原因 | 対処 |
|---|---|---|
| `Connection refused` | QEMU に hostfwd が無い、または QEMU が動いていない | §2 の変更を確認する。`ss -ltn \| grep 8300` |
| つながった直後に切断される | guest の netopeer2-server が止まっている | guest で `systemctl status netopeer2-server` |
| `Authentication failed` | 鍵が登録されていない。authorized_keys が無い。ユーザーが `ubuntu` 以外になっている | §4。guest で `sudo sysrepocfg -X -m ietf-netconf-server` を実行し、users を確認する |
| ホスト鍵が一致しない警告 | guest を作り直したなどで `genkey` が変わった | `knownhosts` で古い `[localhost]:8300` のエントリを消す（`knownhosts --del <index>`） |
| `access-denied` | NACM による拒否 | [04](04_rfc9684_yang.md) §7 のルールを確認する |
| `get-config` がエラーになる | `--source` を指定していない | `get-config --source running` |
