cmake_minimum_required(VERSION 3.12.0)
project(PortAudio VERSION 19.8)

#
# General PortAudio stuff
#

option(BUILD_SHARED_LIBS "Build dynamic library" ON)
option(BUILD_TESTING "Include test projects" OFF)
option(BUILD_EXAMPLES "Include example projects" OFF)

# The tests and examples cannot link to the shared library because they use private symbols
# that are not exported in the shared library. To hack around this, build the object files
# separately and link them into either a SHARED or STATIC library for the PortAudio target,
# as specified by the user with BUILD_SHARED_LIBS. If the PortAudio target is SHARED, then
# link the objects into a separate STATIC PortAudio_test library that the tests and examples
# link to, but do not install nor export PortAudio_test.
add_library(PortAudio_private OBJECT
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
)
target_include_directories(PortAudio_private PUBLIC include src/common)
if(UNIX)
  target_compile_options(PortAudio_private PRIVATE -fPIC)
endif()

add_library(PortAudio)
target_link_libraries(PortAudio PRIVATE PortAudio_private)
set(PORTAUDIO_PUBLIC_HEADERS include/portaudio.h)

if(BUILD_SHARED_LIBS AND (BUILD_TESTING OR BUILD_EXAMPLES))
  add_library(PortAudio_test STATIC)
  target_link_libraries(PortAudio_test PRIVATE PortAudio_private)
  target_include_directories(PortAudio_test PUBLIC include src/common)
  set(TEST_LIB PortAudio_test)
else()
  set(TEST_LIB PortAudio)
endif()

find_package(Threads REQUIRED)
target_link_libraries(PortAudio_private PRIVATE Threads::Threads)

option(DEBUG_OUTPUT "Enable debug output for Portaudio" OFF)
if(DEBUG_OUTPUT)
  target_compile_definitions(PortAudio_private PRIVATE PA_ENABLE_DEBUG_OUTPUT)
endif()

include(TestBigEndian)
TEST_BIG_ENDIAN(IS_BIG_ENDIAN)
if(IS_BIG_ENDIAN)
  target_compile_definitions(PortAudio_private PRIVATE PA_BIG_ENDIAN)
else()
  target_compile_definitions(PortAudio_private PRIVATE PA_LITTLE_ENDIAN)
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
  target_sources(PortAudio_private PRIVATE src/hostapi/skeleton/pa_hostapi_skeleton.c)
  target_compile_definitions(PortAudio_private PRIVATE PA_USE_SKELETON=1)
endif()

include(CMakeDependentOption)
set(CMAKE_MODULE_PATH "${CMAKE_MODULE_PATH}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")

include(GNUInstallDirs)

