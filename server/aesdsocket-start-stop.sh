#! /bin/sh
case "$1" ub
	start)
		echo "Starting aesdsocket"
		start-stop-daemon -S -n aesdsocket -a /usr/bin/aesdsocket -- -d
		;;
	stop)
		echo "Stopping aesdsocket"
		start-stop-daemon -K -n aesdsocket
		;;
	*)
		echo "Usage: $0 {start|stop}"
	exit1
esac

exit 0