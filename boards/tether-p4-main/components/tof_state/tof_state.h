#ifndef TOF_STATE_H
#define TOF_STATE_H

#include <stdint.h>

#define TOF_DISTANCE_INVALID  0xFFFF

void tof_state_set_distance_mm(uint16_t mm);
uint16_t tof_state_get_distance_mm(void);

#endif // TOF_STATE_H
