cmake_minimum_required(VERSION 3.24)
project(Helm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "Windows-only target (Win32 + WebView2).")
endif()
enable_language(RC)

add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

# ---------------------------------------------------------------- llama.cpp
# Pinned. Updating is a deliberate choice: change the tag, rebuild, fix engine/ if llama.h moved.
include(FetchContent)

# llama.cpp is C++17 on purpose ("# don't bump" on its target_compile_features
# line). CMAKE_CXX_STANDARD propagates into FetchContent subprojects, and MSVC
# reports __cplusplus as 199711L unless /Zc:__cplusplus is passed — so the LU8
# guard in llama-chat.cpp does not fire and its u8"" literals stay char8_t.
# Build the dependency at 17. Helm's own targets go back to 20 below.
set(CMAKE_CXX_STANDARD 17)

# Static. llama.cpp defaults BUILD_SHARED_LIBS to ON under MSVC and emits its
# DLLs into ${CMAKE_BINARY_DIR}/bin rather than next to Helm.exe, so a shared
# build produces an executable that cannot start. Static also matches the
# one-process/one-binary rule in the build spec.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_COMMON   OFF CACHE BOOL "" FORCE)
# CUDA. Defaults only — anything passed with -D at configure time wins.
#
# These have to be resolved before FetchContent_MakeAvailable(llama), because
# ggml reads them at configure time. Set them afterwards and you get a build
# that compiles, links, loads a model, and runs entirely on the CPU.
#
# Defaulting to ON rather than requiring the flag: a forgotten -DGGML_CUDA=ON
# produces a working-but-slow binary with no error anywhere, which is a bad
# thing to be able to do by omission.
#
# 89 = Ada (RTX 4090). Blackwell (RTX PRO 5000 / 5090) is 120, Ampere 86.
if(NOT DEFINED GGML_CUDA)
    set(GGML_CUDA ON CACHE BOOL "Build llama.cpp with the CUDA backend")
endif()
if(GGML_CUDA)
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
        set(CMAKE_CUDA_ARCHITECTURES "89" CACHE STRING "CUDA architecture to target")
    endif()
    if(NOT DEFINED GGML_CUDA_GRAPHS)
        set(GGML_CUDA_GRAPHS ON CACHE BOOL "")
    endif()
    message(STATUS "Helm: CUDA backend on, arch sm_${CMAKE_CUDA_ARCHITECTURES}")
else()
    message(WARNING "Helm: building WITHOUT CUDA - inference will run on the CPU")
endif()

FetchContent_Declare(
    llama
    GIT_REPOSITORY https://github.com/ggml-org/llama.cpp
    GIT_TAG        b10158
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(llama)

# Helm's own code is C++20 (std::format, contains(), CTAD in util.h).
set(CMAKE_CXX_STANDARD 20)

# ---------------------------------------------------------------- WebView2 SDK (NuGet package, downloaded at configure time)
set(WEBVIEW2_VERSION "1.0.2903.40" CACHE STRING "WebView2 SDK NuGet version")
set(WEBVIEW2_DIR "${CMAKE_BINARY_DIR}/webview2")
if(NOT EXISTS "${WEBVIEW2_DIR}/build/native/include/WebView2.h")
    message(STATUS "Downloading WebView2 SDK ${WEBVIEW2_VERSION}")
    file(DOWNLOAD
        "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${WEBVIEW2_VERSION}"
        "${CMAKE_BINARY_DIR}/webview2.nupkg"
        STATUS _wv2_status)
    list(GET _wv2_status 0 _wv2_code)
    if(NOT _wv2_code EQUAL 0)
        message(FATAL_ERROR "WebView2 SDK download failed: ${_wv2_status}")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${CMAKE_BINARY_DIR}/webview2.nupkg" DESTINATION "${WEBVIEW2_DIR}")
endif()

# ---------------------------------------------------------------- application
add_executable(helm WIN32
    src/main.cpp
    src/helm.rc
    src/ui/bridge.cpp
    src/ui/file_dialog.cpp
    src/common/config.cpp
    src/engine/engine.cpp
    src/engine/grammar.cpp
    src/agent/prompt_builder.cpp
    src/agent/registry.cpp
    src/agent/harmony.cpp
    src/agent/loop.cpp
    src/agent/jobs.cpp
    src/session/session.cpp
    src/session/memory.cpp
    src/workspace/workspace.cpp
    src/workspace/tool_packs.cpp
    src/tools/tool_time.cpp
    src/tools/tool_dice.cpp
    src/tools/tool_demo_job.cpp
    src/tools/tool_files.cpp
    src/tools/tool_process.cpp
    src/tools/tool_web_crawl.cpp
    src/tools/tool_memory.cpp
    src/tools/external_tools.cpp
)

target_compile_features(helm PRIVATE cxx_std_20)

target_include_directories(helm PRIVATE
    src
    third_party
    "${WEBVIEW2_DIR}/build/native/include"
)

target_link_libraries(helm PRIVATE
    llama
    "${WEBVIEW2_DIR}/build/native/x64/WebView2LoaderStatic.lib"
    user32 gdi32 ole32 shell32 shlwapi version winhttp winmm
)

# WebView2LoaderStatic needs these when building /MD
set_property(TARGET helm PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

if(MSVC)
    target_compile_options(helm PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
endif()
set_target_properties(helm PROPERTIES OUTPUT_NAME "Helm")

# Ship immutable defaults/assets next to the binary. Mutable state lives under LocalAppData.
add_custom_command(TARGET helm POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/web"    "$<TARGET_FILE_DIR:helm>/web"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/config" "$<TARGET_FILE_DIR:helm>/config"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/tools_runtime" "$<TARGET_FILE_DIR:helm>/tools_runtime"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/install_helm_tools.cmd" "$<TARGET_FILE_DIR:helm>/install_helm_tools.cmd"
)

include(CTest)
if(BUILD_TESTING)
    add_executable(helm_grammar_tests
        tests/test_grammar.cpp
        src/engine/grammar.cpp
    )
    target_compile_features(helm_grammar_tests PRIVATE cxx_std_20)
    target_include_directories(helm_grammar_tests PRIVATE src)
    add_test(NAME helm_grammar_tests COMMAND helm_grammar_tests)

    add_executable(helm_harmony_tests
        tests/test_harmony.cpp
        src/agent/harmony.cpp
        src/agent/prompt_builder.cpp
    )
    target_compile_features(helm_harmony_tests PRIVATE cxx_std_20)
    target_include_directories(helm_harmony_tests PRIVATE src third_party)
    add_test(NAME helm_harmony_tests COMMAND helm_harmony_tests)

    add_executable(helm_stream_filter_tests
        tests/test_stream_filter.cpp
    )
    target_compile_features(helm_stream_filter_tests PRIVATE cxx_std_20)
    target_include_directories(helm_stream_filter_tests PRIVATE src)
    add_test(NAME helm_stream_filter_tests COMMAND helm_stream_filter_tests)
endif()
