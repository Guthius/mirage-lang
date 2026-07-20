#pragma once

#include "source_location.hpp"

#include <cstdint>
#include <string>

enum class TokenKind : uint8_t {
    // Literals
    IntLiteral,
    FloatLiteral,
    StringLiteral,
    CharLiteral,
    Identifier,

    // Keywords
    KwConst,
    KwMut,
    KwFn,
    KwType,
    KwTrait,
    KwStruct,
    KwImpl,
    KwFor,
    KwPub,
    KwContinue,
    KwBreak,
    KwReturn,
    KwReturnErr,
    KwReturnOk,
    KwIf,
    KwElse,
    KwWhile,
    KwIn,
    KwImport,
    KwImportBin,
    KwNamespace,
    KwAsm,
    KwMacro,
    KwTry,
    KwWhen,
    KwNil,
    KwTrue,
    KwFalse,
    KwSizeOf,
    KwOffsetOf,
    KwStackAlloc,
    KwCast,
    KwEnum,
    KwIota,
    KwDefault,
    KwUndefined,
    KwLen,
    KwMatch,
    KwUnion,
    KwSwitch,
    KwDefer,

    // Primitive type keywords
    KwU8,
    KwU16,
    KwU32,
    KwU64,
    KwI8,
    KwI16,
    KwI32,
    KwI64,
    KwF32,
    KwF64,
    KwUSize,
    KwBool,
    KwByte,
    KwError,
    KwAnyptr,

    // Operators
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Ampersand,
    Pipe,
    Caret,
    Tilde,
    ShiftLeft,
    ShiftRight,
    EqualEqual,
    BangEqual,
    Less,
    Greater,
    LessEqual,
    GreaterEqual,
    AmpAmp,
    PipePipe,
    Bang,
    Equal,
    PlusEqual,
    MinusEqual,
    StarEqual,
    SlashEqual,
    AmpEqual,
    PipeEqual,
    CaretEqual,
    ShiftLeftEqual,
    ShiftRightEqual,
    PlusPlus,
    MinusMinus,
    ColonEqual,
    Arrow,
    DotDot,
    DotDotDot,
    Question,
    Colon,

    // Punctuation
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    Comma,
    Dot,
    Semicolon,

    // Special
    AsmBlock,
    Eof,
};

auto token_kind_name(TokenKind kind) -> const char *;

struct Token {
    TokenKind kind;
    std::string lexeme;
    SourceLocation location;
};
