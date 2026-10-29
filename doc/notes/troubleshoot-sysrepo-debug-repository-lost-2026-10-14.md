# sysrepo の make install が setup_notifd.sh で「ietf-netconf-nmda ... not found」になる

- 日付: 2026-10-14
- 分類: トラブルシュート（sysrepo、ビルド・インストール）
- 関連: `doc/design/01_build_netconf_stack.md` §4.1・§7.1・§8、`doc/notes/troubleshoot-sysrepo-running-removeipc-2026-10-06.md`、
  `doc/notes/sysrepo-datastore-storage-2026-10-07.md`

## 質問

> ssh で guest にログインして、`~/github/sysrepo/build$ sudo make install` でエラーが出る。
>
> ```
> -- Installing notifd sysrepo modules (setup_notifd.sh)...
> CMake Error at cmake_install.cmake:359 (message):
>    OUTPUT:
>
>    ERROR:
>    [ERR] Loading "ietf-netconf-nmda@2019-01-07" module failed, not found. (path "/")
>    sysrepoctl error: Failed to install modules (libyang error)
> ```

## 回答

### 原因: Debug ビルドの sysrepo の build/ を作り直したので、リポジトリが消えた

guest で確認した事実（2026-10-14、時刻は UTC）:

- guest の sysrepo は Debug でビルドされていた（`CMakeCache.txt` の `CMAKE_BUILD_TYPE=Debug`）。4 つのライブラリとも、
  `CMAKE_BUILD_TYPE` を指定しないと Debug になる（各 `CMakeLists.txt` の `if(NOT CMAKE_BUILD_TYPE) set(CMAKE_BUILD_TYPE Debug)`）。
- sysrepo は Debug のとき、リポジトリ（登録したモジュールの YANG、startup のデータ、権限）を `${CMAKE_BINARY_DIR}/repository`、
  つまり build ディレクトリの中に置く。Release などでは `/etc/sysrepo`（`sysrepo/CMakeLists.txt:214-219`）。
- `~/github/sysrepo/build` は 04:57 に作り直されていた（`stat` の birth）。新しい `build/repository/yang/` は空で、`/etc/sysrepo` も無かった。
  古いリポジトリはどこにも残っていなかった。
- 一方、`/dev/shm/sr_*` には消える前の状態が残っていて、03:47 から動いていた netopeer2-server もそれを使っていた。
- `sudo sysrepoctl -l` は Segmentation fault で落ちた。

`setup_notifd.sh` が `sysrepoctl -i` を実行すると、sysrepo は共有メモリにあるモジュールの一覧から新しい libyang のコンテキストを作ろうとする。
一覧には netopeer2 が入れた `ietf-netconf-nmda` などが載っているが、その YANG ファイルはリポジトリと一緒に消えているので、読み込みに失敗する。
エラーに出ている `ietf-netconf-nmda` は、`setup_notifd.sh` が入れるモジュール（`ietf-subscribed-notifications`、
`ietf-subscribed-notif-receivers`、`ietf-udp-notif-transport`）が import しているものではない。

この危険は `doc/notes/troubleshoot-ldconfig-libsysrepo-2026-10-05.md` に「build ディレクトリを消すとモジュールや設定も消える」と書いてあった。

### 消えたもの

- netopeer2 の `make install`（`setup.sh`、`merge_hostkey.sh`、`merge_config.sh`）で入ったモジュールと設定:
  830 番の待ち受け、ubuntu の公開鍵認証、SSH ホスト鍵 `genkey`
- ④ で入れたもの: `ietf-tpm-remote-attestation`、`ietf-tcg-algs`（と import only の `ietf-hardware`、`iana-hardware`）、`rats-init.xml`、`nacm-rats.xml`

影響が無かったもの: swtpm の中の EK と AK、guest の `~/tpm-remote-attestation-netopeer2`（`build/keys/` を含む）、⑤ で入れた
`rats-attester.so`・`/etc/rats-attester.conf`・drop-in（どれも sysrepo のリポジトリの外にある）。

### 直し方（実施した手順）

`doc/design/01_build_netconf_stack.md` §7.1 の手順を、スクリプト（guest の `~/recover-sysrepo.sh`、ログは `~/recover-sysrepo.log`）で実行した。

