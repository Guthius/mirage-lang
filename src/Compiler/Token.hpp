#pragma once

#include <Compiler/SourceLocation.hpp>

#include <cstdint>
#include <string>

enum class TokenKind : uint8_t {
    // Literals
    int_literal,
    float_literal,
    string_literal,
    identifier,

    // Keywords
    kw_const,
    kw_mut,
    kw_fn,
    kw_type,
    kw_trait,
    kw_struct,
    kw_impl,
    kw_for,
    kw_pub,
    kw_return,
    kw_if,
    kw_else,
    kw_while,
    kw_in,
    kw_import,
    kw_namespace,
    kw_asm,
    kw_macro,
    kw_try,
    kw_when,
    kw_nil,
    kw_true,
    kw_false,
    kw_sizeof,
    kw_offsetof,
    kw_cast,
    kw_enum,
    kw_packed,
    kw_iota,
    kw_default,
    kw_undefined,
    kw_len,

    // Primitive type keywords
    kw_u8,
    kw_u16,
    kw_u32,
    kw_u64,
    kw_i8,
    kw_i16,
    kw_i32,
    kw_i64,
    kw_f32,
    kw_f64,
    kw_usize,
    kw_bool,
    kw_byte,
    kw_error,
    kw_anyptr,

    // Operators
    plus,
    minus,
    star,
    slash,
    percent,
    ampersand,
    pipe,
    caret,
    tilde,
    shift_left,
    shift_right,
    equal_equal,
    bang_equal,
    less,
    greater,
    less_equal,
    greater_equal,
    amp_amp,
    pipe_pipe,
    bang,
    equal,
    plus_equal,
    minus_equal,
    star_equal,
    slash_equal,
    amp_equal,
    pipe_equal,
    caret_equal,
    shift_left_equal,
    shift_right_equal,
    plus_plus,
    minus_minus,
    colon_equal,
    arrow,
    dot_dot,
    question,
    colon,

    // Punctuation
    lparen,
    rparen,
    lbrace,
    rbrace,
    lbracket,
    rbracket,
    comma,
    dot,
    semicolon,

    // Special
    asm_block,
    eof,
};

auto TokenKindName(TokenKind kind) -> const char *;

struct Token {
    TokenKind Kind;
    std::string Lexeme;
    SourceLocation Location;
};
