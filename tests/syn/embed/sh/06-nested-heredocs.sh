cat <<OUTER
before
$(cat <<INNER
inside
INNER
)
OUTER
