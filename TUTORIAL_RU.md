# Полный туториал по mod_audio_stream

## Содержание

1. [Что делает модуль](#1-что-делает-модуль)
2. [Установка](#2-установка)
3. [Подключение модуля в FreeSWITCH](#3-подключение-модуля-в-freeswitch)
4. [Глобальный конфиг](#4-глобальный-конфиг)
5. [Диалплан — базовые примеры](#5-диалплан--базовые-примеры)
6. [Переменные канала](#6-переменные-канала)
7. [API-команды из ESL / fs_cli](#7-api-команды-из-esl--fs_cli)
8. [События FreeSWITCH](#8-события-freeswitch)
9. [Воспроизведение звука от сервера](#9-воспроизведение-звука-от-сервера)
10. [Примеры WebSocket-серверов](#10-примеры-websocket-серверов)
11. [Сценарии использования](#11-сценарии-использования)
12. [Мониторинг и отладка](#12-мониторинг-и-отладка)
13. [Частые ошибки](#13-частые-ошибки)

---

## 1. Что делает модуль

`mod_audio_stream` перехватывает аудиопоток активного звонка и в реальном времени передаёт его на WebSocket-сервер в формате **сырого PCM (L16, little-endian)**. Одновременно модуль принимает аудио от сервера и воспроизводит его абоненту.

**Типичное применение:**
- Подключение ASR (распознавание речи): Google STT, Whisper, Yandex SpeechKit
- Голосовые боты и AI-ассистенты в реальном времени
- Запись и транскрипция звонков
- Двунаправленный аудиомост между телефонией и веб-приложением

**Схема работы:**
```
Абонент (телефон)
      │  RTP (G711/G722/любой кодек)
      ▼
FreeSWITCH
      │  PCM L16 (сырые байты)
      ▼
mod_audio_stream
      │  WebSocket binary frame (PCM) ──► Ваш сервер (ASR/бот)
      │◄─ WebSocket JSON + base64 audio ── Ваш сервер
      ▼
Абонент слышит ответ бота
```

---

## 2. Установка

### 2.1 Зависимости

На сервере с FreeSWITCH должны быть установлены:

```bash
sudo apt-get update
sudo apt-get install -y \
    cmake \
    pkg-config \
    libssl-dev \
    zlib1g-dev \
    libevent-dev \
    libspeexdsp-dev
```

> **FreeSWITCH dev-заголовки** (`libfreeswitch-dev`) устанавливаются автоматически вместе с FreeSWITCH из официального репозитория SignalWire (см. ниже).

### 2.2 Если FreeSWITCH ещё не установлен

Полная установка FreeSWITCH на Debian/Ubuntu (официальный способ):

```bash
# 1. Получить токен на https://id.signalwire.com → Personal Access Tokens
TOKEN=ВАШ_ТОКЕН

# 2. Добавить репозиторий
apt-get install -y gnupg2 wget
wget --http-user=signalwire --http-password=$TOKEN \
     -O /usr/share/keyrings/signalwire-freeswitch-repo.gpg \
     https://freeswitch.signalwire.com/repo/deb/debian-release/pubkey.gpg

echo "machine freeswitch.signalwire.com login signalwire password $TOKEN" \
     > /etc/apt/auth.conf.d/signalwire.conf
chmod 600 /etc/apt/auth.conf.d/signalwire.conf

echo "deb [signed-by=/usr/share/keyrings/signalwire-freeswitch-repo.gpg] \
     https://freeswitch.signalwire.com/repo/deb/debian-release/ bookworm main" \
     > /etc/apt/sources.list.d/freeswitch.list

# 3. Установить
apt-get update
apt-get install -y freeswitch freeswitch-mod-console \
    freeswitch-mod-sofia freeswitch-mod-commands \
    freeswitch-mod-dialplan-xml freeswitch-mod-dptools \
    libfreeswitch-dev
```

### 2.3 Сборка mod_audio_stream

```bash
# Клонируем репозиторий
git clone https://github.com/Tikava/mod_audio_stream.git
cd mod_audio_stream

# Инициализируем сабмодуль libwsc
git submodule init
git submodule update

# Собираем
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)

# Устанавливаем .so в папку модулей FreeSWITCH
sudo make install
```

**Если FreeSWITCH установлен не в стандартный путь** (например, собран из исходников в `/usr/local/freeswitch`):

```bash
export PKG_CONFIG_PATH=/usr/local/freeswitch/lib/pkgconfig
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
sudo make install
```

### 2.4 Установка через DEB-пакет

```bash
cd build
cpack -G DEB
sudo dpkg -i ../_packages/mod-audio-stream_*.deb
```

---

## 3. Подключение модуля в FreeSWITCH

### 3.1 Включить загрузку модуля

Открыть файл `/etc/freeswitch/autoload_configs/modules.conf.xml` и добавить строку:

```xml
<configuration name="modules.conf" description="Modules">
  <modules>
    <!-- ... другие модули ... -->
    <load module="mod_audio_stream"/>
  </modules>
</configuration>
```

### 3.2 Скопировать конфиг (если не скопировался автоматически)

```bash
sudo cp /path/to/mod_audio_stream/conf/audio_stream.conf.xml \
        /etc/freeswitch/autoload_configs/audio_stream.conf.xml
```

### 3.3 Перезагрузить модуль (без перезапуска FreeSWITCH)

Из `fs_cli`:
```
freeswitch> load mod_audio_stream
```

Или перезагрузить конфиг:
```
freeswitch> reload mod_audio_stream
```

### 3.4 Проверить что модуль загружен

```
freeswitch> module_exists mod_audio_stream
true

freeswitch> show api
...
uuid_audio_stream
```

---

## 4. Глобальный конфиг

Файл `/etc/freeswitch/autoload_configs/audio_stream.conf.xml` задаёт **глобальные дефолты** для всех сессий. Переменные канала всегда перекрывают значения из этого файла.

```xml
<configuration name="audio_stream.conf" description="mod_audio_stream config">
  <settings>
    <!-- Отключить сжатие zlib (true = выключить) -->
    <param name="message-deflate" value="false"/>

    <!-- Ping каждые N секунд (0 = выключено) -->
    <param name="heart-beat" value="30"/>

    <!-- Не логировать ответы WebSocket-сервера -->
    <param name="suppress-log" value="false"/>

    <!-- Размер аудио-пакета в мс (кратно 20) -->
    <param name="buffer-size" value="20"/>

    <!-- Отключить авто-переподключение -->
    <param name="no-reconnect" value="false"/>
  </settings>
</configuration>
```

---

## 5. Диалплан — базовые примеры

Все примеры — для `/etc/freeswitch/dialplan/default.xml`.

### 5.1 Минимальный пример — стриминг на ASR

```xml
<extension name="stream_to_asr">
  <condition field="destination_number" expression="^1234$">

    <!-- Ответить на звонок -->
    <action application="answer"/>

    <!-- Запустить стриминг: mono, 16kHz, сразу при ответе -->
    <action application="uuid_audio_stream"
            data="${uuid} start ws://localhost:8765/asr mono 16k"/>

    <!-- Держать канал открытым -->
    <action application="park"/>

  </condition>
</extension>
```

### 5.2 С метаданными (передать info о звонке на сервер)

Первое сообщение после подключения — JSON с данными о звонке:

```xml
<extension name="stream_with_meta">
  <condition field="destination_number" expression="^1235$">
    <action application="answer"/>

    <!-- Формируем JSON с метаданными -->
    <action application="set"
            data="meta={'caller':'${caller_id_number}','called':'${destination_number}','lang':'ru'}"/>

    <action application="uuid_audio_stream"
            data="${uuid} start wss://asr.myserver.com/stream mono 16k ${meta}"/>

    <action application="park"/>
  </condition>
</extension>
```

### 5.3 Стерео-режим (оба участника разговора в разных каналах)

```xml
<extension name="stream_stereo">
  <condition field="destination_number" expression="^1236$">
    <action application="answer"/>

    <!-- stereo: левый канал = входящий (абонент A), правый = исходящий (абонент B) -->
    <action application="uuid_audio_stream"
            data="${uuid} start ws://localhost:8765 stereo 16k"/>

    <action application="park"/>
  </condition>
</extension>
```

### 5.4 Mixed-режим (оба в одном канале, смешанно)

```xml
<extension name="stream_mixed">
  <condition field="destination_number" expression="^1237$">
    <action application="answer"/>

    <!-- mixed: один канал, оба участника смешаны -->
    <action application="uuid_audio_stream"
            data="${uuid} start ws://localhost:8765 mixed 8k"/>

    <action application="park"/>
  </condition>
</extension>
```

### 5.5 Запуск после bridge (подключился второй участник)

Часто нужно начать стриминг только когда разговор установлен:

```xml
<extension name="outbound_stream">
  <condition field="destination_number" expression="^(\d{10})$">
    <action application="bridge" data="sofia/gateway/mygw/$1"/>

    <!-- Запускаем стриминг ПОСЛЕ того как bridge установлен -->
    <action application="uuid_audio_stream"
            data="${uuid} start ws://localhost:8765 mixed 16k"/>

    <action application="park"/>
  </condition>
</extension>
```

### 5.6 Автоматическая остановка при завершении

```xml
<extension name="stream_auto_stop">
  <condition field="destination_number" expression="^1238$">
    <action application="answer"/>
    <action application="uuid_audio_stream"
            data="${uuid} start ws://localhost:8765 mono 16k"/>

    <!-- Воспроизведём приветствие, потом остановим стриминг -->
    <action application="playback" data="ivr/ivr-welcome.wav"/>
    <action application="uuid_audio_stream"
            data="${uuid} stop {'reason':'greeting_done'}"/>

    <action application="hangup"/>
  </condition>
</extension>
```

### 5.7 WSS (защищённое соединение)

```xml
<extension name="stream_secure">
  <condition field="destination_number" expression="^1239$">

    <!-- TLS настройки -->
    <action application="set" data="STREAM_TLS_CA_FILE=/etc/ssl/certs/ca-certificates.crt"/>

    <action application="answer"/>
    <action application="uuid_audio_stream"
            data="${uuid} start wss://secure.myserver.com:8443/stream mono 16k"/>
    <action application="park"/>

  </condition>
</extension>
```

---

## 6. Переменные канала

Устанавливаются через `set` **до** команды `start`. Переопределяют глобальный конфиг.

| Переменная | Значение | Описание |
|---|---|---|
| `STREAM_BUFFER_SIZE` | `20`, `40`, `100`, ... | Размер аудио-пакета в мс. `20` = один фрейм 20мс. `100` = пакеты по 100мс. Кратно 20. |
| `STREAM_HEART_BEAT` | `30` | Ping WebSocket-сервера каждые 30 секунд |
| `STREAM_MESSAGE_DEFLATE` | `true` | Отключить zlib-сжатие |
| `STREAM_SUPPRESS_LOG` | `true` | Не писать ответы сервера в лог FS |
| `STREAM_NO_RECONNECT` | `true` | Не переподключаться при обрыве |
| `STREAM_EXTRA_HEADERS` | JSON-объект | Доп. HTTP-заголовки при WS handshake |
| `STREAM_TLS_CA_FILE` | путь к файлу | CA-сертификат для WSS |
| `STREAM_TLS_CERT_FILE` | путь к файлу | Клиентский сертификат (mTLS) |
| `STREAM_TLS_KEY_FILE` | путь к файлу | Ключ клиентского сертификата (mTLS) |
| `STREAM_TLS_DISABLE_HOSTNAME_VALIDATION` | `true` | Не проверять hostname в сертификате |

**Примеры:**

```xml
<!-- Отправлять пакеты по 100мс вместо 20мс -->
<action application="set" data="STREAM_BUFFER_SIZE=100"/>

<!-- Авторизация через Bearer токен -->
<action application="set" data="STREAM_EXTRA_HEADERS={'Authorization':'Bearer eyJhbGc...'}"/>

<!-- Подключиться к серверу с самоподписанным сертификатом -->
<action application="set" data="STREAM_TLS_DISABLE_HOSTNAME_VALIDATION=true"/>

<!-- Пинговать сервер каждые 30 секунд (держать соединение живым) -->
<action application="set" data="STREAM_HEART_BEAT=30"/>
```

---

## 7. API-команды из ESL / fs_cli

Все команды вызываются через `uuid_audio_stream`.

### Запустить стриминг

```
uuid_audio_stream <uuid> start <url> <mix-type> <sample-rate> [metadata]
```

```bash
# Из fs_cli:
uuid_audio_stream 1234-5678-abcd-ef00 start ws://localhost:8765 mono 16k

# С метаданными:
uuid_audio_stream 1234-5678-abcd-ef00 start ws://localhost:8765 mono 16k {"session_id":"xyz"}
```

### Остановить стриминг

```bash
uuid_audio_stream 1234-5678-abcd-ef00 stop

# Отправить финальный JSON перед закрытием:
uuid_audio_stream 1234-5678-abcd-ef00 stop {"reason":"user_hangup"}
```

### Отправить текст на сервер

```bash
uuid_audio_stream 1234-5678-abcd-ef00 send_text {"action":"start","lang":"ru-RU"}
uuid_audio_stream 1234-5678-abcd-ef00 send_text {"action":"stop"}
```

### Пауза / возобновление

```bash
# Приостановить отправку аудио (звонок продолжается, бот не слышит)
uuid_audio_stream 1234-5678-abcd-ef00 pause

# Возобновить
uuid_audio_stream 1234-5678-abcd-ef00 resume
```

### Метрики текущего соединения

```bash
uuid_audio_stream 1234-5678-abcd-ef00 metrics
```

Ответ:
```json
{
  "bufferedBytes": 320,
  "playbackQueue": 0,
  "backpressureDrops": 0,
  "playbackEnqueued": 5,
  "playbackDrained": 5,
  "reconnectAttempts": 0,
  "connected": "true"
}
```

| Поле | Что значит |
|---|---|
| `bufferedBytes` | Сколько байт ещё не отправлено в сокет |
| `playbackQueue` | Сколько аудио-блоков ждёт воспроизведения |
| `backpressureDrops` | Сколько фреймов было сброшено (сервер не успевал) |
| `playbackEnqueued` | Всего поставлено в очередь на воспроизведение |
| `playbackDrained` | Всего успешно воспроизведено |
| `reconnectAttempts` | Сколько раз переподключались |
| `connected` | `"true"` / `"false"` |

---

## 8. События FreeSWITCH

Модуль генерирует кастомные события. Подписаться на них можно через ESL.

### Список событий

| Событие | Когда | Тело |
|---|---|---|
| `mod_audio_stream::connect` | WS соединение установлено | `{"status":"connected"}` |
| `mod_audio_stream::disconnect` | WS соединение закрыто | `{"status":"disconnected","message":{"code":1000,"reason":"..."}}` |
| `mod_audio_stream::error` | Ошибка соединения | `{"status":"error","message":{"code":6,"error":"TCP connect failed"}}` |
| `mod_audio_stream::json` | Ответ от сервера (не audio) | тело ответа сервера |
| `mod_audio_stream::play` | Получен аудио-блок для воспроизведения | метаданные воспроизведения |

### Коды ошибок

| Код | Значение |
|---|---|
| 1 | I/O ошибка при чтении/записи сокета |
| 2 | Сервер прислал некорректный WS-заголовок |
| 3 | Сервер использует маскировку (нарушение RFC) |
| 4 | Запрошенная функция не поддерживается |
| 5 | Нет ответа на PING (таймаут) |
| 6 | Не удалось установить TCP-соединение |
| 7 | Ошибка инициализации TLS |
| 8 | Ошибка TLS handshake |
| 9 | Ошибка OpenSSL (сертификат, шифр) |

### Подписка через ESL (Python-пример)

```python
import ESL

con = ESL.ESLconnection("127.0.0.1", "8021", "ClueCon")
con.events("plain", "CUSTOM mod_audio_stream::json mod_audio_stream::connect mod_audio_stream::disconnect mod_audio_stream::error mod_audio_stream::play")

while True:
    e = con.recvEvent()
    if e:
        event_name = e.getHeader("Event-Subclass")
        body = e.getBody()
        uuid = e.getHeader("Unique-ID")
        print(f"[{uuid}] {event_name}: {body}")
```

### Подписка через fs_cli

```
freeswitch> event plain CUSTOM mod_audio_stream::json
freeswitch> event plain CUSTOM mod_audio_stream::connect
freeswitch> event plain CUSTOM mod_audio_stream::error
```

---

## 9. Воспроизведение звука от сервера

Сервер может отправить аудио обратно абоненту через WebSocket.

### Формат сообщения от сервера

```json
{
  "type": "streamAudio",
  "data": {
    "audioDataType": "raw",
    "sampleRate": 16000,
    "channels": 1,
    "audioData": "BASE64_ENCODED_PCM_S16LE"
  }
}
```

### Типы аудио

| `audioDataType` | Что происходит |
|---|---|
| `raw` | Декодируется из base64, ресамплируется под частоту канала, воспроизводится сразу. Абонент слышит звук немедленно. |
| `wav` | Сохраняется во временный файл. В событии `play` приходит путь к файлу — его нужно воспроизвести через `playback`. |
| `mp3` | То же самое |
| `ogg` | То же самое |

### Пример: сервер отвечает raw-аудио (Python)

```python
import asyncio, websockets, base64, json

# Сгенерировать 1 секунду тишины как PCM 16bit 8kHz
silence = bytes(8000 * 2)  # 8000 сэмплов × 2 байта = 16000 байт

async def handler(websocket):
    async for message in websocket:
        if isinstance(message, bytes):
            # Получили аудио от FS, отправляем ответ
            response = {
                "type": "streamAudio",
                "data": {
                    "audioDataType": "raw",
                    "sampleRate": 8000,
                    "channels": 1,
                    "audioData": base64.b64encode(silence).decode()
                }
            }
            await websocket.send(json.dumps(response))

asyncio.run(websockets.serve(handler, "0.0.0.0", 8765))
```

### Обработка wav-файла через ESL (после события play)

Если сервер отправил `audioDataType: wav`, в событии `mod_audio_stream::play` придёт путь к файлу:

```json
{"audioDataType":"wav","file":"/tmp/1234-abcd-0.tmp.wav"}
```

Нужно воспроизвести его через ESL:

```python
def on_play_event(event, uuid):
    body = json.loads(event.getBody())
    if body.get("audioDataType") in ("wav", "mp3", "ogg"):
        filepath = body.get("file")
        if filepath:
            con.api(f"uuid_broadcast {uuid} {filepath} aleg")
```

---

## 10. Примеры WebSocket-серверов

В репозитории в папке `tools/` есть готовые тестовые серверы.

### Python-сервер (просто логирует + эхо)

```bash
pip install websockets
python3 tools/ws_test_server.py --port 8765
```

### Python-сервер с отправкой аудио при подключении

```bash
# Подготовить тестовый файл (1 сек тишины, 8kHz mono):
dd if=/dev/zero bs=16000 count=1 of=test.raw

python3 tools/ws_test_server.py --port 8765 --send-raw test.raw --rate 8000
```

### Node.js-сервер (эхо — отправляет всё аудио обратно)

```bash
npm install ws
node tools/ws_test_server.js --port 8765
```

Node.js сервер автоматически эхоирует все бинарные фреймы обратно как `streamAudio` — удобно для теста полного дуплекса: абонент слышит сам себя с небольшой задержкой.

---

## 11. Сценарии использования

### Сценарий 1: ASR (распознавание речи)

**Задача:** Стримить аудио на Whisper/Google STT и получать транскрипцию.

```xml
<!-- dialplan/default.xml -->
<extension name="asr_call">
  <condition field="destination_number" expression="^8001$">
    <action application="answer"/>
    <action application="set" data="STREAM_BUFFER_SIZE=100"/>
    <action application="set" data="STREAM_SUPPRESS_LOG=true"/>
    <action application="uuid_audio_stream"
            data="${uuid} start ws://asr-server:8765/transcribe mono 16k
                  {'caller_id':'${caller_id_number}','language':'ru-RU'}"/>
    <action application="playback" data="ivr/ivr-please_hold_while_we_connect_your_call.wav"/>
    <action application="park"/>
  </condition>
</extension>
```

Ваш ASR-сервер получает бинарные WebSocket фреймы (PCM 16kHz, mono) и должен:
1. Распознавать речь
2. Отправлять результат в JSON: `{"type":"transcription","text":"Привет"}`
3. FreeSWITCH генерирует событие `mod_audio_stream::json` с этим текстом
4. Ваше приложение через ESL слушает события и реагирует

### Сценарий 2: Голосовой бот (TTS ответы)

**Задача:** Бот слушает абонента и отвечает синтезированной речью.

```xml
<extension name="voicebot">
  <condition field="destination_number" expression="^8002$">
    <action application="answer"/>
    <action application="set" data="STREAM_HEART_BEAT=30"/>
    <action application="uuid_audio_stream"
            data="${uuid} start wss://bot.myserver.com/voice mono 16k
                  {'session_id':'${uuid}','scenario':'support'}"/>
    <action application="park"/>
  </condition>
</extension>
```

Сервер бота:
1. Получает аудио → прогоняет через ASR
2. Отвечает текстом через NLU
3. Синтезирует речь (TTS) → отправляет как `streamAudio raw`
4. FreeSWITCH напрямую воспроизводит ответ абоненту

### Сценарий 3: Запись + транскрипция входящих

```xml
<extension name="record_inbound">
  <condition field="destination_number" expression="^\d{4}$">

    <!-- Запускаем стриминг MIXED (оба участника) -->
    <action application="set" data="STREAM_BUFFER_SIZE=60"/>
    <action application="uuid_audio_stream"
            data="${uuid} start ws://recorder:8765/record mixed 16k
                  {'direction':'inbound','number':'${destination_number}'}"/>

    <!-- Обычный диалплан продолжается -->
    <action application="bridge" data="user/${destination_number}"/>

    <!-- После hangup останавливаем с маркером -->
    <action application="uuid_audio_stream"
            data="${uuid} stop {'end_reason':'call_completed'}"/>

  </condition>
</extension>
```

### Сценарий 4: Управление из ESL (BGApi)

Запуск стриминга динамически из вашего приложения:

```python
import ESL, json

con = ESL.ESLconnection("127.0.0.1", "8021", "ClueCon")

def start_streaming(uuid, ws_url, metadata=None):
    meta_str = json.dumps(metadata) if metadata else ""
    cmd = f"uuid_audio_stream {uuid} start {ws_url} mono 16k {meta_str}"
    result = con.api(cmd)
    return result.getBody()

def stop_streaming(uuid, final_data=None):
    meta_str = json.dumps(final_data) if final_data else ""
    cmd = f"uuid_audio_stream {uuid} stop {meta_str}"
    return con.api(cmd).getBody()

def get_metrics(uuid):
    result = con.api(f"uuid_audio_stream {uuid} metrics")
    body = result.getBody()
    if body.startswith("+OK"):
        return json.loads(body[4:])
    return None

# Использование:
uuid = "ваш-uuid-звонка"
start_streaming(uuid, "ws://localhost:8765", {"caller": "+79001234567"})

# ... звонок идёт ...

metrics = get_metrics(uuid)
print(f"Drops: {metrics['backpressureDrops']}, Reconnects: {metrics['reconnectAttempts']}")

stop_streaming(uuid, {"call_duration": 120})
```

---

## 12. Мониторинг и отладка

### Логи FreeSWITCH

Модуль пишет в стандартный лог FreeSWITCH. Смотреть:

```bash
# Реальное время:
fs_cli -x "console loglevel debug"
tail -f /var/log/freeswitch/freeswitch.log

# Только строки mod_audio_stream:
tail -f /var/log/freeswitch/freeswitch.log | grep -i "audio_stream"
```

### Включить / выключить debug-логи

```bash
# Максимальный уровень:
fs_cli -x "fsctl loglevel 7"

# Только ошибки:
fs_cli -x "fsctl loglevel 3"
```

### Проверка активных стримов

```bash
# Список активных каналов:
fs_cli -x "show channels"

# Метрики конкретного канала:
fs_cli -x "uuid_audio_stream ВАШ-UUID metrics"
```

### Тестовый WebSocket-сервер для отладки

Запустите локальный сервер и смотрите что приходит от FreeSWITCH:

```bash
python3 tools/ws_test_server.py --port 8765
```

Вывод сервера покажет:
- `[+] connection from ...` — соединение установлено
- `[<] binary: 320 bytes` — аудио-фрейм (20мс при 8kHz mono)
- `[<] text: {...}` — текстовые сообщения
- `[-] closed code=1000` — соединение закрыто нормально

### Диагностика частых проблем через метрики

```python
metrics = get_metrics(uuid)

if metrics["backpressureDrops"] > 0:
    print("ПРОБЛЕМА: сервер не успевает обрабатывать аудио")
    print("Решение: увеличить STREAM_BUFFER_SIZE или оптимизировать сервер")

if metrics["reconnectAttempts"] > 0:
    print("ПРОБЛЕМА: соединение рвалось и переподключалось")
    print("Решение: проверить стабильность сети, добавить STREAM_HEART_BEAT=30")

if metrics["connected"] == "false":
    print("ПРОБЛЕМА: нет активного соединения")
```

---

## 13. Частые ошибки

### ❌ `mod_audio_stream: bug already attached!`

**Причина:** Попытка запустить стриминг дважды на одном канале.

**Решение:** Сначала остановить: `uuid_audio_stream UUID stop`, потом запустить снова.

---

### ❌ `channel must have reached pre-answer status`

**Причина:** Команда `start` вызвана до того как канал ответил.

**Решение:** Добавить `answer` перед `uuid_audio_stream start`:

```xml
<action application="answer"/>
<action application="uuid_audio_stream" data="${uuid} start ..."/>
```

---

### ❌ `Error initializing mod_audio_stream session`

**Причина:** Обычно — нет подключения к WebSocket-серверу или неверный URL.

**Решение:**
1. Проверить что сервер запущен: `curl -v ws://localhost:8765` или `nc -zv localhost 8765`
2. Проверить URL — должен начинаться с `ws://` или `wss://`
3. Проверить firewall

---

### ❌ Нет звука при `audioDataType: raw`

**Причина 1:** Неправильный `sampleRate` в ответе сервера.

**Решение:** Убедиться что `sampleRate` соответствует тому, что указан при `start`. Модуль ресамплирует, но нужно указать правильное значение:

```json
{
  "type": "streamAudio",
  "data": {
    "audioDataType": "raw",
    "sampleRate": 16000,   ← должно совпадать с частотой вашего TTS
    "channels": 1,
    "audioData": "..."
  }
}
```

**Причина 2:** PCM-данные не в формате s16le (signed 16-bit little-endian).

**Решение:** Конвертировать в s16le перед base64-кодированием.

---

### ❌ `invalid websocket uri`

**Причина:** URL содержит недопустимые символы или неверную схему.

**Решение:**
- URL должен начинаться с `ws://` или `wss://`
- Никаких пробелов в URL
- Параметры запроса (`?key=val`) поддерживаются

---

### ❌ `invalid sample rate`

**Причина:** Частота дискретизации не кратна 8000.

**Допустимые значения:** `8000`, `16000`, `24000`, `32000`, `48000` (или `8k`, `16k`)

---

### ❌ Обрыв соединения через 60 секунд

**Причина:** Балансировщик нагрузки / прокси закрывает idle-соединение.

**Решение:** Включить WebSocket ping:

```xml
<action application="set" data="STREAM_HEART_BEAT=30"/>
```

---

### ❌ Большие задержки при воспроизведении

**Причина:** Большой `STREAM_BUFFER_SIZE` или накопившаяся очередь воспроизведения.

**Решение:** Проверить `playbackQueue` через `metrics`. Если растёт — сервер отправляет аудио быстрее чем оно воспроизводится. Уменьшить объём аудио в одном `streamAudio` сообщении (отправлять по 20-100мс).

---

### ❌ TLS: `SSL_HANDSHAKE_FAILED`

**Причина:** Сертификат сервера не верифицируется.

**Решение варианты:**
```xml
<!-- Указать CA-сертификат -->
<action application="set" data="STREAM_TLS_CA_FILE=/etc/ssl/certs/ca-certificates.crt"/>

<!-- Или отключить проверку (только для разработки!) -->
<action application="set" data="STREAM_TLS_DISABLE_HOSTNAME_VALIDATION=true"/>
```
