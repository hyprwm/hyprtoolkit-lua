#include "Binding.hpp"

#include <hyprtoolkit-lua/Embedding.hpp>
#include <hyprtoolkit-lua/LuaBindings.hpp>
#include <hyprutils/signal/Signal.hpp>

#include <iostream>

using namespace Hyprtoolkit::Lua;
using namespace Hyprtoolkit;

class CTestBackend : public IBackend {
  public:
    explicit CTestBackend(int& destroyed) : m_destroyed(destroyed) {
        ;
    }
    void destroy() override {
        ++m_destroyed;
    }
    void setLogFn(LogFn&&) override {
        ;
    }
    void addFd(int fd, std::function<void()>&& callback) override {
        m_addedFds.emplace_back(fd, std::move(callback));
    }
    void removeFd(int fd) override {
        m_removedFds.emplace_back(fd);
    }
    SP<ISystemIconFactory> systemIcons() override {
        return {};
    }
    TimerPointer addTimer(const TimerDuration&, std::function<void(TimerPointer, void*)>, void*, bool) override {
        return {};
    }
    void addIdle(const std::function<void()>&) override {
        ;
    }
    void enterLoop() override {
        ;
    }
    SP<CPalette> getPalette() override {
        return {};
    }
    std::vector<SP<IOutput>> getOutputs() override {
        return {};
    }
    std::expected<SP<ISessionLockState>, eSessionLockError> aquireSessionLock() override {
        return std::unexpected(LOCK_ERROR_PLATFORM_UNINITIALIZED);
    }

    std::vector<std::pair<int, std::function<void()>>> m_addedFds;
    std::vector<int>                                   m_removedFds;

  private:
    int& m_destroyed;
};

class CTestElement : public IElement {
  public:
    void paint() override {
        ;
    }
    Hyprutils::Math::Vector2D size() override {
        return {4, 4};
    }
};

static void check(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string{message});
}

static void run(lua_State* state, const char* code) {
    if (luaL_dostring(state, code) == LUA_OK)
        return;
    const std::string message = lua_tostring(state, -1);
    lua_pop(state, 1);
    throw std::runtime_error(message);
}

static LuaCallback readCallback(lua_State* state, const char* code) {
    run(state, code);
    auto result = callback(state, -1);
    lua_pop(state, 1);
    return result;
}

static void retirement() {
    LuaCallback retained;
    {
        CLuaState lua;
        lua.openLibs();
        registerAllBindings(lua.state());
        retained = readCallback(lua.state(), R"(
            weak = setmetatable({}, { __mode = "v" })
            local captured = {}
            weak[1] = captured
            return function() return captured end
        )");
        run(lua.state(), "collectgarbage('collect'); assert(weak[1])");
        check(retained->beginCall(), "live callback must execute");
        check(retained->finishCall(0, 1), "callback result");
        lua_pop(lua.state(), 1);

        check(retireState(lua.state()).has_value(), "retire state");
        check(retireState(lua.state()).has_value(), "retirement is idempotent");
        check(!retained->beginCall(), "retired callback must not execute");
        run(lua.state(), R"(
            collectgarbage('collect')
            assert(weak[1] == nil)
            assert(not pcall(function() hyprtoolkit.Backend.create() end))
        )");
        check(lua_gettop(lua.state()) == 0, "retirement preserves the stack");
    }
    check(!retained->beginCall(), "callback must remain inert after lua_close");
}

static void signalRetirement() {
    CLuaState lua;
    lua.openLibs();
    registerAllBindings(lua.state());
    Hyprutils::Signal::CSignalT<> signal;
    const auto                    cb = readCallback(lua.state(), R"(
        calls = 0
        weak = setmetatable({}, { __mode = "v" })
        local connection
        function setConnection(value) connection = value; weak[1] = value end
        return function() calls = calls + 1; return connection end
    )");
    lua_getglobal(lua.state(), "setConnection");
    pushConnection(lua.state(), signal.listen([cb] {
        if (cb->beginCall())
            cb->finishCall(0);
    }));
    check(lua_pcall(lua.state(), 1, 0, 0) == LUA_OK, "capture connection");
    run(lua.state(), "setConnection = nil; collectgarbage('collect'); assert(weak[1]:connected())");
    signal.emit();
    run(lua.state(), "assert(calls == 1)");
    check(retireState(lua.state()).has_value(), "retire signal connection");
    signal.emit();
    run(lua.state(), "collectgarbage('collect'); assert(weak[1] == nil); assert(calls == 1)");
}

