#include "../Binding.hpp"

#include <hyprtoolkit/core/Input.hpp>
#include <hyprtoolkit/core/LogTypes.hpp>
#include <hyprtoolkit/element/Element.hpp>
#include <hyprtoolkit/types/FontTypes.hpp>
#include <hyprtoolkit/types/ImageTypes.hpp>
#include <hyprtoolkit/types/PointerShape.hpp>
#include <hyprtoolkit/types/SizeType.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

using namespace Hyprtoolkit;
using namespace Hyprtoolkit::Lua;
using namespace Hyprutils::Memory;

static double tableNumber(lua_State* state, int table, const char* field, int arrayIndex = 0) {
    table = lua_absindex(state, table);
    lua_pushstring(state, field);
    lua_rawget(state, table);
    if (lua_isnil(state, -1) && arrayIndex > 0) {
        lua_pop(state, 1);
        lua_rawgeti(state, table, arrayIndex);
    }

    if (lua_type(state, -1) != LUA_TNUMBER)
        fail("field '" + std::string{field} + "' must be a number");

    const double result = lua_tonumber(state, -1);
    if (!std::isfinite(result))
        fail("field '" + std::string{field} + "' must be finite");
    lua_pop(state, 1);
    return result;
}

static double optionalTableNumber(lua_State* state, int table, const char* field, double fallback, int arrayIndex = 0) {
    table = lua_absindex(state, table);
    lua_pushstring(state, field);
    lua_rawget(state, table);
    if (lua_isnil(state, -1) && arrayIndex > 0) {
        lua_pop(state, 1);
        lua_rawgeti(state, table, arrayIndex);
    }

    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return fallback;
    }
    if (lua_type(state, -1) != LUA_TNUMBER)
        fail("field '" + std::string{field} + "' must be a number");

    const double result = lua_tonumber(state, -1);
    if (!std::isfinite(result))
        fail("field '" + std::string{field} + "' must be finite");
    lua_pop(state, 1);
    return result;
}

static float tableFloat(double value, const char* field) {
    if (value < std::numeric_limits<float>::lowest() || value > std::numeric_limits<float>::max())
        fail("field '" + std::string{field} + "' is outside the float range");
    return sc<float>(value);
}

static std::string tableString(lua_State* state, int table, const char* field) {
    table = lua_absindex(state, table);
    lua_pushstring(state, field);
    lua_rawget(state, table);
    if (lua_type(state, -1) != LUA_TSTRING)
        fail("field '" + std::string{field} + "' must be a string");

    size_t      length = 0;
    const char* value  = lua_tolstring(state, -1, &length);
    std::string result{value, length};
    lua_pop(state, 1);
    return result;
}

static void pushNumberField(lua_State* state, const char* field, double value) {
    lua_pushnumber(state, value);
    lua_setfield(state, -2, field);
}

static void pushIntegerField(lua_State* state, const char* field, lua_Integer value) {
    lua_pushinteger(state, value);
    lua_setfield(state, -2, field);
}

static void pushStringField(lua_State* state, const char* field, const char* value) {
    lua_pushstring(state, value);
    lua_setfield(state, -2, field);
}

Hyprutils::Math::Vector2D Hyprtoolkit::Lua::vector(lua_State* state, int index) {
    if (!lua_istable(state, index))
        fail("argument " + std::to_string(index) + " must be a vector table");

    return {
        tableNumber(state, index, "x", 1),
        tableNumber(state, index, "y", 2),
    };
}

void Hyprtoolkit::Lua::pushVector(lua_State* state, const Hyprutils::Math::Vector2D& value) {
    lua_createtable(state, 2, 2);
    lua_pushnumber(state, value.x);
    lua_rawseti(state, -2, 1);
    lua_pushnumber(state, value.y);
    lua_rawseti(state, -2, 2);
    pushNumberField(state, "x", value.x);
    pushNumberField(state, "y", value.y);
}

