#include <string>
#include <cstring>
#include <vector>
#include <cctype>

#include "mod_audio_stream.h"
#include "WebSocketClient.h"
#include <switch_json.h>
#include <fstream>
#include <switch_buffer.h>
#include <unordered_map>
#include <unordered_set>
#include "base64.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <condition_variable>
#include <deque>
#include <inttypes.h>
#include <speex/speex_resampler.h>

#define FRAME_SIZE_8000  320 /* 1000x0.02 (20ms)= 160 x(16bit= 2 bytes) 320 frame size*/
static constexpr size_t WS_MAX_BUFFERED_BYTES = 512 * 1024; // 512 KB safety cap

class AudioStreamer {
public:

    AudioStreamer(const char* uuid, const char* wsUri, responseHandler_t callback, int deflate, int heart_beat,
                  bool suppressLog, const char* extra_headers, bool no_reconnect,
                  const char* tls_cafile, const char* tls_keyfile, const char* tls_certfile,
                  bool tls_disable_hostname_validation)
        : m_sessionId(uuid),
          m_notify(callback),
          m_suppress_log(suppressLog),
          m_extra_headers(extra_headers),
          m_playFile(0),
          m_no_reconnect(no_reconnect),
          m_shutdown(false),
          m_reconnecting(false),
          m_backpressure_drops(0),
          m_playback_enqueued(0),
          m_playback_drained(0),
          m_reconnect_attempts(0)
    {
        WebSocketHeaders hdrs;
        WebSocketTLSOptions tls;

        if (m_extra_headers) {
            cJSON *headers_json = cJSON_Parse(m_extra_headers);
            if (headers_json) {
                cJSON *iterator = headers_json->child;
                while (iterator) {
                    if (iterator->type == cJSON_String && iterator->valuestring != nullptr) {
                        hdrs.set(iterator->string, iterator->valuestring);
                    }
                    iterator = iterator->next;
                }
                cJSON_Delete(headers_json);
            }
        }

        client.setUrl(wsUri);

        // TLS options (optional)
        if (tls_cafile) {
            tls.caFile = tls_cafile;
        }

        if (tls_keyfile) {
            tls.keyFile = tls_keyfile;
        }

        if (tls_certfile) {
            tls.certFile = tls_certfile;
        }

        tls.disableHostnameValidation = tls_disable_hostname_validation;
        client.setTLSOptions(tls);

        // Optional heart beat
        if (heart_beat > 0) {
            client.setPingInterval(heart_beat);
        }

        // By default compression is enabled; "deflate" flag here is treated as
        // "disable compression" to stay consistent with исходной логикой.
        if (deflate) {
            client.enableCompression(false);
        }

        // Set extra headers if any
        if (!hdrs.empty()) {
            client.setHeaders(hdrs);
        }

        // Message callback
        client.setMessageCallback([this](const std::string& message) {
            eventCallback(MESSAGE, message.c_str());
        });

        client.setOpenCallback([this]() {
            m_reconnecting = false;
            cJSON *root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "status", "connected");
            char *json_str = cJSON_PrintUnformatted(root);
            eventCallback(CONNECT_SUCCESS, json_str);
            cJSON_Delete(root);
            switch_safe_free(json_str);
        });

