#include "../Binding.hpp"

#include <chrono>
#include <string>
#include <utility>

using namespace Hyprtoolkit;
using namespace Hyprtoolkit::Lua;
using namespace Hyprutils::Memory;

template <typename T>
static SP<T> sharedObject(lua_State* state, int index, std::string_view metatable) {
    if (metatable != MT_PALETTE)
        requireBackendActive(state);
    const auto value = object<SP<T>>(state, index, metatable);
    if (!value)
        fail("argument " + std::to_string(index) + " is an empty " + std::string{metatable});
    return value;
}

static STimer& timerHandle(lua_State* state, int index) {
    auto& value = object<STimer>(state, index, MT_TIMER);
    if (!value.timer)
        fail("argument " + std::to_string(index) + " is an empty " + std::string{MT_TIMER});
    return value;
}

static TimerPointer timerObject(lua_State* state, int index) {
    return timerHandle(state, index).timer;
}

static void releasePending(const std::shared_ptr<SPendingCallback>& pending) {
    if (!pending || !pending->callback)
        return;
    pending->callback->release();
    pending->callback.reset();
}

static void releasePending(SBackend& backend) {
    for (const auto& weak : backend.pendingCallbacks)
        releasePending(weak.lock());
    backend.pendingCallbacks.clear();
}

static void trackPending(SBackend& backend, const std::shared_ptr<SPendingCallback>& pending) {
    std::erase_if(backend.pendingCallbacks, [](const auto& weak) {
        const auto existing = weak.lock();
        return !existing || !existing->callback;
    });
    backend.pendingCallbacks.emplace_back(pending);
}

static SBackend& backendHandle(lua_State* state, bool requireActive = true) {
    if (requireActive)
        requireBackendActive(state);
    auto& handle = object<SBackend>(state, 1, MT_BACKEND);
    if (requireActive && (!handle.active || !handle.backend))
        fail("backend has been destroyed");
    return handle;
}

static SP<IBackend> backendObject(lua_State* state) {
    return backendHandle(state).backend;
}

static std::chrono::milliseconds milliseconds(lua_State* state, int index) {
    const auto value = integer(state, index);
    if (value < 0)
        fail("argument " + std::to_string(index) + " must be non-negative");
    return std::chrono::milliseconds{value};
}

static int checkedInt(lua_State* state, int index) {
    return intNumber(state, index);
}

static int fileDescriptor(lua_State* state, int index) {
    if (lua_isinteger(state, index)) {
        const auto fd = intNumber(state, index);
        if (fd < 0)
            fail("argument " + std::to_string(index) + " must be a non-negative file descriptor");
        return fd;
    }
    if (const auto* stream = sc<const luaL_Stream*>(luaL_testudata(state, index, LUA_FILEHANDLE))) {
        if (!stream->f || !stream->closef)
            fail("argument " + std::to_string(index) + " is a closed file");
        const int fd = ::fileno(stream->f);
        if (fd < 0)
            fail("argument " + std::to_string(index) + " has no valid file descriptor");
        return fd;
    }
    fail("argument " + std::to_string(index) + " must be a file descriptor or file handle");
}

static void pushString(lua_State* state, const std::string& value) {
    lua_pushlstring(state, value.data(), value.size());
}

void Hyprtoolkit::Lua::pushOutput(lua_State* state, SP<IOutput> output) {
    if (!output) {
        lua_pushnil(state);
        return;
    }
    pushObject(state, std::move(output), MT_OUTPUT);
}

void Hyprtoolkit::Lua::pushIcon(lua_State* state, SP<ISystemIconDescription> icon) {
    if (!icon) {
        lua_pushnil(state);
        return;
    }
    pushObject(state, std::move(icon), MT_ICON);
}