CDynamicSize Hyprtoolkit::Lua::dynamicSize(lua_State* state, int index) {
    return object<CDynamicSize>(state, index, MT_SIZE);
}

CFontSize Hyprtoolkit::Lua::fontSize(lua_State* state, int index) {
    return object<CFontSize>(state, index, MT_FONT_SIZE);
}

CHyprColor Hyprtoolkit::Lua::color(lua_State* state, int index) {
    return object<CHyprColor>(state, index, MT_COLOR);
}

CGradientValueData Hyprtoolkit::Lua::gradient(lua_State* state, int index) {
    if (luaL_testudata(state, index, MT_COLOR.data()))
        return CGradientValueData{color(state, index)};
    if (!lua_istable(state, index))
        fail("argument " + std::to_string(index) + " must be a Color or gradient table");

    index = lua_absindex(state, index);
    lua_pushliteral(state, "colors");
    lua_rawget(state, index);
    if (!lua_istable(state, -1))
        fail("gradient field 'colors' must be a table");

    CGradientValueData result;
    const size_t       count = lua_rawlen(state, -1);
    if (count == 0)
        fail("gradient field 'colors' must not be empty");
    result.m_vColors.reserve(count);
    for (size_t i = 1; i <= count; ++i) {
        lua_rawgeti(state, -1, sc<lua_Integer>(i));
        result.m_vColors.emplace_back(color(state, -1));
        lua_pop(state, 1);
    }
    lua_pop(state, 1);

    result.m_fAngle = tableFloat(optionalTableNumber(state, index, "angle", 0.0), "angle");
    return result;
}

SAnimation Hyprtoolkit::Lua::animation(lua_State* state, int index) {
    if (!lua_istable(state, index))
        fail("argument " + std::to_string(index) + " must be an animation table");

    index           = lua_absindex(state, index);
    const auto type = tableString(state, index, "type");
    if (type == "none")
        return SNoAnimation{};
    if (type == "bezier") {
        const double duration = tableNumber(state, index, "durationMs");
        if (!std::isfinite(duration) || duration < 0.0 || std::floor(duration) != duration || duration > sc<double>(std::numeric_limits<int64_t>::max()))
            fail("animation field 'durationMs' must be a non-negative integer");

        lua_pushliteral(state, "control1");
        lua_rawget(state, index);
        const auto control1 = vector(state, -1);
        lua_pop(state, 1);
        lua_pushliteral(state, "control2");
        lua_rawget(state, index);
        const auto control2 = vector(state, -1);
        lua_pop(state, 1);
        return SBezierAnimation{
            .duration = std::chrono::milliseconds{sc<int64_t>(duration)},
            .control1 = control1,
            .control2 = control2,
        };
    }
    if (type == "spring")
        return SSpringAnimation{
            .stiffness       = tableFloat(tableNumber(state, index, "stiffness", 1), "stiffness"),
            .damping         = tableFloat(tableNumber(state, index, "damping", 2), "damping"),
            .mass            = tableFloat(tableNumber(state, index, "mass", 3), "mass"),
            .valueEpsilon    = tableFloat(tableNumber(state, index, "valueEpsilon", 4), "valueEpsilon"),
            .velocityEpsilon = tableFloat(tableNumber(state, index, "velocityEpsilon", 5), "velocityEpsilon"),
        };

    fail("animation field 'type' must be 'none', 'bezier', or 'spring'");
}

std::vector<std::string> Hyprtoolkit::Lua::stringArray(lua_State* state, int index) {
    if (!lua_istable(state, index))
        fail("argument " + std::to_string(index) + " must be a string array");

    index                          = lua_absindex(state, index);
    const size_t             count = lua_rawlen(state, index);
    std::vector<std::string> result;
    result.reserve(count);
    for (size_t i = 1; i <= count; ++i) {
        lua_rawgeti(state, index, sc<lua_Integer>(i));
        result.emplace_back(string(state, -1));
        lua_pop(state, 1);
    }
    return result;
}

