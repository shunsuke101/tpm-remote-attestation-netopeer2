# netopeer2 の make install が libsysrepo.so.8 を読み込めずに止まる

- 日付: 2026-10-05
- 分類: トラブルシュート（ビルド・インストール）
- 関連: `doc/design/01_build_netconf_stack.md` §8

## 質問

> host の `~/github/Netopeer2/build` で `cmake ..`、`make`、`sudo make install` を実行したら、次のエラーで止まった。
>
> ```
> -- Installing missing sysrepo modules (setup.sh)...
> /usr/local/bin/sysrepoctl: error while loading shared libraries: libsysrepo.so.8: cannot open shared object file: No such file or directory
> ```
>
> `/usr/local/lib/libsysrepo.so.8` はあるが root:root になっている。permission denied かな？

## 回答

### 原因は権限ではなく、ldconfig をしていないこと

- `libsysrepo.so.8` が root:root で 644 なのは正常で、どのユーザーからも読める。`sudo make install` は root で動くので、そもそも権限は問題にならない。
- エラーが「Permission denied」ではなく「No such file or directory」になっているのもそのため。
- 実際には、動的リンカのキャッシュにライブラリが載っていなかった。
  - `ldconfig -p` に libsysrepo、libyang、libnetconf2 のどれも出てこない。
  - `ldd /usr/local/bin/sysrepoctl` で `libsysrepo.so.8 => not found`、`libyang.so.5 => not found` になる。
- `/usr/local/lib` は `/etc/ld.so.conf.d/libc.conf` に入っているので、`sudo ldconfig` を一度実行すれば読み込めるようになる。
- インストール時に rpath が外される（ログの「Set non-toolchain portion of runtime path ... to ""」）ので、実行時にはキャッシュに頼ることになる。

### 直し方（host）

設計では host には CLI だけを入れる。ところが、このときの CMakeCache は `BUILD_SERVER=ON`、`SYSREPO_SETUP=ON` だった。
そのまま `make install` をやり直すと、host の sysrepo に netopeer2 のモジュールやホスト鍵まで登録されるので、設定し直してから入れる。

```bash
sudo ldconfig
cd ~/github/Netopeer2/build
cmake -DBUILD_SERVER=OFF -DSYSREPO_SETUP=OFF ..
make
sudo make install          # /usr/local/bin/netopeer2-cli が入る
```

途中まで入った `/usr/local/sbin/netopeer2-server`、`/usr/local/lib/systemd/system/netopeer2-server.service`、
`/usr/local/etc/pam.d/netopeer2.conf` は、有効化していないので残っていても害はない。

### guest でも同じことが起きていた

- guest でも `ldconfig -p` に該当ライブラリが無く、`sysrepoctl` が `libsysrepo.so.8` を読み込めない状態だった。
- guest に `netopeer2-cli` が無かったのも、同じ箇所で `make install` が止まったためと考えられる（ログでも、CLI がインストールされる前に止まっている）。
- guest で確認したこと:
  - `libsystemd-dev` が入っていないので、`netopeer2-server.service` が生成されない。入れてから `cmake ..` をやり直す。
  - sysrepo が Debug ビルドで、リポジトリが `/home/ubuntu/github/sysrepo/build/repository` にある。build ディレクトリを消すとモジュールや設定も消える。

### 分かったこと

netopeer2 の systemd の unit は、`/usr/lib/systemd/system` ではなく `/usr/local/lib/systemd/system` にインストールされる
（pkg-config があると、インストール先のプレフィックスから決まる）。systemd はこのディレクトリも読む。設計書 01 と 02 を修正済み。

## その後

- host: `netopeer2-cli` が `/usr/local/bin` に入った（2026-10-06）。
- guest: `netopeer2-cli` と `netopeer2-server.service` が入った（2026-10-07 に確認）。
