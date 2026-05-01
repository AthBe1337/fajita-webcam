cmake_minimum_required(VERSION 3.16)

project(fajita-webcam LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(JPEG REQUIRED)
find_package(Vulkan)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    add_compile_options(-march=armv8-a+simd)
endif()

# --- Frontend build (Vue 3 + Vite) ---
set(FRONTEND_DIR "${CMAKE_SOURCE_DIR}/frontend")
set(FRONTEND_DIST "${CMAKE_SOURCE_DIR}/static")
set(FRONTEND_STAMP "${CMAKE_BINARY_DIR}/frontend.stamp")

find_program(NPM_EXECUTABLE npm)

file(GLOB_RECURSE FRONTEND_SOURCES CONFIGURE_DEPENDS
    "${FRONTEND_DIR}/src/*"
    "${FRONTEND_DIR}/index.html"
    "${FRONTEND_DIR}/vite.config.js"
    "${FRONTEND_DIR}/package.json"
)

if(NPM_EXECUTABLE)
    # Install dependencies if node_modules is missing
    if(NOT EXISTS "${FRONTEND_DIR}/node_modules")
        message(STATUS "Running npm install...")
        execute_process(
            COMMAND ${NPM_EXECUTABLE} install
            WORKING_DIRECTORY "${FRONTEND_DIR}"
            RESULT_VARIABLE NPM_INSTALL_RESULT
        )
        if(NOT NPM_INSTALL_RESULT EQUAL 0)
            message(FATAL_ERROR "npm install failed")
        endif()
    endif()

    add_custom_command(
        OUTPUT "${FRONTEND_STAMP}"
        COMMAND ${NPM_EXECUTABLE} run build
        COMMAND ${CMAKE_COMMAND} -E touch "${FRONTEND_STAMP}"
        WORKING_DIRECTORY "${FRONTEND_DIR}"
        DEPENDS ${FRONTEND_SOURCES}
        COMMENT "Building frontend (npm run build)"
        VERBATIM
    )
else()
    message(WARNING "npm not found — frontend will not be rebuilt. Pre-built static/ files will be used.")
    # Create stamp so downstream targets don't fail
    if(NOT EXISTS "${FRONTEND_STAMP}")
        file(TOUCH "${FRONTEND_STAMP}")
    endif()
endif()

# --- Embed static assets into C++ ---
file(GLOB_RECURSE STATIC_FILES CONFIGURE_DEPENDS "${FRONTEND_DIST}/*")

set(GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(EMBEDDED_ASSETS_CPP "${GENERATED_DIR}/embedded_assets.cpp")

add_custom_command(
    OUTPUT "${EMBEDDED_ASSETS_CPP}"
    COMMAND ${CMAKE_COMMAND}
            -DSTATIC_ROOT=${FRONTEND_DIST}
            -DOUTPUT_CPP=${EMBEDDED_ASSETS_CPP}
            -DHEADER_PATH=${CMAKE_SOURCE_DIR}/src/embedded_assets.h
            -P ${CMAKE_SOURCE_DIR}/cmake/GenerateEmbeddedAssets.cmake
    DEPENDS
        ${STATIC_FILES}
        ${FRONTEND_STAMP}
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

# --- GPU ISP (Vulkan compute) ---
set(SHADER_GENERATED_DIR "${GENERATED_DIR}/shaders")

if(Vulkan_FOUND)
    find_program(GLSLC_EXECUTABLE NAMES glslc)
    if(NOT GLSLC_EXECUTABLE)
        message(STATUS "Vulkan ISP disabled (glslc not found in PATH)")
        set(VULKAN_ISP_ENABLED FALSE)
    else()
        set(VULKAN_ISP_ENABLED TRUE)
    endif()
else()
    message(STATUS "Vulkan ISP disabled (Vulkan SDK not found)")
    set(VULKAN_ISP_ENABLED FALSE)
endif()

if(VULKAN_ISP_ENABLED)
    file(MAKE_DIRECTORY "${SHADER_GENERATED_DIR}")

    set(SHADER_SOURCES
        unpack.comp
        demosaic.comp
    )
    set(SHADER_HEADERS "")
    foreach(SHADER ${SHADER_SOURCES})
        set(SHADER_SRC "${CMAKE_SOURCE_DIR}/src/shaders/${SHADER}")
        set(SHADER_OUT "${SHADER_GENERATED_DIR}/${SHADER}.h")
        add_custom_command(
            OUTPUT "${SHADER_OUT}"
            COMMAND ${GLSLC_EXECUTABLE}
                    -O
                    --target-env=vulkan1.0
                    -mfmt=c
                    "${SHADER_SRC}"
                    -o "${SHADER_OUT}"
            DEPENDS "${SHADER_SRC}"
            COMMENT "glslc ${SHADER} → SPIR-V C array"
            VERBATIM
        )
        list(APPEND SHADER_HEADERS "${SHADER_OUT}")
    endforeach()

    list(APPEND SOURCES src/vulkan_isp.cpp ${SHADER_HEADERS})
    add_definitions(-DHAS_VULKAN_ISP=1)
    message(STATUS "Vulkan ISP enabled (glslc: ${GLSLC_EXECUTABLE})")
else()
    add_definitions(-DHAS_VULKAN_ISP=0)
endif()

add_executable(fajita-webcam ${SOURCES})

target_include_directories(fajita-webcam PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${GENERATED_DIR}
)

target_compile_options(fajita-webcam PRIVATE -O2 -Wall -Wextra)

target_link_libraries(fajita-webcam PRIVATE
    JPEG::JPEG
    Threads::Threads
)

if(VULKAN_ISP_ENABLED)
    target_link_libraries(fajita-webcam PRIVATE Vulkan::Vulkan)
endif()