static void retirementDuringCallback() {
    CLuaState lua;
    lua.openLibs();
    registerAllBindings(lua.state());
    lua_pushcfunction(lua.state(), [](lua_State* state) {
        lua_pushboolean(state, !retireState(state).has_value());
        return 1;
    });
    lua_setglobal(lua.state(), "tryRetire");
    const auto cb = readCallback(lua.state(), "return function() assert(tryRetire()) end");
    check(cb->beginCall() && cb->finishCall(0), "reentrant retirement is rejected");
    check(cb->beginCall() && cb->finishCall(0), "rejected retirement leaves context active");
    check(retireState(lua.state()).has_value(), "quiescent retirement succeeds");
}

static void rawStateClose() {
    auto* state = luaL_newstate();
    check(state != nullptr, "create external Lua state");
    registerAllBindings(state);
    const auto cb = readCallback(state, "return function() end");
    lua_close(state);
    check(!cb->beginCall(), "GC fallback retires externally owned states");
}

static void rawStateCloseOnAnotherThread() {
    auto* state = luaL_newstate();
    check(state != nullptr, "create external Lua state");
    registerAllBindings(state);
    const auto  cb                 = readCallback(state, "return function() end");
    bool        retirementRejected = false;
    std::thread closer([state, &retirementRejected] {
        retirementRejected = !retireState(state).has_value();
        lua_close(state);
    });
    closer.join();
    check(retirementRejected, "explicit retirement must use the owner thread");
    check(!cb->beginCall() && cb->state() == nullptr, "raw close invalidates retained callbacks even off-thread");
}

static void callbackHooks() {
    CLuaState lua;
    lua.openLibs();
    registerAllBindings(lua.state());
    int                      calls = 0;
    std::vector<std::string> errors;
    check(setCallbackHooks(lua.state(),
                           {
                               .protectedCall =
                                   [&calls](lua_State* state, int arguments, int results) {
                                       ++calls;
                                       return lua_pcall(state, arguments, results, 0);
                                   },
                               .reportError =
                                   [&errors, &lua](std::string_view error) {
                                       check(!retireState(lua.state()), "retirement from error hooks must be rejected");
                                       check(!setCallbackHooks(lua.state(), {}), "changing executing hooks must be rejected");
                                       errors.emplace_back(error);
                                   },
                           })
              .has_value(),
          "install callback hooks");

    const auto value = readCallback(lua.state(), "return function(n) return n + 1 end");
    check(value->beginCall(), "begin value callback");
    lua_pushinteger(lua.state(), 41);
    check(value->finishCall(1, 1), "host hook preserves callback results");
    check(lua_tointeger(lua.state(), -1) == 42, "callback result is preserved");
    lua_pop(lua.state(), 1);

    for (const auto code : {"return function() error('test error') end", "return function() error({}) end"}) {
        const auto failing = readCallback(lua.state(), code);
        check(failing->beginCall() && !failing->finishCall(0), "Lua errors are contained");
        check(lua_gettop(lua.state()) == 0, "error callback restores stack");
    }
    check(calls == 3 && errors.size() == 2, "all calls and errors use host hooks");
    check(errors[0].contains("test error") && errors[1] == "non-string Lua error", "error contents reach reporter");

    check(setCallbackHooks(lua.state(),
                           {
                               .protectedCall = [](lua_State*, int, int) -> int { throw std::runtime_error("host failure"); },
                               .reportError   = [&errors](std::string_view error) { errors.emplace_back(error); },
                           })
              .has_value(),
          "replace quiescent hooks");
    lua_pushinteger(lua.state(), 99);
    check(value->beginCall() && !value->finishCall(0), "host exceptions are contained");
    check(lua_gettop(lua.state()) == 1 && lua_tointeger(lua.state(), -1) == 99, "host failure preserves stack prefix");
    lua_pop(lua.state(), 1);
    check(errors.back() == "host failure", "host exception is reported");

    check(setCallbackHooks(lua.state(),
                           {
                               .protectedCall = [](lua_State*, int, int) { return LUA_OK; },
                               .reportError   = [&errors](std::string_view error) { errors.emplace_back(error); },
                           })
              .has_value(),
          "install invalid hook");
    check(value->beginCall() && !value->finishCall(0), "invalid host stack effects are rejected");
    check(lua_gettop(lua.state()) == 0 && errors.back().contains("stack contract"), "invalid hook restores the stack");

    check(setCallbackHooks(lua.state(), {}).has_value(), "restore standalone defaults");
    const auto empty = readCallback(lua.state(), "return function() end");
    check(empty->beginCall() && empty->finishCall(0), "default callback execution remains available");
}

