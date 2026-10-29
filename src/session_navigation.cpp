#include "session_navigation.h"
#include <ole2.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace {
std::mutex mutex;
std::condition_variable ready;
std::thread worker;
struct NavigationRequest { Session session; std::vector<Session> peers; };
std::optional<NavigationRequest> request;
std::optional<NavigationResult> completed;
NavigationResult last;
std::atomic_bool stopping{false}, busy{false};
std::atomic_int scanWindows{0}, scanTitles{0}, scanRows{0}, scanProjectNames{0},
    scanProjectType{0};
HWND recipient;
UINT completedMessage;

std::wstring Name(IUIAutomationElement *element) {
  BSTR value = nullptr;
  if (!element || FAILED(element->get_CurrentName(&value)))
    return {};
  std::wstring result = value ? value : L"";
  SysFreeString(value);
  return result;
}
ComPtr<IUIAutomationCondition> Property(IUIAutomation *uia, PROPERTYID id,
                                        const std::wstring &text,
                                        bool ignoreCase = false) {
  VARIANT value{};
  value.vt = VT_BSTR;
  value.bstrVal = SysAllocString(text.c_str());
  ComPtr<IUIAutomationCondition> condition;
  uia->CreatePropertyConditionEx(id, value,
                                 ignoreCase ? PropertyConditionFlags_IgnoreCase
                                            : PropertyConditionFlags_None,
                                 &condition);
  VariantClear(&value);
  return condition;
}
std::vector<ComPtr<IUIAutomationElement>>
FindAll(IUIAutomationElement *root, TreeScope scope,
        IUIAutomationCondition *condition) {
  ComPtr<IUIAutomationElementArray> items;
  std::vector<ComPtr<IUIAutomationElement>> result;
  if (!root || !condition || FAILED(root->FindAll(scope, condition, &items)) ||
      !items)
    return result;
  int length = 0;
  items->get_Length(&length);
  for (int i = 0; i < length && !stopping; ++i) {
    ComPtr<IUIAutomationElement> item;
    if (SUCCEEDED(items->GetElement(i, &item)) && item)
      result.push_back(item);
  }
  return result;
}
bool IsZCodeWindow(HWND window) {
  if ((!IsWindowVisible(window) && !IsIconic(window)) ||
      GetWindow(window, GW_OWNER))
    return false;
  wchar_t title[128]{};
  GetWindowTextW(window, title, 128);
  if (wcscmp(title, L"ZCode") != 0)
    return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process)
    return false;
  wchar_t path[32768]{};
  DWORD length = 32768;
  const bool read =
      QueryFullProcessImageNameW(process, 0, path, &length) != FALSE;
  CloseHandle(process);
  return read && _wcsicmp(std::filesystem::path(path).filename().c_str(),
                          L"ZCode.exe") == 0;
}
std::vector<HWND> Windows() {
  std::vector<HWND> windows;
  EnumWindows(
      [](HWND window, LPARAM context) -> BOOL {
        if (IsZCodeWindow(window))
          reinterpret_cast<std::vector<HWND> *>(context)->push_back(window);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&windows));
  return windows;
}
// 项目分组名是列表的兄弟节点；不能在整棵树中只按标题找，正文也可能重复标题。
bool InProject(IUIAutomation *uia, IUIAutomationElement *row,
               const std::wstring &project) {
  ComPtr<IUIAutomationTreeWalker> walker;
  // Chromium 的 Control View 可能折叠无语义
  // div；原始树保留项目与列表的共同容器。
  if (FAILED(uia->get_RawViewWalker(&walker)))
    return false;
  ComPtr<IUIAutomationElement> ancestor = row;
  auto projectCondition = Property(uia, UIA_NamePropertyId, project, true);
  VARIANT listType{};
  listType.vt = VT_I4;
  listType.lVal = UIA_ListControlTypeId;
  ComPtr<IUIAutomationCondition> listCondition;
  uia->CreatePropertyCondition(UIA_ControlTypePropertyId, listType,
                               &listCondition);
  for (int depth = 0; depth < 3 && ancestor && !stopping; ++depth) {
    ComPtr<IUIAutomationElement> parent;
    if (FAILED(walker->GetParentElement(ancestor.Get(), &parent)) || !parent)
      break;
    const auto rowBranch = ancestor;
    ancestor = parent;
    ComPtr<IUIAutomationElement> child;
    walker->GetFirstChildElement(ancestor.Get(), &child);
    for (int n = 0; child && n < 128 && !stopping; ++n) {
      CONTROLTYPEID type = 0;
      child->get_CurrentControlType(&type);
      // 项目标题可能再包一层无语义 div。只看列表的兄弟标题分支，
      // 不允许从另一个包含会话列表的项目分支借用标题来匹配。
      BOOL sameBranch = FALSE;
      uia->CompareElements(child.Get(), rowBranch.Get(), &sameBranch);
      const bool hasHeader =
          !sameBranch &&
          (SameProject(Name(child.Get()), project) ||
           !FindAll(child.Get(), TreeScope_Descendants, projectCondition.Get())
                .empty());
      if (hasHeader &&
          FindAll(child.Get(), TreeScope_Subtree, listCondition.Get())
              .empty()) {
        ++scanProjectNames;
        scanProjectType = type;
        return true;
      }
      ComPtr<IUIAutomationElement> next;
      walker->GetNextSiblingElement(child.Get(), &next);
      child = next;
    }
  }
  return false;
}
ComPtr<IUIAutomationElement> ListRow(IUIAutomation *uia,
                                     IUIAutomationElement *text) {
  ComPtr<IUIAutomationTreeWalker> walker;
  uia->get_ControlViewWalker(&walker);
  ComPtr<IUIAutomationElement> node = text;
  for (int depth = 0; walker && node && depth < 4; ++depth) {
    CONTROLTYPEID type = 0;
    node->get_CurrentControlType(&type);
    if (type == UIA_ListItemControlTypeId) {
      BOOL focusable = FALSE;
      node->get_CurrentIsKeyboardFocusable(&focusable);
      // 普通对话正文的 li 不可聚焦，不能把正文中的同名文本当作侧栏会话。
      return focusable ? node : ComPtr<IUIAutomationElement>{};
    }
    ComPtr<IUIAutomationElement> parent;
    walker->GetParentElement(node.Get(), &parent);
    node = parent;
  }
  return {};
}
bool ContainsTitle(IUIAutomation *uia, IUIAutomationElement *row,
                   const Session &session, const std::vector<Session> &peers) {
  if (AccessibleSessionName(Name(row), session, peers))
    return true;
  ComPtr<IUIAutomationCondition> any;
  uia->CreateTrueCondition(&any);
  for (const auto &child : FindAll(row, TreeScope_Descendants, any.Get()))
    if (AccessibleSessionName(Name(child.Get()), session, peers))
      return true;
  return false;
}
bool Foreground(HWND window) {
  return GetAncestor(GetForegroundWindow(), GA_ROOT) == window;
}
bool Activate(HWND window) {
  if (IsIconic(window))
    ShowWindowAsync(window, SW_RESTORE);
  if (!IsWindowVisible(window))
    ShowWindowAsync(window, SW_SHOW);
  SetForegroundWindow(window);
  for (int i = 0; i < 20 && !stopping; ++i) {
    if (Foreground(window) && !IsIconic(window))
      return true;
    Sleep(20);
  }
  return false;
}
NavigationResult Open(IUIAutomation *uia, const Session &session, const std::vector<Session> &peers) {
  if (!uia)
    return {NavigationCode::AutomationFailed};
  const auto deadline = GetTickCount64() + 6000;
  const auto windows = Windows();
  scanWindows = (int)windows.size();
  if (windows.empty())
    return {NavigationCode::AppClosed};
  ComPtr<IUIAutomationCondition> title;
  VARIANT rowType{}; rowType.vt = VT_I4; rowType.lVal = UIA_ListItemControlTypeId;
  uia->CreatePropertyCondition(UIA_ControlTypePropertyId, rowType, &title);
  if (!title)
    return {NavigationCode::AutomationFailed};
  struct Match {
    HWND window;
    ComPtr<IUIAutomationElement> row;
  };
  std::vector<Match> matches;
  const auto project = ProjectName(session.path);
  for (HWND window : windows) {
    if (stopping || GetTickCount64() > deadline)
      return {NavigationCode::Cancelled};
    ComPtr<IUIAutomationElement> root;
    if (FAILED(uia->ElementFromHandle(window, &root)))
      continue;
    for (const auto &text :
         FindAll(root.Get(), TreeScope_Descendants, title.Get())) {
      if (stopping || GetTickCount64() > deadline)
        return {NavigationCode::Cancelled};
      ++scanTitles;
      auto row = ListRow(uia, text.Get());
      if (row)
        ++scanRows;
      if (!row || !InProject(uia, row.Get(), project) ||
          !ContainsTitle(uia, row.Get(), session, peers))
        continue;
      bool duplicate = false;
      for (const auto &match : matches) {
        BOOL equal = FALSE;
        uia->CompareElements(match.row.Get(), row.Get(), &equal);
        duplicate |= equal != FALSE;
      }
      if (!duplicate)
        matches.push_back({window, row});
    }
  }
  const int count = (int)matches.size();
  if (count != 1)
    return {count ? NavigationCode::Ambiguous : NavigationCode::NotFound,
            count};
  auto &match = matches.front();
  if (stopping || GetTickCount64() > deadline)
    return {NavigationCode::Cancelled, count};
  if (!Activate(match.window))
    return {NavigationCode::FocusFailed, count};
  BOOL enabled = FALSE, offscreen = TRUE;
  if (FAILED(match.row->get_CurrentIsEnabled(&enabled)) ||
      FAILED(match.row->get_CurrentIsOffscreen(&offscreen)))
    return {NavigationCode::AutomationFailed, count};
  if (offscreen)
    return {NavigationCode::Offscreen, count};
  if (!enabled)
    return {NavigationCode::AutomationFailed, count};
  if (!ContainsTitle(uia, match.row.Get(), session, peers) ||
      !InProject(uia, match.row.Get(), project))
    return {NavigationCode::NotFound, count};
  if (stopping || !Foreground(match.window))
    return {NavigationCode::FocusFailed, count};
  ComPtr<IUIAutomationInvokePattern> invoke;
  if (SUCCEEDED(match.row->GetCurrentPatternAs(UIA_InvokePatternId,
                                               IID_PPV_ARGS(&invoke))) &&
      invoke) {
    if (stopping || !Foreground(match.window))
      return {NavigationCode::FocusFailed, count};
    // 动作结果未知时不再尝试其他动作，避免重复触发。
    return {SUCCEEDED(invoke->Invoke()) ? NavigationCode::Opened
                                        : NavigationCode::AutomationFailed,
            count, NavigationAction::Invoke};
  }
  ComPtr<IUIAutomationLegacyIAccessiblePattern> legacy;
  if (SUCCEEDED(match.row->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId,
                                               IID_PPV_ARGS(&legacy))) &&
      legacy) {
    BSTR action = nullptr;
    legacy->get_CurrentDefaultAction(&action);
    const bool available = action && *action;
    SysFreeString(action);
    if (available) {
      if (stopping || !Foreground(match.window))
        return {NavigationCode::FocusFailed, count};
      return {SUCCEEDED(legacy->DoDefaultAction())
                  ? NavigationCode::Opened
                  : NavigationCode::AutomationFailed,
              count, NavigationAction::Legacy};
    }
  }
  if (FAILED(match.row->SetFocus()))
    return {NavigationCode::FocusFailed, count};
  ComPtr<IUIAutomationElement> focused;
  uia->GetFocusedElement(&focused);
  BOOL same = FALSE;
  if (focused)
    uia->CompareElements(focused.Get(), match.row.Get(), &same);
  // 只允许确切列表项持有焦点；绝不把 Enter 发到编辑器、消息框或用户切走的窗口。
  if (stopping || !same || !Foreground(match.window))
    return {NavigationCode::FocusFailed, count};
  for (int key : {VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN, VK_RWIN})
    if (GetAsyncKeyState(key) & 0x8000)
      return {NavigationCode::FocusFailed, count};
  INPUT keys[2]{};
  keys[0].type = keys[1].type = INPUT_KEYBOARD;
  keys[0].ki.wVk = keys[1].ki.wVk = VK_RETURN;
  keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
  const bool sent = SendInput(2, keys, sizeof(INPUT)) == 2;
  return {sent ? NavigationCode::Opened : NavigationCode::AutomationFailed,
          count, NavigationAction::Keyboard};
}
} // namespace
void StartSessionNavigation(HWND target, UINT message) {
  recipient = target;
  completedMessage = message;
  stopping = false;
  worker = std::thread([] {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
      ComPtr<IUIAutomation> uia;
      if (SUCCEEDED(initialized))
        CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&uia));
      ComPtr<IUIAutomation2> settings;
      if (uia && SUCCEEDED(uia.As(&settings))) {
        settings->put_ConnectionTimeout(1000);
        settings->put_TransactionTimeout(1000);
      }
      for (;;) {
        NavigationRequest selected;
        {
          std::unique_lock lock(mutex);
          ready.wait(lock, [] { return stopping || request.has_value(); });
          if (stopping)
            break;
          selected = std::move(*request);
          request.reset();
        }
        NavigationResult result;
        scanWindows = 0;
        scanTitles = 0;
        scanRows = 0;
        scanProjectNames = 0;
        scanProjectType = 0;
        try {
          result = Open(uia.Get(), selected.session, selected.peers);
        } catch (...) {
          result.code = NavigationCode::AutomationFailed;
        }
        {
          std::lock_guard lock(mutex);
          last = result;
          completed = result;
          // 结果被主线程取走前保持 busy，防止旧结果覆盖下一次请求的反馈。
        }
        if (!stopping)
          PostMessageW(recipient, completedMessage, 0, 0);
      }
    }
    if (SUCCEEDED(initialized))
      CoUninitialize();
  });
}
void StopSessionNavigation() {
  stopping = true;
  ready.notify_one();
  if (worker.joinable())
    worker.join();
  std::lock_guard lock(mutex);
  request.reset();
  completed.reset();
  busy = false;
}
bool NavigateToSession(const Session &session, const std::vector<Session> &peers) {
  std::lock_guard lock(mutex);
  if (stopping || busy || !worker.joinable())
    return false;
  busy = true;
  last = {NavigationCode::Busy};
  request = NavigationRequest{session, peers};
  ready.notify_one();
  return true;
}
bool SessionNavigationBusy() { return busy; }
std::optional<NavigationResult> TakeNavigationResult() {
  std::lock_guard lock(mutex);
  auto result = completed;
  completed.reset();
  busy = false;
  return result;
}
NavigationResult LastNavigationResult() {
  std::lock_guard lock(mutex);
  return last;
}
int NavigationDiagnostic(unsigned field) {
  return field == 0   ? scanWindows.load()
         : field == 1 ? scanTitles.load()
         : field == 2 ? scanRows.load()
         : field == 3 ? scanProjectNames.load()
                      : scanProjectType.load();
}
