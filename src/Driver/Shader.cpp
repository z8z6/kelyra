#include "Command.h"
#include "Driver/DirectDxil.h"
#include "Driver/DirectSpirv.h"

#include "IR/Kelyra.h"
#include "Support/Log.h"
#include "Support/Option.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cctype>
#include <iterator>
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
  bool Hlsl;
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

  std::string ConstructorName(const Node &Class) const {
    return Hlsl ? "make_" + ClassName(Class) : ClassName(Class);
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
    Fail(TypeNode, "shader type is not supported by this GPU target");
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
    Fail(At, "inferred shader type is unavailable in this GPU target");
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
        Result = ConstructorName(*Class->Node);
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

  std::string Condition(const Node &Value) const {
    auto Result = Expr(Value);
    if (Result.size() >= 2 && Result.front() == '(' && Result.back() == ')')
      return Result;
    return "(" + Result + ")";
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
      Out << Prefix << "if " << Condition(*Value.children.at(0)) << "\n";
      Statement(Out, *Value.children.at(1), Depth);
      if (Value.children.size() > 2) {
        Out << Prefix << "else\n";
        Statement(Out, *Value.children.at(2), Depth);
      }
      return;
    case K::ast_while:
      Out << Prefix << "while " << Condition(*Value.children.at(0)) << "\n";
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
    if (Hlsl)
      return Scalar + std::to_string(Count);
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
    std::string Result = ConstructorName(*Class) + "(";
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

  mlir::Location IRLocation(const Node &At) {
    const std::string File =
        At.Loc.File.empty() ? "<shader>" : std::string(At.Loc.File);
    return mlir::FileLineColLoc::get(&Context, File, At.Loc.Line,
                                     At.Loc.Column);
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
      if (Found != Classes.end())
        return ir::ShaderRecordType::get(
            &Context, Builder.getStringAttr(ClassName(*Found->second)));
      break;
    }
    default:
      break;
    }
    Fail(At, "type is unavailable in shader IR");
  }

  mlir::Type IRType(const Node &TypeNode) {
    return IRType(Analysis.GetType(TypeNode), TypeNode);
  }

  mlir::Operation *IROp(const Node &At, llvm::StringRef Name,
                        mlir::TypeRange Results = {},
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
      return IROp(Value, "kelyra.shader.load", {ResultType}, {Found->second})
          ->getResult(0);
    }
    case K::ast_literal:
      if (Value.text != "true" && Value.text != "false" &&
          (Value.text.empty() ||
           !std::isdigit(static_cast<unsigned char>(Value.text.front()))))
        Fail(Value, "string values are unavailable in shaders");
      return IROp(Value, "kelyra.shader.literal", {ResultType}, {},
                  {IRAttr("text", Builder.getStringAttr(Value.text))})
          ->getResult(0);
    case K::ast_group:
      return IRExpression(*Value.children.at(0));
    case K::ast_unary: {
      if (Value.text != "-" && Value.text != "!")
        Fail(Value, "unsupported shader unary operator");
      auto Input = IRExpression(*Value.children.at(0));
      return IROp(Value, "kelyra.shader.unary", {ResultType}, {Input},
                  {IRAttr("opcode", Builder.getStringAttr(Value.text))})
          ->getResult(0);
    }
    case K::ast_binary: {
      auto Lhs = IRExpression(*Value.children.at(0));
      auto Rhs = IRExpression(*Value.children.at(1));
      return IROp(Value, "kelyra.shader.binary", {ResultType}, {Lhs, Rhs},
                  {IRAttr("opcode", Builder.getStringAttr(Value.text))})
          ->getResult(0);
    }
    case K::ast_member: {
      auto Record = IRExpression(*Value.children.at(0));
      return IROp(Value, "kelyra.shader.extract", {ResultType}, {Record},
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
        return IROp(Value, "kelyra.shader.construct", {ResultType}, Arguments,
                    {IRAttr("record", mlir::FlatSymbolRefAttr::get(
                                          &Context, ClassName(*Class->Node)))})
            ->getResult(0);
      }
      const auto &Function = CalledFunction(Value);
      if (const auto *Builtin = Annotation(Analysis, Function, "gpu_builtin")) {
        auto Name = AnnotationValue(*Builtin);
        if (Name.size() >= 2 && Name.front() == '"' && Name.back() == '"')
          Name = Name.substr(1, Name.size() - 2);
        if (Name.empty() ||
            !std::all_of(Name.begin(), Name.end(), [](unsigned char Character) {
              return std::isalnum(Character) || Character == '_';
            }))
          Fail(Value, "gpu_builtin name must be an identifier");
        return IROp(Value, "kelyra.shader.intrinsic", {ResultType}, Arguments,
                    {IRAttr("name", Builder.getStringAttr(Name))})
            ->getResult(0);
      }
      return IROp(Value, "kelyra.shader.call", {ResultType}, Arguments,
                  {IRAttr("callee", mlir::FlatSymbolRefAttr::get(
                                        &Context, FunctionName(Function)))})
          ->getResult(0);
    }
    case K::ast_cast: {
      auto Input = IRExpression(*Value.children.at(0));
      return IROp(Value, "kelyra.shader.cast", {ResultType}, {Input})
          ->getResult(0);
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
      IROp(Value, "kelyra.shader.return", {},
           {IRExpression(*Value.children.front())});
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
      auto ValueType = ExplicitType ? IRType(*ExplicitType)
                                    : IRType(Analysis.GetType(Value), Value);
      llvm::SmallVector<mlir::Value> Initial;
      for (const auto &Part : Value.children)
        if (Part->kind != K::ast_name && Part->kind != K::ast_type)
          Initial.push_back(IRExpression(*Part));
      if (Initial.size() > 1)
        Fail(Value, "shader let has multiple initializers");
      auto *Variable = IROp(
          Value, "kelyra.shader.var",
          {ir::ShaderRefType::get(&Context, ValueType)}, Initial,
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
        IROp(Value, "kelyra.shader.store", {},
             {Variables.at(Target->text), ValueToStore});
      else
        IROp(Value, "kelyra.shader.store_field", {},
             {Variables.at(Target->text), ValueToStore},
             {IRAttr("path", Builder.getArrayAttr(Path))});
      return;
    }
    case K::ast_expr_stmt:
      IROp(Value, "kelyra.shader.eval", {},
           {IRExpression(*Value.children.at(0))});
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
      IROp(*Class, "kelyra.shader.record", {}, {},
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
          ParameterTypes.push_back(IRType(RequiredType(*Part)));
          ParameterNames.push_back(Builder.getStringAttr(Part->text));
          InputDecorations.push_back(IRDecorations(*Part));
        }
      const auto ReturnType = IRType(RequiredType(*Function));
      auto *IROperation = IROp(
          *Function, "kelyra.shader.func", {}, {},
          {IRAttr("sym_name", Builder.getStringAttr(FunctionName(*Function))),
           IRAttr("function_type", mlir::TypeAttr::get(Builder.getFunctionType(
                                       ParameterTypes, {ReturnType}))),
           IRAttr("parameter_names", Builder.getArrayAttr(ParameterNames)),
           IRAttr("input_decorations", Builder.getArrayAttr(InputDecorations)),
           IRAttr("stage",
                  Builder.getStringAttr(Function == &Entry
                                            ? (Vertex ? "vertex" : "fragment")
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
          auto Argument = Block->addArgument(ParameterTypes[ParameterIndex],
                                             IRLocation(*Part));
          auto *Variable =
              IROp(*Part, "kelyra.shader.var",
                   {ir::ShaderRefType::get(&Context,
                                           ParameterTypes[ParameterIndex])},
                   {Argument},
                   {IRAttr("name", Builder.getStringAttr(Part->text)),
                    IRAttr("parameter", Builder.getBoolAttr(true))});
          Variables[Part->text] = Variable->getResult(0);
          ++ParameterIndex;
        }
      IRStatement(*Child(*Function, K::ast_block));
      if (Block->empty() ||
          !Block->back().hasTrait<mlir::OpTrait::IsTerminator>())
        Fail(*Function, "shader function must return a value");
      Builder.setInsertionPointToEnd(ShaderModule->getBody());
    }
    if (mlir::failed(mlir::verify(*ShaderModule)))
      throw std::runtime_error("generated Shader IR failed verification");
  }

  std::string IRTypeName(mlir::Type Value) const {
    if (Value.isF32())
      return "float";
    if (Value.isInteger(1))
      return "bool";
    if (Value.isInteger(32))
      return mlir::cast<mlir::IntegerType>(Value).isUnsigned() ? "uint" : "int";
    if (auto Record = mlir::dyn_cast<ir::ShaderRecordType>(Value))
      return Record.getName().getValue().str();
    throw std::runtime_error("unsupported shader IR type in target lowering");
  }

  std::string IRValue(mlir::Value Value) const {
    auto *Operation = Value.getDefiningOp();
    if (!Operation)
      throw std::runtime_error("shader IR block argument used outside a local");
    const auto Name = Operation->getName().getStringRef();
    const auto String = [&](llvm::StringRef Key) {
      return mlir::cast<mlir::StringAttr>(Operation->getAttr(Key))
          .getValue()
          .str();
    };
    if (Name == "kelyra.shader.literal")
      return String("text");
    if (Name == "kelyra.shader.var")
      return String("name");
    if (Name == "kelyra.shader.load")
      return IRValue(Operation->getOperand(0));
    if (Name == "kelyra.shader.unary")
      return "(" + String("opcode") + IRValue(Operation->getOperand(0)) + ")";
    if (Name == "kelyra.shader.binary")
      return "(" + IRValue(Operation->getOperand(0)) + " " + String("opcode") +
             " " + IRValue(Operation->getOperand(1)) + ")";
    if (Name == "kelyra.shader.cast")
      return IRTypeName(Value.getType()) + "(" +
             IRValue(Operation->getOperand(0)) + ")";
    if (Name == "kelyra.shader.extract")
      return IRValue(Operation->getOperand(0)) + "." + String("field");
    if (Name == "kelyra.shader.construct" || Name == "kelyra.shader.call" ||
        Name == "kelyra.shader.intrinsic") {
      std::string Result;
      if (Name == "kelyra.shader.intrinsic")
        Result = String("name");
      else {
        const auto Symbol =
            mlir::cast<mlir::FlatSymbolRefAttr>(Operation->getAttr(
                Name == "kelyra.shader.call" ? "callee" : "record"));
        Result = Symbol.getValue().str();
      }
      if (Name == "kelyra.shader.construct" && Hlsl)
        Result = "make_" + Result;
      Result += "(";
      for (unsigned Index = 0; Index < Operation->getNumOperands(); ++Index) {
        if (Index)
          Result += ", ";
        Result += IRValue(Operation->getOperand(Index));
      }
      return Result + ")";
    }
    throw std::runtime_error("shader IR expression has no target lowering: " +
                             Name.str());
  }

  std::string IRCondition(mlir::Value Value) const {
    auto Result = IRValue(Value);
    if (Result.size() >= 2 && Result.front() == '(' && Result.back() == ')')
      return Result;
    return "(" + Result + ")";
  }

  void IRBody(std::ostringstream &Out, mlir::Block &Block,
              unsigned Depth) const {
    const auto Prefix = Indent(Depth);
    for (auto &Operation : Block) {
      const auto Name = Operation.getName().getStringRef();
      if (Name == "kelyra.shader.var") {
        if (mlir::cast<mlir::BoolAttr>(Operation.getAttr("parameter"))
                .getValue())
          continue;
        const auto Variable =
            mlir::cast<ir::ShaderRefType>(Operation.getResult(0).getType());
        Out << Prefix << IRTypeName(Variable.getValueType()) << " "
            << mlir::cast<mlir::StringAttr>(Operation.getAttr("name"))
                   .getValue()
                   .str();
        if (Operation.getNumOperands())
          Out << " = " << IRValue(Operation.getOperand(0));
        Out << ";\n";
      } else if (Name == "kelyra.shader.store" ||
                 Name == "kelyra.shader.store_field") {
        Out << Prefix << IRValue(Operation.getOperand(0));
        if (Name == "kelyra.shader.store_field")
          for (auto Field :
               mlir::cast<mlir::ArrayAttr>(Operation.getAttr("path")))
            Out << "." << mlir::cast<mlir::StringAttr>(Field).getValue().str();
        Out << " = " << IRValue(Operation.getOperand(1)) << ";\n";
      } else if (Name == "kelyra.shader.eval") {
        Out << Prefix << IRValue(Operation.getOperand(0)) << ";\n";
      } else if (Name == "kelyra.shader.return") {
        Out << Prefix << "return " << IRValue(Operation.getOperand(0)) << ";\n";
      } else if (Name == "kelyra.shader.if") {
        Out << Prefix << "if " << IRCondition(Operation.getOperand(0)) << "\n"
            << Prefix << "{\n";
        IRBody(Out, Operation.getRegion(0).front(), Depth + 1);
        Out << Prefix << "}\n";
        auto &ElseBlock = Operation.getRegion(1).front();
        if (std::next(ElseBlock.begin()) != ElseBlock.end()) {
          Out << Prefix << "else\n" << Prefix << "{\n";
          IRBody(Out, ElseBlock, Depth + 1);
          Out << Prefix << "}\n";
        }
      } else if (Name == "kelyra.shader.while") {
        auto &ConditionBlock = Operation.getRegion(0).front();
        auto &Yield = ConditionBlock.back();
        Out << Prefix << "while " << IRCondition(Yield.getOperand(0)) << "\n"
            << Prefix << "{\n";
        IRBody(Out, Operation.getRegion(1).front(), Depth + 1);
        Out << Prefix << "}\n";
      } else if (Name == "kelyra.shader.break") {
        Out << Prefix << "break;\n";
      } else if (Name == "kelyra.shader.continue") {
        Out << Prefix << "continue;\n";
      } else if (Name == "kelyra.shader.yield") {
        continue;
      }
    }
  }

  std::string GenerateHlsl() const {
    std::ostringstream Out;
    for (const auto *Class : OrderedClasses) {
      Out << "struct " << ClassName(*Class) << " {\n";
      for (const auto &Part : Class->children)
        if (Part->kind == K::ast_field)
          Out << "  " << Type(RequiredType(*Part)) << " " << Part->text
              << ";\n";
      Out << "};\n";
      Out << ClassName(*Class) << " " << ConstructorName(*Class) << "(";
      bool First = true;
      for (const auto &Part : Class->children)
        if (Part->kind == K::ast_field) {
          if (!First)
            Out << ", ";
          Out << Type(RequiredType(*Part)) << " " << Part->text;
          First = false;
        }
      Out << ") {\n  " << ClassName(*Class) << " value;\n";
      for (const auto &Part : Class->children)
        if (Part->kind == K::ast_field)
          Out << "  value." << Part->text << " = " << Part->text << ";\n";
      Out << "  return value;\n}\n";
    }
    const auto &OutputClass = RequiredType(Entry);
    const auto *Output = ClassForType(OutputClass);
    if (!Output)
      Fail(OutputClass, "shader entry must return an interface data class");
    Out << "struct KelyraShaderOutput {\n";
    for (const auto &Field : Output->children) {
      if (Field->kind != K::ast_field)
        continue;
      const auto *Location = Annotation(Analysis, *Field, "location");
      const auto *Position = Annotation(Analysis, *Field, "position");
      if (Location && Position)
        Fail(*Field, "shader output cannot be both location and position");
      std::string Semantic;
      if (Position && Vertex) {
        if (InterfaceType(RequiredType(*Field)) != "float4")
          Fail(*Field, "vertex position must have four f32 components");
        Semantic = "SV_Position";
      } else if (Location) {
        Semantic = Vertex ? "TEXCOORD" : "SV_Target";
        Semantic += AnnotationValue(*Location);
      } else {
        Fail(*Field, "unsupported shader output interface field");
      }
      Out << "  " << InterfaceType(RequiredType(*Field)) << " " << Field->text
          << " : " << Semantic << ";\n";
    }
    Out << "};\n";
    for (const auto *Function : OrderedFunctions)
      Out << Signature(*Function) << ";\n";
    for (const auto *Function : OrderedFunctions) {
      Out << Signature(*Function) << "\n{\n";
      IRBody(Out, IRFunctions.at(Function)->getRegion(0).front(), 1);
      Out << "}\n";
    }
    Out << "KelyraShaderOutput main(";
    bool First = true;
    for (const auto &Parameter : Entry.children) {
      if (Parameter->kind != K::ast_parameter)
        continue;
      if (!First)
        Out << ", ";
      const auto *Location = Annotation(Analysis, *Parameter, "location");
      const auto *VertexIndex =
          Annotation(Analysis, *Parameter, "vertex_index");
      if (Location && VertexIndex)
        Fail(*Parameter,
             "shader input cannot be both location and vertex_index");
      std::string Semantic;
      if (VertexIndex && Vertex && RequiredType(*Parameter).text == "u32")
        Semantic = "SV_VertexID";
      else if (Location)
        Semantic = "TEXCOORD" + AnnotationValue(*Location);
      else
        Fail(*Parameter, "unsupported shader input interface parameter");
      Out << InterfaceType(RequiredType(*Parameter)) << " kelyra_in_"
          << Parameter->text << " : " << Semantic;
      First = false;
    }
    Out << ") {\n  " << Type(OutputClass) << " value = " << FunctionName(Entry)
        << "(";
    First = true;
    for (const auto &Parameter : Entry.children) {
      if (Parameter->kind != K::ast_parameter)
        continue;
      if (!First)
        Out << ", ";
      const auto Name = "kelyra_in_" + Parameter->text;
      Out << (Annotation(Analysis, *Parameter, "vertex_index")
                  ? Name
                  : FromInterface(RequiredType(*Parameter), Name));
      First = false;
    }
    Out << ");\n  KelyraShaderOutput result;\n";
    for (const auto &Field : Output->children)
      if (Field->kind == K::ast_field)
        Out << "  result." << Field->text << " = "
            << ToInterface(RequiredType(*Field), "value." + Field->text)
            << ";\n";
    Out << "  return result;\n}\n";
    return Out.str();
  }

public:
  GlslShader(const ModuleLoader &Loader, const Node &Entry, bool Vertex,
             bool Hlsl, const sema::Sema &Analysis)
      : Entry(Entry), Analysis(Analysis), Vertex(Vertex), Hlsl(Hlsl),
        Builder(&Context) {
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
                                Analysis.GetSymbol(*Declaration));
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

  std::string Generate() const {
    if (Hlsl)
      return GenerateHlsl();
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
      Out << Signature(*Function) << "\n{\n";
      IRBody(Out, IRFunctions.at(Function)->getRegion(0).front(), 1);
      Out << "}\n";
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
        if (Annotation(Analysis, *Field, "position")) {
          Out << "  gl_Position = " << Value << ";\n";
          // Kelyra clip space follows the Direct3D upward Y convention.
          // Vulkan's positive viewport height maps positive Y downward.
          Out << "  gl_Position.y = -gl_Position.y;\n";
        }
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
          Declaration->text == Option::ShaderEntry) {
        if (Entry)
          throw std::runtime_error(
              "shader entry name is overloaded; choose a unique entry name: " +
              Option::ShaderEntry.getValue());
        Entry = Declaration.get();
      }
    if (!Entry)
      throw std::runtime_error("shader entry function not found: " +
                               Option::ShaderEntry.getValue());
    const bool Vertex = Annotation(Analysis, *Entry, "vertex") != nullptr;
    const bool Fragment = Annotation(Analysis, *Entry, "fragment") != nullptr;
    if (Vertex == Fragment)
      Fail(*Entry, "shader entry needs exactly one @vertex or @fragment");
    const bool Hlsl = Option::EmitDxil;
    GlslShader Program(Loader, *Entry, Vertex, Hlsl, Analysis);
    if (Option::EmitShaderIr) {
      Program.DumpIR();
      return 0;
    }
    if (Option::EmitDxil || Option::EmitSpirv) {
      std::string Error;
      const bool Succeeded = Option::EmitDxil
                                 ? EmitDirectDxil(Program.GetIR(),
                                                  Option::OutputFile.getValue(),
                                                  Error)
                                 : EmitDirectSpirv(Program.GetIR(),
                                                   Option::OutputFile.getValue(),
                                                   Error);
      if (!Succeeded)
        throw std::runtime_error(Error);
      return 0;
    }
    throw std::runtime_error("select --emit-shader-ir, --emit-dxil, or "
                             "--emit-spirv");
  } catch (const std::exception &Error) {
    kerr() << Option::InputFile << ": error: " << Error.what() << '\n';
    return 1;
  }
}
