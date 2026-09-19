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
    DiagnosticInfo{"K0002", "expected 'fn' or 'struct'"},
    DiagnosticInfo{"K0002", "expected end of expression"},
    DiagnosticInfo{"K0004", "duplicate function"},
    DiagnosticInfo{"K0004", "duplicate parameter"},
    DiagnosticInfo{"K0004", "unknown builtin type"},
    DiagnosticInfo{"K0004", "declaration is not supported by IR generation"},
    DiagnosticInfo{"K0004", "only a single return statement is supported"},
    DiagnosticInfo{"K0004", "expression is not supported by IR generation"},
    DiagnosticInfo{"K0004", "unknown name"},
    DiagnosticInfo{"K0004", "integer literal does not fit its type"},
    DiagnosticInfo{"K0004", "function must return a value"},
    DiagnosticInfo{"K0004", "type mismatch"},
    DiagnosticInfo{"K0005", "executable requires a main function"},
    DiagnosticInfo{"K0005", "main must have signature 'fn main() -> i32'"},
    DiagnosticInfo{"K0004", "duplicate module"},
    DiagnosticInfo{"K0004", "unknown imported module"},
    DiagnosticInfo{"K0004", "declaration is private to another module"},
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
