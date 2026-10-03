#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#ifdef _WIN32
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <variant>
#include <vector>

namespace fs = std::filesystem;

namespace {
using Value = std::variant<std::string, std::uint64_t, bool, std::vector<std::string>>;

enum class BuildKind { Executable, Library };

struct Config {
  struct Dependency {
    std::string Name;
    std::string Repository;
    std::string Revision;
    fs::path Path;
    fs::path Library;
    bool Local = false;
  };

  bool HasProject = false;
  BuildKind Kind = BuildKind::Executable;
  std::string Name;
  std::string Version;
  fs::path Entry;
  std::string Compiler = "kelyra";
  fs::path Output;
  fs::path PackageOutput;
  unsigned Optimization = 0;
  unsigned SafeLevel = 0;
  std::string Runtime = "host";
  std::string Target;
  std::vector<std::string> CSources;
  std::vector<std::string> CLibraries;
  std::vector<std::string> WindowsImportLibraries;
  std::vector<std::string> CArguments;
  std::vector<std::string> ModulePaths;
  std::vector<std::string> ExternalPaths;
  std::vector<std::string> ModuleInterfaces;
  std::vector<std::string> LinkInputs;
  std::vector<std::string> TestSources;
  std::vector<fs::path> WorkspaceMembers;
  // Git dependency cache shared by a workspace; empty for a standalone project.
  fs::path CacheRoot;
  // Absolute directory that holds this project's artifacts.
  fs::path BuildRoot;
  std::vector<Dependency> Dependencies;
};

bool ValidName(std::string_view Name);

bool SafeRelativePath(const fs::path &Path) {
  if (Path.empty() || Path.is_absolute())
    return false;
  for (const auto &Part : Path)
    if (Part == "..")
      return false;
  return true;
}

bool ValidWindowsImportLibrary(std::string_view Name) {
  if (Name.size() <= 4 || Name.substr(Name.size() - 4) != ".lib")
    return false;
  for (const unsigned char Character : Name)
    if (!std::isalnum(Character) && Character != '_' && Character != '-' && Character != '.')
      return false;
  return true;
}

bool TargetsWindows(std::string_view Target) {
  if (!Target.empty())
    return Target.find("windows") != std::string_view::npos ||
           Target.find("win32") != std::string_view::npos;
#ifdef _WIN32
  return true;
#else
  return false;
#endif
}

std::string Trim(std::string_view Text) {
  while (!Text.empty() && std::isspace(static_cast<unsigned char>(Text.front())))
    Text.remove_prefix(1);
  while (!Text.empty() && std::isspace(static_cast<unsigned char>(Text.back())))
    Text.remove_suffix(1);
  return std::string(Text);
}

std::string StripComment(std::string_view Line) {
  bool Quoted = false;
  bool Escaped = false;
  for (std::size_t I = 0; I < Line.size(); ++I) {
    if (Escaped) {
      Escaped = false;
      continue;
    }
    if (Quoted && Line[I] == '\\') {
      Escaped = true;
      continue;
    }
    if (Line[I] == '"')
      Quoted = !Quoted;
    else if (Line[I] == '#' && !Quoted)
      return std::string(Line.substr(0, I));
  }
  return std::string(Line);
}

std::string ParseString(std::string_view Text) {
  Text = std::string_view(Text.data(), Text.size());
  if (Text.size() < 2 || Text.front() != '"' || Text.back() != '"')
    throw std::runtime_error("expected a quoted string");
  std::string Result;
  for (std::size_t I = 1; I + 1 < Text.size(); ++I) {
    if (Text[I] != '\\') {
      Result.push_back(Text[I]);
      continue;
    }
    if (++I + 1 >= Text.size())
      throw std::runtime_error("unterminated string escape");
    const char Escaped = Text[I];
    if (Escaped == 'n')
      Result.push_back('\n');
    else if (Escaped == 'r')
      Result.push_back('\r');
    else if (Escaped == 't')
      Result.push_back('\t');
    else if (Escaped == '\\' || Escaped == '"')
      Result.push_back(Escaped);
    else
      throw std::runtime_error("unsupported string escape");
  }
  return Result;
}

std::vector<std::string> ParseArray(std::string_view Text) {
  if (Text.size() < 2 || Text.front() != '[' || Text.back() != ']')
    throw std::runtime_error("expected an array");
  Text.remove_prefix(1);
  Text.remove_suffix(1);
  std::vector<std::string> Result;
  while (!Trim(Text).empty()) {
    Text = std::string_view(Text.data(), Text.size());
    while (!Text.empty() && std::isspace(static_cast<unsigned char>(Text.front())))
      Text.remove_prefix(1);
    if (Text.empty() || Text.front() != '"')
      throw std::runtime_error("arrays must contain strings");
    bool Escaped = false;
    std::size_t End = 1;
    for (; End < Text.size(); ++End) {
      if (!Escaped && Text[End] == '"')
        break;
      if (!Escaped && Text[End] == '\\')
        Escaped = true;
      else
        Escaped = false;
    }
    if (End == Text.size())
      throw std::runtime_error("unterminated array string");
    Result.push_back(ParseString(Text.substr(0, End + 1)));
    Text.remove_prefix(End + 1);
    while (!Text.empty() && std::isspace(static_cast<unsigned char>(Text.front())))
      Text.remove_prefix(1);
    if (Text.empty())
      break;
    if (Text.front() != ',')
      throw std::runtime_error("expected ',' in array");
    Text.remove_prefix(1);
  }
  return Result;
}

Value ParseValue(std::string_view Text) {
  const auto Clean = Trim(Text);
  if (Clean.empty())
    throw std::runtime_error("missing value");
  if (Clean.front() == '"')
    return ParseString(Clean);
  if (Clean.front() == '[')
    return ParseArray(Clean);
  if (Clean == "true")
    return true;
  if (Clean == "false")
    return false;
  std::uint64_t Number = 0;
  const auto Parsed = std::from_chars(Clean.data(), Clean.data() + Clean.size(), Number);
  if (Parsed.ec != std::errc() || Parsed.ptr != Clean.data() + Clean.size())
    throw std::runtime_error("unsupported value");
  return Number;
}

std::map<std::string, Value> ParseToml(const fs::path &Path) {
  std::ifstream Input(Path);
  if (!Input)
    throw std::runtime_error("cannot open " + Path.string());
  std::map<std::string, Value> Values;
  std::string Section;
  std::string Line;
  unsigned LineNumber = 0;
  while (std::getline(Input, Line)) {
    ++LineNumber;
    const auto Clean = Trim(StripComment(Line));
    if (Clean.empty())
      continue;
    try {
      if (Clean.front() == '[') {
        if (Clean.size() < 3 || Clean.back() != ']')
          throw std::runtime_error("invalid table header");
        Section = Trim(std::string_view(Clean).substr(1, Clean.size() - 2));
        if (Section.empty())
          throw std::runtime_error("empty table name");
        continue;
      }
      const auto Equal = Clean.find('=');
      if (Equal == std::string::npos)
        throw std::runtime_error("expected '='");
      const auto Key = Trim(std::string_view(Clean).substr(0, Equal));
      if (Key.empty())
        throw std::runtime_error("empty key");
      const auto FullKey = Section.empty() ? Key : Section + "." + Key;
      if (!Values.emplace(FullKey, ParseValue(std::string_view(Clean).substr(Equal + 1))).second)
        throw std::runtime_error("duplicate key '" + FullKey + "'");
    } catch (const std::exception &Error) {
      throw std::runtime_error(Path.string() + ":" + std::to_string(LineNumber) + ": " +
                               Error.what());
    }
  }
  return Values;
}

template <typename T>
T Get(const std::map<std::string, Value> &Values, std::string_view Key,
      std::optional<T> Default = std::nullopt) {
  const auto It = Values.find(std::string(Key));
  if (It == Values.end()) {
    if (Default)
      return *Default;
    throw std::runtime_error("missing required key '" + std::string(Key) + "'");
  }
  const auto *Result = std::get_if<T>(&It->second);
  if (!Result)
    throw std::runtime_error("wrong type for key '" + std::string(Key) + "'");
  return *Result;
}

BuildKind ParseBuildKind(const std::string &Value) {
  if (Value == "executable")
    return BuildKind::Executable;
  if (Value == "library")
    return BuildKind::Library;
  throw std::runtime_error("build.kind must be 'executable' or 'library'");
}

Config LoadConfig(const fs::path &Root) {
  const auto Values = ParseToml(Root / "kelp.toml");
  static const std::vector<std::string> Known{"project.name",
                                              "project.version",
                                              "project.entry",
                                              "build.compiler",
                                              "build.kind",
                                              "build.output",
                                              "build.optimization",
                                              "build.safe-level",
                                              "build.c-sources",
                                              "build.c-args",
                                              "build.c-libraries",
                                              "build.runtime",
                                              "build.target",
                                              "build.windows-import-libraries",
                                              "package.output",
                                              "test.sources",
                                              "workspace.members"};
  std::set<std::string> DependencyNames;
  for (const auto &[Key, Ignored] : Values) {
    (void)Ignored;
    bool Found = false;
    for (const auto &Candidate : Known)
      Found |= Key == Candidate;
    constexpr std::string_view Prefix = "dependencies.";
    if (!Found && Key.compare(0, Prefix.size(), Prefix) == 0) {
      const auto Rest = Key.substr(Prefix.size());
      const auto Dot = Rest.find('.');
      if (Dot == std::string::npos || Rest.find('.', Dot + 1) != std::string::npos)
        throw std::runtime_error("invalid dependency key '" + Key + "'");
      const auto Name = Rest.substr(0, Dot);
      const auto Field = Rest.substr(Dot + 1);
      if (!ValidName(Name) ||
          (Field != "repository" && Field != "revision" && Field != "path" && Field != "library"))
        throw std::runtime_error("invalid dependency key '" + Key + "'");
      DependencyNames.insert(Name);
      Found = true;
    }
    if (!Found)
      throw std::runtime_error("unknown key '" + Key + "'");
  }

  Config Result;
  Result.HasProject = Values.count("project.name") != 0;
  for (const auto &Member :
       Get<std::vector<std::string>>(Values, "workspace.members", std::vector<std::string>{})) {
    const fs::path MemberPath(Member);
    if (!SafeRelativePath(MemberPath))
      throw std::runtime_error("workspace.members must contain relative paths without '..'");
    Result.WorkspaceMembers.push_back(MemberPath);
  }
  if (!Result.HasProject && Result.WorkspaceMembers.empty())
    throw std::runtime_error("kelp.toml must define [project] or a non-empty [workspace] members");
  Result.Kind = ParseBuildKind(Get<std::string>(Values, "build.kind", std::string("executable")));
  const auto Optimization = Get<std::uint64_t>(Values, "build.optimization", std::uint64_t{0});
  const auto SafeLevel = Get<std::uint64_t>(Values, "build.safe-level", std::uint64_t{0});
  if (Optimization > 3)
    throw std::runtime_error("build.optimization must be between 0 and 3");
  if (SafeLevel > 1)
    throw std::runtime_error("build.safe-level must be 0 or 1");
  Result.Optimization = static_cast<unsigned>(Optimization);
  Result.SafeLevel = static_cast<unsigned>(SafeLevel);
  Result.Runtime = Get<std::string>(Values, "build.runtime", std::string("host"));
  Result.Target = Get<std::string>(Values, "build.target", std::string{});
  if (Result.Runtime != "host" && Result.Runtime != "freestanding")
    throw std::runtime_error("build.runtime must be 'host' or 'freestanding'");
  if (Result.Kind == BuildKind::Library && Result.Runtime == "freestanding")
    throw std::runtime_error("build.runtime = 'freestanding' requires an executable project");
  Result.CSources =
      Get<std::vector<std::string>>(Values, "build.c-sources", std::vector<std::string>{});
  Result.CLibraries =
      Get<std::vector<std::string>>(Values, "build.c-libraries", std::vector<std::string>{});
  Result.WindowsImportLibraries = Get<std::vector<std::string>>(
      Values, "build.windows-import-libraries", std::vector<std::string>{});
  Result.CArguments =
      Get<std::vector<std::string>>(Values, "build.c-args", std::vector<std::string>{});
  Result.TestSources =
      Get<std::vector<std::string>>(Values, "test.sources", std::vector<std::string>{});

  if (Result.HasProject) {
    Result.Name = Get<std::string>(Values, "project.name");
    Result.Version = Get<std::string>(Values, "project.version", std::string("0.1.0"));
    Result.Entry = Get<std::string>(Values, "project.entry", std::string("src/main.kly"));
    Result.Compiler = Get<std::string>(Values, "build.compiler", std::string("kelyra"));
    const auto DefaultOutput = Result.Kind == BuildKind::Library
                                   ? std::string("build/") + Result.Name + ".o"
                                   : std::string("build/") + Result.Name;
    Result.Output = Get<std::string>(Values, "build.output", DefaultOutput);
    Result.PackageOutput =
        Get<std::string>(Values,
                         "package.output",
                         std::string("build/") + Result.Name + "-" + Result.Version + ".tar.gz");
    if (!ValidName(Result.Name))
      throw std::runtime_error("project name must use letters, digits, '-' or '_'");
    if (!SafeRelativePath(Result.Entry) || Result.Entry.parent_path().empty())
      throw std::runtime_error("project.entry must be inside a source directory");
    if (!SafeRelativePath(Result.Output) || !SafeRelativePath(Result.PackageOutput))
      throw std::runtime_error("build and package outputs must be relative paths");
    for (const auto &Source : Result.CSources)
      if (!SafeRelativePath(Source))
        throw std::runtime_error("build.c-sources must contain relative paths");
    for (const auto &Library : Result.CLibraries)
      if (!SafeRelativePath(Library))
        throw std::runtime_error("build.c-libraries must contain relative paths");
    for (const auto &Library : Result.WindowsImportLibraries)
      if (!ValidWindowsImportLibrary(Library))
        throw std::runtime_error("build.windows-import-libraries must contain bare .lib names");
    for (const auto &Source : Result.TestSources)
      if (!SafeRelativePath(Source))
        throw std::runtime_error("test.sources must contain relative paths");
  }

  for (const auto &Name : DependencyNames) {
    Config::Dependency Dependency;
    Dependency.Name = Name;
    const auto Repository =
        Get<std::string>(Values, "dependencies." + Name + ".repository", std::string{});
    const auto Revision =
        Get<std::string>(Values, "dependencies." + Name + ".revision", std::string{});
    const auto Path = Get<std::string>(Values, "dependencies." + Name + ".path", std::string{});
    Dependency.Library =
        Get<std::string>(Values, "dependencies." + Name + ".library", std::string{});
    if (!Dependency.Library.empty() && Dependency.Library.native().front() == '-')
      throw std::runtime_error("invalid dependency library for '" + Name + "'");
    if (!Path.empty()) {
      if (!Repository.empty() || !Revision.empty())
        throw std::runtime_error("dependency '" + Name + "' must use either repository or path");
      if (Path.front() == '-')
        throw std::runtime_error("invalid dependency path for '" + Name + "'");
      Dependency.Local = true;
      Dependency.Path = Path;
    } else {
      if (Repository.empty() || Repository.front() == '-' ||
          (!Revision.empty() && Revision.front() == '-'))
        throw std::runtime_error("invalid dependency repository or revision for '" + Name + "'");
      Dependency.Repository = Repository;
      Dependency.Revision = Revision;
    }
    Result.Dependencies.push_back(std::move(Dependency));
  }
  return Result;
}

// Reports how long a command took, and formats it for the progress output.
class Timer {
  std::chrono::steady_clock::time_point Start = std::chrono::steady_clock::now();

public:
  double Seconds() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
  }
};

