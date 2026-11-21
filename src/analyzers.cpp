#include "migratool/core.hpp"
#include "migratool/gateway.hpp"
#include "migratool/ast_engine.hpp"
#include "migratool/analysis_context.hpp"
#include <clang-c/Index.h>
#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <regex>
#include <cctype>
#include <shellapi.h>
#include <shlwapi.h>
#include <xmllite.h>

namespace mt {
namespace {
std::string lower(std::string s) { for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c))); return s; }
std::vector<std::string> compilation_arguments(const Json& entry) {
    if (entry.contains("arguments"))
        return entry.at("arguments").get<std::vector<std::string>>();
    const auto line = entry.value("command", std::string());
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string::npos || line.find('\0') != std::string::npos ||
        line.find_first_of("\r\n") != std::string::npos)
        throw std::runtime_error("compile_commands necesita arguments o un command de una línea");

    // CMake/Ninja escribe la línea de Windows. Solo se separan los argumentos.
    const auto text = wide(line.substr(first));
    int count = 0;
    const std::unique_ptr<wchar_t*, decltype(&LocalFree)> argv(
        CommandLineToArgvW(text.c_str(), &count), &LocalFree);
    if (!argv || count < 1) throw std::runtime_error("command de compile_commands inválido");
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        const auto value = utf8(argv.get()[index]);
        if (value == "&&" || value == "||" || value == "&" || value == "|" ||
            value == ">" || value == ">>" || value == "<")
            throw std::runtime_error("compile_commands no admite operadores de shell");
        args.push_back(value);
    }
    return args;
}

std::vector<std::string> clang_compile_arguments(const Json& entry, const fs::path& directory) {
    const auto arguments = compilation_arguments(entry);
    if (arguments.empty() || arguments.front().empty())
        throw std::runtime_error("Falta el compilador en compile_commands");
    const auto compiler = lower(path_text(fs::path(wide(arguments.front())).stem()));
    const bool msvc = compiler == "cl" || compiler == "clang-cl";
    const auto input = entry.at("file").get<std::string>();
    const auto input_name = lower(path_text(fs::path(wide(input)).filename()));
    const auto file_key = [&](const std::string& name) {
        auto candidate = fs::path(wide(name));
        if (!candidate.is_absolute()) candidate = directory / candidate;
        return lower(path_text(candidate.lexically_normal()));
    };
    const auto input_key = file_key(input);
    std::vector<std::string> result;
    if (msvc) result.push_back("--driver-mode=cl");
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        const auto& arg = arguments[i];
        const auto lower_arg = lower(arg);
        if (compiler == "cl" && arg.starts_with('@') && lower_arg.ends_with(".obj.modmap")) {
            const auto map_path = directory / fs::path(wide(arg.substr(1)));
            std::error_code error;
            const auto size = fs::file_size(map_path, error);
            if (error) throw std::runtime_error("No se pudo leer el mapa de módulos de MSVC: " + path_text(map_path));
            if (size != 0)
                throw std::runtime_error("El mapa de módulos IFC de MSVC requiere un contexto compatible con libclang: " + path_text(map_path));
            continue;
        }
        if (arg == "-o" || arg == "-MF" || arg == "-MT" || arg == "-MQ" ||
            lower_arg == "/fo" || (msvc && (arg == "-Fo" || arg == "/Fd" || arg == "-Fd" ||
                                             arg == "/Fp" || arg == "-Fp"))) {
            ++i;
            continue;
        }
        if (arg == "-c" || lower_arg == "/c" ||
            (lower_arg.ends_with(input_name) && file_key(arg) == input_key) ||
            (!msvc && (arg == "-MD" || arg == "-MMD" || arg == "-MP")) ||
            lower_arg.starts_with("/fo")) continue;
        if (msvc && (arg.starts_with("-Fo") || arg.starts_with("/Fd") || arg.starts_with("-Fd") ||
                     arg.starts_with("/Fp") || arg.starts_with("-Fp") ||
                     arg.starts_with("/Yu") || arg.starts_with("-Yu") ||
                     arg.starts_with("/Yc") || arg.starts_with("-Yc") ||
                     lower_arg == "/showincludes" || lower_arg == "-showincludes")) continue;
        result.push_back(arg);
    }
    result.push_back("-working-directory=" + path_text(directory));
    return result;
}

Json diagnostic(const std::string& file, const std::string& message) { return {{"file",file},{"message",message}}; }

Json xaml_structure(const fs::path& file) {
    struct XmlInput {
        IStream* stream{};
        IXmlReader* reader{};
        ~XmlInput() {
            if (reader) reader->Release();
            if (stream) stream->Release();
        }
    } xml;
    if (FAILED(SHCreateStreamOnFileEx(file.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE,
                                      FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &xml.stream)))
        throw std::runtime_error("No se pudo abrir el archivo XAML");
    if (FAILED(CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(&xml.reader), nullptr)) ||
        FAILED(xml.reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit)) ||
        FAILED(xml.reader->SetInput(xml.stream)))
        throw std::runtime_error("No se pudo iniciar el lector XAML");

    Json result = {{"protocol", "migratool.analyzer/1"}, {"symbols", Json::array()}, {"dependencies", Json::array()}};
    const std::string xaml_namespace = "http://schemas.microsoft.com/winfx/2006/xaml";
    const auto code_file = fs::path(file.wstring() + L".cs");
    std::error_code probe;
    const auto code_behind = fs::is_regular_file(code_file, probe) ? path_text(code_file) : std::string();
    auto owner = path_text(file.filename());
    std::set<std::string> declarations;
    static const std::set<std::string> events = {
        "Checked", "Click", "CloseRequested", "DoubleTapped", "DragDelta", "Invoked", "ItemClick", "KeyDown", "KeyUp",
        "Loaded", "NavigationCompleted", "NavigationStarting", "Opening", "PasswordChanged", "PointerEntered", "PointerExited",
        "PointerPressed", "PointerReleased", "PrimaryButtonClick", "RightTapped", "SelectionChanged", "Tapped",
        "TextChanged", "Toggled", "Unchecked", "Unloaded", "ViewChanged"
    };
    const auto read_string = [&xml](auto getter) {
        const wchar_t* value{};
        UINT size = 0;
        if (FAILED((xml.reader->*getter)(&value, &size)) || !value) return std::string();
        return utf8(std::wstring(value, size));
    };
    struct Attribute { std::string property, ns, value; UINT line, column; };
    XmlNodeType node{};
    HRESULT status;
    bool has_element = false;
    while ((status = xml.reader->Read(&node)) == S_OK) {
        if (node != XmlNodeType_Element) continue;
        has_element = true;
        const auto element = read_string(&IXmlReader::GetQualifiedName);
        std::vector<Attribute> attributes;
        for (auto at = xml.reader->MoveToFirstAttribute(); at == S_OK; at = xml.reader->MoveToNextAttribute()) {
            UINT line = 1, column = 1;
            xml.reader->GetLineNumber(&line);
            xml.reader->GetLinePosition(&column);
            attributes.push_back({read_string(&IXmlReader::GetLocalName), read_string(&IXmlReader::GetNamespaceUri),
                                  read_string(&IXmlReader::GetValue), std::max(1u, line), std::max(1u, column)});
        }
        xml.reader->MoveToElement();
        for (const auto& attr : attributes)
            if (attr.ns == xaml_namespace && attr.property == "Class" && !attr.value.empty()) owner = attr.value;

        for (const auto& attr : attributes) {
            if (attr.value.empty()) continue;
            if (attr.ns == xaml_namespace && (attr.property == "Class" || attr.property == "Name" || attr.property == "Key")) {
                const std::string kind = attr.property == "Class" ? "XamlClass" : attr.property == "Name" ? "XamlElement" : "XamlResource";
                const auto qualified = attr.property == "Class" ? attr.value : owner + "." + attr.value;
                auto signature = element + " x:" + attr.property;
                if (!declarations.insert(kind + "|" + qualified).second)
                    signature += " @" + std::to_string(attr.line) + ":" + std::to_string(attr.column);
                result["symbols"].push_back({{"name", attr.value}, {"qualified_name", qualified}, {"kind", kind},
                    {"signature", signature}, {"line", attr.line}, {"column", attr.column}});
            }

            std::string kind;
            const auto first = attr.value.find_first_not_of(" \t\r\n");
            if (first != std::string::npos && attr.value[first] == '{') {
                const auto end = attr.value.find_first_of(" \t\r\n,}", first + 1);
                const auto markup = attr.value.substr(first + 1, end == std::string::npos ? end : end - first - 1);
                if (markup == "Binding" || markup == "TemplateBinding" || markup.ends_with(":Bind")) kind = "xaml_binding";
                if (markup == "StaticResource" || markup == "ThemeResource") kind = "xaml_resource_reference";
            } else if (attr.ns.empty() && events.contains(attr.property) &&
                       attr.value.find_first_of(" \t\r\n{}") == std::string::npos) kind = "xaml_event";
            if (kind.empty()) continue;
            Json ref = {{"kind", kind}, {"name", attr.value}, {"attribute", attr.property}, {"element", element},
                        {"owner", owner}, {"line", attr.line}, {"column", attr.column},
                        {"resolution", "XAML_DECLARED"}, {"resolved", false}};
            if (kind == "xaml_event" && !code_behind.empty()) ref["candidate_path"] = code_behind;
            result["dependencies"].push_back(std::move(ref));
        }
    }
    if (FAILED(status) || !has_element) {
        UINT line = 1;
        xml.reader->GetLineNumber(&line);
        throw std::runtime_error("XML de la interfaz inválido en la línea " + std::to_string(line));
    }
    return result;
}
bool generated(const fs::path& p) {
    auto n = lower(path_text(p.filename()));
    if (n.ends_with(".g.cs") || n.ends_with(".g.i.cs") || n.ends_with(".designer.cs") || n.ends_with(".min.js")) return true;
    std::ifstream in(p, std::ios::binary); char buffer[2048]{}; in.read(buffer,sizeof(buffer));
    std::string head(buffer, static_cast<size_t>(in.gcount()));
    return head.find("<auto-generated") != std::string::npos || head.find("@generated") != std::string::npos;
}
std::string replace(std::string text, const std::string& from, const std::string& to) {
    size_t at = 0; while ((at=text.find(from,at)) != std::string::npos) { text.replace(at,from.size(),to); at += to.size(); } return text;
}
void merge_result(Json& scan, Json& file, const Json& result, const std::string& engine) {
    if (result.value("protocol", "") != "migratool.analyzer/1") throw std::runtime_error("Protocolo de analizador inválido");
    for (auto symbol : result.at("symbols")) {
        symbol.at("name").get<std::string>(); symbol.at("kind").get<std::string>();
        auto line = symbol.value("line",1);
        if (line < 1) throw std::runtime_error("Línea inválida en símbolo");
        symbol["root"] = file["root"]; symbol["file"] = file["path"]; symbol["absolute_path"] = file["absolute_path"];
        symbol["file_id"] = file["id"]; symbol["engine"] = engine;
        symbol["id"] = file["id"].get<std::string>() + "#" + sha256(symbol.value("qualified_name",symbol["name"].get<std::string>()) + "|" + symbol.value("signature","") + "|" + symbol["kind"].get<std::string>()).substr(0,24);
        scan["symbols"].push_back(symbol);
    }
    file["analysis"] = result.value("errors",0) > 0 ? "PARTIAL" : "ANALYZED";
    for (const auto& d : result.value("diagnostics",Json::array())) scan["diagnostics"].push_back({{"file",file["absolute_path"]},{"engine",engine},{"message",d}});
    for (const auto& d : result.value("semantic_diagnostics",Json::array())) scan["diagnostics"].push_back({{"file",file["absolute_path"]},{"engine",engine},{"severity","semantic_error"},{"message",d}});
    for (auto d : result.value("dependencies",Json::array())) {
        d["file_id"] = file["id"]; d["engine"] = engine; scan["dependencies"].push_back(d);
    }
}

