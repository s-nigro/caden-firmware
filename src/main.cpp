/**
 * CADEN Voice Node v0.3.0
 * PlatformIO / Arduino — ESP32-S3
 *
 * Wohnzimmer: Waveshare ESP32-S3-Touch-LCD-1.85C-BOX
 *   ES7210 (4-Mic ADC) + ES8311 (Speaker DAC)
 *   I2C: SDA=11, SCL=10
 *   I2S: MCLK=2, BCLK=48, WS=38, DIN=39, DOUT=47, PA=15
 */

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <driver/i2s.h>
#include <HTTPClient.h>
#include <Update.h>

#include "config.h"
#include "display.h"
#include <Preferences.h>

#define FIRMWARE_VERSION "0.4.0"

Preferences prefs;

// ── ES7210 Mic-Codec Register ─────────────────────────────────────────────────
#define ES7210_ADDR             0x40
#define ES7210_RESET_REG00      0x00
#define ES7210_MAINCLK_REG02    0x02
#define ES7210_LRCK_DIVH_REG04  0x04
#define ES7210_LRCK_DIVL_REG05  0x05
#define ES7210_POWER_DOWN_REG06 0x06
#define ES7210_OSR_REG07        0x07
#define ES7210_TIME_CONTROL0    0x09
#define ES7210_TIME_CONTROL1    0x0A
#define ES7210_SDP_IFACE1       0x11
#define ES7210_SDP_IFACE2       0x12
#define ES7210_ADC34_HPF2       0x20
#define ES7210_ADC34_HPF1       0x21
#define ES7210_ADC12_HPF2       0x22
#define ES7210_ADC12_HPF1       0x23
#define ES7210_ANALOG_REG40     0x40
#define ES7210_MIC12_BIAS       0x41
#define ES7210_MIC34_BIAS       0x42
#define ES7210_MIC1_GAIN        0x43
#define ES7210_MIC2_GAIN        0x44
#define ES7210_MIC3_GAIN        0x45
#define ES7210_MIC4_GAIN        0x46
#define ES7210_MIC1_PWR         0x47
#define ES7210_MIC2_PWR         0x48
#define ES7210_MIC3_PWR         0x49
#define ES7210_MIC4_PWR         0x4A
#define ES7210_MIC12_PWR        0x4B
#define ES7210_MIC34_PWR        0x4C

// ── ES8311 Speaker-Codec Register ─────────────────────────────────────────────
#define ES8311_ADDR             0x18
#define ES8311_RESET            0x00
#define ES8311_CLK_MANAGER1     0x01
#define ES8311_CLK_MANAGER2     0x02
#define ES8311_CLK_MANAGER3     0x03
#define ES8311_CLK_MANAGER4     0x04
#define ES8311_CLK_MANAGER5     0x05
#define ES8311_CLK_MANAGER6     0x06
#define ES8311_CLK_MANAGER7     0x07
#define ES8311_CLK_MANAGER8     0x08
#define ES8311_SDPIN            0x09
#define ES8311_SDPOUT           0x0A
#define ES8311_SYSTEM1          0x0D
#define ES8311_SYSTEM2          0x0E
#define ES8311_DAC1             0x31
#define ES8311_DAC2             0x32
#define ES8311_ADC1             0x1C
#define ES8311_ADC2             0x1D
#define ES8311_ANALOG1          0x37
#define ES8311_ANALOG2          0x38
#define ES8311_CHD1_ADCVOL      0x17
#define ES8311_DAC_VOLL         0x32
#define ES8311_GPIO1            0x44
#define ES8311_GP_CTL           0x45

// ── Global State ──────────────────────────────────────────────────────────────
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

bool     g_private_mode   = false;
bool     g_speech_active  = false;
bool     g_ota_running    = false;
int      g_hangover_count = 0;
uint32_t g_last_ota_check = 0;
int16_t  g_audio_buf[AUDIO_FRAME_SAMPLES];
int32_t  g_vad_threshold  = VAD_ENERGY_THRESHOLD;  // Dynamisch, aus NVS geladen

