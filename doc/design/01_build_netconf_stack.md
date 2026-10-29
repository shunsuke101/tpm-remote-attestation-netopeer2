# ① libyang / libnetconf2 / sysrepo / netopeer2 の導入

[README](README.md) の手順 ① の詳細。guest（Attester）と host（Verifier）の両方にビルドする。

## 1. 方針

| | guest（riscv64） | host（WSL2 x86_64） |
|---|---|---|
| 目的 | netopeer2-server、sysrepo、sysrepo-plugind（⑤ の plugin `rats-attester.so` を読み込む）を動かす | netopeer2-cli で接続し、yanglint で YANG を検証する |
| ビルドするもの | libyang → libnetconf2 → sysrepo → netopeer2（server と CLI） | libyang → libnetconf2 → sysrepo → netopeer2（CLI だけ） |
| netopeer2 の CMake オプション | 既定（`BUILD_SERVER=ON`、`BUILD_CLI=ON`、`SYSREPO_SETUP=ON`） | `-DBUILD_SERVER=OFF -DSYSREPO_SETUP=OFF` |
| インストール先 | `/usr/local`（既定の `CMAKE_INSTALL_PREFIX`） | `/usr/local` |
| ビルドの種類 | **Release**（§4.1。sysrepo を Debug にするとリポジトリが build の中に置かれる） | Release |
| sysrepo のリポジトリ | `/etc/sysrepo`（Release の既定の `REPO_PATH`） | 使わない（`SYSREPO_SETUP=OFF`） |

- バージョンは host の `~/github` に clone してある master に揃える（[README](README.md) §6）。
  netopeer2 2.8.7 の CMake は、依存先を SO version で照合する（`LIBYANG_DEP_SOVERSION 5.1.3`、`LIBNETCONF2_DEP_SOVERSION 5.4.3` など）。
  そのため、4 つは同じ時期のリリースで揃える。1 つだけ更新するとビルドが通らない。
- host でも sysrepo のビルドが必要。netopeer2 の `CMakeLists.txt` は、`BUILD_SERVER` の値によらず
  `find_package(Sysrepo ${SYSREPO_DEP_SOVERSION} REQUIRED)` を実行するため。
- apt の `libyang2t64`、`libnetconf2-*`、`sysrepo`、`netopeer2` は入れない。
  入っていると、古い `.so` をリンクしたり、`/usr/bin/sysrepoctl` が優先されたりして混乱する。
  事前に `dpkg -l | grep -E 'libyang|libnetconf2|sysrepo|netopeer2'` で確認しておく。

## 2. 依存パッケージ

### 2.1 共通（guest と host）

既存の `~/github/bin/libnetconf2_need_package.sh` を使う。中身は次のとおり。

```
build-essential cmake git pkg-config libcrypt-dev libssh-dev libssl-dev
libcurl4-openssl-dev libpcre2-dev libxxhash-dev libpam0g-dev
```

それぞれの用途:

- libyang: `libpcre2-dev`（必須）、`libxxhash-dev`（任意だが速くなる）
- libnetconf2: libssh 0.9.5 以上、OpenSSL 3.0.0 以上、curl 7.30.0 以上、crypt(3)。PAM は任意

Ubuntu 24.04 の libssh は 0.10 系、OpenSSL は 3.0 系なので、条件を満たす。

### 2.2 追加するもの

| パッケージ | guest | host | 理由 |
|---|---|---|---|
| `libsystemd-dev` | 必須 | 不要 | netopeer2 は libsystemd が見つかったときだけ `netopeer2-server.service`（Type=notify）を生成してインストールする（[02](02_netopeer2_systemd.md)） |
| `libtss2-dev` | 必須 | 必須 | `rats-attester.so`（リポジトリの `attester/`）が tss2-esys、tss2-mu、tss2-tctildr、tss2-rc を使う。libcrypto（`libssl-dev`。§2.1 の共通の依存に含まれる）も使う。host では `rats-verify`（`verifier/`）が tss2-mu で TPM の構造体を unmarshal する |
| `tpm2-tools` | 必須 | 必須 | guest では AK の準備に、host では `tpm2_checkquote` による二重確認に使う（[05](05_challenge_response.md)） |
| `libexpat1-dev`, `libjson-c-dev` | 不要 | 必須 | `rats-verify`（C の Verifier。[05](05_challenge_response.md) §5.2）が reply.xml の XML と golden / state / 結果の JSON を扱う。無ければ CMake は `BUILD_VERIFIER` を OFF にして attester だけを構成する |
| `pipx`（xym、pyang） | 不要 | 必須 | RFC から YANG を抽出して検証する（[04](04_rfc9684_yang.md)） |

