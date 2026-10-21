// SPDX-License-Identifier: Apache-2.0
// See mach.h beside this: the timebase that converts nanoseconds to ticks.
#pragma once

#include <mach/mach.h>

extern "C" {
struct mach_timebase_info {
  uint32_t numer;
  uint32_t denom;
};
typedef struct mach_timebase_info* mach_timebase_info_t;
typedef struct mach_timebase_info mach_timebase_info_data_t;

kern_return_t mach_timebase_info(mach_timebase_info_t info);
uint64_t mach_absolute_time(void);
}
