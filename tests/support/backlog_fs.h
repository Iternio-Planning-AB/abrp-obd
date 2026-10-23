#pragma once
#include <cstring>
#include <map>
#include <memory>

struct String {
  std::string value;
  void trim() {
    const auto begin = value.find_first_not_of(" \r\n\t");
    value = begin == std::string::npos ? "" : value.substr(begin, value.find_last_not_of(" \r\n\t") - begin + 1);
  }
  size_t length() const { return value.size(); }
  const char* c_str() const { return value.c_str(); }
};

struct File {
  std::shared_ptr<std::string> data;
  size_t offset = 0;
  explicit operator bool() const { return bool(data); }
  bool available() const { return data && offset < data->size(); }
  String readStringUntil(char delimiter) {
    auto end = data->find(delimiter, offset);
    if (end == std::string::npos) end = data->size();
    String result{data->substr(offset, end - offset)};
    offset = end + (end < data->size());
    return result;
  }
  void println(const char* value) { *data += std::string(value) + '\n'; }
  size_t print(const char* value) { *data += value; return std::strlen(value); }
  String readString() { String result{data->substr(offset)}; offset = data->size(); return result; }
  void close() {}
};

struct FakeLittleFS {
  std::map<std::string, std::shared_ptr<std::string>> files;
  bool fail_open = false;
  bool exists(const char* path) const { return files.count(path); }
  File open(const char* path, const char* mode) {
    if (fail_open) return {};
    if (*mode == 'w') files[path] = std::make_shared<std::string>();
    return exists(path) ? File{files.at(path)} : File{};
  }
  void remove(const char* path) { files.erase(path); }
  void seed(const std::string& value) { files["/upload_backlog.jsonl"] = std::make_shared<std::string>(value); }
  std::string contents() const { return *files.at("/upload_backlog.jsonl"); }
} LittleFS;

#if !ENABLE_LOGGING
template<typename... Args> void logEvent(const char*, Args...) {}
#endif