guest で実行する例:

```bash
# guest（ubuntu ユーザー）
bash ~/libnetconf2_need_package.sh          # host から scp で持ってくる（§3）
sudo apt install -y libsystemd-dev libtss2-dev tpm2-tools
```

host で実行する例:

```bash
# host
bash ~/github/bin/libnetconf2_need_package.sh
sudo apt install -y tpm2-tools libtss2-dev libexpat1-dev libjson-c-dev pipx
pipx install xym && pipx install pyang
```

## 3. guest へのソースの配置

host の clone を tar にまとめて送る方法を推奨する。同じ commit を確実に使え、guest から GitHub に出られない場合にも困らない。

```bash
# host
cd ~/github
tar czf /tmp/netconf-src.tgz --exclude='*/build' libyang libnetconf2 sysrepo Netopeer2
scp -P 2222 /tmp/netconf-src.tgz bin/libnetconf2_need_package.sh ubuntu@localhost:~

# guest
mkdir -p ~/github
tar xzf ~/netconf-src.tgz -C ~/github
for d in libyang libnetconf2 sysrepo Netopeer2; do git -C ~/github/$d log -1 --format="$d %h %s"; done
```

guest でも host と同じ `~/github/{libyang,libnetconf2,sysrepo,Netopeer2}` に置く。
`.git` も一緒に送られるので、guest でも commit を確認できる。host で作った `build/` は送らない（アーキテクチャが違うので使えない）。

別の方法として、guest で直接 clone してもよい。

```bash
git clone --branch v5.8.6  https://github.com/CESNET/libyang.git
git clone --branch v4.4.10 https://github.com/CESNET/libnetconf2.git
git clone --branch v5.1.0  https://github.com/sysrepo/sysrepo.git
git clone --branch v2.8.7  https://github.com/CESNET/netopeer2.git Netopeer2
```

## 4. guest でのビルド

### 4.1 ビルドの手順

```bash
# guest（ubuntu ユーザー。root シェルにはしない。§4.3 を参照）
build() {   # $1 = ディレクトリ名、$2 以降 = 追加の cmake オプション
    local dir=$1; shift
    mkdir -p ~/github/$dir/build && cd ~/github/$dir/build || return 1
    cmake -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=OFF "$@" .. || return 1
    make -j"$(nproc)" || return 1
    sudo make install || return 1
    sudo ldconfig
}

build libyang
build libnetconf2
build sysrepo   -DNOTIFD_MODULE_OWNER=root -DNOTIFD_MODULE_GROUP=root   # 「Sysrepo repository: /etc/sysrepo」と出ること
build Netopeer2 -DMODULES_OWNER=root -DMODULES_GROUP=root
```

> **sysrepo は必ず Release でビルドする。** 4 つとも、`CMAKE_BUILD_TYPE` を指定しないと Debug になる（各 `CMakeLists.txt` の
> `if(NOT CMAKE_BUILD_TYPE) set(CMAKE_BUILD_TYPE Debug)`）。sysrepo は Debug のとき、リポジトリ（登録したモジュール、startup の設定、
> YANG ファイル）を build ディレクトリの中の `build/repository` に置く（`sysrepo/CMakeLists.txt:214-219`。Release などでは `/etc/sysrepo`）。
> そのため、Debug でビルドした sysrepo の `build/` を消すと、netopeer2 の設定や ④ の YANG と設定もすべて消える（2026-10-14 に guest で発生。§7.1、§8）。
> cmake の出力の `Sysrepo repository:` の行で置き場所を確かめる。

モジュールの所有者は root にする（`NOTIFD_MODULE_OWNER`、`MODULES_OWNER`）。既定では cmake を実行したユーザー（`ubuntu`）になり、
logind の `RemoveIPC=yes` が、そのユーザーのログアウト時に running の共有メモリを消す原因になった
（`doc/notes/troubleshoot-sysrepo-running-removeipc-2026-10-06.md`。guest は `RemoveIPC=no` も設定済み）。
sysrepoctl や sysrepocfg は、どのみち `sudo` を付けて使う。

### 4.2 各ライブラリの注意点

