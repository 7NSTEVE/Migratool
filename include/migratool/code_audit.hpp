#pragma once
#include "migratool/core.hpp"
#include "migratool/sql.hpp"

namespace mt {

// Ensures SQLite tables for the Modular Incremental Continuous Technical Audit
// and Anti-Hallucination Agent Claim Verifier exist (Conversación CGPT.txt lines 838–1046):
//   - code_audit_files (keyed by file_path, file_sha256, compile_flags_sha256, headers_deps_sha256, analyzer_version)
//   - code_audit_header_deps (tu_path, header_path, header_sha256 for transitive header invalidation)
//   - code_audit_functions (func_key = file_path:qualified_name:start_line, AST cyclomatic complexity, TEST_CASE disambiguation)
//   - code_audit_findings (language-aware C vs C++23 rules, test-aware security triage PENDING_VALIDATION vs CONFIRMED)
//   - code_audit_snapshots (audit history snapshots by git commit for regression attribution)
//   - agent_claim_verifications (independent verification of agent delivery claims across 5 evidence dimensions)
void ensure_code_audit_schema(Database& db);

// REQ-AUDIT-001..004: Modular Incremental Technical Audit & Agent-Claim Verifier
// Actions:
//   - "scan_incremental" | "audit" | "scan" (default):
//       Performs incremental AST & quality/security/compilation-db audit. Reuses cached results for
//       unchanged files whose compile flags and transitive #include headers have not changed;
//       re-analyzes only modified files and translation units affected by modified headers or compile_commands.json.
//       Distinguishes C (NULL valid) from C++23 (nullptr), disambiguates duplicate TEST_CASE macros by line,
//       computes AST cyclomatic complexity, and classifies test-file security patterns as PENDING_VALIDATION.
//       Optional `include_clang_tidy: true` runs bounded clang-tidy in a Win32 Job Object on changed files.
//   - "functions":
//       Queries AST-extracted functions/methods/test cases with cyclomatic_complexity, line_count,
//       analysis_source ("clang_ast" | "syntactic_ast"), last_commit_sha, last_author_agent,
//       covering_test_suite, and included_build_id.
//   - "findings":
//       Queries findings filtered by severity, category, triage_status ("CONFIRMED" | "PENDING_VALIDATION" |
//       "FALSE_POSITIVE" | "SUPPRESSED"), file_path, or rule_id.
//   - "triage":
//       Updates triage_status and suppression_reason for a finding_id.
//   - "verify_claim":
//       Anti-hallucination verifier for agent completion claims. Independently checks:
//         1) functions_evidence (AST count vs heuristic/claimed count)
//         2) compilation_evidence (VERIFIED_BUILD_PASS | FAILED | NO_EVIDENCE against build_manifests & current SHA-256)
//         3) tests_evidence (VERIFIED_TESTS_PASS | FAILED | STALE_AFTER_EDIT | NO_EVIDENCE against verified_runs)
//         4) quality_evidence (max AST complexity, open CONFIRMED P0/P1 findings, PENDING_VALIDATION test findings)
//         5) functional_equivalence_evidence (VERIFIED_IN_CASES | NOT_PROVEN — never conflated with build/unit tests)
//       Returns verdict: "VERIFIED" | "PARTIALLY_VERIFIED" | "UNVERIFIED_CLAIM" | "CONTRADICTED_BY_EVIDENCE".
//   - "regressions":
//       Compares current audit state against previous snapshot or base commit, attributing new findings
//       and complexity increases to exact commit, branch/worktree, and agent.
//   - "gate_check":
//       Evaluates configurable acceptance rules (max_confirmed_p0, require_compiled_in_db, require_verified_tests)
//       for pre-merge and canonical build gates.
Json tool_code_audit(const Project& p, Database& db, const Json& args, Cancel* cancel = nullptr);

} // namespace mt
