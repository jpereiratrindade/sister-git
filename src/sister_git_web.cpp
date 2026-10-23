#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sqlite3.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct Result { int status; std::string out; };
struct Response {
  int status = 200;
  std::string type = "application/json; charset=utf-8";
  std::string body;
  std::vector<std::string> extra_headers = {};
};

struct SmtpConfig {
  std::string host = "smtp.gmail.com";
  int port = 587;
  std::string user;
  std::string pass;
  std::string from;
  std::string from_name = "SisTer Git Editorial";
  bool dev_mode = false;
  bool enabled() const { return !user.empty() && !pass.empty(); }
};

struct AuthUser {
  bool authenticated = false;
  std::string email;
  std::string name;
  std::string role = "user"; // "admin", "researcher", "user"
};

static std::string repo_root, web_root, auth_token;
static std::string bootstrap_admin_email;
static size_t max_output = 4 * 1024 * 1024;
static size_t max_upload = 64 * 1024 * 1024;
static SmtpConfig g_smtp;
static sqlite3 *g_db = nullptr;

static bool base64_decode(const std::string &in, std::string &out) {
  out.clear();
  std::vector<int> T(256, -1);
  const std::string b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  for (int i = 0; i < 64; i++) T[static_cast<unsigned char>(b64[i])] = i;

  if (in.empty() || in.size() % 4 != 0) return false;
  out.reserve((in.size() / 4) * 3);
  for (size_t i = 0; i < in.size(); i += 4) {
    bool last = i + 4 == in.size();
    unsigned char c0 = in[i], c1 = in[i + 1], c2 = in[i + 2], c3 = in[i + 3];
    if (T[c0] < 0 || T[c1] < 0) return false;
    if ((c2 == '=' || c3 == '=') && !last) return false;
    if (c2 == '=' && c3 != '=') return false;
    int v0 = T[c0], v1 = T[c1];
    out.push_back(static_cast<char>((v0 << 2) | (v1 >> 4)));
    if (c2 == '=') return (v1 & 0x0f) == 0;
    if (T[c2] < 0) return false;
    int v2 = T[c2];
    out.push_back(static_cast<char>(((v1 & 0x0f) << 4) | (v2 >> 2)));
    if (c3 == '=') return (v2 & 0x03) == 0;
    if (T[c3] < 0) return false;
    out.push_back(static_cast<char>(((v2 & 0x03) << 6) | T[c3]));
  }
  return true;
}

static std::string mime_type_from_filename(const std::string &filename) {
  size_t dot = filename.rfind('.');
  if (dot == std::string::npos) return "application/octet-stream";
  std::string ext = filename.substr(dot);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
  if (ext == ".docx") return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
  if (ext == ".odt") return "application/vnd.oasis.opendocument.text";
  if (ext == ".pdf") return "application/pdf";
  if (ext == ".tex") return "text/x-tex; charset=utf-8";
  if (ext == ".md" || ext == ".markdown") return "text/markdown; charset=utf-8";
  if (ext == ".txt") return "text/plain; charset=utf-8";
  if (ext == ".csv") return "text/csv; charset=utf-8";
  if (ext == ".json") return "application/json; charset=utf-8";
  if (ext == ".png") return "image/png";
  if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
  if (ext == ".svg") return "image/svg+xml";
  return "application/octet-stream";
}

static std::string json(const std::string &s) {
  std::string out = "\"";
  const char *hex = "0123456789abcdef";
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break; case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break; case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break; case '\r': out += "\\r"; break; case '\t': out += "\\t"; break;
      default: if (c < 0x20) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; } else out += char(c);
    }
  }
  return out + "\"";
}

// Small, dependency-free JSON validator. Editorial manifests are stored inside Git
// and therefore cannot be trusted to be syntactically valid before embedding them
// in an API response.
class JsonValidator {
 public:
  explicit JsonValidator(const std::string &input) : input_(input) {}

  bool valid() {
    skip_ws();
    if (!value()) return false;
    skip_ws();
    return pos_ == input_.size();
  }

 private:
  const std::string &input_;
  size_t pos_ = 0;

  void skip_ws() {
    while (pos_ < input_.size() && (input_[pos_] == ' ' || input_[pos_] == '\n' ||
           input_[pos_] == '\r' || input_[pos_] == '\t')) ++pos_;
  }

  bool consume(char expected) {
    skip_ws();
    if (pos_ >= input_.size() || input_[pos_] != expected) return false;
    ++pos_;
    return true;
  }

  bool literal(const char *text) {
    size_t len = std::strlen(text);
    if (input_.compare(pos_, len, text) != 0) return false;
    pos_ += len;
    return true;
  }

  bool string() {
    if (pos_ >= input_.size() || input_[pos_] != '"') return false;
    ++pos_;
    while (pos_ < input_.size()) {
      unsigned char c = input_[pos_++];
      if (c == '"') return true;
      if (c < 0x20) return false;
      if (c != '\\') continue;
      if (pos_ >= input_.size()) return false;
      char escaped = input_[pos_++];
      if (escaped == '"' || escaped == '\\' || escaped == '/' || escaped == 'b' ||
          escaped == 'f' || escaped == 'n' || escaped == 'r' || escaped == 't') continue;
      if (escaped != 'u' || pos_ + 4 > input_.size()) return false;
      for (int i = 0; i < 4; ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(input_[pos_++]))) return false;
      }
    }
    return false;
  }

  bool number() {
    size_t start = pos_;
    if (pos_ < input_.size() && input_[pos_] == '-') ++pos_;
    if (pos_ >= input_.size()) return false;
    if (input_[pos_] == '0') {
      ++pos_;
    } else {
      if (!std::isdigit(static_cast<unsigned char>(input_[pos_]))) return false;
      while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
    }
    if (pos_ < input_.size() && input_[pos_] == '.') {
      ++pos_;
      if (pos_ >= input_.size() || !std::isdigit(static_cast<unsigned char>(input_[pos_]))) return false;
      while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
    }
    if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) ++pos_;
      if (pos_ >= input_.size() || !std::isdigit(static_cast<unsigned char>(input_[pos_]))) return false;
      while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
    }
    return pos_ > start;
  }

  bool array() {
    if (!consume('[')) return false;
    skip_ws();
    if (pos_ < input_.size() && input_[pos_] == ']') { ++pos_; return true; }
    while (value()) {
      skip_ws();
      if (pos_ < input_.size() && input_[pos_] == ']') { ++pos_; return true; }
      if (!consume(',')) return false;
    }
    return false;
  }

  bool object() {
    if (!consume('{')) return false;
    skip_ws();
    if (pos_ < input_.size() && input_[pos_] == '}') { ++pos_; return true; }
    while (true) {
      skip_ws();
      if (!string() || !consume(':') || !value()) return false;
      skip_ws();
      if (pos_ < input_.size() && input_[pos_] == '}') { ++pos_; return true; }
      if (!consume(',')) return false;
    }
  }

  bool value() {
    skip_ws();
    if (pos_ >= input_.size()) return false;
    if (input_[pos_] == '"') return string();
    if (input_[pos_] == '{') return object();
    if (input_[pos_] == '[') return array();
    if (input_[pos_] == 't') return literal("true");
    if (input_[pos_] == 'f') return literal("false");
    if (input_[pos_] == 'n') return literal("null");
    return number();
  }
};

static bool valid_json(const std::string &input) {
  return JsonValidator(input).valid();
}

static bool valid_json_object(const std::string &input) {
  size_t first = input.find_first_not_of(" \n\r\t");
  return first != std::string::npos && input[first] == '{' && valid_json(input);
}

static Result run(const std::vector<std::string> &args, size_t max_out = max_output) {
  int p[2]; if (pipe(p) != 0) return {127, "pipe failed"};
  pid_t pid = fork();
  if (pid == 0) {
    dup2(p[1], STDOUT_FILENO); dup2(p[1], STDERR_FILENO); close(p[0]); close(p[1]);
    std::vector<char*> av; for (const auto &s : args) av.push_back(const_cast<char*>(s.c_str())); av.push_back(nullptr);
    execvp(av[0], av.data()); _exit(127);
  }
  close(p[1]); std::string out; char buf[8192]; ssize_t n;
  while ((n = read(p[0], buf, sizeof(buf))) > 0) { if (out.size() + size_t(n) <= max_out) out.append(buf, size_t(n)); }
  close(p[0]); int st = 0; waitpid(pid, &st, 0);
  return {WIFEXITED(st) ? WEXITSTATUS(st) : 128, out};
}

static Result run_stdin(const std::vector<std::string> &args, const std::string &input, const std::map<std::string, std::string> &env = {}) {
  int in_p[2], out_p[2];
  if (pipe(in_p) != 0 || pipe(out_p) != 0) return {127, "pipe failed"};
  pid_t pid = fork();
  if (pid == 0) {
    dup2(in_p[0], STDIN_FILENO); close(in_p[0]); close(in_p[1]);
    dup2(out_p[1], STDOUT_FILENO); dup2(out_p[1], STDERR_FILENO); close(out_p[0]); close(out_p[1]);
    for (const auto &[k, v] : env) setenv(k.c_str(), v.c_str(), 1);
    std::vector<char*> av; for (const auto &s : args) av.push_back(const_cast<char*>(s.c_str())); av.push_back(nullptr);
    execvp(av[0], av.data()); _exit(127);
  }
  close(in_p[0]); close(out_p[1]);
  if (!input.empty()) {
    size_t written = 0;
    while (written < input.size()) {
      ssize_t w = write(in_p[1], input.data() + written, input.size() - written);
      if (w <= 0) break;
      written += w;
    }
  }
  close(in_p[1]);
  std::string out; char buf[8192]; ssize_t n;
  while ((n = read(out_p[0], buf, sizeof(buf))) > 0) {
    if (out.size() + size_t(n) <= max_output) out.append(buf, size_t(n));
  }
  close(out_p[0]);
  int st = 0; waitpid(pid, &st, 0);
  return {WIFEXITED(st) ? WEXITSTATUS(st) : 128, out};
}

static std::string git_dir(const std::string &name) { return repo_root + "/" + name + ".git"; }
static bool is_repo(const std::string &name) {
  if (name.empty() || name.find('/') != std::string::npos || name.find("..") != std::string::npos) return false;
  return fs::is_directory(git_dir(name)) && fs::exists(git_dir(name) + "/HEAD");
}

static std::string repo_visibility(const std::string &name) {
  auto r = run({"git", "--git-dir=" + git_dir(name), "config", "sister.visibility"});
  std::string v = r.status == 0 ? r.out : "public";
  while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
  return v == "private" ? "private" : "public";
}

static std::string repo_type(const std::string &name) {
  auto r = run({"git", "--git-dir=" + git_dir(name), "config", "sister.type"});
  std::string t = r.status == 0 ? r.out : "software";
  while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
  return t == "editorial" ? "editorial" : "software";
}

static std::string repo_owner(const std::string &name) {
  auto r = run({"git", "--git-dir=" + git_dir(name), "config", "sister.owner"});
  std::string o = r.status == 0 ? r.out : "";
  while (!o.empty() && (o.back() == '\n' || o.back() == '\r' || o.back() == ' ')) o.pop_back();
  return o;
}

static std::string project_member_role(const std::string &repo, const std::string &email) {
  if (!g_db || repo.empty() || email.empty()) return "";
  sqlite3_stmt *stmt = nullptr;
  std::string role;
  if (sqlite3_prepare_v2(g_db, "SELECT role FROM project_members WHERE repo = ? AND email = ?", -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, repo.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, email.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      const char *value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
      if (value) role = value;
    }
    sqlite3_finalize(stmt);
  }
  return role;
}

static bool is_repo_visible(const std::string &name, const AuthUser &user) {
  std::string vis = repo_visibility(name);
  if (vis != "private") return true;
  if (!user.authenticated) return false;
  if (user.role == "admin") return true;
  std::string owner = repo_owner(name);
  if (owner.empty()) return true;
  return owner == user.email || !project_member_role(name, user.email).empty();
}

static bool can_write_repo(const std::string &name, const AuthUser &user) {
  if (!user.authenticated) return false;
  if (user.role == "admin") return true;
  std::string owner = repo_owner(name);
  return (!owner.empty() && owner == user.email) || project_member_role(name, user.email) == "editor";
}

static bool valid_revision(const std::string &rev) {
  if (rev.empty() || rev.size() > 128) return false;
  if (rev.find("..") != std::string::npos || rev.find(' ') != std::string::npos) return false;
  for (char c : rev) {
    if (!(std::isalnum(c) || c == '.' || c == '_' || c == '-' || c == '/' || c == '~' || c == '^')) return false;
  }
  return true;
}

static bool ensure_editorial_push_guard(const std::string &name, std::string &error_message) {
  const std::string marker = "# sister-git-managed-editorial-guard-v1";
  const std::string hook = git_dir(name) + "/hooks/pre-receive";
  if (fs::exists(hook)) {
    std::ifstream existing(hook, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
    if (content.find(marker) == std::string::npos) {
      error_message = "Hook pre-receive existente não gerenciado em " + hook;
      return false;
    }
  }
  const std::string script =
      "#!/usr/bin/env bash\n" + marker + "\n"
      "set -euo pipefail\n"
      "while read -r old_sha new_sha ref_name; do\n"
      "  case \"$ref_name\" in\n"
      "    refs/heads/main|refs/tags/ed/*)\n"
      "      echo '[SISTER] Ref editorial protegida; use o fluxo web autenticado.' >&2\n"
      "      exit 1\n"
      "      ;;\n"
      "  esac\n"
      "done\n";
  const std::string temp = hook + ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out || !(out << script)) { error_message = "Falha ao gravar proteção editorial"; return false; }
  }
  if (chmod(temp.c_str(), 0755) != 0 || rename(temp.c_str(), hook.c_str()) != 0) {
    unlink(temp.c_str());
    error_message = "Falha ao ativar proteção editorial em " + hook;
    return false;
  }
  return true;
}

