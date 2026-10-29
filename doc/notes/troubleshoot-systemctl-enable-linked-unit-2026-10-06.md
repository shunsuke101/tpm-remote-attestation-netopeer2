# systemctl enable netopeer2-server が「linked unit file」で拒否される

- 日付: 2026-10-06
- 分類: トラブルシュート（systemd）
- 関連: `doc/design/02_netopeer2_systemd.md`

## 質問

> QEMU 上で `systemctl enable netopeer2-server` を実行すると、次のエラーになる。
>
> ```
> Failed to enable unit: Refusing to operate on alias name or linked unit file: netopeer2-server.service
> ```

## 回答

### 原因: /etc/systemd/system に余分なシンボリックリンクがある

guest には、同じ名前の unit が 2 か所にあった。

```
/etc/systemd/system/netopeer2-server.service -> /usr/local/lib/systemd/system/netopeer2-server.service   ← 10/5 07:51 に作られたリンク
/usr/local/lib/systemd/system/netopeer2-server.service                                               ← make install が置いた実体
```

- `/usr/local/lib/systemd/system` は、もともと systemd が unit を探す場所に入っている（`systemd-analyze unit-paths` で確認）。
  そのため、`/etc/systemd/system` にリンクを置く必要は無い。
- 同じ名前の unit が、探索先の中の別ファイルへのリンクとして存在すると、systemd はこれを「リンクされた unit」とみなし、`enable` を拒否する。
- その結果、`systemctl show` の `UnitFileState` が `bad` になり、`systemctl is-enabled` も `Too many levels of symbolic links` になっていた。
- リンクはおそらく `systemctl link` か `ln -s` で作られたもの。

### 直し方（guest）

```bash
sudo rm /etc/systemd/system/netopeer2-server.service    # リンクだけを消す（実体は /usr/local/lib に残る）
sudo systemctl daemon-reload
sudo systemctl enable netopeer2-server
systemctl is-enabled netopeer2-server                    # enabled になること
```

`enable` に成功すると、`/etc/systemd/system/multi-user.target.wants/netopeer2-server.service` が実体を指すリンクとして作られる。これが正しい形。
netopeer2-server が動いている最中にリンクを消して daemon-reload しても、サービスは止まらない。

## その後

2026-10-07 に確認: リンクは無くなり、`netopeer2-server` は `enabled` / `active` になっている。
