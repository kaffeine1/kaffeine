-- Copyright (C) 2026, The AROS Development Team. All rights reserved.
-- Author: Michele Dipace (@kaffeine1)
--
-- Standard-input step for Lua 5.4.7: exercises setjmp/longjmp error
-- recovery, IEEE-754 double math and table.sort, string.format with %g and
-- %a, file I/O on EMU68/ and a temporary file, os.clock() and os.time().

print("testing stdin script")

local c0 = os.clock()
assert(type(c0) == "number" and c0 >= 0)

-- 1. setjmp/longjmp across nested pcall, xpcall and coroutines
local function deep_throw(depth, payload)
    if depth <= 0 then
        error(payload, 0)
    end
    return 1 + deep_throw(depth - 1, payload)
end

local ok1, err1 = pcall(deep_throw, 12, "caught-string-error")
assert(not ok1 and err1 == "caught-string-error")

local ok2, err2 = xpcall(function()
    local inner_ok, inner_err = pcall(deep_throw, 8, { code = 42, tag = "tbl" })
    assert(not inner_ok and inner_err.code == 42 and inner_err.tag == "tbl")
    deep_throw(6, "outer-fault")
end, function(msg)
    return "handled:" .. tostring(msg)
end)
assert(not ok2 and err2 == "handled:outer-fault")

local co = coroutine.create(function()
    local cok, cerr = pcall(deep_throw, 5, "co-inner")
    assert(not cok and cerr == "co-inner")
    coroutine.yield("after-pcall")
    deep_throw(3, "co-unhandled")
end)
local r1, v1 = coroutine.resume(co)
assert(r1 and v1 == "after-pcall")
local r2, v2 = coroutine.resume(co)
assert(not r2 and v2 == "co-unhandled" and coroutine.status(co) == "dead")
print("pcall/xpcall ok:", err1, err2, v2)

-- 2. IEEE-754 double math and deterministic table.sort
local nums = {
    0.0, -0.0, 0.1, -0.1, 1.0 / 3.0, -45.0 / 247.0,
    math.pi, 2.75, math.sqrt(2.0), 2.0^-52, 1e30, -3e-20
}
for i = 1, #nums do
    local x = nums[i]
    local g = string.format("%.14g", x)
    local a = string.format("%a", x)
    local A = string.format("%A", x)
    assert(tonumber(a) == x and tonumber(A) == x)
    print(string.format("fmt[%d] g=%s a=%s A=%s", i, g, a, A))
end

local s2 = math.sin(0.7)^2 + math.cos(0.7)^2
assert(math.abs(s2 - 1.0) < 1e-12)
assert(math.abs(math.atan(1.0) * 4.0 - math.pi) < 1e-12)
assert(math.abs(math.log(math.exp(3.5)) - 3.5) < 1e-12)
local ip, fp = math.modf(-123.75)
assert(ip == -123 and fp == -0.75 and math.fmod(-17.5, 4.0) == -1.5)
assert(math.floor(-3.25) == -4 and math.ceil(-3.25) == -3)
print(string.format("math ok: sin2+cos2=%.6g atan4=%.8g modf=%g,%g", s2, math.atan(1.0) * 4.0, ip, fp))

-- exact digits and signs of floating conversions, and a long one
print(string.format("exact: %.0f %.0f %.2f %.17g", 1234567890123456.0, 2.0^53, 1e15 + 0.25, 0.1))
print(string.format("sign: [%020.2f] [%-+12.3e] [% 010.1f]", -1.25, 12345.678, 2.5))
local big = string.format("%.99f", 1e308)
print(string.format("big: %d %s %s", #big, big:sub(1, 30), big:sub(300, 320)))

local arr = {}
for i = 1, 64 do
    arr[i] = ((i * 37 + 13) % 64) * 0.125 - 4.0
end
table.sort(arr)
for i = 2, #arr do
    assert(arr[i - 1] <= arr[i])
end
print(string.format("sort ok: min=%a mid=%a max=%a", arr[1], arr[32], arr[64]))

-- 3. File I/O on EMU68/ and a temporary file
local rf = assert(io.open("EMU68/bwcoercion.lua", "rb"))
local content = assert(rf:read("a"))
assert(rf:close())
local lines = 0
for _ in io.lines("EMU68/bad.lua") do
    lines = lines + 1
end
assert(#content > 100 and lines >= 10)

local rc_probe = io.open("EMU68/lua_stdin-shell-rc.txt", "r")
local tmpname = rc_probe and "lua_stdin_run.tmp" or "lua_stdin_shell.tmp"
if rc_probe then
    rc_probe:close()
end
local wf = assert(io.open(tmpname, "wb"))
assert(wf:write("line-one\n", string.format("%g %a\n", 1.25, -6.5)))
assert(wf:flush())
assert(wf:close())

local tf = assert(io.open(tmpname, "rb"))
local first = assert(tf:read("l"))
local pos = assert(tf:seek("cur", 0))
local second = assert(tf:read("l"))
assert(tf:seek("set", 0) == 0)
local all = assert(tf:read("a"))
assert(tf:close())
assert(os.remove(tmpname))
assert(first == "line-one" and pos == 9 and second == "1.25 -0x1.ap+2")
print(string.format("io ok: bwcoercion=%d bad_lines=%d tmp=%d/%s", #content, lines, #all, second))

-- 4. os.clock() and os.time()
local c1 = os.clock()
assert(type(c1) == "number" and c1 >= c0)
local now = os.time()
assert(math.type(now) == "integer")
local t1 = os.time({ year = 2026, month = 1, day = 15, hour = 12, min = 0, sec = 0, isdst = false })
local t2 = os.time({ year = 2026, month = 1, day = 16, hour = 13, min = 30, sec = 45, isdst = false })
local diff = os.difftime(t2, t1)
local utc = os.date("!%Y-%m-%d %H:%M:%S wday=%w yday=%j", 1768478400)
assert(diff == 91845.0 and utc == "2026-01-15 12:00:00 wday=4 yday=015")
print(string.format("time ok: diff=%g utc=%s", diff, utc))
print("OK")