std::string FormatDuration(double Seconds) {
  char Buffer[32];
  std::snprintf(Buffer, sizeof(Buffer), "%.3fs", Seconds);
  return Buffer;
}

fs::path FindRoot() {
  auto Current = fs::current_path();
  while (true) {
    if (fs::exists(Current / "kelp.toml"))
      return Current;
    if (Current == Current.root_path())
      throw std::runtime_error("could not find kelp.toml");
    Current = Current.parent_path();
  }
}

struct ProjectNode {
  fs::path Root;
  Config Project;
};

// Returns the outermost ancestor manifest (including Root) that declares a
// workspace, so a build started inside one member still shares its cache.
fs::path FindWorkspaceRoot(const fs::path &Root) {
  fs::path Result = Root;
  for (auto Current = Root.parent_path(); !Current.empty() && Current != Current.root_path();
       Current = Current.parent_path()) {
    if (!fs::exists(Current / "kelp.toml"))
      continue;
    const auto Values = ParseToml(Current / "kelp.toml");
    if (Values.count("workspace.members"))
      Result = Current;
  }
  return Result;
}

// Every project builds into the workspace cache instead of a `build` directory
// of its own. Each project keeps a subtree that mirrors where it sits, so
// `libs/math` builds into `.kelp/build/libs/math`. A project outside the
// workspace, such as a path dependency next door, marks each step out of the
// tree with `__external`, so `../lib` becomes `.kelp/build/__external/lib`
// rather than colliding with a same-named directory inside the workspace.
fs::path BuildDirectory(const fs::path &Root, const fs::path &CacheRoot) {
  const auto Base = (CacheRoot.empty() ? Root : CacheRoot) / ".kelp/build";
  if (CacheRoot.empty())
    return Base.lexically_normal();
  std::error_code Error;
  const auto Relative = fs::relative(Root, CacheRoot, Error);
  if (Error || Relative.empty() || Relative == ".")
    return Base.lexically_normal();
  fs::path Result;
  for (const auto &Part : Relative) {
    if (Part == "..")
      Result /= "__external";
    else if (Part != ".")
      Result /= Part;
  }
  return (Base / Result).lexically_normal();
}

