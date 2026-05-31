set(KVAI_INFRA_SOURCES
    cluster_routing.cpp
    config.cpp
    etcd_discovery.cpp
    logging.cpp
    metrics.cpp
    status.cpp
    thread_pool.cpp
    trace_context.cpp)

# spdlog: industrial async logging (fallback implementation when unavailable)
if(KVAI_ENABLE_OPTIONAL_BACKENDS)
    find_package(spdlog CONFIG QUIET)
endif()

if(TARGET spdlog::spdlog)
    target_compile_definitions(kvai_infra PUBLIC KVAI_HAVE_SPDLOG=1)
endif()

# yaml-cpp: proper YAML parsing with nested format support (fallback flat parser when unavailable)
if(KVAI_ENABLE_OPTIONAL_BACKENDS)
    find_package(yaml-cpp CONFIG QUIET)
endif()

if(TARGET yaml-cpp)
    target_compile_definitions(kvai_infra PUBLIC KVAI_HAVE_YAML_CPP=1)
endif()

# etcd-cpp-apiv3: dynamic service discovery (fallback stub compiled when unavailable)
if(KVAI_ENABLE_OPTIONAL_BACKENDS)
    find_package(etcd-cpp-apiv3 CONFIG QUIET)
endif()

if(TARGET etcd-cpp-api)
    target_compile_definitions(kvai_infra PUBLIC KVAI_HAVE_ETCD=1)
endif()

add_library(kvai_infra ${KVAI_INFRA_SOURCES})
add_library(kvai::infra ALIAS kvai_infra)

target_include_directories(kvai_infra PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(kvai_infra PUBLIC kvai_project_options kvai_project_warnings)

# Link conditional libraries
if(TARGET spdlog::spdlog)
    target_link_libraries(kvai_infra PUBLIC spdlog::spdlog)
endif()

if(TARGET yaml-cpp)
    target_link_libraries(kvai_infra PUBLIC yaml-cpp)
endif()

if(TARGET etcd-cpp-api)
    target_link_libraries(kvai_infra PRIVATE etcd-cpp-api)
endif()
