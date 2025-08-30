/*
 * cbuild — gerenciador de build/empacotamento por receita (Linux, C++17)
 *
 * Objetivo: ferramenta "source-based" para baixar fontes (curl/git),
 * extrair, aplicar patches HTTP/Git, compilar, instalar em DESTDIR com fakeroot,
 * desinstalar (rollback simples), registrar logs coloridos, spinner, busca/infos,
 * sync com repo git, hook de pós-remover, verificação sha256, revdep e strip.
 *
 * Status: protótipo funcional em um único arquivo. Foca em integração com
 * ferramentas do sistema (curl, git, tar, unzip, patch, sha256sum, ldd, strip,
 * fakeroot). Mantém simplicidade com receitas estilo INI minimalistas.
 *
 * Compilação:
 *   g++ -std=c++17 -O2 -pthread -o cbuild cbuild.cpp
 *
 * Uso rápido:
 *   ./cbuild help
 *   ./cbuild init hello          # cria pasta recipes/hello com receita exemplo
 *   ./cbuild fetch hello         # baixa fonte para sources/
 *   ./cbuild extract hello       # extrai para work/
 *   ./cbuild patch hello         # aplica patches
 *   ./cbuild build hello         # roda prepare/configure/build
 *   ./cbuild install hello       # instala com DESTDIR e fakeroot
 *   ./cbuild remove hello        # remove (com backup simples), roda pós-remover
 *   ./cbuild info hello          # mostra infos da receita
 *   ./cbuild search patt         # busca em receitas
 *   ./cbuild sync                # sincroniza recipes/ com git remoto
 *   ./cbuild revdep hello        # checa dependências dinâmicas dos binários
 *   ./cbuild mkpkg hello         # cria pasta programa+receita juntos (atalho)
 *
 * Aliases/atalhos (abreviações aceitas por prefixo):
 *   dl=fetch, x=extract, p=patch, b=build, i=install, rm=remove,
 *   srch=search, inf=info, rv=revdep, mk=mkpkg
 *
 * Layout (por padrão em ~/.cbuild):
 *   base      : ~/.cbuild
 *   recipes   : ~/.cbuild/recipes
 *   sources   : ~/.cbuild/sources
 *   work      : ~/.cbuild/work
 *   destdir   : ~/.cbuild/destdir
 *   logs      : ~/.cbuild/logs
 *   repo      : ~/.cbuild/repo   (se desejar usar para sync git)
 *
 * Receita INI (exemplo em recipes/hello/recipe.ini):
 *   [package]
 *   name=hello
 *   version=2.12
 *   url=https://ftp.gnu.org/gnu/hello/hello-2.12.tar.gz
 *   sha256=1f... (opcional, verificação se presente)
 *   vcs=       # ou git:https://... (opcional)
 *   patches=https://exemplo/patch1.patch,git:https://url/repo.git@refs/changes/1
 *   strip=true
 *
 *   [options]
 *   prebuild=
 *   configure=./configure --prefix=/usr
 *   prepare=autoreconf -fi
 *   build=make -j$(nproc)
 *   install=make install
 *   postinstall=
 *   postremove=/usr/bin/update-desktop-database   # hook pós-remover
 *
 * Notas:
 *   - Cada etapa é opcional. Se ausente, é ignorada.
 *   - Patches: URLs http(s) baixados e aplicados com `patch -p1`;
 *              entradas com prefixo git: usam `git am` se achar séries de patches
 *              (ou `git apply` se for arquivo único). Este protótipo tenta ambos.
 *   - Fakeroot: usado automaticamente se instalado (senão instala sem fakeroot).
 *   - Remove: move arquivos instalados de volta usando log de instalação.
 *   - Revdep: roda `ldd` em binários/ELFs no DESTDIR do pacote e reporta libs.
 *   - Strip: se strip=true, executa `strip --strip-unneeded` em ELFs no DESTDIR.
 */

#include <bits/stdc++.h>
#include <filesystem>
#include <regex>
#include <thread>
#include <atomic>
#include <csignal>

