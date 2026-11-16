# Bluetooth proxy

The clock can work as a Bluetooth proxy for Home Assistant. It listens for Bluetooth LE adverts (the short broadcasts that phones, watches, beacons and sensors send) and passes them to Home Assistant. Home Assistant then treats the clock like an ESPHome device that has `bluetooth_proxy:` switched on. Room-presence tools use this to see how strong each device is at each clock. Examples are Bermuda, iBeacon and Private BLE Device.

## What it does

- It scans passively by default. When Home Assistant asks for active scanning (the scanning mode of the ESPHome device), the clock also asks devices for their scan response, as an ESPHome proxy does. Many phones put their name and service data there. It never connects to devices.
- It forwards raw adverts with their signal strength, up to 31 bytes of data each.
- Home Assistant sees it as an ESPHome device with the passive scan, raw advert and scanner state features.
- It does not support active connections (reading or writing device values, pairing). Devices that need an active connection, such as some Bluetooth sensors with a GATT link, need an ESPHome proxy.

## Set up

1. Open the clock's settings page. Go to Advanced > Bluetooth proxy and press **Turn Bluetooth proxy on**. The clock restarts.
2. In Home Assistant, open Settings > Devices & services. The clock appears as a discovered ESPHome device. Press **Configure**. Home Assistant finds the clock through mDNS, so the clock keeps mDNS on while the proxy is on, even after an out-of-memory event.
3. If it does not appear, press **Add integration**, choose ESPHome, and enter the address of the clock with port 6053.
4. Leave the encryption key field empty. This clock does not support the encrypted transport yet.
5. Add the Bluetooth integration if it is not there yet. The clock shows up under its Bluetooth scanners.

Apply the same steps for every clock you want to use as a proxy. Place them in different rooms.

## Limits

- The setting is off by default. While it is on, the Bluetooth controller keeps about 60 KB of memory (RAM). The clock restarts when you switch it, and gives the memory back when the proxy is off.
- The link is plain text on your local network, without encryption. Anyone who can reach port 6053 can read the adverts the clock hears. Set Settings > Network > Who may open the clock to your own network.
- Only one Home Assistant connection works at a time. A new connection replaces the old one.
- The proxy pauses by itself while an update runs, while a notification or picture loads, while WLED has the panel, and when free memory is low. It resumes afterwards.
- Adverts are dropped when the 32-entry queue is full or the link is slow. The settings page shows the counts.
- The classic ESP32 has no extra memory chip, so the proxy competes with the faces for RAM. On the classic ESP32, Home and the sports faces (Football, F1, MotoGP, NBA, MLB, NFL) are very tight: while the proxy is on, WLED, the canvas, GIFs, DLNA and notifications stay off on those faces, and the panel keeps 4 rows ready instead of 8. Home also turns MQTT off while the proxy is on (the MQTT settings are kept and apply again when the proxy is off), and the proxy stops scanning while Home's link to Home Assistant signs in. On these faces the proxy also stops scanning while a feed downloads (ESPN scores, schedules), usually a few seconds, and while the settings page is in use (up to 10 seconds after its last request); the sports faces then also pause their score feeds. An update or face install stops the proxy and gives all its memory to the download; it starts again with the restart (also when the install fails). The proxy pauses scanning while a feed refresh dips the free memory below 10 KB, and lets go of its connection for a few seconds after any allocation failure. The ESP32-S3 board (Adafruit Matrix Portal S3) has extra memory and needs none of this.
- Measured on the classic ESP32 with the proxy on and Home Assistant connected, through an update check, opening the settings page and saving a setting (16 November 2026): the lowest free memory was about 7 KB on Football, 8 KB on MotoGP, 12 KB on F1, 14 KB on NBA, 17 KB on NFL and 22 KB on MLB, and none of them dropped off the network. Home needs more room for its own Home Assistant link (TLS and the energy data): with the proxy on, the classic ESP32 runs Home at 7 bit colour, keeps one 7 KB block for TLS records and turns MQTT off; its lowest point (about 2 KB) comes when its Home Assistant link signs in. The game and simple faces were not measured again after these changes (36 KB or more earlier).
- Pac-Man carries the proxy instead of the old Bluetooth game controller support, which the default build no longer includes.

## API

`GET /api/btproxy` returns `enabled`, `state`, `client`, `sent` and `dropped`. `POST /api/btproxy/enable` with `on=1` or `on=0` switches it and restarts the clock. See the [OpenAPI description](openapi.yaml).
