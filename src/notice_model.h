#pragma once
#include "observer.h"
#include <algorithm>

struct NoticeRecord {
  Session event;
  bool Attention() const { return event.waiting; }
  std::wstring Key() const {
    return event.workspace + L"\n" + event.id + L"\n" + event.turnId + L"\n" +
           (Attention() ? L"wait:" + event.interactionId
                        : L"done:" + std::to_wstring(event.endedAt));
  }
  std::wstring Title() const {
    return event.title.empty() ? L"未命名会话" : event.title;
  }
  std::wstring Detail() const {
    if (!Attention())
      return {};
    return event.questionPreview.empty()
               ? L"需要你处理，请到 ZCode 查看待确认事项。"
               : L"等待你回答：" + event.questionPreview;
  }
  bool Sync(const Snapshot &snapshot) {
    const auto current = std::find_if(
        snapshot.sessions.begin(), snapshot.sessions.end(), [&](const auto &s) {
          return s.id == event.id && s.workspace == event.workspace;
        });
    if (current == snapshot.sessions.end())
      return true;
    if (!Attention() && snapshot.online && snapshot.error.empty()) {
      const bool laterStart =
          current->startedAt > 0 && current->startedAt > event.endedAt;
      const bool laterTurn = !current->turnId.empty() &&
                             current->turnId != event.turnId &&
                             (current->running || current->waiting ||
                              current->endedAt > event.endedAt);
      if (laterStart || laterTurn)
        return false;
    }
    event.title = current->title;
    event.path = current->path;
    return true;
  }
};

struct CompletionCelebrations {
  size_t pending = 0;
  uint64_t until = 0;
  void Push(size_t count) { pending += count; }
  bool Advance(uint64_t now) {
    if (now < until || pending == 0)
      return false;
    --pending;
    until = now + 4000;
    return true;
  }
  bool Active(uint64_t now) const { return now < until; }
};

inline bool NoticeModelTests() {
  Session a;
  a.id = L"a";
  a.workspace = L"w";
  a.title = L"原始描述";
  a.turnId = L"one";
  a.endedAt = 200;
  a.result = L"completed";
  a.recap = L"旧回复";
  NoticeRecord notice{a};
  Snapshot s;
  s.online = true;
  s.sessions = {a};
  s.sessions[0].title = L"会话名称";
  if (!notice.Sync(s) || notice.Title() != L"会话名称" ||
      !notice.Detail().empty())
    return false;
  s.sessions[0].running = true;
  s.sessions[0].turnId = L"two";
  s.sessions[0].startedAt = 300;
  if (notice.Sync(s))
    return false;
  s.online = false;
  if (!notice.Sync(s))
    return false;
  s.online = true;
  s.error = L"读取失败";
  if (!notice.Sync(s))
    return false;
  s.error.clear();
  s.sessions[0].workspace = L"other";
  if (!notice.Sync(s))
    return false;
  s.sessions[0] = a;
  s.sessions[0].turnId = L"two";
  s.sessions[0].endedAt = 400;
  if (notice.Sync(s))
    return false;
  a.waiting = true;
  a.interactionId = L"question";
  NoticeRecord waiting{a};
  if (!waiting.Sync(s) || waiting.Detail().empty() ||
      waiting.Key() == notice.Key())
    return false;
  auto b = a;
  b.id = L"b";
  NoticeRecord other{b};
  if (other.Key() == waiting.Key())
    return false;
  CompletionCelebrations celebrations;
  celebrations.Push(2);
  if (!celebrations.Advance(100) || !celebrations.Active(4099) ||
      celebrations.pending != 1 || celebrations.Advance(200))
    return false;
  if (!celebrations.Advance(4100) || celebrations.pending != 0 ||
      !celebrations.Active(8099))
    return false;
  return !celebrations.Advance(8100) && !celebrations.Active(8100);
}
