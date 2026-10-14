cmake_minimum_required(VERSION 3.16)
set(ULTIMATRIX_MODE "football" CACHE STRING "Clock mode: football, f1, motogp, tetris, word, mario, luigi, pacman or bridge")
set_property(CACHE ULTIMATRIX_MODE PROPERTY STRINGS football f1 motogp tetris word mario luigi pacman bridge)
if(NOT ULTIMATRIX_MODE MATCHES "^(football|f1|motogp|tetris|word|mario|luigi|pacman|bridge)$")
    message(FATAL_ERROR "ULTIMATRIX_MODE must be football, f1, motogp, tetris, word, mario, luigi, pacman or bridge")
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
project(ultimatrix)
