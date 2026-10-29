# Find TSS2 (tpm2-tss)
#  Thin wrapper around pkg-config for the tpm2-tss libraries used by rats-attester:
#    tss2-esys     ESAPI (Esys_Quote, Esys_PCR_Read, ...)
#    tss2-mu       marshalling (Tss2_MU_TPM2B_ATTEST_Marshal, ...)
#    tss2-tctildr  TCTI loader (Tss2_TctiLdr_Initialize)
#    tss2-rc       error decoding (Tss2_RC_Decode)
#
#  Once done, it will define
#
#  TSS2_FOUND - System has the four tpm2-tss libraries
#  TSS2_INCLUDE_DIRS - The tpm2-tss include directories
#  TSS2_LIBRARIES - The libraries needed to use tpm2-tss
#  TSS2_VERSION - Version of tss2-esys

find_package(PkgConfig REQUIRED)
pkg_check_modules(PC_TSS2 QUIET tss2-esys tss2-mu tss2-tctildr tss2-rc)

set(TSS2_INCLUDE_DIRS ${PC_TSS2_INCLUDE_DIRS})
set(TSS2_LIBRARIES ${PC_TSS2_LINK_LIBRARIES})
set(TSS2_VERSION ${PC_TSS2_tss2-esys_VERSION})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(TSS2
    REQUIRED_VARS TSS2_LIBRARIES
    VERSION_VAR TSS2_VERSION)

mark_as_advanced(TSS2_INCLUDE_DIRS TSS2_LIBRARIES)
