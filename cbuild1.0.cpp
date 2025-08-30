// cbuild — gerenciador source-based (Linux, C++17)
// Evoluções: patches git robustos + diretório de patches, rollback em DESTDIR com snapshot,
// melhor tratamento de erros, múltiplos tarballs, git submódulos.
// Compilação: g++ -std=c++17 -O2 -pthread -o cbuild cbuild.cpp

#include <bits/stdc++.h>
#include <filesystem>
#include <regex>
#include <thread>
#include <atomic>
#include <csignal>

namespace fs = std::filesystem;

namespace ansi {
    const std::string reset = "\033[0m";
    const std::string bold = "\033[1m";
    const std::string dim = "\033[2m";
    const std::string red = "\033[31m";
    const std::string green = "\033[32m";
    const std::string yellow = "\033[33m";
    const std::string blue = "\033[34m";
    const std::string magenta = "\033[35m";
    const std::string cyan = "\033[36m";
}

struct Logger {
    fs::path logFile;
    std::mutex mtx;
    bool toTTY{true};
    Logger(const fs::path &f): logFile(f) {
        fs::create_directories(logFile.parent_path());
        std::ofstream ofs(logFile, std::ios::app);
    }
    void write(const std::string &level, const std::string &msg, const std::string &color="") {
        std::lock_guard<std::mutex> lock(mtx);
        std::string line = level + ": " + msg + "\n";
        std::ofstream ofs(logFile, std::ios::app);
        ofs << line;
        ofs.close();
        if (toTTY) {
            if (!color.empty()) std::cerr << color;
            std::cerr << line << ansi::reset;
        }
    }
    void info(const std::string &m){ write("[INFO]", m, ansi::cyan); }
    void ok(const std::string &m){ write("[ OK ]", m, ansi::green); }
    void warn(const std::string &m){ write("[WARN]", m, ansi::yellow); }
    void err(const std::string &m){ write("[ERR ]", m, ansi::red); }
};

class Spinner {
    std::atomic<bool> running{false};
    std::thread th;
public:
    void start(const std::string &prefix="") {
        running = true;
        th = std::thread([this, prefix]{
            const char frames[] = {'|','/','-','\\'};
            size_t i=0;
            while (running) {
                std::cerr << "\r" << ansi::dim << prefix << frames[i++%4] << ansi::reset << std::flush;
                std::this_thread::sleep_for(std::chrono::milliseconds(80));
            }
            std::cerr << "\r" << std::string(prefix.size()+1, ' ') << "\r";
        });
    }
    void stop(){ running=false; if (th.joinable()) th.join(); }
};

// exec helpers
static int exec_cmd(const std::string &cmd, Logger &log, bool echo=true) {
    log.info("$ " + cmd);
    FILE *pipe = popen((cmd + " 2>&1").c_str(), "r");
    if (!pipe) { log.err("Falha ao executar: " + cmd); return 127; }
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) {
        if (echo) std::cerr << buf;
    }
    int rc = pclose(pipe);
    if (rc == -1) return 127;
    int code = WEXITSTATUS(rc);
    if (code==0) log.ok("rc=0"); else log.err("rc="+std::to_string(code));
    return code;
}

static void exec_cmd_strict(const std::string &cmd, Logger &log, const std::string &ctx="") {
    int rc = exec_cmd(cmd, log);
    if (rc != 0) {
        std::string where = ctx.empty()? cmd : ctx;
        throw std::runtime_error("Falha em: " + where + " (rc="+std::to_string(rc)+")");
    }
}

struct Config {
    fs::path base, recipes, sources, work, destroot, logs, repo, snapshots;
    bool color{true};
    bool verbose{true};
};

static Config make_default_config(){
    const char *home = getenv("HOME");
    fs::path base = home ? fs::path(home)/".cbuild" : fs::temp_directory_path()/ "cbuild";
    Config c;
    c.base = base;
    c.recipes = c.base/"recipes";
    c.sources = c.base/"sources";
    c.work = c.base/"work";
    c.destroot = c.base/"destdir";
    c.logs = c.base/"logs";
    c.repo = c.base/"repo";
    c.snapshots = c.base/"snapshots";
    return c;
}

