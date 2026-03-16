/**
 * CADEN Display — Waveshare ESP32-S3-Touch-LCD-1.85C-BOX
 * ST77916 QSPI 360x360, CST816 Touch, TCA9554 GPIO Expander
 */

#include "display.h"

#if defined(CADEN_HAS_DISPLAY) && (CADEN_HAS_DISPLAY == 1)

#include <Wire.h>
#include <math.h>

// ── TCA9554 GPIO Expander ────────────────────────────────────────────────────
static uint8_t tca_output = 0xFF;

static void tca_write(uint8_t val) {
    Wire.beginTransmission(TCA9554_ADDR);
    Wire.write(0x01);  // Output register
    Wire.write(val);
    Wire.endTransmission();
    tca_output = val;
}

static void tca_pin(uint8_t pin, bool high) {
    if (high) tca_output |=  (1 << pin);
    else      tca_output &= ~(1 << pin);
    tca_write(tca_output);
}

static void tca_init() {
    Wire.beginTransmission(TCA9554_ADDR);
    Wire.write(0x03);  // Config register — alle Pins als Output
    Wire.write(0x00);
    Wire.endTransmission();
    tca_write(0xFF);   // Alle Pins HIGH
}

// ── LovyanGFX ST77916 QSPI Config ────────────────────────────────────────────
class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ST77961 _panel;
    lgfx::Bus_SPI       _bus;
    lgfx::Light_PWM     _light;
public:
    LGFX() {
        // QSPI via Bus_SPI (pin_io0-io3 = QSPI D0-D3)
        { auto cfg = _bus.config();
          cfg.spi_host   = SPI2_HOST;
          cfg.spi_mode   = 0;
          cfg.freq_write = 40000000;
          cfg.freq_read  = 16000000;
          cfg.spi_3wire  = true;
          cfg.pin_sclk   = DISP_SCK;
          cfg.pin_mosi   = DISP_QSPI_D0;  // D0 = MOSI
          cfg.pin_miso   = -1;
          cfg.pin_dc     = -1;
          cfg.pin_io0    = DISP_QSPI_D0;
          cfg.pin_io1    = DISP_QSPI_D1;
          cfg.pin_io2    = DISP_QSPI_D2;
          cfg.pin_io3    = DISP_QSPI_D3;
          _bus.config(cfg); }
        _panel.setBus(&_bus);

        { auto cfg = _panel.config();
          cfg.pin_cs      = DISP_CS;
          cfg.pin_rst     = -1;   // Reset via TCA9554
          cfg.pin_busy    = -1;
          cfg.panel_width  = DISP_W;
          cfg.panel_height = DISP_H;
          cfg.offset_x    = 0;
          cfg.offset_y    = 0;
          cfg.invert      = true;
          cfg.rgb_order   = false;
          cfg.bus_shared  = false;
          _panel.config(cfg); }

        { auto cfg = _light.config();
          cfg.pin_bl      = DISP_BL;
          cfg.invert      = false;
          cfg.freq        = 44100;
          cfg.pwm_channel = 0;
          _light.config(cfg); }
        _panel.setLight(&_light);

        setPanel(&_panel);
    }
};

static LGFX        gfx;
static LGFX_Sprite spr(&gfx);

// ── Render-State ──────────────────────────────────────────────────────────────
struct RenderState {
    char    state[16]   = "ready";
    char    icon[8]     = "MIC";
    char    label[24]   = "BEREIT";
    uint8_t cr = 40, cg = 80, cb = 200;  // color RGB

    char    person[32]  = "";

    bool    has_progress = false;
    int     prog_value  = 0;
    int     prog_total  = 8;
    char    prog_label[24] = "";

    bool    has_weather  = false;
    int     weather_temp = 0;
    char    weather_desc[32] = "";

    bool    has_alert    = false;
    char    alert_text[64] = "";
    char    alert_level[12] = "info";
    uint32_t alert_until = 0;
} s;

static uint32_t s_last_anim  = 0;
static uint32_t s_last_touch = 0;

// ── Farben ────────────────────────────────────────────────────────────────────
static uint16_t C_BG()   { return gfx.color565(10, 12, 20); }
static uint16_t C_DIM()  { return gfx.color565(70, 75, 90); }
static uint16_t C_TEXT() { return gfx.color565(210, 215, 225); }