static void nestedCallbackHooks() {
    CLuaState lua;
    lua.openLibs();
    registerAllBindings(lua.state());
    int        calls = 0;
    const auto inner = readCallback(lua.state(), "return function() end");
    const auto outer = readCallback(lua.state(), "return function() return 7 end");
    check(setCallbackHooks(lua.state(),
                           {
                               .protectedCall =
                                   [&calls, inner](lua_State* state, int arguments, int results) {
                                       if (++calls == 1)
                                           check(inner->beginCall() && inner->finishCall(0), "nested callback succeeds");
                                       return lua_pcall(state, arguments, results, 0);
                                   },
                           })
              .has_value(),
          "install nesting hook");
    check(outer->beginCall() && outer->finishCall(0, 1), "outer callback succeeds");
    check(calls == 2 && lua_tointeger(lua.state(), -1) == 7, "nested invocation preserves outer stack");
    lua_pop(lua.state(), 1);
}

static void hostWatchdog() {
    CLuaState lua;
    lua.openLibs();
    registerAllBindings(lua.state());
    std::string error;
    check(setCallbackHooks(lua.state(),
                           {
                               .protectedCall =
                                   [](lua_State* state, int arguments, int results) {
                                       lua_sethook(state, [](lua_State* state, lua_Debug*) { luaL_error(state, "host watchdog"); }, LUA_MASKCOUNT, 100);
                                       const int result = lua_pcall(state, arguments, results, 0);
                                       lua_sethook(state, nullptr, 0, 0);
                                       return result;
                                   },
                               .reportError = [&error](std::string_view message) { error = message; },
                           })
              .has_value(),
          "install host watchdog");
    const auto endless = readCallback(lua.state(), "return function() while true do end end");
    check(endless->beginCall() && !endless->finishCall(0), "host watchdog interrupts Lua");
    check(error.contains("host watchdog") && lua_gettop(lua.state()) == 0, "watchdog errors are contained");
}

static void hookLifetime() {
    CLuaState lua;
    registerAllBindings(lua.state());
    auto                     marker = std::make_shared<int>(0);
    const std::weak_ptr<int> weak   = marker;
    check(setCallbackHooks(lua.state(),
                           {
                               .reportError = [marker](std::string_view) { ++*marker; },
                           })
              .has_value(),
          "install retained host hook");
    marker.reset();
    check(!weak.expired(), "context retains host hook");
    check(retireState(lua.state()).has_value(), "retire host hooks");
    check(weak.expired(), "retirement releases host captures before lua_close");
    check(!setCallbackHooks(lua.state(), {}), "retired context rejects new hooks");
}

static void selfRemovingCallback() {
    CLuaState lua;
    lua.openLibs();
    registerAllBindings(lua.state());
    LuaCallback current;
    lua_pushlightuserdata(lua.state(), &current);
    lua_pushcclosure(
        lua.state(),
        [](lua_State* state) {
            sc<LuaCallback*>(lua_touserdata(state, lua_upvalueindex(1)))->reset();
            return 0;
        },
        1);
    lua_setglobal(lua.state(), "removeCallback");
    int errors = 0;
    check(setCallbackHooks(lua.state(),
                           {
                               .reportError = [&errors](std::string_view) { ++errors; },
                           })
              .has_value(),
          "install self-removal reporter");
    current = readCallback(lua.state(), "return function() removeCallback() end");
    check(current->beginCall() && current->finishCall(0), "self-removing callback succeeds");
    check(!current, "callback was destroyed during its own invocation");
    current = readCallback(lua.state(), "return function() removeCallback(); error('removed') end");
    check(current->beginCall() && !current->finishCall(0), "self-removing callback errors are contained");
    check(!current && errors == 1 && lua_gettop(lua.state()) == 0, "error handling survives callback destruction");
}

