# run script include, AD Aug 1, 2024
ME=$0
LOG="logger -t ${ME}[${PRG}]"
${LOG} "started, currdir is $(pwd)"
[ -z "${PAUSE}" ] && PAUSE=15

if [ -z "${PRG}" ]; then
	${LOG} "PRG not set"
	exit 1
fi

PID=""
PIDFILE="/var/run/${PRG}.pid"

sighandler_INT() {
	${LOG} "terminated"
	[ -f ${PIDFILE} ] && rm  ${PIDFILE}
 	exit 0
}

if [ ! -x ./${PRG} ]; then
	${LOG} "./${PRG} missing"
	exit 1
fi
# SIGINT, SIGKILL and SIGTERM
trap 'sighandler_INT' 2
trap 'sighandler_INT' 9
trap 'sighandler_INT' 15

[ ! -z "${STARTDELAY}" ] && sleep ${STARTDELAY}

while [ 1 ]; do
	if [ ! -x ./${PRG} ]; then
		${LOG} "./${PRG} missing, nfs server not available ?"
		[ -x "$0" ] || ${LOG} "$0 is missing as well"
# shoud we try to remount nfs here ?, to be tested
	else
		${LOG} "starting ${PRG} ${ARGS}"
		./${PRG} ${ARGS} &
		PID=$!
		if ! echo "${PID}" >${PIDFILE}; then
                       	${LOG} "unable to write to pid file ${PIDFILE}"
                fi
		while [ ! -z "$(jobs)" ]; do
			sleep 1
			jobs >/dev/null 	# why the hell is this needed ?
		done
#		while ! kill -0 ${PID} ; do
#			sleep 1
#		done
		[ -f ${PIDFILE} ] && rm ${PIDFILE}
		PID=""
		${LOG} "${PRG} terminated, restarting in ${PAUSE} seconds"
	fi
    	sleep ${PAUSE}
done

