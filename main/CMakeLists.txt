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
