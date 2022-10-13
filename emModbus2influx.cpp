/******************************************************************************
emModbus2influx

Read from modbus energy meters (currently DRT428M-3
and send the data to influxdb (1.x or 2.x API) and/or via mqtt
******************************************************************************/
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <ctype.h>
#include <errno.h>
#include <termios.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include "argparse.h"
#include <math.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <time.h>

#include "log.h"

#include "mqtt_publish.h"

#include "influxdb-post/influxdb-post.h"
#include "meterDef.h"
#include "modbusread.h"
#include "global.h"
#include "parser.h"
#include <modbus.h>
#include "global.h"
#include <endian.h>

#ifndef DISABLE_FORMULAS
#include "muParser.h"
#endif

#include "MQTTClient.h"

#define VER "1.00 Armin Diehl <ad@ardiehl.de> Apr 24,2022, compiled " __DATE__ " " __TIME__
#define ME "emModbus2influx"
#define CONFFILE "emModbus2influx.conf"

#define MQTT_CLIENT_ID ME

#define NUM_RECS_TO_BUFFER_ON_FAILURE 1000

char *configFileName;
char * serDevice;
int serBaudrate = 9600;
char *serParity;
int serStopbits;
int ser_rs485;
int dumpRegisters;
int dryrun;
int queryIntervalSecs = 5;	// seconds
char *formulaValMeterName;
int formulaTry;
int scanRTU;
int modbusDebug;
influx_client_t *iClient;

mqtt_pubT *mClient;

#define MQTT_PREFIX_DEF "ad/house/energy/"


int doTry   = false;

// maximal length of a http line send
#define INFLUX_BUFLEN 2048

char * influxBuf;
size_t influxBufUsed;
int influxBufLen;

#define INFLUX_DEFAULT_MEASUREMENT "energyMeter"
#define INFLUX_DEFAULT_TAGNAME "Meter"
char * influxMeasurement;
char * influxTagName;
int influxWriteMult;    // write to influx only on x th query (>=2)
int mqttQOS;
int mqttRetain;
char * mqttprefix;

int syslogTestCallback(argParse_handleT *a, char * arg) {
	VPRINTF(0,"%s : sending testtext via syslog\n\n",ME);
	log_setSyslogTarget(ME);
	VPRINTF(0,"testtext via syslog by %s",ME);
	exit(0);
}


int showVersionCallback(argParse_handleT *a, char * arg) {
	MQTTClient_nameValue* MQTTVersionInfo;
	char *MQTTVersion = NULL;
	int i;

	printf("%s %s\n",ME,VER);
	printf("  libmodbus: %d.%d.%d\n",LIBMODBUS_VERSION_MAJOR,LIBMODBUS_VERSION_MINOR,LIBMODBUS_VERSION_MICRO);
#ifndef DISABLE_FORMULAS
	//printf("   muparser: %s\n",mu::ParserVersion.c_str());
#else
	//printf("  muparser: disabled at compile time\n");
#endif
	MQTTVersionInfo = MQTTClient_getVersionInfo();
	i = 0;
	while (MQTTVersionInfo[i].name && MQTTVersion == NULL) {
		if (strcasecmp(MQTTVersionInfo[i].name,"Version") == 0) {
			MQTTVersion = (char *)MQTTVersionInfo[i].value;
			break;
		}
		i++;
	}
	if (MQTTVersion)
	printf("paho mqtt-c: %s\n",MQTTVersion);
	exit(2);
}

// to avoid unused error message for --configfile
int dummyCallback(argParse_handleT *a, char * arg) {
	return 0;
}

#define CONFFILEARG "--configfile="

