# Base gateway library: always compiled
set(KVAI_GATEWAY_SOURCES
    auth.cpp
    http_runtime.cpp
    interceptors.cpp
    json_helpers.cpp
    semantic_search_service.cpp
    server.cpp)

# nlohmann-json: proper JSON serialization/deserialization
find_package(nlohmann_json CONFIG REQUIRED)

# BRPC: industrial HTTP+RPC server (fallback HTTP runtime when unavailable)
set(KVAI_GATEWAY_BRPC_SOURCES "")
if(KVAI_ENABLE_OPTIONAL_BACKENDS)
    find_package(brpc CONFIG QUIET)
endif()

if(TARGET brpc)
    set(KVAI_GATEWAY_BRPC_SOURCES
        brpc_http_service.cpp
        brpc_runtime.cpp)
    # Remove the stub when real BRPC service is available
    list(REMOVE_ITEM KVAI_GATEWAY_SOURCES brpc_service_stub.cpp)
else()
    # Keep the stub for fallback builds without BRPC
    list(APPEND KVAI_GATEWAY_SOURCES brpc_service_stub.cpp)
endif()

add_library(kvai_gateway STATIC ${KVAI_GATEWAY_SOURCES} ${KVAI_GATEWAY_BRPC_SOURCES})
add_library(kvai::gateway ALIAS kvai_gateway)

target_include_directories(kvai_gateway PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(kvai_gateway PUBLIC kvai_infra kvai_core kvai_ai kvai_search kvai_proto kvai_project_options kvai_project_warnings)
target_link_libraries(kvai_gateway PUBLIC nlohmann_json::nlohmann_json)

if(TARGET brpc)
    target_compile_definitions(kvai_gateway PUBLIC KVAI_HAVE_BRPC=1)
    target_link_libraries(kvai_gateway PRIVATE brpc)
endif()

add_executable(kvai_server main.cpp)
target_link_libraries(kvai_server PRIVATE kvai_gateway kvai_infra kvai_core kvai_ai kvai_search kvai_proto)

install(TARGETS kvai_server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
