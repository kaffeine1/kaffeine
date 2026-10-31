-- Copyright (C) 2026, The AROS Development Team. All rights reserved.
-- Author: Michele Dipace (@kaffeine1)
--
-- Deliberate unhandled error step for Lua 5.4.7: flushes a line to standard
-- output and raises an uncaught error so that lua exits with status 1 and
-- writes the stack traceback to standard error.

print("testing unhandled error")
io.stdout:flush()

local function fail_step(tag)
    error("unhandled error in " .. tag)
end

fail_step("lua_bad")
