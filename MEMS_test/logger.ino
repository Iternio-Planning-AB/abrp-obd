/*
 * ==============================================================
 *  ICM-42627  Calibration + Verification Tool
 *  ESP32 + FreematicsMEMS
 * ==============================================================
 *
 *  Three activity profiles, each with a specific role:
 *
 *  1. IDLE
 *       Parked car, no motion.
 *       If IDLE for > IDLE_SLEEP_TIMEOUT_MS -> deep sleep.
 *
 *  2. DOOR_EVENT  (door open OR door slam)
 *       Primary WOM (Wake-on-Motion) trigger.
 *       Wakes ESP32 from deep sleep so the system is ready
 *       before the engine even starts.
 *       Calibrate with a GENTLE door open -- the WOM threshold
 *       must catch the softest expected event. A slam will
 *       always trigger it regardless.
 *
 *  3. DRIVING
 *       Vehicle in motion (sustained vibration).
 *       Keeps the system awake; prevents deep sleep while
 *       driving. Handles slow queues / long traffic stops:
 *       system stays awake as long as driving is detected
 *       within the idle timeout window.
 *
 *  WORKFLOW
 *    1.  Flash, open Serial Monitor at 115200 baud.
 *    2.  Press 1 -- record IDLE (parked, completely still).
 *    3.  Press 2 -- record DOOR_EVENT (gently open door 2-3x).
 *    4.  Press 3 -- record DRIVING (drive normally).
 *    5.  Press 't' -- verify calibration (test mode).
 *    6.  Press 'r' -- copy printed block to icm42627_cal.h.
 *
 *  I2C: SDA = GPIO21,  SCL = GPIO22
 * ==============================================================
 */

#include <Arduino.h>
#include <math.h>
#include "FreematicsMEMS.h"

// ---- Tunable parameters --------------------------------------
#define COUNTDOWN_SEC        3

// ICM-42627 WOM register addresses
#define REG_ACCEL_WOM_X_THR  0x44
#define REG_ACCEL_WOM_Y_THR  0x45
#define REG_ACCEL_WOM_Z_THR  0x46
#define REG_SMD_CONFIG       0x57
#define REG_INT_SOURCE0      0x65

// WOM LSB resolution: ACCEL_FS_SEL_2G -> 4 g / 256 = 15.625 mg/LSB
#define WOM_LSB_TO_G         0.015625f
#define G_TO_WOM_LSB(g)      ((uint8_t)constrain((int)((g) / WOM_LSB_TO_G), 1, 255))

// ---- Activity catalogue --------------------------------------
enum Activity {
  ACT_IDLE       = 0,
  ACT_DOOR_EVENT = 1,
  ACT_DRIVING    = 2,
  ACT_COUNT      = 3
};

// Recording duration per activity (ms)
static const uint32_t ACT_DURATION_MS[ACT_COUNT] = {
  10000,   // IDLE       -- 10 s for a stable noise floor
   5000,   // DOOR_EVENT -- 5 s, open/close door 2-3 times
   8000,   // DRIVING    -- 8 s of normal driving
};

static const char* ACT_NAME[ACT_COUNT] = {
  "IDLE",
  "DOOR_EVENT",
  "DRIVING"
};

static const char* ACT_LABEL[ACT_COUNT] = {
  "Idle",
  "Door Event",
  "Driving"
};

static const char* ACT_INSTRUCTIONS[ACT_COUNT] = {
  "Park the car. Sit completely still -- do NOT touch anything.",
  "Gently open the car door 2-3 times. No need to slam --\n"
  "  calibrate for the softest expected door motion so the WOM\n"
  "  threshold is sensitive enough to catch it every time.",
  "Drive the car at normal speed (any road is fine)."
};

// ---- Per-activity statistics ---------------------------------
struct ActivityStats {
  float peakNetMag;
  float meanNetMag;
  float stddevNetMag;
  float peakGyroDPS;
  int   samples;
  bool  recorded;
};

static ActivityStats stats[ACT_COUNT];

// ---- Window sample result ------------------------------------
struct WindowStats {
  float peakNet;
  float meanNet;
  float stddevNet;
  float peakGyro;
  int   samples;
};

// ---- Globals -------------------------------------------------
static ICM_42627 sensor;
static bool      sensorOK = false;
static bool      csvMode  = false;

