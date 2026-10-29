#pragma once
#include <windows.h>
#include <cstdint>
#include <span>
#include <string>
#include <filesystem>
std::span<const uint8_t> EmbeddedBytes(int id);
std::string EmbeddedText(int id);
std::filesystem::path PetSettingsFile();
void ShowEmbeddedLicenses(HWND owner);
bool EmbeddedAssetsTests();
