/**
 * CADEN Display Driver — Waveshare ESP32-S3-Touch-LCD-1.85C-BOX
 * Controller: ST77916, QSPI 4-wire, 360x360, RGB565
 * Treiber:    qspi_lcd (direkt spi_device mit SPI_TRANS_MODE_QIO)
 */

#include "display.h"

#if defined(CADEN_HAS_DISPLAY) && (CADEN_HAS_DISPLAY == 1)

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <ArduinoJson.h>
#include "qspi_lcd.h"

// ── Pins ─────────────────────────────────────────────────────────────────────
#define LCD_BL      5
#define LCD_W     360
#define LCD_H     360
#define LCD_BUF_LINES  8

#define TCA_ADDR      0x20
#define EXIO_LCD_RST   2
#define EXIO_TP_RST    1
#define TP_ADDR       0x15
#define TP_INT_PIN     4

// ── ST77916 Init-Sequenz ──────────────────────────────────────────────────────
typedef struct { uint8_t cmd; const uint8_t* data; uint8_t len; uint32_t delay_ms; } lcd_cmd_t;

static const lcd_cmd_t s_init[] = {
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
    {0x21, NULL, 0, 0},          // Display Inversion ON
    {0x3A, (uint8_t[]){0x55}, 1, 0}, // 16-bit RGB565
    {0x36, (uint8_t[]){0x00}, 1, 0}, // MADCTL normal
    {0x11, NULL, 0, 120},        // Sleep Out + 120ms
    {0x29, NULL, 0, 20},         // Display ON
};

// ── State ─────────────────────────────────────────────────────────────────────
static uint16_t*         s_fb    = NULL;
static uint16_t*         s_dma   = NULL;
static SemaphoreHandle_t s_mutex = NULL;
static bool              s_ready = false;

struct RenderState {
    char    state[16]  = "ready";
    char    icon[8]    = "MIC";
    char    label[24]  = "BEREIT";
    uint8_t cr=40,cg=80,cb=200;
    char    person[32] = "";
    bool    has_progress=false; int prog_v=0,prog_t=8; char prog_lbl[24]="";
    bool    has_weather=false;  int wx_temp=0;          char wx_desc[32]="";
    bool    has_alert=false;    char al_text[64]="";    char al_lvl[12]="info";
    uint32_t al_until=0;
} s;

static uint32_t s_last_anim=0, s_last_touch=0;

// ── TCA9554 ────────────────────────────────────────────────────────────────────
static uint8_t tca_out=0xFF;
static void tca_write(uint8_t v){Wire.beginTransmission(TCA_ADDR);Wire.write(0x01);Wire.write(v);Wire.endTransmission();tca_out=v;}
static void tca_pin(uint8_t p,bool h){if(h)tca_out|=(1<<p);else tca_out&=~(1<<p);tca_write(tca_out);}
static void tca_init(){Wire.beginTransmission(TCA_ADDR);Wire.write(0x03);Wire.write(0x00);Wire.endTransmission();tca_write(0xFF);}

// ── Touch ─────────────────────────────────────────────────────────────────────
static bool touch_poll(){
    Wire.beginTransmission(TP_ADDR);Wire.write(0x02);
    if(Wire.endTransmission(false))return false;
    Wire.requestFrom((uint8_t)TP_ADDR,(uint8_t)1);
    if(!Wire.available())return false;
    return Wire.read()>0;
}

// ── RGB565 (byte-swapped für SPI) ─────────────────────────────────────────────
static inline uint16_t rgb(uint8_t r,uint8_t g,uint8_t b){
    uint16_t v=((r&0xF8)<<8)|((g&0xFC)<<3)|(b>>3);
    return (v<<8)|(v>>8);
}

