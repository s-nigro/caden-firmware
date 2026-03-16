/**
 * CADEN Display — MQTT-driven Renderer
 * ST7789 240x280 via LovyanGFX, CST816 Touch via I2C
 *
 * Alles was angezeigt wird kommt per MQTT — der ESP kennt keine Logik.
 */

#include "display.h"

#if defined(CADEN_HAS_DISPLAY) && (CADEN_HAS_DISPLAY == 1)

#include <Wire.h>
#include <math.h>

// ── LovyanGFX Config (ST7789, SPI) ──────────────────────────────────────────
class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789 _panel;
    lgfx::Bus_SPI      _bus;
    lgfx::Light_PWM    _light;
public:
    LGFX() {
        { auto cfg = _bus.config();
          cfg.spi_host   = SPI3_HOST;
          cfg.spi_mode   = 0;
          cfg.freq_write = 40000000;
          cfg.pin_sclk   = DISP_SPI_SCLK;
          cfg.pin_mosi   = DISP_SPI_MOSI;
          cfg.pin_miso   = -1;
          cfg.pin_dc     = DISP_DC;
          _bus.config(cfg); }
        _panel.setBus(&_bus);

        { auto cfg = _panel.config();
          cfg.pin_cs        = DISP_SPI_CS;
          cfg.pin_rst       = DISP_RST;
          cfg.panel_width   = DISP_W;
          cfg.panel_height  = DISP_H;
          cfg.offset_y      = 20;   // ST7789 frame offset für 1.85"
          cfg.invert        = true;
          cfg.rgb_order     = false;
          _panel.config(cfg); }

        { auto cfg = _light.config();
          cfg.pin_bl      = DISP_BL;
          cfg.invert      = false;
          cfg.freq        = 44100;
          cfg.pwm_channel = 7;
          _light.config(cfg); }
        _panel.setLight(&_light);

        setPanel(&_panel);
    }
};

static LGFX        gfx;
static LGFX_Sprite spr(&gfx);   // Doppelpuffer — kein Flackern

// ── Render-State (kommt alles via MQTT) ─────────────────────────────────────
struct RenderState {
    // Haupt-Zustand
    char     state[16]   = "ready";
    char     icon[8]     = "MIC";
    char     label[24]   = "BEREIT";
    uint16_t color       = 0;      // 0 = auto via state
    uint8_t  color_r     = 40;
    uint8_t  color_g     = 80;
    uint8_t  color_b     = 200;

    // Sprecher
    char     person[32]  = "";

    // Fortschrittsbalken (Enrollment etc.)
    bool     has_progress = false;
    int      prog_value   = 0;
    int      prog_total   = 8;
    char     prog_label[24] = "";

    // Wetter
    bool     has_weather  = false;
    int      weather_temp = 0;
    char     weather_desc[32] = "";

    // Alert-Overlay
    bool     has_alert    = false;
    char     alert_text[64] = "";
    char     alert_level[12] = "info";  // info | warning | alert
    uint32_t alert_until  = 0;          // millis() timeout, 0 = permanent

    // Helligkeit
    uint8_t  brightness   = 200;

    // Animations-Tick
    bool     animated     = false;
} s;

// ── Touch-State ──────────────────────────────────────────────────────────────
static bool     s_tapped   = false;
static uint8_t  s_touch_x  = 0;
static uint8_t  s_touch_y  = 0;
static uint32_t s_last_touch = 0;

// ── Farb-Palette ─────────────────────────────────────────────────────────────
static uint16_t C_BG()      { return gfx.color565(10, 12, 20); }
static uint16_t C_DIM()     { return gfx.color565(70, 75, 90); }
static uint16_t C_TEXT()    { return gfx.color565(210, 215, 225); }
static uint16_t C_WHITE()   { return gfx.color565(240, 240, 245); }

static uint16_t alert_color(const char* level) {
    if (strcmp(level, "alert") == 0)   return gfx.color565(220, 40, 40);
    if (strcmp(level, "warning") == 0) return gfx.color565(220, 160, 20);
    return gfx.color565(40, 140, 220);  // info
}

