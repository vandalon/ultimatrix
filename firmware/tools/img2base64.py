#!/usr/bin/env python3
"""Convert a JPEG/PNG into the MQTT payload for the clock's pixels/set topic (64x64 rgb565_base64).

  python3 img2base64.py picture.jpg                 # full JSON payload
  python3 img2base64.py picture.png --data-only     # just the base64 string
  python3 img2base64.py picture.png --notify --title Door --yaml   # notification with the picture
  python3 img2base64.py picture.png --yaml --topic ultimatrix/abff01/pixels/set   # HA action for automations
  python3 img2base64.py photo.jpg --fill --duration 60 --topic ultimatrix/abff01/pixels/set   # prints mosquitto_pub command

Needs Pillow (pip install pillow). Transparency is composited over black.
"""
import argparse, base64, json, shlex, struct, sys
from PIL import Image, ImageOps

def convert(path, fill):
    im = Image.open(path)
    im = ImageOps.exif_transpose(im).convert("RGBA")
    bg = Image.new("RGBA", im.size, (0, 0, 0, 255))
    im = Image.alpha_composite(bg, im).convert("RGB")
    if fill:  # crop to cover the panel
        im = ImageOps.fit(im, (64, 64), Image.LANCZOS)
    else:     # fit inside, black borders
        im = ImageOps.contain(im, (64, 64), Image.LANCZOS)
        canvas = Image.new("RGB", (64, 64))
        canvas.paste(im, ((64 - im.width) // 2, (64 - im.height) // 2))
        im = canvas
    rgb = im.tobytes()
    raw = b"".join(struct.pack("<H", ((rgb[i] & 0xF8) << 8) | ((rgb[i + 1] & 0xFC) << 3) | (rgb[i + 2] >> 3))
                   for i in range(0, len(rgb), 3))
    return base64.b64encode(raw).decode()

def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("image")
    p.add_argument("--fill", action="store_true", help="crop to fill the panel (default: fit with black borders)")
    p.add_argument("--duration", type=int, default=30, help="seconds shown; 0 = until stopped (default 30)")
    p.add_argument("--no-clear", action="store_true", help='send "clear": false')
    p.add_argument("--data-only", action="store_true", help="print only the base64 string")
    p.add_argument("--notify", action="store_true", help="make a notification payload (topic .../notify/set) instead of a pixels/set image")
    p.add_argument("--title", help="notification title (with --notify)")
    p.add_argument("--text", help="notification text (with --notify)")
    p.add_argument("--high", action="store_true", help="high priority notification (with --notify)")
    p.add_argument("--service", action="store_true", help="with --notify --yaml: use notify.send_message (entity from --topic, default notify.ultimatrix_notification)")
    p.add_argument("--yaml", action="store_true", help="print a Home Assistant mqtt.publish action (use --topic to set the topic)")
    p.add_argument("--topic", help="print a mosquitto_pub command for this topic instead of the JSON")
    a = p.parse_args()
    data = convert(a.image, a.fill)
    if a.data_only:
        print(data); return
    if a.notify:
        fields = {"image_rgb565": data, "seconds": a.duration or 30}
        if a.title: fields["title"] = a.title
        if a.text: fields["message"] = a.text
        if a.high: fields["priority"] = "high"
        payload = json.dumps(fields, separators=(",", ":"))
    else:
        payload = json.dumps({"width": 64, "height": 64, "format": "rgb565_base64",
                          "clear": not a.no_clear, "duration": a.duration, "data": data}, separators=(",", ":"))
    default_topic = "ultimatrix/abff01/" + ("notify/set" if a.notify else "pixels/set")
    if a.yaml and a.service and a.notify:
        entity = a.topic or "notify.ultimatrix_notification"
        print("action: notify.send_message\ntarget:\n  entity_id: %s\ndata:\n  message: '%s'" % (entity, payload.replace("'", "''")))
    elif a.yaml:
        topic = a.topic or default_topic
        print("action: mqtt.publish\ndata:\n  topic: %s\n  retain: false\n  payload: '%s'" % (topic, payload.replace("'", "''")))
    elif a.topic:
        print("mosquitto_pub -h BROKER -t %s -m %s" % (shlex.quote(a.topic), shlex.quote(payload)))
    else:
        print(payload)

if __name__ == "__main__":
    main()
