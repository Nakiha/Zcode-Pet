#pragma once
#include "observer.h"
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <optional>

enum class NavigationCode {
  Idle,
  Busy,
  Opened,
  Unavailable,
  Missing,
  Ambiguous,
  AppClosed,
  NotVisible,
  AutomationFailed,
  FocusFailed,
  Cancelled
};
enum class NavigationAction { None, Invoke, Legacy, Keyboard };
struct NavigationResult {
  NavigationCode code = NavigationCode::Idle;
  int matches = 0;
  NavigationAction action = NavigationAction::None;
};
inline bool SameSession(const Session &a, const Session &b) {
  return a.id == b.id && a.workspace == b.workspace;
}
inline std::wstring ProjectName(std::wstring path) {
  while (!path.empty() && (path.back() == L'/' || path.back() == L'\\'))
    path.pop_back();
  const auto at = path.find_last_of(L"/\\");
  return at == std::wstring::npos ? path : path.substr(at + 1);
}
inline bool SameProject(const std::wstring &a, const std::wstring &b) {
  if (a.size() != b.size())
    return false;
  return std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) {
    return towlower(x) == towlower(y);
  });
}
struct NavigationPreparation {
  NavigationCode code;
  Session session;
};
inline NavigationPreparation PrepareNavigation(const Session &captured,
                                               const Snapshot &latest) {
  if (!latest.online || !latest.error.empty())
    return {NavigationCode::Unavailable, {}};
  auto found =
      std::find_if(latest.sessions.begin(), latest.sessions.end(),
                   [&](const auto &s) { return SameSession(s, captured); });
  if (found == latest.sessions.end())
    return {NavigationCode::Missing, {}};
  if (found->id.empty() || found->workspace.empty() || found->title.empty() ||
      ProjectName(found->path).empty())
    return {NavigationCode::Unavailable, {}};
  for (const auto &s : latest.sessions)
    if (!SameSession(s, *found) && s.title == found->title &&
        SameProject(ProjectName(s.path), ProjectName(found->path)))
      return {NavigationCode::Ambiguous, {}};
  return {NavigationCode::Idle, *found};
}
inline int SessionRowAt(float y, float height, int scroll, size_t count) {
  if (y < 0 || y >= height)
    return -1;
  const auto row = (int)std::floor((y + scroll) / 34.f);
  return row >= 0 && (size_t)row < count ? row : -1;
}
inline bool NavigationModelTests() {
  Session a;
  a.id = L"a";
  a.workspace = L"wa";
  a.path = L"C:\\pets\\Project\\";
  a.title = L"标题 12m";
  Snapshot s;
  s.online = true;
  s.sessions = {a};
  if (ProjectName(a.path) != L"Project" ||
      ProjectName(L"/tmp/Project/") != L"Project" || ProjectName(L"/") != L"" ||
      !SameProject(L"Project", L"project"))
    return false;
  if (PrepareNavigation(a, s).code != NavigationCode::Idle)
    return false;
  auto captured = a;
  s.sessions[0].title = L"新标题";
  if (PrepareNavigation(captured, s).session.title != L"新标题")
    return false;
  auto b = s.sessions[0];
  b.id = L"b";
  b.workspace = L"wb";
  b.path = L"D:\\project";
  s.sessions.push_back(b);
  if (PrepareNavigation(captured, s).code != NavigationCode::Ambiguous)
    return false;
  s.sessions[1].path = L"D:\\Other";
  if (PrepareNavigation(captured, s).code != NavigationCode::Idle)
    return false;
  s.sessions[0].title.clear();
  if (PrepareNavigation(captured, s).code != NavigationCode::Unavailable)
    return false;
  s.sessions.clear();
  if (PrepareNavigation(captured, s).code != NavigationCode::Missing)
    return false;
  s.online = false;
  if (PrepareNavigation(captured, s).code != NavigationCode::Unavailable)
    return false;
  return SessionRowAt(-.1f, 102, 0, 4) == -1 &&
         SessionRowAt(0, 102, 0, 4) == 0 && SessionRowAt(34, 102, 0, 4) == 1 &&
         SessionRowAt(102, 102, 0, 4) == -1 &&
         SessionRowAt(0, 68, 68, 4) == 2 &&
         SessionRowAt(68, 102, 68, 4) == -1 && !SameSession(a, b);
}
