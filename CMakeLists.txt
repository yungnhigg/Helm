cmake_minimum_required(VERSION 3.24)
project(Helm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "Windows-only target (Win32 + WebView2).")
endif()
enable_language(RC)

add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

# /utf-8 is applied per-target below rather than globally. add_compile_options
# would also hand it to nvcc, which does not accept MSVC flags and fails the
# whole CUDA build. Every target that compiles our own C++ needs it: MSVC
# otherwise reads source bytes as the system code page, so a UTF-8 literal
# becomes mojibake with nothing but a C4566 warning to show for it. Test targets
# must get the same flag as the code they test, or their literals differ from the
# ones under test and comparisons fail.

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

# ---------------------------------------------------------------- SQLite (amalgamation, downloaded at configure time)
# Server-side state only. The amalgamation is one .c file with no build system
# of its own, so it is downloaded and compiled directly into helm-server rather
# than vendored into the repo (8 MB of generated C does not belong in git).
set(SQLITE_YEAR "2024" CACHE STRING "SQLite amalgamation release year")
set(SQLITE_VERSION "3450100" CACHE STRING "SQLite amalgamation version")
set(SQLITE_DIR "${CMAKE_BINARY_DIR}/sqlite")
if(NOT EXISTS "${SQLITE_DIR}/sqlite-amalgamation-${SQLITE_VERSION}/sqlite3.c")
    message(STATUS "Downloading SQLite ${SQLITE_VERSION} amalgamation")
    file(DOWNLOAD
        "https://www.sqlite.org/${SQLITE_YEAR}/sqlite-amalgamation-${SQLITE_VERSION}.zip"
        "${CMAKE_BINARY_DIR}/sqlite-amalgamation.zip"
        STATUS _sqlite_status)
    list(GET _sqlite_status 0 _sqlite_code)
    if(NOT _sqlite_code EQUAL 0)
        message(FATAL_ERROR "SQLite amalgamation download failed: ${_sqlite_status}")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${CMAKE_BINARY_DIR}/sqlite-amalgamation.zip" DESTINATION "${SQLITE_DIR}")
endif()
set(SQLITE_SRC_DIR "${SQLITE_DIR}/sqlite-amalgamation-${SQLITE_VERSION}")

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
    src/tools/tool_run_control.cpp
    src/agent/loop.cpp
    src/agent/run_guard.cpp
    src/agent/work_ledger.cpp
    src/agent/jobs.cpp
    src/session/session.cpp
    src/session/memory.cpp
    src/workspace/workspace.cpp
    src/api/anthropic_stream.cpp
    src/api/api_client.cpp
    src/common/dpapi.cpp
    src/net/server_link.cpp
    src/workspace/tool_packs.cpp
    src/tools/tool_time.cpp
    src/tools/tool_dice.cpp
    src/tools/tool_demo_job.cpp
    src/tools/file_guard.cpp
    src/tools/process_guard.cpp
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
    user32 gdi32 ole32 shell32 shlwapi version winhttp winmm dwmapi crypt32
)