// ==============================================================
//  Utility
// ==============================================================

static inline float vecMag(float x, float y, float z) {
  return sqrtf(x*x + y*y + z*z);
}

static void sep(char c = '-', int n = 62) {
  for (int i = 0; i < n; i++) Serial.print(c);
  Serial.println();
}

// ==============================================================
//  Menu
// ==============================================================

static void printMenu() {
  sep('=');
  Serial.println("  ICM-42627 Calibration + Verification Tool");
  sep('=');

  const char* roles[ACT_COUNT] = {
    "sleep trigger   (>5 min idle -> deep sleep)",
    "WOM wake trigger (door open/slam -> wake ESP32)",
    "keep-alive      (driving -> stay awake)"
  };
  for (int i = 0; i < ACT_COUNT; i++) {
    Serial.printf("  [%d] Record: %-14s  %-42s  %s\n",
      i + 1, ACT_LABEL[i], roles[i],
      stats[i].recorded ? "[done]" : "");
  }

  Serial.println();
  Serial.println("  [t] Test mode   -- verify before generating header");
  Serial.println("  [r] Generate    -- print icm42627_cal.h");
  Serial.printf( "  [c] CSV stream  -- toggle  (now: %s)\n", csvMode ? "ON" : "OFF");
  Serial.println("  [s] Live stream -- check sensor");
  Serial.println("  [x] Reset all data");
  sep();
  Serial.printf("  Sensor: %s\n",
    sensorOK ? "ICM-42627 OK" : "NOT FOUND -- check SDA=21 SCL=22");
  sep();
  Serial.print("  Choice: ");
}

// ==============================================================
//  Core window sampling -- shared by recording and test mode
// ==============================================================

static WindowStats sampleWindow(uint32_t durMs, bool showBar) {
  float  acc[3], gyr[3], tmp;
  double sumMag = 0, sumMagSq = 0;
  float  peakNet = 0, peakGyro = 0;
  int    n = 0;

  uint32_t t0    = millis();
  uint32_t tNext = t0;

  while (millis() - t0 < durMs) {
    uint32_t now = millis();
    if (now < tNext) continue;
    tNext += 10;   // 100 Hz

    if (!sensor.read(acc, gyr, nullptr, &tmp, nullptr)) continue;

    float accelMag = vecMag(acc[0], acc[1], acc[2]);
    float netMag   = fabsf(accelMag - 1.0f);
    float gyrMag   = vecMag(gyr[0], gyr[1], gyr[2]);

    sumMag   += netMag;
    sumMagSq += (double)netMag * netMag;
    if (netMag  > peakNet)  peakNet  = netMag;
    if (gyrMag  > peakGyro) peakGyro = gyrMag;
    n++;

    if (showBar) {
      if (csvMode) {
        Serial.printf("%lu,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f,%.4f,%.4f,%.2f\n",
          now - t0, acc[0], acc[1], acc[2], gyr[0], gyr[1], gyr[2],
          accelMag, netMag, gyrMag);
      } else if (n % 10 == 0) {
        int bars = (int)(netMag * 30);
        Serial.printf("  %5d ms |", (int)(now - t0));
        for (int b = 0; b < min(bars, 30); b++) Serial.print('#');
        Serial.printf("  %.3fg  \r", netMag);
      }
    }
  }
  if (showBar && !csvMode) Serial.println();

  float mean = n > 0 ? (float)(sumMag / n) : 0.0f;
  float var  = n > 0 ? (float)(sumMagSq / n) - mean * mean : 0.0f;

  WindowStats ws;
  ws.peakNet   = peakNet;
  ws.meanNet   = mean;
  ws.stddevNet = sqrtf(var > 0 ? var : 0.0f);
  ws.peakGyro  = peakGyro;
  ws.samples   = n;
  return ws;
}

// ==============================================================
//  Activity recording
// ==============================================================

