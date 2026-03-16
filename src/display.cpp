/**
 * CADEN Display Driver — Waveshare ESP32-S3-Touch-LCD-1.85C-BOX
 *
 * Controller : ST77916, QSPI, 360x360
 * Treiber    : esp_lcd_st77916 (Waveshare / Espressif Vendor Driver)
 * Kommunikation: ESP-IDF SPI Master mit QSPI-Opcodes (0x02 cmd / 0x32 data)
 *
 * Basiert auf: github.com/furukawa1020/waveshare-esp32s3-lcd-platformiotest
 */

#include "display.h"

#if defined(CADEN_HAS_DISPLAY) && (CADEN_HAS_DISPLAY == 1)

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <ArduinoJson.h>

#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st77916.h"

// ── Pins ─────────────────────────────────────────────────────────────────────
#define LCD_D0     46
#define LCD_D1     45
#define LCD_D2     42
#define LCD_D3     41
#define LCD_SCK    40
#define LCD_CS     21
#define LCD_TE     18
#define LCD_BL      5
#define LCD_W     360
#define LCD_H     360

#define TCA_ADDR   0x20
#define EXIO_LCD_RST  2
#define EXIO_TP_RST   1

#define TP_ADDR    0x15
#define TP_INT_PIN  4

#define LCD_SPI_HOST     SPI2_HOST
#define LCD_SPI_CLK_HZ   (40 * 1000 * 1000)
#define LCD_CMD_BITS     32
#define LCD_PARAM_BITS    8
#define LCD_QUEUE_DEPTH  10
#define LCD_BUF_LINES    10   // Zeilen pro DMA-Transfer

// ── Vendor Init-Sequenz (vollständig aus Waveshare Demo) ─────────────────────
static const st77916_lcd_init_cmd_t s_vendor_init[] = {
    {0xF0, (uint8_t[]){0x28}, 1, 0},
    {0xF2, (uint8_t[]){0x28}, 1, 0},
    {0x73, (uint8_t[]){0xF0}, 1, 0},
    {0x7C, (uint8_t[]){0xD1}, 1, 0},
    {0x83, (uint8_t[]){0xE0}, 1, 0},
    {0x84, (uint8_t[]){0x61}, 1, 0},
    {0xF2, (uint8_t[]){0x82}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0xF0, (uint8_t[]){0x01}, 1, 0},
    {0xF1, (uint8_t[]){0x01}, 1, 0},
    {0xB0, (uint8_t[]){0x56}, 1, 0},
    {0xB1, (uint8_t[]){0x4D}, 1, 0},
    {0xB2, (uint8_t[]){0x24}, 1, 0},
    {0xB4, (uint8_t[]){0x87}, 1, 0},
    {0xB5, (uint8_t[]){0x44}, 1, 0},
    {0xB6, (uint8_t[]){0x8B}, 1, 0},
    {0xB7, (uint8_t[]){0x40}, 1, 0},
    {0xB8, (uint8_t[]){0x86}, 1, 0},
    {0xBA, (uint8_t[]){0x00}, 1, 0},
    {0xBB, (uint8_t[]){0x08}, 1, 0},
    {0xBC, (uint8_t[]){0x08}, 1, 0},
    {0xBD, (uint8_t[]){0x00}, 1, 0},
    {0xC0, (uint8_t[]){0x80}, 1, 0},
    {0xC1, (uint8_t[]){0x10}, 1, 0},
    {0xC2, (uint8_t[]){0x37}, 1, 0},
    {0xC3, (uint8_t[]){0x80}, 1, 0},
    {0xC4, (uint8_t[]){0x10}, 1, 0},
    {0xC5, (uint8_t[]){0x37}, 1, 0},
    {0xC6, (uint8_t[]){0xA9}, 1, 0},
    {0xC7, (uint8_t[]){0x41}, 1, 0},
    {0xC8, (uint8_t[]){0x01}, 1, 0},
    {0xC9, (uint8_t[]){0xA9}, 1, 0},
    {0xCA, (uint8_t[]){0x41}, 1, 0},
    {0xCB, (uint8_t[]){0x01}, 1, 0},
    {0xD0, (uint8_t[]){0x91}, 1, 0},
    {0xD1, (uint8_t[]){0x68}, 1, 0},
    {0xD2, (uint8_t[]){0x68}, 1, 0},
    {0xF5, (uint8_t[]){0x00, 0xA5}, 2, 0},
    {0xDD, (uint8_t[]){0x4F}, 1, 0},
    {0xDE, (uint8_t[]){0x4F}, 1, 0},
    {0xF1, (uint8_t[]){0x10}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0xF0, (uint8_t[]){0x02}, 1, 0},
    {0xE0, (uint8_t[]){0xF0,0x0A,0x10,0x09,0x09,0x36,0x35,0x33,0x4A,0x29,0x15,0x15,0x2E,0x34}, 14, 0},
    {0xE1, (uint8_t[]){0xF0,0x0A,0x0F,0x08,0x08,0x05,0x34,0x33,0x4A,0x39,0x15,0x15,0x2D,0x33}, 14, 0},
    {0xF0, (uint8_t[]){0x10}, 1, 0},
    {0xF3, (uint8_t[]){0x10}, 1, 0},
    {0xF3, (uint8_t[]){0x01}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0x21, (uint8_t[]){0x00}, 1, 0},   // Display Inversion ON
    {0x11, (uint8_t[]){0x00}, 1, 120}, // Sleep Out + 120ms
    {0x29, (uint8_t[]){0x00}, 1, 0},   // Display ON
};

