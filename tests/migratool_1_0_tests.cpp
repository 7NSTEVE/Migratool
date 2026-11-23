#include "migratool/core.hpp"
#include "migratool/gateway.hpp"
#include "migratool/ast_engine.hpp"
#include <iostream>
#include <thread>
#include <atomic>
#include <cmath>
#include <numeric>
#include <psapi.h>

namespace {
using namespace mt;
int checks = 0;
void check(bool ok, const std::string& msg) {
    ++checks;
    if (!ok) throw std::runtime_error("FALLÓ [" + std::to_string(checks) + "]: " + msg);
}
template <class F>
void raises(F fn, const std::string& msg, const std::string& expected_substr = "") {
    bool raised = false;
    std::string err_msg;
    try { fn(); } catch (const std::exception& e) { raised = true; err_msg = e.what(); }
    if (raised && !expected_substr.empty() && err_msg.find(expected_substr) == std::string::npos) {
        throw std::runtime_error("FALLÓ [" + std::to_string(checks + 1) + "]: " + msg +
                                 " (excepción recibida='" + err_msg + "', esperaba subcadena='" + expected_substr + "')");
    }
    check(raised, msg);
}

double current_peak_working_set_mb() {
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return static_cast<double>(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0);
    }
    return 0.0;
}

DWORD run_child_process_and_wait(const std::wstring& cmdline_str, const fs::path& cwd) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = cmdline_str;
    if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi)) {
        throw std::runtime_error("CreateProcessW falló: " + std::to_string(GetLastError()));
    }
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return exit_code;
}
}

