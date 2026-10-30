#!/bin/bash
set -euo pipefail

EK=0x81010001
AK=0x81010002
OUT=${1:-"$(cd "$(dirname "$0")" && pwd)/keys"}

# plugin（/etc/rats-attester.conf の tcti）と同じ、resource manager 経由のデバイスを使う。
# 指定しないと tpm2-tools が /dev/tpm0 を選ぶことがあり、/dev/tpm0 は tss:root 0660 なので一般ユーザーでは開けない
export TPM2TOOLS_TCTI=${TPM2TOOLS_TCTI:-device:/dev/tpmrm0}

dev=${TPM2TOOLS_TCTI#device:}
if [ "$dev" != "$TPM2TOOLS_TCTI" ] && { [ ! -r "$dev" ] || [ ! -w "$dev" ]; }; then
    echo "$(id -un) cannot read/write $dev." >&2
    echo "add the user to the tss group and log in again: sudo usermod -aG tss $(id -un)" >&2
    exit 1
fi

mkdir -p "$OUT"

# ak.ctx は一時ディレクトリに置き、終わったら消す
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# 先に一覧を取っておく（pipefail の下で tpm2_getcap | grep -q とすると、SIGPIPE で誤って「無い」と判定しうるため）
handles=$(tpm2_getcap handles-persistent)
is_persistent() { grep -qi -- "$1" <<< "$handles"; }

if ! is_persistent "$EK"; then
    tpm2_createek -c "$EK" -G ecc
fi

if ! is_persistent "$AK"; then
    tpm2_createak -C "$EK" -c "$TMP/ak.ctx" -G ecc -g sha256 -s ecdsa
    tpm2_evictcontrol -C o -c "$TMP/ak.ctx" "$AK"
fi

# 鍵が前からあった場合も同じファイルがそろうよう、公開情報は毎回 TPM から読み直して書き出す
tpm2_readpublic -c "$EK" -f tss -o "$OUT/ek.pub" > /dev/null
tpm2_readpublic -c "$EK" -f pem -o "$OUT/ek.pem" > /dev/null
tpm2_readpublic -c "$AK" -f tss -o "$OUT/ak.pub" > /dev/null
tpm2_readpublic -c "$AK" -f pem -o "$OUT/ak-cert-0.pem" -q "$OUT/ak-cert-0.qname" > /dev/null

echo "files written to $OUT"
