#pragma once
#include "observer.h"
#include <map>
#include <algorithm>
#include <cwctype>
#include <cmath>
#include <string>
#include <vector>

enum class SessionState {
  Offline,
  Running,
  Waiting,
  Completed,
  Error,
  Interrupted,
  Unknown
};
inline SessionState StateOf(const Session &s, bool online) {
  if (!online)
    return SessionState::Offline;
  if (s.waiting)
    return SessionState::Waiting;
  if (s.running)
    return SessionState::Running;
  if (s.uncertain)
    return SessionState::Unknown;
  if (s.result == L"completed")
    return SessionState::Completed;
  if (s.result == L"error")
    return SessionState::Error;
  if (s.result == L"interrupted" || s.result == L"cancelled")
    return SessionState::Interrupted;
  return SessionState::Unknown;
}

class StateMotion {
  struct Entry {
    SessionState state;
    uint64_t changedAt = 0;
  };
  std::map<std::wstring, Entry> entries;
  static std::wstring Key(const Session &s) {
    return s.workspace + L"\n" + s.id;
  }

public:
  static constexpr uint64_t Duration = 360;
  void Update(const Snapshot &snapshot, uint64_t now, bool visible) {
    if (!snapshot.error.empty())
      return;
    std::map<std::wstring, Entry> next;
    for (const auto &s : snapshot.sessions) {
      const auto key = Key(s);
      const auto state = StateOf(s, snapshot.online);
      auto previous = entries.find(key);
      // First observation is a baseline. Repeated snapshots, title edits and
      // elapsed-time changes must not retrigger the animation.
      Entry entry{state, 0};
      if (previous != entries.end())
        entry.changedAt = previous->second.state == state
                              ? previous->second.changedAt
                              : visible ? now : 0;
      next.emplace(key, entry);
    }
    entries = std::move(next);
  }
  bool Active(const Session &s, uint64_t now) const {
    auto entry = entries.find(Key(s));
    return entry != entries.end() && entry->second.changedAt != 0 &&
           now >= entry->second.changedAt &&
           now - entry->second.changedAt < Duration;
  }
  float Offset(const Session &s, uint64_t now) const {
    if (!Active(s, now))
      return 0;
    const auto started = entries.find(Key(s))->second.changedAt;
    const float progress = (float)(now - started) / Duration;
    return -2.6f * std::sin(3.141592654f * progress);
  }
};

inline std::wstring ShortAge(int64_t ms) {
  auto sec = std::max<int64_t>(0, ms / 1000);
  if (sec < 60)
    return std::to_wstring(sec) + L"s";
  if (sec < 3600)
    return std::to_wstring(sec / 60) + L"m";
  if (sec < 86400)
    return std::to_wstring(sec / 3600) + L"h";
  return std::to_wstring(sec / 86400) + L"d";
}
class StateClock {
  struct Entry {
    SessionState state;
    std::wstring turn;
    int64_t since;
    bool exact;
  };
  std::map<std::wstring, Entry> entries;
  static std::wstring Key(const Session &s) {
    return s.workspace + L"\n" + s.id;
  }

public:
  void Update(const Snapshot &snapshot, int64_t now) {
    if (!snapshot.error.empty())
      return;
    for (const auto &s : snapshot.sessions) {
      auto state = StateOf(s, snapshot.online);
      auto i = entries.find(Key(s));
      int64_t source =
          state == SessionState::Waiting   ? s.waitingSince
          : state == SessionState::Running ? std::max(s.startedAt, s.resumedAt)
          : (state == SessionState::Completed || state == SessionState::Error ||
             state == SessionState::Interrupted)
              ? s.endedAt
              : 0;
      auto token = s.waiting ? s.turnId + L"/" + s.interactionId : s.turnId;
      if (i == entries.end() || i->second.state != state ||
          i->second.turn != token)
        entries.insert_or_assign(
            Key(s), Entry{state, token, source > 0 ? source : now, source > 0});
      else if (source > 0 && !i->second.exact) {
        i->second.since = source;
        i->second.exact = true;
      }
    }
  }
  std::wstring Age(const Session &s, int64_t now) const {
    auto i = entries.find(Key(s));
    if (i == entries.end())
      return L"—";
    return (i->second.exact ? L"" : L"≥") + ShortAge(now - i->second.since);
  }
};
inline std::wstring CompactPreview(const std::wstring &raw) {
  std::wstring out;
  bool space = false, fenced = false;
  for (size_t i = 0; i < raw.size() && out.size() < 180; ++i) {
    if (raw.compare(i, 3, L"```") == 0) {
      fenced = !fenced;
      i += 2;
      continue;
    }
    if (fenced)
      continue;
    wchar_t ch = raw[i];
    if (ch == L'*' || ch == L'`' || ch == L'#')
      continue;
    if (iswspace(ch)) {
      space = !out.empty();
      continue;
    }
    if (space) {
      out += L' ';
      space = false;
    }
    out += ch;
  }
  if (out.size() >= 180) {
    if (!out.empty() && out.back() >= 0xd800 && out.back() <= 0xdbff)
      out.pop_back();
    out += L"…";
  }
  return out;
}

