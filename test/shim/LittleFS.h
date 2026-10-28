// An in-memory LittleFS: files are strings in a map, so a test can read back
// exactly what the firmware wrote, and start from an empty flash every run.
#pragma once
#include "Arduino.h"
#include <map>

#define FILE_READ   "r"
#define FILE_APPEND "a"

struct FsStore {
  std::map<std::string, std::string> files;
  bool mountOk = true;                     // false = a flash that will not mount
};
static FsStore g_fs;

class File : public Print {
  std::string path_; bool open_ = false; bool append_ = false; size_t pos_ = 0;
public:
  File(){}
  File(const std::string &p, bool app) : path_(p), open_(true), append_(app) {}
  operator bool() const { return open_; }
  size_t size() const { return open_ ? g_fs.files[path_].size() : 0; }
  int  available() { return open_ && !append_ ? (int)(g_fs.files[path_].size() - pos_) : 0; }
  int  read() { if (!available()) return -1; return (uint8_t)g_fs.files[path_][pos_++]; }
  size_t write(uint8_t b) override { if (!open_ || !append_) return 0; g_fs.files[path_].push_back((char)b); return 1; }
  using Print::write;
  void close() { open_ = false; }
};

struct LittleFSClass {
  bool begin(bool) { return g_fs.mountOk; }
  bool exists(const char *p) { return g_fs.files.count(p) > 0; }
  File open(const char *p, const char *mode) {
    bool app = mode[0] == 'a';
    if (!app && !exists(p)) return File();
    if (app) g_fs.files[p];                // creates it, as "a" does
    return File(p, app);
  }
  bool remove(const char *p) { return g_fs.files.erase(p) > 0; }
  bool rename(const char *a, const char *b) {
    if (!exists(a)) return false;
    g_fs.files[b] = g_fs.files[a]; g_fs.files.erase(a); return true;
  }
};
static LittleFSClass LittleFS;
