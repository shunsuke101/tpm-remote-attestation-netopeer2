#!/bin/bash
# host で実行する。doc/design/04_rfc9684_yang.md §3 の (1)〜(4)（reply-sample.xml があれば (5) も）を yanglint で行う。
#
# 使い方: tests/check_yang.sh
#   環境変数 LNC2_MODULES: libnetconf2 の modules ディレクトリ（ietf-keystore などの参照先）。
#   既定は ~/github/libnetconf2/modules。
#
# yanglint は -F を 1 つでも指定すると、指定しなかったモジュールの feature をすべて無効にする。
# guest と同じ状態にするため、ietf-keystore の feature も指定する。
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
YANG="$ROOT/attester/yang"
CONF="$ROOT/attester/config"
SAMPLES="$ROOT/tests/samples"
LNC2_MODULES=${LNC2_MODULES:-"$HOME/github/libnetconf2/modules"}

TRA="$YANG/ietf-tpm-remote-attestation@2024-12-05.yang"
TCG="$YANG/ietf-tcg-algs@2024-12-05.yang"
YL=(yanglint -p "$YANG" -p "$LNC2_MODULES"
    -F ietf-tcg-algs:tpm20
    -F ietf-keystore:central-keystore-supported,asymmetric-keys)

echo "(1) schema";            "${YL[@]}" "$TCG" "$TRA"
echo "(2) tree (to /dev/null)"; "${YL[@]}" -f tree "$TRA" > /dev/null
echo "(3) config rats-init.xml"; "${YL[@]}" -t config "$TRA" "$CONF/rats-init.xml"
echo "(4) rpc challenge-sample.xml"; "${YL[@]}" -t rpc -O "$CONF/rats-init.xml" "$TRA" "$SAMPLES/challenge-sample.xml"
if [ -f "$SAMPLES/reply-sample.xml" ]; then
    echo "(5) reply reply-sample.xml"; "${YL[@]}" -t reply -O "$CONF/rats-init.xml" "$TRA" "$SAMPLES/reply-sample.xml"
else
    echo "(5) skipped: $SAMPLES/reply-sample.xml not yet created (made from a real reply in step 5)"
fi
echo "OK"
