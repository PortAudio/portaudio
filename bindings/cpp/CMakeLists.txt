project(portaudiocpp VERSION 12)

add_library(portaudiocpp
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
target_link_libraries(portaudiocpp PUBLIC portaudio)
target_include_directories(portaudiocpp PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>  
  $<INSTALL_INTERFACE:include>)

set(PORTAUDIOCPP_PUBLIC_HEADERS
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

if(WIN32)
    set(PORTAUDIOCPP_PUBLIC_HEADERS "${PORTAUDIOCPP_PUBLIC_HEADERS}" include/AsioDeviceAdapter.hxx)
endif()

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

if(NOT CMAKE_FRAMEWORK)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/portaudiocpp.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/portaudiocpp.pc" @ONLY)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/portaudiocpp.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")

  configure_package_config_file(cmake/portaudiocppConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/cmake/portaudiocpp/portaudiocppConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/portaudiocpp"
    NO_CHECK_REQUIRED_COMPONENTS_MACRO
  )
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/cmake/portaudiocpp/portaudiocppConfigVersion.cmake"
    VERSION "${PORTAUDIO_VERSION}"
    COMPATIBILITY SameMajorVersion
  )
  install(EXPORT portaudiocpp-targets FILE "portaudiocppTargets.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/portaudiocpp")
  export(TARGETS portaudiocpp
    FILE "${CMAKE_CURRENT_BINARY_DIR}/cmake/portaudiocpp/portaudiocppTargets.cmake")
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/cmake/portaudiocpp/portaudiocppConfig.cmake"
                "${CMAKE_CURRENT_BINARY_DIR}/cmake/portaudiocpp/portaudiocppConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/portaudiocpp")
endif()

set_target_properties(portaudiocpp PROPERTIES
  PUBLIC_HEADER "${PORTAUDIOCPP_PUBLIC_HEADERS}"
  MACOSX_FRAMEWORK_IDENTIFIER com.portaudiocpp
  FRAMEWORK_VERSION A
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION}
  WINDOWS_EXPORT_ALL_SYMBOLS TRUE
)
install(TARGETS portaudiocpp
  EXPORT portaudiocpp-targets
  PUBLIC_HEADER DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FRAMEWORK DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
