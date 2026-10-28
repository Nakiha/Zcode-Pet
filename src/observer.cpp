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
void ReadQuestionsFromDb(Sqlite &sql, Snapshot &snapshot) {
  if (!snapshot.online)
    return;
  const char *query =
      R"SQL(SELECT p.session_id,json_extract(m.data,'$.anchor.turnId'),
 json_extract(p.data,'$.callID'),json_extract(p.data,'$.state.status'),
 json_extract(p.data,'$.state.time.start'),json_extract(p.data,'$.state.time.end'),
 json_extract(p.data,'$.state.input.questions[0].question')
 FROM part p JOIN message m ON m.id=p.message_id AND m.session_id=p.session_id
 WHERE json_extract(p.data,'$.type')='tool' AND json_extract(p.data,'$.tool')='AskUserQuestion'
 ORDER BY p.time_updated DESC LIMIT 128)SQL";
  void *stmt = nullptr;
  if (sql.prepare(sql.db, query, -1, &stmt, nullptr) != 0)
    return;
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
      if (status == L"running") {
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
void ReadCurrentQuestions(const fs::path &root, Snapshot &snapshot) {
  Sqlite sql;
  if (!sql.valid())
    return;
  auto path = Utf8((root / L"cli/db/db.sqlite").wstring());
  if (sql.open(path.c_str(), &sql.db, 1, nullptr) != 0)
    return;
  ReadQuestionsFromDb(sql, snapshot);
}
int64_t ProcessStart() {
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
std::map<std::wstring, Activity> ReadEvents(const fs::path &path,
                                            int64_t start) {
  std::map<std::wstring, Activity> out;
  // Shared read is essential: the official app retains its log handle while
  // running.
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    return out;
  LARGE_INTEGER size;
  if (!GetFileSizeEx(h, &size)) {
    CloseHandle(h);
    return out;
  }
  constexpr DWORD cap = 4 * 1024 * 1024;
  DWORD bytes = (DWORD)std::min<int64_t>(size.QuadPart, cap);
  LARGE_INTEGER pos;
  pos.QuadPart = size.QuadPart - bytes;
  SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
  std::string s(bytes, 0);
  DWORD got = 0;
  ReadFile(h, s.data(), bytes, &got, nullptr);
  CloseHandle(h);
  s.resize(got);
  size_t off = pos.QuadPart ? s.find('\n') + 1 : 0;
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
  return out;
}
} // namespace
Snapshot Observe() {
  Snapshot out;
  int64_t start = ProcessStart();
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
  if (start) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf_s(name, L"%04d-%02d-%02d.log", t.wYear, t.wMonth, t.wDay);
    auto events = ReadEvents(root / L"v2/logs" / name, start);
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
    ReadCurrentQuestions(root, out);
  std::stable_sort(out.sessions.begin(), out.sessions.end(),
                   [](auto &a, auto &b) {
                     return a.waiting != b.waiting ? a.waiting > b.waiting
                                                   : a.running > b.running;
                   });
  return out;
}
std::wstring ReadFinalRecap(const Session &session) {
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