int parseArgs (int argc, char **argv) {
	int res = 0;
	int i;
	char * dbName = NULL;
	char * serverName = NULL;
	char * userName = NULL;
	char * password = NULL;
	char * bucket = NULL;
	char * org = NULL;
	char * token = NULL;
	int syslog = 0;
	int port = 8086;
	int numQueueEntries = NUM_RECS_TO_BUFFER_ON_FAILURE;
	int influxapi=1;
	argParse_handleT *a;

	influxMeasurement = strdup(INFLUX_DEFAULT_MEASUREMENT);
	influxTagName = strdup(INFLUX_DEFAULT_TAGNAME);



	AP_START(argopt)
		AP_HELP
		AP_OPT_STRVAL_CB    (0,0,"configfile"    ,NULL                   ,"config file name",&dummyCallback)
		AP_OPT_STRVAL       (0,'d',"device"      ,&serDevice             ,"specify serial device name")
		AP_OPT_INTVAL       (1,0 ,"baud"         ,&serBaudrate           ,"baudrate")
		AP_OPT_STRVAL       (1,'a',"parity"      ,&serParity             ,"N (default), E or O")
		AP_OPT_INTVAL       (1,'S'  ,"stopbits"    ,&serStopbits           ,"1 or 2 stopbits")
		AP_OPT_INTVAL       (1,'4',"rs485"       ,&ser_rs485             ,"set rs485 mode")

		//AP_REQ_STRVAL_CB    (1,'a',"tags"        ,NULL                  ,"specify influxdb tags for each meter separated by ,", &parseTagCallback)
		AP_OPT_STRVAL       (1,'m',"measurement"    ,&influxMeasurement    ,"Influxdb measurement")
		AP_OPT_STRVAL       (1,'g',"tagname"        ,&influxTagName        ,"Influxdb tag name")
		AP_OPT_STRVAL       (1,'s',"server"         ,&serverName           ,"influxdb server name or ip")
		AP_OPT_INTVAL       (1,'o',"port"           ,&port                 ,"influxdb port")
		AP_OPT_STRVAL       (1,'b',"db"             ,&dbName               ,"Influxdb v1 database name")
		AP_OPT_STRVAL       (1,'u',"user"           ,&userName             ,"Influxdb v1 user name")
		AP_OPT_STRVAL       (1,'p',"password"       ,&password             ,"Influxdb v1 password")
		AP_OPT_STRVAL       (1,'B',"bucket"         ,&bucket               ,"Influxdb v2 bucket")
		AP_OPT_STRVAL       (1,'O',"org"            ,&org                  ,"Influxdb v2 org")
		AP_OPT_STRVAL       (0,'T',"token"          ,&token                ,"Influxdb v2 auth api token")
		AP_OPT_INTVAL       (0, 0 ,"influxwritemult",&influxWriteMult      ,"Influx write multiplicator")
		AP_OPT_INTVAL       (1,'c',"cache"          ,&numQueueEntries      ,"#entries for influxdb cache")
		AP_OPT_STRVAL       (1,'M',"mqttserver"     ,&mClient->hostname    ,"mqtt server name or ip")
		AP_OPT_STRVAL       (1,'C',"mqttprefix"     ,&mqttprefix           ,"prefix for mqtt publish")
		AP_OPT_INTVAL       (1,'R',"mqttport"       ,&mClient->port        ,"ip port for mqtt server")
		AP_OPT_INTVAL       (1,'Q',"mqttqos"        ,&mqttQOS              ,"default mqtt QOS, can be changed for meter")
		AP_OPT_INTVAL       (1,'r',"mqttretain"     ,&mqttRetain           ,"default mqtt retain, can be changed for meter")
		AP_OPT_INTVALFO     (0,'v',"verbose"        ,&log_verbosity        ,"increase or set verbose level")
		AP_OPT_INTVALF      (0,'G',"modbusdebug"    ,&modbusDebug          ,"set debug for libmodbus")
		AP_OPT_INTVAL       (0,'P',"poll"           ,&queryIntervalSecs    ,"poll intervall in seconds")
		AP_OPT_INTVALF      (0,'y',"syslog"         ,&syslog               ,"log to syslog insead of stderr")
		AP_OPT_INTVALF_CB   (0,'Y',"syslogtest"     ,NULL                  ,"send a testtext to syslog and exit",&syslogTestCallback)
		AP_OPT_INTVALF_CB   (0,'e',"version"        ,NULL                  ,"show version and exit",&showVersionCallback)
		AP_OPT_INTVALF      (0,'D',"dumpregisters"  ,&dumpRegisters        ,"Show registers read from all meters and exit, twice to show received data")
		AP_OPT_INTVALFO     (0,'U',"dryrun"         ,&dryrun               ,"Show what would be written to MQQT/Influx for one query and exit")
		AP_OPT_INTVALF      (0,'t',"try"            ,&doTry                ,"try to connect returns 0 on success")
		AP_OPT_STRVAL       (0, 0 ,"formtryt"       ,&formulaValMeterName  ,"interactive try out formula for register values for a given meter name")
		AP_OPT_INTVALF      (0, 0 ,"formtry"        ,&formulaTry           ,"interactive try out formula (global for formulas in meter definition)")
		AP_OPT_INTVALF      (1, 0 ,"scanrtu"        ,&scanRTU              ,"scan for modbus rtu devices")
	AP_END;

	// check if we have a configfile argument
	int len = strlen(CONFFILEARG);
	for (i=1;i<argc;i++) {
		if (strncmp(CONFFILEARG,argv[i],len) == 0) {
			configFileName = strdup(argv[i]+len);
			int fh = open(configFileName,O_RDONLY);
			if (fh < 0) {
				EPRINTFN("unable to open config file '%s'",configFileName);
				exit(1);
			}
			close(fh);
			LOGN(1,"using configfile \"%s\"",configFileName);
			break;
		}
	}

	if (configFileName == NULL) configFileName = strdup(CONFFILE);

	a = argParse_init(argopt, configFileName, NULL,
		"The cache will be used in case the influxdb server is down. In\n" \
        "that case data will be send when the server is reachable again.\n");
	res = argParse (a, argc, argv, 0);
	if (res != 0) {
		argParse_free (a);
		return res;
	}

	//mClient->topicPrefix = mqttprefix;

	if (doTry==0 && serverName != NULL) {
		if (org || token || bucket) influxapi++;
		//if (serverName == NULL) { EPRINTF("influx server name not specified\n"); exit(1); }
		if (influxapi == 1) {
			if (!dbName) { EPRINTF("influxdb database name not specified\n"); exit(1); }
		} else {
			if (!org) { EPRINTF("influxdb org not specified\n"); exit(1); }
			if (!bucket) { EPRINTF("influxdb bucket not specified\n"); exit(1); }
			if (!token) { EPRINTF("influxdb token not specified\n"); exit(1); }
			if (dbName) EPRINTF("Warning: database name ignored for influxdb v2 api\n");
			if (userName) EPRINTF("Warning: user name ignored for influxdb v2 api\n");
			if (password) EPRINTF("Warning: password ignored for influxdb v2 api\n");
		}
	}

	if (mClient->hostname) {
		if (!mqttprefix) {
			EPRINTF("mqttprefix required\n"); exit(1);
		}
	}

	if (syslog) log_setSyslogTarget(ME);

	if (doTry == 0) {
		if (serverName) {
			LOG(1,"Influx init: serverName: %s, port %d, dbName: %s, userName: %s, password: %s, org: %s, bucket:%s, numQueueEntries %d\n",serverName, port, dbName, userName, password, org, bucket, numQueueEntries);
			iClient = influxdb_post_init (serverName, port, dbName, userName, password, org, bucket, token, numQueueEntries);
		} else {
			free(dbName);
			free(serverName);
			free(userName);
			free(password);
			free(bucket);
			free(org);
			free(token);
		}
	}

	argParse_free (a);

    return 0;
}


