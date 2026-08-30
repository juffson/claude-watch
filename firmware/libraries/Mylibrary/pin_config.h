#pragma once
// Pin map for the Waveshare ESP32-S3-Touch-AMOLED-1.75C (32MB flash revision).
// Values follow the 1.75C repo (examples/arduino/libraries/Mylibrary) and its ESP-IDF BSP;
// macro names are kept compatible with the original 1.75 header used by the examples.
// Differences from the 1.75 (non-C) board: LCD_RESET 39->1, TP_RESET 40->2, I2S MCLK 42->16,
// and there is no SD card / RTC / IO expander.

#define XPOWERS_CHIP_AXP2101

// AMOLED (CO5300, QSPI)
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK  38
#define LCD_CS    12
#define LCD_RESET 1
#define LCD_WIDTH  466
#define LCD_HEIGHT 466

// I2C (touch CST9217 0x5A, AXP2101 0x34, QMI8658 0x6B, ES8311 0x18, ES7210 0x40)
#define IIC_SDA  15
#define IIC_SCL  14
#define TP_INT   11
#define TP_RESET 2

// I2S audio (ES8311 DAC + ES7210 ADC share the clocks)
#define MCLKPIN 16   // master clock
#define BCLKPIN 9    // bit clock
#define WSPIN   45   // word select / LRCK
#define DIPIN   8    // ESP32 -> ES8311 DSDIN (playback data out of the MCU)
#define DOPIN   10   // ES7210 -> ESP32 (microphone data into the MCU)
#define PA      46   // NS4150B amplifier enable (active high)
