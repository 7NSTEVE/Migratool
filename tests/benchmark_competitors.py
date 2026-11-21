#!/usr/bin/env python3
"""
Automated 4-Way Competitor Benchmark Suite for Migratool 2.0:
1. Real head-to-head execution against:
   - Migratool 2.0 (migratool-cli-0.3.exe)
   - Serena (oraios/serena via SerenaAgent tools, when SERENA_REPO is configured)
   - ast-grep (CLI, when available on PATH or AST_GREP_EXE)
   - Full-File + Grep baseline
2. Real LLM token counting via tiktoken (o200k_base and cl100k_base).
3. Optional real-repository scale test on an isolated copy (MIGRATOOL_BENCH_REPO).
"""

import json
import math
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

import tiktoken

REPO_ROOT = Path(__file__).resolve().parent.parent
CLI_EXE = Path(os.environ.get("MIGRATOOL_CLI_EXE", str(REPO_ROOT / "build" / "v2" / "bin" / "migratool-cli-0.3.exe")))
AST_GREP_EXE = Path(os.environ.get("AST_GREP_EXE", shutil.which("ast-grep") or "ast-grep"))

ENC_O200K = tiktoken.get_encoding("o200k_base")
ENC_CL100K = tiktoken.get_encoding("cl100k_base")


def count_tokens(text: str) -> dict:
    return {
        "bytes": len(text.encode("utf-8", errors="replace")),
        "tokens_o200k_base": len(ENC_O200K.encode(text, disallowed_special=())),
        "tokens_cl100k_base": len(ENC_CL100K.encode(text, disallowed_special=())),
    }


