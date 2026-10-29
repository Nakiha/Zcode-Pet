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
  NotFound,
  Offscreen,
  AutomationFailed,
  FocusFailed,
  Cancelled,
  UnsupportedSource
};
enum class NavigationAction { None, Invoke, Legacy, Keyboard };
struct NavigationResult {
  NavigationCode code = NavigationCode::Idle;
  int matches = 0;
  NavigationAction action = NavigationAction::None;
};
inline bool SameSession(const Session &a, const Session &b) {
  return a.source == b.source && a.id == b.id && a.workspace == b.workspace;
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
inline std::wstring NavigationProjectName(const Session &session) {
  return session.projectMetadataKnown ? session.projectDisplayName
                                      : ProjectName(session.path);
}
inline bool AccessibleSessionName(const std::wstring &name, const Session &target,
                                  const std::vector<Session> &peers) {
  if (target.title.empty() || !name.starts_with(target.title))
    return false;
  if (name == target.title)
    return true;
  for (const auto &peer : peers)
    if (SameProject(NavigationProjectName(peer), NavigationProjectName(target)) &&
        peer.title.size() > target.title.size() && name.starts_with(peer.title))
      return false;
  auto suffix = name.substr(target.title.size());
  suffix.erase(std::remove_if(suffix.begin(), suffix.end(),
                             [](wchar_t c) { return iswspace(c); }), suffix.end());
  if (suffix == L"刚刚" || suffix == L"justnow")
    return true;
  size_t digits = 0;
  while (digits < suffix.size() && suffix[digits] >= L'0' && suffix[digits] <= L'9')
    ++digits;
  if (!digits || digits > 6)
    return false;
  const auto unit = suffix.substr(digits);
  for (const auto *allowed : {L"秒", L"秒前", L"分", L"分钟前", L"分钟", L"小时",
                              L"小时前", L"天", L"天前", L"周", L"周前", L"个月",
                              L"个月前", L"年", L"年前", L"s", L"m", L"h", L"d"})
    if (unit == allowed)
      return true;
  return false;
}
struct NavigationPreparation {
  NavigationCode code;
  Session session;
};
inline NavigationPreparation PrepareNavigation(const Session &captured,
                                               const Snapshot &latest) {
  if(captured.source!=SourceId::ZCode)return {NavigationCode::UnsupportedSource,{}};
  if (!latest.online || !latest.error.empty())
    return {NavigationCode::Unavailable, {}};
  auto found =
      std::find_if(latest.sessions.begin(), latest.sessions.end(),
                   [&](const auto &s) { return SameSession(s, captured); });
  if (found == latest.sessions.end())
    return {NavigationCode::Missing, {}};
  if (found->id.empty() || found->workspace.empty() || found->title.empty() ||
      NavigationProjectName(*found).empty())
    return {NavigationCode::Unavailable, {}};
  if (found->projectNameAmbiguous)
    return {NavigationCode::Ambiguous, {}};
  for (const auto &s : latest.sessions)
    if (s.source==found->source && !SameSession(s, *found) && s.title == found->title &&
        SameProject(NavigationProjectName(s), NavigationProjectName(*found)))
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
  Session renamed;
  renamed.id = L"renamed";
  renamed.workspace = L"workspace-id";
  renamed.path = L"C:/bilibili-project";
  renamed.title = L"report session";
  renamed.projectMetadataKnown = true;
  renamed.projectId = L"project-id";
  renamed.projectDisplayName = L"bilibili-report";
  Snapshot renamedSnapshot;
  renamedSnapshot.online = true;
  renamedSnapshot.sessions = {renamed};
  if (NavigationProjectName(renamed) != L"bilibili-report" ||
      PrepareNavigation(renamed, renamedSnapshot).code != NavigationCode::Idle)
    return false;
  renamedSnapshot.sessions[0].projectDisplayName = L"report renamed again";
  if (NavigationProjectName(PrepareNavigation(renamed, renamedSnapshot).session) !=
      L"report renamed again")
    return false;
  renamedSnapshot.sessions[0].projectNameAmbiguous = true;
  if (PrepareNavigation(renamed, renamedSnapshot).code != NavigationCode::Ambiguous)
    return false;
  renamedSnapshot.sessions[0].projectDisplayName.clear();
  if (PrepareNavigation(renamed, renamedSnapshot).code != NavigationCode::Unavailable)
    return false;
  Session named;
  named.title = L"字节266";
  named.path = L"C:/Project";
  auto longer = named;
  longer.title = L"字节2662";
  if (!AccessibleSessionName(L"字节2662分", named, {}) ||
      !AccessibleSessionName(L"字节266    2 分", named, {}) ||
      !AccessibleSessionName(L"字节266", named, {}) ||
      AccessibleSessionName(L"字节266其他任务", named, {}) ||
      AccessibleSessionName(L"字节2662分", named, {longer}) ||
      !AccessibleSessionName(L"字节26622分", longer, {named}))
    return false;
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
