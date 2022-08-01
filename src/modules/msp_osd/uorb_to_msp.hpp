/****************************************************************************
 *
 *   Copyright (c) 2022 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/* uorb_to_msp.hpp
 *
 * Declaration of functions which translate UORB messages into MSP specific structures.
 */

#pragma once

// basic types
#include <cmath>

// UORB topic structs
#include <uORB/topics/parameter_update.h>
#include <uORB/topics/sensor_combined.h>
#include <uORB/topics/power_monitor.h>
#include <uORB/topics/battery_status.h>
#include <uORB/topics/vehicle_gps_position.h>
#include <uORB/topics/vehicle_status.h>
#include <uORB/topics/airspeed_validated.h>
#include <uORB/topics/vehicle_air_data.h>
#include <uORB/topics/home_position.h>
#include <uORB/topics/vehicle_global_position.h>
#include <uORB/topics/vehicle_local_position.h>
#include <uORB/topics/vehicle_attitude.h>
#include <uORB/topics/estimator_status.h>
#include <uORB/topics/input_rc.h>

// MSP structs
#include "msp_defines.h"

#include "message_display.hpp"

namespace msp_osd {

// construct an MSP_NAME struct
//  note: this is actually how we display _all_ string information
msp_name_t construct_display_message(const vehicle_status_s& vehicle_status,
				     const struct vehicle_attitude_s& vehicle_attitude,
				     MessageDisplay& display);

// construct an MSP_FC_VARIANT struct
msp_fc_variant_t construct_FC_VARIANT();

// construct an MSP_STATUS struct
msp_status_BF_t construct_STATUS(const struct vehicle_status_s& vehicle_status);

// construct an MSP_ANALOG struct
msp_analog_t construct_ANALOG(const struct battery_status_s& battery_status, const struct input_rc_s& input_rc);

// construct an MSP_BATTERY_STATE struct
msp_battery_state_t construct_BATTERY_STATE(const struct battery_status_s& battery_status);

// construct an MSP_RAW_GPS struct
msp_raw_gps_t construct_RAW_GPS(const struct vehicle_gps_position_s& vehicle_gps_position,
				const struct airspeed_validated_s& airspeed_validated);

// construct an MSP_COMP_GPS struct
msp_comp_gps_t construct_COMP_GPS(const struct home_position_s& home_position,
				  const struct estimator_status_s& estimator_status,
				  const struct vehicle_global_position_s& vehicle_global_position,
				  const bool heartbeat);

// construct an MSP_ATTITUDE struct
msp_attitude_t construct_ATTITUDE(const struct vehicle_attitude_s& vehicle_attitude);

// construct an MSP_ALTITUDE struct
msp_altitude_t construct_ALTITUDE(const struct vehicle_gps_position_s& vehicle_gps_position,
				  const struct estimator_status_s& estimator_status,
				  const struct vehicle_local_position_s& vehicle_local_position);

// construct an MSP_ESC_SENSOR_DATA struct
msp_esc_sensor_data_dji_t construct_ESC_SENSOR_DATA();

} // namespace msp_osd
