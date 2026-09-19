add_executable(hd6301dump
    main.c
    cmd.c
    mem.c
)

pico_generate_pio_header(hd6301dump ${CMAKE_CURRENT_LIST_DIR}/bus.pio)

# The built-in memory image is part 1's assembled output. It is assembled here
# rather than read from 6301/romdump.bin, because that is a gitignored build
# product and would be missing in a fresh clone.
set(ROMDUMP_ASM ${CMAKE_CURRENT_LIST_DIR}/../../6301/romdump.asm)
set(ROMDUMP_BIN ${CMAKE_CURRENT_BINARY_DIR}/romdump.bin)
set(ROM_IMAGE_H ${CMAKE_CURRENT_BINARY_DIR}/rom_image.h)

find_program(TABASM tabasm REQUIRED)
find_program(PYTHON3 python3 REQUIRED)

add_custom_command(
    OUTPUT ${ROMDUMP_BIN}
    COMMAND ${TABASM} --cpu 6303 -b -c -fFF -q
            ${ROMDUMP_ASM} ${ROMDUMP_BIN} ${CMAKE_CURRENT_BINARY_DIR}/romdump.lst
    DEPENDS ${ROMDUMP_ASM}
    COMMENT "Assembling HD6301 image with tabasm"
    VERBATIM
)

add_custom_command(
    OUTPUT ${ROM_IMAGE_H}
    COMMAND ${PYTHON3} ${CMAKE_CURRENT_LIST_DIR}/../tools/bin2c.py
            --name rom_image --addr 0xC000 ${ROMDUMP_BIN} ${ROM_IMAGE_H}
    DEPENDS ${ROMDUMP_BIN} ${CMAKE_CURRENT_LIST_DIR}/../tools/bin2c.py
    COMMENT "Generating rom_image.h"
    VERBATIM
)

add_custom_target(rom_image_h DEPENDS ${ROM_IMAGE_H})
add_dependencies(hd6301dump rom_image_h)

target_include_directories(hd6301dump PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_compile_options(hd6301dump PRIVATE -Wall -Wextra)

target_link_libraries(hd6301dump
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_uart
    hardware_clocks
)

pico_set_program_name(hd6301dump "hd6301dump")
pico_set_program_description(hd6301dump
    "HD6301 bus emulator, Intel HEX loader and ROM dump capture")

# USB CDC is the whole host interface; stdio on UART0 would collide with the
# target's SCI, which owns that peripheral.
pico_enable_stdio_usb(hd6301dump 1)
pico_enable_stdio_uart(hd6301dump 0)

pico_add_extra_outputs(hd6301dump)
