cmake_minimum_required(VERSION 3.20)

project(FarFileClipboard VERSION 1.0.0 LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "FarFileClipboard is a Windows-only Far Manager plugin")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(FFC_ARCH "x86")
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    if(CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
        set(FFC_ARCH "ARM64")
    else()
        set(FFC_ARCH "x64")
    endif()
else()
    message(FATAL_ERROR "Unsupported pointer size: ${CMAKE_SIZEOF_VOID_P}")
endif()

message(STATUS "FarFileClipboard target architecture: ${FFC_ARCH}")

add_library(FarFileClipboard SHARED
    src/plugin.cpp
    src/far3sdk_min.hpp
    src/messages.hpp
    src/version.rc
)

target_compile_features(FarFileClipboard PRIVATE cxx_std_17)
target_compile_definitions(FarFileClipboard PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0601
    FFC_ARCH_NAME=L"${FFC_ARCH}"
)

if(MSVC)
    target_compile_options(FarFileClipboard PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif()

target_link_libraries(FarFileClipboard PRIVATE shell32 ole32 user32)

set_target_properties(FarFileClipboard PROPERTIES
    PREFIX ""
    OUTPUT_NAME "FarFileClipboard"
)

set(DIST_DIR "${CMAKE_SOURCE_DIR}/dist/FarFileClipboard-${FFC_ARCH}")
set(RUNTIME_FILES
    "${CMAKE_SOURCE_DIR}/FarFileClipboardEng.lng"
    "${CMAKE_SOURCE_DIR}/FarFileClipboardRus.lng"
    "${CMAKE_SOURCE_DIR}/FarFileClipboardEng.hlf"
    "${CMAKE_SOURCE_DIR}/FarFileClipboardRus.hlf"
    "${CMAKE_SOURCE_DIR}/LICENSE"
    "${CMAKE_SOURCE_DIR}/file_id.diz"
)

add_custom_command(TARGET FarFileClipboard POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:FarFileClipboard>" "${DIST_DIR}/FarFileClipboard.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RUNTIME_FILES} "${DIST_DIR}"
    COMMENT "Preparing dist/FarFileClipboard-${FFC_ARCH}"
)
