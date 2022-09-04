/****************************************************************************
 *
 *   Copyright (c) 2020 PX4 Development Team. All rights reserved.
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

#include "ICM42688P.hpp"

bool hitl_mode;

using namespace time_literals;

static constexpr int16_t combine(uint8_t msb, uint8_t lsb)
{
	return (msb << 8u) | lsb;
}

ICM42688P::ICM42688P(I2CSPIBusOption bus_option, int bus, uint32_t device, enum Rotation rotation, int bus_frequency,
		     spi_mode_e spi_mode, spi_drdy_gpio_t drdy_gpio) :
	SPI(DRV_IMU_DEVTYPE_ICM42688P, MODULE_NAME, bus, device, spi_mode, bus_frequency),
	I2CSPIDriver(MODULE_NAME, px4::device_bus_to_wq(get_device_id()), bus_option, bus),
	_drdy_gpio(drdy_gpio)
{
	if (drdy_gpio != 0) {
		_drdy_missed_perf = perf_alloc(PC_COUNT, MODULE_NAME": DRDY missed");
	}
	if (!hitl_mode) {
		_px4_accel = std::make_shared<PX4Accelerometer>(get_device_id(), rotation);
		_px4_gyro = std::make_shared<PX4Gyroscope>(get_device_id(), rotation);
		ConfigureSampleRate(_px4_gyro->get_max_rate_hz());
		_imu_server_pub.advertise();
	} else {
		ConfigureSampleRate(0);
	}
}

ICM42688P::~ICM42688P()
{
	perf_free(_bad_register_perf);
	perf_free(_bad_transfer_perf);
	perf_free(_fifo_empty_perf);
	perf_free(_fifo_overflow_perf);
	perf_free(_fifo_reset_perf);
	perf_free(_drdy_missed_perf);

	if (!hitl_mode){
     		_imu_server_pub.unadvertise();
	}
}

int ICM42688P::init()
{
	int ret = SPI::init();

	if (ret != PX4_OK) {
		DEVICE_DEBUG("SPI::init failed (%i)", ret);
		return ret;
	}

	return Reset() ? 0 : -1;
}

bool ICM42688P::Reset()
{
	_state = STATE::RESET;
	DataReadyInterruptDisable();
	ScheduleClear();
	ScheduleNow();
	return true;
}

void ICM42688P::exit_and_cleanup()
{
	DataReadyInterruptDisable();
	I2CSPIDriverBase::exit_and_cleanup();
}

void ICM42688P::print_status()
{
	I2CSPIDriverBase::print_status();

	PX4_INFO("FIFO empty interval: %d us (%.1f Hz)", _fifo_empty_interval_us, 1e6 / _fifo_empty_interval_us);

	perf_print_counter(_bad_register_perf);
	perf_print_counter(_bad_transfer_perf);
	perf_print_counter(_fifo_empty_perf);
	perf_print_counter(_fifo_overflow_perf);
	perf_print_counter(_fifo_reset_perf);
	perf_print_counter(_drdy_missed_perf);
}

int ICM42688P::probe()
{
	const uint8_t whoami = RegisterRead(Register::BANK_0::WHO_AM_I);

	if (whoami != WHOAMI) {
		DEVICE_DEBUG("unexpected WHO_AM_I 0x%02x", whoami);
		return PX4_ERROR;
	}

	return PX4_OK;
}

void ICM42688P::RunImpl()
{
	const hrt_abstime now = hrt_absolute_time();

	switch (_state) {
	case STATE::RESET:
		// DEVICE_CONFIG: Software reset configuration
		RegisterWrite(Register::BANK_0::DEVICE_CONFIG, DEVICE_CONFIG_BIT::SOFT_RESET_CONFIG);
		_reset_timestamp = now;
		_failure_count = 0;
		_state = STATE::WAIT_FOR_RESET;
		ScheduleDelayed(2_ms); // to be safe wait 2 ms for soft reset to be effective
		break;

	case STATE::WAIT_FOR_RESET:
		if ((RegisterRead(Register::BANK_0::WHO_AM_I) == WHOAMI)
		    && (RegisterRead(Register::BANK_0::DEVICE_CONFIG) == 0x00)
		    && (RegisterRead(Register::BANK_0::INT_STATUS) & INT_STATUS_BIT::RESET_DONE_INT)) {

			_state = STATE::CONFIGURE;
			ScheduleDelayed(10_ms); // imu does not start up without a delay here, TODO check why

		} else {
			// RESET not complete
			if (hrt_elapsed_time(&_reset_timestamp) > 1000_ms) {
				PX4_DEBUG("Reset failed, retrying");
				_state = STATE::RESET;
				ScheduleDelayed(100_ms);

			} else {
				PX4_DEBUG("Reset not complete, check again in 10 ms");
				ScheduleDelayed(10_ms);
			}
		}

		break;

	case STATE::CONFIGURE:
		if (Configure()) {

			// Wakeup accel and gyro after configuring registers
			ScheduleDelayed(1_ms); // add a delay here to be safe
			RegisterWrite(Register::BANK_0::PWR_MGMT0, PWR_MGMT0_BIT::GYRO_MODE_LOW_NOISE | PWR_MGMT0_BIT::ACCEL_MODE_LOW_NOISE);
			ScheduleDelayed(30_ms); // 30 ms gyro startup time, 10 ms accel from sleep to valid data

			// if configure succeeded then start reading from FIFO
			_state = STATE::FIFO_READ;

			if (DataReadyInterruptConfigure()) {
				_data_ready_interrupt_enabled = true;

				// backup schedule as a watchdog timeout
				ScheduleDelayed(100_ms);

			} else {
				_data_ready_interrupt_enabled = false;
				ScheduleOnInterval(_fifo_empty_interval_us, _fifo_empty_interval_us);
			}

			FIFOReset();

		} else {
			// CONFIGURE not complete
			if (hrt_elapsed_time(&_reset_timestamp) > 1000_ms) {
				PX4_DEBUG("Configure failed, resetting");
				_state = STATE::RESET;

			} else {
				PX4_DEBUG("Configure failed, retrying");
			}

			ScheduleDelayed(100_ms);
		}

		break;

	case STATE::FIFO_READ: {
#ifndef __PX4_QURT
			uint32_t samples = 0;

			if (_data_ready_interrupt_enabled) {
				// scheduled from interrupt if _drdy_fifo_read_samples was set as expected
				if (_drdy_fifo_read_samples.fetch_and(0) != _fifo_gyro_samples) {
					perf_count(_drdy_missed_perf);

				} else {
					samples = _fifo_gyro_samples;
				}

				// push backup schedule back
				ScheduleDelayed(_fifo_empty_interval_us * 2);
			}

			if (samples == 0) {
				// check current FIFO count
				const uint16_t fifo_count = FIFOReadCount();

				if (fifo_count >= FIFO::SIZE) {
					FIFOReset();
					perf_count(_fifo_overflow_perf);

				} else if (fifo_count == 0) {
					perf_count(_fifo_empty_perf);

				} else {
					// FIFO count (size in bytes)
					samples = (fifo_count / sizeof(FIFO::DATA));

					if (samples > FIFO_MAX_SAMPLES) {
						// not technically an overflow, but more samples than we expected or can publish
						FIFOReset();
						perf_count(_fifo_overflow_perf);
						samples = 0;
					}
				}
			}

			bool success = false;

			if (samples >= 1) {
				if (FIFORead(now, samples)) {
					success = true;

					if (_failure_count > 0) {
						_failure_count--;
					}
				}
			}

			if (!success) {
				_failure_count++;

				// full reset if things are failing consistently
				if (_failure_count > 10) {
					Reset();
					return;
				}
			}

			// check configuration registers periodically or immediately following any failure
			if (RegisterCheck(_register_bank0_cfg[_checked_register_bank0])
			    && RegisterCheck(_register_bank1_cfg[_checked_register_bank1])
			    && RegisterCheck(_register_bank2_cfg[_checked_register_bank2])
			   ) {
				_last_config_check_timestamp = now;
				_checked_register_bank0 = (_checked_register_bank0 + 1) % size_register_bank0_cfg;
				_checked_register_bank1 = (_checked_register_bank1 + 1) % size_register_bank1_cfg;
				_checked_register_bank2 = (_checked_register_bank2 + 1) % size_register_bank2_cfg;

			} else {
				// register check failed, force reset
				perf_count(_bad_register_perf);
				Reset();
			}
#endif
		}

		break;
	}
}

void ICM42688P::ConfigureSampleRate(int sample_rate)
{
	if (sample_rate == 0) {
		sample_rate = 500; // default to 500 Hz
	}

	// round down to nearest FIFO sample dt
	const float min_interval = FIFO_SAMPLE_DT;
	_fifo_empty_interval_us = math::max(roundf((1e6f / (float)sample_rate) / min_interval) * min_interval, min_interval);

	_fifo_gyro_samples = roundf(math::min((float)_fifo_empty_interval_us / (1e6f / GYRO_RATE), (float)FIFO_MAX_SAMPLES));

	// recompute FIFO empty interval (us) with actual gyro sample limit
	_fifo_empty_interval_us = _fifo_gyro_samples * (1e6f / GYRO_RATE);

	ConfigureFIFOWatermark(_fifo_gyro_samples);
}

void ICM42688P::ConfigureFIFOWatermark(uint8_t samples)
{
	// FIFO watermark threshold in number of bytes
	const uint16_t fifo_watermark_threshold = samples * sizeof(FIFO::DATA);

	for (auto &r : _register_bank0_cfg) {
		if (r.reg == Register::BANK_0::FIFO_CONFIG2) {
			// FIFO_WM[7:0]  FIFO_CONFIG2
			r.set_bits = fifo_watermark_threshold & 0xFF;

		} else if (r.reg == Register::BANK_0::FIFO_CONFIG3) {
			// FIFO_WM[11:8] FIFO_CONFIG3
			r.set_bits = (fifo_watermark_threshold >> 8) & 0x0F;
		}
	}
}

void ICM42688P::SelectRegisterBank(enum REG_BANK_SEL_BIT bank)
{
	if (bank != _last_register_bank) {
		// select BANK_0
		uint8_t cmd_bank_sel[2] {};
		cmd_bank_sel[0] = static_cast<uint8_t>(Register::BANK_0::REG_BANK_SEL);
		cmd_bank_sel[1] = bank;
		transfer(cmd_bank_sel, cmd_bank_sel, sizeof(cmd_bank_sel));

		_last_register_bank = bank;
	}
}

bool ICM42688P::Configure()
{
	// first set and clear all configured register bits
	for (const auto &reg_cfg : _register_bank0_cfg) {
		RegisterSetAndClearBits(reg_cfg.reg, reg_cfg.set_bits, reg_cfg.clear_bits);
	}

	for (const auto &reg_cfg : _register_bank1_cfg) {
		RegisterSetAndClearBits(reg_cfg.reg, reg_cfg.set_bits, reg_cfg.clear_bits);
	}

	for (const auto &reg_cfg : _register_bank2_cfg) {
		RegisterSetAndClearBits(reg_cfg.reg, reg_cfg.set_bits, reg_cfg.clear_bits);
	}

	// now check that all are configured
	bool success = true;

	for (const auto &reg_cfg : _register_bank0_cfg) {
		if (!RegisterCheck(reg_cfg)) {
			success = false;
		}
	}

	for (const auto &reg_cfg : _register_bank1_cfg) {
		if (!RegisterCheck(reg_cfg)) {
			success = false;
		}
	}

	for (const auto &reg_cfg : _register_bank2_cfg) {
		if (!RegisterCheck(reg_cfg)) {
			success = false;
		}
	}

	// // 20-bits data format used
	// //  the only FSR settings that are operational are ±2000dps for gyroscope and ±16g for accelerometer
	if (!hitl_mode){
		_px4_accel->set_range(16.f * CONSTANTS_ONE_G);
		_px4_accel->set_scale(CONSTANTS_ONE_G / 8192.f);
		_px4_gyro->set_range(math::radians(2000.f));
		_px4_gyro->set_scale(math::radians(1.f / 131.f));
	}
	return success;
}

int ICM42688P::DataReadyInterruptCallback(int irq, void *context, void *arg)
{
	static_cast<ICM42688P *>(arg)->DataReady();
	return 0;
}

void ICM42688P::DataReady()
{
#ifndef __PX4_QURT
	uint32_t expected = 0;

	if (_drdy_fifo_read_samples.compare_exchange(&expected, _fifo_gyro_samples)) {
		ScheduleNow();
	}
#else
    uint16_t fifo_byte_count = FIFOReadCount();
    PX4_DEBUG("ICM42688P::DataReady reading %u bytes", fifo_byte_count);

    FIFORead(hrt_absolute_time(), fifo_byte_count / sizeof(FIFO::DATA));
#endif
}

bool ICM42688P::DataReadyInterruptConfigure()
{
	if (_drdy_gpio == 0) {
		return false;
	}

	// Setup data ready on falling edge
	return px4_arch_gpiosetevent(_drdy_gpio, false, true, true, &DataReadyInterruptCallback, this) == 0;
}

bool ICM42688P::DataReadyInterruptDisable()
{
	if (_drdy_gpio == 0) {
		return false;
	}

	return px4_arch_gpiosetevent(_drdy_gpio, false, false, false, nullptr, nullptr) == 0;
}

template <typename T>
bool ICM42688P::RegisterCheck(const T &reg_cfg)
{
	bool success = true;

	const uint8_t reg_value = RegisterRead(reg_cfg.reg);

	if (reg_cfg.set_bits && ((reg_value & reg_cfg.set_bits) != reg_cfg.set_bits)) {
		PX4_DEBUG("0x%02hhX: 0x%02hhX (0x%02hhX not set)", (uint8_t)reg_cfg.reg, reg_value, reg_cfg.set_bits);
		success = false;
	}

	if (reg_cfg.clear_bits && ((reg_value & reg_cfg.clear_bits) != 0)) {
		PX4_DEBUG("0x%02hhX: 0x%02hhX (0x%02hhX not cleared)", (uint8_t)reg_cfg.reg, reg_value, reg_cfg.clear_bits);
		success = false;
	}

	return success;
}

template <typename T>
uint8_t ICM42688P::RegisterRead(T reg)
{
	uint8_t cmd[2] {};
	cmd[0] = static_cast<uint8_t>(reg) | DIR_READ;
	SelectRegisterBank(reg);
	transfer(cmd, cmd, sizeof(cmd));
	return cmd[1];
}

template <typename T>
void ICM42688P::RegisterWrite(T reg, uint8_t value)
{
	uint8_t cmd[2] { (uint8_t)reg, value };
	SelectRegisterBank(reg);
	transfer(cmd, cmd, sizeof(cmd));
}

template <typename T>
void ICM42688P::RegisterSetAndClearBits(T reg, uint8_t setbits, uint8_t clearbits)
{
	const uint8_t orig_val = RegisterRead(reg);

	uint8_t val = (orig_val & ~clearbits) | setbits;

	if (orig_val != val) {
		RegisterWrite(reg, val);
	}
}

uint16_t ICM42688P::FIFOReadCount()
{
	// read FIFO count
	uint8_t fifo_count_buf[3] {};
	fifo_count_buf[0] = static_cast<uint8_t>(Register::BANK_0::FIFO_COUNTH) | DIR_READ;
	SelectRegisterBank(REG_BANK_SEL_BIT::USER_BANK_0);

	if (transfer(fifo_count_buf, fifo_count_buf, sizeof(fifo_count_buf)) != PX4_OK) {
		perf_count(_bad_transfer_perf);
		return 0;
	}

	return combine(fifo_count_buf[1], fifo_count_buf[2]);
}

bool ICM42688P::FIFORead(const hrt_abstime &timestamp_sample, uint8_t samples)
{
	FIFOTransferBuffer buffer{};
	const size_t transfer_size = math::min(samples * sizeof(FIFO::DATA) + 4, FIFO::SIZE);
	SelectRegisterBank(REG_BANK_SEL_BIT::USER_BANK_0);

	if (transfer((uint8_t *)&buffer, (uint8_t *)&buffer, transfer_size) != PX4_OK) {
		perf_count(_bad_transfer_perf);
		return false;
	}

	// if (buffer.INT_STATUS & INT_STATUS_BIT::FIFO_FULL_INT) {
	// 	perf_count(_fifo_overflow_perf);
	// 	FIFOReset();
	// 	return false;
	// }
    //
	// const uint16_t fifo_count_bytes = combine(buffer.FIFO_COUNTH, buffer.FIFO_COUNTL);
    //
	// if (fifo_count_bytes >= FIFO::SIZE) {
	// 	perf_count(_fifo_overflow_perf);
	// 	FIFOReset();
	// 	return false;
	// }
    //
	// uint8_t fifo_count_samples = samples;
    //
	// if (fifo_count_samples == 0) {
	// 	perf_count(_fifo_empty_perf);
	// 	return false;
	// }

	// check FIFO header in every sample
	// uint8_t valid_samples = 0;

    // int loop_count = math::min(samples, fifo_count_samples);
	for (int i = 0, j = (samples - 1); i < samples; i++, j--) {
		// const uint8_t FIFO_HEADER = buffer.f[i].FIFO_Header;
		// const uint8_t VALID_FIFO_HEADER = FIFO::FIFO_HEADER_BIT::HEADER_ACCEL |
        //                                   FIFO::FIFO_HEADER_BIT::HEADER_GYRO |
        //                                   FIFO::FIFO_HEADER_BIT::HEADER_20;
        //
		// if ((FIFO_HEADER & 0xF3) == VALID_FIFO_HEADER) {
		// 	valid_samples++;

            // Put valid sample into queue with appropriate timestamp.
            hrt_abstime reported_timestamp = timestamp_sample - ((uint32_t) j * (uint32_t) FIFO_SAMPLE_DT);
            ProcessIMU(reported_timestamp, buffer.f[i]);
            // fifo_count_samples--;
            // PX4_INFO("To IMU server: %d %d %llu %llu %u", i, j, timestamp_sample, reported_timestamp, (uint32_t) FIFO_SAMPLE_DT);
		// } else {
		// 	perf_count(_bad_transfer_perf);
		// 	break;
		// }
	}

	return true;
}

void ICM42688P::FIFOReset()
{
	perf_count(_fifo_reset_perf);

	// SIGNAL_PATH_RESET: FIFO flush
	RegisterSetBits(Register::BANK_0::SIGNAL_PATH_RESET, SIGNAL_PATH_RESET_BIT::FIFO_FLUSH);

	// reset while FIFO is disabled
	_drdy_fifo_read_samples.store(0);

    //_imu_server_samples = 0;
}

static constexpr int32_t reassemble_20bit(const uint32_t a, const uint32_t b, const uint32_t c)
{
	// 0xXXXAABBC
	uint32_t high   = ((a << 12) & 0x000FF000);
	uint32_t low    = ((b << 4)  & 0x00000FF0);
	uint32_t lowest = (c         & 0x0000000F);

	uint32_t x = high | low | lowest;

	if (a & Bit7) {
		// sign extend
		x |= 0xFFF00000u;
	}

	return static_cast<int32_t>(x);
}

void ICM42688P::ProcessIMU(const hrt_abstime &timestamp_sample, const FIFO::DATA &fifo) {

    // TODO: There is a bunch of copied code in here. Refactor into common functions.

    float accel_x = 0.0, accel_y = 0.0, accel_z = 0.0;
    float gyro_x = 0.0,  gyro_y = 0.0,  gyro_z = 0.0;

	// 20 bit hires mode

	// Sign extension + Accel [19:12] + Accel [11:4] + Accel [3:2] (20 bit extension byte)
	// Accel data is 18 bit
	int32_t temp_accel_x = reassemble_20bit(fifo.ACCEL_DATA_X1, fifo.ACCEL_DATA_X0,
					   fifo.Ext_Accel_X_Gyro_X & 0xF0 >> 4);
	int32_t temp_accel_y = reassemble_20bit(fifo.ACCEL_DATA_Y1, fifo.ACCEL_DATA_Y0,
					   fifo.Ext_Accel_Y_Gyro_Y & 0xF0 >> 4);
	int32_t temp_accel_z = reassemble_20bit(fifo.ACCEL_DATA_Z1, fifo.ACCEL_DATA_Z0,
					   fifo.Ext_Accel_Z_Gyro_Z & 0xF0 >> 4);

	// Gyro [19:12] + Gyro [11:4] + Gyro [3:0] (bottom 4 bits of 20 bit extension byte)
	int32_t temp_gyro_x = reassemble_20bit(fifo.GYRO_DATA_X1, fifo.GYRO_DATA_X0,
                       fifo.Ext_Accel_X_Gyro_X & 0x0F);
	int32_t temp_gyro_y = reassemble_20bit(fifo.GYRO_DATA_Y1, fifo.GYRO_DATA_Y0,
                       fifo.Ext_Accel_Y_Gyro_Y & 0x0F);
	int32_t temp_gyro_z = reassemble_20bit(fifo.GYRO_DATA_Z1, fifo.GYRO_DATA_Z0,
                       fifo.Ext_Accel_Z_Gyro_Z & 0x0F);

	// accel samples invalid if -524288
	if (temp_accel_x != -524288 && temp_accel_y != -524288 && temp_accel_z != -524288) {

		// shift accel by 2 (2 least significant bits are always 0)
		accel_x = (float) temp_accel_x / 4.f;
		accel_y = (float) temp_accel_y / 4.f;
		accel_z = (float) temp_accel_z / 4.f;

		// shift gyro by 1 (least significant bit is always 0)
		gyro_x = (float) temp_gyro_x / 2.f;
		gyro_y = (float) temp_gyro_y / 2.f;
		gyro_z = (float) temp_gyro_z / 2.f;

    	// correct frame for publication
    	// sensor's frame is +x forward, +y left, +z up
    	// flip y & z to publish right handed with z down (x forward, y right, z down)
		accel_y = -accel_y;
		accel_z = -accel_z;
		gyro_y  = -gyro_y;
		gyro_z  = -gyro_z;

        // Publish samples for flight control at 500Hz
        if (!hitl_mode){
		if (_imu_server_samples % 2) {
			_px4_accel->update(timestamp_sample, accel_x, accel_y, accel_z);
			_px4_gyro->update(timestamp_sample, gyro_x, gyro_y, gyro_z);
		}
	}
        // Scale everything appropriately
        float accel_scale_factor = (CONSTANTS_ONE_G / 8192.f);
        accel_x *= accel_scale_factor;
        accel_y *= accel_scale_factor;
        accel_z *= accel_scale_factor;

    	float gyro_scale_factor = math::radians(1.f / 131.f);
        gyro_x *= gyro_scale_factor;
        gyro_y *= gyro_scale_factor;
        gyro_z *= gyro_scale_factor;

        if (!hitl_mode){
		// Store the data in our array
		_imu_server_data.accel_x[_imu_server_samples] = accel_x;
		_imu_server_data.accel_y[_imu_server_samples] = accel_y;
		_imu_server_data.accel_z[_imu_server_samples] = accel_z;
		_imu_server_data.gyro_x[_imu_server_samples]  = gyro_x;
		_imu_server_data.gyro_y[_imu_server_samples]  = gyro_y;
		_imu_server_data.gyro_z[_imu_server_samples]  = gyro_z;
		_imu_server_data.ts[_imu_server_samples]      = timestamp_sample;

		_imu_server_samples++;

		// If array is full, publish the data
		if (_imu_server_samples == 10) {
			_imu_server_samples = 0;
			_imu_server_data.timestamp = hrt_absolute_time();
			_imu_server_data.temperature = 0; // Not used right now
			_imu_server_pub.publish(_imu_server_data);
		}
	}

        _temperature_samples++;

        if (_temperature_samples == 40) {
            _temperature_samples = 0;

            // Only update the temperature at 25Hz rate
    		const int16_t t = combine(fifo.TEMP_DATA1, fifo.TEMP_DATA0);

    		// Temperature sample invalid if -32768
    		if (t != -32768) {
    		    const float TEMP_degC = ((float) t / TEMPERATURE_SENSITIVITY) + TEMPERATURE_OFFSET;

        		if (PX4_ISFINITE(TEMP_degC)) {
				if (!hitl_mode){
					_px4_accel->set_temperature(TEMP_degC);
					_px4_gyro->set_temperature(TEMP_degC);
				}
                }
    		}
        }
    }
}

void ICM42688P::ProcessAccel(const hrt_abstime &timestamp_sample, const FIFO::DATA fifo[], const uint8_t samples)
{
	sensor_accel_fifo_s accel{};
	accel.timestamp_sample = timestamp_sample;
	accel.samples = 0;
	accel.dt = FIFO_SAMPLE_DT;

	// 18-bits of accelerometer data
	bool scale_20bit = false;

	// first pass
	for (int i = 0; i < samples; i++) {
		// 20 bit hires mode
		// Sign extension + Accel [19:12] + Accel [11:4] + Accel [3:2] (20 bit extension byte)
		// Accel data is 18 bit ()
		int32_t accel_x = reassemble_20bit(fifo[i].ACCEL_DATA_X1, fifo[i].ACCEL_DATA_X0,
						   fifo[i].Ext_Accel_X_Gyro_X & 0xF0 >> 4);
		int32_t accel_y = reassemble_20bit(fifo[i].ACCEL_DATA_Y1, fifo[i].ACCEL_DATA_Y0,
						   fifo[i].Ext_Accel_Y_Gyro_Y & 0xF0 >> 4);
		int32_t accel_z = reassemble_20bit(fifo[i].ACCEL_DATA_Z1, fifo[i].ACCEL_DATA_Z0,
						   fifo[i].Ext_Accel_Z_Gyro_Z & 0xF0 >> 4);

		// sample invalid if -524288
		if (accel_x != -524288 && accel_y != -524288 && accel_z != -524288) {
			// check if any values are going to exceed int16 limits
			static constexpr int16_t max_accel = INT16_MAX;
			static constexpr int16_t min_accel = INT16_MIN;

			if (accel_x >= max_accel || accel_x <= min_accel) {
				scale_20bit = true;
			}

			if (accel_y >= max_accel || accel_y <= min_accel) {
				scale_20bit = true;
			}

			if (accel_z >= max_accel || accel_z <= min_accel) {
				scale_20bit = true;
			}

			// shift by 2 (2 least significant bits are always 0)
			accel.x[accel.samples] = accel_x / 4;
			accel.y[accel.samples] = accel_y / 4;
			accel.z[accel.samples] = accel_z / 4;
			accel.samples++;
		}
	}

	if (!scale_20bit) {
		// if highres enabled accel data is always 8192 LSB/g
		if (!hitl_mode){
			_px4_accel->set_scale(CONSTANTS_ONE_G / 8192.f);
		}

	} else {
		// 20 bit data scaled to 16 bit (2^4)
		for (int i = 0; i < samples; i++) {
			// 20 bit hires mode
			// Sign extension + Accel [19:12] + Accel [11:4] + Accel [3:2] (20 bit extension byte)
			// Accel data is 18 bit ()
			int16_t accel_x = combine(fifo[i].ACCEL_DATA_X1, fifo[i].ACCEL_DATA_X0);
			int16_t accel_y = combine(fifo[i].ACCEL_DATA_Y1, fifo[i].ACCEL_DATA_Y0);
			int16_t accel_z = combine(fifo[i].ACCEL_DATA_Z1, fifo[i].ACCEL_DATA_Z0);

			accel.x[i] = accel_x;
			accel.y[i] = accel_y;
			accel.z[i] = accel_z;
		}
		if (!hitl_mode){
			_px4_accel->set_scale(CONSTANTS_ONE_G / 2048.f);
		}
	}

	// correct frame for publication
	for (int i = 0; i < accel.samples; i++) {
		// sensor's frame is +x forward, +y left, +z up
		//  flip y & z to publish right handed with z down (x forward, y right, z down)
		accel.x[i] = accel.x[i];
		accel.y[i] = (accel.y[i] == INT16_MIN) ? INT16_MAX : -accel.y[i];
		accel.z[i] = (accel.z[i] == INT16_MIN) ? INT16_MAX : -accel.z[i];
	}

	if (!hitl_mode){
		_px4_accel->set_error_count(perf_event_count(_bad_register_perf) + perf_event_count(_bad_transfer_perf) +
	 			   perf_event_count(_fifo_empty_perf) + perf_event_count(_fifo_overflow_perf));
	}

	if (accel.samples > 0) {
		if (!hitl_mode){
			_px4_accel->updateFIFO(accel);
		}
	}
}

void ICM42688P::ProcessGyro(const hrt_abstime &timestamp_sample, const FIFO::DATA fifo[], const uint8_t samples)
{
	sensor_gyro_fifo_s gyro{};
	gyro.timestamp_sample = timestamp_sample;
	gyro.samples = 0;
	gyro.dt = FIFO_SAMPLE_DT;

	// 20-bits of gyroscope data
	bool scale_20bit = false;

	// first pass
	for (int i = 0; i < samples; i++) {
		// 20 bit hires mode
		// Gyro [19:12] + Gyro [11:4] + Gyro [3:0] (bottom 4 bits of 20 bit extension byte)
		int32_t gyro_x = reassemble_20bit(fifo[i].GYRO_DATA_X1, fifo[i].GYRO_DATA_X0, fifo[i].Ext_Accel_X_Gyro_X & 0x0F);
		int32_t gyro_y = reassemble_20bit(fifo[i].GYRO_DATA_Y1, fifo[i].GYRO_DATA_Y0, fifo[i].Ext_Accel_Y_Gyro_Y & 0x0F);
		int32_t gyro_z = reassemble_20bit(fifo[i].GYRO_DATA_Z1, fifo[i].GYRO_DATA_Z0, fifo[i].Ext_Accel_Z_Gyro_Z & 0x0F);

		// check if any values are going to exceed int16 limits
		static constexpr int16_t max_gyro = INT16_MAX;
		static constexpr int16_t min_gyro = INT16_MIN;

		if (gyro_x >= max_gyro || gyro_x <= min_gyro) {
			scale_20bit = true;
		}

		if (gyro_y >= max_gyro || gyro_y <= min_gyro) {
			scale_20bit = true;
		}

		if (gyro_z >= max_gyro || gyro_z <= min_gyro) {
			scale_20bit = true;
		}

		gyro.x[gyro.samples] = gyro_x / 2;
		gyro.y[gyro.samples] = gyro_y / 2;
		gyro.z[gyro.samples] = gyro_z / 2;
		gyro.samples++;
	}

	if (!scale_20bit) {
		// if highres enabled gyro data is always 131 LSB/dps
		if (!hitl_mode){
			_px4_gyro->set_scale(math::radians(1.f / 131.f));
		}
	} else {
		// 20 bit data scaled to 16 bit (2^4)
		for (int i = 0; i < samples; i++) {
			gyro.x[i] = combine(fifo[i].GYRO_DATA_X1, fifo[i].GYRO_DATA_X0);
			gyro.y[i] = combine(fifo[i].GYRO_DATA_Y1, fifo[i].GYRO_DATA_Y0);
			gyro.z[i] = combine(fifo[i].GYRO_DATA_Z1, fifo[i].GYRO_DATA_Z0);
		}
		if (!hitl_mode){
			_px4_gyro->set_scale(math::radians(2000.f / 32768.f));
		}
	}

	// correct frame for publication
	for (int i = 0; i < gyro.samples; i++) {
		// sensor's frame is +x forward, +y left, +z up
		//  flip y & z to publish right handed with z down (x forward, y right, z down)
		gyro.x[i] = gyro.x[i];
		gyro.y[i] = (gyro.y[i] == INT16_MIN) ? INT16_MAX : -gyro.y[i];
		gyro.z[i] = (gyro.z[i] == INT16_MIN) ? INT16_MAX : -gyro.z[i];
	}

	if (!hitl_mode){
		_px4_gyro->set_error_count(perf_event_count(_bad_register_perf) + perf_event_count(_bad_transfer_perf) +
	 			  perf_event_count(_fifo_empty_perf) + perf_event_count(_fifo_overflow_perf));
	}

	if (gyro.samples > 0) {
		if (!hitl_mode){
			_px4_gyro->updateFIFO(gyro);
		}
	}
}

bool ICM42688P::ProcessTemperature(const FIFO::DATA fifo[], const uint8_t samples)
{
	int16_t temperature[FIFO_MAX_SAMPLES];
	float temperature_sum{0};

	int valid_samples = 0;

	for (int i = 0; i < samples; i++) {
		const int16_t t = combine(fifo[i].TEMP_DATA1, fifo[i].TEMP_DATA0);

		// sample invalid if -32768
		if (t != -32768) {
			temperature_sum += t;
			temperature[valid_samples] = t;
			valid_samples++;
		}
	}

	if (valid_samples > 0) {
		const float temperature_avg = temperature_sum / valid_samples;

		for (int i = 0; i < valid_samples; i++) {
			// temperature changing wildly is an indication of a transfer error
			if (fabsf(temperature[i] - temperature_avg) > 1000) {
				perf_count(_bad_transfer_perf);
				return false;
			}
		}

		// use average temperature reading
		const float TEMP_degC = (temperature_avg / TEMPERATURE_SENSITIVITY) + TEMPERATURE_OFFSET;

		if (PX4_ISFINITE(TEMP_degC)) {
			if (!hitl_mode){
				_px4_accel->set_temperature(TEMP_degC);
				_px4_gyro->set_temperature(TEMP_degC);
				return true;
			}
		} else {
			perf_count(_bad_transfer_perf);
		}
	}

	return false;
}