static void recordActivity(int idx) {
  if (!sensorOK) { Serial.println("  Sensor not ready."); return; }

  Serial.println();
  sep('*');
  Serial.printf("  Recording : %s\n", ACT_LABEL[idx]);
  Serial.printf("  Duration  : %.0f s at 100 Hz\n", ACT_DURATION_MS[idx] / 1000.0f);
  sep('*');
  Serial.printf("  %s\n\n", ACT_INSTRUCTIONS[idx]);

  if (csvMode) {
    Serial.println("ms,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps,accel_mag_g,net_mag_g,gyro_mag_dps");
  }

  WindowStats w = sampleWindow(ACT_DURATION_MS[idx], true);
  if (w.samples == 0) { Serial.println("  No samples!"); return; }

  stats[idx].peakNetMag   = w.peakNet;
  stats[idx].meanNetMag   = w.meanNet;
  stats[idx].stddevNetMag = w.stddevNet;
  stats[idx].peakGyroDPS  = w.peakGyro;
  stats[idx].samples      = w.samples;
  stats[idx].recorded     = true;

  sep();
  Serial.printf("  %-16s  %d samples\n", ACT_LABEL[idx], w.samples);
  Serial.printf("  Net-accel:  peak=%.4fg   mean=%.4fg   stddev=%.4fg\n",
    w.peakNet, w.meanNet, w.stddevNet);
  Serial.printf("  Gyro:       peak=%.2f deg/s\n", w.peakGyro);

  if (idx == ACT_IDLE) {
    float noise3s = w.meanNet + 3.0f * w.stddevNet;
    Serial.printf("  Noise floor (mean+3s): %.4fg -- WOM must sit above this.\n", noise3s);
  }
  if (idx == ACT_DOOR_EVENT && stats[ACT_IDLE].recorded) {
    float noise3s = stats[ACT_IDLE].meanNetMag + 3.0f * stats[ACT_IDLE].stddevNetMag;
    float snr     = w.peakNet / max(noise3s, 0.001f);
    Serial.printf("  SNR vs idle noise floor: %.1fx", snr);
    if (snr < 3.0f)
      Serial.println(" -- [!] Low SNR. Try in a quieter environment or re-record idle.");
    else
      Serial.println(" -- [OK]");
  }
  if (idx == ACT_DRIVING && stats[ACT_IDLE].recorded) {
    float noise3s = stats[ACT_IDLE].meanNetMag + 3.0f * stats[ACT_IDLE].stddevNetMag;
    Serial.printf("  Driving mean is %.1fx above idle noise floor.\n",
      w.meanNet / max(noise3s, 0.001f));
    if (w.meanNet < noise3s * 1.5f)
      Serial.println("  [!] Driving profile barely above idle -- sensor may not distinguish them.");
  }
  sep();
}

// ==============================================================
//  Activity detection engine
// ==============================================================

// NOTE: gyro is intentionally excluded from detection.
// The ICM-42627 gyro has a large uncalibrated DC bias (~193 deg/s at rest)
// making all three activities look nearly identical in the gyro axis.
// Accel peak and mean are sufficient to separate IDLE / DOOR_EVENT / DRIVING.
static void featureRanges(float& pR, float& mR) {
  float minPk=1e9f, maxPk=-1e9f;
  float minMn=1e9f, maxMn=-1e9f;
  for (int i = 0; i < ACT_COUNT; i++) {   // include IDLE (i=0)
    if (!stats[i].recorded) continue;
    if (stats[i].peakNetMag < minPk) minPk = stats[i].peakNetMag;
    if (stats[i].peakNetMag > maxPk) maxPk = stats[i].peakNetMag;
    if (stats[i].meanNetMag < minMn) minMn = stats[i].meanNetMag;
    if (stats[i].meanNetMag > maxMn) maxMn = stats[i].meanNetMag;
  }
  // Use a tight minimum floor so IDLE's tiny values are still discriminated
  pR = (maxPk - minPk) < 0.001f ? 0.001f : maxPk - minPk;
  mR = (maxMn - minMn) < 0.001f ? 0.001f : maxMn - minMn;
}