| 対象 | 入るもの | 注意 |
|---|---|---|
| libyang | `libyang.so`、`yanglint`、内蔵モジュール（`ietf-yang-types@2025-12-22`、`ietf-inet-types@2025-12-22` など） | `libpcre2-dev` が無いと cmake で失敗する |
| libnetconf2 | `libnetconf2.so`、`modules/`（`ietf-keystore@2024-10-10`、`ietf-netconf-server@2025-04-24` など） | `ENABLE_SSH_TLS=ON`（既定）。libpam が見つかれば keyboard-interactive の system auth が使える |
| sysrepo | `libsysrepo.so`、`sysrepoctl`、`sysrepocfg`、`sysrepo-plugind`、`sysrepo-notifd` | Release ビルドの既定値は `REPO_PATH=/etc/sysrepo`（Debug では `<build>/repository`。§4.1 の注意）、`SHM_DIR=/dev/shm`、`NACM_RECOVERY_USER=root`、`SYSREPO_GROUP=""`。sysrepo-plugind（`ENABLE_SYSREPO_PLUGIND=ON` が既定）は ⑤ で `rats-attester.so` を読み込ませるのに使う。libsystemd があれば `sysrepo-plugind.service` も入る（[02](02_netopeer2_systemd.md) §1.3）。`make install` の最後に `setup_notifd.sh` が notifd のモジュール（`ietf-subscribed-notifications` など）を sysrepo に入れる |
| netopeer2 | `netopeer2-server`、`netopeer2-cli`、`netopeer2-server.service`、`netopeer2.conf`（PAM） | §4.3 と §4.4 を参照 |

### 4.3 netopeer2 の `make install` で起きること

`SYSREPO_SETUP=ON`（既定）のとき、`make install` の中で次の 3 つのスクリプトが走る。

1. `scripts/setup.sh`: netopeer2 と libnetconf2 の YANG モジュールを sysrepo に導入する
   （`ietf-netconf-server`、`ietf-keystore`、`ietf-truststore`、`ietf-netconf-acm` など）。
2. `scripts/merge_hostkey.sh`: SSH ホスト鍵 `genkey` を生成し、`ietf-keystore` に入れる。
3. `scripts/merge_config.sh`: listen 用の endpoint `default-ssh`（`0.0.0.0`、ポートは既定の 830）と、ログインできるユーザーを設定する。
   - ユーザーは `$SUDO_USER` になる。**`ubuntu` ユーザーから `sudo make install` すること。** root シェルで実行すると root 用の設定になってしまう。
   - `~ubuntu/.ssh/authorized_keys` があれば、公開鍵認証（`use-system-keys`）が設定される。
   - 無ければ、keyboard-interactive（`use-system-auth`、つまりパスワード）が設定される。
   - すでに endpoint があるときは何もしない。設定をやり直すには、`ietf-netconf-server` の設定を消してから再実行する。

`make install` の前に、認証方式を決めておく。

- 公開鍵を使う場合（推奨）: host の公開鍵（例: `~/.ssh/id_ed25519.pub`）を guest の `~ubuntu/.ssh/authorized_keys` に追記しておく。
  旧方式では `TrustedUserCAKeys` を使っていたので、authorized_keys が無いかもしれない。
- パスワードを使う場合: そのままでよい。

### 4.4 PAM 設定ファイルの場所

libnetconf2 は `pam_start("netopeer2.conf", ...)` を呼ぶので、PAM は `/etc/pam.d/netopeer2.conf` を探す。
しかし netopeer2 はこのファイルを `${CMAKE_INSTALL_SYSCONFDIR}/pam.d`、
つまり既定のプレフィックスでは `/usr/local/etc/pam.d/netopeer2.conf` にインストールする。
見つからない場合は `/etc/pam.d/other` が使われ、Ubuntu ではそれでも common-auth に回るので動く。
意図どおりの設定にするため、パスワード認証を使うならコピーしておく。

```bash
sudo cp /usr/local/etc/pam.d/netopeer2.conf /etc/pam.d/netopeer2.conf
```

### 4.5 ビルド時間を短くする

RISC-V は TCG でエミュレーションされるので、ビルドが遅い。host は 12 コアあるので、ビルドの間だけ QEMU の `-smp` を増やすとよい。
`bin/run_qemu_measuredboot.sh` を直接書き換えず、コピーして使う。

```bash
# host: ビルド用にコピーして -smp を変える（メモリは 4G のまま）
sed 's/-smp 2/-smp 8/' ~/github/bin/run_qemu_measuredboot.sh > ~/github/bin/run_qemu_measuredboot_build.sh
```

`make -j"$(nproc)"` は guest の CPU 数に従う。メモリが足りなくなったら `-j4` に下げる。

## 5. host でのビルド

