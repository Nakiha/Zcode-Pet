#include "data_sources.h"
#include "../third_party/json.hpp"
#include "../third_party/zstd/zstd.h"
#include "../third_party/zstd/zstd_errors.h"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
using json = nlohmann::json;
namespace fs = std::filesystem;
namespace {
std::wstring Wide(const std::string &s) {
  if (s.empty())
    return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring r(n, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), r.data(), n);
  return r;
}
struct Fold {
  Session s;
  bool header = false, hidden = false;
  std::set<std::wstring> approvals;
  void Apply(const json &v) {
    const auto type = v.value("type", std::string{});
    if (type == "session") {
      if (v.value("version", -1) != 4)
        throw std::runtime_error("unsupported Harness session version");
      s.source = SourceId::DeepSeek;
      s.id = Wide(v.at("id").get<std::string>());
      s.path = Wide(v.value("cwd", std::string{}));
      s.workspace = L"deepseek:" + s.path;
      s.title = L"DeepSeek 会话";
      s.updated = v.value("createdAt", int64_t{});
      hidden = v.value("origin", std::string{}) == "subagent";
      header = true;
      return;
    }
    if (!header)
      throw std::runtime_error("missing Harness header");
    const auto &d = v.at("data");
    auto at = v.value("time", int64_t{});
    s.updated = std::max(s.updated, at);
    if (type == "session/title")
      s.title = Wide(d.at("title").get<std::string>());
    if (type == "turn/start") {
      s.turnId = std::to_wstring(d.at("turn").get<int64_t>());
      s.startedAt = at;
      s.endedAt = 0;
      s.running = true;
      s.result.clear();
      approvals.clear();
    }
    if (type == "turn/end" &&
        s.turnId == std::to_wstring(d.at("turn").get<int64_t>())) {
      auto reason = d.at("reason").value("kind", std::string{});
      s.running = false;
      s.endedAt = at;
      approvals.clear();
      s.result = reason == "completed" ? L"completed"
                 : reason == "error"   ? L"error"
                                       : L"interrupted";
    }
    if (type == "approval/asked" && s.running) {
      s.interactionId = Wide(d.at("id").get<std::string>());
      approvals.insert(s.interactionId);
      s.waitingSince = at;
      s.questionPreview = Wide(d.value("reason", std::string("等待操作许可")));
    }
    if (type == "approval/decided") {
      approvals.erase(Wide(d.at("id").get<std::string>()));
      s.resumedAt = at;
    }
    if (s.title.size() > 512)
      s.title.resize(512);
    if (s.questionPreview.size() > 512)
      s.questionPreview.resize(512);
    if (approvals.size() > 256)
      throw std::runtime_error("too many pending approvals");
    s.waiting = !approvals.empty();
  }
};
struct Cursor {
  Fold fold;
  uint64_t offset = 0, observedSize = 0;
  FILETIME written{}, creation{};
  DWORD volume = 0, indexHigh = 0, indexLow = 0;
  bool failed = false;
};
struct Reader {
  fs::path root;
  std::map<fs::path, Cursor> cursors;
  fs::recursive_directory_iterator scan, end;
  ULONGLONG nextScan = 0;
  uint64_t bytes = 0, parses = 0;
  size_t nextRead = 0;
  void Discover() {
    std::error_code ec;
    if (scan == end && GetTickCount64() >= nextScan) {
      scan = fs::recursive_directory_iterator(
          root, fs::directory_options::skip_permission_denied, ec);
      nextScan = GetTickCount64() + 30000;
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(4);
    unsigned visited = 0;
    while (scan != end && visited++ < 256 &&
           std::chrono::steady_clock::now() < deadline) {
      auto entry = *scan;
      auto depth = scan.depth();
      if (entry.is_directory(ec) && depth >= 2)
        scan.disable_recursion_pending();
      const auto name = entry.path().filename().wstring();
      if (depth == 2 &&
          (name == L"session.v4.jsonl" || name == L"session.v4.jsonl.zstd"))
        cursors.try_emplace(entry.path());
      scan.increment(ec);
      if (ec) {
        scan = end;
        break;
      }
    }
    // Bound retained state; recent files win. Discovery work is sliced across
    // polls.
    if (cursors.size() > 80) {
      std::vector<std::pair<fs::file_time_type, fs::path>> rank;
      for (auto &[p, c] : cursors) {
        auto t = fs::last_write_time(p, ec);
        rank.push_back({t, p});
      }
      std::sort(rank.begin(), rank.end(), std::greater<>());
      for (size_t i = 80; i < rank.size(); ++i)
        cursors.erase(rank[i].second);
    }
  }
  bool Read(const fs::path &p, Cursor &c, size_t &budget) {
    WIN32_FILE_ATTRIBUTE_DATA metadata{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &metadata))
      return false;
    const uint64_t knownSize =
        (uint64_t(metadata.nFileSizeHigh) << 32) | metadata.nFileSizeLow;
    if ((c.offset == knownSize || (c.failed && c.observedSize == knownSize)) &&
        CompareFileTime(&c.written, &metadata.ftLastWriteTime) == 0 &&
        CompareFileTime(&c.creation, &metadata.ftCreationTime) == 0)
      return true;
    HANDLE h =
        CreateFileW(p.c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
      return false;
    struct Close {
      HANDLE h;
      ~Close() { CloseHandle(h); }
    } close{h};
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(h, &info))
      return false;
    const uint64_t size =
        (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    const bool identity =
        c.volume != info.dwVolumeSerialNumber ||
        c.indexHigh != info.nFileIndexHigh ||
        c.indexLow != info.nFileIndexLow ||
        CompareFileTime(&c.creation, &info.ftCreationTime) != 0;
    if (identity ||
        (c.failed &&
         (size != c.observedSize ||
          CompareFileTime(&c.written, &info.ftLastWriteTime) != 0)) ||
        size < c.offset ||
        (size == c.offset &&
         CompareFileTime(&c.written, &info.ftLastWriteTime) != 0))
      c = Cursor{};
    c.volume = info.dwVolumeSerialNumber;
    c.indexHigh = info.nFileIndexHigh;
    c.indexLow = info.nFileIndexLow;
    c.creation = info.ftCreationTime;
    if (c.failed)
      return true;
    if (size == c.offset)
      return true;
    if (!budget)
      return true;
    const auto count =
        (DWORD)std::min<uint64_t>({size - c.offset, budget, 4 * 1024 * 1024});
    std::vector<char> input(count);
    LARGE_INTEGER position{};
    position.QuadPart = c.offset;
    DWORD got = 0;
    if (!SetFilePointerEx(h, position, nullptr, FILE_BEGIN) ||
        !ReadFile(h, input.data(), count, &got, nullptr))
      return false;
    budget -= got;
    bytes += got;
    input.resize(got);
    size_t used = 0;
    try {
      const bool compressed = p.extension() == L".zstd";
      auto records = [&](const char *data, size_t n) {
        size_t start = 0;
        for (size_t i = 0; i < n; ++i)
          if (data[i] == '\n') {
            auto v = json::parse(data + start, data + i);
            c.fold.Apply(v);
            ++parses;
            start = i + 1;
          }
        return start;
      };
      if (compressed) {
        const auto decodeDeadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
        while (used < input.size() &&
               std::chrono::steady_clock::now() < decodeDeadline) {
          auto frame = ZSTD_findFrameCompressedSize(input.data() + used,
                                                    input.size() - used);
          if (ZSTD_isError(frame)) {
            if (ZSTD_getErrorCode(frame) != ZSTD_error_srcSize_wrong)
              throw std::runtime_error("corrupt zstd frame");
            break;
          } // writer's incomplete tail: retry from this boundary
          auto length = ZSTD_getFrameContentSize(input.data() + used, frame);
          if (length == ZSTD_CONTENTSIZE_ERROR)
            throw std::runtime_error("invalid zstd frame");
          if (length == ZSTD_CONTENTSIZE_UNKNOWN)
            length = 4 * 1024 * 1024;
          if (length > 4 * 1024 * 1024)
            throw std::runtime_error("Harness frame exceeds budget");
          std::vector<char> decoded((size_t)length);
          auto output = ZSTD_decompress(decoded.data(), decoded.size(),
                                        input.data() + used, frame);
          if (ZSTD_isError(output) || records(decoded.data(), output) != output)
            throw std::runtime_error("invalid Harness frame");
          used += frame;
        }
      } else
        used = records(input.data(), input.size());
      if (used == 0 && input.size() == 4 * 1024 * 1024)
        throw std::runtime_error("Harness record exceeds budget");
      c.offset += used;
      c.written = info.ftLastWriteTime;
      // Never publish an intermediate historical state while catching up.
      c.fold.s.uncertain = c.offset < size;
    } catch (...) {
      c.failed = true;
      c.observedSize = size;
      c.written = info.ftLastWriteTime;
      c.fold.s.uncertain = true;
    }
    return true;
  }
  Snapshot Poll(bool online, int64_t processStart) {
    Discover();
    Snapshot out;
    out.online = online;
    size_t budget = 4 * 1024 * 1024;
    std::vector<fs::path> paths;
    for (auto &[p, c] : cursors)
      paths.push_back(p);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(8);
    size_t serviced = 0;
    for (;
         serviced < paths.size() && std::chrono::steady_clock::now() < deadline;
         ++serviced) {
      const auto &p = paths[(nextRead + serviced) % paths.size()];
      if (!Read(p, cursors.at(p), budget))
        cursors.erase(p);
    }
    if (!paths.empty())
      nextRead = (nextRead + serviced) % paths.size();
    for (auto it = cursors.begin(); it != cursors.end();) {
      auto &c = it->second;
      if (c.failed)
        out.error = L"会话格式不支持、损坏或单条记录超过 4 MiB";
      if (c.fold.header && !c.fold.hidden) {
        auto s = c.fold.s;
        if (!online || (s.running && s.startedAt < processStart)) {
          s.running = false;
          s.waiting = false;
          s.uncertain = true;
        }
        if (s.uncertain) {
          s.running = false;
          s.waiting = false;
          s.result.clear();
        }
        out.sessions.push_back(std::move(s));
      }
      ++it;
    }
    std::sort(out.sessions.begin(), out.sessions.end(),
              [](auto &a, auto &b) { return a.updated > b.updated; });
    if (out.sessions.size() > 40)
      out.sessions.resize(40);
    return out;
  }
};
int64_t HarnessStart() {
  int64_t start = 0;
  EnumWindows(
      [](HWND w, LPARAM param) -> BOOL {
        if (GetWindow(w, GW_OWNER))
          return TRUE;
        wchar_t title[256]{};
        GetWindowTextW(w, title, 256);
        if (!wcsstr(title, L"DeepSeek Harness") &&
            wcscmp(title, L"DSH 本地构建") != 0)
          return TRUE;
        DWORD id = 0;
        GetWindowThreadProcessId(w, &id);
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id);
        if (!p)
          return TRUE;
        wchar_t image[32768]{};
        DWORD count = 32768;
        FILETIME created{}, exit{}, kernel{}, user{};
        wchar_t development[32768]{};
        GetEnvironmentVariableW(L"ZCODEPET_HARNESS_EXECUTABLE", development,
                                32768);
        std::error_code sameFileError;
        if (QueryFullProcessImageNameW(p, 0, image, &count) &&
            (_wcsicmp(fs::path(image).filename().c_str(),
                      L"DeepSeek Harness.exe") == 0 ||
             (development[0] &&
              fs::equivalent(fs::path(image), fs::path(development),
                             sameFileError))) &&
            GetProcessTimes(p, &created, &exit, &kernel, &user)) {
          ULARGE_INTEGER t{};
          t.LowPart = created.dwLowDateTime;
          t.HighPart = created.dwHighDateTime;
          *reinterpret_cast<int64_t *>(param) =
              (int64_t)(t.QuadPart / 10000) - 11644473600000LL;
        }
        CloseHandle(p);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&start));
  return start;
}
} // namespace
Snapshot ObserveDeepSeek(const fs::path &root) {
  static thread_local Reader reader;
  if (reader.root != root / L"sessions") {
    reader = Reader{};
    reader.root = root / L"sessions";
  }
  const auto start = HarnessStart();
  return reader.Poll(start != 0, start);
}
bool FoldTests() {
  Fold f;
  f.Apply(json::parse(
      R"({"type":"session","version":4,"id":"same","createdAt":1,"cwd":"D:/x"})"));
  f.Apply(json::parse(R"({"type":"turn/start","time":10,"data":{"turn":1}})"));
  if (!f.s.running || f.s.workspace != L"deepseek:D:/x")
    return false;
  f.Apply(json::parse(
      R"({"type":"approval/asked","time":11,"data":{"id":"a","reason":"allow?"}})"));
  if (!f.s.waiting)
    return false;
  f.Apply(json::parse(
      R"({"type":"approval/decided","time":12,"data":{"id":"a"}})"));
  if (f.s.waiting)
    return false;
  f.Apply(json::parse(
      R"({"type":"turn/end","time":13,"data":{"turn":1,"reason":{"kind":"completed"}}})"));
  if (f.s.running || f.s.result != L"completed")
    return false;
  f.Apply(json::parse(
      R"({"type":"session/title","time":14,"data":{"title":"renamed"}})"));
  f.Apply(json::parse(R"({"type":"turn/start","time":15,"data":{"turn":2}})"));
  return f.s.title == L"renamed" && f.s.result.empty() && f.s.turnId == L"2";
}