// ── State ─────────────────────────────────────────────────────────────────────
static esp_lcd_panel_io_handle_t s_io   = NULL;
static esp_lcd_panel_handle_t    s_panel = NULL;
static uint16_t*    s_fb   = NULL;
static uint16_t*    s_dma  = NULL;
static SemaphoreHandle_t s_mutex = NULL;

struct RenderState {
    char    state[16]   = "ready";
    char    icon[8]     = "MIC";
    char    label[24]   = "BEREIT";
    uint8_t cr=40, cg=80, cb=200;
    char    person[32]  = "";
    bool    has_progress = false;
    int     prog_value=0, prog_total=8;
    char    prog_label[24] = "";
    bool    has_weather = false;
    int     weather_temp = 0;
    char    weather_desc[32] = "";
    bool    has_alert = false;
    char    alert_text[64] = "";
    char    alert_level[12] = "info";
    uint32_t alert_until = 0;
} s;

static uint32_t s_last_anim  = 0;
static uint32_t s_last_touch = 0;

// ── TCA9554 ───────────────────────────────────────────────────────────────────
static uint8_t tca_out = 0xFF;
static void tca_write(uint8_t v) {
    Wire.beginTransmission(TCA_ADDR);
    Wire.write(0x01); Wire.write(v); Wire.endTransmission();
    tca_out = v;
}
static void tca_pin(uint8_t p, bool h) {
    if (h) tca_out |= (1<<p); else tca_out &= ~(1<<p);
    tca_write(tca_out);
}
static void tca_init() {
    Wire.beginTransmission(TCA_ADDR); Wire.write(0x03); Wire.write(0x00); Wire.endTransmission();
    tca_write(0xFF);
}

// ── Touch ────────────────────────────────────────────────────────────────────
static bool touch_poll() {
    Wire.beginTransmission(TP_ADDR); Wire.write(0x02);
    if (Wire.endTransmission(false)) return false;
    Wire.requestFrom((uint8_t)TP_ADDR, (uint8_t)1);
    if (!Wire.available()) return false;
    return Wire.read() > 0;
}

// ── RGB565 Framebuffer Helpers ────────────────────────────────────────────────
static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    // Byte-swap für ESP32 SPI DMA
    uint16_t v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (v << 8) | (v >> 8);
}

static void fb_fill(uint16_t c)          { for (int i=0;i<LCD_W*LCD_H;i++) s_fb[i]=c; }
static void fb_hline(int x,int y,int w,uint16_t c) {
    for(int i=0;i<w;i++) if(x+i<LCD_W && y<LCD_H) s_fb[y*LCD_W+x+i]=c;
}
static void fb_rect(int x,int y,int w,int h,uint16_t c) {
    for(int dy=0;dy<h;dy++) fb_hline(x,y+dy,w,c);
}
static void fb_circle(int cx,int cy,int r,uint16_t c,int t=2) {
    for(int dt=0;dt<t;dt++) for(int d=0;d<360;d++) {
        float a=d*3.14159f/180.f;
        int x=cx+(int)((r+dt)*cosf(a));
        int y=cy+(int)((r+dt)*sinf(a));
        if(x>=0&&x<LCD_W&&y>=0&&y<LCD_H) s_fb[y*LCD_W+x]=c;
    }
}