inline const wchar_t *SelectPetAnimation(bool running, bool success,
                                         bool failed, bool inside,
                                         bool waiting = false) {
  if (success)
    return L"success";
  if (waiting)
    return L"waiting";
  return running    ? L"working"
         : failed   ? L"paused"
         : inside   ? L"waiting"
                    : L"idle";
}

// The visual clock owns animated paints; the age timer owns resting updates.
struct PanelFramePolicy {
  bool hadMotion = false;
  bool Tick(bool moving, bool contentChanged, bool visualFrame) {
    const bool redraw = visualFrame ? moving || hadMotion || contentChanged
                                    : !moving && !hadMotion && contentChanged;
    if (visualFrame)
      hadMotion = moving;
    return redraw;
  }
  bool NeedsFrames(bool moving) const { return moving || hadMotion; }
};

// UI observes completed facts, never infers completion from disappearance.
class CompletionTracker {
  int64_t startedAt;
  std::map<std::wstring, int64_t> seen;

public:
  explicit CompletionTracker(int64_t start) : startedAt(start) {}
  std::vector<Session> Accept(const Snapshot &snapshot) {
    std::vector<Session> result;
    if (!snapshot.online || !snapshot.error.empty())
      return result;
    for (const auto &s : snapshot.sessions) {
      if (s.endedAt <= startedAt || s.running || s.turnId.empty())
        continue;
      auto key = s.workspace + L"\n" + s.id + L"\n" + s.turnId;
      if (seen[key] >= s.endedAt)
        continue;
      seen[key] = s.endedAt;
      if (s.result == L"completed")
        result.push_back(s);
    }
    return result;
  }
};
class AttentionTracker {
  std::map<std::wstring, bool> seen;

public:
  std::vector<Session> Accept(const Snapshot &snapshot) {
    std::vector<Session> result;
    if (!snapshot.online || !snapshot.error.empty())
      return result;
    for (const auto &s : snapshot.sessions)
      if (s.waiting && !s.interactionId.empty()) {
        auto key = s.workspace + L"\n" + s.id + L"\n" + s.turnId + L"\n" +
                   s.interactionId;
        if (!seen[key]) {
          seen[key] = true;
          result.push_back(s);
        }
      }
    return result;
  }
};
struct HoverPolicy {
  uint64_t enteredAt = 0, leftAt = 0;
  bool shown = false, suppressed = false, wasInside = false;
  bool Update(bool inside, bool dragging, uint64_t now, bool pinned = false) {
    if (!inside)
      suppressed = false;
    if (inside && !wasInside)
      enteredAt = now;
    if (!inside && wasInside)
      leftAt = now;
    wasInside = inside;
    if (suppressed)
      shown = false;
    else if (dragging)
      shown = shown || pinned;
    else if (pinned)
      shown = true;
    else if (inside && now - enteredAt >= 120)
      shown = true;
    else if (!inside && now - leftAt >= 260)
      shown = false;
    return shown;
  }
  void Dismiss() {
    shown = false;
    suppressed = true;
  }
};
inline bool PresentationTests() {
  PanelFramePolicy frames;
  if (frames.Tick(true, true, false) || !frames.NeedsFrames(true))
    return false;
  if (!frames.Tick(true, true, true))
    return false;
  if (frames.Tick(false, true, false) || !frames.NeedsFrames(false))
    return false;
  if (!frames.Tick(false, true, true) || frames.NeedsFrames(false))
    return false;
  if (!frames.Tick(false, true, false) || frames.Tick(false, false, false))
    return false;
  if (ShortAge(10000) != L"10s" || ShortAge(120000) != L"2m" ||
      ShortAge(3600000) != L"1h")
    return false;
  StateClock clock;
  Snapshot timed;
  timed.online = true;
  Session timing;
  timing.id = L"a";
  timing.result = L"completed";
  timing.endedAt = 1000;
  timed.sessions = {timing};
  clock.Update(timed, 11000);
  if (clock.Age(timing, 11000) != L"10s")
    return false;
  clock.Update(timed, 121000);
  if (clock.Age(timing, 121000) != L"2m")
    return false;
  timed.sessions[0].running = true;
  timed.sessions[0].startedAt = 125000;
  timed.sessions[0].turnId = L"new";
  clock.Update(timed, 130000);
  if (clock.Age(timing, 130000) != L"5s")
    return false;
  timed.sessions[0].running = false;
  timed.sessions[0].endedAt = 135000;
  clock.Update(timed, 145000);
  if (clock.Age(timing, 145000) != L"10s")
    return false;
  timed.sessions[0].endedAt = 0;
  timed.sessions[0].turnId = L"old";
  clock.Update(timed, 150000);
  if (clock.Age(timing, 160000) != L"≥10s")
    return false;
  if (CompactPreview(L"**完成**\n\n说明") != L"完成 说明")
    return false;
  if (std::wstring(SelectPetAnimation(true, true, false, true, false)) !=
          L"success" ||
      std::wstring(SelectPetAnimation(true, false, false, false, false)) !=
          L"working" ||
      std::wstring(SelectPetAnimation(true, true, false, false, true)) !=
          L"success" ||
      std::wstring(SelectPetAnimation(false, true, false, false, false)) !=
          L"success" ||
      std::wstring(SelectPetAnimation(false, false, true, false, false)) !=
          L"paused" ||
      std::wstring(SelectPetAnimation(false, false, false, true, false)) !=
          L"waiting" ||
      std::wstring(SelectPetAnimation(false, false, false, true, true)) !=
          L"waiting" ||
      std::wstring(SelectPetAnimation(false, false, false, false, false)) !=
          L"idle")
    return false;
  CompletionTracker t(100);
  Snapshot snap;
  snap.online = true;
  Session s;
  s.id = L"a";
  s.workspace = L"w";
  s.turnId = L"one";
  s.result = L"completed";
  s.endedAt = 90;
  snap.sessions = {s};
  if (!t.Accept(snap).empty())
    return false;
  snap.sessions[0].endedAt = 110;
  if (t.Accept(snap).size() != 1 || !t.Accept(snap).empty())
    return false;
  snap.sessions[0].turnId = L"two";
  snap.sessions[0].endedAt = 120;
  if (t.Accept(snap).size() != 1)
    return false;
  snap.sessions[0].turnId = L"error";
  snap.sessions[0].result = L"error";
  if (!t.Accept(snap).empty())
    return false;
  Snapshot active;
  active.online = true;
  Session note;
  note.id = L"n";
  note.workspace = L"w";
  note.turnId = L"t";
  note.result = L"completed";
  note.endedAt = 500;
  note.waiting = true;
  note.interactionId = L"q";
  note.waitingSince = 1000;
  active.sessions = {note};
  AttentionTracker alerts;
  if (alerts.Accept(active).size() != 1 || !alerts.Accept(active).empty())
    return false;
  StateClock waitClock;
  waitClock.Update(active, 3000);
  if (waitClock.Age(note, 11000) != L"10s")
    return false;
  active.sessions[0].waiting = false;
  active.sessions[0].running = true;
  active.sessions[0].resumedAt = 12000;
  waitClock.Update(active, 13000);
  if (waitClock.Age(note, 14000) != L"2s")
    return false;
  Snapshot longTask;
  longTask.online = true;
  Session longRun;
  longRun.id = L"long";
  longRun.running = true;
  longRun.startedAt = 1000;
  longTask.sessions = {longRun};
  StateClock longClock;
  longClock.Update(longTask, 1201000);
  if (StateOf(longRun, true) != SessionState::Running ||
      longClock.Age(longRun, 1201000) != L"20m")
    return false;
  HoverPolicy h;
  if (h.Update(true, false, 100))
    return false;
  if (!h.Update(true, false, 230))
    return false;
  if (!h.Update(false, false, 250) || !h.Update(true, false, 400))
    return false;
  h.Update(false, false, 500);
  if (h.Update(false, false, 761))
    return false;
  h.Update(true, false, 800);
  h.Update(true, false, 940);
  h.Dismiss();
  if (h.Update(true, false, 1000))
    return false;
  h.Update(false, false, 1100);
  h.Update(true, false, 1200);
  if (!h.Update(true, false, 1340))
    return false;
  if (!h.Update(true, true, 1350))
    return false;
  HoverPolicy pin;
  if (!pin.Update(false, false, 1500, true) ||
      !pin.Update(false, false, 5000, true))
    return false;
  if (!pin.Update(false, true, 5100, true))
    return false;
  if (!pin.Update(false, false, 5200, true))
    return false;
  if (pin.Update(false, false, 6000, false))
    return false;
  pin.Update(false, false, 6100, true);
  pin.Dismiss();
  return !pin.Update(true, false, 6200, false);
}
