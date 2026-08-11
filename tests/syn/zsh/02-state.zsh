cat <<EOF
hello $name
\$escaped
EOF
cat <<-TAB
	body $name
	TAB
print $(print (nested))