// checagem de dependências
static std::vector<std::string> required_tools = {
    "curl","git","tar","patch","sha256sum","ldd","strip","unzip","xz","gzip"
};
static void check_tools(Logger &log, bool strict=true){
    for (auto &t: required_tools){
        std::string cmd = "command -v " + t + " >/dev/null 2>&1";
        int rc = system(cmd.c_str());
        if (rc!=0) {
            std::string msg = "Ferramenta ausente: " + t;
            if (strict) throw std::runtime_error(msg);
            else log.warn(msg);
        }
    }
}

struct Recipe {
    std::string name, version, url, sha256, vcs, patches, postremove;
    bool strip{false};
    bool submodules{false};
    std::string prebuild, configure, prepare, build, install, postinstall;

    // suporta listas em url e sha256 (separadas por vírgula)
    static std::vector<std::string> split_list(const std::string &s){
        std::vector<std::string> out;
        std::stringstream ss(s);
        std::string item;
        while (std::getline(ss,item,',')){
            // trim
            auto t=item;
            t.erase(0, t.find_first_not_of(" \t\r\n"));
            t.erase(t.find_last_not_of(" \t\r\n")+1);
            if(!t.empty()) out.push_back(t);
        }
        return out;
    }

    static Recipe load(const fs::path &file){
        Recipe r;
        std::ifstream in(file);
        if (!in) throw std::runtime_error("Não foi possível abrir receita: "+file.string());
        std::string line, section;
        auto trim=[](std::string s){
            s.erase(0, s.find_first_not_of(" \t\r\n"));
            s.erase(s.find_last_not_of(" \t\r\n")+1);
            return s;
        };
        while (std::getline(in,line)){
            if (line.size()>0 && (line[0]=='#' || line[0]==';')) continue;
            if (std::regex_match(line, std::regex("\\s*\\[.*\\]\\s*"))){
                section = trim(line.substr(line.find('[')+1, line.rfind(']')-line.find('[')-1));
                continue;
            }
            auto pos=line.find('=');
            if (pos==std::string::npos) continue;
            std::string k=trim(line.substr(0,pos));
            std::string v=trim(line.substr(pos+1));
            if (section=="package"){
                if(k=="name") r.name=v; else if(k=="version") r.version=v; else if(k=="url") r.url=v;
                else if(k=="sha256") r.sha256=v; else if(k=="vcs") r.vcs=v; else if(k=="patches") r.patches=v;
                else if(k=="strip") r.strip=(v=="1"||v=="true"||v=="yes");
                else if(k=="postremove") r.postremove=v;
                else if(k=="submodules") r.submodules=(v=="1"||v=="true"||v=="yes");
            } else if (section=="options"){
                if(k=="prebuild") r.prebuild=v; else if(k=="configure") r.configure=v; else if(k=="prepare") r.prepare=v;
                else if(k=="build") r.build=v; else if(k=="install") r.install=v; else if(k=="postinstall") r.postinstall=v;
            }
        }
        if (r.name.empty()) throw std::runtime_error("Campo [package].name ausente na receita");
        if (r.version.empty()) r.version = "1.0.0";
        return r;
    }
};

static fs::path recipe_dir(const Config&c, const std::string &name){ return c.recipes/name; }
static fs::path recipe_ini(const Config&c, const std::string &name){ return recipe_dir(c,name)/"recipe.ini"; }

// múltiplos arquivos fonte
static std::vector<fs::path> source_paths(const Config&c, const Recipe&r){
    std::vector<fs::path> paths;
    auto urls = Recipe::split_list(r.url);
    if (urls.empty()) {
        // se não há url mas há vcs, devolve provável tar fictício
        if(!r.vcs.empty()) return {};
        paths.push_back(c.sources/(r.name+"-"+r.version+".tar"));
        return paths;
    }
    for (auto &u: urls){
        auto pos = u.find_last_of('/');
        std::string fname = (pos==std::string::npos) ? (r.name+"-"+r.version+".tar.gz") : u.substr(pos+1);
        paths.push_back(c.sources/fname);
    }
    return paths;
}
static fs::path work_dir(const Config&c, const Recipe&r){ return c.work/(r.name+"-"+r.version); }
static fs::path destdir_pkg(const Config&c, const Recipe&r){ return c.destroot/(r.name+"-"+r.version); }
static fs::path install_manifest(const Config&c, const Recipe&r){ return c.logs/(r.name+"-"+r.version+".manifest"); }
static fs::path snapshot_tar(const Config&c, const Recipe&r){ return c.snapshots/(r.name+"-"+r.version+".tar.zst"); }