static bool valid_path(const std::string &p) {
  if (p.find("..") != std::string::npos || (!p.empty() && p[0] == '/')) return false;
  for (char c : p) if (c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return false;
  return true;
}

static std::vector<std::string> lines(const std::string &s) {
  std::vector<std::string> out; std::istringstream in(s); std::string l;
  while (std::getline(in, l)) {
    if (!l.empty() && l.back() == '\r') l.pop_back();
    if (!l.empty()) out.push_back(l);
  }
  return out;
}

static std::string url_decode(const std::string &s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size()) {
      int v = 0; std::istringstream hex(s.substr(i + 1, 2)); hex >> std::hex >> v;
      out += char(v); i += 2;
    } else if (s[i] == '+') out += ' ';
    else out += s[i];
  }
  return out;
}

static std::map<std::string, std::string> query(const std::string &qs) {
  std::map<std::string, std::string> out; std::istringstream in(qs); std::string p;
  while (std::getline(in, p, '&')) {
    size_t eq = p.find('=');
    if (eq != std::string::npos) out[url_decode(p.substr(0, eq))] = url_decode(p.substr(eq + 1));
  }
  return out;
}

static Response error(int status, const std::string &msg) {
  return {status, "application/json; charset=utf-8", "{\"error\":" + json(msg) + "}"};
}

static std::string shell_quote(const std::string &s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') out += "'\\''";
    else out += c;
  }
  out += "'";
  return out;
}

static std::string iso_now() {
  std::time_t now = std::time(nullptr);
  std::tm tm_buf{};
  localtime_r(&now, &tm_buf);
  char buf[64];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", &tm_buf);
  return std::string(buf);
}

static std::string json_extract(const std::string &body, const std::string &key) {
  std::string target = "\"" + key + "\":";
  size_t p = body.find(target);
  if (p == std::string::npos) return "";
  p += target.size();
  while (p < body.size() && (body[p] == ' ' || body[p] == '\t' || body[p] == '\r' || body[p] == '\n')) p++;
  if (p >= body.size()) return "";
  if (body[p] == '"') {
    p++;
    std::string val;
    bool esc = false;
    while (p < body.size()) {
      char c = body[p++];
      if (esc) {
        if (c == 'n') val += '\n';
        else if (c == 'r') val += '\r';
        else if (c == 't') val += '\t';
        else val += c;
        esc = false;
      } else if (c == '\\') {
        esc = true;
      } else if (c == '"') {
        break;
      } else {
        val += c;
      }
    }
    return val;
  }
  size_t end = body.find_first_of(",}\r\n", p);
  return body.substr(p, end == std::string::npos ? std::string::npos : end - p);
}

static bool json_replace_string(std::string &body, const std::string &key, const std::string &value) {
  std::string target = "\"" + key + "\":";
  size_t p = body.find(target);
  if (p == std::string::npos) return false;
  p += target.size();
  while (p < body.size() && std::isspace(static_cast<unsigned char>(body[p]))) ++p;
  if (p >= body.size() || body[p] != '"') return false;
  size_t end = p + 1;
  bool escaped = false;
  for (; end < body.size(); ++end) {
    if (!escaped && body[end] == '"') break;
    if (!escaped && body[end] == '\\') escaped = true; else escaped = false;
  }
  if (end >= body.size()) return false;
  body.replace(p, end - p + 1, json(value));
  return true;
}

static bool json_append_object(std::string &body, const std::string &array_key, const std::string &object_json) {
  std::string target = "\"" + array_key + "\":";
  size_t p = body.find(target);
  if (p != std::string::npos) {
    size_t open = body.find('[', p + target.size());
    if (open == std::string::npos) return false;
    size_t close = body.find(']', open + 1);
    if (close == std::string::npos) return false;
    size_t content = open + 1;
    while (content < close && std::isspace(static_cast<unsigned char>(body[content]))) ++content;
    body.insert(close, content == close ? "\n    " + object_json + "\n  " : ",\n    " + object_json + "\n  ");
    return valid_json_object(body);
  }
  size_t close = body.rfind('}');
  if (close == std::string::npos) return false;
  size_t prev = close;
  while (prev > 0 && std::isspace(static_cast<unsigned char>(body[prev - 1]))) --prev;
  bool empty = prev > 0 && body[prev - 1] == '{';
  body.insert(close, std::string(empty ? "\n" : ",\n") + "  \"" + array_key + "\": [\n    " + object_json + "\n  ]\n");
  return valid_json_object(body);
}

static bool json_add_raw_field(std::string &body, const std::string &key, const std::string &raw_value) {
  if (body.find("\"" + key + "\":") != std::string::npos) return true;
  size_t close = body.rfind('}');
  if (close == std::string::npos) return false;
  size_t prev = close;
  while (prev > 0 && std::isspace(static_cast<unsigned char>(body[prev - 1]))) --prev;
  bool empty = prev > 0 && body[prev - 1] == '{';
  body.insert(close, std::string(empty ? "\n" : ",\n") + "  \"" + key + "\": " + raw_value + "\n");
  return valid_json_object(body);
}

static bool migrate_manifest_v1(std::string &manifest, const std::string &repo, bool &changed, std::string &error_message) {
  changed = false;
  if (!valid_json_object(manifest)) { error_message = "Manifesto editorial contém JSON inválido"; return false; }
  std::string version = json_extract(manifest, "schema_version");
  while (!version.empty() && std::isspace(static_cast<unsigned char>(version.back()))) version.pop_back();
  if (!version.empty() && version != "1") { error_message = "Versão de manifesto não suportada: " + version; return false; }
  auto add = [&](const std::string &key, const std::string &value) {
    if (manifest.find("\"" + key + "\":") != std::string::npos) return true;
    changed = true;
    return json_add_raw_field(manifest, key, value);
  };
  if (!add("$schema", json("https://sister-git.labeco.org/schemas/editorial-v1.json")) ||
      !add("schema_version", "1") || !add("project_type", json("editorial")) ||
      !add("title", json(repo)) || !add("document_main", json("manuscrito.md")) ||
      !add("current_stage", json("draft")) || !add("reviews", "[]") ||
      !add("responses", "[]") || !add("decisions", "[]") ||
      !add("transitions", "[]") || !add("editions", "[]")) {
    error_message = "Falha ao normalizar manifesto editorial v1";
    return false;
  }
  return valid_json_object(manifest);
}

// ============================================================================
// AUTHENTICATION & SQLITE ENGINE
// ============================================================================

static void init_auth_db() {
  std::string db_path = repo_root + "/.sister-auth.db";
  int rc = sqlite3_open(db_path.c_str(), &g_db);
  if (rc != SQLITE_OK) {
    std::cerr << "[AUTH WARNING] Não foi possível abrir " << db_path << ": " << sqlite3_errmsg(g_db) << "\n";
    g_db = nullptr;
    return;
  }
  sqlite3_exec(g_db, "PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL; PRAGMA busy_timeout = 5000;", nullptr, nullptr, nullptr);
  const char *ddl = R"(
    CREATE TABLE IF NOT EXISTS users (
      id TEXT PRIMARY KEY,
      email TEXT UNIQUE,
      name TEXT,
      role TEXT,
      created_at TEXT,
      last_login_at TEXT
    );
    CREATE TABLE IF NOT EXISTS otps (
      email TEXT PRIMARY KEY,
      code TEXT,
      expires_at INTEGER,
      attempts INTEGER DEFAULT 0
    );
    CREATE TABLE IF NOT EXISTS sessions (
      token TEXT PRIMARY KEY,
      email TEXT,
      name TEXT,
      role TEXT,
      created_at INTEGER,
      expires_at INTEGER
    );
    CREATE TABLE IF NOT EXISTS allowed_users (
      email TEXT PRIMARY KEY,
      name TEXT,
      role TEXT DEFAULT 'researcher',
      created_by TEXT,
      created_at TEXT
    );
    CREATE TABLE IF NOT EXISTS project_members (
      repo TEXT NOT NULL,
      email TEXT NOT NULL,
      role TEXT NOT NULL CHECK(role IN ('collaborator','reviewer','editor')),
      assigned_by TEXT NOT NULL,
      assigned_at TEXT NOT NULL,
      PRIMARY KEY(repo, email)
    );
    CREATE TABLE IF NOT EXISTS editorial_profiles (
      email TEXT PRIMARY KEY,
      orcid TEXT,
      institution TEXT,
      bio TEXT,
      areas TEXT,
      updated_at TEXT
    );
  )";
  char *err = nullptr;
  sqlite3_exec(g_db, ddl, nullptr, nullptr, &err);
  if (err) {
    std::cerr << "[AUTH DDL ERROR] " << err << "\n";
    sqlite3_free(err);
  }
  sqlite3_exec(g_db, "DELETE FROM sessions WHERE expires_at <= unixepoch(); DELETE FROM otps WHERE expires_at <= unixepoch();", nullptr, nullptr, nullptr);
  sqlite3_exec(g_db, "INSERT OR IGNORE INTO allowed_users (email, name, role, created_by, created_at) SELECT email, name, role, 'migration', created_at FROM users;", nullptr, nullptr, nullptr);
}

static bool has_allowed_users() {
  if (!g_db) return false;
  sqlite3_stmt *stmt = nullptr;
  int count = 0;
  if (sqlite3_prepare_v2(g_db, "SELECT COUNT(*) FROM allowed_users", -1, &stmt, nullptr) == SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
  }
  return count > 0;
}

static bool is_user_allowed(const std::string &email, std::string &out_name, std::string &out_role) {
  if (!g_db) return false;
  if (!has_allowed_users()) return !bootstrap_admin_email.empty() && email == bootstrap_admin_email;
  sqlite3_stmt *stmt = nullptr;
  bool ok = false;
  if (sqlite3_prepare_v2(g_db, "SELECT name, role FROM allowed_users WHERE email = ?", -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, email.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      ok = true;
      const char *n = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
      const char *r = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
      if (n) out_name = n;
      if (r) out_role = r;
    }
    sqlite3_finalize(stmt);
  }
  return ok;
}

static std::string random_hex(size_t bytes = 16) {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::uniform_int_distribution<uint64_t> dis;
  std::ostringstream ss;
  while (ss.str().size() < bytes * 2) {
    uint64_t val = dis(gen);
    ss << std::hex << std::setfill('0') << std::setw(16) << val;
  }
  return ss.str().substr(0, bytes * 2);
}

static std::string random_digits(size_t count = 6) {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<int> dis(0, 9);
  std::string out;
  for (size_t i = 0; i < count; i++) out += std::to_string(dis(gen));
  return out;
}

static bool send_otp_email(const std::string &to, const std::string &code, std::string &err_out) {
  if (g_smtp.dev_mode) return true;
  if (!g_smtp.enabled()) {
    err_out = "Servidor SMTP não configurado (informe remetente e senha no CLI)";
    return false;
  }

  std::string temp_file = "/tmp/sister-mail-" + random_hex(6) + ".eml";
  {
    std::ofstream ofs(temp_file);
    if (!ofs) { err_out = "Erro ao criar arquivo temporário de e-mail"; return false; }
    ofs << "From: \"" << g_smtp.from_name << "\" <" << (g_smtp.from.empty() ? g_smtp.user : g_smtp.from) << ">\r\n"
        << "To: <" << to << ">\r\n"
        << "Subject: SisTer Git - Codigo de Acesso: " << code << "\r\n"
        << "Content-Type: text/plain; charset=utf-8\r\n\r\n"
        << "Ola!\r\n\r\n"
        << "Voce solicitou acesso autenticado ao SisTer Git Editorial (LABECO / Embrapa).\r\n"
        << "Seu codigo temporario de uso unico e:\r\n\r\n"
        << "    " << code << "\r\n\r\n"
        << "Este codigo e valido por 10 minutos para autorizar operacoes de escrita e acesso aos seus projetos privados.\r\n"
        << "Se voce nao solicitou este codigo, por favor desconsidere este e-mail.\r\n\r\n"
        << "--\r\n"
        << "SisTer Git Editorial · Laboratorio de Estudos em AgroEcologia e Recursos Naturais (LABECO)\r\n"
        << "Sempre pronto. Sempre incompleto!\r\n";
  }

  std::string clean_pass = g_smtp.pass;
  clean_pass.erase(std::remove_if(clean_pass.begin(), clean_pass.end(), [](unsigned char c) { return std::isspace(c); }), clean_pass.end());

  std::ostringstream cmd;
  cmd << "curl --silent --show-error --connect-timeout 15 --max-time 30"
      << " --url 'smtp://" << g_smtp.host << ":" << g_smtp.port << "'"
      << " --mail-from " << shell_quote(g_smtp.from.empty() ? g_smtp.user : g_smtp.from)
      << " --mail-rcpt " << shell_quote(to)
      << " --upload-file " << shell_quote(temp_file);

  if (g_smtp.port == 587 || g_smtp.port == 25) {
    cmd << " --ssl-reqd";
  } else if (g_smtp.port == 465) {
    cmd << " --ssl";
  }
  cmd << " --user " << shell_quote(g_smtp.user + ":" + clean_pass) << " 2>&1";

  FILE *p = popen(cmd.str().c_str(), "r");
  std::string curl_out;
  if (p) {
    char buf[512];
    while (fgets(buf, sizeof(buf), p)) curl_out += buf;
    int st = pclose(p);
    unlink(temp_file.c_str());
    if (st != 0) {
      err_out = "Falha no envio SMTP: " + curl_out;
      return false;
    }
  } else {
    unlink(temp_file.c_str());
    err_out = "Não foi possível disparar curl para SMTP";
    return false;
  }
  return true;
}

static std::string request_token(const std::string &headers) {
  std::string token;
  std::string exp = "Authorization: Bearer ";
  size_t pos = headers.find(exp);
  if (pos == std::string::npos) pos = headers.find("authorization: bearer ");
  if (pos != std::string::npos) {
    size_t start = pos + exp.size();
    size_t end = headers.find_first_of("\r\n", start);
    if (end == std::string::npos) end = headers.size();
    token = headers.substr(start, end - start);
    while (!token.empty() && token.back() == ' ') token.pop_back();
  }

  // Fallback 1: Cookie (ex.: sister_token=...)
  if (token.empty()) {
    size_t cpos = headers.find("Cookie: ");
    if (cpos == std::string::npos) cpos = headers.find("cookie: ");
    if (cpos != std::string::npos) {
      std::string cstr = headers.substr(cpos + 8);
      size_t cend = cstr.find_first_of("\r\n");
      if (cend != std::string::npos) cstr = cstr.substr(0, cend);
      size_t tpos = cstr.find("sister_token=");
      if (tpos != std::string::npos) {
        size_t vstart = tpos + 13;
        size_t vend = cstr.find(';', vstart);
        token = cstr.substr(vstart, (vend == std::string::npos ? cstr.size() : vend) - vstart);
        while (!token.empty() && token.back() == ' ') token.pop_back();
      }
    }
  }

  return token;
}

