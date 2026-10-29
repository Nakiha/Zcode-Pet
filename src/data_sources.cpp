#include "data_sources.h"
#include "embedded_assets.h"
#include <windows.h>
#include <atomic>
#include <algorithm>
namespace {
std::atomic<unsigned> enabled{3}, detected{0};
std::atomic_bool harnessBacklog = false;
std::filesystem::path home, harness;
std::wstring Env(const wchar_t *key) {
  wchar_t value[32768]{};
  GetEnvironmentVariableW(key, value, 32768);
  return value;
}
} // namespace
unsigned SourcePollIntervalMs() {
  return harnessBacklog.load() && (enabled.load() & 2) ? 200 : 2000;
}
void SetHarnessBacklog(bool pending) { harnessBacklog = pending; }
void InitializeDataSources() {
  home = Env(L"USERPROFILE");
  auto configured = Env(L"DSH_HOME");
  if (configured == L"~")
    configured = home.wstring();
  else if (configured.starts_with(L"~/") || configured.starts_with(L"~\\"))
    configured = (home / configured.substr(2)).wstring();
  harness =
      configured.empty() ? home / L".dsh" : std::filesystem::path(configured);
  const auto settings = PetSettingsFile();
  unsigned bits = 0;
  if (GetPrivateProfileIntW(L"Sources", L"ZCode", 1, settings.c_str()))
    bits |= 1;
  if (GetPrivateProfileIntW(L"Sources", L"DeepSeek", 1, settings.c_str()))
    bits |= 2;
  enabled = bits;
}
std::array<SourceInfo, 2> DataSources() {
  const auto e = enabled.load(), d = detected.load();
  return {SourceInfo{SourceId::ZCode, L"ZCode", (e & 1) != 0, (d & 1) != 0},
          SourceInfo{SourceId::DeepSeek, L"DeepSeek Harness", (e & 2) != 0,
                     (d & 2) != 0}};
}
void ToggleDataSource(SourceId id) {
  const unsigned bit = id == SourceId::ZCode ? 1 : 2;
  const auto state = enabled.fetch_xor(bit) ^ bit;
  const auto path = PetSettingsFile();
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (!error)
    WritePrivateProfileStringW(L"Sources",
                               id == SourceId::ZCode ? L"ZCode" : L"DeepSeek",
                               state & bit ? L"1" : L"0", path.c_str());
}
Snapshot Observe() {
  // Only probe two known paths; no process-tree or installation-directory scan.
  static thread_local ULONGLONG nextProbe = 0;
  if (GetTickCount64() >= nextProbe) {
    unsigned found = 0;
    if (GetFileAttributesW((home / L".zcode/v2/tasks-index.sqlite").c_str()) !=
        INVALID_FILE_ATTRIBUTES)
      found |= 1;
    if (GetFileAttributesW((harness / L"storages/session_projcache/sessions").c_str()) !=
        INVALID_FILE_ATTRIBUTES)
      found |= 2;
    detected = found;
    nextProbe = GetTickCount64() + 30000;
  }
  Snapshot merged;
  const auto active = enabled.load() & detected.load();
  for (unsigned bit : {1u, 2u})
    if (active & bit) {
      try {
        static thread_local Snapshot zcodeCache;
        static thread_local ULONGLONG nextZcode = 0;
        if (bit == 1 && GetTickCount64() >= nextZcode) {
          zcodeCache = ObserveZCode();
          nextZcode = GetTickCount64() + 2000;
        }
        auto part = bit == 1 ? zcodeCache : ObserveDeepSeek(harness);
        merged.online |= part.online;
        if (!part.error.empty())
          merged.sourceWarning +=
              (merged.sourceWarning.empty() ? L"" : L"；") +
              std::wstring(bit == 1 ? L"ZCode: " : L"Harness: ") + part.error;
        merged.sessions.insert(merged.sessions.end(),
                               std::make_move_iterator(part.sessions.begin()),
                               std::make_move_iterator(part.sessions.end()));
      } catch (...) {
        merged.sourceWarning +=
            bit == 1 ? L"ZCode 数据读取失败；" : L"Harness 数据读取失败；";
      }
    }
  if (merged.sessions.empty())
    merged.error = merged.sourceWarning;
  std::stable_sort(
      merged.sessions.begin(), merged.sessions.end(),
      [](const auto &a, const auto &b) { return a.updated > b.updated; });
  if (merged.sessions.size() > 40)
    merged.sessions.resize(40);
  return merged;
}
std::wstring ReadFinalRecap(const Session &s) {
  return s.source == SourceId::ZCode ? ReadZCodeRecap(s) : s.recap;
}