        client.setErrorCallback([this](int code, const std::string &msg) {
            cJSON *root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "status", "error");

            cJSON *message = cJSON_CreateObject();
            cJSON_AddNumberToObject(message, "code", code);
            cJSON_AddStringToObject(message, "error", msg.c_str());
            cJSON_AddItemToObject(root, "message", message);

            char *json_str = cJSON_PrintUnformatted(root);

            eventCallback(CONNECT_ERROR, json_str);

            cJSON_Delete(root);
            switch_safe_free(json_str);

            scheduleReconnect();
        });

        client.setCloseCallback([this](int code, const std::string &reason) {
            cJSON *root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "status", "disconnected");

            cJSON *message = cJSON_CreateObject();
            cJSON_AddNumberToObject(message, "code", code);
            cJSON_AddStringToObject(message, "reason", reason.c_str());
            cJSON_AddItemToObject(root, "message", message);

            char *json_str = cJSON_PrintUnformatted(root);

            eventCallback(CONNECTION_DROPPED, json_str);

            cJSON_Delete(root);
            switch_safe_free(json_str);

            scheduleReconnect();
        });

        // Start background thread and connect
        client.connect();
    }

    switch_media_bug_t *get_media_bug(switch_core_session_t *session) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        if (!channel) {
            return nullptr;
        }
        auto *bug = (switch_media_bug_t *) switch_channel_get_private(channel, MY_BUG_NAME);
        return bug;
    }

    inline void media_bug_close(switch_core_session_t *session) {
        auto *bug = get_media_bug(session);
        if (bug) {
            auto* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
            tech_pvt->close_requested = 1;
            switch_core_media_bug_close(&bug, SWITCH_FALSE);
        }
    }

    inline void send_initial_metadata(switch_core_session_t *session) {
        auto *bug = get_media_bug(session);
        if (bug) {
            auto* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
            if (tech_pvt && strlen(tech_pvt->initialMetadata) > 0) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                                  "sending initial metadata %s\n", tech_pvt->initialMetadata);
                writeText(tech_pvt->initialMetadata);
            }
        }
    }

    void eventCallback(notifyEvent_t event, const char* message) {
        switch_core_session_t* psession = switch_core_session_locate(m_sessionId.c_str());
        if (psession) {
            switch (event) {
                case CONNECT_SUCCESS:
                    send_initial_metadata(psession);
                    m_notify(psession, EVENT_CONNECT, message);
                    break;
                case CONNECTION_DROPPED:
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_INFO, "connection closed\n");
                    m_notify(psession, EVENT_DISCONNECT, message);
                    break;
                case CONNECT_ERROR:
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_INFO, "connection error\n");
                    m_notify(psession, EVENT_ERROR, message);
                    media_bug_close(psession);
                    break;
                case MESSAGE: {
                    std::string msg(message);
                    if (processMessage(psession, msg) != SWITCH_TRUE) {
                        m_notify(psession, EVENT_JSON, msg.c_str());
                    }
                    if (!m_suppress_log) {
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_DEBUG,
                                          "response: %s\n", msg.c_str());
                    }
                    break;
                }
            }
            switch_core_session_rwunlock(psession);
        }
    }

    switch_status_t streamRawToFreeswitch(switch_core_session_t *session, const std::string &rawAudio, int sampleRate) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        auto *bug = (switch_media_bug_t *) switch_channel_get_private(channel, MY_BUG_NAME);
        if (!bug) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                              "(%s) playback failed - no media bug\n", m_sessionId.c_str());
            return SWITCH_STATUS_FALSE;
        }
        auto *tech_pvt = (private_t *) switch_core_media_bug_get_user_data(bug);
        if (!tech_pvt) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                              "(%s) playback failed - no tech_pvt\n", m_sessionId.c_str());
            return SWITCH_STATUS_FALSE;
        }

        switch_codec_t *codec = switch_core_session_get_read_codec(session);
        if (!codec) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                              "(%s) playback failed - no codec\n", m_sessionId.c_str());
            return SWITCH_STATUS_FALSE;
        }

        const int channels = tech_pvt->channels > 0 ? tech_pvt->channels : 1;
        const size_t bytes_per_sample = sizeof(int16_t) * channels;
        const int target_rate = sampleRate > 0 ? sampleRate : codec->implementation->actual_samples_per_second;
        const size_t samples_per_frame = target_rate / 50; // 20ms chunk
        if (!samples_per_frame || !bytes_per_sample) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                              "(%s) playback failed - invalid frame sizing (rate=%d, channels=%d)\n",
                              m_sessionId.c_str(), target_rate, channels);
            return SWITCH_STATUS_FALSE;
        }

        const size_t frame_bytes = samples_per_frame * bytes_per_sample;
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                          "(%s) playback start len=%zu rate=%d channels=%d frame_bytes=%zu\n",
                          m_sessionId.c_str(), rawAudio.size(), target_rate, channels, frame_bytes);

        switch_status_t status = SWITCH_STATUS_SUCCESS;
        size_t offset = 0;

        while (offset < rawAudio.size()) {
            const size_t chunk = std::min(frame_bytes, rawAudio.size() - offset);
            switch_frame_t write_frame = {0};
            write_frame.codec = codec;
            write_frame.data = (void *)(rawAudio.data() + offset);
            write_frame.datalen = chunk;
            write_frame.buflen = chunk;
            write_frame.samples = chunk / bytes_per_sample;
            write_frame.rate = target_rate;
            write_frame.channels = channels;

            status = switch_core_session_write_frame(session, &write_frame, SWITCH_IO_FLAG_NONE, 0);
            if (status != SWITCH_STATUS_SUCCESS) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                                  "(%s) playback write failed at offset=%zu chunk=%zu\n",
                                  m_sessionId.c_str(), offset, chunk);
                break;
            }
            offset += chunk;
        }

        return status;
    }

    switch_bool_t processMessage(switch_core_session_t* session, std::string& message) {
        cJSON* json = cJSON_Parse(message.c_str());
        switch_bool_t status = SWITCH_FALSE;
        if (!json) {
            return status;
        }

        const char* jsType = cJSON_GetObjectCstr(json, "type");
        if (jsType && strcmp(jsType, "streamAudio") == 0) {
            cJSON* jsonData = cJSON_GetObjectItem(json, "data");
            if (jsonData) {
                cJSON* jsonFile = nullptr;
                cJSON* jsonAudio = cJSON_DetachItemFromObject(jsonData, "audioData");
                const char* jsAudioDataType = cJSON_GetObjectCstr(jsonData, "audioDataType");
                std::string fileType;
                int audioChannels = 1;
                int sampleRate = 0;
                bool handledPlayback = false;

                if (jsAudioDataType && 0 == strcmp(jsAudioDataType, "raw")) {
                    cJSON* jsonSampleRate = cJSON_GetObjectItem(jsonData, "sampleRate");
                    if (jsonSampleRate && cJSON_IsNumber(jsonSampleRate)) {
                        sampleRate = jsonSampleRate->valueint;
                    }

                    std::unordered_map<int, const char*> sampleRateMap = {
                        {8000,  ".r8"},
                        {16000, ".r16"},
                        {24000, ".r24"},
                        {32000, ".r32"},
                        {48000, ".r48"},
                        {64000, ".r64"}
                    };

                    auto it = sampleRateMap.find(sampleRate);
                    if (it == sampleRateMap.end()) {
                        fileType.clear();
                    } else {
                        fileType = it->second;
                    }

                    cJSON* jsonChannels = cJSON_GetObjectItem(jsonData, "channels");
                    if (jsonChannels && cJSON_IsNumber(jsonChannels)) {
                        audioChannels = jsonChannels->valueint;
                        if (audioChannels <= 0 || audioChannels > 8) audioChannels = 1;
                    }
                } else if (jsAudioDataType && 0 == strcmp(jsAudioDataType, "wav")) {
                    fileType = ".wav";
                } else if (jsAudioDataType && 0 == strcmp(jsAudioDataType, "mp3")) {
                    fileType = ".mp3";
                } else if (jsAudioDataType && 0 == strcmp(jsAudioDataType, "ogg")) {
                    fileType = ".ogg";
                } else if (jsAudioDataType) {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                      "(%s) processMessage - unsupported audio type: %s\n",
                                      m_sessionId.c_str(), jsAudioDataType);
                }

                if (jsonAudio && jsonAudio->valuestring != nullptr && jsAudioDataType) {
                    std::string rawAudio;
                    try {
                        rawAudio = base64_decode(jsonAudio->valuestring);
                    } catch (const std::exception& e) {
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                          "(%s) processMessage - base64 decode error: %s\n",
                                          m_sessionId.c_str(), e.what());
                        cJSON_Delete(jsonAudio);
                        cJSON_Delete(json);
                        return status;
                    }

                    if (jsAudioDataType && 0 == strcmp(jsAudioDataType, "raw")) {
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                                          "(%s) processMessage raw audio len=%zu samplerate=%d\n",
                                          m_sessionId.c_str(), rawAudio.size(), sampleRate);

                        auto *bug = get_media_bug(session);
                        auto *tech_pvt = bug ? (private_t *) switch_core_media_bug_get_user_data(bug) : nullptr;
                        switch_codec_t *codec = switch_core_session_get_read_codec(session);
                        const int target_channels = tech_pvt && tech_pvt->channels > 0 ? tech_pvt->channels : 1;
                        const int target_rate = codec ? codec->implementation->actual_samples_per_second : sampleRate;

                        switch_status_t playbackStatus = enqueuePlaybackResampled(rawAudio,
                                                                                  sampleRate,
                                                                                  audioChannels,
                                                                                  target_rate,
                                                                                  target_channels);
                        cJSON_AddNumberToObject(jsonData, "bytes", (double)rawAudio.size());
                        if (!cJSON_GetObjectItem(jsonData, "sampleRate")) {
                            cJSON_AddNumberToObject(jsonData, "sampleRate", sampleRate);
                        }
                        cJSON_AddStringToObject(jsonData, "playback",
                                                playbackStatus == SWITCH_STATUS_SUCCESS ? "ok" : "failed");
                        if (playbackStatus != SWITCH_STATUS_SUCCESS) {
                            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                                              "(%s) playback failed for raw audio\n", m_sessionId.c_str());
                        }
                        handledPlayback = true;
                        status = SWITCH_TRUE;
                    } else if (!fileType.empty()) {
                        char filePath[256];
                        switch_snprintf(filePath, sizeof(filePath), "%s%s%s_%d.tmp%s",
                                        SWITCH_GLOBAL_dirs.temp_dir,
                                        SWITCH_PATH_SEPARATOR,
                                        m_sessionId.c_str(),
                                        m_playFile++,
                                        fileType.c_str());

                        std::ofstream fstream(filePath, std::ofstream::binary);
                        fstream.write(rawAudio.data(), (std::streamsize)rawAudio.size());
                        fstream.close();

                        m_Files.insert(filePath);
                        jsonFile = cJSON_CreateString(filePath);
                        cJSON_AddItemToObject(jsonData, "file", jsonFile);
                    }
                }

                if (handledPlayback || jsonFile) {
                    char *jsonString = cJSON_PrintUnformatted(jsonData);
                    m_notify(session, EVENT_PLAY, jsonString);
                    message.assign(jsonString);
                    free(jsonString);
                    status = SWITCH_TRUE;
                }

                if (jsonAudio) {
                    cJSON_Delete(jsonAudio);
                }

            } else {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                  "(%s) processMessage - no data in streamAudio\n", m_sessionId.c_str());
            }
        }
        cJSON_Delete(json);
        return status;
    }

    ~AudioStreamer() {
        joinShutdownThread();
    }

    void disconnect() {
        m_shutdown = true;
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "disconnecting...\n");
        client.disconnect();
    }

    // Starts a background thread that calls requestShutdown()+disconnect() and
    // can be joined later. Replaces the old detached-thread pattern in finish().
    void startShutdownThread() {
        std::lock_guard<std::mutex> lk(m_shutdown_thread_mutex);
        if (m_shutdown_thread.joinable()) return;
        m_shutdown_thread = std::thread([this]() {
            requestShutdown();
            disconnect();
        });
    }

    void joinShutdownThread() {
        std::lock_guard<std::mutex> lk(m_shutdown_thread_mutex);
        if (m_shutdown_thread.joinable()) {
            m_shutdown_thread.join();
        }
    }

    bool isConnected() {
        return client.isConnected();
    }

    size_t bufferedAmount() {
        return client.bufferedAmount();
    }

    void writeBinary(uint8_t* buffer, size_t len) {
        if (!this->isConnected()) return;
        client.sendBinary(buffer, len);
    }

    void writeText(const char* text) {
        if (!this->isConnected()) return;
        client.sendMessage(text, strlen(text));
    }

    void deleteFiles() {
        if (m_playFile > 0) {
            for (const auto &fileName: m_Files) {
                remove(fileName.c_str());
            }
        }
    }

    void requestShutdown() {
        m_shutdown = true;
        m_no_reconnect = true;
        {
            std::lock_guard<std::mutex> lk(m_playback_mutex);
        }
        m_playback_cv.notify_all();
    }

    void noteBackpressureDrop() {
        m_backpressure_drops.fetch_add(1);
    }

    char* buildMetricsJson() {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "bufferedBytes", (double)bufferedAmount());
        cJSON_AddNumberToObject(root, "playbackQueue", (double)playbackQueueSize());
        cJSON_AddNumberToObject(root, "backpressureDrops", (double)m_backpressure_drops.load());
        cJSON_AddNumberToObject(root, "playbackEnqueued", (double)m_playback_enqueued.load());
        cJSON_AddNumberToObject(root, "playbackDrained", (double)m_playback_drained.load());
        cJSON_AddNumberToObject(root, "reconnectAttempts", (double)m_reconnect_attempts.load());
        cJSON_AddStringToObject(root, "connected", isConnected() ? "true" : "false");

        char *json_str = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        return json_str;
    }