// Automatische Farbe anhand State-Name (Fallback wenn kein color[] im JSON)
static uint16_t auto_color(const char* state) {
    if (strcmp(state, "listening") == 0) return gfx.color565(30, 180, 80);
    if (strcmp(state, "thinking")  == 0) return gfx.color565(220, 180, 30);
    if (strcmp(state, "speaking")  == 0) return gfx.color565(30, 180, 200);
    if (strcmp(state, "private")   == 0) return gfx.color565(200, 40, 40);
    if (strcmp(state, "ota")       == 0) return gfx.color565(220, 120, 20);
    if (strcmp(state, "error")     == 0) return gfx.color565(200, 30, 30);
    return gfx.color565(40, 80, 200);  // ready + default
}

static uint16_t main_color() {
    // Expliziter override aus JSON → nutzen
    if (s.color_r != 0 || s.color_g != 0 || s.color_b != 0)
        return gfx.color565(s.color_r, s.color_g, s.color_b);
    return auto_color(s.state);
}

// ── Animation Helpers ─────────────────────────────────────────────────────────
static void draw_pulse_ring(int cx, int cy, int r, uint16_t color) {
    float t   = (float)(millis() % 1200) / 1200.0f;
    int   rad = r + (int)(5.0f * sinf(t * 2.0f * 3.14159f));
    spr.drawCircle(cx, cy, rad,     color);
    spr.drawCircle(cx, cy, rad + 1, color);
}

static void draw_spinner(int cx, int cy, uint16_t color) {
    uint32_t t = millis() / 70;
    for (int i = 0; i < 8; i++) {
        float a = (i * 45.0f + (t % 24) * 15.0f) * 3.14159f / 180.0f;
        int   x = cx + (int)(22 * cosf(a));
        int   y = cy + (int)(22 * sinf(a));
        uint8_t b = (i == (t % 8)) ? 240 : 50;
        spr.fillCircle(x, y, 3, gfx.color565(b, b, b));
    }
}

static void draw_progress_bar(int x, int y, int w, int h,
                               int value, int total, uint16_t color) {
    spr.drawRect(x, y, w, h, C_DIM());
    if (total > 0 && value > 0) {
        int filled = (int)((float)value / total * (w - 2));
        spr.fillRect(x + 1, y + 1, filled, h - 2, color);
    }
}

// ── Touch (CST816) ───────────────────────────────────────────────────────────
static void touch_init_hw() {
    if (TOUCH_RST_PIN >= 0) {
        pinMode(TOUCH_RST_PIN, OUTPUT);
        digitalWrite(TOUCH_RST_PIN, LOW);  delay(10);
        digitalWrite(TOUCH_RST_PIN, HIGH); delay(50);
    }
    if (TOUCH_INT_PIN >= 0)
        pinMode(TOUCH_INT_PIN, INPUT);
}

static bool touch_poll(uint8_t* out_x, uint8_t* out_y) {
    Wire.beginTransmission(TOUCH_I2C_ADDR);
    Wire.write(0x02);
    if (Wire.endTransmission(false) != 0) return false;
    Wire.requestFrom((uint8_t)TOUCH_I2C_ADDR, (uint8_t)6);
    if (Wire.available() < 6) { while(Wire.available()) Wire.read(); return false; }
    uint8_t fingers = Wire.read();
    Wire.read();                      // gesture
    uint8_t xh = Wire.read() & 0x0F;
    uint8_t xl = Wire.read();
    uint8_t yh = Wire.read() & 0x0F;
    uint8_t yl = Wire.read();
    if (fingers == 0) return false;
    *out_x = (xh << 4) | (xl >> 4);
    *out_y = (yh << 4) | (yl >> 4);
    return true;
}

