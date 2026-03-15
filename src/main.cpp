/**
 * CADEN Voice Node — PlatformIO / Arduino Framework
 * 
 * Läuft auf Wohnzimmer (Waveshare ESP32-S3-Touch-LCD-1.85C-BOX)
 * und Schlafzimmer (Waveshare ESP32-S3-AUDIO-Board).
 * 
 * Board wird per platformio.ini + -DCADEN_ROOM_ID bestimmt.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <driver/i2s.h>
#include <HTTPClient.h>
#include <Update.h>

#include "config.h"

// ── Versionierung ─────────────────────────────────────────────────────────────
#define FIRMWARE_VERSION "0.2.0"

// ── Globale State ─────────────────────────────────────────────────────────────
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

bool         g_private_mode    = false;
bool         g_speech_active   = false;
int          g_hangover_count  = 0;
uint32_t     g_last_ota_check  = 0;

// Audio-Buffer
int16_t      g_audio_buf[AUDIO_FRAME_SAMPLES];

// ── MQTT Topics ───────────────────────────────────────────────────────────────
const char* TOPIC_AUDIO_PUB  = "caden/nodes/" CADEN_ROOM_ID "/audio";
const char* TOPIC_VAD_PUB    = "caden/nodes/" CADEN_ROOM_ID "/vad";
const char* TOPIC_STATUS_PUB = "caden/nodes/" CADEN_ROOM_ID "/status";
const char* TOPIC_CMD_SUB    = "caden/nodes/" CADEN_ROOM_ID "/cmd";

// ── WiFi ──────────────────────────────────────────────────────────────────────
void wifi_connect() {
    Serial.printf("[WiFi] Connecting to %s...\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    WiFi.setAutoReconnect(true);
    
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.printf("\n[WiFi] Connected — IP: %s\n", WiFi.localIP().toString().c_str());
}

// ── I2S Audio ─────────────────────────────────────────────────────────────────
void i2s_init() {
    i2s_config_t cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_TX),
        .sample_rate          = AUDIO_SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 8,
        .dma_buf_len          = AUDIO_FRAME_SAMPLES,
        .use_apll             = true,
        .tx_desc_auto_clear   = true,
    };

    i2s_pin_config_t pins = {
        .bck_io_num   = I2S_BCLK,
        .ws_io_num    = I2S_WS,
        .data_out_num = I2S_DOUT,
        .data_in_num  = I2S_DIN,
    };

    ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_0, &pins));
    ESP_ERROR_CHECK(i2s_zero_dma_buffer(I2S_NUM_0));

#if defined(CODEC_PA_PIN) && (CODEC_PA_PIN >= 0)
    pinMode(CODEC_PA_PIN, OUTPUT);
    digitalWrite(CODEC_PA_PIN, HIGH);
#endif

    Serial.printf("[I2S] Init — BCLK:%d WS:%d DIN:%d DOUT:%d @ %dHz\n",
                  I2S_BCLK, I2S_WS, I2S_DIN, I2S_DOUT, AUDIO_SAMPLE_RATE);
}

// ── VAD (einfache Energie-basierte Erkennung) ─────────────────────────────────
bool vad_detect(const int16_t *samples, int n) {
    int64_t sum = 0;
    for (int i = 0; i < n; i++) {
        sum += (int32_t)samples[i] * samples[i];
    }
    int32_t rms = (int32_t)sqrt((double)sum / n);
    return rms > VAD_ENERGY_THRESHOLD;
}

// ── MQTT Callbacks ────────────────────────────────────────────────────────────
void mqtt_callback(char* topic, byte* payload, unsigned int len) {
    // JSON parsen
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return;

    const char* cmd = doc["cmd"];
    if (!cmd) return;

    if (strcmp(cmd, "private_on") == 0) {
        g_private_mode = true;
        Serial.println("[CMD] Private mode ON");

    } else if (strcmp(cmd, "private_off") == 0) {
        g_private_mode = false;
        Serial.println("[CMD] Private mode OFF");

    } else if (strcmp(cmd, "volume") == 0) {
        int vol = doc["value"] | 70;
        Serial.printf("[CMD] Volume → %d\n", vol);
        // TODO: ES8311 volume via I2C

    } else if (strcmp(cmd, "reboot") == 0) {
        Serial.println("[CMD] Reboot...");
        delay(500);
        ESP.restart();

    } else if (strcmp(cmd, "ota_now") == 0) {
        Serial.println("[CMD] OTA forced by CADEN");
        g_last_ota_check = 0;  // Beim nächsten Loop OTA checken
    }
}

void mqtt_connect() {
    String clientId = String("caden-") + CADEN_ROOM_ID;
    while (!mqtt.connected()) {
        Serial.printf("[MQTT] Connecting as %s...\n", clientId.c_str());
        if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
            mqtt.subscribe(TOPIC_CMD_SUB);
            Serial.printf("[MQTT] Connected — subscribed to %s\n", TOPIC_CMD_SUB);
        } else {
            Serial.printf("[MQTT] Failed (rc=%d) — retry in 3s\n", mqtt.state());
            delay(3000);
        }
    }
}

// ── Status publizieren ────────────────────────────────────────────────────────
void publish_status() {
    JsonDocument doc;
    doc["version"] = FIRMWARE_VERSION;
    doc["room"]    = CADEN_ROOM_ID;
    doc["rssi"]    = WiFi.RSSI();
    doc["heap"]    = ESP.getFreeHeap();
    doc["uptime"]  = millis() / 1000;
    doc["private"] = g_private_mode;

    char buf[256];
    serializeJson(doc, buf);
    mqtt.publish(TOPIC_STATUS_PUB, buf, true);
}

// ── OTA Check ────────────────────────────────────────────────────────────────
void ota_check() {
    if (millis() - g_last_ota_check < OTA_CHECK_MS) return;
    g_last_ota_check = millis();

    HTTPClient http;
    String url = String("http://") + OTA_HOST + ":" + OTA_PORT + "/check/" + CADEN_ROOM_ID;
    http.begin(url);
    int code = http.GET();

    if (code != 200) { http.end(); return; }

    JsonDocument doc;
    if (deserializeJson(doc, http.getString()) != DeserializationError::Ok) { http.end(); return; }
    http.end();

    if (!doc["update_available"].as<bool>()) return;

    const char* newVer = doc["version"];
    if (strcmp(newVer, FIRMWARE_VERSION) == 0) return;  // Gleiche Version

    Serial.printf("[OTA] Update verfügbar: %s → %s\n", FIRMWARE_VERSION, newVer);

    // Firmware herunterladen + flashen
    String fwUrl = doc["url"].as<String>();
    HTTPClient fwHttp;
    fwHttp.begin(fwUrl);
    int fwCode = fwHttp.GET();

    if (fwCode == 200) {
        int fwSize = fwHttp.getSize();
        if (Update.begin(fwSize)) {
            WiFiClient& stream = fwHttp.getStream();
            size_t written = Update.writeStream(stream);
            if (Update.end() && Update.isFinished()) {
                Serial.printf("[OTA] Flash OK (%d bytes) — reboot\n", written);
                fwHttp.end();
                ESP.restart();
            }
        }
    }
    fwHttp.end();
}

// ── Audio Loop ────────────────────────────────────────────────────────────────
void audio_loop() {
    if (g_private_mode) return;

    size_t bytes_read = 0;
    esp_err_t err = i2s_read(I2S_NUM_0, g_audio_buf,
                              AUDIO_BUFFER_BYTES, &bytes_read, pdMS_TO_TICKS(30));
    if (err != ESP_OK || bytes_read == 0) return;

    bool speech = vad_detect(g_audio_buf, AUDIO_FRAME_SAMPLES);

    // Hangover: kurze Pausen überbrücken
    if (speech) {
        g_hangover_count = VAD_HANGOVER_FRAMES;
    } else if (g_hangover_count > 0) {
        g_hangover_count--;
        speech = true;  // Noch als aktiv behandeln
    }

    // VAD-State-Change publizieren
    if (speech != g_speech_active) {
        g_speech_active = speech;

        JsonDocument doc;
        doc["state"]  = speech ? 1 : 0;
        doc["room"]   = CADEN_ROOM_ID;
        char buf[64];
        serializeJson(doc, buf);
        mqtt.publish(TOPIC_VAD_PUB, buf);
    }

    // Audio nur bei aktiver Sprache streamen
    if (speech && mqtt.connected()) {
        mqtt.publish(TOPIC_AUDIO_PUB,
                     (const uint8_t*)g_audio_buf, AUDIO_BUFFER_BYTES);
    }
}

// ── Arduino Setup ─────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.printf("\n[CADEN] Voice Node v%s — Room: %s\n",
                  FIRMWARE_VERSION, CADEN_ROOM_ID);

    wifi_connect();

    // Arduino OTA (für schnelles Dev-Flashing per WiFi)
    ArduinoOTA.setHostname("caden-" CADEN_ROOM_ID);
    ArduinoOTA.onStart([]() { Serial.println("[OTA] Start"); });
    ArduinoOTA.onEnd([]()   { Serial.println("[OTA] Done — reboot"); });
    ArduinoOTA.onError([](ota_error_t e) {
        Serial.printf("[OTA] Error[%u]\n", e);
    });
    ArduinoOTA.begin();

    i2s_init();

    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setCallback(mqtt_callback);
    mqtt.setBufferSize(1024);  // Für Audio-Chunks
    mqtt_connect();

    publish_status();

    Serial.println("[CADEN] Ready — listening 👂");
}

// ── Arduino Loop ──────────────────────────────────────────────────────────────
void loop() {
    // WiFi-Reconnect
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] Reconnecting...");
        WiFi.reconnect();
        delay(1000);
        return;
    }

    // MQTT-Reconnect
    if (!mqtt.connected()) {
        mqtt_connect();
    }

    ArduinoOTA.handle();
    mqtt.loop();
    audio_loop();

    // Status alle 30s
    static uint32_t last_status = 0;
    if (millis() - last_status > 30000) {
        last_status = millis();
        publish_status();
    }

    // OTA-Check alle 5min
    ota_check();
}