1. netopeer2-server を止め、`/dev/shm/sr_*` と `/dev/shm/srsub_*`（16 個）を消した。
2. sysrepo を `-DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=OFF -DNOTIFD_MODULE_OWNER=root -DNOTIFD_MODULE_GROUP=root` で作り直した。
   cmake の出力が `Sysrepo repository: /etc/sysrepo` になることを確かめてから、`make`、`sudo make install`、`sudo ldconfig`。
   `setup_notifd.sh` は今度は成功した（空のリポジトリに入れるので、共有メモリの古い一覧に引きずられない）。
3. Netopeer2 を `-DMODULES_OWNER=root -DMODULES_GROUP=root` で構成し直し、ubuntu から `sudo make install`。
   ライブラリとバイナリは作り直していない（sysrepo の SO version は同じで、リポジトリの場所は libsysrepo の中にだけある）。
4. `attester/scripts/install_yang.sh` と `install_config.sh` で ④ を入れ直した。
5. netopeer2-server を起動した。

全体で約 4 分（05:13〜05:17）。

### 確認したこと

guest:

- `/etc/sysrepo` がリポジトリになり、`~/github/sysrepo/build` の中に `repository` は無い。
- `ietf-tcg-algs`（`I`、feature `tpm20`）、`ietf-tpm-remote-attestation`（`I`）、`ietf-hardware`・`iana-hardware`（`i`）が入っている。
- モジュールはすべて `root:root`（52 個）。`sudo sysrepocfg -X -d running -m ietf-netconf-server` が通る（2026-10-06 の RemoveIPC の件も解消）。
- `ietf-tpm-remote-attestation` と `ietf-netconf-acm` は running と startup が一致する。netopeer2-server は 0.0.0.0:830 で待ち受けている。

host（04 §8 と同じ内容を、ssh の netconf サブシステムで送った）:

- NETCONF のホスト鍵は新しくなった（`SHA256:ZB04bp8fEZfRpnXs2uCWo4MqtVlHYv1jLMJgoEvNOS4`。古い鍵は `SHA256:d5cP8+50pe6Hya5SL6POVGSKUtjJJdMySqmSjpbmGjs`）。
  - 新しい鍵は、host で表示されたものをそのまま受け入れるのではなく、guest の keystore の秘密鍵から guest の中で公開鍵を計算し（公開鍵だけを host に持ってくる）、
    8300 番が提示する鍵と一致することを確かめてから `~/.ssh/known_hosts` に登録した（`ssh-keygen -R "[localhost]:8300"` の後に追加。元は `known_hosts.old`）。
- ietf-yang-library に `ietf-tcg-algs`（feature `tpm20`）と `ietf-tpm-remote-attestation` がある。
- `get-config --source running` で `rats-support-structures` が `rats-init.xml` どおりに返る。
- `get` で `/ietf-netconf-acm:nacm` が空で返る（ubuntu の NETCONF セッションに NACM が効いている）。
- `get-schema` の結果が `attester/yang/ietf-tpm-remote-attestation@2024-12-05.yang` とバイト単位で一致する。
- `get` が通るようになったので、③（`get` が失敗する）も解消した。

### 気づいたこと

- sysrepo は、`sysrepoctl -i` で入れたモジュールをそのままコピーせず、libyang で表示し直して `/etc/sysrepo/yang/` に保存する
  （`sysrepo/src/ly_wrap.c:382` の `lys_print_module`）。コメントや文字列の `+` 連結が無くなるので、元のファイルとはバイト単位では一致しない。
- それでも `get-schema` が元のファイルと一致したのは、今の共有メモリのコンテキストが、入れたときのファイル
  （guest の `~/tpm-remote-attestation-netopeer2/attester/yang/...`）を指したままだから（yang-library の `location` もそのパスになっている）。
  guest を再起動するとコンテキストは `/etc/sysrepo/yang/` から作り直されるので、そのあとは表示し直した版が返る可能性がある。定義の中身は同じ。

## 再発を防ぐ

- sysrepo は必ず Release でビルドする。cmake の出力の `Sysrepo repository:` の行で、`/etc/sysrepo` になっていることを確かめる（01 §4.1）。
- guest の libyang、libnetconf2、Netopeer2 は Debug のまま。これらはデータを build ディレクトリに置かないので、`build/` を作り直してもモジュールや設定は消えない。
