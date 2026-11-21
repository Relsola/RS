add_library(rs-cpu STATIC cpu.cpp cpu_integer.cpp cpu_privilege.cpp cpu_mmu.cpp
    cpu_bitmanip.cpp cpu_compressed.cpp cpu_atomic.cpp)
target_include_directories(rs-cpu PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

add_library(rs-board STATIC bus.cpp console.cpp virtio.cpp)
target_include_directories(rs-board PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(rs-vm main.cpp)
target_link_libraries(rs-vm PRIVATE rs-cpu rs-board)
set_target_properties(rs-vm PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(BUILD_TESTING)
    add_executable(rs-cpu-tests tests/cpu_tests.cpp)
    target_link_libraries(rs-cpu-tests PRIVATE rs-cpu)
    add_test(NAME vm.instructions COMMAND rs-cpu-tests)
    add_executable(rs-atomic-tests tests/atomic_tests.cpp)
    target_link_libraries(rs-atomic-tests PRIVATE rs-cpu rs-board)
    add_test(NAME vm.atomics COMMAND rs-atomic-tests)
    add_executable(rs-napot-tests tests/napot_tests.cpp)
    target_link_libraries(rs-napot-tests PRIVATE rs-cpu rs-board)
    add_test(NAME vm.svnapot COMMAND rs-napot-tests)
    add_executable(rs-bitmanip-tests tests/bitmanip_tests.cpp)
    target_link_libraries(rs-bitmanip-tests PRIVATE rs-cpu)
    add_test(NAME vm.bitmanip COMMAND rs-bitmanip-tests)

    add_executable(rs-compressed-tests tests/compressed_tests.cpp)
    target_link_libraries(rs-compressed-tests PRIVATE rs-cpu)
    add_test(NAME vm.compressed COMMAND rs-compressed-tests)

    add_executable(rs-virtio-tests tests/virtio_tests.cpp)
    target_link_libraries(rs-virtio-tests PRIVATE rs-board)
    add_dependencies(rs-virtio-tests rs_images)
    add_test(NAME vm.virtio COMMAND rs-virtio-tests "${CMAKE_BINARY_DIR}/images/disk.img")
endif()
