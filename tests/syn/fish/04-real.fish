for path in $PATH
    if test -d $path
        printf '%s\n' $path
    end
end
status --is-interactive; and source ~/.config/fish/local.fish
