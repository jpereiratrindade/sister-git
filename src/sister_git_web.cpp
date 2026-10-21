#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
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
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct Result { int status; std::string out; };
struct Response { int status = 200; std::string type = "application/json; charset=utf-8"; std::string body; };

static std::string repo_root, web_root, auth_token;
static size_t max_output = 4 * 1024 * 1024;

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

static Result run(const std::vector<std::string> &args) {
  int p[2]; if (pipe(p) != 0) return {127, "pipe failed"};
  pid_t pid = fork();
  if (pid == 0) {
    dup2(p[1], STDOUT_FILENO); dup2(p[1], STDERR_FILENO); close(p[0]); close(p[1]);
    std::vector<char*> av; for (const auto &s : args) av.push_back(const_cast<char*>(s.c_str())); av.push_back(nullptr);
    execvp(av[0], av.data()); _exit(127);
  }
  close(p[1]); std::string out; char buf[8192]; ssize_t n;
  while ((n = read(p[0], buf, sizeof(buf))) > 0) { if (out.size() + size_t(n) <= max_output) out.append(buf, size_t(n)); }
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
      written += size_t(w);
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

static bool valid_repo(const std::string &s) {
  if (s.empty() || s.size() > 64 || s[0] == '.' || s.find("..") != std::string::npos) return false;
  return std::all_of(s.begin(), s.end(), [](unsigned char c){ return std::isalnum(c) || c == '_' || c == '.' || c == '-'; });
}
static bool valid_revision(const std::string &s) {
  if (s.empty() || s.size() > 255 || s[0] == '-' || s.find("..") != std::string::npos || s.find("@{") != std::string::npos) return false;
  return std::all_of(s.begin(), s.end(), [](unsigned char c){ return std::isalnum(c) || std::strchr("_./-~^", c); });
}
static bool valid_path(const std::string &s) {
  if (s.size() > 2048 || (!s.empty() && s[0] == '/') || s.find("..") != std::string::npos || s.find('\0') != std::string::npos) return false;
  return true;
}
static std::string url_decode(const std::string &s) {
  std::string o; for (size_t i=0;i<s.size();++i) { if (s[i]=='%' && i+2<s.size()) { char h[3]={s[i+1],s[i+2],0}; char *e=nullptr; long v=strtol(h,&e,16); if (*e==0) {o+=char(v);i+=2;continue;} } o += s[i]=='+'?' ':s[i]; } return o;
}
static std::map<std::string,std::string> query(const std::string &s) {
  std::map<std::string,std::string> q; size_t p=0;
  while (p<s.size()) { size_t a=s.find('&',p), e=s.find('=',p); if (a==std::string::npos) a=s.size(); if(e==std::string::npos||e>a) q[url_decode(s.substr(p,a-p))]=""; else q[url_decode(s.substr(p,e-p))]=url_decode(s.substr(e+1,a-e-1)); p=a+1; }
  return q;
}
static std::string git_dir(const std::string &name) { return repo_root + "/" + name + ".git"; }
static bool is_repo(const std::string &name) {
  if (!valid_repo(name)) return false;
  auto r=run({"git","--git-dir="+git_dir(name),"rev-parse","--is-bare-repository"});
  return r.status==0 && r.out.rfind("true",0)==0;
}
static Response error(int status, const std::string &message) { return {status,"application/json; charset=utf-8","{\"error\":"+json(message)+"}"}; }
static std::vector<std::string> lines(const std::string &s) { std::vector<std::string> v; std::istringstream in(s); std::string x; while(std::getline(in,x)) v.push_back(x); return v; }

static std::string iso_now() {
  auto now = std::chrono::system_clock::now();
  std::time_t tt = std::chrono::system_clock::to_time_t(now);
  std::tm tm_buf{};
  localtime_r(&tt, &tm_buf);
  char buf[64];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", &tm_buf);
  return std::string(buf);
}

static std::string json_extract(const std::string &body, const std::string &key) {
  std::string needle = "\"" + key + "\"";
  size_t pos = body.find(needle);
  if (pos == std::string::npos) return "";
  pos += needle.size();
  while (pos < body.size() && (body[pos] == ' ' || body[pos] == ':' || body[pos] == '\t')) pos++;
  if (pos >= body.size()) return "";
  if (body[pos] == '"') {
    pos++;
    std::string val;
    while (pos < body.size()) {
      if (body[pos] == '\\' && pos + 1 < body.size()) {
        char c = body[++pos];
        if (c == 'n') val += '\n';
        else if (c == 't') val += '\t';
        else if (c == 'r') val += '\r';
        else if (c == '"') val += '"';
        else if (c == '\\') val += '\\';
        else val += c;
      } else if (body[pos] == '"') {
        break;
      } else {
        val += body[pos];
      }
      pos++;
    }
    return val;
  }
  size_t end = body.find_first_of(",}\r\n ", pos);
  if (end == std::string::npos) end = body.size();
  return body.substr(pos, end - pos);
}

static std::string repo_visibility(const std::string &name) {
  auto r = run({"git", "--git-dir=" + git_dir(name), "config", "--get", "sister.visibility"});
  if (r.status == 0) {
    std::string v = r.out;
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
    if (v == "public" || v == "private") return v;
  }
  return auth_token.empty() ? "public" : "private";
}

static std::string repo_type(const std::string &name) {
  auto r = run({"git", "--git-dir=" + git_dir(name), "config", "--get", "sister.type"});
  if (r.status == 0) {
    std::string v = r.out;
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
    if (v == "editorial" || v == "software") return v;
  }
  auto m = run({"git", "--git-dir=" + git_dir(name), "cat-file", "-e", "HEAD:.sister-editorial.json"});
  if (m.status == 0) return "editorial";
  return "software";
}

static Result git_commit_file(const std::string &repo_name, const std::string &filename, const std::string &content, const std::string &commit_msg, const std::string &author_name = "Pesquisador LABECO", const std::string &author_email = "pesquisador@labeco.org") {
  std::string gd = git_dir(repo_name);
  auto h = run_stdin({"git", "--git-dir=" + gd, "hash-object", "-w", "--stdin"}, content);
  if (h.status != 0) return {h.status, "Falha ao gravar objeto blob"};
  std::string blob_sha = h.out;
  while (!blob_sha.empty() && (blob_sha.back() == '\n' || blob_sha.back() == '\r' || blob_sha.back() == ' ')) blob_sha.pop_back();

  std::string idx = "/tmp/sister_idx_" + std::to_string(getpid()) + "_" + std::to_string(rand());
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

static Response repos(bool is_auth) {
  std::string body="{\"repositories\":["; bool first=true;
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(repo_root, ec)) {
    std::string file=entry.path().filename().string(); if(file.size()<5||file.substr(file.size()-4)!=".git") continue;
    std::string name=file.substr(0,file.size()-4); if(!is_repo(name)) continue;
    std::string vis = repo_visibility(name);
    if (!is_auth && vis == "private") continue;
    std::string type = repo_type(name);
    auto head=run({"git","--git-dir="+git_dir(name),"symbolic-ref","--short","HEAD"});
    auto counts=run({"git","--git-dir="+git_dir(name),"for-each-ref","--format=%(refname)","refs/heads","refs/tags"}); int branches=0,tags=0;
    for(auto &x:lines(counts.out)){if(x.rfind("refs/heads/",0)==0)branches++;else if(x.rfind("refs/tags/",0)==0)tags++;}
    auto last=run({"git","--git-dir="+git_dir(name),"log","-1","--format=%H%x1f%h%x1f%s%x1f%an%x1f%aI"}); auto f=lines(last.out); std::string log=last.status!=0||f.empty()?"":f[0];
    uintmax_t bytes=0; for(auto i=fs::recursive_directory_iterator(entry.path(),fs::directory_options::skip_permission_denied,ec);!ec&&i!=fs::recursive_directory_iterator();i.increment(ec)) if(i->is_regular_file(ec)) bytes+=i->file_size(ec);
    if(!first) body+=',';
    first=false; body+="{\"name\":"+json(name)+",\"visibility\":"+json(vis)+",\"type\":"+json(type)+",\"default_branch\":"+json(head.status==0?head.out.substr(0,head.out.find('\n')):"")+",\"branches\":"+std::to_string(branches)+",\"tags\":"+std::to_string(tags)+",\"size_bytes\":"+std::to_string(bytes)+",\"integrity\":\"not_checked\"";
    if(log.empty()) body+=",\"empty\":true,\"last_commit\":null}"; else { auto p=std::vector<std::string>{}; size_t b=0,a; while((a=log.find('\x1f',b))!=std::string::npos){p.push_back(log.substr(b,a-b));b=a+1;}p.push_back(log.substr(b)); body+=",\"empty\":false,\"last_commit\":{\"hash\":"+json(p[0])+",\"short_hash\":"+json(p[1])+",\"subject\":"+json(p[2])+",\"author\":"+json(p[3])+",\"date\":"+json(p[4])+"}}"; }
  }
  return {200,"application/json; charset=utf-8",body+"]}"};
}

static Response refs(const std::string &name) {
  auto r=run({"git","--git-dir="+git_dir(name),"for-each-ref","--sort=-creatordate","--format=%(refname)%09%(objectname)%09%(creatordate:iso-strict)","refs/heads","refs/tags"}); if(r.status)return error(500,"Falha ao ler referências");
  std::string b="{\"branches\":["; bool fb=true,ft=true; std::string tags;
  for(auto &x:lines(r.out)){size_t a=x.find('\t'),c=x.find('\t',a+1);if(a==std::string::npos||c==std::string::npos)continue;std::string ref=x.substr(0,a), item="{\"name\":"+json(ref.substr(ref.find('/',5)+1))+",\"hash\":"+json(x.substr(a+1,c-a-1))+",\"date\":"+json(x.substr(c+1))+"}";if(ref.rfind("refs/heads/",0)==0){if(!fb)b+=',';fb=false;b+=item;}else{if(!ft)tags+=',';ft=false;tags+=item;}}
  return {200,"application/json; charset=utf-8",b+"],\"tags\":["+tags+"]}"};
}
static Response commits(const std::string &name,const std::map<std::string,std::string>&q) {
  std::string ref=q.count("ref")?q.at("ref"):"HEAD"; if(!valid_revision(ref))return error(400,"Revisão inválida");
  auto r=run({"git","--git-dir="+git_dir(name),"log","-50","--format=%H%x1f%h%x1f%an%x1f%ae%x1f%aI%x1f%s",ref+"^{commit}"});if(r.status)return error(404,"Revisão inexistente ou repositório vazio");
  std::string b="{\"commits\":[";bool first=true;for(auto&x:lines(r.out)){std::vector<std::string>p;size_t s=0,a;while((a=x.find('\x1f',s))!=std::string::npos){p.push_back(x.substr(s,a-s));s=a+1;}p.push_back(x.substr(s));if(p.size()!=6)continue;if(!first)b+=',';first=false;b+="{\"hash\":"+json(p[0])+",\"short_hash\":"+json(p[1])+",\"author\":"+json(p[2])+",\"email\":"+json(p[3])+",\"date\":"+json(p[4])+",\"subject\":"+json(p[5])+"}";}return{200,"application/json; charset=utf-8",b+"]}"};
}
static Response tree(const std::string &name,const std::map<std::string,std::string>&q) {
  std::string ref=q.count("ref")?q.at("ref"):"HEAD",path=q.count("path")?q.at("path"):"";if(!valid_revision(ref)||!valid_path(path))return error(400,"Revisão ou caminho inválido");
  std::string spec=ref+"^{tree}"+(path.empty()?"":":"+path);auto r=run({"git","--git-dir="+git_dir(name),"ls-tree","-z",spec});if(r.status)return error(404,"Árvore não encontrada");
  std::string b="{\"path\":"+json(path)+",\"entries\":[";bool first=true;size_t p=0;while(p<r.out.size()){size_t z=r.out.find('\0',p);if(z==std::string::npos)break;std::string x=r.out.substr(p,z-p);size_t sp=x.find(' '),sp2=x.find(' ',sp+1),tab=x.find('\t',sp2+1);if(tab!=std::string::npos){if(!first)b+=',';first=false;b+="{\"mode\":"+json(x.substr(0,sp))+",\"type\":"+json(x.substr(sp+1,sp2-sp-1))+",\"hash\":"+json(x.substr(sp2+1,tab-sp2-1))+",\"name\":"+json(x.substr(tab+1))+"}";}p=z+1;}return{200,"application/json; charset=utf-8",b+"]}"};
}
static Response blob(const std::string &name,const std::map<std::string,std::string>&q) {
  std::string ref=q.count("ref")?q.at("ref"):"HEAD",path=q.count("path")?q.at("path"):"";if(!valid_revision(ref)||!valid_path(path)||path.empty())return error(400,"Revisão ou caminho inválido");auto r=run({"git","--git-dir="+git_dir(name),"show",ref+":"+path});if(r.status)return error(404,"Arquivo não encontrado");bool binary=r.out.find('\0')!=std::string::npos;if(binary)return{200,"application/json; charset=utf-8","{\"path\":"+json(path)+",\"binary\":true,\"size\":"+std::to_string(r.out.size())+"}"};return{200,"application/json; charset=utf-8","{\"path\":"+json(path)+",\"binary\":false,\"size\":"+std::to_string(r.out.size())+",\"content\":"+json(r.out)+"}"};
}
static Response compare(const std::string &name,const std::map<std::string,std::string>&q) {
  std::string base=q.count("base")?q.at("base"):"",head=q.count("head")?q.at("head"):"";if(!valid_revision(base)||!valid_revision(head))return error(400,"Revisões inválidas");auto r=run({"git","--git-dir="+git_dir(name),"diff","--no-color","--stat","--patch",base+".."+head});if(r.status)return error(404,"Comparação inválida");return{200,"application/json; charset=utf-8","{\"base\":"+json(base)+",\"head\":"+json(head)+",\"diff\":"+json(r.out)+"}"};
}

static Response editorial(const std::string &name) {
  auto r = run({"git", "--git-dir=" + git_dir(name), "show", "HEAD:.sister-editorial.json"});
  std::string manifest_content = r.status == 0 ? r.out : "";
  while (!manifest_content.empty() && (manifest_content.back() == '\n' || manifest_content.back() == '\r' || manifest_content.back() == ' ')) manifest_content.pop_back();

  auto tags_res = run({"git", "--git-dir=" + git_dir(name), "tag", "-l", "--sort=-creatordate", "--format=%(refname:short)%09%(objectname)%09%(creatordate:iso-strict)%09%(subject)", "ed/*", "v*"});
  std::string editions_json = "[";
  bool first_ed = true;
  for (const auto &line : lines(tags_res.out)) {
    size_t t1 = line.find('\t'), t2 = line.find('\t', t1 + 1), t3 = line.find('\t', t2 + 1);
    if (t1 == std::string::npos || t2 == std::string::npos) continue;
    std::string tag_name = line.substr(0, t1);
    std::string tag_hash = line.substr(t1 + 1, t2 - t1 - 1);
    std::string tag_date = t3 == std::string::npos ? line.substr(t2 + 1) : line.substr(t2 + 1, t3 - t2 - 1);
    std::string tag_subj = t3 == std::string::npos ? "" : line.substr(t3 + 1);
    if (!first_ed) editions_json += ',';
    first_ed = false;
    editions_json += "{\"tag\":" + json(tag_name) + ",\"hash\":" + json(tag_hash) + ",\"date\":" + json(tag_date) + ",\"subject\":" + json(tag_subj) + "}";
  }
  editions_json += "]";

  bool is_ed = repo_type(name) == "editorial";
  std::string out = "{\"name\":" + json(name) + ",\"is_editorial\":" + (is_ed ? "true" : "false");
  if (!manifest_content.empty() && manifest_content[0] == '{') {
    out += ",\"has_manifest\":true,\"manifest\":" + manifest_content;
  } else {
    out += ",\"has_manifest\":false,\"manifest\":null";
  }
  out += ",\"editions\":" + editions_json + "}";
  return {200, "application/json; charset=utf-8", out};
}

static Response create_repo(const std::string &name, const std::string &type, const std::string &visibility, const std::string &title, const std::string &author) {
  if (!valid_repo(name)) return error(400, "Nome de repositório inválido");
  if (fs::exists(git_dir(name))) return error(409, "Repositório já existe");

  auto init_r = run({"git", "init", "--bare", git_dir(name)});
  if (init_r.status != 0) return error(500, "Falha ao inicializar repositório: " + init_r.out);

  run({"git", "--git-dir=" + git_dir(name), "config", "sister.type", type == "editorial" ? "editorial" : "software"});
  run({"git", "--git-dir=" + git_dir(name), "config", "sister.visibility", visibility == "private" ? "private" : "public"});

  std::string doc_title = title.empty() ? name : title;
  std::string auth_name = author.empty() ? "Pesquisador LABECO" : author;

  if (type == "editorial") {
    std::string init_doc = "# " + doc_title + "\n\n"
                           "**" + auth_name + "**  \n"
                           "*Laboratório de Ecologia e Conservação (LABECO)*\n\n---\n\n"
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

    git_commit_file(name, "manuscrito.md", init_doc, "docs(editorial): initial manuscript draft", auth_name);
    git_commit_file(name, ".sister-editorial.json", init_manifest, "feat(editorial): initialize editorial manifest", auth_name);
  } else {
    std::string init_readme = "# " + name + "\n\nRepositório de software hospedado no SisTer Git.\n";
    git_commit_file(name, "README.md", init_readme, "feat: initial commit", auth_name);
  }

  return {200, "application/json; charset=utf-8", "{\"ok\":true,\"status\":\"ok\",\"name\":" + json(name) + ",\"type\":" + json(type) + "}"};
}

static Response static_file(std::string path) {
  if(path=="/") path="/index.html";
  if(path.find("..")!=std::string::npos) return error(400,"Caminho inválido");
  fs::path p=web_root+path;std::ifstream f(p,std::ios::binary);if(!f)return error(404,"Não encontrado");std::ostringstream s;s<<f.rdbuf();std::string ext=p.extension().string(),type="application/octet-stream";if(ext==".html")type="text/html; charset=utf-8";else if(ext==".css")type="text/css; charset=utf-8";else if(ext==".js")type="text/javascript; charset=utf-8";else if(ext==".svg")type="image/svg+xml";return{200,type,s.str()};
}
static Response route(const std::string &target, bool is_auth) {
  size_t qm=target.find('?');std::string path=target.substr(0,qm),qs=qm==std::string::npos?"":target.substr(qm+1);auto q=query(qs);
  if(path=="/api/v1/health")return{200,"application/json; charset=utf-8","{\"status\":\"ok\",\"read_only\":true}"};
  if(path=="/api/v1/auth/status")return{200,"application/json; charset=utf-8","{\"authenticated\":"+std::string(is_auth?"true":"false")+",\"auth_required\":"+std::string(auth_token.empty()?"false":"true")+"}"};
  if(path=="/api/v1/ha")return{200,"application/json; charset=utf-8","{\"nodes\":[{\"name\":\"LABECO-SRV\",\"role\":\"git-host\",\"status\":\"not_verified\"},{\"name\":\"Fedora Silverblue\",\"role\":\"planned-replica\",\"status\":\"not_verified\"}],\"replication\":\"not_verified\",\"last_sync\":null}"};
  if(path=="/api/v1/repos")return repos(is_auth);
  const std::string pre="/api/v1/repos/";if(path.rfind(pre,0)==0){std::string rest=path.substr(pre.size());size_t slash=rest.find('/');std::string name=url_decode(rest.substr(0,slash));if(!is_repo(name))return error(404,"Repositório não encontrado");std::string vis=repo_visibility(name);if(!is_auth&&vis=="private")return error(404,"Repositório não encontrado");std::string action=slash==std::string::npos?"":rest.substr(slash+1);if(action=="refs")return refs(name);if(action=="commits")return commits(name,q);if(action=="tree")return tree(name,q);if(action=="blob")return blob(name,q);if(action=="compare")return compare(name,q);if(action=="editorial")return editorial(name);return error(404,"Rota não encontrada");}
  return static_file(path);
}

static Response route_post(const std::string &target, const std::string &, const std::string &body, bool is_auth) {
  if (!is_auth) {
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
    return create_repo(name, type, vis, title, author);
  }

  const std::string pre = "/api/v1/repos/";
  if (target.rfind(pre, 0) == 0) {
    std::string rest = target.substr(pre.size());
    size_t slash = rest.find('/');
    std::string name = url_decode(rest.substr(0, slash));
    if (!is_repo(name)) return error(404, "Repositório não encontrado");
    std::string action = slash == std::string::npos ? "" : rest.substr(slash + 1);

    if (action == "editorial/save") {
      std::string filename = json_extract(body, "filename");
      std::string content = json_extract(body, "content");
      std::string message = json_extract(body, "message");
      std::string author = json_extract(body, "author");
      if (author.empty()) author = json_extract(body, "author_name");
      if (filename.empty()) filename = "manuscrito.md";
      if (message.empty()) message = "docs(editorial): update manuscript version";
      if (author.empty()) author = "Pesquisador LABECO";

      auto r = git_commit_file(name, filename, content, message, author);
      if (r.status != 0) return error(500, r.out);
      return {200, "application/json; charset=utf-8", "{\"ok\":true,\"status\":\"ok\",\"commit\":" + json(r.out) + ",\"short_hash\":" + json(r.out.substr(0, 7)) + "}"};
    }

    if (action == "editorial/review") {
      std::string reviewer = json_extract(body, "reviewer");
      std::string recommendation = json_extract(body, "recommendation");
      std::string summary = json_extract(body, "summary");
      if (summary.empty()) summary = json_extract(body, "comments");
      std::string resp_author = json_extract(body, "author_response");

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

      auto r = git_commit_file(name, ".sister-editorial.json", man_text, "editorial: register peer review " + rev_id + " (" + recommendation + ")");
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

      git_commit_file(name, ".sister-editorial.json", man_text, "editorial: publish edition " + tag);
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
  if (s == 404) return "Not Found";
  if (s == 405) return "Method Not Allowed";
  return "Internal Server Error";
}

static bool is_authorized(const std::string &headers) {
  if (auth_token.empty()) return true;
  std::string expected = "Authorization: Bearer " + auth_token;
  size_t pos = headers.find(expected);
  if (pos == std::string::npos) return false;
  size_t after = pos + expected.size();
  return after >= headers.size() || headers[after] == '\r' || headers[after] == '\n';
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

  bool auth = is_authorized(header_part);
  Response r;
  if (method == "GET") {
    r = route(target, auth);
  } else if (method == "POST") {
    r = route_post(target, header_part, body, auth);
  } else {
    r = error(405, "Método não permitido");
  }

  std::string h = "HTTP/1.1 " + std::to_string(r.status) + " " + reason(r.status) + "\r\n"
                  "Content-Type: " + r.type + "\r\n"
                  "Content-Length: " + std::to_string(r.body.size()) + "\r\n"
                  "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
                  "Content-Security-Policy: default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'\r\n"
                  "Connection: close\r\n\r\n";
  write(fd, h.data(), h.size());
  write(fd, r.body.data(), r.body.size());
  close(fd);
}

int main(int argc,char**argv){repo_root=getenv("SISTER_GIT_ROOT")?getenv("SISTER_GIT_ROOT"):"/srv/git";web_root=getenv("SISTER_GIT_WEB_ROOT")?getenv("SISTER_GIT_WEB_ROOT"):"web";auth_token=getenv("SISTER_GIT_WEB_TOKEN")?getenv("SISTER_GIT_WEB_TOKEN"):"";std::string bind="127.0.0.1";int port=8787;for(int i=1;i<argc;i++){std::string a=argv[i];if(a=="--bind"&&i+1<argc)bind=argv[++i];else if(a=="--port"&&i+1<argc)port=std::stoi(argv[++i]);else{std::cerr<<"Uso: sister-git-web [--bind IP] [--port PORT]\n";return 2;}}if(auth_token.empty()){std::ifstream tf(repo_root+"/.web_token");if(tf){std::getline(tf,auth_token);while(!auth_token.empty()&&(auth_token.back()=='\n'||auth_token.back()=='\r'||auth_token.back()==' '))auth_token.pop_back();}}if(bind!="127.0.0.1"&&bind!="::1"&&auth_token.empty()){std::cerr<<"[ERRO] SISTER_GIT_WEB_TOKEN obrigatório fora de loopback\n";return 1;}if(!fs::is_directory(repo_root)||!fs::is_directory(web_root)){std::cerr<<"[ERRO] Diretório de repositórios ou web ausente\n";return 1;}signal(SIGPIPE,SIG_IGN);int s=socket(AF_INET,SOCK_STREAM,0),one=1;setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(port);if(inet_pton(AF_INET,bind.c_str(),&addr.sin_addr)!=1||::bind(s,(sockaddr*)&addr,sizeof(addr))||listen(s,64)){std::cerr<<"[ERRO] bind/listen: "<<strerror(errno)<<"\n";return 1;}std::cout<<"[OK] SisTer Git Web em http://"<<bind<<":"<<port<<" (módulo editorial C++26 ativo)\n"<<std::flush;for(;;){int c=accept(s,nullptr,nullptr);if(c>=0)client(c);} }
