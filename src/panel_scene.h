#pragma once
#include "presentation.h"
#include "backdrop.h"

// Only visual inputs belong here. A metadata refresh with identical pixels
// must not rebuild the static texture; animation phase is drawn separately.
struct PanelVisualRow {
  std::wstring title, age, status;
  SessionState state{};
  float y = 0, hop = 0, feedback = 0;
  bool divider = false;
  bool operator==(const PanelVisualRow &) const = default;
};
struct PanelScene {
  float width = 0, height = 0, dpi = 1, strength = .75f;
  bool experimental = false, material = false;
  BackdropMode backdrop = BackdropMode::Acrylic;
  std::wstring message;
  std::vector<PanelVisualRow> rows;
  bool operator==(const PanelScene &) const = default;
};
inline bool PanelSceneTests() {
  PanelScene current;
  current.rows.push_back({L"会话", L"1s", L"工作中", SessionState::Running});
  auto same = current;
  if (!(same == current))
    return false;
  // Changes to age/layout, state, name, hover, hop, material and typography
  // all invalidate cached pixels; clock phase alone is deliberately absent.
  auto changed = [&](auto edit) {
    auto next = current;
    edit(next);
    return !(next == current);
  };
  return changed([](auto &s) { s.rows[0].age = L"1m"; }) &&
         changed([](auto &s) { s.rows[0].title = L"重命名"; }) &&
         changed([](auto &s) { s.rows[0].state = SessionState::Completed; }) &&
         changed([](auto &s) { s.rows[0].feedback = .16f; }) &&
         changed([](auto &s) { s.rows[0].hop = -2; }) &&
         changed([](auto &s) { s.rows[0].y = -34; }) &&
         changed([](auto &s) { s.rows[0].divider = true; }) &&
         changed([](auto &s) { s.dpi = 2; }) &&
         changed([](auto &s) { s.width = 300; }) &&
         changed([](auto &s) { s.height = 34; }) &&
         changed([](auto &s) { s.experimental = true; }) &&
         changed([](auto &s) { s.strength = 1; }) &&
         changed([](auto &s) { s.material = true; }) &&
         changed([](auto &s) { s.backdrop = BackdropMode::Mica; }) &&
         changed([](auto &s) { s.message = L"读取失败"; }) &&
         ReadBackdropMode(99) == BackdropMode::Acrylic &&
         BackdropType(BackdropMode::Mica) == DWMSBT_MAINWINDOW &&
         BackdropType(BackdropMode::Solid) == DWMSBT_NONE;
}
