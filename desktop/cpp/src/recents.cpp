#include "recents.hpp"

#include "util.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <direct.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace dustx {
namespace {

std::mutex g_mu;

void mkdir_p(const std::string& dir) {
#ifdef _WIN32
  std::string cur;
  for (size_t i = 0; i < dir.size(); ++i) {
    cur.push_back(dir[i]);
    if (dir[i] == '\\' || dir[i] == '/' || i + 1 == dir.size()) {
      _mkdir(cur.c_str());
    }
  }
#else
  std::string cur;
  for (size_t i = 0; i < dir.size(); ++i) {
    cur.push_back(dir[i]);
    if (dir[i] == '/' || i + 1 == dir.size()) {
      mkdir(cur.c_str(), 0755);
    }
  }
#endif
}

std::string sep() {
#ifdef _WIN32
  return "\\";
#else
  return "/";
#endif
}

bool valid_kind(const std::string& kind) {
  return kind == "remote" || kind == "mesh" || kind == "cam";
}

std::string sanitize(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '-') out.push_back(static_cast<char>(c));
    else if (c >= 'A' && c <= 'Z') out.push_back(static_cast<char>(c - 'A' + 'a'));
  }
  return out;
}

std::string digits_only(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    if (c >= '0' && c <= '9') out.push_back(static_cast<char>(c));
  }
  return out;
}

bool json_has_key(const std::string& json, const std::string& key) {
  return json.find("\"" + key + "\"") != std::string::npos;
}

bool json_get_bool(const std::string& json, const std::string& key, bool fallback) {
  const std::string pat = "\"" + key + "\"";
  size_t pos = json.find(pat);
  if (pos == std::string::npos) return fallback;
  size_t i = pos + pat.size();
  while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r' || json[i] == ':')) ++i;
  if (json.compare(i, 4, "true") == 0) return true;
  if (json.compare(i, 5, "false") == 0) return false;
  if (i < json.size() && json[i] == '1') return true;
  if (i < json.size() && json[i] == '0') return false;
  return fallback;
}

long long json_get_ll(const std::string& json, const std::string& key, long long fallback) {
  const std::string pat = "\"" + key + "\"";
  size_t pos = json.find(pat);
  if (pos == std::string::npos) return fallback;
  size_t i = pos + pat.size();
  while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r' || json[i] == ':')) ++i;
  if (i < json.size() && json[i] == '"') ++i;
  bool neg = false;
  if (i < json.size() && json[i] == '-') {
    neg = true;
    ++i;
  }
  if (i >= json.size() || json[i] < '0' || json[i] > '9') return fallback;
  long long v = 0;
  while (i < json.size() && json[i] >= '0' && json[i] <= '9') {
    v = v * 10 + (json[i++] - '0');
  }
  return neg ? -v : v;
}

struct Item {
  std::string kind;
  std::string id;
  std::string password;
  std::string pair_token;
  std::string name;
  std::string transport;
  int local_port = 0;
  bool incoming = false;
  bool want = false;
  long long last_at = 0;
  long long started_at = 0;
  long long ended_at = 0;
};

Item parse_item(const std::string& json) {
  Item it;
  it.kind = sanitize(json_get_string(json, "kind"));
  it.id = digits_only(json_get_string(json, "id"));
  it.password = json_get_string(json, "password");
  it.pair_token = json_get_string(json, "pair_token");
  it.name = json_get_string(json, "name");
  it.transport = sanitize(json_get_string(json, "transport"));
  it.local_port = json_get_int(json, "local_port", 0);
  it.incoming = json_get_bool(json, "incoming", false);
  it.want = json_get_bool(json, "want", false);
  it.last_at = json_get_ll(json, "last_at", 0);
  it.started_at = json_get_ll(json, "started_at", 0);
  it.ended_at = json_get_ll(json, "ended_at", 0);
  if (it.kind == "cam" && it.id.empty()) it.id = it.transport.empty() ? "wifi" : it.transport;
  return it;
}

void merge_item(Item* dst, const Item& src, const std::string& raw) {
  if (!src.kind.empty()) dst->kind = src.kind;
  if (!src.id.empty()) dst->id = src.id;
  if (json_has_key(raw, "password") && (!src.password.empty() || dst->password.empty())) {
    dst->password = src.password;
  }
  if (json_has_key(raw, "pair_token") && (!src.pair_token.empty() || dst->pair_token.empty())) {
    dst->pair_token = src.pair_token;
  }
  if (json_has_key(raw, "name")) dst->name = src.name;
  if (!src.transport.empty()) dst->transport = src.transport;
  if (src.local_port > 0 && src.local_port < 65536) dst->local_port = src.local_port;
  if (json_has_key(raw, "incoming")) dst->incoming = src.incoming;
  if (json_has_key(raw, "want")) dst->want = src.want;
  dst->last_at = src.last_at > 0 ? src.last_at : dst->last_at;
  if (json_has_key(raw, "started_at")) dst->started_at = src.started_at;
  if (json_has_key(raw, "ended_at")) dst->ended_at = src.ended_at;
}

