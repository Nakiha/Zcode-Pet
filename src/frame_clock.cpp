#include "frame_clock.h"
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <algorithm>

using Microsoft::WRL::ComPtr;
namespace {
std::atomic_bool running = false, motion = false, continuous = false, pending = false;
std::atomic_bool interaction = false;
std::atomic<HMONITOR> currentMonitor = nullptr;
std::atomic_uint generation = 0;
std::atomic_int displayHz = 60;
std::atomic_bool usingVBlank = false;
std::atomic<double> averageFrame = 0;
std::mutex mutex;
std::condition_variable wake;
std::thread worker;
HWND target;
double lastUiFrame = 0;

double Now() {
  static const double frequency = [] {
    LARGE_INTEGER value;
    QueryPerformanceFrequency(&value);
    return value.QuadPart / 1000.;
  }();
  LARGE_INTEGER value;
  QueryPerformanceCounter(&value);
  return value.QuadPart / frequency;
}
bool Needed() { return motion.load() || continuous.load() || interaction.load(); }
ComPtr<IDXGIOutput> OutputFor(HMONITOR monitor) {
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    return {};
  for (UINT a = 0;; ++a) {
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapters1(a, &adapter)))
      break;
    if (!adapter)
      continue;
    for (UINT o = 0;; ++o) {
      ComPtr<IDXGIOutput> output;
      if (FAILED(adapter->EnumOutputs(o, &output)))
        break;
      if (!output)
        continue;
      DXGI_OUTPUT_DESC desc{};
      if (SUCCEEDED(output->GetDesc(&desc)) && desc.AttachedToDesktop &&
          desc.Monitor == monitor)
        return output;
    }
  }
  return {};
}
int RefreshRate(HMONITOR monitor) {
  MONITORINFOEXW info{};
  info.cbSize = sizeof(info);
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  if (GetMonitorInfoW(monitor, &info) &&
      EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
      mode.dmDisplayFrequency > 1)
    return (int)mode.dmDisplayFrequency;
  return 60;
}
void Run() {
  const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
      CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!timer)
    timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
  ComPtr<IDXGIOutput> output;
  HMONITOR selected = nullptr;
  unsigned selectedGeneration = ~0u;
  double nextFrame = 0;
  while (running) {
    {
      std::unique_lock lock(mutex);
      wake.wait(lock, [] { return !running || Needed(); });
    }
    if (!running)
      break;
    auto monitor = currentMonitor.load();
    const auto version = generation.load();
    if (monitor != selected || version != selectedGeneration) {
      selected = monitor;
      selectedGeneration = version;
      output = OutputFor(monitor);
      displayHz = RefreshRate(monitor);
      nextFrame = Now();
    }
    bool synced = output && SUCCEEDED(output->WaitForVBlank());
    usingVBlank = synced;
    if (!synced) {
      output.Reset();
      const double interval = 1000. / std::max(1, displayHz.load());
      const double now = Now();
      if (nextFrame < now - interval)
        nextFrame = now;
      nextFrame += interval;
      LARGE_INTEGER due;
      due.QuadPart = -(LONGLONG)std::max(1., (nextFrame - now) * 10000.);
      if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
        WaitForSingleObject(timer, (DWORD)(interval + 100));
      else
        Sleep((DWORD)std::max(1., interval));
    }
    if (running && Needed() && !pending.exchange(true)) {
      if (!PostMessageW(target, VisualFrameMessage, 0, 0))
        pending = false;
    }
  }
  if (timer)
    CloseHandle(timer);
  output.Reset();
  if (SUCCEEDED(apartment))
    CoUninitialize();
}
} // namespace

void StartFrameClock(HWND dispatcher) {
  target = dispatcher;
  currentMonitor = MonitorFromWindow(dispatcher, MONITOR_DEFAULTTONEAREST);
  running = true;
  worker = std::thread(Run);
}
void StopFrameClock() {
  {
    std::lock_guard lock(mutex);
    running = false;
  }
  wake.notify_all();
  if (worker.joinable())
    worker.join();
  pending = false;
}
void SetFrameClockMonitor(HMONITOR monitor, bool force) {
  const bool changed = currentMonitor.exchange(monitor) != monitor;
  if (changed || force) {
    ++generation;
    wake.notify_all();
  }
}
void RequestMotionFrames(bool enabled) {
  if (motion.load() == enabled)
    return;
  {
    std::lock_guard lock(mutex);
    motion = enabled;
  }
  wake.notify_all();
}
void RequestContinuousFrames(bool enabled) {
  if (continuous.load() == enabled)
    return;
  {
    std::lock_guard lock(mutex);
    continuous = enabled;
  }
  wake.notify_all();
}
void BeginVisualFrame() {
  pending = false;
  const double now = Now();
  if (lastUiFrame > 0 && now - lastUiFrame < 250) {
    const double previous = averageFrame.load();
    averageFrame = previous > 0 ? previous * .9 + (now - lastUiFrame) * .1
                                : now - lastUiFrame;
  }
  lastUiFrame = now;
}
void RequestInteractionFrames(bool enabled) {
  if (interaction.load() == enabled)
    return;
  {
    std::lock_guard lock(mutex);
    interaction = enabled;
  }
  wake.notify_all();
}
int FrameClockDisplayHz() { return displayHz; }
bool FrameClockUsesVBlank() { return usingVBlank; }
double FrameClockAverageMs() { return averageFrame; }
double VisualClockMs() { return Now(); }
bool FrameClockInteractionRequested() { return interaction; }
bool FrameClockFramesNeeded() { return Needed(); }