static void hostAttachment() {
    int        destroyed = 0;
    const auto backend   = makeShared<CTestBackend>(destroyed);
    {
        CLuaState first;
        CLuaState second;
        first.openLibs();
        second.openLibs();
        check(!attachBackend(first.state(), backend), "attachment requires registered bindings");
        registerAllBindings(first.state());
        registerAllBindings(second.state());
        check(!attachBackend(first.state(), {}), "null backend is rejected");
        check(attachBackend(first.state(), backend).has_value(), "attach host backend");
        check(attachBackend(second.state(), backend).has_value(), "another state can borrow the backend");
        check(!attachBackend(first.state(), backend), "duplicate attachment is rejected");
        check(!attachBackend(first.state(), makeShared<CTestBackend>(destroyed)), "conflicting attachment is rejected");
        run(first.state(), R"(
            local backend, err = hyprtoolkit.Backend.create()
            assert(backend == nil and err:find("owned by the host"))
            local ok, windowErr = pcall(function() hyprtoolkit.WindowBuilder.begin():commence() end)
            assert(not ok and windowErr:find("owned by the host"))
            collectgarbage("collect")
        )");
        check(retireState(first.state()).has_value(), "retire one attachment");
        check(!attachBackend(first.state(), backend), "retired state cannot reattach");
        requireBackendActive(second.state());
        check(destroyed == 0, "retiring one state leaves the host backend intact");
    }
    check(destroyed == 0, "closing borrower states never destroys the host backend");
    backend->destroy();
    check(destroyed == 1, "host remains responsible for backend shutdown");
}

static void reservedHostBackend() {
    int        destroyed = 0;
    const auto backend   = makeShared<CTestBackend>(destroyed);
    CLuaState  lua;
    lua.openLibs();
    check(!reserveBackend(lua.state()), "reservation requires registered bindings");
    registerAllBindings(lua.state());
    check(reserveBackend(lua.state()).has_value(), "reserve host backend");
    check(reserveBackend(lua.state()).has_value(), "reservation is idempotent");
    run(lua.state(), R"(
        local backend, err = hyprtoolkit.Backend.create()
        assert(backend == nil and err:find("owned by the host"))
        local ok, elementErr = pcall(function() hyprtoolkit.RectangleBuilder.begin():commence() end)
        assert(not ok and elementErr:find("not available yet"))
    )");
    check(attachBackend(lua.state(), backend).has_value(), "attach reserved backend");
    check(!attachBackend(lua.state(), backend), "reserved state attaches only once");
    requireBackendActive(lua.state());
}

static void expiredHostBackend() {
    CLuaState lua;
    lua.openLibs();
    registerAllBindings(lua.state());
    int  destroyed = 0;
    auto backend   = makeShared<CTestBackend>(destroyed);
    check(attachBackend(lua.state(), backend).has_value(), "attach temporary backend");
    const auto cb = readCallback(lua.state(), "return function() end");
    backend.reset();
    run(lua.state(), R"(
        local ok, err = pcall(function() hyprtoolkit.RectangleBuilder.begin():commence() end)
        assert(not ok and err:find("no longer available"))
    )");
    check(!cb->beginCall(), "expired backend suppresses toolkit callbacks");
}

static void ownedBackendRemainsOwned() {
    int        destroyed = 0;
    const auto backend   = makeShared<CTestBackend>(destroyed);
    {
        CLuaState lua;
        registerAllBindings(lua.state());
        const auto ctx     = context(lua.state());
        ctx->backendMode   = eBackendMode::OWNED;
        ctx->backendActive = true;
        pushObject(lua.state(), SBackend{.backend = backend, .context = ctx}, MT_BACKEND);
        lua_setglobal(lua.state(), "backend");
        check(!attachBackend(lua.state(), backend), "host attachment cannot replace an owned backend");
        check(!reserveBackend(lua.state()), "host reservation cannot replace an owned backend");
    }
    check(destroyed == 1, "standalone backend GC still destroys its backend");
}