// libclang mantiene el parser nativo fuera de proceso de construcción y evita AST JSON de gran tamaño.
struct Clang {
    HMODULE dll{};
#define MT_CLANG_FUNCS(X) \
    X(clang_createIndex) X(clang_disposeIndex) X(clang_parseTranslationUnit2) X(clang_disposeTranslationUnit) \
    X(clang_getTranslationUnitCursor) X(clang_visitChildren) X(clang_getCursorLocation) X(clang_getSpellingLocation) \
    X(clang_getFileName) X(clang_getCString) X(clang_disposeString) X(clang_getCursorSpelling) \
    X(clang_getCursorDisplayName) X(clang_getCursorUSR) X(clang_getCursorSemanticParent) X(clang_getCursorKind) \
    X(clang_getCursorKindSpelling) X(clang_getCursorType) X(clang_getTypeSpelling) X(clang_isDeclaration) \
    X(clang_getNumDiagnostics) X(clang_getDiagnostic) X(clang_formatDiagnostic) X(clang_defaultDiagnosticDisplayOptions) \
    X(clang_disposeDiagnostic) X(clang_getDiagnosticSeverity) X(clang_getClangVersion) X(clang_isCursorDefinition) \
    X(clang_getCursorExtent) X(clang_getRangeStart) X(clang_getRangeEnd) X(clang_getCursorReferenced) \
    X(clang_CXXMethod_isVirtual) X(clang_CXXMethod_isPureVirtual) X(clang_CXXMethod_isStatic) \
    X(clang_getOverriddenCursors) X(clang_disposeOverriddenCursors) X(clang_equalCursors) X(clang_Cursor_isNull) \
    X(clang_Cursor_getSpellingNameRange) X(clang_getIncludedFile) X(clang_getInclusions)
#define FIELD(fn) decltype(&fn) fn##_p{};
    MT_CLANG_FUNCS(FIELD)
#undef FIELD
    Clang() {
        dll = LoadLibraryExW(MIGRATOOL_CLANG_DLL, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!dll) throw std::runtime_error("libclang no disponible en su ruta configurada");
#define LOAD(fn) fn##_p = reinterpret_cast<decltype(fn##_p)>(GetProcAddress(dll,#fn)); if (!fn##_p) throw std::runtime_error("libclang sin " #fn);
        MT_CLANG_FUNCS(LOAD)
#undef LOAD
    }
    ~Clang() { if(dll) FreeLibrary(dll); }
    std::string str(CXString s) { auto p=clang_getCString_p(s); std::string out = p?p:""; clang_disposeString_p(s); return out; }
};

std::string qualified_cursor_name(Clang& c, CXCursor cur) {
    auto q = c.str(c.clang_getCursorSpelling_p(cur));
    if (q.empty()) return q;
    auto parent = c.clang_getCursorSemanticParent_p(cur);
    for (int depth = 0; depth < 32; ++depth) {
        auto pk = c.clang_getCursorKind_p(parent);
        if (pk == CXCursor_TranslationUnit || pk == CXCursor_FirstInvalid || c.clang_Cursor_isNull_p(parent)) break;
        auto pn = c.str(c.clang_getCursorSpelling_p(parent));
        if (pn.empty()) break;
        q = pn + "::" + q;
        parent = c.clang_getCursorSemanticParent_p(parent);
    }
    return q;
}

struct EnclosingScope {
    std::string usr;
    std::string qualified_name;
    std::string symbol_id;
};

struct Visit {
    Clang* c;
    Json* scan;
    std::map<std::string,Json*>* files;
    std::map<std::string,std::string>* contents;
    std::set<std::string>* ids;
    std::set<std::string>* ref_positions;
    std::vector<EnclosingScope> stack;
    Cancel* cancel;
    std::map<CXFile, std::map<std::string, Json*>::const_iterator> files_by_handle;
};

CXChildVisitResult visitor(CXCursor cur, CXCursor, CXClientData opaque) {
    auto& v = *static_cast<Visit*>(opaque); auto& c = *v.c;
    if (v.cancel && v.cancel->requested) return CXChildVisit_Break;
    CXFile file{}; unsigned line=0, col=0, offset=0;
    c.clang_getSpellingLocation_p(c.clang_getCursorLocation_p(cur),&file,&line,&col,&offset);
    if (!file) {
        c.clang_visitChildren_p(cur, visitor, opaque);
        return CXChildVisit_Continue;
    }
    const auto [location, first_visit] = v.files_by_handle.try_emplace(file, v.files->cend());
    if (first_visit) {
        const auto path = lower(path_text(fs::weakly_canonical(fs::path(wide(c.str(c.clang_getFileName_p(file)))))));
        location->second = v.files->find(path);
    }
    const auto fit = location->second;
    if (fit == v.files->end()) return CXChildVisit_Continue;
    const auto& path = fit->first;
    auto& f = *fit->second; auto kind = c.clang_getCursorKind_p(cur);
    const std::string& content = (*v.contents)[path];

    CXSourceRange extent = c.clang_getCursorExtent_p(cur);
    CXFile sf{}, ef{};
    unsigned s_line=line, s_col=col, s_off=offset, e_line=line, e_col=col, e_off=offset;
    c.clang_getSpellingLocation_p(c.clang_getRangeStart_p(extent), &sf, &s_line, &s_col, &s_off);
    c.clang_getSpellingLocation_p(c.clang_getRangeEnd_p(extent), &ef, &e_line, &e_col, &e_off);
    if (sf != file) { s_line = line; s_col = col; s_off = offset; }
    if (ef != file || e_off < s_off) { e_line = s_line; e_col = s_col; e_off = s_off; }
    if (e_off < content.size() && content[e_off] == ';') { ++e_off; ++e_col; }

    CXSourceRange nr = c.clang_Cursor_getSpellingNameRange_p(cur, 0, 0);
    CXFile nsf{}, nef{};
    unsigned nl=line, nc=col, noff=offset, nel=line, nec=col, neoff=offset;
    c.clang_getSpellingLocation_p(c.clang_getRangeStart_p(nr), &nsf, &nl, &nc, &noff);
    c.clang_getSpellingLocation_p(c.clang_getRangeEnd_p(nr), &nef, &nel, &nec, &neoff);

    bool pushed = false;
    if (c.clang_isDeclaration_p(kind) && kind != CXCursor_ParmDecl && kind != CXCursor_TemplateTypeParameter && kind != CXCursor_NonTypeTemplateParameter) {
        auto name = c.str(c.clang_getCursorSpelling_p(cur));
        if (!name.empty()) {
            auto signature = c.str(c.clang_getCursorDisplayName_p(cur)) + " : " + c.str(c.clang_getTypeSpelling_p(c.clang_getCursorType_p(cur)));
            auto usr = c.str(c.clang_getCursorUSR_p(cur));
            auto id = f["id"].get<std::string>()+"#"+sha256(usr.empty()?signature+std::to_string(line):usr).substr(0,24);
            auto q = qualified_cursor_name(c, cur);
            if (noff == 0 || nsf != file || neoff <= noff) {
                noff = offset;
                neoff = offset + static_cast<unsigned>(name.size());
                nl = line; nc = col;
            }
            v.ref_positions->insert(path + ":" + std::to_string(noff));
            if (v.ids->insert(id).second) {
                bool is_virt = (kind == CXCursor_CXXMethod) && (c.clang_CXXMethod_isVirtual_p(cur) != 0);
                bool is_pure = (kind == CXCursor_CXXMethod) && (c.clang_CXXMethod_isPureVirtual_p(cur) != 0);
                bool is_stat = (kind == CXCursor_CXXMethod) && (c.clang_CXXMethod_isStatic_p(cur) != 0);
                std::string overrides_key, overrides_name;
                if (kind == CXCursor_CXXMethod) {
                    CXCursor* overridden = nullptr; unsigned num_overridden = 0;
                    c.clang_getOverriddenCursors_p(cur, &overridden, &num_overridden);
                    if (num_overridden > 0 && overridden) {
                        overrides_key = c.str(c.clang_getCursorUSR_p(overridden[0]));
                        overrides_name = qualified_cursor_name(c, overridden[0]);
                    }
                    if (overridden) c.clang_disposeOverriddenCursors_p(overridden);
                }
                v.scan->at("symbols").push_back({
                    {"id",id},{"name",name},{"qualified_name",q},{"signature",signature},
                    {"semantic_key",usr},
                    {"kind",c.str(c.clang_getCursorKindSpelling_p(kind))},
                    {"line",s_line},{"column",s_col},{"end_line",e_line},{"end_column",e_col},
                    {"offset",s_off},{"end_offset",e_off},
                    {"name_line",nl},{"name_column",nc},{"name_offset",noff},{"name_end_offset",neoff},
                    {"is_virtual",is_virt},{"is_pure_virtual",is_pure},{"is_static",is_stat},
                    {"is_override",!overrides_key.empty()},{"overrides_key",overrides_key},{"overrides_name",overrides_name},
                    {"base_types",Json::array()},
                    {"root",f["root"]},{"file",f["path"]},{"absolute_path",f["absolute_path"]},
                    {"file_id",f["id"]},{"engine","libclang"},{"definition",c.clang_isCursorDefinition_p(cur)!=0},
                    {"semantic_resolution",true}
                });
            }
            if (kind != CXCursor_FieldDecl && kind != CXCursor_VarDecl && kind != CXCursor_EnumConstantDecl) {
                v.stack.push_back({usr, q, id});
                pushed = true;
            }
        }
    }

    std::string caller_key = v.stack.empty() ? "" : v.stack.back().usr;
    std::string caller_q = v.stack.empty() ? "" : v.stack.back().qualified_name;

    if (kind == CXCursor_InclusionDirective || kind == CXCursor_CallExpr || kind == CXCursor_CXXBaseSpecifier) {
        auto name = c.str(c.clang_getCursorSpelling_p(cur));
        if (!name.empty()) {
            std::string included_path;
            if (kind == CXCursor_InclusionDirective) {
                if (CXFile inc_file = c.clang_getIncludedFile_p(cur)) {
                    auto raw_inc = c.str(c.clang_getFileName_p(inc_file));
                    if (!raw_inc.empty()) {
                        try { included_path = path_text(fs::weakly_canonical(fs::path(wide(raw_inc)))); }
                        catch (...) { included_path = raw_inc; }
                    }
                }
            }
            CXCursor ref = c.clang_getCursorReferenced_p(cur);
            bool has_ref = !c.clang_Cursor_isNull_p(ref) && !c.clang_equalCursors_p(ref, cur);
            std::string target_usr = has_ref ? c.str(c.clang_getCursorUSR_p(ref)) : "";
            std::string target_name = has_ref ? c.str(c.clang_getCursorSpelling_p(ref)) : name;
            std::string target_q = has_ref ? qualified_cursor_name(c, ref) : name;
            std::string target_sig = has_ref ? (c.str(c.clang_getCursorDisplayName_p(ref)) + " : " + c.str(c.clang_getTypeSpelling_p(c.clang_getCursorType_p(ref)))) : "";
            bool is_virt = has_ref && (c.clang_getCursorKind_p(ref) == CXCursor_CXXMethod) && (c.clang_CXXMethod_isVirtual_p(ref) != 0);
            bool resolved = !target_usr.empty() || !included_path.empty();

            unsigned id_off = noff, id_end = neoff, id_line = nl, id_col = nc;
            std::string short_id = target_name.empty() ? name : target_name;
            auto colon_pos = short_id.rfind("::");
            if (colon_pos != std::string::npos) short_id = short_id.substr(colon_pos + 2);
            if ((id_end <= id_off || nsf != file || (id_end - id_off) != short_id.size()) && s_off < content.size() && e_off <= content.size() && e_off > s_off && !short_id.empty()) {
                auto slice = content.substr(s_off, e_off - s_off);
                auto found = slice.find(short_id);
                if (found != std::string::npos) {
                    id_off = s_off + static_cast<unsigned>(found);
                    id_end = id_off + static_cast<unsigned>(short_id.size());
                }
            }
            v.ref_positions->insert(path + ":" + std::to_string(id_off));

            if (kind == CXCursor_CXXBaseSpecifier && !v.stack.empty()) {
                for (auto it = v.scan->at("symbols").rbegin(); it != v.scan->at("symbols").rend(); ++it) {
                    if ((*it)["id"] == v.stack.back().symbol_id) {
                        (*it)["base_types"].push_back({
                            {"name", short_id},
                            {"qualified_name", target_q},
                            {"semantic_key", target_usr},
                            {"kind", "base_class"}
                        });
                        break;
                    }
                }
            }

            Json dep_entry = {
                {"file_id",f["id"]},{"line",line},{"column",col},
                {"offset",s_off},{"end_offset",e_off},
                {"identifier_name",short_id},{"identifier_line",id_line},{"identifier_column",id_col},
                {"identifier_offset",id_off},{"identifier_end_offset",id_end},
                {"kind",c.str(c.clang_getCursorKindSpelling_p(kind))},{"name",name},
                {"target_name",target_name},{"target_qualified_name",target_q},
                {"target_signature",target_sig},{"target_key",target_usr},
                {"caller_key",caller_key},{"caller_qualified_name",caller_q},
                {"resolved",resolved},
                {"resolution",!included_path.empty() ? "CLANG_INCLUSION" : (resolved ? "SEMANTIC_EXACT" : "UNRESOLVED_EXTERNAL")},
                {"dispatch",is_virt ? "virtual_dispatch" : "direct"},
                {"engine","libclang"}
            };
            if (!included_path.empty()) dep_entry["target_path"] = included_path;
            v.scan->at("dependencies").push_back(std::move(dep_entry));
        }
    } else if (kind == CXCursor_DeclRefExpr || kind == CXCursor_MemberRefExpr || kind == CXCursor_TypeRef) {
        CXCursor ref = c.clang_getCursorReferenced_p(cur);
        if (!c.clang_Cursor_isNull_p(ref)) {
            auto ref_kind = c.clang_getCursorKind_p(ref);
            if (ref_kind != CXCursor_ParmDecl && ref_kind != CXCursor_VarDecl) {
                auto target_usr = c.str(c.clang_getCursorUSR_p(ref));
                auto target_name = c.str(c.clang_getCursorSpelling_p(ref));
                unsigned id_off = (nsf == file && neoff > noff) ? noff : offset;
                unsigned id_end = (nsf == file && neoff > noff) ? neoff : (offset + static_cast<unsigned>(target_name.size()));
                std::string pos_key = path + ":" + std::to_string(id_off);
                if (!target_usr.empty() && !target_name.empty() && v.ref_positions->insert(pos_key).second) {
                    bool is_virt = (ref_kind == CXCursor_CXXMethod) && (c.clang_CXXMethod_isVirtual_p(ref) != 0);
                    v.scan->at("dependencies").push_back({
                        {"file_id",f["id"]},{"line",line},{"column",col},
                        {"offset",id_off},{"end_offset",id_end},
                        {"identifier_name",target_name},{"identifier_line",line},{"identifier_column",col},
                        {"identifier_offset",id_off},{"identifier_end_offset",id_end},
                        {"kind","reference"},{"name",target_name},
                        {"target_name",target_name},{"target_qualified_name",qualified_cursor_name(c, ref)},
                        {"target_key",target_usr},
                        {"caller_key",caller_key},{"caller_qualified_name",caller_q},
                        {"resolved",true},{"resolution","SEMANTIC_EXACT"},
                        {"dispatch",is_virt ? "virtual_dispatch" : "direct"},
                        {"engine","libclang"}
                    });
                }
            }
        }
    }

    c.clang_visitChildren_p(cur, visitor, opaque);
    if (pushed) v.stack.pop_back();
    return CXChildVisit_Continue;
}

