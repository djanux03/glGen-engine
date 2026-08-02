#pragma once
// GeneratorSchema.h — the parameter contract between a generator and whoever
// is filling it in (a hand-written recipe, the editor, or an AI).
//
// Each generator publishes a JSON Schema subset describing its parameters:
// type, range, default, and a human-readable description. That one document
// does double duty (AI_ASSET_PIPELINE_PLAN.md §2, point 4) -- it is the
// engine's validator AND the description an MCP tool exposes to the model, so
// the two cannot drift apart.
//
// Validation is deliberately forgiving where forgiveness is safe and strict
// where it isn't:
//   * unknown property      -> warning, ignored
//   * missing property      -> filled from the schema default
//   * number out of range   -> CLAMPED, with a warning
//   * wrong type            -> error
//   * value outside an enum -> error
// Clamping rather than erroring matters for an AI-driven pipeline: the caller
// still gets a usable asset plus text feedback describing what was adjusted,
// which is far more useful than a hard rejection. Enums and types are
// different -- silently substituting there would change what the asset IS
// without the caller having any way to notice.

#include "json.hpp"

#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace gen {

// Fluent builder so generator authors declare parameters in code rather than
// hand-writing schema JSON (which drifts).
class SchemaBuilder {
public:
  SchemaBuilder();

  SchemaBuilder &number(const std::string &name, float defaultValue, float min,
                        float max, const std::string &description);
  SchemaBuilder &integer(const std::string &name, int defaultValue, int min,
                         int max, const std::string &description);
  SchemaBuilder &boolean(const std::string &name, bool defaultValue,
                         const std::string &description);
  SchemaBuilder &enumString(const std::string &name,
                            const std::string &defaultValue,
                            std::vector<std::string> values,
                            const std::string &description);
  // An RGB triple in 0..1, expressed as a 3-element array.
  SchemaBuilder &color(const std::string &name, glm::vec3 defaultValue,
                       const std::string &description);
  // A free-form array of objects (kitbash part lists). Contents are the
  // generator's own business; the schema only records that it is an array.
  SchemaBuilder &objectArray(const std::string &name,
                             const std::string &description);
  SchemaBuilder &string(const std::string &name,
                        const std::string &defaultValue,
                        const std::string &description);

  // Not named json(): a member function by that name shadows the `json` type
  // alias inside every member body, which MSVC reports as a wall of
  // unrelated-looking syntax errors.
  const nlohmann::json &schema() const { return mSchema; }

private:
  nlohmann::json mSchema;
};

// Validates `params` against `schema` IN PLACE: fills defaults, clamps
// numbers, and appends human-readable notes to `warnings`. Returns false with
// `error` set on a type mismatch or an out-of-enum value.
bool validateParams(const nlohmann::json &schema, nlohmann::json &params,
                    std::vector<std::string> &warnings, std::string &error);

// Read accessors for a params object that has already been through
// validateParams(). Each falls back to a harmless value rather than throwing,
// so a generator can never be crashed by malformed input that slipped past.
class Params {
public:
  explicit Params(const nlohmann::json &j) : mJson(j) {}

  float num(const std::string &key, float fallback = 0.0f) const;
  int integer(const std::string &key, int fallback = 0) const;
  bool boolean(const std::string &key, bool fallback = false) const;
  std::string str(const std::string &key,
                  const std::string &fallback = {}) const;
  glm::vec3 color(const std::string &key,
                  glm::vec3 fallback = glm::vec3(1.0f)) const;
  const nlohmann::json &array(const std::string &key) const;

private:
  const nlohmann::json &mJson;
};

} // namespace gen