// Normalised Euclidean distance in (peak, mean) space.
// Feature weights: peak 60% | mean 40%
// Gyro excluded -- see featureRanges() comment above.
// IDLE is included in the comparison (i=0), fixing the bug where the
// detector could never return IDLE from the distance loop.
static int detectActivity(const WindowStats& w, float* confidence) {
  if (!stats[ACT_IDLE].recorded) {
    if (confidence) *confidence = 0;
    return ACT_IDLE;
  }

  float pR, mR;
  featureRanges(pR, mR);   // ranges include IDLE

  float dist[ACT_COUNT];
  for (int i = 0; i < ACT_COUNT; i++) dist[i] = 1e9f;

  int   bestIdx  = ACT_IDLE;
  float bestDist = 1e9f;

  for (int i = 0; i < ACT_COUNT; i++) {   // start at 0 -- IDLE is a valid candidate
    if (!stats[i].recorded) continue;
    float dp = (w.peakNet - stats[i].peakNetMag) / pR;
    float dm = (w.meanNet - stats[i].meanNetMag) / mR;
    dist[i]  = sqrtf(dp*dp * 0.60f + dm*dm * 0.40f);
    if (dist[i] < bestDist) { bestDist = dist[i]; bestIdx = i; }
  }

  if (confidence) {
    float sumAff = 0;
    for (int i = 0; i < ACT_COUNT; i++) {   // include IDLE in softmax
      if (!stats[i].recorded) continue;
      sumAff += expf(-dist[i] * 4.0f);
    }
    float bestAff = expf(-bestDist * 4.0f);
    *confidence = (sumAff > 0) ? min(bestAff / sumAff * 100.0f, 99.9f) : 50.0f;
  }

  return bestIdx;
}

// ==============================================================
//  Test mode
// ==============================================================

static bool runTestRound(int roundNum, int totalRounds, int expectedIdx) {
  Serial.println();
  sep('=');
  Serial.printf("  ROUND %d / %d  --  %s\n", roundNum, totalRounds, ACT_LABEL[expectedIdx]);
  sep('=');
  Serial.printf("  %s\n\n", ACT_INSTRUCTIONS[expectedIdx]);

  for (int i = COUNTDOWN_SEC; i >= 1; i--) {
    Serial.printf("  Get ready...  %d  \r", i);
    delay(1000);
  }
  Serial.println("  >>> GO! <<<              ");
  Serial.println();

  WindowStats w = sampleWindow(ACT_DURATION_MS[expectedIdx], true);
  if (w.samples == 0) {
    Serial.println("  No samples -- skipping.");
    return false;
  }

  float confidence;
  int   detectedIdx = detectActivity(w, &confidence);
  bool  correct     = (detectedIdx == expectedIdx);

  sep();
  Serial.printf("  Expected : %s\n", ACT_LABEL[expectedIdx]);
  Serial.printf("  Detected : %-16s  %s  (confidence: %.0f%%)\n",
    ACT_LABEL[detectedIdx],
    correct ? "CORRECT [OK]" : "WRONG   [!!]",
    confidence);
  Serial.printf("  Measured : peak=%.4fg   mean=%.4fg   gyro=%.2f deg/s\n",
    w.peakNet, w.meanNet, w.peakGyro);

  if (!correct) {
    float pR, mR;
    featureRanges(pR, mR);

    Serial.println("\n  Distance scores (lower = closer match):");
    for (int i = 0; i < ACT_COUNT; i++) {   // include IDLE
      if (!stats[i].recorded) continue;
      float dp = (w.peakNet - stats[i].peakNetMag) / pR;
      float dm = (w.meanNet - stats[i].meanNetMag) / mR;
      float d  = sqrtf(dp*dp*0.6f + dm*dm*0.4f);
      Serial.printf("    %-16s  d=%.3f%s\n", ACT_LABEL[i], d,
        i == detectedIdx  ? "  <- detected"  :
        i == expectedIdx  ? "  <- expected"  : "");
    }
    Serial.println();

    if (expectedIdx == ACT_DOOR_EVENT && detectedIdx == ACT_DRIVING) {
      Serial.println("  [!] Door event looks like driving -- motion too gentle.");
      Serial.println("      Open/close the door a bit more firmly during recording.");
    } else if (expectedIdx == ACT_DOOR_EVENT && detectedIdx == ACT_IDLE) {
      Serial.println("  [!] Door event not rising above idle noise floor.");
      Serial.println("      Re-record idle in a quieter environment, or open the");
      Serial.println("      door more decisively during door event recording.");
    } else if (expectedIdx == ACT_DRIVING && detectedIdx == ACT_IDLE) {
      Serial.println("  [!] Driving profile too close to idle.");
      Serial.println("      Try a rougher road, or re-record idle in the parked car.");
    } else if (expectedIdx == ACT_DRIVING && detectedIdx == ACT_DOOR_EVENT) {
      Serial.println("  [!] Driving looks like a door event -- road is very bumpy.");
      Serial.println("      Re-record driving on a smoother road.");
    }
  }
  sep();
  return correct;
}

