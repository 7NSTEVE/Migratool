using System.Collections.Concurrent;
using System.Reflection.PortableExecutable;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;

if (args.Length != 2) { Console.Error.WriteLine("Uso: RoslynAdapter request.json output.json"); return 2; }
try {
    var request = JsonNode.Parse(File.ReadAllText(args[0]))!;
    var authoritativeContext = request["context_authoritative"]?.GetValue<bool>() ?? false;
    var contextBudget = request["context_budget_bytes"]?.GetValue<long>() ?? long.MaxValue;
    var parallel = new ParallelOptions { MaxDegreeOfParallelism = authoritativeContext ? 4 : -1 };
    var baseDefines = request["defines"]?.AsArray().Select(n => n!.GetValue<string>()).Where(s => !string.IsNullOrWhiteSpace(s)).ToHashSet(StringComparer.Ordinal)
                      ?? new HashSet<string>(StringComparer.Ordinal);

    var requestedPaths = new List<string>();
    var requestedSet = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
    foreach (var f in request["files"]!.AsArray()) {
        var p = f!.GetValue<string>();
        requestedPaths.Add(p);
        requestedSet.Add(Path.GetFullPath(p));
    }

    var contextPaths = new List<string>(requestedPaths);
    if (request["context_files"] is JsonArray ctxArr) {
        foreach (var cf in ctxArr) {
            var p = cf?.GetValue<string>();
            if (!string.IsNullOrEmpty(p) && (authoritativeContext || File.Exists(p)) && requestedSet.Add(Path.GetFullPath(p)))
                contextPaths.Add(p);
        }
    }

    // H-07: Descubrimiento de soluciones (.sln) y proyectos (.csproj) reales
    var csprojPaths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
    var slnPaths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
    var approvedConfigPaths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
    if (request["project_files"] is JsonArray projArr) {
        foreach (var pf in projArr) {
            var p = pf?.GetValue<string>();
            if (!string.IsNullOrEmpty(p) && File.Exists(p)) {
                var full = Path.GetFullPath(p);
                csprojPaths.Add(full);
                approvedConfigPaths.Add(full);
            }
        }
    }
    if (request["project_config_files"] is JsonArray cfgArr) {
        foreach (var cf in cfgArr) {
            var p = cf?.GetValue<string>();
            if (!string.IsNullOrEmpty(p) && File.Exists(p))
                approvedConfigPaths.Add(Path.GetFullPath(p));
        }
    }
    if (request["solution_files"] is JsonArray slnArr) {
        foreach (var sf in slnArr) {
            var p = sf?.GetValue<string>();
            if (!string.IsNullOrEmpty(p) && File.Exists(p)) {
                var full = Path.GetFullPath(p);
                slnPaths.Add(full);
                approvedConfigPaths.Add(full);
            }
        }
    }

    // Buscar también hacia arriba desde los archivos solicitados por si el .csproj o .sln está en un directorio padre
    if (!authoritativeContext) foreach (var p in requestedPaths) {
        var dir = Path.GetDirectoryName(Path.GetFullPath(p));
        int levels = 0;
        while (!string.IsNullOrEmpty(dir) && levels < 5) {
            try {
                foreach (var cp in Directory.GetFiles(dir, "*.csproj")) csprojPaths.Add(Path.GetFullPath(cp));
                foreach (var sp in Directory.GetFiles(dir, "*.sln")) slnPaths.Add(Path.GetFullPath(sp));
            } catch { }
            dir = Path.GetDirectoryName(dir);
            ++levels;
        }
    }

    // Parsear archivos .sln para encontrar .csproj referenciados en la solución
    var slnProjectRegex = new Regex(@"Project\(""\{[^""]+\}""\)\s*=\s*""[^""]+""\s*,\s*""([^""]+\.csproj)""", RegexOptions.IgnoreCase);
    foreach (var sln in slnPaths) {
        try {
            var slnDir = Path.GetDirectoryName(sln)!;
            var slnText = File.ReadAllText(sln);
            foreach (Match m in slnProjectRegex.Matches(slnText)) {
                var rel = m.Groups[1].Value.Replace('\\', Path.DirectorySeparatorChar);
                var full = Path.GetFullPath(Path.Combine(slnDir, rel));
                if (File.Exists(full)) csprojPaths.Add(full);
            }
        } catch { }
    }

    // Cargar todas las referencias válidas de metadatos del runtime .NET actual
    var runtimeReferences = new List<MetadataReference>();
    var runtimeDir = RuntimeEnvironment.GetRuntimeDirectory();
    var loadedDllPaths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
    if (Directory.Exists(runtimeDir)) {
        foreach (var dll in Directory.GetFiles(runtimeDir, "*.dll")) {
            var name = Path.GetFileName(dll);
            if (name.StartsWith("System.", StringComparison.OrdinalIgnoreCase) ||
                name.StartsWith("Microsoft.", StringComparison.OrdinalIgnoreCase) ||
                name.Equals("mscorlib.dll", StringComparison.OrdinalIgnoreCase) ||
                name.Equals("netstandard.dll", StringComparison.OrdinalIgnoreCase) ||
                name.Equals("WindowsBase.dll", StringComparison.OrdinalIgnoreCase)) {
                try {
                    using var fs = File.OpenRead(dll);
                    using var pe = new PEReader(fs);
                    if (pe.HasMetadata) {
                        runtimeReferences.Add(MetadataReference.CreateFromFile(dll));
                        loadedDllPaths.Add(Path.GetFullPath(dll));
                    }
                } catch { }
            }
        }
    }

    // Leer todos los archivos fuente en paralelo
    var textByPath = new ConcurrentDictionary<string, string>(StringComparer.OrdinalIgnoreCase);
    var bomByPath = new ConcurrentDictionary<string, int>(StringComparer.OrdinalIgnoreCase);
    long contextBytesRead = 0;
    Parallel.ForEach(contextPaths, parallel, path => {
        try {
            using var input = File.OpenRead(path);
            var length = input.Length;
            if (authoritativeContext && (length > contextBudget || Interlocked.Add(ref contextBytesRead, length) > contextBudget))
                throw new InvalidOperationException("RESOURCE_CONTEXT_LIMIT: source component changed beyond its recorded byte budget");
            var raw = new byte[checked((int)length)];
            input.ReadExactly(raw);
            int bom = (raw.Length >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) ? 3 : 0;
            textByPath[path] = Encoding.UTF8.GetString(raw, bom, raw.Length - bom);
            bomByPath[path] = bom;
        } catch { if (authoritativeContext) throw; }
    });

    // Estructura de descriptor de cada .csproj con evaluación de Directory.Build.props / .props / .targets / propiedades MSBuild
    var projectsByPath = new Dictionary<string, CsProjInfo>(StringComparer.OrdinalIgnoreCase);
    var nugetRoot = Environment.GetEnvironmentVariable("NUGET_PACKAGES")
                    ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".nuget", "packages");

    bool CanReadConfigFile(string fullPath) =>
        File.Exists(fullPath) && (!authoritativeContext || approvedConfigPaths.Contains(fullPath));

    string FindFileAboveConfig(string fileName, string? startDir, bool dirOnly) {
        if (string.IsNullOrWhiteSpace(startDir)) return "";
        try {
            var cur = Path.GetFullPath(startDir.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
            for (int depth = 0; depth < 32 && !string.IsNullOrEmpty(cur); ++depth) {
                var cand = Path.Combine(cur, fileName);
                if (CanReadConfigFile(cand)) return dirOnly ? cur + Path.DirectorySeparatorChar : cand;
                var parent = Path.GetDirectoryName(cur);
                if (string.IsNullOrEmpty(parent) || parent == cur) break;
                cur = parent;
            }
        } catch { }
        return "";
    }

    string ExpandProps(string value, Dictionary<string, string> props, string currentDir) {
        var cur = value;
        for (int pass = 0; pass < 16; ++pass) {
            bool changed = false;
            var sb = new StringBuilder(cur.Length);
            for (int i = 0; i < cur.Length;) {
                if (i + 1 < cur.Length && cur[i] == '$' && cur[i + 1] == '(') {
                    int depth = 1, j = i + 2;
                    while (j < cur.Length && depth > 0) {
                        if (cur[j] == '$' && j + 1 < cur.Length && cur[j + 1] == '(') { depth++; j += 2; }
                        else if (cur[j] == ')') { depth--; j++; }
                        else j++;
                    }
                    if (depth == 0) {
                        var inner = cur.Substring(i + 2, j - i - 3).Trim();
                        string? repl = null;
                        if (inner.StartsWith("[MSBuild]::GetPathOfFileAbove(", StringComparison.OrdinalIgnoreCase) ||
                            inner.StartsWith("[MSBuild]::GetDirectoryNameOfFileAbove(", StringComparison.OrdinalIgnoreCase)) {
                            bool dirOnly = inner.StartsWith("[MSBuild]::GetDirectoryNameOfFileAbove(", StringComparison.OrdinalIgnoreCase);
                            int op = inner.IndexOf('('), cl = inner.LastIndexOf(')');
                            if (op >= 0 && cl > op) {
                                var parts = inner.Substring(op + 1, cl - op - 1).Split(',');
                                static string StripArg(string s) => s.Trim().Trim('\'', '"');
                                var fn = parts.Length > 0 ? ExpandProps(StripArg(parts[0]), props, currentDir) : "";
                                var sd = parts.Length > 1 ? ExpandProps(StripArg(parts[1]), props, currentDir) : currentDir;
                                if (!string.IsNullOrWhiteSpace(fn)) repl = FindFileAboveConfig(fn, sd, dirOnly);
                            }
                        } else if (props.TryGetValue(inner, out var pv)) {
                            repl = pv;
                        } else {
                            repl = Environment.GetEnvironmentVariable(inner);
                        }
                        if (repl != null) { sb.Append(repl); changed = true; i = j; continue; }
                    }
                }
                sb.Append(cur[i++]);
            }
            cur = sb.ToString();
            if (!changed) break;
        }
        return cur;
    }

    static Regex GlobToRegex(string pattern) {
        var norm = pattern.Replace('\\', '/');
        var escaped = Regex.Escape(norm).Replace(@"\*\*/", "(.+/)?").Replace(@"\*", "[^/]*").Replace(@"\?", ".");
        return new Regex("^" + escaped + "$", RegexOptions.IgnoreCase | RegexOptions.Compiled);
    }

    static bool GlobMatches(string patternFull, string candidateFull) {
        return GlobToRegex(Path.GetFullPath(patternFull)).IsMatch(Path.GetFullPath(candidateFull).Replace('\\', '/'));
    }

    void LoadCsProjRecursive(string csprojFull) {
        if (projectsByPath.ContainsKey(csprojFull) || !File.Exists(csprojFull)) return;
        if (authoritativeContext && !csprojPaths.Contains(csprojFull))
            throw new InvalidOperationException($"CONTEXT_REFERENCE_NOT_IN_REQUEST: {csprojFull}");
        var info = new CsProjInfo(csprojFull, Path.GetDirectoryName(csprojFull)!);
        info.Defines.UnionWith(baseDefines);
        info.Defines.Add("TRACE");
        info.Defines.Add("DEBUG");

        var props = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase) {
            ["MSBuildProjectDirectory"] = info.ProjectDir,
            ["ProjectDir"] = info.ProjectDir + Path.DirectorySeparatorChar,
            ["MSBuildProjectFile"] = Path.GetFileName(csprojFull),
            ["MSBuildProjectName"] = Path.GetFileNameWithoutExtension(csprojFull),
            ["Configuration"] = "Debug",
            ["Platform"] = "AnyCPU"
        };
        var matchedSln = slnPaths.Where(s => info.ProjectDir.StartsWith(Path.GetDirectoryName(s) ?? "", StringComparison.OrdinalIgnoreCase))
                                 .OrderByDescending(s => s.Length).FirstOrDefault();
        if (!string.IsNullOrEmpty(matchedSln))
            props["SolutionDir"] = Path.GetDirectoryName(matchedSln)! + Path.DirectorySeparatorChar;
        else
            props["SolutionDir"] = info.ProjectDir + Path.DirectorySeparatorChar;

        var visitedXml = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        void EvalXmlFile(string xmlFull) {
            xmlFull = Path.GetFullPath(xmlFull);
            if (!visitedXml.Add(xmlFull) || !CanReadConfigFile(xmlFull)) return;
            info.ConfigFiles.Add(xmlFull);
            var fileDir = Path.GetDirectoryName(xmlFull)!;
            var prevDir = props.GetValueOrDefault("MSBuildThisFileDirectory");
            var prevFile = props.GetValueOrDefault("MSBuildThisFile");
            var prevName = props.GetValueOrDefault("MSBuildThisFileName");
            var prevExt = props.GetValueOrDefault("MSBuildThisFileExtension");
            props["MSBuildThisFileDirectory"] = fileDir + Path.DirectorySeparatorChar;
            props["MSBuildThisFile"] = Path.GetFileName(xmlFull);
            props["MSBuildThisFileName"] = Path.GetFileNameWithoutExtension(xmlFull);
            props["MSBuildThisFileExtension"] = Path.GetExtension(xmlFull);
            try {
                var doc = XDocument.Load(xmlFull);
                if (doc.Root != null) {
                    foreach (var elem in doc.Root.Elements()) {
                        var elName = elem.Name.LocalName;
                        if (elName == "Import") {
                            var imp = elem.Attribute("Project")?.Value;
                            if (!string.IsNullOrWhiteSpace(imp)) {
                                var exp = ExpandProps(imp, props, fileDir);
                                if (!exp.Contains("$(")) {
                                    var cand = Path.GetFullPath(Path.IsPathRooted(exp) ? exp : Path.Combine(fileDir, exp.Replace('\\', Path.DirectorySeparatorChar)));
                                    EvalXmlFile(cand);
                                }
                            }
                        } else if (elName == "PropertyGroup") {
                            foreach (var propEl in elem.Elements()) {
                                var pname = propEl.Name.LocalName;
                                var pval = ExpandProps(propEl.Value.Trim(), props, fileDir);
                                props[pname] = pval;
                                if (pname is "TargetFramework" or "TargetFrameworks") {
                                    foreach (var part in pval.Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)) {
                                        info.TargetFrameworks.Add(part);
                                        var upperTf = part.ToUpperInvariant().Replace(".", "_").Replace("-", "_");
                                        info.Defines.Add(upperTf);
                                        if (part.StartsWith("net", StringComparison.OrdinalIgnoreCase)) {
                                            info.Defines.Add("NET"); info.Defines.Add("NETCOREAPP");
                                            info.Defines.Add("NET5_0_OR_GREATER"); info.Defines.Add("NET6_0_OR_GREATER");
                                            info.Defines.Add("NET7_0_OR_GREATER"); info.Defines.Add("NET8_0_OR_GREATER");
                                        }
                                    }
                                } else if (pname == "DefineConstants") {
                                    foreach (var sym in pval.Split([';', ','], StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)) {
                                        if (!sym.StartsWith("$(")) info.Defines.Add(sym);
                                    }
                                } else if (pname == "ImplicitUsings") {
                                    info.ImplicitUsings = pval.Equals("enable", StringComparison.OrdinalIgnoreCase) || pval.Equals("true", StringComparison.OrdinalIgnoreCase);
                                } else if (pname == "EnableDefaultCompileItems" && pval.Equals("false", StringComparison.OrdinalIgnoreCase)) {
                                    info.EnableDefaultCompileItems = false;
                                } else if (pname == "AllowUnsafeBlocks") {
                                    info.AllowUnsafe = !pval.Equals("false", StringComparison.OrdinalIgnoreCase);
                                } else if (pname == "Nullable") {
                                    info.Nullable = pval.Equals("enable", StringComparison.OrdinalIgnoreCase) || pval.Equals("warnings", StringComparison.OrdinalIgnoreCase);
                                }
                            }
                        } else if (elName == "ItemGroup") {
                            foreach (var itemEl in elem.Elements()) {
                                var iname = itemEl.Name.LocalName;
                                if (iname == "Using") {
                                    var inc = itemEl.Attribute("Include")?.Value;
                                    if (!string.IsNullOrWhiteSpace(inc)) {
                                        var exp = ExpandProps(inc, props, fileDir);
                                        var alias = itemEl.Attribute("Alias")?.Value;
                                        var isStatic = itemEl.Attribute("Static")?.Value;
                                        if (string.Equals(isStatic, "true", StringComparison.OrdinalIgnoreCase))
                                            info.GlobalUsings.Add($"global using static {exp};");
                                        else if (!string.IsNullOrWhiteSpace(alias))
                                            info.GlobalUsings.Add($"global using {alias} = {exp};");
                                        else
                                            info.GlobalUsings.Add($"global using {exp};");
                                    }
                                } else if (iname == "ProjectReference") {
                                    var inc = itemEl.Attribute("Include")?.Value;
                                    if (!string.IsNullOrWhiteSpace(inc)) {
                                        foreach (var rawPart in inc.Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)) {
                                            var exp = ExpandProps(rawPart, props, fileDir).Replace('\\', Path.DirectorySeparatorChar);
                                            if (exp.Contains("$(")) continue;
                                            if (exp.IndexOfAny(['*', '?']) >= 0) {
                                                var patFull = Path.GetFullPath(Path.IsPathRooted(exp) ? exp : Path.Combine(fileDir, exp));
                                                foreach (var candCsproj in csprojPaths)
                                                    if (GlobMatches(patFull, candCsproj)) info.ProjectReferences.Add(candCsproj);
                                            } else {
                                                var refFull = Path.GetFullPath(Path.IsPathRooted(exp) ? exp : Path.Combine(fileDir, exp));
                                                if (File.Exists(refFull)) info.ProjectReferences.Add(refFull);
                                            }
                                        }
                                    }
                                } else if (iname == "Compile") {
                                    var inc = itemEl.Attribute("Include")?.Value;
                                    if (!string.IsNullOrWhiteSpace(inc)) {
                                        foreach (var p in inc.Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)) {
                                            var exp = ExpandProps(p, props, fileDir).Replace('\\', Path.DirectorySeparatorChar);
                                            if (!exp.Contains("$("))
                                                info.CompileIncludes.Add(Path.GetFullPath(Path.IsPathRooted(exp) ? exp : Path.Combine(fileDir, exp)));
                                        }
                                    }
                                    var rem = itemEl.Attribute("Remove")?.Value;
                                    if (!string.IsNullOrWhiteSpace(rem)) {
                                        foreach (var p in rem.Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)) {
                                            var exp = ExpandProps(p, props, fileDir).Replace('\\', Path.DirectorySeparatorChar);
                                            if (!exp.Contains("$("))
                                                info.CompileRemoves.Add(Path.GetFullPath(Path.IsPathRooted(exp) ? exp : Path.Combine(fileDir, exp)));
                                        }
                                    }
                                } else if (iname == "Reference") {
                                    var hp = itemEl.Elements().FirstOrDefault(e => e.Name.LocalName == "HintPath")?.Value ?? itemEl.Attribute("HintPath")?.Value;
                                    if (!string.IsNullOrWhiteSpace(hp)) {
                                        var exp = ExpandProps(hp, props, fileDir).Replace('\\', Path.DirectorySeparatorChar);
                                        var dllFull = Path.GetFullPath(Path.IsPathRooted(exp) ? exp : Path.Combine(fileDir, exp));
                                        if (File.Exists(dllFull)) info.ExternalDlls.Add(dllFull);
                                    }
                                }
                            }
                        }
                    }
                }
            } catch { }
            finally {
                if (prevDir != null) props["MSBuildThisFileDirectory"] = prevDir;
                if (prevFile != null) props["MSBuildThisFile"] = prevFile;
                if (prevName != null) props["MSBuildThisFileName"] = prevName;
                if (prevExt != null) props["MSBuildThisFileExtension"] = prevExt;
            }
        }

        try {
            var dbProps = FindFileAboveConfig("Directory.Build.props", info.ProjectDir, false);
            if (!string.IsNullOrEmpty(dbProps)) EvalXmlFile(dbProps);
            EvalXmlFile(csprojFull);
            if (props.GetValueOrDefault("ImportDirectoryBuildTargets", "true") != "false") {
                var dbTargets = FindFileAboveConfig("Directory.Build.targets", info.ProjectDir, false);
                if (!string.IsNullOrEmpty(dbTargets)) EvalXmlFile(dbTargets);
            }

            // Resolver paquetes NuGet desde obj/project.assets.json si existe
            var assetsPath = Path.Combine(info.ProjectDir, "obj", "project.assets.json");
            if (File.Exists(assetsPath) && Directory.Exists(nugetRoot)) {
                var assets = JsonNode.Parse(File.ReadAllText(assetsPath));
                if (assets?["targets"] is JsonObject targetsObj) {
                    foreach (var tfTarget in targetsObj) {
                        if (tfTarget.Value is JsonObject libsObj) {
                            foreach (var libEntry in libsObj) {
                                if (libEntry.Value?["compile"] is JsonObject compileObj) {
                                    foreach (var relDll in compileObj) {
                                        if (relDll.Key.EndsWith(".dll", StringComparison.OrdinalIgnoreCase) && !relDll.Key.EndsWith("_._")) {
                                            var candDll = Path.GetFullPath(Path.Combine(nugetRoot, libEntry.Key.ToLowerInvariant(), relDll.Key.Replace('/', Path.DirectorySeparatorChar)));
                                            if (File.Exists(candDll)) info.ExternalDlls.Add(candDll);
                                        }
                                    }
                                }
                            }
                        }
                        break; // usar el primer target framework resuelto
                    }
                }
            }
        } catch { }

        projectsByPath[csprojFull] = info;
        foreach (var childRef in info.ProjectReferences) {
            LoadCsProjRecursive(childRef);
        }
    }

    foreach (var cp in csprojPaths) LoadCsProjRecursive(cp);

    // Asignar cada archivo .cs al .csproj cuyo directorio o <Compile Include> aplique, respetando <Compile Remove> y <EnableDefaultCompileItems>
    var sortedProjects = projectsByPath.Values.OrderByDescending(p => p.ProjectDir.Length).ToList();
    var looseFiles = new List<string>();
    foreach (var path in contextPaths) {
        if (!textByPath.ContainsKey(path)) continue;
        var full = Path.GetFullPath(path);
        CsProjInfo? owner = null;
        foreach (var proj in sortedProjects) {
            if (proj.CompileIncludes.Any(pat => GlobMatches(pat, full))) { owner = proj; break; }
            var dirWithSep = proj.ProjectDir.EndsWith(Path.DirectorySeparatorChar) ? proj.ProjectDir : proj.ProjectDir + Path.DirectorySeparatorChar;
            if (full.StartsWith(dirWithSep, StringComparison.OrdinalIgnoreCase)) {
                if (!proj.EnableDefaultCompileItems) continue;
                if (proj.CompileRemoves.Any(pat => GlobMatches(pat, full))) continue;
                owner = proj;
                break;
            }
        }
        if (owner != null) owner.SourceFiles.Add(path);
        else looseFiles.Add(path);
    }

    // Si un .csproj referenciado no tenía archivos en contextPaths, recolectar sus archivos .cs para que su compilación esté completa
    foreach (var proj in projectsByPath.Values) {
        if (!authoritativeContext && proj.SourceFiles.Count == 0 && Directory.Exists(proj.ProjectDir)) {
            try {
                foreach (var csFile in Directory.EnumerateFiles(proj.ProjectDir, "*.cs", SearchOption.AllDirectories)) {
                    var rel = Path.GetRelativePath(proj.ProjectDir, csFile);
                    if (rel.StartsWith("bin" + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) ||
                        rel.StartsWith("obj" + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) continue;
                    var fullCs = Path.GetFullPath(csFile);
                    if (!textByPath.ContainsKey(fullCs)) {
                        var raw = File.ReadAllBytes(fullCs);
                        int bom = (raw.Length >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) ? 3 : 0;
                        textByPath[fullCs] = Encoding.UTF8.GetString(raw, bom, raw.Length - bom);
                        bomByPath[fullCs] = bom;
                    }
                    proj.SourceFiles.Add(fullCs);
                }
            } catch { }
        }
    }

    var treeByFile = new ConcurrentDictionary<string, SyntaxTree>(StringComparer.OrdinalIgnoreCase);
    var compilationByFile = new ConcurrentDictionary<string, CSharpCompilation>(StringComparer.OrdinalIgnoreCase);
    var projNameByFile = new ConcurrentDictionary<string, string>(StringComparer.OrdinalIgnoreCase);
    var builtCompilations = new Dictionary<string, CSharpCompilation>(StringComparer.OrdinalIgnoreCase);
    var visiting = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

    CSharpCompilation BuildProjectCompilation(CsProjInfo proj) {
        if (builtCompilations.TryGetValue(proj.CsProjPath, out var existing)) return existing;
        visiting.Add(proj.CsProjPath);

        var parseOpts = CSharpParseOptions.Default
            .WithLanguageVersion(LanguageVersion.Preview)
            .WithPreprocessorSymbols(proj.Defines);

        var trees = new ConcurrentBag<SyntaxTree>();
        if (proj.ImplicitUsings || proj.GlobalUsings.Count > 0) {
            var gu = new StringBuilder();
            if (proj.ImplicitUsings) {
                gu.AppendLine("global using System;");
                gu.AppendLine("global using System.Collections.Generic;");
                gu.AppendLine("global using System.IO;");
                gu.AppendLine("global using System.Linq;");
                gu.AppendLine("global using System.Net.Http;");
                gu.AppendLine("global using System.Threading;");
                gu.AppendLine("global using System.Threading.Tasks;");
            }
            foreach (var line in proj.GlobalUsings) gu.AppendLine(line);
            trees.Add(CSharpSyntaxTree.ParseText(gu.ToString(), parseOpts, Path.Combine(proj.ProjectDir, "__GlobalUsings.g.cs"), Encoding.UTF8));
        }

        Parallel.ForEach(proj.SourceFiles, parallel, srcFile => {
            if (!textByPath.TryGetValue(srcFile, out var srcText)) return;
            var tr = CSharpSyntaxTree.ParseText(srcText, parseOpts, srcFile, Encoding.UTF8);
            trees.Add(tr);
            treeByFile[srcFile] = tr;
            projNameByFile[srcFile] = Path.GetFileName(proj.CsProjPath);
        });

        var refs = new List<MetadataReference>(runtimeReferences);
        foreach (var extDll in proj.ExternalDlls) {
            if (!loadedDllPaths.Contains(extDll)) {
                try { refs.Add(MetadataReference.CreateFromFile(extDll)); } catch { }
            }
        }
        foreach (var pref in proj.ProjectReferences) {
            if (!visiting.Contains(pref) && projectsByPath.TryGetValue(pref, out var refProj)) {
                var refComp = BuildProjectCompilation(refProj);
                refs.Add(refComp.ToMetadataReference());
            }
        }

        var comp = CSharpCompilation.Create(
            Path.GetFileNameWithoutExtension(proj.CsProjPath),
            trees,
            refs,
            new CSharpCompilationOptions(
                OutputKind.DynamicallyLinkedLibrary,
                allowUnsafe: proj.AllowUnsafe,
                nullableContextOptions: proj.Nullable ? NullableContextOptions.Enable : NullableContextOptions.Disable));

        builtCompilations[proj.CsProjPath] = comp;
        foreach (var srcFile in proj.SourceFiles) {
            compilationByFile[srcFile] = comp;
        }
        visiting.Remove(proj.CsProjPath);
        return comp;
    }

    foreach (var proj in projectsByPath.Values) {
        BuildProjectCompilation(proj);
    }

    // Compilación fallback para archivos .cs sueltos sin .csproj (con ImplicitUsings activado y referencias a todos los proyectos)
    if (looseFiles.Count > 0) {
        var fallbackDefines = new HashSet<string>(baseDefines, StringComparer.Ordinal) { "TRACE", "DEBUG", "NET", "NET8_0_OR_GREATER" };
        var fallbackOpts = CSharpParseOptions.Default.WithLanguageVersion(LanguageVersion.Preview).WithPreprocessorSymbols(fallbackDefines);
        var fallbackTrees = new ConcurrentBag<SyntaxTree>();
        fallbackTrees.Add(CSharpSyntaxTree.ParseText(
            "global using System;\nglobal using System.Collections.Generic;\nglobal using System.IO;\nglobal using System.Linq;\nglobal using System.Threading;\nglobal using System.Threading.Tasks;\n",
            fallbackOpts, "__ImplicitUsings.g.cs", Encoding.UTF8));

        Parallel.ForEach(looseFiles, parallel, lf => {
            if (!textByPath.TryGetValue(lf, out var txt)) return;
            var tr = CSharpSyntaxTree.ParseText(txt, fallbackOpts, lf, Encoding.UTF8);
            fallbackTrees.Add(tr);
            treeByFile[lf] = tr;
            projNameByFile[lf] = "<directory_compilation>";
        });

        var fallbackRefs = new List<MetadataReference>(runtimeReferences);
        foreach (var bc in builtCompilations.Values) {
            try { fallbackRefs.Add(bc.ToMetadataReference()); } catch { }
        }
        var fallbackComp = CSharpCompilation.Create(
            "MigratoolDirectoryCompilation",
            fallbackTrees,
            fallbackRefs,
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, allowUnsafe: true));
        foreach (var lf in looseFiles) compilationByFile[lf] = fallbackComp;
    }

    static int ByteOffset(string text, int bom, int charIndex) {
        if (charIndex <= 0) return bom;
        if (charIndex >= text.Length) charIndex = text.Length;
        return bom + Encoding.UTF8.GetByteCount(text.AsSpan(0, charIndex));
    }

    static string CanonicalSymbolKey(ISymbol? sym) {
        if (sym is null) return "";
        var canon = (sym as IMethodSymbol)?.ReducedFrom ?? sym.OriginalDefinition;
        return canon.GetDocumentationCommentId() ?? canon.ToDisplayString();
    }

    static string EnclosingQualifiedName(SyntaxNode node) {
        var parts = new List<string>();
        foreach (var anc in node.AncestorsAndSelf().Reverse()) {
            switch (anc) {
                case BaseNamespaceDeclarationSyntax ns: parts.Add(ns.Name.ToString()); break;
                case BaseTypeDeclarationSyntax t: parts.Add(t.Identifier.ValueText); break;
                case MethodDeclarationSyntax m: parts.Add(m.Identifier.ValueText); break;
                case ConstructorDeclarationSyntax c: parts.Add(c.Identifier.ValueText); break;
                case PropertyDeclarationSyntax p: parts.Add(p.Identifier.ValueText); break;
                case LocalFunctionStatementSyntax lf: parts.Add(lf.Identifier.ValueText); break;
            }
        }
        return string.Join(".", parts);
    }

    static SyntaxNode? FindEnclosingDeclaration(SyntaxNode node) {
        foreach (var anc in node.Ancestors()) {
            if (anc is MethodDeclarationSyntax or ConstructorDeclarationSyntax or DestructorDeclarationSyntax
                or PropertyDeclarationSyntax or IndexerDeclarationSyntax or OperatorDeclarationSyntax
                or ConversionOperatorDeclarationSyntax or LocalFunctionStatementSyntax or BaseTypeDeclarationSyntax)
                return anc;
        }
        return null;
    }

    static SyntaxToken? ExtractIdentifierToken(SyntaxNode expr) => expr switch {
        MemberAccessExpressionSyntax ma => ma.Name.Identifier,
        MemberBindingExpressionSyntax mb => mb.Name.Identifier,
        IdentifierNameSyntax id => id.Identifier,
        GenericNameSyntax gn => gn.Identifier,
        ObjectCreationExpressionSyntax oc => ExtractIdentifierToken(oc.Type),
        QualifiedNameSyntax qn => qn.Right.Identifier,
        AliasQualifiedNameSyntax aqn => aqn.Name.Identifier,
        _ => null
    };

    // H-10: Analizar archivos solicitados en paralelo usando ConcurrentDictionary
    var analyzedFiles = new ConcurrentDictionary<string, JsonObject>(StringComparer.OrdinalIgnoreCase);

    Parallel.ForEach(requestedPaths, parallel, path => {
        if (!treeByFile.TryGetValue(path, out var tree) ||
            !compilationByFile.TryGetValue(path, out var compilation) ||
            !textByPath.TryGetValue(path, out var text)) return;

        var bom = bomByPath.GetValueOrDefault(path, 0);
        var root = tree.GetRoot();
        var model = compilation.GetSemanticModel(tree, ignoreAccessibility: true);
        var symbols = new JsonArray();
        var dependencies = new JsonArray();
        var recordedReferenceSpans = new HashSet<int>();

        foreach (var node in root.DescendantNodes()) {
            string? name = node switch {
                BaseTypeDeclarationSyntax n => n.Identifier.ValueText,
                DelegateDeclarationSyntax n => n.Identifier.ValueText,
                MethodDeclarationSyntax n => n.Identifier.ValueText,
                ConstructorDeclarationSyntax n => n.Identifier.ValueText,
                DestructorDeclarationSyntax n => "~" + n.Identifier.ValueText,
                PropertyDeclarationSyntax n => n.Identifier.ValueText,
                EventDeclarationSyntax n => n.Identifier.ValueText,
                IndexerDeclarationSyntax => "this[]",
                OperatorDeclarationSyntax n => "operator " + n.OperatorToken.ValueText,
                ConversionOperatorDeclarationSyntax n => "operator " + n.Type.ToString(),
                EnumMemberDeclarationSyntax n => n.Identifier.ValueText,
                LocalFunctionStatementSyntax n => n.Identifier.ValueText,
                VariableDeclaratorSyntax n when n.Parent?.Parent is BaseFieldDeclarationSyntax => n.Identifier.ValueText,
                _ => null
            };
            if (name != null) {
                var containers = node.Ancestors().Reverse().Select(n => n switch {
                    BaseNamespaceDeclarationSyntax ns => ns.Name.ToString(),
                    BaseTypeDeclarationSyntax t => t.Identifier.ValueText,
                    MethodDeclarationSyntax m => m.Identifier.ValueText,
                    _ => null
                }).Where(n => n != null);
                var signature = node switch {
                    MethodDeclarationSyntax m => m.TypeParameterList?.ToString() + m.ParameterList.ToString(),
                    BaseMethodDeclarationSyntax m => m.ParameterList.ToString(),
                    LocalFunctionStatementSyntax m => m.ParameterList.ToString(),
                    IndexerDeclarationSyntax i => i.ParameterList.ToString(),
                    _ => ""
                };
                var pos = node.GetLocation().GetLineSpan();
                var span = node.Span;
                var fullSpan = node.FullSpan;
                SyntaxToken? declIdent = node switch {
                    BaseTypeDeclarationSyntax n => n.Identifier,
                    DelegateDeclarationSyntax n => n.Identifier,
                    MethodDeclarationSyntax n => n.Identifier,
                    ConstructorDeclarationSyntax n => n.Identifier,
                    DestructorDeclarationSyntax n => n.Identifier,
                    PropertyDeclarationSyntax n => n.Identifier,
                    EventDeclarationSyntax n => n.Identifier,
                    EnumMemberDeclarationSyntax n => n.Identifier,
                    LocalFunctionStatementSyntax n => n.Identifier,
                    VariableDeclaratorSyntax n => n.Identifier,
                    _ => null
                };
                var declaredSym = model.GetDeclaredSymbol(node);
                var semanticKey = CanonicalSymbolKey(declaredSym);
                var paramTypes = new JsonArray();
                string returnType = "";
                var baseTypes = new JsonArray();
                var implementsArr = new JsonArray();
                string overridesKey = "";
                string overridesName = "";
                bool isVirtual = false, isOverride = false, isAbstract = false, isStatic = false;

                if (declaredSym != null) {
                    isVirtual = declaredSym.IsVirtual;
                    isOverride = declaredSym.IsOverride;
                    isAbstract = declaredSym.IsAbstract;
                    isStatic = declaredSym.IsStatic;
                    if (declaredSym is IMethodSymbol msym) {
                        returnType = msym.ReturnType.ToDisplayString(SymbolDisplayFormat.MinimallyQualifiedFormat);
                        foreach (var p in msym.Parameters)
                            paramTypes.Add(p.Type.ToDisplayString(SymbolDisplayFormat.MinimallyQualifiedFormat));
                        if (msym.OverriddenMethod != null) {
                            overridesKey = CanonicalSymbolKey(msym.OverriddenMethod);
                            overridesName = msym.OverriddenMethod.ContainingType.ToDisplayString() + "." + msym.OverriddenMethod.Name;
                        }
                        if (msym.ContainingType != null) {
                            foreach (var iface in msym.ContainingType.AllInterfaces) {
                                foreach (var imem in iface.GetMembers(msym.Name).OfType<IMethodSymbol>()) {
                                    if (SymbolEqualityComparer.Default.Equals(msym.ContainingType.FindImplementationForInterfaceMember(imem), msym)) {
                                        implementsArr.Add(new JsonObject {
                                            ["semantic_key"] = CanonicalSymbolKey(imem),
                                            ["qualified_name"] = iface.ToDisplayString() + "." + imem.Name
                                        });
                                    }
                                }
                            }
                        }
                    } else if (declaredSym is IPropertySymbol psym) {
                        returnType = psym.Type.ToDisplayString(SymbolDisplayFormat.MinimallyQualifiedFormat);
                        if (psym.OverriddenProperty != null) {
                            overridesKey = CanonicalSymbolKey(psym.OverriddenProperty);
                            overridesName = psym.OverriddenProperty.ContainingType.ToDisplayString() + "." + psym.OverriddenProperty.Name;
                        }
                    } else if (declaredSym is IFieldSymbol fsym) {
                        returnType = fsym.Type.ToDisplayString(SymbolDisplayFormat.MinimallyQualifiedFormat);
                    } else if (declaredSym is INamedTypeSymbol tsym) {
                        if (tsym.BaseType != null && tsym.BaseType.SpecialType != SpecialType.System_Object) {
                            baseTypes.Add(new JsonObject {
                                ["name"] = tsym.BaseType.Name,
                                ["qualified_name"] = tsym.BaseType.ToDisplayString(),
                                ["semantic_key"] = CanonicalSymbolKey(tsym.BaseType),
                                ["kind"] = "base_class"
                            });
                        }
                        foreach (var iface in tsym.Interfaces) {
                            baseTypes.Add(new JsonObject {
                                ["name"] = iface.Name,
                                ["qualified_name"] = iface.ToDisplayString(),
                                ["semantic_key"] = CanonicalSymbolKey(iface),
                                ["kind"] = "interface"
                            });
                        }
                    }
                }

                // Evitar O(N^2) en tipos contenedores: solo recolectar ramas ejecutables en métodos/propiedades/constructores
                var branchesArr = new JsonArray();
                if (node is BaseMethodDeclarationSyntax or LocalFunctionStatementSyntax or PropertyDeclarationSyntax or IndexerDeclarationSyntax) {
                    foreach (var bn in node.DescendantNodes()) {
                        if (bn is IfStatementSyntax or SwitchStatementSyntax or SwitchExpressionArmSyntax
                            or ForStatementSyntax or ForEachStatementSyntax or WhileStatementSyntax or DoStatementSyntax
                            or ConditionalExpressionSyntax or CatchClauseSyntax
                            || bn.IsKind(SyntaxKind.LogicalAndExpression) || bn.IsKind(SyntaxKind.LogicalOrExpression)) {
                            branchesArr.Add(new JsonObject {
                                ["line"] = bn.GetLocation().GetLineSpan().StartLinePosition.Line + 1,
                                ["kind"] = bn.Kind().ToString()
                            });
                        }
                    }
                }

                var symObj = new JsonObject {
                    ["name"] = name,
                    ["qualified_name"] = string.Join(".", containers.Append(name)),
                    ["signature"] = signature,
                    ["semantic_key"] = semanticKey,
                    ["return_type"] = returnType,
                    ["parameter_types"] = paramTypes,
                    ["kind"] = node.Kind().ToString(),
                    ["line"] = pos.StartLinePosition.Line + 1,
                    ["column"] = pos.StartLinePosition.Character + 1,
                    ["end_line"] = pos.EndLinePosition.Line + 1,
                    ["end_column"] = pos.EndLinePosition.Character + 1,
                    ["offset"] = ByteOffset(text, bom, span.Start),
                    ["end_offset"] = ByteOffset(text, bom, span.End),
                    ["full_offset"] = ByteOffset(text, bom, fullSpan.Start),
                    ["full_end_offset"] = ByteOffset(text, bom, fullSpan.End),
                    ["is_virtual"] = isVirtual,
                    ["is_override"] = isOverride,
                    ["is_abstract"] = isAbstract,
                    ["is_static"] = isStatic,
                    ["overrides_key"] = overridesKey,
                    ["overrides_name"] = overridesName,
                    ["base_types"] = baseTypes,
                    ["implements"] = implementsArr,
                    ["branches"] = branchesArr,
                    ["semantic_resolution"] = true,
                    ["compilation_project"] = projNameByFile.GetValueOrDefault(path, "")
                };
                if (declIdent.HasValue && declIdent.Value.Span.Length > 0) {
                    var idPos = declIdent.Value.GetLocation().GetLineSpan();
                    symObj["name_line"] = idPos.StartLinePosition.Line + 1;
                    symObj["name_column"] = idPos.StartLinePosition.Character + 1;
                    symObj["name_offset"] = ByteOffset(text, bom, declIdent.Value.Span.Start);
                    symObj["name_end_offset"] = ByteOffset(text, bom, declIdent.Value.Span.End);
                    recordedReferenceSpans.Add(declIdent.Value.Span.Start);
                }
                symbols.Add(symObj);
            }

            if (node is UsingDirectiveSyntax use && use.Name != null) {
                var pos = use.GetLocation().GetLineSpan();
                var info = model.GetSymbolInfo(use.Name);
                bool resolved = info.Symbol != null && info.Symbol.Kind != SymbolKind.ErrorType;
                dependencies.Add(new JsonObject {
                    ["kind"] = "using",
                    ["name"] = use.Name.ToString(),
                    ["line"] = pos.StartLinePosition.Line + 1,
                    ["column"] = pos.StartLinePosition.Character + 1,
                    ["offset"] = ByteOffset(text, bom, use.Span.Start),
                    ["end_offset"] = ByteOffset(text, bom, use.Span.End),
                    ["resolved"] = resolved,
                    ["resolution"] = resolved ? "SEMANTIC_EXACT" : "UNRESOLVED_EXTERNAL"
                });
            }

            if (node is BaseTypeSyntax baseType) {
                var pos = baseType.GetLocation().GetLineSpan();
                var info = model.GetSymbolInfo(baseType.Type);
                var tsym = info.Symbol as ITypeSymbol;
                bool resolved = tsym != null && tsym.Kind != SymbolKind.ErrorType;
                var encl = FindEnclosingDeclaration(baseType);
                var enclSym = encl != null ? model.GetDeclaredSymbol(encl) : null;
                var tok = ExtractIdentifierToken(baseType.Type);
                if (tok.HasValue) recordedReferenceSpans.Add(tok.Value.Span.Start);
                var dep = new JsonObject {
                    ["kind"] = "base",
                    ["name"] = baseType.Type.ToString(),
                    ["line"] = pos.StartLinePosition.Line + 1,
                    ["column"] = pos.StartLinePosition.Character + 1,
                    ["offset"] = ByteOffset(text, bom, baseType.Span.Start),
                    ["end_offset"] = ByteOffset(text, bom, baseType.Span.End),
                    ["caller_key"] = CanonicalSymbolKey(enclSym),
                    ["caller_qualified_name"] = encl != null ? EnclosingQualifiedName(encl) : "",
                    ["target_key"] = CanonicalSymbolKey(tsym),
                    ["target_qualified_name"] = resolved ? tsym!.ToDisplayString() : baseType.Type.ToString(),
                    ["target_type_kind"] = resolved ? tsym!.TypeKind.ToString() : "",
                    ["resolved"] = resolved,
                    ["resolution"] = resolved ? "SEMANTIC_EXACT" : (!info.CandidateSymbols.IsEmpty ? "CANDIDATES_AMBIGUOUS" : "UNRESOLVED_EXTERNAL")
                };
                if (tok.HasValue) {
                    dep["identifier_name"] = tok.Value.ValueText;
                    dep["identifier_offset"] = ByteOffset(text, bom, tok.Value.Span.Start);
                    dep["identifier_end_offset"] = ByteOffset(text, bom, tok.Value.Span.End);
                }
                dependencies.Add(dep);
            }

            if (node is InvocationExpressionSyntax invoke) {
                var pos = invoke.GetLocation().GetLineSpan();
                var encl = FindEnclosingDeclaration(invoke);
                var enclSym = encl != null ? model.GetDeclaredSymbol(encl) : null;
                var info = model.GetSymbolInfo(invoke);
                var exprText = invoke.Expression.ToString();
                var tok = ExtractIdentifierToken(invoke.Expression);
                if (tok.HasValue) recordedReferenceSpans.Add(tok.Value.Span.Start);

                bool isDynamicOrReflection = false;
                if (invoke.Expression is MemberAccessExpressionSyntax ma) {
                    var recvType = model.GetTypeInfo(ma.Expression).Type;
                    if (recvType?.TypeKind == TypeKind.Dynamic) isDynamicOrReflection = true;
                }
                foreach (var arg in invoke.ArgumentList.Arguments) {
                    if (model.GetTypeInfo(arg.Expression).Type?.TypeKind == TypeKind.Dynamic)
                        isDynamicOrReflection = true;
                }
                if (exprText.Contains("GetMethod") || exprText.Contains("Activator.CreateInstance") || exprText.Contains("InvokeMember"))
                    isDynamicOrReflection = true;

                string resolution = "UNRESOLVED_EXTERNAL";
                string dispatch = "direct";
                bool resolved = false;
                string targetKey = "", targetName = exprText, targetQualified = exprText, targetSig = "";
                var candidatesArr = new JsonArray();

                if (isDynamicOrReflection) {
                    resolution = "DYNAMIC_OR_REFLECTION";
                } else if (info.Symbol is IMethodSymbol msym && msym.Kind != SymbolKind.ErrorType) {
                    var canon = msym.ReducedFrom ?? msym.OriginalDefinition;
                    resolved = true;
                    resolution = "SEMANTIC_EXACT";
                    targetKey = CanonicalSymbolKey(canon);
                    targetName = canon.Name;
                    targetQualified = canon.ContainingType != null ? canon.ContainingType.ToDisplayString() + "." + canon.Name : canon.Name;
                    targetSig = "(" + string.Join(", ", canon.Parameters.Select(p => p.Type.ToDisplayString(SymbolDisplayFormat.MinimallyQualifiedFormat))) + ")";
                    if (canon.IsVirtual || canon.IsAbstract || canon.IsOverride || canon.ContainingType?.TypeKind == TypeKind.Interface)
                        dispatch = "virtual_or_interface";
                } else if (!info.CandidateSymbols.IsEmpty) {
                    resolution = "CANDIDATES_AMBIGUOUS";
                    foreach (var cand in info.CandidateSymbols) {
                        candidatesArr.Add(new JsonObject {
                            ["semantic_key"] = CanonicalSymbolKey(cand),
                            ["qualified_name"] = cand.ToDisplayString()
                        });
                    }
                }

                var dep = new JsonObject {
                    ["kind"] = "call",
                    ["name"] = exprText,
                    ["target_name"] = targetName,
                    ["target_qualified_name"] = targetQualified,
                    ["target_signature"] = targetSig,
                    ["target_key"] = targetKey,
                    ["caller_key"] = CanonicalSymbolKey(enclSym),
                    ["caller_qualified_name"] = encl != null ? EnclosingQualifiedName(encl) : "",
                    ["line"] = pos.StartLinePosition.Line + 1,
                    ["column"] = pos.StartLinePosition.Character + 1,
                    ["offset"] = ByteOffset(text, bom, invoke.Span.Start),
                    ["end_offset"] = ByteOffset(text, bom, invoke.Span.End),
                    ["resolved"] = resolved,
                    ["resolution"] = resolution,
                    ["dispatch"] = dispatch,
                    ["candidates"] = candidatesArr
                };
                if (tok.HasValue) {
                    var tpos = tok.Value.GetLocation().GetLineSpan();
                    dep["identifier_name"] = tok.Value.ValueText;
                    dep["identifier_line"] = tpos.StartLinePosition.Line + 1;
                    dep["identifier_column"] = tpos.StartLinePosition.Character + 1;
                    dep["identifier_offset"] = ByteOffset(text, bom, tok.Value.Span.Start);
                    dep["identifier_end_offset"] = ByteOffset(text, bom, tok.Value.Span.End);
                }
                dependencies.Add(dep);
            }
        }

        // Segunda pasada para referencias de símbolos en fuente (campos, propiedades, tipos, eventos, grupos de métodos)
        foreach (var idNode in root.DescendantNodes().OfType<SimpleNameSyntax>()) {
            var tok = idNode.Identifier;
            if (recordedReferenceSpans.Contains(tok.Span.Start)) continue;
            if (idNode.Ancestors().Any(a => a is UsingDirectiveSyntax)) continue;
            var info = model.GetSymbolInfo(idNode);
            var sym = info.Symbol;
            if (sym != null && sym.Kind != SymbolKind.ErrorType && sym.Kind != SymbolKind.Namespace &&
                sym.Kind != SymbolKind.Local && sym.Kind != SymbolKind.Parameter && sym.Kind != SymbolKind.TypeParameter) {
                if (sym.Locations.Any(loc => loc.IsInSource)) {
                    recordedReferenceSpans.Add(tok.Span.Start);
                    var encl = FindEnclosingDeclaration(idNode);
                    var enclSym = encl != null ? model.GetDeclaredSymbol(encl) : null;
                    var tpos = tok.GetLocation().GetLineSpan();
                    var canon = (sym as IMethodSymbol)?.ReducedFrom ?? sym.OriginalDefinition;
                    dependencies.Add(new JsonObject {
                        ["kind"] = "reference",
                        ["name"] = canon.Name,
                        ["target_name"] = canon.Name,
                        ["target_qualified_name"] = canon.ContainingType != null ? canon.ContainingType.ToDisplayString() + "." + canon.Name : canon.ToDisplayString(),
                        ["target_key"] = CanonicalSymbolKey(canon),
                        ["caller_key"] = CanonicalSymbolKey(enclSym),
                        ["caller_qualified_name"] = encl != null ? EnclosingQualifiedName(encl) : "",
                        ["line"] = tpos.StartLinePosition.Line + 1,
                        ["column"] = tpos.StartLinePosition.Character + 1,
                        ["offset"] = ByteOffset(text, bom, tok.Span.Start),
                        ["end_offset"] = ByteOffset(text, bom, tok.Span.End),
                        ["identifier_name"] = tok.ValueText,
                        ["identifier_line"] = tpos.StartLinePosition.Line + 1,
                        ["identifier_column"] = tpos.StartLinePosition.Character + 1,
                        ["identifier_offset"] = ByteOffset(text, bom, tok.Span.Start),
                        ["identifier_end_offset"] = ByteOffset(text, bom, tok.Span.End),
                        ["resolved"] = true,
                        ["resolution"] = "SEMANTIC_EXACT",
                        ["dispatch"] = (canon.IsVirtual || canon.IsAbstract || canon.IsOverride) ? "virtual_or_interface" : "direct"
                    });
                }
            }
        }

        var diagnostics = tree.GetDiagnostics().Select(d => d.ToString()).ToArray();
        var semanticErrors = model.GetDiagnostics().Where(d => d.Severity == DiagnosticSeverity.Error).Select(d => d.ToString()).ToArray();
        analyzedFiles[path] = new JsonObject {
            ["protocol"] = "migratool.analyzer/1",
            ["compilation_project"] = projNameByFile.GetValueOrDefault(path, ""),
            ["symbols"] = symbols,
            ["dependencies"] = dependencies,
            ["diagnostics"] = JsonSerializer.SerializeToNode(diagnostics),
            ["semantic_diagnostics"] = JsonSerializer.SerializeToNode(semanticErrors),
            ["errors"] = tree.GetDiagnostics().Count(d => d.Severity == DiagnosticSeverity.Error)
        };
    });

    var filesObj = new JsonObject();
    foreach (var p in requestedPaths) {
        if (analyzedFiles.TryGetValue(p, out var fnode)) filesObj[p] = fnode;
    }

    File.WriteAllText(args[1], new JsonObject {
        ["files"] = filesObj,
        ["version"] = typeof(CSharpSyntaxTree).Assembly.GetName().Version!.ToString(),
        ["semantic_model"] = "CSharpCompilation",
        ["compilation_mode"] = projectsByPath.Count > 0 ? "csproj_solution_graph" : "directory_compilation_fallback",
        ["projects_loaded"] = projectsByPath.Count,
        ["solutions_loaded"] = slnPaths.Count
    }.ToJsonString());
    return 0;
} catch (Exception ex) { Console.Error.WriteLine(ex.ToString()); return 1; }

sealed class CsProjInfo(string csProjPath, string projectDir) {
    public string CsProjPath { get; } = csProjPath;
    public string ProjectDir { get; } = projectDir;
    public HashSet<string> Defines { get; } = new(StringComparer.Ordinal);
    public List<string> TargetFrameworks { get; } = [];
    public bool ImplicitUsings { get; set; } = false;
    public bool EnableDefaultCompileItems { get; set; } = true;
    public bool AllowUnsafe { get; set; } = true;
    public bool Nullable { get; set; } = false;
    public List<string> GlobalUsings { get; } = [];
    public List<string> ProjectReferences { get; } = [];
    public List<string> CompileIncludes { get; } = [];
    public List<string> CompileRemoves { get; } = [];
    public List<string> ConfigFiles { get; } = [];
    public HashSet<string> ExternalDlls { get; } = new(StringComparer.OrdinalIgnoreCase);
    public List<string> SourceFiles { get; } = [];
}