static AuthUser authenticate_request(const std::string &headers, const std::string & = "") {
  std::string token = request_token(headers);
  // Master Token
  if (!auth_token.empty() && !token.empty() && token == auth_token) {
    return {true, "admin@labeco.invalid", "Administrador LABECO", "admin"};
  }

  // Session token in SQLite
  if (!token.empty() && g_db) {
    sqlite3_stmt *stmt = nullptr;
    const char *q = "SELECT email, name, role FROM sessions WHERE token = ? AND expires_at > ?";
    if (sqlite3_prepare_v2(g_db, q, -1, &stmt, nullptr) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(stmt, 2, std::time(nullptr));
      if (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string em = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        std::string nm = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        std::string rl = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        sqlite3_finalize(stmt);
        return {true, em, nm, rl};
      }
      sqlite3_finalize(stmt);
    }
  }

  return {false, "", "", ""};
}

// ============================================================================
// GIT PLUMBING FOR BARE REPOSITORIES
// ============================================================================

struct FileUpdate { std::string filename, content; bool normalize_text = true; };

static Result git_commit_files(const std::string &name, const std::vector<FileUpdate> &files, const std::string &commit_msg, const std::string &author_name, const std::string &author_email, const std::string &expected_head = "") {
  if (files.empty()) return {400, "Nenhum arquivo informado para o commit"};
  std::string gd = git_dir(name);
  std::string idx = gd + "/index_ed_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(rand());
  std::map<std::string, std::string> env = {
    {"GIT_INDEX_FILE", idx},
    {"GIT_AUTHOR_NAME", author_name},
    {"GIT_AUTHOR_EMAIL", author_email},
    {"GIT_COMMITTER_NAME", author_name},
    {"GIT_COMMITTER_EMAIL", author_email}
  };

  auto head_chk = run({"git", "--git-dir=" + gd, "rev-parse", "--verify", "HEAD"});
  bool has_head = head_chk.status == 0;
  std::string parent_commit;
  if (has_head) {
    parent_commit = head_chk.out;
    while (!parent_commit.empty() && (parent_commit.back() == '\n' || parent_commit.back() == '\r' || parent_commit.back() == ' ')) parent_commit.pop_back();
    if (!expected_head.empty() && expected_head != parent_commit) {
      unlink(idx.c_str());
      return {409, "Conflito de concorrência: a versão informada não é mais a versão atual"};
    }
    auto rt = run_stdin({"git", "--git-dir=" + gd, "read-tree", "HEAD"}, "", env);
    if (rt.status != 0) { unlink(idx.c_str()); return {rt.status, "Falha ao ler árvore Git atual"}; }
  }

  for (const auto &file : files) {
    std::string clean_content = file.content;
    if (file.normalize_text && !clean_content.empty() && clean_content.back() != '\n') clean_content += '\n';
    auto ho = run_stdin({"git", "--git-dir=" + gd, "hash-object", "-w", "--stdin"}, clean_content);
    if (ho.status != 0) { unlink(idx.c_str()); return {ho.status, "Falha ao gravar blob Git"}; }
    std::string blob_sha = ho.out;
    while (!blob_sha.empty() && (blob_sha.back() == '\n' || blob_sha.back() == '\r' || blob_sha.back() == ' ')) blob_sha.pop_back();
    auto up = run_stdin({"git", "--git-dir=" + gd, "update-index", "--add", "--cacheinfo", "100644", blob_sha, file.filename}, "", env);
    if (up.status != 0) { unlink(idx.c_str()); return {up.status, "Falha ao atualizar índice: " + up.out}; }
  }

  auto wt = run_stdin({"git", "--git-dir=" + gd, "write-tree"}, "", env);
  if (wt.status != 0) { unlink(idx.c_str()); return {wt.status, "Falha ao gravar árvore Git"}; }
  std::string tree_sha = wt.out;
  while (!tree_sha.empty() && (tree_sha.back() == '\n' || tree_sha.back() == '\r' || tree_sha.back() == ' ')) tree_sha.pop_back();

  if (has_head) {
    auto head_tree = run({"git", "--git-dir=" + gd, "rev-parse", "HEAD^{tree}"});
    std::string cur_tree = head_tree.out;
    while (!cur_tree.empty() && (cur_tree.back() == '\n' || cur_tree.back() == '\r' || cur_tree.back() == ' ')) cur_tree.pop_back();
    if (cur_tree == tree_sha) {
      unlink(idx.c_str());
      return {0, parent_commit};
    }
  }

  std::vector<std::string> ct_args = {"git", "--git-dir=" + gd, "commit-tree", tree_sha};
  if (!parent_commit.empty()) {
    ct_args.push_back("-p");
    ct_args.push_back(parent_commit);
  }
  auto ct = run_stdin(ct_args, commit_msg + "\n", env);
  unlink(idx.c_str());
  if (ct.status != 0) return {ct.status, "Falha ao criar commit"};
  std::string new_commit = ct.out;
  while (!new_commit.empty() && (new_commit.back() == '\n' || new_commit.back() == '\r' || new_commit.back() == ' ')) new_commit.pop_back();

  std::vector<std::string> update_args = {"git", "--git-dir=" + gd, "update-ref", "refs/heads/main", new_commit};
  update_args.push_back(has_head ? parent_commit : std::string(40, '0'));
  auto ur = run(update_args);
  if (ur.status != 0) return {409, "Conflito de concorrência: o repositório mudou durante a gravação"};
  if (!has_head) {
    run({"git", "--git-dir=" + gd, "symbolic-ref", "HEAD", "refs/heads/main"});
  }

  return {0, new_commit};
}

static Result git_commit_file(const std::string &name, const std::string &filename, const std::string &content, const std::string &commit_msg, const std::string &author_name = "SisTer Editor", const std::string &author_email = "editorial@labeco.invalid", bool normalize_text = true, const std::string &expected_head = "") {
  return git_commit_files(name, {{filename, content, normalize_text}}, commit_msg, author_name, author_email, expected_head);
}

static Response create_repo(const std::string &name, const std::string &type, const std::string &visibility, const std::string &title, const std::string &description, const std::string &author, const AuthUser &user) {
  if (name.empty() || name.find('/') != std::string::npos || name.find("..") != std::string::npos) {
    return error(400, "Nome de repositório inválido");
  }
  if (is_repo(name)) return error(409, "Repositório já existente");

  std::string gd = git_dir(name);
  auto init = run({"git", "init", "--bare", "--initial-branch=main", gd});
  if (init.status != 0) return error(500, "Falha ao inicializar repositório: " + init.out);

  run({"git", "--git-dir=" + gd, "config", "sister.type", type == "editorial" ? "editorial" : "software"});
  run({"git", "--git-dir=" + gd, "config", "sister.visibility", visibility == "private" ? "private" : "public"});
  if (!user.email.empty()) {
    run({"git", "--git-dir=" + gd, "config", "sister.owner", user.email});
  }
  if (type == "editorial") {
    std::string guard_error;
    if (!ensure_editorial_push_guard(name, guard_error)) return error(500, guard_error);
  }

  std::string doc_title = title.empty() ? name : title;
  std::string auth_name = author.empty() ? (user.name.empty() ? "Pesquisador LABECO" : user.name) : author;
  std::string auth_email = user.email.empty() ? "editorial@labeco.invalid" : user.email;

  if (type == "editorial") {
    std::string doc_abstract = description.empty() ? "Inicie aqui o resumo do seu artigo ou relatório de pesquisa." : description;
    std::string init_doc = "# " + doc_title + "\n\n"
                           "**" + auth_name + "**  \n"
                           "*Laboratório de Estudos em AgroEcologia e Recursos Naturais (LABECO)*\n\n---\n\n"
                           "## Resumo\n\n" + doc_abstract + "\n\n---\n\n"
                           "## 1. Introdução\n\nDescreva a contextualização, o problema ecológico e as hipóteses investigadas.\n\n"
                           "## 2. Metodologia e Delineamento Amostral\n\nDetalhe os procedimentos experimentais, coleta de dados botânicos e análises estatísticas.\n\n"
                           "## 3. Resultados e Discussão\n\nApresente suas evidências empíricas com gráficos, tabelas e referências.\n\n"
                           "## 4. Considerações Finais\n\n*Sempre pronto. Sempre incompleto!*\n";

    std::string init_manifest = "{\n"
                                "  \"$schema\": \"https://sister-git.labeco.org/schemas/editorial-v1.json\",\n"
                                "  \"schema_version\": 1,\n"
                                "  \"project_type\": \"editorial\",\n"
                                "  \"title\": " + json(doc_title) + ",\n"
                                "  \"document_main\": \"manuscrito.md\",\n"
                                "  \"abstract\": " + json(description.empty() ? "Projeto documental sob governança editorial e proveniência Git." : description) + ",\n"
                                "  \"authors\": [{\"name\": " + json(auth_name) + ", \"affiliation\": \"LABECO\"}],\n"
                                "  \"current_stage\": \"draft\",\n"
                                "  \"reviews\": [],\n"
                                "  \"responses\": [],\n"
                                "  \"decisions\": [],\n"
                                "  \"transitions\": [],\n"
                                "  \"editions\": []\n"
                                "}\n";

    git_commit_file(name, "manuscrito.md", init_doc, "docs(editorial): initial manuscript draft", auth_name, auth_email);
    git_commit_file(name, ".sister-editorial.json", init_manifest, "feat(editorial): initialize editorial manifest", auth_name, auth_email);
  } else {
    std::string init_readme = "# " + name + "\n\nRepositório de software hospedado no SisTer Git.\n";
    git_commit_file(name, "README.md", init_readme, "feat: initial commit", auth_name, auth_email);
  }

  return {200, "application/json; charset=utf-8", "{\"ok\":true,\"status\":\"ok\",\"name\":" + json(name) + ",\"type\":" + json(type) + "}"};
}

static Response repos(const AuthUser &user) {
  std::string body = "{\"repositories\":["; bool first = true;
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(repo_root, ec)) {
    std::string file = entry.path().filename().string();
    if (file.size() < 5 || file.substr(file.size() - 4) != ".git") continue;
    std::string name = file.substr(0, file.size() - 4);
    if (!is_repo(name)) continue;
    if (!is_repo_visible(name, user)) continue;

    std::string vis = repo_visibility(name);
    std::string type = repo_type(name);
    std::string owner = repo_owner(name);
    auto head = run({"git", "--git-dir=" + git_dir(name), "symbolic-ref", "--short", "HEAD"});
    auto counts = run({"git", "--git-dir=" + git_dir(name), "for-each-ref", "--format=%(refname)", "refs/heads", "refs/tags"});
    int branches = 0, tags = 0, editorial_tags = 0;
    for (auto &x : lines(counts.out)) {
      if (x.rfind("refs/heads/", 0) == 0) branches++;
      else if (x.rfind("refs/tags/", 0) == 0) { tags++; if (x.rfind("refs/tags/ed/", 0) == 0) editorial_tags++; }
    }
    auto last = run({"git", "--git-dir=" + git_dir(name), "log", "-1", "--format=%H%x1f%h%x1f%s%x1f%an%x1f%aI"});
    auto f = lines(last.out);
    std::string log = last.status != 0 || f.empty() ? "" : f[0];
    uintmax_t bytes = 0;
    for (auto i = fs::recursive_directory_iterator(entry.path(), fs::directory_options::skip_permission_denied, ec); !ec && i != fs::recursive_directory_iterator(); i.increment(ec)) {
      if (i->is_regular_file(ec)) bytes += i->file_size(ec);
    }
    if (!first) body += ',';
    first = false;
    std::string editorial_title, editorial_stage, editorial_abstract;
    if (type == "editorial") {
      auto manifest = run({"git", "--git-dir=" + git_dir(name), "show", "HEAD:.sister-editorial.json"});
      if (manifest.status == 0 && valid_json_object(manifest.out)) {
        editorial_title = json_extract(manifest.out, "title");
        editorial_stage = json_extract(manifest.out, "current_stage");
        editorial_abstract = json_extract(manifest.out, "abstract");
      }
    }
    body += "{\"name\":" + json(name) + ",\"title\":" + json(editorial_title) + ",\"description\":" + json(editorial_abstract) + ",\"current_stage\":" + json(editorial_stage) + ",\"visibility\":" + json(vis) + ",\"owner\":" + json(owner) + ",\"type\":" + json(type) + ",\"default_branch\":" + json(head.status == 0 ? head.out.substr(0, head.out.find('\n')) : "") + ",\"branches\":" + std::to_string(branches) + ",\"tags\":" + std::to_string(tags) + ",\"editorial_tags\":" + std::to_string(editorial_tags) + ",\"size_bytes\":" + std::to_string(bytes) + ",\"integrity\":\"not_checked\"";
    if (log.empty()) body += ",\"empty\":true,\"last_commit\":null}";
    else {
      auto p = std::vector<std::string>{}; size_t b = 0, a;
      while ((a = log.find('\x1f', b)) != std::string::npos) { p.push_back(log.substr(b, a - b)); b = a + 1; }
      p.push_back(log.substr(b));
      body += ",\"empty\":false,\"last_commit\":{\"hash\":" + json(p[0]) + ",\"short_hash\":" + json(p[1]) + ",\"subject\":" + json(p[2]) + ",\"author\":" + json(p[3]) + ",\"date\":" + json(p[4]) + "}}";
    }
  }
  return {200, "application/json; charset=utf-8", body + "]}"};
}

