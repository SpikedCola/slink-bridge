#pragma once

// Board-specific pins and RMT setup. The board is chosen by the PlatformIO
// environment (see platformio.ini), which defines one of the BOARD_* macros.
//
// Circuit (same for every board): TX_PIN -> 1k -> NPN base (47k base to GND),
// collector to the CONTROL A1 tip, emitter to GND/sleeve. RX_PIN -> Schottky
// anode, cathode to the tip, 10k from RX_PIN to 3V3.

#include <driver/rmt.h>

#if defined(BOARD_S2_MINI)
// LOLIN/Wemos ESP32-S2 Mini v1.0.0.
// Working on the STR-DB930 since 2026-10-27 with TX = GPIO16, RX = GPIO18.
static const int TX_PIN = 16;
static const int RX_PIN = 18;
static const rmt_channel_t TX_RMT = RMT_CHANNEL_0;
static const rmt_channel_t RX_RMT = RMT_CHANNEL_1;
// 64 items per block; the S2 can't refill RX memory mid-message, so reserve
// enough for the longest reply (sync + 18 bytes = 145 items).
static const uint8_t RX_MEM_BLOCKS = 3;

#elif defined(BOARD_C3_SUPERMINI)
// ESP32-C3 SuperMini. Not yet tested on hardware.
// TX = GPIO3, RX = GPIO4: both sit beside 3V3 and GND on the same edge
// (swapped from 4/3 to suit the PCB routing; any GPIO works with the RMT).
// Avoid GPIO2/8/9 (boot strapping; 8 is also the LED, 9 the BOOT button),
// 18/19 (USB) and 20/21 (UART).
static const int TX_PIN = 3;
static const int RX_PIN = 4;
// C3: channels 0-1 transmit only, 2-3 receive only.
static const rmt_channel_t TX_RMT = RMT_CHANNEL_0;
static const rmt_channel_t RX_RMT = RMT_CHANNEL_2;
// 48 items per block; RX ping-pong refills, so two blocks are plenty.
static const uint8_t RX_MEM_BLOCKS = 2;
// Many SuperMini boards fail to join WiFi at full power (poor antenna match).
#define WIFI_TX_POWER WIFI_POWER_8_5dBm

#else
#error "No board selected: build with a PlatformIO environment from platformio.ini"
#endif
