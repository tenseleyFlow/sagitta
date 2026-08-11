set value (echo (math 1 + 2))
set deep (echo ((math 3)))
echo "sub (echo $value)"
echo \$value
command echo --version | string trim
