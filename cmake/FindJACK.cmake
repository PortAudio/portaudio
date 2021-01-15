#[=======================================================================[.rst:
FindJACK
--------

Finds the JACK Audio Connection Kit library.

Imported Targets
^^^^^^^^^^^^^^^^

This module provides the following imported targets, if found:

``JACK::jack``
  The JACK library

Result Variables
^^^^^^^^^^^^^^^^

This will define the following variables:

``JACK_FOUND``
  True if the system has the JACK library.
``JACK_INCLUDE_DIRS``
  Include directories needed to use JACK.
``JACK_LIBRARIES``
  Libraries needed to link to JACK.

Cache Variables
^^^^^^^^^^^^^^^

The following cache variables may also be set:

``JACK_INCLUDE_DIR``
  The directory containing ``jack.h``.
``JACK_LIBRARY``
  The path to the JACK library.

#]=======================================================================]

find_path(JACK_INCLUDE_DIR
  NAMES jack/jack.h
  DOC "JACK include directory")
mark_as_advanced(JACK_INCLUDE_DIR)

find_library(JACK_LIBRARY
  NAMES jack
  DOC "JACK library"
)
mark_as_advanced(JACK_LIBRARY)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  JACK
  DEFAULT_MSG
  JACK_LIBRARY
  JACK_INCLUDE_DIR
)

if(JACK_FOUND)
  set(JACK_LIBRARIES "${JACK_LIBRARY}")
  set(JACK_INCLUDE_DIRS "${JACK_INCLUDE_DIR}")

  if(NOT TARGET JACK::jack)
    add_library(JACK::jack UNKNOWN IMPORTED)
    set_target_properties(JACK::jack
      PROPERTIES
        IMPORTED_LOCATION "${JACK_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${JACK_INCLUDE_DIR}"
    )
  endif()
endif()
