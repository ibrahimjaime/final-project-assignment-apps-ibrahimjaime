#!/bin/sh
### BEGIN INIT INFO
# Provides:          data-logger
# Required-Start:    $remote_fs $syslog
# Required-Stop:     $remote_fs $syslog
# Default-Start:     2 3 4 5
# Default-Stop:      0 1 6
# Short-Description: Data Data Logging System daemon
### END INIT INFO

# Mirrors the aesdsocket-start-stop.sh structure: start-stop-daemon is used
# to launch/track the daemon by PID file rather than by process name, so
# multiple unrelated binaries can't be confused with each other.

DAEMON=/usr/bin/data-logger
PIDFILE=/var/run/data-logger.pid

case "$1" in
  start)
    echo "Starting data-logger"
    start-stop-daemon --start --quiet --pidfile "$PIDFILE" --exec "$DAEMON" -- -d
    ;;
  stop)
    echo "Stopping data-logger"
    start-stop-daemon --stop --quiet --pidfile "$PIDFILE" --retry TERM/5/KILL/5
    rm -f "$PIDFILE"
    ;;
  restart)
    "$0" stop
    sleep 1
    "$0" start
    ;;
  status)
    if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
      echo "data-logger is running (pid $(cat "$PIDFILE"))"
    else
      echo "data-logger is not running"
      exit 1
    fi
    ;;
  *)
    echo "Usage: $0 {start|stop|restart|status}"
    exit 1
    ;;
esac

exit 0
