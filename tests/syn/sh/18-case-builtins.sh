case "$1" in
  start|stop) command printf ok ;;
  *) exit 2 ;;
esac
