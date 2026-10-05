/****************************************************************************
 *
 * Copyright (c) 2023-26 David A Kerr... https://github.com/dkerr64/
 * All Rights Reserved.
 * Licensed under terms of the GPL-3.0 License.
 *
 */
#pragma once

// ESP-IDF include files...
#include <esp_err.h>

#include <gdo.h>

extern uint16_t closure_endpoint_id;
extern uint16_t dimension_endpoint_id;
extern uint16_t light_endpoint_id;
extern uint16_t lock_endpoint_id;
extern uint16_t mode_select_endpoint_id;
extern uint16_t occupancy_endpoint_id;

extern bool matter_comms_init_done;

extern void matter_comms_init();
extern void matter_comms_shutdown();

extern esp_err_t matter_notify_door_state(gdo_door_state_t door_state);
extern esp_err_t matter_notify_light_state(bool light_on);
extern esp_err_t matter_notify_lock_state(bool lock_on);
extern esp_err_t matter_notify_occupancy_state(bool occupied);
extern esp_err_t matter_notify_obstruction_state(bool obstructed);
extern esp_err_t matter_set_mode(uint8_t mode);