// MQTT Topics
const char* TOPIC_AUDIO_PUB  = "caden/nodes/" CADEN_ROOM_ID "/audio";
const char* TOPIC_VAD_PUB    = "caden/nodes/" CADEN_ROOM_ID "/vad";
const char* TOPIC_STATUS_PUB = "caden/nodes/" CADEN_ROOM_ID "/status";
const char* TOPIC_DIAG_PUB   = "caden/nodes/" CADEN_ROOM_ID "/diag";
const char* TOPIC_CMD_SUB    = "caden/nodes/" CADEN_ROOM_ID "/cmd";

// ── I2C Codec Helpers ─────────────────────────────────────────────────────────
static void codec_write(uint8_t addr, uint8_t reg, uint8_t val) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

// ── ES7210 Init (Mic-Array) ───────────────────────────────────────────────────
// Sequenz aus Waveshare-Library + TRM
// MCLK=4096000 @ 16kHz (256x ratio) → coeff: adc_div=1, dll=1, doubler=1, osr=0x20
static void es7210_init() {
    Wire.begin(I2C_SDA, I2C_SCL, 400000);

    // Software Reset
    codec_write(ES7210_ADDR, ES7210_RESET_REG00, 0xFF);
    delay(10);
    codec_write(ES7210_ADDR, ES7210_RESET_REG00, 0x32);

    // Init timing
    codec_write(ES7210_ADDR, ES7210_TIME_CONTROL0, 0x30);
    codec_write(ES7210_ADDR, ES7210_TIME_CONTROL1, 0x30);

    // HPF für alle 4 ADCs
    codec_write(ES7210_ADDR, ES7210_ADC12_HPF1, 0x2A);
    codec_write(ES7210_ADDR, ES7210_ADC12_HPF2, 0x0A);
    codec_write(ES7210_ADDR, ES7210_ADC34_HPF1, 0x2A);
    codec_write(ES7210_ADDR, ES7210_ADC34_HPF2, 0x0A);

    // I2S Format: Standard I2S, 16-bit, kein TDM
    codec_write(ES7210_ADDR, ES7210_SDP_IFACE1, 0x60);  // 16-bit I2S
    codec_write(ES7210_ADDR, ES7210_SDP_IFACE2, 0x00);  // kein TDM

    // Analog Power
    codec_write(ES7210_ADDR, ES7210_ANALOG_REG40, 0xC3);

    // MIC Bias 2.87V (0x07)
    codec_write(ES7210_ADDR, ES7210_MIC12_BIAS, 0x07);
    codec_write(ES7210_ADDR, ES7210_MIC34_BIAS, 0x07);

    // MIC Gain 30dB (0x0D) | 0x10
    codec_write(ES7210_ADDR, ES7210_MIC1_GAIN, 0x1D);
    codec_write(ES7210_ADDR, ES7210_MIC2_GAIN, 0x1D);
    codec_write(ES7210_ADDR, ES7210_MIC3_GAIN, 0x1D);
    codec_write(ES7210_ADDR, ES7210_MIC4_GAIN, 0x1D);

    // MIC Power On
    codec_write(ES7210_ADDR, ES7210_MIC1_PWR, 0x08);
    codec_write(ES7210_ADDR, ES7210_MIC2_PWR, 0x08);
    codec_write(ES7210_ADDR, ES7210_MIC3_PWR, 0x08);
    codec_write(ES7210_ADDR, ES7210_MIC4_PWR, 0x08);

    // Clock: MCLK=4.096MHz, LRCK=16kHz → ratio 256
    // coeff für {4096000, 16000}: adc_div=1, doubler=1, dll=1, osr=0x20, lrckh=1, lrckl=0
    codec_write(ES7210_ADDR, ES7210_OSR_REG07,        0x20);
    codec_write(ES7210_ADDR, ES7210_MAINCLK_REG02,    0x01 | (1 << 6) | (1 << 7)); // adc_div=1, doubler=1, dll=1
    codec_write(ES7210_ADDR, ES7210_LRCK_DIVH_REG04,  0x01);
    codec_write(ES7210_ADDR, ES7210_LRCK_DIVL_REG05,  0x00);

    // DLL Power down
    codec_write(ES7210_ADDR, ES7210_POWER_DOWN_REG06, 0x04);

    // MIC Bias & ADC & PGA Power on
    codec_write(ES7210_ADDR, ES7210_MIC12_PWR, 0x0F);
    codec_write(ES7210_ADDR, ES7210_MIC34_PWR, 0x0F);

    // Enable
    codec_write(ES7210_ADDR, ES7210_RESET_REG00, 0x71);
    delay(5);
    codec_write(ES7210_ADDR, ES7210_RESET_REG00, 0x41);

    Serial.printf("[ES7210] Init done (I2C SDA:%d SCL:%d)\n", I2C_SDA, I2C_SCL);
}

