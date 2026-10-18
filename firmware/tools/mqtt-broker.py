#!/usr/bin/env python3
"""Simple anonymous MQTT broker for local testing on macOS.

Install: pip install 'amqtt>=0.11,<0.12'
Run:     python mqtt-broker.py
Point devices at this Mac's LAN IP, port 1883, with no credentials.
"""
import argparse
import asyncio
import logging


async def serve(bind, port):
    from amqtt.broker import Broker

    broker = Broker({
        'listeners': {'default': {'type': 'tcp', 'bind': f'{bind}:{port}'}},
        'plugins': {
            'amqtt.plugins.authentication.AnonymousAuthPlugin': {'allow_anonymous': True},
        },
    })
    try:
        await broker.start()
    except Exception as error:
        raise SystemExit(f'Cannot start MQTT broker on {bind}:{port}: {error}')
    print(f'MQTT broker listening on {bind}:{port}', flush=True)
    print('Use this Mac’s LAN IP on your devices. No username/password. Ctrl+C to stop.', flush=True)
    try:
        await asyncio.Event().wait()
    finally:
        await broker.shutdown()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--bind', default='0.0.0.0', help='Listen address (default: all interfaces)')
    parser.add_argument('--port', type=int, default=1883)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error('--port must be between 1 and 65535')
    logging.basicConfig(level=logging.WARNING)
    try:
        asyncio.run(serve(args.bind, args.port))
    except ImportError:
        raise SystemExit("Install the broker dependency: pip install 'amqtt>=0.11,<0.12'")
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
