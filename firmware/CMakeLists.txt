cmake_minimum_required(VERSION 3.16)
set(ULTIMATRIX_MODE "football" CACHE STRING "Clock mode: football, f1, motogp, tetris, word, mario, luigi, pacman, home, bridge or scanner")
set_property(CACHE ULTIMATRIX_MODE PROPERTY STRINGS football f1 motogp tetris word mario luigi pacman home bridge scanner)
if(NOT ULTIMATRIX_MODE MATCHES "^(football|f1|motogp|tetris|word|mario|luigi|pacman|home|bridge|scanner)$")
    message(FATAL_ERROR "ULTIMATRIX_MODE must be football, f1, motogp, tetris, word, mario, luigi, pacman, home, bridge or scanner")
endif()
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# esp_insights/esp_rainmaker (unused arduino-esp32 dependencies) call git_describe(), which fails in a
# repository without commits. They only use it for a version message, so stub it.
function(git_describe _var _repo_dir)
    set(${_var} "unknown" PARENT_SCOPE)
endfunction()
# Release builds pass -DULTIMATRIX_VERSION=<tag without v>; the mode is appended (32 characters max in the app header).
set(ULTIMATRIX_VERSION "0.2.0" CACHE STRING "Firmware version")
set(PROJECT_VER "${ULTIMATRIX_VERSION}-${ULTIMATRIX_MODE}")
# The Arduino WebServer is single threaded and runs in the loop that also draws. It waits for a silent or slow client up to 5 s for a request line and
# 10 s for the headers while only calling yield(), which never lets the idle task run: the 5 s task watchdog then resets the clock (seen with a browser
# page, a phone app and the editor open together). Keep both waits well under the watchdog.
idf_build_set_property(COMPILE_DEFINITIONS "WEBSERVER_MAX_LINE_WAIT=2000" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "WEBSERVER_MAX_HEADER_WAIT=3000" APPEND)
project(ultimatrix)
# Static RAM budget per face: what a face reserves at link time is missing from the heap in every boot, the update-only boot included.
if(TARGET ultimatrix.elf)
    set(ULTIMATRIX_STATIC_RAM_KB 60 CACHE STRING "Static RAM budget (KB) per face")
    add_custom_command(TARGET ultimatrix.elf POST_BUILD
        COMMAND ${PYTHON} "${CMAKE_CURRENT_LIST_DIR}/tools/check-static-ram.py" "$<TARGET_FILE:ultimatrix.elf>" "${CMAKE_OBJDUMP}" "${ULTIMATRIX_MODE}" "${ULTIMATRIX_STATIC_RAM_KB}"
        VERBATIM)
endif()

