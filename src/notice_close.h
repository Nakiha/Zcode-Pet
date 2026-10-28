#pragma once
#include <windows.h>

void InitNoticeClose(HWND notice, bool preview);
void ShowNoticeClose(float dpi, bool visible);
bool IsNoticeClose(HWND window);
void DestroyNoticeClose();
