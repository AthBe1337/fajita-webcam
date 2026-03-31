cmake_minimum_required(VERSION 3.16)

project(fajita-webcam LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(JPEG REQUIRED)

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

add_executable(fajita-webcam
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

target_include_directories(fajita-webcam PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_options(fajita-webcam PRIVATE -O2 -Wall -Wextra)

target_link_libraries(fajita-webcam PRIVATE
    JPEG::JPEG
    Threads::Threads
)
