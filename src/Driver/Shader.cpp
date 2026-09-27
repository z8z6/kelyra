#include "Command.h"

#include "Support/Log.h"
#include "Support/Option.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace kelyra;

namespace {
using K = lex::TokenKind;
using Node = lex::Node;

const Node *Child(const Node &Parent, K Kind) {
  for (const auto &Item : Parent.children)
    if (Item->kind == Kind)
      return Item.get();
  return nullptr;
}

const sema::AnnotationInstance *Annotation(const sema::Sema &Analysis,
                                           const Node &Target,
                                           std::string_view Name) {
  const auto FullName = "std.graphics." + std::string(Name);
  for (const auto &Instance : Analysis.GetAnnotations(Target))
    if (Instance.Name == FullName)
      return &Instance;
  return nullptr;
}

std::string AnnotationValue(const sema::AnnotationInstance &Instance) {
  return Instance.Arguments.empty() ? ""
                                    : Instance.Arguments.front().Value.Text;
}

[[noreturn]] void Fail(const Node &At, std::string_view Message) {
  throw std::runtime_error(
      std::string(At.Loc.File) + ":" + std::to_string(At.Loc.Line) + ":" +
      std::to_string(At.Loc.Column) + ": " + std::string(Message));
}

std::string Indent(unsigned Depth) { return std::string(Depth * 2, ' '); }

class GlslShader {
  const Node &Entry;
  const sema::Sema &Analysis;
  bool Vertex;
  std::map<std::string, const Node *> Classes;
  std::map<const Node *, std::string> ClassNames;
  std::map<std::string, const Node *> Functions;
  std::map<const Node *, std::string> FunctionNames;
  std::set<std::string> Reached;
  std::set<std::string> Visiting;
  std::vector<const Node *> OrderedClasses;
  std::vector<const Node *> OrderedFunctions;
  std::set<std::string> EmittedClasses;

  static std::string QualifiedName(const Node &Value) {
    if (Value.kind == K::ast_name)
      return Value.text;
    if (Value.kind == K::ast_member && Value.children.size() == 1)
      return QualifiedName(*Value.children.front()) + "." + Value.text;
    Fail(Value, "shader call target must be a named function or class");
  }

  std::string ClassName(const Node &Class) const {
    auto Name = ClassNames.at(&Class);
    std::replace(Name.begin(), Name.end(), '.', '_');
    return "K_" + Name;
  }

  std::string FunctionName(const Node &Function) const {
    auto Name = FunctionNames.at(&Function);
    std::replace(Name.begin(), Name.end(), '.', '_');
    return "kelyra_" + Name;
  }

  const Node &CalledFunction(const Node &Call) const {
    if (Call.children.empty())
      Fail(Call, "shader calls must name a function or constructor");
    QualifiedName(*Call.children.front());
    try {
      const auto &Symbol = Analysis.GetCallee(Call);
      if (const auto It = Functions.find(Symbol); It != Functions.end())
        return *It->second;
    } catch (const std::out_of_range &) {
    }
    Fail(Call, "shader call target is not available in this module");
  }

  const Node *ClassForType(const Node &TypeNode) const {
    if (const auto It = Classes.find(TypeNode.text); It != Classes.end())
      return It->second;
    const auto &Resolved = Analysis.GetType(TypeNode);
    if (Resolved.Element != sema::BuiltinType::Class)
      return nullptr;
    const auto It = Classes.find(Resolved.ClassName);
    return It == Classes.end() ? nullptr : It->second;
  }

  std::string Type(const Node &TypeNode) const {
    if (TypeNode.kind != K::ast_type)
      Fail(TypeNode, "shader type must be a scalar or data class");
    if (TypeNode.text == "void")
      return "void";
    if (TypeNode.text == "f32")
      return "float";
    if (TypeNode.text == "i32")
      return "int";
    if (TypeNode.text == "u32")
      return "uint";
    if (TypeNode.text == "bool")
      return "bool";
    if (const auto *Class = ClassForType(TypeNode))
      return ClassName(*Class);
    Fail(TypeNode, "shader type is not supported by the SPIR-V target");
  }

  std::string Type(const sema::Type &Value, const Node &At) const {
    if (!Value.Modifiers.empty())
      Fail(At, "shader pointers, arrays, and slices are not yet supported");
    switch (Value.Element) {
    case sema::BuiltinType::F32:
      return "float";
    case sema::BuiltinType::I32:
      return "int";
    case sema::BuiltinType::U32:
      return "uint";
    case sema::BuiltinType::Bool:
      return "bool";
    case sema::BuiltinType::Class: {
      if (Classes.contains(Value.ClassName))
        return ClassName(*Classes.at(Value.ClassName));
      break;
    }
    default:
      break;
    }
    Fail(At, "inferred shader type is unavailable in the SPIR-V target");
  }