int terminated;


void sigterm_handler(int signum) {
	LOGN(0,"sigterm_handler called, terminating");
	signal(SIGINT, NULL);
	terminated++;
}



void sigusr1_handler(int signum) {
	log_verbosity++;
	LOGN(0,"verbose: %d",log_verbosity);
}

void sigusr2_handler(int signum) {
	if (log_verbosity) log_verbosity--;
	LOGN(0,"verbose: %d",log_verbosity);
}


#define INITIAL_BUFFER_LEN 256

void appendToStr (const char *src, char **dest, int *len, int *bufsize) {
	int srclen;

	if (src == NULL) return;
	if (*src == 0) return;

	srclen = strlen(src);
	if (*len + srclen + 1 > *bufsize) {
		*bufsize *= 2;
		//printf("Realloc to %d, len=%d, srclen: %d %x %x %s\n",*bufsize,*len,srclen,dest,*dest,*dest);
		*dest = (char *)realloc(*dest,*bufsize);
		if (*dest == NULL) { EPRINTF("Out of memory in appendToStr"); exit(1); };
	}
	strcat(*dest,src);
	*len += srclen;
}

#define VALBUFLEN 64
void appendValue (int includeName, meterRegisterRead_t *rr, char **dest, int *len, int *bufsize) {
	char valbuf[VALBUFLEN];
	char format[30];
	char *nameBuf;
	int nameBufSize;
	char *p = &valbuf[0];

	if (rr->isInt) {
#ifdef BUILD_64
		snprintf(valbuf,VALBUFLEN,"%d",(int) rr->fvalue);
#else
		snprintf(valbuf,VALBUFLEN,"%ld",(int) rr->fvalue);
#endif
	} else {
		snprintf(format,sizeof(format),"%%%d.%df",10+rr->registerDef->decimals,rr->registerDef->decimals);
		snprintf(valbuf,VALBUFLEN,format,rr->fvalue);
		while (*p && *p<=32) p++;
	}
	if (includeName) {
		nameBufSize = strlen(rr->registerDef->name)+10;
		nameBuf = (char *)malloc(nameBufSize);
		snprintf(nameBuf,nameBufSize,"\"%s\":",rr->registerDef->name);
		appendToStr(nameBuf,dest,len,bufsize);
		free(nameBuf);
	}

	appendToStr(p,dest,len,bufsize);
}