// A declared output is relative to the project's build directory.
fs::path BuildArtifact(const Config &Project, const fs::path &Declared) {
  auto Relative = Declared.lexically_normal();
  if (Project.Kind == BuildKind::Executable && TargetsWindows(Project.Target) &&
      Relative.extension().empty())
    Relative += ".exe";
  const auto Base = Project.BuildRoot.empty() ? fs::path(".kelp/build") : Project.BuildRoot;
  return (Base / Relative).lexically_normal();
}

fs::path ArtifactPath(const Config &Project) { return BuildArtifact(Project, Project.Output); }

fs::path InterfacePath(const fs::path &Artifact) { return fs::path(Artifact.string() + ".kmi"); }

class BuildFingerprint {
  std::uint64_t Value = 1469598103934665603ull;
  bool ContainsCImport = false;

public:
  void Add(std::string_view Text) {
    for (const unsigned char Byte : Text) {
      Value ^= Byte;
      Value *= 1099511628211ull;
    }
    Value ^= 0xff;
    Value *= 1099511628211ull;
  }

  void File(const fs::path &Path) {
    Add(fs::absolute(Path).lexically_normal().string());
    std::ifstream Input(Path, std::ios::binary);
    if (!Input)
      throw std::runtime_error("cannot fingerprint build input: " + Path.string());
    char Buffer[65536];
    std::string Previous;
    while (Input.read(Buffer, sizeof(Buffer)) || Input.gcount()) {
      const std::string_view Chunk(Buffer, static_cast<std::size_t>(Input.gcount()));
      Add(Chunk);
      if (Path.extension() == ".kly") {
        const std::string Scan = Previous + std::string(Chunk);
        ContainsCImport |= Scan.find("import c") != std::string::npos;
        Previous = Scan.substr(Scan.size() > 8 ? Scan.size() - 8 : 0);
      }
    }
    if (Input.bad())
      throw std::runtime_error("cannot fingerprint build input: " + Path.string());
  }

  void Tree(const fs::path &Root) {
    std::vector<fs::path> Files;
    std::error_code Error;
    for (fs::recursive_directory_iterator It(Root, Error), End; !Error && It != End;
         It.increment(Error))
      if (It->is_regular_file(Error))
        Files.push_back(It->path());
    if (Error)
      throw std::runtime_error("cannot fingerprint source directory: " + Root.string());
    std::sort(Files.begin(), Files.end());
    for (const auto &Path : Files)
      File(Path);
  }

  std::string Finish() const {
    std::ostringstream Output;
    Output << std::hex << std::setfill('0') << std::setw(16) << Value;
    return Output.str();
  }

  bool HasCImports() const { return ContainsCImport; }
};

