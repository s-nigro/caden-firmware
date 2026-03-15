#pragma once

// ── WiFi ─────────────────────────────────────────────────────────────────────
// Wird aus credentials.h eingelesen (nicht im Repo)
#include "credentials.h"

// ── MQTT ─────────────────────────────────────────────────────────────────────
#define MQTT_HOST        "192.168.178.128"
#define MQTT_PORT        1883
#define MQTT_USER        "esp32-voice"
#define MQTT_PASS        "caden_mqtt_2024"

// ── OTA Server ───────────────────────────────────────────────────────────────
#define OTA_HOST         "192.168.178.128"
#define OTA_PORT         8088
#define OTA_CHECK_MS     300000   // Alle 5 Minuten

// ── Audio ─────────────────────────────────────────────────────────────────────
#define AUDIO_SAMPLE_RATE    16000
#define AUDIO_FRAME_MS       30
#define AUDIO_FRAME_SAMPLES  (AUDIO_SAMPLE_RATE * AUDIO_FRAME_MS / 1000)  // 480
#define AUDIO_BUFFER_BYTES   (AUDIO_FRAME_SAMPLES * 2)                     // 960

// ── VAD ───────────────────────────────────────────────────────────────────────
#define VAD_ENERGY_THRESHOLD    500
#define VAD_HANGOVER_FRAMES     10
#define WAKEWORD                "caden"
