#include <doctest/doctest.h>

#include "ScriptJson.h"

// Lua <-> JSON conversion is the seam between script-authored parameters and
// the generator pipeline's schema validation (AI_ASSET_PIPELINE_PLAN.md Phase
// 2). It carries all the ambiguity: Lua has one table type for arrays and
// maps, and one number type for integers and floats.

using json = nlohmann::json;

namespace {

// A state with just enough libraries to build tables in test snippets.
sol::state makeLua() {
  sol::state lua;
  lua.open_libraries(sol::lib::base, sol::lib::table, sol::lib::math);
  return lua;
}

json evalToJson(sol::state &lua, const char *expr) {
  sol::object value = lua.script(std::string("return ") + expr);
  return scriptjson::toJson(value);
}

} // namespace

TEST_CASE("ScriptJson — scalars round-trip with the right JSON types") {
  sol::state lua = makeLua();

  CHECK(evalToJson(lua, "true") == json(true));
  CHECK(evalToJson(lua, "'hello'") == json("hello"));
  CHECK(evalToJson(lua, "nil").is_null());

  // Lua has one number type. An integer-valued double must come out as a JSON
  // integer, or schema validation sees 3.0 where it expects 3 and dump()
  // writes "3.0" into every recipe hash.
  const json three = evalToJson(lua, "3");
  CHECK(three.is_number_integer());
  CHECK(three.get<int>() == 3);

  const json fraction = evalToJson(lua, "2.5");
  CHECK(fraction.is_number_float());
  CHECK(fraction.get<double>() == doctest::Approx(2.5));

  CHECK(evalToJson(lua, "-7").get<int>() == -7);
}

TEST_CASE("ScriptJson — dense 1..n tables become arrays") {
  sol::state lua = makeLua();

  const json arr = evalToJson(lua, "{10, 20, 30}");
  REQUIRE(arr.is_array());
  REQUIRE(arr.size() == 3);
  CHECK(arr[0].get<int>() == 10);
  CHECK(arr[2].get<int>() == 30);

  const json colors = evalToJson(lua, "{0.2, 0.4, 0.6}");
  REQUIRE(colors.is_array());
  CHECK(colors[1].get<double>() == doctest::Approx(0.4));
}

TEST_CASE("ScriptJson — string-keyed tables become objects") {
  sol::state lua = makeLua();

  const json obj = evalToJson(lua, "{height = 8.5, canopy = 'conifer'}");
  REQUIRE(obj.is_object());
  CHECK(obj["height"].get<double>() == doctest::Approx(8.5));
  CHECK(obj["canopy"].get<std::string>() == "conifer");
}

TEST_CASE("ScriptJson — sparse integer keys are an object, not a lossy array") {
  sol::state lua = makeLua();

  // {[1]=a, [5]=b} has integer keys but gaps. Treating it as an array would
  // silently drop the entry at 5, which is worse than an oddly-shaped object.
  const json sparse = evalToJson(lua, "{[1] = 'a', [5] = 'b'}");
  REQUIRE(sparse.is_object());
  CHECK(sparse["1"].get<std::string>() == "a");
  CHECK(sparse["5"].get<std::string>() == "b");
}

TEST_CASE("ScriptJson — an empty table becomes an object") {
  sol::state lua = makeLua();
  // Genuinely ambiguous in Lua. Object is right for this codebase: almost
  // every empty table here is an omitted params block.
  const json empty = evalToJson(lua, "{}");
  CHECK(empty.is_object());
  CHECK(empty.empty());
}

TEST_CASE("ScriptJson — mixed array and hash parts become an object") {
  sol::state lua = makeLua();
  const json mixed = evalToJson(lua, "{1, 2, name = 'x'}");
  REQUIRE(mixed.is_object());
  CHECK(mixed["name"].get<std::string>() == "x");
  CHECK(mixed["1"].get<int>() == 1);
}