static void testMode() {
  if (!stats[ACT_IDLE].recorded ||
      !stats[ACT_DOOR_EVENT].recorded ||
      !stats[ACT_DRIVING].recorded) {
    Serial.println("\n  Record ALL three activities first (1, 2, 3) before testing.\n");
    return;
  }

  sep('=');
  Serial.println("  TEST MODE");
  sep('=');
  Serial.println("  Tests all 3 activities in random order.");
  Serial.printf( "  Countdown: %d s  |  Recording window per activity as configured.\n\n",
    COUNTDOWN_SEC);
  Serial.println("  [a] Run all 3 tests");
  Serial.println("  [1] Test Idle only");
  Serial.println("  [2] Test Door Event only");
  Serial.println("  [3] Test Driving only");
  Serial.println("  [b] Back");
  sep();
  Serial.print("  Choice: ");

  while (!Serial.available()) delay(10);
  char ch = (char)Serial.read();
  while (Serial.available()) Serial.read();
  Serial.println(ch);

  int rounds[ACT_COUNT];
  int roundCount = 0;

  if (ch == 'a' || ch == 'A') {
    rounds[0] = ACT_IDLE;
    rounds[1] = ACT_DOOR_EVENT;
    rounds[2] = ACT_DRIVING;
    roundCount = 3;
    // Fisher-Yates shuffle
    for (int i = roundCount - 1; i > 0; i--) {
      int j = random(0, i + 1);
      int t = rounds[i]; rounds[i] = rounds[j]; rounds[j] = t;
    }
  } else if (ch >= '1' && ch <= '3') {
    rounds[0] = ch - '1';
    roundCount = 1;
  } else if (ch == 'b' || ch == 'B') {
    return;
  } else {
    Serial.println("  Invalid choice.");
    return;
  }

  bool results[ACT_COUNT];
  for (int i = 0; i < ACT_COUNT; i++) results[i] = false;
  int correct = 0;

  for (int r = 0; r < roundCount; r++) {
    bool ok = runTestRound(r + 1, roundCount, rounds[r]);
    results[r] = ok;
    if (ok) correct++;
    if (r < roundCount - 1) {
      Serial.println("  Press any key for next round...");
      while (!Serial.available()) delay(10);
      while (Serial.available()) Serial.read();
    }
  }

  // Final report
  Serial.println();
  sep('=');
  Serial.println("  TEST RESULTS");
  sep('=');
  for (int r = 0; r < roundCount; r++) {
    Serial.printf("  %-16s  %s\n",
      ACT_LABEL[rounds[r]], results[r] ? "[PASS]" : "[FAIL]");
  }
  sep();
  int pct = roundCount > 0 ? correct * 100 / roundCount : 0;
  Serial.printf("  Score: %d / %d  (%d%%)\n\n", correct, roundCount, pct);

  if (pct == 100) {
    Serial.println("  All tests passed. Calibration verified.");
    Serial.println("  Press 'r' to generate icm42627_cal.h.");
  } else if (pct >= 67) {
    Serial.println("  Most tests passed. Re-record failed activities,");
    Serial.println("  then re-test before generating the header.");
  } else {
    Serial.println("  Too many failures. Common causes:");
    Serial.println("  - IDLE recorded indoors vs activities in car -- redo in car.");
    Serial.println("  - Door and driving profiles too similar -- check sensor mount.");
    Serial.println("  - Sensor loose / not rigidly attached to car body.");
  }

  // Warn if any two profiles are too close
  float pR, mR;
  featureRanges(pR, mR);
  for (int i = 1; i < ACT_COUNT; i++) {   // only check non-idle pairs
    if (!stats[i].recorded) continue;
    for (int j = i + 1; j < ACT_COUNT; j++) {
      if (!stats[j].recorded) continue;
      float dp = (stats[i].peakNetMag - stats[j].peakNetMag) / pR;
      float dm = (stats[i].meanNetMag - stats[j].meanNetMag) / mR;
      float d  = sqrtf(dp*dp*0.6f + dm*dm*0.4f);
      if (d < 0.15f) {
        Serial.printf("\n  [!] '%s' and '%s' profiles are very similar (d=%.2f).\n",
          ACT_LABEL[i], ACT_LABEL[j], d);
        Serial.println("      Re-record in more distinct conditions.");
      }
    }
  }
  sep('=');
}

