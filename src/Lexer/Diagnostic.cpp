//
// Created by zzm on 2026/9/19
// Part of RVision
//
#include "Lexer/Diagnostic.h"

#include <ostream>

using namespace kelyra::lex;

constexpr std::array DiagnosticInfos = {
    DiagnosticInfo{"K0001", "expected digits in exponent"},
    DiagnosticInfo{"K0001", "invalid numeric suffix"},
    DiagnosticInfo{"K0001", "control character in string"},
    DiagnosticInfo{"K0001", "unsupported string escape"},
    DiagnosticInfo{"K0001", "unterminated string literal"},
    DiagnosticInfo{"K0001", "unterminated block comment"},
    DiagnosticInfo{"K0001", "unexpected character"},
    DiagnosticInfo{"K0002", "expected ']'"},
    DiagnosticInfo{"K0002", "expected '>'"},
    DiagnosticInfo{"K0002", "expected ')'"},
    DiagnosticInfo{"K0002", "expected '}'"},
    DiagnosticInfo{"K0002", "expected ';'"},
    DiagnosticInfo{"K0002", "expected '{'"},
    DiagnosticInfo{"K0002", "expected ':'"},
    DiagnosticInfo{"K0002", "expected '('"},
    DiagnosticInfo{"K0002", "expected identifier"},
    DiagnosticInfo{"K0003", "syntax nesting limit exceeded"},
    DiagnosticInfo{"K0003", "AST nesting limit exceeded"},
    DiagnosticInfo{"K0002", "expected integer array length"},
    DiagnosticInfo{"K0002", "expected expression"},
    DiagnosticInfo{"K0002", "comparison chains require parentheses"},
    DiagnosticInfo{"K0002", "expected '}' before declaration"},
    DiagnosticInfo{"K0002", "expected type annotation or initializer"},
    DiagnosticInfo{"K0002", "invalid assignment target"},
    DiagnosticInfo{"K0002", "expected 'fn', 'class', or 'annotation'"},
    DiagnosticInfo{"K0002", "expected end of expression"},
    DiagnosticInfo{"K0004", "duplicate function"},
    DiagnosticInfo{"K0004", "duplicate parameter"},
    DiagnosticInfo{"K0004", "duplicate annotation"},
    DiagnosticInfo{"K0004", "duplicate annotation parameter"},
    DiagnosticInfo{"K0004", "unknown annotation"},
    DiagnosticInfo{"K0004", "invalid annotation declaration or argument"},
    DiagnosticInfo{"K0004", "annotation is not valid on this declaration"},
    DiagnosticInfo{"K0004", "invalid @extern function declaration"},
    DiagnosticInfo{"K0004",
                   "reflection metadata is only available at compile time"},
    DiagnosticInfo{"K0004", "when condition is not a compile-time boolean"},
    DiagnosticInfo{"K0004", "unknown builtin type"},
    DiagnosticInfo{"K0004", "declaration is not supported by IR generation"},
    DiagnosticInfo{"K0004", "only a single return statement is supported"},
    DiagnosticInfo{"K0004", "invalid inline assembly constraint"},
    DiagnosticInfo{"K0004", "expression is not supported by IR generation"},
    DiagnosticInfo{"K0004", "unknown name"},
    DiagnosticInfo{"K0004", "ambiguous name from wildcard imports"},
    DiagnosticInfo{"K0004", "integer literal does not fit its type"},
    DiagnosticInfo{"K0004", "function must return a value"},
    DiagnosticInfo{"K0004", "type mismatch"},
    DiagnosticInfo{"K0005", "executable requires one @main function"},
    DiagnosticInfo{"K0005",
                   "@main requires a body and signature 'fn name() -> i32'"},
    DiagnosticInfo{"K0004", "duplicate module"},
    DiagnosticInfo{"K0004", "unknown imported module"},
    DiagnosticInfo{"K0004", "declaration is private to another module"},
    DiagnosticInfo{
        "K0004",
        "invalid class or duplicate member; at most one init is allowed"},
    DiagnosticInfo{"K0004", "initialize every field once in declaration order "
                            "at the start of init"},
    DiagnosticInfo{"K0004", "cannot read an uninitialized field or use this "
                            "before initialization completes"},
    DiagnosticInfo{
        "K0004",
        "class values require direct construction; copying, assignment, arrays "
        "and value parameters/returns are unsupported"},
    DiagnosticInfo{"K0004",
                   "init and deinit cannot be called as ordinary methods"},
    DiagnosticInfo{
        "K0004",
        "recursive class value layout; use a pointer to break the cycle"},
    DiagnosticInfo{"K0004",
                   "field class has no default constructor; declare init "
                   "explicitly"},
};

static_assert(DiagnosticInfos.size() ==
              static_cast<std::size_t>(DiagnosticKind::Count));

const DiagnosticInfo &kelyra::lex::GetDiagnosticInfo(DiagnosticKind Kind) {
  return DiagnosticInfos[static_cast<std::size_t>(Kind)];
}

std::ostream &kelyra::lex::operator<<(std::ostream &OS,
                                      const Diagnostic &Diagnostic) {
  const auto &Info = GetDiagnosticInfo(Diagnostic.Kind);
  return OS << Diagnostic.Loc.File << ':' << Diagnostic.Loc.Line << ':'
            << Diagnostic.Loc.Column << ": error: " << Info.Msg;
}
