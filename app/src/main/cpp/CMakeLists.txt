cmake_minimum_required(VERSION 3.22.1)
project(haseeb_vm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(HASEEB_BUILD_HOST_TESTS "Build x86_64 host unit and integration tests" OFF)
option(HASEEB_BUILD_GUEST_TEST "Build standalone guest integration test binary" OFF)

add_library(
    haseeb_prop_area
    STATIC
    src/prop_area.cpp
    src/property_service.cpp
)

target_include_directories(
    haseeb_prop_area
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(
    haseeb_prop_area
    PRIVATE
    -Wall
    -Wextra
    -Werror=return-type
    -O2
)

find_package(Threads REQUIRED)
target_link_libraries(haseeb_prop_area PUBLIC Threads::Threads)

add_library(
    haseeb_guest_loader
    STATIC
    src/ext4_reader.cpp
    src/elf64_loader.cpp
)

target_include_directories(
    haseeb_guest_loader
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(
    haseeb_guest_loader
    PRIVATE
    -Wall
    -Wextra
    -Werror=return-type
    -O2
)

if(ANDROID)
    add_library(
        haseeb_vm
        SHARED
        haseeb_vm_jni.cpp
        src/engine_runtime.cpp
    )

    target_include_directories(
        haseeb_vm
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_compile_options(
        haseeb_vm
        PRIVATE
        -Wall
        -Wextra
        -Werror=return-type
        -O2
    )

    # Android 15+ 16 KiB ELF segment page alignment
    target_link_options(
        haseeb_vm
        PRIVATE
        -Wl,-z,max-page-size=16384
    )

    find_library(log-lib log)
    find_library(android-lib android)

    target_link_libraries(
        haseeb_vm
        PRIVATE
        haseeb_prop_area
        haseeb_guest_loader
        ${android-lib}
        ${log-lib}
    )
endif()

if(HASEEB_BUILD_HOST_TESTS OR HASEEB_BUILD_GUEST_TEST)
    add_executable(
        haseeb_guest_prop_test
        tests/guest_prop_integration_test.cpp
    )
    target_include_directories(
        haseeb_guest_prop_test
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_options(haseeb_guest_prop_test PRIVATE -Wall -Wextra -O2)
    target_link_libraries(haseeb_guest_prop_test PRIVATE haseeb_prop_area)

    add_executable(
        haseeb_guest_loader_test
        tests/guest_loader_integration_test.cpp
    )
    target_include_directories(
        haseeb_guest_loader_test
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_options(haseeb_guest_loader_test PRIVATE -Wall -Wextra -O2)
    target_link_libraries(haseeb_guest_loader_test PRIVATE haseeb_guest_loader haseeb_prop_area)

    if(ANDROID)
        target_link_options(haseeb_guest_prop_test PRIVATE -Wl,-z,max-page-size=16384)
        target_link_options(haseeb_guest_loader_test PRIVATE -Wl,-z,max-page-size=16384)
    endif()
endif()

if(HASEEB_BUILD_HOST_TESTS)
    enable_testing()
    add_executable(
        haseeb_m1_unit_tests
        tests/m1_property_area_test.cpp
    )
    target_include_directories(
        haseeb_m1_unit_tests
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_options(haseeb_m1_unit_tests PRIVATE -Wall -Wextra -O2)
    target_link_libraries(haseeb_m1_unit_tests PRIVATE haseeb_prop_area)

    add_executable(
        haseeb_m2_unit_tests
        tests/m2_guest_loader_test.cpp
    )
    target_include_directories(
        haseeb_m2_unit_tests
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_options(haseeb_m2_unit_tests PRIVATE -Wall -Wextra -O2)
    target_link_libraries(haseeb_m2_unit_tests PRIVATE haseeb_guest_loader)

    add_test(NAME m1_property_area_unit_tests COMMAND haseeb_m1_unit_tests)
    add_test(NAME m1_guest_prop_integration_test COMMAND haseeb_guest_prop_test)
    add_test(NAME m2_guest_loader_unit_tests COMMAND haseeb_m2_unit_tests)
    add_test(NAME m2_guest_loader_integration_test COMMAND haseeb_guest_loader_test)
endif()