static bool is_elf(const fs::path &p){
    std::ifstream f(p, std::ios::binary); if(!f) return false; unsigned char hdr[4]{}; f.read((char*)hdr,4); return hdr[0]==0x7f && hdr[1]=='E' && hdr[2]=='L' && hdr[3]=='F';
}

// criar estrutura inicial de receita
static int cmd_init(const Config&c, const std::string &name, Logger &log){
    fs::create_directories(recipe_dir(c,name));
    fs::create_directories(c.sources);
    fs::create_directories(c.work);
    fs::create_directories(c.destroot);
    fs::create_directories(c.logs);
    fs::create_directories(c.repo);
    fs::create_directories(c.snapshots);
    fs::path ini = recipe_ini(c,name);
    if (!fs::exists(ini)){
        std::ofstream out(ini);
        out << "[package]\nname="<<name<<"\nversion=1.0.0\nurl=\nsha256=\nvcs=\npatches=\nstrip=true\npostremove=\nsubmodules=false\n\n";
        out << "[options]\nprebuild=\nconfigure=\nprepare=\nbuild=\ninstall=\npostinstall=\n";
        out.close();
        log.ok("Receita criada: "+ini.string());
    } else log.warn("Receita já existe: "+ini.string());
    return 0;
}

static int ensure_recipe(const Config&c, const std::string &name, Recipe &r, Logger &log){
    fs::path ini = recipe_ini(c,name);
    if(!fs::exists(ini)) { log.err("Receita não encontrada: "+ini.string()); return 2; }
    r = Recipe::load(ini);
    return 0;
}

// sha256 de um arquivo
static std::string sha256_file(const fs::path &p){
    std::string cmd = "sha256sum '" + p.string() + "' | awk '{print $1}'";
    FILE *f = popen(cmd.c_str(),"r");
    if (!f) return "";
    char buf[128]{}; std::string sum;
    if (fgets(buf,sizeof(buf),f)) sum = buf;
    pclose(f);
    sum.erase(std::remove_if(sum.begin(), sum.end(), ::isspace), sum.end());
    return sum;
}

static int cmd_fetch(const Config&c, const Recipe&r, Logger &log){
    check_tools(log, true);
    fs::create_directories(c.sources);
    int rc=0; Spinner sp; sp.start("baixa ");

    // múltiplos tarballs
    auto urls = Recipe::split_list(r.url);
    auto sums = Recipe::split_list(r.sha256);
    for (size_t i=0;i<urls.size();++i){
        fs::path dst = source_paths(c,r).at(i);
        if (!fs::exists(dst)){
            rc = exec_cmd("curl -L --fail -o '" + dst.string() + "' '" + urls[i] + "'", log);
            if (rc) return rc;
        } else log.info("Fonte já presente: "+dst.string());
        if (i < sums.size() && !sums[i].empty()){
            auto got = sha256_file(dst);
            if (got!=sums[i]){ log.err("sha256 diferente: "+got+" != "+sums[i]); return 3; }
            log.ok("sha256 ok: "+dst.filename().string());
        }
    }

    // VCS git opcional (fonte vivo)
    if (!r.vcs.empty() && r.vcs.rfind("git:",0)==0){
        std::string url = r.vcs.substr(4);
        fs::path d = c.sources/(r.name+"-git");
        if (!fs::exists(d)) rc = exec_cmd("git clone '"+url+"' '"+d.string()+"'", log);
        else {
            rc = exec_cmd("git -C '"+d.string()+"' fetch --all --tags", log);
            if (r.submodules) exec_cmd("git -C '"+d.string()+"' submodule update --init --recursive", log);
        }
        if (rc) return rc;
    }

    sp.stop();
    return rc;
}