// ==============================================================
//  Live stream
// ==============================================================

static void liveStream() {
  if (!sensorOK) { Serial.println("  Sensor not ready."); return; }
  Serial.println("\n  Streaming -- press any key to stop\n");
  Serial.println("  ax_g    ay_g    az_g   |  gx/s    gy/s    gz/s   | mag_g   net_g");
  sep();
  float acc[3], gyr[3];
  while (!Serial.available()) {
    if (sensor.read(acc, gyr, nullptr, nullptr, nullptr)) {
      float mag = vecMag(acc[0], acc[1], acc[2]);
      Serial.printf("  %6.3f  %6.3f  %6.3f | %7.2f  %7.2f  %7.2f | %.4f  %.4f\n",
        acc[0], acc[1], acc[2], gyr[0], gyr[1], gyr[2],
        mag, fabsf(mag - 1.0f));
    }
    delay(10);
  }
  while (Serial.available()) Serial.read();
  Serial.println("\n  Stream stopped.");
}

// ==============================================================
//  Header generator  ->  icm42627_cal.h
// ==============================================================

static void generateHeader() {
  if (!stats[ACT_IDLE].recorded ||
      !stats[ACT_DOOR_EVENT].recorded ||
      !stats[ACT_DRIVING].recorded) {
    Serial.println("\n  Record all three activities (1, 2, 3) first.\n");
    return;
  }

  float noise3s  = stats[ACT_IDLE].meanNetMag + 3.0f * stats[ACT_IDLE].stddevNetMag;
  float doorPeak = stats[ACT_DOOR_EVENT].peakNetMag;
  float drivMean = stats[ACT_DRIVING].meanNetMag;
  float womThrG  = (noise3s + doorPeak) * 0.5f;
  uint8_t womLSB = G_TO_WOM_LSB(womThrG);
  float drivThrG = (noise3s + drivMean) * 0.5f;

  sep('=');
  Serial.println("  CALIBRATION SUMMARY");
  sep('=');
  Serial.printf("  Idle noise floor  (mean+3s) : %.4fg\n",  noise3s);
  Serial.printf("  Door event peak             : %.4fg   SNR = %.1fx\n",
    doorPeak, doorPeak / max(noise3s, 0.001f));
  Serial.printf("  WOM threshold (door)        : %.4fg = %d (0x%02X) LSB\n",
    womThrG, womLSB, womLSB);
  Serial.printf("  Driving mean                : %.4fg\n",  drivMean);
  Serial.printf("  Driving detect threshold    : %.4fg\n",  drivThrG);
  sep();

  Serial.println();
  sep('=');
  Serial.println("  COPY BELOW INTO  icm42627_cal.h  IN YOUR MAIN PROJECT");
  sep('=');
  Serial.println();

  Serial.println("#pragma once");
  Serial.println("#include <stdint.h>");
  Serial.println("#include \"FreematicsMEMS.h\"");
  Serial.println();
  Serial.println("// ==============================================================");
  Serial.println("//  ICM-42627 Calibration Data -- icm42627_cal.h");
  Serial.println("//  Calibrate once, reuse on all devices.");
  Serial.println("//");
  Serial.println("//  System behaviour:");
  Serial.println("//    IDLE > IDLE_SLEEP_TIMEOUT_MS  ->  deep sleep");
  Serial.println("//    Door open/slam (WOM interrupt) ->  wake from deep sleep");
  Serial.println("//    Driving detected               ->  stay awake, reset idle timer");
  Serial.println("//");
  Serial.println("//  Sensor config: ACCEL +/-2g / GYRO +/-250dps");
  Serial.printf( "//  WOM LSB: %.4f g\n", WOM_LSB_TO_G);
  Serial.println("// ==============================================================");
  Serial.println();

  Serial.println("// -- Sleep policy ----------------------------------------");
  Serial.println("#define IDLE_SLEEP_TIMEOUT_MS   (5UL * 60UL * 1000UL)  // 5 minutes");
  Serial.println();

  Serial.println("// -- Idle noise floor ------------------------------------");
  Serial.printf( "#define CAL_IDLE_MEAN_G          %.4ff\n", stats[ACT_IDLE].meanNetMag);
  Serial.printf( "#define CAL_IDLE_STDDEV_G        %.4ff\n", stats[ACT_IDLE].stddevNetMag);
  Serial.printf( "#define CAL_IDLE_NOISE_FLOOR_G   %.4ff   // mean + 3 sigma\n\n", noise3s);

  Serial.println("// -- Door event (WOM wake trigger) -----------------------");
  Serial.println("// WOM is calibrated for a GENTLE door open.");
  Serial.println("// A door slam will always exceed this threshold.");
  Serial.printf( "#define CAL_DOOR_PEAK_G          %.4ff\n", doorPeak);
  Serial.printf( "#define CAL_DOOR_GYRO_DPS        %.2ff\n",
    stats[ACT_DOOR_EVENT].peakGyroDPS);
  Serial.printf( "#define WOM_THRESHOLD_G          %.4ff   // midpoint: noise <-> door peak\n",
    womThrG);
  Serial.printf( "#define WOM_THRESHOLD_LSB        %-3d     // 0x%02X\n\n", womLSB, womLSB);

  Serial.println("// -- Driving (keep-alive signal) -------------------------");
  Serial.println("// While net-accel mean stays above DRIVING_THRESHOLD_G the");
  Serial.println("// system resets its idle timer and will NOT go to deep sleep.");
  Serial.printf( "#define CAL_DRIVING_PEAK_G       %.4ff\n",
    stats[ACT_DRIVING].peakNetMag);
  Serial.printf( "#define CAL_DRIVING_MEAN_G       %.4ff\n",  drivMean);
  Serial.printf( "#define CAL_DRIVING_GYRO_DPS     %.2ff\n",
    stats[ACT_DRIVING].peakGyroDPS);
  Serial.printf( "#define DRIVING_THRESHOLD_G      %.4ff   // midpoint: noise <-> driving mean\n\n",
    drivThrG);

  Serial.println("// -- State helpers ----------------------------------------");
  Serial.println("// isDriving: pass a rolling mean of net-accel over ~1 second.");
  Serial.println("inline bool isDriving(float rollingMeanNetG) {");
  Serial.println("  return rollingMeanNetG > DRIVING_THRESHOLD_G;");
  Serial.println("}\n");
  Serial.println("// isIdle: peak net-accel in the last sample window is below noise.");
  Serial.println("inline bool isIdle(float peakNetG) {");
  Serial.println("  return peakNetG < CAL_IDLE_NOISE_FLOOR_G;");
  Serial.println("}\n");
  Serial.println("// matchActivity: nearest-neighbour classifier using accel only.");
  Serial.println("// Gyro is excluded -- ICM-42627 has a large uncalibrated DC bias");
  Serial.println("// making gyro readings unreliable for classification.");
  Serial.println("// Returns: ACT_IDLE(-1), ACT_DOOR(0), ACT_DRIVING(1)");
  Serial.println("#define ACT_MATCH_IDLE    -1");
  Serial.println("#define ACT_MATCH_DOOR     0");
  Serial.println("#define ACT_MATCH_DRIVING  1");
  Serial.println("inline int matchActivity(float peakNetG, float meanNetG) {");
  Serial.printf( "  const float profiles[2][2] = { {%.4ff,%.4ff}, {%.4ff,%.4ff} };\n",
    stats[ACT_DOOR_EVENT].peakNetMag, stats[ACT_DOOR_EVENT].meanNetMag,
    stats[ACT_DRIVING].peakNetMag,   stats[ACT_DRIVING].meanNetMag);
  Serial.printf( "  const float idlePeak = %.4ff, idleMean = %.4ff;\n",
    stats[ACT_IDLE].peakNetMag, stats[ACT_IDLE].meanNetMag);
  Serial.printf( "  const float pR = %.4ff, mR = %.4ff;  // feature ranges\n",
    max(stats[ACT_DOOR_EVENT].peakNetMag, stats[ACT_DRIVING].peakNetMag)
    - stats[ACT_IDLE].peakNetMag,
    max(stats[ACT_DOOR_EVENT].meanNetMag, stats[ACT_DRIVING].meanNetMag)
    - stats[ACT_IDLE].meanNetMag);
  Serial.println("  // Distance to IDLE");
  Serial.println("  float dIdlePk=(peakNetG-idlePeak)/pR, dIdleMn=(meanNetG-idleMean)/mR;");
  Serial.println("  float dIdle=sqrtf(dIdlePk*dIdlePk*0.6f+dIdleMn*dIdleMn*0.4f);");
  Serial.println("  // Distance to each activity profile");
  Serial.println("  float best=dIdle; int bestIdx=ACT_MATCH_IDLE;");
  Serial.println("  for(int i=0;i<2;i++){");
  Serial.println("    float dp=(peakNetG-profiles[i][0])/pR,dm=(meanNetG-profiles[i][1])/mR;");
  Serial.println("    float d=sqrtf(dp*dp*0.6f+dm*dm*0.4f);");
  Serial.println("    if(d<best){best=d;bestIdx=i;}");
  Serial.println("  }");
  Serial.println("  return bestIdx;");
  Serial.println("}\n");

  Serial.println("// -- WOM hardware setup ----------------------------------");
  Serial.println("// Call once after sensor.begin() in your main project setup().");
  Serial.println("// Triggers when any axis change exceeds WOM_THRESHOLD_G.");
  Serial.println("inline void configureWOM(ICM_42627& sensor) {");
  Serial.printf( "  sensor.writeByte(0x%02X, WOM_THRESHOLD_LSB);  // ACCEL_WOM_X_THR\n",
    REG_ACCEL_WOM_X_THR);
  Serial.printf( "  sensor.writeByte(0x%02X, WOM_THRESHOLD_LSB);  // ACCEL_WOM_Y_THR\n",
    REG_ACCEL_WOM_Y_THR);
  Serial.printf( "  sensor.writeByte(0x%02X, WOM_THRESHOLD_LSB);  // ACCEL_WOM_Z_THR\n",
    REG_ACCEL_WOM_Z_THR);
  Serial.println("  sensor.writeByte(0x57, 0x09);  // SMD_CONFIG: compare-prev-sample + WOM_EN");
  Serial.println("  sensor.writeByte(0x65, 0x07);  // INT_SOURCE0: WOM X/Y/Z interrupt enable");
  Serial.println("}");

  Serial.println();
  sep('=');
  Serial.println("  END OF icm42627_cal.h");
  sep('=');
  Serial.println();
}