find_package(JACK)
cmake_dependent_option(JACK "Enable support for JACK Audio Connection Kit" ON JACK_FOUND OFF)
if(JACK)
  target_link_libraries(PortAudio_private PRIVATE JACK::jack)
  target_sources(PortAudio_private PRIVATE src/hostapi/jack/pa_jack.c)
  set(PORTAUDIO_PUBLIC_HEADERS "${PORTAUDIO_PUBLIC_HEADERS}" include/pa_jack.h)
  target_compile_definitions(PortAudio_private PRIVATE PA_USE_JACK=1)
  set(PKGCONFIG_REQUIRES_PRIVATE "${PKGCONFIG_REQUIRES_PRIVATE} jack")

  # needed for PortAudioConfig.cmake so `find_package(PortAudio)` works in downstream projects
  install(FILES cmake/modules/FindJACK.cmake DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/portaudio/modules")
endif()

if(WIN32)
  target_sources(PortAudio_private PRIVATE
    src/os/win/pa_win_hostapis.c
    src/os/win/pa_win_util.c
    src/os/win/pa_win_waveformat.c
    src/os/win/pa_win_coinitialize.c
  )
  target_include_directories(PortAudio_private PRIVATE src/os/win)
  set(PORTAUDIO_PUBLIC_HEADERS "${PORTAUDIO_PUBLIC_HEADERS}" include/pa_win_waveformat.h)
  target_link_libraries(PortAudio_private PRIVATE winmm)

  if(MSVC)
    target_sources(PortAudio_private PRIVATE src/os/win/pa_x86_plain_converters.c)
  else()
    target_compile_definitions(PortAudio_private PRIVATE _WIN32_WINNT=0x0501 WINVER=0x0501)
    set(DEF_EXCLUDE_X86_PLAIN_CONVERTERS ";")
  endif()

  target_compile_definitions(PortAudio_private PRIVATE _CRT_SECURE_NO_WARNINGS)

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
    target_link_libraries(PortAudio_private PRIVATE ole32 uuid)
    target_include_directories(PortAudio_private PRIVATE "${ASIO_INCLUDE_DIRS}")
    set(PORTAUDIO_PUBLIC_HEADERS "${PORTAUDIO_PUBLIC_HEADERS}" include/pa_asio.h)
    target_compile_definitions(PortAudio_private PRIVATE PA_USE_ASIO=1)
    target_sources(PortAudio_private PRIVATE
      src/hostapi/asio/pa_asio.cpp
      src/hostapi/asio/iasiothiscallresolver.cpp
      "${ASIO_SOURCE_FILES}"
    )
  endif()

  option(DIRECTSOUND "Enable support for DirectSound" ON)
  if(DIRECTSOUND)
    target_sources(PortAudio_private PRIVATE
      src/hostapi/dsound/pa_win_ds.c
      src/hostapi/dsound/pa_win_ds_dynlink.c
    )
    target_include_directories(PortAudio_private PRIVATE src/hostapi/dsound)
    set(PORTAUDIO_PUBLIC_HEADERS "${PORTAUDIO_PUBLIC_HEADERS}" include/pa_win_ds.h)
    target_compile_definitions(PortAudio_private PRIVATE PA_USE_DS=1)
    target_link_libraries(PortAudio_private PRIVATE dsound)
    if(NOT MINGW)
      target_compile_definitions(PortAudio_private PRIVATE PAWIN_USE_DIRECTSOUNDFULLDUPLEXCREATE)
    endif()
  endif()

  option(WMME "Enable support for WMME" ON)
  if(WMME)
    target_sources(PortAudio_private PRIVATE src/hostapi/wmme/pa_win_wmme.c)
    set(PORTAUDIO_PUBLIC_HEADERS "${PORTAUDIO_PUBLIC_HEADERS}" include/pa_win_wmme.h)
    target_compile_definitions(PortAudio_private PRIVATE PA_USE_WMME=1)
    target_link_libraries(PortAudio_private PRIVATE ole32 uuid)
    set(DEF_EXCLUDE_WMME_SYMBOLS ";")
  endif()

  option(WASAPI "Enable support for WASAPI" ON)
  if(WASAPI)
    target_sources(PortAudio_private PRIVATE src/hostapi/wasapi/pa_win_wasapi.c)
    set(PORTAUDIO_PUBLIC_HEADERS "${PORTAUDIO_PUBLIC_HEADERS}" include/pa_win_wasapi.h)
    target_compile_definitions(PortAudio_private PRIVATE PA_USE_WASAPI=1)
    target_link_libraries(PortAudio_private PRIVATE ole32 uuid)
  else()
    set(DEF_EXCLUDE_WASAPI_SYMBOLS ";")
  endif()

  option(WDMKS "Enable support for WDMKS" ON)
  if(WDMKS)
    target_sources(PortAudio_private PRIVATE
      src/os/win/pa_win_wdmks_utils.c
      src/hostapi/wdmks/pa_win_wdmks.c
    )
    set(PORTAUDIO_PUBLIC_HEADERS "${PORTAUDIO_PUBLIC_HEADERS}" include/pa_win_wdmks.h)
    target_compile_definitions(PortAudio_private PRIVATE PA_USE_WDMKS=1)
    target_link_libraries(PortAudio_private PRIVATE setupapi ole32 uuid)
  endif()

  option(WDMKS_DEVICE_INFO "Use WDM/KS API for device info" ON)
  if(WDMKS_DEVICE_INFO)
    target_compile_definitions(PortAudio_private PRIVATE PAWIN_USE_WDMKS_DEVICE_INFO)
  endif()

  if(BUILD_SHARED_LIBS)
    configure_file(cmake/windows_dll_exports.def.in ${CMAKE_CURRENT_BINARY_DIR}/windows_dll_exports.def @ONLY)
    target_sources(PortAudio PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/windows_dll_exports.def)
  endif()
elseif(UNIX)
  target_sources(PortAudio_private PRIVATE
    src/os/unix/pa_unix_hostapis.c
    src/os/unix/pa_unix_util.c
  )
  target_include_directories(PortAudio_private PRIVATE src/os/unix)
  target_link_libraries(PortAudio_private PRIVATE m)
  set(PKGCONFIG_LDFLAGS_PRIVATE "${PKGCONFIG_LDFLAGS_PUBLIC} -lm -lpthread")
  set(PKGCONFIG_CFLAGS "${PKGCONFIG_CFLAGS} -pthread")

  if(APPLE)
    set(CMAKE_MACOSX_RPATH 1)
    target_sources(PortAudio_private PRIVATE
      src/hostapi/coreaudio/pa_mac_core.c
      src/hostapi/coreaudio/pa_mac_core_blocking.c
      src/hostapi/coreaudio/pa_mac_core_utilities.c)
    target_include_directories(PortAudio_private PRIVATE src/hostapi/coreaudio)
    set(PORTAUDIO_PUBLIC_HEADERS "${PORTAUDIO_PUBLIC_HEADERS}" include/pa_mac_core.h)

    find_library(COREAUDIO_LIBRARY CoreAudio REQUIRED)
    find_library(AUDIOTOOLBOX_LIBRARY AudioToolbox REQUIRED)
    find_library(AUDIOUNIT_LIBRARY AudioUnit REQUIRED)
    find_library(COREFOUNDATION_LIBRARY CoreFoundation REQUIRED)
    find_library(CORESERVICES_LIBRARY CoreServices REQUIRED)
    target_link_libraries(PortAudio_private PRIVATE
      ${COREAUDIO_LIBRARY}
      ${AUDIOTOOLBOX_LIBRARY}
      ${AUDIOUNIT_LIBRARY}
      ${COREFOUNDATION_LIBRARY}
      ${CORESERVICES_LIBRARY}
    )
    target_compile_definitions(PortAudio_private PRIVATE PA_USE_COREAUDIO=1)
    set(PKGCONFIG_LDFLAGS_PRIVATE
      "${PKGCONFIG_LDFLAGS_PRIVATE} -framework CoreAudio -framework AudioToolbox -framework AudioUnit -framework CoreFoundation -framework CoreServices")
  else()
    find_package(ALSA)
    cmake_dependent_option(ALSA "Enable support for ALSA" ON ALSA_FOUND OFF)
    if(ALSA)
      target_link_libraries(PortAudio_private PRIVATE ALSA::ALSA)
      target_sources(PortAudio_private PRIVATE src/hostapi/alsa/pa_linux_alsa.c)
      set(PORTAUDIO_PUBLIC_HEADERS "${PORTAUDIO_PUBLIC_HEADERS}" include/pa_linux_alsa.h)
      target_compile_definitions(PortAudio_private PRIVATE PA_USE_ALSA=1)

      option(ALSA_DYNAMIC "Enable dynamically loading libasound with dlopen using PaAlsa_SetLibraryPathName" OFF)
      if(ALSA_DYNAMIC)
        target_compile_definitions(PortAudio_private PRIVATE PA_ALSA_DYNAMIC)
        target_link_libraries(PortAudio_private PRIVATE ${CMAKE_DL_LIBS})
        set(PKGCONFIG_LDFLAGS_PRIVATE "${PKGCONFIG_LDFLAGS_PRIVATE} -l${CMAKE_DL_LIBS}")
      else()
        set(PKGCONFIG_REQUIRES_PRIVATE "${PKGCONFIG_REQUIRES_PRIVATE} alsa")
      endif()
    endif()

    option(OSS "Enable support for OSS" OFF)
    if(OSS)
      # OSS has no library to link, only a header
      find_path(OSS_INCLUDE_DIR
        NAMES sys/soundcard.h
        DOC "OSS include directory")
      if(OSS_INCLUDE_DIR)
        target_compile_definitions(PortAudio_private PRIVATE HAVE_SYS_SOUNDCARD_H)
      else()
        find_path(OSS_INCLUDE_DIR
          NAMES linux/soundcard.h
          DOC "OSS include directory")
        if(OSS_INCLUDE_DIR)
          target_compile_definitions(PortAudio_private PRIVATE HAVE_LINUX_SOUNDCARD_H)
        else()
          find_path(OSS_INCLUDE_DIR
            NAMES machine/soundcard.h
            DOC "OSS include directory")
          target_compile_definitions(PortAudio_private PRIVATE HAVE_MACHINE_SOUNDCARD_H)
        endif()
      endif()

      mark_as_advanced(OSS_INCLUDE_DIR)
      if(OSS_INCLUDE_DIR)
        message(STATUS "Found OSS: ${OSS_INCLUDE_DIR}")
        target_include_directories(PortAudio_private PRIVATE "${OSS_INCLUDE_DIR}")
        target_sources(PortAudio_private PRIVATE src/hostapi/oss/pa_unix_oss.c)
        target_compile_definitions(PortAudio_private PRIVATE PA_USE_OSS=1)
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

if(NOT CMAKE_FRAMEWORK)
  install(FILES README.md DESTINATION "${CMAKE_INSTALL_DOCDIR}/portaudio")
  install(FILES LICENSE.txt DESTINATION "${CMAKE_INSTALL_DOCDIR}/portaudio")

  configure_file(cmake/portaudio-2.0.pc.in "${CMAKE_CURRENT_BINARY_DIR}/portaudio-2.0.pc" @ONLY)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/portaudio-2.0.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")

  configure_package_config_file(cmake/PortAudioConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/cmake/portaudio/PortAudioConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/portaudio"
    NO_CHECK_REQUIRED_COMPONENTS_MACRO
  )
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/cmake/portaudio/PortAudioConfigVersion.cmake"
    VERSION "${PORTAUDIO_VERSION}"
    COMPATIBILITY SameMajorVersion
  )
  install(EXPORT PortAudio-targets NAMESPACE "PortAudio::" FILE "PortAudioTargets.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/portaudio")
  export(TARGETS PortAudio
    FILE "${CMAKE_CURRENT_BINARY_DIR}/cmake/portaudio/PortAudioTargets.cmake")
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/cmake/portaudio/PortAudioConfig.cmake"
                "${CMAKE_CURRENT_BINARY_DIR}/cmake/portaudio/PortAudioConfigVersion.cmake"
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

set_target_properties(PortAudio PROPERTIES
  OUTPUT_NAME portaudio
  PUBLIC_HEADER "${PORTAUDIO_PUBLIC_HEADERS}"
  MACOSX_FRAMEWORK_IDENTIFIER com.portaudio
  FRAMEWORK_VERSION A
  WINDOWS_EXPORT_ALL_SYMBOLS TRUE
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION}
)
install(TARGETS PortAudio
  EXPORT PortAudio-targets
  PUBLIC_HEADER DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FRAMEWORK DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

#
# Subdirectories
#

if(BUILD_TESTING)
  macro(add_test appl_name)
    add_executable(${appl_name} "${appl_name}.c")
    target_link_libraries(${appl_name} ${TEST_LIB} Threads::Threads)
    if(UNIX)
      target_link_libraries(${appl_name} m)
    endif()
    set_target_properties(${appl_name}
        PROPERTIES
        FOLDER "Test"
    )
  endmacro()

  subdirs(test)
  subdirs(qa)
endif()

if(BUILD_EXAMPLES)
  subdirs(examples)
endif()
