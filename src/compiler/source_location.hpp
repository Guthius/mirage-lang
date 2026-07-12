#pragma once

#include <string_view>

struct SourceLocation {
    std::string_view filename;
    size_t line = 1;
    size_t column = 1;
    size_t offset = 0;
    size_t length = 1;
};
