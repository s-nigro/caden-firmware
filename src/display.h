#pragma once
/**
 * CADEN Display — MQTT-driven Renderer
 * Waveshare ESP32-S3-Touch-LCD-1.85C-BOX (ST7789 240x280, CST816 Touch)
 *
 * Der ESP rendert nur was er per MQTT bekommt.
 * Alle Logik (States, Enrollment, Alerts) liegt auf dem Mac Mini.
 *
 * Subscribe: caden/nodes/<room>/display  → JSON Render-Command
 * Publish:   caden/nodes/<room>/touch    → {"event":"tap","x":120,"y":140}
 *
 * Render-Command Schema:
 * {
 *   "state":    "ready|listening|thinking|speaking|private|ota|error",
 *   "icon":     "MIC",          // 1-4 Zeichen, im Ring
 *   "label":    "BEREIT",       // Haupttext unter Ring
 *   "color":    [40,80,200],    // RGB override (optional)
 *   "person":   "Sascha",       // Sprecher-Zeile (optional)
 *   "progress": {               // Fortschrittsbalken (optional)
 *     "value":  3,
 *     "total":  8,
 *     "label":  "Enrollment"
 *   },
 *   "alert": {                  // Overlay-Nachricht (optional)
 *     "text":  "WiFi verloren",
 *     "level": "info|warning|alert",
 *     "ttl":   4000             // ms bis auto-dismiss (0 = permanent)
 *   },
 *   "weather": {"temp": 8, "desc": "bewoelkt"},
 *   "dim":     180              // Helligkeit 0-255 (optional)
 * }
 */

#if defined(CADEN_HAS_DISPLAY) && (CADEN_HAS_DISPLAY == 1)

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <ArduinoJson.h>

// ── Waveshare ESP32-S3-Touch-LCD-1.85C Pin-Belegung ─────────────────────────
// ACHTUNG: SPI-Pins müssen vom Board-Schaltplan kommen — ggf. anpassen
#define DISP_SPI_MOSI  13
#define DISP_SPI_SCLK  12
#define DISP_SPI_CS    10
#define DISP_DC         9
#define DISP_RST        8
#define DISP_BL        48   // PWM Backlight
#define DISP_W        240
#define DISP_H        280

// Touch CST816 (I2C, shared mit Codec-Bus)
#define TOUCH_I2C_ADDR 0x15
#define TOUCH_INT_PIN  16
#define TOUCH_RST_PIN  17

void display_init();
void display_handle_mqtt(const char* payload, size_t len);
void display_tick();   // aus loop() aufrufen — Animationen + Touch-Poll

#endif // CADEN_HAS_DISPLAY
