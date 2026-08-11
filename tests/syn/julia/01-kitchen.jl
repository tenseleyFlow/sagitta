# kitchen
module Demo
export run
abstract type Shape end
mutable struct Box <: Shape
  value::Int64
end
macro twice(x) x + x end
function run(x)
  local y = 0xff + 1.5e2
  if true && x !== nothing
    return @twice y
  elseif false
    throw(missing)
  else
    for i in 1:2; continue; end
  end
end
end
