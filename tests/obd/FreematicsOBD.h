// Host stand-in for FreematicsOBD.h, first on the include path of the readPIDs checks: just enough COBD and
// CLink for the real obdwrapper.cpp. The co-processor is the test's scripted CLink.
#pragma once

#include <Arduino.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <map>
#include <string>
#include <vector>

#define OBD_TIMEOUT_SHORT 1000 /* ms */
#define PID_SPEED 0x0D
#define PID_AMBIENT_TEMP 0x46

struct HostSerial { int printf(const char*, ...) { return 0; } };
extern HostSerial Serial;
inline void delay(unsigned long) {}

class CLink {
public:
  virtual ~CLink() {}
  virtual int sendCommand(const char* cmd, char* buf, int bufsize, unsigned int timeout) { return 0; }
  virtual int receive(char* buffer, int bufsize, unsigned int timeout) { return 0; }
  virtual bool send(const char* str) { return false; }
};

class COBD {
public:
  void begin(CLink* link) { this->link = link; }
  bool init(std::vector<std::string>) { return false; }
  bool readPID(byte, int&) { return false; }
  byte dataMode = 1;
  CLink* link = 0;
protected:
  virtual void idleTasks() {}
};
