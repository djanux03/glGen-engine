#include "GeneratorSchema.h"

#include <algorithm>
#include <cmath>

namespace gen {
namespace {

using json = nlohmann::json;

const json kEmptyArray = json::array();

std::string fmtNum(double v) {
  // Compact, locale-independent, and stable enough for warning text.
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.4g", v);
  return buf;
}

} // namespace

// Every property is assembled field by field rather than with nlohmann's
// nested brace-init: MSVC rejects `json{{"a", 1}, {"b", 2}}` in this context
// (C3321), since the inner braces are ambiguous between an initializer list
// and a json object.
SchemaBuilder::SchemaBuilder() {
  mSchema["type"] = "object";
  mSchema["properties"] = json::object();
}

SchemaBuilder &SchemaBuilder::number(const std::string &name,
                                     float defaultValue, float min, float max,
                                     const std::string &description) {
  json spec;
  spec["type"] = "number";
  spec["default"] = defaultValue;
  spec["minimum"] = min;
  spec["maximum"] = max;
  spec["description"] = description;
  mSchema["properties"][name] = spec;
  return *this;
}

SchemaBuilder &SchemaBuilder::integer(const std::string &name, int defaultValue,
                                      int min, int max,
                                      const std::string &description) {
  json spec;
  spec["type"] = "integer";
  spec["default"] = defaultValue;
  spec["minimum"] = min;
  spec["maximum"] = max;
  spec["description"] = description;
  mSchema["properties"][name] = spec;
  return *this;
}

SchemaBuilder &SchemaBuilder::boolean(const std::string &name, bool defaultValue,
                                      const std::string &description) {
  json spec;
  spec["type"] = "boolean";
  spec["default"] = defaultValue;
  spec["description"] = description;
  mSchema["properties"][name] = spec;
  return *this;
}

SchemaBuilder &SchemaBuilder::enumString(const std::string &name,
                                         const std::string &defaultValue,
                                         std::vector<std::string> values,
                                         const std::string &description) {
  json spec;
  spec["type"] = "string";
  spec["default"] = defaultValue;
  spec["enum"] = values;
  spec["description"] = description;
  mSchema["properties"][name] = spec;
  return *this;
}

SchemaBuilder &SchemaBuilder::color(const std::string &name,
                                    glm::vec3 defaultValue,
                                    const std::string &description) {
  json items;
  items["type"] = "number";
  items["minimum"] = 0.0;
  items["maximum"] = 1.0;

  json spec;
  spec["type"] = "array";
  spec["items"] = items;
  spec["minItems"] = 3;
  spec["maxItems"] = 3;
  spec["default"] = json::array({defaultValue.x, defaultValue.y, defaultValue.z});
  spec["description"] = description;
  mSchema["properties"][name] = spec;
  return *this;
}

SchemaBuilder &SchemaBuilder::objectArray(const std::string &name,
                                          const std::string &description) {
  json spec;
  spec["type"] = "array";
  spec["default"] = json::array();
  spec["description"] = description;
  mSchema["properties"][name] = spec;
  return *this;
}

SchemaBuilder &SchemaBuilder::string(const std::string &name,
                                     const std::string &defaultValue,
                                     const std::string &description) {
  json spec;
  spec["type"] = "string";
  spec["default"] = defaultValue;
  spec["description"] = description;
  mSchema["properties"][name] = spec;
  return *this;
}

bool validateParams(const json &schema, json &params,
                    std::vector<std::string> &warnings, std::string &error) {
  if (!params.is_object()) {
    // Treat a missing/null params block as "all defaults" -- a generator whose
    // parameters are all optional should be callable with nothing at all.
    if (params.is_null())
      params = json::object();
    else {
      error = "params must be a JSON object";
      return false;
    }
  }
  if (!schema.contains("properties") || !schema["properties"].is_object())
    return true;

  const json &props = schema["properties"];

  for (auto it = params.begin(); it != params.end(); ++it) {
    if (!props.contains(it.key()))
      warnings.push_back("unknown parameter '" + it.key() + "' ignored");
  }

  for (auto it = props.begin(); it != props.end(); ++it) {
    const std::string &name = it.key();
    const json &spec = it.value();
    const std::string type = spec.value("type", "");

    if (!params.contains(name) || params[name].is_null()) {
      if (spec.contains("default"))
        params[name] = spec["default"];
      continue;
    }
    json &value = params[name];

    if (type == "number" || type == "integer") {
      if (!value.is_number()) {
        error = "parameter '" + name + "' must be a number";
        return false;
      }
      double v = value.get<double>();
      const double lo = spec.value("minimum", -std::numeric_limits<double>::max());
      const double hi = spec.value("maximum", std::numeric_limits<double>::max());
      if (v < lo || v > hi) {
        const double clamped = std::clamp(v, lo, hi);
        warnings.push_back("parameter '" + name + "' " + fmtNum(v) +
                           " out of range [" + fmtNum(lo) + ", " + fmtNum(hi) +
                           "], clamped to " + fmtNum(clamped));
        v = clamped;
      }
      value = (type == "integer") ? json(static_cast<long long>(std::llround(v)))
                                  : json(v);
    } else if (type == "boolean") {
      if (!value.is_boolean()) {
        error = "parameter '" + name + "' must be a boolean";
        return false;
      }
    } else if (type == "string") {
      if (!value.is_string()) {
        error = "parameter '" + name + "' must be a string";
        return false;
      }
      if (spec.contains("enum")) {
        const std::string v = value.get<std::string>();
        bool found = false;
        std::string allowed;
        for (const auto &opt : spec["enum"]) {
          const std::string s = opt.get<std::string>();
          if (!allowed.empty())
            allowed += ", ";
          allowed += s;
          if (s == v)
            found = true;
        }
        if (!found) {
          // Not clamped: substituting a different discrete choice would
          // silently change what the asset is.
          error = "parameter '" + name + "' value '" + v +
                  "' is not one of: " + allowed;
          return false;
        }
      }
    } else if (type == "array") {
      if (!value.is_array()) {
        error = "parameter '" + name + "' must be an array";
        return false;
      }
      const auto minItems = spec.value("minItems", static_cast<size_t>(0));
      const auto maxItems =
          spec.value("maxItems", std::numeric_limits<size_t>::max());
      if (value.size() < minItems || value.size() > maxItems) {
        error = "parameter '" + name + "' must have between " +
                std::to_string(minItems) + " and " + std::to_string(maxItems) +
                " items";
        return false;
      }
      // Element clamping for numeric arrays (colors): same forgiving rule.
      if (spec.contains("items") && spec["items"].value("type", "") == "number") {
        const double lo = spec["items"].value("minimum",
                                              -std::numeric_limits<double>::max());
        const double hi = spec["items"].value("maximum",
                                              std::numeric_limits<double>::max());
        for (auto &el : value) {
          if (!el.is_number()) {
            error = "parameter '" + name + "' must contain only numbers";
            return false;
          }
          const double v = el.get<double>();
          const double c = std::clamp(v, lo, hi);
          if (c != v) {
            warnings.push_back("parameter '" + name + "' element " + fmtNum(v) +
                               " clamped to " + fmtNum(c));
            el = c;
          }
        }
      }
    }
  }
  return true;
}

float Params::num(const std::string &key, float fallback) const {
  const auto it = mJson.find(key);
  if (it == mJson.end() || !it->is_number())
    return fallback;
  return it->get<float>();
}

int Params::integer(const std::string &key, int fallback) const {
  const auto it = mJson.find(key);
  if (it == mJson.end() || !it->is_number())
    return fallback;
  return it->get<int>();
}

bool Params::boolean(const std::string &key, bool fallback) const {
  const auto it = mJson.find(key);
  if (it == mJson.end() || !it->is_boolean())
    return fallback;
  return it->get<bool>();
}

std::string Params::str(const std::string &key,
                        const std::string &fallback) const {
  const auto it = mJson.find(key);
  if (it == mJson.end() || !it->is_string())
    return fallback;
  return it->get<std::string>();
}

glm::vec3 Params::color(const std::string &key, glm::vec3 fallback) const {
  const auto it = mJson.find(key);
  if (it == mJson.end() || !it->is_array() || it->size() < 3)
    return fallback;
  const auto &a = *it;
  if (!a[0].is_number() || !a[1].is_number() || !a[2].is_number())
    return fallback;
  return glm::vec3(a[0].get<float>(), a[1].get<float>(), a[2].get<float>());
}

const nlohmann::json &Params::array(const std::string &key) const {
  const auto it = mJson.find(key);
  if (it == mJson.end() || !it->is_array())
    return kEmptyArray;
  return *it;
}

} // namespace gen
