# $Id: $
#
# - For MSVC builds try to find the MS DirectX SDK, for MinGW just the
# MinGW dsound library
#
# Once done this will define
#
#  DXSDK_FOUND - system has DirectX SDK
#  DXSDK_ROOT_DIR - path to the DirectX SDK base directory
#  DXSDK_INCLUDE_DIR - the DirectX SDK include directory
#  DXSDK_LIBRARY_DIR - DirectX SDK libraries path
#
#  DXSDK_DSOUND_LIBRARY - Path to dsound.lib
#
# MinGW builds have to use dsound provided by MinGW, so we need to avoid finding
# the actual MS-DX-SDK in case it is installed on a build system. With MinGW,
# "DXSDK" boils down to just another library and headers in default locations.
# There might be old MinGW distributions without dsound though, so it is good to
# verify its availability.


if(WIN32)
else(WIN32)
  message(FATAL_ERROR "FindDXSDK.cmake: Unsupported platform ${CMAKE_SYSTEM_NAME}" )
endif(WIN32)

if(MSVC)

  find_path(DXSDK_ROOT_DIR
    include/dxsdkver.h
    HINTS
      $ENV{DXSDK_DIR}
  )

  find_path(DXSDK_INCLUDE_DIR
    dxsdkver.h
    PATHS
      ${DXSDK_ROOT_DIR}/include
  )

  IF(CMAKE_CL_64)
    find_path(DXSDK_LIBRARY_DIR
    dsound.lib
    PATHS
      ${DXSDK_ROOT_DIR}/lib/x64
  )
  ELSE(CMAKE_CL_64)
    find_path(DXSDK_LIBRARY_DIR
    dsound.lib
    PATHS
      ${DXSDK_ROOT_DIR}/lib/x86
  )
  ENDIF(CMAKE_CL_64)

  find_library(DXSDK_DSOUND_LIBRARY
    dsound.lib
    PATHS
      ${DXSDK_LIBRARY_DIR}
  )

  # handle the QUIETLY and REQUIRED arguments and set DXSDK_FOUND to TRUE if
  # all listed variables are TRUE
  INCLUDE(FindPackageHandleStandardArgs)
  FIND_PACKAGE_HANDLE_STANDARD_ARGS(DXSDK DEFAULT_MSG DXSDK_ROOT_DIR DXSDK_INCLUDE_DIR)

ELSEIF(MINGW)

  GET_FILENAME_COMPONENT(MINGW_BIN_DIR ${CMAKE_C_COMPILER} DIRECTORY)
  GET_FILENAME_COMPONENT(MINGW_SYSROOT ${MINGW_BIN_DIR} DIRECTORY)
  # The glob expression below should only return a single folder:
  FILE(GLOB MINGW_TOOLCHAIN_FOLDER ${MINGW_SYSROOT}/*mingw32)
  
  find_library(DXSDK_DSOUND_LIBRARY
    libdsound.a dsound
    HINTS
      "${MINGW_TOOLCHAIN_FOLDER}/lib"
      "${MINGW_SYSROOT}/lib"
  )

  INCLUDE(FindPackageHandleStandardArgs)
  FIND_PACKAGE_HANDLE_STANDARD_ARGS(DXSDK DEFAULT_MSG DXSDK_DSOUND_LIBRARY)

ENDIF(MSVC)

MARK_AS_ADVANCED(
    DXSDK_ROOT_DIR DXSDK_INCLUDE_DIR
    DXSDK_LIBRARY_DIR DXSDK_DSOUND_LIBRARY
)
