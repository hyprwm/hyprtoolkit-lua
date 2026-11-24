# hyprtoolkit-lua

Lua 5.5 bindings for [Hyprtoolkit](https://github.com/hyprwm/hyprtoolkit). The
bindings use the Lua C API directly and can be used either as a native Lua
module or through the included script runner.

## Building

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Hyprtoolkit 0.6.0, Lua 5.5, libdrm, CMake, and pkg-config are required.

The build produces:

- `hyprtoolkit-lua`: the C++ library;
- `hyprtoolkit.so`: the module loaded by `require("hyprtoolkit")`;
- `hyprtoolkit-lua`: a script runner which preloads the module globally.

The module is installed to `${CMAKE_INSTALL_LIBDIR}/lua/5.5`. A Nix package and
development shell are also available from the flake.

## Example

```lua
local ht = require("hyprtoolkit")

local backend, err = ht.Backend.create()
assert(backend, err)

local window = assert(ht.WindowBuilder.begin()
    :appTitle("Hyprtoolkit Lua")
    :appClass("hyprtoolkit-lua-example")
    :preferredSize(ht.vec2(420, 160))
    :commence())

local button = ht.ButtonBuilder.begin()
    :label("Close")
    :onMainClick(function()
        window:close()
        backend:destroy()
    end)
    :commence()

window:root():addChild(button)
window:open()
backend:enterLoop()
```

## API overview

Construct values with `vec2`, `dynamicSize`, `fontSize`, `color`,
`noAnimation`, `bezierAnimation`, and `springAnimation`. Animation presets are
available under `Animation.Slow`, `Medium`, `Fast`, `Snappy`, and `Bouncy`.

The following builders use the same fluent option names as their Hyprtoolkit
C++ counterparts:

- `TextBuilder`, `RectangleBuilder`, `CheckboxBuilder`, `ProgressBarBuilder`;
- `ScrollAreaBuilder`, `SpinboxBuilder`, `SliderBuilder`, `ComboboxBuilder`;
- `ButtonBuilder`, `TextboxBuilder`, `RowLayoutBuilder`, `ColumnLayoutBuilder`;
- `ImageBuilder`, `NullBuilder`, `LineBuilder`, and `WindowBuilder`.

Call `:commence()` to obtain an element or window. Elements expose the common
layout, input, animation, child-management, and rebuild methods. Concrete
element operations such as `setText`, `setState`, `setValue`, `setScroll`,
`setCurrent`, and `focus` validate the element type at runtime.

Other entry points include:

- `Backend.create()`, timers, idles, file-descriptor callbacks, logging,
  outputs, system icons, palettes, and session locking;
- `Palette.empty()` with `color`, `setColor`, `var`, and `setVar`;
- `RadioGroup.create()`; passing `nil` to `setSelected` clears the selection;
- connection objects returned by signals, with `connected()` and
  `disconnect()`.

Spinbox and combobox indexes are one-based in Lua. Textbox cursor and selection
positions are zero-based UTF-8 byte offsets, matching Hyprtoolkit; `-1` denotes
no selection.

## Callback and lifetime rules

- Keep signal connection objects alive for as long as the listener is needed.
  Dropping or disconnecting a connection removes the listener.
- Callbacks execute on the main Lua thread and cannot yield, including
  callbacks registered from a coroutine.
- Clear persistent callbacks with the corresponding setter and `nil`, and
  disconnect signal callbacks which capture their own connection. Capturing an
  owning element or connection in its callback can otherwise create an
  ownership cycle.
- Timers are one-shot. `cancel()` releases their Lua callback immediately.
- Close windows and release backend-dependent resources before destroying the
  backend. Once destroyed, that backend cannot be recreated in the same Lua
  state and backend-dependent wrappers reject further method calls.

Callback errors are caught and written to standard error instead of unwinding
through Hyprtoolkit.