fs::path PackagePath(const Config &Project) {
  return BuildArtifact(Project, Project.PackageOutput);
}

// The workspace a project belongs to, which owns the shared build directory.
const fs::path &WorkspaceOf(const Config &Project, const fs::path &Root) {
  return Project.CacheRoot.empty() ? Root : Project.CacheRoot;
}

// Shortens a path for progress output without ever escaping the project.
std::string DisplayPath(const fs::path &Root, const fs::path &Path) {
  std::error_code Error;
  const auto Relative = fs::relative(Path, Root, Error);
  if (Error || Relative.empty() ||
      (Relative.begin() != Relative.end() && *Relative.begin() == ".."))
    return Path.string();
  return Relative.string();
}

void LoadWorkspace(const fs::path &Root, std::vector<ProjectNode> &Nodes,
                   std::set<std::string> &Seen) {
  const auto Key = fs::weakly_canonical(Root).string();
  if (!Seen.insert(Key).second)
    throw std::runtime_error("duplicate workspace member: " + Root.string());
  auto Project = LoadConfig(Root);
  for (const auto &Member : Project.WorkspaceMembers) {
    const auto MemberRoot = (Root / Member).lexically_normal();
    if (!fs::exists(MemberRoot / "kelp.toml"))
      throw std::runtime_error("workspace member '" + Member.string() + "' has no kelp.toml");
    LoadWorkspace(MemberRoot, Nodes, Seen);
  }
  // Members are visited before their parent so builds run dependencies first.
  Nodes.push_back({Root, std::move(Project)});
}

std::vector<ProjectNode> LoadWorkspace(const fs::path &Root) {
  std::vector<ProjectNode> Nodes;
  std::set<std::string> Seen;
  LoadWorkspace(Root, Nodes, Seen);
  // Every member shares the outermost enclosing workspace dependency cache.
  const auto CacheRoot = FindWorkspaceRoot(Root);
  for (auto &Node : Nodes) {
    Node.Project.CacheRoot = CacheRoot;
    Node.Project.BuildRoot = BuildDirectory(Node.Root, CacheRoot);
  }
  return Nodes;
}

std::string ProjectKindName(const Config &Project) {
  if (!Project.HasProject)
    return "workspace";
  return Project.Kind == BuildKind::Library ? "library" : "executable";
}

std::string RelativeNodePath(const fs::path &Root, const fs::path &WorkspaceRoot) {
  std::error_code Error;
  const auto Relative = fs::relative(Root, WorkspaceRoot, Error);
  if (Error || Relative.empty())
    return ".";
  return Relative.generic_string();
}

bool MatchesSelector(const ProjectNode &Node, const fs::path &WorkspaceRoot,
                     const std::string &Selector) {
  if (Node.Project.HasProject && Node.Project.Name == Selector)
    return true;
  return RelativeNodePath(Node.Root, WorkspaceRoot) == Selector;
}

struct CommandOptions {
  std::optional<std::string> Selector;
  bool Workspace = false;
  bool Debug = false;
};

CommandOptions ParseCommandOptions(int Argc, char **Argv, bool AllowDebug) {
  CommandOptions Options;
  for (int I = 2; I < Argc; ++I) {
    const std::string_view Argument = Argv[I];
    if (Argument == "--workspace" || Argument == "--all" || Argument == "-w")
      Options.Workspace = true;
    else if (AllowDebug && Argument == "--debug")
      Options.Debug = true;
    else if (!Argument.empty() && Argument.front() == '-')
      throw std::runtime_error("unknown option '" + std::string(Argument) + "'");
    else if (Options.Selector)
      throw std::runtime_error("unexpected argument '" + std::string(Argument) + "'");
    else
      Options.Selector = std::string(Argument);
  }
  return Options;
}

std::vector<const ProjectNode *> SelectTargets(const std::vector<ProjectNode> &Nodes,
                                               const CommandOptions &Options, bool RequireSingle) {
  std::vector<const ProjectNode *> Targets;
  if (Options.Selector) {
    const ProjectNode *Match = nullptr;
    for (const auto &Node : Nodes)
      if (MatchesSelector(Node, Nodes.back().Root, *Options.Selector)) {
        if (Match)
          throw std::runtime_error("ambiguous workspace member '" + *Options.Selector + "'");
        Match = &Node;
      }
    if (!Match)
      throw std::runtime_error("unknown workspace member '" + *Options.Selector + "'");
    if (!Match->Project.HasProject)
      throw std::runtime_error("workspace member '" + *Options.Selector + "' is not a project");
    Targets.push_back(Match);
  } else if (Options.Workspace) {
    for (const auto &Node : Nodes)
      if (Node.Project.HasProject)
        Targets.push_back(&Node);
  } else {
    const auto &RootNode = Nodes.back();
    if (RootNode.Project.HasProject)
      Targets.push_back(&RootNode);
    else
      for (const auto &Node : Nodes)
        if (Node.Project.HasProject)
          Targets.push_back(&Node);
  }
  if (Targets.empty())
    throw std::runtime_error("no buildable project found");
  if (RequireSingle && Targets.size() != 1)
    throw std::runtime_error("select one project with a member name");
  return Targets;
}

int Execute(const fs::path &Root, const std::vector<std::string> &Arguments) {
  if (Arguments.empty())
    throw std::runtime_error("empty command");
#ifdef _WIN32
  std::vector<const char *> Argv;
  for (const auto &Argument : Arguments)
    Argv.push_back(Argument.c_str());
  Argv.push_back(nullptr);
  const fs::path PreviousDirectory = fs::current_path();
  fs::current_path(Root);
  const intptr_t Status = _spawnvp(_P_WAIT, Argv.front(), Argv.data());
  const int SpawnError = errno;
  fs::current_path(PreviousDirectory);
  if (Status < 0)
    throw std::runtime_error("cannot execute " + Arguments.front() + ": " +
                             std::strerror(SpawnError));
  return static_cast<int>(Status);
#else
  const pid_t Child = fork();
  if (Child < 0)
    throw std::runtime_error("cannot create process");
  if (Child == 0) {
    if (chdir(Root.c_str()) != 0) {
      std::cerr << "kelp: cannot enter " << Root << ": " << std::strerror(errno) << '\n';
      _exit(126);
    }
    std::vector<char *> Argv;
    for (const auto &Argument : Arguments)
      Argv.push_back(const_cast<char *>(Argument.c_str()));
    Argv.push_back(nullptr);
    execvp(Argv.front(), Argv.data());
    std::cerr << "kelp: cannot execute " << Arguments.front() << ": " << std::strerror(errno)
              << '\n';
    _exit(errno == ENOENT ? 127 : 126);
  }
  int Status = 0;
  while (waitpid(Child, &Status, 0) < 0) {
    if (errno != EINTR)
      throw std::runtime_error("cannot wait for process");
  }
  if (WIFEXITED(Status))
    return WEXITSTATUS(Status);
  return 128 + WTERMSIG(Status);
#endif
}

