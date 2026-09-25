#pragma once

#include <stdint.h>
#include <math.h>
#include "FreematicsMEMS.h"
#include <driver/i2c.h>

// ==============================================================
//  ICM-42627 Calibration Data & WOM Configuration
//  Calibrate once, reuse on all devices.
//
//  System behaviour:
//    IDLE > IDLE_SLEEP_TIMEOUT_MS   -> deep sleep
//    Door open/slam (WOM event)     -> wake from deep sleep (GPIO 39)
//    Driving detected               -> stay awake, reset idle timer
//
//  Hardware:
//    Sensor INT1 is wired to ESP32 GPIO 39.
//    Configured as Active-HIGH, Push-Pull.
//
//  Sensor config: ACCEL +/-2g / GYRO +/-250dps
//  WOM LSB resolution: 15.625 mg  (4g / 256)
// ==============================================================

// -- Sleep policy ----------------------------------------
#define IDLE_SLEEP_TIMEOUT_MS   (5UL * 60UL * 1000UL)  // 5 minutes in production

// -- Idle noise floor ------------------------------------
#define CAL_IDLE_MEAN_G          0.0027f
#define CAL_IDLE_STDDEV_G        0.0024f
#define CAL_IDLE_NOISE_FLOOR_G   0.0099f   // mean + 3 sigma

// -- Door event (WOM wake trigger) -----------------------
#define CAL_DOOR_PEAK_G          0.3066f
#define CAL_DOOR_GYRO_DPS        200.60f
#define WOM_THRESHOLD_G          0.1582f   // midpoint: noise floor <-> door peak
#define WOM_THRESHOLD_LSB        10        // 0x0A  (0.1582 / 0.015625 = 10.1)

// -- Driving (keep-alive signal) -------------------------
#define CAL_DRIVING_PEAK_G       0.1636f
#define CAL_DRIVING_MEAN_G       0.0205f
#define CAL_DRIVING_GYRO_DPS     199.56f
#define DRIVING_THRESHOLD_G      0.0152f   // midpoint: noise floor <-> driving mean

// ==============================================================
//  State Helpers
// ==============================================================

// isDriving: pass a rolling mean of net-accel over ~1 second.
inline bool isDriving(float rollingMeanNetG) {
  return rollingMeanNetG > DRIVING_THRESHOLD_G;
}

// isIdle: peak net-accel in the last sample window is below noise floor.
inline bool isIdle(float peakNetG) {
  return peakNetG < CAL_IDLE_NOISE_FLOOR_G;
}

// ==============================================================
//  Activity Matching Classifier
// ==============================================================

#define ACT_MATCH_IDLE     -1
#define ACT_MATCH_DOOR      0
#define ACT_MATCH_DRIVING   1

// Nearest-neighbour classifier (accel peak + mean only).
inline int matchActivity(float peakNetG, float meanNetG) {
  const float profiles[2][2] = { {0.3066f, 0.0159f},   // DOOR_EVENT
                                 {0.1636f, 0.0205f} }; // DRIVING
  const float idlePeak = 0.0241f, idleMean = 0.0027f;
  const float pR = 0.2824f, mR = 0.0179f;

  float dIdlePk = (peakNetG - idlePeak) / pR;
  float dIdleMn = (meanNetG - idleMean) / mR;
  float dIdle   = sqrtf(dIdlePk * dIdlePk * 0.6f + dIdleMn * dIdleMn * 0.4f);

  float best = dIdle;
  int   bestIdx = ACT_MATCH_IDLE;
  for (int i = 0; i < 2; i++) {
    float dp = (peakNetG - profiles[i][0]) / pR;
    float dm = (meanNetG - profiles[i][1]) / mR;
    float d  = sqrtf(dp * dp * 0.6f + dm * dm * 0.4f);
    if (d < best) { best = d; bestIdx = i; }
  }
  return bestIdx;
}

// ==============================================================
//  Raw I2C Access (ICM-42627 at address 0x68)
// ==============================================================
// Used by configureWOM() because the standard library hides 
// the required APEX memory bank switching registers.

static inline void icm42627_write_reg(uint8_t reg, uint8_t data) {
  uint8_t buf[2] = { reg, data };
  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (0x68 << 1) | I2C_MASTER_WRITE, true);
  i2c_master_write(cmd, buf, 2, true);
  i2c_master_stop(cmd);
  i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(10));
  i2c_cmd_link_delete(cmd);
}

static inline uint8_t icm42627_read_reg(uint8_t reg) {
  uint8_t data = 0;
  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (0x68 << 1) | I2C_MASTER_WRITE, true);
  i2c_master_write_byte(cmd, reg, true);
  i2c_master_stop(cmd);
  i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(10));
  i2c_cmd_link_delete(cmd);
  
  cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (0x68 << 1) | I2C_MASTER_READ, true);
  i2c_master_read_byte(cmd, &data, I2C_MASTER_NACK);
  i2c_master_stop(cmd);
  i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(10));
  i2c_cmd_link_delete(cmd);
  
  return data;
}

// ==============================================================
//  Wake-on-Motion (WOM) Hardware Setup
// ==============================================================
// Call once after sensor.begin() and just before entering deep sleep.
//
// Register Map:
//   0x76  REG_BANK_SEL      -- Switches memory banks
//   0x14  INT_CONFIG        -- Interrupt pin configuration
//   0x4A  ACCEL_WOM_X_THR   -- WOM threshold X axis (Bank 4)
//   0x4B  ACCEL_WOM_Y_THR   -- WOM threshold Y axis (Bank 4)
//   0x4C  ACCEL_WOM_Z_THR   -- WOM threshold Z axis (Bank 4)
//   0x57  SMD_CONFIG        -- WOM / SMD mode enable
//   0x66  INT_SOURCE1       -- Routes WOM interrupt to physical INT1 pin
//   0x2D  INT_STATUS2       -- Read to clear latched WOM interrupt

inline void configureWOM(ICM_42627& sensor) {
  // 1. Ensure we are in Memory Bank 0 (Default)
  icm42627_write_reg(0x76, 0x00);

  // INT_CONFIG: Latched, push-pull, active-HIGH (0x07)
  icm42627_write_reg(0x14, 0x07);

  // 2. Switch to Memory Bank 4 to access APEX WOM Thresholds
  icm42627_write_reg(0x76, 0x04);
  
  icm42627_write_reg(0x4A, WOM_THRESHOLD_LSB); // ACCEL_WOM_X_THR
  icm42627_write_reg(0x4B, WOM_THRESHOLD_LSB); // ACCEL_WOM_Y_THR
  icm42627_write_reg(0x4C, WOM_THRESHOLD_LSB); // ACCEL_WOM_Z_THR

  // 3. Switch back to Memory Bank 0 for standard operations
  icm42627_write_reg(0x76, 0x00);

  // SMD_CONFIG: OR condition, compare to previous sample, WOM enabled (0x0D)
  icm42627_write_reg(0x57, 0x0D);

  // INT_SOURCE1: Route WOM X/Y/Z interrupt to physical INT1 pin (GPIO 39)
  icm42627_write_reg(0x66, 0x07); 

  // Read INT_STATUS2 to clear any stale latched interrupts before sleeping
  icm42627_read_reg(0x2D);
}