// Minimal Font 5x7
static const uint8_t F57[][5] = {
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},
    {0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},{0x36,0x49,0x49,0x49,0x36},
    {0x06,0x49,0x49,0x29,0x1E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},
    {0x3E,0x41,0x41,0x41,0x22},{0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},
    {0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},
    {0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},
    {0x3E,0x41,0x41,0x41,0x3E},{0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},
    {0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},
    {0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},
};
static void fb_char(int x,int y,char c,uint16_t col,int sc=1) {
    int idx=-1;
    if(c>='A'&&c<='Z') idx=c-'A'+10;
    else if(c>='a'&&c<='z') idx=c-'a'+10;
    else if(c>='0'&&c<='9') idx=c-'0';
    if(idx<0) return;
    for(int col2=0;col2<5;col2++) {
        uint8_t b=F57[idx][col2];
        for(int row=0;row<7;row++) if(b&(1<<row))
            fb_rect(x+col2*sc,y+row*sc,sc,sc,col);
    }
}
static int fb_tw(const char* s,int sc=1) {
    int w=0; for(int i=0;s[i];i++) w+=(s[i]==' '?4:6)*sc; return w;
}
static void fb_text(int x,int y,const char* str,uint16_t col,int sc=1) {
    for(int i=0;str[i];i++) {
        if(str[i]==' '){x+=4*sc;continue;}
        fb_char(x,y,str[i],col,sc); x+=6*sc;
    }
}
static void fb_text_c(int y,const char* s,uint16_t c,int sc=1) {
    fb_text((LCD_W-fb_tw(s,sc))/2,y,s,c,sc);
}

// ── Animationen ───────────────────────────────────────────────────────────────
static uint16_t mc() {
    if(s.cr||s.cg||s.cb) return rgb(s.cr,s.cg,s.cb);
    if(!strcmp(s.state,"listening")) return rgb(30,180,80);
    if(!strcmp(s.state,"thinking"))  return rgb(220,180,30);
    if(!strcmp(s.state,"speaking"))  return rgb(30,180,200);
    if(!strcmp(s.state,"private"))   return rgb(200,40,40);
    if(!strcmp(s.state,"ota"))       return rgb(220,120,20);
    if(!strcmp(s.state,"error"))     return rgb(200,30,30);
    if(!strcmp(s.state,"enroll"))    return rgb(140,60,220);
    return rgb(40,80,200);
}

static void render() {
    if(!s_fb||!s_panel||!s_mutex) return;
    if(xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return;
    uint16_t bg  = rgb(10,12,20);
    uint16_t dim = rgb(70,75,90);
    uint16_t mcv = mc();
    int cx = LCD_W/2;

    fb_fill(bg);
    fb_rect(0,0,LCD_W,4,mcv);

    // Ring
    bool anim = !strcmp(s.state,"listening")||!strcmp(s.state,"speaking");
    bool spin = !strcmp(s.state,"thinking");
    if(spin) {
        uint32_t t=millis()/70;
        for(int i=0;i<8;i++){
            float a=(i*45.f+(t%24)*15.f)*3.14159f/180.f;
            int x=cx+(int)(32*cosf(a)); int y=140+(int)(32*sinf(a));
            uint8_t b=(i==(int)(t%8))?230:50;
            fb_rect(x-3,y-3,6,6,rgb(b,b,b));
        }
    } else {
        int r=42;
        if(anim){float t=(float)(millis()%1200)/1200.f; r+=7*sinf(t*2*3.14159f);}
        fb_circle(cx,140,r,mcv,3);
    }

    // Icon (3x)
    char icon[8]; strncpy(icon,s.icon,7);
    for(int i=0;icon[i];i++) icon[i]=toupper(icon[i]);
    fb_text((cx-fb_tw(icon,3)/2),130,icon,mcv,3);

    // Label (2x)
    char lbl[24]; strncpy(lbl,s.label,23);
    for(int i=0;lbl[i];i++) lbl[i]=toupper(lbl[i]);
    fb_text_c(207,lbl,mcv,2);

    // Progress
    if(s.has_progress){
        char pt[40]; snprintf(pt,sizeof(pt),"%s %d/%d",s.prog_label,s.prog_value,s.prog_total);
        fb_text_c(230,pt,dim,1);
        int bx=40,bw=LCD_W-80;
        fb_rect(bx,238,bw,8,dim);
        if(s.prog_total>0&&s.prog_value>0)
            fb_rect(bx+1,239,(s.prog_value*(bw-2))/s.prog_total,6,mcv);
    }

    fb_hline(20,258,LCD_W-40,dim);

    if(s.person[0]){
        char pl[48]; snprintf(pl,sizeof(pl),"%s - " CADEN_ROOM_ID,s.person);
        fb_text_c(266,pl,rgb(210,215,225),1);
    }

    if(s.has_weather){
        fb_hline(20,280,LCD_W-40,dim);
        char wb[40]; snprintf(wb,sizeof(wb),"%dC %s",s.weather_temp,s.weather_desc);
        fb_text_c(288,wb,dim,1);
    }

    // Alert overlay
    if(s.has_alert){
        uint16_t ac=(!strcmp(s.alert_level,"alert"))?rgb(220,40,40):
                    (!strcmp(s.alert_level,"warning"))?rgb(220,160,20):rgb(40,140,220);
        fb_rect(18,78,LCD_W-36,72,rgb(20,22,32));
        fb_hline(18,78,LCD_W-36,ac); fb_hline(18,149,LCD_W-36,ac);
        fb_rect(18,78,1,72,ac); fb_rect(LCD_W-19,78,1,72,ac);
        char lvl[12]; strncpy(lvl,s.alert_level,11);
        for(int i=0;lvl[i];i++) lvl[i]=toupper(lvl[i]);
        fb_text_c(93,lvl,ac,2);
        fb_text_c(118,s.alert_text,rgb(210,215,225),1);
    }

    // Push via DMA — FB liegt in PSRAM, DMA braucht DRAM-Puffer
    for(int y=0;y<LCD_H;y+=LCD_BUF_LINES) {
        int h = min(LCD_BUF_LINES, LCD_H-y);
        memcpy(s_dma, s_fb + y*LCD_W, h * LCD_W * 2);
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_W, y+h, s_dma);
    }
    xSemaphoreGive(s_mutex);
}