std::string item_json(const Item& it) {
  std::ostringstream o;
  o << "{\"kind\":\"" << json_escape(it.kind) << "\""
    << ",\"id\":\"" << json_escape(it.id) << "\""
    << ",\"password\":\"" << json_escape(it.password) << "\""
    << ",\"pair_token\":\"" << json_escape(it.pair_token) << "\""
    << ",\"name\":\"" << json_escape(it.name) << "\""
    << ",\"transport\":\"" << json_escape(it.transport) << "\""
    << ",\"local_port\":" << it.local_port
    << ",\"incoming\":" << (it.incoming ? "true" : "false")
    << ",\"want\":" << (it.want ? "true" : "false")
    << ",\"last_at\":" << it.last_at
    << ",\"started_at\":" << it.started_at
    << ",\"ended_at\":" << it.ended_at
    << "}";
  return o.str();
}

std::string recents_dir() {
  return dustx_config_dir() + sep() + "connections";
}

std::string item_filename(const Item& it) {
  std::string key = it.kind == "cam" ? (it.transport.empty() ? it.id : it.transport) : it.id;
  key = sanitize(key);
  if (key.empty()) key = "unknown";
  return recents_dir() + sep() + sanitize(it.kind) + "-" + key + ".json";
}

std::vector<std::string> list_json_files(const std::string& dir) {
  std::vector<std::string> out;
#ifdef _WIN32
  WIN32_FIND_DATAW fd;
  const std::wstring pat = utf8_to_wide(dir + "\\*.json");
  HANDLE h = FindFirstFileW(pat.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    out.push_back(dir + "\\" + wide_to_utf8(fd.cFileName));
  } while (FindNextFileW(h, &fd));
  FindClose(h);
#else
  DIR* d = opendir(dir.c_str());
  if (!d) return out;
  while (dirent* ent = readdir(d)) {
    const std::string n = ent->d_name;
    if (n.size() > 5 && n.rfind(".json") == n.size() - 5) out.push_back(dir + "/" + n);
  }
  closedir(d);
#endif
  return out;
}

void write_item(const Item& it) {
  mkdir_p(recents_dir());
  std::ofstream out(item_filename(it), std::ios::binary);
  out << item_json(it) << "\n";
}

void forget_persisted_want() {
  static bool done = false;
  if (done) return;
  done = true;
  for (const auto& path : list_json_files(recents_dir())) {
    Item it = parse_item(read_file(path));
    if (!valid_kind(it.kind) || !it.want) continue;
    it.want = false;
    write_item(it);
  }
}

void share_peer_password(const Item& src) {
  if ((src.kind != "remote" && src.kind != "mesh") || src.id.empty() || src.password.empty()) return;
  Item sib;
  sib.kind = src.kind == "remote" ? "mesh" : "remote";
  sib.id = src.id;
  const std::string raw = read_file(item_filename(sib));
  if (!raw.empty()) {
    sib = parse_item(raw);
    if (sib.kind.empty()) sib.kind = src.kind == "remote" ? "mesh" : "remote";
    if (sib.password == src.password && sib.pair_token == src.pair_token) return;
    sib.password = src.password;
    if (!src.pair_token.empty()) sib.pair_token = src.pair_token;
    write_item(sib);
    return;
  }
  sib.password = src.password;
  sib.pair_token = src.pair_token;
  sib.name = src.name;
  sib.last_at = src.last_at;
  write_item(sib);
}

void unify_peer_passwords() {
  std::map<std::string, Item> best;
  const auto paths = list_json_files(recents_dir());
  for (const auto& path : paths) {
    Item it = parse_item(read_file(path));
    if ((it.kind != "remote" && it.kind != "mesh") || it.id.empty() || it.password.empty()) continue;
    auto& cur = best[it.id];
    if (cur.password.empty() || it.last_at >= cur.last_at) cur = it;
  }
  for (const auto& path : paths) {
    Item it = parse_item(read_file(path));
    if ((it.kind != "remote" && it.kind != "mesh") || it.id.empty()) continue;
    auto found = best.find(it.id);
    if (found == best.end() || it.password == found->second.password) continue;
    it.password = found->second.password;
    write_item(it);
  }
}

