#!/usr/bin/env luajit
function module.run(items)
  while #items > 0 do
    table.remove(items, 1)
  end
end