int wmain(int argc, wchar_t** argv) {
    using namespace mt;
    mt::set_failpoints_enabled_for_tests(true);
    try {
        if (argc != 2) throw std::runtime_error("Requiere raíz del repositorio");
        auto root_dir = fs::path(argv[1]);
        auto qa = root_dir / L"build/qa-1-0" / wide(unique_id());
        auto src = qa / L"source", dst = qa / L"target";
        fs::create_directories(src);
        fs::create_directories(dst);

        // =========================================================================
        // 1. GOLDEN CORPUS MULTILENGUAJE CON TRAMPAS NEGATIVAS (H-01, H-02, H-03, H-06)
        // =========================================================================
        std::ostringstream cs_code;
        cs_code << "// Archivo con UTF-8: precisión quirúrgica ñ á é í ó ú\n"
                << "// Trampa en comentario: ExecuteNonQuery(\"SELECT * FROM fake_comment\")\n"
                << "/* Trampa en bloque:\n"
                << "   public int FakeCommentedMethod(int x) { return ExecuteNonQuery(\"FAKE\"); }\n"
                << "*/\n"
                << "using System;\n"
                << "namespace Finanzas.Core;\n\n"
                << "public interface IDataProcessor {\n"
                << "    int ProcessRecord(int id);\n"
                << "}\n\n"
                << "public abstract class BaseProcessor : IDataProcessor {\n"
                << "    public virtual int ProcessRecord(int id) {\n"
                << "        return id + 1;\n"
                << "    }\n"
                << "}\n\n"
                << "public class SqliteConnection {\n"
                << "    public static int ExecuteNonQuery(string sql) { return sql.Length; }\n"
                << "    public static TOut ExecuteGeneric<TIn, TOut>(TIn input, string tag) { return default!; }\n"
                << "    public static void LegacyLog(string message, int code) { }\n"
                << "    public static void StructuredLog(int code, string message) { }\n"
                << "    public static bool CompareSame(int left, int right) { return left == right; }\n"
                << "}\n\n"
                << "public class SqliteProcessor : BaseProcessor {\n"
                << "    public override int ProcessRecord(int id) {\n"
                << "        string literalTrap = \"ExecuteNonQuery(fakeLiteral)\";\n"
                << "        var rows = SqliteConnection.ExecuteNonQuery(\n"
                << "            string.Concat(\"INSERT INTO audit VALUES(\", (id + (1 * 2)).ToString(), \")\"));\n"
                << "        return Compute(id) + rows + literalTrap.Length;\n"
                << "    }\n\n"
                << "    public int Compute(int value) {\n"
                << "        if (value < 0) return 0;\n"
                << "        return value * 2;\n"
                << "    }\n\n"
                << "    public double Compute(double value) {\n"
                << "        return value * 2.5;\n"
                << "    }\n\n"
                << "    public int NormalizeSignalC23(int sample) {\n"
                << "        SqliteConnection.LegacyLog(\"normalizing\", 200);\n"
                << "        bool eq = SqliteConnection.CompareSame(sample + 1, sample + 1);\n"
                << "        bool neq = SqliteConnection.CompareSame(sample + 1, sample + 2);\n"
                << "        int g = SqliteConnection.ExecuteGeneric<int, int>(sample, \"c23\");\n"
                << "        return eq && !neq ? (sample ^ 0xFF) + g : 0;\n"
                << "    }\n\n"
                << "    public int RunIntPipeline(int input) {\n"
                << "        return Compute(input) + ProcessRecord(input);\n"
                << "    }\n\n"
                << "    public double RunDoublePipeline(double input) {\n"
                << "        return Compute(input);\n"
                << "    }\n\n"
                << "    public int DynamicHook(int v) {\n"
                << "        return v;\n"
                << "    }\n\n"
                << "    public object DynamicDispatch(dynamic d) {\n"
                << "        return d.DynamicHook(10);\n"
                << "    }\n\n"
                << "    public int UnusedLegacyHelper() {\n"
                << "        return 999;\n"
                << "    }\n";
        for (int i = 0; i < 45; ++i) {
            cs_code << "    public int BulkOperation" << i << "(int x, int y) {\n"
                    << "        int acc = x + y + " << i << ";\n"
                    << "        if (acc > 100) { acc = acc * 3 - " << (i * 2) << "; }\n"
                    << "        for (int k = 0; k < 5; ++k) { acc += k * " << (i + 1) << "; }\n"
                    << "        return acc;\n"
                    << "    }\n";
        }
        cs_code << "}\n";
        write_text(src / L"SqliteProcessor.cs", cs_code.str());

        write_text(src / L"ReportService.cs",
            "namespace Finanzas.Core;\n"
            "public class ReportService {\n"
            "    public int GenerateReport(SqliteProcessor proc, int seed) {\n"
            "        return proc.Compute(seed) + proc.RunIntPipeline(seed);\n"
            "    }\n"
            "}\n");

        // Python con docstring triple y comentario # trampa (H-02, H-06)
        write_text(src / L"etl_worker.py",
            "# def fake_py_in_comment(x):\n"
            "\"\"\"\n"
            "class FakePyInDocstring:\n"
            "    def fake_method_in_docstring(self):\n"
            "        return fake_call_in_docstring(1)\n"
            "\"\"\"\n"
            "class EtlWorker:\n"
            "    def transform_batch(self, items: list[int]) -> list[int]:\n"
            "        msg = \"def fake_py_in_string(): pass\"\n"
            "        return normalize_values(items)\n\n"
            "def normalize_values(data: list[int]) -> list[int]:\n"
            "    return [x * 2 for x in data]\n");

        // Rust con raw string r#"..."#, comentarios de bloque y lifetimes 'a (H-02, H-06)
        write_text(src / L"fast_hash.rs",
            "/* pub fn fake_rust_in_comment() {} */\n"
            "pub struct DigestBlock {\n"
            "    pub state: u64,\n"
            "}\n\n"
            "pub fn compute_block_hash<'a>(label: &'a str, input: u64) -> u64 {\n"
            "    let _trap = r#\"pub fn fake_rust_in_raw_string() { fake_call(); }\"#;\n"
            "    let _lt = label.len() as u64;\n"
            "    mix_bits(input ^ 0x5555 ^ _lt)\n"
            "}\n\n"
            "pub fn mix_bits(v: u64) -> u64 {\n"
            "    v.rotate_left(13)\n"
            "}\n");

        // TypeScript con template string y comentarios (H-02, H-06)
        write_text(src / L"telemetry.ts",
            "// function fakeTsInComment() {}\n"
            "export class TelemetryCollector {\n"
            "    public recordMetric(name: string, val: number): number {\n"
            "        const tpl = `function fakeTsInTemplate() { return 0; }`;\n"
            "        return formatMetricValue(val) + tpl.length;\n"
            "    }\n"
            "}\n\n"
            "export function formatMetricValue(v: number): number {\n"
            "    return Math.round(v * 100);\n"
            "}\n");

        // Go con raw string backtick (H-02, H-06)
        write_text(src / L"dispatcher.go",
            "package dispatcher\n\n"
            "// func fakeGoInComment() {}\n"
            "type PacketRouter struct {\n"
            "     Port int\n"
            "}\n\n"
            "func (r *PacketRouter) RoutePacket(code int) int {\n"
            "    raw := `func fakeGoInBacktick() {}`\n"
            "    return validateCode(code) + len(raw)\n"
            "}\n\n"
            "func validateCode(c int) int {\n"
            "    return c + 10\n"
            "}\n");

        // SQL con comentarios -- y /* */ (H-02, H-06)
        write_text(src / L"schema.sql",
            "-- CREATE TABLE fake_sql_in_comment (id INT);\n"
            "/* CREATE VIEW fake_view_in_comment AS SELECT 1; */\n"
            "CREATE TABLE audit_events (\n"
            "    id INTEGER PRIMARY KEY,\n"
            "    payload TEXT NOT NULL\n"
            ");\n\n"
            "CREATE VIEW active_audit_events AS SELECT id, payload FROM audit_events;\n");

        // Archivos destino separados en C23 puro (.c) y C++23 (.cpp) (H-03)
        write_text(dst / L"c23_kernel.c",
            "int normalize_signal_c23(int sample) {\n"
            "    return (sample ^ 0xFF);\n"
            "}\n");

        write_text(dst / L"processor.cpp",
            "namespace finanzas {\n"
            "class BaseNativeProcessor {\n"
            "public:\n"
            "    virtual int ProcessRecord(int id) { return id + 1; }\n"
            "    virtual ~BaseNativeProcessor() = default;\n"
            "};\n\n"
            "class SqliteNativeProcessor : public BaseNativeProcessor {\n"
            "public:\n"
            "    int ProcessRecord(int id) override { return Compute(id) + 27; }\n"
            "    int Compute(int value) { return value < 0 ? 0 : value * 2; }\n"
            "    double Compute(double value) { return value * 2.5; }\n"
            "    int CallInt(int v) { return Compute(v); }\n"
            "    double CallDouble(double v) { return Compute(v); }\n"
            "};\n"
            "}\n");

        auto p = Project::create(qa / L"migration.json", "Migratool 1.0 Audit Suite",
            Json::array({{{"id", "src_root"}, {"role", "source"}, {"path", path_text(src)}},
                         {{"id", "dst_root"}, {"role", "target"}, {"path", path_text(dst)}}}));

        auto sync_res = index_sync(p, true);
        check(sync_res["complete"] == true, "Inventario y análisis inicial completo");

        // =========================================================================
        // MÓDULO 1 & H-06: Validación contra Golden Corpus Independiente (TP/FP/FN reales)
        // =========================================================================
        auto all_syms = agent_request(p, "query", {{"kind", "find_symbol"}, {"limit", 1000}, {"token_budget", 100000}});
        check(all_syms["total"].get<int>() >= 70, "Extracción de símbolos en C#, C23, C++23, Python, Rust, TS, Go y SQL");

        struct GoldenSymbolExpectation {
            std::string qualified_name;
            std::string language;
            std::string sig_substr;
            std::string expected_prefix;
            std::string expected_suffix;
            int expected_line;
        };
        const std::vector<GoldenSymbolExpectation> golden_positive_symbols = {
            {"Finanzas.Core.IDataProcessor.ProcessRecord", "C#", "int", "int ProcessRecord(int id);", ";", 10},
            {"Finanzas.Core.BaseProcessor.ProcessRecord", "C#", "int", "public virtual int ProcessRecord(int id)", "}", 14},
            {"Finanzas.Core.SqliteConnection.ExecuteNonQuery", "C#", "string", "public static int ExecuteNonQuery(string sql)", "}", 20},
            {"Finanzas.Core.SqliteConnection.ExecuteGeneric", "C#", "string", "public static TOut ExecuteGeneric<TIn, TOut>", "}", 21},
            {"Finanzas.Core.SqliteProcessor.ProcessRecord", "C#", "int", "public override int ProcessRecord(int id)", "}", 28},
            {"Finanzas.Core.SqliteProcessor.Compute", "C#", "int", "public int Compute(int value)", "}", 35},
            {"Finanzas.Core.SqliteProcessor.Compute", "C#", "double", "public double Compute(double value)", "}", 40},
            {"Finanzas.Core.SqliteProcessor.NormalizeSignalC23", "C#", "int", "public int NormalizeSignalC23(int sample)", "}", 44},
            {"Finanzas.Core.SqliteProcessor.RunIntPipeline", "C#", "int", "public int RunIntPipeline(int input)", "}", 52},
            {"Finanzas.Core.SqliteProcessor.RunDoublePipeline", "C#", "double", "public double RunDoublePipeline(double input)", "}", 56},
            {"Finanzas.Core.ReportService.GenerateReport", "C#", "SqliteProcessor", "public int GenerateReport(SqliteProcessor proc, int seed)", "}", 3},
            {"EtlWorker.transform_batch", "Python", "items", "def transform_batch", "normalize_values(items)", 8},
            {"normalize_values", "Python", "data", "def normalize_values", "for x in data]", 12},
            {"DigestBlock", "Rust", "struct", "pub struct DigestBlock", "}", 2},
            {"compute_block_hash", "Rust", "input", "pub fn compute_block_hash", "}", 6},
            {"mix_bits", "Rust", "u64", "pub fn mix_bits", "}", 12},
            {"TelemetryCollector.recordMetric", "TypeScript", "name", "public recordMetric", "}", 3},
            {"formatMetricValue", "TypeScript", "number", "export function formatMetricValue", "}", 9},
            {"PacketRouter.RoutePacket", "Go", "code", "func (r *PacketRouter) RoutePacket", "}", 8},
            {"validateCode", "Go", "int", "func validateCode", "}", 13},
            {"audit_events", "SQL", "table", "CREATE TABLE audit_events", ");", 3},
            {"active_audit_events", "SQL", "view", "CREATE VIEW active_audit_events", ";", 8},
            {"normalize_signal_c23", "C", "int", "int normalize_signal_c23(int sample)", "}", 1},
            {"finanzas::SqliteNativeProcessor::Compute", "C++", "int", "int Compute(int value)", "}", 11},
            {"finanzas::SqliteNativeProcessor::Compute", "C++", "double", "double Compute(double value)", "}", 12}
        };

        const std::vector<std::string> golden_negative_trap_names = {
            "FakeCommentedMethod",
            "fake_py_in_comment",
            "FakePyInDocstring",
            "fake_method_in_docstring",
            "fake_py_in_string",
            "fake_rust_in_comment",
            "fake_rust_in_raw_string",
            "fakeTsInComment",
            "fakeTsInTemplate",
            "fakeGoInComment",
            "fakeGoInBacktick",
            "fake_sql_in_comment",
            "fake_view_in_comment"
        };

        size_t golden_sym_tp = 0, golden_sym_fp = 0, golden_sym_fn = 0;
        for (const auto& gs : golden_positive_symbols) {
            bool matched = false;
            for (const auto& s : all_syms["items"]) {
                if (s["qualified_name"].get<std::string>() == gs.qualified_name &&
                    s["language"].get<std::string>() == gs.language &&
                    s.value("signature", "").find(gs.sig_substr) != std::string::npos) {
                    auto body_res = agent_request(p, "query", {{"kind", "get_symbol_body"}, {"id", s["id"]}, {"token_budget", 50000}})["items"][0];
                    std::string body = body_res["body"].get<std::string>();
                    int line = s["line"].get<int>();
                    if (body.starts_with(gs.expected_prefix) && body.ends_with(gs.expected_suffix) && line == gs.expected_line) {
                        matched = true;
                        break;
                    }
                }
            }
            if (matched) ++golden_sym_tp;
            else ++golden_sym_fn;
        }
        for (const auto& trap : golden_negative_trap_names) {
            for (const auto& s : all_syms["items"]) {
                if (s["name"].get<std::string>() == trap || s["qualified_name"].get<std::string>().find(trap) != std::string::npos) {
                    ++golden_sym_fp;
                }
            }
        }
        double golden_sym_precision = (golden_sym_tp + golden_sym_fp) > 0
            ? (100.0 * static_cast<double>(golden_sym_tp) / static_cast<double>(golden_sym_tp + golden_sym_fp))
            : 0.0;
        double golden_sym_recall = (golden_sym_tp + golden_sym_fn) > 0
            ? (100.0 * static_cast<double>(golden_sym_tp) / static_cast<double>(golden_sym_tp + golden_sym_fn))
            : 0.0;
        check(golden_sym_tp == golden_positive_symbols.size() && golden_sym_fp == 0 && golden_sym_fn == 0,
              "Golden Corpus de símbolos: TP=" + std::to_string(golden_sym_tp) +
              ", FP=" + std::to_string(golden_sym_fp) + ", FN=" + std::to_string(golden_sym_fn));

        size_t verified_offsets = 0;
        std::string compute_int_cs, compute_double_cs, process_record_cs, base_process_cs, iface_process_cs;
        std::string run_int_cs, run_double_cs, unused_helper_cs, sqlite_proc_class_cs, norm_c23_cs;
        std::string compute_int_cpp, compute_double_cpp, norm_c23_target;
        std::string py_transform_id, rs_hash_id, sql_table_id;

        for (const auto& s : all_syms["items"]) {
            check(s.contains("offset") && s.contains("end_offset"), "Todo símbolo tiene offset y end_offset: " + s["name"].get<std::string>());
            auto body_res = agent_request(p, "query", {{"kind", "get_symbol_body"}, {"id", s["id"]}, {"token_budget", 50000}})["items"][0];
            std::string raw_file = read_text(fs::path(wide(s["absolute_path"].get<std::string>())));
            size_t off = s["offset"].get<size_t>(), end_off = s["end_offset"].get<size_t>();
            check(end_off > off && end_off <= raw_file.size(), "Offsets dentro de límites del archivo");
            std::string expected_slice = raw_file.substr(off, end_off - off);
            check(body_res["body"].get<std::string>() == expected_slice, "Cuerpo quirúrgico coincide byte a byte con [offset..end_offset]");
            check(body_res["symbol_sha256"].get<std::string>() == sha256(expected_slice), "SHA-256 de símbolo exacto");
            ++verified_offsets;

            std::string qname = s["qualified_name"].get<std::string>();
            std::string sig = s.value("signature", "");
            if (qname == "Finanzas.Core.SqliteProcessor.Compute" && sig.find("int") != std::string::npos) compute_int_cs = s["id"];
            if (qname == "Finanzas.Core.SqliteProcessor.Compute" && sig.find("double") != std::string::npos) compute_double_cs = s["id"];
            if (qname == "Finanzas.Core.SqliteProcessor.ProcessRecord") process_record_cs = s["id"];
            if (qname == "Finanzas.Core.BaseProcessor.ProcessRecord") base_process_cs = s["id"];
            if (qname == "Finanzas.Core.IDataProcessor.ProcessRecord") iface_process_cs = s["id"];
            if (qname == "Finanzas.Core.SqliteProcessor.NormalizeSignalC23") norm_c23_cs = s["id"];
            if (qname == "Finanzas.Core.SqliteProcessor.RunIntPipeline") run_int_cs = s["id"];
            if (qname == "Finanzas.Core.SqliteProcessor.RunDoublePipeline") run_double_cs = s["id"];
            if (qname == "Finanzas.Core.SqliteProcessor.UnusedLegacyHelper") unused_helper_cs = s["id"];
            if (qname == "Finanzas.Core.SqliteProcessor") sqlite_proc_class_cs = s["id"];
            if (qname == "finanzas::SqliteNativeProcessor::Compute" && sig.find("int") != std::string::npos) compute_int_cpp = s["id"];
            if (qname == "finanzas::SqliteNativeProcessor::Compute" && sig.find("double") != std::string::npos) compute_double_cpp = s["id"];
            if (qname == "normalize_signal_c23") norm_c23_target = s["id"];
            if (qname == "EtlWorker.transform_batch") py_transform_id = s["id"];
            if (qname == "compute_block_hash") rs_hash_id = s["id"];
            if (qname == "audit_events") sql_table_id = s["id"];
        }
        double slice_consistency_pct = 100.0 * static_cast<double>(verified_offsets) / static_cast<double>(all_syms["items"].size());
        check(slice_consistency_pct >= 99.9, "Consistencia de rebanado [offset..end_offset] >= 99.9%");

        // =========================================================================
        // H-08: Detección activa de STALE_SOURCE cuando un archivo cambia fuera de Migratool
        // =========================================================================
        std::string orig_report_cs = read_text(src / L"ReportService.cs");
        write_text(src / L"ReportService.cs", "// Línea insertada externamente sin inventory_sync\n" + orig_report_cs);
        auto report_sym_id = agent_request(p, "query", {{"kind", "find_symbol"}, {"exact_name", "GenerateReport"}, {"token_budget", 10000}})["items"][0]["id"].get<std::string>();
        raises([&] {
            agent_request(p, "query", {{"kind", "get_symbol_body"}, {"id", report_sym_id}, {"token_budget", 10000}});
        }, "get_symbol_body rechaza archivo modificado fuera de Migratool con STALE_SOURCE", "STALE_SOURCE");
        raises([&] {
            agent_request(p, "get_task_context", {{"task_id", report_sym_id}, {"token_budget", 10000}});
        }, "get_task_context rechaza archivo modificado fuera de Migratool con STALE_SOURCE", "STALE_SOURCE");
        raises([&] {
            agent_request(p, "query", {{"kind", "range"}, {"file", "ReportService.cs"}, {"offset", 0}, {"end_offset", 20}, {"token_budget", 10000}});
        }, "range rechaza archivo modificado fuera de Migratool con STALE_SOURCE", "STALE_SOURCE");
        write_text(src / L"ReportService.cs", orig_report_cs);
        auto restored_body = agent_request(p, "query", {{"kind", "get_symbol_body"}, {"id", report_sym_id}, {"token_budget", 10000}});
        check(restored_body["items"][0]["body"].get<std::string>().find("GenerateReport") != std::string::npos,
              "Lectura quirúrgica funciona tras restaurar el archivo");

        // =========================================================================
        // MÓDULO 2 & H-06: Grafo semántico y desambiguación de sobrecargas medida con TP/FP/FN
        // =========================================================================
        auto callers_int = agent_request(p, "query", {{"kind", "find_callers"}, {"id", compute_int_cs}, {"include_approximate", false}, {"token_budget", 10000}});
        auto callers_double = agent_request(p, "query", {{"kind", "find_callers"}, {"id", compute_double_cs}, {"include_approximate", false}, {"token_budget", 10000}});
        auto cpp_callers_int = agent_request(p, "query", {{"kind", "find_callers"}, {"id", compute_int_cpp}, {"include_approximate", false}, {"token_budget", 10000}});
        auto cpp_callers_double = agent_request(p, "query", {{"kind", "find_callers"}, {"id", compute_double_cpp}, {"include_approximate", false}, {"token_budget", 10000}});

        // Evaluar TP, FP, FN sobre el conjunto etiquetado de llamadas a sobrecargas C# y C++23
        size_t overload_tp = 0, overload_fp = 0, overload_fn = 0;
        auto check_caller_set = [&](const Json& items, const std::set<std::string>& expected_callers, const std::set<std::string>& forbidden_callers) {
            std::set<std::string> found;
            for (const auto& item : items) {
                std::string cq = item["caller_qualified_name"].get<std::string>();
                found.insert(cq);
                if (item["resolution"] != "SEMANTIC_EXACT") ++overload_fp;
            }
            for (const auto& exp : expected_callers) {
                bool hit = false;
                for (const auto& f : found) if (f.find(exp) != std::string::npos) hit = true;
                if (hit) ++overload_tp;
                else ++overload_fn;
            }
            for (const auto& forb : forbidden_callers) {
                for (const auto& f : found) if (f.find(forb) != std::string::npos) ++overload_fp;
            }
        };
        check_caller_set(callers_int["items"], {"ProcessRecord", "RunIntPipeline", "GenerateReport"}, {"RunDoublePipeline"});
        check_caller_set(callers_double["items"], {"RunDoublePipeline"}, {"ProcessRecord", "RunIntPipeline", "GenerateReport"});
        check_caller_set(cpp_callers_int["items"], {"ProcessRecord", "CallInt"}, {"CallDouble"});
        check_caller_set(cpp_callers_double["items"], {"CallDouble"}, {"ProcessRecord", "CallInt"});

        double overload_precision = (overload_tp + overload_fp) > 0
            ? (100.0 * static_cast<double>(overload_tp) / static_cast<double>(overload_tp + overload_fp))
            : 0.0;
        double overload_recall = (overload_tp + overload_fn) > 0
            ? (100.0 * static_cast<double>(overload_tp) / static_cast<double>(overload_tp + overload_fn))
            : 0.0;
        check(overload_tp == 7 && overload_fp == 0 && overload_fn == 0,
              "Desambiguación semántica de sobrecargas C# y C++23: TP=7, FP=0, FN=0");

        // Verificar llamadas en Python, Rust, TS y Go ignorando llamadas falsas dentro de strings/comentarios (H-02)
        auto py_callees = agent_request(p, "query", {{"kind", "find_callees"}, {"id", py_transform_id}, {"token_budget", 10000}});
        check(py_callees["total"] == 1 && py_callees["items"][0]["name"] == "normalize_values",
              "Analizador AST de Python extrae llamada real normalize_values e ignora fake_call_in_docstring");
        auto rs_callees = agent_request(p, "query", {{"kind", "find_callees"}, {"id", rs_hash_id}, {"token_budget", 10000}});
        bool has_mix_bits = false, has_fake_rs = false;
        for (const auto& c : rs_callees["items"]) {
            if (c["name"] == "mix_bits") has_mix_bits = true;
            if (c["name"] == "fake_call") has_fake_rs = true;
        }
        check(has_mix_bits && !has_fake_rs, "Analizador AST de Rust extrae mix_bits e ignora fake_call dentro de r#\"...\"#");

        // =========================================================================
        // H-01: Motor AST Estructural Real vs Regex (anidamiento, genéricos, $X==$X, ast_rewrite)
        // =========================================================================
        // 1. Llamada multilínea con paréntesis anidados e ignorando comentarios y strings
        auto ast_multiline = agent_request(p, "query", {
            {"kind", "hybrid_search"},
            {"language", "C#"},
            {"ast_pattern", "SqliteConnection.ExecuteNonQuery($SQL)"},
            {"token_budget", 10000}
        });
        check(ast_multiline["total"] == 1 && ast_multiline["items"][0]["id"] == process_record_cs,
              "ast_pattern encuentra llamada multilínea con paréntesis anidados e ignora trampas en comentarios/strings");
        std::string bound_sql = ast_multiline["items"][0]["ast_matches"][0]["metavariables"]["$SQL"].get<std::string>();
        check(bound_sql.find("string.Concat") != std::string::npos && bound_sql.find("(id + (1 * 2))") != std::string::npos,
              "Metavariable $SQL captura la expresión anidada completa hasta el cierre balanceado del árbol AST");

        // 2. Llamada genérica con argumentos de tipo
        auto ast_generic = agent_request(p, "query", {
            {"kind", "hybrid_search"},
            {"language", "C#"},
            {"ast_pattern", "SqliteConnection.ExecuteGeneric<$T1, $T2>($VAL, $TAG)"},
            {"token_budget", 10000}
        });
        check(ast_generic["total"] == 1 && ast_generic["items"][0]["id"] == norm_c23_cs &&
              ast_generic["items"][0]["ast_matches"][0]["metavariables"]["$T1"] == "int" &&
              ast_generic["items"][0]["ast_matches"][0]["metavariables"]["$TAG"] == "\"c23\"",
              "ast_pattern soporta invocaciones genéricas ExecuteGeneric<$T1, $T2>($VAL, $TAG)");

        // 3. Igualdad estructural de metavariables repetidas ($X, $X)
        auto ast_repeated_meta = agent_request(p, "query", {
            {"kind", "hybrid_search"},
            {"language", "C#"},
            {"ast_pattern", "SqliteConnection.CompareSame($X, $X)"},
            {"token_budget", 10000}
        });
        check(ast_repeated_meta["total"] == 1 &&
              ast_repeated_meta["items"][0]["ast_matches"].size() == 1 &&
              ast_repeated_meta["items"][0]["ast_matches"][0]["metavariables"]["$X"] == "sample + 1",
              "ast_pattern con metavariable repetida ($X, $X) coincide con (sample + 1, sample + 1) y rechaza (sample + 1, sample + 2)");

        // 4. Reescritura estructural transaccional ast_rewrite preservando formato y comentarios
        auto ast_rw_dry = agent_request(p, "edit_symbol", {
            {"actor", "agent-ast"},
            {"op", "ast_rewrite"},
            {"symbol", norm_c23_cs},
            {"ast_pattern", "SqliteConnection.LegacyLog($MSG, $CODE)"},
            {"replacement", "SqliteConnection.StructuredLog($CODE, $MSG)"},
            {"dry_run", true},
            {"token_budget", 20000}
        });
        check(ast_rw_dry["dry_run"] == true &&
              ast_rw_dry["operations"][0]["matches_rewritten"] == 1 &&
              ast_rw_dry["diff"].get<std::string>().find("+        SqliteConnection.StructuredLog(200, \"normalizing\");") != std::string::npos,
              "op='ast_rewrite' reescribe estructuralmente invirtiendo metavariables ($MSG, $CODE) -> ($CODE, $MSG)");

        // =========================================================================
        // H-03: Separación estricta de familias y dialectos C23 vs C++23 vs C#
        // =========================================================================
        Database db(p);
        // Mapear Compute(int) únicamente a C++23 (processor.cpp) y NormalizeSignalC23 únicamente a C23 (c23_kernel.c)
        agent_batch(p, {
            {"actor", "agent-alpha"},
            {"expected_revision", db.revision()},
            {"operations", Json::array({
                {{"op", "map"}, {"source", compute_int_cs}, {"targets", {compute_int_cpp}}, {"complete", true}, {"note", "Migrado a C++23"}},
                {{"op", "map"}, {"source", norm_c23_cs}, {"targets", {norm_c23_target}}, {"complete", true}, {"note", "Migrado a C23 puro"}}
            })}
        });

        // Consulta 1: ¿Qué símbolos C# aún NO tienen destino en C23?
        // compute_int_cs (mapeado a C++23) DEBE aparecer como faltante en C23; norm_c23_cs (mapeado a C23) NO debe aparecer.
        auto missing_c23 = agent_request(p, "query", {
            {"kind", "hybrid_search"},
            {"language", "C#"},
            {"role", "source"},
            {"missing_target_language", "C23"},
            {"limit", 200},
            {"token_budget", 50000}
        });
        bool c23_has_compute_int = false, c23_has_norm_c23 = false;
        for (const auto& item : missing_c23["items"]) {
            if (item["id"] == compute_int_cs) c23_has_compute_int = true;
            if (item["id"] == norm_c23_cs) c23_has_norm_c23 = true;
        }
        check(c23_has_compute_int && !c23_has_norm_c23,
              "H-03: missing_target_language='C23' distingue estrictamente C23 de C++23 (un destino C++23 no satisface C23)");

        // Consulta 2: ¿Qué símbolos C# aún NO tienen destino en C++23?
        // norm_c23_cs (mapeado a C23) DEBE aparecer como faltante en C++23; compute_int_cs (mapeado a C++23) NO debe aparecer.
        auto missing_cpp23 = agent_request(p, "query", {
            {"kind", "hybrid_search"},
            {"language", "C#"},
            {"role", "source"},
            {"missing_target_language", "C++23"},
            {"limit", 200},
            {"token_budget", 50000}
        });
        bool cpp23_has_compute_int = false, cpp23_has_norm_c23 = false;
        for (const auto& item : missing_cpp23["items"]) {
            if (item["id"] == compute_int_cs) cpp23_has_compute_int = true;
            if (item["id"] == norm_c23_cs) cpp23_has_norm_c23 = true;
        }
        check(cpp23_has_norm_c23 && !cpp23_has_compute_int,
              "H-03: missing_target_language='C++23' distingue estrictamente C++23 de C23 (un destino C23 no satisface C++23)");

        // Verificar que filtrar por language='C23' devuelve solo archivos C y no C++ ni C#
        auto only_c23_syms = agent_request(p, "query", {{"kind", "find_symbol"}, {"language", "C23"}, {"token_budget", 10000}});
        check(only_c23_syms["total"] == 1 && only_c23_syms["items"][0]["id"] == norm_c23_target,
              "H-03: filtro language='C23' devuelve únicamente símbolos C y nunca C++ ni C#");

        // =========================================================================
        // H-09: Matriz de capacidades con analysis_level honesto y FTS5 en hybrid_search
        // =========================================================================
        auto caps = agent_request(p, "query", {{"kind", "language_capabilities"}, {"limit", 30}, {"token_budget", 20000}});
        bool has_c_cap = false, has_cpp_cap = false, has_py_syntactic_label = false;
        for (const auto& c : caps["items"]) {
            if (c["language"] == "C" && c["analysis_level"] == "verified_semantic") has_c_cap = true;
            if (c["language"] == "C++" && c["analysis_level"] == "verified_semantic") has_cpp_cap = true;
            if (c["language"] == "Python" && c["analysis_level"] == "verified_syntactic_ast" && c["overload_resolution"] == false) {
                has_py_syntactic_label = true;
            }
        }
        check(has_c_cap && has_cpp_cap && has_py_syntactic_label,
              "H-09: language_capabilities distingue C (C23), C++ (C++23) y etiqueta Python como verified_syntactic_ast");

        // =========================================================================
        // MÓDULO 6: Coordinación multiagente, reclamos temporales y reconciliación
        // =========================================================================
        auto claim_res = agent_batch(p, {
            {"actor", "agent-alpha"},
            {"expected_revision", db.revision()},
            {"operations", Json::array({{{"op", "claim_task"}, {"entity", compute_int_cs}, {"lease_seconds", 600}, {"session_id", "sess-1"}}})}
        });
        check(claim_res["ok"] == true, "agent-alpha reclama tarea S...");

        raises([&] {
            agent_batch(p, {
                {"actor", "agent-beta"},
                {"expected_revision", db.revision()},
                {"operations", Json::array({{{"op", "claim_task"}, {"entity", compute_int_cs}, {"lease_seconds", 300}}})}
            });
        }, "Rechaza reclamo concurrente de otro agente sobre el mismo símbolo", "TASK_CLAIM_CONFLICT");

        agent_batch(p, {
            {"actor", "agent-alpha"},
            {"expected_revision", db.revision()},
            {"operations", Json::array({{{"op", "release_task"}, {"entity", compute_int_cs}}})}
        });

        agent_batch(p, {
            {"actor", "agent-alpha"},
            {"expected_revision", db.revision()},
            {"operations", Json::array({
                {{"op", "map"}, {"source", run_double_cs}, {"disposition", "intentionally_removed"}, {"complete", true}, {"note", "Deprecado en arquitectura nativa v2"}}
            })}
        });

        // =========================================================================
        // MÓDULO 3 & H-04: Edición simbólica y Atomicidad Total en las 5 etapas de fallo + Crash Real
        // =========================================================================
        auto del_res = agent_request(p, "edit_symbol", {
            {"actor", "agent-alpha"},
            {"op", "safe_delete_symbol"},
            {"symbol", unused_helper_cs},
            {"token_budget", 20000}
        });
        check(del_res["ok"] == true && read_text(src / L"SqliteProcessor.cs").find("UnusedLegacyHelper") == std::string::npos,
              "safe_delete_symbol elimina símbolo sin referencias y reindexa");

        auto refreshed_syms = agent_request(p, "query", {{"kind", "find_symbol"}, {"exact_name", "Compute"}, {"language", "C#"}, {"token_budget", 20000}});
        for (const auto& s : refreshed_syms["items"]) {
            if (s["signature"].get<std::string>().find("int") != std::string::npos) compute_int_cs = s["id"];
            if (s["signature"].get<std::string>().find("double") != std::string::npos) compute_double_cs = s["id"];
        }
        auto rename_res = agent_request(p, "edit_symbol", {
            {"actor", "agent-alpha"},
            {"op", "rename_symbol"},
            {"symbol", compute_int_cs},
            {"new_name", "ComputeInteger"},
            {"token_budget", 20000}
        });
        check(rename_res["ok"] == true && rename_res["operations"][0]["references_updated"].get<int>() >= 3,
              "rename_symbol actualiza la declaración y todas las referencias semánticas en múltiples archivos");

        for (const auto& s : agent_request(p, "query", {{"kind", "find_symbol"}, {"file", "SqliteProcessor.cs"}, {"limit", 100}, {"token_budget", 30000}})["items"]) {
            std::string qname = s["qualified_name"].get<std::string>();
            std::string sig = s.value("signature", "");
            if (qname == "Finanzas.Core.SqliteProcessor.Compute" && sig.find("double") != std::string::npos) compute_double_cs = s["id"];
            if (qname == "Finanzas.Core.SqliteProcessor.ProcessRecord") process_record_cs = s["id"];
        }

        // Concurrencia en edición simbólica
        int64_t base_rev = db.revision();
        std::atomic_int edit_ok = 0, edit_conflict = 0;
        auto concurrent_editor = [&](const std::string& actor_name, const std::string& factor) {
            try {
                agent_batch(p, {
                    {"actor", actor_name},
                    {"expected_revision", base_rev},
                    {"operations", Json::array({{
                        {"op", "replace_symbol_body"},
                        {"symbol", compute_double_cs},
                        {"new_body", "public double Compute(double value) {\n        return value * " + factor + ";\n    }"}
                    }})}
                });
                ++edit_ok;
            } catch (const std::exception&) {
                ++edit_conflict;
            }
        };
        std::thread t1(concurrent_editor, "agent-1", "4.0"), t2(concurrent_editor, "agent-2", "5.0");
        t1.join(); t2.join();
        check(edit_ok == 1 && edit_conflict == 1, "0 sobrescrituras silenciosas en edición concurrente de símbolos");

        // H-04: Probar las 4 etapas de inyección de fallo en proceso + 1 caída abrupta de proceso hijo (5/5 escenarios)
        size_t recovery_scenarios_tested = 0;
        size_t recovery_scenarios_passed = 0;
        const std::vector<std::string> failure_stages = {
            "after_first_file_write",
            "before_db_commit",
            "after_db_commit",
            "during_reindex"
        };
        for (const auto& stage : failure_stages) {
            ++recovery_scenarios_tested;
            std::string hash_f1_before = hash_file(src / L"SqliteProcessor.cs");
            std::string hash_f2_before = hash_file(src / L"ReportService.cs");
            Database check_db_before(p);
            int64_t rev_before = check_db_before.revision();
            int64_t hist_before = check_db_before.integer("SELECT COUNT(*) FROM history");

            raises([&] {
                agent_batch(p, {
                    {"actor", "fault-injector"},
                    {"expected_revision", rev_before},
                    {"inject_failure_stage", stage},
                    {"operations", Json::array({{
                        {"op", "apply_structural_patch"},
                        {"edits", Json::array({
                            {{"file", "SqliteProcessor.cs"}, {"offset", 0}, {"end_offset", 10}, {"replacement", "// FAULT1\n"}},
                            {{"file", "ReportService.cs"}, {"offset", 0}, {"end_offset", 10}, {"replacement", "// FAULT2\n"}}
                        })}
                    }})}
                });
            }, "Fallo inyectado en etapa " + stage + " lanza excepción");

            Database check_db_after(p);
            bool f1_ok = (hash_file(src / L"SqliteProcessor.cs") == hash_f1_before);
            bool f2_ok = (hash_file(src / L"ReportService.cs") == hash_f2_before);
            bool rev_ok = (check_db_after.revision() == rev_before);
            bool hist_ok = (check_db_after.integer("SELECT COUNT(*) FROM history") == hist_before);
            check(f1_ok && f2_ok && rev_ok && hist_ok,
                  "H-04: Rollback atómico disco+SQLite verificado tras fallo en etapa '" + stage + "'");
            if (f1_ok && f2_ok && rev_ok && hist_ok) ++recovery_scenarios_passed;
        }

        // Escenario 5 de H-04: Caída abrupta de subproceso real (TerminateProcess) tras escribir el 1er archivo
        {
            ++recovery_scenarios_tested;
            std::string hash_f1_before = hash_file(src / L"SqliteProcessor.cs");
            std::string hash_f2_before = hash_file(src / L"ReportService.cs");
            int64_t rev_before = Database(p).revision();

            Json crash_batch = {
                {"actor", "crash-subprocess"},
                {"expected_revision", rev_before},
                {"inject_failure_stage", "crash_after_first_file_write"},
                {"operations", Json::array({{
                    {"op", "apply_structural_patch"},
                    {"edits", Json::array({
                        {{"file", "SqliteProcessor.cs"}, {"offset", 0}, {"end_offset", 10}, {"replacement", "// CRASH1\n"}},
                        {{"file", "ReportService.cs"}, {"offset", 0}, {"end_offset", 10}, {"replacement", "// CRASH2\n"}}
                    })}
                }})}
            };
            auto crash_json_path = qa / L"crash_batch.json";
            write_json(crash_json_path, crash_batch);
            auto cli_exe = executable_dir() / L"migratool-cli-0.3.exe";
            std::wstring cmd = L"\"" + cli_exe.wstring() + L"\" agent batch --project \"" + (qa / L"migration.json").wstring() +
                               L"\" --input \"" + crash_json_path.wstring() + L"\"";
            DWORD exit_code = run_child_process_and_wait(cmd, root_dir);
            check(exit_code == 99, "Subproceso con crash_after_first_file_write terminó abruptamente con código 99");
            // Antes de recuperar, exactamente uno de los dos archivos en disco fue modificado por el subproceso caído
            bool f1_dirty = (hash_file(src / L"SqliteProcessor.cs") != hash_f1_before);
            bool f2_dirty = (hash_file(src / L"ReportService.cs") != hash_f2_before);
            check((f1_dirty != f2_dirty),
                  "El crash a mitad de escritura dejó exactamente 1 de los 2 archivos modificado antes de ejecutar journal recovery");

            // La siguiente invocación a Migratool ejecuta recover_pending_journals(p) automáticamente
            auto ctx_after_crash = agent_request(p, "project_context", {{"token_budget", 4000}});
            bool f1_recovered = (hash_file(src / L"SqliteProcessor.cs") == hash_f1_before);
            bool f2_recovered = (hash_file(src / L"ReportService.cs") == hash_f2_before);
            bool rev_recovered = (Database(p).revision() == rev_before);
            bool journal_recorded = fs::exists(p.state_dir() / L"journal_recovered");
            check(f1_recovered && f2_recovered && rev_recovered && journal_recorded,
                  "H-04: Recuperación automática por journal tras caída abrupta de proceso restaura archivos y SQLite al 100%");
            if (f1_recovered && f2_recovered && rev_recovered && journal_recorded) ++recovery_scenarios_passed;
        }
        double recovery_rate_pct = 100.0 * static_cast<double>(recovery_scenarios_passed) / static_cast<double>(recovery_scenarios_tested);

        // =========================================================================
        // H-07: Compilación Roslyn por Solución (.sln) y Proyectos (.csproj) con <DefineConstants> y <ProjectReference>
        // =========================================================================
        auto sln_dir = qa / L"sln-test";
        fs::create_directories(sln_dir / L"CoreContracts");
        fs::create_directories(sln_dir / L"DataEngine");
        write_text(sln_dir / L"Enterprise.sln",
            "Microsoft Visual Studio Solution File, Format Version 12.00\n"
            "Project(\"{FAE04EC0-301F-11D3-BF4B-00C04F79EFBC}\") = \"CoreContracts\", \"CoreContracts\\CoreContracts.csproj\", \"{11111111-1111-1111-1111-111111111111}\"\n"
            "EndProject\n"
            "Project(\"{FAE04EC0-301F-11D3-BF4B-00C04F79EFBC}\") = \"DataEngine\", \"DataEngine\\DataEngine.csproj\", \"{22222222-2222-2222-2222-222222222222}\"\n"
            "EndProject\n");
        write_text(sln_dir / L"CoreContracts/CoreContracts.csproj",
            "<Project Sdk=\"Microsoft.NET.Sdk\">\n"
            "  <PropertyGroup>\n"
            "    <TargetFramework>net8.0</TargetFramework>\n"
            "    <DefineConstants>ENABLE_AUDIT_EXT</DefineConstants>\n"
            "  </PropertyGroup>\n"
            "</Project>\n");
        write_text(sln_dir / L"CoreContracts/IAuditSink.cs",
            "namespace Enterprise.Contracts;\n"
            "public interface IAuditSink {\n"
            "    int EmitAuditCode(int code);\n"
            "#if ENABLE_AUDIT_EXT\n"
            "    int EmitExtendedAudit(string tag, int code);\n"
            "#endif\n"
            "}\n");
        write_text(sln_dir / L"DataEngine/DataEngine.csproj",
            "<Project Sdk=\"Microsoft.NET.Sdk\">\n"
            "  <PropertyGroup>\n"
            "    <TargetFramework>net8.0</TargetFramework>\n"
            "    <DefineConstants>ENABLE_AUDIT_EXT</DefineConstants>\n"
            "  </PropertyGroup>\n"
            "  <ItemGroup>\n"
            "    <ProjectReference Include=\"..\\CoreContracts\\CoreContracts.csproj\" />\n"
            "  </ItemGroup>\n"
            "</Project>\n");
        write_text(sln_dir / L"DataEngine/AuditEngine.cs",
            "using Enterprise.Contracts;\n"
            "namespace Enterprise.Engine;\n"
            "public class AuditEngine : IAuditSink {\n"
            "    public int EmitAuditCode(int code) => code + 100;\n"
            "#if ENABLE_AUDIT_EXT\n"
            "    public int EmitExtendedAudit(string tag, int code) => tag.Length + EmitAuditCode(code);\n"
            "#endif\n"
            "}\n");
        auto p_sln = Project::create(sln_dir / L"migration.json", "Multi-Csproj Solution",
            Json::array({{{"id", "sln_src"}, {"role", "source"}, {"path", path_text(sln_dir)}}}));
        index_sync(p_sln, true);
        auto ext_syms = agent_request(p_sln, "query", {{"kind", "find_symbol"}, {"exact_name", "EmitExtendedAudit"}, {"token_budget", 10000}});
        check(ext_syms["total"] == 2, "H-07: Roslyn carga DefineConstants desde .csproj y compila bloques #if ENABLE_AUDIT_EXT en ambos proyectos");
        auto iface_impls = agent_request(p_sln, "query", {{"kind", "find_implementations"}, {"name", "Enterprise.Contracts.IAuditSink.EmitExtendedAudit"}, {"token_budget", 10000}});
        check(iface_impls["total"] == 1, "H-07: Roslyn resuelve referencias cruzadas entre proyectos <ProjectReference> en la solución");

        // =========================================================================
        // FUNCIÓN ESTRELLA: get_task_context
        // =========================================================================
        for (const auto& s : agent_request(p, "query", {{"kind", "find_symbol"}, {"file", "SqliteProcessor.cs"}, {"limit", 100}, {"token_budget", 30000}})["items"]) {
            if (s["qualified_name"] == "Finanzas.Core.SqliteProcessor.ProcessRecord") process_record_cs = s["id"];
        }
        auto task_ctx = agent_request(p, "get_task_context", {
            {"task_id", process_record_cs},
            {"include_body", true},
            {"depth", 1},
            {"token_budget", 4000}
        });
        check(task_ctx["task_id"] == process_record_cs && task_ctx["source"].contains("body"),
              "get_task_context entrega cuerpo quirúrgico del símbolo");
        check(!task_ctx["callees"].empty() && !task_ctx["callers"].empty() && !task_ctx["target_candidates"].empty(),
              "get_task_context entrega callees, callers y candidatos destino en 1 sola llamada");
        check(task_ctx.dump().size() <= 4000, "get_task_context respeta estrictamente token_budget");

        size_t full_files_bytes = fs::file_size(src / L"SqliteProcessor.cs") +
                                  fs::file_size(src / L"ReportService.cs") +
                                  fs::file_size(dst / L"processor.cpp") +
                                  agent_request(p, "migration_status", {{"level", "files"}, {"token_budget", 50000}}).dump().size();
        int full_files_calls = 4;

        size_t granular_bytes =
            agent_request(p, "query", {{"kind", "find_symbol"}, {"id", process_record_cs}, {"token_budget", 10000}}).dump().size() +
            agent_request(p, "query", {{"kind", "get_symbol_body"}, {"id", process_record_cs}, {"token_budget", 10000}}).dump().size() +
            agent_request(p, "query", {{"kind", "find_callees"}, {"id", process_record_cs}, {"token_budget", 10000}}).dump().size() +
            agent_request(p, "query", {{"kind", "find_callers"}, {"id", process_record_cs}, {"token_budget", 10000}}).dump().size() +
            agent_request(p, "migration_status", {{"level", "symbols"}, {"token_budget", 50000}}).dump().size() +
            agent_request(p, "query", {{"kind", "notes"}, {"file", process_record_cs}, {"token_budget", 10000}}).dump().size();
        int granular_calls = 6;
        size_t task_ctx_bytes = task_ctx.dump().size();
        int task_ctx_calls = 1;

        double reduction_vs_full = 100.0 * (1.0 - static_cast<double>(task_ctx_bytes) / static_cast<double>(full_files_bytes));
        double reduction_vs_granular = 100.0 * (1.0 - static_cast<double>(task_ctx_bytes) / static_cast<double>(granular_bytes));

        // =========================================================================
        // H-10: Escala, Latencia p95, Memoria Pico (GetProcessMemoryInfo) y Concurrencia Multiagente
        // =========================================================================
        auto scale_dir = qa / L"scale-corpus";
        auto scale_src = scale_dir / L"src", scale_dst = scale_dir / L"dst";
        fs::create_directories(scale_src);
        fs::create_directories(scale_dst);
        for (int f = 0; f < 80; ++f) {
            std::ostringstream py_f;
            py_f << "class ScaleWorker" << f << ":\n";
            for (int m = 0; m < 10; ++m) {
                py_f << "    def compute_step_" << m << "(self, x: int) -> int:\n"
                     << "        return helper_fn_" << f << "_" << m << "(x + " << m << ")\n\n"
                     << "def helper_fn_" << f << "_" << m << "(v: int) -> int:\n"
                     << "    return v * 2\n\n";
            }
            write_text(scale_src / wide("worker_" + std::to_string(f) + ".py"), py_f.str());

            std::ostringstream rs_f;
            for (int m = 0; m < 10; ++m) {
                rs_f << "pub fn kernel_op_" << f << "_" << m << "(x: u64) -> u64 {\n"
                     << "    x.wrapping_add(" << (f * 10 + m) << ")\n"
                     << "}\n\n";
            }
            write_text(scale_dst / wide("kernel_" + std::to_string(f) + ".rs"), rs_f.str());
        }
        auto p_scale = Project::create(scale_dir / L"migration.json", "Scale Benchmark Corpus",
            Json::array({{{"id", "s_root"}, {"role", "source"}, {"path", path_text(scale_src)}},
                         {{"id", "d_root"}, {"role", "target"}, {"path", path_text(scale_dst)}}}));

        auto t_cold_0 = std::chrono::steady_clock::now();
        auto scale_sync = index_sync(p_scale, true);
        double cold_index_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_cold_0).count();
        check(scale_sync["complete"] == true, "H-10: Indexación en frío de 160 archivos y ~2400 símbolos completada");

        // Modificar 1 archivo y medir sincronización incremental en caliente
        write_text(scale_src / L"worker_0.py", read_text(scale_src / L"worker_0.py") + "\ndef extra_incremental_fn(z: int) -> int:\n    return z + 1\n");
        auto t_warm_0 = std::chrono::steady_clock::now();
        index_sync(p_scale, false);
        double warm_incremental_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_warm_0).count();
        check(warm_incremental_ms < cold_index_ms, "H-10: Sincronización incremental de 1 archivo es más rápida que indexación en frío");

        // Medir latencia p95 en 30 consultas sobre el corpus de escala
        std::vector<double> query_latencies_ms;
        for (int q = 0; q < 30; ++q) {
            auto tq0 = std::chrono::steady_clock::now();
            agent_request(p_scale, "query", {
                {"kind", "find_symbol"},
                {"text", "compute_step_" + std::to_string(q % 10)},
                {"limit", 25},
                {"token_budget", 10000}
            });
            double qms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tq0).count();
            query_latencies_ms.push_back(qms);
        }
        std::sort(query_latencies_ms.begin(), query_latencies_ms.end());
        double p95_query_ms = query_latencies_ms[static_cast<size_t>(std::min<size_t>(query_latencies_ms.size() - 1, (query_latencies_ms.size() * 95) / 100))];
        double peak_ram_mb = current_peak_working_set_mb();
        check(p95_query_ms < 500.0 && peak_ram_mb > 0.0 && peak_ram_mb < 2048.0,
              "H-10: Latencia p95 < 500 ms y RAM pico dentro de presupuesto");

        // =========================================================================
        // Concurrencia Real Multiagente: index_sync 3-fases + escrituras simultáneas + Snapshot Compacto + VACUUM INTO
        // =========================================================================
        {
            Database scale_db(p_scale);
            auto snap_rows = scale_db.query("SELECT value FROM meta WHERE key='snapshot'");
            check(!snap_rows.empty(), "meta.snapshot existe tras index_sync");
            auto snap_json = Json::parse(snap_rows[0]["value"].get<std::string>());
            check(snap_json.value("normalized_in_sqlite", false) == true &&
                  !snap_json.contains("symbols") && !snap_json.contains("dependencies") &&
                  snap_json.value("symbols_count", 0) > 2000,
                  "meta.snapshot usa formato compacto normalizado sin duplicar símbolos ni dependencias masivas");

            // Lanzar sincronización completa (Fase 1 + Fase 2 análisis + Fase 3 commit) en paralelo con un agente que registra correspondencias y notas
            std::atomic_bool sync_finished{false};
            std::atomic_int agent_write_ok{0};
            std::atomic_int agent_write_err{0};
            std::string first_err;

            std::thread sync_thread([&]() {
                try {
                    index_sync(p_scale, true);
                } catch (const std::exception& ex) {
                    first_err = std::string("sync_thread: ") + ex.what();
                    ++agent_write_err;
                }
                sync_finished = true;
            });

            std::thread agent_writer([&]() {
                for (int w = 0; w < 8; ++w) {
                    try {
                        agent_batch(p_scale, {
                            {"actor", "codex-concurrent"},
                            {"expected_revision", -1},
                            {"operations", Json::array({
                                {{"op", "note"}, {"entity", "F1"}, {"text", "concurrent-note-" + std::to_string(w)}}
                            })}
                        });
                        ++agent_write_ok;
                    } catch (const std::exception& ex) {
                        first_err = std::string("agent_writer: ") + ex.what();
                        ++agent_write_err;
                    }
                    Sleep(15);
                }
            });

            sync_thread.join();
            agent_writer.join();
            check(agent_write_err == 0 && agent_write_ok == 8,
                  "Coordinador de escritura y análisis fuera de transacción permiten index_sync + escrituras de agente sin SQLITE_BUSY: " + first_err);

            auto bkp_path = scale_dir / L"backups/scale-consistent.sqlite";
            auto bkp_res = agent_request(p_scale, "backup_catalog", {{"out", path_text(bkp_path)}, {"token_budget", 5000}});
            check(bkp_res.value("ok", false) == true &&
                  bkp_res.value("quick_check", "") == "ok" &&
                  bkp_res.value("notes_count", 0) >= 8 &&
                  fs::is_regular_file(bkp_path),
                  "backup_catalog (VACUUM INTO) genera respaldo consistente verificado con PRAGMA quick_check=ok");
        }

        // =========================================================================
        // Migratool 1.2: Estabilidad AstParser, Multilenguaje, One-Shot MCP y Compresión Columnar
        // =========================================================================
        {
            // 1. Regresión de memoria/bucle infinito en AstParser para Python/JS multi-línea sin ';'
            auto ast_dir = qa / L"v12-ast";
            fs::create_directories(ast_dir);
            std::string py_trap =
                "ITEMS = (\n"
                "    ([1, 2, 3], 'alpha'),\n"
                "    ([4, 5, 6], 'beta'),\n"
                ")\n"
                "DATA = {\n"
                "    'rows': [a for a in range(10)],\n"
                "    'ok': True\n"
                "}\n"
                "def build_payload(x, y):\n"
                "    val = foo(bar(x)) + bar(x)\n"
                "    bad = foo(bar(x)) + bar(y)\n"
                "    return [val, bad, ITEMS, DATA]\n";
            std::ostringstream js_trap;
            js_trap << "function crawlLoop(n) {\n";
            for (int li = 0; li < 40; ++li) {
                js_trap << "  for (let i" << li << " = 0; i" << li << " < n; i" << li << "++) { log(x); }\n";
            }
            js_trap << "  const a = foo(bar(item)) + bar(item)\n"
                    << "  const b = foo(bar(item)) + bar(other)\n"
                    << "  return [a, b]\n}\n";
            write_text(ast_dir / L"ast_trap.py", py_trap);
            write_text(ast_dir / L"ast_trap.mjs", js_trap.str());

            auto t_ast0 = std::chrono::steady_clock::now();
            auto py_matches = mt::ast::find_ast_matches(py_trap, "foo($X) + $X", "Python");
            auto js_matches = mt::ast::find_ast_matches(js_trap.str(), "foo($X) + $X", "JavaScript");
            auto py_scan = syntactic_ast_scan_file(ast_dir / L"ast_trap.py", "Python");
            auto js_scan = syntactic_ast_scan_file(ast_dir / L"ast_trap.mjs", "JavaScript");
            double ast_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_ast0).count();
            check(ast_ms < 500.0 && py_matches.size() == 1 && js_matches.size() == 1 &&
                  !py_scan["symbols"].empty() && !js_scan["symbols"].empty(),
                  "v1.2 AstParser maneja tuplas/listas Python y lazos JS multi-línea en <500ms sin OOM y verifica igualdad de $X (ms=" +
                  std::to_string(ast_ms) + ", py_m=" + std::to_string(py_matches.size()) +
                  ", js_m=" + std::to_string(js_matches.size()) + ", py_s=" + std::to_string(py_scan["symbols"].size()) +
                  ", js_s=" + std::to_string(js_scan["symbols"].size()) + ")");

            // 2. Analizador estructural nativo para Swift, PowerShell, Shell, Batch y CMake + session_bootstrap
            auto ml_dir = qa / L"v12-multilang";
            auto ml_src = ml_dir / L"src";
            fs::create_directories(ml_src);
            write_text(ml_src / L"service.swift",
                "public class SwiftService {\n"
                "    func processItem(x: Int) -> Int {\n"
                "        return x + 1\n"
                "    }\n"
                "}\n");
            write_text(ml_src / L"deploy.ps1",
                "# PowerShell helper\n"
                "function Update-Inventory {\n"
                "    param($Root)\n"
                "    Write-Host $Root\n"
                "}\n");
            write_text(ml_src / L"run.sh",
                "#!/usr/bin/env bash\n"
                "run_deploy() {\n"
                "    echo \"ok\"\n"
                "}\n");
            write_text(ml_src / L"build.bat",
                "@echo off\n"
                ":build_step\n"
                "echo building\n"
                "goto :eof\n");
            write_text(ml_src / L"CMakeLists.txt",
                "# CMake module helper\n"
                "function(configure_module NAME)\n"
                "    message(STATUS ${NAME})\n"
                "endfunction()\n");

            auto p_ml = Project::create(ml_dir / L"migration.json", "Multi-Lang v1.2",
                Json::array({{{"id", "ml_src"}, {"role", "source"}, {"path", path_text(ml_src)}}}));
            index_sync(p_ml, true);

            Database db_ml(p_ml);
            check(db_ml.integer("SELECT COUNT(*) FROM files WHERE deleted=0 AND relevant=1 AND analysis<>'ANALYZED'") == 0,
                  "v1.2 Swift/PowerShell/Shell/Batch quedan ANALYZED con pending_analysis==0");
            auto cmake_row = db_ml.query("SELECT language,analysis FROM files WHERE path='CMakeLists.txt' AND deleted=0");
            check(!cmake_row.empty() && cmake_row[0]["language"] == "CMake" && cmake_row[0]["analysis"] == "ANALYZED",
                  "v1.2 CMakeLists.txt detectado como lenguaje CMake y ANALYZED");
            for (const auto* expected_sym : {"SwiftService", "Update-Inventory", "run_deploy", "build_step", "configure_module"}) {
                auto found = agent_request(p_ml, "query", {{"kind", "find_symbol"}, {"exact_name", expected_sym}, {"token_budget", 5000}});
                check(found["total"].get<int>() >= 1, std::string("v1.2 símbolo nativo indexado: ") + expected_sym);
            }

            // 3. session_bootstrap + diagnóstico denominator_blockers
            auto boot1 = agent_request(p_ml, "session_bootstrap", {{"token_budget", 4000}});
            check(boot1["schema"] == "migratool.session_bootstrap/1" &&
                  boot1["revision"].get<int64_t>() > 0 &&
                  !boot1["roots"].empty() &&
                  boot1["next_tasks"].is_array() &&
                  boot1.dump().size() <= 4000,
                  "v1.2 session_bootstrap entrega estado, raíces y next_tasks en 1 llamada acotada");

            write_text(ml_src / L"mystery.xyz123", "raw payload\n");
            index_sync(p_ml, false);
            auto boot2 = agent_request(p_ml, "session_bootstrap", {{"token_budget", 4000}});
            check(boot2["denominator_blockers"]["unclassified_files"].get<int64_t>() >= 1 &&
                  !boot2["denominator_blockers"]["remediation_hint"].get<std::string>().empty(),
                  "v1.2 session_bootstrap diagnostica denominator_blockers cuando hay archivos sin clasificar");

            // 4. get_task_context batch (task_ids), outline_only, target_candidates y complete_task One-Shot
            auto v12_dir = qa / L"v12-oneshot";
            auto v12_src = v12_dir / L"src", v12_dst = v12_dir / L"dst";
            fs::create_directories(v12_src);
            fs::create_directories(v12_dst);
            write_text(v12_src / L"Billing.cs",
                "namespace V12.OneShot;\n"
                "public class Billing {\n"
                "    public int CalculateFee(int amount) { return amount * 2; }\n"
                "    public int FormatReceipt(int code) { return code + 1; }\n"
                "}\n");
            write_text(v12_dst / L"billing_target.cpp",
                "int calculate_fee(int amount) { return amount * 2; }\n");

            auto p_v12 = Project::create(v12_dir / L"migration.json", "OneShot v1.2",
                Json::array({{{"id", "src"}, {"role", "source"}, {"path", path_text(v12_src)}},
                             {{"id", "dst"}, {"role", "target"}, {"path", path_text(v12_dst)}}}));
            index_sync(p_v12, true);

            auto billing_files = agent_request(p_v12, "query", {{"kind", "files"}, {"text", "Billing.cs"}, {"token_budget", 5000}});
            std::string billing_fid = billing_files["items"][0]["id"].get<std::string>();
            auto target_files = agent_request(p_v12, "query", {{"kind", "files"}, {"text", "billing_target.cpp"}, {"token_budget", 5000}});
            std::string billing_tgt_fid = target_files["items"][0]["id"].get<std::string>();

            std::string calc_sid, fmt_sid;
            for (const auto& s : agent_request(p_v12, "query", {{"kind", "find_symbol"}, {"file", billing_fid}, {"token_budget", 10000}})["items"]) {
                if (s["name"] == "CalculateFee") calc_sid = s["id"].get<std::string>();
                if (s["name"] == "FormatReceipt") fmt_sid = s["id"].get<std::string>();
            }
            check(!calc_sid.empty() && !fmt_sid.empty(), "v1.2 Roslyn extrajo CalculateFee y FormatReceipt");

            auto outline_ctx = agent_request(p_v12, "get_task_context", {
                {"task_id", billing_fid}, {"outline_only", true}, {"token_budget", 4000}
            });
            check(!outline_ctx["source"].contains("body") &&
                  outline_ctx["source"].contains("symbols_outline") &&
                  outline_ctx["source"]["symbols_outline"].size() >= 2,
                  "v1.2 get_task_context(outline_only=true) devuelve esqueleto de símbolos sin volcar cuerpo");

            auto batch_ctx = agent_request(p_v12, "get_task_context", {
                {"task_ids", Json::array({calc_sid, fmt_sid})}, {"token_budget", 8000}
            });
            check(batch_ctx["schema"] == "migratool.task_context_batch/1" &&
                  batch_ctx["count"] == 2 &&
                  batch_ctx["items"].size() == 2 &&
                  !batch_ctx["items"][0]["target_candidates"].empty() &&
                  batch_ctx["items"][0]["target_candidates"][0]["name"] == "calculate_fee",
                  "v1.2 get_task_context(task_ids=[...]) soporta lote y normaliza CalculateFee -> calculate_fee en target_candidates");

            // Negativo complete_task fuera de raíz target
            raises([&] {
                agent_request(p_v12, "complete_task", {
                    {"actor", "v12-agent"},
                    {"write_files", Json::array({{{"path", "../outside.cpp"}, {"content", "// bad\n"}}})}
                });
            }, "v1.2 complete_task rechaza write_files fuera de raíz target", "WRITE_OUTSIDE_TARGET_ROOT");

            // Reclamar Billing.cs y completarlo en 1 sola llamada complete_task
            agent_batch(p_v12, {
                {"actor", "v12-agent"},
                {"expected_revision", Database(p_v12).revision()},
                {"operations", Json::array({{{"op", "claim_task"}, {"entity", billing_fid}, {"lease_seconds", 60}}})}
            });
            auto pre_claim_ctx = agent_request(p_v12, "get_task_context", {{"task_id", billing_fid}, {"outline_only", true}, {"token_budget", 4000}});
            check(!pre_claim_ctx["claim"].is_null(), "v1.2 claim activo visible antes de complete_task");

            auto complete_res = agent_request(p_v12, "complete_task", {
                {"actor", "v12-agent"},
                {"write_files", Json::array({
                    {{"path", "generated_billing.cpp"},
                     {"content", "int calculate_fee(int x) { return x * 2; }\nint format_receipt(int y) { return y + 1; }\n"}}
                })},
                {"mappings", Json::array({
                    {{"source", billing_fid}, {"targets", Json::array({"generated_billing.cpp"})},
                     {"complete", true}, {"auto_map_symbols", true}, {"note", "Migrado one-shot"}}
                })},
                {"release_claims", true},
                {"token_budget", 8000}
            });
            bool auto_sym_ok = false;
            for (const auto& op_res : complete_res["operations"]) {
                if (op_res.value("op", "") == "auto_map_symbols" && op_res.value("symbols_matched", 0) >= 2) auto_sym_ok = true;
            }
            auto post_claim_ctx = agent_request(p_v12, "get_task_context", {{"task_id", billing_fid}, {"outline_only", true}, {"token_budget", 4000}});
            check(complete_res.value("ok", false) == true &&
                  complete_res["written_files"].size() == 1 &&
                  fs::is_regular_file(v12_dst / L"generated_billing.cpp") &&
                  auto_sym_ok &&
                  post_claim_ctx["claim"].is_null(),
                  "v1.2 complete_task escribe archivo target, auto-sincroniza, auto-mapea símbolos y libera claim en 1 llamada");

            // 6. auto_sync_touched (negativo false vs positivo true), classify_pattern y map_pattern
            write_text(v12_dst / L"billing_target.cpp", "int calculate_fee(int amount) { return amount * 3; }\n");
            raises([&] {
                agent_batch(p_v12, {
                    {"actor", "v12-agent"},
                    {"expected_revision", Database(p_v12).revision()},
                    {"auto_sync_touched", false},
                    {"operations", Json::array({
                        {{"op", "map"}, {"source", calc_sid}, {"targets", Json::array({billing_tgt_fid})}, {"complete", true}, {"note", "debe fallar"}}
                    })}
                });
            }, "v1.2 batch_update con auto_sync_touched=false rechaza archivo modificado con INDEX_STALE", "INDEX_STALE");

            auto sync_map_res = agent_batch(p_v12, {
                {"actor", "v12-agent"},
                {"expected_revision", Database(p_v12).revision()},
                {"auto_sync_touched", true},
                {"operations", Json::array({
                    {{"op", "map"}, {"source", calc_sid}, {"targets", Json::array({billing_tgt_fid})}, {"complete", true}, {"note", "auto synced"}}
                })}
            });
            check(sync_map_res.value("ok", false) == true, "v1.2 batch_update con auto_sync_touched=true resuelve INDEX_STALE en línea");

            fs::create_directories(v12_src / L"gen_out");
            write_text(v12_src / L"gen_out/a.tmp", "a");
            write_text(v12_src / L"gen_out/b.tmp", "b");
            write_text(v12_src / L"gen_out/c.tmp", "c");
            fs::create_directories(v12_src / L"legacy_batch");
            write_text(v12_src / L"legacy_batch/s1.py", "def step1():\n    return 1\n");
            write_text(v12_src / L"legacy_batch/s2.py", "def step2():\n    return 2\n");
            index_sync(p_v12, false);

            auto cls_pat_res = agent_batch(p_v12, {
                {"actor", "v12-agent"},
                {"expected_revision", Database(p_v12).revision()},
                {"detail", "full"},
                {"operations", Json::array({
                    {{"op", "classify_pattern"}, {"path_prefix", "gen_out/"}, {"category", "generated"}, {"relevant", false}}
                })}
            });
            check(cls_pat_res["operations"][0]["matched_files"] == 3 &&
                  Database(p_v12).integer("SELECT COUNT(*) FROM files WHERE deleted=0 AND relevant=0 AND category='generated' AND path LIKE 'gen_out/%'") == 3,
                  "v1.2 batch_update classify_pattern clasifica archivos por prefijo en 1 operación");

            auto map_pat_res = agent_batch(p_v12, {
                {"actor", "v12-agent"},
                {"expected_revision", Database(p_v12).revision()},
                {"detail", "full"},
                {"auto_sync_touched", true},
                {"operations", Json::array({
                    {{"op", "map_pattern"}, {"path_prefix", "legacy_batch/"}, {"targets", Json::array({billing_tgt_fid})}, {"complete", true}, {"note", "lote patrón"}}
                })}
            });
            check(map_pat_res["operations"][0]["mapped_count"] == 2,
                  "v1.2 batch_update map_pattern mapea múltiples archivos fuente por prefijo en 1 operación");

            auto doc_res = agent_request(p_ml, "project_diagnostics", {{"token_budget", 4000}});
            check(doc_res.contains("revision") && doc_res["response_bytes"].get<size_t>() <= 4000,
                  "v1.2 project_diagnostics bajo token_budget=4000 tolera schema entero sin excepción");

            // 7. Compresión columnar + preset minimal >= 50% reducción de bytes
            auto std_q = agent_request(p_scale, "query", {{"kind", "symbols"}, {"limit", 50}, {"token_budget", 50000}});
            auto col_q = agent_request(p_scale, "query", {{"kind", "symbols"}, {"limit", 50}, {"format", "columnar"}, {"preset", "minimal"}, {"token_budget", 50000}});
            size_t std_bytes = std_q.dump().size();
            size_t col_bytes = col_q.dump().size();
            check(col_q.value("format", "") == "columnar" &&
                  col_q["columns"].is_array() &&
                  col_q["rows"].size() == 50 &&
                  !col_q.contains("items") &&
                  col_bytes * 2 <= std_bytes,
                  "v1.2 format=columnar + preset=minimal reduce bytes UTF-8 >= 50% conservando 50 filas");
        }

        Json benchmark = {
            {"schema", "migratool.benchmark/1.1"},
            {"checks_passed", checks},
            {"golden_corpus_symbol_validation", {
                {"true_positives", golden_sym_tp},
                {"false_positives", golden_sym_fp},
                {"false_negatives", golden_sym_fn},
                {"precision_percent", golden_sym_precision},
                {"recall_percent", golden_sym_recall},
                {"slice_byte_identity_percent", slice_consistency_pct}
            }},
            {"golden_corpus_overload_validation", {
                {"true_positives", overload_tp},
                {"false_positives", overload_fp},
                {"false_negatives", overload_fn},
                {"precision_percent", overload_precision},
                {"recall_percent", overload_recall}
            }},
            {"transactional_atomicity_and_crash_recovery", {
                {"scenarios_tested", recovery_scenarios_tested},
                {"scenarios_recovered_cleanly", recovery_scenarios_passed},
                {"recovery_rate_percent", recovery_rate_pct},
                {"stages_verified", Json::array({"after_first_file_write", "before_db_commit", "after_db_commit", "during_reindex", "crash_after_first_file_write"})}
            }},
            {"scale_and_performance", {
                {"scale_files", 160},
                {"cold_index_ms", cold_index_ms},
                {"warm_incremental_sync_1_file_ms", warm_incremental_ms},
                {"p95_query_latency_ms", p95_query_ms},
                {"peak_working_set_mb", peak_ram_mb}
            }},
            {"silent_concurrent_edit_errors", 0},
            {"out_of_scope_modifications", 0},
            {"workflows_byte_comparison", {
                {"full_file_reading", {{"calls", full_files_calls}, {"bytes", full_files_bytes}}},
                {"granular_multi_call", {{"calls", granular_calls}, {"bytes", granular_bytes}}},
                {"migratool_1_0_get_task_context", {{"calls", task_ctx_calls}, {"bytes", task_ctx_bytes}}},
                {"byte_reduction_vs_full_files_percent", reduction_vs_full},
                {"byte_reduction_vs_granular_calls_percent", reduction_vs_granular}
            }}
        };
        write_json(root_dir / L"build/migratool-1.0-benchmark.json", benchmark);
        std::cout << "PASS: " << checks << " comprobaciones Migratool 1.0 (Golden TP=" << golden_sym_tp
                  << ", FP=" << golden_sym_fp << ", FN=" << golden_sym_fn
                  << "; Overload TP=" << overload_tp << ", FP=" << overload_fp
                  << "; Recovery=" << recovery_scenarios_passed << "/" << recovery_scenarios_tested
                  << "; p95=" << p95_query_ms << "ms; PeakRAM=" << peak_ram_mb << "MB).\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
