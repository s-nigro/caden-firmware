/**
 * Minimaler QSPI LCD-Treiber für ST77916
 *
 * Nutzt spi_device direkt mit SPI_TRANS_MODE_QIO.
 * Opcode-Schema (wie ST77916 erwartet):
 *   Write CMD:   [0x02][0x00][CMD][0x00]  + param in 4-wire mode
 *   Write COLOR: [0x32][0x00][CMD][0x00]  + pixel data in 4-wire mode
 */

#include "qspi_lcd.h"
#include <Arduino.h>
#include <string.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define LCD_D0    46
#define LCD_D1    45
#define LCD_D2    42
#define LCD_D3    41
#define LCD_SCK   40
#define LCD_CS    21
#define LCD_CLK   (40 * 1000 * 1000)

#define OPCODE_CMD   0x02
#define OPCODE_COLOR 0x32

static const char* TAG = "qspi_lcd";
static spi_device_handle_t s_spi  = NULL;
static spi_host_device_t   s_host = SPI3_HOST;

// ── Transaktion senden ────────────────────────────────────────────────────────
// cmd_word: 32-bit QSPI Command (MSB first: opcode | 00 | lcd_cmd | 00)
static void spi_send(uint32_t cmd_word, const void* data, size_t len, bool is_color) {
    spi_transaction_ext_t t = {};
    t.base.flags = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;

    if (is_color) {
        // Pixeldaten: 4-Leitungs-Mode für Data-Phase
        t.base.flags  |= SPI_TRANS_MODE_QIO;
    }

    t.base.cmd         = cmd_word;
    t.command_bits     = 32;
    t.address_bits     = 0;
    t.dummy_bits       = 0;

    if (data && len > 0) {
        t.base.tx_buffer = data;
        t.base.length    = len * 8;  // bits
    }

    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, (spi_transaction_t*)&t));
}

// 32-bit Command-Word zusammenbauen
static uint32_t make_cmd_word(uint8_t opcode, uint8_t lcd_cmd) {
    // Format: [opcode(8)] [0x00(8)] [lcd_cmd(8)] [0x00(8)]
    return ((uint32_t)opcode << 24) | ((uint32_t)lcd_cmd << 8);
}

// ── Public API ────────────────────────────────────────────────────────────────
void qspi_lcd_cmd(uint8_t cmd, const uint8_t* data, size_t len) {
    uint32_t cw = make_cmd_word(OPCODE_CMD, cmd);
    spi_send(cw, data, len, false);
}

void qspi_lcd_send_pixels(const uint16_t* data, size_t len_bytes) {
    // RAMWR Command, dann Pixel-Daten in QIO-Mode
    uint32_t cw = make_cmd_word(OPCODE_COLOR, 0x2C);  // RAMWR = 0x2C
    spi_send(cw, data, len_bytes, true);
}

void qspi_lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    uint8_t caset[4] = {(uint8_t)(x0>>8), (uint8_t)x0, (uint8_t)(x1>>8), (uint8_t)x1};
    uint8_t raset[4] = {(uint8_t)(y0>>8), (uint8_t)y0, (uint8_t)(y1>>8), (uint8_t)y1};
    qspi_lcd_cmd(0x2A, caset, 4);  // CASET
    qspi_lcd_cmd(0x2B, raset, 4);  // RASET
}

bool qspi_lcd_init() {
    // SPI Bus
    spi_bus_config_t bus = {};
    bus.data0_io_num   = LCD_D0;
    bus.data1_io_num   = LCD_D1;
    bus.sclk_io_num    = LCD_SCK;
    bus.data2_io_num   = LCD_D2;
    bus.data3_io_num   = LCD_D3;
    bus.data4_io_num   = -1;
    bus.data5_io_num   = -1;
    bus.data6_io_num   = -1;
    bus.data7_io_num   = -1;
    bus.max_transfer_sz = 360 * 10 * 2 + 64;
    bus.flags = SPICOMMON_BUSFLAG_MASTER;

    s_host = SPI3_HOST;
    esp_err_t ret = spi_bus_initialize(s_host, &bus, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI3 failed (%d), trying SPI2", ret);
        s_host = SPI2_HOST;
        ret = spi_bus_initialize(s_host, &bus, SPI_DMA_CH_AUTO);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SPI2 also failed (%d)", ret);
            return false;
        }
    }
    ESP_LOGI(TAG, "SPI bus %d initialized", s_host);

    // SPI Device
    spi_device_interface_config_t dev = {};
    dev.mode           = 0;
    dev.clock_speed_hz = LCD_CLK;
    dev.spics_io_num   = LCD_CS;
    dev.queue_size     = 7;
    dev.flags          = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_DUMMY;

    if (spi_bus_add_device(s_host, &dev, &s_spi) != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed");
        return false;
    }

    ESP_LOGI(TAG, "QSPI bus ready @ %dMHz", LCD_CLK/1000000);
    return true;
}
