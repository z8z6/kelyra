//
// Created by zzm on 2026/9/18
// Part of RVision
//

#pragma once

#include <string>

namespace kelyra::lex {
enum class TokenKind {
  illegal,            // illegal
  name,               // identifier
  number,             // numeric literal
  string,             // string literal
  keyword_let,        // let
  keyword_var,        // var
  keyword_mut,        // mut
  keyword_fn,         // fn
  keyword_struct,     // struct
  keyword_if,         // if
  keyword_else,       // else
  keyword_while,      // while
  keyword_return,     // return
  keyword_break,      // break
  keyword_continue,   // continue
  keyword_true,       // true
  keyword_false,      // false
  keyword_meta,       // meta
  keyword_when,       // when
  keyword_parallel,   // parallel
  keyword_extern,     // extern
  keyword_defer,      // defer
  keyword_module,     // module
  keyword_import,     // import
  keyword_pub,        // pub
  op_arrow,           // ->
  op_assign,          // =
  op_add,             // +
  op_subtract,        // -
  op_multiply,        // *
  op_divide,          // /
  op_remainder,       // %
  op_equal,           // ==
  op_not_equal,       // !=
  op_less,            // <
  op_less_equal,      // <=
  op_greater,         // >
  op_greater_equal,   // >=
  op_and,             // &&
  op_or,              // ||
  op_not,             // !
  punc_left_paren,    // (
  punc_right_paren,   // )
  punc_left_brace,    // {
  punc_right_brace,   // }
  punc_left_bracket,  // [
  punc_right_bracket, // ]
  punc_comma,         // ,
  punc_colon,         // :
  punc_semicolon,     // ;
  punc_dot,           // .
  punc_at,            // @
  comment,            // //... or /*...*/
  end,                // end of input
  ast_module,         // AST module
  ast_module_decl,    // AST module declaration
  ast_import,         // AST import declaration
  ast_public,         // AST public visibility marker
  ast_annotation,     // AST @annotation
  ast_function,       // AST function declaration
  ast_parameter,      // AST function parameter
  ast_struct,         // AST struct declaration
  ast_field,          // AST struct field
  ast_type,           // AST named type
  ast_pointer_type,   // AST type*
  ast_array_type,     // AST type[length]
  ast_block,          // AST {...}
  ast_let,            // AST let declaration
  ast_assign,         // AST assignment
  ast_return,         // AST return statement
  ast_if,             // AST if statement
  ast_while,          // AST while statement
  ast_break,          // AST break statement
  ast_continue,       // AST continue statement
  ast_expr_stmt,      // AST expression statement
  ast_literal,        // AST literal expression
  ast_name,           // AST name expression
  ast_unary,          // AST unary expression
  ast_binary,         // AST binary expression
  ast_call,           // AST call expression
  ast_index,          // AST index expression
  ast_member,         // AST member expression
  ast_group           // AST parenthesized expression
};

std::string GetTokenName(TokenKind kind);
inline std::ostream &operator<<(std::ostream &os, TokenKind kind) {
  return os << GetTokenName(kind);
}
} // namespace kelyra::lex
