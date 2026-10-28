#pragma once
#include <cstdint>
#include <string>
#include <vector>
struct Session {
  std::wstring id, workspace, title, path, result;
  bool running = false, uncertain = false;
  int64_t updated = 0;
  std::wstring turnId;
  int64_t endedAt = 0;
  int64_t startedAt = 0;
  std::wstring recap;
  bool waiting = false;
  std::wstring interactionId, questionPreview;
  int64_t waitingSince = 0, resumedAt = 0;
};
struct Snapshot {
  std::vector<Session> sessions;
  bool online = false;
  std::wstring error;
};
Snapshot Observe();
bool ObserverTests();
std::wstring ReadFinalRecap(const Session &session);