# WebView2LoaderStatic needs these when building /MD
set_property(TARGET helm PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

if(MSVC)
    target_compile_options(helm PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
endif()
set_target_properties(helm PROPERTIES OUTPUT_NAME "Helm")

# Ship immutable defaults/assets next to the binary. Mutable state lives under LocalAppData.
#
# This MUST NOT be a POST_BUILD command on `helm`. POST_BUILD only fires when the
# target actually relinks, so a change to web/ alone - no C++ touched - leaves the
# binary "up to date", skips the link, and silently skips the copy with it. The
# result is edited source that never reaches the running app, which is
# indistinguishable from the patch not applying. A standalone ALL target always
# runs, and depending on helm keeps the ordering.
add_custom_target(helm_assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/web"    "$<TARGET_FILE_DIR:helm>/web"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/config" "$<TARGET_FILE_DIR:helm>/config"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/tools_runtime" "$<TARGET_FILE_DIR:helm>/tools_runtime"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/install_helm_tools.cmd" "$<TARGET_FILE_DIR:helm>/install_helm_tools.cmd"
    COMMENT "Copying web/, config/, and tools_runtime/ next to Helm.exe"
    VERBATIM
)
add_dependencies(helm_assets helm)

# ---------------------------------------------------------------- helm-server
# The registry and authority half of the multi-instance design: a console
# binary, deliberately separate from the WIN32 app. It links no llama, no
# WebView2, and no Helm sources - the only shared surface is the wire protocol.
add_executable(helm_server
    server/main.cpp
    server/db.cpp
    "${SQLITE_SRC_DIR}/sqlite3.c"
)
target_compile_features(helm_server PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(helm_server PRIVATE /utf-8)
    # The amalgamation is third-party C: its own warnings are not actionable
    # here, and treating them as ours buries real diagnostics.
    set_source_files_properties("${SQLITE_SRC_DIR}/sqlite3.c" PROPERTIES COMPILE_OPTIONS "/w")
endif()
target_include_directories(helm_server PRIVATE server third_party "${SQLITE_SRC_DIR}")
# Trim the amalgamation to what the server uses. Fewer features compiled in is
# less attack surface on a network-reachable process.
target_compile_definitions(helm_server PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_DQS=0
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_OMIT_DEPRECATED
    SQLITE_DEFAULT_FOREIGN_KEYS=1
)
target_link_libraries(helm_server PRIVATE ws2_32 crypt32 bcrypt)
set_target_properties(helm_server PROPERTIES OUTPUT_NAME "helm-server")

include(CTest)
if(BUILD_TESTING)
    add_executable(helm_grammar_tests
        tests/test_grammar.cpp
        src/engine/grammar.cpp
    )
    target_compile_features(helm_grammar_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(helm_grammar_tests PRIVATE /utf-8)
    endif()
    target_include_directories(helm_grammar_tests PRIVATE src)
    add_test(NAME helm_grammar_tests COMMAND helm_grammar_tests)

    add_executable(helm_harmony_tests
        tests/test_harmony.cpp
        src/agent/harmony.cpp
        src/agent/prompt_builder.cpp
    )
    target_compile_features(helm_harmony_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(helm_harmony_tests PRIVATE /utf-8)
    endif()
    target_include_directories(helm_harmony_tests PRIVATE src third_party)
    add_test(NAME helm_harmony_tests COMMAND helm_harmony_tests)

    add_executable(helm_registry_tests
        tests/test_registry.cpp
        src/agent/registry.cpp
    )
    target_compile_features(helm_registry_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(helm_registry_tests PRIVATE /utf-8)
    endif()
    target_include_directories(helm_registry_tests PRIVATE src third_party)
    add_test(NAME helm_registry_tests COMMAND helm_registry_tests)
    add_executable(helm_run_guard_tests
        tests/test_run_guard.cpp
        src/agent/run_guard.cpp
    )
    target_compile_features(helm_run_guard_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(helm_run_guard_tests PRIVATE /utf-8)
    endif()
    target_include_directories(helm_run_guard_tests PRIVATE src third_party)
    add_test(NAME helm_run_guard_tests COMMAND helm_run_guard_tests)

    add_executable(helm_file_guard_tests
        tests/test_file_guard.cpp
        src/tools/file_guard.cpp
    )
    target_compile_features(helm_file_guard_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(helm_file_guard_tests PRIVATE /utf-8)
    endif()
    target_include_directories(helm_file_guard_tests PRIVATE src third_party)
    target_link_libraries(helm_file_guard_tests PRIVATE shell32 ole32)
    add_test(NAME helm_file_guard_tests COMMAND helm_file_guard_tests)

    add_executable(helm_api_stream_tests
        tests/test_api_stream.cpp
        src/api/anthropic_stream.cpp
    )
    target_compile_features(helm_api_stream_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(helm_api_stream_tests PRIVATE /utf-8)
    endif()
    target_include_directories(helm_api_stream_tests PRIVATE src third_party)
    add_test(NAME helm_api_stream_tests COMMAND helm_api_stream_tests)

    add_executable(helm_server_db_tests
        tests/test_server_db.cpp
        server/db.cpp
        "${SQLITE_SRC_DIR}/sqlite3.c"
    )
    target_compile_features(helm_server_db_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(helm_server_db_tests PRIVATE /utf-8)
        set_source_files_properties("${SQLITE_SRC_DIR}/sqlite3.c" PROPERTIES COMPILE_OPTIONS "/w")
    endif()
    target_include_directories(helm_server_db_tests PRIVATE server third_party "${SQLITE_SRC_DIR}")
    target_compile_definitions(helm_server_db_tests PRIVATE
        SQLITE_THREADSAFE=1 SQLITE_DQS=0 SQLITE_OMIT_LOAD_EXTENSION
        SQLITE_OMIT_DEPRECATED SQLITE_DEFAULT_FOREIGN_KEYS=1)
    add_test(NAME helm_server_db_tests COMMAND helm_server_db_tests)

    add_executable(helm_process_guard_tests
        tests/test_process_guard.cpp
        src/tools/process_guard.cpp
    )
    target_compile_features(helm_process_guard_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(helm_process_guard_tests PRIVATE /utf-8)
    endif()
    target_include_directories(helm_process_guard_tests PRIVATE src third_party)
    add_test(NAME helm_process_guard_tests COMMAND helm_process_guard_tests)

    add_executable(helm_tool_validation_tests
        tests/test_tool_validation.cpp
        src/agent/registry.cpp
        src/agent/jobs.cpp
        src/tools/tool_demo_job.cpp
        src/tools/tool_files.cpp
        src/tools/file_guard.cpp
    )
    target_compile_features(helm_tool_validation_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(helm_tool_validation_tests PRIVATE /utf-8)
    endif()
    target_include_directories(helm_tool_validation_tests PRIVATE src third_party)
    target_link_libraries(helm_tool_validation_tests PRIVATE shell32 ole32)
    add_test(NAME helm_tool_validation_tests COMMAND helm_tool_validation_tests)

    add_executable(helm_stream_filter_tests
        tests/test_stream_filter.cpp
    )
    target_compile_features(helm_stream_filter_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(helm_stream_filter_tests PRIVATE /utf-8)
    endif()
    target_include_directories(helm_stream_filter_tests PRIVATE src)
    add_test(NAME helm_stream_filter_tests COMMAND helm_stream_filter_tests)
endif()