static int backendCreate(lua_State* state) {
    expectArguments(state, 0, 0);
    const auto luaContext = context(state);
    if (luaContext->backendMode == eBackendMode::HOST) {
        lua_pushnil(state);
        lua_pushliteral(state, "backend is owned by the host");
        return 2;
    }
    if (luaContext->backendMode != eBackendMode::NONE) {
        lua_pushnil(state);
        lua_pushliteral(state, "a backend cannot be recreated in the same Lua state");
        return 2;
    }
    const auto backend = IBackend::create();
    if (!backend) {
        lua_pushnil(state);
        lua_pushliteral(state, "backend is unavailable");
        return 2;
    }

    luaContext->backendMode   = eBackendMode::OWNED;
    luaContext->backendActive = true;
    pushObject(state, SBackend{.backend = backend, .context = luaContext}, MT_BACKEND);
    return 1;
}

static int backendDestroy(lua_State* state) {
    expectArguments(state, 1, 1);
    auto& handle = backendHandle(state, false);
    if (!handle.active)
        return 0;
    const auto backend = handle.backend;
    handle.active      = false;
    if (const auto luaContext = handle.context.lock())
        luaContext->backendActive = false;
    releasePending(handle);
    handle.backend.reset();
    backend->destroy();
    return 0;
}

static int backendEnterLoop(lua_State* state) {
    expectArguments(state, 1, 1);
    backendObject(state)->enterLoop();
    return 0;
}

static int backendAddTimer(lua_State* state) {
    expectArguments(state, 3, 4);
    auto&      handle  = backendHandle(state);
    const auto backend = handle.backend;
    const auto timeout = milliseconds(state, 2);
    const auto pending = std::make_shared<SPendingCallback>(SPendingCallback{.callback = callback(state, 3)});
    const bool force   = lua_gettop(state) == 4 ? boolean(state, 4) : false;

    auto       timer = backend->addTimer(
        timeout,
        [pending](TimerPointer self, void*) {
            const auto cb = pending->callback;
            if (cb && cb->beginCall()) {
                pushObject(cb->state(), STimer{.timer = std::move(self), .pending = pending}, MT_TIMER);
                cb->finishCall(1);
            }
            releasePending(pending);
        },
        nullptr, force);

    if (!timer) {
        lua_pushnil(state);
        return 1;
    }
    trackPending(handle, pending);
    pushObject(state, STimer{.timer = std::move(timer), .pending = std::move(pending)}, MT_TIMER);
    return 1;
}

static int backendAddIdle(lua_State* state) {
    expectArguments(state, 2, 2);
    auto&      handle  = backendHandle(state);
    const auto pending = std::make_shared<SPendingCallback>(SPendingCallback{.callback = callback(state, 2)});
    handle.backend->addIdle([pending] {
        const auto cb = pending->callback;
        if (cb && cb->beginCall())
            cb->finishCall(0);
        releasePending(pending);
    });
    trackPending(handle, pending);
    return 0;
}

static int backendAddFd(lua_State* state) {
    expectArguments(state, 3, 3);
    auto&      handle  = backendHandle(state);
    const int  fd      = fileDescriptor(state, 2);
    const auto pending = std::make_shared<SPendingCallback>(SPendingCallback{.callback = callback(state, 3)});
    handle.backend->addFd(fd, [pending] {
        const auto cb = pending->callback;
        if (cb && cb->beginCall())
            cb->finishCall(0);
    });
    trackPending(handle, pending);
    return 0;
}

static int backendRemoveFd(lua_State* state) {
    expectArguments(state, 2, 2);
    backendObject(state)->removeFd(fileDescriptor(state, 2));
    return 0;
}

static int backendSetLogFn(lua_State* state) {
    expectArguments(state, 2, 2);
    auto& handle = backendHandle(state);

    if (lua_isnil(state, 2)) {
        handle.backend->setLogFn({});
        releasePending(handle.logCallback);
        handle.logCallback.reset();
        return 0;
    }

    const auto pending = std::make_shared<SPendingCallback>(SPendingCallback{.callback = callback(state, 2)});
    handle.backend->setLogFn([pending](eLogLevel level, const std::string& message) {
        const auto cb = pending->callback;
        if (!cb || !cb->beginCall())
            return;
        lua_pushinteger(cb->state(), sc<lua_Integer>(level));
        pushString(cb->state(), message);
        cb->finishCall(2);
    });
    releasePending(handle.logCallback);
    handle.logCallback = pending;
    trackPending(handle, pending);
    return 0;
}

