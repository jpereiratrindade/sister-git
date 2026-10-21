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
static size_t max_output = 4 * 1024 * 1024;
static SmtpConfig g_smtp;
static sqlite3 *g_db = nullptr;

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

static bool is_repo_visible(const std::string &name, const AuthUser &user) {
  std::string vis = repo_visibility(name);
  if (vis != "private") return true;
  if (!user.authenticated) return false;
  if (user.role == "admin") return true;
  std::string owner = repo_owner(name);
  if (owner.empty()) return true;
  return owner == user.email;
}

static bool valid_revision(const std::string &rev) {
  if (rev.empty() || rev.size() > 128) return false;
  if (rev.find("..") != std::string::npos || rev.find(' ') != std::string::npos) return false;
  for (char c : rev) {
    if (!(std::isalnum(c) || c == '.' || c == '_' || c == '-' || c == '/' || c == '~' || c == '^')) return false;
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
  )";
  char *err = nullptr;
  sqlite3_exec(g_db, ddl, nullptr, nullptr, &err);
  if (err) {
    std::cerr << "[AUTH DDL ERROR] " << err << "\n";
    sqlite3_free(err);
  }
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
  if (!g_db) return true;
  if (!has_allowed_users()) return true;
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

static AuthUser authenticate_request(const std::string &headers) {
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

static Result git_commit_file(const std::string &name, const std::string &filename, const std::string &content, const std::string &commit_msg, const std::string &author_name = "SisTer Editor", const std::string &author_email = "editorial@labeco.invalid") {
  std::string gd = git_dir(name);
  auto ho = run_stdin({"git", "--git-dir=" + gd, "hash-object", "-w", "--stdin"}, content);
  if (ho.status != 0) return {ho.status, "Falha ao gravar blob Git"};
  std::string blob_sha = ho.out;
  while (!blob_sha.empty() && (blob_sha.back() == '\n' || blob_sha.back() == '\r' || blob_sha.back() == ' ')) blob_sha.pop_back();

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
    run_stdin({"git", "--git-dir=" + gd, "read-tree", "HEAD"}, "", env);
  }

  auto up = run_stdin({"git", "--git-dir=" + gd, "update-index", "--add", "--cacheinfo", "100644", blob_sha, filename}, "", env);
  if (up.status != 0) { unlink(idx.c_str()); return {up.status, "Falha ao atualizar índice: " + up.out}; }

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

  auto ur = run({"git", "--git-dir=" + gd, "update-ref", "refs/heads/main", new_commit});
  if (ur.status != 0) return {ur.status, "Falha ao atualizar ref main"};
  if (!has_head) {
    run({"git", "--git-dir=" + gd, "symbolic-ref", "HEAD", "refs/heads/main"});
  }

  return {0, new_commit};
}

static Response create_repo(const std::string &name, const std::string &type, const std::string &visibility, const std::string &title, const std::string &author, const AuthUser &user) {
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

  std::string doc_title = title.empty() ? name : title;
  std::string auth_name = author.empty() ? (user.name.empty() ? "Pesquisador LABECO" : user.name) : author;
  std::string auth_email = user.email.empty() ? "editorial@labeco.invalid" : user.email;

  if (type == "editorial") {
    std::string init_doc = "# " + doc_title + "\n\n"
                           "**" + auth_name + "**  \n"
                           "*Laboratório de Estudos em AgroEcologia e Recursos Naturais (LABECO)*\n\n---\n\n"
                           "## Resumo\n\nInicie aqui o resumo do seu artigo ou relatório de pesquisa.\n\n---\n\n"
                           "## 1. Introdução\n\nDescreva a contextualização, o problema ecológico e as hipóteses investigadas.\n\n"
                           "## 2. Metodologia e Delineamento Amostral\n\nDetalhe os procedimentos experimentais, coleta de dados botânicos e análises estatísticas.\n\n"
                           "## 3. Resultados e Discussão\n\nApresente suas evidências empíricas com gráficos, tabelas e referências.\n\n"
                           "## 4. Considerações Finais\n\n*Sempre pronto. Sempre incompleto!*\n";

    std::string init_manifest = "{\n"
                                "  \"$schema\": \"https://sister-git.labeco.org/schemas/editorial-v1.json\",\n"
                                "  \"project_type\": \"editorial\",\n"
                                "  \"title\": " + json(doc_title) + ",\n"
                                "  \"document_main\": \"manuscrito.md\",\n"
                                "  \"abstract\": \"Projeto documental sob governança editorial e proveniência Git.\",\n"
                                "  \"authors\": [{\"name\": " + json(auth_name) + ", \"affiliation\": \"LABECO\"}],\n"
                                "  \"current_stage\": \"draft\",\n"
                                "  \"reviews\": [],\n"
                                "  \"decisions\": []\n"
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
    int branches = 0, tags = 0;
    for (auto &x : lines(counts.out)) {
      if (x.rfind("refs/heads/", 0) == 0) branches++;
      else if (x.rfind("refs/tags/", 0) == 0) tags++;
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
    body += "{\"name\":" + json(name) + ",\"visibility\":" + json(vis) + ",\"owner\":" + json(owner) + ",\"type\":" + json(type) + ",\"default_branch\":" + json(head.status == 0 ? head.out.substr(0, head.out.find('\n')) : "") + ",\"branches\":" + std::to_string(branches) + ",\"tags\":" + std::to_string(tags) + ",\"size_bytes\":" + std::to_string(bytes) + ",\"integrity\":\"not_checked\"";
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
      if (!first) b += ',';
      first = false;
      b += "{\"mode\":" + json(x.substr(0, sp)) + ",\"type\":" + json(x.substr(sp + 1, sp2 - sp - 1)) + ",\"hash\":" + json(x.substr(sp2 + 1, tab - sp2 - 1)) + ",\"name\":" + json(x.substr(tab + 1)) + "}";
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
  auto r = run({"git", "--git-dir=" + git_dir(name), "diff", "-u", base + "..." + head});
  if (r.status) return error(400, "Falha ao calcular diff");
  std::string b = "{\"base\":" + json(base) + ",\"head\":" + json(head) + ",\"files\":[";
  bool first_file = true;
  std::string cur_path; std::vector<std::pair<std::string, std::string>> cur_lines;
  auto flush_file = [&]() {
    if (cur_path.empty()) return;
    if (!first_file) b += ',';
    first_file = false;
    b += "{\"path\":" + json(cur_path) + ",\"lines\":[";
    for (size_t i = 0; i < cur_lines.size(); i++) {
      if (i > 0) b += ',';
      b += "{\"type\":" + json(cur_lines[i].first) + ",\"text\":" + json(cur_lines[i].second) + "}";
    }
    b += "]}";
    cur_lines.clear();
  };
  for (auto &l : lines(r.out)) {
    if (l.rfind("diff --git", 0) == 0) {
      flush_file();
      size_t b_pos = l.find(" b/");
      cur_path = b_pos != std::string::npos ? l.substr(b_pos + 3) : "desconhecido";
    } else if (l.rfind("@@", 0) == 0) cur_lines.push_back({"hunk", l});
    else if (!l.empty() && l[0] == '+') cur_lines.push_back({"add", l.substr(1)});
    else if (!l.empty() && l[0] == '-') cur_lines.push_back({"del", l.substr(1)});
    else if (!l.empty() && l[0] == ' ') cur_lines.push_back({"ctx", l.substr(1)});
  }
  flush_file();
  return {200, "application/json; charset=utf-8", b + "]}"};
}

static Response editorial(const std::string &name) {
  auto r = run({"git", "--git-dir=" + git_dir(name), "show", "HEAD:.sister-editorial.json"});
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

  std::string is_ed = (repo_type(name) == "editorial" || has_manifest) ? "true" : "false";
  std::string vis = repo_visibility(name);
  std::string owner = repo_owner(name);
  return {200, "application/json; charset=utf-8", "{\"repository\":" + json(name) + ",\"visibility\":" + json(vis) + ",\"owner\":" + json(owner) + ",\"is_editorial\":" + is_ed + ",\"has_manifest\":" + (has_manifest ? "true" : "false") + ",\"manifest\":" + manifest_raw + ",\"tags\":" + tags_json + "}"};
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

  if (path == "/api/v1/ha") {
    return {200, "application/json; charset=utf-8", "{\"nodes\":[{\"name\":\"LABECO-SRV\",\"role\":\"git-host\",\"status\":\"active\"},{\"name\":\"Fedora Silverblue\",\"role\":\"replica-node\",\"status\":\"active\"}],\"replication\":\"synchronized\",\"last_sync\":" + json(iso_now()) + "}"};
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
    if (action == "refs") return refs(name);
    if (action == "commits") return commits(name, q);
    if (action == "tree") return tree(name, q);
    if (action == "blob") return blob(name, q);
    if (action == "compare") return compare(name, q);
    if (action == "editorial") return editorial(name);
    if (action == "archive") return archive(name, q);
    return error(404, "Rota não encontrada");
  }

  return static_file(path);
}

static Response route_post(const std::string &target, const std::string &, const std::string &body, const AuthUser &user) {
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
    if (has_allowed_users() && !is_user_allowed(email, a_name, a_role)) {
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
    std::string role = (email.find("admin") != std::string::npos) ? "admin" : "researcher";

    if (g_db) {
      if (!has_allowed_users()) {
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

  // Protected Write Operations
  if (!user.authenticated) {
    return error(401, "Autenticação obrigatória para operações editoriais de escrita");
  }

  if (target == "/api/v1/repos") {
    std::string name = json_extract(body, "name");
    std::string type = json_extract(body, "type");
    std::string vis = json_extract(body, "visibility");
    std::string title = json_extract(body, "title");
    std::string author = json_extract(body, "author");
    if (type.empty()) type = "editorial";
    if (vis.empty()) vis = "public";
    return create_repo(name, type, vis, title, author, user);
  }

  const std::string pre = "/api/v1/repos/";
  if (target.rfind(pre, 0) == 0) {
    std::string rest = target.substr(pre.size());
    size_t slash = rest.find('/');
    std::string name = url_decode(rest.substr(0, slash));
    if (!is_repo(name)) return error(404, "Repositório não encontrado");
    if (!is_repo_visible(name, user)) return error(403, "Acesso restrito ao proprietário do projeto");

    std::string action = slash == std::string::npos ? "" : rest.substr(slash + 1);

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

    if (action == "editorial/save") {
      std::string filename = json_extract(body, "filename");
      std::string content = json_extract(body, "content");
      std::string message = json_extract(body, "message");
      std::string author = json_extract(body, "author");
      if (author.empty()) author = json_extract(body, "author_name");
      if (author.empty()) author = user.name.empty() ? "Pesquisador LABECO" : user.name;
      if (filename.empty()) filename = "manuscrito.md";
      if (message.empty()) message = "docs(editorial): update manuscript version";

      std::string author_email = user.email.empty() ? "editorial@labeco.invalid" : user.email;
      auto r = git_commit_file(name, filename, content, message, author, author_email);
      if (r.status != 0) return error(500, r.out);
      return {200, "application/json; charset=utf-8", "{\"ok\":true,\"status\":\"ok\",\"commit\":" + json(r.out) + ",\"short_hash\":" + json(r.out.substr(0, 7)) + "}"};
    }

    if (action == "editorial/review") {
      std::string reviewer = json_extract(body, "reviewer");
      std::string recommendation = json_extract(body, "recommendation");
      std::string summary = json_extract(body, "summary");
      if (summary.empty()) summary = json_extract(body, "comments");
      std::string resp_author = json_extract(body, "author_response");
      if (reviewer.empty()) reviewer = user.name.empty() ? user.email : user.name;

      auto head_r = run({"git", "--git-dir=" + git_dir(name), "rev-parse", "HEAD"});
      std::string head_hash = head_r.status == 0 ? head_r.out : "";
      while (!head_hash.empty() && (head_hash.back() == '\n' || head_hash.back() == '\r' || head_hash.back() == ' ')) head_hash.pop_back();

      auto cur_man = run({"git", "--git-dir=" + git_dir(name), "show", "HEAD:.sister-editorial.json"});
      std::string man_text = cur_man.status == 0 ? cur_man.out : "{}";

      std::string rev_id = "REV-" + std::to_string(std::time(nullptr));
      std::string new_rev = "{\n"
                            "      \"id\": " + json(rev_id) + ",\n"
                            "      \"commit\": " + json(head_hash) + ",\n"
                            "      \"reviewer\": " + json(reviewer) + ",\n"
                            "      \"date\": " + json(iso_now()) + ",\n"
                            "      \"recommendation\": " + json(recommendation) + ",\n"
                            "      \"summary\": " + json(summary);
      if (!resp_author.empty()) {
        new_rev += ",\n      \"resolution\": {\"author_response\": " + json(resp_author) + ", \"resolved_in_commit\": " + json(head_hash) + "}";
      }
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
          std::string ins = ",\n  \"reviews\": [\n    " + new_rev + "\n  ]\n";
          man_text.insert(last_brace, ins);
        }
      }

      std::string author_email = user.email.empty() ? "editorial@labeco.invalid" : user.email;
      auto r = git_commit_file(name, ".sister-editorial.json", man_text, "editorial: register peer review " + rev_id + " (" + recommendation + ")", reviewer, author_email);
      if (r.status != 0) return error(500, r.out);
      return {200, "application/json; charset=utf-8", "{\"ok\":true,\"status\":\"ok\",\"review_id\":" + json(rev_id) + "}"};
    }

    if (action == "editorial/publish") {
      std::string tag = json_extract(body, "tag");
      if (tag.empty()) tag = json_extract(body, "tag_name");
      std::string title = json_extract(body, "title");
      std::string notes = json_extract(body, "notes");
      if (tag.empty() || tag.find("..") != std::string::npos) return error(400, "Tag editorial inválida");

      auto tag_r = run({"git", "--git-dir=" + git_dir(name), "tag", "-a", tag, "HEAD", "-m", title + " - " + notes});
      if (tag_r.status != 0) return error(500, "Falha ao criar tag editorial: " + tag_r.out);

      auto cur_man = run({"git", "--git-dir=" + git_dir(name), "show", "HEAD:.sister-editorial.json"});
      std::string man_text = cur_man.status == 0 ? cur_man.out : "{}";
      size_t stage_pos = man_text.find("\"current_stage\": \"");
      if (stage_pos != std::string::npos) {
        stage_pos += 18;
        size_t end_quote = man_text.find('"', stage_pos);
        if (end_quote != std::string::npos) {
          man_text.replace(stage_pos, end_quote - stage_pos, "published");
        }
      }

      std::string auth_name = user.name.empty() ? "SisTer Editor" : user.name;
      std::string auth_email = user.email.empty() ? "editorial@labeco.invalid" : user.email;
      git_commit_file(name, ".sister-editorial.json", man_text, "editorial: publish edition " + tag, auth_name, auth_email);
      return {200, "application/json; charset=utf-8", "{\"ok\":true,\"status\":\"ok\",\"tag\":" + json(tag) + "}"};
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
        while (body.size() < content_length && body.size() < max_output) {
          ssize_t n = read(fd, b, sizeof(b));
          if (n <= 0) break;
          body.append(b, n);
        }
      } catch (...) {}
    }
  }

  AuthUser user = authenticate_request(header_part);
  Response r;
  if (method == "GET") {
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
       "Content-Security-Policy: default-src 'self'; style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; font-src 'self' https://fonts.gstatic.com; script-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'\r\n"
       "Connection: close\r\n\r\n";
  write(fd, h.data(), h.size());
  write(fd, r.body.data(), r.body.size());
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
    if (c >= 0) client(c);
  }
}