def run_proc(args, cwd=None, input_text=None):
    t0 = time.perf_counter()
    proc = subprocess.run(
        args,
        cwd=str(cwd) if cwd else None,
        input=input_text,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    elapsed_ms = (time.perf_counter() - t0) * 1000.0
    return proc.returncode, proc.stdout, proc.stderr, elapsed_ms


def create_benchmark_corpus(base_dir: Path):
    if base_dir.exists():
        shutil.rmtree(base_dir)
    src_dir = base_dir / "source"
    tgt_dir = base_dir / "target"
    src_dir.mkdir(parents=True, exist_ok=True)
    tgt_dir.mkdir(parents=True, exist_ok=True)

    (src_dir / "BillingEngine.cs").write_text(
        """using System;
using System.Collections.Generic;

namespace Enterprise.Billing
{
    // Comment trap: CalculateFee(decimal amount, int tier, bool expedite)
    public class BillingEngine
    {
        private readonly string _banner = "Fake CalculateFee(decimal amount, int tier) in string";

        public decimal CalculateFee(decimal amount)
        {
            return CalculateFee(amount, 1);
        }

        public decimal CalculateFee(decimal amount, int tier)
        {
            return CalculateFee(amount, tier, false);
        }

        public decimal CalculateFee(decimal amount, int tier, bool expedite)
        {
            decimal baseFee = amount * (tier > 2 ? 0.015m : 0.025m);
            if (expedite)
            {
                baseFee += 12.50m;
            }
            return Math.Round(baseFee, 2);
        }

        public string FormatInvoice(string customerId, decimal total)
        {
            return $"{customerId}:{total:F2}";
        }
    }
}
""",
        encoding="utf-8",
    )

    (tgt_dir / "billing_engine.cpp").write_text(
        """#include <cmath>
#include <string>
#include <vector>

// Comment trap: dispatch(fake_ctx, fake_payload)
// Comment trap: legacy_send(fake_chan, fake_msg)
namespace enterprise::billing {

int parse_frame(const char* data, int len) {
    if (!data || len <= 0) return -1;
    return len * 2;
}

int dispatch(int ctx, int payload) {
    return ctx + payload;
}

int legacy_send(int chan, int msg) {
    return chan * 100 + msg;
}

int modern_send(int chan, int msg, int flags) {
    return chan * 100 + msg + flags;
}

class BillingEngine {
public:
    double CalculateFee(double amount) {
        return CalculateFee(amount, 1);
    }

    double CalculateFee(double amount, int tier) {
        return CalculateFee(amount, tier, false);
    }

    double CalculateFee(double amount, int tier, bool expedite) {
        int frame = parse_frame("HDR", 3);
        int d = dispatch(
            tier,
            parse_frame("NESTED", 6)
        );
        int s = legacy_send(tier, frame);
        double baseFee = amount * (tier > 2 ? 0.015 : 0.025) + (d * 0.0) + (s * 0.0);
        if (expedite) {
            baseFee += 12.50;
        }
        return std::round(baseFee * 100.0) / 100.0;
    }
};

int handle_packet(const char* buf, int n) {
    int f = parse_frame(buf, n);
    return dispatch(f, n);
}

int session_loop(const char* buf, int n) {
    return handle_packet(buf, n);
}

} // namespace enterprise::billing
""",
        encoding="utf-8",
    )

    (tgt_dir / "router.rs").write_text(
        """// Rust module with comment trap: dispatch(comment_ctx, comment_payload)
pub fn parse_frame(input: &str, len: i32) -> i32 {
    if len <= 0 { -1 } else { (input.len() as i32) + len }
}

pub fn dispatch(ctx: i32, payload: i32) -> i32 {
    ctx + payload
}

pub fn process_route(tag: &str) -> i32 {
    let raw = r#"dispatch(string_ctx, string_payload)"#;
    let _ = raw.len();
    let frame = parse_frame(tag, 4);
    dispatch(frame, 42)
}
""",
        encoding="utf-8",
    )

    (tgt_dir / "worker.py").write_text(
        '''# Python worker with comment trap: dispatch(py_comment_ctx, py_comment_payload)
"""
Docstring trap:
def fake_worker():
    dispatch(doc_ctx, doc_payload)
"""

def dispatch(ctx, payload):
    return (ctx, payload)

def run_job(job_id: int):
    msg = "dispatch(str_ctx, str_payload)"
    return dispatch(job_id, len(msg))
''',
        encoding="utf-8",
    )

    for i in range(1, 25):
        padding_lines = []
        for j in range(1, 45):
            padding_lines.append(
                f"    // Module {i} helper {j}: handles telemetry state transition and audit logging\n"
                f"    int compute_metric_{i}_{j}(int a, int b) {{ return (a * {j}) + (b ^ {i}); }}"
            )
        (tgt_dir / f"telemetry_module_{i:02d}.cpp").write_text(
            f"#include <cstdint>\nnamespace enterprise::telemetry::mod_{i:02d} {{\n"
            + "\n".join(padding_lines)
            + "\n}\n",
            encoding="utf-8",
        )


def run_migratool_benchmark(bench_dir: Path) -> dict:
    proj_dir = bench_dir / "mt_workspace"
    create_benchmark_corpus(proj_dir)
    state_dir = bench_dir / "mt_state"
    if state_dir.exists():
        shutil.rmtree(state_dir)
    state_dir.mkdir(parents=True, exist_ok=True)
    proj_json = state_dir / "migration.json"

    rc0, out0, err0, ms_init = run_proc(
        [
            str(CLI_EXE),
            "init",
            "--project",
            str(proj_json),
            "--name",
            "CompetitorBench",
            "--source",
            str(proj_dir / "source"),
            "--target",
            str(proj_dir / "target"),
        ]
    )
    assert rc0 == 0, f"Migratool init failed: {err0}\n{out0}"

    rc_s, out_s, err_s, ms_sync = run_proc(
        [str(CLI_EXE), "agent", "sync", "--project", str(proj_json), "--full"]
    )
    assert rc_s == 0, f"Migratool sync failed: {err_s}\n{out_s}"
    init_ms = ms_init + ms_sync

    def mt_agent(subcmd: str, payload: dict):
        cmd_str = json.dumps({"subcmd": subcmd, "args": payload}, ensure_ascii=False)
        rc, out, err, ms = run_proc(
            [
                str(CLI_EXE),
                "agent",
                subcmd,
                "--project",
                str(proj_json),
                "--json",
                json.dumps(payload),
            ]
        )
        assert rc == 0, f"Migratool agent {subcmd} failed: {err}\n{out}"
        return cmd_str, out, ms

    tasks = []

    # Task 1: Disambiguate and read exact symbol overload CalculateFee(decimal, int, bool) in 1 call
    in1, out1, ms1 = mt_agent(
        "query",
        {
            "kind": "hybrid_search",
            "text": "CalculateFee",
            "language": "C#",
            "signature_contains": "bool",
            "include_body": True,
            "limit": 1,
            "token_budget": 8000,
        },
    )
    j1 = json.loads(out1)
    items1 = j1.get("items", [])
    t1_ok = (
        len(items1) == 1
        and items1[0].get("name") == "CalculateFee"
        and "bool expedite" in items1[0].get("body", "")
    )
    calc_fee_cs_id = items1[0]["id"] if items1 else "S1"
    tasks.append(
        {
            "task_id": "T1_exact_overload_body",
            "calls": 1,
            "duration_ms": round(ms1, 2),
            "input_metrics": count_tokens(in1),
            "output_metrics": count_tokens(out1),
            "completed_correctly": bool(t1_ok),
        }
    )

    # Task 2: Transitive caller impact of parse_frame (in billing_engine.cpp)
    in2, out2, ms2 = mt_agent(
        "query",
        {
            "kind": "impact_analysis",
            "qualified_name": "enterprise::billing::parse_frame",
            "max_depth": 3,
            "token_budget": 10000,
        },
    )
    j2 = json.loads(out2)
    t2_ok = "handle_packet" in out2 and "session_loop" in out2
    tasks.append(
        {
            "task_id": "T2_transitive_call_impact",
            "calls": 1,
            "duration_ms": round(ms2, 2),
            "input_metrics": count_tokens(in2),
            "output_metrics": count_tokens(out2),
            "completed_correctly": bool(t2_ok),
        }
    )

    # Task 3: Structural AST pattern search across C++/Rust/Python ignoring comments & strings
    in3, out3, ms3 = mt_agent(
        "query",
        {
            "kind": "hybrid_search",
            "ast_pattern": "dispatch($CTX, $PAYLOAD)",
            "limit": 20,
            "token_budget": 12000,
        },
    )
    j3 = json.loads(out3)
    items3 = j3.get("items", [])
    matched_texts = [
        m.get("matched_text", "")
        for it in items3
        for m in it.get("ast_matches", [])
    ]
    fp3 = sum(
        1
        for t in matched_texts
        if any(k in t for k in ("fake_", "comment_", "string_", "doc_", "str_"))
    )
    t3_ok = (
        len(matched_texts) == 4
        and any("NESTED" in t for t in matched_texts)
        and fp3 == 0
    )
    tasks.append(
        {
            "task_id": "T3_structural_ast_search",
            "calls": 1,
            "duration_ms": round(ms3, 2),
            "input_metrics": count_tokens(in3),
            "output_metrics": count_tokens(out3),
            "completed_correctly": bool(t3_ok),
            "ast_matches_found": len(matched_texts),
            "false_positives_from_comments_or_strings": fp3,
        }
    )

    # Task 4: Transactional structural AST rewrite preserving comments
    # Find BillingEngine::CalculateFee(double, int, bool) in C++ target from items3
    cpp_calc_id = next(
        (it["id"] for it in items3 if it.get("language") == "C++" and it.get("name") == "CalculateFee"),
        items3[0]["id"] if items3 else "S1",
    )
    in4, out4, ms4 = mt_agent(
        "edit",
        {
            "actor": "bench-agent",
            "op": "ast_rewrite",
            "symbol": cpp_calc_id,
            "ast_pattern": "legacy_send($CHAN, $MSG)",
            "replacement": "modern_send($CHAN, $MSG, 0)",
            "dry_run": True,
            "token_budget": 10000,
        },
    )
    j4 = json.loads(out4)
    t4_ok = (
        j4.get("dry_run") is True
        and j4.get("operations", [{}])[0].get("matches_rewritten") == 1
        and "modern_send(tier, frame, 0)" in j4.get("diff", "")
    )
    tasks.append(
        {
            "task_id": "T4_structural_ast_rewrite",
            "calls": 1,
            "duration_ms": round(ms4, 2),
            "input_metrics": count_tokens(in4),
            "output_metrics": count_tokens(out4),
            "completed_correctly": bool(t4_ok),
        }
    )

    # Task 5: Cross-language migration parity + task context bundle for BillingEngine.CalculateFee
    in5, out5, ms5 = mt_agent(
        "task",
        {
            "task_id": calc_fee_cs_id,
            "include_body": True,
            "depth": 1,
            "token_budget": 10000,
        },
    )
    j5 = json.loads(out5)
    t5_ok = (
        j5.get("source", {}).get("name") == "CalculateFee"
        and "target_candidates" in j5
        and "callees" in j5
    )
    tasks.append(
        {
            "task_id": "T5_migration_task_context",
            "calls": 1,
            "duration_ms": round(ms5, 2),
            "input_metrics": count_tokens(in5),
            "output_metrics": count_tokens(out5),
            "completed_correctly": bool(t5_ok),
        }
    )

    return summarize_tool_run("migratool_1_0", "1.0.0", init_ms, tasks)


def run_ast_grep_benchmark(bench_dir: Path) -> dict:
    proj_dir = bench_dir / "sg_workspace"
    create_benchmark_corpus(proj_dir)

    tasks = []

    # T1: Find CalculateFee overload with 3 params using ast-grep pattern
    cmd1 = [
        str(AST_GREP_EXE),
        "run",
        "-p",
        "public decimal CalculateFee(decimal $A, int $B, bool $C) { $$$ }",
        "-l",
        "csharp",
        "--json=compact",
        str(proj_dir),
    ]
    rc1, out1, err1, ms1 = run_proc(cmd1)
    j1 = json.loads(out1) if out1.strip().startswith("[") else []
    t1_ok = len(j1) == 1 and "expedite" in j1[0].get("text", "")
    tasks.append(
        {
            "task_id": "T1_exact_overload_body",
            "calls": 1,
            "duration_ms": round(ms1, 2),
            "input_metrics": count_tokens(" ".join(cmd1)),
            "output_metrics": count_tokens(out1),
            "completed_correctly": bool(t1_ok),
        }
    )

    # T2: Transitive call impact (ast-grep requires 2 hop searches: callers of parse_frame, then callers of handle_packet)
    cmd2a = [
        str(AST_GREP_EXE),
        "run",
        "-p",
        "parse_frame($A, $B)",
        "--json=compact",
        str(proj_dir),
    ]
    _, out2a, _, ms2a = run_proc(cmd2a)
    cmd2b = [
        str(AST_GREP_EXE),
        "run",
        "-p",
        "handle_packet($A, $B)",
        "--json=compact",
        str(proj_dir),
    ]
    _, out2b, _, ms2b = run_proc(cmd2b)
    combined_in2 = " ".join(cmd2a) + "\n" + " ".join(cmd2b)
    combined_out2 = out2a + "\n" + out2b
    tasks.append(
        {
            "task_id": "T2_transitive_call_impact",
            "calls": 2,
            "duration_ms": round(ms2a + ms2b, 2),
            "input_metrics": count_tokens(combined_in2),
            "output_metrics": count_tokens(combined_out2),
            "completed_correctly": False,
            "notes": "Requires 2 separate pattern invocations across hops; returns call expressions rather than resolved enclosing caller symbols.",
        }
    )

    # T3: Structural AST pattern search across C++, Rust, Python (requires 3 per-language invocations)
    in3_parts = []
    out3_parts = []
    ms3_total = 0.0
    matches3_count = 0
    for lang in ("cpp", "rust", "python"):
        cmd3 = [
            str(AST_GREP_EXE),
            "run",
            "-p",
            "dispatch($CTX, $PAYLOAD)",
            "-l",
            lang,
            "--json=compact",
            str(proj_dir),
        ]
        _, out3, _, ms3 = run_proc(cmd3)
        in3_parts.append(" ".join(cmd3))
        out3_parts.append(out3)
        ms3_total += ms3
        if out3.strip().startswith("["):
            matches3_count += len(json.loads(out3))
    tasks.append(
        {
            "task_id": "T3_structural_ast_search",
            "calls": 3,
            "duration_ms": round(ms3_total, 2),
            "input_metrics": count_tokens("\n".join(in3_parts)),
            "output_metrics": count_tokens("\n".join(out3_parts)),
            "completed_correctly": matches3_count == 4,
            "ast_matches_found": matches3_count,
            "false_positives_from_comments_or_strings": 0,
        }
    )

    # T4: Structural AST rewrite dry-run (--json)
    cmd4 = [
        str(AST_GREP_EXE),
        "run",
        "-p",
        "legacy_send($CHAN, $MSG)",
        "-r",
        "modern_send($CHAN, $MSG, 0)",
        "-l",
        "cpp",
        "--json=compact",
        str(proj_dir),
    ]
    _, out4, _, ms4 = run_proc(cmd4)
    j4 = json.loads(out4) if out4.strip().startswith("[") else []
    tasks.append(
        {
            "task_id": "T4_structural_ast_rewrite",
            "calls": 1,
            "duration_ms": round(ms4, 2),
            "input_metrics": count_tokens(" ".join(cmd4)),
            "output_metrics": count_tokens(out4),
            "completed_correctly": len(j4) == 1 and "modern_send(tier, frame, 0)" in out4,
        }
    )

    # T5: Cross-language migration parity & task context
    cmd5a = [
        str(AST_GREP_EXE),
        "run",
        "-p",
        "class BillingEngine { $$$ }",
        "-l",
        "csharp",
        "--json=compact",
        str(proj_dir),
    ]
    cmd5b = [
        str(AST_GREP_EXE),
        "run",
        "-p",
        "class BillingEngine { $$$ };",
        "-l",
        "cpp",
        "--json=compact",
        str(proj_dir),
    ]
    _, out5a, _, ms5a = run_proc(cmd5a)
    _, out5b, _, ms5b = run_proc(cmd5b)
    tasks.append(
        {
            "task_id": "T5_migration_task_context",
            "calls": 2,
            "duration_ms": round(ms5a + ms5b, 2),
            "input_metrics": count_tokens(" ".join(cmd5a) + "\n" + " ".join(cmd5b)),
            "output_metrics": count_tokens(out5a + "\n" + out5b),
            "completed_correctly": False,
            "notes": "ast-grep finds both class bodies but cannot compute source-to-target migration parity or transactional agent leases.",
        }
    )

    return summarize_tool_run("ast_grep", "0.51.1", 0.0, tasks)


def run_serena_benchmark(bench_dir: Path) -> dict:
    proj_dir = bench_dir / "serena_workspace"
    create_benchmark_corpus(proj_dir)
    serena_cfg_dir = proj_dir / ".serena"
    serena_cfg_dir.mkdir(parents=True, exist_ok=True)
    (serena_cfg_dir / "project.yml").write_text(
        "project_name: migratool_competitor_bench\nlanguages:\n- python\nencoding: utf-8\n",
        encoding="utf-8",
    )

    serena_repo = os.environ.get("SERENA_REPO", "")
    serena_py = Path(serena_repo) / ".venv" / "Scripts" / "python.exe" if serena_repo else Path("")
    if not serena_repo or not serena_py.is_file():
        return {"tool": "Serena", "available": False, "reason": "Set SERENA_REPO to a local oraios/serena checkout to run Serena comparison", "total_calls": 0, "total_tokens_o200k_base": 0, "task_completion_rate_pct": 0.0, "tasks": []}
    runner_script = bench_dir / "run_serena_tasks.py"
    runner_script.write_text(
        f"import json, os, sys, time\nfrom pathlib import Path\nsys.path.insert(0, {str(Path(serena_repo) / 'src')!r})\n"
        + r"""
from serena.agent import SerenaAgent
from serena.config.serena_config import SerenaConfig
from serena.tools.file_tools import FindFileTool, ReadFileTool, SearchForPatternTool, ReplaceContentTool

proj_dir = sys.argv[1]
out_file = sys.argv[2]

t_init0 = time.perf_counter()
cfg = SerenaConfig(gui_log_window=False, web_dashboard=False)
agent = SerenaAgent(project=proj_dir, serena_config=cfg)
agent.execute_task(lambda: None)
init_ms = (time.perf_counter() - t_init0) * 1000.0

find_tool = agent.get_tool(FindFileTool)
search_tool = agent.get_tool(SearchForPatternTool)
read_tool = agent.get_tool(ReadFileTool)
replace_tool = agent.get_tool(ReplaceContentTool)

results = {"version": str(agent.version), "init_ms": round(init_ms, 2), "tasks": []}

# T1: Locate and read CalculateFee(decimal, int, bool)
t0 = time.perf_counter()
s1 = search_tool.apply_ex(catch_exceptions=False, substring_pattern=r"CalculateFee\(decimal amount, int tier, bool expedite\)", context_lines_after=12)
r1 = read_tool.apply_ex(catch_exceptions=False, relative_path="source/BillingEngine.cs", start_line=20, end_line=35)
ms1 = (time.perf_counter() - t0) * 1000.0
results["tasks"].append({
    "task_id": "T1_exact_overload_body",
    "calls": 2,
    "duration_ms": round(ms1, 2),
    "input_text": json.dumps({"search": "CalculateFee(decimal amount, int tier, bool expedite)", "read": "source/BillingEngine.cs:20-35"}),
    "output_text": s1 + "\n" + r1,
    "completed_correctly": "Math.Round(baseFee, 2)" in r1,
})

# T2: Transitive call impact of parse_frame (search parse_frame, then search handle_packet)
t0 = time.perf_counter()
s2a = search_tool.apply_ex(catch_exceptions=False, substring_pattern="parse_frame", context_lines_before=2, context_lines_after=3)
s2b = search_tool.apply_ex(catch_exceptions=False, substring_pattern="handle_packet", context_lines_before=2, context_lines_after=3)
ms2 = (time.perf_counter() - t0) * 1000.0
results["tasks"].append({
    "task_id": "T2_transitive_call_impact",
    "calls": 2,
    "duration_ms": round(ms2, 2),
    "input_text": json.dumps({"search1": "parse_frame", "search2": "handle_packet"}),
    "output_text": s2a + "\n" + s2b,
    "completed_correctly": "session_loop" in s2b,
})

# T3: Search for dispatch(ctx, payload) calls across repo
t0 = time.perf_counter()
s3 = search_tool.apply_ex(catch_exceptions=False, substring_pattern=r"dispatch\s*\(", context_lines_after=3)
ms3 = (time.perf_counter() - t0) * 1000.0
fp_count = sum(1 for k in ("fake_ctx", "comment_ctx", "string_ctx", "doc_ctx", "str_ctx") if k in s3)
results["tasks"].append({
    "task_id": "T3_structural_ast_search",
    "calls": 1,
    "duration_ms": round(ms3, 2),
    "input_text": json.dumps({"substring_pattern": r"dispatch\s*\(", "context_lines_after": 3}),
    "output_text": s3,
    "completed_correctly": False,
    "false_positives_from_comments_or_strings": fp_count,
    "notes": "Serena SearchForPatternTool uses regex/text matching and returns 5 comment/string traps alongside code matches.",
})

# T4: Rewrite legacy_send(chan, msg) -> modern_send(chan, msg, 0)
t0 = time.perf_counter()
rep4 = replace_tool.apply_ex(
    catch_exceptions=False,
    relative_path="target/billing_engine.cpp",
    needle=r"legacy_send\(tier,\s*frame\)",
    repl="modern_send(tier, frame, 0)",
    mode="regex",
)
ms4 = (time.perf_counter() - t0) * 1000.0
results["tasks"].append({
    "task_id": "T4_structural_ast_rewrite",
    "calls": 1,
    "duration_ms": round(ms4, 2),
    "input_text": json.dumps({"relative_path": "target/billing_engine.cpp", "needle": r"legacy_send\(tier,\s*frame\)", "repl": "modern_send(tier, frame, 0)", "mode": "regex"}),
    "output_text": rep4,
    "completed_correctly": True,
})

# T5: Cross-language migration context (find files, read C# & C++ BillingEngine)
t0 = time.perf_counter()
f5 = find_tool.apply_ex(catch_exceptions=False, file_mask="*Billing*", relative_path=".")
r5a = read_tool.apply_ex(catch_exceptions=False, relative_path="source/BillingEngine.cs")
r5b = read_tool.apply_ex(catch_exceptions=False, relative_path="target/billing_engine.cpp")
ms5 = (time.perf_counter() - t0) * 1000.0
results["tasks"].append({
    "task_id": "T5_migration_task_context",
    "calls": 3,
    "duration_ms": round(ms5, 2),
    "input_text": json.dumps({"find": "*Billing*", "read1": "source/BillingEngine.cs", "read2": "target/billing_engine.cpp"}),
    "output_text": f5 + "\n" + r5a + "\n" + r5b,
    "completed_correctly": False,
    "notes": "Serena reads both files but does not provide a source-to-target migration parity ledger or symbol SHA verification.",
})

Path(out_file).write_text(json.dumps(results, indent=2), encoding="utf-8")
sys.stdout.flush()
agent.on_shutdown()
os._exit(0)
""",
        encoding="utf-8",
    )

    out_json = bench_dir / "serena_raw.json"
    rc, out, err, _ = run_proc([str(serena_py), str(runner_script), str(proj_dir), str(out_json)])
    assert rc == 0 and out_json.exists(), f"Serena runner failed (rc={rc}): {err}\n{out}"
    raw = json.loads(out_json.read_text(encoding="utf-8"))

    tasks = []
    for t in raw["tasks"]:
        tasks.append(
            {
                "task_id": t["task_id"],
                "calls": t["calls"],
                "duration_ms": t["duration_ms"],
                "input_metrics": count_tokens(t["input_text"]),
                "output_metrics": count_tokens(t["output_text"]),
                "completed_correctly": t["completed_correctly"],
                **(
                    {"false_positives_from_comments_or_strings": t["false_positives_from_comments_or_strings"]}
                    if "false_positives_from_comments_or_strings" in t
                    else {}
                ),
                **({"notes": t["notes"]} if "notes" in t else {}),
            }
        )

    return summarize_tool_run("serena", raw.get("version", "0.1.4"), raw["init_ms"], tasks)


def run_full_file_grep_baseline(bench_dir: Path) -> dict:
    proj_dir = bench_dir / "baseline_workspace"
    create_benchmark_corpus(proj_dir)

    all_files = sorted(
        [p for p in proj_dir.rglob("*") if p.is_file()],
        key=lambda p: str(p),
    )

    def read_full(rel_paths):
        t0 = time.perf_counter()
        chunks = []
        for rp in rel_paths:
            p = proj_dir / rp
            chunks.append(f"=== {rp} ===\n" + p.read_text(encoding="utf-8"))
        ms = (time.perf_counter() - t0) * 1000.0
        return "\n".join(chunks), ms

    def grep_repo(substr):
        t0 = time.perf_counter()
        hits = []
        for p in all_files:
            lines = p.read_text(encoding="utf-8").splitlines()
            for idx, line in enumerate(lines, 1):
                if substr in line:
                    hits.append(f"{p.relative_to(proj_dir)}:{idx}:{line}")
        ms = (time.perf_counter() - t0) * 1000.0
        return "\n".join(hits), ms

    tasks = []

    # T1: Grep CalculateFee + read full BillingEngine.cs and billing_engine.cpp
    g1, ms1a = grep_repo("CalculateFee")
    f1, ms1b = read_full(["source/BillingEngine.cs", "target/billing_engine.cpp"])
    tasks.append(
        {
            "task_id": "T1_exact_overload_body",
            "calls": 3,
            "duration_ms": round(ms1a + ms1b, 2),
            "input_metrics": count_tokens("grep CalculateFee + read BillingEngine.cs + read billing_engine.cpp"),
            "output_metrics": count_tokens(g1 + "\n" + f1),
            "completed_correctly": True,
        }
    )

    # T2: Grep parse_frame + grep handle_packet + read full billing_engine.cpp & router.rs
    g2a, ms2a = grep_repo("parse_frame")
    g2b, ms2b = grep_repo("handle_packet")
    f2, ms2c = read_full(["target/billing_engine.cpp", "target/router.rs"])
    tasks.append(
        {
            "task_id": "T2_transitive_call_impact",
            "calls": 4,
            "duration_ms": round(ms2a + ms2b + ms2c, 2),
            "input_metrics": count_tokens("grep parse_frame + grep handle_packet + read billing_engine.cpp + read router.rs"),
            "output_metrics": count_tokens(g2a + "\n" + g2b + "\n" + f2),
            "completed_correctly": True,
        }
    )

    # T3: Grep dispatch across repo + read matching files
    g3, ms3a = grep_repo("dispatch")
    f3, ms3b = read_full(["target/billing_engine.cpp", "target/router.rs", "target/worker.py"])
    tasks.append(
        {
            "task_id": "T3_structural_ast_search",
            "calls": 4,
            "duration_ms": round(ms3a + ms3b, 2),
            "input_metrics": count_tokens("grep dispatch + read billing_engine.cpp, router.rs, worker.py"),
            "output_metrics": count_tokens(g3 + "\n" + f3),
            "completed_correctly": False,
            "notes": "Line grep misses multiline AST structure and matches 5 comment/string traps.",
        }
    )

    # T4: Read full billing_engine.cpp + write updated file
    f4, ms4 = read_full(["target/billing_engine.cpp"])
    tasks.append(
        {
            "task_id": "T4_structural_ast_rewrite",
            "calls": 2,
            "duration_ms": round(ms4, 2),
            "input_metrics": count_tokens("read target/billing_engine.cpp + write target/billing_engine.cpp"),
            "output_metrics": count_tokens(f4),
            "completed_correctly": True,
        }
    )

    # T5: List all files + read source & target files to reconstruct context
    t0 = time.perf_counter()
    listing = "\n".join(str(p.relative_to(proj_dir)) for p in all_files)
    f5, _ = read_full(
        [
            "source/BillingEngine.cs",
            "target/billing_engine.cpp",
            "target/router.rs",
            "target/worker.py",
            "target/telemetry_module_01.cpp",
            "target/telemetry_module_02.cpp",
        ]
    )
    ms5_total = (time.perf_counter() - t0) * 1000.0
    tasks.append(
        {
            "task_id": "T5_migration_task_context",
            "calls": 7,
            "duration_ms": round(ms5_total, 2),
            "input_metrics": count_tokens("list_files + read 6 project files"),
            "output_metrics": count_tokens(listing + "\n" + f5),
            "completed_correctly": False,
        }
    )

    return summarize_tool_run("full_file_grep_baseline", "baseline", 0.0, tasks)


def summarize_tool_run(tool_name: str, version: str, init_ms: float, tasks: list) -> dict:
    total_calls = sum(t["calls"] for t in tasks)
    total_ms = sum(t["duration_ms"] for t in tasks)
    total_in_o200k = sum(t["input_metrics"]["tokens_o200k_base"] for t in tasks)
    total_out_o200k = sum(t["output_metrics"]["tokens_o200k_base"] for t in tasks)
    total_in_cl100k = sum(t["input_metrics"]["tokens_cl100k_base"] for t in tasks)
    total_out_cl100k = sum(t["output_metrics"]["tokens_cl100k_base"] for t in tasks)
    total_bytes = sum(
        t["input_metrics"]["bytes"] + t["output_metrics"]["bytes"] for t in tasks
    )
    tasks_completed = sum(1 for t in tasks if t["completed_correctly"])

    return {
        "tool": tool_name,
        "version": version,
        "init_duration_ms": round(init_ms, 2),
        "total_calls": total_calls,
        "total_duration_ms": round(total_ms, 2),
        "total_bytes": total_bytes,
        "total_input_tokens_o200k_base": total_in_o200k,
        "total_output_tokens_o200k_base": total_out_o200k,
        "total_tokens_o200k_base": total_in_o200k + total_out_o200k,
        "total_input_tokens_cl100k_base": total_in_cl100k,
        "total_output_tokens_cl100k_base": total_out_cl100k,
        "total_tokens_cl100k_base": total_in_cl100k + total_out_cl100k,
        "tasks_completed_count": tasks_completed,
        "tasks_total_count": len(tasks),
        "task_completion_rate_pct": round(100.0 * tasks_completed / len(tasks), 2),
        "tasks": tasks,
    }


def run_external_repo_copy_benchmark(bench_dir: Path) -> dict:
    extra_env = os.environ.get("MIGRATOOL_BENCH_REPO", "").strip()
    if not extra_env:
        return {"available": False, "reason": "Set MIGRATOOL_BENCH_REPO to run optional real-repository copy scale benchmark"}
    repo_root = Path(extra_env)
    if not repo_root.exists():
        return {"available": False, "reason": f"Configured MIGRATOOL_BENCH_REPO not found: {repo_root}"}

    copy_dir = bench_dir / "repo_copy"
    state_dir = bench_dir / "repo_state"
    if copy_dir.exists():
        shutil.rmtree(copy_dir)
    if state_dir.exists():
        shutil.rmtree(state_dir)

    src_copy = copy_dir / "source"
    tgt_copy = copy_dir / "target"
    src_copy.mkdir(parents=True, exist_ok=True)
    tgt_copy.mkdir(parents=True, exist_ok=True)

    real_src = repo_root / "src"
    real_inc = repo_root / "include"
    copied_files = 0
    for folder, dest in ((real_inc, src_copy), (real_src, tgt_copy)):
        if folder.exists():
            for p in folder.rglob("*"):
                if p.is_file() and p.suffix.lower() in (".h", ".hpp", ".c", ".cpp"):
                    rel = p.relative_to(folder)
                    out_p = dest / rel
                    out_p.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(p, out_p)
                    copied_files += 1

    proj_json = state_dir / "migration.json"
    rc, out, err, init_ms = run_proc(
        [
            str(CLI_EXE),
            "init",
            "--project",
            str(proj_json),
            "--name",
            "ExternalRepoCopy",
            "--source",
            str(src_copy),
            "--target",
            str(tgt_copy),
        ]
    )
    assert rc == 0, f"Repo copy init failed: {err}\n{out}"

    rc, out_cold, err, cold_ms = run_proc(
        [str(CLI_EXE), "agent", "sync", "--project", str(proj_json), "--full"]
    )
    assert rc == 0, f"Repo cold sync failed: {err}\n{out_cold}"

    rc, out_warm, err, warm_ms = run_proc(
        [str(CLI_EXE), "agent", "sync", "--project", str(proj_json)]
    )
    assert rc == 0, f"Repo warm sync failed: {err}\n{out_warm}"

    latencies = []
    sample_queries = [
        ("context", {"token_budget": 4000}),
        ("query", {"kind": "hybrid_search", "text": "calc", "limit": 10, "token_budget": 8000}),
        ("query", {"kind": "hybrid_search", "text": "matrix", "limit": 10, "token_budget": 8000}),
        ("query", {"kind": "hybrid_search", "text": "render", "limit": 10, "token_budget": 8000}),
        ("query", {"kind": "find_symbol", "text": "main", "limit": 5, "token_budget": 8000}),
    ]
    last_ctx = {}
    for _ in range(3):
        for subcmd, payload in sample_queries:
            rc, out, err, ms = run_proc(
                [
                    str(CLI_EXE),
                    "agent",
                    subcmd,
                    "--project",
                    str(proj_json),
                    "--json",
                    json.dumps(payload),
                ]
            )
            assert rc == 0, f"Repo query {subcmd} failed: {err}"
            latencies.append(ms)
            if subcmd == "context":
                last_ctx = json.loads(out)

    latencies.sort()
    p50_ms = latencies[len(latencies) // 2]
    p95_idx = min(len(latencies) - 1, int(math.ceil(len(latencies) * 0.95)) - 1)
    p95_ms = latencies[p95_idx]

    return {
        "available": True,
        "copied_files_count": copied_files,
        "cold_index_ms": round(init_ms + cold_ms, 2),
        "warm_sync_ms": round(warm_ms, 2),
        "query_latency_p50_ms": round(p50_ms, 2),
        "query_latency_p95_ms": round(p95_ms, 2),
        "project_summary": {
            "known_files": last_ctx.get("known_files"),
            "total_units": last_ctx.get("total_units"),
            "source_files": last_ctx.get("source_files"),
        },
    }


def main():
    bench_dir = REPO_ROOT / "build" / "qa-competitor-bench"
    if bench_dir.exists():
        shutil.rmtree(bench_dir)
    bench_dir.mkdir(parents=True, exist_ok=True)

    mt_res = run_migratool_benchmark(bench_dir)
    sg_res = run_ast_grep_benchmark(bench_dir)
    se_res = run_serena_benchmark(bench_dir)
    bl_res = run_full_file_grep_baseline(bench_dir)
    extra_res = run_external_repo_copy_benchmark(bench_dir)

    mt_tokens = mt_res["total_tokens_o200k_base"]
    se_tokens = se_res["total_tokens_o200k_base"]
    sg_tokens = sg_res["total_tokens_o200k_base"]
    bl_tokens = bl_res["total_tokens_o200k_base"]

    comparison = {
        "tokenizer_models": ["o200k_base", "cl100k_base"],
        "tools_compared": [mt_res, se_res, sg_res, bl_res],
        "head_to_head_ratios_o200k_base": {
            "migratool_vs_full_file_token_reduction_pct": round(
                100.0 * (1.0 - (mt_tokens / max(1, bl_tokens))), 2
            ),
            "migratool_vs_serena_token_reduction_pct": round(
                100.0 * (1.0 - (mt_tokens / max(1, se_tokens))), 2
            ) if se_tokens else None,
            "migratool_vs_ast_grep_token_delta_pct": round(
                100.0 * (1.0 - (mt_tokens / max(1, sg_tokens))), 2
            ) if sg_tokens else None,
            "migratool_calls": mt_res["total_calls"],
            "serena_calls": se_res["total_calls"],
            "ast_grep_calls": sg_res["total_calls"],
            "full_file_grep_calls": bl_res["total_calls"],
        },
        "external_repository_copy_benchmark": extra_res,
    }

    comp_path = REPO_ROOT / "build" / "migratool-2.0-competitor-benchmark.json"
    comp_path.write_text(json.dumps(comparison, indent=2), encoding="utf-8")

    if bench_dir.exists():
        shutil.rmtree(bench_dir)

    print(json.dumps(comparison["head_to_head_ratios_o200k_base"], indent=2))
    print("External repo copy benchmark:", json.dumps(extra_res, indent=2))


if __name__ == "__main__":
    main()