static int backendSystemIcons(lua_State* state) {
    expectArguments(state, 1, 1);
    const auto factory = backendObject(state)->systemIcons();
    if (!factory) {
        lua_pushnil(state);
        return 1;
    }
    pushObject(state, factory, MT_ICON_FACTORY);
    return 1;
}

static int backendGetPalette(lua_State* state) {
    expectArguments(state, 1, 1);
    const auto palette = backendObject(state)->getPalette();
    if (!palette) {
        lua_pushnil(state);
        return 1;
    }
    pushObject(state, palette, MT_PALETTE);
    return 1;
}

static int backendGetOutputs(lua_State* state) {
    expectArguments(state, 1, 1);
    const auto outputs = backendObject(state)->getOutputs();
    lua_createtable(state, 0, 0);
    lua_Integer index = 1;
    for (const auto& output : outputs) {
        pushOutput(state, output);
        lua_rawseti(state, -2, index++);
    }
    return 1;
}

static int backendAcquireSessionLock(lua_State* state) {
    expectArguments(state, 1, 1);
    auto result = backendObject(state)->aquireSessionLock();
    if (!result) {
        lua_pushnil(state);
        lua_pushinteger(state, sc<lua_Integer>(result.error()));
        return 2;
    }

    pushObject(state, std::move(result.value()), MT_SESSION_LOCK);
    return 1;
}

static int backendOnOutputAdded(lua_State* state) {
    expectArguments(state, 2, 2);
    const auto backend = backendObject(state);
    const auto cb      = callback(state, 2);
    pushConnection(state, backend->m_events.outputAdded.listen([cb](SP<IOutput> output) {
        if (!cb->beginCall())
            return;
        pushOutput(cb->state(), output);
        cb->finishCall(1);
    }));
    return 1;
}

static int timerCancel(lua_State* state) {
    expectArguments(state, 1, 1);
    auto& handle = timerHandle(state, 1);
    handle.timer->cancel();
    releasePending(handle.pending);
    return 0;
}

static int timerCancelled(lua_State* state) {
    expectArguments(state, 1, 1);
    lua_pushboolean(state, timerObject(state, 1)->cancelled());
    return 1;
}

static int timerPassed(lua_State* state) {
    expectArguments(state, 1, 1);
    lua_pushboolean(state, timerObject(state, 1)->passed());
    return 1;
}

static int timerCanForceUpdate(lua_State* state) {
    expectArguments(state, 1, 1);
    lua_pushboolean(state, timerObject(state, 1)->canForceUpdate());
    return 1;
}

static int timerUpdateTimeout(lua_State* state) {
    expectArguments(state, 2, 2);
    timerObject(state, 1)->updateTimeout(milliseconds(state, 2));
    return 0;
}

static int timerLeftMs(lua_State* state) {
    expectArguments(state, 1, 1);
    lua_pushnumber(state, timerObject(state, 1)->leftMs());
    return 1;
}

static int outputHandle(lua_State* state) {
    expectArguments(state, 1, 1);
    lua_pushinteger(state, sharedObject<IOutput>(state, 1, MT_OUTPUT)->handle());
    return 1;
}

static int outputPort(lua_State* state) {
    expectArguments(state, 1, 1);
    pushString(state, sharedObject<IOutput>(state, 1, MT_OUTPUT)->port());
    return 1;
}

static int outputDesc(lua_State* state) {
    expectArguments(state, 1, 1);
    pushString(state, sharedObject<IOutput>(state, 1, MT_OUTPUT)->desc());
    return 1;
}

static int outputFps(lua_State* state) {
    expectArguments(state, 1, 1);
    lua_pushinteger(state, sharedObject<IOutput>(state, 1, MT_OUTPUT)->fps());
    return 1;
}

static int outputOnRemoved(lua_State* state) {
    expectArguments(state, 2, 2);
    const auto output = sharedObject<IOutput>(state, 1, MT_OUTPUT);
    const auto cb     = callback(state, 2);
    pushConnection(state, output->m_events.removed.listen([cb] {
        if (!cb->beginCall())
            return;
        cb->finishCall(0);
    }));
    return 1;
}

