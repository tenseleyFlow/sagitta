plain = <<EOF
hello ${user.name} $${escaped}
%{ for item in items }${item}%{ endfor }
EOF
quoted = <<"RAW"
quoted ${still_template}
RAW
indented = <<-INDENT
  body ${value} %%
	INDENT
quoted_indented = <<-"QEND"
	%{ if ok }yes%{ endif }
  QEND