// ── Framebuffer Ops ───────────────────────────────────────────────────────────
static void fb_fill(uint16_t c){for(int i=0;i<LCD_W*LCD_H;i++)s_fb[i]=c;}
static void fb_hline(int x,int y,int w,uint16_t c){for(int i=0;i<w;i++)if(x+i<LCD_W&&y>=0&&y<LCD_H)s_fb[y*LCD_W+x+i]=c;}
static void fb_rect(int x,int y,int w,int h,uint16_t c){for(int dy=0;dy<h;dy++)fb_hline(x,y+dy,w,c);}
static void fb_circle(int cx,int cy,int r,uint16_t c,int t=2){
    for(int dt=0;dt<t;dt++)for(int d=0;d<360;d++){
        float a=d*3.14159f/180.f;
        int x=cx+(int)((r+dt)*cosf(a)),y=cy+(int)((r+dt)*sinf(a));
        if(x>=0&&x<LCD_W&&y>=0&&y<LCD_H)s_fb[y*LCD_W+x]=c;
    }
}

// ── Font 5x7 (A-Z, 0-9) ──────────────────────────────────────────────────────
static const uint8_t F57[36][5]={
    // 0-9
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},
    {0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},{0x36,0x49,0x49,0x49,0x36},
    {0x06,0x49,0x49,0x29,0x1E},
    // A-Z
    {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},
};
static int fb_char_idx(char c){
    if(c>='0'&&c<='9')return c-'0';
    if(c>='A'&&c<='Z')return c-'A'+10;
    if(c>='a'&&c<='z')return c-'a'+10;
    return -1;
}
static void fb_char(int x,int y,char c,uint16_t col,int sc=1){
    int idx=fb_char_idx(c); if(idx<0)return;
    for(int col2=0;col2<5;col2++){uint8_t b=F57[idx][col2];
        for(int row=0;row<7;row++)if(b&(1<<row))fb_rect(x+col2*sc,y+row*sc,sc,sc,col);}
}
static int fb_tw(const char* s,int sc=1){int w=0;for(int i=0;s[i];i++)w+=(s[i]==' '?4:6)*sc;return w;}
static void fb_text(int x,int y,const char* str,uint16_t col,int sc=1){
    for(int i=0;str[i];i++){if(str[i]==' '){x+=4*sc;continue;}fb_char(x,y,str[i],col,sc);x+=6*sc;}
}
static void fb_tc(int y,const char* s,uint16_t c,int sc=1){fb_text((LCD_W-fb_tw(s,sc))/2,y,s,c,sc);}

// ── Render ────────────────────────────────────────────────────────────────────
static uint16_t mc(){
    if(s.cr||s.cg||s.cb)return rgb(s.cr,s.cg,s.cb);
    if(!strcmp(s.state,"listening"))return rgb(30,180,80);
    if(!strcmp(s.state,"thinking")) return rgb(220,180,30);
    if(!strcmp(s.state,"speaking")) return rgb(30,180,200);
    if(!strcmp(s.state,"private"))  return rgb(200,40,40);
    if(!strcmp(s.state,"error"))    return rgb(200,30,30);
    if(!strcmp(s.state,"ota"))      return rgb(220,120,20);
    return rgb(40,80,200);
}

static void push_fb() {
    for(int y=0;y<LCD_H;y+=LCD_BUF_LINES){
        int h=min(LCD_BUF_LINES,LCD_H-y);
        memcpy(s_dma,s_fb+y*LCD_W,h*LCD_W*2);
        qspi_lcd_set_window(0,y,LCD_W-1,y+h-1);
        qspi_lcd_send_pixels(s_dma,h*LCD_W*2);
    }
}