static int iconFactoryLookupIcon(lua_State* state) {
    expectArguments(state, 2, 2);
    pushIcon(state, sharedObject<ISystemIconFactory>(state, 1, MT_ICON_FACTORY)->lookupIcon(string(state, 2)));
    return 1;
}

static int iconExists(lua_State* state) {
    expectArguments(state, 1, 1);
    lua_pushboolean(state, sharedObject<ISystemIconDescription>(state, 1, MT_ICON)->exists());
    return 1;
}

static int iconScalable(lua_State* state) {
    expectArguments(state, 1, 1);
    lua_pushboolean(state, sharedObject<ISystemIconDescription>(state, 1, MT_ICON)->scalable());
    return 1;
}

enum class ePaletteColor : uint8_t {
    BACKGROUND,
    TEXT,
    BASE,
    ALTERNATE_BASE,
    BRIGHT_TEXT,
    LINK_TEXT,
    ACCENT,
    ACCENT_SECONDARY,
};

static void pushPaletteColor(lua_State* state, const SP<CPalette>& palette, ePaletteColor color) {
    switch (color) {
        case ePaletteColor::BACKGROUND: pushObject(state, palette->m_colors.background, MT_COLOR); break;
        case ePaletteColor::TEXT: pushObject(state, palette->m_colors.text, MT_COLOR); break;
        case ePaletteColor::BASE: pushObject(state, palette->m_colors.base, MT_COLOR); break;
        case ePaletteColor::ALTERNATE_BASE: pushObject(state, palette->m_colors.alternateBase, MT_COLOR); break;
        case ePaletteColor::BRIGHT_TEXT: pushObject(state, palette->m_colors.brightText, MT_COLOR); break;
        case ePaletteColor::LINK_TEXT: pushObject(state, palette->m_colors.linkText, MT_COLOR); break;
        case ePaletteColor::ACCENT: pushObject(state, palette->m_colors.accent, MT_COLOR); break;
        case ePaletteColor::ACCENT_SECONDARY: pushObject(state, palette->m_colors.accentSecondary, MT_COLOR); break;
    }
}

static int paletteColor(lua_State* state) {
    expectArguments(state, 2, 2);
    const auto palette = sharedObject<CPalette>(state, 1, MT_PALETTE);
    const auto name    = string(state, 2);
    if (name == "background")
        pushPaletteColor(state, palette, ePaletteColor::BACKGROUND);
    else if (name == "text")
        pushPaletteColor(state, palette, ePaletteColor::TEXT);
    else if (name == "base")
        pushPaletteColor(state, palette, ePaletteColor::BASE);
    else if (name == "alternateBase")
        pushPaletteColor(state, palette, ePaletteColor::ALTERNATE_BASE);
    else if (name == "brightText")
        pushPaletteColor(state, palette, ePaletteColor::BRIGHT_TEXT);
    else if (name == "linkText")
        pushPaletteColor(state, palette, ePaletteColor::LINK_TEXT);
    else if (name == "accent")
        pushPaletteColor(state, palette, ePaletteColor::ACCENT);
    else if (name == "accentSecondary")
        pushPaletteColor(state, palette, ePaletteColor::ACCENT_SECONDARY);
    else
        fail("unknown palette color '" + name + "'");
    return 1;
}

static int paletteSetColor(lua_State* state) {
    expectArguments(state, 3, 3);
    const auto palette = sharedObject<CPalette>(state, 1, MT_PALETTE);
    const auto name    = string(state, 2);
    const auto value   = color(state, 3);
    if (name == "background")
        palette->m_colors.background = value;
    else if (name == "text")
        palette->m_colors.text = value;
    else if (name == "base")
        palette->m_colors.base = value;
    else if (name == "alternateBase")
        palette->m_colors.alternateBase = value;
    else if (name == "brightText")
        palette->m_colors.brightText = value;
    else if (name == "linkText")
        palette->m_colors.linkText = value;
    else if (name == "accent")
        palette->m_colors.accent = value;
    else if (name == "accentSecondary")
        palette->m_colors.accentSecondary = value;
    else
        fail("unknown palette color '" + name + "'");
    return 0;
}