private:
    struct PlaybackItem {
        std::vector<int16_t> samples;
        size_t pos = 0; // position in samples (not bytes)
        int sample_rate = 0;
        int channels = 1;
    };

    static inline int16_t clamp16(int32_t v) {
        if (v > 32767) return 32767;
        if (v < -32768) return -32768;
        return (int16_t)v;
    }

    switch_status_t enqueuePlaybackRaw(const std::string& rawAudio, int sampleRate, int channels) {
        if (rawAudio.size() < sizeof(int16_t) || sampleRate <= 0 || channels <= 0) {
            return SWITCH_STATUS_FALSE;
        }

        if (rawAudio.size() % (sizeof(int16_t) * channels) != 0) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                              "(%s) enqueuePlayback: raw audio size not aligned to sample size\n",
                              m_sessionId.c_str());
            return SWITCH_STATUS_FALSE;
        }

        PlaybackItem item;
        item.sample_rate = sampleRate;
        item.channels = channels > 0 ? channels : 1;
        const size_t sample_count = rawAudio.size() / sizeof(int16_t);
        item.samples.resize(sample_count);
        memcpy(item.samples.data(), rawAudio.data(), rawAudio.size());

        {
            std::lock_guard<std::mutex> lk(m_playback_mutex);
            m_playback_queue.push_back(std::move(item));
            m_playback_enqueued.fetch_add(1);
        }
        m_playback_cv.notify_one();
        ensurePlaybackThread();
        return SWITCH_STATUS_SUCCESS;
    }

    static void convertChannels(const std::vector<int16_t>& in, int in_ch, int out_ch, std::vector<int16_t>& out) {
        if (in_ch == out_ch) {
            out = in;
            return;
        }

        const size_t frames = in.size() / in_ch;
        out.resize(frames * out_ch);

        if (in_ch == 2 && out_ch == 1) {
            for (size_t i = 0; i < frames; ++i) {
                int32_t mixed = ((int32_t)in[2 * i] + (int32_t)in[2 * i + 1]) / 2;
                out[i] = clamp16(mixed);
            }
        } else if (in_ch == 1 && out_ch == 2) {
            for (size_t i = 0; i < frames; ++i) {
                out[2 * i] = in[i];
                out[2 * i + 1] = in[i];
            }
        } else {
            // unsupported conversion, pass through first channel duplicated
            for (size_t i = 0; i < frames; ++i) {
                for (int c = 0; c < out_ch; ++c) {
                    out[i * out_ch + c] = in[std::min<size_t>(i * in_ch, in.size() - 1)];
                }
            }
        }
    }

    switch_status_t enqueuePlaybackResampled(const std::string& rawAudio,
                                             int in_rate,
                                             int in_channels,
                                             int target_rate,
                                             int target_channels) {
        if (rawAudio.size() < sizeof(int16_t) || in_rate <= 0 || target_rate <= 0) {
            return SWITCH_STATUS_FALSE;
        }

        // decode input samples
        std::vector<int16_t> in_samples(rawAudio.size() / sizeof(int16_t));
        memcpy(in_samples.data(), rawAudio.data(), rawAudio.size());

        // convert channels first if needed to match resampler channel count
        std::vector<int16_t> ch_converted;
        convertChannels(in_samples, in_channels, target_channels, ch_converted);

        // resample if rate differs
        std::vector<int16_t> final_samples;
        if (in_rate == target_rate) {
            final_samples = std::move(ch_converted);
        } else {
            spx_uint32_t in_len = ch_converted.size() / target_channels;
            // estimate output length
            spx_uint32_t out_len = (in_len * target_rate) / in_rate + 16;
            std::vector<int16_t> out(out_len * target_channels);

            int err = 0;
            SpeexResamplerState* st = speex_resampler_init(target_channels, in_rate, target_rate, SWITCH_RESAMPLE_QUALITY, &err);
            if (!st || err != RESAMPLER_ERR_SUCCESS) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                                  "(%s) enqueuePlaybackResampled: resampler init failed: %s\n",
                                  m_sessionId.c_str(), speex_resampler_strerror(err));
                if (st) speex_resampler_destroy(st);
                return SWITCH_STATUS_FALSE;
            }

            spx_uint32_t in_len_tmp = in_len;
            spx_uint32_t out_len_tmp = out_len;
            if (target_channels == 1) {
                speex_resampler_process_int(st, 0,
                                            ch_converted.data(), &in_len_tmp,
                                            out.data(), &out_len_tmp);
            } else {
                speex_resampler_process_interleaved_int(st,
                                                        ch_converted.data(), &in_len_tmp,
                                                        out.data(), &out_len_tmp);
            }
            speex_resampler_destroy(st);

            out.resize(out_len_tmp * target_channels);
            final_samples = std::move(out);
        }

        // pack back to bytes and enqueue
        std::string packed(reinterpret_cast<const char*>(final_samples.data()),
                           final_samples.size() * sizeof(int16_t));
        return enqueuePlaybackRaw(packed, target_rate, target_channels);
    }

    size_t playbackQueueSize() {
        std::lock_guard<std::mutex> lk(m_playback_mutex);
        return m_playback_queue.size();
    }

    void ensurePlaybackThread() {
        bool expected = false;
        if (!m_playback_thread_started.compare_exchange_strong(expected, true)) {
            return;
        }

        m_playback_thread = std::thread([this](){
            playbackLoop();
        });
        m_playback_thread.detach();
    }

    void playbackLoop() {
        while (true) {
            {
                std::unique_lock<std::mutex> lk(m_playback_mutex);
                m_playback_cv.wait(lk, [this]{
                    return m_shutdown || !m_playback_queue.empty();
                });
                if (m_shutdown && m_playback_queue.empty()) {
                    break;
                }
            }

            switch_core_session_t* session = switch_core_session_locate(m_sessionId.c_str());
            if (!session) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            switch_codec_t *codec = switch_core_session_get_read_codec(session);
            if (!codec) {
                switch_core_session_rwunlock(session);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            const int target_rate = codec->implementation->actual_samples_per_second;
            const int target_channels = codec->implementation->number_of_channels;
            const size_t samples_per_frame = target_rate / 50;

            while (true) {
                std::vector<int16_t> mixed;
                bool have_data = false;

                {
                    std::lock_guard<std::mutex> lk(m_playback_mutex);
                    if (m_shutdown || m_playback_queue.empty()) {
                        break;
                    }

                    std::vector<int32_t> accum(samples_per_frame * target_channels, 0);

                    for (auto it = m_playback_queue.begin(); it != m_playback_queue.end(); ) {
                        if (it->sample_rate != target_rate || it->channels != target_channels) {
                            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                                              "(%s) dropping playback item (rate/ch mismatch %d/%d vs %d/%d)\n",
                                              m_sessionId.c_str(),
                                              it->sample_rate, it->channels,
                                              target_rate, target_channels);
                            it = m_playback_queue.erase(it);
                            continue;
                        }

                        size_t total_samples = it->samples.size() / it->channels;
                        if (it->pos >= total_samples) {
                            it = m_playback_queue.erase(it);
                            continue;
                        }

                        const size_t remaining = total_samples - it->pos;
                        const size_t take = std::min(samples_per_frame, remaining);
                        have_data = true;

                        for (size_t s = 0; s < take * it->channels; ++s) {
                            const size_t idx = s;
                            const int32_t cur = accum[idx];
                            const int32_t add = it->samples[it->pos * it->channels + s];
                            int32_t sum = cur + add;
                            if (sum > 32767) sum = 32767;
                            if (sum < -32768) sum = -32768;
                            accum[idx] = sum;
                        }

                        it->pos += take;
                        if (it->pos >= total_samples) {
                            m_playback_drained.fetch_add(1);
                            it = m_playback_queue.erase(it);
                        } else {
                            ++it;
                        }
                    }

                    if (have_data) {
                        mixed.resize(samples_per_frame * target_channels);
                        for (size_t i = 0; i < mixed.size(); ++i) {
                            mixed[i] = static_cast<int16_t>(accum[i]);
                        }
                    }
                } // mutex scope

                if (!have_data) {
                    break;
                }

                switch_frame_t frame = {0};
                frame.codec = codec;
                frame.data = mixed.data();
                frame.datalen = mixed.size() * sizeof(int16_t);
                frame.buflen = frame.datalen;
                frame.samples = samples_per_frame;
                frame.rate = target_rate;
                frame.channels = target_channels;

                if (switch_core_session_write_frame(session, &frame, SWITCH_IO_FLAG_NONE, 0) != SWITCH_STATUS_SUCCESS) {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                                      "(%s) playback mixer write failed\n", m_sessionId.c_str());
                    break;
                }
            }

            switch_core_session_rwunlock(session);
        }
    }

    void scheduleReconnect() {
        if (m_no_reconnect || m_shutdown) {
            return;
        }

        bool expected = false;
        if (!m_reconnecting.compare_exchange_strong(expected, true)) {
            return; // already reconnecting
        }

        std::thread([this](){
            const int delays[] = {1, 2, 4, 8, 16};
            for (int delay : delays) {
                if (m_shutdown || m_no_reconnect) break;
                if (client.isConnected()) break;
                std::this_thread::sleep_for(std::chrono::seconds(delay));
                if (m_shutdown || m_no_reconnect) break;
                m_reconnect_attempts.fetch_add(1);
                client.connect();
            }
            m_reconnecting = false;
        }).detach();
    }

    std::string m_sessionId;
    responseHandler_t m_notify;
    WebSocketClient client;
    bool m_suppress_log;
    const char* m_extra_headers;
    int m_playFile;
    std::unordered_set<std::string> m_Files;
    std::atomic<bool> m_no_reconnect;
    std::atomic<bool> m_shutdown;
    std::atomic<bool> m_reconnecting;
    std::atomic<uint64_t> m_backpressure_drops;
    std::atomic<uint64_t> m_playback_enqueued;
    std::atomic<uint64_t> m_playback_drained;
    std::atomic<uint64_t> m_reconnect_attempts;
    std::mutex m_playback_mutex;
    std::condition_variable m_playback_cv;
    std::deque<PlaybackItem> m_playback_queue;
    std::atomic<bool> m_playback_thread_started{false};
    std::thread m_playback_thread;
    std::thread m_shutdown_thread;
    std::mutex m_shutdown_thread_mutex;
};


