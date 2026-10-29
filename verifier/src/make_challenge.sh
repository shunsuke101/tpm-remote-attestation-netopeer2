#!/bin/bash
# host で実行する。nonce を CSPRNG で生成し、templates/challenge.xml.tmpl から challenge.xml を作る
# （doc/design/05_challenge_response.md §4.2）。
# なんか微妙かも!!!!!!!!!!!!!!!
# 使い方: build/verifier/make_challenge.sh [OUT_DIR]
#   cmake の構成時に、templates/ と一緒に build/verifier/ にコピーされる（verifier/CMakeLists.txt）。
#   ソースの verifier/src/ からは実行しない（隣に templates/ が無い）。
#   OUT_DIR（既定: このスクリプトと同じディレクトリの out/、つまり build/verifier/out/）に nonce.bin と challenge.xml を書く。
#   その後 build/verifier/ で netopeer2-cli を起動して:
#     > connect --host localhost --login ubuntu -p 8300
#     > user-rpc --content out/challenge.xml --out out/reply.xml
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
TMPL="$HERE/templates/challenge.xml.tmpl"
OUT=${1:-"$HERE/out"}

if [ ! -r "$TMPL" ]; then
    echo "$TMPL not found." >&2
    echo "run the copy in the build tree (build/verifier/make_challenge.sh), which cmake creates next to templates/." >&2
    exit 1
fi
mkdir -p "$OUT"

openssl rand 32 > "$OUT/nonce.bin"
sed "s|@NONCE_B64@|$(base64 -w0 "$OUT/nonce.bin")|" "$TMPL" > "$OUT/challenge.xml"
echo "wrote $OUT/nonce.bin ($(stat -c %s "$OUT/nonce.bin") bytes) and $OUT/challenge.xml"
