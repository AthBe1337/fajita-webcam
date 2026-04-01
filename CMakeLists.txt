cmake_minimum_required(VERSION 3.16)

project(fajita-webcam LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(JPEG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(EGL IMPORTED_TARGET egl)
pkg_check_modules(GLESV2 IMPORTED_TARGET glesv2)
pkg_check_modules(GBM IMPORTED_TARGET gbm)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    add_compile_options(-march=armv8-a+simd)
endif()

file(GLOB_RECURSE STATIC_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/static/*")

set(GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(EMBEDDED_ASSETS_CPP "${GENERATED_DIR}/embedded_assets.cpp")

add_custom_command(
    OUTPUT "${EMBEDDED_ASSETS_CPP}"
    COMMAND ${CMAKE_COMMAND}
            -DSTATIC_ROOT=${CMAKE_SOURCE_DIR}/static
            -DOUTPUT_CPP=${EMBEDDED_ASSETS_CPP}
            -DHEADER_PATH=${CMAKE_SOURCE_DIR}/src/embedded_assets.h
            -P ${CMAKE_SOURCE_DIR}/cmake/GenerateEmbeddedAssets.cmake
    DEPENDS
        ${STATIC_FILES}
        ${CMAKE_SOURCE_DIR}/cmake/GenerateEmbeddedAssets.cmake
        ${CMAKE_SOURCE_DIR}/src/embedded_assets.h
    VERBATIM
    COMMENT "Embedding static web assets"
)

set(SOURCES
    src/autoexposure.cpp
    src/autofocus.cpp
    src/autowhitebalance.cpp
    src/camera.cpp
    src/http.cpp
    src/isp.cpp
    src/main.cpp
    src/pipeline.cpp
    src/stream.cpp
    ${EMBEDDED_ASSETS_CPP}
)

if(EGL_FOUND AND GLESV2_FOUND AND GBM_FOUND)
    list(APPEND SOURCES src/gpu_isp.cpp)
    add_definitions(-DHAS_GPU_ISP=1)
    message(STATUS "GPU ISP enabled (EGL + GLES + GBM found)")
else()
    add_definitions(-DHAS_GPU_ISP=0)
    message(STATUS "GPU ISP disabled (missing EGL/GLES/GBM)")
endif()

add_executable(fajita-webcam ${SOURCES})

target_include_directories(fajita-webcam PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_options(fajita-webcam PRIVATE -O2 -Wall -Wextra)

target_link_libraries(fajita-webcam PRIVATE
    JPEG::JPEG
    Threads::Threads
)

if(EGL_FOUND AND GLESV2_FOUND AND GBM_FOUND)
    target_link_libraries(fajita-webcam PRIVATE
        PkgConfig::EGL
        PkgConfig::GLESV2
        PkgConfig::GBM
    )
endif()
