#pragma once
#include <cstdint>
#include <string>
#include <vector>
enum class SourceId { ZCode, DeepSeek };
struct Session {
  std::wstring id, workspace, title, path, result;
  SourceId source = SourceId::ZCode;
  bool running = false, uncertain = false;
  int64_t updated = 0;
  std::wstring turnId;
  int64_t endedAt = 0;
  int64_t startedAt = 0;
  std::wstring recap;
  bool waiting = false;
  std::wstring interactionId, questionPreview;
  int64_t waitingSince = 0, resumedAt = 0;
  std::wstring projectId, projectDisplayName;
  bool projectMetadataKnown = false, projectNameAmbiguous = false;
};
struct Snapshot {
  std::vector<Session> sessions;
  bool online = false;
  std::wstring error, sourceWarning;
};
Snapshot Observe();
bool ObserverTests();
struct ObserverTiming {
  uint64_t processUs = 0, indexUs = 0, metadataUs = 0, logUs = 0,
           fallbackUs = 0, questionsUs = 0, totalUs = 0;
};
ObserverTiming LastObserverTiming();
std::wstring ReadFinalRecap(const Session &session);
