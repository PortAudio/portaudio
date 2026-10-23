#[=======================================================================[.rst:
Findoboe
--------

Finds the oboe library. OBOE_DIRECTORY has to be set to the path of the directory where
the oboe repository was cloned (see src/hostapi/oboe/README.md for more information).

Imported Targets
^^^^^^^^^^^^^^^^

This module provides the following imported target, if found:

``Oboe::oboe``
  The OBOE library

#]=======================================================================]

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(OBOE Oboe)
else()
    if(NOT OBOE_LIBRARIES)
        find_library(OBOE_LIBRARIES
                NAMES oboe
                HINTS ${OBOE_LIBRARY_DIRS}
                DOC "Oboe Library"
                )
    endif()
    if(NOT OBOE_INCLUDE_DIR)
        find_path(OBOE_INCLUDE_DIR
                NAMES oboe/Oboe.h
                DOC "Oboe header"
                )
    endif()
endif()

find_library(LOG_LIBRARY log) #used by pa_oboe.cpp and pa_oboe.h as a logging tool

set(OBOE_LINK_LIBRARIES ${OBOE_LIBRARIES} ${LOG_LIBRARY})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
        Oboe
        DEFAULT_MSG
        OBOE_LINK_LIBRARIES
        OBOE_INCLUDE_DIR
)

if(OBOE_INCLUDE_DIR AND OBOE_LINK_LIBRARIES)
    set(OBOE_FOUND TRUE)
    if(NOT TARGET Oboe::oboe)
        add_library(Oboe::oboe INTERFACE IMPORTED GLOBAL)
        target_link_libraries(Oboe::oboe INTERFACE "${OBOE_LINK_LIBRARIES}")
        target_include_directories(Oboe::oboe INTERFACE "${OBOE_INCLUDE_DIR}")
    endif()
endif()