void appendFormulaValue (int includeName, meterFormula_t *mf, char **dest, int *len, int *bufsize) {
	char valbuf[VALBUFLEN];
	char format[30];
	char *nameBuf;
	int nameBufSize;
	char *p = &valbuf[0];


	if (mf->forceType == force_int) {
#ifdef BUILD_64
		snprintf(valbuf,VALBUFLEN,"%d",(int) mf->fvalue);
#else
		snprintf(valbuf,VALBUFLEN,"%ld",(int) mf->fvalue);
#endif
	} else {
		snprintf(format,sizeof(format),"%%%d.%df",10+mf->decimals,mf->decimals);
		snprintf(valbuf,VALBUFLEN,format,mf->fvalue);
		while (*p && *p<=32) p++;
	}
	if (includeName) {
		nameBufSize = strlen(mf->name)+10;
		nameBuf = (char *)malloc(nameBufSize);
		snprintf(nameBuf,nameBufSize,"\"%s\":",mf->name);
		appendToStr(nameBuf,dest,len,bufsize);
		free(nameBuf);
	}

	appendToStr(p,dest,len,bufsize);
}



#define APPEND(SRC) appendToStr(SRC,&buf,&buflen,&bufsize)

int mqttSendData (meter_t * meter,int dryrun) {
	int bufsize = INITIAL_BUFFER_LEN;
	char *buf;
	int buflen = 0;
	int first = 1;
	meterRegisterRead_t *rr = meter->registerRead;
	meterFormula_t *mf = meter->meterFormula;
	char *arrayName;
	char emptyStr = 0;
	int rc = 0;

	// check if we have something to write
	if (meter->disabled) return 0;
	if (! meter->numEnabledRegisters_mqtt) return 0;

	buf = (char *)malloc(bufsize);
	if (buf == NULL) return -1;
	*buf=0;

	arrayName = &emptyStr;
	APPEND("{");
	// registers from meter type
	while (rr) {
        if (rr->registerDef->enableMqttWrite) {
            if (rr->registerDef->arrayName) {
                if (strcmp(arrayName,rr->registerDef->arrayName) != 0) {		// start new array
                    if (strlen(arrayName)) APPEND("]");						// close previous array
                    arrayName = rr->registerDef->arrayName;
                    if (!first) APPEND(", ");
                    first = 0;
                    APPEND("\""); APPEND(arrayName); APPEND("\":[");			// start new array
                    appendValue(0,rr,&buf,&buflen,&bufsize);
                } else {				// add to array as long as we are in the same array
                    APPEND(", ");
                    appendValue(0,rr,&buf,&buflen,&bufsize);
                }
            } else {		// register not as array
                if (strlen(arrayName)) { APPEND("]"); arrayName = &emptyStr; } // close previous array
                if (!first) APPEND(", ");
                first = 0;
                appendValue(1,rr,&buf,&buflen,&bufsize);
            }
        }
		rr = rr->next;
	}

	// registers from meter specific formulas
	while (mf) {
		if (mf->enableMqttWrite) {
            if (mf->arrayName) {
                if (strcmp(arrayName,mf->arrayName) != 0) {					// start new array
                    if (strlen(arrayName)) APPEND("]");						// close previous array
                    arrayName = mf->arrayName;
                    if (!first) APPEND(", ");
                    first = 0;
                    APPEND("\""); APPEND(arrayName); APPEND("\":[");			// start new array
                    appendFormulaValue(0,mf,&buf,&buflen,&bufsize);
                } else {				// add to array as long as we are in the same array
                    APPEND(", ");
                    appendFormulaValue(0,mf,&buf,&buflen,&bufsize);
                }
            } else {		// register not an array
                if (strlen(arrayName)) { APPEND("]"); arrayName = &emptyStr; } // close previous array
                if (!first) APPEND(", ");
                first = 0;
                appendFormulaValue(1,mf,&buf,&buflen,&bufsize);
            }
        }
		mf = mf->next;
	}

	if (strlen(arrayName)) APPEND("]");

	APPEND("}");
	if (dryrun) {
		printf("%s = %s\n",meter->name,buf);
	} else {
		mClient->topicPrefix = meter->mqttprefix;
		rc = mqtt_pub_strF (mClient,meter->name, 0, meter->mqttQOS,meter->mqttRetain, buf);
		if (rc != 0) LOGN(0,"mqtt publish failed with rc: %d",rc);
		mClient->topicPrefix = NULL;
	}

	free(buf);
	return rc;
}



