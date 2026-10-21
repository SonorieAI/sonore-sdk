// SPDX-License-Identifier: Apache-2.0
// See mach.h beside this: the time-constraint policy, as <mach/thread_policy.h>
// declares it.
#pragma once

#include <mach/mach.h>

extern "C" {
typedef natural_t thread_policy_flavor_t;
typedef integer_t* thread_policy_t;

#define THREAD_TIME_CONSTRAINT_POLICY 2

struct thread_time_constraint_policy {
  uint32_t period;
  uint32_t computation;
  uint32_t constraint;
  boolean_t preemptible;
};
typedef struct thread_time_constraint_policy thread_time_constraint_policy_data_t;
typedef struct thread_time_constraint_policy* thread_time_constraint_policy_t;

#define THREAD_TIME_CONSTRAINT_POLICY_COUNT \
  ((mach_msg_type_number_t) (sizeof(thread_time_constraint_policy_data_t) / sizeof(integer_t)))
}