std::vector<Hyprutils::Math::Vector2D> Hyprtoolkit::Lua::vectorArray(lua_State* state, int index) {
    if (!lua_istable(state, index))
        fail("argument " + std::to_string(index) + " must be a vector array");

    index                                        = lua_absindex(state, index);
    const size_t                           count = lua_rawlen(state, index);
    std::vector<Hyprutils::Math::Vector2D> result;
    result.reserve(count);
    for (size_t i = 1; i <= count; ++i) {
        lua_rawgeti(state, index, sc<lua_Integer>(i));
        result.emplace_back(vector(state, -1));
        lua_pop(state, 1);
    }
    return result;
}

void Hyprtoolkit::Lua::pushKeyboardEvent(lua_State* state, const Input::SKeyboardKeyEvent& event) {
    lua_createtable(state, 0, 5);
    pushIntegerField(state, "xkbKeysym", event.xkbKeysym);
    lua_pushboolean(state, event.down);
    lua_setfield(state, -2, "down");
    lua_pushboolean(state, event.repeat);
    lua_setfield(state, -2, "repeat");
    lua_pushlstring(state, event.utf8.data(), event.utf8.size());
    lua_setfield(state, -2, "utf8");
    pushIntegerField(state, "modMask", event.modMask);
}

void Hyprtoolkit::Lua::pushTouchEvent(lua_State* state, const Input::STouchEvent& event) {
    lua_createtable(state, 0, 3);
    pushIntegerField(state, "id", event.id);
    pushVector(state, event.local);
    lua_setfield(state, -2, "local");
    pushIntegerField(state, "timeMs", event.timeMs);
}

template <typename Enum>
static Enum checkedEnum(lua_State* state, int index, Enum first, Enum last, const char* name) {
    const lua_Integer value = integer(state, index);
    if (value < sc<lua_Integer>(first) || value > sc<lua_Integer>(last))
        fail("argument " + std::to_string(index) + " is not a valid " + name);
    return sc<Enum>(value);
}

static int createDynamicSize(lua_State* state) {
    expectArguments(state, 3, 4);
    const auto typeX = checkedEnum(state, 1, CDynamicSize::HT_SIZE_ABSOLUTE, CDynamicSize::HT_SIZE_AUTO, "size type");
    const auto typeY = checkedEnum(state, 2, CDynamicSize::HT_SIZE_ABSOLUTE, CDynamicSize::HT_SIZE_AUTO, "size type");
    const auto value = lua_gettop(state) == 3 ? vector(state, 3) : Hyprutils::Math::Vector2D{number(state, 3), number(state, 4)};
    pushObject(state, CDynamicSize{typeX, typeY, value}, MT_SIZE);
    return 1;
}

static int createVector(lua_State* state) {
    expectArguments(state, 2, 2);
    pushVector(state, {number(state, 1), number(state, 2)});
    return 1;
}

static int createFontSize(lua_State* state) {
    expectArguments(state, 1, 2);
    const auto base = checkedEnum(state, 1, CFontSize::HT_FONT_H1, CFontSize::HT_FONT_ABSOLUTE, "font sizing base");
    pushObject(state, CFontSize{base, sc<float>(lua_gettop(state) == 2 ? number(state, 2) : 1.0)}, MT_FONT_SIZE);
    return 1;
}

static int createColor(lua_State* state) {
    const int count = lua_gettop(state);
    if (count == 1) {
        const lua_Integer argb = integer(state, 1);
        if (argb < 0 || sc<uint64_t>(argb) > std::numeric_limits<uint32_t>::max())
            fail("ARGB color must be between 0 and 0xFFFFFFFF");
        pushObject(state, CHyprColor{sc<uint64_t>(argb)}, MT_COLOR);
        return 1;
    }
    if (count != 3 && count != 4)
        fail("color expects (argb) or (r, g, b[, a])");

    pushObject(state,
               CHyprColor{
                   floatNumber(state, 1),
                   floatNumber(state, 2),
                   floatNumber(state, 3),
                   count == 4 ? floatNumber(state, 4) : 1.0F,
               },
               MT_COLOR);
    return 1;
}

