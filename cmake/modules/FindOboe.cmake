#[=======================================================================[.rst:
FindOboe
--------

Finds the Oboe Directory by searching for it in the PA_DIRECTORY, which is the CMAKE_SOURCE_DIR if
not set. You may manually specify the path of the Oboe Directory with the OBOE_DIRECTORY variable.

This module provides the following imported target, if found:
    ``Oboe``

#]=======================================================================]

if (NOT DEFINED PA_DIRECTORY)
    set(PA_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif ()

MESSAGE("Searching for Oboe...")

set(OBOE_DIRECTORY ${PA_DIRECTORY}/../../oboe)

set(OBOE_INCLUDE_DIR ${OBOE_DIRECTORY}/include)
set(OBOE_BUILD_DIR ${OBOE_DIRECTORY}/build)

set(OBOE_LIBRARY_DIR ${OBOE_DIRECTORY}/lib)
set(OBOE_LINK_LIBRARIES ${OBOE_LIBRARY_DIR}/liboboe.so)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(OBOE Oboe)
else()
    find_library(OBOE_LINK_LIBRARIES
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
list(APPEND OBOE_LINK_LIBRARIES ${LOG_LIBRARY})


include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
    Oboe
    DEFAULT_MSG
    OBOE_LINK_LIBRARIES
    OBOE_INCLUDE_DIR
)

if(OBOE_INCLUDE_DIR AND OBOE_LINK_LIBRARIES)
    set(OBOE_FOUND TRUE)
    if(NOT TARGET Oboe)
        add_library(Oboe INTERFACE IMPORTED)
        target_link_libraries(Oboe INTERFACE "${OBOE_LINK_LIBRARIES}")
        target_include_directories(Oboe INTERFACE "${OBOE_INCLUDE_DIR}")
    endif()
endif()