find_program(RS_GUEST_CLANG NAMES clang REQUIRED)
find_program(RS_LLVM_OBJCOPY NAMES llvm-objcopy REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(image_dir "${CMAKE_BINARY_DIR}/images")
set(kernel_base 0x80200000)
set(user_base 0x40000000)
math(EXPR user_data_base "${user_base} + 5 * 4096" OUTPUT_FORMAT HEXADECIMAL)
set(guest_flags
    --target=riscv64-unknown-elf
    -mabi=lp64 -mcmodel=medany
    -msmall-data-limit=0 -mno-relax
    -std=c23 -pedantic-errors -Wall -Wextra -Werror
    -ffreestanding -ffunction-sections -fdata-sections
    -nostdlib -fuse-ld=lld -Wl,--gc-sections
    -g -Os "-I${CMAKE_CURRENT_SOURCE_DIR}" "-DRS_KERNEL_BASE=${kernel_base}ULL"
)

function(rs_guest_image name script arch)
    cmake_parse_arguments(IMAGE "ELF_ONLY" "" "SOURCES;FLAGS;DEPENDS" ${ARGN})
    set(sources ${IMAGE_SOURCES})
    list(TRANSFORM sources PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
    set(link_script "${CMAKE_CURRENT_SOURCE_DIR}/${script}")
    add_custom_command(
        OUTPUT "${image_dir}/${name}.elf"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${image_dir}"
        COMMAND "${RS_GUEST_CLANG}" ${guest_flags} "-march=${arch}" ${IMAGE_FLAGS}
            "-Wl,-T,${link_script}" ${sources} -o "${image_dir}/${name}.elf"
        DEPENDS ${sources} ${link_script} ${IMAGE_DEPENDS}
        COMMENT "Building RISC-V ${name} with Clang"
        VERBATIM
    )
    if(NOT IMAGE_ELF_ONLY)
        add_custom_command(
            OUTPUT "${image_dir}/${name}.bin"
            COMMAND "${RS_LLVM_OBJCOPY}" -O binary
                "${image_dir}/${name}.elf" "${image_dir}/${name}.bin"
            DEPENDS "${image_dir}/${name}.elf"
            VERBATIM)
    endif()
endfunction()

rs_guest_image(firmware image.ld rv64im_zicsr_zifencei_sstc
    SOURCES entry.S platform.c boot/firmware.c boot/disk.c
    FLAGS "-Wl,--defsym=IMAGE_BASE=0x80000000,--defsym=IMAGE_LIMIT=${kernel_base}"
    DEPENDS platform.h boot/disk.h boot/disk_format.h)

rs_guest_image(user user/user.ld rv64imac
    SOURCES user/demo.c user/syscall.S user/compute.S
    FLAGS "-DRS_USER_BASE=${user_base}" "-Wl,--defsym=USER_BASE=${user_base},--defsym=USER_DATA_BASE=${user_data_base}"
    DEPENDS user/abi.h user/api.h user/layout.h)

rs_guest_image(kernel image.ld rv64imac_zicsr_zifencei_sstc
    SOURCES entry.S platform.c boot/disk.c kernel/fs.c kernel/vfs.c kernel/procfs.c kernel/elf.c
        kernel/shell.c kernel/user_files.c kernel/paging.c kernel/string.c kernel/trap.c kernel/trap.S user/embed.S
    FLAGS -DRS_SUPERVISOR=1 "-DRS_USER_BASE=${user_base}" "-DRS_USER_DATA_BASE=${user_data_base}" "-I${image_dir}"
        "-Wl,--defsym=IMAGE_BASE=${kernel_base},--defsym=IMAGE_LIMIT=0x80a00000"
    DEPENDS platform.h boot/disk.h boot/disk_format.h kernel/fs.h kernel/elf.h kernel/paging.h kernel/trap.h
        kernel/vfs.h kernel/vfs_internal.h kernel/user_files.h user/abi.h user/layout.h "${image_dir}/user.bin")

rs_guest_image(hello user/disk.ld rv64imac_zba_zbb_zcb_zicond ELF_ONLY
    SOURCES user/hello.c user/hello_bytes.S user/syscall.S
    FLAGS "-Wl,--defsym=APP_BASE=0x40010000"
    DEPENDS user/abi.h user/api.h)

rs_guest_image(memory user/disk.ld rv64imac ELF_ONLY
    SOURCES user/memory_files.c user/syscall.S
    FLAGS "-Wl,--defsym=APP_BASE=0x40010000"
    DEPENDS user/abi.h user/api.h user/layout.h)

rs_guest_image(family user/disk.ld rv64imac ELF_ONLY
    SOURCES user/family.c user/syscall.S
    FLAGS "-Wl,--defsym=APP_BASE=0x40010000"
    DEPENDS user/abi.h user/api.h)

set(disk_builder "${CMAKE_CURRENT_SOURCE_DIR}/../tools/mkdisk.py")
add_custom_command(
    OUTPUT "${image_dir}/disk.img"
    COMMAND "${Python3_EXECUTABLE}" "${disk_builder}"
        "${image_dir}/kernel.bin" "${image_dir}/disk.img"
        --file "readme.txt=${CMAKE_CURRENT_SOURCE_DIR}/files/readme.txt"
        --file "hello.elf=${image_dir}/hello.elf"
        --file "memory.elf=${image_dir}/memory.elf"
        --file "family.elf=${image_dir}/family.elf"
    DEPENDS "${image_dir}/kernel.bin" "${disk_builder}"
        "${CMAKE_CURRENT_SOURCE_DIR}/files/readme.txt" "${image_dir}/hello.elf"
        "${image_dir}/memory.elf" "${image_dir}/family.elf"
    COMMENT "Packing the boot disk"
    VERBATIM
)
add_custom_target(rs_images ALL DEPENDS "${image_dir}/firmware.bin" "${image_dir}/disk.img")
