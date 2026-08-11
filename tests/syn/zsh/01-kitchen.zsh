#!/usr/bin/env zsh
# options and parameter flags
setopt extendedglob
typeset -A map
name=world
count=42
print "hello \${(@U)name} $(print nested) $name" 'literal'
print ${(q)map} $name \*
print ${name:-fallback}
print **/*.c(.N)
if [[ 3 -gt 2 ]]; then
  print `whence zsh \*`
fi
