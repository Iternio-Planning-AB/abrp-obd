#pragma once
// The oracle for upload_helpers_checks' batch-body group (#136): bulkStringV2 as it was up to 65a15f4, building the
// whole batch as one json array before dump(). Verbatim but for the name. The production serialiser must stay
// byte-identical to it, so a deliberate change to the v2 mapping goes in both.
// Include after telemetry_format.h: it uses that file's helpers, so only the serialisation is compared.

std::string bulkStringV2Tree(const std::vector<OBDVector>& frames, double send_utc = 0, GeoMarks* marks = nullptr,
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