struct ResolvedDependency {
  fs::path Root;
  Config Project;
  fs::path PrebuiltLibrary;
};

struct DependencyUse {
  std::string Repository;
  std::string Revision;
};

// Records where each cache directory was resolved from during one Kelp run, so
// a workspace shares one cache without silently repointing another member's
// dependency.
std::map<std::string, DependencyUse> &DependencyUses() {
  static std::map<std::string, DependencyUse> Uses;
  return Uses;
}

void ResolveDependencies(const fs::path &CacheRoot, const fs::path &ProjectRoot,
                         const Config &Project, std::vector<ResolvedDependency> &Result,
                         std::set<std::string> &Resolving, std::set<std::string> &Resolved) {
  for (const auto &Dependency : Project.Dependencies) {
    fs::path DependencyRoot;
    if (Dependency.Local) {
      DependencyRoot =
          Dependency.Path.is_absolute() ? Dependency.Path : ProjectRoot / Dependency.Path;
      DependencyRoot = DependencyRoot.lexically_normal();
      if (!fs::exists(DependencyRoot / "kelp.toml"))
        throw std::runtime_error("path dependency '" + Dependency.Name +
                                 "' is not a Kelp project: " + DependencyRoot.string());
    } else {
      const auto Cache = CacheRoot / ".kelp/dependencies";
      fs::create_directories(Cache);
      DependencyRoot = Cache / Dependency.Name;
    }
    const auto Key = fs::weakly_canonical(DependencyRoot).string();
    if (Resolved.count(Key))
      continue;
    if (!Resolving.insert(Key).second)
      throw std::runtime_error("cyclic dependency involving '" + Dependency.Name + "'");
    if (!Dependency.Local) {
      auto &Uses = DependencyUses();
      const auto Use = Uses.find(Key);
      if (Use != Uses.end()) {
        if (Use->second.Repository != Dependency.Repository ||
            Use->second.Revision != Dependency.Revision)
          throw std::runtime_error("workspace dependency '" + Dependency.Name +
                                   "' is requested from conflicting sources");
      } else {
        if (!fs::exists(DependencyRoot)) {
          std::cout << "fetching " << Dependency.Name << " from " << Dependency.Repository << '\n';
          if (const int Status = Execute(ProjectRoot,
                                         {"git",
                                          "clone",
                                          "--quiet",
                                          "--",
                                          Dependency.Repository,
                                          DependencyRoot.string()}))
            throw std::runtime_error("git clone failed with status " + std::to_string(Status));
        } else if (!fs::exists(DependencyRoot / ".git")) {
          throw std::runtime_error("dependency cache is not a Git repository: " +
                                   DependencyRoot.string());
        } else if (const int Status =
                       Execute(DependencyRoot,
                               {"git", "remote", "set-url", "origin", Dependency.Repository})) {
          throw std::runtime_error("cannot update dependency remote, status " +
                                   std::to_string(Status));
        }
        if (!Dependency.Revision.empty()) {
          if (const int Status = Execute(
                  DependencyRoot, {"git", "fetch", "--quiet", "origin", Dependency.Revision}))
            throw std::runtime_error("git fetch failed with status " + std::to_string(Status));
          if (const int Status =
                  Execute(DependencyRoot, {"git", "checkout", "--quiet", "--detach", "FETCH_HEAD"}))
            throw std::runtime_error("git checkout failed with status " + std::to_string(Status));
        }
        Uses.emplace(Key, DependencyUse{Dependency.Repository, Dependency.Revision});
      }
    }
    auto DependencyProject = LoadConfig(DependencyRoot);
    if (!DependencyProject.HasProject)
      throw std::runtime_error("dependency '" + Dependency.Name +
                               "' is a workspace, not a project");
    fs::path PrebuiltLibrary;
    if (!Dependency.Library.empty()) {
      if (DependencyProject.Kind != BuildKind::Library)
        throw std::runtime_error("dependency '" + Dependency.Name +
                                 "' provides a library but is not a library project");
      PrebuiltLibrary = Dependency.Library.is_absolute() ? Dependency.Library
                                                         : DependencyRoot / Dependency.Library;
      PrebuiltLibrary = PrebuiltLibrary.lexically_normal();
      if (!fs::is_regular_file(PrebuiltLibrary))
        throw std::runtime_error("missing prebuilt library: " + PrebuiltLibrary.string());
    }
    ResolveDependencies(CacheRoot, DependencyRoot, DependencyProject, Result, Resolving, Resolved);
    Result.push_back({DependencyRoot, std::move(DependencyProject), std::move(PrebuiltLibrary)});
    Resolving.erase(Key);
    Resolved.insert(Key);
  }
}

std::vector<ResolvedDependency>
ResolveDependencies(const fs::path &CacheRoot, const fs::path &ProjectRoot, const Config &Project) {
  std::vector<ResolvedDependency> Result;
  std::set<std::string> Resolving;
  std::set<std::string> Resolved;
  ResolveDependencies(CacheRoot, ProjectRoot, Project, Result, Resolving, Resolved);
  return Result;
}

struct PreparedProject {
  Config Project;
  std::vector<ResolvedDependency> Dependencies;
};

// A compiler path with a separator is resolved against the project root, so the
// same absolute toolchain can build a dependency from a different directory. A
// bare name is left to PATH lookup.
std::string ResolveCompiler(const fs::path &Root, const std::string &Compiler) {
  if (Compiler.find('/') == std::string::npos)
    return Compiler;
  const fs::path Path(Compiler);
  if (Path.is_absolute())
    return Path.lexically_normal().string();
  return fs::absolute(Root / Path).lexically_normal().string();
}

