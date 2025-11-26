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

if(NOT DEFINED OBOE_DIRECTORY)
    #Insert the path of the directory where you cloned Oboe, i.e. ${CMAKE_CURRENT_SOURCE_DIR}/../oboe
    set(OBOE_DIRECTORY FALSE)
endif()

if(NOT OBOE_DIRECTORY)
    message(AUTHOR_WARNING
            "If you're trying to use Oboe as a Host API, please specify the directory where you "
            "cloned its repository. For further information, please read src/hostapi/oboe/README.md"
            )
    set(OBOE_FOUND FALSE)
else()
    if(NOT DEFINED OBOE_INCLUDE_DIR)
        set(OBOE_INCLUDE_DIR ${OBOE_DIRECTORY}/include)
    endif()

    if(NOT DEFINED OBOE_LIBRARY_DIRS)
        set(OBOE_LIBRARY_DIRS ${OBOE_DIRECTORY}/build/${ANDROID_ABI})
    endif()
    set(OBOE_LIBRARIES ${OBOE_LIBRARY_DIRS}/liboboe.so)

    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(OBOE Oboe)
    else()
        find_library(OBOE_LIBRARIES
                NAMES liboboe.so
                HINTS ${OBOE_LIBRARY_DIRS}
                DOC "Oboe Library"
                )
        find_path(OBOE_INCLUDE_DIR
                NAMES oboe/Oboe.h
                DOC "Oboe header"
                )
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
endif()
