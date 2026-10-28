#pragma once
#include <windows.h>

inline constexpr UINT VisualFrameMessage = WM_APP + 27;
inline constexpr UINT VisualFrameInfoQuery = WM_APP + 28;
void StartFrameClock(HWND dispatcher);
void StopFrameClock();
void SetFrameClockMonitor(HMONITOR monitor, bool force = false);
void RequestMotionFrames(bool enabled);
void RequestContinuousFrames(bool enabled);
void RequestInteractionFrames(bool enabled);
void BeginVisualFrame();
int FrameClockDisplayHz();
bool FrameClockUsesVBlank();
double FrameClockAverageMs();
double VisualClockMs();
bool FrameClockInteractionRequested();
bool FrameClockFramesNeeded();