static uint16_t main_color() {
    // Expliziter Override
    if (s.cr || s.cg || s.cb)
        return gfx.color565(s.cr, s.cg, s.cb);
    // Auto
    if (!strcmp(s.state, "listening")) return gfx.color565(30, 180, 80);
    if (!strcmp(s.state, "thinking"))  return gfx.color565(220, 180, 30);
    if (!strcmp(s.state, "speaking"))  return gfx.color565(30, 180, 200);
    if (!strcmp(s.state, "private"))   return gfx.color565(200, 40, 40);
    if (!strcmp(s.state, "ota"))       return gfx.color565(220, 120, 20);
    if (!strcmp(s.state, "error"))     return gfx.color565(200, 30, 30);
    if (!strcmp(s.state, "enroll"))    return gfx.color565(140, 60, 220);
    return gfx.color565(40, 80, 200);  // ready
}

static uint16_t alert_color() {
    if (!strcmp(s.alert_level, "alert"))   return gfx.color565(220, 40, 40);
    if (!strcmp(s.alert_level, "warning")) return gfx.color565(220, 160, 20);
    return gfx.color565(40, 140, 220);
}

// ── Animationen ───────────────────────────────────────────────────────────────
static void draw_pulse_ring(int cx, int cy, int r, uint16_t color) {
    float t = (float)(millis() % 1200) / 1200.0f;
    int rad = r + (int)(6.0f * sinf(t * 2.0f * 3.14159f));
    spr.drawCircle(cx, cy, rad,     color);
    spr.drawCircle(cx, cy, rad + 1, color);
    spr.drawCircle(cx, cy, rad + 2, color);
}

static void draw_spinner(int cx, int cy, uint16_t color) {
    uint32_t t = millis() / 70;
    for (int i = 0; i < 8; i++) {
        float a = (i * 45.0f + (t % 24) * 15.0f) * 3.14159f / 180.0f;
        int x = cx + (int)(26 * cosf(a));
        int y = cy + (int)(26 * sinf(a));
        uint8_t b = (i == (t % 8)) ? 240 : 45;
        spr.fillCircle(x, y, 4, gfx.color565(b, b, b));
    }
}

static void draw_progress_bar(int x, int y, int w, int h,
                               int value, int total, uint16_t color) {
    spr.drawRoundRect(x, y, w, h, 3, C_DIM());
    if (total > 0 && value > 0) {
        int filled = max(1, (int)((float)value / total * (w - 4)));
        spr.fillRoundRect(x + 2, y + 2, filled, h - 4, 2, color);
    }
}

// ── Touch (CST816) ────────────────────────────────────────────────────────────
static bool touch_poll() {
    Wire.beginTransmission(TOUCH_I2C_ADDR);
    Wire.write(0x02);
    if (Wire.endTransmission(false) != 0) return false;
    Wire.requestFrom((uint8_t)TOUCH_I2C_ADDR, (uint8_t)6);
    if (Wire.available() < 1) { while(Wire.available()) Wire.read(); return false; }
    uint8_t fingers = Wire.read();
    while (Wire.available()) Wire.read();
    return fingers > 0;
}