// ==============================================================
//  setup / loop
// ==============================================================

void setup() {
  Serial.begin(115200);
  randomSeed(analogRead(0));
  delay(800);

  Serial.println("\n");
  sep('=');
  Serial.println("  ICM-42627 Calibration + Verification Tool");
  sep('=');
  memset(stats, 0, sizeof(stats));

  if (sensor.begin(false)) {
    sensorOK = true;
    Serial.println("  Sensor: OK");
    float acc[3], gyr[3], tmp;
    sensor.read(acc, gyr, nullptr, &tmp, nullptr);
    Serial.printf("  Live check -- a=[%.2f, %.2f, %.2f]g  T=%.1fC\n",
      acc[0], acc[1], acc[2], tmp);
  } else {
    Serial.println("  ERROR: Sensor not found. Check SDA=21 SCL=22.");
  }

  printMenu();
}

void loop() {
  if (!Serial.available()) return;
  char ch = (char)Serial.read();
  while (Serial.available()) Serial.read();
  Serial.println(ch);

  if (ch >= '1' && ch <= '3') {
    recordActivity(ch - '1');
  } else {
    switch (ch) {
      case 't': case 'T': testMode();       break;
      case 'r': case 'R': generateHeader(); break;
      case 'c': case 'C':
        csvMode = !csvMode;
        Serial.printf("  CSV mode: %s\n", csvMode ? "ON" : "OFF"); break;
      case 's': case 'S': liveStream();     break;
      case 'x': case 'X':
        memset(stats, 0, sizeof(stats));
        Serial.println("  All data cleared."); break;
      default:
        if (ch != '\n' && ch != '\r') Serial.println("  Unknown command.");
    }
  }
  printMenu();
}
