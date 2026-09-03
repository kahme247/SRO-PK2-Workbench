cmake_minimum_required(VERSION 3.20)

project(PK2WorkbenchPRO
    VERSION 0.3.1
    DESCRIPTION "PK2 Workbench PRO by kahme247"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(pk2core STATIC
    src/pk2core/archive.cpp
    src/pk2core/crypto.cpp
    src/pk2core/path.cpp
    src/pk2core/server_config.cpp)

target_include_directories(pk2core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(MSVC)
    add_compile_options(/EHsc)
    target_compile_options(pk2core PRIVATE /W4 /permissive-)
else()
    target_compile_options(pk2core PRIVATE -Wall -Wextra -pedantic)
endif()

add_executable(pk2cli
    apps/pk2cli/main.cpp)
target_link_libraries(pk2cli PRIVATE pk2core)

if(WIN32)
    enable_language(RC)
    add_executable(pk2win WIN32
        apps/pk2win/main.cpp
        apps/pk2win/pk2win.rc)
    target_link_libraries(pk2win PRIVATE pk2core comctl32 shell32 comdlg32 ole32 uxtheme)
    target_compile_definitions(pk2win PRIVATE UNICODE _UNICODE)
    if(MINGW)
        target_link_options(pk2win PRIVATE -municode)
    endif()
endif()

enable_testing()
add_executable(pk2core_tests
    tests/pk2core_tests.cpp)
target_link_libraries(pk2core_tests PRIVATE pk2core)
add_test(NAME pk2core_tests COMMAND pk2core_tests)
