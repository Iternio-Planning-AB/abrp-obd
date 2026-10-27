#pragma once
/*
  Telemetry payload formatting - serializers from OBDVector frames to the wire formats.
  Contains: logString (single-frame {"tlm":...} for the API), bleTlmFrame/bleTlmString (the
  BLE ABD1 frame, fitted to the notify size), bulkString (the legacy v1 /1/tlm/bulk body) and
  bulkStringV2 (the v2 /2/app/tlm InputPointList, which is what actually goes out - see
  TLM_V2_PATH in config.h).

  Extracted verbatim from logger.ino (definitions moved, logic unchanged). This file is
  #included by logger.ino into the single sketch translation unit - it is NOT a standalone
  compilation unit - so it shares logger.ino's globals and the config.h feature flags.
*/

/*
  Convert vector of PID results to suitable form for iternio telemetry API
*/

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <cstring>

bool isGeoMetaKey(const std::string& key) { return key.compare(0, 6, "__geo_") == 0; }

bool isGeoValueKey(const std::string& key) { return key == "lat" || key == "lon" || key == "heading"; }

// The ABD1 seq a queued frame was relayed under, so the app's ack can take it off the queue (Helpers/ble_relay.h,
// #90). Seqs restart every wake, so it is never written to the backlog, and the v2 body never carries it.
static const char* const RELAY_SEQ_KEY = "__relay_seq";
bool isRelaySeqKey(const std::string& key) { return key == RELAY_SEQ_KEY; }

void scrubGeoMetadata(OBDVector& frame) {
  for (auto it = frame.begin(); it != frame.end();) {
    if (isGeoMetaKey(it->first)) it = frame.erase(it);
    else ++it;
  }
}

bool geoValidUtc(double value, double frame_utc) {
  return std::isfinite(value) && value >= 978307200.0 && value <= 4294967295.0 &&
         std::floor(value) == value && value <= frame_utc;
}

bool geoEligibleAtSend(double value, double frame_utc, double send_utc) {
  if (!geoValidUtc(value, frame_utc) || value > CLOCK_MAX_EPOCH) return false;
  return send_utc > 1000000000.0 && send_utc <= CLOCK_MAX_EPOCH && geoValidUtc(send_utc, send_utc) &&
         send_utc - value <= GEO_MAX_SEND_AGE_SECONDS;
}

bool geoValidLocation(double lat, double lon) {
  return std::isfinite(lat) && std::isfinite(lon) && lat >= -90 && lat <= 90 &&
         lon >= -180 && lon <= 180;
}

std::string logString(OBDVector vec) {
  json tlm = json::object(); // force a JSON object so an empty frame serializes to {} and never the literal null
  std::stringstream ss;

  for (auto res : vec) {
    if (!isGeoMetaKey(res.first) && !isGeoValueKey(res.first)) tlm[res.first] = res.second;
  }
  ss << "{\"tlm\":" << tlm.dump() << "}";
  return ss.str();
}

// BLE telemetry frame for characteristic ABD1, {"tlm":{...}}. Measurements only: the phone supplies
// utc, location and heading itself. Keys align to the app's VehicleJsonTelemetryKeys; is_charging/
// is_parked/is_dcfc are JSON booleans. Numbers are %.2f with trailing zeros dropped: the app's
// effective 2 dp contract, same value, shorter text. A non-finite value is omitted (it is not JSON), and
// so is a key outside [A-Za-z0-9_], which could otherwise break out of its string.
//
// A notify carries at most MTU-3 bytes and the stack truncates the rest, which the app sees as broken
// JSON. So the notify gets a frame trimmed to fit, by the priority below, and the readable value stays
// the full frame: idle Android sits at MTU 23 and reads (a long read, not MTU-bound) after each notify.
// At 20 bytes the notify may be the bare {"tlm":{}}; it is still the cue for that read.
struct BleTlmFrame {
  std::string full;   // readable value of ABD1
  std::string notify; // fits the budget
  int dropped;        // fields left out of notify
};