  const Node &RequiredType(const Node &Parent) const {
    const auto *Result = Child(Parent, K::ast_type);
    if (!Result)
      Fail(Parent, "shader declaration needs an explicit type");
    return *Result;
  }

  void VisitClass(const Node &Class) {
    const auto &Name = ClassNames.at(&Class);
    if (EmittedClasses.contains(Name))
      return;
    if (Visiting.contains(Name))
      Fail(Class, "recursive shader data class");
    Visiting.insert(Name);
    std::vector<const Node *> Fields;
    const Node *Constructor = nullptr;
    for (const auto &Member : Class.children) {
      if (Member->kind == K::ast_field) {
        Fields.push_back(Member.get());
        const auto &FieldType = RequiredType(*Member);
        if (const auto *Nested = ClassForType(FieldType))
          VisitClass(*Nested);
        else
          Type(FieldType);
      } else if (Member->kind == K::ast_constructor) {
        if (Constructor)
          Fail(*Member, "shader data class has multiple constructors");
        Constructor = Member.get();
      } else if (Member->kind != K::ast_public &&
                 Member->kind != K::ast_annotation)
        Fail(*Member,
             "shader data classes may contain fields and constructors");
    }
    if (Fields.empty())
      Fail(Class, "shader data class needs at least one field");
    if (Constructor) {
      std::vector<const Node *> Parameters;
      for (const auto &Part : Constructor->children)
        if (Part->kind == K::ast_parameter)
          Parameters.push_back(Part.get());
      const auto *Body = Child(*Constructor, K::ast_block);
      if (!Body || Parameters.size() != Fields.size() ||
          Body->children.size() != Fields.size())
        Fail(*Constructor,
             "shader constructor must assign each field from one parameter");
      for (std::size_t Index = 0; Index < Fields.size(); ++Index) {
        const auto &Assignment = *Body->children[Index];
        if (Assignment.kind != K::ast_assign ||
            Assignment.children.size() != 2 ||
            Assignment.children[0]->kind != K::ast_member ||
            Assignment.children[0]->text != Fields[Index]->text ||
            Assignment.children[0]->children.size() != 1 ||
            Assignment.children[0]->children[0]->kind != K::ast_name ||
            Assignment.children[0]->children[0]->text != "this" ||
            Assignment.children[1]->kind != K::ast_name ||
            Assignment.children[1]->text != Parameters[Index]->text ||
            RequiredType(*Fields[Index]).text !=
                RequiredType(*Parameters[Index]).text)
          Fail(Assignment, "shader constructor must copy fields in order");
      }
    }
    Visiting.erase(Name);
    EmittedClasses.insert(Name);
    OrderedClasses.push_back(&Class);
  }

  void VisitCalls(const Node &Expr) {
    if (Expr.kind == K::ast_type)
      if (const auto *Class = ClassForType(Expr))
        VisitClass(*Class);
    if (Expr.kind == K::ast_call && !Expr.children.empty()) {
      if (const auto *Class = Analysis.GetConstructorCall(Expr))
        VisitClass(*Class->Node);
      else
        VisitFunction(CalledFunction(Expr));
    }
    for (const auto &Part : Expr.children)
      VisitCalls(*Part);
  }

  void VisitFunction(const Node &Function) {
    const auto &Name = FunctionNames.at(&Function);
    if (Reached.contains(Name))
      return;
    if (Visiting.contains(Name))
      Fail(Function, "recursive shader calls are not supported");
    Visiting.insert(Name);
    VisitCalls(RequiredType(Function));
    for (const auto &Part : Function.children)
      if (Part->kind == K::ast_parameter)
        VisitCalls(RequiredType(*Part));
    const auto *Body = Child(Function, K::ast_block);
    if (!Body)
      Fail(Function, "shader function must have a body");
    VisitCalls(*Body);
    Visiting.erase(Name);
    Reached.insert(Name);
    OrderedFunctions.push_back(&Function);
  }

