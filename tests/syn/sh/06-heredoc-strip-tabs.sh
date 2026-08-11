cat <<-EOF
	$USER
	EOF
echo after
cat <<-'RAW'
$USER
	RAW
cat <<-"MORE"
$USER
	MORE