// ── Render ────────────────────────────────────────────────────────────────────
static void render() {
    spr.fillScreen(C_BG());
    uint16_t mc = main_color();
    int cx = DISP_W / 2;   // 180
    int cy = DISP_H / 2;   // 180

    // ── Farbige Kopflinie ──────────────────────────────────────────────────
    spr.fillRect(0, 0, DISP_W, 4, mc);

    // ── Icon-Ring (Mitte) ──────────────────────────────────────────────────
    bool animated = (!strcmp(s.state,"listening") || !strcmp(s.state,"speaking"));
    bool spinning = !strcmp(s.state,"thinking");

    if      (animated) draw_pulse_ring(cx, 150, 38, mc);
    else if (spinning)  draw_spinner(cx, 150, mc);
    else {
        spr.drawCircle(cx, 150, 38, mc);
        spr.drawCircle(cx, 150, 39, mc);
        spr.drawCircle(cx, 150, 40, mc);
    }

    // Icon-Text
    spr.setFont(&fonts::FreeSansBold12pt7b);
    spr.setTextColor(mc, C_BG());
    int iw = spr.textWidth(s.icon);
    spr.setCursor(cx - iw / 2, 157);
    spr.print(s.icon);

    // ── Status-Label ──────────────────────────────────────────────────────
    spr.setFont(&fonts::FreeSans9pt7b);
    spr.setTextColor(mc, C_BG());
    int lw = spr.textWidth(s.label);
    spr.setCursor(cx - lw / 2, 205);
    spr.print(s.label);

    // ── Progressbar (Enrollment / OTA) ────────────────────────────────────
    if (s.has_progress) {
        spr.setFont(&fonts::FreeSans9pt7b);
        spr.setTextColor(C_DIM(), C_BG());
        char pt[40];
        snprintf(pt, sizeof(pt), "%s  %d / %d", s.prog_label, s.prog_value, s.prog_total);
        int ptw = spr.textWidth(pt);
        spr.setCursor(cx - ptw / 2, 225);
        spr.print(pt);
        draw_progress_bar(30, 232, DISP_W - 60, 10, s.prog_value, s.prog_total, mc);
    }

    // ── Trennlinie ────────────────────────────────────────────────────────
    spr.drawFastHLine(20, 250, DISP_W - 40, C_DIM());

    // ── Sprecher ──────────────────────────────────────────────────────────
    if (s.person[0]) {
        spr.setFont(&fonts::FreeSans9pt7b);
        spr.setTextColor(C_TEXT(), C_BG());
        char pl[48];
        snprintf(pl, sizeof(pl), "%s  \xb7  " CADEN_ROOM_ID, s.person);
        int pw = spr.textWidth(pl);
        spr.setCursor(cx - pw / 2, 270);
        spr.print(pl);
    }

    // ── Wetter ────────────────────────────────────────────────────────────
    if (s.has_weather) {
        spr.drawFastHLine(20, 283, DISP_W - 40, C_DIM());
        spr.setFont(&fonts::FreeSans9pt7b);
        spr.setTextColor(C_DIM(), C_BG());
        char wb[40];
        snprintf(wb, sizeof(wb), "%d\xb0""C  %s", s.weather_temp, s.weather_desc);
        int ww = spr.textWidth(wb);
        spr.setCursor(cx - ww / 2, 305);
        spr.print(wb);
    }

    // ── Alert Overlay ─────────────────────────────────────────────────────
    if (s.has_alert) {
        uint16_t ac = alert_color();
        spr.fillRoundRect(15, 70, DISP_W - 30, 70, 10, C_BG());
        spr.drawRoundRect(15, 70, DISP_W - 30, 70, 10, ac);
        spr.drawRoundRect(16, 71, DISP_W - 32, 68, 9,  ac);
        // Level
        char lvl[12] = "";
        for (int i = 0; s.alert_level[i]; i++) lvl[i] = toupper(s.alert_level[i]);
        spr.setFont(&fonts::FreeSansBold9pt7b);
        spr.setTextColor(ac, C_BG());
        int bw = spr.textWidth(lvl);
        spr.setCursor(cx - bw / 2, 92);
        spr.print(lvl);
        // Text
        spr.setFont(&fonts::FreeSans9pt7b);
        spr.setTextColor(C_TEXT(), C_BG());
        int aw = spr.textWidth(s.alert_text);
        spr.setCursor(cx - min(aw, DISP_W - 40) / 2, 118);
        spr.print(s.alert_text);
    }

    spr.pushSprite(0, 0);
}

