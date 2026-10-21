#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
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

static std::string repo_visibility(const std::string &name) {
  auto r = run({"git", "--git-dir=" + git_dir(name), "config", "--get", "sister.visibility"});
  if (r.status == 0) {
    std::string v = r.out;
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
    if (v == "public" || v == "private") return v;
  }
  return auth_token.empty() ? "public" : "private";
}

static Response repos(bool is_auth) {
  std::string body="{\"repositories\":["; bool first=true;
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(repo_root, ec)) {
    std::string file=entry.path().filename().string(); if(file.size()<5||file.substr(file.size()-4)!=".git") continue;
    std::string name=file.substr(0,file.size()-4); if(!is_repo(name)) continue;
    std::string vis = repo_visibility(name);
    if (!is_auth && vis == "private") continue;
    auto head=run({"git","--git-dir="+git_dir(name),"symbolic-ref","--short","HEAD"});
    auto counts=run({"git","--git-dir="+git_dir(name),"for-each-ref","--format=%(refname)","refs/heads","refs/tags"}); int branches=0,tags=0;
    for(auto &x:lines(counts.out)){if(x.rfind("refs/heads/",0)==0)branches++;else if(x.rfind("refs/tags/",0)==0)tags++;}
    auto last=run({"git","--git-dir="+git_dir(name),"log","-1","--format=%H%x1f%h%x1f%s%x1f%an%x1f%aI"}); auto f=lines(last.out); std::string log=last.status!=0||f.empty()?"":f[0];
    uintmax_t bytes=0; for(auto i=fs::recursive_directory_iterator(entry.path(),fs::directory_options::skip_permission_denied,ec);!ec&&i!=fs::recursive_directory_iterator();i.increment(ec)) if(i->is_regular_file(ec)) bytes+=i->file_size(ec);
    if(!first) body+=',';
    first=false; body+="{\"name\":"+json(name)+",\"visibility\":"+json(vis)+",\"default_branch\":"+json(head.status==0?head.out.substr(0,head.out.find('\n')):"")+",\"branches\":"+std::to_string(branches)+",\"tags\":"+std::to_string(tags)+",\"size_bytes\":"+std::to_string(bytes)+",\"integrity\":\"not_checked\"";
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
  const std::string pre="/api/v1/repos/";if(path.rfind(pre,0)==0){std::string rest=path.substr(pre.size());size_t slash=rest.find('/');std::string name=url_decode(rest.substr(0,slash));if(!is_repo(name))return error(404,"Repositório não encontrado");std::string vis=repo_visibility(name);if(!is_auth&&vis=="private")return error(404,"Repositório não encontrado");std::string action=slash==std::string::npos?"":rest.substr(slash+1);if(action=="refs")return refs(name);if(action=="commits")return commits(name,q);if(action=="tree")return tree(name,q);if(action=="blob")return blob(name,q);if(action=="compare")return compare(name,q);return error(404,"Rota não encontrada");}
  return static_file(path);
}
static std::string reason(int s){if(s==200)return"OK";if(s==400)return"Bad Request";if(s==401)return"Unauthorized";if(s==404)return"Not Found";if(s==405)return"Method Not Allowed";return"Internal Server Error";}
static bool is_authorized(const std::string &headers){if(auth_token.empty())return true;return headers.find("Authorization: Bearer "+auth_token+"\r\n")!=std::string::npos;}
static void client(int fd){std::string req;char b[4096];while(req.find("\r\n\r\n")==std::string::npos&&req.size()<65536){ssize_t n=read(fd,b,sizeof(b));if(n<=0)break;req.append(b,n);}std::istringstream in(req);std::string method,target,version;in>>method>>target>>version;Response r;if(method!="GET")r=error(405,"Somente GET é permitido");else{bool auth=is_authorized(req);r=route(target,auth);}std::string h="HTTP/1.1 "+std::to_string(r.status)+" "+reason(r.status)+"\r\nContent-Type: "+r.type+"\r\nContent-Length: "+std::to_string(r.body.size())+"\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nContent-Security-Policy: default-src 'self'; style-src 'self'; script-src 'self'; img-src 'self' data:; connect-src 'self'\r\nConnection: close\r\n\r\n";write(fd,h.data(),h.size());write(fd,r.body.data(),r.body.size());close(fd);}
int main(int argc,char**argv){repo_root=getenv("SISTER_GIT_ROOT")?getenv("SISTER_GIT_ROOT"):"/srv/git";web_root=getenv("SISTER_GIT_WEB_ROOT")?getenv("SISTER_GIT_WEB_ROOT"):"web";auth_token=getenv("SISTER_GIT_WEB_TOKEN")?getenv("SISTER_GIT_WEB_TOKEN"):"";std::string bind="127.0.0.1";int port=8787;for(int i=1;i<argc;i++){std::string a=argv[i];if(a=="--bind"&&i+1<argc)bind=argv[++i];else if(a=="--port"&&i+1<argc)port=std::stoi(argv[++i]);else{std::cerr<<"Uso: sister-git-web [--bind IP] [--port PORT]\n";return 2;}}if(auth_token.empty()){std::ifstream tf(repo_root+"/.web_token");if(tf){std::getline(tf,auth_token);while(!auth_token.empty()&&(auth_token.back()=='\n'||auth_token.back()=='\r'||auth_token.back()==' '))auth_token.pop_back();}}if(bind!="127.0.0.1"&&bind!="::1"&&auth_token.empty()){std::cerr<<"[ERRO] SISTER_GIT_WEB_TOKEN obrigatório fora de loopback\n";return 1;}if(!fs::is_directory(repo_root)||!fs::is_directory(web_root)){std::cerr<<"[ERRO] Diretório de repositórios ou web ausente\n";return 1;}signal(SIGPIPE,SIG_IGN);int s=socket(AF_INET,SOCK_STREAM,0),one=1;setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(port);if(inet_pton(AF_INET,bind.c_str(),&addr.sin_addr)!=1||::bind(s,(sockaddr*)&addr,sizeof(addr))||listen(s,64)){std::cerr<<"[ERRO] bind/listen: "<<strerror(errno)<<"\n";return 1;}std::cout<<"[OK] SisTer Git Web em http://"<<bind<<":"<<port<<" (somente leitura)\n"<<std::flush;for(;;){int c=accept(s,nullptr,nullptr);if(c>=0)client(c);} }
