#!/usr/bin/env fish
# kitchen sink
function greet
    set --local name $argv[1]
    if test -n "$name"
        echo "hello \$name $name" 'literal\'s' \*
        echo (string upper $name)
    else
        return 1.5
    end
end