void prune_kind(const std::string& kind) {
  struct Row {
    long long last_at = 0;
    std::string path;
  };
  std::vector<Row> rows;
  for (const auto& path : list_json_files(recents_dir())) {
    const std::string raw = read_file(path);
    Item it = parse_item(raw);
    if (it.kind != kind) continue;
    rows.push_back({it.last_at, path});
  }
  if (rows.size() <= 20) return;
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.last_at > b.last_at; });
  for (size_t i = 20; i < rows.size(); ++i) {
#ifdef _WIN32
    _wremove(utf8_to_wide(rows[i].path).c_str());
#else
    ::remove(rows[i].path.c_str());
#endif
  }
}

}  // namespace

std::string dustx_config_dir() {
#ifdef _WIN32
  wchar_t appdata[MAX_PATH];
  if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) == 0) return ".";
  return wide_to_utf8(appdata) + "\\DustX";
#else
  const char* home = std::getenv("HOME");
  if (!home || !*home) return ".";
  return std::string(home) + "/Library/Application Support/DustX";
#endif
}

std::string load_connections_json() {
  std::lock_guard<std::mutex> lock(g_mu);
  forget_persisted_want();
  unify_peer_passwords();
  std::vector<Item> remote;
  std::vector<Item> mesh;
  std::vector<Item> cam;
  for (const auto& path : list_json_files(recents_dir())) {
    Item it = parse_item(read_file(path));
    if (!valid_kind(it.kind)) continue;
    if (it.kind == "remote") remote.push_back(it);
    else if (it.kind == "mesh") mesh.push_back(it);
    else cam.push_back(it);
  }
  auto newer = [](const Item& a, const Item& b) { return a.last_at > b.last_at; };
  std::sort(remote.begin(), remote.end(), newer);
  std::sort(mesh.begin(), mesh.end(), newer);
  std::sort(cam.begin(), cam.end(), newer);
  if (remote.size() > 20) remote.resize(20);
  if (mesh.size() > 20) mesh.resize(20);
  if (cam.size() > 20) cam.resize(20);
  std::ostringstream o;
  o << "{\"remote\":[";
  for (size_t i = 0; i < remote.size(); ++i) {
    if (i) o << ",";
    o << item_json(remote[i]);
  }
  o << "],\"mesh\":[";
  for (size_t i = 0; i < mesh.size(); ++i) {
    if (i) o << ",";
    o << item_json(mesh[i]);
  }
  o << "],\"cam\":[";
  for (size_t i = 0; i < cam.size(); ++i) {
    if (i) o << ",";
    o << item_json(cam[i]);
  }
  o << "]}";
  return o.str();
}

bool upsert_connection(const std::string& body, std::string* err) {
  Item incoming = parse_item(body);
  if (!valid_kind(incoming.kind)) {
    if (err) *err = "bad kind";
    return false;
  }
  if (incoming.kind != "cam" && incoming.id.empty()) {
    if (err) *err = "missing id";
    return false;
  }
  if (incoming.last_at <= 0) incoming.last_at = static_cast<long long>(std::time(nullptr));
  if (incoming.kind == "cam" && incoming.transport.empty()) {
    incoming.transport = incoming.id.empty() ? "wifi" : incoming.id;
    incoming.id = incoming.transport;
  }
  std::lock_guard<std::mutex> lock(g_mu);
  Item cur = incoming;
  const std::string raw = read_file(item_filename(incoming));
  if (!raw.empty()) {
    cur = parse_item(raw);
    if (cur.kind.empty()) cur.kind = incoming.kind;
    merge_item(&cur, incoming, body);
  }
  if (cur.last_at <= 0) cur.last_at = incoming.last_at;
  write_item(cur);
  share_peer_password(cur);
  prune_kind(cur.kind);
  return true;
}

bool remove_connection(const std::string& body, std::string* err) {
  Item it = parse_item(body);
  if (!valid_kind(it.kind)) {
    if (err) *err = "bad kind";
    return false;
  }
  if (it.kind == "cam" && it.id.empty()) it.id = it.transport.empty() ? "wifi" : it.transport;
  if (it.id.empty() && it.transport.empty()) {
    if (err) *err = "missing id";
    return false;
  }
  std::lock_guard<std::mutex> lock(g_mu);
#ifdef _WIN32
  _wremove(utf8_to_wide(item_filename(it)).c_str());
#else
  ::remove(item_filename(it).c_str());
#endif
  return true;
}

}  // namespace dustx
