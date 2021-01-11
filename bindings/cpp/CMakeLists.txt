add_library(portaudiocpp
    src/BlockingStream
    src/CallbackInterface
    src/CallbackStream
    src/CFunCallbackStream
    src/CppFunCallbackStream
    src/Device
    src/DirectionSpecificStreamParameters
    src/Exception
    src/HostApi
    src/InterfaceCallbackStream
    src/MemFunCallbackStream
    src/Stream
    src/StreamParameters
    src/System
    src/SystemDeviceIterator
    src/SystemHostApiIterator
)
target_link_libraries(portaudiocpp PUBLIC portaudio)
target_include_directories(portaudiocpp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

include(GNUInstallDirs)
install(TARGETS portaudiocpp
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

install(FILES
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
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/portaudiocpp"
)
    
if(WIN32)
    install(FILES include/AsioDeviceAdapter.hxx DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/portaudiocpp")
endif()

set(PORTAUDIOCPP_VERSION 12)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/portaudiocpp.pc.in ${CMAKE_CURRENT_BINARY_DIR}/portaudiocpp.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/portaudiocpp.pc DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
