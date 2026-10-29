#!/bin/bash
# guest で root として実行する。attester/yang/ の YANG を sysrepo に登録する（doc/design/04_rfc9684_yang.md §4）。
#
# 使い方: sudo install_yang.sh [YANG_DIR]
#   YANG_DIR の既定は、このスクリプトから見た ../yang（リポジトリ内で実行する場合）。
#   /usr/local/sbin にインストールした後は引数で指定する。
#
# - ietf-tcg-algs は implemented にして feature tpm20 を有効にする（-e は直前の -i にだけ効く）
# - ietf-tpm-remote-attestation は implemented（feature は有効にしない）
# - ietf-hardware と iana-hardware は -s の検索先から import only として自動で入る
# 登録済みのモジュールは飛ばすので、何度実行してもよい。
set -euo pipefail

YANG_DIR=${1:-"$(cd "$(dirname "$0")/../yang" 2>/dev/null && pwd || true)"}
if [ -z "$YANG_DIR" ] || [ ! -f "$YANG_DIR/ietf-tpm-remote-attestation@2024-12-05.yang" ]; then
    echo "usage: $0 <dir containing ietf-tcg-algs@2024-12-05.yang and ietf-tpm-remote-attestation@2024-12-05.yang>" >&2
    exit 2
fi

installed() { sysrepoctl -l | grep -q "^$1 "; }

cd "$YANG_DIR"
if installed ietf-tcg-algs; then
    echo "ietf-tcg-algs: already installed"
else
    sysrepoctl -s . -i ietf-tcg-algs@2024-12-05.yang -e tpm20
fi
if installed ietf-tpm-remote-attestation; then
    echo "ietf-tpm-remote-attestation: already installed"
else
    sysrepoctl -s . -i ietf-tpm-remote-attestation@2024-12-05.yang
fi

# 期待: ietf-tcg-algs と ietf-tpm-remote-attestation が I（implemented）、ietf-tcg-algs の feature に tpm20、
#       ietf-hardware と iana-hardware が i（import only）
sysrepoctl -l | grep -E 'hardware|tcg-algs|tpm-remote|keystore'
echo "modules changed: restart the daemons -> systemctl restart netopeer2-server sysrepo-plugind"
