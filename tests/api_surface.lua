local ht = assert(hyprtoolkit)
assert(require("hyprtoolkit") == ht)

assert(type(ht.version) == "string")
assert(ht.HT_SIZE_ABSOLUTE == 0)
assert(ht.HT_SIZE_PERCENT == 1)
assert(ht.HT_SIZE_AUTO == 2)
assert(ht.HT_WINDOW_TOPLEVEL == 0)
assert(ht.HT_MODIFIER_CTRL == 4)

local vector = ht.vec2(12.5, -4)
assert(vector.x == 12.5 and vector.y == -4)
assert(vector[1] == vector.x and vector[2] == vector.y)

local size = ht.dynamicSize(ht.HT_SIZE_PERCENT, ht.HT_SIZE_ABSOLUTE, vector)
assert(type(size) == "userdata")
assert(type(ht.DynamicSize.new(ht.HT_SIZE_AUTO, ht.HT_SIZE_AUTO, 1, 1)) == "userdata")
assert(type(ht.fontSize(ht.HT_FONT_TEXT, 1.5)) == "userdata")

local color = ht.color(0xFF336699)
assert(color:getAsHex() == 0xFF336699)
assert(math.abs(color.r - 0.2) < 0.01)
assert(math.abs(color.g - 0.4) < 0.01)
assert(math.abs(color.b - 0.6) < 0.01)
assert(color.a == 1)
assert(color:stripA().a == 1)
assert(type(color:brighten(0.1)) == "userdata")
assert(type(color:darken(0.1)) == "userdata")
assert(type(color:mix(ht.color(0, 0, 0, 1), 0.5)) == "userdata")
assert(type(color:asRGB()) == "table")
assert(type(color:asOkLab()) == "table")
assert(type(color:asHSL()) == "table")
assert(not pcall(function()
    color.r = 0
end))
assert(not pcall(function()
    ht.color(0 / 0, 0, 0)
end))
assert(not pcall(function()
    ht.LineBuilder.begin():thick(math.maxinteger)
end))
assert(not pcall(function()
    ht.springAnimation({stiffness = math.huge})
end))
assert(not pcall(function()
    ht.RectangleBuilder.begin():commence()
end))

assert(ht.noAnimation().type == "none")
assert(ht.bezierAnimation(200).type == "bezier")
assert(ht.springAnimation().type == "spring")
assert(not pcall(function()
    ht.Animation.Slow.stiffness = 0
end))

local builders = {
    "TextBuilder",
    "RectangleBuilder",
    "CheckboxBuilder",
    "ProgressBarBuilder",
    "ScrollAreaBuilder",
    "SpinboxBuilder",
    "SliderBuilder",
    "ComboboxBuilder",
    "ButtonBuilder",
    "TextboxBuilder",
    "RowLayoutBuilder",
    "ImageBuilder",
    "ColumnLayoutBuilder",
    "NullBuilder",
    "LineBuilder",
}

for _, name in ipairs(builders) do
    assert(type(ht[name]) == "table", name)
    assert(type(ht[name].begin) == "function", name .. ".begin")
    assert(type(ht[name].begin()) == "userdata", name .. " builder")
end

assert(ht.TextBuilder.begin()
    :text("hello")
    :color(color)
    :fontSize(ht.fontSize(ht.HT_FONT_ABSOLUTE, 14))
    :align(ht.HT_FONT_ALIGN_CENTER)
    :fontFamily("sans")
    :clampSize(ht.vec2(100, 30))
    :callback(function() end)
    :noEllipsize(true)
    :async(false)
    :interactable(true)
    :size(size))

assert(ht.RectangleBuilder.begin()
    :color(function()
        return color
    end)
    :borderColor(color)
    :borderGradient({colors = {color, ht.color(0xFFFFFFFF)}, angle = 1.2})
    :rounding(4)
    :borderThickness(1)
    :size(size))

assert(ht.CheckboxBuilder.begin():onToggled(function() end):toggled(true):style(ht.HT_CHECKBOX_STYLE_RADIO):size(size))
assert(ht.ProgressBarBuilder.begin():value(0.5):indeterminate(false):size(size))
assert(ht.ScrollAreaBuilder.begin():scrollX(true):scrollY(true):blockUserScroll(false):showScrollbar(true):size(size))
assert(ht.SpinboxBuilder.begin():label("choice"):items({}):onChanged(function() end):fill(true):size(size))
assert(ht.SliderBuilder.begin():min(-1):max(1):val(0):snapInt(false):onChanged(function() end):size(size))
assert(ht.ComboboxBuilder.begin():items({"one", "two"}):currentItem(2):onChanged(function() end):size(size))
assert(ht.ButtonBuilder.begin()
    :label("button")
    :noBorder(true)
    :noBg(false)
    :accent(true)
    :ellipsize(true)
    :enabled(true)
    :alignText(ht.HT_FONT_ALIGN_LEFT)
    :fontFamily("sans")
    :fontSize(ht.fontSize(ht.HT_FONT_TEXT))
    :onMainClick(function() end)
    :onRightClick(function() end)
    :size(size))
assert(ht.TextboxBuilder.begin():placeholder("type"):defaultText(""):onTextEdited(function() end):multiline(false):password(false):eyeIcon(true):size(size))
assert(ht.RowLayoutBuilder.begin():gap(4):size(size))
assert(ht.ImageBuilder.begin():path("image.svg"):a(0.5):fitMode(ht.IMAGE_FIT_MODE_CONTAIN):sync(true):rounding(2):size(size))
assert(ht.ImageBuilder.begin():data("\0PNG\0"))
assert(ht.ColumnLayoutBuilder.begin():gap(4):size(size))
assert(ht.NullBuilder.begin():size(size))
assert(ht.LineBuilder.begin():color(color):thick(2):points({ht.vec2(0, 0), ht.vec2(1, 1)}):size(size))

assert(type(ht.WindowBuilder.begin()
    :type(ht.HT_WINDOW_TOPLEVEL)
    :appTitle("Lua test")
    :appClass("hyprtoolkit-lua-test")
    :preferredSize(ht.vec2(640, 480))
    :minSize(ht.vec2(100, 100))
    :maxSize(ht.vec2(1920, 1080))
    :resizable(true)
    :autosize(false)
    :inhibitShortcuts(false)
    :pos(ht.vec2(0, 0))) == "userdata")

local group = ht.RadioGroup.create()
assert(type(group) == "userdata")
group:setSelected(nil)

local palette = ht.Palette.empty()
palette:setColor("accent", color)
assert(palette:accent() == color)
palette:setVar("fontSize", 13)
palette:setVar("fontFamily", "sans")
assert(palette:fontSize() == 13)
assert(palette:fontFamily() == "sans")

assert(not pcall(function()
    ht.TextBuilder.begin():rounding(2)
end))

local colorMetatable = debug.getmetatable(color)
colorMetatable.__gc(size)
assert(type(size:calculate(ht.vec2(100, 100))) == "table")
colorMetatable.__gc(color)
colorMetatable.__gc(color)
assert(not pcall(function()
    color:getAsHex()
end))

collectgarbage("collect")

local context
for _, value in pairs(debug.getregistry()) do
    if type(value) == "userdata" then
        local metatable = debug.getmetatable(value)
        if metatable and metatable.__metatable == "hyprtoolkit.Context" then
            context = value
            break
        end
    end
end
assert(context)
local contextMetatable = debug.getmetatable(context)
contextMetatable.__gc(context)
contextMetatable.__gc(context)
