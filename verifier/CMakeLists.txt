# rats-verify: tpm20-challenge-response-attestation の応答を検証する Verifier（host 側）。
# design: doc/design/05_challenge_response.md §5.2（検査 1〜8）、§5.3（--dump）
#
# ルートの CMakeLists.txt から add_subdirectory(verifier) で入る。
# 実行ファイルは build ディレクトリの直下、作業ディレクトリ（enroll/ など）は <build>/verifier/ に置く。
# 作業ディレクトリから ../rats-verify で呼べる。install はしない（host で build/ から実行する）。

if(NOT PROJECT_DESC)
    message(FATAL_ERROR "Please use the root CMakeLists file instead.")
endif()

set(RATS_VERIFY_WORK_DIR "${CMAKE_BINARY_DIR}/verifier")
add_executable(rats-verify src/rats-verify.c)
target_compile_definitions(rats-verify PRIVATE _GNU_SOURCE)
# ルートの CMakeLists.txt で見つけたライブラリを使う
target_link_libraries(rats-verify PRIVATE EXPAT::EXPAT PkgConfig::JSONC PkgConfig::TSS2_MU OpenSSL::Crypto)
set_target_properties(rats-verify PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${RATS_VERIFY_WORK_DIR}")

# 作業ディレクトリ <build>/verifier/ を構成時に作る（§5.1）。
#   make_challenge.sh  ソースからコピーする（§4.2）。隣の templates/ を読み、out/ に書く
#   templates/         challenge のテンプレート。ソースからコピーする
#   enroll/            AK の公開情報（provision_ak.sh の出力を登録する。§2.3）
#   ref-values/        golden（§2.5）
#   state/             clockInfo の履歴（--state）
#   out/               nonce.bin、challenge.xml、reply.xml、--dump の出力
# コピーしたファイルは、ソースを変えると make でコピーし直される。enroll/ などの中身は build ディレクトリにしか無いので、
# build ディレクトリを消すと一緒に消える。
foreach(_dir enroll ref-values templates state out)
    file(MAKE_DIRECTORY "${RATS_VERIFY_WORK_DIR}/${_dir}")
endforeach()
configure_file(src/make_challenge.sh "${RATS_VERIFY_WORK_DIR}/make_challenge.sh" COPYONLY)
configure_file(templates/challenge.xml.tmpl "${RATS_VERIFY_WORK_DIR}/templates/challenge.xml.tmpl" COPYONLY)

message(STATUS "rats-verify        ${RATS_VERIFY_WORK_DIR}/rats-verify (expat ${EXPAT_VERSION_STRING}, json-c ${JSONC_VERSION}, tss2-mu ${TSS2_MU_VERSION})")
message(STATUS "rats-verify work dir:      ${RATS_VERIFY_WORK_DIR} (make_challenge.sh templates/ enroll/ ref-values/ state/ out/)")
