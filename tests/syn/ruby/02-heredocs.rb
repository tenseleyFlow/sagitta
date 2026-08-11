a = <<RAW
hello #{name} #@name \q
RAW
b = <<'LIT'
raw #@name
LIT
c = <<-INDENT
expanded #{name} #@name \q
  INDENT
d = <<~'SQUIGGLE'
raw #{name}
	SQUIGGLE
e = <<"QUOTED"
quoted #{name}
QUOTED
f = <<~"QINDENT"
quoted indented #{name}
 QINDENT