// ── Init ─────────────────────────────────────────────────────────────────────
void display_init() {
    Serial.println("[Display] Init ST77916 QSPI...");

    // TCA9554
    tca_init();

    // Touch Reset
    tca_pin(EXIO_TP_RST, false); delay(10);
    tca_pin(EXIO_TP_RST, true);  delay(50);

    // LCD Reset
    tca_pin(EXIO_LCD_RST, false); delay(20);
    tca_pin(EXIO_LCD_RST, true);  delay(100);

    // TE Pin
    pinMode(LCD_TE, OUTPUT);

    // Backlight PWM (Channel 1 — Channel 0 nutzt Audio)
    ledcSetup(1, 5000, 10);
    ledcAttachPin(LCD_BL, 1);
    ledcWrite(1, 512);  // 50%

    // Touch INT
    if(TP_INT_PIN>=0) pinMode(TP_INT_PIN, INPUT);

    // SPI Bus
    spi_bus_config_t host = {};
    host.data0_io_num  = LCD_D0;
    host.data1_io_num  = LCD_D1;
    host.sclk_io_num   = LCD_SCK;
    host.data2_io_num  = LCD_D2;
    host.data3_io_num  = LCD_D3;
    host.data4_io_num  = -1; host.data5_io_num=-1;
    host.data6_io_num  = -1; host.data7_io_num=-1;
    host.max_transfer_sz = LCD_W * LCD_BUF_LINES * 2 + 64;
    host.flags = SPICOMMON_BUSFLAG_MASTER;
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &host, SPI_DMA_CH_AUTO));

    // Panel IO (niedrige CLK für Init, dann erhöhen)
    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num       = LCD_CS;
    io_cfg.dc_gpio_num       = -1;
    io_cfg.spi_mode          = 0;
    io_cfg.pclk_hz           = 5 * 1000 * 1000;  // Niedrig für Init-Phase
    io_cfg.trans_queue_depth = LCD_QUEUE_DEPTH;
    io_cfg.lcd_cmd_bits      = LCD_CMD_BITS;
    io_cfg.lcd_param_bits    = LCD_PARAM_BITS;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &s_io));

    // Vendor Config mit QSPI-Flag + Init-Sequenz
    st77916_vendor_config_t vendor = {};
    vendor.init_cmds      = s_vendor_init;
    vendor.init_cmds_size = sizeof(s_vendor_init) / sizeof(s_vendor_init[0]);
    vendor.flags.use_qspi_interface = 1;

    // Panel Config
    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num  = -1;
    panel_cfg.bits_per_pixel  = 16;
    panel_cfg.vendor_config   = &vendor;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(s_io, &panel_cfg, &s_panel));

    esp_lcd_panel_reset(s_panel);
    delay(50);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_disp_on_off(s_panel, true);

    // Framebuffer in PSRAM (253KB — passt nicht in DRAM)
    s_fb = (uint16_t*)heap_caps_malloc(LCD_W * LCD_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!s_fb){ Serial.println("[Display] ERROR: PSRAM alloc failed"); return; }

    // DMA-Puffer in DRAM (10 Zeilen, 7.2KB) — DMA kann nicht direkt aus PSRAM lesen
    s_dma = (uint16_t*)heap_caps_malloc(LCD_W * LCD_BUF_LINES * 2, MALLOC_CAP_DMA);
    if(!s_dma){ Serial.println("[Display] ERROR: DMA buf alloc failed"); return; }

    Serial.printf("[Display] FB: %dKB PSRAM, DMA buf: %dKB DRAM\n",
        LCD_W*LCD_H*2/1024, LCD_W*LCD_BUF_LINES*2/1024);
    s_mutex = xSemaphoreCreateMutex();

    // Backlight voll
    ledcWrite(1, 900);

    // Splash
    uint16_t bg = rgb(10,12,20);
    fb_fill(bg);
    fb_text_c(152,"CADEN",rgb(40,80,200),3);
    fb_text_c(182,CADEN_ROOM_ID,rgb(70,75,90),2);
    for(int y=0;y<LCD_H;y+=LCD_BUF_LINES) {
        int h=min(LCD_BUF_LINES,LCD_H-y);
        memcpy(s_dma, s_fb+y*LCD_W, h*LCD_W*2);
        esp_lcd_panel_draw_bitmap(s_panel,0,y,LCD_W,y+h,s_dma);
    }

    delay(1500);
    Serial.println("[Display] ST77916 360x360 ready");
}