// MTU-3, inside 23 (unnegotiated) .. 247 (our setMTU ceiling: larger notifications are lost).
size_t bleNotifyBudget(uint16_t mtu) {
  return (size_t)std::min<uint16_t>(std::max<uint16_t>(mtu, 23), 247) - 3;
}

static bool bleTlmNumber(double value, char* out, size_t size) {
  if (!std::isfinite(value)) return false;
  const int n = snprintf(out, size, "%.2f", value);
  if (n < 0 || (size_t)n >= size) return false;
  char* end = out + n - 1;
  while (*end == '0') *end-- = '\0';
  if (*end == '.') *end = '\0';
  if (strcmp(out, "-0") == 0) strcpy(out, "0");
  return true;
}

static bool bleTlmKey(const std::string& key) {
  if (key.empty()) return false;
  for (unsigned char c : key)
    if (!isalnum(c) && c != '_') return false;
  return true;
}

// Notify priority, highest first; "*" is where unlisted (profile-private) keys rank. The app has the
// first ones on screen, the rest are detail.
static int bleTlmRank(const std::string& key) {
  static const char* const order[] = {
    "soc", "power", "current", "voltage", "speed", "is_charging", "is_parked", "is_dcfc",
    "soh", "est_battery_range", "batt_temp", "ext_temp", "odometer", "soe", "capacity",
    "hvac_power", "cabin_temp", "elevation", "*", "kwh_charged", "batt_temp_min", "batt_temp_max",
  };
  int unlisted = 0;
  for (int i = 0; i < (int)(sizeof(order) / sizeof(order[0])); ++i) {
    if (key == order[i]) return i;
    if (order[i][0] == '*') unlisted = i;
  }
  return unlisted;
}

// seq > 0 (a session whose app announced relay acks, #90) leads the frame as "seq":n and is the last field trimmed:
// an ack names it. With seq 0 the frame is exactly what apps without acks have always read.
BleTlmFrame bleTlmFrame(const OBDVector& vec, size_t budget, uint32_t seq = 0) {
  struct Field { std::string key, text; };
  std::vector<Field> fields;
  if (seq) fields.push_back({ "", "\"seq\":" + std::to_string(seq) });
  for (const auto& res : vec) {
    const std::string& k = res.first;
    if (isGeoMetaKey(k) || isGeoValueKey(k) || k == "utc" || !bleTlmKey(k) || (seq && k == "seq")) continue;
    char num[32];
    if (k == "is_charging" || k == "is_parked" || k == "is_dcfc") strcpy(num, res.second != 0.0 ? "true" : "false");
    else if (!bleTlmNumber(res.second, num, sizeof(num))) continue;
    fields.push_back({ k, "\"" + k + "\":" + num });
  }

  auto join = [&](const std::vector<bool>& keep) {
    std::string out = "{\"tlm\":{";
    bool first = true;
    for (size_t i = 0; i < fields.size(); ++i) {
      if (!keep[i]) continue;
      if (!first) out += ",";
      first = false;
      out += fields[i].text;
    }
    return out + "}}";
  };

  BleTlmFrame frame;
  std::vector<bool> keep(fields.size(), true);
  frame.full = join(keep);
  frame.dropped = 0;
  if (frame.full.size() <= budget) {
    frame.notify = frame.full;
    return frame;
  }

  // Greedy by priority: a field that does not fit is skipped and a smaller, lower one may still go.
  // The empty envelope (10 B) always fits: the budget is at least 20. The seq (empty key) goes first; only an
  // unnegotiated MTU 23 can be too small for it, and the readable value, which an app at 23 reads, carries it.
  std::vector<size_t> by_rank(fields.size());
  for (size_t i = 0; i < by_rank.size(); ++i) by_rank[i] = i;
  auto rank = [&](size_t i) { return fields[i].key.empty() ? -1 : bleTlmRank(fields[i].key); };
  std::stable_sort(by_rank.begin(), by_rank.end(), [&](size_t a, size_t b) { return rank(a) < rank(b); });
  std::fill(keep.begin(), keep.end(), false);
  size_t used = 10, kept = 0;
  for (size_t i : by_rank) {
    const size_t cost = fields[i].text.size() + (kept ? 1 : 0);
    if (used + cost > budget) continue;
    keep[i] = true;
    used += cost;
    ++kept;
  }
  frame.notify = join(keep);
  frame.dropped = (int)(fields.size() - kept);
  return frame;
}

