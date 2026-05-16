// Storage for the dev clock-animation test trigger. See
// include/diag/clock_anim_test.h for the cross-core contract.

#include "clock_anim_test.h"

volatile uint8_t  g_clock_anim_test_kind = 0;
volatile uint32_t g_clock_anim_click_seq = 0;