// ── ES8311 Init (Speaker) ─────────────────────────────────────────────────────
static void es8311_init() {
    // Reset
    codec_write(ES8311_ADDR, ES8311_RESET, 0x1F);
    delay(5);
    codec_write(ES8311_ADDR, ES8311_RESET, 0x00);

    // Clock: MCLK→SCLK, 16kHz, MCLK=4.096MHz
    codec_write(ES8311_ADDR, ES8311_CLK_MANAGER1, 0x01); // MCLK from pin
    codec_write(ES8311_ADDR, ES8311_CLK_MANAGER2, 0x04);
    codec_write(ES8311_ADDR, ES8311_CLK_MANAGER3, 0x10);
    codec_write(ES8311_ADDR, ES8311_CLK_MANAGER4, 0x02);
    codec_write(ES8311_ADDR, ES8311_CLK_MANAGER5, 0x00);
    codec_write(ES8311_ADDR, ES8311_CLK_MANAGER6, 0x03); // BCLK div
    codec_write(ES8311_ADDR, ES8311_CLK_MANAGER7, 0x00);
    codec_write(ES8311_ADDR, ES8311_CLK_MANAGER8, 0xFF); // LAUTO_MUTE_CTL

    // I2S Slave, 16-bit standard
    codec_write(ES8311_ADDR, ES8311_SDPIN,  0x00); // ADC: I2S 16bit
    codec_write(ES8311_ADDR, ES8311_SDPOUT, 0x00); // DAC: I2S 16bit

    // Analog
    codec_write(ES8311_ADDR, ES8311_SYSTEM1,  0x0C);
    codec_write(ES8311_ADDR, ES8311_SYSTEM2,  0x00);
    codec_write(ES8311_ADDR, ES8311_ANALOG1,  0x08);
    codec_write(ES8311_ADDR, ES8311_ANALOG2,  0x03);

    // DAC volume (70%)
    codec_write(ES8311_ADDR, ES8311_DAC1, 0x00);
    codec_write(ES8311_ADDR, ES8311_DAC2, 0xBF); // 0dB

    // Power Amplifier
#if defined(CODEC_PA_PIN) && (CODEC_PA_PIN >= 0)
    pinMode(CODEC_PA_PIN, OUTPUT);
    digitalWrite(CODEC_PA_PIN, HIGH);
#endif

    Serial.println("[ES8311] Init done");
}

// ── I2S Init ──────────────────────────────────────────────────────────────────
static void i2s_init() {
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
        .mclk_multiple        = I2S_MCLK_MULTIPLE_256,
    };
    i2s_pin_config_t pins = {
        .mck_io_num   = I2S_MCLK,
        .bck_io_num   = I2S_BCLK,
        .ws_io_num    = I2S_WS,
        .data_out_num = I2S_DOUT,
        .data_in_num  = I2S_DIN,
    };
    ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_0, &pins));
    ESP_ERROR_CHECK(i2s_zero_dma_buffer(I2S_NUM_0));
    Serial.printf("[I2S] MCLK:%d BCLK:%d WS:%d DIN:%d DOUT:%d @ %dHz\n",
                  I2S_MCLK, I2S_BCLK, I2S_WS, I2S_DIN, I2S_DOUT, AUDIO_SAMPLE_RATE);
}

