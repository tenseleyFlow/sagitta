autoload -Uz compinit
compinit
for file in **/*.(om[1,3]); do
  [[ -f $file ]] && print -r -- $file
done