static int dynamicSizeCalculate(lua_State* state) {
    expectArguments(state, 2, 2);
    pushVector(state, dynamicSize(state, 1).calculate(vector(state, 2)));
    return 1;
}

static int fontSizePoints(lua_State* state) {
    expectArguments(state, 1, 1);
    lua_pushnumber(state, fontSize(state, 1).ptSize());
    return 1;
}

static int colorIndex(lua_State* state) {
    expectArguments(state, 2, 2);
    const auto& value = object<CHyprColor>(state, 1, MT_COLOR);
    if (lua_type(state, 2) == LUA_TSTRING) {
        const std::string key = string(state, 2);
        if (key == "r") {
            lua_pushnumber(state, value.r);
            return 1;
        }
        if (key == "g") {
            lua_pushnumber(state, value.g);
            return 1;
        }
        if (key == "b") {
            lua_pushnumber(state, value.b);
            return 1;
        }
        if (key == "a") {
            lua_pushnumber(state, value.a);
            return 1;
        }
    }

    luaL_getmetatable(state, MT_COLOR.data());
    lua_getfield(state, -1, "__methods");
    lua_remove(state, -2);
    lua_pushvalue(state, 2);
    lua_rawget(state, -2);
    lua_remove(state, -2);
    return 1;
}

static int colorNewIndex(lua_State* state) {
    object<CHyprColor>(state, 1, MT_COLOR);
    fail("Color values are immutable");
}

static int colorEqual(lua_State* state) {
    expectArguments(state, 2, 2);
    lua_pushboolean(state, color(state, 1) == color(state, 2));
    return 1;
}

static int colorGetAsHex(lua_State* state) {
    expectArguments(state, 1, 1);
    lua_pushinteger(state, color(state, 1).getAsHex());
    return 1;
}

static int colorStripA(lua_State* state) {
    expectArguments(state, 1, 1);
    pushObject(state, color(state, 1).stripA(), MT_COLOR);
    return 1;
}

static int colorBrighten(lua_State* state) {
    expectArguments(state, 2, 2);
    pushObject(state, color(state, 1).brighten(floatNumber(state, 2)), MT_COLOR);
    return 1;
}

static int colorDarken(lua_State* state) {
    expectArguments(state, 2, 2);
    pushObject(state, color(state, 1).darken(floatNumber(state, 2)), MT_COLOR);
    return 1;
}

static int colorMix(lua_State* state) {
    expectArguments(state, 3, 3);
    pushObject(state, color(state, 1).mix(color(state, 2), floatNumber(state, 3)), MT_COLOR);
    return 1;
}

static int colorAsRGB(lua_State* state) {
    expectArguments(state, 1, 1);
    const auto value = color(state, 1).asRGB();
    lua_createtable(state, 0, 3);
    pushNumberField(state, "r", value.r);
    pushNumberField(state, "g", value.g);
    pushNumberField(state, "b", value.b);
    return 1;
}

static int colorAsOkLab(lua_State* state) {
    expectArguments(state, 1, 1);
    const auto value = color(state, 1).asOkLab();
    lua_createtable(state, 0, 3);
    pushNumberField(state, "l", value.l);
    pushNumberField(state, "a", value.a);
    pushNumberField(state, "b", value.b);
    return 1;
}

static int colorAsHSL(lua_State* state) {
    expectArguments(state, 1, 1);
    const auto value = color(state, 1).asHSL();
    lua_createtable(state, 0, 3);
    pushNumberField(state, "h", value.h);
    pushNumberField(state, "s", value.s);
    pushNumberField(state, "l", value.l);
    return 1;
}

static int colorGetR(lua_State* state) {
    expectArguments(state, 1, 1);
    lua_pushnumber(state, color(state, 1).r);
    return 1;
}

static int colorGetG(lua_State* state) {
    expectArguments(state, 1, 1);
    lua_pushnumber(state, color(state, 1).g);
    return 1;
}