static Response refs(const std::string &name) {
  auto r = run({"git", "--git-dir=" + git_dir(name), "for-each-ref", "--sort=-creatordate", "--format=%(refname)%09%(objectname)%09%(creatordate:iso-strict)", "refs/heads", "refs/tags"});
  if (r.status) return error(500, "Falha ao ler referências");
  std::string b = "{\"branches\":["; bool fb = true, ft = true; std::string tags;
  for (auto &x : lines(r.out)) {
    size_t a = x.find('\t'), c = x.find('\t', a + 1);
    if (a == std::string::npos || c == std::string::npos) continue;
    std::string ref = x.substr(0, a), item = "{\"name\":" + json(ref.substr(ref.find('/', 5) + 1)) + ",\"hash\":" + json(x.substr(a + 1, c - a - 1)) + ",\"date\":" + json(x.substr(c + 1)) + "}";
    if (ref.rfind("refs/heads/", 0) == 0) { if (!fb) b += ','; fb = false; b += item; }
    else { if (!ft) tags += ','; ft = false; tags += item; }
  }
  return {200, "application/json; charset=utf-8", b + "],\"tags\":[" + tags + "]}"};
}

static Response commits(const std::string &name, const std::map<std::string, std::string> &q) {
  std::string ref = q.count("ref") ? q.at("ref") : "HEAD";
  if (!valid_revision(ref)) return error(400, "Revisão inválida");
  auto r = run({"git", "--git-dir=" + git_dir(name), "log", "-50", "--format=%H%x1f%h%x1f%an%x1f%ae%x1f%aI%x1f%s", ref + "^{commit}"});
  if (r.status) return error(404, "Revisão inexistente ou repositório vazio");
  std::string b = "{\"commits\":["; bool first = true;
  for (auto &x : lines(r.out)) {
    std::vector<std::string> p; size_t s = 0, a;
    while ((a = x.find('\x1f', s)) != std::string::npos) { p.push_back(x.substr(s, a - s)); s = a + 1; }
    p.push_back(x.substr(s));
    if (p.size() != 6) continue;
    if (!first) b += ',';
    first = false;
    b += "{\"hash\":" + json(p[0]) + ",\"short_hash\":" + json(p[1]) + ",\"author\":" + json(p[2]) + ",\"email\":" + json(p[3]) + ",\"date\":" + json(p[4]) + ",\"subject\":" + json(p[5]) + "}";
  }
  return {200, "application/json; charset=utf-8", b + "]}"};
}

static Response tree(const std::string &name, const std::map<std::string, std::string> &q) {
  std::string ref = q.count("ref") ? q.at("ref") : "HEAD", path = q.count("path") ? q.at("path") : "";
  if (!valid_revision(ref) || !valid_path(path)) return error(400, "Revisão ou caminho inválido");
  std::string spec = ref + "^{tree}" + (path.empty() ? "" : ":" + path);
  auto r = run({"git", "--git-dir=" + git_dir(name), "ls-tree", "-z", spec});
  if (r.status) return error(404, "Árvore não encontrada");
  std::string b = "{\"path\":" + json(path) + ",\"entries\":["; bool first = true; size_t p = 0;
  while (p < r.out.size()) {
    size_t z = r.out.find('\0', p); if (z == std::string::npos) break;
    std::string x = r.out.substr(p, z - p);
    size_t sp = x.find(' '), sp2 = x.find(' ', sp + 1), tab = x.find('\t', sp2 + 1);
    if (tab != std::string::npos) {
      std::string entry_name = x.substr(tab + 1);
      std::string entry_path = path.empty() ? entry_name : path + "/" + entry_name;
      auto last = run({"git", "--git-dir=" + git_dir(name), "log", "-1", "--format=%h%x1f%s%x1f%aI", ref + "^{commit}", "--", entry_path});
      std::string short_hash, subject, date;
      if (last.status == 0) {
        while (!last.out.empty() && std::isspace(static_cast<unsigned char>(last.out.back()))) last.out.pop_back();
        size_t a = last.out.find('\x1f'), c = a == std::string::npos ? std::string::npos : last.out.find('\x1f', a + 1);
        if (a != std::string::npos && c != std::string::npos) {
          short_hash = last.out.substr(0, a);
          subject = last.out.substr(a + 1, c - a - 1);
          date = last.out.substr(c + 1);
        }
      }
      if (!first) b += ',';
      first = false;
      b += "{\"mode\":" + json(x.substr(0, sp)) + ",\"type\":" + json(x.substr(sp + 1, sp2 - sp - 1)) + ",\"hash\":" + json(x.substr(sp2 + 1, tab - sp2 - 1)) + ",\"name\":" + json(entry_name) + ",\"last_commit\":{\"short_hash\":" + json(short_hash) + ",\"subject\":" + json(subject) + ",\"date\":" + json(date) + "}}";
    }
    p = z + 1;
  }
  return {200, "application/json; charset=utf-8", b + "]}"};
}

static Response blob(const std::string &name, const std::map<std::string, std::string> &q) {
  std::string ref = q.count("ref") ? q.at("ref") : "HEAD", path = q.count("path") ? q.at("path") : "";
  if (!valid_revision(ref) || !valid_path(path) || path.empty()) return error(400, "Parâmetros inválidos");
  std::string spec = ref + ":" + path;
  auto r = run({"git", "--git-dir=" + git_dir(name), "show", spec});
  if (r.status) return error(404, "Arquivo não encontrado");
  bool bin = false; for (size_t i = 0; i < std::min(r.out.size(), size_t(8000)); i++) if (r.out[i] == '\0') { bin = true; break; }
  return {200, "application/json; charset=utf-8", "{\"path\":" + json(path) + ",\"binary\":" + (bin ? "true" : "false") + ",\"size\":" + std::to_string(r.out.size()) + ",\"content\":" + (bin ? "null" : json(r.out)) + "}"};
}

static Response compare(const std::string &name, const std::map<std::string, std::string> &q) {
  std::string base = q.count("base") ? q.at("base") : "", head = q.count("head") ? q.at("head") : "";
  if (!valid_revision(base) || !valid_revision(head)) return error(400, "Revisões inválidas");
  std::vector<std::string> args = {"git", "--git-dir=" + git_dir(name), "diff", "-u", base, head};
  if (q.count("path") && !q.at("path").empty() && valid_path(q.at("path"))) {
    args.push_back("--");
    args.push_back(q.at("path"));
  }
  auto r = run(args);
  if (r.status) return error(400, "Falha ao calcular diff");
  std::string b = "{\"base\":" + json(base) + ",\"head\":" + json(head) + ",\"files\":[";
  bool first_file = true;
  std::string cur_path;
  size_t cur_add = 0, cur_del = 0;
  std::vector<std::pair<std::string, std::string>> cur_lines;
  auto flush_file = [&]() {
    if (cur_path.empty()) return;
    if (!first_file) b += ',';
    first_file = false;
    b += "{\"path\":" + json(cur_path) + ",\"additions\":" + std::to_string(cur_add) + ",\"deletions\":" + std::to_string(cur_del) + ",\"lines\":[";
    for (size_t i = 0; i < cur_lines.size(); i++) {
      if (i > 0) b += ',';
      b += "{\"type\":" + json(cur_lines[i].first) + ",\"text\":" + json(cur_lines[i].second) + "}";
    }
    b += "]}";
    cur_lines.clear();
    cur_add = 0;
    cur_del = 0;
  };
  for (auto &l : lines(r.out)) {
    if (l.rfind("diff --git", 0) == 0) {
      flush_file();
      size_t b_pos = l.find(" b/");
      cur_path = b_pos != std::string::npos ? l.substr(b_pos + 3) : "desconhecido";
    } else if (l.rfind("@@", 0) == 0) cur_lines.push_back({"hunk", l});
    else if (!l.empty() && l[0] == '+') { cur_lines.push_back({"add", l.substr(1)}); cur_add++; }
    else if (!l.empty() && l[0] == '-') { cur_lines.push_back({"del", l.substr(1)}); cur_del++; }
    else if (!l.empty() && l[0] == ' ') cur_lines.push_back({"ctx", l.substr(1)});
  }
  flush_file();
  return {200, "application/json; charset=utf-8", b + "]}"};
}

static Response editorial(const std::string &name, const AuthUser &user) {
  std::string manifest_ref = "HEAD";
  if (!user.authenticated) {
    auto latest = run({"git", "--git-dir=" + git_dir(name), "for-each-ref", "--sort=-creatordate", "--count=1", "--format=%(refname:short)", "refs/tags/ed"});
    manifest_ref = latest.status == 0 ? latest.out : "";
    while (!manifest_ref.empty() && std::isspace(static_cast<unsigned char>(manifest_ref.back()))) manifest_ref.pop_back();
    if (manifest_ref.empty()) return error(404, "Nenhuma edição pública disponível");
  }
  auto r = run({"git", "--git-dir=" + git_dir(name), "show", manifest_ref + ":.sister-editorial.json"});
  bool has_manifest = r.status == 0;
  std::string manifest_raw = has_manifest ? r.out : "{}";
  auto tr = run({"git", "--git-dir=" + git_dir(name), "for-each-ref", "--sort=-creatordate", "--format=%(refname)%09%(objectname)%09%(creatordate:iso-strict)%09%(subject)", "refs/tags/ed"});
  std::string tags_json = "["; bool first_t = true;
  for (auto &x : lines(tr.out)) {
    std::vector<std::string> p; size_t s = 0, a;
    while ((a = x.find('\t', s)) != std::string::npos) { p.push_back(x.substr(s, a - s)); s = a + 1; }
    p.push_back(x.substr(s));
    if (p.size() < 3) continue;
    std::string tag_name = p[0].substr(p[0].find('/', 5) + 1);
    if (!first_t) tags_json += ',';
    first_t = false;
    tags_json += "{\"name\":" + json(tag_name) + ",\"hash\":" + json(p[1]) + ",\"date\":" + json(p[2]) + ",\"subject\":" + json(p.size() > 3 ? p[3] : "") + "}";
  }
  tags_json += "]";

  while (!manifest_raw.empty() && (manifest_raw.back() == '\n' || manifest_raw.back() == '\r' || manifest_raw.back() == ' ')) manifest_raw.pop_back();
  if (manifest_raw.empty()) manifest_raw = "{}";
  bool manifest_valid = valid_json_object(manifest_raw);

  std::string is_ed = (repo_type(name) == "editorial" || has_manifest) ? "true" : "false";
  std::string vis = repo_visibility(name);
  std::string owner = repo_owner(name);
  std::string project_role = user.role == "admin" ? "editor" : (owner == user.email ? "owner" : project_member_role(name, user.email));
  std::string manifest_json = manifest_valid ? manifest_raw : "{}";
  if (!user.authenticated && manifest_valid) {
    if (json_extract(manifest_raw, "current_stage") != "published") return error(404, "Edição pública inválida");
    manifest_json = "{\"title\":" + json(json_extract(manifest_raw, "title")) +
                    ",\"abstract\":" + json(json_extract(manifest_raw, "abstract")) +
                    ",\"document_main\":" + json(json_extract(manifest_raw, "document_main")) +
                    ",\"current_stage\":\"published\",\"published_ref\":" + json(manifest_ref) + "}";
  }
  std::string manifest_error = manifest_valid ? "null" : json("Manifesto .sister-editorial.json contém JSON inválido");
  auto head = run({"git", "--git-dir=" + git_dir(name), "rev-parse", manifest_ref});
  std::string head_sha = head.status == 0 ? head.out : "";
  while (!head_sha.empty() && std::isspace(static_cast<unsigned char>(head_sha.back()))) head_sha.pop_back();
  auto commit_date_result = run({"git", "--git-dir=" + git_dir(name), "show", "-s", "--format=%aI", manifest_ref});
  std::string commit_date = commit_date_result.status == 0 ? commit_date_result.out : "";
  while (!commit_date.empty() && std::isspace(static_cast<unsigned char>(commit_date.back()))) commit_date.pop_back();
  if (!user.authenticated) {
    return {200, "application/json; charset=utf-8", "{\"repository\":" + json(name) + ",\"published_commit\":" + json(head_sha) + ",\"commit_date\":" + json(commit_date) + ",\"is_editorial\":true,\"manifest\":" + manifest_json + ",\"tags\":" + tags_json + "}"};
  }
  bool migration_required = manifest_valid && json_extract(manifest_raw, "schema_version") != "1";
  return {200, "application/json; charset=utf-8", "{\"repository\":" + json(name) + ",\"head\":" + json(head_sha) + ",\"commit_date\":" + json(commit_date) + ",\"visibility\":" + json(vis) + ",\"owner\":" + json(owner) + ",\"project_role\":" + json(project_role) + ",\"is_editorial\":" + is_ed + ",\"has_manifest\":" + (has_manifest ? "true" : "false") + ",\"migration_required\":" + (migration_required ? "true" : "false") + ",\"manifest\":" + manifest_json + ",\"manifest_error\":" + manifest_error + ",\"tags\":" + tags_json + "}"};
}

static Response archive(const std::string &name, const std::map<std::string, std::string> &q) {
  std::string ref = q.count("ref") ? q.at("ref") : "HEAD";
  std::string fmt = q.count("format") ? q.at("format") : "zip";
  if (!valid_revision(ref)) ref = "HEAD";
  if (fmt != "zip" && fmt != "tar.gz") fmt = "zip";

  std::string clean_ref = ref;
  if (clean_ref == "HEAD") clean_ref = "master";
  std::replace(clean_ref.begin(), clean_ref.end(), '/', '-');

  std::string prefix = name + "-" + clean_ref + "/";
  auto r = run({"git", "--git-dir=" + git_dir(name), "archive", "--format=" + fmt, "--prefix=" + prefix, ref}, 128 * 1024 * 1024);
  if (r.status != 0) {
    return error(500, "Falha ao gerar arquivo para download: " + r.out);
  }

  std::string content_type = (fmt == "zip") ? "application/zip" : "application/gzip";
  std::string filename = name + "-" + clean_ref + (fmt == "zip" ? ".zip" : ".tar.gz");
  return {200, content_type, r.out, {"Content-Disposition: attachment; filename=\"" + filename + "\""}};
}

static Response raw(const std::string &name, const std::map<std::string, std::string> &q) {
  std::string ref = q.count("ref") ? q.at("ref") : "HEAD", path = q.count("path") ? q.at("path") : "";
  if (!valid_revision(ref) || !valid_path(path) || path.empty()) return error(400, "Parâmetros inválidos");
  std::string spec = ref + ":" + path;
  auto r = run({"git", "--git-dir=" + git_dir(name), "show", spec}, 64 * 1024 * 1024);
  if (r.status) return error(404, "Arquivo não encontrado");

  size_t slash = path.rfind('/');
  std::string filename = (slash == std::string::npos) ? path : path.substr(slash + 1);
  std::string mime = mime_type_from_filename(filename);
  return {200, mime, r.out, {"Content-Disposition: attachment; filename=\"" + filename + "\""}};
}

