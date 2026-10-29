#pragma once
#include "observer.h"
#include <filesystem>
#include <array>
struct SourceInfo {
  SourceId id;
  const wchar_t *name;
  bool enabled, detected;
};
void InitializeDataSources();
unsigned SourcePollIntervalMs();
void SetHarnessBacklog(bool pending);
std::array<SourceInfo, 2> DataSources();
void ToggleDataSource(SourceId id);
Snapshot ObserveZCode();
Snapshot ObserveDeepSeek(const std::filesystem::path &root);
std::wstring ReadZCodeRecap(const Session &);
bool DataSourceTests(bool benchmark = false);