static int extract_one(const fs::path &src, const fs::path &dst, Logger &log){
    std::string s = src.string();
    if (s.size()>=4 && s.substr(s.size()-4)==".zip") return exec_cmd("unzip -qq '"+s+"' -d '"+dst.string()+"'", log);
    if (s.find(".tar.gz")!=std::string::npos || s.find(".tgz")!=std::string::npos) return exec_cmd("tar -xzf '"+s+"' -C '"+dst.string()+"' --strip-components=1", log);
    if (s.find(".tar.xz")!=std::string::npos) return exec_cmd("tar -xJf '"+s+"' -C '"+dst.string()+"' --strip-components=1", log);
    if (s.find(".tar.bz2")!=std::string::npos) return exec_cmd("tar -xjf '"+s+"' -C '"+dst.string()+"' --strip-components=1", log);
    if (s.size()>=3 && s.substr(s.size()-3)==".xz") return exec_cmd("mkdir -p '"+(dst.string()+"/.tmpx")+"' && xz -dc '"+s+"' > '"+(dst.string()+"/.tmpx/arch")+"' && tar -xf '"+(dst.string()+"/.tmpx/arch")+"' -C '"+dst.string()+"' --strip-components=1 && rm -rf '"+(dst.string()+"/.tmpx")+"'", log);
    if (s.size()>=3 && s.substr(s.size()-3)==".gz") return exec_cmd("mkdir -p '"+(dst.string()+"/.tmpg")+"' && gzip -dc '"+s+"' > '"+(dst.string()+"/.tmpg/arch")+"' && tar -xf '"+(dst.string()+"/.tmpg/arch")+"' -C '"+dst.string()+"' --strip-components=1 && rm -rf '"+(dst.string()+"/.tmpg")+"'", log);
    return exec_cmd("tar -xf '"+s+"' -C '"+dst.string()+"' --strip-components=1", log);
}

static int cmd_extract(const Config&c, const Recipe&r, Logger &log){
    fs::create_directories(c.work);
    fs::path dst = work_dir(c,r);
    if (fs::exists(dst)) { log.warn("Removendo work antigo: "+dst.string()); fs::remove_all(dst); }
    fs::create_directories(dst);

    int rc=0; Spinner sp; sp.start("extrai ");
    auto srcs = source_paths(c,r);
    if (!srcs.empty()){
        for (auto &src: srcs){
            if (!fs::exists(src)) { log.err("Fonte não encontrada: "+src.string()); return 4; }
            rc = extract_one(src, dst, log); if (rc) return rc;
        }
    } else if (!r.vcs.empty() && r.vcs.rfind("git:",0)==0){
        fs::path gitd = c.sources/(r.name+"-git");
        rc = exec_cmd("cp -a '"+gitd.string()+"'/. '"+dst.string()+"/'", log);
        if (rc) return rc;
        if (r.submodules) exec_cmd("git -C '"+dst.string()+"' submodule update --init --recursive", log);
    } else {
        log.err("Nada para extrair (sem arquivo fonte nem VCS)");
        return 4;
    }
    sp.stop();
    return 0;
}

// tornar a árvore work um repositório git com commit base (necessário para cherry-pick/am)
static void ensure_git_repo(const fs::path &wd, Logger &log){
    if (!fs::exists(wd/".git")){
        exec_cmd_strict("git -C '"+wd.string()+"' init", log, "git init");
        exec_cmd("git -C '"+wd.string()+"' add -A", log);
        exec_cmd("git -C '"+wd.string()+"' -c user.email=cbuild@local -c user.name=cbuild commit -m base || true", log);
    }
}

static int apply_patch_file(const fs::path &patch, const fs::path &wd, Logger &log){
    // tenta git am, se falhar tenta patch -p1 e -p0
    int rc = exec_cmd("bash -lc 'cd "+wd.string()+" && git am --3way --keep-cr "+patch.string()+"'", log);
    if (rc==0) return 0;
    rc = exec_cmd("patch -d '"+wd.string()+"' -p1 < '"+patch.string()+"'", log);
    if (rc) rc = exec_cmd("patch -d '"+wd.string()+"' -p0 < '"+patch.string()+"'", log);
    return rc;
}

static bool is_url(const std::string &s){ return s.rfind("http://",0)==0 || s.rfind("https://",0)==0; }
static bool is_git(const std::string &s){ return s.rfind("git:",0)==0; }