static Response static_file(std::string path) {
  if (path == "/") path = "/index.html";
  if (path.find("..") != std::string::npos) return error(400, "Caminho inválido");
  fs::path p = web_root + path;
  std::ifstream f(p, std::ios::binary);
  if (!f) return error(404, "Não encontrado");
  std::ostringstream s; s << f.rdbuf();
  std::string ext = p.extension().string(), type = "application/octet-stream";
  if (ext == ".html") type = "text/html; charset=utf-8";
  else if (ext == ".css") type = "text/css; charset=utf-8";
  else if (ext == ".js") type = "text/javascript; charset=utf-8";
  else if (ext == ".svg") type = "image/svg+xml";
  return {200, type, s.str()};
}

// ============================================================================
// ROUTING & REQUEST HANDLERS
// ============================================================================

static Response route(const std::string &target, const AuthUser &user) {
  size_t qm = target.find('?');
  std::string path = target.substr(0, qm), qs = qm == std::string::npos ? "" : target.substr(qm + 1);
  auto q = query(qs);

  if (path == "/api/v1/health") {
    return {200, "application/json; charset=utf-8", "{\"status\":\"ok\",\"cxx\":\"c++26\",\"smtp_enabled\":" + std::string(g_smtp.enabled() ? "true" : "false") + "}"};
  }

  if (path == "/api/v1/auth/status") {
    std::string usr = user.authenticated
      ? "{\"email\":" + json(user.email) + ",\"name\":" + json(user.name) + ",\"role\":" + json(user.role) + "}"
      : "null";
    return {200, "application/json; charset=utf-8", "{\"authenticated\":" + std::string(user.authenticated ? "true" : "false") + ",\"user\":" + usr + ",\"auth_required\":true,\"smtp_enabled\":" + std::string(g_smtp.enabled() ? "true" : "false") + "}"};
  }

  if (path == "/api/v1/admin/users") {
    if (!user.authenticated || user.role != "admin") {
      return error(403, "Acesso restrito ao administrador");
    }
    std::string list = "[";
    if (g_db) {
      sqlite3_stmt *stmt = nullptr;
      const char *sql = "SELECT email, name, role, created_at, created_by FROM allowed_users ORDER BY name ASC";
      if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        bool first = true;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
          if (!first) list += ",";
          first = false;
          std::string em = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
          std::string nm = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
          std::string rl = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
          std::string ca = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
          std::string cb = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
          list += "{\"email\":" + json(em) + ",\"name\":" + json(nm) + ",\"role\":" + json(rl) + ",\"created_at\":" + json(ca) + ",\"created_by\":" + json(cb) + "}";
        }
        sqlite3_finalize(stmt);
      }
    }
    list += "]";
    return {200, "application/json; charset=utf-8", "{\"ok\":true,\"users\":" + list + "}"};
  }

  if (path == "/api/v1/editorial/profile") {
    std::string target_email = user.email;
    if (q.count("email") && !q["email"].empty()) {
      target_email = q["email"];
      std::transform(target_email.begin(), target_email.end(), target_email.begin(), [](unsigned char c) { return std::tolower(c); });
    }
    std::string orcid = "", institution = "", bio = "", areas = "", updated_at = "";
    if (g_db && !target_email.empty()) {
      sqlite3_stmt *stmt = nullptr;
      const char *sql = "SELECT orcid, institution, bio, areas, updated_at FROM editorial_profiles WHERE email = ?";
      if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, target_email.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
          const char *o = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
          const char *i = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
          const char *b = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
          const char *a = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
          const char *u = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
          if (o) orcid = o;
          if (i) institution = i;
          if (b) bio = b;
          if (a) areas = a;
          if (u) updated_at = u;
        }
        sqlite3_finalize(stmt);
      }
    }
    return {200, "application/json; charset=utf-8", "{\"ok\":true,\"email\":" + json(target_email) + ",\"orcid\":" + json(orcid) + ",\"institution\":" + json(institution) + ",\"bio\":" + json(bio) + ",\"areas\":" + json(areas) + ",\"updated_at\":" + json(updated_at) + "}"};
  }

  if (path == "/api/v1/ha") {
    return {200, "application/json; charset=utf-8", "{\"nodes\":[{\"name\":\"LABECO-SRV\",\"role\":\"git-host\",\"status\":\"active\"},{\"name\":\"Fedora Silverblue\",\"role\":\"replica-node\",\"status\":\"unknown\"}],\"replication\":\"unverified\",\"last_sync\":null}"};
  }

  if (path == "/api/v1/repos") {
    return repos(user);
  }

  const std::string pre = "/api/v1/repos/";
  if (path.rfind(pre, 0) == 0) {
    std::string rest = path.substr(pre.size());
    size_t slash = rest.find('/');
    std::string name = url_decode(rest.substr(0, slash));
    if (!is_repo(name)) return error(404, "Repositório não encontrado");
    if (!is_repo_visible(name, user)) return error(404, "Repositório não encontrado");

    std::string action = slash == std::string::npos ? "" : rest.substr(slash + 1);

    bool anonymous_editorial = !user.authenticated && repo_type(name) == "editorial";
    if (anonymous_editorial && action != "editorial") {
      std::string ref = q.count("ref") ? q.at("ref") : "";
      bool published_ref = ref.rfind("ed/", 0) == 0 && valid_revision(ref);
      if (published_ref) {
        auto published_manifest = run({"git", "--git-dir=" + git_dir(name), "show", ref + ":.sister-editorial.json"});
        published_ref = published_manifest.status == 0 && valid_json_object(published_manifest.out) &&
                        json_extract(published_manifest.out, "current_stage") == "published";
      }
      if ((action != "raw" && action != "archive" && action != "tree" && action != "blob") || !published_ref) {
        return error(403, "Acesso público limitado a artefatos de edições publicadas ed/*");
      }
    }

    if (action == "refs") return refs(name);
    if (action == "commits") return commits(name, q);
    if (action == "tree") return tree(name, q);
    if (action == "blob") return blob(name, q);
    if (action == "compare") return compare(name, q);
    if (action == "editorial") return editorial(name, user);
    if (action == "archive") return archive(name, q);
    if (action == "raw") return raw(name, q);
    return error(404, "Rota não encontrada");
  }

  return static_file(path);
}

