#include <windows.h>
#include "observer.h"
#include "presentation.h"
#include "../third_party/json.hpp"
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <tlhelp32.h>
using json = nlohmann::json;
namespace fs = std::filesystem;
namespace {
thread_local ObserverTiming timing;
uint64_t Microseconds() {
  LARGE_INTEGER ticks, frequency;
  QueryPerformanceCounter(&ticks);
  QueryPerformanceFrequency(&frequency);
  return (uint64_t)(ticks.QuadPart * 1000000. / frequency.QuadPart);
}
std::wstring Wide(const std::string &s) {
  if (s.empty())
    return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring r(n, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), r.data(), n);
  return r;
}
std::string Utf8(const std::wstring &s) {
  int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0,
                              nullptr, nullptr);
  std::string r(n, 0);
  WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), r.data(), n, nullptr,
                      nullptr);
  return r;
}
struct Sqlite {
  HMODULE mod = LoadLibraryW(L"winsqlite3.dll");
  void *db = nullptr;
  int(__cdecl *open)(const char *, void **, int, const char *) = nullptr;
  int(__cdecl *prepare)(void *, const char *, int, void **,
                        const char **) = nullptr;
  int(__cdecl *step)(void *) = nullptr;
  int(__cdecl *bind)(void *, int, const char *, int,
                     void(__cdecl *)(void *)) = nullptr;
  const unsigned char *(__cdecl *column)(void *, int) = nullptr;
  int64_t(__cdecl *integer)(void *, int) = nullptr;
  int(__cdecl *finalize)(void *) = nullptr;
  int(__cdecl *close)(void *) = nullptr;
  Sqlite() {
    if (!mod)
      return;
#define GET(member, name)                                                      \
  member = reinterpret_cast<decltype(member)>(GetProcAddress(mod, name))
    GET(open, "sqlite3_open_v2");
    GET(prepare, "sqlite3_prepare_v2");
    GET(step, "sqlite3_step");
    GET(bind, "sqlite3_bind_text");
    GET(column, "sqlite3_column_text");
    GET(integer, "sqlite3_column_int64");
    GET(finalize, "sqlite3_finalize");
    GET(close, "sqlite3_close");
#undef GET
  }
  ~Sqlite() {
    if (db && close)
      close(db);
    if (mod)
      FreeLibrary(mod);
  }
  bool valid() {
    return open && prepare && step && column && integer && finalize && close;
  }
  std::wstring text(void *s, int i) {
    auto p = column(s, i);
    return p ? Wide(reinterpret_cast<const char *>(p)) : L"";
  }
};
std::wstring RecapFromDb(Sqlite &sql, const Session &session) {
  if (!sql.bind || session.turnId.empty() || session.result != L"completed")
    return {};
  const char *query =
      R"SQL(SELECT json_extract(p.data,'$.text') FROM part p WHERE p.message_id=(
 SELECT id FROM message WHERE session_id=?1 AND json_extract(data,'$.anchor.turnId')=?2
 AND json_extract(data,'$.role')='assistant' AND json_extract(data,'$.finish')='stop'
 AND json_extract(data,'$.semantics.kind')='assistant_response'
 AND json_extract(data,'$.semantics.uiVisibility')='visible'
 ORDER BY sequence DESC,time_created DESC LIMIT 1)
 AND p.session_id=?1 AND json_extract(p.data,'$.type')='text'
 AND coalesce(json_extract(p.data,'$.synthetic'),0)=0 AND coalesce(json_extract(p.data,'$.ignored'),0)=0
 ORDER BY p.sequence,p.time_created LIMIT 8)SQL";
  void *stmt = nullptr;
  if (sql.prepare(sql.db, query, -1, &stmt, nullptr) != 0)
    return {};
  const auto id = Utf8(session.id), turn = Utf8(session.turnId);
  if (sql.bind(stmt, 1, id.c_str(), -1, nullptr) != 0 ||
      sql.bind(stmt, 2, turn.c_str(), -1, nullptr) != 0) {
    sql.finalize(stmt);
    return {};
  }
  std::wstring text;
  int rc = 101;
  while ((rc = sql.step(stmt)) == 100) {
    text += sql.text(stmt, 0) + L" ";
    if (text.size() > 2000)
      break;
  }
  sql.finalize(stmt);
  if (rc != 100 && rc != 101)
    return {};
  return CompactPreview(text);
}
const char *QuestionQuery() {
  return
      R"SQL(SELECT p.session_id,json_extract(m.data,'$.anchor.turnId'),
 json_extract(p.data,'$.callID'),json_extract(p.data,'$.state.status'),
 json_extract(p.data,'$.state.time.start'),json_extract(p.data,'$.state.time.end'),
 json_extract(p.data,'$.state.input.questions[0].question')
 FROM part p JOIN message m ON m.id=p.message_id AND m.session_id=p.session_id
 WHERE p.message_id IN (SELECT id FROM message WHERE session_id=?1
 AND json_extract(data,'$.anchor.turnId')=?2)
 AND p.session_id=?1 AND m.session_id=?1
 AND json_extract(m.data,'$.anchor.turnId')=?2
 AND json_extract(p.data,'$.type')='tool' AND json_extract(p.data,'$.tool')='AskUserQuestion'
 ORDER BY p.time_updated DESC LIMIT 128)SQL";
}
void ReadQuestionsFromDb(Sqlite &sql, Snapshot &snapshot) {
  if (!snapshot.online || !sql.bind)
    return;
  const char *query = QuestionQuery();
  for (auto &current : snapshot.sessions) {
    if (current.turnId.empty() || current.endedAt || !current.startedAt ||
        std::count_if(snapshot.sessions.begin(), snapshot.sessions.end(),
                      [&](const auto &s) { return s.id == current.id; }) != 1)
      continue;
    void *stmt = nullptr;
    if (sql.prepare(sql.db, query, -1, &stmt, nullptr) != 0)
      return;
    const auto sessionId = Utf8(current.id), turnId = Utf8(current.turnId);
    if (sql.bind(stmt, 1, sessionId.c_str(), -1, nullptr) != 0 ||
        sql.bind(stmt, 2, turnId.c_str(), -1, nullptr) != 0) {
      sql.finalize(stmt);
      continue;
    }
    while (sql.step(stmt) == 100) {
      auto id = sql.text(stmt, 0), turn = sql.text(stmt, 1),
           request = sql.text(stmt, 2), status = sql.text(stmt, 3);
      if (turn.empty() || request.empty())
        continue;
      for (auto &s : snapshot.sessions) {
        if (s.id != id || s.turnId != turn || s.endedAt || !s.startedAt)
          continue;
        if (std::count_if(snapshot.sessions.begin(), snapshot.sessions.end(),
                          [&](const auto &x) { return x.id == id; }) != 1)
          continue;
        if (status == L"running" && !sql.integer(stmt, 5)) {
          auto since = sql.integer(stmt, 4);
          if (since < s.startedAt || s.waiting)
            continue;
          s.waiting = true;
          s.interactionId = request;
          s.waitingSince = since;
          s.questionPreview = CompactPreview(sql.text(stmt, 6));
          s.uncertain = false;
        } else {
          s.resumedAt = std::max(s.resumedAt, sql.integer(stmt, 5));
          if (s.resumedAt > 0) {
            s.running = true;
            s.uncertain = false;
          }
        }
      }
    }
    sql.finalize(stmt);
  }
}
void ReadProjectMetadata(Sqlite &sql, Snapshot &snapshot) {
  for (auto &session : snapshot.sessions) {
    session.projectMetadataKnown = true;
    session.projectId.clear();
    session.projectDisplayName.clear();
    session.projectNameAmbiguous = false;
  }
  void *schema = nullptr;
  if (sql.prepare(sql.db,
                  "SELECT EXISTS(SELECT 1 FROM pragma_table_info('tasks') "
                  "WHERE name='project_id') "
                  "AND EXISTS(SELECT 1 FROM sqlite_master WHERE type='table' "
                  "AND name='projects')",
                  -1, &schema, nullptr) != 0)
    return;
  const int schemaResult = sql.step(schema);
  const bool legacy = schemaResult == 100 && sql.integer(schema, 0) == 0;
  sql.finalize(schema);
  if (legacy) {
    for (auto &session : snapshot.sessions)
      session.projectMetadataKnown = false;
    return;
  }
  if (schemaResult != 100)
    return;
  const char *query =
      R"SQL(SELECT t.workspace_key,t.task_id,coalesce(t.project_id,''),
 CASE WHEN p.deleted=0 THEN p.name ELSE '' END,
 (SELECT count(*) FROM projects q WHERE q.deleted=0 AND q.name=p.name COLLATE NOCASE)
 FROM tasks t LEFT JOIN projects p ON p.project_id=t.project_id
 WHERE t.deleted=0 AND t.archived=0 ORDER BY t.updated_at DESC LIMIT 40)SQL";
  void *stmt = nullptr;
  if (sql.prepare(sql.db, query, -1, &stmt, nullptr) != 0)
    return; // Metadata failures must not fall back to a guessed directory
            // label.
  struct Record {
    std::wstring workspace, id, project, name;
    bool ambiguous;
  };
  std::vector<Record> records;
  int rc;
  while ((rc = sql.step(stmt)) == 100)
    records.push_back({sql.text(stmt, 0), sql.text(stmt, 1), sql.text(stmt, 2),
                       sql.text(stmt, 3), sql.integer(stmt, 4) > 1});
  sql.finalize(stmt);
  if (rc != 101)
    return;
  for (auto &session : snapshot.sessions) {
    for (const auto &record : records)
      if (session.workspace == record.workspace && session.id == record.id) {
        session.projectId = record.project;
        session.projectDisplayName = record.name;
        session.projectNameAmbiguous = record.ambiguous;
        break;
      }
  }
}
void ReadRunningFallback(Sqlite &sql, Snapshot &snapshot,
                         int64_t processStart) {
  if (!snapshot.online || !processStart || !sql.bind)
    return;
  const char *query = R"SQL(SELECT m.id,json_extract(m.data,'$.anchor.turnId'),
 m.time_created,coalesce(json_extract(m.data,'$.finish'),''),
 coalesce(json_extract(m.data,'$.time.completed'),0),
 coalesce((SELECT max(p.time_updated) FROM part p
 WHERE p.message_id=m.id AND p.session_id=m.session_id
 AND json_extract(p.data,'$.state.status')='running'
 AND coalesce(json_extract(p.data,'$.state.time.end'),json_extract(p.data,'$.time.end'),0)=0),0)
 FROM message m WHERE m.session_id=?1 AND json_extract(m.data,'$.role')='assistant'
 AND NOT EXISTS(SELECT 1 FROM message u WHERE u.session_id=m.session_id
 AND json_extract(u.data,'$.role')='user' AND u.sequence>m.sequence)
 ORDER BY m.sequence DESC,m.time_created DESC LIMIT 1)SQL";
  for (auto &session : snapshot.sessions) {
    if (session.running || session.waiting ||
        std::count_if(
            snapshot.sessions.begin(), snapshot.sessions.end(),
            [&](const auto &other) { return other.id == session.id; }) != 1)
      continue;
    void *stmt = nullptr;
    if (sql.prepare(sql.db, query, -1, &stmt, nullptr) != 0)
      return; // Optional/schema-incompatible database never overrides log
              // evidence.
    const auto id = Utf8(session.id);
    if (sql.bind(stmt, 1, id.c_str(), -1, nullptr) == 0 &&
        sql.step(stmt) == 100) {
      const auto turn = sql.text(stmt, 1), finish = sql.text(stmt, 3);
      const auto created = sql.integer(stmt, 2),
                 completed = sql.integer(stmt, 4),
                 partUpdated = sql.integer(stmt, 5);
      const auto evidence = std::max(created, partUpdated);
      const bool fresh =
          evidence >= processStart &&
          evidence > std::max(session.startedAt, session.endedAt);
      const bool closedTurn =
          session.endedAt && !turn.empty() && turn == session.turnId;
      if (fresh && !closedTurn && !completed &&
          (finish.empty() ||
           (finish == L"tool-calls" && partUpdated >= processStart))) {
        session.running = true;
        session.uncertain = true;
        session.turnId = turn;
        session.startedAt = created >= processStart ? created : partUpdated;
        session.endedAt = 0;
      }
    }
    sql.finalize(stmt);
  }
}
void ReadCurrentQuestions(const fs::path &root, Snapshot &snapshot,
                          int64_t processStart) {
  Sqlite sql;
  if (!sql.valid())
    return;
  auto path = Utf8((root / L"cli/db/db.sqlite").wstring());
  if (sql.open(path.c_str(), &sql.db, 1, nullptr) != 0)
    return;
  auto before = Microseconds();
  ReadRunningFallback(sql, snapshot, processStart);
  timing.fallbackUs = Microseconds() - before;
  before = Microseconds();
  ReadQuestionsFromDb(sql, snapshot);
  timing.questionsUs = Microseconds() - before;
}
int64_t ProcessStart() {
  std::set<DWORD> windowProcesses;
  EnumWindows(
      [](HWND window, LPARAM data) -> BOOL {
        wchar_t title[16]{};
        GetWindowTextW(window, title, 16);
        if (wcscmp(title, L"ZCode") == 0) {
          DWORD pid = 0;
          GetWindowThreadProcessId(window, &pid);
          if (pid)
            reinterpret_cast<std::set<DWORD> *>(data)->insert(pid);
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&windowProcesses));
  int64_t visibleStart = 0;
  for (const auto pid : windowProcesses) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
      continue;
    wchar_t image[32768]{};
    DWORD length = 32768;
    FILETIME created{}, ended{}, kernel{}, user{};
    if (QueryFullProcessImageNameW(process, 0, image, &length) &&
        _wcsicmp(fs::path(image).filename().c_str(), L"ZCode.exe") == 0 &&
        GetProcessTimes(process, &created, &ended, &kernel, &user)) {
      ULARGE_INTEGER value{};
      value.LowPart = created.dwLowDateTime;
      value.HighPart = created.dwHighDateTime;
      visibleStart = std::max(
          visibleStart, (int64_t)(value.QuadPart / 10000 - 11644473600000ULL));
    }
    CloseHandle(process);
  }
  if (visibleStart)
    return visibleStart;
  // Keep background/startup detection when no regular ZCode window exists.

  HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (h == INVALID_HANDLE_VALUE)
    return 0;
  PROCESSENTRY32W p{sizeof(p)};
  std::vector<PROCESSENTRY32W> ps;
  std::set<DWORD> ids;
  if (Process32FirstW(h, &p))
    do {
      if (_wcsicmp(p.szExeFile, L"ZCode.exe") == 0) {
        ps.push_back(p);
        ids.insert(p.th32ProcessID);
      }
    } while (Process32NextW(h, &p));
  CloseHandle(h);
  int64_t newest = 0;
  for (auto &q : ps) {
    if (ids.count(q.th32ParentProcessID))
      continue;
    HANDLE ph =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, q.th32ProcessID);
    if (!ph)
      continue;
    FILETIME c, e, k, u;
    if (GetProcessTimes(ph, &c, &e, &k, &u)) {
      ULARGE_INTEGER v;
      v.LowPart = c.dwLowDateTime;
      v.HighPart = c.dwHighDateTime;
      newest =
          std::max(newest, (int64_t)(v.QuadPart / 10000 - 11644473600000ULL));
    }
    CloseHandle(ph);
  }
  return newest;
}
struct Activity {
  bool running = false;
  std::string turn, outcome;
  int64_t at = 0;
  int64_t startedAt = 0;
};
void Apply(std::map<std::wstring, Activity> &events, const json &j,
           int64_t at) {
  auto id = j.value("sessionId", std::string{}),
       kind = j.value("kind", std::string{});
  if (id.empty())
    return;
  auto turn = j.value("turnId", std::string{});
  auto &a = events[Wide(id)];
  if (at < a.at)
    return;
  if (kind == "turn-started")
    a = {true, turn, "", at, at};
  else if (kind == "turn-ended" && (!a.running || a.turn == turn)) {
    // 仅继承同一回合的起点；日志窗口缺少 start 时不能借用别的回合计时。
    auto began = a.turn == turn ? a.startedAt : 0;
    a = {false, turn, j.value("outcome", std::string{}), at, began};
  }
}
struct EventLogCursor {
  fs::path path;
  int64_t processStart = 0, seenSize = -1, cursor = 0;
  DWORD volume = 0, indexHigh = 0, indexLow = 0;
  FILETIME modified{};
  uint64_t bytesRead = 0;
  bool skipFragment = false;
  std::string tail;
  std::map<std::wstring, Activity> events;
};
const std::map<std::wstring, Activity> &
ReadEvents(const fs::path &path, int64_t start, EventLogCursor &cache) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    cache.events.clear();
    cache.seenSize = -1;
    return cache.events;
  }
  BY_HANDLE_FILE_INFORMATION info{};
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(h, &size) || !GetFileInformationByHandle(h, &info)) {
    CloseHandle(h);
    cache.events.clear();
    cache.seenSize = -1;
    return cache.events;
  }
  constexpr DWORD cap = 4 * 1024 * 1024;
  bool reset =
      cache.path != path || cache.processStart != start || cache.seenSize < 0 ||
      cache.volume != info.dwVolumeSerialNumber ||
      cache.indexHigh != info.nFileIndexHigh ||
      cache.indexLow != info.nFileIndexLow || size.QuadPart < cache.seenSize ||
      (size.QuadPart == cache.seenSize &&
       CompareFileTime(&cache.modified, &info.ftLastWriteTime) != 0);
  if (!reset && size.QuadPart == cache.seenSize) {
    CloseHandle(h);
    return cache.events;
  }
  if (!reset && !cache.tail.empty()) {
    LARGE_INTEGER point{};
    point.QuadPart = cache.cursor - (int64_t)cache.tail.size();
    std::string check(cache.tail.size(), 0);
    DWORD got = 0;
    SetFilePointerEx(h, point, nullptr, FILE_BEGIN);
    if (!ReadFile(h, check.data(), (DWORD)check.size(), &got, nullptr) ||
        got != check.size() || check != cache.tail)
      reset = true;
    cache.bytesRead += got;
  }
  if (size.QuadPart - cache.cursor > cap)
    reset = true;
  if (reset) {
    cache.events.clear();
    cache.tail.clear();
    cache.cursor = std::max<int64_t>(0, size.QuadPart - cap);
    cache.skipFragment = cache.cursor > 0;
  }
  cache.path = path;
  cache.processStart = start;
  cache.volume = info.dwVolumeSerialNumber;
  cache.indexHigh = info.nFileIndexHigh;
  cache.indexLow = info.nFileIndexLow;
  cache.modified = info.ftLastWriteTime;
  LARGE_INTEGER pos{};
  pos.QuadPart = cache.cursor;
  SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
  const DWORD bytes =
      (DWORD)std::min<int64_t>(size.QuadPart - pos.QuadPart, cap);
  std::string s(bytes, 0);
  DWORD got = 0;
  if (!ReadFile(h, s.data(), bytes, &got, nullptr)) {
    CloseHandle(h);
    cache.seenSize = -1;
    cache.events.clear();
    return cache.events;
  }
  CloseHandle(h);
  s.resize(got);
  cache.bytesRead += got;
  cache.seenSize = pos.QuadPart + got;
  size_t off = 0;
  if (cache.skipFragment) {
    const auto newline = s.find('\n');
    if (newline == std::string::npos) {
      cache.cursor = cache.seenSize;
      cache.tail = s.substr(s.size() - std::min<size_t>(64, s.size()));
      return cache.events;
    }
    off = newline + 1;
    cache.skipFragment = false;
  }
  auto &out = cache.events;
  while (off < s.size()) {
    auto end = s.find('\n', off);
    if (end == std::string::npos)
      break;
    auto line = s.substr(off, end - off);
    off = end + 1;
    if (line.find("[cua-pip-session]") == std::string::npos)
      continue;
    auto jpos = line.find("{\"eventId\"");
    if (jpos == std::string::npos)
      continue;
    int y, mo, d, hh, mm, ss, ms;
    if (sscanf_s(line.c_str(), "[%d-%d-%d %d:%d:%d.%d]", &y, &mo, &d, &hh, &mm,
                 &ss, &ms) != 7)
      continue;
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = hh;
    tm.tm_min = mm;
    tm.tm_sec = ss;
    tm.tm_isdst = -1;
    int64_t at = (int64_t)std::mktime(&tm) * 1000 + ms;
    if (at < start)
      continue;
    auto j = json::parse(line.substr(jpos), nullptr, false);
    if (!j.is_object())
      continue;
    try {
      Apply(out, j, at);
    } catch (const json::exception &) {
    }
  }
  cache.cursor = pos.QuadPart + (int64_t)off;
  if (off)
    cache.tail =
        s.substr(off - std::min<size_t>(64, off), std::min<size_t>(64, off));
  return cache.events;
}
bool EventCursorTests() {
  wchar_t directory[MAX_PATH]{}, filename[MAX_PATH]{};
  if (!GetTempPathW(MAX_PATH, directory) ||
      !GetTempFileNameW(directory, L"zcp", 0, filename))
    return false;
  struct TempLog {
    fs::path path;
    ~TempLog() { DeleteFileW(path.c_str()); }
  } file{filename};
  auto write = [&](const std::string &text, bool append = false) {
    std::ofstream output(file.path,
                         std::ios::binary |
                             (append ? std::ios::app : std::ios::trunc));
    output.write(text.data(), text.size());
    return output.good();
  };
  auto line = [](const char *kind, const char *turn) {
    return std::string("[2026-10-29 12:00:00.000] [cua-pip-session] "
                       "{\"eventId\":\"fixture\",\"sessionId\":\"log-test\","
                       "\"kind\":\"") +
           kind + "\",\"turnId\":\"" + turn + "\",\"outcome\":\"completed\"}\n";
  };
  EventLogCursor cursor;
  if (!write(line("turn-started", "one")))
    return false;
  ReadEvents(file.path, 0, cursor);
  if (!cursor.events[L"log-test"].running)
    return false;
  const auto initialBytes = cursor.bytesRead;
  ReadEvents(file.path, 0, cursor);
  if (cursor.bytesRead != initialBytes)
    return false;
  auto ended = line("turn-ended", "one");
  ended.pop_back();
  if (!write(ended, true))
    return false;
  ReadEvents(file.path, 0, cursor);
  if (!cursor.events[L"log-test"].running)
    return false;
  if (!write("\n", true))
    return false;
  ReadEvents(file.path, 0, cursor);
  if (cursor.events[L"log-test"].running)
    return false;
  if (!write(line("turn-started", "two"), true))
    return false;
  ReadEvents(file.path, 0, cursor);
  if (cursor.events[L"log-test"].turn != "two")
    return false;
  if (!write(line("turn-started", "three")))
    return false;
  ReadEvents(file.path, 0, cursor);
  if (cursor.events[L"log-test"].turn != "three")
    return false;
  wchar_t replacement[MAX_PATH]{};
  if (!GetTempFileNameW(directory, L"zcp", 0, replacement))
    return false;
  TempLog newer{replacement};
  {
    std::ofstream output(newer.path, std::ios::binary);
    output << line("turn-started", "four");
  }
  if (!MoveFileExW(newer.path.c_str(), file.path.c_str(),
                   MOVEFILE_REPLACE_EXISTING))
    return false;
  ReadEvents(file.path, 0, cursor);
  if (cursor.events[L"log-test"].turn != "four")
    return false;
  if (!write(std::string(4 * 1024 * 1024 + 100, 'x') + "\n" +
             line("turn-started", "five")))
    return false;
  ReadEvents(file.path, 0, cursor);
  if (cursor.events[L"log-test"].turn != "five")
    return false;
  ReadEvents(file.path, INT64_MAX, cursor);
  return cursor.events.empty();
}
} // namespace
Snapshot ObserveZCode() {
  timing = {};
  const auto began = Microseconds();
  Snapshot out;
  int64_t start = ProcessStart();
  timing.processUs = Microseconds() - began;
  out.online = start != 0;
  wchar_t home[32768];
  if (!GetEnvironmentVariableW(L"USERPROFILE", home, 32768)) {
    out.error = L"无法定位用户目录";
    return out;
  }
  fs::path root = fs::path(home) / L".zcode";
  Sqlite sql;
  if (!sql.valid()) {
    out.error = L"Windows SQLite 不可用";
    return out;
  }
  auto path = Utf8((root / L"v2/tasks-index.sqlite").wstring());
  if (sql.open(path.c_str(), &sql.db, 1, nullptr) != 0) {
    out.error = L"无法只读打开 ZCode 会话索引";
    return out;
  }
  void *stmt = nullptr;
  const char *query =
      "SELECT "
      "task_id,workspace_key,title,workspace_path,COALESCE(cli_last_outcome,"
      "task_status,''),updated_at FROM tasks WHERE deleted=0 AND archived=0 "
      "ORDER BY updated_at DESC LIMIT 40";
  if (sql.prepare(sql.db, query, -1, &stmt, nullptr) != 0) {
    out.error = L"官方会话索引格式不兼容";
    return out;
  }
  int rc;
  while ((rc = sql.step(stmt)) == 100) {
    Session s;
    s.id = sql.text(stmt, 0);
    s.workspace = sql.text(stmt, 1);
    s.title = sql.text(stmt, 2);
    s.path = sql.text(stmt, 3);
    s.result = sql.text(stmt, 4);
    s.updated = sql.integer(stmt, 5);
    out.sessions.push_back(std::move(s));
  }
  sql.finalize(stmt);
  if (rc != 101) {
    out.sessions.clear();
    out.error = L"会话索引暂时忙碌，稍后重试";
    return out;
  }
  timing.indexUs = Microseconds() - began - timing.processUs;
  auto checkpoint = Microseconds();
  ReadProjectMetadata(sql, out);
  timing.metadataUs = Microseconds() - checkpoint;
  if (start) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf_s(name, L"%04d-%02d-%02d.log", t.wYear, t.wMonth, t.wDay);
    checkpoint = Microseconds();
    static thread_local EventLogCursor cursor;
    const auto &events = ReadEvents(root / L"v2/logs" / name, start, cursor);
    timing.logUs = Microseconds() - checkpoint;
    for (auto &s : out.sessions) {
      if (std::count_if(out.sessions.begin(), out.sessions.end(),
                        [&](const Session &x) { return x.id == s.id; }) != 1)
        continue;
      auto i = events.find(s.id);
      if (i == events.end())
        continue;
      auto &a = i->second;
      s.running = a.running;
      s.uncertain = false;
      s.turnId = Wide(a.turn);
      s.startedAt = a.startedAt;
      if (!a.running && !a.outcome.empty()) {
        s.result = Wide(a.outcome);
        s.endedAt = a.at;
      }
    }
  }
  if (start)
    ReadCurrentQuestions(root, out, start);
  std::stable_sort(out.sessions.begin(), out.sessions.end(),
                   [](auto &a, auto &b) {
                     return a.waiting != b.waiting ? a.waiting > b.waiting
                                                   : a.running > b.running;
                   });
  timing.totalUs = Microseconds() - began;
  return out;
}
ObserverTiming LastObserverTiming() { return timing; }
std::wstring ReadZCodeRecap(const Session &session) {
  wchar_t home[32768];
  if (!GetEnvironmentVariableW(L"USERPROFILE", home, 32768))
    return {};
  Sqlite sql;
  if (!sql.valid())
    return {};
  auto path = Utf8((fs::path(home) / L".zcode/cli/db/db.sqlite").wstring());
  if (sql.open(path.c_str(), &sql.db, 1, nullptr) != 0)
    return {};
  return RecapFromDb(sql, session);
}
bool ObserverTests() {
  if (!EventCursorTests())
    return false;
  Sqlite fixture;
  if (!fixture.valid() ||
      fixture.open(":memory:", &fixture.db, 6, nullptr) != 0)
    return false;
  auto exec = [&](const char *query) {
    void *q = nullptr;
    if (fixture.prepare(fixture.db, query, -1, &q, nullptr) != 0)
      return false;
    int rc = fixture.step(q);
    fixture.finalize(q);
    return rc == 101;
  };
  for (
      const char *q :
      {"CREATE TABLE message(id TEXT,session_id TEXT,time_created "
       "INTEGER,sequence INTEGER,data TEXT)",
       "CREATE TABLE part(message_id TEXT,session_id TEXT,time_created "
       "INTEGER,sequence INTEGER,data TEXT)",
       R"SQL(INSERT INTO message VALUES('m1','a',1,1,'{"role":"assistant","finish":"stop","anchor":{"turnId":"t1"},"semantics":{"kind":"assistant_response","uiVisibility":"visible"}}'))SQL",
       R"SQL(INSERT INTO message VALUES('m2','a',2,2,'{"role":"assistant","finish":"stop","anchor":{"turnId":"other"},"semantics":{"kind":"assistant_response","uiVisibility":"visible"}}'))SQL",
       R"SQL(INSERT INTO part VALUES('m1','a',1,1,'{"type":"reasoning","text":"private"}'))SQL",
       R"SQL(INSERT INTO part VALUES('m1','a',2,2,'{"type":"text","text":"**safe** recap"}'))SQL",
       R"SQL(INSERT INTO part VALUES('m1','a',3,3,'{"type":"text","text":"synthetic","synthetic":true}'))SQL",
       R"SQL(INSERT INTO part VALUES('m2','a',4,4,'{"type":"text","text":"wrong turn"}'))SQL",
       R"SQL(INSERT INTO part VALUES('m1','b',4,4,'{"type":"text","text":"wrong session"}'))SQL"})
    if (!exec(q))
      return false;
  Session sample;
  sample.id = L"a";
  sample.turnId = L"t1";
  sample.result = L"completed";
  if (RecapFromDb(fixture, sample) != L"safe recap")
    return false;
  sample.turnId = L"missing";
  if (!RecapFromDb(fixture, sample).empty())
    return false;

  if (!exec("ALTER TABLE part ADD COLUMN time_updated INTEGER DEFAULT 10"))
    return false;
  if (!exec(
          R"SQL(INSERT INTO part(message_id,session_id,time_created,sequence,data) VALUES('m1','a',200,5,'{"type":"tool","tool":"AskUserQuestion","callID":"q1","state":{"status":"running","time":{"start":200},"input":{"questions":[{"question":"Pick one"}]}}}'))SQL"))
    return false;
  Snapshot waiting;
  waiting.online = true;
  Session pending;
  pending.id = L"a";
  pending.workspace = L"w";
  pending.turnId = L"t1";
  pending.startedAt = 100;
  pending.running = true;
  waiting.sessions = {pending};
  ReadQuestionsFromDb(fixture, waiting);
  if (!waiting.sessions[0].waiting || waiting.sessions[0].waitingSince != 200 ||
      waiting.sessions[0].interactionId != L"q1")
    return false;
  waiting.sessions[0] = pending;
  waiting.sessions[0].turnId = L"other";
  ReadQuestionsFromDb(fixture, waiting);
  if (waiting.sessions[0].waiting)
    return false;
  if (!exec(
          R"SQL(UPDATE part SET data=json_set(data,'$.state.status','completed','$.state.time.end',400) WHERE json_extract(data,'$.tool')='AskUserQuestion')SQL"))
    return false;
  waiting.sessions[0] = pending;
  ReadQuestionsFromDb(fixture, waiting);
  if (waiting.sessions[0].waiting || waiting.sessions[0].resumedAt != 400)
    return false;
  if (!exec(
          R"SQL(INSERT INTO message VALUES('fallback','fallback',1000,1,'{"role":"assistant","anchor":{"turnId":"new"}}'))SQL"))
    return false;
  Snapshot fallback;
  fallback.online = true;
  Session row;
  row.id = L"fallback";
  fallback.sessions = {row};
  ReadRunningFallback(fixture, fallback, 900);
  if (!fallback.sessions[0].running || !fallback.sessions[0].uncertain)
    return false;
  fallback.sessions = {row};
  ReadRunningFallback(fixture, fallback, 1100);
  if (fallback.sessions[0].running)
    return false;
  if (!exec(
          R"SQL(UPDATE message SET data=json_set(data,'$.time.completed',1200) WHERE id='fallback')SQL"))
    return false;
  fallback.sessions = {row};
  ReadRunningFallback(fixture, fallback, 900);
  if (fallback.sessions[0].running)
    return false;
  if (!exec(
          R"SQL(UPDATE message SET data=json_remove(json_set(data,'$.finish','tool-calls'),'$.time.completed') WHERE id='fallback')SQL") ||
      !exec(
          R"SQL(INSERT INTO part(message_id,session_id,time_created,sequence,data,time_updated) VALUES('fallback','fallback',1000,1,'{"state":{"status":"running"}}',1100))SQL"))
    return false;
  fallback.sessions = {row};
  ReadRunningFallback(fixture, fallback, 900);
  if (!fallback.sessions[0].running)
    return false;
  if (!exec(
          R"SQL(UPDATE part SET data=json_set(data,'$.state.time.end',1200) WHERE message_id='fallback')SQL"))
    return false;
  fallback.sessions = {row};
  ReadRunningFallback(fixture, fallback, 900);
  if (fallback.sessions[0].running)
    return false;
  if (!exec(
          R"SQL(UPDATE part SET data=json_remove(data,'$.state.time.end') WHERE message_id='fallback')SQL"))
    return false;
  row.turnId = L"new";
  row.endedAt = 1050;
  fallback.sessions = {row};
  ReadRunningFallback(fixture, fallback, 900);
  if (fallback.sessions[0].running)
    return false; // An explicit log end wins over a lingering running part.
  row.turnId.clear();
  row.endedAt = 0;
  if (!exec(
          R"SQL(INSERT INTO message VALUES('newest','fallback',1200,2,'{"role":"assistant","finish":"stop"}'))SQL"))
    return false;
  fallback.sessions = {row};
  ReadRunningFallback(fixture, fallback, 900);
  if (fallback.sessions[0].running)
    return false; // Never inspect running parts belonging to older messages.
  ReadProjectMetadata(fixture, fallback);
  if (fallback.sessions[0].projectMetadataKnown)
    return false; // Confirmed old schema keeps the old directory-label
                  // behavior.
  if (!exec(
          "CREATE TABLE projects(project_id TEXT,name TEXT,deleted INTEGER)") ||
      !exec("CREATE TABLE tasks(workspace_key TEXT,task_id TEXT,project_id "
            "TEXT,deleted INTEGER,archived INTEGER,updated_at INTEGER)") ||
      !exec("INSERT INTO projects "
            "VALUES('p1','bilibili-report',0),('p2','other-report',0)") ||
      !exec(
          "INSERT INTO tasks VALUES('w','a','p1',0,0,2),('w','b','p2',0,0,1)"))
    return false;
  Snapshot projects;
  Session projectRow;
  projectRow.id = L"a";
  projectRow.workspace = L"w";
  projectRow.path = L"C:/bilibili-project";
  auto otherProject = projectRow;
  otherProject.id = L"b";
  projects.sessions = {projectRow, otherProject};
  ReadProjectMetadata(fixture, projects);
  if (!projects.sessions[0].projectMetadataKnown ||
      projects.sessions[0].projectDisplayName != L"bilibili-report" ||
      projects.sessions[1].projectDisplayName != L"other-report")
    return false;
  if (!exec("UPDATE projects SET name='renamed' WHERE project_id='p1'"))
    return false;
  ReadProjectMetadata(fixture, projects);
  if (projects.sessions[0].projectDisplayName != L"renamed")
    return false;
  if (!exec("UPDATE projects SET name='RENAMED' WHERE project_id='p2'"))
    return false;
  ReadProjectMetadata(fixture, projects);
  if (!projects.sessions[0].projectNameAmbiguous ||
      !projects.sessions[1].projectNameAmbiguous)
    return false;
  if (!exec("UPDATE projects SET deleted=1 WHERE project_id='p1'"))
    return false;
  ReadProjectMetadata(fixture, projects);
  if (!projects.sessions[0].projectDisplayName.empty())
    return false;
  // A large history in the SAME session must not be scanned for a new turn.
  if (!exec("CREATE UNIQUE INDEX message_id_fixture ON message(id)") ||
      !exec("CREATE INDEX message_session_sequence_idx ON "
            "message(session_id,sequence,time_created,id)") ||
      !exec("CREATE INDEX part_session_message_sequence_idx ON "
            "part(session_id,message_id,sequence)") ||
      !exec(
          R"SQL(INSERT INTO message VALUES('history','a',0,0,'{"role":"assistant","anchor":{"turnId":"old-history"}}'))SQL") ||
      !exec(
          R"SQL(WITH RECURSIVE numbers(n) AS (VALUES(1) UNION ALL SELECT n+1 FROM numbers WHERE n<20000)
 INSERT INTO part(message_id,session_id,time_created,sequence,data,time_updated)
 SELECT 'history','a',0,n,'{"type":"text","text":"fixture history"}',0 FROM numbers)SQL"))
    return false;
  void *boundedQuery = nullptr;
  if (fixture.prepare(fixture.db, QuestionQuery(), -1, &boundedQuery,
                      nullptr) != 0)
    return false;
  fixture.bind(boundedQuery, 1, "a", -1, nullptr);
  fixture.bind(boundedQuery, 2, "t1", -1, nullptr);
  int boundedResult;
  while ((boundedResult = fixture.step(boundedQuery)) == 100) {
  }
  auto statementStatus = reinterpret_cast<int(__cdecl *)(void *, int, int)>(
      GetProcAddress(fixture.mod, "sqlite3_stmt_status"));
  const int fullScan =
      statementStatus ? statementStatus(boundedQuery, 1, 0) : INT_MAX;
  fixture.finalize(boundedQuery);
  if (boundedResult != 101 || fullScan > 128)
    return false;
  std::map<std::wstring, Activity> e;
  Apply(e, {{"sessionId", "a"}, {"kind", "turn-started"}, {"turnId", "t1"}},
        100);
  Apply(e, {{"sessionId", "a"}, {"kind", "turn-ended"}, {"turnId", "old"}},
        101);
  if (!e[L"a"].running)
    return false;
  Apply(e,
        {{"sessionId", "a"},
         {"kind", "turn-ended"},
         {"turnId", "t1"},
         {"outcome", "completed"}},
        102);
  if (e[L"a"].running)
    return false;
  Apply(e, {{"sessionId", "a"}, {"kind", "turn-started"}, {"turnId", "stale"}},
        99);
  if (e[L"a"].at != 102 || e[L"a"].startedAt != 100)
    return false;
  Apply(e,
        {{"sessionId", "a"},
         {"kind", "turn-ended"},
         {"turnId", "unseen"},
         {"outcome", "completed"}},
        110);
  return e[L"a"].startedAt == 0;
}
