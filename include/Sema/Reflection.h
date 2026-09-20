#pragma once

#include "Lexer/Lexer.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kelyra::sema {
using MetaId = std::uint32_t;
inline constexpr MetaId InvalidMetaId = std::numeric_limits<MetaId>::max();

enum class AnnotationValueKind { Bool, Integer, Float, String, Symbol, Type };

struct AnnotationValue {
  AnnotationValueKind Kind;
  std::string Text;
  MetaId Reference = InvalidMetaId;
};

struct AnnotationArgument {
  std::string Name;
  AnnotationValue Value;
};

struct AnnotationInstance {
  std::string Name;
  std::vector<AnnotationArgument> Arguments;
};

enum class MetaKind {
  Module,
  Function,
  Struct,
  Field,
  Parameter,
  Type,
  Annotation,
  AnnotationParameter,
};

enum class MetaTypeKind { None, Builtin, Pointer, Array, Record };

struct MetaDeclaration {
  MetaId Id = InvalidMetaId;
  MetaKind Kind = MetaKind::Module;
  std::string Name;
  std::string QualifiedName;
  MetaId Module = InvalidMetaId;
  bool Public = false;
  lex::Location Loc;
  std::vector<MetaId> Children;
  MetaId Type = InvalidMetaId;
  std::string Symbol;
  std::vector<AnnotationInstance> Annotations;
  MetaTypeKind TypeKind = MetaTypeKind::None;
  unsigned BitWidth = 0;
  unsigned PointerDepth = 0;
  std::vector<std::uint64_t> Dimensions;
};

class Sema;

class ReflectionDatabase {
  std::vector<MetaDeclaration> Records;
  std::unordered_map<const lex::Node *, MetaId> Nodes;

  friend class Sema;
  void Clear();
  MetaId Add(const lex::Node *Node, MetaDeclaration Declaration);
  void SetAnnotations(const lex::Node &Node,
                      const std::vector<AnnotationInstance> &Annotations);

public:
  const MetaDeclaration &Get(MetaId Id) const { return Records.at(Id); }
  std::optional<MetaId> GetId(const lex::Node &Node) const;
  std::optional<MetaId> Find(std::string_view QualifiedName,
                             MetaKind Kind) const;
  const std::vector<MetaDeclaration> &GetRecords() const { return Records; }
};
} // namespace kelyra::sema