template <ePaletteColor Color>
static int paletteNamedColor(lua_State* state) {
    expectArguments(state, 1, 1);
    pushPaletteColor(state, sharedObject<CPalette>(state, 1, MT_PALETTE), Color);
    return 1;
}

enum class ePaletteVar : uint8_t {
    H1_SIZE,
    H2_SIZE,
    H3_SIZE,
    FONT_SIZE,
    SMALL_FONT_SIZE,
    ICON_THEME,
    BIG_ROUNDING,
    SMALL_ROUNDING,
    FONT_FAMILY,
    FONT_FAMILY_MONOSPACE,
};

static void pushPaletteVar(lua_State* state, const SP<CPalette>& palette, ePaletteVar var) {
    switch (var) {
        case ePaletteVar::H1_SIZE: lua_pushinteger(state, palette->m_vars.h1Size); break;
        case ePaletteVar::H2_SIZE: lua_pushinteger(state, palette->m_vars.h2Size); break;
        case ePaletteVar::H3_SIZE: lua_pushinteger(state, palette->m_vars.h3Size); break;
        case ePaletteVar::FONT_SIZE: lua_pushinteger(state, palette->m_vars.fontSize); break;
        case ePaletteVar::SMALL_FONT_SIZE: lua_pushinteger(state, palette->m_vars.smallFontSize); break;
        case ePaletteVar::ICON_THEME: pushString(state, palette->m_vars.iconTheme); break;
        case ePaletteVar::BIG_ROUNDING: lua_pushinteger(state, palette->m_vars.bigRounding); break;
        case ePaletteVar::SMALL_ROUNDING: lua_pushinteger(state, palette->m_vars.smallRounding); break;
        case ePaletteVar::FONT_FAMILY: pushString(state, palette->m_vars.fontFamily); break;
        case ePaletteVar::FONT_FAMILY_MONOSPACE: pushString(state, palette->m_vars.fontFamilyMonospace); break;
    }
}

static int paletteVar(lua_State* state) {
    expectArguments(state, 2, 2);
    const auto palette = sharedObject<CPalette>(state, 1, MT_PALETTE);
    const auto name    = string(state, 2);
    if (name == "h1Size")
        pushPaletteVar(state, palette, ePaletteVar::H1_SIZE);
    else if (name == "h2Size")
        pushPaletteVar(state, palette, ePaletteVar::H2_SIZE);
    else if (name == "h3Size")
        pushPaletteVar(state, palette, ePaletteVar::H3_SIZE);
    else if (name == "fontSize")
        pushPaletteVar(state, palette, ePaletteVar::FONT_SIZE);
    else if (name == "smallFontSize")
        pushPaletteVar(state, palette, ePaletteVar::SMALL_FONT_SIZE);
    else if (name == "iconTheme")
        pushPaletteVar(state, palette, ePaletteVar::ICON_THEME);
    else if (name == "bigRounding")
        pushPaletteVar(state, palette, ePaletteVar::BIG_ROUNDING);
    else if (name == "smallRounding")
        pushPaletteVar(state, palette, ePaletteVar::SMALL_ROUNDING);
    else if (name == "fontFamily")
        pushPaletteVar(state, palette, ePaletteVar::FONT_FAMILY);
    else if (name == "fontFamilyMonospace")
        pushPaletteVar(state, palette, ePaletteVar::FONT_FAMILY_MONOSPACE);
    else
        fail("unknown palette variable '" + name + "'");
    return 1;
}

