cmake_minimum_required(VERSION 3.24)

project(
    RangeVecKV
    VERSION 0.1.0
    DESCRIPTION "Distributed KV plus semantic retrieval engine"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(CTest)
include(CheckIPOSupported)
include(GNUInstallDirs)

option(KVAI_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(KVAI_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(KVAI_ENABLE_IPO "Enable interprocedural optimization for release-like builds" ON)
option(KVAI_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(KVAI_ENABLE_PACKAGING "Enable installation and CPack packaging" ON)

if(DEFINED CMAKE_TOOLCHAIN_FILE)
    option(KVAI_ENABLE_OPTIONAL_BACKENDS "Enable ONNX Runtime, FAISS, and RocksDB backends when available" ON)
else()
    option(KVAI_ENABLE_OPTIONAL_BACKENDS "Enable ONNX Runtime, FAISS, and RocksDB backends when available" OFF)
endif()

if(KVAI_ENABLE_ASAN AND KVAI_ENABLE_TSAN)
    message(FATAL_ERROR "KVAI_ENABLE_ASAN and KVAI_ENABLE_TSAN are mutually exclusive")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O0 -g3")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -O2 -g -DNDEBUG")
set(CMAKE_CXX_FLAGS_MINSIZEREL "${CMAKE_CXX_FLAGS_MINSIZEREL} -Os -DNDEBUG")

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(KVAI_WARNINGS_AS_ERRORS)
        add_compile_options(-Werror)
    endif()
endif()

add_library(kvai_project_options INTERFACE)
add_library(kvai::project_options ALIAS kvai_project_options)

add_library(kvai_project_warnings INTERFACE)
add_library(kvai::project_warnings ALIAS kvai_project_warnings)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(
        kvai_project_warnings
        INTERFACE
            -Wall
            -Wextra
            -Wpedantic)

    if(KVAI_WARNINGS_AS_ERRORS)
        target_compile_options(kvai_project_warnings INTERFACE -Werror)
    endif()
endif()

if(KVAI_ENABLE_ASAN AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(kvai_project_options INTERFACE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(kvai_project_options INTERFACE -fsanitize=address)
endif()

if(KVAI_ENABLE_TSAN AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(kvai_project_options INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(kvai_project_options INTERFACE -fsanitize=thread)
endif()

check_ipo_supported(RESULT kvai_ipo_supported OUTPUT kvai_ipo_error)
if(KVAI_ENABLE_IPO AND kvai_ipo_supported)
    set_property(TARGET kvai_project_options PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
elseif(KVAI_ENABLE_IPO AND NOT kvai_ipo_supported)
    message(WARNING "IPO/LTO disabled: ${kvai_ipo_error}")
endif()

if(DEFINED CMAKE_TOOLCHAIN_FILE)
    message(STATUS "Using toolchain file: ${CMAKE_TOOLCHAIN_FILE}")
else()
    message(STATUS "Configure with the vcpkg toolchain file to enable manifest mode dependency resolution")
endif()

set(KVAI_VCPKG_PREFIX "")
if(DEFINED VCPKG_TARGET_TRIPLET)
    set(KVAI_VCPKG_PREFIX "${CMAKE_SOURCE_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}")
elseif(EXISTS "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-linux")
    set(KVAI_VCPKG_PREFIX "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-linux")
endif()

if(KVAI_ENABLE_OPTIONAL_BACKENDS AND KVAI_VCPKG_PREFIX)
    list(PREPEND CMAKE_PREFIX_PATH ${KVAI_VCPKG_PREFIX} ${KVAI_VCPKG_PREFIX}/share)
    if(EXISTS "${KVAI_VCPKG_PREFIX}/share/onnxruntime/onnxruntimeConfig.cmake")
        set(onnxruntime_DIR "${KVAI_VCPKG_PREFIX}/share/onnxruntime" CACHE PATH "Path to onnxruntime CMake config" FORCE)
    endif()
    if(EXISTS "${KVAI_VCPKG_PREFIX}/share/rocksdb/RocksDBConfig.cmake")
        set(RocksDB_DIR "${KVAI_VCPKG_PREFIX}/share/rocksdb" CACHE PATH "Path to RocksDB CMake config" FORCE)
    endif()
    if(EXISTS "${KVAI_VCPKG_PREFIX}/share/faiss/faiss-config.cmake")
        set(faiss_DIR "${KVAI_VCPKG_PREFIX}/share/faiss" CACHE PATH "Path to faiss CMake config" FORCE)
    endif()
endif()

link_directories(${CMAKE_BINARY_DIR}/src/core ${CMAKE_BINARY_DIR}/src/ai)

set(KVAI_PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(KVAI_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(KVAI_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/test")

foreach(subdir IN ITEMS proto src)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${subdir}/CMakeLists.txt")
        add_subdirectory(${subdir})
    endif()
endforeach()

if(BUILD_TESTING AND EXISTS "${KVAI_TEST_DIR}/CMakeLists.txt")
    add_subdirectory(test)
endif()

if(KVAI_ENABLE_PACKAGING)
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/config DESTINATION ${CMAKE_INSTALL_DATADIR}/rangeveckv)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/docs")
        install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/docs DESTINATION ${CMAKE_INSTALL_DATADIR}/rangeveckv)
    endif()
    install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/README.md
        ${CMAKE_CURRENT_SOURCE_DIR}/README.zh-CN.md
        ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
        DESTINATION ${CMAKE_INSTALL_DOCDIR})

    set(CPACK_PACKAGE_NAME "rangeveckv")
    set(CPACK_PACKAGE_VENDOR "RangeVecKV")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Distributed KV plus semantic retrieval engine")
    set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
    set(CPACK_PACKAGE_FILE_NAME "rangeveckv-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
    set(CPACK_SOURCE_GENERATOR "TGZ")
    set(CPACK_GENERATOR "TGZ")
    include(CPack)
endif()
