# Draw on the panel with MQTT

Use Home Assistant automations to draw pixels or send a full image. This uses the existing MQTT connection; it does not expose the clock as an MQTT light.

Enable **Home Assistant (MQTT)** in the clock settings and connect Home Assistant to the same broker. Replace `abff01` in the examples with your clock's MQTT ID: the last six hexadecimal digits of its Wi-Fi MAC address, lowercase, without colons. The topic stays the same when you rename the clock. You can also find it in the device's MQTT discovery information or listen to `ultimatrix/+/display` in Home Assistant's MQTT integration.

## Topics and entities

| Topic, below `ultimatrix/abff01/` | Purpose |
| --- | --- |
| `pixels/set` | Draw pixels, replace the image, or stop pixel display. Publish JSON with retain off. |
| `pixels/status` | Command result: `ok`, or an error message. Not retained. |
| `display` | Current display mode. Retained; updates when the mode changes. |

Home Assistant discovers a new entity on the clock's device:

- **Display**, a sensor whose state is `clock`, `pixels`, `wled`, `image`, `notification`, `birthday`, or `boot`. The dim night clock also counts as `clock`.

The existing Screen camera remains available for a visual preview.

## Draw a small image from an automation

This example shows a green tick near the centre when a washing-machine sensor changes to `finished`. Use your own entity and the state it actually reports. Paste this into the automation YAML editor:

```yaml
alias: Clock - washing finished
triggers:
  - trigger: state
    entity_id: sensor.washing_machine
    to: "finished"
actions:
  - action: mqtt.publish
    data:
      topic: ultimatrix/abff01/pixels/set
      retain: false
      payload: >-
        {
          "clear": true,
          "duration": 30,
          "pixels": [
            [29, 32, "00FF00"],
            [30, 33, "00FF00"],
            [31, 34, "00FF00"],
            [32, 33, "00FF00"],
            [33, 32, "00FF00"],
            [34, 31, "00FF00"]
          ]
        }
mode: restart
```

Each pixel is `[x, y, "RRGGBB"]`. Coordinates range from 0 to 63, with `(0, 0)` at the top left of the oriented display. Colours are exactly six hexadecimal digits, without `#`.

- `clear: true` starts with a black image. Otherwise, updates keep the previous MQTT pixel image. If there is no MQTT image yet, the starting image is black; it does not copy the clock face.
- `pixels` updates just the listed positions. The last entry wins if a position appears more than once.
- `duration` is seconds, defaults to 30, and resets after every accepted drawing command. Values from 1 to 86400 are timed; 0 holds the image until stopped, replaced, MQTT is disabled, or the clock restarts.
- Optional `format: "xy_rgb"`, `width: 64`, and `height: 64` describe this format. Other sizes are rejected.
- A message can contain at most 512 coordinate entries and 12,288 UTF-8 bytes. Split larger coordinate drawings into messages, clearing only in the first. Each message commits atomically; separate messages are separate visible updates. Use the compact full-frame format below to replace a complete image in one update.

Pixel images take priority over the night clock, DLNA, WLED and the usual face. Notifications that qualify to show still take priority over pixels. Brightness, automatic dimming and the night brightness limit still apply. When the pixel image ends, the usual display resumes, including an existing WLED effect or DLNA picture. Nothing is saved to the clock's flash.

## Update only a few pixels

For example, change the tick to red without clearing other pixels:

```yaml
action: mqtt.publish
data:
  topic: ultimatrix/abff01/pixels/set
  retain: false
  payload: >-
    {"clear":false,"duration":30,"pixels":[[29,32,"FF0000"],[30,33,"FF0000"],[31,34,"FF0000"],[32,33,"FF0000"],[33,32,"FF0000"],[34,31,"FF0000"]]}
```

Use Home Assistant templates in `payload` to choose colours or coordinates from sensor values. For frequent updates, send related pixels together in one message. This interface is intended for automation-driven images and indicators, not video. Use DDP for smooth high-rate streams.

## Send a full base64 image

A full-frame command replaces all pixels and carries the whole picture as base64 RGB565:

```json
{
  "width": 64,
  "height": 64,
  "format": "rgb565_base64",
  "clear": true,
  "duration": 30,
  "data": "..."
}
```

The real `data` value is 10,924 base64 characters (the one above is shortened and cannot be sent). It encodes 8,192 bytes: 4,096 little-endian RGB565 words, left to right, then top to bottom. Colours are reduced to 5 red, 6 green and 5 blue bits. A full-frame command cannot also contain `pixels`. This keeps a full image to about 11 KB instead of a much larger list of 4,096 coordinate entries. To convert a JPEG or PNG, run `python3 firmware/tools/img2base64.py picture.jpg` (needs Pillow; `--fill` crops instead of adding black borders, `--duration`, `--data-only`). It prints the complete payload. Add `--notify` to get a notification payload instead (`image_rgb565` for `notify/set`, with optional `--title`, `--text`, `--high`, `--duration`), and `--yaml` for a complete Home Assistant action. Publish the pixel payload as the payload of `pixels/set`:

```yaml
action: mqtt.publish
data:
  topic: ultimatrix/abff01/pixels/set
  retain: false
  payload: >-
    PASTE_THE_COMPLETE_JSON_OBJECT_HERE
```

## Stop the image

```yaml
action: mqtt.publish
data:
  topic: ultimatrix/abff01/pixels/set
  retain: false
  payload: '{"stop":true}'
```

## Storage and errors

Keep `retain: false` on drawing commands. Retained drawing commands are rejected so they cannot unexpectedly take over after reconnecting.

Uploads are assembled across MQTT transport fragments. An incomplete upload times out after ten seconds. Unknown fields, duplicate fields, malformed JSON, invalid colours/coordinates, oversized messages and unavailable memory are rejected without changing the current image. Keys and string values in this command format do not support JSON escape sequences; the documented keys, hex colours and base64 data do not need them.

Only one completed pixel command can wait to be applied at a time. If a sender outruns the clock, `pixels/status` reports that it is busy; slow down and retry. A rejected command does not refresh the display timeout. Feeds pause while a pixel image is held and resume afterwards, as they do for other full-panel images. Memory is limited, so large uploads can fail while other services use the heap. The existing screen camera is usually cheaper for a live preview.

Home Assistant references: [MQTT publish](https://www.home-assistant.io/actions/mqtt.publish/).
