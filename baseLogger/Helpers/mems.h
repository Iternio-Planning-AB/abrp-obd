#pragma once
/*
  MEMS helpers - ICM-42627 motion sensor (accelerometer/IMU).
  Contains: the Point3D/memData containers and the sensor handle, bias filtering
  (calculateBias), the per-cycle read/motion computation (processMEMS), the boot-time
  calibration (calibrateMEMS), the boot soft-reset that clears WOM config persisted across
  deep sleep (softResetMEMS), and the movement detector the activity state machine polls
  (detectMovement).

  Extracted verbatim from logger.ino (definitions moved, logic unchanged). This file is
  #included by logger.ino into the single sketch translation unit - it is NOT a standalone
  compilation unit - so it shares logger.ino's globals and the config.h feature flags.
*/

/*
  Initialize values for motionsensors.
*/

#if ENABLE_MEMS
struct Point3D {
  float x{ 0 };
  float y{ 0 };
  float z{ 0 };
  Point3D(float x = 0.0f, float y = 0.0f, float z = 0.0f) : x(x), y(y), z(z) { }
  Point3D(float xyz[3]) : x(xyz[0]), y(xyz[1]), z(xyz[2]) { }
};

struct memData {
  float accSum[3] = { 0 };
  float acc[3] = { 0 };
  float gyr[3] = { 0 };
  float mag[3] = { 0 };
  Point3D accBias = { 0.0f };
  std::deque<Point3D> acc_bias_window;
  float motion = 0;
  uint8_t accCount = 0;
  int deviceTemp = 0;
} mem_data;

MEMS_I2C* mems = 0;
#endif

/*
  Read data from motionsensor
*/

#if ENABLE_MEMS

// High pass filter, averages 10 latest acceleration values

void calculateBias(Point3D new_acc) {

  Point3D accBias{ 0.0f,0.0f,0.0f };
  for (const auto& bias : mem_data.acc_bias_window) {
    accBias.x += bias.x;
    accBias.y += bias.y;
    accBias.z += bias.z;
  }
  mem_data.acc_bias_window.pop_back();
  mem_data.acc_bias_window.emplace_front(new_acc);

  auto size = mem_data.acc_bias_window.size();

  accBias.x /= size;
  accBias.y /= size;
  accBias.z /= size;
  mem_data.accBias = accBias;
}

//update mem_data

void processMEMS() {
  float temp;
  if (!mems->read(mem_data.acc, mem_data.gyr, mem_data.mag, &temp)) return;
  mem_data.deviceTemp = (int)temp;

  calculateBias(Point3D(mem_data.acc));

  mem_data.accSum[0] += mem_data.acc[0];
  mem_data.accSum[1] += mem_data.acc[1];
  mem_data.accSum[2] += mem_data.acc[2];
  mem_data.accCount++;

  if (mem_data.accCount) {
    mem_data.acc[0] = mem_data.accSum[0] / mem_data.accCount - mem_data.accBias.x;
    mem_data.acc[1] = mem_data.accSum[1] / mem_data.accCount - mem_data.accBias.y;
    mem_data.acc[2] = mem_data.accSum[2] / mem_data.accCount - mem_data.accBias.z;
#if PRINT_MEMS
    Serial.printf("[ACC] %.2f/%.2f/%.2f\n", mem_data.acc[0], mem_data.acc[1], mem_data.acc[2]);
    Serial.printf("[GYR] %.2f/%.2f/%.2f\n", mem_data.gyr[0], mem_data.gyr[1], mem_data.gyr[2]);
#endif
    // calculate motion
    mem_data.motion = 0;
    for (byte i = 0; i < 3; i++) {
      mem_data.motion += mem_data.acc[i] * mem_data.acc[i];
    }
#if PRINT_MEMS
    Serial.printf("Motion: %.2f\n", mem_data.motion);
#endif
  }
  mem_data.accSum[0] = 0;
  mem_data.accSum[1] = 0;
  mem_data.accSum[2] = 0;
  mem_data.accCount = 0;
}

//Initial calibration

void calibrateMEMS() {
  Point3D accBias = { 0.0f };
  int n;
  unsigned long t = millis();
  for (n = 0; millis() - t < 1000; n++) {
    float acc[3];
    if (!mems->read(acc)) continue;
    mem_data.accBias.x += acc[0];
    mem_data.accBias.y += acc[1];
    mem_data.accBias.z += acc[2];
    delay(10);
  }
  mem_data.accBias.x /= n;
  mem_data.accBias.y /= n;
  mem_data.accBias.z /= n;
  mem_data.acc_bias_window.resize(10, mem_data.accBias);
#if PRINT
  Serial.printf("ACC BIAS: %.2f/%.2f/%.2f\n", mem_data.accBias.x, mem_data.accBias.y, mem_data.accBias.z);
#endif
}
#endif

#if ENABLE_SLEEP && ENABLE_MEMS
/*
  The ICM-42627 keeps its register contents across ESP32 deep sleep, so the WOM
  config written before sleeping persists into the next boot. Soft-reset the sensor
  (DEVICE_CONFIG 0x11 bit0) on every boot to restore factory defaults before
  mems->begin() re-applies the normal config. I2C is installed manually here and
  torn down so begin() can re-install it cleanly.
*/
void softResetMEMS() {
  i2c_config_t conf = {};
  conf.mode             = I2C_MODE_MASTER;
  conf.sda_io_num       = 21;
  conf.scl_io_num       = 22;
  conf.sda_pullup_en    = GPIO_PULLUP_ENABLE;
  conf.scl_pullup_en    = GPIO_PULLUP_ENABLE;
  conf.master.clk_speed = 100000;
  i2c_param_config(I2C_NUM_0, &conf);
  i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
  icm42627_write_reg(0x11, 0x01);  // DEVICE_CONFIG: SOFT_RESET_CONFIG bit
  delay(20);                        // reset takes <1 ms; 20 ms is safe
  i2c_driver_delete(I2C_NUM_0);
  delay(5);
}
#endif

// Poll the accelerometer for up to `duration` ms and report whether motion was seen; refreshes
// the awake leash (last_active) on the first movement. Used by handleActivityState() every loop.
#if ENABLE_MEMS
bool detectMovement(unsigned long duration) {
  bool active = false;
  auto start = millis();
  while (millis() - start < duration) {
    processMEMS();
    if (mem_data.acc[0] > 0.1 || mem_data.acc[1] > 0.1 || mem_data.acc[2] > 0.1) {
      active = true;
      last_active = millis();
      break;
    }
  }
#if PRINT_MEMS
  Serial.printf("[ACC] %.2f/%.2f/%.2f\n", mem_data.acc[0], mem_data.acc[1], mem_data.acc[2]);
  Serial.printf("[GYR] %.2f/%.2f/%.2f\n", mem_data.gyr[0], mem_data.gyr[1], mem_data.gyr[2]);
  Serial.printf("[Motion] %.2f\n", mem_data.motion);
#endif
  return active;
}
#endif
