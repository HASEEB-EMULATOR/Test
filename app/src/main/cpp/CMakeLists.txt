cmake_minimum_required(VERSION 3.22.1)
project(haseeb_vm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(HASEEB_BUILD_HOST_TESTS "Build host unit and integration tests" OFF)
option(HASEEB_BUILD_GUEST_TEST "Build standalone VM integration test binary" OFF)

file(GLOB HASEEB_CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
list(REMOVE_ITEM HASEEB_CORE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/engine_runtime.cpp
)

add_library(
    haseeb_vm_core
    STATIC
    ${HASEEB_CORE_SOURCES}
)

target_include_directories(
    haseeb_vm_core
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(
    haseeb_vm_core
    PRIVATE
    -Wall
    -Wextra
    -Werror=return-type
    -O2
)

find_package(Threads REQUIRED)
target_link_libraries(haseeb_vm_core PUBLIC Threads::Threads)

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
        haseeb_vm_core
        ${android-lib}
        ${log-lib}
    )
endif()

if(HASEEB_BUILD_HOST_TESTS OR HASEEB_BUILD_GUEST_TEST)
    enable_testing()
    add_executable(
        haseeb_m1_vm_tests
        tests/m1_boot_machine_test.cpp
    )
    target_include_directories(
        haseeb_m1_vm_tests
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_options(haseeb_m1_vm_tests PRIVATE -Wall -Wextra -O2)
    target_link_libraries(haseeb_m1_vm_tests PRIVATE haseeb_vm_core)
    if(ANDROID)
        target_link_options(haseeb_m1_vm_tests PRIVATE -Wl,-z,max-page-size=16384)
    endif()
    add_test(NAME m1_boot_machine_tests COMMAND haseeb_m1_vm_tests)

    add_executable(
        haseeb_m2_cpu_mmu_tests
        tests/m2_cpu_mmu_test.cpp
    )
    target_include_directories(
        haseeb_m2_cpu_mmu_tests
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_options(haseeb_m2_cpu_mmu_tests PRIVATE -Wall -Wextra -O2)
    target_link_libraries(haseeb_m2_cpu_mmu_tests PRIVATE haseeb_vm_core)
    if(ANDROID)
        target_link_options(haseeb_m2_cpu_mmu_tests PRIVATE -Wl,-z,max-page-size=16384)
    endif()
    add_test(NAME m2_cpu_mmu_tests COMMAND haseeb_m2_cpu_mmu_tests)
endif()
