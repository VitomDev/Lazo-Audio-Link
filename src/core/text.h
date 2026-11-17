#pragma once

#include <string>
#include <string_view>

namespace lazo {

std::string ToUtf8(std::wstring_view text);
std::wstring FromUtf8(std::string_view text);

}  // namespace lazo
