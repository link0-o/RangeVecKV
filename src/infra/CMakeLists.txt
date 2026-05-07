add_library(
    kvai_infra
    cluster_routing.cpp
    config.cpp
    logging.cpp
    metrics.cpp
    status.cpp
    thread_pool.cpp
    trace_context.cpp)

add_library(kvai::infra ALIAS kvai_infra)

target_include_directories(kvai_infra PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(kvai_infra PUBLIC kvai_project_options kvai_project_warnings)