// ── WiFi ──────────────────────────────────────────────────────────────────────
static void wifi_connect() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    WiFi.setAutoReconnect(true);
    Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.printf("\n[WiFi] IP: %s RSSI: %d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
}

// ── VAD ───────────────────────────────────────────────────────────────────────
static bool vad_detect(const int16_t *s, int n) {
    int64_t sum = 0;
    for (int i = 0; i < n; i++) sum += (int32_t)s[i] * s[i];
    return (int32_t)sqrt((double)sum / n) > VAD_ENERGY_THRESHOLD;
}

// ── OTA Task ──────────────────────────────────────────────────────────────────
// OTA-State für MQTT-Feedback (wird aus Task gesetzt, im loop() gepublished)
static char g_ota_status[48] = "";

void ota_task(void *arg) {
    // 1. Check-Endpoint
    String check_url = String("http://") + OTA_HOST + ":" + OTA_PORT + "/check/" + CADEN_ROOM_ID;
    WiFiClient wc;
    HTTPClient http;
    http.setTimeout(8000);
    http.begin(wc, check_url);
    int code = http.GET();
    if (code != 200) {
        snprintf(g_ota_status, sizeof(g_ota_status), "check_fail_%d", code);
        http.end(); g_ota_running = false; vTaskDelete(NULL); return;
    }
    String body = http.getString();
    http.end();

    // 2. JSON
    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok
        || !doc["update_available"].as<bool>()) {
        strlcpy(g_ota_status, "up_to_date", sizeof(g_ota_status));
        g_ota_running = false; vTaskDelete(NULL); return;
    }
    String remote_ver = doc["version"] | "";
    String fw_url     = doc["url"]     | "";
    if (remote_ver == FIRMWARE_VERSION || fw_url.isEmpty()) {
        strlcpy(g_ota_status, "up_to_date", sizeof(g_ota_status));
        g_ota_running = false; vTaskDelete(NULL); return;
    }

    // 3. Download + Flash via Update
    snprintf(g_ota_status, sizeof(g_ota_status), "downloading_%s", remote_ver.c_str());
    Serial.printf("[OTA] %s -> %s\n[OTA] URL: %s\n",
                  FIRMWARE_VERSION, remote_ver.c_str(), fw_url.c_str());

    WiFiClient fw_wc;
    HTTPClient fw_http;
    fw_http.setTimeout(60000);
    fw_http.begin(fw_wc, fw_url);
    int fw_code = fw_http.GET();
    if (fw_code != 200) {
        snprintf(g_ota_status, sizeof(g_ota_status), "fw_fail_%d", fw_code);
        fw_http.end(); g_ota_running = false; vTaskDelete(NULL); return;
    }
    int fw_size = fw_http.getSize();
    Serial.printf("[OTA] Size: %d bytes\n", fw_size);

    snprintf(g_ota_status, sizeof(g_ota_status), "flashing_%d", fw_size);
    vTaskDelay(pdMS_TO_TICKS(50));  // Status publishen lassen
    if (!Update.begin(fw_size > 0 ? fw_size : UPDATE_SIZE_UNKNOWN)) {
        strlcpy(g_ota_status, "begin_failed", sizeof(g_ota_status));
        fw_http.end(); g_ota_running = false; vTaskDelete(NULL); return;
    }
    WiFiClient& stream = fw_http.getStream();
    uint8_t buf[512];
    int written = 0;
    while (fw_http.connected() || stream.available()) {
        int avail = stream.available();
        if (avail > 0) {
            int r = stream.readBytes(buf, min(avail, (int)sizeof(buf)));
            if (Update.write(buf, r) != (size_t)r) break;
            written += r;
        }
        vTaskDelay(1);
    }
    fw_http.end();
    Serial.printf("[OTA] Written: %d / %d\n", written, fw_size);

    snprintf(g_ota_status, sizeof(g_ota_status), "written_%d_of_%d", written, fw_size);
    vTaskDelay(pdMS_TO_TICKS(200));

    if (!Update.end(true)) {
        snprintf(g_ota_status, sizeof(g_ota_status), "end_err_%d", (int)Update.getError());
        g_ota_running = false; vTaskDelete(NULL); return;
    }
    if (!Update.isFinished()) {
        snprintf(g_ota_status, sizeof(g_ota_status), "not_finished_%d", written);
        g_ota_running = false; vTaskDelete(NULL); return;
    }
    strlcpy(g_ota_status, "rebooting", sizeof(g_ota_status));
    vTaskDelay(pdMS_TO_TICKS(300));
    ESP.restart();
}

