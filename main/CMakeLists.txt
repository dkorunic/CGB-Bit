idf_component_register(SRCS "main.cpp"
                       PRIV_REQUIRES esp_driver_ledc esp_driver_gpio esp_timer efuse esp_adc esp_partition esp_app_format)

# Walnut-CGB from the submodule, patched with patches/*.diff into the build dir by
# tools/patch_walnut.sh, so the submodule stays clean. The patch list is on the command line, so
# adding or removing a patch re-runs it.
set(WALNUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/walnut)
set(WALNUT_SRC ${CMAKE_CURRENT_LIST_DIR}/../walnut-cgb/walnut_cgb.h)
file(GLOB WALNUT_PATCHES CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/../patches/*.diff)
list(SORT WALNUT_PATCHES)
if(NOT EXISTS ${WALNUT_SRC})
    message(FATAL_ERROR "walnut-cgb submodule is empty, run: git submodule update --init")
endif()
add_custom_command(OUTPUT ${WALNUT_DIR}/walnut_cgb.h
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${WALNUT_DIR} # inside build/
    COMMAND sh ${CMAKE_CURRENT_LIST_DIR}/../tools/patch_walnut.sh ${WALNUT_DIR} ${WALNUT_PATCHES}
    DEPENDS ${WALNUT_SRC} ${WALNUT_PATCHES} ${CMAKE_CURRENT_LIST_DIR}/../tools/patch_walnut.sh
    COMMENT "Patching Walnut-CGB with patches/*.diff"
    VERBATIM)
add_custom_target(walnut_patched DEPENDS ${WALNUT_DIR}/walnut_cgb.h)
add_dependencies(${COMPONENT_LIB} walnut_patched)
target_include_directories(${COMPONENT_LIB} PRIVATE ${WALNUT_DIR})
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function)

# Walnut-CGB's small hot helpers (memory access, CB opcodes, line drawing; ~10 KB) run from internal
# RAM, out of the flash cache that the 30 KB CPU loop and both cores share (main.cpp's own hot
# callbacks carry IRAM_ATTR). The whole core in IRAM (~43 KB) cost too much heap; see CLAUDE.md.
set(WALNUT_IRAM _Z9__gb_readP4gb_st _Z11__gb_read16P4gb_st _Z11__gb_read32P4gb_st
    _Z10__gb_writeP4gb_sjh _Z12__gb_write32P4gb_stm _Z15__gb_execute_cbP4gb_s _Z14__gb_draw_lineP4gb_s)
add_custom_command(TARGET ${COMPONENT_LIB} POST_BUILD
    COMMAND sh ${CMAKE_CURRENT_LIST_DIR}/../tools/iram_hot.sh ${CMAKE_OBJCOPY} ${CMAKE_OBJDUMP}
        $<TARGET_FILE:${COMPONENT_LIB}> ${WALNUT_IRAM}
    VERBATIM)

# The emulator (main.cpp holds Walnut-CGB) built for size: its CPU loop is near the 32 KB instruction
# cache, and fewer cache misses beat -O2's faster code (measured: CLAUDE.md, Performance).
set_source_files_properties(main.cpp PROPERTIES COMPILE_OPTIONS -Os)

# The game written to the rom partition by `idf.py flash` (and in the merged image):
# idf.py -DGB_ROM=path/to/game.gbc build (a relative path is taken from the project directory)
set(GB_ROM ${CMAKE_CURRENT_LIST_DIR}/../roms/tobudx.gb CACHE STRING "Game Boy ROM flashed to the rom partition")
get_filename_component(GB_ROM_PATH "${GB_ROM}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
if(NOT EXISTS ${GB_ROM_PATH})
    message(FATAL_ERROR "GB_ROM ${GB_ROM_PATH} not found")
endif()
file(SIZE ${GB_ROM_PATH} GB_ROM_SIZE)
if(GB_ROM_SIZE GREATER 4194304)
    message(FATAL_ERROR "GB_ROM is ${GB_ROM_SIZE} bytes, the rom partition holds 4 MB")
endif()
# The firmware reads up to the ROM size the header declares: a shorter file would leave the previous
# game's bytes after it in the partition.
file(READ ${GB_ROM_PATH} GB_ROM_SIZE_CODE OFFSET 328 LIMIT 1 HEX) # header byte 0x148
math(EXPR GB_ROM_SIZE_CODE "0x${GB_ROM_SIZE_CODE}")
if(GB_ROM_SIZE_CODE LESS_EQUAL 8)
    math(EXPR GB_ROM_DECLARED "32768 << ${GB_ROM_SIZE_CODE}")
    if(GB_ROM_SIZE LESS GB_ROM_DECLARED)
        message(FATAL_ERROR "GB_ROM is ${GB_ROM_SIZE} bytes but its header declares ${GB_ROM_DECLARED}: pad it with 0xFF bytes to that size")
    endif()
endif()
esptool_py_flash_to_partition(flash "rom" ${GB_ROM_PATH})