  std::string Expr(const Node &Value) const {
    switch (Value.kind) {
    case K::ast_name:
      return Value.text;
    case K::ast_literal:
      if (Value.text == "true" || Value.text == "false" ||
          (!Value.text.empty() &&
           (std::isdigit(static_cast<unsigned char>(Value.text.front())))))
        return Value.text;
      Fail(Value, "string values are unavailable in shaders");
    case K::ast_group:
      return "(" + Expr(*Value.children.at(0)) + ")";
    case K::ast_unary:
      if (Value.text != "-" && Value.text != "!")
        Fail(Value, "unsupported shader unary operator");
      return "(" + Value.text + Expr(*Value.children.at(0)) + ")";
    case K::ast_binary:
      return "(" + Expr(*Value.children.at(0)) + " " + Value.text + " " +
             Expr(*Value.children.at(1)) + ")";
    case K::ast_member:
      return Expr(*Value.children.at(0)) + "." + Value.text;
    case K::ast_call: {
      if (Value.children.empty())
        Fail(Value, "shader calls must name a function or constructor");
      std::string Result;
      const auto *Class = Analysis.GetConstructorCall(Value);
      if (Class)
        Result = ClassName(*Class->Node);
      else
        Result = FunctionName(CalledFunction(Value));
      if (Class) {
        unsigned Fields = 0;
        for (const auto &Member : Class->Node->children)
          Fields += Member->kind == K::ast_field;
        if (Value.children.size() - 1 != Fields)
          Fail(Value,
               "shader data class construction needs one value per field");
      }
      Result += "(";
      for (std::size_t Index = 1; Index < Value.children.size(); ++Index) {
        if (Index > 1)
          Result += ", ";
        Result += Expr(*Value.children[Index]);
      }
      return Result + ")";
    }
    case K::ast_cast:
      return Type(*Value.children.at(1)) + "(" + Expr(*Value.children.at(0)) +
             ")";
    default:
      Fail(Value, "expression is unavailable in shaders");
    }
  }

  void Statement(std::ostringstream &Out, const Node &Value,
                 unsigned Depth) const {
    const auto Prefix = Indent(Depth);
    switch (Value.kind) {
    case K::ast_block:
      Out << Prefix << "{\n";
      for (const auto &Item : Value.children)
        Statement(Out, *Item, Depth + 1);
      Out << Prefix << "}\n";
      return;
    case K::ast_return:
      if (Value.children.size() != 1)
        Fail(Value, "shader functions must return one value");
      Out << Prefix << "return " << Expr(*Value.children.front()) << ";\n";
      return;
    case K::ast_if:
      Out << Prefix << "if (" << Expr(*Value.children.at(0)) << ")\n";
      Statement(Out, *Value.children.at(1), Depth);
      if (Value.children.size() > 2) {
        Out << Prefix << "else\n";
        Statement(Out, *Value.children.at(2), Depth);
      }
      return;
    case K::ast_while:
      Out << Prefix << "while (" << Expr(*Value.children.at(0)) << ")\n";
      Statement(Out, *Value.children.at(1), Depth);
      return;
    case K::ast_assign:
      Out << Prefix << Expr(*Value.children.at(0)) << " = "
          << Expr(*Value.children.at(1)) << ";\n";
      return;
    case K::ast_expr_stmt:
      Out << Prefix << Expr(*Value.children.at(0)) << ";\n";
      return;
    case K::ast_let: {
      if (Value.children.empty() || Value.children.front()->kind != K::ast_name)
        Fail(Value, "shader let must bind one name");
      const auto *ExplicitType = Child(Value, K::ast_type);
      Out << Prefix
          << (ExplicitType ? Type(*ExplicitType)
                           : Type(Analysis.GetType(Value), Value))
          << " " << Value.children.front()->text;
      for (const auto &Part : Value.children)
        if (Part->kind != K::ast_name && Part->kind != K::ast_type)
          Out << " = " << Expr(*Part);
      Out << ";\n";
      return;
    }
    case K::ast_break:
      Out << Prefix << "break;\n";
      return;
    case K::ast_continue:
      Out << Prefix << "continue;\n";
      return;
    default:
      Fail(Value, "statement is unavailable in shaders");
    }
  }

  std::string Signature(const Node &Function) const {
    std::string Result =
        Type(RequiredType(Function)) + " " + FunctionName(Function) + "(";
    bool First = true;
    for (const auto &Part : Function.children)
      if (Part->kind == K::ast_parameter) {
        if (!First)
          Result += ", ";
        Result += Type(RequiredType(*Part)) + " " + Part->text;
        First = false;
      }
    return Result + ")";
  }