// ── MQTT Handler ─────────────────────────────────────────────────────────────
void display_handle_mqtt(const char* payload, size_t len) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return;

    if (doc["state"].is<const char*>())
        strncpy(s.state, doc["state"].as<const char*>(), sizeof(s.state) - 1);
    if (doc["icon"].is<const char*>())
        strncpy(s.icon, doc["icon"].as<const char*>(), sizeof(s.icon) - 1);
    if (doc["label"].is<const char*>())
        strncpy(s.label, doc["label"].as<const char*>(), sizeof(s.label) - 1);

    if (doc["color"].is<JsonArray>()) {
        s.cr = doc["color"][0] | 0;
        s.cg = doc["color"][1] | 0;
        s.cb = doc["color"][2] | 0;
    } else {
        s.cr = s.cg = s.cb = 0;
    }

    if (doc["person"].is<const char*>())
        strncpy(s.person, doc["person"].as<const char*>(), sizeof(s.person) - 1);
    else if (doc["person"].isNull())
        s.person[0] = '\0';

    if (doc["progress"].is<JsonObject>()) {
        s.has_progress = true;
        s.prog_value   = doc["progress"]["value"] | 0;
        s.prog_total   = doc["progress"]["total"] | 8;
        if (doc["progress"]["label"].is<const char*>())
            strncpy(s.prog_label, doc["progress"]["label"].as<const char*>(), sizeof(s.prog_label) - 1);
    } else if (doc["progress"].isNull()) {
        s.has_progress = false;
    }

    if (doc["weather"].is<JsonObject>()) {
        s.has_weather  = true;
        s.weather_temp = doc["weather"]["temp"] | 0;
        if (doc["weather"]["desc"].is<const char*>())
            strncpy(s.weather_desc, doc["weather"]["desc"].as<const char*>(), sizeof(s.weather_desc) - 1);
    }

    if (doc["alert"].is<JsonObject>()) {
        s.has_alert = true;
        if (doc["alert"]["text"].is<const char*>())
            strncpy(s.alert_text, doc["alert"]["text"].as<const char*>(), sizeof(s.alert_text) - 1);
        if (doc["alert"]["level"].is<const char*>())
            strncpy(s.alert_level, doc["alert"]["level"].as<const char*>(), sizeof(s.alert_level) - 1);
        int ttl = doc["alert"]["ttl"] | 0;
        s.alert_until = ttl > 0 ? millis() + ttl : 0;
    } else if (doc["alert"].isNull()) {
        s.has_alert = false;
    }

    if (doc["dim"].is<int>())
        gfx.setBrightness((uint8_t)doc["dim"].as<int>());

    render();
}

// ── Init ─────────────────────────────────────────────────────────────────────
void display_init() {
    // TCA9554 Expander initialisieren
    tca_init();

    // Touch Reset via Expander
    tca_pin(EXIO_TP_RST, false); delay(10);
    tca_pin(EXIO_TP_RST, true);  delay(50);

    // Display Reset via Expander
    tca_pin(EXIO_LCD_RST, false); delay(20);
    tca_pin(EXIO_LCD_RST, true);  delay(100);

    // Touch INT
    if (TOUCH_INT_PIN >= 0) pinMode(TOUCH_INT_PIN, INPUT);

    // LovyanGFX
    gfx.init();
    gfx.setRotation(0);
    gfx.setBrightness(200);
    gfx.fillScreen(gfx.color565(10, 12, 20));

    spr.createSprite(DISP_W, DISP_H);

    // Splash
    gfx.setFont(&fonts::FreeSansBold12pt7b);
    gfx.setTextColor(gfx.color565(40, 80, 200));
    int tw = gfx.textWidth("CADEN");
    gfx.setCursor((DISP_W - tw) / 2, 165);
    gfx.print("CADEN");
    gfx.setFont(&fonts::FreeSans9pt7b);
    gfx.setTextColor(gfx.color565(70, 75, 90));
    int rw = gfx.textWidth(CADEN_ROOM_ID);
    gfx.setCursor((DISP_W - rw) / 2, 190);
    gfx.print(CADEN_ROOM_ID);
    delay(1500);

    Serial.println("[Display] ST77916 QSPI 360x360 ready");
}

// ── Loop ─────────────────────────────────────────────────────────────────────
void display_tick() {
    // Alert TTL
    if (s.has_alert && s.alert_until > 0 && millis() > s.alert_until) {
        s.has_alert = false;
        render();
    }

    // Animationen
    bool anim = (!strcmp(s.state,"listening") ||
                 !strcmp(s.state,"thinking")  ||
                 !strcmp(s.state,"speaking"));
    if (anim && millis() - s_last_anim >= 33) {
        s_last_anim = millis();
        render();
    }

    // Touch (300ms entprellt)
    if (millis() - s_last_touch > 300 && touch_poll()) {
        s_last_touch = millis();
        Serial.println("[Touch] tap");
    }
}

#endif // CADEN_HAS_DISPLAY