namespace {

    switch_status_t stream_data_init(private_t *tech_pvt, switch_core_session_t *session, char *wsUri,
                                     uint32_t sampling, int desiredSampling, int channels, char *metadata, responseHandler_t responseHandler,
                                     int deflate, int heart_beat, bool suppressLog, int rtp_packets, const char* extra_headers,
                                     bool no_reconnect, const char *tls_cafile, const char *tls_keyfile,
                                     const char *tls_certfile, bool tls_disable_hostname_validation)
    {
        int err = 0; // speex

        switch_memory_pool_t *pool = switch_core_session_get_pool(session);

        memset(tech_pvt, 0, sizeof(private_t));

        strncpy(tech_pvt->sessionId, switch_core_session_get_uuid(session), MAX_SESSION_ID);
        strncpy(tech_pvt->ws_uri, wsUri, MAX_WS_URI);
        tech_pvt->sampling = desiredSampling;
        tech_pvt->responseHandler = responseHandler;
        tech_pvt->rtp_packets = rtp_packets;
        tech_pvt->channels = channels;
        tech_pvt->audio_paused = 0;

        if (metadata) {
            strncpy(tech_pvt->initialMetadata, metadata, MAX_METADATA_LEN);
        }

        const size_t buflen = (FRAME_SIZE_8000 * desiredSampling / 8000 * channels * rtp_packets);

        auto* as = new AudioStreamer(tech_pvt->sessionId,
                                     wsUri,
                                     responseHandler,
                                     deflate,
                                     heart_beat,
                                     suppressLog,
                                     extra_headers,
                                     no_reconnect,
                                     tls_cafile,
                                     tls_keyfile,
                                     tls_certfile,
                                     tls_disable_hostname_validation);

        tech_pvt->pAudioStreamer = static_cast<void *>(as);

        switch_mutex_init(&tech_pvt->mutex, SWITCH_MUTEX_NESTED, pool);

        if (switch_buffer_create(pool, &tech_pvt->sbuffer, buflen) != SWITCH_STATUS_SUCCESS) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                              "%s: Error creating switch buffer.\n", tech_pvt->sessionId);
            return SWITCH_STATUS_FALSE;
        }

        if (desiredSampling != (int)sampling) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                              "(%s) resampling from %u to %u\n", tech_pvt->sessionId, sampling, desiredSampling);
            tech_pvt->resampler = speex_resampler_init(channels, sampling, desiredSampling, SWITCH_RESAMPLE_QUALITY, &err);
            if (0 != err) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                  "Error initializing resampler: %s.\n", speex_resampler_strerror(err));
                return SWITCH_STATUS_FALSE;
            }
        } else {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                              "(%s) no resampling needed for this call\n", tech_pvt->sessionId);
        }

        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                          "(%s) stream_data_init\n", tech_pvt->sessionId);

        return SWITCH_STATUS_SUCCESS;
    }

    void destroy_tech_pvt(private_t* tech_pvt) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "%s destroy_tech_pvt\n", tech_pvt->sessionId);

        if (tech_pvt->resampler) {
            speex_resampler_destroy(tech_pvt->resampler);
            tech_pvt->resampler = nullptr;
        }

        if (tech_pvt->mutex) {
            switch_mutex_destroy(tech_pvt->mutex);
            tech_pvt->mutex = nullptr;
        }

        if (tech_pvt->pAudioStreamer) {
            auto* as = (AudioStreamer *) tech_pvt->pAudioStreamer;
            as->joinShutdownThread(); // wait for finish() thread if it was started
            delete as;
            tech_pvt->pAudioStreamer = nullptr;
        }
    }

    void finish(private_t* tech_pvt) {
        auto* as = static_cast<AudioStreamer *>(tech_pvt->pAudioStreamer);
        tech_pvt->pAudioStreamer = nullptr;
        if (as) {
            as->startShutdownThread();
        }
    }

}

