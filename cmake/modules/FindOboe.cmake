#[=======================================================================[.rst:
FindOboe
--------

Finds the Oboe Directory by searching for it in the PA_DIRECTORY, which is the CMAKE_SOURCE_DIR if
not set. You may manually specify the path of the Oboe Directory with the OBOE_DIRECTORY variable.

This module provides the following imported target, if found:
    ``Oboe``

#]=======================================================================]

if (NOT DEFINED PA_DIRECTORY)
    set(PA_DIRECTORY ${CMAKE_SOURCE_DIR})
endif ()

set(OBOE_DIRECTORY ${PA_DIRECTORY}/../../oboe-main)

set(OBOE_INCLUDE_DIR ${OBOE_DIRECTORY}/include)
set(OBOE_BUILD_DIR ${OBOE_DIRECTORY}/build)

set(OBOE_LIBRARY_DIRS ${OBOE_BUILD_DIR}/${ANDROID_ABI})
set(OBOE_LIBRARY ${OBOE_BUILD_DIR}/${ANDROID_ABI}/liboboe.so)

if(OBOE_INCLUDE_DIR)
    # Already in cache, be silent
    set(OBOE_FIND_QUIETLY TRUE)
else()
    find_package(PkgConfig)
    pkg_check_modules(PC_OBOE QUIET Oboe)
endif(OBOE_INCLUDE_DIR)

find_path(OBOE_INCLUDE_DIR
        NAMES oboe/Oboe.h
        DOC "Oboe include directory")

find_library(OBOE_LIBRARY
        NAMES liboboe.so
        HINTS ${OBOE_LIBRARY_DIRS}
        DOC "Oboe Shared Library")

find_library(LOG_LIBRARY log) #used by pa_oboe.cpp and pa_oboe.h as a logging tool

# Handle the QUIETLY and REQUIRED arguments and set OPENSL_FOUND to TRUE if
# all listed variables are TRUE.
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
    Oboe
    DEFAULT_MSG
    OBOE_INCLUDE_DIR
    OBOE_LIBRARY
)

if(OBOE_INCLUDE_DIR AND OBOE_LIBRARY)
    set(OBOE_FOUND TRUE)
    if(NOT TARGET Oboe)
        add_library(Oboe INTERFACE IMPORTED)
        target_include_directories(Oboe INTERFACE "${OBOE_INCLUDE_DIR}")
        target_link_libraries(Oboe INTERFACE ${LOG_LIBRARY})
    endif()
else()
    if (Oboe_FIND_REQUIRED)
        message(FATAL_ERROR "Could NOT find OBOE")
    endif()
endif()