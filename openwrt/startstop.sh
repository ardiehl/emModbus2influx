#!/bin/sh

if [ -z "${PRG}" ]; then
	echo "$0: PRG not defined, existing"
	exit 1
fi

PIDFILERUN="/var/run/${PRG}.run.pid"
PIDFILEPRG="/var/run/${PRG}.pid"
#PID=$(ps | grep ${PRG} | grep -v ${PRG}.sh | grep -v grep | awk '{print $1}')
[ -f ${PIDFILERUN} ] && PIDRUN=$(cat ${PIDFILERUN})
[ -f ${PIDFILEPRG} ] && PIDPRG=$(cat ${PIDFILEPRG})

case "$1" in
	stop)
#		echo "$PID1 $PID2"
		if [ -z "${PIDRUN}" ]; then
			echo "${PRG} not started"
			exit 1
		fi
		echo "Stopping ${PRG}"
		[ ! -z "${PIDRUN}" ] && kill ${PIDRUN}
		[ ! -z "${PIDPRG}" ] && kill ${PIDPRG} 2>/dev/null	# not really needed
		[ -f ${PIDFILERUN} ] && rm ${PIDFILERUN}
		# wait until terminated
		if [ ! -z "${PIDPRG}" ]; then
			while [ ! -z $(ps | awk '{print $1}' | grep ${PIDPRG}) ]; do
				sleep 1
			done
		fi
 		if [ ! -z "${PIDRUN}" ]; then
			while [ ! -z $(ps | awk '{print $1}' | grep ${PIDRUN}) ]; do
				sleep 1
			done
		fi
		sleep 2
	;;
	start)
		if [ ! -z "$PIDRUN" ]; then
			echo "$0: ${PRG} already running"
			exit 1
		fi
		if [ ! -x ./${PRG} ]; then
			${LOG} "./${PRG} missing"
			exit 1
		fi
		if [ ! -x ../lib/run.sh ]; then
			${LOG} "../lib/run.sh missing"
			exit 1
		fi
		echo "Starting ${PRG} via ../lib/run.sh"
		../lib/run.sh &
		if ! echo "$!" >${PIDFILERUN}; then
			echo "unable to write to pid file ${PIDFILERUN}"
		fi

	;;
	restart)
		if [ ! -z "${PIDPRG}" ]; then
			kill ${PIDPRG}
		else
			if [ -z "${PIDRUN}" ]; then
				$0 start
			else
				echo "restart in progress already"
			fi
		fi
	;;
	*)	echo "Usage: $0 start|stop|restart"
		exit 1
	;;
esac

