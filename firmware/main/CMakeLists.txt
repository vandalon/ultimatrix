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
    "${root}/lib/core/Mqtt.cpp"
    "${root}/lib/core/MqttPixels.cpp"
    "${root}/lib/core/Notice.cpp"
    "${root}/lib/core/Storage.cpp"
    "${root}/lib/core/AdminPin.cpp"
    "${root}/lib/core/NoticeImage.cpp"
    "${root}/lib/core/NoticePng.cpp"
    "${root}/lib/core/DlnaRenderer.cpp"
    "${root}/vendor/uzlib/tinflate.c"
    "${root}/vendor/uzlib/adler32.c"
    "${root}/vendor/uzlib/crc32.c"
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
            "${root}/clockfaces/word/WordLanguages.cpp"
            "${root}/clockfaces/word/WordDates.cpp")
        set(mode_definitions UM_SIMPLE_FACE UM_WORD UM_MODE_NAME="Words")
    elseif(ULTIMATRIX_MODE STREQUAL "mario")
        set(mode_definitions UM_SIMPLE_FACE UM_MARIO UM_MODE_NAME="Mario")
    elseif(ULTIMATRIX_MODE STREQUAL "luigi")
        set(mode_definitions UM_SIMPLE_FACE UM_LUIGI UM_MODE_NAME="Luigi")
    else()
        set(mode_definitions UM_SIMPLE_FACE UM_PACMAN UM_MODE_NAME="Pac-Man")
        if(ULTIMATRIX_BLUETOOTH)
            list(APPEND sources "${root}/lib/core/Gamepad.cpp")
            list(APPEND mode_definitions UM_GAMEPAD)
        endif()
    endif()
elseif(ULTIMATRIX_MODE STREQUAL "home")
    # The Home Assistant face: its own client (WebSocket, one template subscription) and renderer
    set(sport "ha-feed")
    list(APPEND sources
        "${root}/lib/${sport}/HaConfig.cpp"
        "${root}/lib/${sport}/HaDefaults.cpp"
        "${root}/lib/${sport}/HaTemplate.cpp"
        "${root}/lib/${sport}/HaClient.cpp"
        "${root}/lib/${sport}/HaQuery.cpp"
        "${root}/lib/${sport}/HaMedia.cpp"
        "${root}/lib/${sport}/HaDevices.cpp"
        "${root}/clockfaces/home/HomeRenderer.cpp")
    set(mode_definitions UM_SIMPLE_FACE UM_HOME UM_MODE_NAME="Home")
elseif(ULTIMATRIX_MODE STREQUAL "scanner")
    set(sport "faces")
    set(mode_definitions UM_SIMPLE_FACE UM_SCANNER UM_MODE_NAME="Scanner")
    if(DEFINED ENV{UM_PATTERN})
        list(APPEND mode_definitions UM_PATTERN)  # static test card instead of the moving bar (set UM_PATTERN=1 when building)
        if("$ENV{UM_PATTERN}" STREQUAL "2")
            list(APPEND mode_definitions UM_PATTERN_EDGE)  # UM_PATTERN=2: edge test card (lit edge pixels/lines on black)
        endif()
    endif()
elseif(ULTIMATRIX_MODE STREQUAL "bridge")
    # The stop-over image for old clocks: no feed and no face of its own
    set(sport "faces")
    # Where the bridge looks for the signed face it installs (the update server setting; override with -DULTIMATRIX_BRIDGE_SERVER=...)
    set(ULTIMATRIX_BRIDGE_SERVER "http://192.168.7.77/ultiMatrix" CACHE STRING "Update server the bridge saves when none is set")
    set(mode_definitions UM_SIMPLE_FACE UM_BRIDGE UM_MODE_NAME="Bridge" UM_BRIDGE_SERVER="${ULTIMATRIX_BRIDGE_SERVER}")
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
# The WLED overlay (lib/wled) is compiled into every face except the bridge; ULTIMATRIX_WLED=OFF leaves it out (size comparisons).
# It starts on by default only where the heap allows: the racing and football faces ship with it off until measured.
option(ULTIMATRIX_WLED "Compile the WLED overlay into the face" ON)
if(ULTIMATRIX_WLED AND NOT ULTIMATRIX_MODE STREQUAL "bridge" AND NOT ULTIMATRIX_MODE STREQUAL "scanner")
    list(APPEND sources
        "${root}/lib/wled/WledRealtime.cpp"
        "${root}/lib/wled/WledState.cpp"
        "${root}/lib/wled/WledEffects.cpp"
        "${root}/lib/wled/WledCanvas.cpp"
        "${root}/lib/wled/WledWs.cpp"
        "${root}/lib/wled/WledApi.cpp")
    list(APPEND mode_definitions UM_WLED_OVERLAY)
