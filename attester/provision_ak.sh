#!/bin/bash
# guest で ubuntu ユーザーとして実行する（sudo は不要）。EK と AK を永続化し、Verifier に渡す公開情報を書き出す。
# doc/design/05_challenge_response.md §2.2。何度実行しても同じ状態になる。
#
# 使い方: ./provision_ak.sh [OUT_DIR]
#   cmake の構成時に build/ にコピーされる（attester/CMakeLists.txt）。
#   OUT_DIR の既定は、このスクリプトと同じディレクトリの keys/（build から実行すれば build/keys/）。
#
#   EK: endorsement 階層、RSA2048、0x81010001
#   AK: EK の子、ECC NIST P-256 / ECDSA / SHA-256、restricted|sign|fixedtpm、0x81010002
#   鍵の本体（秘密鍵）は TPM の中にあり、外には出ない。書き出すのは公開部分と name だけ。
#
# 出力（OUT_DIR）:
#   ek.pub           EK の公開部分（TPM2B_PUBLIC。tpm2-tools の tss 形式）
#   ek.pem           EK の公開鍵（SubjectPublicKeyInfo の PEM）
#   ek.name          EK の name
#   ak.pub           AK の公開部分（TPM2B_PUBLIC）
#   ak.pem    AK の公開鍵（PEM）         
#                 ├ Verifier に登録する（§2.3）
#   ak-cert-0.qname  AK の qualified name
#
# 前提: /dev/tpmrm0 を読み書きできること。Ubuntu の udev 規則では tss:tss 0660 なので、実行するユーザーを tss グループに入れる。
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

# 先に一覧を取っておく（pipefail の下で tpm2_getcap | grep -q とすると、SIGPIPE で誤って「無い」と判定しうるため）
handles=$(tpm2_getcap handles-persistent)
is_persistent() { grep -qi -- "$1" <<< "$handles"; }

if ! is_persistent "$EK"; then
    tpm2_createek -c "$EK" -G ecc -u "$OUT/ek.pub"
fi

if ! is_persistent "$AK"; then
    tpm2_createak -C "$EK" -c "$OUT/ak.ctx" -G ecc -g sha256 -s ecdsa -u "$OUT/ak.pub" -q "$OUT/ak.qname"
    tpm2_evictcontrol -C o -c "$OUT/ak.ctx" "$AK"
fi

# 鍵が前からあった場合も同じファイルがそろうよう、公開情報は毎回 TPM から読み直して書き出す
tpm2_readpublic -c "$EK" -f pem -o "$OUT/ek.pem" > /dev/null
tpm2_readpublic -c "$AK" -f pem -o "$OUT/ak.pem" > /dev/null

hex() { xxd -p -c 256 "$1" 2>/dev/null || od -An -tx1 "$1" | tr -d ' \n'; }
echo "EK $EK name: $(hex "$OUT/ek.name")"
echo "AK $AK name: $(hex "$OUT/ak-cert-0.name")"
echo "files written to $OUT"