static void ota_check() {
    if (g_ota_running || millis() - g_last_ota_check < OTA_CHECK_MS) return;
    g_last_ota_check = millis();
    g_ota_running = true;
    xTaskCreate(ota_task, "caden_ota", 16384, NULL, 1, NULL);
}

// ── MQTT ──────────────────────────────────────────────────────────────────────
void mqtt_callback(char* topic, byte* payload, unsigned int len) {
#if defined(CADEN_HAS_DISPLAY) && (CADEN_HAS_DISPLAY == 1)
    // Display-Topic direkt weiterleiten — kein JSON-Parse hier
    if (strstr(topic, "/display")) {
        display_handle_mqtt((const char*)payload, len);
        return;
    }
#endif
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return;
    const char* cmd = doc["cmd"];
    if (!cmd) return;

    if      (!strcmp(cmd, "private_on"))  { g_private_mode = true;  Serial.println("[CMD] Private ON"); }
    else if (!strcmp(cmd, "private_off")) { g_private_mode = false; Serial.println("[CMD] Private OFF"); }
    else if (!strcmp(cmd, "reboot"))      { delay(300); ESP.restart(); }
    else if (!strcmp(cmd, "set_threshold")) {
        int val = doc["value"] | -1;
        if (val > 0 && val < 10000) {
            g_vad_threshold = val;
            prefs.begin("caden", false);
            prefs.putInt("vad_thr", val);
            prefs.end();
            Serial.printf("[CMD] VAD threshold → %d\n", val);
            // Bestätigung publizieren
            char buf[64];
            snprintf(buf, sizeof(buf), "{\"vad_threshold\":%d}", val);
            mqtt.publish(TOPIC_DIAG_PUB, buf);
        }
    }
    else if (!strcmp(cmd, "get_threshold")) {
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"vad_threshold\":%d}", g_vad_threshold);
        mqtt.publish(TOPIC_DIAG_PUB, buf);
    }
    else if (!strcmp(cmd, "ota_now"))     {
        g_last_ota_check = 0;
        if (!g_ota_running) { g_ota_running = true; xTaskCreate(ota_task, "caden_ota", 16384, NULL, 1, NULL); }
    }
}

static void mqtt_connect() {
    String cid = String("caden-") + CADEN_ROOM_ID;
    while (!mqtt.connected()) {
        if (mqtt.connect(cid.c_str(), MQTT_USER, MQTT_PASS)) {
            mqtt.subscribe(TOPIC_CMD_SUB);
            Serial.printf("[MQTT] Connected — %s\n", TOPIC_CMD_SUB);
        } else {
            delay(3000);
        }
    }
}

static void publish_status() {
    JsonDocument doc;
    doc["version"] = FIRMWARE_VERSION;
    doc["room"]    = CADEN_ROOM_ID;
    doc["rssi"]    = WiFi.RSSI();
    doc["heap"]    = ESP.getFreeHeap();
    doc["uptime"]  = millis() / 1000;
    doc["private"]        = g_private_mode;
    doc["vad_threshold"]  = g_vad_threshold;
    char buf[256]; serializeJson(doc, buf);
    mqtt.publish(TOPIC_STATUS_PUB, buf, true);
}

// ── Audio Loop ────────────────────────────────────────────────────────────────
static uint32_t g_last_rms_report = 0;