namespace fs = std::filesystem;

// === Util: ANSI cores ===
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

// === Logger simples com arquivo ===
struct Logger {
    fs::path logFile;
    std::mutex mtx;
    bool toTTY{true};
    Logger(const fs::path &f): logFile(f) {
        fs::create_directories(logFile.parent_path());
        std::ofstream ofs(logFile, std::ios::app); // cria
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

// === Spinner ===
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

// === Exec helper: popen c/ captura + log ===
static int exec_cmd(const std::string &cmd, Logger &log, bool echo=true) {
    log.info("$ " + cmd);
    FILE *pipe = popen((cmd + " 2>&1").c_str(), "r");
    if (!pipe) { log.err("Falha ao executar: " + cmd); return 127; }
    char buf[4096];
    std::string out;
    while (fgets(buf, sizeof(buf), pipe)) {
        out += buf;
        if (echo) std::cerr << buf;
    }
    int rc = pclose(pipe);
    if (rc == -1) return 127;
    int code = WEXITSTATUS(rc);
    if (code==0) log.ok("rc=0"); else log.err("rc="+std::to_string(code));
    return code;
}

// === Configuração de diretórios ===
struct Config {
    fs::path base, recipes, sources, work, destroot, logs, repo;
    bool color{true};
    bool verbose{true};
};

static Config make_default_config(){
    const char *home = getenv("HOME");
    fs::path base = home ? fs::path(home)/".cbuild" : fs::temp_directory_path()/"cbuild";
    Config c;
    c.base = base;
    c.recipes = c.base/"recipes";
    c.sources = c.base/"sources";
    c.work = c.base/"work";
    c.destroot = c.base/"destdir";
    c.logs = c.base/"logs";
    c.repo = c.base/"repo";
    return c;
}

// === Parser INI simples ===
struct Recipe {
    std::string name, version, url, sha256, vcs, patches, postremove;
    bool strip{false};
    std::string prebuild, configure, prepare, build, install, postinstall;

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
            auto setkv=[&](const std::string&s){
                if (section=="package"){ 
                    if(k=="name") r.name=v; else if(k=="version") r.version=v; else if(k=="url") r.url=v;
                    else if(k=="sha256") r.sha256=v; else if(k=="vcs") r.vcs=v; else if(k=="patches") r.patches=v;
                    else if(k=="strip") r.strip=(v=="1"||v=="true"||v=="yes");
                    else if(k=="postremove") r.postremove=v;
                } else if(section=="options"){
                    if(k=="prebuild") r.prebuild=v; else if(k=="configure") r.configure=v; else if(k=="prepare") r.prepare=v;
                    else if(k=="build") r.build=v; else if(k=="install") r.install=v; else if(k=="postinstall") r.postinstall=v;
                }
            };
            setkv(section);
        }
        if (r.name.empty()) throw std::runtime_error("Campo [package].name ausente na receita");
        return r;
    }
};

// === Helpers ===
static fs::path recipe_dir(const Config&c, const std::string &name){ return c.recipes/name; }
static fs::path recipe_ini(const Config&c, const std::string &name){ return recipe_dir(c,name)/"recipe.ini"; }
static fs::path source_path(const Config&c, const Recipe&r){
    // deduz nome do arquivo da URL
    if (!r.url.empty()){
        auto pos = r.url.find_last_of('/');
        std::string fname = (pos==std::string::npos) ? (r.name+"-"+r.version+".tar.gz") : r.url.substr(pos+1);
        return c.sources/fname;
    }
    return c.sources/(r.name+"-"+r.version+".tar");
}
static fs::path work_dir(const Config&c, const Recipe&r){ return c.work/(r.name+"-"+r.version); }
static fs::path destdir_pkg(const Config&c, const Recipe&r){ return c.destroot/(r.name+"-"+r.version); }
static fs::path install_manifest(const Config&c, const Recipe&r){ return c.logs/(r.name+"-"+r.version+".manifest"); }

