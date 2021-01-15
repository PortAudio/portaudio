#[=======================================================================[.rst:
FindASIO
--------

Finds the ASIO SDK by searching for the SDK ZIP in CMAKE_BINARY_DIR,
CMAKE_SOURCE_DIR, and CMAKE_PREFIX_PATH. Alternatively, you may manually specify
the path of the SDK ZIP with the ASIO_SDK_ZIP_PATH variable, which can be used
for caching in CI scripts.

If the ZIP is found, this module extracts it.
The ZIP extraction is skipped if the unzipped SDK is found.

This module exports targets for building applications which use ASIO drivers.
If you want to build an ASIO driver, this may serve as a useful start but you
will need to modify it.

This module does not provide any library targets to link to.

Result Variables
^^^^^^^^^^^^^^^^

This will define the following variables:

``ASIO_FOUND``
  True if the ASIO SDK was found.
``ASIO_INCLUDE_DIRS``
  Include directories needed to use the ASIO SDK
``ASIO_SOURCE_FILES``
  Source code files that need to be built to use the SDK
  
Cache Variables
^^^^^^^^^^^^^^^

The following cache variables may also be set:

``ASIO_ROOT``
  The directory containing ``common/asio.h``.

#]=======================================================================]

if(NOT WIN32)
  message(FATAL_ERROR "ASIO is only supported on Windows")
endif()

file(GLOB_RECURSE HEADER_FILE "${CMAKE_BINARY_DIR}/*/asio.h")
if(NOT EXISTS "${HEADER_FILE}")
  file(GLOB results
    "${ASIO_SDK_ZIP_PATH}"
    "${CMAKE_PREFIX_PATH}/asiosdk*.zip"
    "${CMAKE_CURRENT_BINARY_DIR}/asiosdk*.zip"
    "${CMAKE_CURRENT_SOURCE_DIR}/asiosdk*.zip"
    # The old build systems used to look for the ASIO SDK
    # in the same parent directory as the source code repository.
    "${CMAKE_CURRENT_SOURCE_DIR}/../asiosdk*.zip")
  foreach(f ${results})
    if(EXISTS "${f}")
      message(STATUS "Extracting ASIO SDK ZIP archive: ${f}")
      file(ARCHIVE_EXTRACT INPUT "${f}")
    endif()
  endforeach()
endif()

file(GLOB_RECURSE HEADER_FILE "${CMAKE_BINARY_DIR}/*/asio.h")
get_filename_component(HEADER_DIR "${HEADER_FILE}" DIRECTORY)
get_filename_component(ASIO_ROOT "${HEADER_DIR}" DIRECTORY)

if(ASIO_ROOT)
  set(ASIO_FOUND TRUE)
  message(STATUS "Found ASIO SDK: ${ASIO_ROOT}")

  set(ASIO_INCLUDE_DIRS
    "${ASIO_ROOT}/common"
    "${ASIO_ROOT}/host"
    "${ASIO_ROOT}/host/pc"
  )
    
  set(ASIO_SOURCE_FILES
    "${ASIO_ROOT}/common/asio.cpp"
    "${ASIO_ROOT}/host/asiodrivers.cpp"
    "${ASIO_ROOT}/host/pc/asiolist.cpp"
  )
else()
  message(STATUS "ASIO SDK NOT found")
endif()
