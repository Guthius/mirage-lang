#pragma once

#include <string_view>

struct SourceLocation {
    std::string_view Filename;
    size_t Line = 1;
    size_t Column = 1;
    size_t Offset = 0;
};
