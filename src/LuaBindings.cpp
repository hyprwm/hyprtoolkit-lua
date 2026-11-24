#include "Binding.hpp"

#include <hyprtoolkit-lua/LuaBindings.hpp>
#include <hyprtoolkit-lua/Embedding.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <format>
#include <limits>

using namespace Hyprtoolkit;
using namespace Hyprtoolkit::Lua;

static char                       CONTEXT_KEY       = 0;
static constexpr std::string_view CONTEXT_METATABLE = "hyprtoolkit.Context";

struct SContextHolder {
    std::shared_ptr<SLuaContext> context;
};

static std::expected<void, std::string> retireContext(const std::shared_ptr<SLuaContext>& context) {
    if (context->ownerThread != std::this_thread::get_id())
        return std::unexpected("bindings must be retired on their owner thread");
    if (context->activeCalls != 0)
        return std::unexpected("bindings cannot be retired from an executing callback");
    if (!context->alive || context->retiring)
        return {};

    context->retiring      = true;
    context->backendActive = false;
    for (const auto& weak : context->connections) {
        if (const auto connection = weak.lock())
            connection->listener.reset();
    }
    context->connections.clear();
    for (const auto& weak : context->callbacks) {
        if (const auto callback = weak.lock())
            callback->release();
    }
    context->callbacks.clear();
    context->hooks = {};
    context->hostBackend.reset();
    context->alive = false;
    context->state = nullptr;
    return {};
}

static int destroyContext(lua_State* state) {
    const auto storage = static_cast<SUserdata<SContextHolder>*>(luaL_testudata(state, 1, CONTEXT_METATABLE.data()));
    if (!storage || !storage->engaged)
        return 0;
    const auto holder = storage->value();
    if (holder->context->activeCalls != 0)
        return 0;
    if (!retireContext(holder->context)) {
        // A quiescent raw lua_close() may run off the owner thread. Registry
        // cleanup belongs to Lua here, but retained callbacks must be inert.
        holder->context->alive         = false;
        holder->context->state         = nullptr;
        holder->context->backendActive = false;
        holder->context->retiring      = true;
        holder->context->hooks         = {};
        holder->context->hostBackend.reset();
    }
    storage->engaged = false;
    std::destroy_at(holder);
    return 0;
}

std::shared_ptr<SLuaContext> Hyprtoolkit::Lua::findContext(lua_State* state) {
    if (!state)
        return {};
    lua_rawgetp(state, LUA_REGISTRYINDEX, &CONTEXT_KEY);
    if (const auto holder = testObject<SContextHolder>(state, -1, CONTEXT_METATABLE); holder) {
        auto result = holder->context;
        lua_pop(state, 1);
        return result;
    }
    lua_pop(state, 1);
    return {};
}

