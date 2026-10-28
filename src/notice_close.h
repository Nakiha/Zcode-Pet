#pragma once
#include <windows.h>

void InitNoticeClose(HWND notice, bool preview);
void ShowNoticeClose(HWND notice, float dpi, bool visible);
bool IsNoticeClose(HWND notice, HWND window);
void DestroyNoticeClose(HWND notice);
