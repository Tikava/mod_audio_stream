#!/usr/bin/env python3
"""
Minimal WebSocket test server for mod_audio_stream.

- Logs all incoming text and binary messages.
- Echoes text back with a small JSON envelope.
- Optionally sends a streamAudio JSON with a provided raw PCM file (16-bit little-endian).

Usage:
  python3 tools/ws_test_server.py --port 8765
  python3 tools/ws_test_server.py --port 8765 --send-raw sample.raw --rate 8000 --channels 1

Requires:
  pip install websockets
"""
import argparse
import asyncio
import base64
import json
import pathlib
import websockets


async def handler(websocket, path, opts):
    peer = websocket.remote_address
    print(f"[+] connection from {peer}")

    # Optionally send one streamAudio message on connect
    if opts.send_raw:
        try:
            raw = pathlib.Path(opts.send_raw).read_bytes()
            payload = {
                "type": "streamAudio",
                "data": {
                    "audioDataType": "raw",
                    "sampleRate": opts.rate,
                    "audioData": base64.b64encode(raw).decode("ascii"),
                },
            }
            await websocket.send(json.dumps(payload))
            print(f"[>] sent streamAudio: bytes={len(raw)} rate={opts.rate}")
        except Exception as exc:
            print(f"[!] failed to send raw audio: {exc}")

    try:
        async for message in websocket:
            if isinstance(message, str):
                print(f"[<] text: {message[:200]}")
                reply = {"echo": message}
                await websocket.send(json.dumps(reply))
                print(f"[>] echo reply")
            else:
                print(f"[<] binary: {len(message)} bytes")
                # no echo for binary to avoid flooding
    except websockets.ConnectionClosed as e:
        print(f"[-] connection closed: code={e.code} reason={e.reason}")
    except Exception as exc:
        print(f"[!] handler exception: {exc}")


def main():
    parser = argparse.ArgumentParser(description="Test WebSocket server for mod_audio_stream")
    parser.add_argument("--port", type=int, default=8765, help="Port to listen on")
    parser.add_argument("--host", default="0.0.0.0", help="Host to bind")
    parser.add_argument("--send-raw", help="Path to raw PCM file (16-bit little-endian) to send once on connect")
    parser.add_argument("--rate", type=int, default=8000, help="Sample rate for the raw PCM (used in JSON metadata)")
    parser.add_argument("--channels", type=int, default=1, help="Channels for the raw PCM (unused by client but informational)")
    args = parser.parse_args()

    async def ws_handler(ws, path):
        await handler(ws, path, args)

    start_server = websockets.serve(ws_handler, args.host, args.port)
    print(f"[*] listening on ws://{args.host}:{args.port}")
    asyncio.get_event_loop().run_until_complete(start_server)
    asyncio.get_event_loop().run_forever()


if __name__ == "__main__":
    main()