extern "C" {
    int validate_ws_uri(const char* url, char* wsUri) {
        const char* scheme = nullptr;
        const char* hostStart = nullptr;
        const char* hostEnd = nullptr;
        const char* portStart = nullptr;

        // Check scheme
        if (strncmp(url, "ws://", 5) == 0) {
            scheme = "ws";
            (void)scheme; // unused but kept for clarity
            hostStart = url + 5;
        } else if (strncmp(url, "wss://", 6) == 0) {
            scheme = "wss";
            (void)scheme;
            hostStart = url + 6;
        } else {
            return 0;
        }

        // Find host end or port start
        hostEnd = hostStart;
        while (*hostEnd && *hostEnd != ':' && *hostEnd != '/') {
            if (!std::isalnum(static_cast<unsigned char>(*hostEnd)) &&
                *hostEnd != '-' && *hostEnd != '.') {
                return 0;
            }
            ++hostEnd;
        }

        // Check if host is empty
        if (hostStart == hostEnd) {
            return 0;
        }

        // Check for port
        if (*hostEnd == ':') {
            portStart = hostEnd + 1;
            while (*portStart && *portStart != '/') {
                if (!std::isdigit(static_cast<unsigned char>(*portStart))) {
                    return 0;
                }
                ++portStart;
            }
        }

        // Copy valid URI to wsUri
        std::strncpy(wsUri, url, MAX_WS_URI);
        wsUri[MAX_WS_URI - 1] = '\0';
        return 1;
    }

    switch_status_t is_valid_utf8(const char *str) {
        switch_status_t status = SWITCH_STATUS_FALSE;
        while (*str) {
            if ((*str & 0x80) == 0x00) {
                // 1-byte character
                str++;
            } else if ((*str & 0xE0) == 0xC0) {
                // 2-byte character
                if ((str[1] & 0xC0) != 0x80) {
                    return status;
                }
                str += 2;
            } else if ((*str & 0xF0) == 0xE0) {
                // 3-byte character
                if ((str[1] & 0xC0) != 0x80 || (str[2] & 0xC0) != 0x80) {
                    return status;
                }
                str += 3;
            } else if ((*str & 0xF8) == 0xF0) {
                // 4-byte character
                if ((str[1] & 0xC0) != 0x80 || (str[2] & 0xC0) != 0x80 || (str[3] & 0xC0) != 0x80) {
                    return status;
                }
                str += 4;
            } else {
                // invalid character
                return status;
            }
        }
        return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t stream_session_send_text(switch_core_session_t *session, char* text) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        auto *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);
        if (!bug) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                              "stream_session_send_text failed because no bug\n");
            return SWITCH_STATUS_FALSE;
        }
        auto *tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);

        if (!tech_pvt) return SWITCH_STATUS_FALSE;
        auto *pAudioStreamer = static_cast<AudioStreamer *>(tech_pvt->pAudioStreamer);
        if (pAudioStreamer && text) pAudioStreamer->writeText(text);

        return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t stream_session_metrics(switch_core_session_t *session, char** json_metrics) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        auto *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);
        if (!bug) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                              "stream_session_metrics failed because no bug\n");
            return SWITCH_STATUS_FALSE;
        }
        auto *tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);

        if (!tech_pvt || !json_metrics) return SWITCH_STATUS_FALSE;
        auto *pAudioStreamer = static_cast<AudioStreamer *>(tech_pvt->pAudioStreamer);
        if (!pAudioStreamer) return SWITCH_STATUS_FALSE;

        char *metrics = pAudioStreamer->buildMetricsJson();
        *json_metrics = metrics;
        return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t stream_session_pauseresume(switch_core_session_t *session, int pause) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        auto *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);
        if (!bug) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                              "stream_session_pauseresume failed because no bug\n");
            return SWITCH_STATUS_FALSE;
        }
        auto *tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);

        if (!tech_pvt) return SWITCH_STATUS_FALSE;

        switch_core_media_bug_flush(bug);
        tech_pvt->audio_paused = pause;
        return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t stream_session_init(switch_core_session_t *session,
                                        responseHandler_t responseHandler,
                                        uint32_t samples_per_second,
                                        char *wsUri,
                                        int sampling,
                                        int channels,
                                        char* metadata,
                                        void **ppUserData)
    {
        int deflate = 0;
        int heart_beat = 0;
        bool suppressLog = false;
        const char* buffer_size = nullptr;
        const char* extra_headers = nullptr;
        int rtp_packets = 1;
        bool no_reconnect = false;
        const char* tls_cafile = nullptr;
        const char* tls_keyfile = nullptr;
        const char* tls_certfile = nullptr;
        bool tls_disable_hostname_validation = false;

        switch_channel_t *channel = switch_core_session_get_channel(session);

        // Load XML config defaults first; channel variables override them below.
        switch_xml_t cfg, xml, settings, param;
        if ((xml = switch_xml_open_cfg("audio_stream.conf", &cfg, nullptr))) {
            if ((settings = switch_xml_child(cfg, "settings"))) {
                for (param = switch_xml_child(settings, "param"); param; param = param->next) {
                    const char *name = switch_xml_attr_soft(param, "name");
                    const char *val  = switch_xml_attr_soft(param, "value");
                    if (!name || !val) continue;
                    if (!strcasecmp(name, "message-deflate") && switch_true(val)) {
                        deflate = 1;
                    } else if (!strcasecmp(name, "heart-beat")) {
                        char *ep = nullptr;
                        long v = strtol(val, &ep, 10);
                        if (*ep == '\0') heart_beat = (int)v;
                    } else if (!strcasecmp(name, "suppress-log") && switch_true(val)) {
                        suppressLog = true;
                    } else if (!strcasecmp(name, "buffer-size")) {
                        int bSize = atoi(val);
                        if (bSize >= 20 && bSize % 20 == 0) rtp_packets = bSize / 20;
                    } else if (!strcasecmp(name, "no-reconnect") && switch_true(val)) {
                        no_reconnect = true;
                    }
                }
            }
            switch_xml_free(xml);
        }

        // Channel variables override XML config.
        if (switch_channel_var_true(channel, "STREAM_MESSAGE_DEFLATE")) {
            deflate = 1;
        }

        if (switch_channel_var_true(channel, "STREAM_SUPPRESS_LOG")) {
            suppressLog = true;
        }

        if (switch_channel_var_true(channel, "STREAM_NO_RECONNECT")) {
            no_reconnect = true;
        }

        tls_cafile = switch_channel_get_variable(channel, "STREAM_TLS_CA_FILE");
        tls_keyfile = switch_channel_get_variable(channel, "STREAM_TLS_KEY_FILE");
        tls_certfile = switch_channel_get_variable(channel, "STREAM_TLS_CERT_FILE");

        if (switch_channel_var_true(channel, "STREAM_TLS_DISABLE_HOSTNAME_VALIDATION")) {
            tls_disable_hostname_validation = true;
        }

        const char* heartBeat = switch_channel_get_variable(channel, "STREAM_HEART_BEAT");
        if (heartBeat) {
            char *endptr = nullptr;
            long value = strtol(heartBeat, &endptr, 10);
            if (*endptr == '\0' && value <= INT_MAX && value >= INT_MIN) {
                heart_beat = (int) value;
            }
        }

        if ((buffer_size = switch_channel_get_variable(channel, "STREAM_BUFFER_SIZE"))) {
            int bSize = atoi(buffer_size);
            if (bSize % 20 != 0) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                                  "%s: Buffer size of %s is not a multiple of 20ms. Using default 20ms.\n",
                                  switch_channel_get_name(channel), buffer_size);
            } else if (bSize >= 20) {
                rtp_packets = bSize / 20;
            }
        }

        extra_headers = switch_channel_get_variable(channel, "STREAM_EXTRA_HEADERS");

        // allocate per-session tech_pvt
        auto* tech_pvt = (private_t *) switch_core_session_alloc(session, sizeof(private_t));

        if (!tech_pvt) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                              "error allocating memory!\n");
            return SWITCH_STATUS_FALSE;
        }

        if (SWITCH_STATUS_SUCCESS != stream_data_init(tech_pvt,
                                                      session,
                                                      wsUri,
                                                      samples_per_second,
                                                      sampling,
                                                      channels,
                                                      metadata,
                                                      responseHandler,
                                                      deflate,
                                                      heart_beat,
                                                      suppressLog,
                                                      rtp_packets,
                                                      extra_headers,
                                                      no_reconnect,
                                                      tls_cafile,
                                                      tls_keyfile,
                                                      tls_certfile,
                                                      tls_disable_hostname_validation)) {
            destroy_tech_pvt(tech_pvt);
            return SWITCH_STATUS_FALSE;
        }

        *ppUserData = tech_pvt;

        return SWITCH_STATUS_SUCCESS;
    }

    switch_bool_t stream_frame(switch_media_bug_t *bug) {
        auto *tech_pvt = (private_t *)switch_core_media_bug_get_user_data(bug);
        if (!tech_pvt || tech_pvt->audio_paused) return SWITCH_TRUE;
        switch_core_session_t *session = switch_core_media_bug_get_session(bug);

        if (switch_mutex_trylock(tech_pvt->mutex) == SWITCH_STATUS_SUCCESS) {

            if (!tech_pvt->pAudioStreamer) {
                switch_mutex_unlock(tech_pvt->mutex);
                return SWITCH_TRUE;
            }

            auto *pAudioStreamer = static_cast<AudioStreamer *>(tech_pvt->pAudioStreamer);

            if (!pAudioStreamer->isConnected()) {
                switch_mutex_unlock(tech_pvt->mutex);
                return SWITCH_TRUE;
            }

            if (nullptr == tech_pvt->resampler) {

                uint8_t data_buf[SWITCH_RECOMMENDED_BUFFER_SIZE];
                switch_frame_t frame = {0};
                frame.data = data_buf;
                frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

                while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS) {
                    if (frame.datalen) {
                        if (pAudioStreamer->bufferedAmount() >= WS_MAX_BUFFERED_BYTES) {
                            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                                                  "(%s) websocket backpressure - dropping frame (buffered=%zu bytes)\n",
                                                  tech_pvt->sessionId, pAudioStreamer->bufferedAmount());
                            pAudioStreamer->noteBackpressureDrop();
                            continue;
                        }
                        if (tech_pvt->rtp_packets == 1) {
                            pAudioStreamer->writeBinary((uint8_t *) frame.data, frame.datalen);
                            continue;
                        }

                        size_t available = switch_buffer_freespace(tech_pvt->sbuffer);

                        if (available >= frame.datalen) {
                            switch_buffer_write(tech_pvt->sbuffer,
                                                static_cast<uint8_t *>(frame.data),
                                                frame.datalen);
                        }

                        if (switch_buffer_freespace(tech_pvt->sbuffer) == 0) {
                            switch_size_t inuse = switch_buffer_inuse(tech_pvt->sbuffer);
                            if (inuse > 0) {
                                std::vector<uint8_t> tmp(inuse);
                                switch_buffer_read(tech_pvt->sbuffer, tmp.data(), inuse);
                                switch_buffer_zero(tech_pvt->sbuffer);
                                pAudioStreamer->writeBinary(tmp.data(), inuse);
                            }
                        }
                    }
                }

            } else {

                uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
                switch_frame_t frame = {};
                frame.data = data;
                frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

                while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS) {
                    if (frame.datalen) {
                        if (pAudioStreamer->bufferedAmount() >= WS_MAX_BUFFERED_BYTES) {
                            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                                              "(%s) websocket backpressure - dropping resampled frame (buffered=%zu bytes)\n",
                                              tech_pvt->sessionId, pAudioStreamer->bufferedAmount());
                            pAudioStreamer->noteBackpressureDrop();
                            continue;
                        }
                        size_t available = switch_buffer_freespace(tech_pvt->sbuffer);

                        spx_uint32_t in_len = frame.samples;
                        size_t max_samples = available / sizeof(spx_int16_t);

                        if (max_samples == 0 && tech_pvt->rtp_packets != 1) {
                            // Нет места в буфере, попробуем сбросить
                            switch_size_t inuse = switch_buffer_inuse(tech_pvt->sbuffer);
                            if (inuse > 0) {
                                std::vector<uint8_t> tmp(inuse);
                                switch_buffer_read(tech_pvt->sbuffer, tmp.data(), inuse);
                                switch_buffer_zero(tech_pvt->sbuffer);
                                pAudioStreamer->writeBinary(tmp.data(), inuse);
                            }
                            available = switch_buffer_freespace(tech_pvt->sbuffer);
                            max_samples = available / sizeof(spx_int16_t);
                        }

                        if (max_samples == 0 && tech_pvt->rtp_packets != 1) {
                            continue;
                        }

                        // Даже если rtp_packets == 1, нам нужно хоть немного места
                        if (max_samples == 0 && tech_pvt->rtp_packets == 1) {
                            continue;
                        }

                        std::vector<spx_int16_t> out(max_samples);
                        spx_uint32_t out_len = max_samples / tech_pvt->channels;

                        if (tech_pvt->channels == 1) {
                            speex_resampler_process_int(tech_pvt->resampler,
                                                        0,
                                                        (const spx_int16_t *)frame.data,
                                                        &in_len,
                                                        out.data(),
                                                        &out_len);
                        } else {
                            speex_resampler_process_interleaved_int(tech_pvt->resampler,
                                                                    (const spx_int16_t *)frame.data,
                                                                    &in_len,
                                                                    out.data(),
                                                                    &out_len);
                        }

                        if (out_len > 0) {
                            const size_t bytes_written = out_len * tech_pvt->channels * sizeof(spx_int16_t);
                            if (tech_pvt->rtp_packets == 1) { // 20ms packet
                                pAudioStreamer->writeBinary((uint8_t *) out.data(), bytes_written);
                                continue;
                            }

                            if (bytes_written <= available) {
                                switch_buffer_write(tech_pvt->sbuffer,
                                                    (const uint8_t *)out.data(),
                                                    bytes_written);
                            }
                        }

                        if (switch_buffer_freespace(tech_pvt->sbuffer) == 0 && tech_pvt->rtp_packets != 1) {
                            switch_size_t inuse = switch_buffer_inuse(tech_pvt->sbuffer);
                            if (inuse > 0) {
                                std::vector<uint8_t> tmp(inuse);
                                switch_buffer_read(tech_pvt->sbuffer, tmp.data(), inuse);
                                switch_buffer_zero(tech_pvt->sbuffer);
                                pAudioStreamer->writeBinary(tmp.data(), inuse);
                            }
                        }
                    }
                }
            }

            switch_mutex_unlock(tech_pvt->mutex);
        }

        return SWITCH_TRUE;
    }

    switch_status_t stream_session_cleanup(switch_core_session_t *session, char* text, int channelIsClosing) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        auto *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);
        if (bug) {
            auto* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
            char sessionId[MAX_SESSION_ID];
            strncpy(sessionId, tech_pvt->sessionId, MAX_SESSION_ID - 1);
        sessionId[MAX_SESSION_ID - 1] = '\0';

            switch_mutex_lock(tech_pvt->mutex);
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                              "(%s) stream_session_cleanup\n", sessionId);

            switch_channel_set_private(channel, MY_BUG_NAME, nullptr);
            if (!channelIsClosing) {
                switch_core_media_bug_remove(session, &bug);
            }

            auto* audioStreamer = (AudioStreamer *) tech_pvt->pAudioStreamer;
            if (audioStreamer) {
                audioStreamer->deleteFiles();
                if (text) audioStreamer->writeText(text);
                finish(tech_pvt);
            }

            switch_mutex_unlock(tech_pvt->mutex);
            destroy_tech_pvt(tech_pvt);

            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                              "(%s) stream_session_cleanup: connection closed\n", sessionId);
            return SWITCH_STATUS_SUCCESS;
        }

        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                          "stream_session_cleanup: no bug - websocket connection already closed\n");
        return SWITCH_STATUS_FALSE;
    }
}
