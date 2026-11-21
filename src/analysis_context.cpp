#include "migratool/analysis_context.hpp"
#include "migratool/sql.hpp"
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <shlwapi.h>
#include <xmllite.h>

namespace mt {
namespace {
std::string lower_path(std::string value){
    std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return static_cast<char>(tolower(c));});return value;
}
std::string canonical_path(const fs::path& path){return path_text(path.lexically_normal());}
std::string directory_prefix(const std::string& path){return path.empty()||path.ends_with('/')?path:path+"/";}
constexpr const char* source_predicate="deleted=0 AND substr(lower(path),-3)='.cs'";
constexpr const char* project_predicate="substr(lower(path),-7)='.csproj' AND (deleted=0 OR COALESCE(json_extract(data,'$.present'),0)=1)";
constexpr const char* config_predicate="(substr(lower(path),-7)='.csproj' OR substr(lower(path),-6)='.props' OR substr(lower(path),-8)='.targets' OR substr(lower(path),-4)='.sln' OR substr(lower(path),-5)='.slnx') AND (deleted=0 OR COALESCE(json_extract(data,'$.present'),0)=1)";
struct Root {std::string id;fs::path path;};
struct Located {std::string root,relative;};
std::string trim_ws(std::string_view s){
    size_t a=0,b=s.size();
    while(a<b&&(s[a]==' '||s[a]=='\t'||s[a]=='\r'||s[a]=='\n'))++a;
    while(b>a&&(s[b-1]==' '||s[b-1]=='\t'||s[b-1]=='\r'||s[b-1]=='\n'))--b;
    return std::string(s.substr(a,b-a));
}
std::string slash_norm(std::string s){
    std::replace(s.begin(),s.end(),'\\','/');
    return s;
}
std::string ensure_trailing_slash(std::string s){
    s=slash_norm(std::move(s));
    if(!s.empty()&&!s.ends_with('/'))s.push_back('/');
    return s;
}
bool glob_match_ci(std::string_view pat,std::string_view str){
    size_t p=0,s=0,star=std::string_view::npos,ss=0;
    while(s<str.size()){
        if(p<pat.size()&&(pat[p]=='?'||tolower(static_cast<unsigned char>(pat[p]))==tolower(static_cast<unsigned char>(str[s])))){++p;++s;}
        else if(p<pat.size()&&pat[p]=='*'){star=p++;ss=s;}
        else if(star!=std::string_view::npos){p=star+1;s=++ss;}
        else return false;
    }
    while(p<pat.size()&&pat[p]=='*')++p;
    return p==pat.size();
}
Located locate(const fs::path& path,const std::vector<Root>& roots){
    Located result;
    size_t best=0;
    const auto absolute=wide(canonical_path(path));
    for(const auto& root:roots){
        const auto prefix=wide(directory_prefix(canonical_path(root.path)));
        const auto length=static_cast<int>(prefix.size());
        if(absolute.size()>prefix.size()&&prefix.size()>best&&CompareStringOrdinal(absolute.data(),length,prefix.data(),length,TRUE)==CSTR_EQUAL){
            best=prefix.size();
            result={root.id,utf8(absolute.substr(best))};
        }
    }
    if(!best)throw std::runtime_error("CONTEXT_REFERENCE_OUTSIDE_INVENTORY: "+canonical_path(path));return result;
}
bool skipped_context_path(const std::string& relative){
    const auto path=fs::path(wide(relative));
    for(const auto& component:path.parent_path()){
        const auto name=lower_path(path_text(component));
        if(name=="bin"||name=="obj"||name==".git"||name==".migratool"||name=="node_modules"||name=="build"||name=="packages")return true;
    }
    const auto name=lower_path(path_text(path.filename()));return name.ends_with(".g.cs")||name.ends_with(".g.i.cs")||name.ends_with(".designer.cs");
}
using PropMap=std::map<std::string,std::string>;
void set_prop(PropMap& props,const std::string& key,const std::string& val){
    props[lower_path(key)]=val;
}
std::optional<std::string> get_prop(const PropMap& props,const std::string& key){
    auto it=props.find(lower_path(key));
    if(it!=props.end())return it->second;
    return std::nullopt;
}
std::string expand_msbuild_value(std::string text,const PropMap& props,const std::function<std::string(const std::string&,const std::string&,bool)>& find_above){
    for(int pass=0;pass<16;++pass){
        bool changed=false;
        std::string out;
        out.reserve(text.size());
        for(size_t i=0;i<text.size();){
            if(i+1<text.size()&&text[i]=='$'&&text[i+1]=='('){
                int depth=1;size_t j=i+2;
                while(j<text.size()&&depth>0){
                    if(text[j]=='$'&&j+1<text.size()&&text[j+1]=='('){++depth;j+=2;}
                    else if(text[j]==')'){--depth;++j;}
                    else ++j;
                }
                if(depth==0){
                    auto inner=trim_ws(std::string_view(text).substr(i+2,j-i-3));
                    std::optional<std::string> repl;
                    if(inner.rfind("[MSBuild]::GetPathOfFileAbove(",0)==0||inner.rfind("[MSBuild]::GetDirectoryNameOfFileAbove(",0)==0){
                        const bool dir_only=inner.rfind("[MSBuild]::GetDirectoryNameOfFileAbove(",0)==0;
                        auto open=inner.find('('),close=inner.rfind(')');
                        if(open!=std::string::npos&&close!=std::string::npos&&close>open){
                            auto args_str=inner.substr(open+1,close-open-1);
                            auto comma=args_str.find(',');
                            auto clean_arg=[&](std::string s){
                                s=trim_ws(s);
                                if(s.size()>=2&&((s.front()=='\''&&s.back()=='\'')||(s.front()=='"'&&s.back()=='"')))s=s.substr(1,s.size()-2);
                                return expand_msbuild_value(s,props,find_above);
                            };
                            std::string fname=clean_arg(comma==std::string::npos?args_str:args_str.substr(0,comma));
                            std::string start_dir=comma==std::string::npos?get_prop(props,"MSBuildThisFileDirectory").value_or(""):clean_arg(args_str.substr(comma+1));
                            if(!fname.empty()&&find_above)repl=find_above(fname,start_dir,dir_only);
                        }
                    }else{
                        repl=get_prop(props,inner);
                        if(!repl){
                            wchar_t buf[4096]{};
                            auto n=GetEnvironmentVariableW(wide(inner).c_str(),buf,4096);
                            if(n>0&&n<4096)repl=utf8(buf);
                        }
                    }
                    if(repl){out+=*repl;changed=true;i=j;continue;}
                }
            }
            out.push_back(text[i++]);
        }
        text=std::move(out);
        if(!changed)break;
    }
    return text;
}
struct MsBuildProjectEval{
    std::vector<fs::path> references;
    std::vector<fs::path> config_files;
    std::vector<std::string> compile_include_patterns;
    std::vector<std::string> compile_remove_patterns;
    bool enable_default_compile_items=true;
};
MsBuildProjectEval evaluate_msbuild_project(const fs::path& csproj_path,const std::vector<Root>& roots,
    const std::function<bool(const fs::path&)>& in_census,
    const std::function<std::vector<fs::path>(const fs::path&,const std::string&)>& match_glob_csproj,
    const std::function<std::string(const std::string&,const std::string&,bool)>& find_above,
    const std::function<std::string(const fs::path&)>& find_solution_dir){
    MsBuildProjectEval result;
    std::set<std::string> visited;
    PropMap props;
    const auto proj_dir=canonical_path(csproj_path.parent_path());
    set_prop(props,"MSBuildProjectDirectory",proj_dir);
    set_prop(props,"ProjectDir",ensure_trailing_slash(proj_dir));
    set_prop(props,"MSBuildProjectFile",utf8(csproj_path.filename().wstring()));
    set_prop(props,"MSBuildProjectName",utf8(csproj_path.stem().wstring()));
    set_prop(props,"Configuration","Debug");
    set_prop(props,"Platform","AnyCPU");
    const auto sol=find_solution_dir?find_solution_dir(csproj_path.parent_path()):std::string();
    if(!sol.empty())set_prop(props,"SolutionDir",ensure_trailing_slash(sol));
    else{
        try{
            const auto loc=locate(csproj_path,roots);
            for(const auto& r:roots)if(r.id==loc.root){set_prop(props,"SolutionDir",ensure_trailing_slash(canonical_path(r.path)));break;}
        }catch(...){}
    }
    auto set_file_props=[&](const fs::path& f){
        set_prop(props,"MSBuildThisFileDirectory",ensure_trailing_slash(canonical_path(f.parent_path())));
        set_prop(props,"MSBuildThisFile",utf8(f.filename().wstring()));
        set_prop(props,"MSBuildThisFileName",utf8(f.stem().wstring()));
        set_prop(props,"MSBuildThisFileExtension",utf8(f.extension().wstring()));
    };
    auto eval_xml=[&](auto& self,const fs::path& file)->void{
        const auto norm=file.lexically_normal();
        const auto key=lower_path(canonical_path(norm));
        if(!visited.insert(key).second)return;
        std::error_code ec;const auto sz=fs::file_size(native_path(norm),ec);
        if(ec||sz>4*1024*1024)return;
        result.config_files.push_back(norm);
        const auto saved_dir=get_prop(props,"MSBuildThisFileDirectory");
        const auto saved_file=get_prop(props,"MSBuildThisFile");
        const auto saved_name=get_prop(props,"MSBuildThisFileName");
        const auto saved_ext=get_prop(props,"MSBuildThisFileExtension");
        set_file_props(norm);
        IStream* stream{};IXmlReader* reader{};
        struct Close{IStream*& stream;IXmlReader*& reader;~Close(){if(reader)reader->Release();if(stream)stream->Release();}}close{stream,reader};
        if(FAILED(SHCreateStreamOnFileEx(native_path(norm).c_str(),STGM_READ|STGM_SHARE_DENY_WRITE,FILE_ATTRIBUTE_NORMAL,FALSE,nullptr,&stream)))
            throw std::runtime_error("CONTEXT_PROJECT_OPEN: "+canonical_path(norm));
        if(FAILED(CreateXmlReader(__uuidof(IXmlReader),reinterpret_cast<void**>(&reader),nullptr))||FAILED(reader->SetInput(stream)))
            throw std::runtime_error("CONTEXT_PROJECT_XML: "+canonical_path(norm));
        reader->SetProperty(XmlReaderProperty_DtdProcessing,DtdProcessing_Prohibit);
        XmlNodeType type;HRESULT status;
        std::vector<std::string> stack;
        std::string prop_name,prop_val;
        auto attr_val=[&](const wchar_t* k)->std::string{
            std::string v;if(SUCCEEDED(reader->MoveToAttributeByName(k,nullptr))){const wchar_t* s{};reader->GetValue(&s,nullptr);if(s)v=utf8(s);reader->MoveToElement();}return v;
        };
        auto split_semi=[&](const std::string& raw,auto fn){
            std::string cur;
            for(size_t i=0;i<=raw.size();++i){
                if(i==raw.size()||raw[i]==';'){auto part=trim_ws(cur);cur.clear();if(!part.empty())fn(part);}
                else cur.push_back(raw[i]);
            }
        };
        while((status=reader->Read(&type))==S_OK){
            if(type==XmlNodeType_Element){
                const wchar_t* wn{};reader->GetLocalName(&wn,nullptr);
                std::string el=wn?utf8(wn):"";
                const bool empty_el=reader->IsEmptyElement()!=FALSE;
                if(stack.size()>=2&&stack.back()=="PropertyGroup"&&prop_name.empty()&&el!="PropertyGroup"){
                    prop_name=el;prop_val.clear();
                }else if(el=="Import"){
                    auto imp=attr_val(L"Project");
                    if(!imp.empty()){
                        auto exp=expand_msbuild_value(imp,props,find_above);
                        if(!exp.empty()&&exp.find("$(")==std::string::npos){
                            fs::path candidate(wide(slash_norm(exp)));
                            if(!candidate.is_absolute())candidate=(norm.parent_path()/candidate).lexically_normal();
                            if(in_census&&in_census(candidate))self(self,candidate);
                        }
                    }
                }else if(el=="ProjectReference"){
                    auto inc=attr_val(L"Include");
                    if(!inc.empty()){
                        split_semi(inc,[&](const std::string& part){
                            auto exp=slash_norm(expand_msbuild_value(part,props,find_above));
                            if(exp.find("$(")!=std::string::npos)throw std::runtime_error("CONTEXT_PROJECT_REFERENCE_UNRESOLVED: "+canonical_path(norm));
                            if(exp.find_first_of("*?")!=std::string::npos){
                                for(const auto& m:match_glob_csproj(norm.parent_path(),exp))result.references.push_back(m);
                            }else{
                                result.references.push_back((norm.parent_path()/fs::path(wide(exp))).lexically_normal());
                            }
                        });
                    }
                }else if(el=="Compile"){
                    auto inc=attr_val(L"Include");
                    if(!inc.empty()){
                        split_semi(inc,[&](const std::string& part){
                            auto exp=slash_norm(expand_msbuild_value(part,props,find_above));
                            if(exp.find("$(")==std::string::npos)
                                result.compile_include_patterns.push_back(canonical_path((norm.parent_path()/fs::path(wide(exp))).lexically_normal()));
                        });
                    }
                    auto rem=attr_val(L"Remove");
                    if(!rem.empty()){
                        split_semi(rem,[&](const std::string& part){
                            auto exp=slash_norm(expand_msbuild_value(part,props,find_above));
                            if(exp.find("$(")==std::string::npos)
                                result.compile_remove_patterns.push_back(canonical_path((norm.parent_path()/fs::path(wide(exp))).lexically_normal()));
                        });
                    }
                }
                if(!empty_el)stack.push_back(el);
                else if(prop_name==el){prop_name.clear();prop_val.clear();}
            }else if(type==XmlNodeType_Text||type==XmlNodeType_CDATA){
                if(!prop_name.empty()){const wchar_t* ws{};reader->GetValue(&ws,nullptr);if(ws)prop_val+=utf8(ws);}
            }else if(type==XmlNodeType_EndElement){
                const wchar_t* wn{};reader->GetLocalName(&wn,nullptr);
                std::string el=wn?utf8(wn):"";
                if(!prop_name.empty()&&el==prop_name){
                    const auto evaluated=expand_msbuild_value(trim_ws(prop_val),props,find_above);
                    set_prop(props,prop_name,evaluated);
                    if(lower_path(prop_name)=="enabledefaultcompileitems"&&lower_path(evaluated)=="false")
                        result.enable_default_compile_items=false;
                    prop_name.clear();prop_val.clear();
                }
                if(!stack.empty())stack.pop_back();
            }
        }
        if(FAILED(status))throw std::runtime_error("CONTEXT_PROJECT_XML: "+canonical_path(norm));
        if(saved_dir)set_prop(props,"MSBuildThisFileDirectory",*saved_dir);
        if(saved_file)set_prop(props,"MSBuildThisFile",*saved_file);
        if(saved_name)set_prop(props,"MSBuildThisFileName",*saved_name);
        if(saved_ext)set_prop(props,"MSBuildThisFileExtension",*saved_ext);
    };
    if(find_above&&get_prop(props,"ImportDirectoryBuildProps").value_or("true")!="false"){
        const auto dbp=find_above("Directory.Build.props",canonical_path(csproj_path.parent_path()),false);
        if(!dbp.empty())eval_xml(eval_xml,fs::path(wide(dbp)));
    }
    eval_xml(eval_xml,csproj_path);
    if(find_above&&get_prop(props,"ImportDirectoryBuildTargets").value_or("true")!="false"){
        const auto dbt=find_above("Directory.Build.targets",canonical_path(csproj_path.parent_path()),false);
        if(!dbt.empty())eval_xml(eval_xml,fs::path(wide(dbt)));
    }
    return result;
}
}