  std::string InterfaceType(const Node &TypeNode) const {
    const auto *Class = ClassForType(TypeNode);
    if (!Class)
      return Type(TypeNode);
    std::string Scalar;
    unsigned Count = 0;
    for (const auto &Part : Class->children)
      if (Part->kind == K::ast_field) {
        const auto &FieldType = RequiredType(*Part);
        const auto Mapped = Type(FieldType);
        if (Mapped != "float" && Mapped != "int" && Mapped != "uint")
          Fail(FieldType, "shader interface vectors need scalar components");
        if (!Scalar.empty() && Scalar != Mapped)
          Fail(FieldType, "shader interface vector components must match");
        Scalar = Mapped;
        ++Count;
      }
    if (Count < 2 || Count > 4)
      Fail(TypeNode, "shader interface vector needs two to four components");
    return (Scalar == "float" ? "vec"
            : Scalar == "int" ? "ivec"
                              : "uvec") +
           std::to_string(Count);
  }

  std::string FromInterface(const Node &TypeNode,
                            const std::string &Name) const {
    const auto *Class = ClassForType(TypeNode);
    if (!Class)
      return Name;
    std::string Result = Type(TypeNode) + "(";
    bool First = true;
    for (const auto &Part : Class->children)
      if (Part->kind == K::ast_field) {
        if (!First)
          Result += ", ";
        Result += Name + "." + Part->text;
        First = false;
      }
    return Result + ")";
  }

  std::string ToInterface(const Node &TypeNode, const std::string &Name) const {
    const auto *Class = ClassForType(TypeNode);
    if (!Class)
      return Name;
    std::string Result = InterfaceType(TypeNode) + "(";
    bool First = true;
    for (const auto &Part : Class->children)
      if (Part->kind == K::ast_field) {
        if (!First)
          Result += ", ";
        Result += Name + "." + Part->text;
        First = false;
      }
    return Result + ")";
  }

public:
  GlslShader(const ModuleLoader &Loader, const Node &Entry, bool Vertex,
             const sema::Sema &Analysis)
      : Entry(Entry), Analysis(Analysis), Vertex(Vertex) {
    for (const auto &Loaded : Loader.GetModules()) {
      const auto &Module = *Loaded.Parsed.root;
      std::string ModuleName;
      for (const auto &Declaration : Module.children)
        if (Declaration->kind == K::ast_module_decl)
          ModuleName = Declaration->text;
      for (const auto &Declaration : Module.children) {
        if (Declaration->kind == K::ast_class) {
          const auto FullName = ModuleName.empty()
                                    ? Declaration->text
                                    : ModuleName + "." + Declaration->text;
          Classes.emplace(FullName, Declaration.get());
          ClassNames.emplace(Declaration.get(), FullName);
          if (Loaded.IsEntry)
            Classes.emplace(Declaration->text, Declaration.get());
        }
        if (Declaration->kind == K::ast_function) {
          Functions.emplace(Analysis.GetSymbol(*Declaration),
                            Declaration.get());
          FunctionNames.emplace(Declaration.get(),
                                ModuleName.empty()
                                    ? Declaration->text
                                    : ModuleName + "." + Declaration->text);
        }
      }
    }
    VisitFunction(Entry);
  }

