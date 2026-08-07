/**
 * @file rtc.h
 * @author Alexander Ellul (igsalexcodes@gmail.com)
 * @brief Timebase helpers for the application.
 * 
 * @copyright
 * Copyright (c) 2026 Alexander Ellul.
 * 
 * SPDX-License-Identifier: GPL-3.0-only
 * 
 * This file is part of the OpenCanoe Timing System prototype firmware.
 * 
 * This software is licensed under the GNU General Public License v3.0.
 * See the LICENSE.md file in the root directory of this project for details.
 * 
 * This software is provided "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * either expressed or implied.
 * 
 */
#ifndef __RTC_H
#define __RTC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

void initalise_timer(void);

#ifdef __cplusplus
}
#endif

#endif /* __RTC_H */