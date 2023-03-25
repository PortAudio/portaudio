#[=======================================================================[.rst:
FindOSS
--------

Finds the Open Sound System include directory. There is no library to link.

Imported Targets
^^^^^^^^^^^^^^^^

This module provides the following imported targets, if found:

``OSS::oss``
  Target for the OSS header include directory. The following compile
  definition is added to the target:
  HAVE_SYS_SOUNDCARD_H for the header sys/soundcard.h

#]=======================================================================]

find_path(OSS_INCLUDE_DIR
  NAMES sys/soundcard.h
  DOC "OSS include directory")
if(OSS_INCLUDE_DIR)
  set(OSS_DEFINITIONS HAVE_SYS_SOUNDCARD_H)
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  OSS
  DEFAULT_MSG
  OSS_INCLUDE_DIR
  OSS_DEFINITIONS
)

if(OSS_INCLUDE_DIR AND OSS_DEFINITIONS)
  set(OSS_FOUND TRUE)
  if(NOT TARGET OSS::oss)
    add_library(OSS::oss INTERFACE IMPORTED)
    target_include_directories(OSS::oss INTERFACE "${OSS_INCLUDE_DIR}")
    target_compile_definitions(OSS::oss INTERFACE "${OSS_DEFINITIONS}")
  endif()
endif()