PreparedProject Prepare(const fs::path &Root, const Config &Project) {
  PreparedProject Prepared;
  Config &Result = Prepared.Project;
  Result = Project;
  Result.Compiler = ResolveCompiler(Root, Result.Compiler);
  // Sources are compiled where they live. Instead of copying the project and
  // dependency sources into a staging tree, every source directory is passed
  // to the compiler as a module search path.
  Result.ModulePaths.push_back(
      fs::absolute(Root / Project.Entry.parent_path()).lexically_normal().string());
  for (const auto &Library : Project.CLibraries)
    Result.LinkInputs.push_back(fs::absolute(Root / Library).lexically_normal().string());
  // Remove the staging tree created by older Kelp versions so no stale copies
  // remain next to the originals.
  std::error_code Error;
  fs::remove_all(Root / ".kelp/stage", Error);
  if (Project.Dependencies.empty()) {
    if (TargetsWindows(Result.Target))
      Result.LinkInputs.insert(Result.LinkInputs.end(),
                               Project.WindowsImportLibraries.begin(),
                               Project.WindowsImportLibraries.end());
    return Prepared;
  }
  const auto CacheRoot = Project.CacheRoot.empty() ? Root : Project.CacheRoot;
  for (const auto &Dependency : ResolveDependencies(CacheRoot, Root, Project)) {
    const auto DependencySourceRoot = Dependency.Project.Entry.parent_path();
    const auto AbsoluteSourceRoot =
        fs::absolute(Dependency.Root / DependencySourceRoot).lexically_normal();
    Result.ModulePaths.push_back(AbsoluteSourceRoot.string());
    if (Dependency.Project.Kind != BuildKind::Library)
      for (const auto &Source : Dependency.Project.CSources)
        Result.CSources.push_back(fs::absolute(Dependency.Root / Source).string());
    for (const auto &Library : Dependency.Project.CLibraries)
      Result.LinkInputs.push_back(
          fs::absolute(Dependency.Root / Library).lexically_normal().string());
    Result.WindowsImportLibraries.insert(Result.WindowsImportLibraries.end(),
                                         Dependency.Project.WindowsImportLibraries.begin(),
                                         Dependency.Project.WindowsImportLibraries.end());
    Result.CArguments.insert(Result.CArguments.end(),
                             Dependency.Project.CArguments.begin(),
                             Dependency.Project.CArguments.end());
    if (Dependency.Project.Kind == BuildKind::Library) {
      // A library dependency is compiled once into its own object: consumers
      // only declare its modules and link the object.
      auto DependencyProject = Dependency.Project;
      DependencyProject.BuildRoot = BuildDirectory(Dependency.Root, CacheRoot);
      Result.ExternalPaths.push_back(AbsoluteSourceRoot.string());
      const auto Library = Dependency.PrebuiltLibrary.empty() ? ArtifactPath(DependencyProject)
                                                              : Dependency.PrebuiltLibrary;
      Result.LinkInputs.push_back(Library.string());
      const auto Interface = InterfacePath(Library);
      if (!Dependency.PrebuiltLibrary.empty() && !fs::is_regular_file(Interface))
        throw std::runtime_error("missing prebuilt module interface: " + Interface.string());
      Result.ModuleInterfaces.push_back(Interface.string());
    }
    Prepared.Dependencies.push_back(Dependency);
  }
  if (TargetsWindows(Result.Target))
    for (const auto &Library : Result.WindowsImportLibraries)
      if (std::find(Result.LinkInputs.begin(), Result.LinkInputs.end(), Library) ==
          Result.LinkInputs.end())
        Result.LinkInputs.push_back(Library);
  return Prepared;
}

std::vector<std::string> CompilerCommand(const Config &Config, std::string Action) {
  const bool ProducesArtifact = Action == "--emit-exe" || Action == "--emit-obj";
  const bool Executable = Action == "--emit-exe";
  std::vector<std::string> Result{Config.Compiler};
  if (!Action.empty())
    Result.push_back(std::move(Action));
  for (const auto &Path : Config.ModulePaths)
    Result.push_back("--module-search-path=" + Path);
  for (const auto &Interface : Config.ModuleInterfaces)
    Result.push_back("--module-interface-path=" + Interface);
  if (!Config.Target.empty())
    Result.push_back("--target=" + Config.Target);
  for (const auto &Argument : Config.CArguments)
    Result.push_back("--clang-args=" + Argument);
  if (ProducesArtifact) {
    if (!Executable && Config.Kind == BuildKind::Library) {
      Result.push_back("--library-root=" + Config.ModulePaths.front());
      Result.push_back("--emit-interface=" + InterfacePath(ArtifactPath(Config)).string());
    }
    Result.push_back("-O" + std::to_string(Config.Optimization));
    Result.push_back("--safe-level=" + std::to_string(Config.SafeLevel));

    if (Executable)
      Result.push_back("--main-runtime=" + Config.Runtime);
    if (Executable)
      for (const auto &Source : Config.CSources)
        Result.push_back("--link=" + Source);
    if (Executable) {
      for (const auto &Input : Config.LinkInputs)
        Result.push_back("--link=" + Input);
    }

    Result.push_back("-o");
    Result.push_back(ArtifactPath(Config).string());
  }
  Result.push_back(Config.Entry.string());
  return Result;
}

std::optional<fs::path> FindCompilerFile(const std::string &Compiler) {
  const fs::path Direct(Compiler);
  if (Direct.is_absolute() || Direct.has_parent_path())
    return fs::is_regular_file(Direct) ? std::optional<fs::path>(Direct) : std::nullopt;
  const char *Path = std::getenv("PATH");
  if (!Path)
    return std::nullopt;
#ifdef _WIN32
  constexpr char Separator = ';';
#else
  constexpr char Separator = ':';
#endif
  std::istringstream Parts(Path);
  std::string Directory;
  while (std::getline(Parts, Directory, Separator)) {
    const auto Candidate = fs::path(Directory) / Compiler;
    if (fs::is_regular_file(Candidate))
      return Candidate;
#ifdef _WIN32
    const auto Executable = fs::path(Candidate.string() + ".exe");
    if (fs::is_regular_file(Executable))
      return Executable;
#endif
  }
  return std::nullopt;
}

std::optional<std::string> FingerprintProject(const fs::path &Root, const Config &Project,
                                              std::string_view Action) {
  const auto Compiler = FindCompilerFile(Project.Compiler);
  if (!Compiler)
    return std::nullopt;
  BuildFingerprint Fingerprint;
  Fingerprint.Add("kelp-build-cache-v1");
  Fingerprint.File(*Compiler);
  Fingerprint.File(Root / "kelp.toml");
  for (const auto &Argument : CompilerCommand(Project, std::string(Action)))
    Fingerprint.Add(Argument);
  for (const auto &Path : Project.ModulePaths) {
    if (fs::is_directory(Path)) {
      Fingerprint.Tree(Path);
    } else if (std::find(Project.ExternalPaths.begin(), Project.ExternalPaths.end(), Path) ==
               Project.ExternalPaths.end()) {
      throw std::runtime_error("missing module source directory: " + Path);
    }
  }
  for (const auto &Path : Project.ModuleInterfaces)
    Fingerprint.File(Path);
  for (const auto &Path : Project.CSources)
    Fingerprint.File(fs::path(Path).is_absolute() ? fs::path(Path) : Root / Path);
  for (const auto &Path : Project.LinkInputs)
    if (fs::is_regular_file(Path))
      Fingerprint.File(Path);
  // An imported header can include files outside the source tree. Until the
  // compiler emits a complete C dependency file, do not cache such builds.
  if (Fingerprint.HasCImports() || !Project.CArguments.empty())
    return std::nullopt;
  return Fingerprint.Finish();
}

int Build(const fs::path &Root, const Config &Config);

