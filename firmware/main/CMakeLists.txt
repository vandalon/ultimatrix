set(root "${CMAKE_CURRENT_LIST_DIR}/..")
# The folder of the face: MotoGP is the F1 face with another data service
set(face "${ULTIMATRIX_MODE}")
if(ULTIMATRIX_MODE STREQUAL "motogp")
    set(face "f1")
endif()
set(sources
    "${root}/src/main.cpp"
    "${root}/lib/core/ClockTime.cpp"
    "${root}/lib/core/Settings.cpp"
    "${root}/lib/core/DisplayControl.cpp"
    "${root}/lib/core/NightClock.cpp"
    "${root}/lib/core/FirmwareUpdate.cpp"
    "${root}/lib/core/TimeService.cpp"
    "${root}/lib/core/JsonScan.cpp"
    "${root}/lib/core/BootScreen.cpp"
    "${root}/lib/core/TelnetConsole.cpp"
    "${root}/lib/core/qrcodegen.c"
    "${root}/clockfaces/${face}/Clockface.cpp")
if(ULTIMATRIX_MODE STREQUAL "football")
    set(sport "football-feed")
    list(APPEND sources
        "${root}/lib/${sport}/FootballTicker.cpp"
        "${root}/lib/${sport}/EspnParse.cpp"
        "${root}/clockfaces/football/FootballRenderer.cpp"
        "${root}/clockfaces/football/FootballScreens.cpp"
        "${root}/lib/${sport}/GoalAnimation.cpp"
        "${root}/lib/${sport}/TeamColors.cpp")
    set(mode_definitions UM_FOOTBALL_ONLY UM_FOOTBALL_CATALOG UM_MODE_NAME="Football")
elseif(ULTIMATRIX_MODE MATCHES "^(word|mario|luigi|pacman)$")
    # The pixel-art faces: no feed, so only the face itself and its shared helpers
    set(sport "faces")
    if(ULTIMATRIX_MODE STREQUAL "word")
        list(APPEND sources
            "${root}/clockfaces/word/WordFont.cpp"
            "${root}/clockfaces/word/WordLanguages.cpp")
        set(mode_definitions UM_SIMPLE_FACE UM_WORD UM_MODE_NAME="Words")
    elseif(ULTIMATRIX_MODE STREQUAL "mario")
        set(mode_definitions UM_SIMPLE_FACE UM_MARIO UM_MODE_NAME="Mario")
    elseif(ULTIMATRIX_MODE STREQUAL "luigi")
        set(mode_definitions UM_SIMPLE_FACE UM_LUIGI UM_MODE_NAME="Luigi")
    else()
        set(mode_definitions UM_SIMPLE_FACE UM_PACMAN UM_MODE_NAME="Pac-Man")
    endif()
elseif(ULTIMATRIX_MODE STREQUAL "tetris")
    set(sport "tetris")
    list(APPEND sources
        "${root}/lib/${sport}/TetrisDigits.cpp"
        "${root}/lib/${sport}/TetrisShapes.cpp"
        "${root}/lib/${sport}/TetrisLayout.cpp"
        "${root}/clockfaces/tetris/TetrisRenderer.cpp"
        "${root}/clockfaces/tetris/TetrisScreens.cpp")
    set(mode_definitions UM_SIMPLE_FACE UM_TETRIS UM_MODE_NAME="Tetris")
elseif(ULTIMATRIX_MODE STREQUAL "motogp")
    set(sport "f1-feed")
    list(APPEND sources
        "${root}/lib/motogp-feed/MotoGpTicker.cpp"
        "${root}/clockfaces/f1/F1Renderer.cpp"
        "${root}/clockfaces/f1/F1Screens.cpp")
    set(mode_definitions UM_F1 UM_MOTOGP UM_MODE_NAME="MotoGP")
else()
    set(sport "f1-feed")
    list(APPEND sources
        "${root}/lib/${sport}/F1Ticker.cpp"
        "${root}/lib/${sport}/F1Live.cpp"
        "${root}/lib/${sport}/F1LiveState.cpp"
        "${root}/clockfaces/f1/F1Renderer.cpp"
        "${root}/clockfaces/f1/F1Screens.cpp")
    set(mode_definitions UM_F1 UM_MODE_NAME="F1")
endif()
# The timezone table is generated on every build from tools/zones.csv
set(tzdb "${CMAKE_CURRENT_BINARY_DIR}/generated/TzDb.h")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
if(NOT CMAKE_BUILD_EARLY_EXPANSION)  # commands are not scriptable in IDF's requirements pass
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_custom_command(OUTPUT "${tzdb}"
        COMMAND ${Python3_EXECUTABLE} "${root}/tools/gen_tzdb.py" "${tzdb}"
        DEPENDS "${root}/tools/gen_tzdb.py" "${root}/tools/zones.csv"
        COMMENT "Generating TzDb.h")
    add_custom_target(tzdb_header DEPENDS "${tzdb}")
endif()
idf_component_register(SRCS ${sources}
    INCLUDE_DIRS "${root}/lib/core" "${CMAKE_CURRENT_BINARY_DIR}/generated" "${root}/lib/${sport}" "${root}/lib/faces" "${root}/lib/motogp-feed" "${root}/clockfaces/${face}"
    REQUIRES espressif__arduino-esp32 clock-libraries esp_wifi esp_netif esp_event espressif__network_provisioning nvs_flash esp_http_client app_update json)
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    add_dependencies(${COMPONENT_LIB} tzdb_header)
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ${mode_definitions} UM_MODE_ID="${ULTIMATRIX_MODE}" DOUBLE_BUFFER_ON UM_FOOTBALL_SIM UM_PIXEL_DEPTH_BITS=7)

# Serve the setup assets directly from flash rather than assembling a page on the heap.
foreach(asset settings.html theme.css settings.js)
    target_add_binary_data(${COMPONENT_LIB} "${root}/web/${asset}" TEXT)
endforeach()
# Installable-app manifest and its LED-matrix U icons.
target_add_binary_data(${COMPONENT_LIB} "${root}/web/manifest.webmanifest" TEXT)
foreach(icon icon-192 icon-512 icon-maskable-192 apple-touch-icon favicon-32)
    target_add_binary_data(${COMPONENT_LIB} "${root}/web/icons/${icon}.png" BINARY)
endforeach()
