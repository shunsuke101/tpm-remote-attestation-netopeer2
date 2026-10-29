# rats-attester: sysrepo-plugind plugin implementing the RFC 9684 RPCs for a TPM 2.0 (swtpm) attester (guest side).
# design: doc/design/05_challenge_response.md §3 (build and install: §3.8)

if(NOT PROJECT_DESC)
    message(FATAL_ERROR "Please use the root CMakeLists file instead.")
endif()

set(RATS_ATTESTER_SRC
    src/plugin.c
    src/conf.c
    src/tpm.c
    src/rats-support-structures.c
    src/tpm20-challenge-response-attestation.c
    src/tpm12-challenge-response-attestation.c
    src/log-retrieval.c)
set(RATS_ATTESTER_WORK_DIR "${CMAKE_BINARY_DIR}/attester")

add_library(rats-attester MODULE ${RATS_ATTESTER_SRC})
# sysrepo-plugind takes the plugin name from the file name: rats-attester.so -> "rats-attester"
# it is built at the top of the build tree (build/rats-attester.so), next to rats-verify and provision_ak.sh
set_target_properties(rats-attester PROPERTIES PREFIX "" LIBRARY_OUTPUT_DIRECTORY "${RATS_ATTESTER_WORK_DIR}")
# reuse the libraries found in the root CMakeLists.txt
target_include_directories(rats-attester PRIVATE ${LIBYANG_INCLUDE_DIRS} ${SYSREPO_INCLUDE_DIRS} ${TSS2_INCLUDE_DIRS})
target_link_libraries(rats-attester ${LIBYANG_LIBRARIES} ${SYSREPO_LIBRARIES} ${TSS2_LIBRARIES} OpenSSL::Crypto Threads::Threads)

# compiled-in default of the configuration file (the environment variable RATS_ATTESTER_CONF overrides it at run time)
set(RATS_ATTESTER_CONF "/etc/rats-attester.conf" CACHE STRING "Path of the rats-attester configuration file")
target_compile_definitions(rats-attester PRIVATE _GNU_SOURCE RATS_ATTESTER_CONF="${RATS_ATTESTER_CONF}")

# provision_ak.sh is run by the ubuntu user without sudo (it only needs /dev/tpmrm0, group tss), so it is not installed
# into the system: cmake copies it (with its permissions) to the top of the build tree, and make copies it again when it
# changes. It writes the EK/AK public parts to build/keys/
configure_file(provision_ak.sh "${RATS_ATTESTER_WORK_DIR}/provision_ak.sh" COPYONLY)

#
# install locations
#
# sysrepo exports the sysrepo-plugind plugin directory in its pkg-config file (SRPD_PLUGINS_PATH);
# on the guest this is /usr/local/lib/sysrepo-plugind/plugins/
if(NOT SRPD_PLUGINS_PATH)
    pkg_get_variable(_srpd_plugins_path sysrepo SRPD_PLUGINS_PATH)
    if(NOT _srpd_plugins_path)
        set(_srpd_plugins_path "${CMAKE_INSTALL_FULL_LIBDIR}/sysrepo-plugind/plugins/")
    endif()
    set(SRPD_PLUGINS_PATH "${_srpd_plugins_path}" CACHE PATH "Directory sysrepo-plugind loads plugins from")
endif()
set(SYSTEMD_DROPIN_DIR "/etc/systemd/system/sysrepo-plugind.service.d" CACHE PATH "Drop-in directory of sysrepo-plugind.service")
set(YANG_MODULE_DIR "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/yang/modules/tpm-remote-attestation-netopeer2" CACHE STRING "Directory where to copy the YANG modules to")

message(STATUS "rats-attester plugin dir:  ${SRPD_PLUGINS_PATH}")
message(STATUS "rats-attester conf:        ${RATS_ATTESTER_CONF}")
message(STATUS "sysrepo-plugind drop-in:   ${SYSTEMD_DROPIN_DIR}")
message(STATUS "YANG modules dir:          ${YANG_MODULE_DIR}")
message(STATUS "provision_ak.sh:           ${RATS_ATTESTER_WORK_DIR}/provision_ak.sh (keys -> ${RATS_ATTESTER_WORK_DIR}/keys)")

get_filename_component(RATS_ATTESTER_CONF_DIR "${RATS_ATTESTER_CONF}" DIRECTORY)
get_filename_component(RATS_ATTESTER_CONF_NAME "${RATS_ATTESTER_CONF}" NAME)

install(TARGETS rats-attester DESTINATION ${SRPD_PLUGINS_PATH})
install(FILES rats-attester.conf DESTINATION ${RATS_ATTESTER_CONF_DIR} RENAME ${RATS_ATTESTER_CONF_NAME})
install(PROGRAMS scripts/install_yang.sh scripts/install_config.sh DESTINATION ${CMAKE_INSTALL_SBINDIR})
install(FILES systemd/sysrepo-plugind.service.d/10-rats-attester.conf DESTINATION ${SYSTEMD_DROPIN_DIR})
install(DIRECTORY yang/ DESTINATION ${YANG_MODULE_DIR} FILES_MATCHING PATTERN "*.yang")