```bash
# host
build_host() {   # $1 = ディレクトリ名、$2 以降 = 追加の cmake オプション
    local dir=$1; shift
    mkdir -p ~/github/$dir/build && cd ~/github/$dir/build || return 1
    cmake -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=OFF "$@" .. || return 1
    make -j"$(nproc)" || return 1
    sudo make install || return 1
    sudo ldconfig
}

build_host libyang
build_host libnetconf2
build_host sysrepo                                   # netopeer2 の CMake が必須としているため
build_host Netopeer2 -DBUILD_SERVER=OFF -DSYSREPO_SETUP=OFF
```

- `SYSREPO_SETUP=OFF` にするので、host の sysrepo にはモジュールが入らず、データストアも使わない。
- `yanglint` は libyang と一緒に host にも入る。[04](04_rfc9684_yang.md) の検証に使う。

## 6. 確認

guest:

```bash
yanglint --version
ls -ld /etc/sysrepo                               # リポジトリが build の外にあること（§4.1）
sudo sysrepoctl -l | head -30                     # モジュールの一覧。owner が root:root であること
netopeer2-server -V                               # バージョン
sudo sysrepocfg -X -d running -m ietf-netconf-server   # endpoint default-ssh とユーザー ubuntu
sudo sysrepocfg -X -d running -m ietf-keystore | head  # ホスト鍵 genkey
ls -l /usr/local/lib/systemd/system/netopeer2-server.service # libsystemd-dev があれば生成されている
sysrepo-plugind -V                                # ⑤ で使う
ls -l /usr/local/lib/systemd/system/sysrepo-plugind.service  # 同上
```

host:

```bash
yanglint --version
netopeer2-cli        # プロンプト > が出たら help で一覧を確認し、exit で抜ける
```

## 7. やり直し・アンインストール

| 目的 | 方法 |
|---|---|
| netopeer2 のモジュールを sysrepo から外す | `Netopeer2/scripts/remove.sh`（`SYSREPOCTL_EXECUTABLE` などの環境変数が必要） |
| インストールしたファイルを消す | 各 `build/` で `sudo xargs rm -v < install_manifest.txt` |
| sysrepo のリポジトリを初期化する（**破壊的**） | sysrepo を使うプロセス（netopeer2-server、sysrepo-plugind）を止めてから `sudo rm -rf /etc/sysrepo /dev/shm/sr_*`。すべてのモジュールと設定が消えるので、§4.3 からやり直しになる |

### 7.1 sysrepo のリポジトリが消えたときの復旧（2026-10-14 に guest で実施）

Debug でビルドした sysrepo の `build/` を消したあとの状態（§8 の該当する行）から、Release で作り直す。
消えたモジュールと設定は、それぞれを入れたスクリプトをもう一度実行して入れ直す。

```bash
# guest（ubuntu ユーザー）
# 1. sysrepo を使うデーモンを止め、消えたリポジトリを指している古い共有メモリを消す
sudo systemctl stop netopeer2-server sysrepo-plugind sysrepo-notifd
sudo rm -f /dev/shm/sr_* /dev/shm/srsub_*

# 2. sysrepo を Release で作り直す（リポジトリは /etc/sysrepo。setup_notifd.sh が notifd のモジュールを入れる）
cd ~/github/sysrepo && rm -rf build && mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=OFF -DNOTIFD_MODULE_OWNER=root -DNOTIFD_MODULE_GROUP=root ..
make -j"$(nproc)" && sudo make install && sudo ldconfig

# 3. netopeer2 のモジュールと設定を入れ直す（setup.sh、merge_hostkey.sh、merge_config.sh。§4.3）
cd ~/github/Netopeer2/build
cmake -DMODULES_OWNER=root -DMODULES_GROUP=root ..
sudo make install

# 4. ④ の YANG、初期設定、NACM を入れ直す（04 §4、§5、§7.3）
cd ~/tpm-remote-attestation-netopeer2
sudo attester/scripts/install_yang.sh
sudo attester/scripts/install_config.sh

# 5. 起動する（⑤ の plugin を入れていれば sysrepo-plugind も）
sudo systemctl daemon-reload
sudo systemctl start netopeer2-server
```

- netopeer2 のライブラリとバイナリは作り直さなくてよい。sysrepo の SO version は変わらず、リポジトリの場所は libsysrepo の中にだけ埋め込まれているため。
- 4 は netopeer2-server を起動する前に行う。netopeer2 のモジュール（`ietf-keystore` など）は 3 の `make install` で入っているので、
  `ietf-tpm-remote-attestation` の import は解決できる。