static int colorGetB(lua_State* state) {
    expectArguments(state, 1, 1);
    lua_pushnumber(state, color(state, 1).b);
    return 1;
}

static int colorGetA(lua_State* state) {
    expectArguments(state, 1, 1);
    lua_pushnumber(state, color(state, 1).a);
    return 1;
}

static void pushNoAnimation(lua_State* state) {
    lua_createtable(state, 0, 1);
    pushStringField(state, "type", "none");
}

static void pushBezierAnimation(lua_State* state, const SBezierAnimation& animation) {
    lua_createtable(state, 0, 4);
    pushStringField(state, "type", "bezier");
    pushIntegerField(state, "durationMs", animation.duration.count());
    pushVector(state, animation.control1);
    lua_setfield(state, -2, "control1");
    pushVector(state, animation.control2);
    lua_setfield(state, -2, "control2");
}

static void pushSpringAnimation(lua_State* state, const SSpringAnimation& animation) {
    lua_createtable(state, 0, 6);
    pushStringField(state, "type", "spring");
    const double values[] = {
        animation.stiffness, animation.damping, animation.mass, animation.valueEpsilon, animation.velocityEpsilon,
    };
    const char* fields[] = {
        "stiffness", "damping", "mass", "valueEpsilon", "velocityEpsilon",
    };
    for (int i = 0; i < 5; ++i)
        pushNumberField(state, fields[i], values[i]);
}

static int createNoAnimation(lua_State* state) {
    expectArguments(state, 0, 0);
    pushNoAnimation(state);
    return 1;
}

static int createBezierAnimation(lua_State* state) {
    const int count = lua_gettop(state);
    if (count != 1 && count != 3)
        fail("bezierAnimation expects (durationMs[, control1, control2])");

    const lua_Integer duration = integer(state, 1);
    if (duration < 0)
        fail("durationMs must be non-negative");
    const SBezierAnimation value{
        .duration = std::chrono::milliseconds{duration},
        .control1 = count == 3 ? vector(state, 2) : Hyprutils::Math::Vector2D{0.23, 1.0},
        .control2 = count == 3 ? vector(state, 3) : Hyprutils::Math::Vector2D{0.32, 1.0},
    };
    pushBezierAnimation(state, value);
    return 1;
}

static int createSpringAnimation(lua_State* state) {
    const int        count = lua_gettop(state);
    SSpringAnimation value;
    if (count == 1 && lua_istable(state, 1)) {
        value.stiffness       = tableFloat(optionalTableNumber(state, 1, "stiffness", value.stiffness, 1), "stiffness");
        value.damping         = tableFloat(optionalTableNumber(state, 1, "damping", value.damping, 2), "damping");
        value.mass            = tableFloat(optionalTableNumber(state, 1, "mass", value.mass, 3), "mass");
        value.valueEpsilon    = tableFloat(optionalTableNumber(state, 1, "valueEpsilon", value.valueEpsilon, 4), "valueEpsilon");
        value.velocityEpsilon = tableFloat(optionalTableNumber(state, 1, "velocityEpsilon", value.velocityEpsilon, 5), "velocityEpsilon");
    } else if (count == 5) {
        value = {
            .stiffness       = floatNumber(state, 1),
            .damping         = floatNumber(state, 2),
            .mass            = floatNumber(state, 3),
            .valueEpsilon    = floatNumber(state, 4),
            .velocityEpsilon = floatNumber(state, 5),
        };
    } else if (count != 0)
        fail("springAnimation expects (), (table), or five numeric arguments");

    pushSpringAnimation(state, value);
    return 1;
}

static int immutableAnimation(lua_State*) {
    fail("animation presets are immutable");
}

static void makeAnimationImmutable(lua_State* state) {
    const int values = lua_absindex(state, -1);
    lua_newtable(state);
    lua_newtable(state);
    lua_pushvalue(state, values);
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, protect<immutableAnimation>);
    lua_setfield(state, -2, "__newindex");
    lua_pushliteral(state, "hyprtoolkit.AnimationPreset");
    lua_setfield(state, -2, "__metatable");
    lua_setmetatable(state, -2);
    lua_remove(state, values);
}

