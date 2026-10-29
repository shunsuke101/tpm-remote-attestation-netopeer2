#!/bin/bash
# guest で root として実行する。rats-support-structures の初期設定と NACM ルールを sysrepo に入れる
# （doc/design/04_rfc9684_yang.md §5、§7.3）。
#
# 使い方: sudo install_config.sh [CONFIG_DIR]
#   CONFIG_DIR の既定は、このスクリプトから見た ../config（リポジトリ内で実行する場合）。
#
# - rats-init.xml は --import（モジュールの設定を置き換える）で running に入れ、startup にコピーする
# - nacm-rats.xml は、既存の rule-list を消さないよう --edit（merge）で入れる。rule-list は ordered-by user なので
#   既存のリストの後ろに付く。既存のリストに広い permit があるとそちらが先に一致するので、投入前の -X の出力を見ておく
# sysrepocfg は NETCONF を通らないので NACM の対象外。root で実行する。
set -euo pipefail

CONFIG_DIR=${1:-"$(cd "$(dirname "$0")/../config" 2>/dev/null && pwd || true)"}
if [ -z "$CONFIG_DIR" ] || [ ! -f "$CONFIG_DIR/rats-init.xml" ] || [ ! -f "$CONFIG_DIR/nacm-rats.xml" ]; then
    echo "usage: $0 <dir containing rats-init.xml and nacm-rats.xml>" >&2
    exit 2
fi

echo "== rats-support-structures (ietf-tpm-remote-attestation) =="
sysrepocfg --import="$CONFIG_DIR/rats-init.xml" -d running -m ietf-tpm-remote-attestation
sysrepocfg -C running -d startup -m ietf-tpm-remote-attestation
sysrepocfg -X -d startup -m ietf-tpm-remote-attestation

echo "== NACM (ietf-netconf-acm): before =="
sysrepocfg -X -d running -m ietf-netconf-acm
sysrepocfg --edit="$CONFIG_DIR/nacm-rats.xml" -d running -m ietf-netconf-acm
sysrepocfg -C running -d startup -m ietf-netconf-acm
echo "== NACM (ietf-netconf-acm): after =="
sysrepocfg -X -d running -m ietf-netconf-acm
