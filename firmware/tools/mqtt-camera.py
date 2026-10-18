#!/usr/bin/env python3
"""View Ultimatrix MQTT camera images without Home Assistant.

macOS setup (Python 3):
  python3 -m venv /tmp/ultimatrix-camera-venv
  /tmp/ultimatrix-camera-venv/bin/pip install 'paho-mqtt>=2,<3' Pillow 'amqtt>=0.11,<0.12'
  /tmp/ultimatrix-camera-venv/bin/python firmware/tools/mqtt-camera.py

By default this script hosts a broker on port 1883. Enable MQTT on the clock,
set its broker to this Mac's LAN IP and leave credentials empty.
Use --host BROKER_IP instead to connect to an existing broker. The default subscription finds
all Ultimatrix screen topics; use --topic ultimatrix/DEVICE_ID/screen to pick one.
The built-in broker accepts anonymous LAN clients; it is for local testing.
Images are changed-only and not retained: change the panel after connecting.
"""
import argparse
import asyncio
from datetime import datetime
from getpass import getpass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from io import BytesIO
from pathlib import Path
import threading
import webbrowser

try:
    import paho.mqtt.client as mqtt
    from PIL import Image
except ImportError:
    raise SystemExit("Install dependencies in a virtual environment: pip install 'paho-mqtt>=2,<3' Pillow 'amqtt>=0.11,<0.12'")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--host', help='Use an existing broker instead of hosting one')
    parser.add_argument('--bind', default='0.0.0.0', help='Built-in broker listen address (default: all interfaces)')
    parser.add_argument('--port', type=int, help='Broker port (1883, or 8883 with --tls)')
    parser.add_argument('--topic', default='ultimatrix/+/screen')
    parser.add_argument('--username')
    parser.add_argument('--password', action='store_true', help='Prompt securely for broker password')
    parser.add_argument('--tls', action='store_true', help='Use TLS with certificate verification')
    parser.add_argument('--ca', help='Custom CA certificate file (requires --tls)')
    parser.add_argument('--output', type=Path, default=Path('mqtt-camera-captures'))
    parser.add_argument('--web-port', type=int, default=0, help='Local preview port (default: choose a free port)')
    parser.add_argument('--no-browser', action='store_true')
    args = parser.parse_args()
    if args.ca and not args.tls:
        parser.error('--ca requires --tls')
    if args.password and not args.username:
        parser.error('--password requires --username')
    if not args.host and (args.tls or args.username or args.password):
        parser.error('Credentials/TLS are for --host; the built-in test broker uses anonymous plain MQTT')
    args.output.mkdir(parents=True, exist_ok=True)
    lock = threading.Lock()
    latest = b''
    count = 0

    class Preview(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path.split('?')[0] == '/frame.png':
                with lock:
                    body = latest
                self.send_response(200 if body else 204)
                self.send_header('Content-Type', 'image/png')
            elif self.path == '/':
                body = b'''<!doctype html><title>Ultimatrix MQTT camera</title>
<style>body{background:#16191c;color:white;font:16px system-ui;text-align:center}img{width:min(80vw,640px);image-rendering:pixelated;background:black}</style>
<h1>MQTT camera</h1><p id="status">Waiting for an image. Change the panel after connecting.</p><img id="screen" hidden>
<script>let active=false;setInterval(async()=>{if(active)return;active=true;try{const r=await fetch('/frame.png?t='+Date.now());if(r.status===200){const blob=await r.blob(),im=document.getElementById('screen'),old=im.src;im.src=URL.createObjectURL(blob);im.hidden=false;if(old)URL.revokeObjectURL(old);document.getElementById('status').textContent='Latest received frame (updates only when panel changes)';}}catch(e){}finally{active=false}},1000)</script>'''
                self.send_response(200)
                self.send_header('Content-Type', 'text/html; charset=utf-8')
            else:
                self.send_error(404)
                return
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *args):
            pass

    def on_connect(client, userdata, flags, reason_code, properties):
        if reason_code.is_failure:
            print(f'Broker rejected connection: {reason_code}', flush=True)
            return
        print(f'Connected; subscribing to {args.topic}', flush=True)
        client.subscribe([(args.topic, 0), (args.topic.rsplit('/', 1)[0] + '/status', 0)])

    def on_message(client, userdata, message):
        nonlocal latest, count
        if message.topic.endswith('/status'):
            if message.payload == b'online':
                client.publish(message.topic.rsplit('/', 1)[0] + '/screen/set', 'snapshot', qos=1)
                print('Requested current screen from ' + message.topic.rsplit('/', 1)[0], flush=True)
            return
        try:
            is_png = message.payload.startswith(b'\x89PNG\r\n\x1a\n')
            if len(message.payload) > 100000 or not (is_png or message.payload.startswith(b'BM')):
                raise ValueError('Expected a small BMP or PNG image')
            with Image.open(BytesIO(message.payload)) as image:
                if image.size not in ((64, 64), (512, 512)):
                    raise ValueError(f'Unexpected dimensions: {image.size}')
                dimensions = image.size
                image.load()
                png = BytesIO()
                image.save(png, format='PNG')
            count += 1
            stamp = datetime.now().strftime('%Y%m%d-%H%M%S-%f')
            suffix = 'png' if is_png else 'bmp'
            path = args.output / f'{stamp}-{count:05d}.{suffix}'
            path.write_bytes(message.payload)
            with lock:
                latest = png.getvalue()
            print(f'#{count} {message.topic}: {len(message.payload)} bytes, {dimensions[0]}x{dimensions[1]} -> {path}', flush=True)
        except (OSError, ValueError) as error:
            print(f'Cannot save/decode {message.topic}: {error}', flush=True)

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_connect = on_connect
    client.on_message = on_message
    if args.username:
        client.username_pw_set(args.username, getpass('MQTT password: ') if args.password else None)
    if args.tls:
        client.tls_set(ca_certs=args.ca)
    broker_loop = None
    broker = None
    broker_thread = None
    if not args.host:
        try:
            from amqtt.broker import Broker
        except ImportError:
            raise SystemExit("Install the broker dependency: pip install 'amqtt>=0.11,<0.12'")
        broker_loop = asyncio.new_event_loop()
        broker_thread = threading.Thread(target=broker_loop.run_forever, daemon=True)
        broker_thread.start()

        async def start_broker():
            instance = Broker({
                'listeners': {'default': {'type': 'tcp', 'bind': f'{args.bind}:{args.port or 1883}'}},
                'plugins': {'amqtt.plugins.authentication.AnonymousAuthPlugin': {'allow_anonymous': True}},
            })
            await instance.start()
            return instance

        try:
            broker = asyncio.run_coroutine_threadsafe(start_broker(), broker_loop).result(timeout=15)
        except Exception as error:
            broker_loop.call_soon_threadsafe(broker_loop.stop)
            raise SystemExit(f'Cannot start MQTT broker: {error}')
        print(f'MQTT broker listening on {args.bind}:{args.port or 1883}. Point the clock at this Mac’s LAN IP; no credentials.', flush=True)
    server = ThreadingHTTPServer(('127.0.0.1', args.web_port), Preview)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = f'http://127.0.0.1:{server.server_port}/'
    print(f'Preview: {url}\nCaptures: {args.output.resolve()}\nCtrl+C to stop.', flush=True)
    if not args.no_browser:
        webbrowser.open(url)
    try:
        client.connect(args.host or '127.0.0.1', args.port or (8883 if args.tls else 1883), keepalive=30)
        client.loop_forever()
    except KeyboardInterrupt:
        pass
    except (OSError, ValueError) as error:
        raise SystemExit(f'MQTT connection failed: {error}')
    finally:
        client.disconnect()
        server.shutdown()
        server.server_close()
        if broker:
            asyncio.run_coroutine_threadsafe(broker.shutdown(), broker_loop).result(timeout=10)
            broker_loop.call_soon_threadsafe(broker_loop.stop)
            broker_thread.join(timeout=5)
            broker_loop.close()


if __name__ == '__main__':
    main()