static constexpr luaL_Reg DYNAMIC_SIZE_METHODS[] = {
    {"calculate", protect<dynamicSizeCalculate>},
    {nullptr, nullptr},
};

static constexpr luaL_Reg FONT_SIZE_METHODS[] = {
    {"ptSize", protect<fontSizePoints>},
    {nullptr, nullptr},
};

static constexpr luaL_Reg COLOR_METHODS[] = {
    {"__index", protect<colorIndex>}, {"__newindex", protect<colorNewIndex>}, {"__eq", protect<colorEqual>},    {"getAsHex", protect<colorGetAsHex>},
    {"stripA", protect<colorStripA>}, {"brighten", protect<colorBrighten>},   {"darken", protect<colorDarken>}, {"mix", protect<colorMix>},
    {"asRGB", protect<colorAsRGB>},   {"asOkLab", protect<colorAsOkLab>},     {"asHSL", protect<colorAsHSL>},   {"getR", protect<colorGetR>},
    {"getG", protect<colorGetG>},     {"getB", protect<colorGetB>},           {"getA", protect<colorGetA>},     {nullptr, nullptr},
};

static constexpr luaL_Reg DYNAMIC_SIZE_CLASS[] = {
    {"new", protect<createDynamicSize>},
    {nullptr, nullptr},
};

static constexpr luaL_Reg FONT_SIZE_CLASS[] = {
    {"new", protect<createFontSize>},
    {nullptr, nullptr},
};

static constexpr luaL_Reg COLOR_CLASS[] = {
    {"new", protect<createColor>},
    {nullptr, nullptr},
};

static constexpr luaL_Reg ANIMATION_CLASS[] = {
    {"none", protect<createNoAnimation>},
    {"bezier", protect<createBezierAnimation>},
    {"spring", protect<createSpringAnimation>},
    {"noAnimation", protect<createNoAnimation>},
    {"bezierAnimation", protect<createBezierAnimation>},
    {"springAnimation", protect<createSpringAnimation>},
    {nullptr, nullptr},
};

struct SConstant {
    const char* name;
    lua_Integer value;
};