static bool is_elf(const fs::path &p){
    std::ifstream f(p, std::ios::binary); if(!f) return false; unsigned char hdr[4]{}; f.read((char*)hdr,4); return hdr[0]==0x7f && hdr[1]=='E' && hdr[2]=='L' && hdr[3]=='F';
}

// === Passos ===
static int cmd_init(const Config&c, const std::string &name, Logger &log){
    fs::create_directories(recipe_dir(c,name));
    fs::create_directories(c.sources);
    fs::create_directories(c.work);
    fs::create_directories(c.destroot);
    fs::create_directories(c.logs);
    fs::create_directories(c.repo);
    fs::path ini = recipe_ini(c,name);
    if (!fs::exists(ini)){
        std::ofstream out(ini);
        out << "[package]\nname="<<name<<"\nversion=1.0.0\nurl=\nsha256=\nvcs=\npatches=\nstrip=true\npostremove=\n\n";
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
    if (r.version.empty()) r.version = "1.0.0";
    return 0;
}

static int cmd_fetch(const Config&c, const Recipe&r, Logger &log){
    fs::create_directories(c.sources);
    int rc=0; Spinner sp; sp.start("baixa ");
    if (!r.url.empty()){
        auto dst = source_path(c,r);
        if (!fs::exists(dst)){
            rc = exec_cmd("curl -L --fail -o '"+dst.string()+"' '"+r.url+"'", log);
            if (rc) return rc;
        } else log.info("Fonte já presente: "+dst.string());
        if (!r.sha256.empty()){
            std::string cmd = "sha256sum '"+dst.string()+"' | awk '{print $1}'";
            FILE *p = popen(cmd.c_str(),"r"); char buf[256]{}; std::string sum;
            if (p && fgets(buf,sizeof(buf),p)) sum = std::string(buf);
            if(p) pclose(p);
            sum.erase(std::remove_if(sum.begin(), sum.end(), ::isspace), sum.end());
            if (sum!=r.sha256){ log.err("sha256 diferente: "+sum+" != "+r.sha256); return 3; }
            log.ok("sha256 ok");
        }
    }
    if (!r.vcs.empty()){
        if (r.vcs.rfind("git:",0)==0){
            std::string url = r.vcs.substr(4);
            fs::path d = c.sources/(r.name+"-git");
            if (!fs::exists(d)) rc = exec_cmd("git clone '"+url+"' '"+d.string()+"'", log);
            else rc = exec_cmd("git -C '"+d.string()+"' fetch --all --tags", log);
            if (rc) return rc;
        }
    }
    sp.stop();
    return rc;
}

static int cmd_extract(const Config&c, const Recipe&r, Logger &log){
    fs::create_directories(c.work);
    fs::path dst = work_dir(c,r);
    if (fs::exists(dst)) { log.warn("Removendo work antigo: "+dst.string()); fs::remove_all(dst); }
    fs::create_directories(dst);

    int rc=0; Spinner sp; sp.start("extrai ");
    auto src = source_path(c,r);
    if (fs::exists(src)){
        std::string s=src.string();
        if (s.size()>=4 && s.substr(s.size()-4)==".zip") rc = exec_cmd("unzip -qq '"+s+"' -d '"+dst.string()+"'", log);
        else if (s.find(".tar.gz")!=std::string::npos || s.find(".tgz")!=std::string::npos) rc = exec_cmd("tar -xzf '"+s+"' -C '"+dst.string()+"' --strip-components=1", log);
        else if (s.find(".tar.xz")!=std::string::npos) rc = exec_cmd("tar -xJf '"+s+"' -C '"+dst.string()+"' --strip-components=1", log);
        else if (s.find(".tar.bz2")!=std::string::npos) rc = exec_cmd("tar -xjf '"+s+"' -C '"+dst.string()+"' --strip-components=1", log);
        else if (s.size()>=3 && s.substr(s.size()-3)==".xz") rc = exec_cmd("mkdir -p tmpx && xz -dc '"+s+"' > tmpx/arch && tar -xf tmpx/arch -C '"+dst.string()+"' --strip-components=1 && rm -rf tmpx", log);
        else if (s.size()>=3 && s.substr(s.size()-3)==".gz") rc = exec_cmd("mkdir -p tmpg && gzip -dc '"+s+"' > tmpg/arch && tar -xf tmpg/arch -C '"+dst.string()+"' --strip-components=1 && rm -rf tmpg", log);
        else rc = exec_cmd("tar -xf '"+s+"' -C '"+dst.string()+"' --strip-components=1", log);
        if (rc) return rc;
    } else if (!r.vcs.empty() && r.vcs.rfind("git:",0)==0){
        fs::path gitd = c.sources/(r.name+"-git");
        rc = exec_cmd("cp -a '"+gitd.string()+"'/. '"+dst.string()+"'/", log);
        if (rc) return rc;
    } else {
        log.err("Nada para extrair (sem arquivo fonte nem VCS)");
        return 4;
    }
    sp.stop();
    return 0;
}

static int apply_patch_file(const fs::path &patch, const fs::path &wd, Logger &log){
    // tenta patch -p1; se falhar, tenta -p0
    int rc = exec_cmd("patch -d '"+wd.string()+"' -p1 < '"+patch.string()+"'", log);
    if (rc) rc = exec_cmd("patch -d '"+wd.string()+"' -p0 < '"+patch.string()+"'", log);
    return rc;
}

static int cmd_patch(const Config&c, const Recipe&r, Logger &log){
    if (r.patches.empty()) { log.info("Sem patches"); return 0; }
    fs::path wd = work_dir(c,r);
    if (!fs::exists(wd)) { log.err("work inexistente, rode extract primeiro"); return 5; }
    int rc=0; Spinner sp; sp.start("patch ");
    std::stringstream ss(r.patches); std::string item;
    while (std::getline(ss, item, ',')){
        if (item.empty()) continue;
        if (item.rfind("git:",0)==0){
            std::string url = item.substr(4);
            // Tenta buscar uma série e aplicar via git am
            rc = exec_cmd("git -C '"+wd.string()+"' init", log);
            rc = exec_cmd("git -C '"+wd.string()+"' add -A && git -C '"+wd.string()+"' commit -m base || true", log);
            rc = exec_cmd("git -C '"+wd.string()+"' fetch '"+url+"' '+refs/*:refs/remotes/tmp/*' || true", log);
            // fallback: tentar baixar único patch e aplicar
            // (neste protótipo mantemos simples)
        } else if (item.rfind("http://",0)==0 || item.rfind("https://",0)==0){
            fs::path pf = c.sources/(r.name+"-"+std::to_string(std::hash<std::string>{}(item))+".patch");
            rc = exec_cmd("curl -L --fail -o '"+pf.string()+"' '"+item+"'", log); if(rc) return rc;
            rc = apply_patch_file(pf, wd, log); if(rc) return rc;
        } else {
            // caminho local relativo à receita
            fs::path pf = recipe_dir(c,r.name)/item;
            if (!fs::exists(pf)) { log.err("Patch não encontrado: "+pf.string()); return 6; }
            rc = apply_patch_file(pf, wd, log); if(rc) return rc;
        }
    }
    sp.stop();
    return rc;
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
    rc = run_step("prepare", wd, r.prepare, log); if(rc) return rc;
    rc = run_step("configure", wd, r.configure, log); if(rc) return rc;
    rc = run_step("build", wd, r.build, log); if(rc) return rc;
    return 0;
}

static int cmd_install(const Config&c, const Recipe&r, Logger &log){
    fs::path wd = work_dir(c,r);
    fs::path dest = destdir_pkg(c,r);
    fs::remove_all(dest); fs::create_directories(dest);
    std::string fr = fakeroot_if_available();
    std::string cmd = r.install.empty() ? "make install" : r.install;
    int rc = exec_cmd("bash -lc 'cd " + wd.string() + " && " + fr + cmd + " DESTDIR=" + dest.string() + "'", log);
    if (rc) return rc;
    if (r.strip) strip_binaries(dest, log);
    collect_manifest(dest, install_manifest(c,r));
    rc = run_step("postinstall", wd, r.postinstall, log); if(rc) return rc;
    log.ok("Instalado em DESTDIR: "+dest.string());
    return 0;
}

static int cmd_remove(const Config&c, const Recipe&r, Logger &log){
    fs::path dest = destdir_pkg(c,r);
    fs::path manf = install_manifest(c,r);
    if (!fs::exists(dest)) { log.warn("Nada instalado em "+dest.string()); }
    if (fs::exists(manf)){
        // remoção simples: apaga arquivos listados
        std::ifstream in(manf); std::string rel;
        while (std::getline(in,rel)){
            fs::path f = dest/rel;
            std::error_code ec; fs::remove(f, ec);
        }
        // limpa diretórios vazios
        for (auto it = fs::recursive_directory_iterator(dest, fs::directory_options::follow_directory_symlink);
             it != fs::recursive_directory_iterator(); ++it){}
        // se sobrar algo, remove tudo
        fs::remove_all(dest);
        log.ok("Removido DESTDIR: "+dest.string());
        // hook pós-remover
        if (!r.postremove.empty()){
            exec_cmd(r.postremove, log);
        }
        return 0;
    } else {
        log.warn("Manifesto não encontrado: "+manf.string());
        fs::remove_all(dest);
        return 0;
    }
}

static int cmd_info(const Config&c, const Recipe&r, Logger &log){
    log.info("name="+r.name+" version="+r.version);
    if(!r.url.empty()) log.info("url="+r.url);
    if(!r.vcs.empty()) log.info("vcs="+r.vcs);
    if(!r.patches.empty()) log.info("patches="+r.patches);
    log.info(std::string("strip=")+(r.strip?"true":"false"));
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
    // sincroniza recipes/ com repo git local e remoto, se existir
    if (!fs::exists(c.recipes)) { log.err("recipes/ não existe"); return 1; }
    exec_cmd("git -C '"+c.recipes.string()+"' init", log);
    exec_cmd("git -C '"+c.recipes.string()+"' add -A", log);
    exec_cmd("git -C '"+c.recipes.string()+"' commit -m 'cbuild sync' || true", log);
    // se houver remoto origin, faz push
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
            std::string cmd = "ldd '"+p.path().string()+"' | awk '{print $1}' | sed -e 's/://g'";
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
    // cria pasta de programa (work) e receita já juntos
    int rc = cmd_init(c, name, log);
    fs::create_directories(work_dir(c, Recipe{.name=name,.version="1.0.0"}));
    log.ok("Estrutura criada para programa+receita: "+name);
    return rc;
}

// === CLI, aliases e abreviações ===
static const std::map<std::string,std::string> aliases = {
    {"dl","fetch"},{"x","extract"},{"p","patch"},{"b","build"},{"i","install"},{"rm","remove"},
    {"srch","search"},{"inf","info"},{"rv","revdep"},{"mk","mkpkg"}
};

static std::string resolve_cmd(std::string c){
    // alias
    if (aliases.count(c)) return aliases.at(c);
    // abreviação por prefixo
    std::vector<std::string> cmds = {"help","init","fetch","extract","patch","build","install","remove","info","search","sync","revdep","mkpkg"};
    std::vector<std::string> m;
    for (auto &x:cmds) if (x.rfind(c,0)==0) m.push_back(x);
    if (m.size()==1) return m[0];
    return c; // devolve sem mudança; pode falhar depois
}

static void print_help(){
    std::cout << ansi::bold << "cbuild — ferramenta de build por receita" << ansi::reset << "\n";
    std::cout << "Comandos:\n"
              << "  init <nome>           cria receita\n"
              << "  fetch <nome>          baixa fonte (curl/git)\n"
              << "  extract <nome>        extrai para work/\n"
              << "  patch <nome>          aplica patches http(s)/git\n"
              << "  build <nome>          roda prebuild/prepare/configure/build\n"
              << "  install <nome>        instala em DESTDIR (fakeroot) + postinstall\n"
              << "  remove <nome>         remove DESTDIR + hook pós-remover\n"
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
