#! /bin/sh

case "$1" in
	start)
		echo "Starting aesdsocket"
		start-stop-daemon -S -n aesdsocket -a /usr/bin/aesdsocket -d
		;;
	stop)
		echo "Stopping aesdserver"
		start-stop-daemon -K aesdsocket
		;;
	*)
		echo "Usage: $0 {start|stop}"
	exit 1
esac

exit 0
