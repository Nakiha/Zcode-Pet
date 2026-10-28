#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <string>
bool DrawStatusBadge(ID2D1RenderTarget *target, ID2D1Factory *graphics,
                     IDWriteFactory *text, const std::wstring &label,
                     const D2D1_RECT_F &bounds, D2D1_COLOR_F fill,
                     D2D1_COLOR_F ink);
bool TextInkBounds(ID2D1Factory *graphics, IDWriteTextLayout *layout,
                   D2D1_RECT_F &bounds);
float CenteredTextOriginY(ID2D1Factory *graphics, IDWriteTextLayout *layout,
                          float height);
bool StatusBadgeTests();
ID2D1StrokeStyle *RoundStatusStroke(ID2D1Factory *graphics);
void DrawCheckMark(ID2D1RenderTarget *target, ID2D1Factory *graphics,
                   ID2D1Brush *brush, float x, float y, float scale,
                   float strokeWidth);
