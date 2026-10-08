#pragma once
/*
  Telemetry payload formatting - serializers from OBDVector frames to the wire formats.
  Contains: logString (single-frame {"tlm":...} for the API), bleTlmString (trimmed <185B
  frame for the BLE ABD1 characteristic), bulkString (the legacy v1 /1/tlm/bulk body) and
  bulkStringV2 (the v2 /2/app/tlm InputPointList, which is what actually goes out - see
  TLM_V2_PATH in config.h).

  Extracted verbatim from logger.ino (definitions moved, logic unchanged). This file is
  #included by logger.ino into the single sketch translation unit - it is NOT a standalone
  compilation unit - so it shares logger.ino's globals and the config.h feature flags.
*/

/*
  Convert vector of PID results to suitable form for iternio telemetry API
*/

#include <cmath>

bool isGeoMetaKey(const std::string& key) { return key.compare(0, 6, "__geo_") == 0; }

bool isGeoValueKey(const std::string& key) { return key == "lat" || key == "lon" || key == "heading"; }

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

// BLE telemetry frame for characteristic ABD1. Deliberately NOT logString(): the app pulls the whole frame in
// ONE ~185B MTU read with no reassembly, so it must stay under that. logString's frame (lat/lon/utc + nlohmann
// float noise) runs ~200-220B and would truncate -> the app's JSON.parse fails. So: measurements only (the app
// adds utc + location itself), fixed 2dp numbers (compact, no float-noise), and is_charging/is_parked/is_dcfc
// as real JSON booleans (the contract shows `false`, not 1.0). Result ~150B. Keys align to the app's
// VehicleJsonTelemetryKeys; unknown keys (lat/lon/utc) are dropped.
std::string bleTlmString(const OBDVector& vec) {
  std::stringstream ss;
  ss << "{\"tlm\":{";
  bool first = true;
  for (const auto& res : vec) {
    const std::string& k = res.first;
    if (isGeoMetaKey(k) || isGeoValueKey(k) || k == "utc") continue; // phone supplies location/heading/utc itself
    if (!first) ss << ",";
    first = false;
    ss << "\"" << k << "\":";
    if (k == "is_charging" || k == "is_parked" || k == "is_dcfc") {
      ss << (res.second != 0.0 ? "true" : "false");
    } else {
      char num[24];
      snprintf(num, sizeof(num), "%.2f", res.second);
      ss << num;
    }
  }
  ss << "}}";
  return ss.str();
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
  return isGeoMetaKey(k) || k == "utc" || k == "is_parked" || k == "is_charging" || k == "is_dcfc" ||
         k == "lat" || k == "lon";
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

std::string bulkStringV2(const std::vector<OBDVector>& frames) {
  json points = json::array();

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
        if (geoValidUtc(head_utc, utc) && v >= 0 && v <= 360) {
          put("heading", "degrees", v == 360 ? 0 : v);
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
    if (have_lat && have_lon && geoValidLocation(lat, lon) && geoValidUtc(loc_utc, utc)) {
      json v = json::object();
      v["time"] = v2Time((time_t)loc_utc);
      v["lat"] = lat;
      v["long"] = lon; // "long", not "lon" - v2 spells it out
      p["location"] = v;
    }

    // provider alone is not a telemetry point. Frames that reduce to nothing (every key unmapped, or a
    // dongle-derived-only frame) are skipped so a POST is never spent on a payload the server will
    // count and discard.
    if (p.size() > 1) points.push_back(p);
  }

  return points.dump();
}