static int apply_patches_from_dir(const fs::path &dir, const fs::path &wd, Logger &log){
    std::vector<fs::path> files;
    for (auto &e: fs::directory_iterator(dir)){
        if (!fs::is_regular_file(e)) continue;
        auto ext = e.path().extension().string();
        if (ext==".patch" || ext==".diff" || ext==".mbox") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    for (auto &p: files){
        int rc = apply_patch_file(p, wd, log);
        if (rc) return rc;
    }
    return 0;
}

static int cherry_pick_ref(const std::string &repo_url, const std::string &refspec, const fs::path &wd, Logger &log){
    // busca ref temporária e cherry-pick (suporta A..B)
    std::string tmpref = "refs/tmp/cbuild";
    exec_cmd_strict("git -C '"+wd.string()+"' fetch '"+repo_url+"' '"+refspec+":"+tmpref+"'", log, "git fetch "+refspec);
    int rc = exec_cmd("bash -lc 'cd "+wd.string()+" && git cherry-pick -x "+tmpref+"'", log);
    // limpa tmp
    exec_cmd("git -C '"+wd.string()+"' update-ref -d "+tmpref, log);
    return rc;
}

static int cmd_patch(const Config&c, const Recipe&r, Logger &log){
    if (r.patches.empty()) { log.info("Sem patches"); return 0; }
    fs::path wd = work_dir(c,r);
    if (!fs::exists(wd)) { log.err("work inexistente, rode extract primeiro"); return 5; }
    ensure_git_repo(wd, log);

    int rc=0; Spinner sp; sp.start("patch ");
    std::stringstream ss(r.patches); std::string item;
    while (std::getline(ss, item, ',')){
        // trim
        auto t=item;
        t.erase(0, t.find_first_not_of(" \t\r\n"));
        t.erase(t.find_last_not_of(" \t\r\n")+1);
        if (t.empty()) continue;

        if (is_git(t)){
            // formato: git:https://repo.git@REF|COMMIT|A..B
            auto rest = t.substr(4);
            auto at = rest.find('@');
            if (at==std::string::npos){ log.err("patch git sem @ref: "+t); return 6; }
            std::string url = rest.substr(0,at);
            std::string ref = rest.substr(at+1);
            rc = cherry_pick_ref(url, ref, wd, log);
            if (rc) return rc;
        } else if (is_url(t)){
            // baixa e aplica (git am -> fallback patch)
            fs::path pf = c.sources/(r.name+"-"+std::to_string(std::hash<std::string>{}(t))+".patch");
            rc = exec_cmd("curl -L --fail -o '"+pf.string()+"' '"+t+"'", log); if(rc) return rc;
            rc = apply_patch_file(pf, wd, log); if(rc) return rc;
        } else {
            fs::path p = t;
            if (p.is_relative()) p = recipe_dir(c,r.name)/p;
            if (!fs::exists(p)) { log.err("Patch não encontrado: "+p.string()); return 6; }
            if (fs::is_directory(p)){
                rc = apply_patches_from_dir(p, wd, log); if (rc) return rc;
            } else {
                rc = apply_patch_file(p, wd, log); if(rc) return rc;
            }
        }
    }
    sp.stop();
    return 0;
}

static int run_step(const std::string &label, const fs::path &wd, const std::string &cmd, Logger &log){
    if (cmd.empty()) { log.info(label+": (vazio)"); return 0; }
    return exec_cmd("bash -lc 'cd " + wd.string() + " && set -e; " + cmd + "'", log);
}
static std::string fakeroot_if_available(){
    int rc = system("command -v fakeroot >/dev/null 2>&1");
    return (rc==0) ? "fakeroot " : "";
}

static int collect_manifest(const fs::path &dest, const fs::path &manifest){
    std::ofstream out(manifest);
    if (!out) return 1;
    for (auto &p: fs::recursive_directory_iterator(dest)){
        if (fs::is_regular_file(p.path())) out << fs::relative(p.path(), dest).string() << "\n";
    }
    return 0;
}

static int strip_binaries(const fs::path &dest, Logger &log){
    int rc=0;
    for (auto &p: fs::recursive_directory_iterator(dest)){
        if (fs::is_regular_file(p.path()) && is_elf(p.path())){
            rc |= exec_cmd("strip --strip-unneeded '"+p.path().string()+"' || true", log);
        }
    }
    return rc;
}

static int cmd_build_all(const Config&c, const Recipe&r, Logger &log){
    fs::path wd = work_dir(c,r);
    if (!fs::exists(wd)) { log.err("work inexistente, rode extract/patch"); return 7; }
    int rc=0; rc = run_step("prebuild", wd, r.prebuild, log); if(rc) return rc;
    rc = run_step("prepare",  wd, r.prepare, log);  if(rc) return rc;
    rc = run_step("configure",wd, r.configure, log);if(rc) return rc;
    rc = run_step("build",    wd, r.build, log);    if(rc) return rc;
    return 0;
}

// snapshot de DESTDIR do pacote (rollback)
static void make_snapshot(const fs::path &dest, const fs::path &snap, Logger &log){
    fs::create_directories(snap.parent_path());
    // usa tar + zstd (se disponível) ou gzip
    int has_zstd = system("command -v zstd >/dev/null 2>&1");
    std::string comp = (has_zstd==0) ? " | zstd -cq > '"+snap.string()+"'" : " -czf '"+snap.string()+"'";
    if (fs::exists(dest) && !fs::is_empty(dest)){
        if (has_zstd==0) {
            exec_cmd_strict("bash -lc 'cd "+dest.string()+" && tar -cf - . "+comp+"'", log, "snapshot zstd");
        } else {
            auto gz = snap; gz.replace_extension(".tar.gz");
            exec_cmd_strict("bash -lc 'cd "+dest.string()+" && tar -czf "+gz.string()+" .'", log, "snapshot gzip");
        }
    }
}

static void restore_snapshot(const fs::path &dest, const fs::path &snap, Logger &log){
    if (!fs::exists(snap) && !(fs::exists(snap.parent_path()/(snap.filename().string().substr(0, snap.filename().string().size()-4)+".tar.gz")))) return;
    fs::remove_all(dest);
    fs::create_directories(dest);
    int has_zstd = system("command -v zstd >/dev/null 2>&1");
    if (has_zstd==0 && fs::exists(snap)){
        exec_cmd_strict("bash -lc 'cd "+dest.string()+" && zstd -dc < "+snap.string()+" | tar -xf -'", log, "restore zstd");
    } else {
        fs::path gz = snap; gz.replace_extension(".tar.gz");
        if (fs::exists(gz))
            exec_cmd_strict("bash -lc 'cd "+dest.string()+" && tar -xzf "+gz.string()+"'", log, "restore gzip");
    }
}

static std::string ensure_destdir_in_install(const std::string &cmd){
    // Se o comando não menciona DESTDIR=, adiciona no final
    if (cmd.find("DESTDIR=") != std::string::npos) return cmd;
    return cmd + " DESTDIR=${DESTDIR}";
}

static int cmd_install(const Config&c, const Recipe&r, Logger &log){
    fs::path wd = work_dir(c,r);
    fs::path dest = destdir_pkg(c,r);
    fs::remove_all(dest); fs::create_directories(dest);

    // snapshot antes de instalar (para rollback se falhar)
    fs::path snap = snapshot_tar(c,r);
    make_snapshot(dest, snap, log);

    std::string fr = fakeroot_if_available();
    std::string base = r.install.empty() ? "make install" : r.install;
    base = ensure_destdir_in_install(base);
    int rc = exec_cmd("bash -lc 'cd " + wd.string() + " && " + fr + " DESTDIR="+dest.string()+" " + base + "'", log);
    if (rc) {
        log.err("Instalação falhou — restaurando snapshot");
        restore_snapshot(dest, snap, log);
        return rc;
    }
    if (r.strip) strip_binaries(dest, log);
    collect_manifest(dest, install_manifest(c,r));
    rc = run_step("postinstall", wd, r.postinstall, log); if(rc) return rc;
    log.ok("Instalado em DESTDIR: "+dest.string());
    return 0;
}

static int cmd_remove(const Config&c, const Recipe&r, Logger &log){
    fs::path dest = destdir_pkg(c,r);
    fs::path manf = install_manifest(c,r);
    fs::path snap = snapshot_tar(c,r);

    // snapshot atual antes de remover
    make_snapshot(dest, snap, log);

    if (fs::exists(manf)){
        std::ifstream in(manf); std::string rel;
        while (std::getline(in,rel)){
            fs::path f = dest/rel;
            std::error_code ec; fs::remove(f, ec);
        }
        fs::remove_all(dest);
        log.ok("Removido DESTDIR: "+dest.string());
        if (!r.postremove.empty()){
            exec_cmd(r.postremove, log);
        }
        return 0;
    } else {
        log.warn("Manifesto não encontrado: "+manf.string()+", limpando DESTDIR");
        fs::remove_all(dest);
        if (!r.postremove.empty()){
            exec_cmd(r.postremove, log);
        }
        return 0;
    }
}

static int cmd_info(const Config&c, const Recipe&r, Logger &log){
    log.info("name="+r.name+" version="+r.version);
    if(!r.url.empty()) log.info("url="+r.url);
    if(!r.vcs.empty()) log.info("vcs="+r.vcs);
    if(!r.patches.empty()) log.info("patches="+r.patches);
    log.info(std::string("strip=")+(r.strip?"true":"false"));
    log.info(std::string("submodules=")+(r.submodules?"true":"false"));
    return 0;
}

static int cmd_search(const Config&c, const std::string &pattern, Logger &log){
    std::regex rx(pattern, std::regex::icase);
    for (auto &d: fs::directory_iterator(c.recipes)){
        if (!fs::is_directory(d)) continue;
        fs::path ini = d.path()/"recipe.ini";
        if (!fs::exists(ini)) continue;
        std::ifstream in(ini); std::string s((std::istreambuf_iterator<char>(in)),{});
        if (std::regex_search(s, rx)) std::cout << d.path().filename().string() << "\n";
    }
    return 0;
}

static int cmd_sync(const Config&c, Logger &log){
    if (!fs::exists(c.recipes)) { log.err("recipes/ não existe"); return 1; }
    exec_cmd("git -C '"+c.recipes.string()+"' init", log);
    exec_cmd("git -C '"+c.recipes.string()+"' add -A", log);
    exec_cmd("git -C '"+c.recipes.string()+"' -c user.email=cbuild@local -c user.name=cbuild commit -m 'cbuild sync' || true", log);
    int rc = system(("git -C '"+c.recipes.string()+"' remote get-url origin >/dev/null 2>&1").c_str());
    if (rc==0) exec_cmd("git -C '"+c.recipes.string()+"' push origin HEAD", log);
    return 0;
}

static int cmd_revdep(const Config&c, const Recipe&r, Logger &log){
    fs::path dest = destdir_pkg(c,r);
    if (!fs::exists(dest)) { log.err("DESTDIR inexistente: "+dest.string()); return 1; }
    std::map<std::string, std::set<std::string>> dep2bins;
    for (auto &p: fs::recursive_directory_iterator(dest)){
        if (fs::is_regular_file(p.path()) && is_elf(p.path())){
            std::string cmd = "ldd '" + p.path().string() + "' | awk '{print $1}' | sed -e 's/://g'";
            FILE *f = popen(cmd.c_str(),"r"); char buf[512];
            while (f && fgets(buf,sizeof(buf),f)){
                std::string lib(buf); lib.erase(std::remove_if(lib.begin(), lib.end(), ::isspace), lib.end());
                if (!lib.empty()) dep2bins[lib].insert(fs::relative(p.path(), dest).string());
            }
            if (f) pclose(f);
        }
    }
    for (auto &kv: dep2bins){
        std::cout << ansi::bold << kv.first << ansi::reset << " <- ";
        bool first=true; for (auto &b: kv.second){ if(!first) std::cout << ", "; std::cout << b; first=false; }
        std::cout << "\n";
    }
    return 0;
}

static int cmd_mkpkg(const Config&c, const std::string &name, Logger &log){
    int rc = cmd_init(c, name, log);
    Recipe dummy; dummy.name=name; dummy.version="1.0.0";
    fs::create_directories(work_dir(c, dummy));
    log.ok("Estrutura criada para programa+receita: "+name);
    return rc;
}

static const std::map<std::string,std::string> aliases = {
    {"dl","fetch"},{"x","extract"},{"p","patch"},{"b","build"},{"i","install"},{"rm","remove"},
    {"srch","search"},{"inf","info"},{"rv","revdep"},{"mk","mkpkg"}
};

static std::string resolve_cmd(std::string c){
    if (aliases.count(c)) return aliases.at(c);
    std::vector<std::string> cmds = {"help","init","fetch","extract","patch","build","install","remove","info","search","sync","revdep","mkpkg"};
    std::vector<std::string> m;
    for (auto &x:cmds) if (x.rfind(c,0)==0) m.push_back(x);
    if (m.size()==1) return m[0];
    return c;
}

static void print_help(){
    std::cout << ansi::bold << "cbuild — ferramenta de build por receita" << ansi::reset << "\n";
    std::cout << "Comandos:\n"
              << "  init <nome>           cria receita\n"
              << "  fetch <nome>          baixa fonte (curl/git) [suporta múltiplos tarballs]\n"
              << "  extract <nome>        extrai para work/\n"
              << "  patch <nome>          aplica patches http(s)/git/dir (git:@REF|A..B)\n"
              << "  build <nome>          roda prebuild/prepare/configure/build\n"
              << "  install <nome>        instala em DESTDIR (fakeroot) + postinstall [rollback]\n"
              << "  remove <nome>         remove DESTDIR + hook pós-remover [snapshot]\n"
              << "  info <nome>           mostra infos da receita\n"
              << "  search <regex>        busca em receitas\n"
              << "  sync                  commit/push recipes/ (se origin configurado)\n"
              << "  revdep <nome>         verifica libs usadas pelos binários\n"
              << "  mkpkg <nome>          cria pasta do programa + receita juntos\n"
              << "\nAliases: dl, x, p, b, i, rm, srch, inf, rv, mk\n";
}

int main(int argc, char **argv){
    std::ios::sync_with_stdio(false);
    Config cfg = make_default_config();
    fs::create_directories(cfg.base);

    std::string cmd = (argc>=2) ? argv[1] : "help";
    cmd = resolve_cmd(cmd);

    fs::path logpath = cfg.logs/"cbuild.log";
    Logger log(logpath);

    if (cmd=="help") { print_help(); return 0; }

    auto need_name = [&](int minArgc){ if (argc<minArgc) { std::cerr << "Uso: "<<argv[0]<<" "<<cmd<<" <nome>\n"; return false;} return true; };

    try{
        if (cmd=="init"){
            if (!need_name(3)) return 1; return cmd_init(cfg, argv[2], log);
        } else if (cmd=="fetch"){
            if (!need_name(3)) return 1; Recipe r; if(ensure_recipe(cfg, argv[2], r, log)) return 1; return cmd_fetch(cfg,r,log);
        } else if (cmd=="extract"){
            if (!need_name(3)) return 1; Recipe r; if(ensure_recipe(cfg, argv[2], r, log)) return 1; return cmd_extract(cfg,r,log);
        } else if (cmd=="patch"){
            if (!need_name(3)) return 1; Recipe r; if(ensure_recipe(cfg, argv[2], r, log)) return 1; return cmd_patch(cfg,r,log);
        } else if (cmd=="build"){
            if (!need_name(3)) return 1; Recipe r; if(ensure_recipe(cfg, argv[2], r, log)) return 1; return cmd_build_all(cfg,r,log);
        } else if (cmd=="install"){
            if (!need_name(3)) return 1; Recipe r; if(ensure_recipe(cfg, argv[2], r, log)) return 1; return cmd_install(cfg,r,log);
        } else if (cmd=="remove"){
            if (!need_name(3)) return 1; Recipe r; if(ensure_recipe(cfg, argv[2], r, log)) return 1; return cmd_remove(cfg,r,log);
        } else if (cmd=="info"){
            if (!need_name(3)) return 1; Recipe r; if(ensure_recipe(cfg, argv[2], r, log)) return 1; return cmd_info(cfg,r,log);
        } else if (cmd=="search"){
            if (!need_name(3)) return 1; return cmd_search(cfg, argv[2], log);
        } else if (cmd=="sync"){
            return cmd_sync(cfg, log);
        } else if (cmd=="revdep"){
            if (!need_name(3)) return 1; Recipe r; if(ensure_recipe(cfg, argv[2], r, log)) return 1; return cmd_revdep(cfg,r,log);
        } else if (cmd=="mkpkg"){
            if (!need_name(3)) return 1; return cmd_mkpkg(cfg, argv[2], log);
        } else {
            log.err("Comando desconhecido: "+cmd);
            print_help();
            return 2;
        }
    } catch (const std::exception &e){
        log.err(std::string("Exceção: ")+e.what());
        return 100;
    }
}
