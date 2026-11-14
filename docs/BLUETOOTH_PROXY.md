# Bluetooth proxy

The clock can work as a Bluetooth proxy for Home Assistant. It listens for Bluetooth LE adverts (the short broadcasts that phones, watches, beacons and sensors send) and passes them to Home Assistant. Home Assistant then treats the clock like an ESPHome device that has `bluetooth_proxy:` switched on. Room-presence tools use this to see how strong each device is at each clock. Examples are Bermuda, iBeacon and Private BLE Device.

## What it does

- It scans passively by default. When Home Assistant asks for active scanning (the scanning mode of the ESPHome device), the clock also asks devices for their scan response, as an ESPHome proxy does. Many phones put their name and service data there. It never connects to devices.
- It forwards raw adverts with their signal strength, up to 31 bytes of data each.
- Home Assistant sees it as an ESPHome device with the passive scan, raw advert and scanner state features.
- It does not support active connections (reading or writing device values, pairing). Devices that need an active connection, such as some Bluetooth sensors with a GATT link, need an ESPHome proxy.

## Set up

1. Open the clock's settings page. Go to Advanced > Bluetooth proxy and press **Turn Bluetooth proxy on**. The clock restarts.
2. In Home Assistant, open Settings > Devices & services. The clock appears as a discovered ESPHome device. Press **Configure**.
3. If it does not appear, press **Add integration**, choose ESPHome, and enter the address of the clock with port 6053.
4. Leave the encryption key field empty. This clock does not support the encrypted transport yet.
5. Add the Bluetooth integration if it is not there yet. The clock shows up under its Bluetooth scanners.

Apply the same steps for every clock you want to use as a proxy. Place them in different rooms.

## Limits

- The setting is off by default. While it is on, the Bluetooth stack keeps about 90 KB of memory, and the flash image is larger. The clock restarts when you switch it.
- The link is plain text on your local network, without encryption. Anyone who can reach port 6053 can read the adverts the clock hears. Set Settings > Network > Who may open the clock to your own network.
- Only one Home Assistant connection works at a time. A new connection replaces the old one.
- The proxy pauses by itself while an update runs, while a notification or picture loads, while WLED has the panel, and when free memory is low. It resumes afterwards.
- Adverts are dropped when the 32-entry queue is full or the link is slow. The settings page shows the counts.
- Faces without enough free flash do not carry it: Pac-Man, Football and Home.
- Only the classic ESP32 boards are supported, as for the rest of the firmware.

## API

`GET /api/btproxy` returns `enabled`, `state`, `client`, `sent` and `dropped`. `POST /api/btproxy/enable` with `on=1` or `on=0` switches it and restarts the clock. See the [OpenAPI description](openapi.yaml).
