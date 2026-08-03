#pragma once

// ADL-safe rendering of standard containers, maps, and std::optional for
// logging and debugging (opcua namespace).
//
// Wrapping a value in opcua::AsList / AsDict / AsOpt yields a lightweight,
// non-owning wrapper owned by namespace opcua. Both `stream << opcua::AsList(v)`
// and `std::format("{}", opcua::AsList(v))` are found by ADL, with no global
// operator<< overloads on std:: types. The stream operator is templated on the
// stream type and delegates to std::format, so one definition serves both
// std::ostream and boost::log::formatting_ostream. Elements must themselves be
// std::formattable.
//
//   LOG_INFO(logger) << "ids=" << opcua::AsList(node_ids);
//   std::string s = ToString(opcua::AsDict(name_to_value));

#include <concepts>
#include <format>
#include <optional>
#include <string>

namespace opcua {

// Common base so a single constrained operator<< serves every wrapper.
struct DumpTag {};

// Renders a range as `[a, b, c]`.
template <class R>
struct ListDump : DumpTag {
  const R& range;
};

// Renders an associative range as `{k: v, k2: v2}`.
template <class M>
struct DictDump : DumpTag {
  const M& map;
};

// Renders an optional as its value or `nullopt`.
template <class T>
struct OptDump : DumpTag {
  const std::optional<T>& opt;
};

// Factories. The wrapper borrows its argument for the enclosing full-expression.
template <class R>
[[nodiscard]] ListDump<R> AsList(const R& range) {
  return {{}, range};
}

template <class M>
[[nodiscard]] DictDump<M> AsDict(const M& map) {
  return {{}, map};
}

template <class T>
[[nodiscard]] OptDump<T> AsOpt(const std::optional<T>& opt) {
  return {{}, opt};
}

namespace internal {

// Shared std::formatter::parse for the dump wrappers: accept only the empty
// "{}" spec, reject any non-empty spec with std::format_error.
struct DumpParse {
  constexpr std::format_parse_context::iterator parse(
      std::format_parse_context& ctx) const {
    const std::format_parse_context::iterator it = ctx.begin();
    if (it != ctx.end() && *it != '}')
      throw std::format_error{"dump wrapper takes no format spec"};
    return it;
  }
};

}  // namespace internal

// Stream operator for any dump wrapper, delegating to its std::formatter.
// Templated on the stream type (constrained to accept a std::string). The
// return type is the stream expression's own type so a derived stream such as
// std::ostringstream binds correctly.
template <class StreamT, class W>
  requires std::derived_from<W, DumpTag>
auto operator<<(StreamT& stream, const W& wrapper)
    -> decltype(stream << std::string{}) {
  return stream << std::format("{}", wrapper);
}

}  // namespace opcua

template <class R>
struct std::formatter<opcua::ListDump<R>> : opcua::internal::DumpParse {
  template <class FormatContext>
  auto format(const opcua::ListDump<R>& w, FormatContext& ctx) const {
    typename FormatContext::iterator out = ctx.out();
    *out++ = '[';
    bool first = true;
    for (const auto& element : w.range) {
      if (!first)
        out = std::format_to(out, ", ");
      out = std::format_to(out, "{}", element);
      first = false;
    }
    *out++ = ']';
    return out;
  }
};

template <class M>
struct std::formatter<opcua::DictDump<M>> : opcua::internal::DumpParse {
  template <class FormatContext>
  auto format(const opcua::DictDump<M>& w, FormatContext& ctx) const {
    typename FormatContext::iterator out = ctx.out();
    *out++ = '{';
    bool first = true;
    for (const auto& [key, value] : w.map) {
      if (!first)
        out = std::format_to(out, ", ");
      out = std::format_to(out, "{}: {}", key, value);
      first = false;
    }
    *out++ = '}';
    return out;
  }
};

template <class T>
struct std::formatter<opcua::OptDump<T>> : opcua::internal::DumpParse {
  template <class FormatContext>
  auto format(const opcua::OptDump<T>& w, FormatContext& ctx) const {
    if (w.opt.has_value())
      return std::format_to(ctx.out(), "{}", *w.opt);
    return std::format_to(ctx.out(), "nullopt");
  }
};