- `merge_hostkey.sh` が新しい SSH ホスト鍵 `genkey` を作るので、host の netopeer2-cli はホスト鍵の不一致で接続を拒む
  （netopeer2-cli は libssh の既定どおり `~/.ssh/known_hosts` を使う）。表示された鍵をそのまま受け入れず、
  guest の keystore から求めた公開鍵と照合してから登録する。keystore には秘密鍵しか入っていないので、公開鍵は guest の中で計算し、
  host には公開鍵だけを持ってくる。

  ```bash
  # host
  ssh -p 2222 ubuntu@localhost 'sudo sysrepocfg -X -d running -m ietf-keystore -f json \
      | python3 -c "import json,sys; ks=json.load(sys.stdin)[\"ietf-keystore:keystore\"][\"asymmetric-keys\"][\"asymmetric-key\"]; print([k for k in ks if k[\"name\"]==\"genkey\"][0][\"cleartext-private-key\"])" \
      | base64 -d | openssl rsa -inform DER -pubout 2>/dev/null' > genkey.pub.pem
  ssh-keygen -i -m PKCS8 -f genkey.pub.pem > genkey.ssh.pub
  ssh-keygen -l -f genkey.ssh.pub                                                   # keystore の鍵
  ssh-keyscan -p 8300 -t rsa localhost 2>/dev/null | cut -d' ' -f2- | ssh-keygen -l -f -   # 8300 番が提示する鍵。一致すること
  ssh-keygen -R "[localhost]:8300"                                                  # 古い鍵を消す（元は known_hosts.old に残る）
  echo "[localhost]:8300 $(cat genkey.ssh.pub)" >> ~/.ssh/known_hosts
  ```

  そのあと [04](04_rfc9684_yang.md) §8 の確認をやり直す。
- swtpm の中の EK と AK、リポジトリ（`~/tpm-remote-attestation-netopeer2`）は sysrepo と関係ないので、影響を受けない。

## 8. よくある失敗

| 症状 | 原因 | 対処 |
|---|---|---|
| `Could NOT find LibYANG ... (Required is at least version ...)` | 4 つのバージョンが揃っていない | §1 の表のタグに揃える |
| `error while loading shared libraries: libyang.so.5` | ldconfig をしていない | `sudo ldconfig`。`/usr/local/lib` が `/etc/ld.so.conf.d/` に含まれているか確認する |
| netopeer2 の `sudo make install` が `Installing missing sysrepo modules (setup.sh)...` の後で `sysrepoctl: error while loading shared libraries: libsysrepo.so.8` を出して止まる（2026-10-05 に host と guest の両方で発生） | sysrepo などを入れた後に ldconfig をしておらず、動的リンカのキャッシュに `/usr/local/lib` の新しいライブラリが載っていない。ファイルが root:root で 644 なのは正常で、権限の問題ではない | `sudo ldconfig` を実行してから、netopeer2 の `sudo make install` をやり直す。止まった時点で `netopeer2-cli` はまだインストールされていない |
| `sysrepoctl: ... Permission denied` | `/etc/sysrepo` や `/dev/shm/sr_*` は root のもの | `sudo` を付ける |
| `netopeer2-server.service` が無い | ビルド時に libsystemd-dev が無かった | 入れてから netopeer2 を再ビルドする。または [02](02_netopeer2_systemd.md) §1.2 の代わりの unit を使う |
| sysrepo の `sudo make install` が `Installing notifd sysrepo modules (setup_notifd.sh)...` の後で `[ERR] Loading "ietf-netconf-nmda@2019-01-07" module failed, not found.` を出して止まる。`sudo sysrepoctl -l` が Segmentation fault で落ちる（2026-10-14 に guest で発生） | sysrepo を Debug でビルドしていたので、リポジトリが `build/repository` にあり、`build/` を作り直したときに一緒に消えた。`/dev/shm/sr_*` には消える前のモジュールの一覧が残っていて（動いている netopeer2-server も使っている）、そこに載っている netopeer2 のモジュール（`ietf-netconf-nmda` など）の YANG ファイルがもう無いので、新しいコンテキストを作れない。エラーに出るモジュールは、`setup_notifd.sh` が入れようとしているものとは関係ない | §7.1 の手順で、Release（`/etc/sysrepo`）で作り直し、モジュールと設定を入れ直す。古いリポジトリは戻せない |
| `merge_config.sh` の結果が root 用になった | root シェルで `make install` した | `ietf-netconf-server` の設定を消し、`ubuntu` から `sudo make install` し直す |
