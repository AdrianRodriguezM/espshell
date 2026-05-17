/* SPDX-License-Identifier: GPL-3.0-or-later
 * targets.h — per-chip capability matrix.
 *
 * Single source of truth for which peripherals/features the current build
 * target exposes. Use the ESPSHELL_HAS_* macros in #if conditions instead of
 * branching on CONFIG_IDF_TARGET_* directly — that keeps cmd_builtin.c and
 * the rest of the core readable.
 */
#ifndef ESPSHELL_TARGETS_H
#define ESPSHELL_TARGETS_H

#include "sdkconfig.h"

#if defined(CONFIG_IDF_TARGET_ESP32)
#  define ESPSHELL_TARGET_NAME       "esp32"
#  define ESPSHELL_HAS_DUAL_CORE     1
#  define ESPSHELL_HAS_BT_CLASSIC    1
#  define ESPSHELL_HAS_BLE           1
#  define ESPSHELL_HAS_DAC           1
#  define ESPSHELL_HAS_HALL_SENSOR   1
#  define ESPSHELL_HAS_TOUCH         1
#  define ESPSHELL_HAS_USB_OTG       0
#  define ESPSHELL_HAS_ETHERNET      1
#  define ESPSHELL_HAS_THREAD        0
#  define ESPSHELL_CHIP_TEMP_OK      0   /* internal temp sensor unreliable on classic */
#  define ESPSHELL_SPI_NUM_HOSTS     2   /* SPI2_HOST + SPI3_HOST */
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
#  define ESPSHELL_TARGET_NAME       "esp32s3"
#  define ESPSHELL_HAS_DUAL_CORE     1
#  define ESPSHELL_HAS_BT_CLASSIC    0
#  define ESPSHELL_HAS_BLE           1
#  define ESPSHELL_HAS_DAC           0
#  define ESPSHELL_HAS_HALL_SENSOR   0
#  define ESPSHELL_HAS_TOUCH         1
#  define ESPSHELL_HAS_USB_OTG       1
#  define ESPSHELL_HAS_ETHERNET      0
#  define ESPSHELL_HAS_THREAD        0
#  define ESPSHELL_CHIP_TEMP_OK      1
#  define ESPSHELL_SPI_NUM_HOSTS     2   /* SPI2_HOST + SPI3_HOST */
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
#  define ESPSHELL_TARGET_NAME       "esp32c3"
#  define ESPSHELL_HAS_DUAL_CORE     0
#  define ESPSHELL_HAS_BT_CLASSIC    0
#  define ESPSHELL_HAS_BLE           1
#  define ESPSHELL_HAS_DAC           0
#  define ESPSHELL_HAS_HALL_SENSOR   0
#  define ESPSHELL_HAS_TOUCH         0
#  define ESPSHELL_HAS_USB_OTG       0
#  define ESPSHELL_HAS_ETHERNET      0
#  define ESPSHELL_HAS_THREAD        0
#  define ESPSHELL_CHIP_TEMP_OK      1
#  define ESPSHELL_SPI_NUM_HOSTS     1   /* SPI2_HOST only */
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
#  define ESPSHELL_TARGET_NAME       "esp32c6"
#  define ESPSHELL_HAS_DUAL_CORE     0
#  define ESPSHELL_HAS_BT_CLASSIC    0
#  define ESPSHELL_HAS_BLE           1
#  define ESPSHELL_HAS_DAC           0
#  define ESPSHELL_HAS_HALL_SENSOR   0
#  define ESPSHELL_HAS_TOUCH         0
#  define ESPSHELL_HAS_USB_OTG       0
#  define ESPSHELL_HAS_ETHERNET      0
#  define ESPSHELL_HAS_THREAD        1
#  define ESPSHELL_CHIP_TEMP_OK      1
#  define ESPSHELL_SPI_NUM_HOSTS     1   /* SPI2_HOST only */
#else
#  error "Unsupported IDF target. Add it to components/core/include/targets.h."
#endif

#endif /* ESPSHELL_TARGETS_H */
