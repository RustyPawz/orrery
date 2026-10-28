# ORY-1's native parts for VST3 hosting (Windows x64):
#   ory1-vsthost.exe   the helper process that hosts plugins
#   vstbridge.node     the Node-API addon the desktop app's preload loads
# Build with:  npm run native:build
cmake_minimum_required(VERSION 3.25)

set(SMTG_USE_STATIC_CRT ON CACHE BOOL "" FORCE)
set(vst3sdk_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vst3sdk")
list(APPEND CMAKE_MODULE_PATH "${vst3sdk_SOURCE_DIR}/cmake/modules")
include(SMTG_VST3_SDK)

project(ory1_native LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
smtg_setup_platform_toolset()

set(SDK_ROOT "${vst3sdk_SOURCE_DIR}")
set(public_sdk_SOURCE_DIR "${SDK_ROOT}/public.sdk")
set(pluginterfaces_SOURCE_DIR "${SDK_ROOT}/pluginterfaces")
include_directories(${SDK_ROOT})
smtg_create_lib_base_target()
smtg_create_pluginterfaces_target()
smtg_create_public_sdk_common_target()
smtg_create_public_sdk_hosting_target()

add_executable(ory1-vsthost
    host/main.cpp
    ${SDK_ROOT}/public.sdk/source/common/memorystream.cpp
    ${SDK_ROOT}/public.sdk/source/vst/hosting/module_win32.cpp
    ${SDK_ROOT}/public.sdk/source/vst/hosting/plugprovider.cpp
)
target_link_libraries(ory1-vsthost PRIVATE sdk_hosting avrt ole32)
target_compile_definitions(ory1-vsthost PRIVATE UNICODE _UNICODE)
target_link_options(ory1-vsthost PRIVATE /SUBSYSTEM:CONSOLE)

# Node-API comes from the running executable (Electron), not from a node.lib: make an import library for "node.exe"
# from node-api-headers' symbol list and delay-load it, so delayhook.cpp can redirect it.
set(NODE_API_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/../node_modules/node-api-headers")
set(NODE_API_LIB "${CMAKE_CURRENT_BINARY_DIR}/node_api.lib")
add_custom_command(OUTPUT ${NODE_API_LIB}
    COMMAND ${CMAKE_AR} /def:${NODE_API_HEADERS}/def/node_api.def /out:${NODE_API_LIB} /machine:x64 /name:node.exe /nologo
    DEPENDS ${NODE_API_HEADERS}/def/node_api.def)
add_custom_target(node_api_lib DEPENDS ${NODE_API_LIB})

add_library(vstbridge SHARED bridge/bridge.cpp bridge/delayhook.cpp)
add_dependencies(vstbridge node_api_lib)
set_target_properties(vstbridge PROPERTIES PREFIX "" SUFFIX ".node")
target_include_directories(vstbridge PRIVATE ${NODE_API_HEADERS}/include)
target_compile_definitions(vstbridge PRIVATE NODE_GYP_MODULE_NAME=vstbridge NAPI_VERSION=8)
target_link_libraries(vstbridge PRIVATE ${NODE_API_LIB} delayimp winmm)
target_link_options(vstbridge PRIVATE /DELAYLOAD:node.exe)

# both land in native/bin, where the desktop app looks for them while developing and electron-builder copies them from
foreach(t ory1-vsthost vstbridge)
    set_target_properties(${t} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/bin"
        LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/bin")
endforeach()
