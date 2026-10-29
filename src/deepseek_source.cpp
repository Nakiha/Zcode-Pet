#include "data_sources.h"
#include "harness_cache.h"
#include "../third_party/json.hpp"
#include "../third_party/zstd/zstd.h"
#include "../third_party/zstd/zstd_errors.h"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
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
  bool header = false, hidden = false, boundary = false, seeded = false;
  int64_t createdAt = 0, titleSeq = -1;
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
      s.title = L"新会话";
      s.updated = v.value("createdAt", int64_t{});
      createdAt = s.updated;
      seeded = v.value("isSeeded", false);
      hidden = v.value("origin", std::string{}) == "subagent";
      header = true;
      return;
    }
    if (!header)
      throw std::runtime_error("missing Harness header");
    const auto &d = v.at("data");
    auto at = v.value("time", int64_t{});
    s.updated = std::max(s.updated, at);
    if (type == "session/title") {
      s.title = Wide(d.at("title").get<std::string>());
      titleSeq = v.value("seq", int64_t(-1));
    }
    if (type == "turn/start") {
      boundary = true;
      s.turnId = std::to_wstring(d.at("turn").get<int64_t>());
      s.startedAt = at;
      s.endedAt = 0;
      s.running = true;
      s.result.clear();
      approvals.clear();
    }
    if (type == "turn/end" &&
        (s.turnId.empty() ||
         s.turnId == std::to_wstring(d.at("turn").get<int64_t>()))) {
      boundary = true;
      s.turnId = std::to_wstring(d.at("turn").get<int64_t>());
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
  bool failed = false, bufferedReady = false, unavailable=false;
  std::vector<char> pending;
  uint64_t target = 0;
  std::optional<Fold> published;
};
struct Reader {
  fs::path root;
  std::map<fs::path, Cursor> cursors;
  std::unique_ptr<HarnessCacheIndex> cache;
  std::map<fs::path, HarnessCacheFact> cacheFacts;
  uint64_t bytes = 0, parses = 0;
  size_t nextRead = 0;
  bool deferred = false;
  void Discover() {
    if (!cache)
      cache = std::make_unique<HarnessCacheIndex>(
          root.parent_path() / L"storages/session_projcache/sessions");
    cache->Poll();
    cacheFacts.clear();
    for (const auto &[file, fact] : cache->facts) {
      auto log = HarnessLogPath(root, fact);
      if (GetFileAttributesW(log.c_str()) == INVALID_FILE_ATTRIBUTES) {
        auto plain = log;
        plain.replace_extension();
        if (GetFileAttributesW(plain.c_str()) != INVALID_FILE_ATTRIBUTES)
          log = plain;
      }
      cacheFacts[log] = fact;
      cursors.try_emplace(log);
    }
    for (auto it = cursors.begin(); it != cursors.end();)
      if (!cacheFacts.contains(it->first))
        it = cursors.erase(it);
      else
        ++it;
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
        size < c.offset + c.pending.size() ||
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
    if (!budget && c.pending.empty())
      return true;
    const bool compressed = p.extension() == L".zstd";
    auto records = [&](Fold &fold, const char *data, size_t n) {
      size_t begin = 0;
      for (size_t i = 0; i < n; ++i)
        if (data[i] == '\n') {
          auto value = json::parse(
              data + begin, data + i,
              [](int, json::parse_event_t event, json &value) {
                if (event != json::parse_event_t::key)
                  return true;
                const auto key = value.get<std::string>();
                return key == "seq" || key == "isSeeded" || key == "type" ||
                       key == "data" || key == "version" || key == "id" ||
                       key == "createdAt" || key == "cwd" || key == "origin" ||
                       key == "time" || key == "turn" || key == "reason" ||
                       key == "kind" || key == "title";
              });
          fold.Apply(value);
          ++parses;
          begin = i + 1;
        }
      return begin;
    };
    auto decode = [&](Fold &fold, const char *data, size_t frame) {
      auto length = ZSTD_getFrameContentSize(data, frame);
      if (length == ZSTD_CONTENTSIZE_ERROR)
        throw std::runtime_error("invalid frame");
      if (length == ZSTD_CONTENTSIZE_UNKNOWN)
        length = 32 * 1024 * 1024;
      if (length > 32 * 1024 * 1024)
        throw std::runtime_error("oversize frame");
      std::vector<char> decoded((size_t)length);
      auto output =
          ZSTD_decompress(decoded.data(), decoded.size(), data, frame);
      if (ZSTD_isError(output) ||
          records(fold, decoded.data(), output) != output)
        throw std::runtime_error("invalid frame records");
    };
    auto readAt = [&](uint64_t at, size_t length) {
      std::vector<char> data(length);
      LARGE_INTEGER pos{};
      pos.QuadPart = at;
      DWORD got = 0;
      if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) ||
          !ReadFile(h, data.data(), (DWORD)length, &got, nullptr))
        throw std::runtime_error("read failed");
      data.resize(got);
      bytes += got;
      budget -= std::min(budget, (size_t)got);
      return data;
    };
    try {
      // Independent Zstd frames allow a recent-state bootstrap without
      // replaying old tool outputs. Accept only a validated contiguous frame
      // chain.
      if ((!c.published || c.offset + 2 * 1024 * 1024 < size) &&
          size > 2 * 1024 * 1024 && budget >= 2 * 1024 * 1024 + 64 * 1024 &&
          compressed) {
        auto head = readAt(0, 64 * 1024);
        auto first = ZSTD_findFrameCompressedSize(head.data(), head.size());
        if (!ZSTD_isError(first)) {
          Fold headerState;
          size_t headerUsed = 0;
          while (headerUsed < head.size()) {
            auto part = ZSTD_findFrameCompressedSize(head.data() + headerUsed,
                                                     head.size() - headerUsed);
            if (ZSTD_isError(part))
              break;
            decode(headerState, head.data() + headerUsed, part);
            headerUsed += part;
          }
          if (headerState.header) {
            headerState.s.running = false;
            headerState.s.turnId.clear();
            headerState.boundary = false;
            headerState.s.result.clear();
            headerState.s.endedAt = 0;
            headerState.s.startedAt = 0;
            headerState.s.waiting = false;
            headerState.approvals.clear();
            const auto start = size - 2 * 1024 * 1024;
            auto tail = readAt(start, 2 * 1024 * 1024);
            for (size_t i = 0; i + 4 < tail.size(); ++i) {
              if ((unsigned char)tail[i] != 0x28 ||
                  (unsigned char)tail[i + 1] != 0xb5 ||
                  (unsigned char)tail[i + 2] != 0x2f ||
                  (unsigned char)tail[i + 3] != 0xfd)
                continue;
              auto frame = ZSTD_findFrameCompressedSize(tail.data() + i,
                                                        tail.size() - i);
              if (ZSTD_isError(frame))
                continue;
              Fold recent = headerState;
              size_t consumed = i;
              try {
                while (consumed < tail.size()) {
                  frame = ZSTD_findFrameCompressedSize(tail.data() + consumed,
                                                       tail.size() - consumed);
                  if (ZSTD_isError(frame)) {
                    if (ZSTD_getErrorCode(frame) == ZSTD_error_srcSize_wrong)
                      break;
                    throw std::runtime_error("bad tail");
                  }
                  decode(recent, tail.data() + consumed, frame);
                  consumed += frame;
                }
                if (recent.boundary) {
                  c.fold = recent;
                  c.published = recent;
                  c.offset = start + consumed;
                  c.pending.assign(tail.begin() + consumed, tail.end());
                  c.target = size;
                  break;
                }
              } catch (
                  ...) { /* A magic-like byte sequence is not a valid chain. */
              }
              break; // One valid chain, not an unbounded search/decode loop.
            }
          }
        }
      }
      if ((!c.published || c.offset + 2 * 1024 * 1024 < size) &&
          size > 2 * 1024 * 1024 && budget >= 2 * 1024 * 1024 + 64 * 1024 &&
          !compressed) {
        auto head = readAt(0, 64 * 1024);
        auto newline = std::find(head.begin(), head.end(), '\n');
        if (newline != head.end()) {
          Fold recent;
          records(recent, head.data(), newline - head.begin() + 1);
          const auto start = size - 2 * 1024 * 1024;
          auto tail = readAt(start, 2 * 1024 * 1024);
          auto line = std::find(tail.begin(), tail.end(), '\n');
          if (line != tail.end()) {
            size_t skip = line - tail.begin() + 1,
                   used =
                       records(recent, tail.data() + skip, tail.size() - skip);
            if (recent.boundary) {
              c.fold = recent;
              c.published = recent;
              c.offset = start + skip + used;
              c.pending.assign(tail.begin() + skip + used, tail.end());
              c.target = size;
            }
          }
        }
      }
      if (!c.target)
        c.target = size;
      const uint64_t readOffset = c.offset + c.pending.size();
      if (readOffset < size && budget) {
        size_t window = std::max<size_t>(256 * 1024, c.pending.size());
        auto data = readAt(
            readOffset,
            (size_t)std::min<uint64_t>({size - readOffset, budget, window}));
        c.pending.insert(c.pending.end(), data.begin(), data.end());
      }
      size_t used = 0;
      bool incomplete = false;
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(8);
      if (compressed) {
        while (used < c.pending.size() &&
               std::chrono::steady_clock::now() < deadline) {
          auto frame = ZSTD_findFrameCompressedSize(c.pending.data() + used,
                                                    c.pending.size() - used);
          if (ZSTD_isError(frame)) {
            if (ZSTD_getErrorCode(frame) != ZSTD_error_srcSize_wrong)
              throw std::runtime_error("corrupt frame");
            incomplete = true;
            break;
          }
          decode(c.fold, c.pending.data() + used, frame);
          used += frame;
        }
      } else
        used = records(c.fold, c.pending.data(), c.pending.size());
      c.bufferedReady = !incomplete && used < c.pending.size();
      c.offset += used;
      c.pending.erase(c.pending.begin(), c.pending.begin() + used);
      if (c.pending.empty() && c.pending.capacity() > 256 * 1024)
        std::vector<char>().swap(c.pending);
      if (c.pending.size() > 4 * 1024 * 1024)
        throw std::runtime_error("oversize compressed record");
      c.written = info.ftLastWriteTime;
      if (c.offset >= c.target ||
          (incomplete && c.offset + c.pending.size() == size)) {
        c.fold.s.uncertain = false;
        c.published = c.fold;
        c.target = size;
      }
      c.fold.s.uncertain = !c.published && c.offset < size;
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
      if (!budget && cursors.at(p).pending.empty())
        break;
      if (!Read(p, cursors.at(p), budget)){
        cursors.at(p) = Cursor{};cursors.at(p).unavailable=true;
      }
    }
    deferred = serviced < paths.size();
    if (!paths.empty())
      nextRead = (nextRead + serviced) % paths.size();
    for (auto it = cursors.begin(); it != cursors.end();) {
      auto &c = it->second;
      if (c.failed)
        out.error = L"会话格式不支持、损坏或单条记录过大";
      if (c.fold.header && !c.fold.hidden) {
        auto s = c.published ? c.published->s : c.fold.s;
        const auto named = cacheFacts.find(it->first);
        if (named != cacheFacts.end())
          ApplyHarnessCacheTitle(named->second, s, c.fold.createdAt,
                                 c.fold.seeded, c.fold.titleSeq);
        s.catchingUp = c.offset < size_t(c.target);
        if (c.failed)
          s.uncertain = true;
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
      } else if (auto fact = cacheFacts.find(it->first);
                 fact != cacheFacts.end()) {
        Session s;
        s.source = SourceId::DeepSeek;
        s.id = fact->second.id;
        s.path = fact->second.cwd;
        s.workspace = L"deepseek:" + s.path;
        s.title =
            fact->second.title.empty() ? L"标题同步中" : fact->second.title;
        s.updated = fact->second.createdAt;
        s.uncertain = true;
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
  auto result = reader.Poll(start != 0, start);
  bool backlog = reader.deferred || (reader.cache && reader.cache->Busy());
  for (const auto &[path, c] : reader.cursors)
    if (!c.failed && !c.unavailable && ((!c.fold.header && c.target == 0) || c.bufferedReady ||
                      c.offset + c.pending.size() < c.target))
      backlog = true;
  SetHarnessBacklog(backlog);
  return result;
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
  if (!HarnessCacheTests() || !FoldTests())
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
      !r.Read(path, c, budget) || (!c.published || !c.published->s.running))
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

  // 64 MiB of old assistant payload must not be replayed to find a recent end.
  const auto header = Frame("{\"type\":\"session\",\"version\":4,\"id\":"
                            "\"large\",\"createdAt\":1}\n");
  const auto filler =
      Frame("{\"type\":\"assistant/message\",\"time\":5,\"data\":{\"text\":\"" +
            std::string(4096, 'x') + "\"}}\n");
  {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << header;
    for (int i = 0; i < 16384; ++i)
      f << filler;
    f << ended;
  }
  c = Cursor{};
  Reader large;
  budget = 4 * 1024 * 1024;
  if (!large.Read(path, c, budget) || c.failed || !c.published ||
      c.published->s.result != L"completed" || large.bytes > 3 * 1024 * 1024)
    return false;
  const auto readBefore = large.bytes;
  budget = 4 * 1024 * 1024;
  const auto next =
      Frame("{\"type\":\"turn/start\",\"time\":30,\"data\":{\"turn\":2}}\n");
  if (!write(next, true) || !large.Read(path, c, budget) ||
      !c.published->s.running || large.bytes - readBefore > 1024)
    return false;
  // Buffered work must progress even after another source exhausted the I/O
  // budget.
  const auto finish = Frame("{\"type\":\"turn/"
                            "end\",\"time\":40,\"data\":{\"turn\":2,\"reason\":"
                            "{\"kind\":\"completed\"}}}\n");
  if (!write(finish, true))
    return false;
  c.pending.assign(finish.begin(), finish.end());
  budget = 0;
  const auto cachedBytes = large.bytes;
  if (!large.Read(path, c, budget) || large.bytes != cachedBytes ||
      !c.published || c.published->s.result != L"completed" ||
      !c.pending.empty())
    return false;
  if (benchmark) {
    std::ofstream f("source-benchmark.txt", std::ios::app);
    f << "large_history_bytes=" << fs::file_size(path)
      << "\nbootstrap_bytes=" << readBefore
      << "\nappend_bytes=" << large.bytes - readBefore << "\n";
  }

  const auto third =
      Frame("{\"type\":\"turn/start\",\"time\":50,\"data\":{\"turn\":3}}\n");
  if (!write(third, true))
    return false;
  {
    std::ofstream f(path, std::ios::binary | std::ios::app);
    for (int i = 0; i < 2048; ++i)
      f << filler;
  }
  for (int round = 0; round < 4; ++round) {
    {
      std::ofstream f(path, std::ios::binary | std::ios::app);
      for (int i = 0; i < 16; ++i)
        f << filler;
    }
    budget = 4 * 1024 * 1024;
    if (!large.Read(path, c, budget) || c.failed || !c.published ||
        c.published->s.uncertain || !c.published->s.running)
      return false;
  }
  const auto finalEnd = Frame("{\"type\":\"turn/"
                              "end\",\"time\":60,\"data\":{\"turn\":3,"
                              "\"reason\":{\"kind\":\"completed\"}}}\n");
  if (!write(finalEnd, true))
    return false;
  budget = 4 * 1024 * 1024;
  if (!large.Read(path, c, budget) || c.failed || !c.published ||
      c.published->s.result != L"completed" || c.published->s.turnId != L"3")
    return false;
  return true;
}
