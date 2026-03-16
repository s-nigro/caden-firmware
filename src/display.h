#pragma once
/**
 * CADEN Display Driver — Waveshare ESP32-S3-Touch-LCD-1.85C-BOX
 *
 * Controller:  ST77916, QSPI 4-bit
 * Resolution:  360 x 360
 * Touch:       CST816, I2C (GPIO10/11)
 * GPIO Expander: TCA9554 (I2C 0x20) — LCD_RST=EXIO2, TP_RST=EXIO1
 *
 * Pin-Mapping (aus Waveshare Wiki):
 *   LCD_SDA0  → GPIO46   (QSPI D0)
 *   LCD_SDA1  → GPIO45   (QSPI D1)
 *   LCD_SDA2  → GPIO42   (QSPI D2)
 *   LCD_SDA3  → GPIO41   (QSPI D3)
 *   LCD_SCK   → GPIO40
 *   LCD_CS    → GPIO21
 *   LCD_TE    → GPIO18
 *   LCD_BL    → GPIO5    (PWM Backlight)
 *   LCD_RST   → TCA9554 EXIO2
 *   TP_SDA    → GPIO11   (shared I2C)
 *   TP_SCL    → GPIO10   (shared I2C)
 *   TP_INT    → GPIO4
 *   TP_RST    → TCA9554 EXIO1
 */

#if defined(CADEN_HAS_DISPLAY) && (CADEN_HAS_DISPLAY == 1)

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <ArduinoJson.h>

// ── Pins ─────────────────────────────────────────────────────────────────────
#define DISP_QSPI_D0    46
#define DISP_QSPI_D1    45
#define DISP_QSPI_D2    42
#define DISP_QSPI_D3    41
#define DISP_SCK        40
#define DISP_CS         21
#define DISP_TE         18
#define DISP_BL          5
#define DISP_W         360
#define DISP_H         360

// Touch CST816
#define TOUCH_I2C_ADDR  0x15
#define TOUCH_INT_PIN    4

// TCA9554 GPIO Expander (I2C 0x20)
#define TCA9554_ADDR    0x20
#define EXIO_LCD_RST     2   // EXIO2
#define EXIO_TP_RST      1   // EXIO1

void display_init();
void display_handle_mqtt(const char* payload, size_t len);
void display_tick();

#endif