Json roslyn_indexed_context(const Project& project,const Json& requested,Cancel* cancel){
    Database database(project);
    if(database.integer("SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name IN('roslyn_source_paths','roslyn_project_paths','roslyn_config_paths')")!=3){
        database.begin();try{
            database.exec(std::string("CREATE INDEX IF NOT EXISTS roslyn_source_paths ON files(root,path COLLATE NOCASE) WHERE ")+source_predicate);
            database.exec(std::string("CREATE INDEX IF NOT EXISTS roslyn_project_paths ON files(root,path COLLATE NOCASE) WHERE ")+project_predicate);
            database.exec(std::string("CREATE INDEX IF NOT EXISTS roslyn_config_paths ON files(root,path COLLATE NOCASE) WHERE ")+config_predicate);
            database.commit();
        }catch(...){database.rollback();throw;}
    }
    const auto generation=database.integer("SELECT COALESCE(MAX(CAST(value AS INTEGER)),0) FROM meta WHERE key='published_generation'");
    if(!generation)throw std::runtime_error("CONTEXT_CENSUS_REQUIRED");
    std::vector<Root> roots;for(const auto& root:project.config.at("roots"))roots.push_back({root.at("id"),project.resolve(root.at("path"))});
    const auto memory=project.config.value("resources",Json::object()).value("memory_mib",4096);
    const uint64_t budget=static_cast<uint64_t>(std::clamp(memory,128,4096))*1024*1024/32;
    constexpr size_t maximum_files=20000,maximum_projects=512;
    auto check=[&]{if(cancel&&cancel->requested)throw std::runtime_error("CONTEXT_CANCELLED");};
    std::map<std::string,std::vector<std::string>> owners;
    // An immediate-directory range in this partial index avoids repeated scans
    // of unrelated inventory rows, including millions of binary inputs.
    Statement at_directory(database,std::string("SELECT absolute_path FROM files INDEXED BY roslyn_project_paths WHERE ")+project_predicate+
        " AND root=? AND path>=? COLLATE NOCASE AND path<? COLLATE NOCASE AND instr(substr(path,length(?)+1),'/')=0 ORDER BY path COLLATE NOCASE LIMIT 513");
    auto owner=[&](const Located& located){
        auto directory=fs::path(wide(located.relative)).parent_path();
        for(;;){
            check();const auto relative=canonical_path(directory);const auto key=located.root+"|"+lower_path(relative);
            auto found=owners.find(key);
            if(found==owners.end()){
                const auto prefix=directory_prefix(relative);std::vector<std::string> projects;
                at_directory.bind({located.root,prefix,prefix+"\xF4\x8F\xBF\xBF",prefix});
                while(at_directory.next()){
                    if(projects.size()>=maximum_projects)throw std::runtime_error("RESOURCE_CONTEXT_PROJECT_LIMIT");
                    projects.push_back(at_directory.row()["absolute_path"]);
                }
                at_directory.reset();
                if(owners.size()>=4096)owners.clear();
                found=owners.emplace(key,std::move(projects)).first;
            }
            if(!found->second.empty())return found->second;
            if(directory.empty())break;directory=directory.parent_path();
        }
        return std::vector<std::string>();
    };
    Statement exists(database,"SELECT EXISTS(SELECT 1 FROM files WHERE absolute_path=? AND (deleted=0 OR COALESCE(json_extract(data,'$.present'),0)=1)) AS present");
    Statement exact_rel(database,"SELECT absolute_path,sha256 FROM files WHERE root=? AND lower(path)=lower(?) AND (deleted=0 OR COALESCE(json_extract(data,'$.present'),0)=1) LIMIT 1");
    auto in_census_fn=[&](const fs::path& p){
        try{locate(p,roots);}catch(...){return false;}
        return exists.first(Json::array({canonical_path(p)}))["present"]==1;
    };
    auto find_above_fn=[&](const std::string& filename,const std::string& start_dir,bool dir_only)->std::string{
        try{
            fs::path start=start_dir.empty()?fs::path():fs::path(wide(start_dir)).lexically_normal();
            if(start.empty())return "";
            auto loc=locate(start/L"__probe",roots);
            fs::path root_path;
            for(const auto& r:roots)if(r.id==loc.root){root_path=r.path.lexically_normal();break;}
            auto cur=start;
            for(int guard=0;guard<64;++guard){
                const auto cand=(cur/fs::path(wide(filename))).lexically_normal();
                Located cloc;
                try{cloc=locate(cand,roots);}catch(...){break;}
                if(cloc.root==loc.root){
                    auto row=exact_rel.first(Json::array({cloc.root,slash_norm(cloc.relative)}));
                    if(!row.is_null())return dir_only?canonical_path(cur):row["absolute_path"].get<std::string>();
                }
                if(lower_path(canonical_path(cur))==lower_path(canonical_path(root_path)))break;
                auto parent=cur.parent_path();if(parent==cur||parent.empty())break;cur=parent;
            }
        }catch(...){}
        return "";
    };
    Statement list_slns(database,std::string("SELECT absolute_path FROM files INDEXED BY roslyn_config_paths WHERE ")+config_predicate+" AND root=? AND (substr(lower(path),-4)='.sln' OR substr(lower(path),-5)='.slnx') ORDER BY path COLLATE NOCASE LIMIT 64");
    auto find_solution_dir_fn=[&](const fs::path& start_dir)->std::string{
        try{
            auto loc=locate(start_dir/L"__probe",roots);
            list_slns.bind({loc.root});
            std::string best_dir;size_t best_len=0;
            const auto start_str=lower_path(ensure_trailing_slash(canonical_path(start_dir)));
            while(list_slns.next()){
                auto sp=fs::path(wide(list_slns.row()["absolute_path"].get<std::string>())).parent_path();
                auto sdir=ensure_trailing_slash(canonical_path(sp));
                auto slow=lower_path(sdir);
                if(start_str.rfind(slow,0)==0&&sdir.size()>best_len){best_len=sdir.size();best_dir=sdir;}
            }
            list_slns.reset();
            return best_dir;
        }catch(...){return "";}
    };
    auto match_glob_csproj_fn=[&](const fs::path& base_dir,const std::string& pattern)->std::vector<fs::path>{
        std::vector<fs::path> matched;
        try{
            auto abs_pat=(base_dir/fs::path(wide(pattern))).lexically_normal();
            auto ploc=locate(abs_pat,roots);
            Statement all_csprojs(database,std::string("SELECT path,absolute_path FROM files INDEXED BY roslyn_project_paths WHERE ")+project_predicate+" AND root=? ORDER BY path COLLATE NOCASE LIMIT 1024");
            all_csprojs.bind({ploc.root});
            const auto norm_pat=slash_norm(ploc.relative);
            while(all_csprojs.next()){
                const auto row=all_csprojs.row();
                if(glob_match_ci(norm_pat,slash_norm(row["path"].get<std::string>())))
                    matched.push_back(fs::path(wide(row["absolute_path"].get<std::string>())).lexically_normal());
            }
            all_csprojs.reset();
        }catch(...){}
        return matched;
    };
    Json result={{"context_files",Json::array()},{"project_files",Json::array()},{"solution_files",Json::array()},
        {"project_config_files",Json::array()},{"context_authoritative",true},{"context_generation",generation},{"context_budget_bytes",budget}};
    std::map<std::string,fs::path> projects;
    std::map<std::string,MsBuildProjectEval> evals;
    std::set<std::string> loose_roots,requested_set;
    std::map<std::string,std::string> config_file_hashes;
    std::vector<fs::path> queue;
    std::set<std::string> queued_projects;
    auto enqueue=[&](const fs::path& path){
        if(!queued_projects.insert(lower_path(canonical_path(path))).second)return;
        if(queued_projects.size()>maximum_projects)throw std::runtime_error("RESOURCE_CONTEXT_PROJECT_LIMIT");
        queue.push_back(path);
    };
    auto record_config=[&](const fs::path& cf){
        const auto norm=canonical_path(cf.lexically_normal());
        const auto key=lower_path(norm);
        if(config_file_hashes.contains(key))return;
        std::string h;
        try{h=hash_file(native_path(cf));}catch(...){h="unreadable";}
        config_file_hashes[key]=h;
        result["project_config_files"].push_back(norm);
    };
    for(const auto& file:requested){check();const auto path=fs::path(wide(file));requested_set.insert(lower_path(canonical_path(path)));
        const auto located=locate(path,roots);const auto candidates=owner(located);
        if(candidates.empty()){
            loose_roots.insert(located.root);
            const auto dir=fs::path(wide(file)).parent_path();
            for(const auto* auto_cfg:{"Directory.Build.props","Directory.Build.targets"}){
                auto found=find_above_fn(auto_cfg,canonical_path(dir),false);
                if(!found.empty())record_config(fs::path(wide(found)));
            }
        }else for(const auto& candidate:candidates)enqueue(fs::path(wide(candidate)));
    }
    for(size_t index=0;index<queue.size();++index){
        check();const auto path=queue[index].lexically_normal();const auto key=lower_path(canonical_path(path));if(projects.contains(key))continue;
        if(projects.size()>=maximum_projects)throw std::runtime_error("RESOURCE_CONTEXT_PROJECT_LIMIT");
        locate(path,roots);
        if(exists.first(Json::array({canonical_path(path)}))["present"]!=1)throw std::runtime_error("CONTEXT_REFERENCE_NOT_IN_CENSUS: "+canonical_path(path));
        std::error_code error;const auto size=fs::file_size(native_path(path),error);if(error||size>4*1024*1024)throw std::runtime_error("RESOURCE_CONTEXT_PROJECT_FILE_LIMIT: "+canonical_path(path));
        projects.emplace(key,path);result["project_files"].push_back(canonical_path(path));
        auto eval=evaluate_msbuild_project(path,roots,in_census_fn,match_glob_csproj_fn,find_above_fn,find_solution_dir_fn);
        for(const auto& cf:eval.config_files)record_config(cf);
        for(const auto& reference:eval.references)enqueue(reference);
        evals.emplace(key,std::move(eval));
    }
    struct Region{std::string root,prefix;std::vector<std::string> includes,removes;bool default_items=true;};std::vector<Region> regions;
    for(const auto& [key,path]:projects){
        const auto located=locate(path,roots);
        const auto& ev=evals[key];
        regions.push_back({located.root,directory_prefix(canonical_path(fs::path(wide(located.relative)).parent_path())),ev.compile_include_patterns,ev.compile_remove_patterns,ev.enable_default_compile_items});
        for(const auto& inc_pat:ev.compile_include_patterns){
            try{
                auto p=fs::path(wide(inc_pat));
                if(inc_pat.find_first_of("*?")!=std::string::npos)p=p.parent_path();
                auto iloc=locate(p,roots);
                auto rel_dir=fs::path(wide(iloc.relative));
                if(inc_pat.find_first_of("*?")==std::string::npos)rel_dir=rel_dir.parent_path();
                regions.push_back({iloc.root,directory_prefix(canonical_path(rel_dir)),{inc_pat},{},false});
            }catch(...){}
        }
    }
    for(const auto& root:loose_roots)regions.push_back({root,"",{},{},true});
    std::set<std::string> included;uint64_t bytes=0;
    Json file_hashes=Json::array();
    Statement sources(database,std::string("SELECT path,absolute_path,bytes,sha256 FROM files INDEXED BY roslyn_source_paths WHERE ")+source_predicate+
        " AND root=? AND path>=? COLLATE NOCASE AND path<? COLLATE NOCASE ORDER BY path COLLATE NOCASE");
    for(const auto& region:regions){
        sources.bind({region.root,region.prefix,region.prefix+"\xF4\x8F\xBF\xBF"});
        while(sources.next()){
            check();const auto row=sources.row();const auto absolute=row["absolute_path"].get<std::string>(),relative=row["path"].get<std::string>();
            const auto key=lower_path(absolute);if(included.contains(key))continue;
            const bool is_req=requested_set.contains(key);
            if(!is_req&&skipped_context_path(relative))continue;
            bool explicit_match=false;
            for(const auto& pat:region.includes)if(glob_match_ci(slash_norm(pat),slash_norm(absolute))){explicit_match=true;break;}
            if(!region.default_items&&!explicit_match&&!is_req)continue;
            bool removed=false;
            if(!is_req)for(const auto& pat:region.removes)if(glob_match_ci(slash_norm(pat),slash_norm(absolute))){removed=true;break;}
            if(removed)continue;
            if(!explicit_match){
                const auto candidates=owner({region.root,relative});
                if(!candidates.empty()&&std::none_of(candidates.begin(),candidates.end(),[&](const auto& candidate){return projects.contains(lower_path(candidate));}))continue;
                if(candidates.empty()&&!loose_roots.contains(region.root))continue;
            }
            const auto file_bytes=row["bytes"].get<uint64_t>();
            if(included.size()>=maximum_files||file_bytes>budget-bytes)throw std::runtime_error("RESOURCE_CONTEXT_LIMIT: recorded C# component exceeds "+std::to_string(budget)+" bytes or "+std::to_string(maximum_files)+" files");
            bytes+=file_bytes;included.insert(key);result["context_files"].push_back(absolute);
            std::string fhash=row["sha256"].is_string()?row["sha256"].get<std::string>():"";
            if(fhash.empty())try{fhash=hash_file(native_path(fs::path(wide(absolute))));}catch(...){}
            file_hashes.push_back(Json::array({key,fhash}));
        }
        sources.reset();
    }
    for(const auto& requested_path:requested_set)if(!included.contains(requested_path))throw std::runtime_error("CONTEXT_REQUEST_NOT_IN_CENSUS: "+requested_path);
    Json cfg_arr=Json::array();for(const auto& [k,v]:config_file_hashes)cfg_arr.push_back(Json::array({k,v}));
    result["context_config_sha256"]=sha256(cfg_arr.dump());
    result["context_files_sha256"]=sha256(file_hashes.dump());
    result["context_bytes"]=bytes;result["context_file_count"]=included.size();result["context_project_count"]=projects.size();
    result["context_mode"]="indexed_project_reference_component";return result;
}
}