  std::string Generate() const {
    std::ostringstream Out;
    Out << "#version 450\n";
    for (const auto *Class : OrderedClasses) {
      Out << "struct " << ClassName(*Class) << " {\n";
      for (const auto &Part : Class->children)
        if (Part->kind == K::ast_field)
          Out << "  " << Type(RequiredType(*Part)) << " " << Part->text
              << ";\n";
      Out << "};\n";
    }
    const auto &OutputClass = RequiredType(Entry);
    const auto *Output = ClassForType(OutputClass);
    if (!Output)
      Fail(OutputClass, "shader entry must return an interface data class");
    for (const auto &Field : Output->children) {
      if (Field->kind != K::ast_field)
        continue;
      const auto *Location = Annotation(Analysis, *Field, "location");
      const auto *Position = Annotation(Analysis, *Field, "position");
      if (Location && Position)
        Fail(*Field, "shader output cannot be both location and position");
      if (Location) {
        Out << "layout(location = " << AnnotationValue(*Location) << ") out "
            << InterfaceType(RequiredType(*Field)) << " kelyra_out_"
            << Field->text << ";\n";
      } else if (!Position || !Vertex)
        Fail(*Field, "unsupported shader output interface field");
      else if (InterfaceType(RequiredType(*Field)) != "vec4")
        Fail(*Field, "vertex position must have four f32 components");
    }
    for (const auto &Parameter : Entry.children) {
      if (Parameter->kind != K::ast_parameter)
        continue;
      const auto *Location = Annotation(Analysis, *Parameter, "location");
      const auto *VertexIndex =
          Annotation(Analysis, *Parameter, "vertex_index");
      if (Location && VertexIndex)
        Fail(*Parameter,
             "shader input cannot be both location and vertex_index");
      if (Location)
        Out << "layout(location = " << AnnotationValue(*Location) << ") in "
            << InterfaceType(RequiredType(*Parameter)) << " kelyra_in_"
            << Parameter->text << ";\n";
      else if (!VertexIndex || !Vertex ||
               RequiredType(*Parameter).text != "u32")
        Fail(*Parameter, "unsupported shader input interface parameter");
    }
    for (const auto *Function : OrderedFunctions)
      Out << Signature(*Function) << ";\n";
    for (const auto *Function : OrderedFunctions) {
      Out << Signature(*Function) << "\n";
      Statement(Out, *Child(*Function, K::ast_block), 0);
    }
    Out << "void main() {\n  " << Type(OutputClass)
        << " result = " << FunctionName(Entry) << "(";
    bool First = true;
    for (const auto &Parameter : Entry.children)
      if (Parameter->kind == K::ast_parameter) {
        if (!First)
          Out << ", ";
        if (Annotation(Analysis, *Parameter, "vertex_index"))
          Out << "uint(gl_VertexIndex)";
        else
          Out << FromInterface(RequiredType(*Parameter),
                               "kelyra_in_" + Parameter->text);
        First = false;
      }
    Out << ");\n";
    for (const auto &Field : Output->children)
      if (Field->kind == K::ast_field) {
        const auto Value =
            ToInterface(RequiredType(*Field), "result." + Field->text);
        if (Annotation(Analysis, *Field, "position"))
          Out << "  gl_Position = " << Value << ";\n";
        else
          Out << "  kelyra_out_" << Field->text << " = " << Value << ";\n";
      }
    Out << "}\n";
    return Out.str();
  }
};

} // namespace

int driver::EmitShader(const ModuleLoader &Loader, const sema::Sema &Analysis) {
  try {
    const auto &Root = *Loader.GetModules().front().Parsed.root;
    const Node *Entry = nullptr;
    for (const auto &Declaration : Root.children)
      if (Declaration->kind == K::ast_function &&
          Declaration->text == Option::ShaderEntry)
        Entry = Declaration.get();
    if (!Entry)
      throw std::runtime_error("shader entry function not found: " +
                               Option::ShaderEntry.getValue());
    const bool Vertex = Annotation(Analysis, *Entry, "vertex") != nullptr;
    const bool Fragment = Annotation(Analysis, *Entry, "fragment") != nullptr;
    if (Vertex == Fragment)
      Fail(*Entry, "shader entry needs exactly one @vertex or @fragment");
    const auto Source = GlslShader(Loader, *Entry, Vertex, Analysis).Generate();
    llvm::SmallString<256> SourcePath;
    int SourceFD = -1;
    if (const auto Error = llvm::sys::fs::createUniqueFile(
            "/tmp/kelyra-shader-%%%%%%.glsl", SourceFD, SourcePath))
      throw std::runtime_error("cannot create temporary shader source: " +
                               Error.message());
    {
      llvm::raw_fd_ostream Output(SourceFD, true);
      Output << Source;
    }
    const auto Compiler = llvm::sys::findProgramByName("glslangValidator");
    if (!Compiler) {
      llvm::sys::fs::remove(SourcePath);
      throw std::runtime_error(
          "glslangValidator is required for SPIR-V output");
    }
    const auto Stage = Vertex ? "vert" : "frag";
    const std::vector<llvm::StringRef> Arguments{*Compiler,
                                                 "--quiet",
                                                 "-V",
                                                 "-S",
                                                 Stage,
                                                 "-o",
                                                 Option::OutputFile.getValue(),
                                                 SourcePath};
    const int Status = llvm::sys::ExecuteAndWait(*Compiler, Arguments);
    llvm::sys::fs::remove(SourcePath);
    if (Status != 0)
      throw std::runtime_error("SPIR-V backend failed for shader entry " +
                               Option::ShaderEntry.getValue());
    const auto Validator = llvm::sys::findProgramByName("spirv-val");
    if (!Validator)
      throw std::runtime_error(
          "spirv-val is required to validate shader output");
    const std::vector<llvm::StringRef> Validation{
        *Validator, "--target-env", "vulkan1.0", Option::OutputFile.getValue()};
    if (llvm::sys::ExecuteAndWait(*Validator, Validation) != 0)
      throw std::runtime_error("generated SPIR-V failed validation");
    return 0;
  } catch (const std::exception &Error) {
    kerr() << Option::InputFile << ": error: " << Error.what() << '\n';
    return 1;
  }
}
