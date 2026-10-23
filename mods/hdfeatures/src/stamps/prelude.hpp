#pragma once

// Include first in every translation unit that also includes game headers (both features: the cave's
// files include it as "stamps/prelude.hpp").
//
// The forced include (global.h -> dolphin/types.h) defines `nullptr` as NULL when __cplusplus
// reads below C++11, which it does under MSVC without /Zc:__cplusplus; NULL is then also
// redefined. Standard and fmt headers parsed after that break (std::exchange(p, nullptr) becomes
// std::exchange(p, 0)), so every library header the mod uses is pulled in here, before any game
// header. Template bodies keep the tokens they were parsed with.

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <mods/svc/hook.hpp>
#include <mods/svc/log.hpp>
