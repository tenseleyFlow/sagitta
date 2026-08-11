$name = "world"
$expand = @"
hello $name
sum $(2 + $name)
`n
"@
$literal = @'
$name stays literal
'@
