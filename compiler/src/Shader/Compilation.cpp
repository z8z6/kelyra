#include "Shader/Compilation.h"

#include "IR/Kelyra.h"
#include "Support/Log.h"
#include "Support/Option.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace kelyra;

namespace kelyra::shader {
// Backend entry points are private to the shader compilation implementation.
bool EmitDirectDxil(mlir::ModuleOp Module, llvm::StringRef Output, std::string &Error);
bool EmitDirectSpirv(mlir::ModuleOp Module, llvm::StringRef Output, std::string &Error);
} // namespace kelyra::shader

namespace {
enum class ShaderFormat { DXIL, SPIRV };
llvm::cl::opt<ShaderFormat> Format{
    "shader-format",
    llvm::cl::desc("Shader output format"),
    llvm::cl::values(clEnumValN(ShaderFormat::SPIRV, "spirv", "Vulkan SPIR-V"),
                     clEnumValN(ShaderFormat::DXIL, "dxil", "Direct3D 12 DXIL")),
    llvm::cl::init(ShaderFormat::SPIRV),
    llvm::cl::cat(Option::KelyraCategory)};

using K = lex::NodeKind;
using Node = lex::Node;

const Node *Child(const Node &Parent, K Kind) {
  for (const auto &Item : Parent.children)
    if (Item->kind == Kind)
      return Item.get();
  return nullptr;
}

const sema::AnnotationInstance *Annotation(const sema::Sema &Analysis, const Node &Target,
                                           std::string_view Name) {
  const auto FullName = "std.graphics." + std::string(Name);
  for (const auto &Instance : Analysis.GetAnnotations(Target))
    if (Instance.Name == FullName)
      return &Instance;
  return nullptr;
}

std::string AnnotationValue(const sema::AnnotationInstance &Instance) {
  return Instance.Arguments.empty() ? "" : Instance.Arguments.front().Value.Text;
}

[[noreturn]] void Fail(const Node &At, std::string_view Message) {
  throw std::runtime_error(std::string(At.Loc.File) + ":" + std::to_string(At.Loc.Line) + ":" +
                           std::to_string(At.Loc.Column) + ": " + std::string(Message));
}

class ShaderProgram {
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
  mlir::MLIRContext Context;
  mlir::OpBuilder Builder;
  mlir::OwningOpRef<mlir::ModuleOp> ShaderModule;
  std::map<const Node *, mlir::Operation *> IRFunctions;
  std::map<std::string, mlir::Value> Variables;

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

  std::string ResourceKind(const Node &Class) const {
    const auto *Resource = Annotation(Analysis, Class, "resource");
    if (!Resource)
      return {};
    const auto Kind = AnnotationValue(*Resource);
    if (Kind.ends_with("Texture2D"))
      return "texture2d";
    if (Kind.ends_with("SamplerState"))
      return "sampler";
    Fail(Class, "unsupported shader resource kind");
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
    Fail(TypeNode, "shader type is not supported by this GPU target");
  }

  const Node &RequiredType(const Node &Parent) const {
    const auto *Result = Child(Parent, K::ast_type);
    if (!Result)
      Fail(Parent, "shader declaration needs an explicit type");
    return *Result;
  }