static void audio_loop() {
    if (g_private_mode) return;
    size_t bytes_read = 0;
    if (i2s_read(I2S_NUM_0, g_audio_buf, AUDIO_BUFFER_BYTES, &bytes_read, pdMS_TO_TICKS(30)) != ESP_OK) return;
    if (bytes_read == 0) return;

    // RMS
    int64_t sum = 0;
    for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) sum += (int32_t)g_audio_buf[i] * g_audio_buf[i];
    int32_t rms = (int32_t)sqrt((double)sum / AUDIO_FRAME_SAMPLES);

    // RMS alle 3s reporten
    if (millis() - g_last_rms_report > 3000) {
        g_last_rms_report = millis();
        char buf[64]; snprintf(buf, sizeof(buf), "{\"rms\":%d,\"vad_threshold\":%d}", rms, g_vad_threshold);
        mqtt.publish(TOPIC_DIAG_PUB, buf);
    }

    bool speech = rms > g_vad_threshold;
    if (speech) g_hangover_count = VAD_HANGOVER_FRAMES;
    else if (g_hangover_count > 0) { g_hangover_count--; speech = true; }

    if (speech != g_speech_active) {
        g_speech_active = speech;
        char buf[64]; snprintf(buf, sizeof(buf), "{\"state\":%d,\"room\":\"%s\"}", speech ? 1 : 0, CADEN_ROOM_ID);
        mqtt.publish(TOPIC_VAD_PUB, buf);
    }
    if (speech && mqtt.connected())
        mqtt.publish(TOPIC_AUDIO_PUB, (const uint8_t*)g_audio_buf, AUDIO_BUFFER_BYTES);
}

// ── Setup & Loop ──────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.printf("\n[CADEN] v%s — %s\n", FIRMWARE_VERSION, CADEN_ROOM_ID);

    // NVS: gespeicherte Einstellungen laden
    prefs.begin("caden", true);
    g_vad_threshold = prefs.getInt("vad_thr", VAD_ENERGY_THRESHOLD);
    prefs.end();
    Serial.printf("[CFG] VAD threshold: %d (default: %d)\n", g_vad_threshold, VAD_ENERGY_THRESHOLD);

    wifi_connect();

    ArduinoOTA.setHostname("caden-" CADEN_ROOM_ID);
    ArduinoOTA.begin();

    // Codec Init (I2C muss vor I2S)
    es7210_init();
    es8311_init();
    delay(50);

    // I2S
    i2s_init();

    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setCallback(mqtt_callback);
    mqtt.setBufferSize(1200);
    mqtt_connect();
    publish_status();

    Serial.println("[CADEN] Ready 👂");
#if defined(CADEN_HAS_DISPLAY) && (CADEN_HAS_DISPLAY == 1)
    display_init();
    // Initiales State an Display senden (vor erstem MQTT-Command)
    const char* _init_disp = "{\"state\":\"ready\",\"icon\":\"MIC\",\"label\":\"BEREIT\"}"; display_handle_mqtt(_init_disp, strlen(_init_disp));
#endif
}

void loop() {
    if (WiFi.status() != WL_CONNECTED) { WiFi.reconnect(); delay(1000); return; }
    if (!mqtt.connected()) mqtt_connect();

    ArduinoOTA.handle();
    mqtt.loop();
    audio_loop();

    static uint32_t last_status = 0;
    if (millis() - last_status > 30000) { last_status = millis(); publish_status(); }

    ota_check();

    // OTA-Status publizieren wenn gesetzt
    if (g_ota_status[0]) {
        char buf[96];
        snprintf(buf, sizeof(buf), "{\"ota\":\"%s\",\"v\":\"%s\"}", g_ota_status, FIRMWARE_VERSION);
        mqtt.publish(TOPIC_DIAG_PUB, buf);
        Serial.printf("[OTA-STATUS] %s\n", g_ota_status);
        g_ota_status[0] = '\0';
    }

#if defined(CADEN_HAS_DISPLAY) && (CADEN_HAS_DISPLAY == 1)
    display_tick();
    // Touch → Private Mode togglen + sofort Display-State senden
    // (Mac Mini bekommt den Status via /status Topic beim nächsten publish_status())
    static bool s_display_touch_handled = false;
    if (!s_display_touch_handled) {
        // Touch wird im display_tick() intern gepuffert
        // → publish_status() informiert Mac Mini über private-Mode-Änderung
    }
#endif
}