endif()
# Automatic switching to the sport that is on (lib/core/SportsWatch): every face that has the settings page, so a normal clock can
# switch to a sports face for an event and back (not the bridge or the scanner)
if(NOT ULTIMATRIX_MODE STREQUAL "bridge" AND NOT ULTIMATRIX_MODE STREQUAL "scanner")
    list(APPEND sources "${root}/lib/core/SportsWatch.cpp")
    list(APPEND mode_definitions UM_SPORTS_SWITCH)
endif()
# The timezone table is generated on every build from tools/zones.csv
set(tzdb "${CMAKE_CURRENT_BINARY_DIR}/generated/TzDb.h")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
if(NOT CMAKE_BUILD_EARLY_EXPANSION)  # commands are not scriptable in IDF's requirements pass
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_custom_command(OUTPUT "${tzdb}"
        COMMAND ${Python3_EXECUTABLE} "${root}/tools/gen_tzdb.py" "${tzdb}"
        DEPENDS "${root}/tools/gen_tzdb.py" "${root}/tools/zones.csv" "${root}/tools/zone_coords.csv"
        COMMENT "Generating TzDb.h")
    add_custom_target(tzdb_header DEPENDS "${tzdb}")
endif()
set(extra_requires "")
if(ULTIMATRIX_MODE STREQUAL "pacman" AND ULTIMATRIX_BLUETOOTH)
    set(extra_requires bt)
endif()
idf_component_register(SRCS ${sources}
    INCLUDE_DIRS "${root}/vendor/uzlib" "${root}/lib/core" "${CMAKE_CURRENT_BINARY_DIR}/generated" "${root}/lib/${sport}" "${root}/lib/faces" "${root}/lib/motogp-feed" "${root}/lib/wled" "${root}/clockfaces/${face}"
    REQUIRES espressif__arduino-esp32 clock-libraries esp_wifi esp_netif esp_event espressif__network_provisioning nvs_flash esp_http_client esp-tls app_update json mqtt ${extra_requires})
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    add_dependencies(${COMPONENT_LIB} tzdb_header)
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ${mode_definitions} UM_MODE_ID="${ULTIMATRIX_MODE}" DOUBLE_BUFFER_ON UM_FOOTBALL_SIM)

# Serve the setup assets directly from flash rather than assembling a page on the heap. The text assets are gzip-compressed
# at build time and sent with Content-Encoding: gzip (see lib/core/GzipAsset.h).
set(web_assets settings.html theme.css settings.js auth.js)
if(ULTIMATRIX_WLED AND NOT ULTIMATRIX_MODE STREQUAL "bridge" AND NOT ULTIMATRIX_MODE STREQUAL "scanner")
    list(APPEND web_assets wled.html canvas.html)  # the light page the WLED app opens, and the pixel editor
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/web")
set(web_gz_files "")
foreach(asset ${web_assets})
    set(gz "${CMAKE_CURRENT_BINARY_DIR}/generated/web/${asset}.gz")
    add_custom_command(OUTPUT "${gz}"
        COMMAND ${Python3_EXECUTABLE} "${root}/tools/gzip_asset.py" "${root}/web/${asset}" "${gz}"
        DEPENDS "${root}/web/${asset}" "${root}/tools/gzip_asset.py"
        COMMENT "Compressing ${asset}")
    list(APPEND web_gz_files "${gz}")
    target_add_binary_data(${COMPONENT_LIB} "${gz}" BINARY)
endforeach()
add_custom_target(web_gz DEPENDS ${web_gz_files})
add_dependencies(${COMPONENT_LIB} web_gz)
# Installable-app manifest and its LED-matrix U icons.
target_add_binary_data(${COMPONENT_LIB} "${root}/web/manifest.webmanifest" TEXT)
# Clock face thumbnails for the face picker (64x64, one frame of each face).
foreach(thumb football f1 motogp tetris word mario luigi pacman home)
    target_add_binary_data(${COMPONENT_LIB} "${root}/web/faces/${thumb}.png" BINARY)
endforeach()
foreach(icon icon-192 icon-512 icon-maskable-192 apple-touch-icon favicon-32)
    target_add_binary_data(${COMPONENT_LIB} "${root}/web/icons/${icon}.png" BINARY)
endforeach()
