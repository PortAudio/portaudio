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

# Dsound.lib is statically linked (i.e. dsound.dll not required) and DXSDK_LIBRARY_DIR not used.
# In the environments supported by VCPKG we may as well avoid looking out for DX9 to avoid version
# mismatch in find.

if(MSVC AND MSVC_VERSION GREATER_EQUAL 1900)

  # if the environment is set up properly, matching lib and header will be found

  find_path(DXSDK_INCLUDE_DIR
    dsound.h
  )
  find_library(DXSDK_DSOUND_LIBRARY
    dsound.lib
  )

  INCLUDE(FindPackageHandleStandardArgs)
  FIND_PACKAGE_HANDLE_STANDARD_ARGS(DXSDK DEFAULT_MSG DXSDK_INCLUDE_DIR DXSDK_DSOUND_LIBRARY)

  MARK_AS_ADVANCED(
    DXSDK_INCLUDE_DIR DXSDK_DSOUND_LIBRARY
  )

else()

  find_path(DXSDK_ROOT_DIR
    include/dxsdkver.h
    HINTS
      $ENV{DXSDK_DIR}
  )

  find_path(DXSDK_INCLUDE_DIR
    dxsdkver.h
    HINTS
      ${DXSDK_ROOT_DIR}/include
  )

  IF(CMAKE_CL_64)
  find_path(DXSDK_LIBRARY_DIR
    dsound.lib
    HINTS
    ${DXSDK_ROOT_DIR}/lib/x64
  )
  ELSE(CMAKE_CL_64)
  find_path(DXSDK_LIBRARY_DIR
    dsound.lib
    HINTS
    ${DXSDK_ROOT_DIR}/lib/x86
  )
  ENDIF(CMAKE_CL_64)

  find_library(DXSDK_DSOUND_LIBRARY
    dsound.lib
    HINTS
    ${DXSDK_LIBRARY_DIR}
  )

  # handle the QUIETLY and REQUIRED arguments and set DXSDK_FOUND to TRUE if
  # all listed variables are TRUE
  INCLUDE(FindPackageHandleStandardArgs)
  FIND_PACKAGE_HANDLE_STANDARD_ARGS(DXSDK DEFAULT_MSG DXSDK_ROOT_DIR DXSDK_INCLUDE_DIR)

  MARK_AS_ADVANCED(
      DXSDK_ROOT_DIR DXSDK_INCLUDE_DIR
      DXSDK_LIBRARY_DIR DXSDK_DSOUND_LIBRARY
  )

endif()