TEST_CASE("ScriptJson — nested structures convert recursively") {
  sol::state lua = makeLua();
  const json nested = evalToJson(lua, R"({
    id = 'tree/oak',
    params = { height = 9.0, barkColor = {0.3, 0.2, 0.1} },
    material = { bark = { generator = 'tex.bark', params = { resolution = 256 } } }
  })");

  REQUIRE(nested.is_object());
  CHECK(nested["id"].get<std::string>() == "tree/oak");
  CHECK(nested["params"]["height"].get<double>() == doctest::Approx(9.0));
  REQUIRE(nested["params"]["barkColor"].is_array());
  CHECK(nested["params"]["barkColor"][2].get<double>() == doctest::Approx(0.1));
  CHECK(nested["material"]["bark"]["generator"].get<std::string>() == "tex.bark");
  CHECK(nested["material"]["bark"]["params"]["resolution"].get<int>() == 256);
}

TEST_CASE("ScriptJson — unsupported values become null instead of throwing") {
  sol::state lua = makeLua();
  // A stray function in a params table should surface as a validation error
  // naming the field, not as a Lua exception from inside the converter.
  const json withFn = evalToJson(lua, "{ height = 3, cb = function() end }");
  REQUIRE(withFn.is_object());
  CHECK(withFn["height"].get<int>() == 3);
  CHECK(withFn["cb"].is_null());
}

TEST_CASE("ScriptJson — self-referencing tables terminate") {
  sol::state lua = makeLua();
  lua.script("t = {}; t.self = t");
  // Depth-bounded rather than cycle-detecting: cheap, and a cyclic params
  // table is a caller bug, not a case worth supporting.
  const json result = scriptjson::toJson(lua["t"]);
  CHECK(result.is_object());
}

TEST_CASE("ScriptJson — JSON converts back to Lua with 1-based arrays") {
  sol::state lua = makeLua();

  json j;
  j["name"] = "tree.v1";
  j["minimum"] = 0.3;
  j["count"] = 4;
  j["enum"] = json::array({"conifer", "broadleaf"});
  j["nested"]["flag"] = true;

  lua["v"] = scriptjson::toLua(lua, j);

  CHECK(lua.script("return v.name").get<std::string>() == "tree.v1");
  CHECK(lua.script("return v.minimum").get<double>() == doctest::Approx(0.3));
  CHECK(lua.script("return v.count").get<int>() == 4);
  // Lua arrays start at 1; an off-by-one here makes every schema enum unreadable.
  CHECK(lua.script("return v.enum[1]").get<std::string>() == "conifer");
  CHECK(lua.script("return v.enum[2]").get<std::string>() == "broadleaf");
  CHECK(lua.script("return #v.enum").get<int>() == 2);
  CHECK(lua.script("return v.nested.flag").get<bool>() == true);
}

TEST_CASE("ScriptJson — a params table survives a full round trip") {
  sol::state lua = makeLua();

  const json original = evalToJson(lua, R"({
    height = 9.0, branchLevels = 3, canopy = 'broadleaf',
    barkColor = {0.3, 0.2, 0.1}, wind = true
  })");

  lua["back"] = scriptjson::toLua(lua, original);
  const json again = scriptjson::toJson(lua["back"]);

  // Equality, not similarity: this value feeds recipeHash(), so any drift
  // through the round trip would change asset identity.
  CHECK(again == original);
}

TEST_CASE("ScriptJson — null and empty containers survive the round trip") {
  sol::state lua = makeLua();

  json j;
  j["emptyObj"] = json::object();
  j["emptyArr"] = json::array();
  j["nothing"] = nullptr;

  lua["v"] = scriptjson::toLua(lua, j);
  const json back = scriptjson::toJson(lua["v"]);

  // An empty JSON array cannot survive: Lua represents it as an empty table,
  // which converts back to an object (see the empty-table case above). Nil
  // keys vanish entirely, since Lua has no way to store one.
  CHECK(back["emptyObj"].is_object());
  CHECK(back["emptyArr"].is_object());
  CHECK_FALSE(back.contains("nothing"));
}