namespace {
std::string Frame(const std::string &text) {
  std::string out = "\x28\xb5\x2f\xfd\xa0";
  auto word = [&](uint32_t value, int n) {
    for (int i = 0; i < n; ++i)
      out += char(value >> (i * 8));
  };
  word((uint32_t)text.size(), 4);
  word(((uint32_t)text.size() << 3) | 1, 3);
  out += text;
  return out;
}
} // namespace
bool DataSourceTests(bool benchmark) {
  if (!FoldTests())
    return false;
  wchar_t dir[MAX_PATH]{}, name[MAX_PATH]{};
  GetTempPathW(MAX_PATH, dir);
  if (!GetTempFileNameW(dir, L"dsp", 0, name))
    return false;
  fs::path path = std::wstring(name) + L".zstd";
  DeleteFileW(name);
  struct Cleanup {
    fs::path p;
    ~Cleanup() { DeleteFileW(p.c_str()); }
  } cleanup{path};
  const auto initial = Frame(
      "{\"type\":\"session\",\"version\":4,\"id\":\"fixture\",\"createdAt\":1}"
      "\n{\"type\":\"turn/start\",\"time\":10,\"data\":{\"turn\":1}}\n");
  const auto ended = Frame("{\"type\":\"turn/"
                           "end\",\"time\":20,\"data\":{\"turn\":1,\"reason\":{"
                           "\"kind\":\"completed\"}}}\n");
  auto write = [&](const std::string &data, bool append = false) {
    std::ofstream f(path, std::ios::binary |
                              (append ? std::ios::app : std::ios::trunc));
    f.write(data.data(), data.size());
    return f.good();
  };
  Reader r;
  Cursor c;
  size_t budget = 4 * 1024 * 1024;
  if (!write(initial) || !r.Read(path, c, budget) || c.failed ||
      !c.fold.s.running)
    return false;
  auto bytes = r.bytes, parses = r.parses;
  const auto benchStart = std::chrono::steady_clock::now();
  for (int i = 0; i < 1000; ++i)
    if (!r.Read(path, c, budget))
      return false;
  if (benchmark) {
    std::ofstream out("source-benchmark.txt");
    out << "unchanged_reads=1000\naverage_microseconds="
        << std::chrono::duration<double, std::micro>(
               std::chrono::steady_clock::now() - benchStart)
                   .count() /
               1000
        << "\nadditional_body_bytes=" << r.bytes - bytes
        << "\nadditional_json_records=" << r.parses - parses << "\n";
  }
  if (r.bytes != bytes || r.parses != parses)
    return false;
  if (!write(ended.substr(0, ended.size() - 2), true) ||
      !r.Read(path, c, budget) || !c.fold.s.uncertain)
    return false;
  if (!write(ended.substr(ended.size() - 2), true) ||
      !r.Read(path, c, budget) || c.fold.s.result != L"completed" ||
      c.fold.s.uncertain)
    return false;
  if (!write(initial) || !r.Read(path, c, budget) || !c.fold.s.running ||
      c.failed)
    return false;
  // An unsupported generation fails visibly rather than looking completed.
  if (!write(Frame("{\"type\":\"session\",\"version\":999,\"id\":\"x\"}\n")) ||
      !r.Read(path, c, budget) || !c.failed)
    return false;
  const std::string entropy(
      "\x28\xb5\x2f\xfd\x20\x6e\xbd\x02\x00\xb2\x45\x12\x17\x90\xa7\xe9\x56\xce"
      "\x47\xb9\xa1\x59\x0a\xca\x3e\xdd\xf5\xcb\xa3\x28\xd5\x67\xc6\x32\x9c\x0b"
      "\x64\xad\x72\x1f\xb7\xa9\x39\x5b\xa0\xdc\x58\x40\xdd\x6e\x9a\x2a\x40\x4a"
      "\x43\xc9\x2a\x77\x07\x07\x6b\x30\x09\xdb\x46\x41\x64\x13\x7e\x1f\xb4\x09"
      "\x2e\xf1\xb0\x9d\x08\xa4\x14\xaa\x6f\x2c\x8c\x7e\x04\x03\x00\x0a\x25\x5c"
      "\xbe\xaa\xa7\x57\x69\x02",
      96);
  if (!write(entropy) || !r.Read(path, c, budget) || c.failed ||
      c.fold.s.id != L"entropy" || !c.fold.s.running)
    return false;
  return true;
}
