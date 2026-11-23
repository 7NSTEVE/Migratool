#pragma once
#include "migratool/core.hpp"
#include <string>
#include <vector>

namespace mt {

struct ClangAstFunction {
    std::string qualified_name;
    std::string display_name;
    std::string kind;
    std::string signature;
    int start_line = 0;
    int end_line = 0;
    int cyclomatic_complexity = 1;
    bool is_definition = false;
};

// Extracts C/C++ function, method, constructor, destructor, conversion function,
// and function template definitions using the process-lifetime libclang DLL.
// Returns true when libclang loaded and parsed a valid translation unit;
// returns false if libclang is unavailable or failed to parse the translation unit.
bool extract_cpp_functions_libclang(
    const fs::path& file_path,
    const std::vector<std::string>& compile_args,
    std::vector<ClangAstFunction>& out_functions,
    std::string& out_clang_version,
    std::string& out_error);

} // namespace mt
