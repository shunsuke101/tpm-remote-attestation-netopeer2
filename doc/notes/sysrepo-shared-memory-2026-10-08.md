# sysrepo の共有メモリはどこにあるのか

- 日付: 2026-10-08
- 分類: sysrepo の仕組み
- 関連: `doc/notes/sysrepo-datastore-storage-2026-10-07.md`、`doc/notes/troubleshoot-sysrepo-running-removeipc-2026-10-06.md`

## 質問

> sysrepo の shared memory は `/dev/shm` なのかな。その中に running datastore と operational datastore があるから？
> それとも他のところに shared memory ってあるの？

## 回答

sysrepo の共有メモリは `/dev/shm` にあり、他の場所には無い。ただし、「running と operational があるから共有メモリ」という順序は逆。

### /dev/shm とは

Linux の `/dev/shm` は、メモリ上のファイルシステム（tmpfs）。プロセス間の共有メモリはここにファイルとして作られ、再起動すると消える。
sysrepo はこのディレクトリを共有メモリの置き場所に使っていて、`sysrepo/src/config.h.in` で次のように決まっている。

- 場所: `SR_SHM_PATH`（ビルド時の `SHM_DIR`、既定値 `/dev/shm`）。環境変数 `SYSREPO_SHM_DIR` で変更できる。
- ファイル名の接頭辞: 既定値 `sr`。環境変数 `SYSREPO_SHM_PREFIX` で変更できる。

sysrepo は System V の共有メモリ（`shmget`）は使っていない。

### /dev/shm の中身は 3 種類

running などのデータストアは、その一部にすぎない。

| 種類 | ファイル | 役割 | 使い方 |
|---|---|---|---|
| ① 管理用の共有メモリ | `sr_main` | プロセス間で共有するロック、接続・セッション・購読の ID の払い出し、リポジトリのパス（`shm_types.h:198-219`） | 全プロセスが `mmap(MAP_SHARED)` で同じメモリを見る（`common.c:1534`） |
| | `sr_mod` | 導入済みのモジュールの一覧、依存関係、モジュールとデータストアごとのロック、push 型 operational のセッションの一覧 | 同上 |
| | `sr_ext` | 購読の情報（どのプロセスが、どのモジュールの変更・状態データ・RPC を購読しているか） | 同上 |
| | `sr_ctx` | コンパイル済みの YANG スキーマ（libyang の context）。各プロセスがスキーマを一からコンパイルしないで済むようにする | 同上 |
| ② 購読のための受け渡し領域 | `srsub_<mod>.<種類>[.<ハッシュ>]`、`srsub_data_<mod>.…` | 要求する側と購読する側の間でイベントとデータを渡す。例えば netopeer2 から rats-attesterd へ RPC を渡すのに使う | `mmap` |
| ③ 消えてよいデータストア | `sr_<mod>.running`、`sr_<mod>.candidate`、`sr_<mod>.operational.<cid>-<sid>` | 設定と状態データの中身（JSON） | 普通のファイルとして読み書きする（tmpfs 上なので速い） |

ファイル名は `common.c:1096-1140`（`sr_main`、`sr_mod`、`sr_ctx`、`sr_ext`）と `common.c:1148-1180`（`srsub_*`）で決まっている。

sysrepo を使う全プロセス（netopeer2-server、sysrepocfg、将来の rats-attesterd）が、①②を共有して協調している。
sysrepo には常駐するデーモンが無く、これらの共有メモリそのものが、プロセスの間の「まとめ役」になっている。

③が `/dev/shm` にあるのは、running と candidate が「再起動したら startup から作り直す、消えてよいデータ」だから。
sysrepo は、メモリ上で速く、再起動で自然に消える場所として、データストアもここに置いている。

### /dev/shm 以外にあるもの

| もの | 場所（この guest の場合） |
|---|---|
| startup、factory-default、権限の情報（`.perm`）、YANG ファイル、通知の履歴 | ディスク上のリポジトリ `/home/ubuntu/github/sysrepo/build/repository/`（`data/`、`yang/` など） |
| イベントパイプ（購読しているプロセスを起こすための FIFO） | 同じリポジトリの `sr_evpipe<番号>`（`common.c:1190`） |
| 各プロセスのキャッシュ（running のキャッシュなど） | そのプロセス自身のメモリ。共有はしない |

### これまでの問題との関係

- **RemoveIPC の件**（`doc/notes/troubleshoot-sysrepo-running-removeipc-2026-10-06.md`）: logind が消したのは、`/dev/shm` にある ubuntu 所有のファイル。
  消えたのは③のデータストアのファイルだけで、①②は root 所有だったので残っていた。
- **`/dev/shm` を作り直す対処**: `sr_*` と `srsub_*` を消すと、①②③がすべて作り直される。running は、ディスク上の startup からコピーし直される。
  設定を startup にも入れておく必要があったのは、このため。
