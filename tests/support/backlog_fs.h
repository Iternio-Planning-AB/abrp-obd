#pragma once
#include <cstdint>
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
  void (*read_observer)() = nullptr;
  explicit operator bool() const { return bool(data); }
  bool available() const { return data && offset < data->size(); }
  String readStringUntil(char delimiter) {
    auto end = data->find(delimiter, offset);
    if (end == std::string::npos) end = data->size();
    String result{data->substr(offset, end - offset)};
    offset = end + (end < data->size());
    if (read_observer) read_observer();
    return result;
  }
  size_t* room = nullptr; // bytes the filesystem can still take, when the test sets a capacity
  size_t* lines_left = nullptr; // writes that still succeed, when the test makes them fail
  size_t println(const char* value) {
    const size_t n = std::strlen(value) + 1;
    if (room && *room < n) return 0;
    if (lines_left && !(*lines_left)--) return 0;
    if (room) *room -= n;
    *data += std::string(value) + '\n';
    return n;
  }
  size_t print(const char* value) { *data += value; return std::strlen(value); }
  String readString() { String result{data->substr(offset)}; offset = data->size(); return result; }
  size_t size() const { return data ? data->size() : 0; }
  void close() {}
};

struct FakeLittleFS {
  std::map<std::string, std::shared_ptr<std::string>> files;
  bool fail_open = false;
  void (*read_observer)() = nullptr;
  size_t capacity = 3538944, other_used = 0; // the ONE+ partition; other_used stands in for the flash log
  size_t room = 0;
  bool enforce_capacity = false; // writes past the capacity fail, as on a full filesystem
  size_t write_lines = SIZE_MAX;  // println succeeds this many times per open, then fails
  size_t usedBytes() const {
    size_t n = other_used;
    for (const auto& file : files) n += file.second->size();
    return n;
  }
  size_t totalBytes() const { return capacity; }
  bool exists(const char* path) const { return files.count(path); }
  File open(const char* path, const char* mode) {
    if (fail_open) return {};
    if (*mode == 'w' || (*mode == 'a' && !exists(path))) files[path] = std::make_shared<std::string>();
    if (!exists(path)) return File{};
    File file{files.at(path), 0, read_observer};
    if (write_lines != SIZE_MAX) file.lines_left = &write_lines;
    if (enforce_capacity) {
      room = capacity > usedBytes() ? capacity - usedBytes() : 0;
      file.room = &room;
    }
    return file;
  }
  void remove(const char* path) { files.erase(path); }
  void seed(const std::string& value) { files["/upload_backlog.jsonl"] = std::make_shared<std::string>(value); }
  std::string contents() const { return *files.at("/upload_backlog.jsonl"); }
} LittleFS;

#if !ENABLE_LOGGING
template<typename... Args> void logEvent(const char*, Args...) {}
#endif