static Response route_post(const std::string &target, const std::string &headers, const std::string &body, const AuthUser &user) {
  // Public Auth Endpoints
  if (target == "/api/v1/auth/request-code") {
    std::string email = json_extract(body, "email");
    while (!email.empty() && (email.front() == ' ' || email.front() == '\t')) email.erase(email.begin());
    while (!email.empty() && (email.back() == ' ' || email.back() == '\t')) email.pop_back();
    std::transform(email.begin(), email.end(), email.begin(), [](unsigned char c) { return std::tolower(c); });

    if (email.empty() || email.find('@') == std::string::npos || email.find('.') == std::string::npos) {
      return error(400, "Endereço de e-mail institucional inválido");
    }

    std::string a_name, a_role;
    if (!is_user_allowed(email, a_name, a_role)) {
      return error(403, "E-mail institucional não cadastrado no SisTer Git. Solicite a liberação de acesso ao administrador do laboratório.");
    }

    std::string code = random_digits(6);
    int64_t expires = std::time(nullptr) + 600; // 10 min

    if (g_db) {
      sqlite3_stmt *stmt = nullptr;
      const char *sql = "INSERT INTO otps(email, code, expires_at, attempts) VALUES(?, ?, ?, 0) "
                        "ON CONFLICT(email) DO UPDATE SET code = excluded.code, expires_at = excluded.expires_at, attempts = 0";
      if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, email.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, code.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, expires);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
      }
    }

    std::string err_out;
    bool sent = send_otp_email(email, code, err_out);
    if (!sent && !g_smtp.dev_mode) {
      return error(500, err_out);
    }

    std::string dev_info = g_smtp.dev_mode ? ",\"dev_code\":" + json(code) : "";
    return {200, "application/json; charset=utf-8", "{\"ok\":true,\"status\":\"ok\",\"message\":\"Código de 6 dígitos enviado para seu e-mail\"" + dev_info + "}"};
  }

  if (target == "/api/v1/auth/verify-code") {
    std::string email = json_extract(body, "email");
    std::string code = json_extract(body, "code");
    std::transform(email.begin(), email.end(), email.begin(), [](unsigned char c) { return std::tolower(c); });
    while (!code.empty() && (code.front() == ' ' || code.front() == '\t')) code.erase(code.begin());
    while (!code.empty() && (code.back() == ' ' || code.back() == '\t')) code.pop_back();

    if (email.empty() || code.empty()) return error(400, "E-mail e código de verificação são obrigatórios");

    bool code_ok = false;
    if (g_smtp.dev_mode && code == "123456") code_ok = true;

    if (!code_ok && g_db) {
      sqlite3_stmt *stmt = nullptr;
      const char *q = "SELECT code, expires_at, attempts FROM otps WHERE email = ?";
      if (sqlite3_prepare_v2(g_db, q, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, email.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
          std::string real_code = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
          int64_t exp = sqlite3_column_int64(stmt, 1);
          int attempts = sqlite3_column_int(stmt, 2);
          if (attempts < 5 && exp >= std::time(nullptr) && real_code == code) {
            code_ok = true;
          }
        }
        sqlite3_finalize(stmt);
      }
    }

    if (!code_ok) {
      return error(401, "Código de verificação incorreto ou expirado. Tente solicitar novo código.");
    }

    // Success: Issue session token
    std::string token = random_hex(24);
    int64_t exp_session = std::time(nullptr) + (30 * 86400); // 30 days
    std::string name = email.substr(0, email.find('@'));
    std::replace(name.begin(), name.end(), '.', ' ');
    if (!name.empty()) name[0] = std::toupper(name[0]);
    std::string role = "researcher";

    if (g_db) {
      if (!has_allowed_users()) {
        if (bootstrap_admin_email.empty() || email != bootstrap_admin_email) {
          return error(403, "Administrador inicial não autorizado pela configuração do servidor");
        }
        sqlite3_stmt *astmt = nullptr;
        if (sqlite3_prepare_v2(g_db, "INSERT OR IGNORE INTO allowed_users(email, name, role, created_by, created_at) VALUES(?, ?, 'admin', 'system', ?)", -1, &astmt, nullptr) == SQLITE_OK) {
          sqlite3_bind_text(astmt, 1, email.c_str(), -1, SQLITE_TRANSIENT);
          sqlite3_bind_text(astmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
          sqlite3_bind_text(astmt, 3, iso_now().c_str(), -1, SQLITE_TRANSIENT);
          sqlite3_step(astmt);
          sqlite3_finalize(astmt);
        }
        role = "admin";
      } else {
        std::string a_name, a_role;
        if (is_user_allowed(email, a_name, a_role)) {
          if (!a_name.empty()) name = a_name;
          if (!a_role.empty()) role = a_role;
        }
      }

      // Upsert user
      sqlite3_stmt *ustmt = nullptr;
      const char *usql = "INSERT INTO users(id, email, name, role, created_at, last_login_at) VALUES(?, ?, ?, ?, ?, ?) "
                         "ON CONFLICT(email) DO UPDATE SET last_login_at = excluded.last_login_at, name = excluded.name, role = excluded.role";
      if (sqlite3_prepare_v2(g_db, usql, -1, &ustmt, nullptr) == SQLITE_OK) {
        std::string uid = random_hex(8);
        sqlite3_bind_text(ustmt, 1, uid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ustmt, 2, email.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ustmt, 3, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ustmt, 4, role.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ustmt, 5, iso_now().c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ustmt, 6, iso_now().c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(ustmt);
        sqlite3_finalize(ustmt);
      }

      // Insert session
      sqlite3_stmt *sstmt = nullptr;
      const char *ssql = "INSERT INTO sessions(token, email, name, role, created_at, expires_at) VALUES(?, ?, ?, ?, ?, ?)";
      if (sqlite3_prepare_v2(g_db, ssql, -1, &sstmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(sstmt, 1, token.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(sstmt, 2, email.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(sstmt, 3, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(sstmt, 4, role.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(sstmt, 5, std::time(nullptr));
        sqlite3_bind_int64(sstmt, 6, exp_session);
        sqlite3_step(sstmt);
        sqlite3_finalize(sstmt);
      }

      // Consume OTP
      sqlite3_exec(g_db, ("DELETE FROM otps WHERE email = " + shell_quote(email)).c_str(), nullptr, nullptr, nullptr);
    }

    return {200, "application/json; charset=utf-8", "{\"ok\":true,\"status\":\"ok\",\"token\":" + json(token) + ",\"user\":{\"email\":" + json(email) + ",\"name\":" + json(name) + ",\"role\":" + json(role) + "}}"};
  }

  if (target == "/api/v1/auth/logout") {
    std::string token = request_token(headers);
    if (!token.empty() && token != auth_token && g_db) {
      sqlite3_stmt *stmt = nullptr;
      if (sqlite3_prepare_v2(g_db, "DELETE FROM sessions WHERE token = ?", -1, &stmt, nullptr) != SQLITE_OK) return error(500, "Falha ao preparar revogação da sessão");
      sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_TRANSIENT);
      bool revoked = sqlite3_step(stmt) == SQLITE_DONE;
      sqlite3_finalize(stmt);
      if (!revoked) return error(500, "Falha ao revogar sessão");
    }
    return {200, "application/json; charset=utf-8", "{\"ok\":true,\"status\":\"ok\"}"};
  }

  // Admin Management Endpoints
  if (target == "/api/v1/admin/users") {
    if (!user.authenticated || user.role != "admin") {
      return error(403, "Acesso restrito ao administrador");
    }
    std::string email = json_extract(body, "email");
    std::string name = json_extract(body, "name");
    std::string role = json_extract(body, "role");
    if (role.empty()) role = "researcher";
    std::transform(email.begin(), email.end(), email.begin(), [](unsigned char c) { return std::tolower(c); });
    while (!email.empty() && (email.front() == ' ' || email.front() == '\t')) email.erase(email.begin());
    while (!email.empty() && (email.back() == ' ' || email.back() == '\t')) email.pop_back();

    if (email.empty() || email.find('@') == std::string::npos) return error(400, "E-mail inválido");
    if (name.empty()) return error(400, "Nome é obrigatório");

    if (g_db) {
      sqlite3_stmt *stmt = nullptr;
      const char *sql = "INSERT INTO allowed_users(email, name, role, created_by, created_at) VALUES(?, ?, ?, ?, ?) "
                        "ON CONFLICT(email) DO UPDATE SET name = excluded.name, role = excluded.role";
      if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, email.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, role.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, user.email.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, iso_now().c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
      }
    }
    return {200, "application/json; charset=utf-8", "{\"ok\":true,\"status\":\"ok\"}"};
  }

  if (target == "/api/v1/admin/users/delete") {
    if (!user.authenticated || user.role != "admin") {
      return error(403, "Acesso restrito ao administrador");
    }
    std::string email = json_extract(body, "email");
    if (email == user.email) return error(400, "Não é permitido remover o próprio usuário administrador");
    if (g_db) {
      sqlite3_stmt *stmt = nullptr;
      if (sqlite3_prepare_v2(g_db, "DELETE FROM allowed_users WHERE email = ?", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, email.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
      }
    }
    return {200, "application/json; charset=utf-8", "{\"ok\":true,\"status\":\"ok\"}"};
  }

  if (target == "/api/v1/editorial/profile") {
    if (!user.authenticated) return error(401, "Autenticação obrigatória para atualizar perfil editorial");
    std::string orcid = json_extract(body, "orcid");
    std::string institution = json_extract(body, "institution");
    std::string bio = json_extract(body, "bio");
    std::string areas = json_extract(body, "areas");

    if (g_db) {
      sqlite3_stmt *stmt = nullptr;
      const char *sql = "INSERT INTO editorial_profiles(email, orcid, institution, bio, areas, updated_at) "
                        "VALUES(?, ?, ?, ?, ?, ?) "
                        "ON CONFLICT(email) DO UPDATE SET "
                        "orcid = excluded.orcid, institution = excluded.institution, bio = excluded.bio, "
                        "areas = excluded.areas, updated_at = excluded.updated_at";
      if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, user.email.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, orcid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, institution.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, bio.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, areas.c_str(), -1, SQLITE_TRANSIENT);
        std::string now_iso = iso_now();
        sqlite3_bind_text(stmt, 6, now_iso.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
      }
    }
    return {200, "application/json; charset=utf-8", "{\"ok\":true,\"status\":\"ok\",\"message\":\"Perfil editorial salvo com sucesso\"}"};
  }

  // Protected Write Operations
  if (!user.authenticated) {
    return error(401, "Autenticação obrigatória para operações editoriais de escrita");
  }

  if (target == "/api/v1/repos") {
    std::string name = json_extract(body, "name");
    std::string type = json_extract(body, "type");
    std::string vis = json_extract(body, "visibility");
    std::string title = json_extract(body, "title");
    std::string description = json_extract(body, "description");
    std::string author = json_extract(body, "author");
    if (type.empty()) type = "editorial";
    if (vis.empty()) vis = type == "editorial" ? "private" : "public";
    if (vis != "private" && vis != "public") return error(400, "Visibilidade deve ser 'private' ou 'public'");
    return create_repo(name, type, vis, title, description, author, user);
  }

  const std::string pre = "/api/v1/repos/";
  if (target.rfind(pre, 0) == 0) {
    std::string rest = target.substr(pre.size());
    size_t slash = rest.find('/');
    std::string name = url_decode(rest.substr(0, slash));
    if (!is_repo(name)) return error(404, "Repositório não encontrado");
    if (!is_repo_visible(name, user)) return error(403, "Acesso restrito ao proprietário do projeto");

    std::string action = slash == std::string::npos ? "" : rest.substr(slash + 1);

    if (action != "editorial/review" && !can_write_repo(name, user)) {
      return error(403, "Apenas o proprietário do projeto ou administrador pode alterar este repositório");
    }

    if (action == "visibility") {
      std::string owner = repo_owner(name);
      if (!owner.empty() && user.role != "admin" && owner != user.email) {
        return error(403, "Apenas o proprietário do repositório ou o administrador podem alterar sua visibilidade");
      }
      std::string vis = json_extract(body, "visibility");
      if (vis != "private" && vis != "public") return error(400, "Visibilidade deve ser 'public' ou 'private'");
      run({"git", "--git-dir=" + git_dir(name), "config", "sister.visibility", vis});
      if (owner.empty() && !user.email.empty()) {
        run({"git", "--git-dir=" + git_dir(name), "config", "sister.owner", user.email});
      }
      return {200, "application/json; charset=utf-8", "{\"ok\":true,\"name\":" + json(name) + ",\"visibility\":" + json(vis) + "}"};
    }

    if (action == "delete") {
      std::string owner = repo_owner(name);
      if (user.role != "admin" && (owner.empty() || owner != user.email)) {
        return error(403, "Apenas o proprietário do repositório ou o administrador podem excluí-lo");
      }
      std::string confirm_name = json_extract(body, "confirm_name");
      if (confirm_name != name) {
        return error(400, "Para confirmar a exclusão, digite o nome exato do repositório: '" + name + "'");
      }
      auto tags_res = run({"git", "--git-dir=" + git_dir(name), "tag", "-l", "ed/*"});
      if (!tags_res.out.empty() && user.role != "admin") {
        return error(403, "Este projeto contém edições publicadas (tags ed/*). A exclusão permanente de patrimônio epistêmico publicado é restrita ao administrador.");
      }
      std::error_code ec;
      fs::remove_all(git_dir(name), ec);
      if (ec) {
        return error(500, "Falha ao remover repositório do disco: " + ec.message());
      }
      if (g_db) {
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(g_db, "DELETE FROM project_members WHERE repo = ?", -1, &stmt, nullptr) == SQLITE_OK) {
          sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
          sqlite3_step(stmt);
          sqlite3_finalize(stmt);
        }
      }
      return {200, "application/json; charset=utf-8", "{\"ok\":true,\"deleted\":" + json(name) + "}"};
    }

    if (action == "delete-file") {
      std::string filename = json_extract(body, "filename");
      if (!valid_path(filename) || filename.empty()) return error(400, "Nome de arquivo inválido");
      if (filename == ".sister-editorial.json") return error(400, "O manifesto de governança editorial não pode ser excluído");
      
      std::string gd = git_dir(name);
      auto head_chk = run({"git", "--git-dir=" + gd, "rev-parse", "--verify", "HEAD"});
      if (head_chk.status != 0) return error(404, "Repositório vazio ou sem HEAD");
      std::string parent_commit = head_chk.out;
      while (!parent_commit.empty() && std::isspace(static_cast<unsigned char>(parent_commit.back()))) parent_commit.pop_back();
      
      std::string idx = gd + "/index_del_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(rand());
      std::string auth_name = user.name.empty() ? "SisTer Editor" : user.name;
      std::string auth_email = user.email.empty() ? "editorial@labeco.invalid" : user.email;
      std::map<std::string, std::string> env = {
        {"GIT_INDEX_FILE", idx},
        {"GIT_AUTHOR_NAME", auth_name},
        {"GIT_AUTHOR_EMAIL", auth_email},
        {"GIT_COMMITTER_NAME", auth_name},
        {"GIT_COMMITTER_EMAIL", auth_email}
      };
      run_stdin({"git", "--git-dir=" + gd, "read-tree", "HEAD"}, "", env);
      auto rm_r = run_stdin({"git", "--git-dir=" + gd, "update-index", "--force-remove", filename}, "", env);
      if (rm_r.status != 0) { unlink(idx.c_str()); return error(500, "Falha ao remover arquivo do índice Git"); }
      
      auto wt = run_stdin({"git", "--git-dir=" + gd, "write-tree"}, "", env);
      if (wt.status != 0) { unlink(idx.c_str()); return error(500, "Falha ao gravar árvore Git"); }
      std::string tree_sha = wt.out;
      while (!tree_sha.empty() && std::isspace(static_cast<unsigned char>(tree_sha.back()))) tree_sha.pop_back();
      
      auto ct = run_stdin({"git", "--git-dir=" + gd, "commit-tree", tree_sha, "-p", parent_commit}, "chore: remove file " + filename + "\n", env);
      unlink(idx.c_str());
      if (ct.status != 0) return error(500, "Falha ao gerar commit de remoção");
      std::string new_commit = ct.out;
      while (!new_commit.empty() && std::isspace(static_cast<unsigned char>(new_commit.back()))) new_commit.pop_back();
      run({"git", "--git-dir=" + gd, "update-ref", "refs/heads/main", new_commit, parent_commit});
      return {200, "application/json; charset=utf-8", "{\"ok\":true,\"status\":\"ok\",\"file\":" + json(filename) + ",\"commit\":" + json(new_commit) + "}"};
    }

    if (action == "editorial/migrate") {
      auto current = run({"git", "--git-dir=" + git_dir(name), "show", "HEAD:.sister-editorial.json"});
      if (current.status != 0) return error(409, "Manifesto editorial ausente");
      std::string manifest = current.out, migration_error;
      bool changed = false;
      if (!migrate_manifest_v1(manifest, name, changed, migration_error)) return error(409, migration_error);
      auto head = run({"git", "--git-dir=" + git_dir(name), "rev-parse", "HEAD"});
      std::string head_sha = head.out;
      while (!head_sha.empty() && std::isspace(static_cast<unsigned char>(head_sha.back()))) head_sha.pop_back();
      if (!changed) return {200, "application/json; charset=utf-8", "{\"ok\":true,\"migrated\":false,\"schema_version\":1,\"commit\":" + json(head_sha) + "}"};
      std::string expected_head = json_extract(body, "expected_head");
      auto migrated = git_commit_file(name, ".sister-editorial.json", manifest, "chore(editorial): migrate manifest to schema v1", user.name, user.email, true, expected_head);
      if (migrated.status != 0) return error(migrated.status == 409 ? 409 : 500, migrated.out);
      return {200, "application/json; charset=utf-8", "{\"ok\":true,\"migrated\":true,\"schema_version\":1,\"commit\":" + json(migrated.out) + "}"};
    }

    if (action == "editorial/save") {
      std::string filename = json_extract(body, "filename");
      std::string content = json_extract(body, "content");
      std::string message = json_extract(body, "message");
      std::string expected_head = json_extract(body, "expected_head");
      std::string author = json_extract(body, "author");
      if (author.empty()) author = json_extract(body, "author_name");
      if (author.empty()) author = user.name.empty() ? "Pesquisador LABECO" : user.name;
      if (filename.empty()) filename = "manuscrito.md";
      if (filename == ".sister-editorial.json") return error(403, "Manifesto editorial só pode ser alterado por operações editoriais controladas");
      if (message.empty()) message = "docs(editorial): update manuscript version";

      std::string author_email = user.email.empty() ? "editorial@labeco.invalid" : user.email;
      auto r = git_commit_file(name, filename, content, message, author, author_email, true, expected_head);
      if (r.status != 0) return error(r.status == 409 ? 409 : 500, r.out);
      return {200, "application/json; charset=utf-8", "{\"ok\":true,\"status\":\"ok\",\"commit\":" + json(r.out) + ",\"short_hash\":" + json(r.out.substr(0, 7)) + "}"};
    }

    if (action == "editorial/assign-member") {
      if (user.role != "admin") return error(403, "Designação de equipe restrita ao administrador editorial");
      std::string email = json_extract(body, "email");
      std::string role = json_extract(body, "role");
      std::transform(email.begin(), email.end(), email.begin(), [](unsigned char c) { return std::tolower(c); });
      if (role != "collaborator" && role != "reviewer" && role != "editor") return error(400, "Papel de projeto inválido");
      std::string allowed_name, allowed_role;
      if (!is_user_allowed(email, allowed_name, allowed_role)) return error(400, "Usuário deve estar previamente autorizado");
      sqlite3_stmt *stmt = nullptr;
      const char *sql = "INSERT INTO project_members(repo,email,role,assigned_by,assigned_at) VALUES(?,?,?,?,?) ON CONFLICT(repo,email) DO UPDATE SET role=excluded.role,assigned_by=excluded.assigned_by,assigned_at=excluded.assigned_at";
      if (!g_db || sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return error(500, "Falha ao preparar designação editorial");
      sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 2, email.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 3, role.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 4, user.email.c_str(), -1, SQLITE_TRANSIENT);
      std::string assigned_at = iso_now();
      sqlite3_bind_text(stmt, 5, assigned_at.c_str(), -1, SQLITE_TRANSIENT);
      bool ok = sqlite3_step(stmt) == SQLITE_DONE;
      sqlite3_finalize(stmt);
      if (!ok) return error(500, "Falha ao salvar designação editorial");
      return {200, "application/json; charset=utf-8", "{\"ok\":true,\"email\":" + json(email) + ",\"role\":" + json(role) + "}"};
    }

    if (action == "editorial/transition") {
      std::string next = json_extract(body, "to");
      std::string reason_text = json_extract(body, "reason");
      auto cur_man = run({"git", "--git-dir=" + git_dir(name), "show", "HEAD:.sister-editorial.json"});
      if (cur_man.status != 0 || !valid_json_object(cur_man.out)) return error(409, "Manifesto editorial ausente ou inválido");
      std::string current = json_extract(cur_man.out, "current_stage");
      bool admin = user.role == "admin" || project_member_role(name, user.email) == "editor";
      bool allowed =
        (current == "draft" && next == "submitted") ||
        (admin && current == "submitted" && next == "in_review") ||
        (admin && current == "in_review" && (next == "revision_required" || next == "accepted" || next == "rejected")) ||
        (current == "revision_required" && next == "resubmitted") ||
        (admin && current == "resubmitted" && next == "in_review") ||
        (current == "published" && next == "draft") ||
        ((current == "draft" || current == "submitted" || current == "revision_required") && next == "withdrawn");
      if (!allowed) return error(409, "Transição editorial inválida: " + current + " -> " + next);
      if ((next == "in_review" || next == "accepted" || next == "rejected" || next == "revision_required") && !admin) {
        return error(403, "Transição reservada ao administrador editorial");
      }
      bool is_decision = next == "accepted" || next == "rejected" || next == "revision_required";
      if (is_decision && reason_text.empty()) return error(400, "Decisão editorial exige justificativa");
      std::string manifest = cur_man.out;
      if (!json_replace_string(manifest, "current_stage", next)) return error(409, "Campo current_stage ausente ou inválido");
      auto head = run({"git", "--git-dir=" + git_dir(name), "rev-parse", "HEAD"});
      std::string source_commit = head.out;
      while (!source_commit.empty() && std::isspace(static_cast<unsigned char>(source_commit.back()))) source_commit.pop_back();
      std::string event = "{\"from\":" + json(current) + ",\"to\":" + json(next) + ",\"actor\":" + json(user.email) + ",\"date\":" + json(iso_now()) + ",\"reason\":" + json(reason_text) + ",\"source_commit\":" + json(source_commit) + "}";
      if (!json_append_object(manifest, "transitions", event)) return error(409, "Falha ao registrar histórico de transição");
      if (is_decision) {
        std::string decision = "{\"outcome\":" + json(next) + ",\"editor\":" + json(user.email) + ",\"date\":" + json(iso_now()) + ",\"justification\":" + json(reason_text) + ",\"source_commit\":" + json(source_commit) + "}";
        if (!json_append_object(manifest, "decisions", decision)) return error(409, "Falha ao registrar decisão editorial formal");
      }
      auto changed = git_commit_file(name, ".sister-editorial.json", manifest, "editorial: transition " + current + " -> " + next, user.name, user.email);
      if (changed.status != 0) return error(changed.status == 409 ? 409 : 500, changed.out);
      std::string submission_ref;
      if (next == "submitted" || next == "resubmitted") {
        submission_ref = "refs/sister/submissions/" + std::to_string(std::time(nullptr));
        auto ref_result = run({"git", "--git-dir=" + git_dir(name), "update-ref", submission_ref, changed.out, std::string(40, '0')});
        if (ref_result.status != 0) return error(500, "Transição salva, mas referência de submissão falhou");
      }
      return {200, "application/json; charset=utf-8", "{\"ok\":true,\"from\":" + json(current) + ",\"to\":" + json(next) + ",\"commit\":" + json(changed.out) + ",\"submission_ref\":" + (submission_ref.empty() ? "null" : json(submission_ref)) + "}"};
    }

    if (action == "editorial/review") {
      if (user.role != "admin" && project_member_role(name, user.email) != "reviewer") return error(403, "Parecer permitido somente a revisor formalmente designado");
      std::string reviewer = user.name.empty() ? user.email : user.name;
      std::string recommendation = json_extract(body, "recommendation");
      std::string summary = json_extract(body, "summary");
      if (summary.empty()) summary = json_extract(body, "comments");
      if (summary.empty()) return error(400, "Síntese do parecer é obrigatória");
      if (recommendation != "accept" && recommendation != "accept_with_revisions" && recommendation != "revise_and_resubmit" && recommendation != "reject") return error(400, "Recomendação editorial inválida");

      auto submitted = run({"git", "--git-dir=" + git_dir(name), "for-each-ref", "--sort=-creatordate", "--count=1", "--format=%(objectname)", "refs/sister/submissions"});
      std::string head_hash = submitted.status == 0 ? submitted.out : "";
      while (!head_hash.empty() && (head_hash.back() == '\n' || head_hash.back() == '\r' || head_hash.back() == ' ')) head_hash.pop_back();
      if (head_hash.empty()) return error(409, "Nenhuma submissão congelada encontrada para avaliação");

      auto cur_man = run({"git", "--git-dir=" + git_dir(name), "show", "HEAD:.sister-editorial.json"});
      std::string man_text = cur_man.status == 0 ? cur_man.out : "{}";
      if (!valid_json_object(man_text) || json_extract(man_text, "current_stage") != "in_review") {
        return error(409, "Parecer só pode ser registrado durante in_review");
      }

      std::string rev_id = "REV-" + std::to_string(std::time(nullptr));
      std::string new_rev = "{\n"
                            "      \"id\": " + json(rev_id) + ",\n"
                            "      \"commit\": " + json(head_hash) + ",\n"
                            "      \"reviewer\": " + json(reviewer) + ",\n"
                            "      \"date\": " + json(iso_now()) + ",\n"
                            "      \"recommendation\": " + json(recommendation) + ",\n"
                            "      \"summary\": " + json(summary);
      new_rev += "\n    }";

      size_t rev_pos = man_text.find("\"reviews\": [");
      if (rev_pos != std::string::npos) {
        rev_pos += 12;
        while (rev_pos < man_text.size() && (man_text[rev_pos] == ' ' || man_text[rev_pos] == '\n' || man_text[rev_pos] == '\r')) rev_pos++;
        if (rev_pos < man_text.size() && man_text[rev_pos] != ']') {
          man_text.insert(rev_pos, new_rev + ",\n    ");
        } else {
          man_text.insert(rev_pos, "\n    " + new_rev + "\n  ");
        }
      } else {
        size_t last_brace = man_text.rfind('}');
        if (last_brace != std::string::npos) {
          size_t prev = last_brace;
          while (prev > 0 && (man_text[prev - 1] == ' ' || man_text[prev - 1] == '\t' || man_text[prev - 1] == '\r' || man_text[prev - 1] == '\n')) {
            prev--;
          }
          bool is_empty = (prev > 0 && man_text[prev - 1] == '{');
          std::string comma = is_empty ? "\n" : ",\n";
          std::string ins = comma + "  \"reviews\": [\n    " + new_rev + "\n  ]\n";
          man_text.insert(last_brace, ins);
        }
      }

      std::string author_email = user.email.empty() ? "editorial@labeco.invalid" : user.email;
      auto r = git_commit_file(name, ".sister-editorial.json", man_text, "editorial: register peer review " + rev_id + " (" + recommendation + ")", reviewer, author_email);
      if (r.status != 0) return error(r.status == 409 ? 409 : 500, r.out);
      return {200, "application/json; charset=utf-8", "{\"ok\":true,\"status\":\"ok\",\"review_id\":" + json(rev_id) + "}"};
    }

    if (action == "editorial/respond") {
      if (!can_write_repo(name, user)) return error(403, "Resposta permitida somente ao autor responsável ou administrador");
      std::string review_id = json_extract(body, "review_id");
      std::string response_text = json_extract(body, "response");
      if (review_id.empty() || response_text.empty()) return error(400, "Parecer e resposta são obrigatórios");
      auto cur_man = run({"git", "--git-dir=" + git_dir(name), "show", "HEAD:.sister-editorial.json"});
      if (cur_man.status != 0 || !valid_json_object(cur_man.out)) return error(409, "Manifesto editorial ausente ou inválido");
      if (cur_man.out.find(json(review_id)) == std::string::npos) return error(404, "Parecer não encontrado");
      auto head = run({"git", "--git-dir=" + git_dir(name), "rev-parse", "HEAD"});
      std::string commit = head.out;
      while (!commit.empty() && std::isspace(static_cast<unsigned char>(commit.back()))) commit.pop_back();
      std::string manifest = cur_man.out;
      std::string response = "{\"review_id\":" + json(review_id) + ",\"author\":" + json(user.email) + ",\"date\":" + json(iso_now()) + ",\"response\":" + json(response_text) + ",\"commit\":" + json(commit) + "}";
      if (!json_append_object(manifest, "responses", response)) return error(409, "Falha ao registrar resposta no manifesto");
      auto saved = git_commit_file(name, ".sister-editorial.json", manifest, "editorial: author response to " + review_id, user.name, user.email);
      if (saved.status != 0) return error(saved.status == 409 ? 409 : 500, saved.out);
      return {200, "application/json; charset=utf-8", "{\"ok\":true,\"review_id\":" + json(review_id) + ",\"commit\":" + json(saved.out) + "}"};
    }

    if (action == "editorial/publish") {
      if (user.role != "admin" && project_member_role(name, user.email) != "editor") return error(403, "Publicação restrita ao editor responsável pelo projeto");
      std::string tag = json_extract(body, "tag");
      if (tag.empty()) tag = json_extract(body, "tag_name");
      std::string title = json_extract(body, "title");
      std::string notes = json_extract(body, "notes");
      if (tag.rfind("ed/", 0) != 0 || !valid_revision(tag)) return error(400, "Tag editorial deve seguir ed/vX.Y");
      if (title.empty()) title = tag;

      auto existing_tag = run({"git", "--git-dir=" + git_dir(name), "rev-parse", "--verify", tag + "^{commit}"});
      if (existing_tag.status == 0) {
        std::string commit = existing_tag.out;
        while (!commit.empty() && std::isspace(static_cast<unsigned char>(commit.back()))) commit.pop_back();
        auto tagged_manifest = run({"git", "--git-dir=" + git_dir(name), "show", tag + ":.sister-editorial.json"});
        if (tagged_manifest.status == 0 && valid_json_object(tagged_manifest.out) && json_extract(tagged_manifest.out, "current_stage") == "published" && tagged_manifest.out.find(json(tag)) != std::string::npos && tagged_manifest.out.find(json(title)) != std::string::npos && tagged_manifest.out.find(json(notes)) != std::string::npos) {
          return {200, "application/json; charset=utf-8", "{\"ok\":true,\"status\":\"ok\",\"idempotent\":true,\"tag\":" + json(tag) + ",\"commit\":" + json(commit) + "}"};
        }
        return error(409, "Tag editorial já existe, mas não representa esta publicação");
      }

      auto cur_man = run({"git", "--git-dir=" + git_dir(name), "show", "HEAD:.sister-editorial.json"});
      std::string man_text = cur_man.status == 0 ? cur_man.out : "{}";
      if (!valid_json_object(man_text)) return error(409, "Manifesto editorial ausente ou inválido");
      std::string current_stage = json_extract(man_text, "current_stage");
      if (current_stage == "published" && man_text.find(json(tag)) != std::string::npos) {
        if (man_text.find(json(title)) == std::string::npos || man_text.find(json(notes)) == std::string::npos) {
          return error(409, "Publicação parcial encontrada, mas os metadados informados não correspondem ao manifesto");
        }
        auto head = run({"git", "--git-dir=" + git_dir(name), "rev-parse", "HEAD"});
        std::string commit = head.out;
        while (!commit.empty() && std::isspace(static_cast<unsigned char>(commit.back()))) commit.pop_back();
        auto recovered_tag = run({"git", "--git-dir=" + git_dir(name), "tag", "-a", tag, commit, "-m", title + " - " + notes});
        if (recovered_tag.status != 0) return error(500, "Falha ao recuperar tag da publicação: " + recovered_tag.out);
        return {200, "application/json; charset=utf-8", "{\"ok\":true,\"status\":\"ok\",\"recovered\":true,\"tag\":" + json(tag) + ",\"commit\":" + json(commit) + "}"};
      }
      if (current_stage != "accepted") return error(409, "Publicação exige estado editorial accepted");
      if (man_text.find("\"outcome\":\"accepted\"") == std::string::npos && man_text.find("\"outcome\": \"accepted\"") == std::string::npos) {
        return error(409, "Publicação exige decisão editorial formal de aceite");
      }
      if (!json_replace_string(man_text, "current_stage", "published")) return error(409, "Campo current_stage ausente ou inválido");
      auto accepted_head = run({"git", "--git-dir=" + git_dir(name), "rev-parse", "HEAD"});
      std::string source_commit = accepted_head.out;
      while (!source_commit.empty() && std::isspace(static_cast<unsigned char>(source_commit.back()))) source_commit.pop_back();
      std::string publish_event = "{\"from\":\"accepted\",\"to\":\"published\",\"actor\":" + json(user.email) + ",\"date\":" + json(iso_now()) + ",\"reason\":" + json(notes) + ",\"source_commit\":" + json(source_commit) + "}";
      if (!json_append_object(man_text, "transitions", publish_event)) return error(409, "Falha ao registrar transição de publicação");
      std::string edition = "{\"tag\":" + json(tag) + ",\"title\":" + json(title) + ",\"notes\":" + json(notes) + ",\"publisher\":" + json(user.email) + ",\"date\":" + json(iso_now()) + "}";
      if (!json_append_object(man_text, "editions", edition)) return error(409, "Falha ao registrar metadados da edição");

      std::string auth_name = user.name.empty() ? "SisTer Editor" : user.name;
      std::string auth_email = user.email.empty() ? "editorial@labeco.invalid" : user.email;
      auto published = git_commit_file(name, ".sister-editorial.json", man_text, "editorial: publish edition " + tag, auth_name, auth_email);
      if (published.status != 0) return error(published.status == 409 ? 409 : 500, published.out);

      auto tag_r = run({"git", "--git-dir=" + git_dir(name), "tag", "-a", tag, published.out, "-m", title + " - " + notes});
      if (tag_r.status != 0) return error(500, "Manifesto publicado, mas falhou criação da tag editorial: " + tag_r.out);
      return {200, "application/json; charset=utf-8", "{\"ok\":true,\"status\":\"ok\",\"tag\":" + json(tag) + ",\"commit\":" + json(published.out) + "}"};
    }

    if (action == "upload") {
      if (!user.authenticated) return error(401, "Autenticação requerida para envio de arquivos");
      if (repo_visibility(name) == "private" && user.role != "admin" && repo_owner(name) != user.email) {
        return error(403, "Sem permissão para alterar este repositório privado");
      }

      std::string filename = json_extract(body, "filename");
      std::string b64 = json_extract(body, "content_base64");
      if (b64.empty()) b64 = json_extract(body, "content_b64");
      std::string commit_msg = json_extract(body, "commit_msg");
      std::string expected_head = json_extract(body, "expected_head");
      if (commit_msg.empty()) commit_msg = json_extract(body, "message");
      std::string is_main = json_extract(body, "is_main");
      if (filename.empty() || !valid_path(filename)) return error(400, "Nome de arquivo inválido");
      if (filename == ".sister-editorial.json") return error(403, "Manifesto editorial só pode ser alterado por operações editoriais controladas");
      if (commit_msg.empty()) commit_msg = "Upload de arquivo: " + filename;

      std::string content;
      if (!base64_decode(b64, content)) return error(400, "Conteúdo Base64 ausente ou inválido");
      std::string author_name = user.name.empty() ? "Pesquisador LABECO" : user.name;
      std::string author_email = user.email.empty() ? "editorial@labeco.invalid" : user.email;
      std::vector<FileUpdate> updates = {{filename, content, false}};
      if (is_main == "true") {
        auto m_res = run({"git", "--git-dir=" + git_dir(name), "show", "HEAD:.sister-editorial.json"});
        if (m_res.status != 0 || !valid_json_object(m_res.out)) return error(409, "Manifesto editorial ausente ou inválido; upload principal cancelado");
        std::string m = m_res.out;
        size_t doc_main_pos = m.find("\"document_main\":");
        if (doc_main_pos != std::string::npos) {
          size_t val_start = m.find('"', doc_main_pos + 16);
          size_t val_end = val_start == std::string::npos ? std::string::npos : m.find('"', val_start + 1);
          if (val_end == std::string::npos) return error(409, "Campo document_main inválido; upload cancelado");
          m.replace(val_start, val_end - val_start + 1, json(filename));
        } else {
          size_t brace = m.find('{');
          m.insert(brace + 1, "\n  \"document_main\": " + json(filename) + ",");
        }
        if (!valid_json_object(m)) return error(409, "Não foi possível atualizar document_main com segurança");
        updates.push_back({".sister-editorial.json", m, true});
      }

      auto r = git_commit_files(name, updates, commit_msg, author_name, author_email, expected_head);
      if (r.status != 0) return error(r.status == 409 ? 409 : 500, "Falha ao gravar upload atômico: " + r.out);
      return {200, "application/json; charset=utf-8", "{\"ok\":true,\"status\":\"ok\",\"filename\":" + json(filename) + ",\"commit\":" + json(r.out) + "}"};
    }

    return error(404, "Ação não encontrada");
  }

  return error(404, "Rota não encontrada");
}

static std::string reason(int s) {
  if (s == 200) return "OK";
  if (s == 400) return "Bad Request";
  if (s == 401) return "Unauthorized";
  if (s == 403) return "Forbidden";
  if (s == 404) return "Not Found";
  if (s == 405) return "Method Not Allowed";
  if (s == 409) return "Conflict";
  return "Internal Server Error";
}

static void client(int fd) {
  std::string req;
  char b[4096];
  while (req.find("\r\n\r\n") == std::string::npos && req.size() < 65536) {
    ssize_t n = read(fd, b, sizeof(b));
    if (n <= 0) break;
    req.append(b, n);
  }
  size_t header_end = req.find("\r\n\r\n");
  if (header_end == std::string::npos) { close(fd); return; }

  std::string header_part = req.substr(0, header_end);
  std::string body = req.substr(header_end + 4);

  std::istringstream in(header_part);
  std::string method, target, version;
  in >> method >> target >> version;

  if (method == "POST") {
    size_t cl_pos = header_part.find("Content-Length: ");
    if (cl_pos == std::string::npos) cl_pos = header_part.find("content-length: ");
    if (cl_pos != std::string::npos) {
      size_t line_end = header_part.find("\r\n", cl_pos);
      std::string len_str = header_part.substr(cl_pos + 16, line_end - (cl_pos + 16));
      try {
        size_t content_length = std::stoul(len_str);
        while (body.size() < content_length && body.size() < max_upload) {
          ssize_t n = read(fd, b, sizeof(b));
          if (n <= 0) break;
          body.append(b, n);
        }
      } catch (...) {}
    }
  }

  AuthUser user = authenticate_request(header_part, target);
  Response r;
  if (method == "GET" || method == "HEAD") {
    r = route(target, user);
  } else if (method == "POST") {
    r = route_post(target, header_part, body, user);
  } else {
    r = error(405, "Método não permitido");
  }

  std::string h = "HTTP/1.1 " + std::to_string(r.status) + " " + reason(r.status) + "\r\n"
                  "Content-Type: " + r.type + "\r\n"
                  "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
  for (const auto &eh : r.extra_headers) {
    h += eh + "\r\n";
  }
  h += "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
       "Content-Security-Policy: default-src 'self'; style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; font-src 'self' https://fonts.gstatic.com; script-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self' blob:; frame-src 'self' blob:; object-src 'self' blob:;\r\n"
       "Connection: close\r\n\r\n";

  auto write_all = [](int sfd, const char *buf, size_t len) -> bool {
    size_t sent = 0;
    while (sent < len) {
      ssize_t n = write(sfd, buf + sent, len - sent);
      if (n <= 0) {
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        return false;
      }
      sent += n;
    }
    return true;
  };

  write_all(fd, h.data(), h.size());
  if (method != "HEAD") {
    write_all(fd, r.body.data(), r.body.size());
  }
  close(fd);
}


static void load_env_file(const std::string &path) {
  std::ifstream f(path);
  if (!f) return;
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    size_t eq = line.find('=');
    if (eq != std::string::npos) {
      std::string k = line.substr(0, eq), v = line.substr(eq + 1);
      while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
      while (!v.empty() && (v.back() == '\r' || v.back() == '\n' || v.back() == ' ')) v.pop_back();
      if (!v.empty() && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
      setenv(k.c_str(), v.c_str(), 0);
    }
  }
}

int main(int argc, char **argv) {
  repo_root = getenv("SISTER_GIT_ROOT") ? getenv("SISTER_GIT_ROOT") : "";
  if (repo_root.empty()) {
    if (fs::is_directory("/srv/git")) repo_root = "/srv/git";
    else repo_root = ".";
  }

  web_root = getenv("SISTER_GIT_WEB_ROOT") ? getenv("SISTER_GIT_WEB_ROOT") : "";
  if (web_root.empty()) {
    if (fs::is_directory("web")) web_root = "web";
    else if (fs::is_directory("/usr/local/share/sister-git/web")) web_root = "/usr/local/share/sister-git/web";
    else web_root = "web";
  }

  auth_token = getenv("SISTER_GIT_WEB_TOKEN") ? getenv("SISTER_GIT_WEB_TOKEN") : "";
  bootstrap_admin_email = getenv("SISTER_GIT_ADMIN_EMAIL") ? getenv("SISTER_GIT_ADMIN_EMAIL") : "";
  std::transform(bootstrap_admin_email.begin(), bootstrap_admin_email.end(), bootstrap_admin_email.begin(), [](unsigned char c) { return std::tolower(c); });

  // Load .smtp.env if exists
  load_env_file(repo_root + "/.smtp.env");

  // Load SMTP config from environment
  if (getenv("SISTER_SMTP_USER")) g_smtp.user = getenv("SISTER_SMTP_USER");
  if (getenv("SISTER_SMTP_PASS")) g_smtp.pass = getenv("SISTER_SMTP_PASS");
  if (getenv("SISTER_SMTP_FROM")) g_smtp.from = getenv("SISTER_SMTP_FROM");
  if (getenv("SISTER_SMTP_HOST")) g_smtp.host = getenv("SISTER_SMTP_HOST");
  if (getenv("SISTER_SMTP_PORT")) {
    try { g_smtp.port = std::stoi(getenv("SISTER_SMTP_PORT")); } catch (...) {}
  }
  if (getenv("SISTER_DEV_AUTH")) g_smtp.dev_mode = (std::string(getenv("SISTER_DEV_AUTH")) == "1");

  std::string bind = "127.0.0.1";
  if (getenv("SISTER_BIND")) bind = getenv("SISTER_BIND");
  int port = 8787;
  if (getenv("SISTER_PORT")) {
    try { port = std::stoi(getenv("SISTER_PORT")); } catch (...) {}
  }
  bool smtp_prompt = false;

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--bind" && i + 1 < argc) bind = argv[++i];
    else if (a == "--port" && i + 1 < argc) port = std::stoi(argv[++i]);
    else if (a == "--repos" && i + 1 < argc) repo_root = argv[++i];
    else if (a == "--web" && i + 1 < argc) web_root = argv[++i];
    else if (a == "--smtp-user" && i + 1 < argc) g_smtp.user = argv[++i];
    else if (a == "--smtp-pass" && i + 1 < argc) g_smtp.pass = argv[++i];
    else if (a == "--smtp-from" && i + 1 < argc) g_smtp.from = argv[++i];
    else if (a == "--smtp-host" && i + 1 < argc) g_smtp.host = argv[++i];
    else if (a == "--smtp-port" && i + 1 < argc) g_smtp.port = std::stoi(argv[++i]);
    else if (a == "--smtp-prompt") smtp_prompt = true;
    else if (a == "--dev-auth") g_smtp.dev_mode = true;
    else if (a == "--set-admin" && i + 1 < argc) {
      std::string admin_target = argv[++i];
      std::transform(admin_target.begin(), admin_target.end(), admin_target.begin(), [](unsigned char c) { return std::tolower(c); });
      init_auth_db();
      if (!g_db) {
        std::cerr << "[ERRO] Não foi possível abrir o banco de dados em " << repo_root << "/.sister-auth.db\n";
        return 1;
      }
      sqlite3_stmt *stmt = nullptr;
      const char *sql_allowed = "INSERT INTO allowed_users(email, name, role, created_by, created_at) VALUES(?, ?, 'admin', 'cli', datetime('now')) "
                                "ON CONFLICT(email) DO UPDATE SET role = 'admin'";
      if (sqlite3_prepare_v2(g_db, sql_allowed, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, admin_target.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, admin_target.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
      }
      const char *sql_users = "UPDATE users SET role = 'admin' WHERE email = ?";
      if (sqlite3_prepare_v2(g_db, sql_users, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, admin_target.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
      }
      const char *sql_sessions = "UPDATE sessions SET role = 'admin' WHERE email = ?";
      if (sqlite3_prepare_v2(g_db, sql_sessions, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, admin_target.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
      }
      std::cout << "[OK] Usuário '" << admin_target << "' configurado como 'admin' com sucesso no SisTer Git!\n";
      return 0;
    }
    else {
      std::cerr << "Uso: sister-git-web [--bind 0.0.0.0] [--port 8787] [--smtp-user USER] [--smtp-pass SENHA] [--smtp-prompt]\n";
      return 2;
    }
  }

  // Interactive prompt if requested
  if (smtp_prompt && (g_smtp.user.empty() || g_smtp.pass.empty())) {
    std::cout << "\n=======================================================\n"
              << " SisTer Git Editorial · Autenticação SMTP Institucional\n"
              << "=======================================================\n";
    if (g_smtp.user.empty()) {
      std::cout << "Remetente Gmail (ex: seu.email@gmail.com): " << std::flush;
      std::getline(std::cin, g_smtp.user);
    }
    if (g_smtp.pass.empty()) {
      if (isatty(STDIN_FILENO)) {
        char *p = getpass("Senha de App Google (16 caracteres, entrada protegida): ");
        if (p) g_smtp.pass = p;
      } else {
        std::cout << "Senha de App Google (16 caracteres): " << std::flush;
        std::getline(std::cin, g_smtp.pass);
      }
    }
    std::cout << "\n";
  }

  // Sanitize credentials: remove accidental spaces from app passwords
  g_smtp.user.erase(std::remove_if(g_smtp.user.begin(), g_smtp.user.end(), [](unsigned char c){ return std::isspace(c); }), g_smtp.user.end());
  g_smtp.pass.erase(std::remove_if(g_smtp.pass.begin(), g_smtp.pass.end(), [](unsigned char c){ return std::isspace(c); }), g_smtp.pass.end());

  if (auth_token.empty()) {
    std::ifstream tf(repo_root + "/.web_token");
    if (tf) {
      std::getline(tf, auth_token);
      while (!auth_token.empty() && (auth_token.back() == '\n' || auth_token.back() == '\r' || auth_token.back() == ' ')) auth_token.pop_back();
    }
  }

  // Security gate when exposed outside loopback (0.0.0.0 or LAN IP):
  // Either master static token OR SMTP authentication MUST be active!
  if (bind != "127.0.0.1" && bind != "::1" && auth_token.empty() && !g_smtp.enabled() && !g_smtp.dev_mode) {
    std::cerr << "[ERRO] Fora de loopback (0.0.0.0), configure credenciais SMTP (--smtp-user e --smtp-pass ou --smtp-prompt) ou um token mestre.\n";
    return 1;
  }

  if (!fs::is_directory(repo_root) || !fs::is_directory(web_root)) {
    std::cerr << "[ERRO] Diretório de repositórios ou web ausente\n";
    return 1;
  }

  // Initialize SQLite Authentication DB
  init_auth_db();

  std::error_code guard_ec;
  for (const auto &entry : fs::directory_iterator(repo_root, guard_ec)) {
    std::string file = entry.path().filename().string();
    if (file.size() <= 4 || file.substr(file.size() - 4) != ".git") continue;
    std::string name = file.substr(0, file.size() - 4);
    if (!is_repo(name) || repo_type(name) != "editorial") continue;
    std::string guard_error;
    if (!ensure_editorial_push_guard(name, guard_error)) {
      std::cerr << "[ERRO] Governança Git editorial não protegida: " << guard_error << "\n";
      return 1;
    }
  }

  signal(SIGPIPE, SIG_IGN);
  int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, bind.c_str(), &addr.sin_addr) != 1 || ::bind(s, (sockaddr*)&addr, sizeof(addr)) || listen(s, 64)) {
    std::cerr << "[ERRO] bind/listen em " << bind << ":" << port << " : " << strerror(errno) << "\n";
    return 1;
  }

  std::cout << "[OK] SisTer Git Web escutando em http://" << bind << ":" << port
            << " (módulo editorial C++26 · SMTP " << (g_smtp.enabled() ? "ativo" : "inativo") << ")\n";
  if (bind == "0.0.0.0") {
    std::cout << "[LAN] Acesso via rede local disponível: http://<ip-do-servidor>:" << port << "\n";
  }
  std::cout << std::flush;

  for (;;) {
    int c = accept(s, nullptr, nullptr);
    if (c >= 0) {
      struct timeval tv{};
      tv.tv_sec = 10;
      tv.tv_usec = 0;
      setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
      setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
      client(c);
    }
  }
}
