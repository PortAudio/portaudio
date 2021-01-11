add_library(portaudiocpp
    source/portaudiocpp/BlockingStream
    source/portaudiocpp/CallbackInterface
    source/portaudiocpp/CallbackStream
    source/portaudiocpp/CFunCallbackStream
    source/portaudiocpp/CppFunCallbackStream
    source/portaudiocpp/Device
    source/portaudiocpp/DirectionSpecificStreamParameters
    source/portaudiocpp/Exception
    source/portaudiocpp/HostApi
    source/portaudiocpp/InterfaceCallbackStream
    source/portaudiocpp/MemFunCallbackStream
    source/portaudiocpp/Stream
    source/portaudiocpp/StreamParameters
    source/portaudiocpp/System
    source/portaudiocpp/SystemDeviceIterator
    source/portaudiocpp/SystemHostApiIterator
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
    include/portaudiocpp/AutoSystem.hxx
    include/portaudiocpp/BlockingStream.hxx
    include/portaudiocpp/CallbackInterface.hxx
    include/portaudiocpp/CallbackStream.hxx
    include/portaudiocpp/CFunCallbackStream.hxx
    include/portaudiocpp/CppFunCallbackStream.hxx
    include/portaudiocpp/Device.hxx
    include/portaudiocpp/DirectionSpecificStreamParameters.hxx
    include/portaudiocpp/Exception.hxx
    include/portaudiocpp/HostApi.hxx
    include/portaudiocpp/InterfaceCallbackStream.hxx
    include/portaudiocpp/MemFunCallbackStream.hxx
    include/portaudiocpp/PortAudioCpp.hxx
    include/portaudiocpp/SampleDataFormat.hxx
    include/portaudiocpp/Stream.hxx
    include/portaudiocpp/StreamParameters.hxx
    include/portaudiocpp/SystemDeviceIterator.hxx
    include/portaudiocpp/SystemHostApiIterator.hxx
    include/portaudiocpp/System.hxx
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/portaudiocpp"
)
    
if(WIN32)
    install(FILES include/portaudiocpp/AsioDeviceAdapter.hxx DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/portaudiocpp")
endif()

set(PORTAUDIOCPP_VERSION 12)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/portaudiocpp.pc.in ${CMAKE_CURRENT_BINARY_DIR}/portaudiocpp.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/portaudiocpp.pc DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
