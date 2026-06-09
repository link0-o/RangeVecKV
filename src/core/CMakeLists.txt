add_library(kvai_core STATIC document.cpp kv_store.cpp persistence_codec.cpp)
add_library(kvai::core ALIAS kvai_core)

set(KVAI_VCPKG_SHARE_DIR "")
if(KVAI_ENABLE_OPTIONAL_BACKENDS AND DEFINED VCPKG_TARGET_TRIPLET)
	set(KVAI_VCPKG_SHARE_DIR "${CMAKE_SOURCE_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/share")
elseif(KVAI_ENABLE_OPTIONAL_BACKENDS AND EXISTS "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-linux/share")
	set(KVAI_VCPKG_SHARE_DIR "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-linux/share")
endif()

if(KVAI_ENABLE_OPTIONAL_BACKENDS)
	find_package(RocksDB CONFIG QUIET HINTS ${KVAI_VCPKG_SHARE_DIR}/rocksdb)
endif()

target_include_directories(kvai_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(kvai_core PUBLIC kvai_infra kvai_proto kvai_project_options kvai_project_warnings)

if(TARGET RocksDB::rocksdb)
    target_compile_definitions(kvai_core PRIVATE KVAI_HAVE_ROCKSDB=1)
    target_compile_features(kvai_core PRIVATE cxx_std_20)
    target_link_libraries(kvai_core PRIVATE RocksDB::rocksdb)
endif()
