# Check if the user is building PortAudio C++ Bindings stand-alone or as part of a larger
# project. If this is part of a larger project (i.e. the CMakeLists.txt has
# been imported by some other CMakeLists.txt), we don't want to trump over
# the top of that project's global settings.
IF(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_CURRENT_LIST_DIR})
  PROJECT(portaudiocpp)
  CMAKE_MINIMUM_REQUIRED(VERSION 2.8.12)
  SET(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ../../cmake_support)
  OPTION(PA_BUILD_STATIC "Build static library" ON)
  OPTION(PA_BUILD_SHARED "Build shared/dynamic library" ON)
  IF(MSVC)
    OPTION(PA_LIBNAME_ADD_SUFFIX "Add suffix _static to static library name" ON)
  ELSE()
    OPTION(PA_LIBNAME_ADD_SUFFIX "Add suffix _static to static library name" OFF)
  ENDIF()

  # MSVC: if PA_LIBNAME_ADD_SUFFIX is not used, and both static and shared libraries are
  # built, one, of import- and static libraries, will overwrite the other. In
  # embedded builds this is not an issue as they will only build the configuration
  # used in the host application.
  MARK_AS_ADVANCED(PA_LIBNAME_ADD_SUFFIX)
  IF(MSVC AND PA_BUILD_STATIC AND PA_BUILD_SHARED AND NOT PA_LIBNAME_ADD_SUFFIX)
    MESSAGE(WARNING "Building both shared and static libraries, and avoiding the suffix _static will lead to a name conflict")
    SET(PA_LIBNAME_ADD_SUFFIX ON CACHE BOOL "Forcing use of suffix _static to avoid name conflict between static and import library" FORCE)
    MESSAGE(WARNING "PA_LIBNAME_ADD_SUFFIX was set to ON")
  ENDIF()

  IF(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    MESSAGE(STATUS "Setting CMAKE_BUILD_TYPE type to 'Debug' as none was specified.")
    SET(CMAKE_BUILD_TYPE Debug CACHE STRING "Choose the type of build." FORCE)
    SET_PROPERTY(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release")
  ENDIF()
ENDIF()

set(PA_CPP_SOURCES
  src/BlockingStream.cxx
  src/CallbackInterface.cxx
  src/CallbackStream.cxx
  src/CFunCallbackStream.cxx
  src/CppFunCallbackStream.cxx
  src/Device.cxx
  src/DirectionSpecificStreamParameters.cxx
  src/Exception.cxx
  src/HostApi.cxx
  src/InterfaceCallbackStream.cxx
  src/MemFunCallbackStream.cxx
  src/Stream.cxx
  src/StreamParameters.cxx
  src/System.cxx
  src/SystemDeviceIterator.cxx
  src/SystemHostApiIterator.cxx
)

set(PA_CPP_INCLUDES
  include/AutoSystem.hxx
  include/BlockingStream.hxx
  include/CallbackInterface.hxx
  include/CallbackStream.hxx
  include/CFunCallbackStream.hxx
  include/CppFunCallbackStream.hxx
  include/Device.hxx
  include/DirectionSpecificStreamParameters.hxx
  include/Exception.hxx
  include/HostApi.hxx
  include/InterfaceCallbackStream.hxx
  include/MemFunCallbackStream.hxx
  include/PortAudioCpp.hxx
  include/SampleDataFormat.hxx
  include/Stream.hxx
  include/StreamParameters.hxx
  include/SystemDeviceIterator.hxx
  include/SystemHostApiIterator.hxx
  include/System.hxx
)

IF(WIN32)
  if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_LIST_DIR)
    FIND_PACKAGE(ASIOSDK)
    IF(ASIOSDK_FOUND)
      OPTION(PA_USE_ASIO "Enable support for ASIO" ON)
    ELSE()
      OPTION(PA_USE_ASIO "Enable support for ASIO" OFF)
    ENDIF()
  endif()

  if(PA_USE_ASIO)
    list(APPEND PA_CPP_INCLUDES include/AsioDeviceAdapter.hxx)
    list(APPEND PA_CPP_SOURCES src/AsioDeviceAdapter.cxx)
  endif()
ENDIF(WIN32)

IF(PA_BUILD_SHARED)
  add_library(portaudiocpp SHARED ${PA_CPP_SOURCES})
  target_link_libraries(portaudiocpp PUBLIC portaudio)
  target_include_directories(portaudiocpp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
ENDIF(PA_BUILD_SHARED)
IF(PA_BUILD_STATIC)
  add_library(portaudiocpp_static STATIC ${PA_CPP_SOURCES})
  target_link_libraries(portaudiocpp_static PUBLIC portaudio)
  target_include_directories(portaudiocpp_static PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
  IF(NOT PA_LIBNAME_ADD_SUFFIX)
    SET_PROPERTY(TARGET portaudiocpp_static PROPERTY OUTPUT_NAME portaudiocpp)
  ENDIF()
ENDIF(PA_BUILD_STATIC)

include(GNUInstallDirs)
install(TARGETS portaudiocpp portaudiocpp_static
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

install(FILES ${PA_CPP_INCLUDES} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/portaudiocpp")

if(WIN32)
    install(FILES include/AsioDeviceAdapter.hxx DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/portaudiocpp")
endif()

set(PORTAUDIOCPP_VERSION 12)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/portaudiocpp.pc.in ${CMAKE_CURRENT_BINARY_DIR}/portaudiocpp.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/portaudiocpp.pc DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
