cmake_minimum_required(VERSION 3.4.0)
project(portaudio VERSION 19.8)

#
# General PortAudio stuff
#

option(BUILD_SHARED_LIBS "Build dynamic library" ON)

add_library(portaudio
  src/common/pa_allocation.c
  src/common/pa_converters.c
  src/common/pa_cpuload.c
  src/common/pa_debugprint.c
  src/common/pa_dither.c
  src/common/pa_front.c
  src/common/pa_process.c
  src/common/pa_ringbuffer.c
  src/common/pa_stream.c
  src/common/pa_trace.c
  src/hostapi/skeleton/pa_hostapi_skeleton.c
)
target_include_directories(portaudio PRIVATE src/common)
target_include_directories(portaudio PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
set(PORTAUDIO_PUBLIC_HEADERS include/portaudio.h)

find_package(Threads REQUIRED)
target_link_libraries(portaudio Threads::Threads)

option(DEBUG_OUTPUT "Enable debug output for Portaudio" OFF)
if(DEBUG_OUTPUT)
  target_compile_definitions(portaudio PRIVATE PA_ENABLE_DEBUG_OUTPUT)
endif()

include(TestBigEndian)
TEST_BIG_ENDIAN(IS_BIG_ENDIAN)
if(IS_BIG_ENDIAN)
  target_compile_definitions(portaudio PRIVATE PA_BIG_ENDIAN)
else()
  target_compile_definitions(portaudio PRIVATE PA_LITTLE_ENDIAN)
endif()

if(WIN32 AND MSVC AND BUILD_SHARED_LIBS
  # Check if the user is building PortAudio stand-alone or as part of a larger
  # project. If this is part of a larger project (i.e. the CMakeLists.txt has
  # been imported by some other CMakeLists.txt), we don't want to override
  # that project's global settings.
  AND "${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_CURRENT_LIST_DIR}")
  option(DLL_LINK_WITH_STATIC_RUNTIME
    "Link with static runtime libraries (minimizes runtime dependencies)" ON)
  if(DLL_LINK_WITH_STATIC_RUNTIME)
    foreach(flag_var
      CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE
      CMAKE_C_FLAGS_MINSIZEREL CMAKE_C_FLAGS_RELWITHDEBINFO
      CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
      CMAKE_CXX_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_RELWITHDEBINFO)
      if(${flag_var} MATCHES "/MD")
        string(REGEX REPLACE "/MD" "/MT" ${flag_var} "${${flag_var}}")
      endif()
    endforeach()
  endif()
endif()

#
# Host APIs
#

option(SKELETON "Use skeleton host API" OFF)
if(SKELETON)
  target_compile_definitions(portaudio PRIVATE PA_USE_SKELETON=1)
endif()

include(CMakeDependentOption)
set(CMAKE_MODULE_PATH "${CMAKE_MODULE_PATH}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package(JACK)
cmake_dependent_option(JACK "Enable support for JACK Audio Connection Kit" ON JACK_FOUND OFF)
if(JACK)
  target_link_libraries(portaudio JACK::jack)
  target_sources(portaudio PRIVATE src/hostapi/jack/pa_jack.c)
  set(PORTAUDIO_PUBLIC_HEADERS "${PORTAUDIO_PUBLIC_HEADERS}" include/pa_jack.h)
  target_compile_definitions(portaudio PRIVATE PA_USE_JACK=1)
endif()

if(WIN32)
  target_sources(portaudio PRIVATE
    src/os/win/pa_win_hostapis.c
    src/os/win/pa_win_util.c
    src/os/win/pa_win_waveformat.c
    src/os/win/pa_win_coinitialize.c
  )
  target_include_directories(portaudio PRIVATE src/os/win)
  set(PORTAUDIO_PUBLIC_HEADERS "${PORTAUDIO_PUBLIC_HEADERS}" include/pa_win_waveformat.h)
  target_link_libraries(portaudio winmm)

  if(MSVC)
    target_sources(portaudio PRIVATE src/os/win/pa_x86_plain_converters.c)
  else()
    target_compile_definitions(portaudio PRIVATE _WIN32_WINNT=0x0501 WINVER=0x0501)
  endif()

  target_compile_definitions(portaudio PRIVATE _CRT_SECURE_NO_WARNINGS)

  option(ASIO "Enable support for ASIO" OFF)
  if(ASIO)
    find_package(ASIO)
    if(NOT ASIO_FOUND)
      # The ASIO SDK license allows for downloading it from Steinberg and using it without charge,
      # but it is not allowed to be redistributed.
      if(NOT ASIO_SDK_ZIP_PATH)
        set(ASIO_SDK_ZIP_PATH "${CMAKE_CURRENT_BINARY_DIR}/asiosdk.zip")
      endif()
      message(STATUS "Downloading ASIO SDK... ${ASIO_SDK_ZIP_PATH}")
      file(DOWNLOAD "https://www.steinberg.net/asiosdk"
        ${ASIO_SDK_ZIP_PATH}
        STATUS ASIO_DOWNLOAD_STATUS
        SHOW_PROGRESS)
      if(NOT "${ASIO_DOWNLOAD_STATUS}" EQUAL 0)
        list(GET "${ASIO_DOWNLOAD_STATUS}" 1 DOWNLOAD_ERROR)
        message(FATAL_ERROR "Error downloading ASIO SDK: ${DOWNLOAD_ERROR}."
          "Reconfigure CMake with -DASIO=OFF to build without ASIO.")
      endif()
      find_package(ASIO)
      if(NOT ASIO_FOUND)
        message(FATAL_ERROR "ASIO SDK not found. "
          "Reconfigure CMake with -DASIO=OFF to build without ASIO.")
      endif()
    endif()
  endif()
  if(ASIO AND ASIO_FOUND)
    target_link_libraries(portaudio ole32 uuid)
    target_include_directories(portaudio PRIVATE "${ASIO_INCLUDE_DIRS}")
    set(PORTAUDIO_PUBLIC_HEADERS "${PORTAUDIO_PUBLIC_HEADERS}" include/pa_asio.h)
    target_compile_definitions(portaudio PRIVATE PA_USE_ASIO=1)
    target_sources(portaudio PRIVATE
      src/hostapi/asio/pa_asio.cpp
      src/hostapi/asio/iasiothiscallresolver.cpp
      "${ASIO_SOURCE_FILES}"
    )
  endif()

  option(DIRECTSOUND "Enable support for DirectSound" ON)
  if(DIRECTSOUND)
    target_sources(portaudio PRIVATE
      src/hostapi/dsound/pa_win_ds.c
      src/hostapi/dsound/pa_win_ds_dynlink.c
    )
    target_include_directories(portaudio PRIVATE src/hostapi/dsound)
    set(PORTAUDIO_PUBLIC_HEADERS "${PORTAUDIO_PUBLIC_HEADERS}" include/pa_win_ds.h)
    target_compile_definitions(portaudio PRIVATE PA_USE_DS=1)
    target_link_libraries(portaudio dsound)
    if(NOT MINGW)
      target_compile_definitions(portaudio PRIVATE PAWIN_USE_DIRECTSOUNDFULLDUPLEXCREATE)
    endif()
  endif()

  option(WMME "Enable support for WMME" ON)
  if(WMME)
    target_sources(portaudio PRIVATE src/hostapi/wmme/pa_win_wmme.c)
    set(PORTAUDIO_PUBLIC_HEADERS "${PORTAUDIO_PUBLIC_HEADERS}" include/pa_win_wmme.h)
    target_compile_definitions(portaudio PRIVATE PA_USE_WMME=1)
    target_link_libraries(portaudio ole32 uuid)
  endif()

  option(WASAPI "Enable support for WASAPI" ON)
  if(WASAPI)
    target_sources(portaudio PRIVATE src/hostapi/wasapi/pa_win_wasapi.c)
    set(PORTAUDIO_PUBLIC_HEADERS "${PORTAUDIO_PUBLIC_HEADERS}" include/pa_win_wasapi.h)
    target_compile_definitions(portaudio PRIVATE PA_USE_WASAPI=1)
    target_link_libraries(portaudio ole32 uuid)
  endif()

  option(WDMKS "Enable support for WDMKS" ON)
  if(WDMKS)
    target_sources(portaudio PRIVATE
      src/os/win/pa_win_wdmks_utils.c
      src/hostapi/wdmks/pa_win_wdmks.c
    )
    set(PORTAUDIO_PUBLIC_HEADERS "${PORTAUDIO_PUBLIC_HEADERS}" include/pa_win_wdmks.h)
    target_compile_definitions(portaudio PRIVATE PA_USE_WDMKS=1)
    target_link_libraries(portaudio setupapi ole32 uuid)
  endif()

  option(WDMKS_DEVICE_INFO "Use WDM/KS API for device info" ON)
  if(WDMKS_DEVICE_INFO)
    target_compile_definitions(portaudio PRIVATE PAWIN_USE_WDMKS_DEVICE_INFO)
  endif()
elseif(UNIX)
  target_sources(portaudio PRIVATE
    src/os/unix/pa_unix_hostapis.c
    src/os/unix/pa_unix_util.c
  )
  target_include_directories(portaudio PRIVATE src/os/unix)
  target_link_libraries(portaudio m)

  if(APPLE)
    set(CMAKE_MACOSX_RPATH 1)
    target_sources(portaudio PRIVATE
      src/hostapi/coreaudio/pa_mac_core.c
      src/hostapi/coreaudio/pa_mac_core_blocking.c
      src/hostapi/coreaudio/pa_mac_core_utilities.c)
    target_include_directories(portaudio PRIVATE src/hostapi/coreaudio)
    set(PORTAUDIO_PUBLIC_HEADERS "${PORTAUDIO_PUBLIC_HEADERS}" include/pa_mac_core.h)

    find_library(COREAUDIO_LIBRARY CoreAudio REQUIRED)
    find_library(AUDIOTOOLBOX_LIBRARY AudioToolbox REQUIRED)
    find_library(AUDIOUNIT_LIBRARY AudioUnit REQUIRED)
    find_library(COREFOUNDATION_LIBRARY CoreFoundation REQUIRED)
    find_library(CORESERVICES_LIBRARY CoreServices REQUIRED)
    target_link_libraries(portaudio
      ${COREAUDIO_LIBRARY}
      ${AUDIOTOOLBOX_LIBRARY}
      ${AUDIOUNIT_LIBRARY}
      ${COREFOUNDATION_LIBRARY}
      ${CORESERVICES_LIBRARY}
    )
    target_compile_definitions(portaudio PRIVATE PA_USE_COREAUDIO=1)
  else()
    find_package(ALSA)
    cmake_dependent_option(ALSA "Enable support for ALSA" ON ALSA_FOUND OFF)
    if(ALSA)
      target_link_libraries(portaudio ALSA::ALSA)
      target_sources(portaudio PRIVATE src/hostapi/alsa/pa_linux_alsa.c)
      set(PORTAUDIO_PUBLIC_HEADERS "${PORTAUDIO_PUBLIC_HEADERS}" include/pa_linux_alsa.h)
      target_compile_definitions(portaudio PRIVATE PA_USE_ALSA=1)
    endif()

    option(OSS "Enable support for OSS" OFF)
    if(OSS)
      # OSS has no library to link, only a header
      find_path(OSS_INCLUDE_DIR
        NAMES sys/soundcard.h
        DOC "OSS include directory")
      if(OSS_INCLUDE_DIR)
        target_compile_definitions(portaudio PRIVATE HAVE_SYS_SOUNDCARD_H)
      else()
        find_path(OSS_INCLUDE_DIR
          NAMES linux/soundcard.h
          DOC "OSS include directory")
        if(OSS_INCLUDE_DIR)
          target_compile_definitions(portaudio PRIVATE HAVE_LINUX_SOUNDCARD_H)
        else()
          find_path(OSS_INCLUDE_DIR
            NAMES machine/soundcard.h
            DOC "OSS include directory")
          target_compile_definitions(portaudio PRIVATE HAVE_MACHINE_SOUNDCARD_H)
        endif()
      endif()

      mark_as_advanced(OSS_INCLUDE_DIR)
      if(OSS_INCLUDE_DIR)
        message(STATUS "Found OSS: ${OSS_INCLUDE_DIR}")
        target_include_directories(portaudio PRIVATE "${OSS_INCLUDE_DIR}")
        target_sources(portaudio PRIVATE src/hostapi/oss/pa_unix_oss.c)
        target_compile_definitions(portaudio PRIVATE PA_USE_OSS=1)
      else()
        message(WARNING "OSS NOT found")
      endif()
    endif()
  endif()
endif()

#
# Installation
#

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

if(NOT CMAKE_FRAMEWORK)
  install(FILES README.md DESTINATION "${CMAKE_INSTALL_DOCDIR}/portaudio")
  install(FILES LICENSE.txt DESTINATION "${CMAKE_INSTALL_DOCDIR}/portaudio")

  configure_file(cmake/portaudio-2.0.pc.in "${CMAKE_CURRENT_BINARY_DIR}/portaudio-2.0.pc" @ONLY)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/portaudio-2.0.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")

  configure_package_config_file(cmake/portaudioConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/cmake/portaudio/portaudioConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/portaudio"
    NO_CHECK_REQUIRED_COMPONENTS_MACRO
  )
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/cmake/portaudio/portaudioConfigVersion.cmake"
    VERSION "${PORTAUDIO_VERSION}"
    COMPATIBILITY SameMajorVersion
  )
  install(EXPORT portaudio-targets NAMESPACE "portaudio::" FILE "portaudioTargets.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/portaudio")
  export(TARGETS portaudio
    FILE "${CMAKE_CURRENT_BINARY_DIR}/cmake/portaudio/portaudioTargets.cmake")
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/cmake/portaudio/portaudioConfig.cmake"
                "${CMAKE_CURRENT_BINARY_DIR}/cmake/portaudio/portaudioConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/portaudio")

  if(NOT TARGET uninstall)
    configure_file(
      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in"
      "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
      IMMEDIATE @ONLY)
    add_custom_target(uninstall
      COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake")
  endif()
endif()

set_target_properties(portaudio PROPERTIES
  PUBLIC_HEADER "${PORTAUDIO_PUBLIC_HEADERS}"
  MACOSX_FRAMEWORK_IDENTIFIER com.portaudio
  FRAMEWORK_VERSION A
  WINDOWS_EXPORT_ALL_SYMBOLS TRUE
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION}
)
install(TARGETS portaudio
  EXPORT portaudio-targets
  PUBLIC_HEADER DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FRAMEWORK DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

#
# Subdirectories
#

option(BUILD_TESTING "Include test projects" OFF)
if(BUILD_TESTING)
  macro(add_test appl_name)
    add_executable(${appl_name} "${appl_name}.c")
    target_link_libraries(${appl_name} portaudio)
    target_include_directories(${appl_name} PRIVATE "${CMAKE_SOURCE_DIR}/src/common")
    set_target_properties(${appl_name}
        PROPERTIES
        FOLDER "Test"
    )
  endmacro()

  subdirs(test)
  subdirs(qa)
endif()

option(BUILD_EXAMPLES "Include example projects" OFF)
if(BUILD_EXAMPLES)
  subdirs(examples)
endif()
