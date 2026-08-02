#pragma once
// ScriptJson.h — Lua table <-> nlohmann::json conversion.
//
// The bridge between script-authored parameters and the generator pipeline's
// schema-validated JSON (AI_ASSET_PIPELINE_PLAN.md Phase 2). Kept in its own
// header, free of GLFW/ECS includes, so the conversion rules can be tested
// directly -- they are the fiddly part of the whole binding layer.
//
// Two asymmetries are worth knowing about, both forced by Lua itself:
//
//   * Lua has ONE table type for both arrays and maps, so `toJson` decides by
//     inspecting the keys: a table whose keys are exactly the integers 1..n is
//     an array, anything else is an object. An EMPTY table is ambiguous and
//     becomes an object -- the common case by far in this codebase (a params
//     block), and callers who need an empty array can say json::array()
//     themselves.
//   * Lua numbers are doubles. An integer-valued double is written as a JSON
//     integer so schema validation and `dump()` produce 3 rather than 3.0.

#define SOL_ALL_SAFETIES_ON 1
#include "json.hpp"
#include <sol/sol.hpp>

#include <cmath>
#include <string>

namespace scriptjson {

// Converts a Lua value to JSON. Unsupported types (functions, userdata,
// threads) become null rather than throwing -- a script passing a stray
// function in a params table should get a validation error naming the field,
// not a Lua exception from deep inside the converter.
inline nlohmann::json toJson(const sol::object &value, int depth = 0) {
  // Bounded so a self-referencing table cannot recurse forever.
  if (depth > 32)
    return nullptr;

  switch (value.get_type()) {
  case sol::type::boolean:
    return value.as<bool>();
  case sol::type::string:
    return value.as<std::string>();
  case sol::type::number: {
    const double d = value.as<double>();
    double intPart = 0.0;
    if (std::modf(d, &intPart) == 0.0 && std::fabs(d) < 9.0e15)
      return static_cast<long long>(intPart);
    return d;
  }
  case sol::type::table: {
    const sol::table t = value.as<sol::table>();

    // Array test: keys are exactly 1..n with no gaps and nothing else.
    size_t count = 0;
    bool arrayLike = true;
    for (const auto &kv : t) {
      ++count;
      if (kv.first.get_type() != sol::type::number) {
        arrayLike = false;
        break;
      }
      const double k = kv.first.as<double>();
      double ip = 0.0;
      if (std::modf(k, &ip) != 0.0 || k < 1.0) {
        arrayLike = false;
        break;
      }
    }
    if (arrayLike && count > 0) {
      // Confirm density: every index 1..count must exist, or {[1]=a,[5]=b}
      // would silently lose entries.
      for (size_t i = 1; i <= count; ++i) {
        if (!t[i].valid()) {
          arrayLike = false;
          break;
        }
      }
    }

    if (arrayLike && count > 0) {
      nlohmann::json out = nlohmann::json::array();
      for (size_t i = 1; i <= count; ++i)
        out.push_back(toJson(t[i].get<sol::object>(), depth + 1));
      return out;
    }

    nlohmann::json out = nlohmann::json::object();
    for (const auto &kv : t) {
      std::string key;
      if (kv.first.get_type() == sol::type::string)
        key = kv.first.as<std::string>();
      else if (kv.first.get_type() == sol::type::number)
        key = std::to_string(kv.first.as<long long>());
      else
        continue; // non-representable key
      out[key] = toJson(kv.second, depth + 1);
    }
    return out;
  }
  case sol::type::nil:
  case sol::type::none:
  default:
    return nullptr;
  }
}

// Converts JSON back to a Lua value. JSON arrays become 1-based Lua tables.
inline sol::object toLua(sol::state_view lua, const nlohmann::json &value,
                         int depth = 0) {
  if (depth > 32)
    return sol::nil;

  switch (value.type()) {
  case nlohmann::json::value_t::boolean:
    return sol::make_object(lua, value.get<bool>());
  case nlohmann::json::value_t::string:
    return sol::make_object(lua, value.get<std::string>());
  case nlohmann::json::value_t::number_integer:
  case nlohmann::json::value_t::number_unsigned:
    return sol::make_object(lua, value.get<long long>());
  case nlohmann::json::value_t::number_float:
    return sol::make_object(lua, value.get<double>());
  case nlohmann::json::value_t::array: {
    sol::table t = lua.create_table(static_cast<int>(value.size()), 0);
    int i = 1;
    for (const auto &el : value)
      t[i++] = toLua(lua, el, depth + 1);
    return t;
  }
  case nlohmann::json::value_t::object: {
    sol::table t = lua.create_table(0, static_cast<int>(value.size()));
    for (auto it = value.begin(); it != value.end(); ++it)
      t[it.key()] = toLua(lua, it.value(), depth + 1);
    return t;
  }
  case nlohmann::json::value_t::null:
  default:
    return sol::nil;
  }
}

} // namespace scriptjson
