# mod_audio_stream

A FreeSWITCH module that streams L16 audio from a channel to a WebSocket endpoint and plays back audio received from the server. Suitable for ASR engines, real-time AI voice assistants, or any bidirectional audio streaming use case.

## Features

- **Full-duplex streaming** — sends audio to the server and plays back server audio simultaneously
- **Automatic resampling** — SpeexDSP resampler for any sample rate conversion in both directions
- **Raw PCM playback** — incoming raw audio is decoded, resampled, and written directly to the FreeSWITCH channel (no temp files)
- **File-based playback** — wav/mp3/ogg received as base64 are saved to a temp file; path sent via `play` event for the application to act on
- **Auto-reconnect** — exponential backoff reconnect (1 → 2 → 4 → 8 → 16 s) on connection drop
- **Backpressure control** — frames are dropped (and counted) when the WebSocket send buffer exceeds 512 KB
- **Per-message deflate** — zlib compression enabled by default
- **mTLS support** — optional client certificate + CA verification for WSS connections
- **Metrics API** — live connection and playback statistics via `uuid_audio_stream metrics`

## About

- Uses [libwsc](https://github.com/amigniter/libwsc), an in-house RFC-6455 compliant WebSocket client based on libevent.
- Inspired by mod_audio_fork.

---

## Installation

### Dependencies

```
libfreeswitch-dev  libssl-dev  zlib1g-dev  libevent-dev  libspeexdsp-dev
```

### Building from source

```bash
git clone https://github.com/amigniter/mod_audio_stream.git
cd mod_audio_stream
git submodule init && git submodule update

mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make
sudo make install
```

If FreeSWITCH was built from source (e.g. installed to `/usr/local/freeswitch`):
```bash
export PKG_CONFIG_PATH=/usr/local/freeswitch/lib/pkgconfig
```

#### DEB package

```bash
cd build
cpack -G DEB
# output: _packages/mod-audio-stream_*.deb
```

### Scripted install (Debian/Ubuntu)

```bash
sudo apt-get install -y git \
  && cd /usr/src/ \
  && git clone https://github.com/amigniter/mod_audio_stream.git \
  && cd mod_audio_stream \
  && sudo bash ./build-mod-audio-stream.sh
```

---

## FreeSWITCH XML Configuration

Global defaults can be set in `autoload_configs/audio_stream.conf.xml`:

```xml
<configuration name="audio_stream.conf" description="mod_audio_stream config">
  <settings>
    <!-- Disable per-message deflate compression globally -->
    <param name="message-deflate" value="false"/>

    <!-- WebSocket ping interval in seconds (0 = disabled) -->
    <param name="heart-beat" value="0"/>

    <!-- Suppress websocket response logging -->
    <param name="suppress-log" value="false"/>

    <!-- Audio buffer size in ms sent per WebSocket frame (must be multiple of 20) -->
    <param name="buffer-size" value="20"/>

    <!-- Disable automatic reconnection on connection drop -->
    <param name="no-reconnect" value="false"/>
  </settings>
</configuration>
```

Channel variables always override XML config values (see below).

---

## Channel Variables

Fine-tune the connection per-session by setting these variables before calling `start`:

| Variable | Description | Default |
|---|---|---|
| `STREAM_MESSAGE_DEFLATE` | `true`/`1` — disable per-message deflate (zlib) | compression **on** |
| `STREAM_HEART_BEAT` | Seconds between WebSocket pings; `0` = disabled | `0` |
| `STREAM_SUPPRESS_LOG` | `true`/`1` — suppress logging of server responses | `false` |
| `STREAM_BUFFER_SIZE` | Audio buffer duration in ms before sending (multiple of 20) | `20` |
| `STREAM_EXTRA_HEADERS` | JSON object of extra HTTP headers sent on WS handshake | — |
| `STREAM_NO_RECONNECT` | `true`/`1` — disable auto-reconnect on drop | `false` |
| `STREAM_TLS_CA_FILE` | CA certificate/bundle file for WSS. Special: `SYSTEM` or `NONE` | `SYSTEM` |
| `STREAM_TLS_KEY_FILE` | Client TLS key file | — |
| `STREAM_TLS_CERT_FILE` | Client TLS certificate file | — |
| `STREAM_TLS_DISABLE_HOSTNAME_VALIDATION` | `true`/`1` — skip TLS hostname check | `false` |

**Examples:**

```
<action application="set" data="STREAM_BUFFER_SIZE=100"/>
<action application="set" data="STREAM_HEART_BEAT=30"/>
<action application="set" data="STREAM_EXTRA_HEADERS={'Authorization':'Bearer token123'}"/>
<action application="set" data="STREAM_TLS_CA_FILE=/etc/ssl/certs/ca-certificates.crt"/>
```

---

## API

### `uuid_audio_stream <uuid> start <wss-url> <mix-type> <sample-rate> [metadata]`

Attaches a media bug and opens a WebSocket connection to start streaming.

| Parameter | Values | Description |
|---|---|---|
| `uuid` | FreeSWITCH channel UUID | Target channel |
| `wss-url` | `ws://` or `wss://` URL | WebSocket server endpoint |
| `mix-type` | `mono` / `mixed` / `stereo` | Audio capture mode |
| `sample-rate` | `8k`, `16k`, or any multiple of 8000 | Output sample rate (resampled if needed) |
| `metadata` | UTF-8 string (optional) | Sent as text before audio streaming begins |

**Mix types:**
- `mono` — captures inbound audio only (caller → server)
- `mixed` — mixes inbound + outbound into a single channel
- `stereo` — two channels: inbound on left, outbound on right

**Example:**
```
uuid_audio_stream 1234-abcd-... start wss://asr.example.com/stream mono 16k {"language":"en"}
```

---

### `uuid_audio_stream <uuid> stop [metadata]`

Stops streaming and closes the WebSocket. If `metadata` is provided it is sent as a final text message before closing.

```
uuid_audio_stream 1234-abcd-... stop {"reason":"user_hangup"}
```

---

### `uuid_audio_stream <uuid> send_text <text>`

Sends a UTF-8 text message to the WebSocket server on the active connection.

```
uuid_audio_stream 1234-abcd-... send_text {"action":"start_recognition"}
```

---

### `uuid_audio_stream <uuid> pause`

Pauses audio capture. Incoming frames are not forwarded to the server. Playback continues.

---

### `uuid_audio_stream <uuid> resume`

Resumes audio capture after pause.

---

### `uuid_audio_stream <uuid> metrics`

Returns a JSON object with live statistics for the session. Returns `+OK <json>` on success.

```
uuid_audio_stream 1234-abcd-... metrics
```

Response:
```json
{
  "bufferedBytes": 0,
  "playbackQueue": 0,
  "backpressureDrops": 0,
  "playbackEnqueued": 12,
  "playbackDrained": 12,
  "reconnectAttempts": 0,
  "connected": "true"
}
```

| Field | Description |
|---|---|
| `bufferedBytes` | Bytes queued in the WebSocket send buffer |
| `playbackQueue` | Number of audio items waiting to be played |
| `backpressureDrops` | Frames dropped due to WebSocket buffer overflow (>512 KB) |
| `playbackEnqueued` | Total audio chunks enqueued for playback since session start |
| `playbackDrained` | Total audio chunks fully played back |
| `reconnectAttempts` | Number of automatic reconnection attempts |
| `connected` | `"true"` / `"false"` |

---

## Events

The module fires the following FreeSWITCH custom events:

### `mod_audio_stream::connect`

Fired when the WebSocket connection is established. Initial metadata (if set) is sent to the server before this event fires.

```json
{ "status": "connected" }
```

---

### `mod_audio_stream::disconnect`

Fired when the WebSocket connection is closed by the server or network.

```json
{
  "status": "disconnected",
  "message": { "code": 1000, "reason": "Normal closure" }
}
```

---

### `mod_audio_stream::error`

Fired on a connection error. The module will attempt automatic reconnect (unless `STREAM_NO_RECONNECT=true`).

```json
{
  "status": "error",
  "message": { "code": 6, "error": "TCP connection failed" }
}
```

| Code | Name | Meaning |
|:---:|---|---|
| 1 | `IO` | I/O error reading/writing socket |
| 2 | `INVALID_HEADER` | Server sent malformed WebSocket header |
| 3 | `SERVER_MASKED` | Server sent masked frames (spec violation) |
| 4 | `NOT_SUPPORTED` | Requested extension not supported |
| 5 | `PING_TIMEOUT` | No PONG within timeout |
| 6 | `CONNECT_FAILED` | TCP connect or DNS lookup failed |
| 7 | `TLS_INIT_FAILED` | SSL context initialisation failed |
| 8 | `SSL_HANDSHAKE_FAILED` | TLS handshake failed |
| 9 | `SSL_ERROR` | Generic OpenSSL error (cert, cipher, etc.) |

---

### `mod_audio_stream::json`

Fired when the server sends a message that the module did not handle internally (anything that is not `{"type":"streamAudio",...}`). The event body contains the raw server response.

---

### `mod_audio_stream::play`

Fired when the server sends a `streamAudio` message with audio data.

**Server message format:**

```json
{
  "type": "streamAudio",
  "data": {
    "audioDataType": "raw",
    "sampleRate": 16000,
    "channels": 1,
    "audioData": "<base64-encoded PCM s16le>"
  }
}
```

Supported `audioDataType` values:

| Type | Behaviour |
|---|---|
| `raw` | Decoded from base64, resampled to channel rate/channels via SpeexDSP, written directly to the FreeSWITCH channel. Event contains playback status. |
| `wav` | Saved to a temp file. Event contains `file` path. Application must play it (e.g. `playback`). |
| `mp3` | Same as `wav`. |
| `ogg` | Same as `wav`. |

**Event body for `raw`:**

```json
{
  "audioDataType": "raw",
  "sampleRate": 16000,
  "channels": 1,
  "bytes": 6400,
  "playback": "ok"
}
```

**Event body for `wav`/`mp3`/`ogg`:**

```json
{
  "audioDataType": "wav",
  "sampleRate": 8000,
  "file": "/tmp/1234-abcd-0.tmp.wav"
}
```

Temp files are automatically deleted when the session ends (`stop` or hangup).

---

## Audio Flow

### Outbound (channel → server)

```
FreeSWITCH channel (RTP, 20ms frames, PCM s16le)
  │
  ▼ stream_frame()  [media thread, non-blocking trylock]
  │
  ├─ [no resampler] ──► writeBinary() ──► WebSocket frame (binary, raw PCM)
  │
  └─ [resampler]    ──► speex_resampler ──► writeBinary() ──► WebSocket frame
                         (e.g. 8 kHz → 16 kHz)
```

- Compression: per-message deflate (zlib), enabled by default
- Transport: `ws://` (plain) or `wss://` (TLS via OpenSSL)
- Frame size: controlled by `STREAM_BUFFER_SIZE` (default 20 ms)
- Backpressure: frames dropped when WebSocket buffer ≥ 512 KB

### Inbound (server → channel)

```
WebSocket server sends {"type":"streamAudio", "data":{...}}
  │
  ▼ processMessage()  [WebSocket event thread]
  │ base64 decode
  │ speex_resampler: src rate/channels → channel rate/channels
  │ enqueuePlaybackResampled() ──► m_playback_queue (deque, mutex-protected)
  │
  ▼ playbackLoop()  [dedicated playback thread, started lazily]
    mix multiple queued items (int32 accumulator, clamped to int16)
    switch_core_session_write_frame()  [20ms chunks]
```

---

## Thread Model

Each active streaming session uses **up to 3 threads** plus a short-lived reconnect thread:

| Thread | Lifetime | Role |
|---|---|---|
| **FreeSWITCH media thread** | Duration of channel | Captures audio frames, calls `stream_frame()` |
| **WebSocket thread** (libwsc/libevent) | From `start` to `stop` | Drives the WebSocket event loop, fires message/open/close/error callbacks |
| **Playback thread** | Lazy start on first queued audio; exits when queue is empty and shutdown requested | Reads `m_playback_queue`, mixes, writes to channel |
| **Reconnect thread** | Short-lived on drop | Exponential backoff (1 → 2 → 4 → 8 → 16 s), max 5 attempts |

Synchronisation:
- `tech_pvt->mutex` — guards WebSocket writes from the media thread (`trylock`, never blocks)
- `m_playback_mutex` + `m_playback_cv` — guards the playback queue
- Atomics: `m_shutdown`, `m_reconnecting`, `m_playback_thread_started`, all metric counters

---

## Tools

The `tools/` directory contains minimal test servers:

### `tools/ws_test_server.py` (Python)

```bash
pip install websockets
# Listen only, log all traffic
python3 tools/ws_test_server.py --port 8765

# Send a raw PCM file to the caller on connect
python3 tools/ws_test_server.py --port 8765 --send-raw sample.raw --rate 16000
```

### `tools/ws_test_server.js` (Node.js)

```bash
npm install ws
# Listen only
node tools/ws_test_server.js --port 8765

# Echo all binary frames back as streamAudio (loopback test)
node tools/ws_test_server.js --port 8765

# Send a raw PCM file on connect
node tools/ws_test_server.js --port 8765 --send-raw sample.raw --rate 16000 --channels 1
```

The Node.js server automatically echoes received binary audio back as `streamAudio` messages — useful for testing the full duplex playback loop.

---

## Debian Repository

See [README.debian](README.debian) for instructions on using the pre-built Debian package repository.