std::shared_ptr<SLuaContext> Hyprtoolkit::Lua::context(lua_State* state) {
    if (const auto existing = findContext(state)) {
        if (!existing->alive || existing->retiring)
            fail("bindings have been retired");
        if (existing->ownerThread != std::this_thread::get_id())
            fail("bindings must be used on their owner thread");
        return existing;
    }

    lua_rawgetp(state, LUA_REGISTRYINDEX, &CONTEXT_KEY);
    const bool finalized = !lua_isnil(state, -1);
    lua_pop(state, 1);
    if (finalized)
        fail("bindings have been retired");

    lua_rawgeti(state, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
    auto luaContext   = std::make_shared<SLuaContext>();
    luaContext->state = lua_tothread(state, -1);
    lua_pop(state, 1);
    if (luaL_newmetatable(state, CONTEXT_METATABLE.data())) {
        lua_pushcfunction(state, destroyContext);
        lua_setfield(state, -2, "__gc");
        lua_pushliteral(state, "hyprtoolkit.Context");
        lua_setfield(state, -2, "__metatable");
    }
    lua_pop(state, 1);

    pushObject(state, SContextHolder{.context = luaContext}, CONTEXT_METATABLE);
    lua_pushvalue(state, -1);
    lua_rawsetp(state, LUA_REGISTRYINDEX, &CONTEXT_KEY);
    lua_pop(state, 1);
    return luaContext;
}

void Hyprtoolkit::Lua::closeContext(lua_State* state) {
    if (!retireState(state))
        std::terminate();
}

std::expected<void, std::string> Hyprtoolkit::Lua::retireState(lua_State* state) {
    if (!state)
        return std::unexpected("Lua state is null");
    const auto existing = findContext(state);
    return existing ? retireContext(existing) : std::expected<void, std::string>{};
}

std::expected<void, std::string> Hyprtoolkit::Lua::setCallbackHooks(lua_State* state, SCallbackHooks hooks) {
    const auto existing = findContext(state);
    if (!existing || !existing->alive || existing->retiring)
        return std::unexpected("bindings are not active");
    if (existing->ownerThread != std::this_thread::get_id())
        return std::unexpected("callback hooks must be set on the owner thread");
    if (existing->activeCalls != 0)
        return std::unexpected("callback hooks cannot be changed from an executing callback");
    existing->hooks = std::move(hooks);
    return {};
}

static std::expected<std::shared_ptr<SLuaContext>, std::string> hostContext(lua_State* state, std::string_view action) {
    const auto existing = findContext(state);
    if (!existing || !existing->alive || existing->retiring)
        return std::unexpected("bindings are not active");
    if (existing->ownerThread != std::this_thread::get_id())
        return std::unexpected(std::format("backend must be {} on the owner thread", action));
    if (existing->activeCalls != 0)
        return std::unexpected(std::format("backend cannot be {} from an executing callback", action));
    return existing;
}

std::expected<void, std::string> Hyprtoolkit::Lua::attachBackend(lua_State* state, const SP<IBackend>& backend) {
    if (!backend)
        return std::unexpected("host backend is null");
    const auto existing = hostContext(state, "attached");
    if (!existing)
        return std::unexpected(existing.error());
    // a reserved state is in host mode without an active backend
    if ((*existing)->backendMode == eBackendMode::OWNED || (*existing)->backendActive)
        return std::unexpected("a backend has already been bound to this state");
    (*existing)->hostBackend   = backend;
    (*existing)->backendMode   = eBackendMode::HOST;
    (*existing)->backendActive = true;
    return {};
}

std::expected<void, std::string> Hyprtoolkit::Lua::reserveBackend(lua_State* state) {
    const auto existing = hostContext(state, "reserved");
    if (!existing)
        return std::unexpected(existing.error());
    if ((*existing)->backendMode == eBackendMode::OWNED)
        return std::unexpected("a backend has already been bound to this state");
    (*existing)->backendMode = eBackendMode::HOST;
    return {};
}

void Hyprtoolkit::Lua::requireBackendActive(lua_State* state) {
    const auto luaContext = context(state);
    if (!luaContext->backendActive && luaContext->backendMode == eBackendMode::HOST)
        fail("host backend is not available yet");
    if (!luaContext->backendActive)
        fail(luaContext->backendMode != eBackendMode::NONE ? "backend has been destroyed" : "a backend must be created first");
    if (luaContext->backendMode == eBackendMode::HOST && luaContext->hostBackend.expired())
        fail("host backend is no longer available");
}

CLuaCallback::CLuaCallback(lua_State* state, int index) : m_context(context(state)) {
    lua_pushvalue(state, lua_absindex(state, index));
    m_reference = luaL_ref(state, LUA_REGISTRYINDEX);
}

CLuaCallback::~CLuaCallback() {
    if (!m_context->alive || !m_context->state || m_reference == LUA_NOREF)
        return;
    if (m_context->ownerThread != std::this_thread::get_id())
        return;
    luaL_unref(m_context->state, LUA_REGISTRYINDEX, m_reference);
}

void CLuaCallback::release() {
    if (m_reference == LUA_NOREF)
        return;
    if (m_context->alive && m_context->state && m_context->ownerThread == std::this_thread::get_id())
        luaL_unref(m_context->state, LUA_REGISTRYINDEX, m_reference);
    m_reference = LUA_NOREF;
}

lua_State* CLuaCallback::state() const {
    return m_context->state;
}

bool CLuaCallback::beginCall() const {
    if (!m_context->alive || m_context->retiring || !m_context->state || m_reference == LUA_NOREF || m_context->ownerThread != std::this_thread::get_id())
        return false;
    if (m_context->backendMode == eBackendMode::HOST && m_context->hostBackend.expired())
        return false;
    lua_rawgeti(m_context->state, LUA_REGISTRYINDEX, m_reference);
    return true;
}

static void reportCallbackError(std::shared_ptr<SLuaContext> context, std::string_view message) {
    if (!context->alive || context->retiring || context->ownerThread != std::this_thread::get_id())
        return;
    ++context->activeCalls;
    const int                           top = lua_gettop(context->state);
    const Hyprutils::Utils::CScopeGuard finished([context, top] {
        lua_settop(context->state, top);
        --context->activeCalls;
    });
    if (context->hooks.reportError) {
        try {
            context->hooks.reportError(message);
            return;
        } catch (...) { /* Fall back to stderr if the host reporter fails. */
        }
    }
    std::fprintf(stderr, "hyprtoolkit-lua callback error: %.*s\n", static_cast<int>(message.size()), message.data());
}

bool CLuaCallback::finishCall(int argumentCount, int resultCount) const {
    // Lua may remove the callback which owns this object during the call.
    const auto context = m_context;
    if (!context->alive || !context->state)
        return false;
    ++context->activeCalls;
    const Hyprutils::Utils::CScopeGuard finished([context] { --context->activeCalls; });
    const int                           base   = lua_gettop(context->state) - argumentCount - 1;
    int                                 status = LUA_ERRRUN;
    try {
        status = context->hooks.protectedCall ? context->hooks.protectedCall(context->state, argumentCount, resultCount) : lua_pcall(context->state, argumentCount, resultCount, 0);
    } catch (const std::exception& error) {
        lua_settop(context->state, base);
        reportCallbackError(context, error.what());
        return false;
    } catch (...) {
        lua_settop(context->state, base);
        reportCallbackError(context, "unknown host callback exception");
        return false;
    }

    if (lua_gettop(context->state) != base + (status == LUA_OK ? resultCount : 1)) {
        lua_settop(context->state, base);
        reportCallbackError(context, "protected-call hook violated the Lua stack contract");
        return false;
    }
    if (status == LUA_OK)
        return true;

    size_t            length  = 0;
    const char*       error   = lua_tolstring(context->state, -1, &length);
    const std::string message = error ? std::string{error, length} : "non-string Lua error";
    lua_settop(context->state, base);
    reportCallbackError(context, message);
    return false;
}

void CLuaCallback::reportError(std::string_view message) const {
    reportCallbackError(m_context, message);
}

[[noreturn]] void Hyprtoolkit::Lua::fail(std::string message) {
    throw std::runtime_error(std::move(message));
}

void Hyprtoolkit::Lua::expectArguments(lua_State* state, int minimum, int maximum) {
    const int count = lua_gettop(state);
    if (count >= minimum && (maximum < 0 || count <= maximum))
        return;
    if (maximum < 0)
        fail(std::format("expected {} or more arguments, got {}", minimum, count));
    if (maximum == minimum)
        fail(std::format("expected {} arguments, got {}", minimum, count));
    fail(std::format("expected {} to {} arguments, got {}", minimum, maximum, count));
}

bool Hyprtoolkit::Lua::boolean(lua_State* state, int index) {
    if (!lua_isboolean(state, index))
        fail("argument " + std::to_string(index) + " must be a boolean");
    return lua_toboolean(state, index);
}

double Hyprtoolkit::Lua::number(lua_State* state, int index) {
    if (lua_type(state, index) != LUA_TNUMBER)
        fail("argument " + std::to_string(index) + " must be a number");
    const auto value = lua_tonumber(state, index);
    if (!std::isfinite(value))
        fail("argument " + std::to_string(index) + " must be finite");
    return value;
}

float Hyprtoolkit::Lua::floatNumber(lua_State* state, int index) {
    const auto value = number(state, index);
    if (value < std::numeric_limits<float>::lowest() || value > std::numeric_limits<float>::max())
        fail("argument " + std::to_string(index) + " is outside the float range");
    return sc<float>(value);
}

int Hyprtoolkit::Lua::intNumber(lua_State* state, int index) {
    const auto value = integer(state, index);
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
        fail("argument " + std::to_string(index) + " is outside the integer range");
    return sc<int>(value);
}

lua_Integer Hyprtoolkit::Lua::integer(lua_State* state, int index) {
    if (!lua_isinteger(state, index))
        fail("argument " + std::to_string(index) + " must be an integer");
    return lua_tointeger(state, index);
}

std::string Hyprtoolkit::Lua::string(lua_State* state, int index) {
    if (lua_type(state, index) != LUA_TSTRING)
        fail("argument " + std::to_string(index) + " must be a string");
    size_t      length = 0;
    const char* value  = lua_tolstring(state, index, &length);
    return {value, length};
}

bool Hyprtoolkit::Lua::isNoneOrNil(lua_State* state, int index) {
    return lua_isnoneornil(state, index);
}

template <typename T>
static void trackReference(std::vector<T>& references, T reference) {
    if (references.size() == references.capacity()) {
        std::erase_if(references, [](const auto& weak) { return weak.expired(); });
        references.reserve(std::max<size_t>(8, references.size() * 2));
    }
    references.emplace_back(std::move(reference));
}

LuaCallback Hyprtoolkit::Lua::callback(lua_State* state, int index, bool optional) {
    if (optional && lua_isnoneornil(state, index))
        return {};
    if (!lua_isfunction(state, index))
        fail("argument " + std::to_string(index) + " must be a function");
    const auto luaContext = context(state);
    auto       result     = std::make_shared<CLuaCallback>(state, index);
    trackReference(luaContext->callbacks, std::weak_ptr<CLuaCallback>{result});
    return result;
}

void Hyprtoolkit::Lua::registerMetatable(lua_State* state, std::string_view name, const luaL_Reg* methods, lua_CFunction gc) {
    if (!luaL_newmetatable(state, name.data())) {
        lua_pop(state, 1);
        return;
    }
    lua_pushlstring(state, name.data(), name.size());
    lua_pushcclosure(state, gc, 1);
    lua_setfield(state, -2, "__gc");

    lua_newtable(state);
    for (const luaL_Reg* method = methods; method->name; ++method) {
        lua_pushcfunction(state, method->func);
        if (method->name[0] == '_' && method->name[1] == '_')
            lua_setfield(state, -3, method->name);
        else
            lua_setfield(state, -2, method->name);
    }
    lua_setfield(state, -2, "__methods");

    lua_getfield(state, -1, "__index");
    const bool customIndex = !lua_isnil(state, -1);
    lua_pop(state, 1);
    if (!customIndex) {
        lua_getfield(state, -1, "__methods");
        lua_setfield(state, -2, "__index");
    }

    lua_pushlstring(state, name.data(), name.size());
    lua_setfield(state, -2, "__metatable");
    lua_pop(state, 1);
}

void Hyprtoolkit::Lua::setFunction(lua_State* state, int table, const char* name, lua_CFunction function) {
    table = lua_absindex(state, table);
    lua_pushcfunction(state, function);
    lua_setfield(state, table, name);
}

void Hyprtoolkit::Lua::setInteger(lua_State* state, int table, const char* name, lua_Integer value) {
    table = lua_absindex(state, table);
    lua_pushinteger(state, value);
    lua_setfield(state, table, name);
}

void Hyprtoolkit::Lua::setClass(lua_State* state, int module, const char* name, const luaL_Reg* functions) {
    module = lua_absindex(state, module);
    lua_newtable(state);
    luaL_setfuncs(state, functions, 0);
    lua_setfield(state, module, name);
}

void Hyprtoolkit::Lua::pushConnection(lua_State* state, Hyprutils::Signal::CHyprSignalListener listener) {
    const auto luaContext = context(state);
    auto       connection = makeShared<SConnection>(SConnection{.listener = std::move(listener)});
    trackReference(luaContext->connections, WP<SConnection>{connection});
    pushObject(state, std::move(connection), MT_CONNECTION);
}

static int disconnect(lua_State* state) {
    object<SP<SConnection>>(state, 1, MT_CONNECTION)->listener.reset();
    return 0;
}

static int connected(lua_State* state) {
    lua_pushboolean(state, !!object<SP<SConnection>>(state, 1, MT_CONNECTION)->listener);
    return 1;
}

static constexpr luaL_Reg CONNECTION_METHODS[] = {
    {"disconnect", protect<disconnect>},
    {"connected", protect<connected>},
    {nullptr, nullptr},
};

void Hyprtoolkit::Lua::registerAllBindings(lua_State* state) {
    createModule(state);
    luaL_getsubtable(state, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    lua_pushvalue(state, -2);
    lua_setfield(state, -2, "hyprtoolkit");
    lua_pop(state, 1);
    lua_setglobal(state, "hyprtoolkit");
}

int Hyprtoolkit::Lua::createModule(lua_State* state) {
    context(state);
    registerMetatable(state, MT_CONNECTION, CONNECTION_METHODS, destroyObject<SP<SConnection>>);

    lua_newtable(state);
    const int module = lua_gettop(state);
    registerTypes(state, module);
    registerElements(state, module);
    registerElementBuilders(state, module);
    registerCore(state, module);
    registerWindow(state, module);

    lua_pushliteral(state, HYPRTOOLKIT_LUA_VERSION);
    lua_setfield(state, module, "version");
    return 1;
}

int Hyprtoolkit::Lua::openModule(lua_State* state) noexcept {
    return protect<createModule>(state);
}

SP<CLuaState> Hyprtoolkit::Lua::createLuaState() {
    return makeShared<CLuaState>();
}
