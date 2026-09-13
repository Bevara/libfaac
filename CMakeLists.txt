cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(LIBFAAC_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/enc_aac.c
        ${CMAKE_CURRENT_SOURCE_DIR}/faac_stubs.c
)

SET(LIBFAAC_LIB
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libfaac.a
)

SET(LIBFAAC_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_filter(libfaac
        "${LIBFAAC_SRC}"
        "${LIBFAAC_LIB}"
        []
        ""
        "${LIBFAAC_INC}"
        ""
        "1")