std::string bleTlmString(const OBDVector& vec) {
  return bleTlmFrame(vec, SIZE_MAX).full;
}

/*
  Build a /tlm/bulk request body from every buffered frame:
    {"data":[{"token":"<token>","tlm_list":[ {frame}, {frame}, ... ]}]}
  One vehicle, many telemetry points, uploaded in a single POST.
*/
std::string bulkString(const std::vector<OBDVector>& frames, const std::string& token) {
  json tlm_list = json::array();
  for (const auto& frame : frames) {
    json tlm = json::object();
    for (const auto& res : frame) {
      if (!isGeoMetaKey(res.first) && !isGeoValueKey(res.first)) tlm[res.first] = res.second;
    }
    tlm_list.push_back(tlm);
  }
  json entry = json::object();
  entry["token"] = token;
  entry["tlm_list"] = tlm_list;
  json body = json::object();
  body["data"] = json::array({ entry });
  return body.dump();
}

/*
  Build a /2/app/tlm request body: the v2 InputPointList, an array of typed points.

    [{"provider":"ABRP_OTA","soc":{"time":"2026-09-07T17:22:07Z","frac":0.8}, ...}, ...]

  The v1 endpoint took whatever keys the vehicle profile happened to produce and sorted it out server
  side. v2 is TYPED: every value is a named object with its own timestamp and a fixed unit. That is what
  the table below is for, and it is also the one real behaviour change in moving over - a profile key
  with no v2 equivalent is DROPPED rather than forwarded. In practice the profiles use ABRP's own
  conventional names (the same set v1 documents), and the two the Scenic profile adds beyond them - _soh
  and _bms_soc - already mark themselves private with a leading underscore. Anything genuinely new needs
  a line here.

  Units are the second trap: v2 is SI throughout - watts not kilowatts, Wh not kWh, metres not km, m/s
  not km/h, fractions not percent - while the profiles are in the human units v1 used. Every conversion
  below is doing real work; none of them is cosmetic.
*/
// The telemetry provider for the dongle's own cellular uploads (no relation to firmware OTA); the phone
// posts frames it relays over BLE as APP_OBD. Self-declared, so it attributes data but proves nothing
// about origin. The API validates this enum: a value it does not list fails the whole POST with a 400.
static const char* const V2_PROVIDER = "ABRP_OTA";

// Per-point keys that are metadata or are consumed by a state field below, so they never map 1:1.
static bool isV2MetaKey(const std::string& k) {
  return isGeoMetaKey(k) || isRelaySeqKey(k) || k == "utc" || k == "is_parked" || k == "is_charging" ||
         k == "is_dcfc" || k == "lat" || k == "lon";
}

// RFC 3339 in UTC, which is what WithTime.time wants. One per VALUE in v2, where v1 had one integer utc
// per point - a large part of why the v2 body is ~2.4x bigger before compression.
static std::string v2Time(time_t utc) {
  char buf[24];
  struct tm tm_utc;
  gmtime_r(&utc, &tm_utc);
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
  return std::string(buf);
}

// The last location and heading put in a body, keyed on the whole observation: the single-fix fallback can
// move the coordinates without moving their time. {} means none: a zero utc is never eligible.
struct GeoMark { double lat, lon, utc; };
struct HeadingMark { double degrees, utc; };
struct GeoMarks { GeoMark location; HeadingMark heading; };

