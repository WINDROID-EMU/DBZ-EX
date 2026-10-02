cmake_minimum_required(VERSION 3.22.1)

project("dbz_native")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/horizon
    ${CMAKE_CURRENT_SOURCE_DIR}/gpu
    ${CMAKE_CURRENT_SOURCE_DIR}/recomp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../recomp_out
)

# Bridge C++ sources
set(BRIDGE_SOURCES
    main_jni.cpp
    horizon/horizon_os.cpp
    gpu/egl_manager.cpp
    gpu/pica_gles.cpp
    gpu/pica_fbo.cpp
    gpu/pica_display_transfer.cpp
    gpu/pica_cmd_processor.cpp
    gpu/rg_etc1.cpp
)

# Recompiled 3DS C code sources (36 code units + entries)
file(GLOB RECOMP_C_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../recomp_out/code*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../recomp_out/entries.c"
)

add_library(dbz_native SHARED
    ${BRIDGE_SOURCES}
    ${RECOMP_C_SOURCES}
)

target_link_libraries(dbz_native
    EGL
    GLESv3
    android
    log
    m
)
