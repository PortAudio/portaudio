IF(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_CURRENT_LIST_DIR})
  PROJECT(portaudiocpp)
  CMAKE_MINIMUM_REQUIRED(VERSION 2.8.12)
  SET(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ../../cmake_support)
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

add_library(portaudiocpp ${PA_CPP_SOURCES})

target_link_libraries(portaudiocpp PUBLIC portaudio)
target_include_directories(portaudiocpp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

include(GNUInstallDirs)
install(TARGETS portaudiocpp
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
