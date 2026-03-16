#pragma once
#include <stdint.h>
#include <stddef.h>

// Minimaler QSPI LCD-Treiber für ST77916
// Nutzt spi_device direkt mit SPI_TRANS_MODE_QIO

bool  qspi_lcd_init();
void  qspi_lcd_cmd(uint8_t cmd, const uint8_t* data, size_t len);
void  qspi_lcd_send_pixels(const uint16_t* data, size_t len_bytes);
void  qspi_lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