  void VisitClass(const Node &Class) {
    if (!ResourceKind(Class).empty())
      return;
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
      } else if (Member->kind != K::ast_public && Member->kind != K::ast_annotation)
        Fail(*Member, "shader data classes may contain fields and constructors");
    }
    if (Fields.empty())
      Fail(Class, "shader data class needs at least one field");
    if (Constructor) {
      std::vector<const Node *> Parameters;
      for (const auto &Part : Constructor->children)
        if (Part->kind == K::ast_parameter)
          Parameters.push_back(Part.get());
      const auto *Body = Child(*Constructor, K::ast_block);
      if (!Body || Parameters.size() != Fields.size() || Body->children.size() != Fields.size())
        Fail(*Constructor, "shader constructor must assign each field from one parameter");
      for (std::size_t Index = 0; Index < Fields.size(); ++Index) {
        const auto &Assignment = *Body->children[Index];
        if (Assignment.kind != K::ast_assign || Assignment.children.size() != 2 ||
            Assignment.children[0]->kind != K::ast_member ||
            Assignment.children[0]->text != Fields[Index]->text ||
            Assignment.children[0]->children.size() != 1 ||
            Assignment.children[0]->children[0]->kind != K::ast_name ||
            Assignment.children[0]->children[0]->text != "this" ||
            Assignment.children[1]->kind != K::ast_name ||
            Assignment.children[1]->text != Parameters[Index]->text ||
            RequiredType(*Fields[Index]).text != RequiredType(*Parameters[Index]).text)
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
      else {
        const auto &Function = CalledFunction(Expr);
        if (!Annotation(Analysis, Function, "gpu_builtin"))
          VisitFunction(Function);
      }
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

  mlir::Location IRLocation(const Node &At) {
    const std::string File = At.Loc.File.empty() ? "<shader>" : std::string(At.Loc.File);
    return mlir::FileLineColLoc::get(&Context, File, At.Loc.Line, At.Loc.Column);
  }

  mlir::Type IRType(const sema::Type &Value, const Node &At) {
    if (!Value.Modifiers.empty())
      Fail(At, "shader IR cannot contain pointers, arrays, or slices");
    switch (Value.Element) {
    case sema::BuiltinType::F32:
      return Builder.getF32Type();
    case sema::BuiltinType::I32:
      return mlir::IntegerType::get(&Context, 32, mlir::IntegerType::Signed);
    case sema::BuiltinType::U32:
      return mlir::IntegerType::get(&Context, 32, mlir::IntegerType::Unsigned);
    case sema::BuiltinType::Bool:
      return Builder.getI1Type();
    case sema::BuiltinType::Class: {
      const auto Found = Classes.find(Value.ClassName);
      if (Found != Classes.end()) {
        const auto Kind = ResourceKind(*Found->second);
        if (!Kind.empty())
          return ir::ShaderResourceType::get(&Context, Builder.getStringAttr(Kind));
        return ir::ShaderRecordType::get(&Context,
                                         Builder.getStringAttr(ClassName(*Found->second)));
      }
      break;
    }
    default:
      break;
    }
    Fail(At, "type is unavailable in shader IR");
  }

  mlir::Type IRType(const Node &TypeNode) { return IRType(Analysis.GetType(TypeNode), TypeNode); }

  mlir::Operation *IROp(const Node &At, llvm::StringRef Name, mlir::TypeRange Results = {},
                        mlir::ValueRange Operands = {},
                        llvm::ArrayRef<mlir::NamedAttribute> Attributes = {},
                        unsigned Regions = 0) {
    mlir::OperationState State(IRLocation(At), Name);
    State.addTypes(Results);
    State.addOperands(Operands);
    State.addAttributes(Attributes);
    for (unsigned Index = 0; Index < Regions; ++Index)
      State.addRegion();
    return Builder.create(State);
  }

  mlir::NamedAttribute IRAttr(llvm::StringRef Name, mlir::Attribute Value) {
    return Builder.getNamedAttr(Name, Value);
  }

  mlir::Value IRExpression(const Node &Value) {
    const auto ResultType = IRType(Analysis.GetType(Value), Value);
    switch (Value.kind) {
    case K::ast_name: {
      const auto Found = Variables.find(Value.text);
      if (Found == Variables.end())
        Fail(Value, "shader IR cannot resolve local name");
      return IROp(Value, "kelyra.shader.load", {ResultType}, {Found->second})->getResult(0);
    }
    case K::ast_literal:
      if (Value.text != "true" && Value.text != "false" &&
          (Value.text.empty() || !std::isdigit(static_cast<unsigned char>(Value.text.front()))))
        Fail(Value, "string values are unavailable in shaders");
      return IROp(Value,
                  "kelyra.shader.literal",
                  {ResultType},
                  {},
                  {IRAttr("text", Builder.getStringAttr(Value.text))})
          ->getResult(0);
    case K::ast_group:
      return IRExpression(*Value.children.at(0));
    case K::ast_unary: {
      if (Value.text != "-" && Value.text != "!")
        Fail(Value, "unsupported shader unary operator");
      auto Input = IRExpression(*Value.children.at(0));
      return IROp(Value,
                  "kelyra.shader.unary",
                  {ResultType},
                  {Input},
                  {IRAttr("opcode", Builder.getStringAttr(Value.text))})
          ->getResult(0);
    }
    case K::ast_binary: {
      auto Lhs = IRExpression(*Value.children.at(0));
      auto Rhs = IRExpression(*Value.children.at(1));
      return IROp(Value,
                  "kelyra.shader.binary",
                  {ResultType},
                  {Lhs, Rhs},
                  {IRAttr("opcode", Builder.getStringAttr(Value.text))})
          ->getResult(0);
    }
    case K::ast_member: {
      auto Record = IRExpression(*Value.children.at(0));
      return IROp(Value,
                  "kelyra.shader.extract",
                  {ResultType},
                  {Record},
                  {IRAttr("field", Builder.getStringAttr(Value.text))})
          ->getResult(0);
    }
    case K::ast_call: {
      if (Value.children.empty())
        Fail(Value, "shader call has no callee");
      llvm::SmallVector<mlir::Value> Arguments;
      for (std::size_t Index = 1; Index < Value.children.size(); ++Index)
        Arguments.push_back(IRExpression(*Value.children[Index]));
      if (const auto *Class = Analysis.GetConstructorCall(Value)) {
        unsigned Fields = 0;
        for (const auto &Member : Class->Node->children)
          Fields += Member->kind == K::ast_field;
        if (Fields != Arguments.size())
          Fail(Value, "shader constructor field count does not match");
        return IROp(Value,
                    "kelyra.shader.construct",
                    {ResultType},
                    Arguments,
                    {IRAttr("record",
                            mlir::FlatSymbolRefAttr::get(&Context, ClassName(*Class->Node)))})
            ->getResult(0);
      }
      const auto &Function = CalledFunction(Value);
      if (const auto *Builtin = Annotation(Analysis, Function, "gpu_builtin")) {
        auto Name = AnnotationValue(*Builtin);
        if (Name.size() >= 2 && Name.front() == '"' && Name.back() == '"')
          Name = Name.substr(1, Name.size() - 2);
        if (Name.empty() || !std::all_of(Name.begin(), Name.end(), [](unsigned char Character) {
              return std::isalnum(Character) || Character == '_';
            }))
          Fail(Value, "gpu_builtin name must be an identifier");
        return IROp(Value,
                    "kelyra.shader.intrinsic",
                    {ResultType},
                    Arguments,
                    {IRAttr("name", Builder.getStringAttr(Name))})
            ->getResult(0);
      }
      return IROp(
                 Value,
                 "kelyra.shader.call",
                 {ResultType},
                 Arguments,
                 {IRAttr("callee", mlir::FlatSymbolRefAttr::get(&Context, FunctionName(Function)))})
          ->getResult(0);
    }
    case K::ast_cast: {
      auto Input = IRExpression(*Value.children.at(0));
      return IROp(Value, "kelyra.shader.cast", {ResultType}, {Input})->getResult(0);
    }
    default:
      Fail(Value, "expression is unavailable in shader IR");
    }
  }

  void IRYieldIfNeeded(const Node &At, mlir::Block &Block) {
    if (Block.empty() || !Block.back().hasTrait<mlir::OpTrait::IsTerminator>())
      IROp(At, "kelyra.shader.yield");
  }

  void IRStatement(const Node &Value) {
    switch (Value.kind) {
    case K::ast_block:
      for (const auto &Item : Value.children) {
        if (Builder.getBlock()->empty() ||
            !Builder.getBlock()->back().hasTrait<mlir::OpTrait::IsTerminator>())
          IRStatement(*Item);
      }
      return;
    case K::ast_return:
      if (Value.children.size() != 1)
        Fail(Value, "shader functions must return one value");
      IROp(Value, "kelyra.shader.return", {}, {IRExpression(*Value.children.front())});
      return;
    case K::ast_if: {
      auto Condition = IRExpression(*Value.children.at(0));
      auto *Branch = IROp(Value, "kelyra.shader.if", {}, {Condition}, {}, 2);
      auto SavedVariables = Variables;
      for (unsigned RegionIndex = 0; RegionIndex < 2; ++RegionIndex) {
        auto &Region = Branch->getRegion(RegionIndex);
        auto *Block = new mlir::Block();
        Region.push_back(Block);
        Builder.setInsertionPointToStart(Block);
        Variables = SavedVariables;
        if (RegionIndex == 0 || Value.children.size() > 2)
          IRStatement(*Value.children.at(RegionIndex == 0 ? 1 : 2));
        IRYieldIfNeeded(Value, *Block);
      }
      Variables = std::move(SavedVariables);
      Builder.setInsertionPointAfter(Branch);
      return;
    }
    case K::ast_while: {
      auto *Loop = IROp(Value, "kelyra.shader.while", {}, {}, {}, 2);
      auto SavedVariables = Variables;
      auto *ConditionBlock = new mlir::Block();
      Loop->getRegion(0).push_back(ConditionBlock);
      Builder.setInsertionPointToStart(ConditionBlock);
      auto Condition = IRExpression(*Value.children.at(0));
      IROp(Value, "kelyra.shader.yield", {}, {Condition});
      auto *BodyBlock = new mlir::Block();
      Loop->getRegion(1).push_back(BodyBlock);
      Builder.setInsertionPointToStart(BodyBlock);
      Variables = SavedVariables;
      IRStatement(*Value.children.at(1));
      IRYieldIfNeeded(Value, *BodyBlock);
      Variables = std::move(SavedVariables);
      Builder.setInsertionPointAfter(Loop);
      return;
    }
    case K::ast_let: {
      if (Value.children.empty() || Value.children.front()->kind != K::ast_name)
        Fail(Value, "shader let must bind one name");
      const auto *ExplicitType = Child(Value, K::ast_type);
      auto ValueType =
          ExplicitType ? IRType(*ExplicitType) : IRType(Analysis.GetType(Value), Value);
      llvm::SmallVector<mlir::Value> Initial;
      for (const auto &Part : Value.children)
        if (Part->kind != K::ast_name && Part->kind != K::ast_type)
          Initial.push_back(IRExpression(*Part));
      if (Initial.size() > 1)
        Fail(Value, "shader let has multiple initializers");
      auto *Variable = IROp(Value,
                            "kelyra.shader.var",
                            {ir::ShaderRefType::get(&Context, ValueType)},
                            Initial,
                            {IRAttr("name", Builder.getStringAttr(Value.children.front()->text)),
                             IRAttr("parameter", Builder.getBoolAttr(false))});
      Variables[Value.children.front()->text] = Variable->getResult(0);
      return;
    }
    case K::ast_assign: {
      const auto *Target = Value.children.at(0).get();
      llvm::SmallVector<mlir::Attribute> Path;
      while (Target->kind == K::ast_member) {
        Path.insert(Path.begin(), Builder.getStringAttr(Target->text));
        Target = Target->children.at(0).get();
      }
      if (Target->kind != K::ast_name || !Variables.contains(Target->text))
        Fail(Value, "shader assignment needs a local variable target");
      auto ValueToStore = IRExpression(*Value.children.at(1));
      if (Path.empty())
        IROp(Value, "kelyra.shader.store", {}, {Variables.at(Target->text), ValueToStore});
      else
        IROp(Value,
             "kelyra.shader.store_field",
             {},
             {Variables.at(Target->text), ValueToStore},
             {IRAttr("path", Builder.getArrayAttr(Path))});
      return;
    }
    case K::ast_expr_stmt:
      IROp(Value, "kelyra.shader.eval", {}, {IRExpression(*Value.children.at(0))});
      return;
    case K::ast_break:
      IROp(Value, "kelyra.shader.break");
      return;
    case K::ast_continue:
      IROp(Value, "kelyra.shader.continue");
      return;
    default:
      Fail(Value, "statement is unavailable in shader IR");
    }
  }

  mlir::ArrayAttr IRDecorations(const Node &Target) {
    llvm::SmallVector<mlir::Attribute> Values;
    for (const auto Name : {"position", "location", "vertex_index"})
      if (const auto *Instance = Annotation(Analysis, Target, Name)) {
        std::string Value = Name;
        if (!Instance->Arguments.empty())
          Value += ":" + AnnotationValue(*Instance);
        Values.push_back(Builder.getStringAttr(Value));
      }
    if (const auto *Binding = Annotation(Analysis, Target, "binding")) {
      if (Binding->Arguments.size() != 2)
        Fail(Target, "@std.graphics.binding requires set and slot");
      const auto Set = Binding->Arguments[0].Value.Text;
      const auto Slot = Binding->Arguments[1].Value.Text;
      auto Numeric = [](const std::string &Text) {
        return !Text.empty() && std::all_of(Text.begin(), Text.end(), [](unsigned char C) {
          return std::isdigit(C);
        });
      };
      if (!Numeric(Set) || !Numeric(Slot))
        Fail(Target, "@std.graphics.binding requires literal unsigned numbers");
      Values.push_back(Builder.getStringAttr("binding:" + Set + ":" + Slot));
    }
    return Builder.getArrayAttr(Values);
  }

  void BuildIR() {
    Context.loadDialect<ir::KelyraDialect>();
    ShaderModule = mlir::ModuleOp::create(mlir::UnknownLoc::get(&Context));
    Builder.setInsertionPointToEnd(ShaderModule->getBody());
    for (const auto *Class : OrderedClasses) {
      llvm::SmallVector<mlir::Attribute> Fields;
      llvm::SmallVector<mlir::Attribute> Types;
      llvm::SmallVector<mlir::Attribute> Decorations;
      for (const auto &Member : Class->children)
        if (Member->kind == K::ast_field) {
          Fields.push_back(Builder.getStringAttr(Member->text));
          Types.push_back(mlir::TypeAttr::get(IRType(RequiredType(*Member))));
          Decorations.push_back(IRDecorations(*Member));
        }
      IROp(*Class,
           "kelyra.shader.record",
           {},
           {},
           {IRAttr("sym_name", Builder.getStringAttr(ClassName(*Class))),
            IRAttr("fields", Builder.getArrayAttr(Fields)),
            IRAttr("field_types", Builder.getArrayAttr(Types)),
            IRAttr("decorations", Builder.getArrayAttr(Decorations))});
    }
    for (const auto *Function : OrderedFunctions) {
      llvm::SmallVector<mlir::Type> ParameterTypes;
      llvm::SmallVector<mlir::Attribute> ParameterNames;
      llvm::SmallVector<mlir::Attribute> InputDecorations;
      for (const auto &Part : Function->children)
        if (Part->kind == K::ast_parameter) {
          auto ParameterType = IRType(RequiredType(*Part));
          const bool IsResource = mlir::isa<ir::ShaderResourceType>(ParameterType);
          const bool HasBinding = Annotation(Analysis, *Part, "binding") != nullptr;
          if (Function == &Entry && IsResource != HasBinding)
            Fail(*Part,
                 IsResource ? "shader resource parameter needs @std.graphics.binding"
                            : "@std.graphics.binding requires a shader resource");
          if (Function != &Entry && HasBinding)
            Fail(*Part, "helper shader parameter cannot have @std.graphics.binding");
          ParameterTypes.push_back(ParameterType);
          ParameterNames.push_back(Builder.getStringAttr(Part->text));
          InputDecorations.push_back(IRDecorations(*Part));
        }
      const auto ReturnType = IRType(RequiredType(*Function));
      auto *IROperation =
          IROp(*Function,
               "kelyra.shader.func",
               {},
               {},
               {IRAttr("sym_name", Builder.getStringAttr(FunctionName(*Function))),
                IRAttr("function_type",
                       mlir::TypeAttr::get(Builder.getFunctionType(ParameterTypes, {ReturnType}))),
                IRAttr("parameter_names", Builder.getArrayAttr(ParameterNames)),
                IRAttr("input_decorations", Builder.getArrayAttr(InputDecorations)),
                IRAttr("stage",
                       Builder.getStringAttr(Function == &Entry ? (Vertex ? "vertex" : "fragment")
                                                                : "helper"))},
               1);
      IRFunctions.emplace(Function, IROperation);
      auto *Block = new mlir::Block();
      IROperation->getRegion(0).push_back(Block);
      Builder.setInsertionPointToStart(Block);
      Variables.clear();
      unsigned ParameterIndex = 0;
      for (const auto &Part : Function->children)
        if (Part->kind == K::ast_parameter) {
          auto Argument = Block->addArgument(ParameterTypes[ParameterIndex], IRLocation(*Part));
          auto *Variable = IROp(*Part,
                                "kelyra.shader.var",
                                {ir::ShaderRefType::get(&Context, ParameterTypes[ParameterIndex])},
                                {Argument},
                                {IRAttr("name", Builder.getStringAttr(Part->text)),
                                 IRAttr("parameter", Builder.getBoolAttr(true))});
          Variables[Part->text] = Variable->getResult(0);
          ++ParameterIndex;
        }
      IRStatement(*Child(*Function, K::ast_block));
      if (Block->empty() || !Block->back().hasTrait<mlir::OpTrait::IsTerminator>())
        Fail(*Function, "shader function must return a value");
      Builder.setInsertionPointToEnd(ShaderModule->getBody());
    }
    if (mlir::failed(mlir::verify(*ShaderModule)))
      throw std::runtime_error("generated Shader IR failed verification");
  }

public:
  ShaderProgram(const ModuleLoader &Loader, const Node &Entry, bool Vertex,
                const sema::Sema &Analysis)
      : Entry(Entry), Analysis(Analysis), Vertex(Vertex), Builder(&Context) {
    for (const auto *Loaded : Loader.GetModules()) {
      const auto &Module = *Loaded->Lex.root;
      const auto &ModuleName = Loaded->Name;
      for (const auto &Declaration : Module.children) {
        if (Declaration->kind == K::ast_class) {
          const auto FullName =
              ModuleName.empty() ? Declaration->text : ModuleName + "." + Declaration->text;
          Classes.emplace(FullName, Declaration.get());
          ClassNames.emplace(Declaration.get(), FullName);
          if (Loaded->IsEntry)
            Classes.emplace(Declaration->text, Declaration.get());
        }
        if (Declaration->kind == K::ast_function) {
          Functions.emplace(Analysis.GetSymbol(*Declaration), Declaration.get());
          FunctionNames.emplace(Declaration.get(), Analysis.GetSymbol(*Declaration));
        }
      }
    }
    VisitFunction(Entry);
    BuildIR();
  }

  void DumpIR() {
    ShaderModule->print(llvm::outs());
    llvm::outs() << '\n';
  }

  mlir::ModuleOp GetIR() const { return *ShaderModule; }
};

} // namespace

int shader::Emit(const ModuleLoader &Loader, const sema::Sema &Analysis, bool NativeOutput) {
  try {
    struct ShaderInput {
      const Node *Entry;
      bool Vertex;
      std::string Filename;
    };
    const bool Dxil = Format == ShaderFormat::DXIL;
    std::vector<ShaderInput> Inputs;
    std::set<std::string> Filenames;
    for (const auto *Source : Loader.GetModules()) {
      if (Source->IsExternal || Analysis.IsMetaModule(Source->Name))
        continue;
      for (const auto &Declaration : Source->Lex.root->children) {
        if (Analysis.IsMetaDeclaration(*Declaration) || !Analysis.IsShaderDeclaration(*Declaration))
          continue;
        const bool Vertex = Annotation(Analysis, *Declaration, "vertex") != nullptr;
        const bool Fragment = Annotation(Analysis, *Declaration, "fragment") != nullptr;
        if (Vertex == Fragment)
          Fail(*Declaration, "shader needs exactly one @vertex or @fragment");
        auto Filename = (Source->Name.empty() ? Source->Path.stem().string() : Source->Name) + "." +
                        Declaration->text + (Vertex ? ".vert" : ".frag") +
                        (Dxil ? ".dxil" : ".spv");
        // Keep generated identifiers from becoming paths on either host platform.
        for (auto &Character : Filename)
          if (static_cast<unsigned char>(Character) < 32 ||
              std::string_view("<>:\"/\\|?*").find(Character) != std::string_view::npos)
            Character = '_';
        auto Key = Filename;
        std::transform(Key.begin(), Key.end(), Key.begin(), [](unsigned char Ch) {
          return static_cast<char>(std::tolower(Ch));
        });
        if (!Filenames.insert(Key).second)
          Fail(*Declaration, "shader output name conflicts with another shader: " + Filename);
        Inputs.push_back({Declaration.get(), Vertex, std::move(Filename)});
      }
    }
    if (Inputs.empty())
      return 0;
    if (Option::OutputFile.empty() && !Option::DumpMlir)
      throw std::runtime_error("shader output requires -o");
    auto Directory = std::filesystem::path(Option::OutputFile.getValue());
    if (NativeOutput)
      Directory = Directory.parent_path() / "shaders";
    if (!Option::OutputFile.empty())
      std::filesystem::create_directories(Directory);
    for (const auto &Input : Inputs) {
      ShaderProgram Program(Loader, *Input.Entry, Input.Vertex, Analysis);
      if (Option::DumpMlir)
        Program.DumpIR();
      if (Option::OutputFile.empty())
        continue;
      const auto Output = (Directory / Input.Filename).string();
      std::string Error;
      if (!(Dxil ? shader::EmitDirectDxil(Program.GetIR(), Output, Error)
                 : shader::EmitDirectSpirv(Program.GetIR(), Output, Error)))
        Fail(*Input.Entry, Error);
    }
    return 0;
  } catch (const std::exception &Error) {
    kerr() << Option::InputFile << ": error: " << Error.what() << '\n';
    return 1;
  }
}

bool shader::ValidateOutput(bool ExclusiveAction) {
  if (ExclusiveAction && Format.getNumOccurrences() != 0) {
    kerr() << "--shader-format cannot be combined with --emit-c-defs\n";
    return false;
  }
  return true;
}
