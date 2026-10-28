#pragma once
#include <windows.h>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
class SpinePet {
  struct Impl;
  std::unique_ptr<Impl> impl;

public:
  SpinePet();
  ~SpinePet();
  bool Initialize(HWND window, const std::filesystem::path &folder,
                  bool hardware = false) noexcept;
  bool Ready() const noexcept;
  bool SetClip(const std::string &name, bool loop = true) noexcept;
  bool SetState(const std::wstring &state) noexcept;
  int DesiredFps() const noexcept;
  bool Render(float delta, int side) noexcept;
  bool Snapshot(const std::filesystem::path &path, int side) noexcept;
  std::vector<std::string> Clips() const;
  std::string CurrentClip() const;
  void Shutdown() noexcept;
  LRESULT Metric(unsigned field) const noexcept;
};
bool RenderSpinePreview(const std::filesystem::path &folder,
                        const std::filesystem::path &output,
                        bool hardware = true);
bool SpineModelTests(const std::filesystem::path &folder);
bool SpineLifecycleTests(const std::filesystem::path &folder);