// ── Render ───────────────────────────────────────────────────────────────────
static void render() {
    spr.fillScreen(C_BG());
    uint16_t mc = main_color();
    int cx = DISP_W / 2;  // 120

    // ── Kopfzeile: Status-Indikator (farbige Linie oben) ────────────────────
    spr.fillRect(0, 0, DISP_W, 3, mc);

    // ── Icon-Ring (Mitte, Y=95) ──────────────────────────────────────────────
    int icon_y = 110;
    bool animated = (strcmp(s.state, "listening") == 0 ||
                     strcmp(s.state, "speaking")  == 0);
    bool spinning = (strcmp(s.state, "thinking") == 0);

    if (animated) {
        draw_pulse_ring(cx, icon_y, 30, mc);
    } else if (spinning) {
        draw_spinner(cx, icon_y, mc);
    } else {
        spr.drawCircle(cx, icon_y, 30, mc);
        spr.drawCircle(cx, icon_y, 31, mc);
    }

    // Icon-Text im Ring
    spr.setFont(&fonts::FreeSansBold9pt7b);
    spr.setTextColor(mc, C_BG());
    int iw = spr.textWidth(s.icon);
    spr.setCursor(cx - iw / 2, icon_y + 6);
    spr.print(s.icon);

    // ── Label ────────────────────────────────────────────────────────────────
    spr.setFont(&fonts::FreeSans9pt7b);
    spr.setTextColor(mc, C_BG());
    int lw = spr.textWidth(s.label);
    spr.setCursor(cx - lw / 2, 153);
    spr.print(s.label);

    // ── Fortschrittsbalken (Enrollment / OTA) ────────────────────────────────
    if (s.has_progress) {
        // Label
        spr.setFont(&fonts::FreeSans9pt7b);
        spr.setTextColor(C_DIM(), C_BG());
        char ptext[40];
        snprintf(ptext, sizeof(ptext), "%s  %d / %d",
                 s.prog_label, s.prog_value, s.prog_total);
        int ptw = spr.textWidth(ptext);
        spr.setCursor(cx - ptw / 2, 172);
        spr.print(ptext);
        // Balken
        int bx = 20, bw = DISP_W - 40, bh = 8;
        draw_progress_bar(bx, 178, bw, bh, s.prog_value, s.prog_total, mc);
    }

    // ── Trennlinie ────────────────────────────────────────────────────────────
    spr.drawFastHLine(12, 197, DISP_W - 24, C_DIM());

    // ── Sprecher ─────────────────────────────────────────────────────────────
    if (s.person[0]) {
        spr.setFont(&fonts::FreeSans9pt7b);
        spr.setTextColor(C_TEXT(), C_BG());
        char pline[48];
        snprintf(pline, sizeof(pline), "%s  \xb7  " CADEN_ROOM_ID, s.person);
        int pw = spr.textWidth(pline);
        spr.setCursor(cx - pw / 2, 215);
        spr.print(pline);
    }

    // ── Wetter ───────────────────────────────────────────────────────────────
    if (s.has_weather) {
        spr.drawFastHLine(12, 228, DISP_W - 24, C_DIM());
        spr.setFont(&fonts::FreeSans9pt7b);
        spr.setTextColor(C_DIM(), C_BG());
        char wbuf[40];
        snprintf(wbuf, sizeof(wbuf), "%d\xb0""C  %s",
                 s.weather_temp, s.weather_desc);
        int ww = spr.textWidth(wbuf);
        spr.setCursor(cx - ww / 2, 250);
        spr.print(wbuf);
    }

    // ── Alert-Overlay (über allem) ────────────────────────────────────────────
    if (s.has_alert) {
        uint16_t ac = alert_color(s.alert_level);
        // Halbtransparentes Overlay (Rechteck mit Rahmen)
        spr.fillRoundRect(10, 60, DISP_W - 20, 60, 8, C_BG());
        spr.drawRoundRect(10, 60, DISP_W - 20, 60, 8, ac);
        spr.drawRoundRect(11, 61, DISP_W - 22, 58, 7, ac);
        // Level-Badge
        spr.setFont(&fonts::FreeSansBold9pt7b);
        spr.setTextColor(ac, C_BG());
        char lvl_upper[12] = "";
        for (int i = 0; s.alert_level[i]; i++)
            lvl_upper[i] = toupper(s.alert_level[i]);
        int bw = spr.textWidth(lvl_upper);
        spr.setCursor(cx - bw / 2, 78);
        spr.print(lvl_upper);
        // Alert-Text
        spr.setFont(&fonts::FreeSans9pt7b);
        spr.setTextColor(C_TEXT(), C_BG());
        int aw = spr.textWidth(s.alert_text);
        int ax = max(12, cx - aw / 2);
        spr.setCursor(ax, 102);
        spr.print(s.alert_text);
    }

    spr.pushSprite(0, 0);
}

