#include "harness_cache.h"
#include "../third_party/json.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <optional>
namespace fs = std::filesystem;
using json = nlohmann::json;
namespace {
std::wstring Wide(const std::string &s) {
  if (s.empty())
    return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring r(n, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), r.data(), n);
  return r;
}
bool Safe(wchar_t c) {
  return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
         (c >= L'0' && c <= L'9') || c == L'.' || c == L'_' || c == L'-';
}
std::wstring Escape(wchar_t c) {
  wchar_t text[6];
  swprintf_s(text, L"~%04X", (unsigned)c);
  return text;
}
std::optional<HarnessCacheFact> Decode(const json &document,
                                       const std::wstring &id) {
  if (id.empty() || id == L"." || id == L".." ||
      !std::all_of(id.begin(), id.end(), Safe))
    return {};
  const auto version = document.value("version", 0);
  if (version < 3 || version > 7)
    return {};
  const auto &r = document.at("record");
  const auto &i = r.at("identity");
  if (i.value("formatVersion", -1) != 4)
    return {};
  HarnessCacheFact f;
  f.id = id;
  f.createdAt = i.at("createdAt").get<int64_t>();
  if (f.createdAt < 0)
    return {};
  f.cwd = Wide(i.value("cwd", std::string{}));
  f.seeded = i.value("isSeeded", false);
  const auto &rows = r.at("rows");
  if (rows.contains("title")) {
    const auto &t = rows.at("title");
    if (t.value("ver", -1) == 1 && t.at("val").is_string()) {
      f.title = Wide(t.at("val").get<std::string>());
      f.titleSeq = t.at("seq").get<int64_t>();
      if (f.titleSeq < 0)
        return {};
      if (f.title.size() > 512)
        f.title.resize(512);
    }
  }
  return f;
}
} // namespace
struct HarnessCacheIndex::Watch {
  HANDLE directory = INVALID_HANDLE_VALUE;
  OVERLAPPED operation{};
  alignas(DWORD) std::array<BYTE, 65536> buffer{};
  bool armed = false;
  explicit Watch(const fs::path &path) {
    directory =
        CreateFileW(path.c_str(), FILE_LIST_DIRECTORY,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Arm();
  }
  void Arm() {
    if (directory == INVALID_HANDLE_VALUE || !operation.hEvent)
      return;
    ResetEvent(operation.hEvent);
    armed = ReadDirectoryChangesW(
                directory, buffer.data(), (DWORD)buffer.size(), FALSE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
                    FILE_NOTIFY_CHANGE_SIZE,
                nullptr, &operation, nullptr) != FALSE;
  }
  ~Watch() {
    if (directory != INVALID_HANDLE_VALUE) {
      CancelIoEx(directory, &operation);
      if (armed) {
        DWORD got;
        GetOverlappedResult(directory, &operation, &got, TRUE);
      }
      CloseHandle(directory);
    }
    if (operation.hEvent)
      CloseHandle(operation.hEvent);
  }
};
HarnessCacheIndex::HarnessCacheIndex(fs::path folder)
    : directory(std::move(folder)) {}
HarnessCacheIndex::~HarnessCacheIndex() = default;
bool HarnessCacheIndex::Busy() const {
  return initial != end || !pending.empty();
}
void HarnessCacheIndex::Poll() {
  std::error_code ec;
  if (!initialized) {
    watch = std::make_unique<Watch>(directory);
    initial = fs::directory_iterator(
        directory, fs::directory_options::skip_permission_denied, ec);
    initialized = !ec;
  }
  if (watch && watch->armed &&
      WaitForSingleObject(watch->operation.hEvent, 0) == WAIT_OBJECT_0) {
    DWORD length = 0;
    if (GetOverlappedResult(watch->directory, &watch->operation, &length,
                            FALSE)) {
      if (!length) {
        initial = fs::directory_iterator(
            directory, fs::directory_options::skip_permission_denied, ec);
      } else
        for (size_t offset = 0;
             offset + sizeof(FILE_NOTIFY_INFORMATION) <= length;) {
          const auto *n = reinterpret_cast<const FILE_NOTIFY_INFORMATION *>(
              watch->buffer.data() + offset);
          if (offset + offsetof(FILE_NOTIFY_INFORMATION, FileName) +
                  n->FileNameLength >
              length)
            break;
          fs::path name(
              std::wstring(n->FileName, n->FileNameLength / sizeof(wchar_t)));
          if (name == name.filename() && name.extension() == L".json")
            pending.insert(directory / name);
          if (!n->NextEntryOffset)
            break;
          offset += n->NextEntryOffset;
        }
    } else
      initialized = false;
    watch->Arm();
  }
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(4);
  unsigned count = 0;
  while (initial != end && count++ < 128 &&
         std::chrono::steady_clock::now() < deadline) {
    auto path = initial->path();
    if (path.extension() == L".json")
      pending.insert(path);
    initial.increment(ec);
    if (ec) {
      initial = end;
      break;
    }
  }
  count = 0;
  while (!pending.empty() && count++ < 80 &&
         std::chrono::steady_clock::now() < deadline) {
    auto path = *pending.begin();
    pending.erase(pending.begin());
    auto size = fs::file_size(path, ec);
    if (ec) {
      facts.erase(path);
      continue;
    }
    if (size > 4 * 1024 * 1024)
      continue;
    try {
      ++recordReads;
      std::ifstream input(path, std::ios::binary);
      std::string text((size_t)size, '\0');
      input.read(text.data(), (std::streamsize)size);
      if ((uint64_t)input.gcount() != size)
        continue;
      auto value = json::parse(text, nullptr, true);
      auto f = Decode(value, path.stem().wstring());
      if (f) {
        f->modified = fs::last_write_time(path, ec);
        facts[path] = std::move(*f);
      } else
        facts.erase(path);
    } catch (...) { /* Atomic cache replacement may race this read; next
                       notification retries. */
    }
  }
  if (facts.size() > 80) {
    std::vector<fs::path> order;
    for (auto &[p, f] : facts)
      order.push_back(p);
    std::sort(order.begin(), order.end(), [&](auto &a, auto &b) {
      return facts.at(a).modified > facts.at(b).modified;
    });
    for (size_t i = 80; i < order.size(); ++i)
      facts.erase(order[i]);
  }
}
fs::path HarnessLogPath(const fs::path &root, const HarnessCacheFact &fact) {
  std::wstring project = L"_no-cwd";
  if (!fact.cwd.empty()) {
    std::wstring readable;
    bool separator = false;
    for (auto c : fact.cwd) {
      if (c == L'/' || c == L'\\' || c == L':') {
        if (!separator)
          readable += L'-';
        separator = true;
      } else {
        readable += Safe(c) ? std::wstring(1, c) : Escape(c);
        separator = false;
      }
    }
    auto at = readable.find_first_not_of(L'-');
    readable = at == std::wstring::npos ? L"root" : readable.substr(at);
    if (readable.size() > 251)
      readable.resize(251);
    project = L"--" + readable + L"--";
  }
  std::wstring id;
  for (auto c : fact.id)
    id += Safe(c) ? std::wstring(1, c) : Escape(c);
  if (id == L"." || id == L"..") {
    id.clear();
    for (auto c : fact.id)
      id += Escape(c);
  }
  return root / project / id / L"session.v4.jsonl.zstd";
}
bool ApplyHarnessCacheTitle(const HarnessCacheFact &f, Session &view,
                            int64_t createdAt, bool seeded,
                            int64_t logTitleSeq) {
  if (f.id != view.id || f.cwd != view.path || f.createdAt != createdAt ||
      f.seeded != seeded || f.title.empty() || f.titleSeq < logTitleSeq)
    return false;
  view.title = f.title;
  return true;
}
bool HarnessCacheTests() {
  const auto document = json::parse(
      R"({"version":7,"record":{"identity":{"formatVersion":4,"createdAt":10,"cwd":"D:/test"},"rows":{"title":{"ver":1,"seq":300,"val":"title beyond 1.17 MiB frame"}}}})");
  auto fact = Decode(document, L"session-test");
  if (!fact || fact->title.empty() || fact->titleSeq != 300)
    return false;
  Session view;
  view.id = L"session-test";
  view.path = L"D:/test";
  if (!ApplyHarnessCacheTitle(*fact, view, 10, false, 299) ||
      view.title != fact->title)
    return false;
  if (ApplyHarnessCacheTitle(*fact, view, 11, false, 299) ||
      ApplyHarnessCacheTitle(*fact, view, 10, true, 299) ||
      ApplyHarnessCacheTitle(*fact, view, 10, false, 301))
    return false;

  if (HarnessLogPath(L"C:/root/sessions", *fact) !=
      fs::path(
          L"C:/root/sessions/--D-test--/session-test/session.v4.jsonl.zstd"))
    return false;
  if (Decode(document, L".."))
    return false;
  auto bad = document;
  bad["record"]["identity"]["formatVersion"] = 99;
  if (Decode(bad, L"test"))
    return false;
  bad = document;
  bad["record"]["rows"]["title"]["ver"] = 99;
  auto unsupported = Decode(bad, L"test");
  if (!unsupported || !unsupported->title.empty())
    return false;
  wchar_t temp[MAX_PATH]{}, unique[MAX_PATH]{};
  GetTempPathW(MAX_PATH, temp);
  if (!GetTempFileNameW(temp, L"dsc", 0, unique))
    return false;
  DeleteFileW(unique);
  fs::path root = unique;
  fs::create_directory(root);
  const auto file = root / L"session-test.json";
  bool good = false;
  {
    std::ofstream out(file);
    out << document.dump();
  }
  {
    HarnessCacheIndex index(root);
    for (int i = 0; i < 100 && index.facts.empty(); ++i) {
      index.Poll();
      Sleep(2);
    }
    if (index.facts.size() == 1) {
      auto reads = index.recordReads;
      for (int i = 0; i < 20; ++i)
        index.Poll();
      good = index.recordReads == reads;
      auto changed = document;
      changed["record"]["rows"]["title"]["val"] = "renamed";
      changed["record"]["rows"]["title"]["seq"] = 301;
      {
        std::ofstream out(file);
        out << changed.dump();
      }
      bool updated = false;
      for (int i = 0; i < 100 && !updated; ++i) {
        index.Poll();
        auto f = index.facts.find(file);
        updated = f != index.facts.end() && f->second.title == L"renamed";
        if (!updated)
          Sleep(5);
      }
      good &= updated;
    }
  }
  DeleteFileW(file.c_str());
  RemoveDirectoryW(root.c_str());
  return good;
}