static void elementExchange() {
    CLuaState lua;
    lua.openLibs();
    check(!getElement(lua.state(), 1), "unregistered state rejects extraction");
    check(!getElement(nullptr, 1), "null state rejects extraction");
    registerAllBindings(lua.state());
    int        destroyed = 0;
    const auto backend   = makeShared<CTestBackend>(destroyed);
    check(attachBackend(lua.state(), backend).has_value(), "attach element host");
    auto               element = makeShared<CTestElement>();
    const WP<IElement> weak    = element;
    pushElement(lua.state(), element);
    lua_pushinteger(lua.state(), 42);
    const int top = lua_gettop(lua.state());
    check(getElement(lua.state(), 1).value() == element, "element identity survives exchange");
    check(getElement(lua.state(), -2).value() == element, "negative stack index works");
    check(!getElement(lua.state(), -1), "wrong userdata type is rejected");
    check(!getElement(lua.state(), top + 1), "missing stack slot is rejected");
    check(lua_gettop(lua.state()) == top, "extraction preserves stack");
    pushElement(lua.state(), {});
    check(lua_isnil(lua.state(), -1) && !getElement(lua.state(), -1).value(), "null element roundtrip");
    element.reset();
    check(!weak.expired(), "Lua userdata retains the element");
    lua_settop(lua.state(), 0);
    lua_gc(lua.state(), LUA_GCCOLLECT, 0);
    check(weak.expired(), "element is released when Lua userdata is collected");

    element = makeShared<CTestElement>();
    pushElement(lua.state(), element);
    lua_pushvalue(lua.state(), -1);
    lua_setglobal(lua.state(), "element");
    run(lua.state(), "debug.getmetatable(element).__gc(element)");
    check(!getElement(lua.state(), -1), "finalized userdata is rejected");
    lua_pop(lua.state(), 1);
    pushElement(lua.state(), element);
    check(retireState(lua.state()).has_value(), "retire element state");
    check(!getElement(lua.state(), -1), "retired state rejects retained userdata");
    bool rejected = false;
    try {
        pushElement(lua.state(), element);
    } catch (const std::exception&) { rejected = true; }
    check(rejected && lua_gettop(lua.state()) == 1, "retired state rejects push without changing stack");
}

static void fileDescriptorCallbacks() {
    int        destroyed = 0;
    const auto backend   = makeShared<CTestBackend>(destroyed);
    CLuaState  lua;
    lua.openLibs();
    registerAllBindings(lua.state());
    const auto ctx     = context(lua.state());
    ctx->backendMode   = eBackendMode::OWNED;
    ctx->backendActive = true;
    pushObject(lua.state(), SBackend{.backend = backend, .context = ctx}, MT_BACKEND);
    lua_setglobal(lua.state(), "backend");

    run(lua.state(), R"(
        calls = 0
        backend:addFd(42, function() calls = calls + 1 end)
        backend:removeFd(42)

        testFile = assert(io.tmpfile())
        backend:addFd(testFile, function() calls = calls + 10 end)
        backend:removeFd(testFile)

        local closed = assert(io.tmpfile())
        closed:close()
        assert(not pcall(function() backend:addFd(closed, function() end) end))
        assert(not pcall(function() backend:removeFd(closed) end))
        assert(not pcall(function() backend:addFd(-1, function() end) end))
        assert(not pcall(function() backend:addFd("bad", function() end) end))
        assert(not pcall(function() backend:removeFd("bad") end))
    )");

    lua_getglobal(lua.state(), "testFile");
    const auto* stream = sc<const luaL_Stream*>(luaL_testudata(lua.state(), -1, LUA_FILEHANDLE));
    check(stream != nullptr && stream->f != nullptr, "testFile is a valid Lua file handle");
    const int expectedFd = ::fileno(stream->f);
    lua_pop(lua.state(), 1);

    check(backend->m_addedFds.size() == 2, "addFd recorded integer and file handle");
    check(backend->m_addedFds[0].first == 42, "integer fd passed through");
    check(backend->m_addedFds[1].first == expectedFd, "file handle fileno passed through");
    check(backend->m_removedFds.size() == 2 && backend->m_removedFds[0] == 42 && backend->m_removedFds[1] == expectedFd, "removeFd recorded integer and file handle");

    backend->m_addedFds[0].second();
    backend->m_addedFds[1].second();
    run(lua.state(), "assert(calls == 11); testFile:close()");
}

int main() {
    try {
        fileDescriptorCallbacks();
        retirement();
        signalRetirement();
        retirementDuringCallback();
        rawStateClose();
        rawStateCloseOnAnotherThread();
        callbackHooks();
        nestedCallbackHooks();
        hostWatchdog();
        hookLifetime();
        selfRemovingCallback();
        hostAttachment();
        reservedHostBackend();
        expiredHostBackend();
        ownedBackendRemainsOwned();
        elementExchange();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
