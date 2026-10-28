#include "frame_clock.h"
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;
namespace {
std::atomic_bool running = false, motion = false, continuous = false,
                 pending = false;
std::atomic_bool interaction = false;
std::atomic_int petFps = 0;
std::atomic<HMONITOR> currentMonitor = nullptr;
std::atomic_uint generation = 0;
std::atomic_int displayHz = 60;
std::atomic_bool usingVBlank = false;
std::atomic<double> averageFrame = 0;
std::mutex mutex;
std::condition_variable wake;
std::thread worker;
HANDLE cadenceWake = nullptr;
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
bool UiNeeded() {
  return motion.load() || continuous.load() || interaction.load();
}
bool Needed() { return UiNeeded() || petFps.load() > 0; }
void Signal() {
  if (cadenceWake)
    SetEvent(cadenceWake);
  wake.notify_all();
}
double FrameDeadline(double now, double previous, int fps) {
  const double interval = 1000. / std::max(1, fps);
  // Some detached/blanked outputs return S_OK from WaitForVBlank without
  // waiting. Never allow that driver behavior to become an unbounded loop.
  return previous > 0 && now - previous < interval * .9 ? previous + interval
                                                        : now;
}
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
                                        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
  if (!timer)
    timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
  ComPtr<IDXGIOutput> output;
  HMONITOR selected = nullptr;
  unsigned selectedGeneration = ~0u;
  double nextFrame = 0, lastSignal = 0;
  bool previousPetOnly = false;
  int previousRate = 0;
  auto waitUntil = [&](double deadline) {
    const double delay = std::max(0., deadline - Now());
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)std::max(1., delay * 10000.);
    if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
      HANDLE handles[] = {timer, cadenceWake};
      return WaitForMultipleObjects(cadenceWake ? 2 : 1, handles, FALSE,
                                    (DWORD)(delay + 100)) != WAIT_OBJECT_0 + 1;
    }
    if (cadenceWake)
      return WaitForSingleObject(cadenceWake,
                                 (DWORD)std::max(1., std::ceil(delay))) ==
             WAIT_TIMEOUT;
    Sleep((DWORD)std::max(1., std::ceil(delay)));
    return true;
  };
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
      lastSignal = 0;
    }
    const bool petOnly = !UiNeeded() && petFps.load() > 0;
    const int rate =
        std::max(1, petOnly ? std::min(petFps.load(), displayHz.load())
                            : displayHz.load());
    if (petOnly != previousPetOnly || rate != previousRate) {
      nextFrame = Now();
      lastSignal = 0;
      previousPetOnly = petOnly;
      previousRate = rate;
    }
    bool synced = !petOnly && output && SUCCEEDED(output->WaitForVBlank());
    usingVBlank = synced;
    if (synced) {
      const auto now = Now(), deadline = FrameDeadline(now, lastSignal, rate);
      if (deadline > now) {
        usingVBlank = false;
        if (!waitUntil(deadline))
          continue;
      }
    }
    if (!synced) {
      if (!petOnly)
        output.Reset();
      const double interval = 1000. / rate;
      const double now = Now();
      if (nextFrame < now - interval)
        nextFrame = now;
      nextFrame += interval;
      if (!waitUntil(nextFrame))
        continue;
    }
    lastSignal = Now();
    if (running && Needed() && !pending.exchange(true)) {
      if (!PostMessageW(target, VisualFrameMessage, petOnly ? 1 : 0, 0))
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
  cadenceWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  worker = std::thread(Run);
}
void StopFrameClock() {
  {
    std::lock_guard lock(mutex);
    running = false;
  }
  Signal();
  if (worker.joinable())
    worker.join();
  pending = false;
  if (cadenceWake)
    CloseHandle(cadenceWake);
  cadenceWake = nullptr;
}
void SetFrameClockMonitor(HMONITOR monitor, bool force) {
  const bool changed = currentMonitor.exchange(monitor) != monitor;
  if (changed || force) {
    ++generation;
    Signal();
  }
}
void RequestMotionFrames(bool enabled) {
  if (motion.load() == enabled)
    return;
  {
    std::lock_guard lock(mutex);
    motion = enabled;
  }
  Signal();
}
void RequestContinuousFrames(bool enabled) {
  if (continuous.load() == enabled)
    return;
  {
    std::lock_guard lock(mutex);
    continuous = enabled;
  }
  Signal();
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
  Signal();
}
void RequestPetFrames(int fps) {
  fps = std::clamp(fps, 0, 120);
  if (petFps.load() == fps)
    return;
  {
    std::lock_guard lock(mutex);
    petFps = fps;
  }
  Signal();
}
int FrameClockDisplayHz() { return displayHz; }
bool FrameClockUsesVBlank() { return usingVBlank; }
double FrameClockAverageMs() { return averageFrame; }
double VisualClockMs() { return Now(); }
bool FrameClockInteractionRequested() { return interaction; }
bool FrameClockFramesNeeded() { return Needed(); }
int FrameClockPetFps() { return petFps; }
bool FrameClockContinuousRequested() { return continuous; }
bool FrameClockTimingTests() {
  if (std::abs(FrameDeadline(100.1, 100, 120) - 108.3333333) > .001)
    return false;
  if (FrameDeadline(1000, 100, 120) != 1000)
    return false;
  double now = 100;
  for (int i = 0; i < 120; ++i)
    now = FrameDeadline(now + .01, now, 120);
  return std::abs(now - 1100) < .001 &&
         std::abs(FrameDeadline(100.1, 100, 60) - 116.6666667) < .001;
}