int Check(const fs::path &Root, const Config &Config) {
  const auto Prepared = Prepare(Root, Config);
  for (const auto &Dependency : Prepared.Dependencies)
    if (Dependency.Project.Kind == BuildKind::Library && Dependency.PrebuiltLibrary.empty()) {
      auto LibraryProject = Dependency.Project;
      LibraryProject.Compiler = Prepared.Project.Compiler;
      LibraryProject.Target = Prepared.Project.Target;
      LibraryProject.CacheRoot = Config.CacheRoot;
      LibraryProject.BuildRoot = BuildDirectory(Dependency.Root, Config.CacheRoot);
      if (const int Status = Build(Dependency.Root, LibraryProject))
        return Status;
    }
  Timer Timer;
  const int Status = Execute(Root, CompilerCommand(Prepared.Project, {}));
  if (Status == 0)
    std::cerr << "checked " << Config.Name << " in " << FormatDuration(Timer.Seconds()) << '\n';
  return Status;
}

int Build(const fs::path &Root, const Config &Config, std::set<std::string> &Built) {
  if (!Config.HasProject)
    throw std::runtime_error("workspace root has no buildable project");
  const auto Key = fs::weakly_canonical(Root).string();
  if (!Built.insert(Key).second)
    return 0;
  const std::string Action = Config.Kind == BuildKind::Library ? "--emit-obj" : "--emit-exe";
  std::cerr << "[1/3] Preparing " << Config.Name << '\n';
  std::error_code Error;
  fs::create_directories(ArtifactPath(Config).parent_path(), Error);
  if (Error)
    throw std::runtime_error("cannot create build directory: " + Error.message());
  const auto Prepared = Prepare(Root, Config);
  // A linked library must exist before the project that links it is compiled.
  // It builds with the consumer's toolchain and shares its dependency cache, so
  // a relative compiler path in the dependency still resolves.
  for (const auto &Dependency : Prepared.Dependencies)
    if (Dependency.Project.Kind == BuildKind::Library && Dependency.PrebuiltLibrary.empty()) {
      auto LibraryProject = Dependency.Project;
      LibraryProject.Compiler = Prepared.Project.Compiler;
      LibraryProject.Target = Prepared.Project.Target;
      LibraryProject.CacheRoot = Config.CacheRoot;
      LibraryProject.BuildRoot = BuildDirectory(Dependency.Root, Config.CacheRoot);
      if (const int Status = Build(Dependency.Root, LibraryProject, Built))
        return Status;
    }
  const auto Fingerprint = FingerprintProject(Root, Prepared.Project, Action);
  const auto Stamp = fs::path(ArtifactPath(Config).string() + ".fingerprint");
  if (Fingerprint && fs::is_regular_file(ArtifactPath(Config)) &&
      (Config.Kind != BuildKind::Library ||
       fs::is_regular_file(InterfacePath(ArtifactPath(Config))))) {
    std::ifstream Previous(Stamp);
    std::string Value;
    if (Previous >> Value && Value == *Fingerprint) {
      std::cerr << "[3/3] Reused " << DisplayPath(WorkspaceOf(Config, Root), ArtifactPath(Config))
                << '\n';
      return 0;
    }
  }
  fs::remove(Stamp, Error);
  std::cerr << "[2/3] Building " << Prepared.Project.Entry.string() << " -> "
            << DisplayPath(WorkspaceOf(Config, Root), ArtifactPath(Config)) << '\n';
  Timer Timer;
  const int Status = Execute(Root, CompilerCommand(Prepared.Project, Action));
  if (Status == 0) {
    if (Fingerprint) {
      std::ofstream Output(Stamp, std::ios::trunc);
      if (!Output || !(Output << *Fingerprint << '\n'))
        throw std::runtime_error("cannot write build fingerprint: " + Stamp.string());
    }
    std::cerr << "[3/3] Finished " << DisplayPath(WorkspaceOf(Config, Root), ArtifactPath(Config))
              << " in " << FormatDuration(Timer.Seconds()) << '\n';
  } else
    std::cerr << "Build failed (exit " << Status << ") after " << FormatDuration(Timer.Seconds())
              << '\n';
  return Status;
}

int Build(const fs::path &Root, const Config &Config) {
  std::set<std::string> Built;
  return Build(Root, Config, Built);
}

int RunProject(const ProjectNode &Node, const std::vector<std::string> &Arguments) {
  if (Node.Project.Kind == BuildKind::Library)
    throw std::runtime_error("library projects cannot be run");
  if (const int Status = Build(Node.Root, Node.Project))
    return Status;
  std::vector<std::string> Command{ArtifactPath(Node.Project).string()};
  Command.insert(Command.end(), Arguments.begin(), Arguments.end());
  return Execute(Node.Root, Command);
}

int Package(const fs::path &Root, const Config &Config, std::set<std::string> &Built) {
  if (const int Status = Build(Root, Config, Built))
    return Status;
  std::error_code Error;
  fs::create_directories(PackagePath(Config).parent_path(), Error);
  if (Error)
    throw std::runtime_error("cannot create package directory: " + Error.message());
  std::vector<std::string> Arguments{"tar",
                                     "-czf",
                                     PackagePath(Config).string(),
                                     "kelp.toml",
                                     Config.Entry.parent_path().string()};
  if (fs::exists(Root / "README.md"))
    Arguments.push_back("README.md");
  Timer Timer;
  const int Status = Execute(Root, Arguments);
  if (Status == 0)
    std::cerr << "packaged " << DisplayPath(WorkspaceOf(Config, Root), PackagePath(Config))
              << " in " << FormatDuration(Timer.Seconds()) << '\n';
  return Status;
}

bool ValidName(std::string_view Name) {
  if (Name.empty() || !std::isalpha(static_cast<unsigned char>(Name.front())))
    return false;
  for (const char Character : Name)
    if (!std::isalnum(static_cast<unsigned char>(Character)) && Character != '-' &&
        Character != '_')
      return false;
  return true;
}

void CreateProject(const fs::path &Root, std::string Name) {
  if (!ValidName(Name))
    throw std::runtime_error("project name must use letters, digits, '-' or '_'");
  if (fs::exists(Root / "kelp.toml"))
    throw std::runtime_error("kelp.toml already exists");
  if (fs::exists(Root / "src/main.kly"))
    throw std::runtime_error("src/main.kly already exists");
  fs::create_directories(Root / "src");
  std::ofstream Toml(Root / "kelp.toml");
  if (!Toml)
    throw std::runtime_error("cannot create kelp.toml");
  Toml << "[project]\n"
          "name = \""
       << Name
       << "\"\n"
          "version = \"0.1.0\"\n"
          "entry = \"src/main.kly\"\n\n"
          "[build]\n"
          "compiler = \"kelyra\"\n"
          "output = \""
       << Name
       << "\"\n"
          "optimization = 0\n"
          "safe-level = 0\n"
          "c-sources = []\n"
          "c-args = []\n\n"
          "[package]\n"
          "output = \""
       << Name
       << "-0.1.0.tar.gz\"\n\n"
          "[test]\n"
          "sources = []\n\n"
          "# [dependencies.kstd]\n"
          "# path = \"../kstd\"\n";
  std::ofstream Main(Root / "src/main.kly");
  if (!Main)
    throw std::runtime_error("cannot create src/main.kly");
  Main << "@main\npub fn main() -> i32 {\n  return 0;\n}\n";
  if (!fs::exists(Root / ".gitignore")) {
    std::ofstream Ignore(Root / ".gitignore");
    Ignore << "/.kelp/\n";
  }
  std::cout << "created " << Name << " in " << Root << '\n';
}