struct LineIndex {
    std::vector<size_t> starts;
    explicit LineIndex(const std::string& text) {
        starts.push_back(0);
        for (size_t i = 0; i < text.size(); ++i)
            if (text[i] == '\n') starts.push_back(i + 1);
    }
    std::pair<int,int> line_col(size_t offset) const {
        auto it = std::upper_bound(starts.begin(), starts.end(), offset);
        size_t idx = (it == starts.begin()) ? 0 : static_cast<size_t>(std::distance(starts.begin(), it) - 1);
        return {static_cast<int>(idx + 1), static_cast<int>(offset - starts[idx] + 1)};
    }
};

size_t find_matching_token(const std::vector<mt::ast::Token>& toks, size_t start_idx,
                           std::string_view open_str, std::string_view close_str) {
    int depth = 0;
    for (size_t i = start_idx; i < toks.size() && toks[i].kind != mt::ast::TokenKind::EndOfFile; ++i) {
        if (toks[i].text == open_str) ++depth;
        else if (toks[i].text == close_str) {
            --depth;
            if (depth == 0) return i;
        }
    }
    return toks.size() - 1;
}

void collect_calls_from_ast_tree(const mt::ast::AstNode& node, const LineIndex& li,
                                 size_t decl_name_off, const std::string& sem_key,
                                 const std::string& qname, Json& dependencies) {
    static const std::set<std::string> non_call_keywords = {
        "if","for","while","switch","catch","return","sizeof","typeof","def","class","fn",
        "function","match","loop","pub","async","await","import","from","where","select",
        "super","new","delete","alignof","decltype","static_assert"
    };
    if (node.kind == mt::ast::AstNodeKind::CallExpr && !node.children.empty()) {
        const auto& callee_node = node.children[0];
        std::string callee_name = callee_node.text;
        size_t id_off = callee_node.offset;
        size_t id_end = callee_node.end_offset;
        if (callee_node.kind == mt::ast::AstNodeKind::MemberExpr && callee_node.children.size() == 2) {
            callee_name = callee_node.children[1].text;
            id_off = callee_node.children[1].offset;
            id_end = callee_node.children[1].end_offset;
        } else if (callee_node.kind == mt::ast::AstNodeKind::GenericExpr && !callee_node.children.empty()) {
            const auto& base = callee_node.children[0];
            if (base.kind == mt::ast::AstNodeKind::MemberExpr && base.children.size() == 2) {
                callee_name = base.children[1].text;
                id_off = base.children[1].offset;
                id_end = base.children[1].end_offset;
            } else {
                callee_name = base.text;
                id_off = base.offset;
                id_end = base.end_offset;
            }
        }
        if (!callee_name.empty() && id_off != decl_name_off && !non_call_keywords.contains(callee_name)) {
            auto [cl, cc] = li.line_col(id_off);
            dependencies.push_back({
                {"kind", "call"}, {"name", callee_name}, {"target_name", callee_name},
                {"target_qualified_name", callee_name}, {"target_key", ""},
                {"caller_key", sem_key}, {"caller_qualified_name", qname},
                {"line", cl}, {"column", cc},
                {"offset", node.offset}, {"end_offset", node.end_offset},
                {"identifier_name", callee_name}, {"identifier_line", cl}, {"identifier_column", cc},
                {"identifier_offset", id_off}, {"identifier_end_offset", id_end},
                {"resolved", false}, {"resolution", "SYNTACTIC_AST"}, {"dispatch", "direct"}
            });
        }
    }
    for (const auto& ch : node.children) {
        collect_calls_from_ast_tree(ch, li, decl_name_off, sem_key, qname, dependencies);
    }
}
} // anonymous namespace

