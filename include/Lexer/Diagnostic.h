//
// Created by zzm on 2026/9/19
// Part of RVision
//

#pragma once

#include <array>
#include <cstddef>
#include <iosfwd>
#include <string_view>

namespace kelyra::lex {
struct Location {
  std::string_view File;
  std::size_t Offset = 0;
  std::size_t Line = 1;
  std::size_t Column = 1;
  std::size_t Len = 0;

  std::size_t End() const { return Offset + Len; }
};

enum class DiagnosticKind {
  ExpectedExponentDigits,              // Numeric exponent has no digits.
  InvalidNumericSuffix,                // Numeric literal has an invalid suffix.
  ControlCharacterInString,            // String contains a control character.
  UnsupportedStringEscape,             // String escape is not supported.
  UnterminatedStringLiteral,           // String literal has no closing quote.
  UnterminatedBlockComment,            // Block comment has no closing marker.
  UnexpectedCharacter,                 // Character is not part of the language.
  ExpectedRightBracket,                // Expected ']'.
  ExpectedRightParen,                  // Expected ')'.
  ExpectedRightBrace,                  // Expected '}'.
  ExpectedSemicolon,                   // Expected ';'.
  ExpectedLeftBrace,                   // Expected '{'.
  ExpectedColon,                       // Expected ':'.
  ExpectedLeftParen,                   // Expected '('.
  ExpectedIdentifier,                  // Expected an identifier.
  SyntaxNestingLimitExceeded,          // Parser recursion limit was exceeded.
  AstNestingLimitExceeded,             // AST height limit was exceeded.
  ExpectedIntegerArrayLength,          // Array length must be an integer.
  ExpectedExpression,                  // Expected an expression.
  ComparisonChainsRequireParentheses,  // Comparison chain is ambiguous.
  ExpectedRightBraceBeforeDeclaration, // Block is missing '}' before a
                                       // declaration.
  ExpectedTypeAnnotationOrInitializer, // Let needs a type or initializer.
  InvalidAssignmentTarget,             // Assignment target is not writable.
  ExpectedDeclaration,                 // Expected a top-level declaration.
  ExpectedEndOfExpression,             // Tokens remain after the expression.
  DuplicateFunction,                   // Function name is already declared.
  DuplicateParameter,                  // Parameter name is already declared.
  DuplicateAnnotation,          // Annotation is repeated or already declared.
  DuplicateAnnotationParameter, // Annotation parameter is repeated.
  UnknownAnnotation,            // Annotation name cannot be resolved.
  InvalidAnnotation,            // Annotation declaration or arguments invalid.
  InvalidAnnotationTarget,      // Annotation cannot target this declaration.
  MetaValueInRuntimeExpression, // Reflection metadata cannot escape.
  InvalidWhenCondition,         // When requires a compile-time boolean.
  UnsupportedType,              // Type is not supported by IR generation.
  UnsupportedDeclaration, // Declaration is not supported by IR generation.
  UnsupportedStatement,   // Statement is not supported by IR generation.
  InvalidInlineAssembly,  // Inline assembly constraints are invalid.
  UnsupportedExpression,  // Expression is not supported by IR generation.
  UnknownName,            // Name cannot be resolved.
  AmbiguousName,          // Name is provided by multiple wildcard imports.
  InvalidIntegerLiteral,  // Integer literal does not fit its target type.
  MissingReturn,          // Function does not return a value.
  TypeMismatch,           // Expression type does not match its context.
  MissingEntrypoint,      // Executable has no main function.
  InvalidEntrypoint,      // Main must have the executable ABI signature.
  DuplicateModule,        // Module name is already defined.
  UnknownModule,          // Imported module cannot be resolved.
  PrivateDeclaration,     // Declaration is private to another module.
  Count,                  // Number of diagnostic kinds.
};

struct Diagnostic {
  DiagnosticKind Kind;
  Location Loc;
};

struct DiagnosticInfo {
  std::string_view Code;
  std::string_view Msg;
};

const DiagnosticInfo &GetDiagnosticInfo(DiagnosticKind Kind);
std::ostream &operator<<(std::ostream &OS, const Diagnostic &Diagnostic);
} // namespace kelyra::lex
