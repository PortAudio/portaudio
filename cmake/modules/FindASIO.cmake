#[=======================================================================[.rst:
FindASIO
--------

Finds the ASIO SDK by searching for the SDK ZIP in CMAKE_PREFIX_PATH and
CMAKE_CURRENT_BINARY_DIR. Alternatively, you may manually specify the path of
the SDK ZIP with the ASIO_SDK_ZIP_PATH variable, which can be used for caching
in CI scripts.

If the ZIP is found, this module extracts it.
The ZIP extraction is skipped if the unzipped SDK is found.

This module provides an `ASIO::host` IMPORT library target for building host
applications which use ASIO drivers. If you want to build an ASIO driver, this
module may serve as a useful start but you will need to modify it.

As of ASIO-SDK_2.3.4_2025-10-15, the ASIO SDK file host/pc/asiolist.cpp contains
a bug. This cmake file detects the bug and patches the bug if it is detected.
Alternatively, you can supply an SDK with the bug already patched, or supply an
SDK with a file host/pc/patched_asiolist.cpp which, if present, will be used in
preference to host/pc/asiolist.cpp. All of these options work without additional
configuration.

#]=======================================================================]

if(NOT WIN32)
  message(FATAL_ERROR "ASIO is only supported on Windows.")
endif()

file(GLOB HEADER_FILE
  "${CMAKE_CURRENT_BINARY_DIR}/asiosdk*/common/asio.h"
  "${CMAKE_PREFIX_PATH}/asiosdk*/common/asio.h"
  # The old build systems before PortAudio 19.8 used to look for the ASIO SDK
  # in the same parent directory as the source code repository. This is no
  # longer advised or documented but kept for backwards compatibility.
  "${CMAKE_CURRENT_SOURCE_DIR}/../asiosdk*/common/asio.h"
)
if(NOT EXISTS "${HEADER_FILE}")
  # The file(ARCHIVE_EXTRACT) command was added in CMake 3.18, so if using an
  # older version of CMake, the user needs to extract it themselves.
  if(CMAKE_VERSION VERSION_LESS 3.18)
    message(STATUS "ASIO SDK NOT found. Download the ASIO SDK ZIP from "
      "https://www.steinberg.net/asiosdk and extract it to "
      "${CMAKE_PREFIX_PATH} or ${CMAKE_CURRENT_BINARY_DIR}"
    )
    return()
  endif()
  file(GLOB results
    "${ASIO_SDK_ZIP_PATH}"
    "${CMAKE_CURRENT_BINARY_DIR}/asiosdk*.zip"
    "${CMAKE_PREFIX_PATH}/asiosdk*.zip"
    "${CMAKE_CURRENT_SOURCE_DIR}/../asiosdk*.zip"
  )
  foreach(f ${results})
    if(EXISTS "${f}")
      message(STATUS "Extracting ASIO SDK ZIP archive: ${f}")
      file(ARCHIVE_EXTRACT INPUT "${f}" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
  endforeach()
  file(GLOB HEADER_FILE "${CMAKE_CURRENT_BINARY_DIR}/asiosdk*/common/asio.h")
endif()

get_filename_component(HEADER_DIR "${HEADER_FILE}" DIRECTORY)
get_filename_component(ASIO_ROOT "${HEADER_DIR}" DIRECTORY)

if(ASIO_ROOT)
  set(ASIO_FOUND TRUE)
  message(STATUS "Found ASIO SDK: ${ASIO_ROOT}")

  if(ASIO_FOUND AND NOT TARGET ASIO::host)
    add_library(ASIO::host INTERFACE IMPORTED)

    # Work around ASIO SDK pc/asiolist.cpp bug where `lpdrv` is allocated using array new:
    #   lpdrv = new ASIODRVSTRUCT[1];
    # but deleted using scalar delete:
    #   delete lpdrv; // BUG! should be `delete [] lpdrv;`
    #
    set(ORIGINAL_ASIOLIST "${ASIO_ROOT}/host/pc/asiolist.cpp")
    set(SOURCE_PATCHED_ASIOLIST "${ASIO_ROOT}/host/pc/patched_asiolist.cpp")
    if(EXISTS "${SOURCE_PATCHED_ASIOLIST}")
      message(STATUS "ASIO: pc/asiolist.cpp: using pre-patched source: ${SOURCE_PATCHED_ASIOLIST}")
      set(GOOD_ASIOLIST "${SOURCE_PATCHED_ASIOLIST}")
    else()
      # add a configure dependency so that editing asiolist.cpp or swapping the SDK triggers a reconfigure
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ORIGINAL_ASIOLIST}")
      file(READ "${ORIGINAL_ASIOLIST}" ASIOLIST_CONTENTS)
      # match: `lpdrv = new ASIODRVSTRUCT[1]` i.e. only apply delete[] patch if new[] was used
      # assumes all allocation sites consistently allocate using the same new form
      set(ALLOC_LPDRV_NEW_ARRAY_REGEX "lpdrv[ \t]*=[ \t]*new[ \t]+[a-zA-Z_][a-zA-Z0-9_]*[ \t]*\\[[0-9]+\\]")
      if(NOT ASIOLIST_CONTENTS MATCHES "${ALLOC_LPDRV_NEW_ARRAY_REGEX}")
        message(STATUS "ASIO: pc/asiolist.cpp does not use array allocation for lpdrv - skipping patch.")
        set(GOOD_ASIOLIST "${ORIGINAL_ASIOLIST}")
      else()
        # patch asiolist.cpp. this has no effect if asiolist.cpp already contains the fix
        # use configure_file to keep timestamp stable across reconfigures
        set(PATCHED_ASIOLIST_IN "${CMAKE_CURRENT_BINARY_DIR}/patched_asiolist.cpp.in")
        set(PATCHED_ASIOLIST "${CMAKE_CURRENT_BINARY_DIR}/patched_asiolist.cpp")

        string(REGEX REPLACE "delete[ \t]+lpdrv[ \t]*;" "delete [] lpdrv;" PATCHED_ASIOLIST_CONTENTS "${ASIOLIST_CONTENTS}")
        file(WRITE "${PATCHED_ASIOLIST_IN}" "${PATCHED_ASIOLIST_CONTENTS}")
        configure_file("${PATCHED_ASIOLIST_IN}" "${PATCHED_ASIOLIST}" COPYONLY)
        if(PATCHED_ASIOLIST_CONTENTS STREQUAL ASIOLIST_CONTENTS)
          message(STATUS "ASIO: pc/asiolist.cpp: no scalar delete found - already fixed or unrecognised.")
        else()
          message(STATUS "ASIO: pc/asiolist.cpp: patched scalar delete -> delete [] in: ${PATCHED_ASIOLIST}")
        endif()
        unset(PATCHED_ASIOLIST_CONTENTS)

        set(GOOD_ASIOLIST "${PATCHED_ASIOLIST}")
      endif()
      unset(ASIOLIST_CONTENTS)
    endif()
    # ^^^ end pc/asiolist.cpp bug workaround.

    target_sources(ASIO::host INTERFACE
      "${ASIO_ROOT}/common/asio.cpp"
      "${ASIO_ROOT}/host/asiodrivers.cpp"
      "${GOOD_ASIOLIST}"
    )
    target_include_directories(ASIO::host INTERFACE
      "${ASIO_ROOT}/common"
      "${ASIO_ROOT}/host"
      "${ASIO_ROOT}/host/pc"
    )
    target_link_libraries(ASIO::host INTERFACE ole32 uuid)
  endif()
else()
  message(STATUS "ASIO SDK NOT found")
endif()
