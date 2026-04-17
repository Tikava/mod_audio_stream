#!/usr/bin/env node
/**
 * Minimal WebSocket test server for mod_audio_stream (Node.js).
 *
 * - Logs incoming text/binary messages.
 * - Echoes text back in a JSON envelope.
 * - Optionally sends a streamAudio JSON with a provided raw PCM file (16-bit LE).
 *
 * Usage:
 *   node tools/ws_test_server.js --port 8765
 *   node tools/ws_test_server.js --port 8765 --send-raw sample.raw --rate 8000 --channels 1
 *
 * Requires: npm install ws
 */

const fs = require('fs');
const path = require('path');
const WebSocket = require('ws');

function parseArgs() {
  const args = {
    host: '0.0.0.0',
    port: 8765,
    sendRaw: null,
    rate: 8000,
    channels: 1,
    frameMs: 20,
    help: false
  };

  const argv = process.argv.slice(2);
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--help' || a === '-h') {
      args.help = true;
    } else if (a === '--port' && argv[i + 1]) {
      args.port = parseInt(argv[++i], 10);
    } else if (a === '--host' && argv[i + 1]) {
      args.host = argv[++i];
    } else if (a === '--send-raw' && argv[i + 1]) {
      args.sendRaw = argv[++i];
    } else if (a === '--rate' && argv[i + 1]) {
      args.rate = parseInt(argv[++i], 10);
    } else if (a === '--channels' && argv[i + 1]) {
      args.channels = parseInt(argv[++i], 10);
    } else if (a === '--frame-ms' && argv[i + 1]) {
      args.frameMs = parseInt(argv[++i], 10);
    }
  }
  return args;
}

function printUsage() {
  console.log(`Usage:
  node tools/ws_test_server.js [--host 0.0.0.0] [--port 8765] [--send-raw file.raw] [--rate 8000] [--channels 1] [--frame-ms 20]

Examples:
  node tools/ws_test_server.js --port 8765
  node tools/ws_test_server.js --port 8765 --send-raw sample.raw --rate 16000 --channels 1 --frame-ms 20`);
}

async function main() {
  const opts = parseArgs();
  if (opts.help) {
    printUsage();
    process.exit(0);
  }

  let rawPayload = null;
  if (opts.sendRaw) {
    try {
      const buf = fs.readFileSync(path.resolve(opts.sendRaw));
      rawPayload = buf.toString('base64');
      console.log(`[init] loaded raw file ${opts.sendRaw} (${buf.length} bytes)`);
    } catch (err) {
      console.error(`[init] failed to read raw file: ${err.message}`);
    }
  }

  const wss = new WebSocket.Server({ host: opts.host, port: opts.port }, () => {
    console.log(`[*] listening on ws://${opts.host}:${opts.port}`);
  });

  wss.on('connection', (ws, req) => {
    const peer = req.socket.remoteAddress + ':' + req.socket.remotePort;
    console.log(`[+] connection from ${peer}`);

    let seq = 0;
    let upstreamRate = opts.rate || 8000;
    let upstreamChannels = opts.channels || 1;
    const frameMs = opts.frameMs || 20;

    if (rawPayload) {
      const payload = {
        type: 'streamAudio',
        data: {
          audioDataType: 'raw',
          sampleRate: upstreamRate,
          channels: upstreamChannels,
          audioData: rawPayload
        }
      };
      ws.send(JSON.stringify(payload));
      console.log(`[>] sent streamAudio: bytes=${Buffer.from(rawPayload, 'base64').length} rate=${upstreamRate} ch=${upstreamChannels}`);
    }

    ws.on('message', (data, isBinary) => {
      if (isBinary) {
        // если знаем длительность кадра, попробуем оценить частоту дискретизации
        const samplesPerFrame = data.length / 2 / upstreamChannels;
        const guessedRate = Math.round(samplesPerFrame * (1000 / frameMs));
        if (guessedRate > 0 && guessedRate !== upstreamRate) {
          upstreamRate = guessedRate;
        }
        console.log(`[<] binary: ${data.length} bytes (rate≈${upstreamRate} Hz, ch=${upstreamChannels})`);
        // Эхо назад как streamAudio raw с теми же rate/ch
        const payload = {
          type: 'streamAudio',
          data: {
            audioDataType: 'raw',
            sampleRate: upstreamRate,
            channels: upstreamChannels,
            audioData: data.toString('base64'),
            seq: seq++
          }
        };
        ws.send(JSON.stringify(payload));
        return;
      }
      const text = data.toString();
      console.log(`[<] text: ${text.substring(0, 200)}`);
      try {
        const msg = JSON.parse(text);
        if (msg.type === 'rawAudio' && msg.data) {
          if (msg.data.sampleRate) upstreamRate = msg.data.sampleRate;
          if (msg.data.channels) upstreamChannels = msg.data.channels;
          console.log(`[meta] rawAudio from FS: rate=${upstreamRate} ch=${upstreamChannels}`);
        }
      } catch (e) {
        const reply = { echo: text };
        ws.send(JSON.stringify(reply));
        console.log('[>] echo reply (non-JSON)');
      }
    });

    ws.on('close', (code, reason) => {
      console.log(`[-] closed code=${code} reason=${reason}`);
    });

    ws.on('error', (err) => {
      console.error(`[!] error: ${err.message}`);
    });
  });
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