// A location or heading equal to *marks (the last one sent, then each earlier frame of this batch) is omitted,
// so a parked car's fix goes up once and not on every point. Frames must stay in FIFO order for that to mean
// "last". On return *marks holds the marks as of the end of the body; *location_dups counts the omissions.
std::string bulkStringV2(const std::vector<OBDVector>& frames, double send_utc = 0, GeoMarks* marks = nullptr,
                         unsigned* location_dups = nullptr) {
  json points = json::array();
  GeoMarks none{};
  GeoMarks& last = marks ? *marks : none;
  if (location_dups) *location_dups = 0;

  for (const auto& frame : frames) {
    // Pull the point's own timestamp first - every value in the point is stamped with it. A frame with
    // no utc cannot be expressed in v2 at all (WithTime.time is required), and the API rejected such
    // frames under v1 too, so skipping it here loses nothing and saves a round trip.
    time_t utc = 0;
    double is_charging = 0, is_dcfc = 0, is_parked = 0;
    bool have_parked = false, have_charging = false, have_current = false;
    double lat = 0, lon = 0, loc_utc = 0, head_utc = 0;
    bool have_lat = false, have_lon = false;
    for (const auto& res : frame) {
      if (res.first == "utc") utc = (time_t)res.second;
      else if (res.first == "is_charging") { is_charging = res.second; have_charging = true; }
      else if (res.first == "is_dcfc") is_dcfc = res.second;
      else if (res.first == "is_parked") { is_parked = res.second; have_parked = true; }
      else if (res.first == "current") have_current = res.second > -2000.0 && res.second < 2000.0;
      else if (res.first == "lat") { lat = res.second; have_lat = true; }
      else if (res.first == "lon") { lon = res.second; have_lon = true; }
      else if (res.first == "__geo_loc_utc") loc_utc = res.second;
      else if (res.first == "__geo_head_utc") head_utc = res.second;
    }
    if (utc <= 0) continue;
    const std::string t = v2Time(utc);

    json p = json::object();
    p["provider"] = V2_PROVIDER;

    // One helper so each mapping below is a single readable line: field name, unit key, converted value.
    auto put = [&](const char* field, const char* unit, double value) {
      json v = json::object();
      v["time"] = t;
      v[unit] = value;
      p[field] = v;
    };

    for (const auto& res : frame) {
      const std::string& k = res.first;
      const double v = res.second;
      if (isV2MetaKey(k)) continue;

      if (k == "soc")                     put("soc", "frac", v / 100.0);
      else if (k == "soh")                put("soh", "frac", v / 100.0);
      else if (k == "soe")                put("soe", "wh", v);
      else if (k == "capacity")           put("batteryCapacity", "wh", v * 1000.0);
      else if (k == "kwh_charged")        put("chargingEnergyAdded", "wh", v * 1000.0);
      else if (k == "power")              put("power", "w", v * 1000.0);
      else if (k == "hvac_power")         put("hvacPower", "w", v * 1000.0);
      else if (k == "voltage")            put("voltage", "v", v);
      else if (k == "current")            put("current", "a", v);
      else if (k == "batt_temp")          put("batteryTemperature", "c", v);
      else if (k == "batt_temp_min")      put("batteryTemperatureMin", "c", v);
      else if (k == "batt_temp_max")      put("batteryTemperatureMax", "c", v);
      else if (k == "ext_temp")           put("externalTemperature", "c", v);
      else if (k == "cabin_temp")         put("cabinTemperature", "c", v);
      else if (k == "odometer")           put("odometer", "m", v * 1000.0);
      else if (k == "est_battery_range")  put("estimatedBatteryRange", "m", v * 1000.0);
      else if (k == "speed")              put("speed", "ms", v / 3.6);
      else if (k == "elevation")          put("elevation", "m", v);
      // Last line of defence, and the only one that reaches frames already saved in the flash backlog:
      // the API rejects the WHOLE batch over one out-of-range heading (-360 < degrees < 360), and a
      // rejected batch is retried forever. 360 is north; anything else outside 0-360 (including NaN,
      // the uint16 wrap of a negative angle and GPS_HEADING_INVALID) drops just the heading.
      else if (k == "heading")            {
        if (geoEligibleAtSend(head_utc, utc, send_utc) && v >= 0 && v <= 360) {
          const double degrees = v == 360 ? 0 : v;
          if (last.heading.degrees == degrees && last.heading.utc == head_utc) continue;
          last.heading = {degrees, head_utc};
          put("heading", "degrees", degrees);
          p["heading"]["time"] = v2Time((time_t)head_utc);
        }
      }
      // else: no v2 field for this key (the profile's private _-prefixed values land here). Dropped.
    }

    /*
      The two state fields are enums, not numbers, so they are built from the flags gathered above
      rather than through put(). is_dcfc only distinguishes the two charging kinds - it says nothing
      on its own, so it is never read unless is_charging is set.

      chargingState is OMITTED unless the dongle was in a position to judge, which means parked:
      is_charging is computed only while is_parked, since regen braking would otherwise read as
      charging mid-drive. So on an unparked frame a false is_charging means "not evaluated", not
      "not charging". v1 could be loose about that - it shipped is_charging: 0 and the backend had
      the current sign to reason with - but v2 asks for a typed enum, and emitting NOT_CHARGING
      turns an absence of evidence into a positive claim.

      Not hypothetical: a car driven into a garage kept a latched GNSS "moving" verdict, so is_parked
      stayed 0, and the server was told NOT_CHARGING while current sat at -52 A. That latch bug is
      fixed separately (gpsMovingFresh); this makes the mapping honest regardless of which way the
      latch is wrong, which is the property worth having in a typed API.
    */
    if (have_charging && have_current && have_parked && is_parked != 0) {
      json v = json::object();
      v["time"] = t;
      v["state"] = (is_charging != 0) ? (is_dcfc != 0 ? "CHARGING_DC" : "CHARGING_AC") : "NOT_CHARGING";
      p["chargingState"] = v;
    }
    // PARK vs DRIVE only. The dongle reads a parked LATCH, not the car's selector, so REVERSE and
    // NEUTRAL are not ours to claim - a reversing car reports DRIVE, which is true at this resolution.
    if (have_parked) {
      json v = json::object();
      v["time"] = t;
      v["state"] = (is_parked != 0) ? "PARK" : "DRIVE";
      p["drivingState"] = v;
    }
    // Coordinates travel as one value, and the schema requires BOTH halves - a frame carrying only one
    // is a GNSS fix mid-acquisition, so it is dropped rather than sent as a half position.
    if (have_lat && have_lon && geoValidLocation(lat, lon) && geoEligibleAtSend(loc_utc, utc, send_utc)) {
      if (last.location.lat == lat && last.location.lon == lon && last.location.utc == loc_utc) {
        if (location_dups) ++*location_dups;
      } else {
        last.location = {lat, lon, loc_utc};
        json v = json::object();
        v["time"] = v2Time((time_t)loc_utc);
        v["lat"] = lat;
        v["long"] = lon; // "long", not "lon" - v2 spells it out
        p["location"] = v;
      }
    }

    // provider alone is not a telemetry point. Frames that reduce to nothing (every key unmapped, or a
    // dongle-derived-only frame) are skipped so a POST is never spent on a payload the server will
    // count and discard.
    if (p.size() > 1) points.push_back(p);
  }

  return points.dump();
}

// Would this frame produce a v2 point? Asked before is_parked is added, so drivingState cannot count.
// A location or heading equal to *marks is not news; *marks then includes this frame's.
bool v2HasTelemetry(const OBDVector& frame, double send_utc, GeoMarks* marks = nullptr) {
  return bulkStringV2({ frame }, send_utc, marks) != "[]";
}
