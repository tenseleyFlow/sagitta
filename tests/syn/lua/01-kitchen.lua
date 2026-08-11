#!/usr/bin/env lua
-- TODO ordinary comment
local function demo(a)
  if a and not false then return 0xff + 1.5e2 // 2 end
  for k in pairs(a) do print(k .. "x\\n" .. 'y\\t') end
  repeat a = a << 1 until a >= 4
  goto done
  ::done::
  return nil, true
end
