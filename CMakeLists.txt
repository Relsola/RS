cmake_minimum_required(VERSION 3.29)
if(CMAKE_HOST_WIN32)
    set(CMAKE_LINKER_TYPE SYSTEM) # clang invokes Microsoft's link.exe for host binaries.
endif()
project(RS LANGUAGES CXX)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    message(FATAL_ERROR "Use the clang driver, e.g. cmake --preset debug")
endif()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
add_compile_options(-Wall -Wextra -Wpedantic)
if(NOT WIN32)
    # The executable is named clang; at link time request its C++ runtime.
    add_link_options(--driver-mode=g++)
endif()
if(WIN32 AND NOT CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    message(FATAL_ERROR "The Windows build uses Clang targeting the MSVC ABI")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
include(CTest)
add_subdirectory(VM)
add_subdirectory(OS)

if(BUILD_TESTING)
    foreach(scenario IN ITEMS console user tasks files ring-wrap bad-header bad-crc bad-size short-disk)
        add_test(NAME boot.${scenario} COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/boot.py" ${scenario} "$<TARGET_FILE:rs-vm>"
            "${CMAKE_BINARY_DIR}/images/firmware.bin" "${CMAKE_BINARY_DIR}/images/disk.img")
        set_tests_properties(boot.${scenario} PROPERTIES TIMEOUT 30)
    endforeach()
    foreach(format IN ITEMS rsfs elf vfs_vm)
        add_test(NAME disk.${format} COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/${format}_tests.py" "$<TARGET_FILE:rs-vm>"
            "${CMAKE_BINARY_DIR}/images/firmware.bin" "${CMAKE_BINARY_DIR}/images/disk.img")
        set_tests_properties(disk.${format} PROPERTIES TIMEOUT 90)
    endforeach()
    set(RS_REFERENCE_QEMU "" CACHE FILEPATH "Optional qemu-system-riscv64 with the rva23s64 CPU model")
    if(RS_REFERENCE_QEMU)
        add_test(NAME reference.rva23s64 COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/reference.py" "${RS_REFERENCE_QEMU}" "${CMAKE_BINARY_DIR}/images")
        set_tests_properties(reference.rva23s64 PROPERTIES TIMEOUT 30)
    endif()
endif()