static void render(){
    if(!s_ready||!s_mutex)return;
    if(xSemaphoreTake(s_mutex,pdMS_TO_TICKS(200))!=pdTRUE)return;

    uint16_t bg=rgb(10,12,20),dim=rgb(70,75,90),mcv=mc();
    int cx=LCD_W/2;
    fb_fill(bg);
    fb_rect(0,0,LCD_W,4,mcv);

    // Icon-Ring mit Puls/Spinner
    bool anim=!strcmp(s.state,"listening")||!strcmp(s.state,"speaking");
    bool spin=!strcmp(s.state,"thinking");
    if(spin){
        uint32_t t=millis()/70;
        for(int i=0;i<8;i++){float a=(i*45.f+(t%24)*15.f)*3.14159f/180.f;
            int x=cx+(int)(32*cosf(a)),y=140+(int)(32*sinf(a));
            uint8_t b=(i==(int)(t%8))?230:50;fb_rect(x-3,y-3,6,6,rgb(b,b,b));}
    }else{
        int r=42;if(anim){float t=(float)(millis()%1200)/1200.f;r+=(int)(7*sinf(t*2*3.14159f));}
        fb_circle(cx,140,r,mcv,3);
    }

    // Icon (3x Schrift)
    char icon[8];strncpy(icon,s.icon,7);for(int i=0;icon[i];i++)icon[i]=toupper(icon[i]);
    fb_text(cx-fb_tw(icon,3)/2,130,icon,mcv,3);

    // Label (2x)
    char lbl[24];strncpy(lbl,s.label,23);for(int i=0;lbl[i];i++)lbl[i]=toupper(lbl[i]);
    fb_tc(207,lbl,mcv,2);

    // Progressbar
    if(s.has_progress){
        char pt[40];snprintf(pt,sizeof(pt),"%s %d/%d",s.prog_lbl,s.prog_v,s.prog_t);
        fb_tc(230,pt,dim,1);
        int bx=40,bw=LCD_W-80;fb_rect(bx,238,bw,8,dim);
        if(s.prog_t>0&&s.prog_v>0)fb_rect(bx+1,239,(s.prog_v*(bw-2))/s.prog_t,6,mcv);
    }
    fb_hline(20,258,LCD_W-40,dim);
    if(s.person[0]){char pl[48];snprintf(pl,sizeof(pl),"%s - " CADEN_ROOM_ID,s.person);fb_tc(266,pl,rgb(210,215,225),1);}
    if(s.has_weather){fb_hline(20,280,LCD_W-40,dim);char wb[40];snprintf(wb,sizeof(wb),"%dC %s",s.wx_temp,s.wx_desc);fb_tc(288,wb,dim,1);}
    if(s.has_alert){
        uint16_t ac=(!strcmp(s.al_lvl,"alert"))?rgb(220,40,40):(!strcmp(s.al_lvl,"warning"))?rgb(220,160,20):rgb(40,140,220);
        fb_rect(18,78,LCD_W-36,72,rgb(20,22,32));
        fb_hline(18,78,LCD_W-36,ac);fb_hline(18,149,LCD_W-36,ac);
        fb_rect(18,78,1,72,ac);fb_rect(LCD_W-19,78,1,72,ac);
        char lvl[12];strncpy(lvl,s.al_lvl,11);for(int i=0;lvl[i];i++)lvl[i]=toupper(lvl[i]);
        fb_tc(93,lvl,ac,2);fb_tc(118,s.al_text,rgb(210,215,225),1);
    }

    push_fb();
    xSemaphoreGive(s_mutex);
}

// ── Init ─────────────────────────────────────────────────────────────────────
void display_init(){
    Serial.println("[Display] Init ST77916 QSPI...");

    tca_init();
    tca_pin(EXIO_TP_RST,false);delay(10);
    tca_pin(EXIO_TP_RST,true);delay(50);
    tca_pin(EXIO_LCD_RST,false);delay(20);
    tca_pin(EXIO_LCD_RST,true);delay(120);

    if(TP_INT_PIN>=0)pinMode(TP_INT_PIN,INPUT);
    ledcSetup(1,5000,10);ledcAttachPin(LCD_BL,1);ledcWrite(1,512);

    if(!qspi_lcd_init()){Serial.println("[Display] QSPI init FAILED");return;}

    // Init-Sequenz
    for(int i=0;i<(int)(sizeof(s_init)/sizeof(s_init[0]));i++){
        qspi_lcd_cmd(s_init[i].cmd,s_init[i].data,s_init[i].len);
        if(s_init[i].delay_ms)delay(s_init[i].delay_ms);
    }

    // Framebuffer in PSRAM
    s_fb=(uint16_t*)heap_caps_malloc(LCD_W*LCD_H*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!s_fb){Serial.println("[Display] PSRAM alloc failed");return;}

    // DMA-Puffer in DRAM
    s_dma=(uint16_t*)heap_caps_malloc(LCD_W*LCD_BUF_LINES*2,MALLOC_CAP_DMA);
    if(!s_dma){Serial.println("[Display] DMA buf failed");return;}

    s_mutex=xSemaphoreCreateMutex();
    s_ready=true;
    ledcWrite(1,900);

    Serial.printf("[Display] Ready — FB:%dKB PSRAM, DMA:%dKB DRAM\n",
        LCD_W*LCD_H*2/1024,LCD_W*LCD_BUF_LINES*2/1024);

    // Splash
    fb_fill(rgb(10,12,20));
    fb_tc(152,"CADEN",rgb(40,80,200),3);
    fb_tc(182,CADEN_ROOM_ID,rgb(70,75,90),2);
    push_fb();
    delay(1500);
}