int influxAppendData (meter_t *meter, uint64_t timestamp) {
	meterRegisterRead_t *rr;
	meterFormula_t *mf;
	int regCount = 0;

	// use the global measurement or the one from the meter (if defined)
	char * measurement = influxMeasurement;
	if (meter->influxMeasurement) measurement = meter->influxMeasurement;

	char * tagname = influxTagName;
	if (meter->influxTagName) tagname = meter->influxTagName;

	// check if we have something to write
	if (meter->disabled) return 0;
	if (! meter->numEnabledRegisters_influx) return 0;

	influxBufUsed = influxdb_format_line(&influxBuf, &influxBufLen, influxBufUsed, INFLUX_MEAS(measurement), INFLUX_TAG(tagname, meter->name),INFLUX_END);
	if (influxBufUsed < 0) { EPRINTFN("influxdb_format_line failed, INFLUX_MEAS"); exit(1); }

    rr = meter->registerRead;
	while (rr) {
        if (rr->registerDef->enableInfluxWrite) {
            if (rr->isInt) {
                influxBufUsed = influxdb_format_line(&influxBuf, &influxBufLen, influxBufUsed, INFLUX_F_INT(rr->registerDef->name, (int)rr->fvalueInflux), INFLUX_END);
                if (influxBufUsed < 0) { EPRINTFN("influxdb_format_line failed, INFLUX_F_INT"); exit(1); }
            } else {
                influxBufUsed = influxdb_format_line(&influxBuf, &influxBufLen, influxBufUsed, INFLUX_F_FLT(rr->registerDef->name, rr->fvalueInflux, rr->registerDef->decimals), INFLUX_END);
                if (influxBufUsed < 0) { EPRINTFN("influxdb_format_line failed, INFLUX_F_FLT"); exit(1); }
            }
        }
        regCount++;
		rr = rr->next;
	}

	// meter specific register formulas
	mf = meter->meterFormula;
	while (mf) {
		if (mf->forceType == force_int) {
			influxBufUsed = influxdb_format_line(&influxBuf, &influxBufLen, influxBufUsed, INFLUX_F_INT(mf->name, (int)mf->fvalueInflux) ,INFLUX_END);
			if (influxBufUsed < 0) { EPRINTFN("influxdb_format_line failed, INFLUX_F_INT"); exit(1); }
		} else {
			influxBufUsed = influxdb_format_line(&influxBuf, &influxBufLen, influxBufUsed, INFLUX_F_FLT(mf->name, mf->fvalueInflux, mf->decimals), INFLUX_END);
			if (influxBufUsed < 0) { EPRINTFN("influxdb_format_line failed, INFLUX_F_FLT"); exit(1); }
		}
		mf = mf->next;
	}
	influxBufUsed = influxdb_format_line(&influxBuf, &influxBufLen, influxBufUsed, INFLUX_TS(timestamp), INFLUX_END);
	if (influxBufUsed < 0) { EPRINTFN("influxdb_format_line failed, INFLUX_TS"); exit(1); }


	return regCount;
}



