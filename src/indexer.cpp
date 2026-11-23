#include "migratool/gateway.hpp"
#include "migratool/scale.hpp"
#include "migratool/duplicates.hpp"
#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <cctype>
#include <sstream>

namespace mt {
namespace {
std::string low(std::string s){for(char& c:s)c=static_cast<char>(tolower(static_cast<unsigned char>(c)));return s;}
std::string fid(int64_t n){return "F"+std::to_string(n);}
struct Identity{std::string id;int64_t modified{};uint64_t size{};};
Identity identity(const fs::path& p){
    auto h=CreateFileW(p.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(h==INVALID_HANDLE_VALUE)throw std::runtime_error("GetFileInformation: Win32 "+std::to_string(GetLastError()));
    BY_HANDLE_FILE_INFORMATION i{};auto ok=GetFileInformationByHandle(h,&i);CloseHandle(h);
    if(!ok)throw std::runtime_error("GetFileInformation falló");
    auto join=[](DWORD high,DWORD lowpart){return(static_cast<uint64_t>(high)<<32)|lowpart;};
    return {std::to_string(i.dwVolumeSerialNumber)+":"+std::to_string(join(i.nFileIndexHigh,i.nFileIndexLow))+":"+std::to_string(join(i.ftCreationTime.dwHighDateTime,i.ftCreationTime.dwLowDateTime)),
        static_cast<int64_t>(join(i.ftLastWriteTime.dwHighDateTime,i.ftLastWriteTime.dwLowDateTime)),join(i.nFileSizeHigh,i.nFileSizeLow)};
}
struct Classification {
    std::string category, lang;
    bool relevant = false;
    std::string content;
    bool truncated = false;
    bool source_code = false;
    bool blocked = false;
};

struct CodeDetection {
    bool source_only;
    std::set<std::string> extensions, filenames, adapters;

    explicit CodeDetection(const Json& config) : source_only(config.value("inventory_mode", "universal") == "source_only") {
        for (const auto& ext : config.value("code_extensions", Json::array())) extensions.insert(low(ext));
        for (const auto& name : config.value("code_filenames", Json::array())) filenames.insert(low(name));
        for (const auto& adapter : config.value("analyzers", Json::array()))
            for (const auto& ext : adapter.at("extensions")) adapters.insert(low(ext));
    }

    std::string key(bool include_all_code) const {
        return sha256(Json::array({1, include_all_code, source_only, extensions, filenames, adapters}).dump());
    }
};
bool protected_filename(const fs::path& file){auto name=low(path_text(file.filename())),ext=low(path_text(file.extension()));return name==".env"||name.starts_with(".env.")||name=="credentials.json"||name=="secrets.json"||name=="id_rsa"||name=="id_ed25519"||ext==".pem"||ext==".key"||ext==".pfx"||ext==".p12";}
struct CodeKind {
    Classification classification;
    std::string name, extension;
    bool build_script = false;
};
CodeKind classify_name(const fs::path& file,const CodeDetection& policy){
    CodeKind result;
    auto& c=result.classification;
    const auto& ext=result.extension=low(path_text(file.extension()));
    const auto& name=result.name=low(path_text(file.filename()));
    c.lang=language(file);c.category="unknown";
    static const std::set<std::string> source={".c",".h",".cc",".cpp",".cxx",".c++",".hpp",".hh",".hxx",".h++",".inl",".inc",".ipp",".tpp",".ixx",".cppm",".cs",".fs",".fsx",".vb",".py",".pyw",".pyi",".js",".mjs",".cjs",".jsx",".ts",".tsx",".mts",".cts",".java",".kt",".kts",".rs",".go",".swift",".m",".mm",".rb",".php",".lua",".dart",".ex",".exs",".erl",".hrl",".pas",".dpr",".cob",".cbl",".f90",".f",".asm",".s",".sql",".vue",".svelte",".r",".jl",".zig",".scala",".clj",".pl",".hs",".v",".sv",".vhd",".ml",".mli",".glsl",".hlsl",".wgsl",".cu",".cuh",".wat",".proto"};
    static const std::set<std::string> docs={".md",".txt",".rst",".adoc",".tex",".org"},scripts={".ps1",".psm1",".sh",".bash",".zsh",".fish",".bat",".cmd",".cmake",".vbs"},configs={".json",".yml",".yaml",".xml",".toml",".ini",".cfg",".csproj",".sln",".slnx",".vcxproj",".props",".targets"};
    result.build_script=name=="cmakelists.txt"||name=="makefile"||name=="gnumakefile"||name=="dockerfile"||name.starts_with("dockerfile.");
    if(source.contains(ext)){c.category="source";c.relevant=true;}
    if(docs.contains(ext))c.category="documentation";
    if(scripts.contains(ext)){c.category="script";c.relevant=true;}
    if(configs.contains(ext)||result.build_script||name==".gitignore")c.category="configuration";
    if(ext==".xaml"||ext==".html"||ext==".htm"||ext==".css"||ext==".scss"||ext==".less"||ext==".svg"||ext==".qrc"||ext==".rc"||ext==".qml")c.category="resource";
    const bool declared_code = policy.extensions.contains(ext) || policy.filenames.contains(name);
    if (declared_code || (c.category == "unknown" && policy.adapters.contains(ext))) {
        c.category = "source";
        c.relevant = true;
    }
    if (protected_filename(file)) {
        c.category = "protected_configuration";
        c.relevant = false;
        c.blocked = true;
    }
    return result;
}

Classification classify_prefix(CodeKind kind,const fs::path& relative,std::string_view head,bool include_all_code){
    auto& c=kind.classification;
    if(c.blocked)return c;
    bool external=false,generated=false,artifact=false,test=false;
    for(const auto& part:relative.parent_path()){
        auto v=low(path_text(part));
        if(v=="third_party"||v=="vendor"||v=="external"||v=="node_modules"||v==".venv"||v=="venv")external=true;
        if(v=="bin"||v=="obj"||v=="build"||v=="out"||v=="dist"||v=="target"||v=="__pycache__")artifact=true;
        if(v=="tests"||v=="test"||v=="specs")test=true;
    }
    const auto& name=kind.name;
    if(name.ends_with(".g.cs")||name.ends_with(".designer.cs")||name.ends_with(".g.i.cs")||name.ends_with(".min.js"))generated=true;
    bool utf16=head.starts_with("\xff\xfe")||head.starts_with("\xfe\xff");
    bool binary=!utf16&&(head.find('\0')!=std::string::npos||head.starts_with("MZ")||head.starts_with("\x7f" "ELF")||head.starts_with("PK\x03\x04")||head.starts_with("\x89PNG"));
    if(head.find("<auto-generated")!=std::string::npos||head.find("@generated")!=std::string::npos)generated=true;
    if((kind.extension.empty()||c.category=="unknown")&&(head.starts_with("#!")||head.starts_with("\xef\xbb\xbf#!"))){
        auto shebang=head.substr(0,head.find('\n'));c.category="script";c.relevant=true;
        if(shebang.find("python")!=std::string::npos)c.lang="Python";else if(shebang.find("node")!=std::string::npos)c.lang="JavaScript";else if(shebang.find("ruby")!=std::string::npos)c.lang="Ruby";else c.lang="Shell";
    }
    if(test&&c.category=="source")c.category="test";
    c.source_code = !binary && (c.category=="source"||c.category=="test"||c.category=="script"||c.category=="resource"||kind.build_script);
    c.blocked = binary;
    if(binary){c.category="binary";c.relevant=false;}
    if(artifact){c.category="build_artifact";c.relevant=false;}
    if(generated){c.category="generated";c.relevant=false;}
    if(external){c.category="external_dependency";c.relevant=false;}
    if(include_all_code&&c.source_code)c.relevant=true;
    if(head.find("PRIVATE KEY-----")!=std::string::npos){c.category="protected_configuration";c.relevant=false;c.source_code=false;c.blocked=true;return c;}
    return c;
}

Classification classify(const fs::path& file,const fs::path& base,const CodeDetection& policy,bool include_all_code,bool manually_code){
    auto kind=classify_name(file,policy);
    const auto& initial=kind.classification;
    if(initial.blocked||(policy.source_only&&!manually_code&&!kind.build_script&&
       (initial.category=="documentation"||initial.category=="configuration")))return initial;
    std::ifstream in(file,std::ios::binary);if(!in)throw std::runtime_error("No se pudo clasificar archivo");
    std::string head(8192,'\0');in.read(head.data(),static_cast<std::streamsize>(head.size()));head.resize(static_cast<size_t>(in.gcount()));
    auto c=classify_prefix(std::move(kind),file.lexically_relative(base),head,include_all_code);
    const bool utf16=head.starts_with("\xff\xfe")||head.starts_with("\xfe\xff");
    // Solo documentación y texto desconocido se indexan como contenido FTS; código conserva símbolos y rutas.
    if(!policy.source_only&&!c.blocked&&(c.category=="documentation"||c.category=="unknown"||c.category=="configuration")){
        constexpr size_t max_text=1024*1024;auto n=fs::file_size(file);c.truncated=n>max_text;std::string data(static_cast<size_t>(std::min<uintmax_t>(n,max_text)),'\0');in.clear();in.seekg(0);in.read(data.data(),static_cast<std::streamsize>(data.size()));
        if(utf16){std::wstring w;bool be=data.starts_with("\xfe\xff");for(size_t i=2;i+1<data.size();i+=2){auto a=static_cast<unsigned char>(data[i]),b=static_cast<unsigned char>(data[i+1]);w+=static_cast<wchar_t>(be?(a<<8)|b:(b<<8)|a);}try{c.content=utf8(w);}catch(...){c.content="";}}
        else {try{wide(data);c.content=data;}catch(...){c.content="";}}
    }
    return c;
}

struct ArchiveFile { fs::path path; std::string root, role; };

Json collect_zip_entries(Database& db,const std::vector<ArchiveFile>& archives,const CodeDetection& policy,
                         bool full,Log log,Cancel* cancel){
    Json cached=Json::object();
    if(!full){
        const auto rows=db.query("SELECT value FROM meta WHERE key='code_zip_cache'");
        if(!rows.empty())cached=Json::parse(rows[0].at("value").get<std::string>());
    }
    const auto settings=policy.key(false);
    const auto probe=[&](const fs::path& member,std::optional<std::string_view> prefix){
        auto kind=classify_name(member,policy);
        if(kind.classification.blocked||kind.extension==".zip")return false;
        if(prefix)return classify_prefix(std::move(kind),member,*prefix,false).source_code;
        const auto& category=kind.classification.category;
        return kind.build_script||(category!="documentation"&&category!="configuration");
    };
    Json current=Json::object();
    for(const auto& archive:archives){
        if(cancel&&cancel->requested)throw std::runtime_error("Indexación cancelada");
        const auto path=path_text(archive.path);
        Json record={{"entries",Json::array()},{"errors",Json::array()}};
        try{
            const auto before=identity(archive.path);
            const auto fingerprint=Json::array({before.id,before.size,before.modified,settings});
            const auto previous=cached.find(path);
            if(previous!=cached.end()&&previous->at("fingerprint")==fingerprint&&previous->at("errors").empty()){
                record=std::move(*previous);
            }else{
                if(log)log("Duplicados ZIP: "+path);
                record=zip_code_entries(archive.path,probe,cancel);
                const auto after=identity(archive.path);
                if(after.id!=before.id||after.size!=before.size||after.modified!=before.modified){
                    record["entries"]=Json::array();
                    throw std::runtime_error("El ZIP cambió durante la lectura");
                }
                record["fingerprint"]=fingerprint;
            }
        }catch(const std::exception& failure){
            if(cancel&&cancel->requested)throw;
            record["entries"]=Json::array();
            record["errors"].push_back({{"error",failure.what()}});
        }
        record["root"]=archive.root;
        record["role"]=archive.role;
        current[path]=std::move(record);
    }
    return current;
}
Json git_states(const Project& p,const fs::path& base){
    Json result={{"available",false},{"states",Json::object()}};
    try{
        auto logs=p.state_dir()/L"git"/wide(unique_id());
        auto prefix_proc=run_process({"git","-C",path_text(base),"rev-parse","--show-prefix"},base,"",p.state_dir()/L"git"/wide(unique_id()),30000);
        std::string prefix;if(prefix_proc["exit_code"]==0){prefix=read_text(fs::path(wide(prefix_proc["stdout"])));while(!prefix.empty()&&(prefix.back()=='\n'||prefix.back()=='\r'))prefix.pop_back();}
        auto proc=run_process({"git","-C",path_text(base),"status","--porcelain=v1","-z","--untracked-files=all","--ignored=matching","--","."},base,"",logs,30000);
        if(proc["exit_code"]!=0||proc["timed_out"]==true)return result;
        result["available"]=true;auto raw=read_text(fs::path(wide(proc["stdout"])));
        for(size_t i=0;i<raw.size();){auto end=raw.find('\0',i);if(end==std::string::npos)break;auto item=raw.substr(i,end-i);i=end+1;if(item.size()<4)continue;auto state=item.substr(0,2),path=item.substr(3);if(!prefix.empty()&&path.starts_with(prefix))path.erase(0,prefix.size());result["states"][low(path)]=state;
            if(state.find('R')!=std::string::npos||state.find('C')!=std::string::npos){end=raw.find('\0',i);if(end==std::string::npos)break;i=end+1;}}
    }catch(const std::exception& e){result["error"]=e.what();}
    return result;
}
std::string git_state(const Json& git,const std::string& rel){
    if(!git.value("available",false))return "UNKNOWN";auto path=low(rel);if(git["states"].contains(path))return git["states"][path];
    size_t pos=0;while((pos=path.find('/',pos))!=std::string::npos){auto dir=path.substr(0,++pos);if(git["states"].contains(dir))return git["states"][dir];}return "tracked_clean";
}
std::string build_state(const Project& p,const Json& root,const fs::path& path){
    if(!root.contains("compile_commands"))return "UNKNOWN";
    try{auto db=read_json(p.resolve(root["compile_commands"]));for(const auto& cmd:db){auto source=fs::path(wide(cmd.at("file")));if(!source.is_absolute())source=fs::path(wide(cmd.at("directory")))/source;if(fs::weakly_canonical(source)==path)return "IN_COMPILATION_DATABASE";}return "NOT_IN_COMPILATION_DATABASE";}catch(...){return "COMPILATION_DATABASE_ERROR";}
}
Json make_scan(Database& db,bool include_text=true){
    Json out={{"schema","migratool.scan/1"},{"at",now()},{"complete",true},{"roots",Json::array()},{"files",Json::array()},{"symbols",Json::array()},{"dependencies",Json::array()},{"diagnostics",Json::array()},{"exclusions",Json::array()}};
    for(const auto& r:db.query("SELECT * FROM roots ORDER BY id")){auto data=Json::parse(r["data"].get<std::string>());out["roots"].push_back(data);if(!data.value("complete",false))out["complete"]=false;for(const auto& d:data.value("diagnostics",Json::array()))out["diagnostics"].push_back(d);for(const auto& e:data.value("exclusions",Json::array()))out["exclusions"].push_back(e);}
    if(out["roots"].empty())out["complete"]=false;
    for(const auto& diag:db.query("SELECT value FROM meta WHERE key='analysis_diagnostics'")){
        auto parsed_diags=Json::parse(diag["value"].get<std::string>());
        if(parsed_diags.is_array()){
            for(const auto& d:parsed_diags){if(out["diagnostics"].size()>=1000)break;out["diagnostics"].push_back(d);}
        }
    }
    Json hashes=Json::object();
    const auto file_query=include_text?"SELECT * FROM files WHERE deleted=0 ORDER BY root,path":
        "SELECT id,root,path,absolute_path,sha256,analysis,relevant,category,language,git_state,build_state,json_remove(data,'$.text_content') AS data FROM files WHERE deleted=0 ORDER BY root,path";
    for(const auto& row:db.query(file_query)){
        auto f=Json::parse(row["data"].get<std::string>());f["id"]=fid(row["id"]);f["absolute_path"]=row["absolute_path"];f["path"]=row["path"];f["root"]=row["root"];f["sha256"]=row["sha256"];f["analysis"]=row["analysis"];f["relevant"]=row["relevant"]==1;f["category"]=row["category"];f["language"]=row["language"];f["git_state"]=row["git_state"];f["build_state"]=row["build_state"];
        if(!f.value("error","").empty())out["complete"]=false;
        out["files"].push_back(f);hashes[f["root"].get<std::string>()].push_back({f["path"],f["sha256"]});
        out["symbols"].push_back({{"id",f["id"]},{"file_id",f["id"]},{"root",f["root"]},{"file",f["path"]},{"absolute_path",f["absolute_path"]},{"name",path_text(fs::path(wide(f["path"])).filename())},{"qualified_name",f["path"]},{"signature",""},{"kind","file"},{"line",1},{"engine","inventory"}});
    }
    for(auto& r:out["roots"])r["fingerprint"]=sha256(hashes.value(r["id"].get<std::string>(),Json::array()).dump());
    for(const auto& sym:db.query("SELECT s.id,s.data,f.absolute_path,f.path,f.root FROM symbols s JOIN files f ON s.file_id=f.id WHERE s.active=1 AND f.deleted=0 ORDER BY s.id")){
        auto data=Json::parse(sym["data"].get<std::string>());data["id"]="S"+std::to_string(sym["id"].get<int64_t>());data["absolute_path"]=sym["absolute_path"];data["file"]=sym["path"];data["root"]=sym["root"];out["symbols"].push_back(data);
    }
    const auto dep_count=db.integer("SELECT COUNT(*) FROM dependencies d JOIN files f ON d.file_id=f.id WHERE f.deleted=0");
    out["dependencies_count"]=dep_count;
    if(dep_count<=50000){
        for(const auto& d:db.query("SELECT d.data FROM dependencies d JOIN files f ON d.file_id=f.id WHERE f.deleted=0 ORDER BY d.id"))out["dependencies"].push_back(Json::parse(d["data"].get<std::string>()));
    }else{
        out["dependencies_omitted_in_snapshot"]=true;
    }
    return out;
}
}
Json classify_inventory_file(const Project& p,const fs::path& file,const fs::path& base,bool all){
    auto c=classify(file,base,CodeDetection(p.config),all,false);
    return {{"category",c.category},{"language",c.lang},{"relevant",c.relevant},{"source_code",c.source_code},
            {"text_content",c.content},{"text_truncated",c.truncated},{"blocked_code",c.blocked}};
}
Json classify_inventory_name(const Project& p,const fs::path& file,const fs::path& base,bool all){
    auto c=classify_prefix(classify_name(file,CodeDetection(p.config)),file.lexically_relative(base),"",all);
    return {{"category",c.category},{"language",c.lang},{"relevant",c.relevant},{"source_code",c.source_code},
        {"text_content",""},{"text_truncated",false},{"blocked_code",c.blocked},{"present",true}};
}
bool probe_inventory_code(const Project& p,const fs::path& file,std::optional<std::string_view> prefix){
    auto kind=classify_name(file,CodeDetection(p.config));if(kind.classification.blocked)return false;
    if(!prefix)return kind.classification.relevant||kind.classification.category=="resource"||kind.build_script||file.extension().empty();
    return classify_prefix(std::move(kind),file,*prefix,true).source_code;
}
Json make_compact_scan(Database& db,const Project& p){
    Json out={{"schema","migratool.scan/1"},{"at",now()},{"complete",true},{"normalized_in_sqlite",true},
              {"roots",Json::array()},{"files",Json::array()},{"diagnostics",Json::array()},{"exclusions",Json::array()}};
    std::map<std::string,std::string> root_roles;
    for(const auto& r:db.query("SELECT * FROM roots ORDER BY id")){
        auto data=Json::parse(r["data"].get<std::string>());
        root_roles[r["id"].get<std::string>()]=r["role"].get<std::string>();
        out["roots"].push_back(data);
        if(!data.value("complete",false))out["complete"]=false;
        for(const auto& d:data.value("diagnostics",Json::array())){if(out["diagnostics"].size()<250)out["diagnostics"].push_back(d);}
        for(const auto& e:data.value("exclusions",Json::array())){if(out["exclusions"].size()<250)out["exclusions"].push_back(e);}
    }
    if(out["roots"].empty())out["complete"]=false;
    for(const auto& diag:db.query("SELECT value FROM meta WHERE key='analysis_diagnostics'")){
        auto parsed_diags=Json::parse(diag["value"].get<std::string>());
        if(parsed_diags.is_array()){
            for(const auto& d:parsed_diags){if(out["diagnostics"].size()>=250)break;out["diagnostics"].push_back(d);}
        }
    }
    Json hashes=Json::object();
    for(const auto& row:db.query("SELECT id,root,path,absolute_path,sha256,analysis,relevant,category,language,git_state,build_state,COALESCE(json_extract(data,'$.generated'),0) AS generated,COALESCE(json_extract(data,'$.error'),'') AS err FROM files WHERE deleted=0 ORDER BY root,path")){
        std::string r_id=row["root"].get<std::string>();
        std::string err=row["err"].is_string()?row["err"].get<std::string>():"";
        if(!err.empty())out["complete"]=false;
        Json f={
            {"id",fid(row["id"])},
            {"root",r_id},
            {"role",root_roles.contains(r_id)?root_roles[r_id]:""},
            {"path",row["path"]},
            {"absolute_path",row["absolute_path"]},
            {"sha256",row["sha256"]},
            {"analysis",row["analysis"]},
            {"relevant",row["relevant"]==1},
            {"category",row["category"]},
            {"language",row["language"]},
            {"generated",row["generated"]==1||row["category"]=="generated"}
        };
        if(!err.empty())f["error"]=err;
        out["files"].push_back(std::move(f));
        hashes[r_id].push_back({row["path"],row["sha256"]});
    }
    for(auto& r:out["roots"])r["fingerprint"]=sha256(hashes.value(r["id"].get<std::string>(),Json::array()).dump());
    for(const auto& root:p.config["roots"]){
        bool found=false;
        for(const auto& r:out["roots"])if(r["id"]==root["id"]&&r["path"]==path_text(p.resolve(root["path"])))found=true;
        if(!found){
            out["complete"]=false;
            out["diagnostics"].push_back({{"file",root["path"]},{"message","Raíz configurada todavía sin sincronizar"}});
        }
    }
    const auto active_syms=db.integer("SELECT COUNT(*) FROM symbols s JOIN files f ON s.file_id=f.id WHERE s.active=1 AND f.deleted=0");
    const auto dep_cnt=db.integer("SELECT COUNT(*) FROM dependencies d JOIN files f ON d.file_id=f.id WHERE f.deleted=0");
    out["symbols_count"]=static_cast<int64_t>(out["files"].size())+active_syms;
    out["dependencies_count"]=dep_cnt;
    out["inventory_config"]=sha256(Json{{"roots",p.config["roots"]},{"exclude_dirs",p.config.value("exclude_dirs",Json::array())}}.dump());
    return out;
}
Json index_snapshot(const Project& p,bool include_text){Database db(p);if(scale_enabled(p,db))return scale_snapshot(p,db);auto snapshot=make_scan(db,include_text);for(const auto& root:p.config["roots"]){bool found=false;for(const auto& r:snapshot["roots"])if(r["id"]==root["id"]&&r["path"]==path_text(p.resolve(root["path"])))found=true;if(!found){snapshot["complete"]=false;snapshot["diagnostics"].push_back({{"file",root["path"]},{"message","Raíz configurada todavía sin sincronizar"}});}}return snapshot;}
Json index_sync(const Project& p,bool full,Log log,Cancel* cancel,const Json& scope){
    {
        Database catalog(p);bool durable=scale_enabled(p,catalog);
        if(!durable){size_t count=0;for(const auto& root:p.config["roots"]){std::error_code ec;fs::recursive_directory_iterator it(p.resolve(root["path"]),fs::directory_options::skip_permission_denied,ec),end;
            for(;!ec&&it!=end;it.increment(ec)){const auto attrs=GetFileAttributesW(it->path().c_str());if(attrs!=INVALID_FILE_ATTRIBUTES&&(attrs&FILE_ATTRIBUTE_REPARSE_POINT))it.disable_recursion_pending();if(++count>10000){durable=true;break;}}if(durable)break;}}
        if(durable)return scale_run(p,{{"scope",scope},{"full",full}},log,cancel);
    }
    if(!scope.is_object())throw std::runtime_error("scope debe ser un objeto");
    const bool scoped=!scope.empty();
    const auto scope_root=scope.value("root",std::string());
    const auto relative_scope=fs::path(wide(scope.value("subdirectory",std::string()))).lexically_normal();
    if(scoped){
        bool known_root=false;
        for(const auto& root:p.config["roots"])if(root["id"]==scope_root)known_root=true;
        if(!known_root||relative_scope.empty()||relative_scope=="."||relative_scope.has_root_path())
            throw std::runtime_error("scope requiere una raíz configurada y un subdirectorio relativo");
        for(const auto& part:relative_scope)if(part=="..")
            throw std::runtime_error("El subdirectorio de scope debe quedar dentro de su raíz");
    }
    auto scope_prefix=path_text(relative_scope);
    if(scoped&&!scope_prefix.ends_with('/'))scope_prefix+='/';

    const auto start=GetTickCount64();Database db(p);auto initial=db.revision();
    const CodeDetection policy(p.config);
    const auto duplicate_mode=scoped?"off":p.config.value("code_duplicate_mode","off");
    std::vector<ArchiveFile> archive_paths;
    const bool was_dirty=db.integer("SELECT COALESCE(MAX(CAST(value AS INTEGER)),0) FROM meta WHERE key='dirty'")!=0;
    std::string oldfiles_sql=policy.source_only?
        "SELECT id,root,path,absolute_path,winid,sha256,bytes,mtime,category,language,relevant,git_state,build_state,analysis,deleted,last_revision,json_remove(data,'$.text_content') AS data FROM files":
        "SELECT * FROM files";
    Json oldfiles_params=Json::array();
    if(scoped){
        oldfiles_sql+=" WHERE root=? AND path>=? COLLATE NOCASE AND path<? COLLATE NOCASE";
        oldfiles_params=Json::array({scope_root,scope_prefix,scope_prefix+"\xF4\x8F\xBF\xBF"});
    }
    const auto oldfiles=db.query(oldfiles_sql,oldfiles_params);
    std::map<std::string,const Json*> oldpaths;
    std::map<std::string,std::vector<const Json*>> oldids;
    for(const auto& f:oldfiles){oldpaths[low(f["absolute_path"])]=&f;if(!f["winid"].is_null())oldids[f["winid"]].push_back(&f);}
    std::map<std::string,Json> manual_classifications;
    for(const auto& row:db.query("SELECT entity,detail FROM history WHERE action='classification' ORDER BY revision"))
        manual_classifications[row["entity"].get<std::string>()]=Json::parse(row["detail"].get<std::string>());
    std::set<int64_t> seen,filtered;
    std::set<std::string> complete_roots;Json changes=Json::array(),errors=Json::array();
    Json scope_result;
    Json analyzed={{"files",Json::array()},{"symbols",Json::array()},{"dependencies",Json::array()},{"diagnostics",Json::array()},{"roots",Json::array()}};
    Json pending_files=Json::array(),root_results=Json::array();size_t hashed=0,reused=0;std::set<std::string> touched_roots;
    Json tool_digests=Json::object();
    {
        std::error_code tec;
        fs::path clang_dll = executable_dir() / L"libclang.dll";
        if (!fs::exists(native_path(clang_dll), tec)) clang_dll = fs::path(MIGRATOOL_CLANG_DLL);
        if (fs::exists(native_path(clang_dll), tec) && fs::is_regular_file(native_path(clang_dll), tec)) {
            tool_digests["clang"] = hash_file(clang_dll);
        }
        const fs::path roslyn_dll=executable_dir()/L"adapters/roslyn/RoslynAdapter.dll";
        if(fs::exists(roslyn_dll,tec)&&fs::is_regular_file(roslyn_dll,tec))tool_digests["roslyn"]=hash_file(roslyn_dll);
        for(const auto& item:p.config.value("analyzers",Json::array())){
            if(!item.is_object())continue;
            for(const auto* key:{"argv","command"}){
                if(!item.contains(key)||!item[key].is_array())continue;
                for(const auto& tok:item[key]){
                    if(!tok.is_string())continue;
                    auto cand=p.resolve(tok.get<std::string>());
                    if(fs::exists(cand,tec)&&fs::is_regular_file(cand,tec))tool_digests[tok.get<std::string>()]=hash_file(cand);
                }
            }
        }
    }
    auto config_hash=sha256(Json{{"roots",p.config["roots"]},{"defines",p.config.value("defines",Json::array())},{"clang_args",p.config.value("clang_args",Json::array())},{"analyzers",p.config.value("analyzers",Json::array())},{"analyze_cpp",p.config.value("analyze_cpp",true)},{"code_detection",policy.key(false)},{"tool_digests",tool_digests}}.dump());
    auto meta=db.query("SELECT value FROM meta WHERE key='analyzer_config'");bool config_changed=meta.empty()||meta[0]["value"]!=config_hash;
    // Los escaneos de filesystem ocurren fuera de la transacción; revisión esperada protege el commit.
    for(const auto& root:p.config["roots"]){
        if(scoped&&root["id"]!=scope_root)continue;
        auto base=p.resolve(root["path"]);auto rid=root["id"].get<std::string>();
        const bool scoped_root=scoped&&rid==scope_root;
        const auto scan_base=scoped_root?base/relative_scope:base;
        if(scoped_root){
            auto component=base;
            for(const auto& part:relative_scope){
                component/=part;
                const auto attrs=GetFileAttributesW(component.c_str());
                if(attrs!=INVALID_FILE_ATTRIBUTES&&(attrs&FILE_ATTRIBUTE_REPARSE_POINT))
                    throw std::runtime_error("El subdirectorio de scope atraviesa un punto de reanálisis: "+path_text(component));
            }
        }
        if(log)log("Índice: "+rid+" — "+path_text(scan_base));
        const bool include_all_code=root.value("include_all_code",false);
        const auto classification_key=policy.key(include_all_code);
        size_t filtered_count=0;
        Json rr={{"id",rid},{"role",root["role"]},{"path",path_text(base)},{"complete",true},{"diagnostics",Json::array()},{"exclusions",Json::array()}};analyzed["roots"].push_back(rr);
        const auto store_root=[&] {
            if(scoped_root){
                scope_result={{"root",rid},{"path",path_text(scan_base)},{"complete",rr["complete"]},
                              {"diagnostics",rr["diagnostics"]},{"exclusions",rr["exclusions"]},{"at",now()}};
                const auto prior=db.query("SELECT data FROM roots WHERE id=? AND path=?",{rid,path_text(base)});
                if(prior.empty())rr["complete"]=false;
                else rr=Json::parse(prior.front().at("data").get<std::string>());
                if(scope_result["complete"]!=true)rr["complete"]=false;
                rr["last_scoped_sync"]=scope_result;
            }else if(rr["complete"]==true)complete_roots.insert(rid);
            root_results.push_back(rr);
        };
        if(!fs::is_directory(scan_base)){rr["complete"]=false;rr["diagnostics"].push_back({{"file",path_text(scan_base)},{"message","Directorio inexistente"}});store_root();continue;}
        auto git=git_states(p,base);rr["git_available"]=git["available"];
        std::error_code ec;fs::recursive_directory_iterator it(scan_base,fs::directory_options::none,ec),end;
        if(ec){rr["complete"]=false;rr["diagnostics"].push_back({{"file",path_text(scan_base)},{"message",ec.message()}});}
        const auto advance=[&](const std::string& path){
            it.increment(ec);
            if(ec){rr["complete"]=false;rr["diagnostics"].push_back({{"file",path},{"message",ec.message()}});ec.clear();}
        };
        while(it!=end){
            if(cancel&&cancel->requested)throw std::runtime_error("Indexación cancelada");
            const auto item=it->path();const auto attrs=GetFileAttributesW(item.c_str());auto path=path_text(item);bool skip=false;
            if(attrs==INVALID_FILE_ATTRIBUTES){rr["complete"]=false;rr["diagnostics"].push_back({{"file",path},{"message","No se pudo leer atributos"}});}
            else if((attrs&FILE_ATTRIBUTE_REPARSE_POINT)||(attrs&FILE_ATTRIBUTE_DIRECTORY&&(item.filename()==L".git"||item.filename()==L".svn"||item.filename()==L".hg"||item.filename()==L".migratool"))){
                if(attrs&FILE_ATTRIBUTE_DIRECTORY)it.disable_recursion_pending();skip=true;rr["exclusions"].push_back({{"path",path},{"reason",(attrs&FILE_ATTRIBUTE_REPARSE_POINT)?"reparse_point_no_follow":"internal_metadata"}});
            }
            if(!skip&&attrs!=INVALID_FILE_ATTRIBUTES&&!(attrs&FILE_ATTRIBUTE_DIRECTORY)){
                if(duplicate_mode=="files_and_zip"&&low(path_text(item.extension()))==".zip")
                    archive_paths.push_back({item,rid,root["role"]});
                const auto prior=oldpaths.find(low(path));
                const Json* previous=prior==oldpaths.end()?nullptr:prior->second;
                try{
                    auto ident=identity(item);
                    if(!previous){
                        const auto identities=oldids.find(ident.id);
                        if(identities!=oldids.end())for(const auto* candidate:identities->second){
                            if(!seen.contains(candidate->at("id"))&&!fs::exists(fs::path(wide(candidate->at("absolute_path"))))){previous=candidate;break;}
                        }
                    }
                    const Json no_previous;
                    const auto& old=previous?*previous:no_previous;
                    bool unchanged=!old.is_null()&&old["winid"]==ident.id&&old["bytes"]==ident.size&&old["mtime"]==ident.modified&&!old["sha256"].is_null();
                    const auto old_data=old.is_null()?Json::object():Json::parse(old["data"].get<std::string>());
                    const bool matching_policy=previous&&old["absolute_path"]==path&&old["root"]==rid&&
                        old_data.value("classification_key", "")==classification_key;
                    const auto cached_classification=[&]{return Classification{old["category"],old["language"],old["relevant"]==1,
                        old_data.value("text_content",""),old_data.value("text_truncated",false),
                        old_data.value("source_code",false),old_data.value("blocked_code",false)};};
                    const auto override=previous?manual_classifications.find(fid(old["id"])):manual_classifications.end();
                    const Json* manual=override==manual_classifications.end()?nullptr:&override->second.at("after");
                    const auto manual_code=[&]{
                        if(!manual)return false;
                        const auto& category=manual->at("category");
                        return manual->value("relevant",false)||category=="source"||category=="script"||category=="test"||category=="resource";
                    };
                    const auto filter_file=[&]{
                        ++filtered_count;
                        if(previous)filtered.insert(old["id"].get<int64_t>());
                        advance(path);
                    };
                    Classification cl;
                    if(policy.source_only){
                        cl=matching_policy&&unchanged&&!full?cached_classification():classify(item,base,policy,include_all_code,manual_code());
                        if(cl.blocked||(!cl.source_code&&!manual_code())){filter_file();continue;}
                    }
                    auto hash=(!full&&unchanged)?old["sha256"].get<std::string>():hash_file(item);
                    if(!full&&unchanged)++reused;else ++hashed;
                    const bool content_same=previous&&old["sha256"]==hash;
                    if(!policy.source_only)cl=content_same&&matching_policy?cached_classification():classify(item,base,policy,include_all_code,manual_code());
                    if(manual&&!cl.blocked&&override->second.at("sha256")==hash){
                        cl.category=manual->at("category");
                        cl.relevant=manual->at("relevant");
                        cl.source_code=manual_code()||(cl.source_code&&(cl.category=="generated"||cl.category=="external_dependency"||cl.category=="build_artifact"));
                    }
                    if(policy.source_only&&!cl.source_code){filter_file();continue;}
                    auto verify=identity(item);if(verify.id!=ident.id||verify.modified!=ident.modified||verify.size!=ident.size)throw std::runtime_error("Archivo cambió durante indexación");
                    Json f={{"root",rid},{"role",root["role"]},{"path",path_text(item.lexically_relative(base))},{"absolute_path",path},{"analysis","NO_ANALYZER"},
                        {"include_all_code",include_all_code},{"classification_key",classification_key},{"source_code",cl.source_code},{"blocked_code",cl.blocked}};
                    f["winid"]=ident.id;f["mtime"]=ident.modified;f["bytes"]=ident.size;f["sha256"]=hash;f["category"]=cl.category;f["language"]=cl.lang;f["relevant"]=cl.relevant;f["text_content"]=cl.content;f["text_truncated"]=cl.truncated;
                    f["generated"]=cl.category=="generated";f["git_state"]=git_state(git,f["path"]);f["build_state"]=build_state(p,root,item);
                    if(!old.is_null()) {f["numeric_id"]=old["id"];f["analysis"]=content_same?old["analysis"]:Json("NO_ANALYZER");seen.insert(old["id"].get<int64_t>());}
                    bool moved=!old.is_null()&&old["absolute_path"]!=path;bool changed=old.is_null()||!content_same||moved||old["deleted"]==1||old["git_state"]!=f["git_state"]||old["build_state"]!=f["build_state"]||old["category"]!=cl.category||old["relevant"]!=(cl.relevant?1:0);
                    // La comprobación exacta incluye metadatos y texto, además del hash.
                    bool reuse_storage=false;
                    if(!full&&!changed&&unchanged&&matching_policy&&old.value("first_revision",int64_t(0))>0){
                        auto persisted=f;
                        persisted.erase("numeric_id");
                        persisted["id"]=fid(old["id"].get<int64_t>());
                        reuse_storage=persisted==old_data;
                    }
                    f["reuse_storage"]=reuse_storage;
                    // El cuerpo FTS no depende del estado del analizador.
                    f["reuse_search"]=!full&&!old.is_null()&&old["deleted"]==0&&
                        old["absolute_path"]==path&&old["language"]==f["language"]&&
                        old["category"]==f["category"]&&old_data.value("text_content","")==cl.content;
                    f["index_changed"]=changed;f["previous_revision"]=old.is_null()?Json(0):old["last_revision"];
                    f["need_analysis"]=(cl.relevant||cl.category=="resource"||cl.lang=="CMake")&&(config_changed||!content_same||full||moved);
                    bool parent_moved=moved&&(old["root"]!=rid||fs::path(wide(old["absolute_path"])).parent_path()!=item.parent_path());
                    if(changed){changes.push_back({{"action",old.is_null()?"created":old["deleted"]==1?"restored":parent_moved?"moved":moved?"renamed":"modified"},{"path",path},{"old",old.is_null()?Json():Json{{"id",fid(old["id"])},{"path",old["absolute_path"]},{"sha256",old["sha256"]}}},{"sha256",hash}});touched_roots.insert(rid);}
                    pending_files.push_back(f);
                }catch(const std::exception& e){rr["complete"]=false;rr["diagnostics"].push_back({{"file",path},{"message",e.what()}});errors.push_back({{"file",path},{"error",e.what()}});if(previous)seen.insert(previous->at("id").get<int64_t>());}
            }
            advance(path);
        }
        rr["filtered_non_code"]=filtered_count;
        store_root();
    }
    Json zip_cache=duplicate_mode=="files_and_zip"?collect_zip_entries(db,archive_paths,policy,full,log,cancel):Json::object();
    Json duplicate_report;
    if(duplicate_mode!="off"){
        Json archive_errors=Json::array();
        size_t zip_sources=0;
        for(const auto& [path,zip]:zip_cache.items()){
            zip_sources+=zip.at("entries").size();
            for(auto issue:zip.at("errors")){
                issue["absolute_path"]=path;
                archive_errors.push_back(std::move(issue));
            }
        }
        auto groups=group_code_copies(pending_files,zip_cache);
        const auto count=groups.size();
        const bool complete=archive_errors.empty()&&complete_roots.size()==root_results.size();
        duplicate_report={{"schema","migratool.code-duplicates/1"},{"mode",duplicate_mode},{"complete",complete},
            {"archive_count",zip_cache.size()},{"zip_sources",zip_sources},{"group_count",count},
            {"groups",std::move(groups)},{"errors",std::move(archive_errors)}};
    }
    // Phase 2: Prepare and run code analysis OUTSIDE the SQLite write transaction
    // so Roslyn / libclang / AST scanners never hold the WAL writer lock.
    for(size_t idx=0;idx<pending_files.size();++idx){
        auto& f=pending_files[idx];
        std::string pre_id=f.contains("numeric_id")?fid(f["numeric_id"].get<int64_t>()):("FNEW_"+std::to_string(idx));
        f["id"]=pre_id;
        auto need=f["need_analysis"].get<bool>();
        if((f["language"]=="C++"||f["language"]=="C"||f["language"]=="C/C++")&&touched_roots.contains(f["root"].get<std::string>()))
            need=true;
        f["need_analysis"]=need;
        if(need){
            auto stored=f;
            stored.erase("numeric_id");
            stored.erase("need_analysis");
            stored.erase("index_changed");
            stored.erase("previous_revision");
            stored.erase("reuse_storage");
            stored.erase("reuse_search");
            analyzed["files"].push_back(std::move(stored));
        }
    }

    std::string phase="analyze";
    if(!analyzed["files"].empty()){
        analyzed=analyze(p,std::move(analyzed),log,cancel);
        // Pre-compute symbol_sha256 outside the SQLite transaction
        std::map<std::string,std::string> analyzed_file_contents;
        for(auto& sym:analyzed["symbols"]){
            if(sym.contains("offset")&&sym.contains("end_offset")){
                auto off=sym["offset"].get<size_t>(),end_off=sym["end_offset"].get<size_t>();
                if(end_off>off){
                    auto abs_p=sym["absolute_path"].get<std::string>();
                    auto it=analyzed_file_contents.find(abs_p);
                    if(it==analyzed_file_contents.end()){
                        try{it=analyzed_file_contents.emplace(abs_p,read_text(fs::path(wide(abs_p)),32*1024*1024)).first;}
                        catch(...){it=analyzed_file_contents.emplace(abs_p,"").first;}
                    }
                    if(end_off<=it->second.size())sym["symbol_sha256"]=sha256(it->second.substr(off,end_off-off));
                }
            }
        }
    }

    phase="validate current inputs";
    for(const auto& f:pending_files){
        if(cancel&&cancel->requested)throw std::runtime_error("Indexación cancelada");
        auto current=identity(fs::path(wide(f["absolute_path"])));
        if(current.id!=f["winid"].get<std::string>()||current.modified!=f["mtime"].get<int64_t>()||current.size!=f["bytes"].get<uint64_t>())
            throw std::runtime_error("Archivo cambió antes del commit: "+f["absolute_path"].get<std::string>());
    }
    if(!duplicate_report.is_null()){
        for(const auto& [path,zip]:zip_cache.items())if(zip.contains("fingerprint")){
            const auto actual=identity(fs::path(wide(path)));
            const auto& expected=zip.at("fingerprint");
            if(expected[0]!=actual.id||expected[1]!=actual.size||expected[2]!=actual.modified)
                throw std::runtime_error("El ZIP cambió antes del commit: "+path);
        }
    }

    // Phase 3: Short atomic SQLite write transaction (no external process or heavy file I/O)
    phase="begin";
    db.begin();
    const auto tx_start_rev=db.revision();
    try{
        for(const auto& old:db.query("SELECT id,path FROM roots")){
            bool retained=false;for(const auto& root:p.config["roots"])if(root["id"]==old["id"])retained=true;
            if(!retained){auto rev=db.change("indexer","root_removed",{{"root",old["id"]},{"path",old["path"]}});db.exec("UPDATE files SET deleted=1,last_revision=? WHERE root=?",{rev,old["id"]});db.exec("DELETE FROM roots WHERE id=?",{old["id"]});}
        }
        // Asignar identidad antes de registrar el cambio permite consultar TODO el historial por F-id, incluso renombres.
        std::map<std::string,int64_t> pending_ids;
        std::map<std::string,std::string> temp_to_real_id;
        if(log)log("Registro de inventario: "+std::to_string(pending_files.size())+" archivos");
        for(size_t idx=0;idx<pending_files.size();++idx){
            auto& f=pending_files[idx];
            if(!f.contains("numeric_id")){
                db.exec("INSERT INTO files(root,path,absolute_path,first_revision,last_revision,data)VALUES(?,?,?,0,0,'{}')",{f["root"],f["path"],f["absolute_path"]});
                auto new_num=db.integer("SELECT last_insert_rowid()");
                f["numeric_id"]=new_num;
                temp_to_real_id["FNEW_"+std::to_string(idx)]=fid(new_num);
            }
            pending_ids.emplace(f["absolute_path"].get<std::string>(),f["numeric_id"].get<int64_t>());
        }
        if(!temp_to_real_id.empty()){
            for(auto& af:analyzed["files"]){
                auto it=temp_to_real_id.find(af["id"].get<std::string>());
                if(it!=temp_to_real_id.end())af["id"]=it->second;
            }
            for(auto& sym:analyzed["symbols"]){
                auto fit=temp_to_real_id.find(sym["file_id"].get<std::string>());
                if(fit!=temp_to_real_id.end()){
                    const auto old_fid=fit->first;
                    const auto new_fid=fit->second;
                    sym["file_id"]=new_fid;
                    auto sid_str=sym["id"].get<std::string>();
                    if(sid_str.starts_with(old_fid+"#"))
                        sym["id"]=new_fid+sid_str.substr(old_fid.size());
                }
            }
            for(auto& d:analyzed["dependencies"]){
                auto dit=temp_to_real_id.find(d["file_id"].get<std::string>());
                if(dit!=temp_to_real_id.end())d["file_id"]=dit->second;
            }
        }
        // entity_type/entity_id son columnas UNINDEXED de FTS5. Borrar por
        // esos campos para cada archivo hacía un recorrido completo de FTS.
        // Se resuelven una vez los rowid, conservando todos los duplicados.
        using SearchRows=std::map<std::string,std::vector<int64_t>>;
        SearchRows file_search_rows,symbol_search_rows;
        for(const auto& row:db.query("SELECT rowid,entity_type,entity_id FROM search WHERE entity_type IN('file','symbol')")){
            auto& rows=row["entity_type"]=="file"?file_search_rows:symbol_search_rows;
            rows[row["entity_id"].get<std::string>()].push_back(row["rowid"].get<int64_t>());
        }
        const auto remove_search=[&](const SearchRows& rows,const std::string& id){
            const auto found=rows.find(id);
            if(found!=rows.end())for(auto rowid:found->second)db.exec("DELETE FROM search WHERE rowid=?",{rowid});
        };
        for(auto& change:changes){auto id="F"+std::to_string(pending_ids.at(change["path"].get<std::string>()));change["id"]=id;auto rev=db.change("indexer",change["action"],change,id);change["revision"]=rev;}
        size_t reused_records=0;
        for(auto& f:pending_files){
            if(cancel&&cancel->requested)throw std::runtime_error("Indexación cancelada");
            int64_t id=f.value("numeric_id",int64_t(0));auto rel=f["path"].get<std::string>(),abs=f["absolute_path"].get<std::string>();
            if(!id){db.exec("INSERT INTO files(root,path,absolute_path,first_revision,last_revision,data)VALUES(?,?,?,?,?,?)",{f["root"],rel,abs,db.revision(),db.revision(),"{}"});id=db.integer("SELECT last_insert_rowid()");f["numeric_id"]=id;}
            const auto search_id=fid(id);
            f["id"]=search_id;
            seen.insert(id);
            const auto rows=file_search_rows.find(search_id);
            const bool search_current=f["reuse_search"]==true&&rows!=file_search_rows.end()&&rows->second.size()==1;
            if(search_current&&f["reuse_storage"]==true&&f["need_analysis"]!=true){
                ++reused_records;
                continue;
            }
            auto last_revision=f["index_changed"]==true?Json(db.revision()):f["previous_revision"];
            auto stored=f;stored.erase("numeric_id");stored.erase("need_analysis");stored.erase("index_changed");stored.erase("previous_revision");stored.erase("reuse_storage");stored.erase("reuse_search");
            db.exec("UPDATE files SET root=?,path=?,absolute_path=?,winid=?,sha256=?,bytes=?,mtime=?,category=?,language=?,relevant=?,git_state=?,build_state=?,analysis=?,deleted=0,last_revision=?,data=? WHERE id=?",
                {f["root"],rel,abs,f["winid"],f["sha256"],f["bytes"],f["mtime"],f["category"],f["language"],f["relevant"],f["git_state"],f["build_state"],f["analysis"],last_revision,stored.dump(),id});
            db.exec("UPDATE files SET first_revision=? WHERE id=? AND first_revision=0",{db.revision(),id});
            if(!search_current){
                remove_search(file_search_rows,search_id);
                db.exec("INSERT INTO search(entity_type,entity_id,body)VALUES('file',?,?)",{search_id,abs+" "+f["language"].get<std::string>()+" "+f["category"].get<std::string>()+" "+f["text_content"].get<std::string>()});
            }
        }
        if(log)log("Registros intactos reutilizados: "+std::to_string(reused_records));
        for(const auto& old:oldfiles)if(old["deleted"]==0&&!seen.contains(old["id"])){
            const bool by_filter=filtered.contains(old["id"]);
            const bool in_complete_scope=scoped&&old["root"]==scope_root&&scope_result["complete"]==true;
            if(!by_filter&&!in_complete_scope&&!complete_roots.contains(old["root"]))continue;
            const auto* action=by_filter?"filtered_out":"deleted";
            auto change=Json{{"action",action},{"id",fid(old["id"])},{"path",old["absolute_path"]},{"sha256",old["sha256"]}};
            auto rev=db.change("indexer",action,change,fid(old["id"]));change["revision"]=rev;changes.push_back(change);
            db.exec("UPDATE files SET deleted=1,last_revision=?,data=json_set(data,'$.filtered_by_inventory_mode',?) WHERE id=?",{rev,by_filter?1:0,old["id"]});
            remove_search(file_search_rows,fid(old["id"]));
            if(by_filter)for(const auto& symbol:db.query("SELECT id FROM symbols WHERE file_id=?",{old["id"]}))
                remove_search(symbol_search_rows,"S"+std::to_string(symbol["id"].get<int64_t>()));
        }
        for(const auto& r:root_results)db.exec("INSERT INTO roots(id,role,path,complete,data)VALUES(?,?,?,?,?) ON CONFLICT(id)DO UPDATE SET role=excluded.role,path=excluded.path,complete=excluded.complete,data=excluded.data",{r["id"],r["role"],r["path"],r["complete"],r.dump()});
        if(scoped)db.change("indexer","scoped_inventory",scope_result,scope_root);
        if(config_changed||!errors.empty())db.change("indexer","index_configuration",{{"configuration_sha256",config_hash},{"errors",errors}});
        if(was_dirty&&db.revision()==tx_start_rev&&analyzed["files"].empty())db.change("indexer","inventory_reconciled",{{"roots",root_results.size()}});
        if(!analyzed["files"].empty())db.change("indexer","analysis_updated",{{"files",analyzed["files"].size()},{"roslyn",analyzed.value("roslyn",Json::object())},{"clang",analyzed.value("clang",Json::object())}});
        phase="store files";
        if(!analyzed["files"].empty()){
            // También se aplica a catálogos existentes sin alterar su versión.
            db.exec("CREATE INDEX IF NOT EXISTS dependency_source ON dependencies(file_id)");
            Json bounded_diags=Json::array();
            for(const auto& d:analyzed["diagnostics"]){if(bounded_diags.size()>=500)break;bounded_diags.push_back(d);}
            db.exec("INSERT INTO meta(key,value)VALUES('analysis_diagnostics',?)ON CONFLICT(key)DO UPDATE SET value=excluded.value",{bounded_diags.dump()});
        }
        for(const auto& f:analyzed["files"]){
            auto id=std::stoll(f["id"].get<std::string>().substr(1));
            db.exec("UPDATE files SET analysis=?,data=json_set(data,'$.analysis',?) WHERE id=?", {f["analysis"],f["analysis"],id});
            db.exec("UPDATE symbols SET active=0 WHERE file_id=?",{id});
            for(const auto& symbol:db.query("SELECT id FROM symbols WHERE file_id=?",{id}))
                remove_search(symbol_search_rows,"S"+std::to_string(symbol["id"].get<int64_t>()));
            db.exec("DELETE FROM dependencies WHERE file_id=?",{id});
        }
        phase="store symbols";
        for(const auto& sym:analyzed["symbols"]){
            auto file_id=std::stoll(sym["file_id"].get<std::string>().substr(1));auto key=sym.at("id").get<std::string>();
            // La clave de declaración se mantiene separada del identificador secuencial estable.
            db.exec("INSERT INTO symbols(file_id,stable_key,name,qualified_name,kind,line,data,active)VALUES(?,?,?,?,?,?,?,1) ON CONFLICT(file_id,stable_key)DO UPDATE SET name=excluded.name,qualified_name=excluded.qualified_name,kind=excluded.kind,line=excluded.line,data=excluded.data,active=1",
                {file_id,key,sym["name"],sym.value("qualified_name",sym["name"].get<std::string>()),sym["kind"],sym["line"],sym.dump()});
            auto sid=db.integer("SELECT id FROM symbols WHERE file_id=? AND stable_key=?",{file_id,key});
            db.exec("INSERT INTO search(entity_type,entity_id,body)VALUES('symbol',?,?)",{"S"+std::to_string(sid),sym.value("qualified_name",sym["name"].get<std::string>())+" "+sym.value("signature","")+" "+sym["kind"].get<std::string>()+" "+sym["absolute_path"].get<std::string>()});
        }
        phase="store dependencies";
        if(!analyzed["dependencies"].empty()){
            struct SymTarget { std::string sid; int64_t file_id; std::string qualified_name; std::string signature; };
            std::map<std::string,SymTarget> by_sem_key;
            std::map<std::pair<int64_t,std::string>,std::string> by_file_qname;
            std::map<std::string,std::vector<SymTarget>> by_qname, by_name;
            for(const auto& srow:db.query("SELECT s.id,s.file_id,s.name,s.qualified_name,s.data FROM symbols s JOIN files f ON s.file_id=f.id WHERE s.active=1 AND f.deleted=0")){
                auto sid="S"+std::to_string(srow["id"].get<int64_t>());
                auto fid_num=srow["file_id"].get<int64_t>();
                auto sname=srow["name"].get<std::string>();
                auto sqname=srow["qualified_name"].get<std::string>();
                auto sdata=Json::parse(srow["data"].get<std::string>());
                auto ssig=sdata.value("signature","");
                SymTarget st{sid,fid_num,sqname,ssig};
                auto sem=sdata.value("semantic_key","");
                if(!sem.empty())by_sem_key[sem]=st;
                by_file_qname[{fid_num,sqname}]=sid;
                by_qname[sqname].push_back(st);
                by_name[sname].push_back(st);
            }
            for(auto d:analyzed["dependencies"]){
                auto id=std::stoll(d["file_id"].get<std::string>().substr(1));
                Json target_fid_sql=nullptr;
                auto ckey=d.value("caller_key","");
                if(!ckey.empty()&&by_sem_key.contains(ckey))d["caller_symbol_id"]=by_sem_key[ckey].sid;
                else{
                    auto cq=d.value("caller_qualified_name","");
                    if(!cq.empty()&&by_file_qname.contains({id,cq}))d["caller_symbol_id"]=by_file_qname[{id,cq}];
                }
                auto tkey=d.value("target_key","");
                if(!tkey.empty()&&by_sem_key.contains(tkey)){
                    const auto& tgt=by_sem_key[tkey];
                    d["target_symbol_id"]=tgt.sid;
                    d["target_file_id"]="F"+std::to_string(tgt.file_id);
                    d["resolved"]=true;
                    d["resolution"]="SEMANTIC_EXACT";
                    target_fid_sql=tgt.file_id;
                }else if(d.value("resolution","")!="DYNAMIC_OR_REFLECTION"&&d.value("resolution","")!="CANDIDATES_AMBIGUOUS"&&
                         d.value("resolution","")!="XAML_DECLARED"){
                    auto tq=d.value("target_qualified_name","");
                    auto tn=d.value("target_name",d.value("name",""));
                    const std::vector<SymTarget>* matches=nullptr;
                    if(!tq.empty()&&by_qname.contains(tq))matches=&by_qname[tq];
                    else if(!tn.empty()&&by_name.contains(tn))matches=&by_name[tn];
                    if(matches){
                        if(matches->size()==1){
                            d["target_symbol_id"]=(*matches)[0].sid;
                            d["target_file_id"]="F"+std::to_string((*matches)[0].file_id);
                            if(d.value("resolution","")!="SEMANTIC_EXACT")d["resolution"]="SYNTACTIC_UNIQUE";
                            target_fid_sql=(*matches)[0].file_id;
                        }else if(matches->size()>1){
                            d["resolution"]="CANDIDATES_AMBIGUOUS";
                            Json cands=Json::array();
                            for(const auto& m:*matches)cands.push_back({{"symbol_id",m.sid},{"qualified_name",m.qualified_name},{"signature",m.signature}});
                            d["candidate_symbols"]=cands;
                        }
                    }
                }
                db.exec("INSERT INTO dependencies(file_id,target_id,kind,name,data)VALUES(?,?,?,?,?)",{id,target_fid_sql,d.value("kind","reference"),d.value("name",""),d.dump()});
            }
        }
        // Agrupar una vez por nombre, conservando coincidencias de nombre
        // corto/calificado y ambigüedades entre archivos. La clave temporal
        // evita recorrer todos los símbolos por cada dependencia del proyecto.
        phase="resolve dependencies";
        if(log)log("Resolviendo referencias mediante nombres agrupados");
        db.exec("CREATE TEMP TABLE mt_dependency_targets(name TEXT PRIMARY KEY,target_id INTEGER) WITHOUT ROWID");
        db.exec(R"sql(
INSERT INTO mt_dependency_targets(name,target_id)
SELECT name,MIN(file_id) FROM (
    SELECT name,file_id FROM symbols WHERE active=1 AND name IS NOT NULL
    UNION ALL
    SELECT qualified_name AS name,file_id FROM symbols WHERE active=1 AND qualified_name IS NOT NULL
) GROUP BY name HAVING COUNT(DISTINCT file_id)=1
)sql");
        db.exec("UPDATE dependencies SET target_id=(SELECT target_id FROM mt_dependency_targets WHERE name=dependencies.name) WHERE target_id IS NULL");
        db.exec("DROP TABLE mt_dependency_targets");
        db.exec("DELETE FROM doc_links");
        // Reutilizar las rutas de cada raíz. Consultarlas de nuevo para cada
        // documento releía el mismo catálogo completo miles de veces.
        const auto document_targets=db.query("SELECT id,root,path,absolute_path FROM files WHERE deleted=0 AND relevant=1 ORDER BY id");
        std::map<std::string,std::vector<const Json*>> targets_by_root;
        for(const auto& f:document_targets)targets_by_root[f["root"].get<std::string>()].push_back(&f);
        if(log)log("Enlaces documentales: "+std::to_string(document_targets.size())+" rutas reutilizadas por raíz");
        for(const auto& doc:db.query("SELECT id,root,data FROM files WHERE deleted=0 AND category='documentation'")){
            auto body=Json::parse(doc["data"].get<std::string>()).value("text_content","");
            for(const auto* candidate:targets_by_root[doc["root"].get<std::string>()]){
                const auto& f=*candidate;
                auto rel=f["path"].get<std::string>();if(body.find(rel)!=std::string::npos||body.find(f["absolute_path"].get<std::string>())!=std::string::npos)
                    db.exec("INSERT OR IGNORE INTO doc_links(doc_id,code_id,reason)VALUES(?,?,'literal_path_reference')",{doc["id"],f["id"]});
            }
        }
        db.exec("INSERT INTO meta(key,value)VALUES('last_sync',?)ON CONFLICT(key)DO UPDATE SET value=excluded.value",{now()});
        if(!scoped){
            db.exec("INSERT INTO meta(key,value)VALUES('analyzer_config',?)ON CONFLICT(key)DO UPDATE SET value=excluded.value",{config_hash});
            db.exec("INSERT INTO meta(key,value)VALUES('dirty','0')ON CONFLICT(key)DO UPDATE SET value=excluded.value");
            db.exec("INSERT INTO meta(key,value)VALUES('last_config',?)ON CONFLICT(key)DO UPDATE SET value=excluded.value",{sha256(p.config.dump())});
        }
        if(!duplicate_report.is_null()){
            phase="code duplicates";
            const auto signature=sha256(duplicate_report.dump());
            const auto previous=db.query("SELECT value FROM meta WHERE key='code_duplicate_signature'");
            if(previous.empty()||previous[0]["value"]!=signature)
                db.change("indexer","code_duplicates_updated",{{"groups",duplicate_report["group_count"]},{"mode",duplicate_mode}});
            duplicate_report["configuration"]=sha256(p.config.dump());
            duplicate_report["at"]=now();
            db.exec("INSERT INTO meta(key,value)VALUES('code_duplicate_report',?),('code_duplicate_signature',?) ON CONFLICT(key)DO UPDATE SET value=excluded.value",
                    {duplicate_report.dump(),signature});
            if(duplicate_mode=="files_and_zip")
                db.exec("INSERT INTO meta(key,value)VALUES('code_zip_cache',?) ON CONFLICT(key)DO UPDATE SET value=excluded.value",{zip_cache.dump()});
        }
        // Snapshot compacto normalizado: nunca duplica cientos de miles de
        // símbolos o dependencias en meta(key='snapshot'), evitando SQLITE_TOOBIG.
        phase="snapshot";
        auto snapshot=make_compact_scan(db,p);
        db.exec("INSERT INTO meta(key,value)VALUES('snapshot',?)ON CONFLICT(key)DO UPDATE SET value=excluded.value",{snapshot.dump()});
        db.commit();
        // Export de compatibilidad fuera de la transacción SQLite.
        write_json(p.state_dir()/L"scan.json",snapshot);
        auto dups=db.query("SELECT sha256,COUNT(*) AS copies FROM files WHERE deleted=0 AND sha256 IS NOT NULL GROUP BY sha256 HAVING COUNT(*)>1");
        size_t filtered_total=0;for(const auto& root:root_results)filtered_total+=root.value("filtered_non_code",size_t(0));
        Json result={{"schema","migratool.sync/1"},{"revision",db.revision()},{"from_revision",initial},{"changes",changes},{"errors",errors},
            {"inventory_mode",policy.source_only?"source_only":"universal"},{"filtered_non_code",filtered_total},
            {"files",snapshot["files"].size()},{"symbols",snapshot["symbols_count"]},{"complete",snapshot["complete"]},{"hashed_files",hashed},{"reused_hashes",reused},
            {"reused_records",reused_records},{"duration_ms",GetTickCount64()-start},{"duplicates",dups},{"index_path",path_text(p.state_dir()/L"catalog.sqlite")}};
        if(scoped)result["scope"]=scope_result;
        if(!duplicate_report.is_null())result["code_duplicates"]={{"mode",duplicate_mode},{"complete",duplicate_report["complete"]},
            {"group_count",duplicate_report["group_count"]},{"archive_count",duplicate_report["archive_count"]},
            {"zip_sources",duplicate_report["zip_sources"]},{"error_count",duplicate_report["errors"].size()}};
        return result;
    }catch(const std::exception& e){db.rollback();throw std::runtime_error(phase+": "+e.what());}
}
void index_record_event(const Project& p,const std::string& action,const Json& detail,const std::string& actor){
    Database db(p);db.begin();try{
        auto entity=detail.value("source","");if(!entity.empty()){auto matches=db.query("SELECT id FROM files WHERE absolute_path=? AND deleted=0",{path_text(p.resolve(entity))});if(!matches.empty())entity=fid(matches[0]["id"]);}
        auto rev=db.change(actor.empty()?"local-user":actor,action,detail,entity);
        if(action=="note"){db.exec("INSERT INTO notes(entity,text,actor,at,revision)VALUES(?,?,?,?,?)",{entity,detail.at("text"),actor,now(),rev});auto note_id=db.integer("SELECT last_insert_rowid()");db.exec("INSERT INTO search(entity_type,entity_id,body)VALUES('note',?,?)",{"N"+std::to_string(note_id),detail.at("text")});}db.commit();
    }catch(...){db.rollback();throw;}
}
void save_evidence(const Project& p, const std::string& kind, Json value,
                   const std::function<void(Database&, Json&)>& prepare) {
    Database db(p);
    db.begin();
    try {
        if (prepare) prepare(db, value);
        const auto rev = db.change("verifier", kind + "_recorded", {{"id", value.value("id", "")}});
        db.exec("INSERT INTO evidence(id,kind,data,revision,at)VALUES(?,?,?,?,?) "
                "ON CONFLICT(id)DO UPDATE SET data=excluded.data,revision=excluded.revision,at=excluded.at",
                {value.value("id", unique_id()), kind, value.dump(), rev, now()});
        db.commit();
    } catch (...) {
        db.rollback();
        throw;
    }
}
}
