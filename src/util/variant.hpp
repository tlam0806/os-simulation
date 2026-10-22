// Small helpers for std::variant.
#pragma once

#include <cstdlib>
#include <type_traits>
#include <variant>

namespace util {

// True if T is one of Types.
template <typename T, typename... Types>
inline constexpr bool contains = (std::is_same_v<T, Types> || ...);

namespace detail {

template <std::size_t index, typename... ResultTypes, typename... ArgTypes>
std::variant<ResultTypes...> convert_variant(const std::variant<ArgTypes...>& var) {
    if constexpr (index == sizeof...(ResultTypes)) {
        std::abort();   // var holds a type that ResultTypes doesn't have
    } else {
        using type = std::variant_alternative_t<index, std::variant<ResultTypes...>>;
        if constexpr (contains<type, ArgTypes...>) {
            if (const auto* value = std::get_if<type>(&var)) return *value;
        }
        return convert_variant<index + 1, ResultTypes...>(var);
    }
}

}  // namespace detail

// Converts a variant to another variant type. Whatever `var` currently holds
// must also be one of ResultTypes; otherwise the program aborts.
template <typename... ResultTypes, typename... ArgTypes>
std::variant<ResultTypes...> convert_variant(const std::variant<ArgTypes...>& var) {
    return detail::convert_variant<0, ResultTypes...>(var);
}

}  // namespace util