void traceCallback(enum MQTTCLIENT_TRACE_LEVELS level, char *message) {
	printf(message); printf("\n");
}

#define NANO_PER_SEC 1000000000.0

int main(int argc, char *argv[]) {
	int rc,i;
	meter_t *meter;
	uint64_t influxTimestamp;
	struct timespec timeStart, timeEnd;
	time_t nextQueryTime;
	int isFirstQuery = 1;  // takes longer due to init and/or getting sunspec id's
	double queryTime;

	//printf("byte_order: %d\n",__BYTE_ORDER);

	if (LIBMODBUS_VERSION_MAJOR != libmodbus_version_major || LIBMODBUS_VERSION_MINOR != libmodbus_version_minor /*|| LIBMODBUS_VERSION_MICRO != libmodbus_version_micro */) {
		EPRINTFN("%s: compiled with libmodbus %d.%d.%d but the version loaded is %d.%d.%d, recompile required",argv[0],LIBMODBUS_VERSION_MAJOR,LIBMODBUS_VERSION_MINOR,LIBMODBUS_VERSION_MICRO,libmodbus_version_major,libmodbus_version_minor,libmodbus_version_micro);
		exit(1);
	}

	mqttprefix = strdup(MQTT_PREFIX_DEF);

	mClient = mqtt_pub_init (NULL, 0, (char *)MQTT_CLIENT_ID, NULL);

	if (parseArgs(argc,argv) != 0) exit(1);

// for valgrind leak check
#if 0
	printf("read definitions\n");
	readMeterDefinitions (CONFFILE);
	printf("free meters\n");
	freeMeters();
	free(mqttprefix);
	if (mClient) mqtt_pub_free(mClient);
	influxdb_post_free(iClient);
	exit(1);
#endif



	// open serial port, needed by readMeterDefinitions if we have Modbus RTU connections
	if (serDevice)	// not needed if we only have TCP connections
		if (modbusRTU_open (serDevice,serBaudrate,serParity,serStopbits,ser_rs485) != 0) {
			EPRINTF("%s: unable to open serial port at %s\n",argv[0],serDevice);
			exit(3);
		}

	if (scanRTU) {
		if (! serDevice) {
			EPRINTFN("scanrtu: no serial port specified");
			exit(1);
		}
		modbus_t ** mb = modbusRTU_getmh();
		uint16_t regValue;
		printf("scanning for modbus RTU devices\n");
		for (i=1;i<254;i++) {
			printf("\r%d ",i); fflush(stdout);
			modbus_set_slave(*mb,i);
			rc = modbus_read_registers(*mb, 0,1,&regValue);
			if (rc == 1) rc = 0;
			if (rc == -1) rc = errno;
			VPRINTFN(1,"modbus_read_registers for slave %d returned %d (%s)",i,rc,modbus_strerror(rc));
			if (rc == 0) printf("\rfound device at id %d\n",i);
			msleep(100);
		}
		exit(0);
	}

	readMeterDefinitions (configFileName);

	if (dumpRegisters) {
		printf("Querying all meters\n" \
			   "===================\n");
		rc = queryMeters(dumpRegisters);
		setTarif (1);
		exit(1);
	}

	if (formulaValMeterName) {
        if (findMeter(formulaValMeterName) == NULL) {
            EPRINTFN("Invalid meter name '%s'",formulaValMeterName);
            exit(1);
        }
	    printf("Querying all meters before formula test\n" \
			   "=======================================\n");
		rc = queryMeters(dumpRegisters);
		setTarif (1);
		testRegCalcFormula(formulaValMeterName);
        exit(1);
	}

	if (formulaTry) {
	    printf("Querying all meters before formula test\n" \
			   "=======================================\n");
		rc = queryMeters(dumpRegisters);
		setTarif (1);
		testRegCalcFormula(NULL);
        exit(1);
	}

	if (sizeof(time_t) <= 4) {
		LOGN(0,"Warning: TimeT is less than 64 bit, this may fail after year 2038, recompile with newer kernel and glibc to avoid this");
	}

	if (!iClient) LOGN(0,"no influxdb host specified, influx sender disabled");


	if (!mClient->hostname) {
		mqtt_pub_free(mClient);
		mClient = NULL;
		LOGN(0,"no mqtt host specified, mqtt sender disabled");
		if (!iClient) {
			EPRINTFN("No mqtt host and no influxdb host specified, specify one or both");
			exit(1);
		}
	} else {
		rc = mqtt_pub_connect (mClient);
		if (rc != 0) LOGN(0,"mqtt_pub_connect returned %d, will retry later",rc);
	}

	if (doTry) {
	    if (!serDevice) {
	        EPRINTFN("try: no serial port defined");
            exit(1);
	    }
	    if (testRTUpresent()) {
            VPRINTFN(1,"found meter on %s",serDevice);
            exit(0);
	    }
	    VPRINTFN(1,"no meter found on %s",serDevice);
	    exit(1);
	}

	// term handler for ^c and SIGTERM send by systemd
	//signal(SIGKILL, sigterm_handler);
	signal(SIGTERM, sigterm_handler);
	signal(SIGINT, sigterm_handler);

	signal(SIGUSR1, sigusr2_handler);	// used for verbose level inc/dec via kill command
	signal(SIGUSR2, sigusr1_handler);

	LOGN(0,"mainloop started (%s %s)",ME,VER);

	int loopCount = 0;
	while (!terminated) {
		nextQueryTime = time(NULL) + queryIntervalSecs;
        loopCount++;
        if (dryrun) printf("- %d -----------------------------------------------------------------------\n",loopCount);
        if (dryrun || verbose>0) clock_gettime(CLOCK_REALTIME,&timeStart);
		rc = queryMeters(verbose);
		setTarif (verbose);
		clock_gettime(CLOCK_REALTIME,&timeEnd);
		queryTime = (double)(timeEnd.tv_sec + timeEnd.tv_nsec / NANO_PER_SEC)-(double)(timeStart.tv_sec + timeStart.tv_nsec / NANO_PER_SEC);
		if (dryrun || verbose>0)
			printf("Query took %4.2f seconds\n",queryTime);

		if (rc >= 0) {
			if (iClient) {		// influx
				influxBufUsed = 0; influxBuf=NULL;
				influxTimestamp = influxdb_getTimestamp();
				meter = meters;
				while(meter) {
                    if(meter->influxWriteCountdown == 0) {
                        influxAppendData (meter, influxTimestamp);
                        meter->influxWriteCountdown = meter->influxWriteMult;
                    }
					meter = meter->next;
				}
				if (dryrun) {
                    if (influxBuf) printf("\nDryrun: would send to influxdb:\n%s\n",influxBuf);
					free(influxBuf); influxBuf = NULL;
				} else {
					rc = influxdb_post_http_line(iClient, influxBuf);
					influxBuf=NULL;
					if (rc != 0) {
						LOGN(0,"Error: influxdb_post_http_line failed with rc %d",rc);
					}
				}
			}

			if (mClient) {		// mqtt
				if (dryrun) printf("Dryrun: would send to mqtt:\n");
				meter = meters;
				while(meter) {
					mqttSendData (meter,dryrun);
					meter = meter->next;
				}

			}
		}

		if (time(NULL) >= nextQueryTime && !isFirstQuery)
			LOGN(0,"Warning: query took more time (%1.2f seconds) than the defined poll interval of %d seconds",queryTime,queryIntervalSecs);

		while (time(NULL) < nextQueryTime && !terminated) {
			msleep(100);
		}

		if (isFirstQuery) isFirstQuery--;

        if (dryrun) {
            dryrun--;
            if (!dryrun) terminated++;
        }
	}

#ifndef DISABLE_FORMULAS
	freeFormulaParser();
#endif // DISABLE_FORMULAS

	modbusTCP_freeAll();

	if (mClient) mqtt_pub_free(mClient);
	influxdb_post_free(iClient);

    free(configFileName);
	free(mqttprefix);

	free(influxMeasurement);
	free(influxTagName);
	free(serDevice);
	free(serParity);
	freeMeters();

	LOGN(0,"terminated");

	return 0;
}