// H-02: Analizador sintáctico basado en Lexer + Árbol Sintáctico (AST) sin regex.
// Ignora por completo código dentro de comentarios (#, //, /* */, --) y literales de cadena ("""...""", r#"..."#, "...", '...', `...`).
Json syntactic_ast_scan_file(const fs::path& file_path, const std::string& lang) {
    std::string text = read_text(file_path, 16 * 1024 * 1024);
    LineIndex li(text);
    auto spec = mt::ast::parse_language_spec(lang);
    auto toks = mt::ast::lex_source(text, spec.family, /*allow_metavars=*/false, /*keep_comments=*/false);
    Json symbols = Json::array();
    Json dependencies = Json::array();

    auto line_indent_columns = [&](int line_1based) -> int {
        if (line_1based < 1 || static_cast<size_t>(line_1based - 1) >= li.starts.size()) return 0;
        size_t ls = li.starts[static_cast<size_t>(line_1based - 1)];
        int indent = 0;
        while (ls < text.size() && (text[ls] == ' ' || text[ls] == '\t')) {
            indent += (text[ls] == '\t') ? 4 : 1;
            ++ls;
        }
        return indent;
    };

    auto add_sym = [&](const std::string& name, const std::string& qname, const std::string& sig,
                       const std::string& kind, size_t start_off, size_t end_off, size_t name_off,
                       size_t body_start_off = 0) {
        if (end_off <= start_off || end_off > text.size()) return;
        auto [sl, sc] = li.line_col(start_off);
        auto [el, ec] = li.line_col(end_off);
        auto [nl, nc] = li.line_col(name_off);
        std::string sem_key = lang + ":" + qname + sig;
        symbols.push_back({
            {"name", name}, {"qualified_name", qname}, {"signature", sig},
            {"semantic_key", sem_key}, {"kind", kind},
            {"line", sl}, {"column", sc}, {"end_line", el}, {"end_column", ec},
            {"offset", start_off}, {"end_offset", end_off},
            {"name_line", nl}, {"name_column", nc},
            {"name_offset", name_off}, {"name_end_offset", name_off + name.size()},
            {"branches", Json::array()}, {"semantic_resolution", false},
            {"analysis_level", "verified_syntactic_ast"}
        });

        size_t call_scan_start = (body_start_off >= start_off && body_start_off < end_off) ? body_start_off : start_off;
        if (end_off > call_scan_start && (end_off - call_scan_start) <= 262144 &&
            (kind == "function" || kind == "method" || kind == "fn" || kind == "func" || kind == "procedure" || kind == "trigger")) {
            std::string_view slice(text.data() + call_scan_start, end_off - call_scan_start);
            auto body_toks = mt::ast::lex_source(slice, spec.family, false, false);
            for (auto& bt : body_toks) {
                bt.offset += call_scan_start;
                bt.end_offset += call_scan_start;
            }
            mt::ast::AstParser parser(body_toks);
            auto ast_root = parser.parse_root();
            collect_calls_from_ast_tree(ast_root, li, name_off, sem_key, qname, dependencies);
        }
    };

    if (spec.family == "Python") {
        // Análisis por flujo de tokens con seguimiento de nivel de paréntesis y bloques de indentación PEP-8
        struct PyClassScope { std::string name; int indent; size_t end_off; };
        std::vector<PyClassScope> class_stack;
        int paren_depth = 0;
        bool at_logical_line_start = true;
        std::optional<size_t> decorator_start_off;

        for (size_t i = 0; i < toks.size() && toks[i].kind != mt::ast::TokenKind::EndOfFile; ++i) {
            const auto& t = toks[i];
            if (t.kind == mt::ast::TokenKind::Newline) {
                if (paren_depth == 0) at_logical_line_start = true;
                continue;
            }
            if (t.text == "(" || t.text == "[" || t.text == "{") { ++paren_depth; at_logical_line_start = false; continue; }
            if (t.text == ")" || t.text == "]" || t.text == "}") { if (paren_depth > 0) --paren_depth; at_logical_line_start = false; continue; }
            if (!at_logical_line_start) continue;
            at_logical_line_start = false;

            int indent = line_indent_columns(t.line);
            while (!class_stack.empty() && indent <= class_stack.back().indent) {
                class_stack.pop_back();
            }

            if (t.text == "@") {
                if (!decorator_start_off) decorator_start_off = t.offset;
                continue;
            }

            size_t kw_idx = i;
            if (t.text == "async" && i + 1 < toks.size() && toks[i + 1].text == "def") {
                kw_idx = i + 1;
            }
            const auto& kw_tok = toks[kw_idx];
            if ((kw_tok.text == "def" || kw_tok.text == "class") && kw_idx + 1 < toks.size() &&
                toks[kw_idx + 1].kind == mt::ast::TokenKind::Identifier) {
                size_t start_off = decorator_start_off.value_or(t.offset);
                decorator_start_off.reset();
                const auto& name_tok = toks[kw_idx + 1];
                std::string name = name_tok.text;
                std::string sig;
                size_t cur = kw_idx + 2;
                if (cur < toks.size() && toks[cur].text == "(") {
                    size_t close_p = find_matching_token(toks, cur, "(", ")");
                    sig = text.substr(toks[cur].offset, toks[close_p].end_offset - toks[cur].offset);
                    cur = close_p + 1;
                }
                // Avanzar hasta ':' de cierre de cabecera
                int hdr_paren = 0;
                while (cur < toks.size() && toks[cur].kind != mt::ast::TokenKind::EndOfFile) {
                    if (toks[cur].text == "(" || toks[cur].text == "[") ++hdr_paren;
                    else if (toks[cur].text == ")" || toks[cur].text == "]") { if (hdr_paren > 0) --hdr_paren; }
                    else if (hdr_paren == 0 && toks[cur].text == ":") break;
                    ++cur;
                }
                size_t colon_end_off = (cur < toks.size()) ? toks[cur].end_offset : name_tok.end_offset;
                int colon_line = (cur < toks.size()) ? toks[cur].line : name_tok.line;

                // Determinar el final del bloque Python usando los tokens siguientes fuera de paréntesis
                size_t block_end_off = colon_end_off;
                int scan_paren = 0;
                bool scan_line_start = false;
                bool same_line_suite = false;
                for (size_t j = cur + 1; j < toks.size() && toks[j].kind != mt::ast::TokenKind::EndOfFile; ++j) {
                    const auto& st = toks[j];
                    if (st.kind == mt::ast::TokenKind::Newline) {
                        if (scan_paren == 0) {
                            if (same_line_suite) break;
                            scan_line_start = true;
                        }
                        continue;
                    }
                    if (st.line == colon_line && !scan_line_start) {
                        same_line_suite = true;
                    }
                    if (scan_line_start && scan_paren == 0) {
                        int st_indent = line_indent_columns(st.line);
                        if (st_indent <= indent) break;
                        scan_line_start = false;
                    }
                    if (st.text == "(" || st.text == "[" || st.text == "{") ++scan_paren;
                    else if (st.text == ")" || st.text == "]" || st.text == "}") { if (scan_paren > 0) --scan_paren; }
                    block_end_off = st.end_offset;
                }

                if (kw_tok.text == "class") {
                    std::string qname = class_stack.empty() ? name : (class_stack.back().name + "." + name);
                    add_sym(name, qname, sig, "class", start_off, block_end_off, name_tok.offset, colon_end_off);
                    class_stack.push_back({qname, indent, block_end_off});
                } else {
                    std::string qname = class_stack.empty() ? name : (class_stack.back().name + "." + name);
                    std::string kind = class_stack.empty() ? "function" : "method";
                    add_sym(name, qname, sig, kind, start_off, block_end_off, name_tok.offset, colon_end_off);
                }
            } else {
                decorator_start_off.reset();
            }
        }
    } else if (spec.family == "Rust") {
        struct RsScope { std::string name; int brace_depth; };
        std::vector<RsScope> scopes;
        int brace_depth = 0;
        for (size_t i = 0; i < toks.size() && toks[i].kind != mt::ast::TokenKind::EndOfFile; ++i) {
            const auto& t = toks[i];
            if (t.text == "{") { ++brace_depth; continue; }
            if (t.text == "}") {
                --brace_depth;
                while (!scopes.empty() && brace_depth < scopes.back().brace_depth) scopes.pop_back();
                continue;
            }
            if (t.text == "fn" || t.text == "struct" || t.text == "enum" || t.text == "trait" || t.text == "impl") {
                size_t start_off = t.offset;
                // Retroceder modificadores (pub, async, unsafe, const) en la misma declaración
                size_t back = i;
                while (back > 0) {
                    const auto& prev = toks[back - 1];
                    if (prev.kind == mt::ast::TokenKind::Newline) { --back; continue; }
                    if (prev.text == "pub" || prev.text == "async" || prev.text == "unsafe" || prev.text == "const") {
                        start_off = prev.offset;
                        --back;
                    } else if (prev.text == ")" && back >= 3 && toks[back - 3].text == "pub") {
                        start_off = toks[back - 3].offset;
                        back -= 3;
                    } else break;
                }

                size_t cur = i + 1;
                while (cur < toks.size() && toks[cur].kind == mt::ast::TokenKind::Newline) ++cur;
                if (t.text == "impl" && cur < toks.size() && toks[cur].text == "<") {
                    cur = find_matching_token(toks, cur, "<", ">") + 1;
                    while (cur < toks.size() && toks[cur].kind == mt::ast::TokenKind::Newline) ++cur;
                }
                if (cur >= toks.size() || toks[cur].kind != mt::ast::TokenKind::Identifier) continue;
                size_t name_idx = cur;
                if (t.text == "impl") {
                    // Si es `impl Trait for Type`, tomar Type como nombre de ámbito
                    size_t scan = cur + 1;
                    while (scan < toks.size() && toks[scan].text != "{" && toks[scan].text != ";") {
                        if (toks[scan].text == "for" && scan + 1 < toks.size() && toks[scan + 1].kind == mt::ast::TokenKind::Identifier) {
                            name_idx = scan + 1;
                            break;
                        }
                        ++scan;
                    }
                }
                const auto& name_tok = toks[name_idx];
                std::string name = name_tok.text;
                cur = name_idx + 1;
                while (cur < toks.size() && toks[cur].kind == mt::ast::TokenKind::Newline) ++cur;
                if (cur < toks.size() && toks[cur].text == "<") {
                    cur = find_matching_token(toks, cur, "<", ">") + 1;
                }
                std::string sig;
                while (cur < toks.size() && toks[cur].kind == mt::ast::TokenKind::Newline) ++cur;
                if (cur < toks.size() && toks[cur].text == "(") {
                    size_t close_p = find_matching_token(toks, cur, "(", ")");
                    sig = text.substr(toks[cur].offset, toks[close_p].end_offset - toks[cur].offset);
                    cur = close_p + 1;
                }
                while (cur < toks.size() && toks[cur].kind != mt::ast::TokenKind::EndOfFile &&
                       toks[cur].text != "{" && toks[cur].text != ";") {
                    ++cur;
                }
                size_t end_off = name_tok.end_offset;
                size_t body_start_off = name_tok.end_offset;
                if (cur < toks.size() && toks[cur].text == "{") {
                    body_start_off = toks[cur].offset;
                    size_t close_b = find_matching_token(toks, cur, "{", "}");
                    end_off = toks[close_b].end_offset;
                    if (t.text == "impl" || t.text == "trait") {
                        scopes.push_back({name, brace_depth + 1});
                    }
                } else if (cur < toks.size() && toks[cur].text == ";") {
                    end_off = toks[cur].end_offset;
                }
                std::string qname = (!scopes.empty() && t.text == "fn") ? (scopes.back().name + "::" + name) : name;
                if (sig.empty()) sig = t.text;
                add_sym(name, qname, sig, t.text, start_off, end_off, name_tok.offset, body_start_off);
            }
        }
    } else if (spec.family == "TypeScript" || spec.family == "JavaScript" || spec.family == "Go" ||
               spec.family == "Java" || spec.family == "Kotlin" || spec.family == "Swift" ||
               spec.family == "PowerShell" || spec.family == "Dart" || spec.family == "PHP" ||
               spec.family == "C" || spec.family == "C++" || spec.family == "C/C++" || spec.family == "C#") {
        struct BraceScope { std::string name; int brace_depth; };
        std::vector<BraceScope> scopes;
        int brace_depth = 0;
        auto low_kw = [](std::string s) {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        for (size_t i = 0; i < toks.size() && toks[i].kind != mt::ast::TokenKind::EndOfFile; ++i) {
            const auto& t = toks[i];
            if (t.text == "{") { ++brace_depth; continue; }
            if (t.text == "}") {
                --brace_depth;
                while (!scopes.empty() && brace_depth < scopes.back().brace_depth) scopes.pop_back();
                continue;
            }
            const std::string kw = (spec.family == "PowerShell" || spec.family == "PHP") ? low_kw(t.text) : t.text;
            // Declaraciones explícitas por palabra clave (class, interface, enum, struct, type, function, func, fun, record, protocol, extension, actor, filter, workflow, mixin)
            if (kw == "class" || kw == "interface" || kw == "enum" || kw == "record" ||
                kw == "function" || kw == "func" || kw == "fun" || kw == "type" ||
                (spec.family == "Swift" && (kw == "struct" || kw == "protocol" || kw == "extension" || kw == "actor")) ||
                (spec.family == "PowerShell" && (kw == "filter" || kw == "workflow")) ||
                (spec.family == "Dart" && (kw == "mixin" || kw == "extension")) ||
                (spec.family == "PHP" && kw == "trait")) {
                size_t start_off = t.offset;
                size_t back = i;
                while (back > 0) {
                    const auto& prev = toks[back - 1];
                    if (prev.kind == mt::ast::TokenKind::Newline) { --back; continue; }
                    if (prev.text == "export" || prev.text == "default" || prev.text == "async" ||
                        prev.text == "public" || prev.text == "private" || prev.text == "protected" ||
                        prev.text == "internal" || prev.text == "static" || prev.text == "final" ||
                        prev.text == "abstract" || prev.text == "open" || prev.text == "override" ||
                        prev.text == "suspend" || prev.text == "data" || prev.text == "sealed" ||
                        prev.text == "fileprivate" || prev.text == "mutating" || prev.text == "nonmutating") {
                        start_off = prev.offset;
                        --back;
                    } else break;
                }
                size_t cur = i + 1;
                while (cur < toks.size() && toks[cur].kind == mt::ast::TokenKind::Newline) ++cur;
                std::string recv_type;
                // En Go: `func (r *Receiver) Method(...)`
                if (spec.family == "Go" && kw == "func" && cur < toks.size() && toks[cur].text == "(") {
                    size_t close_r = find_matching_token(toks, cur, "(", ")");
                    for (size_t r = cur + 1; r < close_r; ++r) {
                        if (toks[r].kind == mt::ast::TokenKind::Identifier) recv_type = toks[r].text;
                    }
                    cur = close_r + 1;
                    while (cur < toks.size() && toks[cur].kind == mt::ast::TokenKind::Newline) ++cur;
                }
                if (cur >= toks.size() || toks[cur].kind != mt::ast::TokenKind::Identifier) continue;
                const auto& name_tok = toks[cur];
                std::string name = name_tok.text;
                ++cur;
                // En PowerShell o nombres con guion: `function Update-Toolchain`
                while (spec.family == "PowerShell" && cur + 1 < toks.size() &&
                       toks[cur].text == "-" && toks[cur + 1].kind == mt::ast::TokenKind::Identifier) {
                    name += "-" + toks[cur + 1].text;
                    cur += 2;
                }
                while (cur < toks.size() && toks[cur].kind == mt::ast::TokenKind::Newline) ++cur;
                if (cur < toks.size() && toks[cur].text == "<") {
                    cur = find_matching_token(toks, cur, "<", ">") + 1;
                    while (cur < toks.size() && toks[cur].kind == mt::ast::TokenKind::Newline) ++cur;
                }
                std::string sig;
                if (cur < toks.size() && toks[cur].text == "(") {
                    size_t close_p = find_matching_token(toks, cur, "(", ")");
                    sig = text.substr(toks[cur].offset, toks[close_p].end_offset - toks[cur].offset);
                    cur = close_p + 1;
                }
                while (cur < toks.size() && toks[cur].kind != mt::ast::TokenKind::EndOfFile &&
                       toks[cur].text != "{" && toks[cur].text != ";") {
                    ++cur;
                }
                size_t end_off = name_tok.end_offset;
                size_t body_start_off = name_tok.end_offset;
                if (cur < toks.size() && toks[cur].text == "{") {
                    body_start_off = toks[cur].offset;
                    size_t close_b = find_matching_token(toks, cur, "{", "}");
                    end_off = toks[close_b].end_offset;
                    if (kw == "class" || kw == "interface" || kw == "enum" || kw == "record" ||
                        kw == "struct" || kw == "protocol" || kw == "extension" || kw == "actor" ||
                        kw == "mixin" || kw == "trait") {
                        ++brace_depth;
                        scopes.push_back({name, brace_depth});
                        i = cur; // escanear miembros internos tras '{'
                    } else {
                        i = close_b;
                    }
                } else if (cur < toks.size() && toks[cur].text == ";") {
                    end_off = toks[cur].end_offset;
                    i = cur;
                }
                std::string qname = name;
                if (!recv_type.empty()) qname = recv_type + "." + name;
                else if (!scopes.empty() && (kw == "function" || kw == "fun" || kw == "func")) qname = scopes.back().name + "." + name;
                std::string kind = (kw == "fun" || kw == "func" || kw == "filter" || kw == "workflow") ? (scopes.empty() ? "function" : "method") : kw;
                if (sig.empty()) sig = kind;
                add_sym(name, qname, sig, kind, start_off, end_off, name_tok.offset, body_start_off);
                continue;
            }

            // Métodos de clase y funciones de nivel superior en lenguajes con llaves
            const bool top_level_func_lang = (scopes.empty() && brace_depth == 0 &&
                (spec.family == "C" || spec.family == "C++" || spec.family == "C/C++" ||
                 spec.family == "C#" || spec.family == "Dart"));
            if (((!scopes.empty() && brace_depth == scopes.back().brace_depth) || top_level_func_lang) &&
                t.kind == mt::ast::TokenKind::Identifier && i + 1 < toks.size() && toks[i + 1].text == "(") {
                static const std::set<std::string> non_method = {
                    "if","for","while","switch","catch","synchronized","return","super","this","new",
                    "guard","defer","else","namespace","extern","sizeof","decltype","alignof","static_assert"
                };
                if (!non_method.contains(t.text)) {
                    size_t start_off = t.offset;
                    size_t back = i;
                    while (back > 0) {
                        const auto& prev = toks[back - 1];
                        if (prev.kind == mt::ast::TokenKind::Newline) { --back; continue; }
                        if (prev.text == ";" || prev.text == "{" || prev.text == "}") break;
                        if (prev.line != t.line) {
                            size_t ls = back - 1;
                            while (ls > 0 && toks[ls - 1].line == prev.line) --ls;
                            while (ls < back && toks[ls].kind == mt::ast::TokenKind::Newline) ++ls;
                            if (ls < back && toks[ls].text == "#") break;
                        }
                        start_off = prev.offset;
                        --back;
                    }
                    size_t close_p = find_matching_token(toks, i + 1, "(", ")");
                    std::string sig = text.substr(toks[i + 1].offset, toks[close_p].end_offset - toks[i + 1].offset);
                    size_t cur = close_p + 1;
                    while (cur < toks.size() && toks[cur].kind != mt::ast::TokenKind::EndOfFile &&
                           toks[cur].text != "{" && toks[cur].text != ";") {
                        ++cur;
                    }
                    if (cur < toks.size() && toks[cur].text == "{") {
                        size_t close_b = find_matching_token(toks, cur, "{", "}");
                        std::string qname = scopes.empty() ? t.text : (scopes.back().name + "." + t.text);
                        std::string sym_kind = scopes.empty() ? "function" : "method";
                        add_sym(t.text, qname, sig, sym_kind, start_off, toks[close_b].end_offset, t.offset, toks[cur].offset);
                        i = close_b;
                        continue;
                    }
                }
            }
        }
    } else if (spec.family == "CMake") {
        auto low_kw = [](std::string s) {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        for (size_t i = 0; i < toks.size() && toks[i].kind != mt::ast::TokenKind::EndOfFile; ++i) {
            std::string kw = low_kw(toks[i].text);
            if ((kw == "function" || kw == "macro") && i + 2 < toks.size() && toks[i + 1].text == "(" &&
                toks[i + 2].kind == mt::ast::TokenKind::Identifier) {
                size_t start_off = toks[i].offset;
                const auto& name_tok = toks[i + 2];
                size_t close_p = find_matching_token(toks, i + 1, "(", ")");
                std::string sig = text.substr(toks[i + 1].offset, toks[close_p].end_offset - toks[i + 1].offset);
                std::string end_kw = (kw == "function") ? "endfunction" : "endmacro";
                size_t cur = close_p + 1;
                size_t end_off = toks[close_p].end_offset;
                while (cur < toks.size() && toks[cur].kind != mt::ast::TokenKind::EndOfFile) {
                    if (low_kw(toks[cur].text) == end_kw) {
                        end_off = toks[cur].end_offset;
                        if (cur + 1 < toks.size() && toks[cur + 1].text == "(") {
                            size_t cp = find_matching_token(toks, cur + 1, "(", ")");
                            end_off = toks[cp].end_offset;
                            cur = cp;
                        }
                        break;
                    }
                    ++cur;
                }
                add_sym(name_tok.text, name_tok.text, sig, "function", start_off, end_off, name_tok.offset, toks[close_p].end_offset);
                i = cur;
            }
        }
    } else if (spec.family == "Shell" || spec.family == "Batch" || spec.family == "Lua" || spec.family == "Ruby") {
        for (size_t i = 0; i < toks.size() && toks[i].kind != mt::ast::TokenKind::EndOfFile; ++i) {
            const auto& t = toks[i];
            if (spec.family == "Shell") {
                if (t.text == "function" && i + 1 < toks.size() && toks[i + 1].kind == mt::ast::TokenKind::Identifier) {
                    const auto& name_tok = toks[i + 1];
                    size_t cur = i + 2;
                    if (cur + 1 < toks.size() && toks[cur].text == "(" && toks[cur + 1].text == ")") cur += 2;
                    while (cur < toks.size() && toks[cur].kind == mt::ast::TokenKind::Newline) ++cur;
                    if (cur < toks.size() && toks[cur].text == "{") {
                        size_t close_b = find_matching_token(toks, cur, "{", "}");
                        add_sym(name_tok.text, name_tok.text, "()", "function", t.offset, toks[close_b].end_offset, name_tok.offset, toks[cur].offset);
                        i = close_b;
                    }
                } else if (t.kind == mt::ast::TokenKind::Identifier && i + 2 < toks.size() &&
                           toks[i + 1].text == "(" && toks[i + 2].text == ")") {
                    size_t cur = i + 3;
                    while (cur < toks.size() && toks[cur].kind == mt::ast::TokenKind::Newline) ++cur;
                    if (cur < toks.size() && toks[cur].text == "{") {
                        size_t close_b = find_matching_token(toks, cur, "{", "}");
                        add_sym(t.text, t.text, "()", "function", t.offset, toks[close_b].end_offset, t.offset, toks[cur].offset);
                        i = close_b;
                    }
                }
            } else if (spec.family == "Batch") {
                bool at_line_start = (i == 0 || toks[i - 1].kind == mt::ast::TokenKind::Newline);
                if (at_line_start && t.text == ":" && i + 1 < toks.size() &&
                    toks[i + 1].kind == mt::ast::TokenKind::Identifier && toks[i + 1].line == t.line) {
                    const auto& name_tok = toks[i + 1];
                    size_t cur = i + 2;
                    size_t end_off = name_tok.end_offset;
                    while (cur < toks.size() && toks[cur].kind != mt::ast::TokenKind::EndOfFile) {
                        if (toks[cur].kind == mt::ast::TokenKind::Newline && cur + 2 < toks.size() &&
                            toks[cur + 1].text == ":" && toks[cur + 2].kind == mt::ast::TokenKind::Identifier) {
                            break;
                        }
                        end_off = toks[cur].end_offset;
                        ++cur;
                    }
                    add_sym(name_tok.text, name_tok.text, "label", "function", t.offset, end_off, name_tok.offset, name_tok.end_offset);
                }
            } else if (spec.family == "Lua" || spec.family == "Ruby") {
                if ((spec.family == "Lua" && t.text == "function") ||
                    (spec.family == "Ruby" && (t.text == "def" || t.text == "class" || t.text == "module"))) {
                    if (i + 1 < toks.size() && toks[i + 1].kind == mt::ast::TokenKind::Identifier) {
                        const auto& name_tok = toks[i + 1];
                        std::string name = name_tok.text;
                        size_t cur = i + 2;
                        while (cur + 1 < toks.size() && (toks[cur].text == "." || toks[cur].text == ":") &&
                               toks[cur + 1].kind == mt::ast::TokenKind::Identifier) {
                            name += toks[cur].text + toks[cur + 1].text;
                            cur += 2;
                        }
                        std::string sig = "()";
                        if (cur < toks.size() && toks[cur].text == "(") {
                            size_t close_p = find_matching_token(toks, cur, "(", ")");
                            sig = text.substr(toks[cur].offset, toks[close_p].end_offset - toks[cur].offset);
                            cur = close_p + 1;
                        }
                        int depth = 1;
                        size_t end_off = name_tok.end_offset;
                        while (cur < toks.size() && toks[cur].kind != mt::ast::TokenKind::EndOfFile && depth > 0) {
                            const auto& st = toks[cur].text;
                            if (st == "function" || st == "def" || st == "class" || st == "module" ||
                                st == "if" || st == "do" || st == "while") ++depth;
                            else if (st == "end") {
                                --depth;
                                if (depth == 0) { end_off = toks[cur].end_offset; break; }
                            }
                            end_off = toks[cur].end_offset;
                            ++cur;
                        }
                        std::string kind = (t.text == "class" || t.text == "module") ? t.text : "function";
                        add_sym(name, name, sig, kind, t.offset, end_off, name_tok.offset, name_tok.end_offset);
                    }
                }
            }
        }
    } else if (spec.family == "SQL") {
        auto upper_str = [](std::string s) {
            for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return s;
        };
        for (size_t i = 0; i < toks.size() && toks[i].kind != mt::ast::TokenKind::EndOfFile; ++i) {
            if (upper_str(toks[i].text) != "CREATE") continue;
            size_t start_off = toks[i].offset;
            size_t cur = i + 1;
            auto skip_nl = [&]() { while (cur < toks.size() && toks[cur].kind == mt::ast::TokenKind::Newline) ++cur; };
            skip_nl();
            if (cur + 1 < toks.size() && upper_str(toks[cur].text) == "OR" && upper_str(toks[cur + 1].text) == "REPLACE") {
                cur += 2; skip_nl();
            }
            if (cur < toks.size() && (upper_str(toks[cur].text) == "TEMP" || upper_str(toks[cur].text) == "TEMPORARY" || upper_str(toks[cur].text) == "UNIQUE")) {
                ++cur; skip_nl();
            }
            if (cur >= toks.size()) break;
            std::string obj_kw = lower(toks[cur].text);
            if (obj_kw != "table" && obj_kw != "view" && obj_kw != "procedure" &&
                obj_kw != "function" && obj_kw != "trigger" && obj_kw != "index") continue;
            ++cur; skip_nl();
            if (cur + 2 < toks.size() && upper_str(toks[cur].text) == "IF" &&
                upper_str(toks[cur + 1].text) == "NOT" && upper_str(toks[cur + 2].text) == "EXISTS") {
                cur += 3; skip_nl();
            }
            if (cur >= toks.size() || toks[cur].kind == mt::ast::TokenKind::EndOfFile) break;
            size_t name_off = toks[cur].offset;
            std::string name = toks[cur].text;
            ++cur;
            while (cur + 1 < toks.size() && toks[cur].text == "." && toks[cur + 1].kind == mt::ast::TokenKind::Identifier) {
                name += "." + toks[cur + 1].text;
                cur += 2;
            }
            int begin_depth = 0;
            size_t end_off = text.size();
            while (cur < toks.size() && toks[cur].kind != mt::ast::TokenKind::EndOfFile) {
                std::string u = upper_str(toks[cur].text);
                if (u == "BEGIN") ++begin_depth;
                else if (u == "END" && begin_depth > 0) --begin_depth;
                else if (toks[cur].text == ";" && begin_depth == 0) {
                    end_off = toks[cur].end_offset;
                    break;
                }
                ++cur;
            }
            add_sym(name, name, obj_kw, obj_kw, start_off, end_off, name_off, name_off);
            i = cur;
        }
    }

    return {
        {"protocol", "migratool.analyzer/1"},
        {"symbols", symbols},
        {"dependencies", dependencies},
        {"diagnostics", Json::array()},
        {"errors", 0}
    };
}

std::string language(const fs::path& p) {
    static const std::map<std::string,std::string> langs={
        {".c","C"},{".h","C/C++"},{".cpp","C++"},{".cxx","C++"},{".cc","C++"},{".hpp","C++"},{".hxx","C++"},{".hh","C++"},{".ixx","C++"},
        {".cs","C#"},{".fs","F#"},{".vb","VB.NET"},{".py","Python"},{".pyw","Python"},{".pyi","Python"},{".js","JavaScript"},{".mjs","JavaScript"},{".cjs","JavaScript"},{".jsx","JSX"},
        {".ts","TypeScript"},{".tsx","TSX"},{".mts","TypeScript"},{".cts","TypeScript"},{".java","Java"},{".kt","Kotlin"},{".kts","Kotlin"},{".rs","Rust"},{".go","Go"},{".swift","Swift"},
        {".m","Objective-C"},{".mm","Objective-C++"},{".rb","Ruby"},{".php","PHP"},{".lua","Lua"},{".dart","Dart"},{".ex","Elixir"},{".erl","Erlang"},
        {".pas","Pascal"},{".dpr","Delphi"},{".cob","COBOL"},{".cbl","COBOL"},{".f90","Fortran"},{".f","Fortran"},{".asm","Assembly"},{".s","Assembly"},
        {".sql","SQL"},{".xaml","XAML"},{".xml","XML"},{".html","HTML"},{".css","CSS"},{".json","JSON"},{".yaml","YAML"},{".yml","YAML"},
        {".sh","Shell"},{".bash","Shell"},{".zsh","Shell"},{".ps1","PowerShell"},{".psm1","PowerShell"},{".psd1","PowerShell"},
        {".bat","Batch"},{".cmd","Batch"},{".cmake","CMake"},{".vue","Vue"},{".svelte","Svelte"},{".r","R"},{".jl","Julia"}};
    auto fname=lower(path_text(p.filename()));
    if(fname=="cmakelists.txt")return "CMake";
    auto ext=lower(path_text(p.extension())); auto it=langs.find(ext);
    return it==langs.end()?(ext.empty()?"Sin extensión":ext.substr(1)):it->second;
}

Json language_capabilities(const Project& p) {
    Json matrix = Json::array({
        {{"language","C#"},{"dialect","C# 12 / Preview"},{"extensions",{".cs"}},{"engine","Roslyn/CSharpCompilation"},{"tier","semantic_compiler"},
         {"analysis_level","verified_semantic"},{"cross_file_type_resolution","solution_csproj_compilation"},
         {"syntactic_symbols",true},{"exact_byte_offsets",true},{"semantic_references",true},{"overload_resolution",true},
         {"type_hierarchy",true},{"symbolic_editing",true},{"ast_pattern_search",true},{"ast_structural_rewrite",true},
         {"diagnostics",true},{"differential_testing",true}},
        {{"language","C"},{"dialect","C23"},{"extensions",{".c"}},{"engine","libclang"},{"tier","semantic_compiler"},
         {"analysis_level","verified_semantic"},{"cross_file_type_resolution","translation_unit_usr"},
         {"syntactic_symbols",true},{"exact_byte_offsets",true},{"semantic_references",true},{"overload_resolution",false},
         {"type_hierarchy",false},{"symbolic_editing",true},{"ast_pattern_search",true},{"ast_structural_rewrite",true},
         {"diagnostics",true},{"differential_testing",true}},
        {{"language","C++"},{"dialect","C++23"},{"extensions",{".cpp",".cxx",".cc",".h",".hpp",".hxx",".hh",".ixx"}},{"engine","libclang"},{"tier","semantic_compiler"},
         {"analysis_level","verified_semantic"},{"cross_file_type_resolution","translation_unit_usr"},
         {"syntactic_symbols",true},{"exact_byte_offsets",true},{"semantic_references",true},{"overload_resolution",true},
         {"type_hierarchy",true},{"symbolic_editing",true},{"ast_pattern_search",true},{"ast_structural_rewrite",true},
         {"diagnostics",true},{"differential_testing",true}},
        {{"language","XAML"},{"extensions",{".xaml"}},{"engine","XmlLite/XAML"},{"tier","structural_markup"},
         {"analysis_level","structural_markup"},{"cross_file_type_resolution","none"},
         {"syntactic_symbols",true},{"exact_byte_offsets",false},{"semantic_references",false},
         {"declarative_references",true},{"overload_resolution",false},{"type_hierarchy",false},
         {"symbolic_editing",false},{"ast_pattern_search",false},{"ast_structural_rewrite",false},
         {"diagnostics",true},{"differential_testing",true}},
        {{"language","Python"},{"dialect","Python 3"},{"extensions",{".py",".pyw",".pyi"}},{"engine","syntactic_ast/Python"},{"tier","syntactic_ast"},
         {"analysis_level","verified_syntactic_ast"},{"cross_file_type_resolution","syntactic_name_only"},
         {"syntactic_symbols",true},{"exact_byte_offsets",true},{"semantic_references",false},{"overload_resolution",false},
         {"type_hierarchy",false},{"symbolic_editing",true},{"ast_pattern_search",true},{"ast_structural_rewrite",true},
         {"diagnostics",false},{"differential_testing",true}},
        {{"language","Rust"},{"dialect","Rust 2021/2024"},{"extensions",{".rs"}},{"engine","syntactic_ast/Rust"},{"tier","syntactic_ast"},
         {"analysis_level","verified_syntactic_ast"},{"cross_file_type_resolution","syntactic_name_only"},
         {"syntactic_symbols",true},{"exact_byte_offsets",true},{"semantic_references",false},{"overload_resolution",false},
         {"type_hierarchy",false},{"symbolic_editing",true},{"ast_pattern_search",true},{"ast_structural_rewrite",true},
         {"diagnostics",false},{"differential_testing",true}},
        {{"language","TypeScript/JavaScript"},{"dialect","ES2024 / TS 5"},{"extensions",{".ts",".tsx",".js",".jsx",".mjs",".cjs"}},{"engine","syntactic_ast/TypeScript"},{"tier","syntactic_ast"},
         {"analysis_level","verified_syntactic_ast"},{"cross_file_type_resolution","syntactic_name_only"},
         {"syntactic_symbols",true},{"exact_byte_offsets",true},{"semantic_references",false},{"overload_resolution",false},
         {"type_hierarchy",false},{"symbolic_editing",true},{"ast_pattern_search",true},{"ast_structural_rewrite",true},
         {"diagnostics",false},{"differential_testing",true}},
        {{"language","Go"},{"dialect","Go 1.22+"},{"extensions",{".go"}},{"engine","syntactic_ast/Go"},{"tier","syntactic_ast"},
         {"analysis_level","verified_syntactic_ast"},{"cross_file_type_resolution","syntactic_name_only"},
         {"syntactic_symbols",true},{"exact_byte_offsets",true},{"semantic_references",false},{"overload_resolution",false},
         {"type_hierarchy",false},{"symbolic_editing",true},{"ast_pattern_search",true},{"ast_structural_rewrite",true},
         {"diagnostics",false},{"differential_testing",true}},
        {{"language","Java/Kotlin"},{"dialect","Java 21 / Kotlin 2.0"},{"extensions",{".java",".kt",".kts"}},{"engine","syntactic_ast/Java"},{"tier","syntactic_ast"},
         {"analysis_level","verified_syntactic_ast"},{"cross_file_type_resolution","syntactic_name_only"},
         {"syntactic_symbols",true},{"exact_byte_offsets",true},{"semantic_references",false},{"overload_resolution",false},
         {"type_hierarchy",false},{"symbolic_editing",true},{"ast_pattern_search",true},{"ast_structural_rewrite",true},
         {"diagnostics",false},{"differential_testing",true}},
        {{"language","SQL"},{"dialect","SQL-92 / SQLite / PostgreSQL / T-SQL"},{"extensions",{".sql"}},{"engine","syntactic_ast/SQL"},{"tier","syntactic_ast"},
         {"analysis_level","verified_syntactic_ast"},{"cross_file_type_resolution","syntactic_name_only"},
         {"syntactic_symbols",true},{"exact_byte_offsets",true},{"semantic_references",false},{"overload_resolution",false},
         {"type_hierarchy",false},{"symbolic_editing",true},{"ast_pattern_search",true},{"ast_structural_rewrite",true},
         {"diagnostics",false},{"differential_testing",true}},
        {{"language","Swift"},{"dialect","Swift 5 / 6"},{"extensions",{".swift"}},{"engine","syntactic_ast/Swift"},{"tier","syntactic_ast"},
         {"analysis_level","verified_syntactic_ast"},{"cross_file_type_resolution","syntactic_name_only"},
         {"syntactic_symbols",true},{"exact_byte_offsets",true},{"semantic_references",false},{"overload_resolution",false},
         {"type_hierarchy",false},{"symbolic_editing",true},{"ast_pattern_search",true},{"ast_structural_rewrite",true},
         {"diagnostics",false},{"differential_testing",true}},
        {{"language","PowerShell/Shell/CMake"},{"dialect","PowerShell 7 / Bash / Batch / CMake"},{"extensions",{".ps1",".psm1",".sh",".bash",".bat",".cmd",".cmake"}},{"engine","syntactic_ast/Script"},{"tier","syntactic_ast"},
         {"analysis_level","verified_syntactic_ast"},{"cross_file_type_resolution","syntactic_name_only"},
         {"syntactic_symbols",true},{"exact_byte_offsets",true},{"semantic_references",false},{"overload_resolution",false},
         {"type_hierarchy",false},{"symbolic_editing",true},{"ast_pattern_search",true},{"ast_structural_rewrite",true},
         {"diagnostics",false},{"differential_testing",true}}
    });
    for (const auto& adapter : p.config.value("analyzers", Json::array())) {
        matrix.push_back({
            {"language", adapter.value("id", "custom")},
            {"extensions", adapter.value("extensions", Json::array())},
            {"engine", "external/" + adapter.value("id", std::string("custom"))},
            {"tier", "external_adapter"},
            {"analysis_level", adapter.value("analysis_level", "external_adapter")},
            {"syntactic_symbols", true},
            {"exact_byte_offsets", adapter.value("exact_byte_offsets", false)},
            {"semantic_references", adapter.value("semantic_references", false)},
            {"overload_resolution", adapter.value("overload_resolution", false)},
            {"type_hierarchy", adapter.value("type_hierarchy", false)},
            {"symbolic_editing", adapter.value("symbolic_editing", false)},
            {"diagnostics", true},
            {"differential_testing", true}
        });
    }
    return matrix;
}

[[maybe_unused]] static Json legacy_inventory(const Project& p, Log log, Cancel* cancel) {
    Json s={{"schema","migratool.scan/1"},{"at",now()},{"complete",true},{"roots",Json::array()},{"files",Json::array()},
            {"symbols",Json::array()},{"dependencies",Json::array()},{"diagnostics",Json::array()},{"exclusions",Json::array()}};
    std::set<std::string> excluded;
    for (const auto& x:p.config.value("exclude_dirs",Json::array())) excluded.insert(lower(x.get<std::string>()));
    for (const auto& root:p.config.at("roots")) {
        auto id=root.at("id").get<std::string>(); auto base=p.resolve(root.at("path"));
        if(log) log("Inventario: "+id+" — "+path_text(base));
        Json r={{"id",id},{"role",root.at("role")},{"path",path_text(base)},{"complete",true}};
        if(!fs::is_directory(base)) {
            s["complete"]=false; r["complete"]=false; s["diagnostics"].push_back(diagnostic(path_text(base),"Raíz inexistente")); s["roots"].push_back(r); continue;
        }
        std::vector<fs::path> paths;
        std::error_code ec;
        fs::recursive_directory_iterator it(base,fs::directory_options::none,ec),end;
        if(ec) { r["complete"]=false; s["complete"]=false; s["diagnostics"].push_back(diagnostic(path_text(base),ec.message())); }
        while(it!=end) {
            if(cancel && cancel->requested) throw std::runtime_error("Inventario cancelado");
            auto entry=*it; auto item=entry.path(); bool skip=false;
            auto attrs=GetFileAttributesW(item.c_str());
            if(attrs!=INVALID_FILE_ATTRIBUTES && (attrs&FILE_ATTRIBUTE_REPARSE_POINT)) {
                skip=true; if(entry.is_directory(ec)) it.disable_recursion_pending();
                s["exclusions"].push_back({{"root",id},{"path",path_text(item)},{"reason","reparse_point"}});
            } else if(entry.is_directory(ec) && (excluded.contains(lower(path_text(item.filename()))) || within(item,p.state_dir()))) {
                it.disable_recursion_pending(); skip=true;
                s["exclusions"].push_back({{"root",id},{"path",path_text(item)},{"reason","exclude_dirs"}});
            }
            if(!skip && entry.is_regular_file(ec)) paths.push_back(item);
            it.increment(ec);
            if(ec) { r["complete"]=false; s["complete"]=false; s["diagnostics"].push_back(diagnostic(path_text(item),ec.message())); ec.clear(); }
        }
        std::sort(paths.begin(),paths.end()); Json digest=Json::array();
        for(const auto& path:paths) {
            auto rel=path_text(path.lexically_relative(base));
            Json f={{"id",id+":"+rel},{"root",id},{"role",root["role"]},{"path",rel},{"absolute_path",path_text(path)},
                    {"language",language(path)},{"analysis","NO_ANALYZER"}};
            try {
                auto before=fs::last_write_time(path); auto bytes=fs::file_size(path);
                f["sha256"]=hash_file(path); f["bytes"]=bytes; f["generated"]=generated(path);
                if(before!=fs::last_write_time(path)||bytes!=fs::file_size(path)) throw std::runtime_error("Archivo cambió durante lectura");
                digest.push_back({rel,f["sha256"]});
            } catch(const std::exception& e) { f["error"]=e.what(); r["complete"]=false; s["complete"]=false; }
            s["files"].push_back(f);
            s["symbols"].push_back({{"id",f["id"]},{"file_id",f["id"]},{"root",id},{"file",rel},{"absolute_path",path_text(path)},
                {"name",path_text(path.filename())},{"qualified_name",rel},{"signature",""},{"kind","file"},{"line",1},{"engine","inventory"}});
        }
        r["fingerprint"]=sha256(digest.dump()); r["file_count"]=paths.size(); s["roots"].push_back(r);
    }
    s["inventory_config"]=sha256(Json{{"roots",p.config["roots"]},{"exclude_dirs",p.config.value("exclude_dirs",Json::array())}}.dump());
    return s;
}

Json analyze(const Project& p, Json s, Log log, Cancel* cancel) {
    // ordered_json puede copiar sus valores al añadir una clave: reservar metadatos antes de retener punteros a archivos.
    s["roslyn"]=Json::object();s["clang"]=Json::object();
    auto run_dir=p.state_dir()/L"analysis"/wide(unique_id()); fs::create_directories(run_dir);
    Json request={{"files",Json::array()},{"context_files",Json::array()},{"defines",p.config.value("defines",Json::array())}};
    std::map<std::string,Json*> by_path;
    std::map<std::string,std::string> cpp_contents;
    std::vector<Json*> cs, cpp;
    std::set<std::string> cs_roots;
    static const std::set<std::string> structural_langs = {
        "Python","Rust","TypeScript","TSX","JavaScript","JSX","Go","Java","Kotlin","SQL",
        "Swift","PowerShell","Shell","Batch","CMake","Dart","Lua","Ruby","PHP"
    };
    for(auto& f:s["files"]) {
        if(f.contains("error")) continue;
        f.erase("analysis_error");
        auto ext=lower(path_text(fs::path(wide(f["path"])).extension())); bool custom=false;
        for(const auto& adapter:p.config.value("analyzers",Json::array())) {
            bool match=false; for(const auto& ex:adapter.at("extensions")) if(lower(ex.get<std::string>())==ext) match=true;
            if(!match) continue; custom=true;
            if(log) log("Analizador "+adapter.at("id").get<std::string>()+": "+f["path"].get<std::string>());
            try {
                auto dir=run_dir/wide(unique_id()); auto output=dir/L"result.json";
                auto command=adapter.at("argv").get<std::vector<std::string>>();
                auto root=std::find_if(s["roots"].begin(),s["roots"].end(),[&](const auto& r){return r["id"]==f["root"];});
                for(auto& arg:command) {
                    arg=replace(arg,"{file}",f["absolute_path"]); arg=replace(arg,"{root}",(*root)["path"]);
                    arg=replace(arg,"{project}",path_text(p.directory())); arg=replace(arg,"{output}",path_text(output));
                }
                auto proc=run_process(command,p.directory(),"",dir,adapter.value("timeout_ms",30000u),cancel);
                if(proc["exit_code"]!=0 || proc["timed_out"]==true || proc["cancelled"]==true || proc["output_limit"]==true) throw std::runtime_error("Analizador terminó con error; consulte "+path_text(dir));
                merge_result(s,f,read_json(fs::exists(output)?output:fs::path(wide(proc["stdout"]))),adapter["id"]);
            } catch(const std::exception& e) { f["analysis"]="ERROR"; f["analysis_error"]=e.what(); s["diagnostics"].push_back(diagnostic(f["absolute_path"],e.what())); }
            break;
        }
        if(custom) continue;
        if(ext==".cs") {
            cs.push_back(&f);
            request["files"].push_back(f["absolute_path"]);
            cs_roots.insert(f["root"].get<std::string>());
            continue;
        }
        auto lang = f["language"].get<std::string>();
        if(lang=="C" || lang=="C++" || lang=="C/C++" || lang=="Objective-C" || lang=="Objective-C++") {
            if(!p.config.value("analyze_cpp",true)) {
                f["analysis"]="DEFERRED";
                continue;
            }
            cpp.push_back(&f);
            auto low_abs = lower(f["absolute_path"]);
            by_path[low_abs] = &f;
            try { cpp_contents[low_abs] = read_text(fs::path(wide(f["absolute_path"])), 32 * 1024 * 1024); } catch(...) {}
            continue;
        }
        if(lang=="XAML" || structural_langs.contains(lang)) {
            try {
                const auto file = fs::path(wide(f["absolute_path"]));
                if(lang=="XAML") {
                    f["analysis_config"]="xaml_structure";
                    merge_result(s, f, xaml_structure(file), "XmlLite/XAML");
                } else {
                    merge_result(s, f, syntactic_ast_scan_file(file, lang), "syntactic_ast/" + lang);
                }
            } catch(const std::exception& e) {
                f["analysis"]="ERROR";
                f["analysis_error"]=e.what();
                s["diagnostics"].push_back(diagnostic(f["absolute_path"], e.what()));
            }
        }
    }
    if(!cs.empty()) {
        request["project_files"] = Json::array();
        request["solution_files"] = Json::array();
        try {
        if(s.value("indexed_context",false)){
            const auto context=roslyn_indexed_context(p,request["files"],cancel);
            for(const auto& [key,value]:context.items())request[key]=value;
        }else{
        // Recolectar archivos .sln, .csproj y todos los .cs del árbol para construir las compilaciones reales por proyecto/solución
        for(const auto& rid : cs_roots) {
            auto r_it = std::find_if(p.config["roots"].begin(), p.config["roots"].end(), [&](const auto& r){ return r["id"] == rid; });
            if(r_it == p.config["roots"].end()) continue;
            auto base = p.resolve(r_it->at("path"));
            std::error_code ec;
            for(fs::recursive_directory_iterator it(base, fs::directory_options::none, ec), end; it != end; it.increment(ec)) {
                if(ec) { ec.clear(); continue; }
                const auto& item = it->path();
                if(it->is_directory(ec)) {
                    auto dn = lower(path_text(item.filename()));
                    if(dn=="bin"||dn=="obj"||dn==".git"||dn==".migratool"||dn=="node_modules"||dn=="build"||dn=="packages") it.disable_recursion_pending();
                    continue;
                }
                auto ext_low = lower(path_text(item.extension()));
                if(ext_low == ".csproj") {
                    request["project_files"].push_back(path_text(item));
                } else if(ext_low == ".sln" || ext_low == ".slnx") {
                    request["solution_files"].push_back(path_text(item));
                } else if(ext_low == ".cs" && !generated(item)) {
                    request["context_files"].push_back(path_text(item));
                }
            }
        }
        }
        if(log) log("Roslyn: "+std::to_string(cs.size())+" archivos C#");
        auto request_file=run_dir/L"roslyn-request.json"; auto output=run_dir/L"roslyn-result.json"; write_json(request_file,request);
            auto helper=executable_dir()/L"adapters/roslyn/RoslynAdapter.dll";
            auto proc=run_process({"dotnet",path_text(helper),path_text(request_file),path_text(output)},p.directory(),"",run_dir/L"roslyn",600000,cancel);
            if(proc["exit_code"]!=0 || proc["timed_out"]==true || proc["cancelled"]==true) throw std::runtime_error("Roslyn falló; consulte "+path_text(run_dir/L"roslyn"));
            auto result=read_json(output);
            for(auto* f:cs) {
                merge_result(s,*f,result.at("files").at(f->at("absolute_path").get<std::string>()),"Roslyn/semantic");
                if(request.contains("context_config_sha256"))(*f)["context_config_sha256"]=request["context_config_sha256"];
                if(request.contains("context_files_sha256"))(*f)["context_files_sha256"]=request["context_files_sha256"];
            }
            Json adapter_hashes=Json::array();
            for(const auto* rel_name:{"RoslynAdapter.dll","RoslynAdapter.deps.json","RoslynAdapter.runtimeconfig.json"}){
                auto fp=helper.parent_path()/wide(rel_name);
                if(fs::exists(fp))adapter_hashes.push_back(Json::array({rel_name,hash_file(fp)}));
            }
            s["roslyn"]={
                {"assembly_sha256",hash_file(helper)},
                {"adapter_sha256",sha256(adapter_hashes.dump())},
                {"version",result["version"]},
                {"semantic_resolution",result.value("compilation_mode","CSharpCompilation")},
                {"projects_loaded",result.value("projects_loaded",0)},
                {"solutions_loaded",result.value("solutions_loaded",0)},
                {"context_mode",request.value("context_mode",std::string("legacy_root_discovery"))},
                {"context_file_count",request["context_files"].size()},
                {"context_bytes",request.value("context_bytes",uint64_t(0))},
                {"context_budget_bytes",request.value("context_budget_bytes",uint64_t(0))},
                {"context_config_sha256",request.value("context_config_sha256",std::string())},
                {"context_files_sha256",request.value("context_files_sha256",std::string())}
            };
        } catch(const std::exception& e) { for(auto* f:cs){(*f)["analysis"]="ERROR";(*f)["analysis_error"]=e.what();}s["diagnostics"].push_back(diagnostic("Roslyn",e.what())); }
    }
    if(!cpp.empty()) {
        if(log) log("Clang: "+std::to_string(cpp.size())+" archivos C/C++");
        std::string clang_phase="load";
        try {
            Clang c; auto index=c.clang_createIndex_p(1,0); std::set<std::string> ids, ref_positions;
            s["clang"]={{"version",c.str(c.clang_getClangVersion_p())},{"library_sha256",hash_file(MIGRATOOL_CLANG_DLL)}};
            struct RootCompileDb {
                std::map<std::string, Json> entries;
                std::string path;
                std::string error;
            };
            std::map<std::string, RootCompileDb> cc_cache;
            for(const auto& r:p.config.value("roots",Json::array())){
                if(!r.contains("compile_commands"))continue;
                RootCompileDb rdb;
                try{
                    const auto cc_path=fs::weakly_canonical(p.resolve(r.at("compile_commands")));
                    rdb.path=path_text(cc_path);
                    const auto arr=read_json(cc_path);
                    for(const auto& cmd:arr){
                        auto dir=fs::path(wide(cmd.at("directory"))); auto source=fs::path(wide(cmd.at("file")));
                        if(!source.is_absolute())source=dir/source;
                        rdb.entries[lower(path_text(fs::weakly_canonical(source)))]=cmd;
                    }
                }catch(const std::exception& e){rdb.error=e.what();}
                cc_cache[r.at("id").get<std::string>()]=std::move(rdb);
            }
            std::vector<fs::path> root_bases;
            for(const auto& r:p.config.value("roots",Json::array()))root_bases.push_back(fs::weakly_canonical(p.resolve(r.at("path"))));
            size_t clang_processed = 0;
            for(auto* f:cpp) {
                clang_phase="file "+f->value("absolute_path",std::string("<null>"));
                if(cancel && cancel->requested) { c.clang_disposeIndex_p(index); throw std::runtime_error("Análisis cancelado"); }
                // Recycle CXIndex every 64 translation units to bound libclang internal arena memory and prevent Win32 1450
                if (clang_processed > 0 && (clang_processed % 64) == 0) {
                    c.clang_disposeIndex_p(index);
                    index = c.clang_createIndex_p(1, 0);
                }
                ++clang_processed;
                auto ext=lower(path_text(fs::path(wide(f->at("path"))).extension()));
                bool header=ext==".h"||ext==".hpp"||ext==".hxx"||ext==".hh";
                auto args=p.config.value("clang_args",Json::array()).get<std::vector<std::string>>();
                for(const auto& def:p.config.value("defines",Json::array()))args.push_back("-D"+def.get<std::string>());
                args.insert(args.begin(), f->at("language")=="C"?"-std=c23":"-std=c++23");
                if(header) { args.insert(args.begin(),"c++-header"); args.insert(args.begin(),"-x"); }
                auto root=std::find_if(p.config["roots"].begin(),p.config["roots"].end(),[&](const auto& r){return r["id"]==f->at("root");});
                if(root==p.config["roots"].end())throw std::runtime_error("Raíz de archivo no está en proyecto: "+f->dump());
                auto base=p.resolve(root->at("path")); (*f)["analysis_config"]="fallback_flags";
                if(auto cc_it=cc_cache.find(f->at("root").get<std::string>()); cc_it!=cc_cache.end()) {
                    if(!cc_it->second.path.empty())(*f)["compile_commands_path"]=cc_it->second.path;
                    if(!cc_it->second.error.empty()) throw std::runtime_error(cc_it->second.error);
                    bool found=false;
                    const auto key_abs=lower(path_text(fs::weakly_canonical(fs::path(wide(f->at("absolute_path"))))));
                    auto eit=cc_it->second.entries.find(key_abs);
                    if(eit==cc_it->second.entries.end())eit=cc_it->second.entries.find(lower(f->at("absolute_path").get<std::string>()));
                    if(eit!=cc_it->second.entries.end()){
                        const auto& cmd=eit->second;
                        try {
                            const auto dir=fs::path(wide(cmd.at("directory")));
                            args=clang_compile_arguments(cmd,dir);
                        } catch (const std::exception& error) {
                            (*f)["analysis_config"]="compile_commands";
                            (*f)["analysis"]="ERROR";
                            (*f)["analysis_error"]=error.what();
                            s["diagnostics"].push_back(diagnostic(f->at("absolute_path"),error.what()));
                            continue;
                        }
                        found=true;
                    }
                    (*f)["analysis_config"]=found?"compile_commands":"fallback_flags";
                }
                args.push_back("-I"+path_text(fs::path(wide(f->at("absolute_path"))).parent_path()));
                args.push_back("-I"+path_text(base));
                if(fs::is_directory(base/L"include")) args.push_back("-I"+path_text(base/L"include"));
                if(fs::is_directory(base/L"src")) args.push_back("-I"+path_text(base/L"src"));
                args.push_back("-ferror-limit=10");
                (*f)["compile_flags_sha256"]=sha256(Json(args).dump());
                std::vector<const char*> argv; for(auto& a:args) argv.push_back(a.c_str());
                CXTranslationUnit tu{};
                auto rc=c.clang_parseTranslationUnit2_p(index,f->at("absolute_path").get_ref<const std::string&>().c_str(),argv.data(),static_cast<int>(argv.size()),nullptr,0,
                    CXTranslationUnit_DetailedPreprocessingRecord | CXTranslationUnit_KeepGoing,&tu);
                if(rc!=CXError_Success || !tu) { (*f)["analysis"]="ERROR"; (*f)["analysis_error"]="Clang parse error "+std::to_string(rc); s["diagnostics"].push_back(diagnostic(f->at("absolute_path"),"Clang parse error "+std::to_string(rc))); continue; }
                bool errors=false;
                for(unsigned i=0;i<c.clang_getNumDiagnostics_p(tu);++i) {
                    auto d=c.clang_getDiagnostic_p(tu,i);
                    if(c.clang_getDiagnosticSeverity_p(d)>=CXDiagnostic_Error) errors=true;
                    s["diagnostics"].push_back(diagnostic(f->at("absolute_path"),c.str(c.clang_formatDiagnostic_p(d,c.clang_defaultDiagnosticDisplayOptions_p()))));
                    c.clang_disposeDiagnostic_p(d);
                }
                clang_phase="visit "+f->at("absolute_path").get<std::string>();
                const size_t dep_start=s["dependencies"].size();
                Visit context{&c,&s,&by_path,&cpp_contents,&ids,&ref_positions,{},cancel};
                c.clang_visitChildren_p(c.clang_getTranslationUnitCursor_p(tu),visitor,&context);
                struct InclusionCtx {
                    Clang* c;
                    Json* deps;
                    const std::vector<fs::path>* roots;
                    std::string file_id;
                    std::set<std::string> recorded;
                };
                InclusionCtx inc_ctx{&c,&s["dependencies"],&root_bases,f->at("id").get<std::string>(),{}};
                for(size_t di=dep_start;di<s["dependencies"].size();++di){
                    if(s["dependencies"][di].contains("target_path"))
                        inc_ctx.recorded.insert(lower(s["dependencies"][di]["target_path"].get<std::string>()));
                }
                c.clang_getInclusions_p(tu,[](CXFile included_file,CXSourceLocation*,unsigned include_len,CXClientData client_data){
                    if(include_len==0||!included_file)return;
                    auto* ic=static_cast<InclusionCtx*>(client_data);
                    auto raw=ic->c->str(ic->c->clang_getFileName_p(included_file));
                    if(raw.empty())return;
                    fs::path p;
                    try{p=fs::weakly_canonical(fs::path(wide(raw)));}catch(...){p=fs::path(wide(raw)).lexically_normal();}
                    auto canon=path_text(p);
                    auto low=lower(canon);
                    if(!ic->recorded.insert(low).second)return;
                    bool in_project=false;
                    for(const auto& rb:*ic->roots)if(within(p,rb)){in_project=true;break;}
                    if(!in_project)return;
                    ic->deps->push_back({
                        {"file_id",ic->file_id},
                        {"kind","include"},
                        {"name",path_text(p.filename())},
                        {"target_name",path_text(p.filename())},
                        {"target_path",canon},
                        {"resolved",true},
                        {"resolution",include_len<=1?"CLANG_INCLUSION":"CLANG_TRANSITIVE_INCLUSION"},
                        {"direct",include_len<=1},
                        {"transitive",include_len>1},
                        {"engine","libclang"}
                    });
                },&inc_ctx);
                (*f)["analysis"]=errors?"PARTIAL":"ANALYZED";
                c.clang_disposeTranslationUnit_p(tu);
            }
            c.clang_disposeIndex_p(index);
        } catch(const std::exception& e) { for(auto* f:cpp){if((*f)["analysis"]=="PENDING"){(*f)["analysis"]="ERROR";(*f)["analysis_error"]=clang_phase+": "+e.what();}}s["diagnostics"].push_back(diagnostic("Clang",clang_phase+": "+e.what())); }
    }
    std::set<std::string> unique; Json symbols=Json::array();
    for(const auto& sym:s["symbols"]) if(unique.insert(sym["id"]).second) symbols.push_back(sym);
    s["symbols"]=symbols;
    return s;
}
Json scan(const Project& p, Log log, Cancel* cancel) {
    index_sync(p,true,log,cancel);auto s=index_snapshot(p);
    if(log) log("Inventario guardado: "+std::to_string(s["files"].size())+" archivos; "+std::to_string(s["symbols"].size())+" unidades (incluye archivos)");
    return s;
}
Json inventory(const Project& p,Log log,Cancel* cancel){index_sync(p,false,log,cancel);return index_snapshot(p);}
Json load_scan(const Project& p) {
    if(fs::exists(p.state_dir()/L"catalog.sqlite"))return index_snapshot(p);
    if(!fs::exists(p.state_dir()/L"scan.json")) throw std::runtime_error("Ejecute scan antes de consultar evidencia");
    return read_json(p.state_dir()/L"scan.json");
}
Json freshness(const Project& p, const Json& old, Cancel* cancel) {
    auto current=inventory(p,{},cancel); Json result={{"fresh",current.value("complete",false) && old.value("complete",false)}, {"roots",Json::array()}};
    if(current["inventory_config"]!=old["inventory_config"]) result["fresh"]=false;
    for(const auto& root:current["roots"]) {
        auto prior=std::find_if(old["roots"].begin(),old["roots"].end(),[&](const auto& r){return r["id"]==root["id"];});
        bool fresh=prior!=old["roots"].end() && root.value("complete",false) && prior->value("complete",false) && root.value("fingerprint","")==prior->value("fingerprint","");
        result["roots"].push_back({{"id",root["id"]},{"fresh",fresh},{"fingerprint",root.value("fingerprint","")}});
        if(!fresh) result["fresh"]=false;
    }
    return result;
}
Json suggestions(const Project& p, const Json& s) {
    (void)p; Json out=Json::array();
    std::map<std::string,std::vector<const Json*>> targets;
    for(const auto& sym:s["symbols"]) {
        auto f=std::find_if(s["files"].begin(),s["files"].end(),[&](const auto& x){return x["id"]==sym["file_id"];});
        if(f!=s["files"].end() && (*f)["role"]=="target") targets[lower(sym["name"])].push_back(&sym);
    }
    for(const auto& sym:s["symbols"]) {
        auto f=std::find_if(s["files"].begin(),s["files"].end(),[&](const auto& x){return x["id"]==sym["file_id"];});
        if(f==s["files"].end()||(*f)["role"]!="source") continue;
        auto it=targets.find(lower(sym["name"])); if(it==targets.end()) continue;
        for(auto* t:it->second) {
            if((sym["kind"]=="file")!=(t->at("kind")=="file")) continue;
            out.push_back({{"source",sym["id"]},{"target",t->at("id")},{"source_path",sym["absolute_path"]},{"target_path",t->at("absolute_path")},
                {"reason","same_name_only"},{"reviewed",false},{"verified",false}});
        }
    }
    return out;
}
}