static int paletteSetVar(lua_State* state) {
    expectArguments(state, 3, 3);
    const auto palette = sharedObject<CPalette>(state, 1, MT_PALETTE);
    const auto name    = string(state, 2);
    if (name == "h1Size")
        palette->m_vars.h1Size = checkedInt(state, 3);
    else if (name == "h2Size")
        palette->m_vars.h2Size = checkedInt(state, 3);
    else if (name == "h3Size")
        palette->m_vars.h3Size = checkedInt(state, 3);
    else if (name == "fontSize")
        palette->m_vars.fontSize = checkedInt(state, 3);
    else if (name == "smallFontSize")
        palette->m_vars.smallFontSize = checkedInt(state, 3);
    else if (name == "iconTheme")
        palette->m_vars.iconTheme = string(state, 3);
    else if (name == "bigRounding")
        palette->m_vars.bigRounding = checkedInt(state, 3);
    else if (name == "smallRounding")
        palette->m_vars.smallRounding = checkedInt(state, 3);
    else if (name == "fontFamily")
        palette->m_vars.fontFamily = string(state, 3);
    else if (name == "fontFamilyMonospace")
        palette->m_vars.fontFamilyMonospace = string(state, 3);
    else
        fail("unknown palette variable '" + name + "'");
    return 0;
}

static int paletteEmpty(lua_State* state) {
    expectArguments(state, 0, 0);
    pushObject(state, CPalette::emptyPalette(), MT_PALETTE);
    return 1;
}

template <ePaletteVar Var>
static int paletteNamedVar(lua_State* state) {
    expectArguments(state, 1, 1);
    pushPaletteVar(state, sharedObject<CPalette>(state, 1, MT_PALETTE), Var);
    return 1;
}

static int sessionLockUnlock(lua_State* state) {
    expectArguments(state, 1, 1);
    sharedObject<ISessionLockState>(state, 1, MT_SESSION_LOCK)->unlock();
    return 0;
}

static int sessionLockOnFinished(lua_State* state) {
    expectArguments(state, 2, 2);
    const auto lock = sharedObject<ISessionLockState>(state, 1, MT_SESSION_LOCK);
    const auto cb   = callback(state, 2);
    pushConnection(state, lock->m_events.finished.listen([cb] {
        if (!cb->beginCall())
            return;
        cb->finishCall(0);
    }));
    return 1;
}

static int finalizeBackend(lua_State* state) noexcept {
    auto handle = testObject<SBackend>(state, 1, MT_BACKEND);
    if (!handle)
        return 0;
    if (handle->active && handle->backend) {
        const auto backend = handle->backend;
        handle->active     = false;
        if (const auto luaContext = handle->context.lock())
            luaContext->backendActive = false;
        releasePending(*handle);
        handle->backend.reset();
        try {
            backend->destroy();
        } catch (...) { std::fputs("hyprtoolkit-lua: backend destruction failed during finalization\n", stderr); }
    }
    return destroyObject<SBackend>(state);
}

static constexpr luaL_Reg BACKEND_METHODS[] = {
    {"destroy", protect<backendDestroy>},
    {"enterLoop", protect<backendEnterLoop>},
    {"setLogFn", protect<backendSetLogFn>},
    {"addFd", protect<backendAddFd>},
    {"removeFd", protect<backendRemoveFd>},
    {"addTimer", protect<backendAddTimer>},
    {"addIdle", protect<backendAddIdle>},
    {"systemIcons", protect<backendSystemIcons>},
    {"getPalette", protect<backendGetPalette>},
    {"getOutputs", protect<backendGetOutputs>},
    {"acquireSessionLock", protect<backendAcquireSessionLock>},
    {"aquireSessionLock", protect<backendAcquireSessionLock>},
    {"onOutputAdded", protect<backendOnOutputAdded>},
    {nullptr, nullptr},
};

static constexpr luaL_Reg TIMER_METHODS[] = {
    {"cancel", protect<timerCancel>},
    {"cancelled", protect<timerCancelled>},
    {"passed", protect<timerPassed>},
    {"canForceUpdate", protect<timerCanForceUpdate>},
    {"updateTimeout", protect<timerUpdateTimeout>},
    {"leftMs", protect<timerLeftMs>},
    {nullptr, nullptr},
};

static constexpr luaL_Reg OUTPUT_METHODS[] = {
    {"handle", protect<outputHandle>},
    {"port", protect<outputPort>},
    {"desc", protect<outputDesc>},
    {"description", protect<outputDesc>},
    {"fps", protect<outputFps>},
    {"onRemoved", protect<outputOnRemoved>},
    {nullptr, nullptr},
};