// ── MQTT ─────────────────────────────────────────────────────────────────────
void display_handle_mqtt(const char* payload,size_t len){
    JsonDocument doc;
    if(deserializeJson(doc,payload,len)!=DeserializationError::Ok)return;

    if(doc["state"].is<const char*>())  strncpy(s.state,doc["state"].as<const char*>(),sizeof(s.state)-1);
    if(doc["icon"].is<const char*>())   strncpy(s.icon, doc["icon"].as<const char*>(), sizeof(s.icon)-1);
    if(doc["label"].is<const char*>())  strncpy(s.label,doc["label"].as<const char*>(),sizeof(s.label)-1);
    if(doc["color"].is<JsonArray>()){s.cr=doc["color"][0]|0;s.cg=doc["color"][1]|0;s.cb=doc["color"][2]|0;}
    else{s.cr=s.cg=s.cb=0;}
    if(doc["person"].is<const char*>())strncpy(s.person,doc["person"].as<const char*>(),sizeof(s.person)-1);
    else if(doc["person"].isNull())s.person[0]='\0';
    if(doc["progress"].is<JsonObject>()){s.has_progress=true;s.prog_v=doc["progress"]["value"]|0;s.prog_t=doc["progress"]["total"]|8;if(doc["progress"]["label"].is<const char*>())strncpy(s.prog_lbl,doc["progress"]["label"].as<const char*>(),sizeof(s.prog_lbl)-1);}
    else if(doc["progress"].isNull())s.has_progress=false;
    if(doc["weather"].is<JsonObject>()){s.has_weather=true;s.wx_temp=doc["weather"]["temp"]|0;if(doc["weather"]["desc"].is<const char*>())strncpy(s.wx_desc,doc["weather"]["desc"].as<const char*>(),sizeof(s.wx_desc)-1);}
    if(doc["alert"].is<JsonObject>()){s.has_alert=true;if(doc["alert"]["text"].is<const char*>())strncpy(s.al_text,doc["alert"]["text"].as<const char*>(),sizeof(s.al_text)-1);if(doc["alert"]["level"].is<const char*>())strncpy(s.al_lvl,doc["alert"]["level"].as<const char*>(),sizeof(s.al_lvl)-1);int ttl=doc["alert"]["ttl"]|0;s.al_until=ttl>0?millis()+ttl:0;}
    else if(doc["alert"].isNull())s.has_alert=false;
    if(doc["dim"].is<int>())ledcWrite(1,(int)(doc["dim"].as<int>()*4));

    if(s_ready)render();
}

// ── Tick ─────────────────────────────────────────────────────────────────────
void display_tick(){
    if(!s_ready)return;
    if(s.has_alert&&s.al_until>0&&millis()>s.al_until){s.has_alert=false;render();}
    bool anim=!strcmp(s.state,"listening")||!strcmp(s.state,"thinking")||!strcmp(s.state,"speaking");
    if(anim&&millis()-s_last_anim>=50){s_last_anim=millis();render();}
    if(millis()-s_last_touch>300&&touch_poll()){s_last_touch=millis();Serial.println("[Touch] tap");}
}

#endif
