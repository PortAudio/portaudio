# Try to find the PulseAudio library
#
# Once done this will define:
#
#  PULSEAUDIO_FOUND - system has the PulseAudio library
#  PULSEAUDIO_INCLUDE_DIR - the PulseAudio include directory
#  PULSEAUDIO_LIBRARY - the libraries needed to use PulseAudio
#
# Copyright (c) 2008, Matthias Kretz, <kretz@kde.org>
#
# Redistribution and use is allowed according to the terms of the BSD license.
# For details see the accompanying COPYING-CMAKE-SCRIPTS file.
#
# This version is from Github from pulseaudio-qt
# but this is valided with cmakelint --spaces 4

IF(PULSEAUDIO_INCLUDE_DIR AND PULSEAUDIO_LIBRARY)
    # Already in cache, be silent
    SET(PULSEAUDIO_FIND_QUIETLY TRUE)
ENDIF()

IF(NOT WIN32)
    INCLUDE(FindPkgConfig)
    PKG_CHECK_MODULES(PULSEAUDIO libpulse)
    IF(PULSEAUDIO_FOUND)
        SET(PULSEAUDIO_LIBRARY ${PULSEAUDIO_LIBRARIES} CACHE
        FILEPATH "Path to the PulseAudio library")
        SET(PULSEAUDIO_INCLUDE_DIR ${PULSEAUDIO_INCLUDEDIR} CACHE
        PATH "Path to the PulseAudio includes")
    ENDIF()
ENDIF()

IF(NOT PULSEAUDIO_INCLUDE_DIR)
    FIND_PATH(PULSEAUDIO_INCLUDE_DIR pulse/pulseaudio.h)
ENDIF()

IF(NOT PULSEAUDIO_LIBRARY)
    FIND_LIBRARY(PULSEAUDIO_LIBRARY NAMES pulse)
ENDIF()

IF(PULSEAUDIO_INCLUDE_DIR AND PULSEAUDIO_LIBRARY)
    SET(PULSEAUDIO_FOUND TRUE)
ELSE()
    SET(PULSEAUDIO_FOUND FALSE)
ENDIF()

IF(PULSEAUDIO_FOUND)
    IF(NOT PULSEAUDIO_FIND_QUIETLY)
        MESSAGE(STATUS "Found PulseAudio: ${PULSEAUDIO_LIBRARY}")
    ENDIF()
ELSE()
    MESSAGE(STATUS "Could NOT find PulseAudio")
ENDIF()

# handle the QUIETLY and REQUIRED arguments and set ZLIB_FOUND to TRUE if
# all listed variables are TRUE
INCLUDE(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(PULSEAUDIO REQUIRED_VARS
    PULSEAUDIO_LIBRARY
    PULSEAUDIO_INCLUDE_DIR
    VERSION_VAR PULSEAUDIO_VERSION_STRING)