// ── MQTT JSON Handler ─────────────────────────────────────────────────────────
void display_handle_mqtt(const char* payload, size_t len) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return;

    // ── Haupt-State ──────────────────────────────────────────────────────────
    if (doc["state"].is<const char*>())
        strncpy(s.state, doc["state"], sizeof(s.state) - 1);
    if (doc["icon"].is<const char*>())
        strncpy(s.icon, doc["icon"], sizeof(s.icon) - 1);
    if (doc["label"].is<const char*>())
        strncpy(s.label, doc["label"], sizeof(s.label) - 1);

    // ── Farb-Override ─────────────────────────────────────────────────────────
    if (doc["color"].is<JsonArray>()) {
        s.color_r = doc["color"][0] | 0;
        s.color_g = doc["color"][1] | 0;
        s.color_b = doc["color"][2] | 0;
    } else {
        // Reset → auto_color
        s.color_r = s.color_g = s.color_b = 0;
    }

    // ── Sprecher ──────────────────────────────────────────────────────────────
    if (doc["person"].is<const char*>())
        strncpy(s.person, doc["person"], sizeof(s.person) - 1);
    else if (doc["person"].isNull())
        s.person[0] = '\0';

    // ── Fortschritt ───────────────────────────────────────────────────────────
    if (doc["progress"].is<JsonObject>()) {
        s.has_progress  = true;
        s.prog_value    = doc["progress"]["value"] | 0;
        s.prog_total    = doc["progress"]["total"] | 8;
        if (doc["progress"]["label"].is<const char*>())
            strncpy(s.prog_label, doc["progress"]["label"], sizeof(s.prog_label) - 1);
    } else if (doc["progress"].isNull()) {
        s.has_progress = false;
    }

    // ── Wetter ────────────────────────────────────────────────────────────────
    if (doc["weather"].is<JsonObject>()) {
        s.has_weather   = true;
        s.weather_temp  = doc["weather"]["temp"] | 0;
        if (doc["weather"]["desc"].is<const char*>())
            strncpy(s.weather_desc, doc["weather"]["desc"], sizeof(s.weather_desc) - 1);
    }

    // ── Alert ─────────────────────────────────────────────────────────────────
    if (doc["alert"].is<JsonObject>()) {
        s.has_alert = true;
        if (doc["alert"]["text"].is<const char*>())
            strncpy(s.alert_text, doc["alert"]["text"], sizeof(s.alert_text) - 1);
        if (doc["alert"]["level"].is<const char*>())
            strncpy(s.alert_level, doc["alert"]["level"], sizeof(s.alert_level) - 1);
        int ttl = doc["alert"]["ttl"] | 0;
        s.alert_until = ttl > 0 ? millis() + ttl : 0;
    } else if (doc["alert"].isNull()) {
        s.has_alert = false;
    }

    // ── Helligkeit ────────────────────────────────────────────────────────────
    if (doc["dim"].is<int>()) {
        s.brightness = (uint8_t)(doc["dim"].as<int>());
        gfx.setBrightness(s.brightness);
    }

    // Sofort rendern
    render();
}

// ── Public API ────────────────────────────────────────────────────────────────
void display_init() {
    gfx.init();
    gfx.setRotation(0);
    gfx.setBrightness(200);
    gfx.fillScreen(gfx.color565(10, 12, 20));
    spr.createSprite(DISP_W, DISP_H);
    touch_init_hw();

    // Boot-Splash
    gfx.setFont(&fonts::FreeSansBold9pt7b);
    gfx.setTextColor(gfx.color565(40, 80, 200));
    gfx.setCursor(55, 125);
    gfx.print("CADEN");
    gfx.setFont(&fonts::FreeSans9pt7b);
    gfx.setTextColor(gfx.color565(70, 75, 90));
    gfx.setCursor(40, 148);
    gfx.print(CADEN_ROOM_ID);
    delay(1200);

    Serial.println("[Display] Ready — waiting for MQTT");
}

void display_tick() {
    // Alert-TTL prüfen
    if (s.has_alert && s.alert_until > 0 && millis() > s.alert_until) {
        s.has_alert = false;
        render();
    }

    // Animierter State → kontinuierlich neu rendern (30 FPS)
    bool anim = (strcmp(s.state, "listening") == 0 ||
                 strcmp(s.state, "thinking")  == 0 ||
                 strcmp(s.state, "speaking")  == 0);
    static uint32_t last_tick = 0;
    uint32_t interval = anim ? 33 : 0;
    if (anim && millis() - last_tick >= interval) {
        last_tick = millis();
        render();
    }

    // Touch-Poll (entprellt 300ms)
    if (millis() - s_last_touch > 300) {
        uint8_t tx, ty;
        if (touch_poll(&tx, &ty)) {
            s_tapped   = true;
            s_touch_x  = tx;
            s_touch_y  = ty;
            s_last_touch = millis();
            Serial.printf("[Touch] tap x=%d y=%d\n", tx, ty);
        }
    }
}

#endif // CADEN_HAS_DISPLAY
