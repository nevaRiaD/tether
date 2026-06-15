#include "tof_state.h"

// 16-bit aligned scalar read/write on ESP32 is atomic; volatile prevents register caching across tasks.
static volatile uint16_t s_distance_mm = TOF_DISTANCE_INVALID;

void tof_state_set_distance_mm(uint16_t mm)
{
    s_distance_mm = mm;
}

uint16_t tof_state_get_distance_mm(void)
{
    return s_distance_mm;
}
