#pragma once
#include <windows.h>
#include "observer.h"
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
struct HarnessCacheFact {
  std::wstring id, title, cwd;
  int64_t createdAt = 0, titleSeq = -1;
  bool seeded = false;
  std::filesystem::file_time_type modified{};
};
struct HarnessCacheIndex {
  struct Watch;
  std::unique_ptr<Watch> watch;
  std::filesystem::path directory;
  std::filesystem::directory_iterator initial, end;
  std::set<std::filesystem::path> pending;
  std::map<std::filesystem::path, HarnessCacheFact> facts;
  bool initialized = false;
  uint64_t recordReads = 0;
  explicit HarnessCacheIndex(std::filesystem::path folder);
  ~HarnessCacheIndex();
  void Poll();
  bool Busy() const;
};
std::filesystem::path HarnessLogPath(const std::filesystem::path &root,
                                     const HarnessCacheFact &fact);
bool HarnessCacheTests();

bool ApplyHarnessCacheTitle(const HarnessCacheFact &fact, Session &view,
                            int64_t createdAt, bool seeded,
                            int64_t logTitleSeq);
