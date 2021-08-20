#.rst:
# FindPulseAudio
# --------------
#
# This is base on
# https://raw.githubusercontent.com/KDE/extra-cmake-modules/master/find-modules/FindPulseAudio.cmake
#
# Try to locate the PulseAudio library.
# If found, this will define the following variables:
#
# ``PULSEAUDIO_FOUND``
#      True if the system has the PulseAudio library of at least
#      the minimum version specified by either the version parameter
#      to find_package() or the variable PULSEAUDIO_MINIMUM_VERSION
# ``PULSEAUDIO_INCLUDE_DIRS``
#      The PulseAudio include directory
# ``PULSEAUDIO_LIBRARIES``
#      The PulseAudio libraries for linking
# ``PULSEAUDIO_MAINLOOP_LIBRARY``
#      The libraries needed to use PulseAudio Mainloop
# ``PULSEAUDIO_VERSION``
#      The version of PulseAudio that was found
# ``PULSEAUDIO_INCLUDE_DIR``
#     Deprecated, use ``PULSEAUDIO_INCLUDE_DIRS``
# ``PULSEAUDIO_LIBRARY``
#     Deprecated, use ``PULSEAUDIO_LIBRARIES``
#
# If ``PULSEAUDIO_FOUND`` is TRUE, it will also define the following
# imported target:
#
# ``PulseAudio::PulseAudio``
#     The PulseAudio library
#
# Since 5.41.0.

#=============================================================================
# SPDX-FileCopyrightText: 2008 Matthias Kretz <kretz@kde.org>
# SPDX-FileCopyrightText: 2009 Marcus Hufgard <Marcus.Hufgard@hufgard.de>
#
# SPDX-License-Identifier: BSD-3-Clause
#=============================================================================

# Support PULSEAUDIO_MINIMUM_VERSION for compatibility:
if(NOT PULSEAUDIO_FIND_VERSION)
  set(PULSEAUDIO_FIND_VERSION "${PULSEAUDIO_MINIMUM_VERSION}")
endif()

# the minimum version of PulseAudio we require
if(NOT PULSEAUDIO_FIND_VERSION)
  set(PULSEAUDIO_FIND_VERSION "1.0.0")
endif()

find_package(PkgConfig)
pkg_check_modules(PC_PulseAudio QUIET libpulse>=${PULSEAUDIO_FIND_VERSION})
pkg_check_modules(PC_PULSEAUDIO_MAINLOOP QUIET libpulse-mainloop-glib)

find_path(PULSEAUDIO_INCLUDE_DIRS pulse/pulseaudio.h
  HINTS
  ${PC_PULSEAUDIO_INCLUDEDIR}
  ${PC_PULSEAUDIO_INCLUDE_DIRS}
  )

find_library(PULSEAUDIO_LIBRARIES NAMES pulse libpulse
  HINTS
  ${PC_PULSEAUDIO_LIBDIR}
  ${PC_PULSEAUDIO_LIBRARY_DIRS}
  )

find_library(PULSEAUDIO_MAINLOOP_LIBRARY
  NAMES pulse-mainloop pulse-mainloop-glib libpulse-mainloop-glib
  HINTS
  ${PC_PULSEAUDIO_LIBDIR}
  ${PC_PULSEAUDIO_LIBRARY_DIRS}
  )

# Store the version number in the cache,
# so we don't have to search every time again:
if(PULSEAUDIO_INCLUDE_DIRS AND NOT PULSEAUDIO_VERSION)

  # get PulseAudio's version from its version.h
  file(STRINGS "${PULSEAUDIO_INCLUDE_DIRS}/pulse/version.h" pulse_version_h
    REGEX ".*pa_get_headers_version\\(\\).*")
  string(REGEX REPLACE ".*pa_get_headers_version\\(\\)\ \\(\"([0-9]+\\.[0-9]+\\.[0-9]+)[^\"]*\"\\).*" "\\1"
    _PULSEAUDIO_VERSION "${pulse_version_h}")

  set(PULSEAUDIO_VERSION "${_PULSEAUDIO_VERSION}"
    CACHE STRING "Version number of PulseAudio"
    FORCE)
endif()

# Use the new extended syntax of
# find_package_handle_standard_args(),
# which also handles version checking:
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(PULSEAUDIO
  REQUIRED_VARS PULSEAUDIO_LIBRARIES
  PULSEAUDIO_INCLUDE_DIRS
  VERSION_VAR PULSEAUDIO_VERSION)

# Deprecated synonyms
set(PULSEAUDIO_INCLUDE_DIR "${PULSEAUDIO_INCLUDE_DIRS}")
set(PULSEAUDIO_LIBRARY "${PULSEAUDIO_LIBRARIES}")
set(PULSEAUDIO_MAINLOOP_LIBRARY "${PULSEAUDIO_MAINLOOP_LIBRARY}")
set(PULSEAUDIO_FOUND "${PULSEAUDIO_FOUND}")

if(PULSEAUDIO_FOUND AND NOT TARGET PulseAudio::PulseAudio)
  add_library(PulseAudio::PulseAudio UNKNOWN IMPORTED)
  set_target_properties(PulseAudio::PulseAudio PROPERTIES
    IMPORTED_LOCATION "${PULSEAUDIO_LIBRARIES}"
    INTERFACE_INCLUDE_DIRECTORIES "${PULSEAUDIO_INCLUDE_DIRS}")
endif()

mark_as_advanced(PULSEAUDIO_INCLUDE_DIRS PULSEAUDIO_INCLUDE_DIR
  PULSEAUDIO_LIBRARIES PULSEAUDIO_LIBRARY
  PULSEAUDIO_MAINLOOP_LIBRARY PULSEAUDIO_MAINLOOP_LIBRARY)

include(FeatureSummary)
set_package_properties(PulseAudio PROPERTIES
  URL "https://www.freedesktop.org/wiki/Software/PulseAudio"
  DESCRIPTION "Sound server, for sound stream routing and mixing")
