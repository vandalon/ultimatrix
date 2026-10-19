# Draw and capture the panel with MQTT

Use Home Assistant automations to draw pixels, capture the current panel, and replay a captured image later. This uses the existing MQTT connection; it does not expose the clock as an MQTT light.

Enable **Home Assistant (MQTT)** in the clock settings and connect Home Assistant to the same broker. Replace `abff01` in the examples with your clock's MQTT ID: the last six hexadecimal digits of its Wi-Fi MAC address, lowercase, without colons. The topic stays the same when you rename the clock. You can also find it in the device's MQTT discovery information or listen to `ultimatrix/+/display` in Home Assistant's MQTT integration.

## Topics and entities

| Topic, below `ultimatrix/abff01/` | Purpose |
| --- | --- |
| `pixels/set` | Draw pixels, replace the image, or stop pixel display. Publish JSON with retain off. |
| `pixels/status` | Command result: `ok`, or an error message. Also reports snapshot errors. Not retained. |
| `display` | Current display mode. Retained; updates when the mode changes. |
| `display/image` | Captured image and capture metadata. Retained; changes only on requested captures (or an explicitly enabled capture interval). |
| `snapshot/set` | Request a capture by publishing `PRESS` with retain off. |

Home Assistant discovers two new entities on the clock's device:

- **Display**, a sensor whose state is `clock`, `pixels`, `wled`, `image`, `notification`, `birthday`, or `boot`. The dim night clock also counts as `clock`.
- **Capture display**, a button that requests a new image. The existing Screen camera remains available for a visual preview.

The Display sensor's `image` attribute is a drawing command you can replay unchanged. Its other attributes are `source` (the mode at capture time) and `captured_uptime_ms` (milliseconds since the clock started). These attributes describe the **last requested capture**. They do not update with every animation frame; automatic captures are off by default. The sensor's state continues to show the current mode.

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

## Capture an image and save its JSON

1. Show the image you want on the panel. It can be a face, a drawing, an effect, a notice, or a DLNA picture.
2. Press **Capture display** on the clock's Home Assistant device page.
3. Open **Developer tools → States** and select the clock's **Display** sensor. Wait for `captured_uptime_ms` to change. A retained capture can be from an earlier boot, so compare it with the value before pressing the button.
4. In **Developer tools → Template**, enter the following, using your sensor's actual entity ID:

   ```jinja
   {{ state_attr('sensor.living_room_clock_display', 'image') | to_json }}
   ```

5. Copy the rendered JSON object. Copy just `image`, not the entire sensor state or the outer attributes object. Keep this JSON in your automation or a reusable script.

A capture is a single still frame of the visible front buffer, taken after rendering. It includes clock digits, overlays and any animation pose visible at that instant. The pixels are stored as RGB565: colours are reduced to 5 red bits, 6 green bits and 5 blue bits. Captures preserve image content, not the physical LEDs' brightness setting. Replaying uses the clock's current brightness controls.

The JSON has this shape (the `data` value below is shortened and **cannot be replayed**):

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

The real `data` value is 10,924 base64 characters. It encodes 8,192 bytes: 4,096 little-endian RGB565 words, left to right, then top to bottom. A full-frame command replaces all pixels; it cannot also contain `pixels`. This keeps a full image to about 11 KB instead of a much larger list of 4,096 coordinate entries.

To replay the saved picture, use an MQTT publish action with the **complete copied object** as its payload:

```yaml
action: mqtt.publish
data:
  topic: ultimatrix/abff01/pixels/set
  retain: false
  payload: >-
    PASTE_THE_COMPLETE_COPIED_JSON_OBJECT_HERE
```

Replace the placeholder line with your JSON. You can change its `duration` before saving the automation. This stored JSON remains the same picture even after the sensor receives a newer capture.

## Replay the last capture directly

This action reads the sensor's current `image` attribute. Unlike copying the JSON into a script, it follows whichever picture was captured most recently:

```yaml
action: mqtt.publish
data:
  topic: ultimatrix/abff01/pixels/set
  retain: false
  payload: >-
    {{ state_attr('sensor.living_room_clock_display', 'image') | to_json }}
```

Only run it after a successful capture; an empty attribute renders as `null`, which is not a drawing command.

You can also request a capture from an automation or Developer tools → Actions:

```yaml
action: mqtt.publish
data:
  topic: ultimatrix/abff01/snapshot/set
  retain: false
  payload: PRESS
```

Captures are on demand, with a minimum two-second gap. Requests may be combined while one is pending. They wait for the MQTT outbox to empty. If there is insufficient heap or display readback is unavailable, the previous capture stays unchanged and `pixels/status` reports why. Listen to that topic when diagnosing a failed capture or upload.

## Optional periodic capture

Automatic captures are off by default. Publish a whole-number interval from `10` to `3600` seconds to `snapshot/set` to enable them. For example:

```yaml
action: mqtt.publish
data:
  topic: ultimatrix/abff01/snapshot/set
  retain: false
  payload: "30"
```

This requests a capture immediately, then every 30 seconds. Publish `OFF` to the same topic to stop automatic captures. Pressing **Capture display** still requests one capture without changing the interval. The interval is not saved across clock restarts. Periodic captures can fail under memory pressure just like manual ones; keep this sensor out of Recorder.

## Stop the image

```yaml
action: mqtt.publish
data:
  topic: ultimatrix/abff01/pixels/set
  retain: false
  payload: '{"stop":true}'
```

## Storage and errors

Keep `retain: false` on drawing and capture commands. Retained drawing commands are rejected so they cannot unexpectedly take over after reconnecting. The captured image itself is retained on the broker for Home Assistant to receive after reconnecting. A broker restart without persistent storage can lose it; keeping the JSON in a script is the durable copy.

Exclude the Display sensor from Recorder to avoid storing its large image attribute repeatedly. Merge this into your existing configuration; use your sensor's entity ID:

```yaml
recorder:
  exclude:
    entities:
      - sensor.living_room_clock_display
```

Uploads are assembled across MQTT transport fragments. An incomplete upload times out after ten seconds. Unknown fields, duplicate fields, malformed JSON, invalid colours/coordinates, oversized messages and unavailable memory are rejected without changing the current image. Keys and string values in this command format do not support JSON escape sequences; the documented keys, hex colours and base64 data do not need them.

Only one completed pixel command can wait to be applied at a time. If a sender outruns the clock, `pixels/status` reports that it is busy; slow down and retry. A rejected command does not refresh the display timeout. Feeds pause while a pixel image is held and resume afterwards, as they do for other full-panel images. Memory is limited, so large uploads or captures can fail while other services use the heap. The existing screen camera is usually cheaper for a live preview.

Home Assistant references: [MQTT publish](https://www.home-assistant.io/actions/mqtt.publish/), [MQTT sensor attributes](https://www.home-assistant.io/integrations/sensor.mqtt/), and [Recorder exclusions](https://www.home-assistant.io/integrations/recorder/).
