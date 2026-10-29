#include "embedded_assets.h"
#include "resource.h"
#include <algorithm>

std::span<const uint8_t> EmbeddedBytes(int id) {
  const auto module = GetModuleHandleW(nullptr);
  const auto resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
  if (!resource)
    return {};
  const auto size = SizeofResource(module, resource);
  const auto data = LockResource(LoadResource(module, resource));
  return data ? std::span(static_cast<const uint8_t *>(data), size)
              : std::span<const uint8_t>{};
}
std::string EmbeddedText(int id) {
  const auto bytes = EmbeddedBytes(id);
  return bytes.empty()
             ? std::string{}
             : std::string(reinterpret_cast<const char *>(bytes.data()),
                           bytes.size());
}
std::filesystem::path PetSettingsFile() {
  wchar_t local[32768]{};
  if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768))
    return {};
  return std::filesystem::path(local) / L"ZCodePet/settings.ini";
}
namespace {
INT_PTR CALLBACK LicenseDialog(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_INITDIALOG) {
    SetWindowTextW(window, L"许可与素材来源");
    SetDlgItemTextW(window, IDOK, L"关闭");
    SetDlgItemTextW(window, IDC_LICENSE_TEXT,
                    reinterpret_cast<const wchar_t *>(l));
    SendDlgItemMessageW(window, IDC_LICENSE_TEXT, EM_SETSEL, 0, 0);
    return TRUE;
  }
  if (message == WM_SIZE) {
    RECT r{};
    GetClientRect(window, &r);
    const int inset = MulDiv(10, GetDpiForWindow(window), 96),
              button = MulDiv(80, GetDpiForWindow(window), 96),
              height = MulDiv(28, GetDpiForWindow(window), 96);
    MoveWindow(GetDlgItem(window, IDC_LICENSE_TEXT), inset, inset,
               std::max(1L, r.right - 2 * inset),
               std::max(1L, r.bottom - height - 3 * inset), TRUE);
    MoveWindow(GetDlgItem(window, IDOK), r.right - button - inset,
               r.bottom - height - inset, button, height, TRUE);
    return TRUE;
  }
  if (message == WM_COMMAND && (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL)) {
    EndDialog(window, 0);
    return TRUE;
  }
  return FALSE;
}
} // namespace
void ShowEmbeddedLicenses(HWND owner) {
  const auto source =
      std::string("CHARACTER CREDITS\r\n\r\n") + EmbeddedText(IDR_PET_CREDITS) +
      "\r\n\r\nSPINE RUNTIMES LICENSE\r\n\r\n" +
      EmbeddedText(IDR_SPINE_LICENSE) +
      "\r\n\r\nNLOHMANN/JSON LICENSE\r\n\r\n" + EmbeddedText(IDR_JSON_LICENSE);
  // A Win32 multiline edit expects CRLF, including notices authored on Unix.
  std::string text;
  text.reserve(source.size());
  for (size_t i = 0; i < source.size(); ++i) {
    if (source[i] == '\n' && (i == 0 || source[i - 1] != '\r'))
      text += '\r';
    text += source[i];
  }
  const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                        (int)text.size(), nullptr, 0);
  std::wstring wide(count, 0);
  MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), wide.data(),
                      count);
  DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_LICENSES),
                  owner, LicenseDialog, reinterpret_cast<LPARAM>(wide.c_str()));
}
bool EmbeddedAssetsTests() {
  const auto image = EmbeddedBytes(IDR_PET_PNG);
  return image.size() > 8 && image[0] == 137 && image[1] == 'P' &&
         image[2] == 'N' && image[3] == 'G' &&
         EmbeddedText(IDR_PET_JSON).find("4.2.43") != std::string::npos &&
         EmbeddedText(IDR_PET_ATLAS).find("leimi.png") != std::string::npos &&
         EmbeddedText(IDR_SPINE_LICENSE)
                 .find("Spine Runtimes License Agreement") !=
             std::string::npos &&
         EmbeddedText(IDR_JSON_LICENSE).find("MIT License") !=
             std::string::npos &&
         !EmbeddedText(IDR_PET_CREDITS).empty() && EmbeddedBytes(65534).empty() &&
         FindResourceW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_LICENSES), RT_DIALOG);
}