// ── MQTT ─────────────────────────────────────────────────────────────────────
void display_handle_mqtt(const char* payload, size_t len) {
    JsonDocument doc;
    if(deserializeJson(doc,payload,len)!=DeserializationError::Ok) return;

    if(doc["state"].is<const char*>())  strncpy(s.state,doc["state"].as<const char*>(),sizeof(s.state)-1);
    if(doc["icon"].is<const char*>())   strncpy(s.icon, doc["icon"].as<const char*>(), sizeof(s.icon)-1);
    if(doc["label"].is<const char*>())  strncpy(s.label,doc["label"].as<const char*>(),sizeof(s.label)-1);

    if(doc["color"].is<JsonArray>()){s.cr=doc["color"][0]|0;s.cg=doc["color"][1]|0;s.cb=doc["color"][2]|0;}
    else{s.cr=s.cg=s.cb=0;}

    if(doc["person"].is<const char*>()) strncpy(s.person,doc["person"].as<const char*>(),sizeof(s.person)-1);
    else if(doc["person"].isNull()) s.person[0]='\0';

    if(doc["progress"].is<JsonObject>()){
        s.has_progress=true;
        s.prog_value=doc["progress"]["value"]|0;
        s.prog_total=doc["progress"]["total"]|8;
        if(doc["progress"]["label"].is<const char*>())
            strncpy(s.prog_label,doc["progress"]["label"].as<const char*>(),sizeof(s.prog_label)-1);
    } else if(doc["progress"].isNull()) s.has_progress=false;

    if(doc["weather"].is<JsonObject>()){
        s.has_weather=true;
        s.weather_temp=doc["weather"]["temp"]|0;
        if(doc["weather"]["desc"].is<const char*>())
            strncpy(s.weather_desc,doc["weather"]["desc"].as<const char*>(),sizeof(s.weather_desc)-1);
    }

    if(doc["alert"].is<JsonObject>()){
        s.has_alert=true;
        if(doc["alert"]["text"].is<const char*>())  strncpy(s.alert_text, doc["alert"]["text"].as<const char*>(), sizeof(s.alert_text)-1);
        if(doc["alert"]["level"].is<const char*>()) strncpy(s.alert_level,doc["alert"]["level"].as<const char*>(),sizeof(s.alert_level)-1);
        int ttl=doc["alert"]["ttl"]|0;
        s.alert_until=ttl>0?millis()+ttl:0;
    } else if(doc["alert"].isNull()) s.has_alert=false;

    if(doc["dim"].is<int>()) ledcWrite(1,(int)(doc["dim"].as<int>()*4));  // 0-255 → 0-1023

    render();
}

// ── Tick ─────────────────────────────────────────────────────────────────────
void display_tick() {
    if(!s_fb||!s_panel) return;

    if(s.has_alert&&s.alert_until>0&&millis()>s.alert_until){
        s.has_alert=false; render();
    }

    bool anim=(!strcmp(s.state,"listening")||!strcmp(s.state,"thinking")||!strcmp(s.state,"speaking"));
    if(anim&&millis()-s_last_anim>=50){
        s_last_anim=millis(); render();
    }

    if(millis()-s_last_touch>300&&touch_poll()){
        s_last_touch=millis();
        Serial.println("[Touch] tap");
    }
}

#endif // CADEN_HAS_DISPLAY
