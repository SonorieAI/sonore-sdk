// SPDX-License-Identifier: Apache-2.0
// A shim of the Mach declarations audio_workgroup.h uses to make a thread
// real-time: thread_policy_set and its types. See ../README.md -- internal
// consistency, not ABI correctness.
#pragma once

#include <stdint.h>

extern "C" {
typedef int kern_return_t;
typedef int integer_t;
typedef unsigned int natural_t;
typedef int boolean_t;
typedef natural_t mach_port_t;
typedef mach_port_t thread_act_t;
typedef natural_t mach_msg_type_number_t;
#define KERN_SUCCESS 0
}

#include <mach/thread_policy.h>

extern "C" {
kern_return_t thread_policy_set(thread_act_t thread, thread_policy_flavor_t flavor,
                                thread_policy_t policy_info, mach_msg_type_number_t policy_infoCnt);
kern_return_t thread_policy_get(thread_act_t thread, thread_policy_flavor_t flavor,
                                thread_policy_t policy_info, mach_msg_type_number_t* policy_infoCnt,
                                boolean_t* get_default);
}
