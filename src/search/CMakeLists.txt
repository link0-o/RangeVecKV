add_library(kvai_search STATIC vector_index.cpp)
add_library(kvai::search ALIAS kvai_search)

set(KVAI_VCPKG_SHARE_DIR "")
if(KVAI_ENABLE_OPTIONAL_BACKENDS AND DEFINED VCPKG_TARGET_TRIPLET)
	set(KVAI_VCPKG_SHARE_DIR "${CMAKE_SOURCE_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/share")
elseif(KVAI_ENABLE_OPTIONAL_BACKENDS AND EXISTS "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-linux/share")
	set(KVAI_VCPKG_SHARE_DIR "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-linux/share")
endif()

find_package(OpenMP QUIET COMPONENTS CXX)
if(KVAI_ENABLE_OPTIONAL_BACKENDS)
	find_package(faiss CONFIG QUIET HINTS ${KVAI_VCPKG_SHARE_DIR}/faiss)
endif()

target_include_directories(kvai_search PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(kvai_search PUBLIC kvai_infra kvai_core kvai_project_options kvai_project_warnings)

if(TARGET faiss)
	target_compile_definitions(kvai_search PRIVATE KVAI_HAVE_FAISS=1)
	target_link_libraries(kvai_search PRIVATE faiss)
endif()