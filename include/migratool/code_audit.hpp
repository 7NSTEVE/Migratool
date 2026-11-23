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
//       Performs incremental AST & quality/security/compilation-db audit (default analyzer_version:
//       "code_audit/2-libclang"). Reuses cached results for unchanged files whose SHA-256 content hash,
//       compile flags, transitive #include headers, and analyzer_version have not changed; re-analyzes
//       only modified files and translation units affected by modified headers or compile_commands.json.
//       Uses real libclang AST extraction for C/C++ files (.c, .cc, .cpp, .cxx, .c++, .h, .hh, .hpp, .hxx)
//       labeling libclang-extracted definitions "clang_ast", while standalone test macros (TEST_CASE/TEST/SCENARIO
//       when headers are unavailable), libclang fallbacks, and non-C/C++ files are honestly labeled
//       "syntactic_tokens" (never mislabeled as "clang_ast").
//       Distinguishes C (NULL valid) from C++23 (nullptr), disambiguates duplicate TEST_CASE macros by line,
//       computes AST cyclomatic complexity, and classifies test-file security patterns as PENDING_VALIDATION.
//       Optional `verify_content_hash: true` enforces live SHA-256 verification against disk (catching
//       same-mtime/same-size edits). Optional `include_clang_tidy: true` runs bounded clang-tidy in a
//       Win32 Job Object on changed files.
//   - "functions":
//       Queries AST-extracted functions/methods/test cases with signature, cyclomatic_complexity, line_count,
//       honest analysis_source ("clang_ast" | "syntactic_tokens"), last_commit_sha, last_author_agent,
//       covering_test_suite, and included_build_id.
//   - "findings":
//       Queries findings filtered by severity, category, triage_status ("CONFIRMED" | "PENDING_VALIDATION" |
//       "FALSE_POSITIVE" | "SUPPRESSED"), file_path, or rule_id.
//   - "triage":
//       Updates triage_status and suppression_reason for a finding_id.
//   - "verify_claim":
//       Anti-hallucination verifier for agent completion claims with strict cryptographic and repository
//       identity binding across 5 independent evidence dimensions:
//         1) functions_evidence (real AST/token count vs claimed count with honest analysis_source)
//         2) compilation_evidence (VERIFIED_BUILD_PASS | FAILED | UNCOMPILED_FILE | STALE_BUILD |
//            EVIDENCE_COMMIT_MISMATCH | EVIDENCE_WORKTREE_MISMATCH | UNVERIFIED_EXTERNAL_BUILD |
//            ARTIFACT_HASH_MISMATCH | NO_EVIDENCE — strictly bound to canonical worktree_path, live git HEAD,
//            exit_code == 0, and OBSERVED_BUILD provenance or DECLARED_EXTERNAL with live disk SHA-256 verification)
//         3) tests_evidence (VERIFIED_TESTS_PASS | FAILED | STALE_AFTER_EDIT | STALE_COMMIT_TEST_EVIDENCE |
//            NO_MODULE_TEST_EVIDENCE | NO_EVIDENCE — requires exit_code == 0, tests_detected != 0, passed_count > 0,
//            live git HEAD match, post-edit freshness, and structural JSON module/suite binding over
//            selected_targets_json and command_json; never substring-matched against raw logs)
//         4) quality_evidence (max AST complexity, open CONFIRMED P0/P1 findings, PENDING_VALIDATION test findings)
//         5) functional_equivalence_evidence (VERIFIED_IN_CASES | NOT_PROVEN — never conflated with build/unit tests)
//       Returns verdict: "VERIFIED" (only when BOTH compilation_evidence == VERIFIED_BUILD_PASS AND
//       tests_evidence == VERIFIED_TESTS_PASS with zero contradictions) | "PARTIALLY_VERIFIED" |
//       "UNVERIFIED_CLAIM" | "CONTRADICTED_BY_EVIDENCE".
//   - "regressions":
//       Compares current audit state against previous snapshot or base commit, attributing new findings
//       and complexity increases to exact commit, branch/worktree, and agent.
//   - "gate_check":
//       Always performs a fresh incremental rescan with verify_content_hash=true before evaluating gates
//       (unless `allow_stale_cache: true` is explicitly passed) and reports `cache_fresh`, `rescan_performed`,
//       and `rescan_summary`. Evaluates fail-closed acceptance rules (`max_confirmed_p0` defaults to 0;
//       when `strict: true` or `release_gate: true`, `max_confirmed_p1` also defaults to 0; plus
//       `require_compiled_in_db` and `require_verified_tests`).
Json tool_code_audit(const Project& p, Database& db, const Json& args, Cancel* cancel = nullptr);

} // namespace mt