void Help() {
  std::cout << "Kelp - Kelyra project manager\n\n"
               "usage: kelp <command> [arguments]\n\n"
               "commands:\n"
               "  new <name>        Create a project in ./<name>\n"
               "  init [name]       Create a project in the current directory\n"
               "  members           List the workspace projects\n"
               "  check [<member>] [--workspace]   Type-check a project\n"
               "  build [<member>] [--debug] [--workspace]\n"
               "                    Build it (--debug uses -O0)\n"
               "  output [<member>] Print the absolute artifact path\n"
               "  run [<member>] [-- ...]  Build and run an executable\n"
               "  test [<member>] [--workspace]    Check test sources\n"
               "  package [<member>] [--workspace] Build and archive sources\n"
               "  help              Show this help\n\n"
               "[workspace] members in kelp.toml nest subprojects, each with\n"
               "its own kelp.toml. Dependencies use repository/revision for Git\n"
               "or path for a local project. build.kind is 'executable' or\n"
               "'library' (an object artifact).\n";
}
} // namespace

int main(int Argc, char **Argv) {
  try {
    if (Argc == 2 && std::string_view(Argv[1]) == "--version") {
      std::cout << "kelp " << KELP_VERSION << '\n';
      return 0;
    }
    if (Argc < 2 || std::string_view(Argv[1]) == "help" || std::string_view(Argv[1]) == "-h" ||
        std::string_view(Argv[1]) == "--help") {
      Help();
      return Argc < 2 ? 2 : 0;
    }
    const std::string Command = Argv[1];
    if (Command == "new") {
      if (Argc != 3)
        throw std::runtime_error("usage: kelp new <name>");
      CreateProject(fs::current_path() / Argv[2], Argv[2]);
      return 0;
    }
    if (Command == "init") {
      if (Argc > 3)
        throw std::runtime_error("usage: kelp init [name]");
      auto Name = Argc == 3 ? std::string(Argv[2]) : fs::current_path().filename().string();
      CreateProject(fs::current_path(), std::move(Name));
      return 0;
    }

    const auto Root = FindRoot();
    if (Command == "members") {
      if (Argc != 2)
        throw std::runtime_error("usage: kelp members");
      for (const auto &Node : LoadWorkspace(Root))
        std::cout << RelativeNodePath(Node.Root, Root) << ' '
                  << (Node.Project.HasProject ? Node.Project.Name : "-") << ' '
                  << ProjectKindName(Node.Project) << ' '
                  << (Node.Project.HasProject ? DisplayPath(Root, ArtifactPath(Node.Project))
                                              : std::string("-"))
                  << '\n';
      return 0;
    }

    const auto Nodes = LoadWorkspace(Root);
    if (Command == "check") {
      const auto Options = ParseCommandOptions(Argc, Argv, false);
      const auto Targets = SelectTargets(Nodes, Options, false);
      Timer Total;
      for (const auto *Node : Targets)
        if (const int Status = Check(Node->Root, Node->Project))
          return Status;
      if (Targets.size() > 1)
        std::cerr << "checked " << Targets.size() << " projects in "
                  << FormatDuration(Total.Seconds()) << '\n';
      return 0;
    }
    if (Command == "build") {
      const auto Options = ParseCommandOptions(Argc, Argv, true);
      const auto Targets = SelectTargets(Nodes, Options, false);
      std::set<std::string> Built;
      Timer Total;
      for (const auto *Node : Targets) {
        auto Project = Node->Project;
        if (Options.Debug)
          Project.Optimization = 0;
        if (const int Status = Build(Node->Root, Project, Built))
          return Status;
      }
      if (Targets.size() > 1)
        std::cerr << "built " << Targets.size() << " projects in "
                  << FormatDuration(Total.Seconds()) << '\n';
      return 0;
    }
    if (Command == "output") {
      const auto Options = ParseCommandOptions(Argc, Argv, false);
      const auto Targets = SelectTargets(Nodes, Options, true);
      std::cout << ArtifactPath(Targets.front()->Project).string() << '\n';
      return 0;
    }
    if (Command == "run") {
      CommandOptions Options;
      std::vector<std::string> ProgramArguments;
      int I = 2;
      for (; I < Argc; ++I) {
        const std::string_view Argument = Argv[I];
        if (Argument == "--") {
          ++I;
          break;
        }
        if (!Argument.empty() && Argument.front() == '-')
          throw std::runtime_error("unknown option '" + std::string(Argument) + "'");
        if (Options.Selector)
          throw std::runtime_error("usage: kelp run [<member>] [-- <args>]");
        Options.Selector = std::string(Argument);
      }
      for (; I < Argc; ++I)
        ProgramArguments.emplace_back(Argv[I]);
      const auto Targets = SelectTargets(Nodes, Options, true);
      return RunProject(*Targets.front(), ProgramArguments);
    }
    if (Command == "test") {
      const auto Options = ParseCommandOptions(Argc, Argv, false);
      const auto Targets = SelectTargets(Nodes, Options, false);
      Timer Total;
      for (const auto *Node : Targets) {
        Timer Target;
        const auto Prepared = Prepare(Node->Root, Node->Project);
        if (Prepared.Project.TestSources.empty()) {
          if (const int Status = Execute(Node->Root, CompilerCommand(Prepared.Project, {})))
            return Status;
        } else {
          for (const auto &Source : Prepared.Project.TestSources) {
            auto Arguments = CompilerCommand(Prepared.Project, {});
            Arguments.back() = Source;
            if (const int Status = Execute(Node->Root, Arguments))
              return Status;
          }
        }
        std::cerr << "tested " << Node->Project.Name << " in " << FormatDuration(Target.Seconds())
                  << '\n';
      }
      if (Targets.size() > 1)
        std::cerr << "tested " << Targets.size() << " projects in "
                  << FormatDuration(Total.Seconds()) << '\n';
      return 0;
    }
    if (Command == "package") {
      const auto Options = ParseCommandOptions(Argc, Argv, false);
      const auto Targets = SelectTargets(Nodes, Options, false);
      std::set<std::string> Built;
      Timer Total;
      for (const auto *Node : Targets)
        if (const int Status = Package(Node->Root, Node->Project, Built))
          return Status;
      if (Targets.size() > 1)
        std::cerr << "packaged " << Targets.size() << " projects in "
                  << FormatDuration(Total.Seconds()) << '\n';
      return 0;
    }
    throw std::runtime_error("unknown command '" + Command + "'");
  } catch (const std::exception &Error) {
    std::cerr << "kelp: " << Error.what() << '\n';
    return 2;
  }
}
