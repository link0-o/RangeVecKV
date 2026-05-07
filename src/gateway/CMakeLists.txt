add_library(kvai_gateway STATIC auth.cpp brpc_service_stub.cpp http_runtime.cpp interceptors.cpp semantic_search_service.cpp server.cpp)
add_library(kvai::gateway ALIAS kvai_gateway)

target_include_directories(kvai_gateway PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(kvai_gateway PUBLIC kvai_infra kvai_core kvai_ai kvai_search kvai_proto kvai_project_options kvai_project_warnings)

add_executable(kvai_server main.cpp)
target_link_libraries(kvai_server PRIVATE kvai_gateway kvai_infra kvai_core kvai_ai kvai_search kvai_proto)

install(TARGETS kvai_server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})