static constexpr SConstant CONSTANTS[] = {
    {"HT_SIZE_ABSOLUTE", CDynamicSize::HT_SIZE_ABSOLUTE},
    {"HT_SIZE_PERCENT", CDynamicSize::HT_SIZE_PERCENT},
    {"HT_SIZE_AUTO", CDynamicSize::HT_SIZE_AUTO},
    {"HT_FONT_H1", CFontSize::HT_FONT_H1},
    {"HT_FONT_H2", CFontSize::HT_FONT_H2},
    {"HT_FONT_H3", CFontSize::HT_FONT_H3},
    {"HT_FONT_TEXT", CFontSize::HT_FONT_TEXT},
    {"HT_FONT_SMALL", CFontSize::HT_FONT_SMALL},
    {"HT_FONT_ABSOLUTE", CFontSize::HT_FONT_ABSOLUTE},
    {"HT_FONT_ALIGN_LEFT", HT_FONT_ALIGN_LEFT},
    {"HT_FONT_ALIGN_CENTER", HT_FONT_ALIGN_CENTER},
    {"HT_FONT_ALIGN_RIGHT", HT_FONT_ALIGN_RIGHT},
    {"HT_POINTER_ARROW", HT_POINTER_ARROW},
    {"HT_POINTER_POINTER", HT_POINTER_POINTER},
    {"HT_POINTER_TEXT", HT_POINTER_TEXT},
    {"HT_POINTER_RESIZE_NS", HT_POINTER_RESIZE_NS},
    {"HT_POINTER_RESIZE_EW", HT_POINTER_RESIZE_EW},
    {"HT_POINTER_RESIZE_NESW", HT_POINTER_RESIZE_NESW},
    {"HT_POINTER_RESIZE_NWSE", HT_POINTER_RESIZE_NWSE},
    {"IMAGE_FIT_MODE_STRETCH", IMAGE_FIT_MODE_STRETCH},
    {"IMAGE_FIT_MODE_COVER", IMAGE_FIT_MODE_COVER},
    {"IMAGE_FIT_MODE_CONTAIN", IMAGE_FIT_MODE_CONTAIN},
    {"IMAGE_FIT_MODE_TILE", IMAGE_FIT_MODE_TILE},
    {"HT_WINDOW_TOPLEVEL", HT_WINDOW_TOPLEVEL},
    {"HT_WINDOW_POPUP", HT_WINDOW_POPUP},
    {"HT_WINDOW_LAYER", HT_WINDOW_LAYER},
    {"HT_WINDOW_LOCK_SURFACE", HT_WINDOW_LOCK_SURFACE},
    {"HT_RESIZE_EDGE_NONE", HT_RESIZE_EDGE_NONE},
    {"HT_RESIZE_EDGE_TOP", HT_RESIZE_EDGE_TOP},
    {"HT_RESIZE_EDGE_BOTTOM", HT_RESIZE_EDGE_BOTTOM},
    {"HT_RESIZE_EDGE_LEFT", HT_RESIZE_EDGE_LEFT},
    {"HT_RESIZE_EDGE_TOP_LEFT", HT_RESIZE_EDGE_TOP_LEFT},
    {"HT_RESIZE_EDGE_BOTTOM_LEFT", HT_RESIZE_EDGE_BOTTOM_LEFT},
    {"HT_RESIZE_EDGE_RIGHT", HT_RESIZE_EDGE_RIGHT},
    {"HT_RESIZE_EDGE_TOP_RIGHT", HT_RESIZE_EDGE_TOP_RIGHT},
    {"HT_RESIZE_EDGE_BOTTOM_RIGHT", HT_RESIZE_EDGE_BOTTOM_RIGHT},
    {"HT_POSITION_ABSOLUTE", IElement::HT_POSITION_ABSOLUTE},
    {"HT_POSITION_AUTO", IElement::HT_POSITION_AUTO},
    {"HT_POSITION_FLAG_HCENTER", IElement::HT_POSITION_FLAG_HCENTER},
    {"HT_POSITION_FLAG_VCENTER", IElement::HT_POSITION_FLAG_VCENTER},
    {"HT_POSITION_FLAG_CENTER", IElement::HT_POSITION_FLAG_CENTER},
    {"HT_POSITION_FLAG_LEFT", IElement::HT_POSITION_FLAG_LEFT},
    {"HT_POSITION_FLAG_RIGHT", IElement::HT_POSITION_FLAG_RIGHT},
    {"HT_POSITION_FLAG_TOP", IElement::HT_POSITION_FLAG_TOP},
    {"HT_POSITION_FLAG_BOTTOM", IElement::HT_POSITION_FLAG_BOTTOM},
    {"HT_POSITION_FLAG_ALL", IElement::HT_POSITION_FLAG_ALL},
    {"HT_CHECKBOX_STYLE_CHECKMARK", HT_CHECKBOX_STYLE_CHECKMARK},
    {"HT_CHECKBOX_STYLE_RADIO", HT_CHECKBOX_STYLE_RADIO},
    {"MOUSE_BUTTON_UNKNOWN", Input::MOUSE_BUTTON_UNKNOWN},
    {"MOUSE_BUTTON_LEFT", Input::MOUSE_BUTTON_LEFT},
    {"MOUSE_BUTTON_RIGHT", Input::MOUSE_BUTTON_RIGHT},
    {"MOUSE_BUTTON_MIDDLE", Input::MOUSE_BUTTON_MIDDLE},
    {"MOUSE_BUTTON_BACK", Input::MOUSE_BUTTON_BACK},
    {"MOUSE_BUTTON_FORWARD", Input::MOUSE_BUTTON_FORWARD},
    {"AXIS_AXIS_HORIZONTAL", Input::AXIS_AXIS_HORIZONTAL},
    {"AXIS_AXIS_VERTICAL", Input::AXIS_AXIS_VERTICAL},
    {"HT_MODIFIER_SHIFT", Input::HT_MODIFIER_SHIFT},
    {"HT_MODIFIER_CAPS", Input::HT_MODIFIER_CAPS},
    {"HT_MODIFIER_CTRL", Input::HT_MODIFIER_CTRL},
    {"HT_MODIFIER_ALT", Input::HT_MODIFIER_ALT},
    {"HT_MODIFIER_MOD2", Input::HT_MODIFIER_MOD2},
    {"HT_MODIFIER_MOD3", Input::HT_MODIFIER_MOD3},
    {"HT_MODIFIER_META", Input::HT_MODIFIER_META},
    {"HT_MODIFIER_MOD5", Input::HT_MODIFIER_MOD5},
    {"HT_MODIFIER_CTRL_SHIFT", Input::HT_MODIFIER_CTRL_SHIFT},
    {"HT_LOG_TRACE", HT_LOG_TRACE},
    {"HT_LOG_DEBUG", HT_LOG_DEBUG},
    {"HT_LOG_WARNING", HT_LOG_WARNING},
    {"HT_LOG_ERROR", HT_LOG_ERROR},
    {"HT_LOG_CRITICAL", HT_LOG_CRITICAL},
    {"LOCK_ERROR_PLATFORM_UNINITIALIZED", LOCK_ERROR_PLATFORM_UNINITIALIZED},
    {"LOCK_ERROR_DENIED", LOCK_ERROR_DENIED},
};

