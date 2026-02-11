/*
 * SPDX-FileCopyrightText: 2022-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#pragma once

/* Ints are used here to be able to include the file in assembly as well */
#define EXAMPLE_ADC_CHANNEL     0 // ADC_CHANNEL_0, GPIO36 on ESP32, GPIO7 on ESP32-S3
#define EXAMPLE_ADC_UNIT        0 // ADC_UNIT_1
#define EXAMPLE_ADC_ATTEN       3 // ADC_ATTEN_DB_12
#define EXAMPLE_ADC_WIDTH       12 // ADC_BITWIDTH_DEFAULT


#define EXAMPLE_ADC_LOW_TRESHOLD    1960    // 3.4V 1920:3.42V, 1900:3.38V
                                            // Power monitor 3.7V: 2125
//                                          // 3.6v: 2078
//                                          // 3.5v: 2003
//                                          // 3.45v: 1981
//                                          // 3.4v: 1953
//
#define EXAMPLE_ADC_HIGH_TRESHOLD   2529    // 4.3V
