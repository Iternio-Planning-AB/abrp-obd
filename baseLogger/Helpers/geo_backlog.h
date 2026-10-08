#pragma once

json encodeUploadFrame(const OBDVector& frame) {
  json core = json::object(), geo = json::object();
  for (const auto& res : frame) {
    if (isGeoValueKey(res.first) || res.first == "__geo_loc_utc" || res.first == "__geo_head_utc")
      geo[res.first] = res.second;
    else if (!isGeoMetaKey(res.first)) core[res.first] = res.second;
  }
  double utc = core.value("utc", 0.0);
  double loc_utc = geo.value("__geo_loc_utc", 0.0), head_utc = geo.value("__geo_head_utc", 0.0);
  if (geo.contains("lat") && geo.contains("lon") && geoValidUtc(loc_utc, utc) &&
      geoValidLocation(geo["lat"].get<double>(), geo["lon"].get<double>())) {
    core["__geo_lat"] = geo["lat"];
    core["__geo_lon"] = geo["lon"];
    core["__geo_loc_utc"] = loc_utc;
    core["__geo_v"] = 1;
  }
  if (geo.contains("heading") && geoValidUtc(head_utc, utc)) {
    double heading = geo["heading"].get<double>();
    if (heading >= 0 && heading <= 360) {
      core["__geo_heading"] = heading;
      core["__geo_head_utc"] = head_utc;
      core["__geo_v"] = 1;
    }
  }
  return core;
}

OBDVector decodeUploadFrame(const json& row) {
  OBDVector frame;
  if (!row.is_object()) return frame;
  for (const auto& item : row.items()) {
    if (!isGeoMetaKey(item.key()) && !isGeoValueKey(item.key()))
      frame.emplace_back(item.key(), item.value().get<double>());
  }
  if (!row.contains("__geo_v") || !row["__geo_v"].is_number() || row["__geo_v"].get<double>() != 1 ||
      row.contains("lat") || row.contains("lon") || row.contains("heading")) return frame;
  auto number = [&](const char* key, double& value) {
    if (!row.contains(key) || !row[key].is_number()) return false;
    value = row[key].get<double>();
    return std::isfinite(value);
  };
  double utc = 0, lat = 0, lon = 0, loc_utc = 0, heading = 0, head_utc = 0;
  if (!number("utc", utc)) return frame;
  if (number("__geo_lat", lat) && number("__geo_lon", lon) && number("__geo_loc_utc", loc_utc) &&
      geoValidLocation(lat, lon) && geoValidUtc(loc_utc, utc)) {
    frame.emplace_back("lat", lat);
    frame.emplace_back("lon", lon);
    frame.emplace_back("__geo_loc_utc", loc_utc);
  }
  if (number("__geo_heading", heading) && number("__geo_head_utc", head_utc) &&
      heading >= 0 && heading <= 360 && geoValidUtc(head_utc, utc)) {
    frame.emplace_back("heading", heading);
    frame.emplace_back("__geo_head_utc", head_utc);
  }
  return frame;
}
