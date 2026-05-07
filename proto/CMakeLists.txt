set(KVAI_VCPKG_PROTOC "")
if(KVAI_ENABLE_OPTIONAL_BACKENDS AND DEFINED VCPKG_TARGET_TRIPLET)
    set(KVAI_VCPKG_PROTOC "${CMAKE_SOURCE_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/tools/protobuf/protoc")
elseif(KVAI_ENABLE_OPTIONAL_BACKENDS AND EXISTS "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-linux/tools/protobuf/protoc")
    set(KVAI_VCPKG_PROTOC "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-linux/tools/protobuf/protoc")
endif()

if(KVAI_VCPKG_PROTOC AND EXISTS "${KVAI_VCPKG_PROTOC}")
    set(Protobuf_PROTOC_EXECUTABLE "${KVAI_VCPKG_PROTOC}" CACHE FILEPATH "Path to protoc" FORCE)
endif()

if(KVAI_ENABLE_OPTIONAL_BACKENDS)
    find_package(Protobuf QUIET)
endif()

if(KVAI_ENABLE_OPTIONAL_BACKENDS AND Protobuf_FOUND AND COMMAND protobuf_generate)
    set(KVAI_PROTO_FILES ${CMAKE_CURRENT_SOURCE_DIR}/search.proto)
    add_library(kvai_proto STATIC ${KVAI_PROTO_FILES})
    add_library(kvai::proto ALIAS kvai_proto)

    target_link_libraries(kvai_proto PUBLIC protobuf::libprotobuf kvai_project_options kvai_project_warnings)
    target_include_directories(kvai_proto PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

    protobuf_generate(
        TARGET kvai_proto
        LANGUAGE cpp)
else()
    add_library(kvai_proto INTERFACE)
    add_library(kvai::proto ALIAS kvai_proto)
    target_include_directories(kvai_proto INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
    message(STATUS "Protobuf generation disabled for fallback-only local builds")
endif()