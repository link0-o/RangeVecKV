add_library(kvai_ai STATIC embedding_service.cpp)
add_library(kvai::ai ALIAS kvai_ai)

set(KVAI_VCPKG_SHARE_DIR "")
if(KVAI_ENABLE_OPTIONAL_BACKENDS AND DEFINED VCPKG_TARGET_TRIPLET)
	set(KVAI_VCPKG_SHARE_DIR "${CMAKE_SOURCE_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/share")
elseif(KVAI_ENABLE_OPTIONAL_BACKENDS AND EXISTS "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-linux/share")
	set(KVAI_VCPKG_SHARE_DIR "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-linux/share")
endif()

if(KVAI_ENABLE_OPTIONAL_BACKENDS)
	find_package(onnxruntime CONFIG QUIET HINTS ${KVAI_VCPKG_SHARE_DIR}/onnxruntime)
endif()

target_include_directories(kvai_ai PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_include_directories(kvai_ai SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party/stb)
target_link_libraries(kvai_ai PUBLIC kvai_infra kvai_project_options kvai_project_warnings)

if(TARGET onnxruntime::onnxruntime)
	target_compile_definitions(kvai_ai PRIVATE KVAI_HAVE_ONNXRUNTIME=1)
	target_link_libraries(kvai_ai PRIVATE onnxruntime::onnxruntime)
endif()