void Hyprtoolkit::Lua::registerTypes(lua_State* state, int module) {
    module = lua_absindex(state, module);
    registerMetatable(state, MT_SIZE, DYNAMIC_SIZE_METHODS, protect<destroyObject<CDynamicSize>>);
    registerMetatable(state, MT_FONT_SIZE, FONT_SIZE_METHODS, protect<destroyObject<CFontSize>>);
    registerMetatable(state, MT_COLOR, COLOR_METHODS, protect<destroyObject<CHyprColor>>);

    setClass(state, module, "DynamicSize", DYNAMIC_SIZE_CLASS);
    setClass(state, module, "FontSize", FONT_SIZE_CLASS);
    setClass(state, module, "Color", COLOR_CLASS);
    setClass(state, module, "Animation", ANIMATION_CLASS);
    lua_getfield(state, module, "Animation");
    pushSpringAnimation(state, AnimationPresets::Slow);
    makeAnimationImmutable(state);
    lua_setfield(state, -2, "Slow");
    pushSpringAnimation(state, AnimationPresets::Medium);
    makeAnimationImmutable(state);
    lua_setfield(state, -2, "Medium");
    pushSpringAnimation(state, AnimationPresets::Fast);
    makeAnimationImmutable(state);
    lua_setfield(state, -2, "Fast");
    pushSpringAnimation(state, AnimationPresets::Snappy);
    makeAnimationImmutable(state);
    lua_setfield(state, -2, "Snappy");
    pushSpringAnimation(state, AnimationPresets::Bouncy);
    makeAnimationImmutable(state);
    lua_setfield(state, -2, "Bouncy");
    lua_pop(state, 1);
    setFunction(state, module, "vec2", protect<createVector>);
    setFunction(state, module, "dynamicSize", protect<createDynamicSize>);
    setFunction(state, module, "fontSize", protect<createFontSize>);
    setFunction(state, module, "color", protect<createColor>);
    setFunction(state, module, "noAnimation", protect<createNoAnimation>);
    setFunction(state, module, "bezierAnimation", protect<createBezierAnimation>);
    setFunction(state, module, "springAnimation", protect<createSpringAnimation>);

    for (const auto& [name, value] : CONSTANTS)
        setInteger(state, module, name, value);
}
