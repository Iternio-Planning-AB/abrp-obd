
#pragma once
#include <stdint.h>
#include "FreematicsMEMS.h"

// ==============================================================
//  ICM-42627 Calibration Data -- icm42627_cal.h
//  Calibrate once, reuse on all devices.
//
//  System behaviour:
//    IDLE > IDLE_SLEEP_TIMEOUT_MS  ->  deep sleep
//    Door open/slam (WOM interrupt) ->  wake from deep sleep
//    Driving detected               ->  stay awake, reset idle timer
//
//  Sensor config: ACCEL +/-2g / GYRO +/-250dps
//  WOM LSB: 0.0156 g
// ==============================================================

// -- Sleep policy ----------------------------------------
#define IDLE_SLEEP_TIMEOUT_MS   (5UL * 60UL * 1000UL)  // 5 minutes

// -- Idle noise floor ------------------------------------
#define CAL_IDLE_MEAN_G          0.0027f
#define CAL_IDLE_STDDEV_G        0.0024f
#define CAL_IDLE_NOISE_FLOOR_G   0.0099f   // mean + 3 sigma

// -- Door event (WOM wake trigger) -----------------------
// WOM is calibrated for a GENTLE door open.
// A door slam will always exceed this threshold.
#define CAL_DOOR_PEAK_G          0.3066f
#define CAL_DOOR_GYRO_DPS        200.60f
#define WOM_THRESHOLD_G          0.1582f   // midpoint: noise <-> door peak
#define WOM_THRESHOLD_LSB        10      // 0x0A

// -- Driving (keep-alive signal) -------------------------
// While net-accel mean stays above DRIVING_THRESHOLD_G the
// system resets its idle timer and will NOT go to deep sleep.
#define CAL_DRIVING_PEAK_G       0.1636f
#define CAL_DRIVING_MEAN_G       0.0205f
#define CAL_DRIVING_GYRO_DPS     199.56f
#define DRIVING_THRESHOLD_G      0.0152f   // midpoint: noise <-> driving mean

// -- State helpers ----------------------------------------
// isDriving: pass a rolling mean of net-accel over ~1 second.
inline bool isDriving(float rollingMeanNetG) {
  return rollingMeanNetG > DRIVING_THRESHOLD_G;
}

// isIdle: peak net-accel in the last sample window is below noise.
inline bool isIdle(float peakNetG) {
  return peakNetG < CAL_IDLE_NOISE_FLOOR_G;
}

// matchActivity: nearest-neighbour classifier using accel only.
// Gyro is excluded -- ICM-42627 has a large uncalibrated DC bias
// making gyro readings unreliable for classification.
// Returns: ACT_IDLE(-1), ACT_DOOR(0), ACT_DRIVING(1)
#define ACT_MATCH_IDLE    -1
#define ACT_MATCH_DOOR     0
#define ACT_MATCH_DRIVING  1
inline int matchActivity(float peakNetG, float meanNetG) {
  const float profiles[2][2] = { {0.3066f,0.0159f}, {0.1636f,0.0205f} };
  const float idlePeak = 0.0241f, idleMean = 0.0027f;
  const float pR = 0.2824f, mR = 0.0179f;  // feature ranges
  // Distance to IDLE
  float dIdlePk=(peakNetG-idlePeak)/pR, dIdleMn=(meanNetG-idleMean)/mR;
  float dIdle=sqrtf(dIdlePk*dIdlePk*0.6f+dIdleMn*dIdleMn*0.4f);
  // Distance to each activity profile
  float best=dIdle; int bestIdx=ACT_MATCH_IDLE;
  for(int i=0;i<2;i++){
    float dp=(peakNetG-profiles[i][0])/pR,dm=(meanNetG-profiles[i][1])/mR;
    float d=sqrtf(dp*dp*0.6f+dm*dm*0.4f);
    if(d<best){best=d;bestIdx=i;}
  }
  return bestIdx;
}

// -- WOM hardware setup ----------------------------------
// Call once after sensor.begin() in your main project setup().
// Triggers when any axis change exceeds WOM_THRESHOLD_G.
inline void configureWOM(ICM_42627& sensor) {
  sensor.writeByte(0x44, WOM_THRESHOLD_LSB);  // ACCEL_WOM_X_THR
  sensor.writeByte(0x45, WOM_THRESHOLD_LSB);  // ACCEL_WOM_Y_THR
  sensor.writeByte(0x46, WOM_THRESHOLD_LSB);  // ACCEL_WOM_Z_THR
  sensor.writeByte(0x57, 0x09);  // SMD_CONFIG: compare-prev-sample + WOM_EN
  sensor.writeByte(0x65, 0x07);  // INT_SOURCE0: WOM X/Y/Z interrupt enable
}