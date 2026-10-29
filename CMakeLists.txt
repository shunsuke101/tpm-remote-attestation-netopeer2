cmake_minimum_required(VERSION 3.10...3.28)

project(tpm-remote-attestation-netopeer2 C)
set(PROJECT_DESC "RFC 9684 challenge-response remote attestation over NETCONF (netopeer2 + sysrepo + swtpm)")

# include and custom modules:
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/CMakeModules/")

include(GNUInstallDirs)

# default build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()
string(TOUPPER "${CMAKE_BUILD_TYPE}" BUILD_TYPE_UPPER)


if("${BUILD_TYPE_UPPER}" STREQUAL "RELEASE")
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build Type" FORCE)
    set(CMAKE_C_FLAGS "-DNDEBUG -O2 ${CMAKE_C_FLAGS}")
elseif("${BUILD_TYPE_UPPER}" STREQUAL "DEBUG")
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build Type" FORCE)
    set(CMAKE_C_FLAGS "-g -O0 ${CMAKE_C_FLAGS}")
endif()


set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -std=c99")

#
# options
#
option(BUILD_ATTESTER "Build and install the rats-attester Attester sysrepo-plugind plugin (guest side) needs libyang/sysrepo/tss2" ON)
option(BUILD_VERIFIER "Build and install the rats-verify Verifier command (host side) needs libexpat1-dev, libjson-c-dev, libtss2-dev, libssl-dev" ON)

#
# checks
#

# PKGCONFIG {

find_package(PkgConfig REQUIRED)
if(PkgConfig_FOUND)
	if(BUILD_VERIFIER)
    	# json-c (golden, state and result JSON)
   	 pkg_check_modules(JSONC REQUIRED IMPORTED_TARGET json-c)
    	# tss2-mu (unmarshal TPM2B_ATTEST / TPMS_ATTEST / TPMT_SIGNATURE)
    	pkg_check_modules(TSS2_MU REQUIRED IMPORTED_TARGET tss2-mu)
    	# expat (XML)
    	find_package(EXPAT REQUIRED)
	endif()
endif()

# } PKGCONFIG

#
# dependencies
#

# OpenSSL (libcrypto, used by both the attester and the verifier)
find_package(OpenSSL REQUIRED)

if(BUILD_ATTESTER)
    # pthread
    find_package(Threads REQUIRED)
    # libyang
    find_package(LibYANG REQUIRED)
    # sysrepo
    find_package(Sysrepo REQUIRED)
    # tpm2-tss: tss2-esys, tss2-mu, tss2-tctildr, tss2-rc (CMakeModules/FindTSS2.cmake)
    find_package(TSS2 REQUIRED)
endif()

#
# targets
#

# attester
if(BUILD_ATTESTER)
    add_subdirectory(attester)
endif()

# verifier
if(BUILD_VERIFIER)
    add_subdirectory(verifier)
endif()

# uninstall (uses install_manifest.txt in the build directory)
add_custom_target(uninstall "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_SOURCE_DIR}/CMakeModules/uninstall.cmake")