static constexpr luaL_Reg ICON_FACTORY_METHODS[] = {
    {"lookupIcon", protect<iconFactoryLookupIcon>},
    {nullptr, nullptr},
};

static constexpr luaL_Reg ICON_METHODS[] = {
    {"exists", protect<iconExists>},
    {"scalable", protect<iconScalable>},
    {nullptr, nullptr},
};

static constexpr luaL_Reg PALETTE_METHODS[] = {
    {"color", protect<paletteColor>},
    {"setColor", protect<paletteSetColor>},
    {"var", protect<paletteVar>},
    {"setVar", protect<paletteSetVar>},
    {"background", protect<paletteNamedColor<ePaletteColor::BACKGROUND>>},
    {"text", protect<paletteNamedColor<ePaletteColor::TEXT>>},
    {"base", protect<paletteNamedColor<ePaletteColor::BASE>>},
    {"alternateBase", protect<paletteNamedColor<ePaletteColor::ALTERNATE_BASE>>},
    {"brightText", protect<paletteNamedColor<ePaletteColor::BRIGHT_TEXT>>},
    {"linkText", protect<paletteNamedColor<ePaletteColor::LINK_TEXT>>},
    {"accent", protect<paletteNamedColor<ePaletteColor::ACCENT>>},
    {"accentSecondary", protect<paletteNamedColor<ePaletteColor::ACCENT_SECONDARY>>},
    {"h1Size", protect<paletteNamedVar<ePaletteVar::H1_SIZE>>},
    {"h2Size", protect<paletteNamedVar<ePaletteVar::H2_SIZE>>},
    {"h3Size", protect<paletteNamedVar<ePaletteVar::H3_SIZE>>},
    {"fontSize", protect<paletteNamedVar<ePaletteVar::FONT_SIZE>>},
    {"smallFontSize", protect<paletteNamedVar<ePaletteVar::SMALL_FONT_SIZE>>},
    {"iconTheme", protect<paletteNamedVar<ePaletteVar::ICON_THEME>>},
    {"bigRounding", protect<paletteNamedVar<ePaletteVar::BIG_ROUNDING>>},
    {"smallRounding", protect<paletteNamedVar<ePaletteVar::SMALL_ROUNDING>>},
    {"fontFamily", protect<paletteNamedVar<ePaletteVar::FONT_FAMILY>>},
    {"fontFamilyMonospace", protect<paletteNamedVar<ePaletteVar::FONT_FAMILY_MONOSPACE>>},
    {nullptr, nullptr},
};

static constexpr luaL_Reg SESSION_LOCK_METHODS[] = {
    {"unlock", protect<sessionLockUnlock>},
    {"onFinished", protect<sessionLockOnFinished>},
    {nullptr, nullptr},
};

static constexpr luaL_Reg BACKEND_CLASS[] = {
    {"create", protect<backendCreate>},
    {nullptr, nullptr},
};

static constexpr luaL_Reg PALETTE_CLASS[] = {
    {"empty", protect<paletteEmpty>},
    {nullptr, nullptr},
};

void Hyprtoolkit::Lua::registerCore(lua_State* state, int module) {
    registerMetatable(state, MT_BACKEND, BACKEND_METHODS, protect<finalizeBackend>);
    registerMetatable(state, MT_TIMER, TIMER_METHODS, protect<destroyObject<STimer>>);
    registerMetatable(state, MT_OUTPUT, OUTPUT_METHODS, protect<destroyObject<SP<IOutput>>>);
    registerMetatable(state, MT_ICON_FACTORY, ICON_FACTORY_METHODS, protect<destroyObject<SP<ISystemIconFactory>>>);
    registerMetatable(state, MT_ICON, ICON_METHODS, protect<destroyObject<SP<ISystemIconDescription>>>);
    registerMetatable(state, MT_PALETTE, PALETTE_METHODS, protect<destroyObject<SP<CPalette>>>);
    registerMetatable(state, MT_SESSION_LOCK, SESSION_LOCK_METHODS, protect<destroyObject<SP<ISessionLockState>>>);
    setClass(state, module, "Backend", BACKEND_CLASS);
    setClass(state, module, "Palette", PALETTE_CLASS);
}
