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
#ifndef DISABLE_MQTT
#include "mqtt_publish.h"
#endif

#include "influxdb-post/influxdb-post.h"
#include "meterDef.h"
#include "modbusread.h"
#include "global.h"
#include "parser.h"
#include <modbus.h>
#include "global.h"
#include <endian.h>
#include "cron.h"
#include <inttypes.h>
#ifdef CURL_STATIC
#include "curl/curl.h"
#else
#include <curl/curl.h>
#endif


#ifndef DISABLE_FORMULAS
#include "muParser.h"
#endif

#ifndef DISABLE_MQTT
#include "MQTTClient.h"
#endif

#define VER "1.39 Armin Diehl <ad@ardiehl.de> Oct 1,2025 compiled " __DATE__ " " __TIME__ " "
#define ME "emModbus2influx"
#define CONFFILE "emModbus2influx.conf"

#define MQTT_CLIENT_ID ME

#define NUM_RECS_TO_BUFFER_ON_FAILURE 1000

// this is for grafana live (via MQTT and telegraf), if we only send data when changed
// grafana will, after some time, show "NO DATA"
#define MQTT_SEND_UNCHANGED

#ifndef DISABLE_FORMULAS
extern double formulaNumPolls;
#endif
char *configFileName;
char * serDevice;
char * serBaudrate;
char *serParity;
char *serStopbits;
char * ser_rs485;
int dumpRegisters;
int dryrun;
int queryIntervalSecs = 5;	// seconds
char *formulaValMeterName;
int formulaTry;
int scanRTU;
int scanRTUReg;
int modbusDebug;
influx_client_t *iClient;
int showModbusRetries;
int ModbusRTU_exitErrorCount = 5;

#ifndef DISABLE_MQTT
mqtt_pubT *mClient;
#endif

#define MQTT_PREFIX_DEF "ad/house/energy/"


int doTry   = false;

#define INFLUX_DEFAULT_MEASUREMENT "energyMeter"
#define INFLUX_DEFAULT_TAGNAME "Meter"
char * influxMeasurement;
char * influxTagName;
int influxWriteMult;    // write to influx only on x th query (>=2)
int iVerifyPeer = 1;
int mqttQOS;
int mqttRetain;
int mqttFormat;
int mqttDelayMs;
char * mqttprefix;
char * mqttstatprefix;
char * cronExpression;

int scan;
int scanStart;
int scanEnd = 0xffff;
char * scanHost;
char * scanPort = strdup("502");
int scanAddr;
int scanInput;
int scanHolding;

// Grafana Live
char *ghost;
int gport = 3000;
char *gtoken;
char *gpushid;
char *gpushStatId;
int gUseInfluxMeasurement;
int gVerifyPeer = 1;
influx_client_t *gClient;

void scanAddresses() {
	modbus_t *ctx;
	int reg;
	uint16_t value;
	int res;
	int isSerial = 0;
	int numFound;

	if (!scanInput && !scanHolding) {
		scanInput++; scanHolding++;
	}

	if (scanHost) {
		ctx = modbus_new_tcp_pi(scanHost, scanPort);
	} else {
		ctx = *modbusRTU_getmh(0,"scanAddresses");
		isSerial++;
	}
	modbus_set_debug(ctx,modbusDebug);

	if (!isSerial && scanAddr == 0) scanAddr = 0xff;

	if (!scanAddr) {
		fprintf(stderr, "no slave address specified\n");
		return;
	}


	res = modbus_connect(ctx);
	if (res != 0) {
		if (scanHost) {
			fprintf(stderr,"unable to connect to %s:%s (%s)\n",scanHost,scanPort,modbus_strerror(errno));
		} else
			fprintf(stderr,"unable to connect to Serial0 (%s)\n",modbus_strerror(errno));
		return;
	}

	res = modbus_set_slave(ctx, scanAddr);
	if (res == -1) {
		fprintf(stderr, "Invalid slave address\n");
		modbus_free(ctx);
		return;
	}

	fprintf(stderr,"connected to %s, slave address %d\n",scanHost ? scanHost : "Serial0", scanAddr);

	if (scanEnd < scanStart) scanEnd = scanStart+1;
	printf("Scanning from %d to %d\n",scanStart,scanEnd);

	if (scanHolding) {
		printf("Scanning Holding registers (function 3):\n");
		numFound = 0;
		for (reg=scanStart; reg<=scanEnd; reg++) {
			res = modbus_read_registers(ctx, reg,1,&value);
			if (res == ETIMEDOUT) {
				msleep(50);
				res = modbus_read_registers(ctx, reg,1,&value);
			}
			if (reg % 10 == 0) { printf("\r%d ",reg); fflush(stdout); }
			if (res >= 0) {
				printf("\r%d (0x%04x): 0x%04x (%d)\n",reg,reg,value,value);
				numFound++;
			}
			if (verbose && (res <= 0)) printf("\r%d Res:%d (%s)\n",reg,errno,modbus_strerror(errno));
			if (isSerial) {
					modbusRTU_SilentDelay(modbusRTU_getBaudrate(0));
					modbusRTU_SilentDelay(modbusRTU_getBaudrate(0));

			}
			//msleep(250);
		}
		printf("\rfound %d holding registers\n\n",numFound);
	}

	if (scanInput) {
		printf("\nInput registers (function 4):\n");
		numFound = 0;
		for (reg=scanStart; reg<=scanEnd; reg++) {
			res = modbus_read_input_registers(ctx, reg, 1,&value);
			if (res == ETIMEDOUT) {
				msleep(50);
				res = modbus_read_input_registers(ctx, reg,1,&value);
			}
			if (reg % 10 == 0) { printf("\r%d ",reg); fflush(stdout); }
			if (res >= 0) {
				printf("\r%d (0x%04x): 0x%04x (%d)\n",reg,reg,value,value);
				numFound++;
			}
			if (verbose && (res <= 0)) printf("\r%d Res:%d (%s)\n",reg,errno,modbus_strerror(errno));
			if (isSerial) modbusRTU_SilentDelay(modbusRTU_getBaudrate(0));
		}
		printf("\rfound %d input registers\n\n",numFound);
	}
}



int syslogTestCallback(argParse_handleT *a, char * arg) {
	VPRINTF(0,"%s : sending testtext via syslog\n\n",ME);
	log_setSyslogTarget(ME);
	VPRINTF(0,"testtext via syslog by %s",ME);
	exit(0);
}


int showVersionCallback(argParse_handleT *a, char * arg) {
#ifndef DISABLE_MQTT
	MQTTClient_nameValue* MQTTVersionInfo;
	char *MQTTVersion = NULL;
	int i;
#endif

	printf("%s %s\n",ME,VER);
	printf("  libmodbus: %d.%d.%d - ",LIBMODBUS_VERSION_MAJOR,LIBMODBUS_VERSION_MINOR,LIBMODBUS_VERSION_MICRO);
#ifdef MODBUS_STATIC
	printf("static\n");
#else
	printf("dynamic\n");
#endif
#ifndef DISABLE_FORMULAS
	printf("   muparser: %s - ",mu::ParserVersion.c_str());
#ifdef MUPARSER_STATIC
	printf("static\n");
#else
	printf("dynamic\n");
#endif
#else
	printf("  muparser: disabled at compile time\n");
#endif

#ifndef DISABLE_MQTT
	MQTTVersionInfo = MQTTClient_getVersionInfo();
	i = 0;
	while (MQTTVersionInfo[i].name && MQTTVersion == NULL) {
		if (strcasecmp(MQTTVersionInfo[i].name,"Version") == 0) {
			MQTTVersion = (char *)MQTTVersionInfo[i].value;
			break;
		}
		i++;
	}
	if (MQTTVersion) {
		printf("paho mqtt-c: %s - ",MQTTVersion);
#ifdef PAHO_STATIC
		printf("static\n");
#else
		printf("dynamic\n");
#endif
	}
#else
	printf("paho mqtt-c: disabled at compile time\n");
#endif

	curl_version_info_data *ver = curl_version_info(CURLVERSION_NOW);
	printf("    libcurl: %u.%u.%u - ", (ver->version_num >> 16) & 0xff, (ver->version_num >> 8) & 0xff, ver->version_num & 0xff);
#ifdef CURL_STATIC
		printf("static\n");
#else
		printf("dynamic\n");
#endif

#ifdef __GNUC__
	printf("        gcc: " __VERSION__ "\n");
#endif
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
	char * influxApiStr = NULL;
	int syslog = 0;
	int port = 8086;
	int numQueueEntries = NUM_RECS_TO_BUFFER_ON_FAILURE;
	int influxapi=1;
	argParse_handleT *a;

	influxMeasurement = strdup(INFLUX_DEFAULT_MEASUREMENT);
	influxTagName = strdup(INFLUX_DEFAULT_TAGNAME);
	serBaudrate = strdup("9600");
	serStopbits = strdup("1");


	AP_START(argopt)
		AP_HELP
		AP_OPT_STRVAL_CB    (0,0,"configfile"    ,NULL                   ,"config file name",&dummyCallback)
		AP_OPT_STRVAL       (0,'d',"device"      ,&serDevice             ,"specify serial device names separated by ,")
		AP_OPT_STRVAL       (1,0 ,"baud"         ,&serBaudrate           ,"baudrates separated by ,")
		AP_OPT_STRVAL       (1,'a',"parity"      ,&serParity             ,"N (default), E or O")
		AP_OPT_STRVAL       (1,'S'  ,"stopbits"    ,&serStopbits           ,"1 or 2 stopbits")
		AP_OPT_STRVAL       (1,'4',"rs485"       ,&ser_rs485             ,"set rs485 mode")

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
		AP_OPT_INTVAL       (1,0  ,"isslverifypeer" ,&iVerifyPeer          ,"Influx SSL certificate verification (0=off)")
		AP_OPT_STRVAL       (0,'A',"influxapi"      ,&influxApiStr         ,"Influxdb api string, if specified db..token will not be used")
		AP_OPT_INTVAL       (1,'c',"cache"          ,&numQueueEntries      ,"#entries for influxdb cache")
#ifndef DISABLE_MQTT
		AP_OPT_STRVAL       (1,'M',"mqttserver"     ,&mClient->hostname    ,"mqtt server name or ip")
		AP_OPT_STRVAL       (1,'C',"mqttprefix"     ,&mqttprefix           ,"prefix for mqtt publish")
		AP_OPT_STRVAL       (1,0  ,"mqttstatprefix" ,&mqttstatprefix       ,"prefix for mqtt statistics publish")
		AP_OPT_INTVAL       (1,'R',"mqttport"       ,&mClient->port        ,"ip port for mqtt server")
		AP_OPT_INTVAL       (1,'Q',"mqttqos"        ,&mqttQOS              ,"default mqtt QOS, can be changed for meter")
		AP_OPT_INTVAL       (1,'l',"mqttdelay"      ,&mqttDelayMs          ,"delay milliseconds after mqtt publish")
		AP_OPT_INTVAL       (1,'r',"mqttretain"     ,&mqttRetain           ,"default mqtt retain, can be changed for meter")
		AP_OPT_STRVAL       (1,'i',"mqttclientid"   ,&mClient->clientId    ,"mqtt client id")
		AP_OPT_INTVAL       (1,0  ,"mqttformat"     ,&mqttFormat           ,"json format, 0=std,1=Logo array")

#endif
		AP_OPT_STRVAL       (1,0  ,"ghost"          ,&ghost                ,"grafana server url w/o port, e.g. ws://localost or https://localhost")
		AP_OPT_INTVAL       (1,0  ,"gport"          ,&gport                ,"grafana port")
		AP_OPT_STRVAL       (1,0  ,"gtoken"         ,&gtoken               ,"authorisation api token for Grafana")
		AP_OPT_STRVAL       (1,0  ,"gpushid"        ,&gpushid              ,"push id for Grafana")
		AP_OPT_STRVAL       (1,0  ,"gpushstatid"    ,&gpushStatId          ,"statistics push id for Grafana")
		AP_OPT_INTVAL       (1,0  ,"ginfluxmeas"    ,&gUseInfluxMeasurement,"use influx measurement names for grafana as well")
		AP_OPT_INTVAL       (1,0  ,"gsslverifypeer" ,&gVerifyPeer          ,"grafana SSL certificate verification (0=off)")

		AP_OPT_INTVALFO     (0,'v',"verbose"        ,&log_verbosity        ,"increase or set verbose level")
		AP_OPT_INTVALF      (0,'G',"modbusdebug"    ,&modbusDebug          ,"set debug for libmodbus")
		AP_OPT_INTVALF      (0,0,"modbusshowretries",&showModbusRetries    ,"show retries on failed modbus reads")

		AP_OPT_INTVAL       (0,'P',"poll"           ,&queryIntervalSecs    ,"poll intervall in seconds")
		AP_OPT_STRVAL       (0, 'H',"cron"          ,&cronExpression       ,"Crontab style expression like Sec Min Hour Day Mon Wday")
		AP_OPT_INTVALF      (0, 0 ,"no-threads"     ,&disableThreadedQuery  ,"enable threaded query (one thread for each serial port and one for tcp)")

		AP_OPT_INTVAL       (1,0  ,"maxrtuerrs"     ,&ModbusRTU_exitErrorCount,"exit if all meters on a serial port failed n times")

		AP_OPT_INTVALF      (0,'y',"syslog"         ,&syslog               ,"log to syslog insead of stderr")
		AP_OPT_INTVALF_CB   (0,'Y',"syslogtest"     ,NULL                  ,"send a testtext to syslog and exit",&syslogTestCallback)
		AP_OPT_INTVALF_CB   (0,'e',"version"        ,NULL                  ,"show version and exit",&showVersionCallback)
		AP_OPT_INTVALF      (0,'D',"dumpregisters"  ,&dumpRegisters        ,"Show registers read from all meters and exit, twice to show received data")
		AP_OPT_INTVALFO     (0,'U',"dryrun"         ,&dryrun               ,"Show what would be written to MQTT/Influx for one query and exit")
		AP_OPT_INTVALF      (0,'t',"try"            ,&doTry                ,"try to connect returns 0 on success")
		AP_OPT_STRVAL       (0, 0 ,"formtryt"       ,&formulaValMeterName  ,"interactive try out formula for register values for a given meter name")
		AP_OPT_INTVALF      (0, 0 ,"formtry"        ,&formulaTry           ,"interactive try out formula (global for formulas in meter definition)")
		AP_OPT_INTVALF      (1, 0 ,"scanrtu"        ,&scanRTU              ,"scan for modbus rtu devices")
		AP_OPT_INTVAL       (1, 0 ,"scanreg"        ,&scanRTUReg           ,"register to be used for scanrtu")
		AP_OPT_INTVALF      (1, 0 ,"scan"           ,&scan                 ,"scan a device for available registers")
		AP_OPT_INTVAL       (1, 0 ,"scanstart"      ,&scanStart            ,"register to start scan with")
		AP_OPT_INTVAL       (1, 0 ,"scanend"        ,&scanEnd              ,"register to end scan with")
		AP_OPT_STRVAL       (0, 0 ,"scanhost"       ,&scanHost             ,"TCP hostname, if not specified Modbus RTU will be used for scan")
		AP_OPT_STRVAL       (0, 0 ,"scanport"       ,&scanPort             ,"TCP port number or name")
		AP_OPT_INTVAL       (1, 0 ,"scanaddr"       ,&scanAddr             ,"Modbus address to scan")
		AP_OPT_INTVALF      (1, 0 ,"scaninput"      ,&scanInput            ,"scan input registers (default=both)")
		AP_OPT_INTVALF      (1, 0 ,"scanholding"    ,&scanHolding          ,"scan holding registers (default=both)")
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
		if (!influxApiStr) {
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
	}
#ifndef DISABLE_MQTT
	if (mClient->hostname) {
		if (!mqttprefix) {
			EPRINTF("mqttprefix required\n"); exit(1);
		}
	}
#endif

	if (syslog) log_setSyslogTarget(ME);

	if (doTry == 0) {
		if (serverName) {
			LOG(1,"Influx init: serverName: %s, port %d, dbName: %s, userName: %s, password: %s, org: %s, bucket:%s, numQueueEntries %d, influxApiStr: %s\n",serverName, port, dbName, userName, password, org, bucket, numQueueEntries, influxApiStr);
			iClient = influxdb_post_init (serverName, port, dbName, userName, password, org, bucket, token, numQueueEntries, influxApiStr, iVerifyPeer);
		}
	}

	free(dbName);
	free(serverName);
	free(userName);
	free(password);
	free(bucket);
	free(org);
	free(token);
	free(influxApiStr);

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
	while (*len + srclen + 1 > *bufsize) {
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
		//snprintf(valbuf,VALBUFLEN,"%lld",(int64_t) rr->fvalue);
		snprintf(valbuf,VALBUFLEN,"%" PRId64,(int64_t) rr->fvalue);
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
		snprintf(valbuf,VALBUFLEN,"%" PRId64,(int64_t) mf->fvalue);
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

#ifndef DISABLE_MQTT
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
	char *measurement;
	char valBuf[VALBUFLEN];
	double timeSecs;
	int numRegs = 0;

	// check if we have something to write
	if (meter->disabled) return 0;
	if (! meter->numEnabledRegisters_mqtt) return 0;

	buf = (char *)malloc(bufsize);
	if (buf == NULL) return -1;
	*buf=0;

	switch (meter->mqttFormat) {
		case mqttFormatStd:
			arrayName = &emptyStr;
			APPEND("{\"name\":\"");
			measurement = meter->influxMeasurement;
			if (! measurement) measurement = influxMeasurement;
			if (measurement) {
				APPEND(measurement); APPEND(".");
			}
			APPEND(meter->name);
			APPEND("\", ");

			// registers from meter type
			while (rr) {
				if (rr->registerDef->enableMqttWrite) {
					numRegs++;
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
					numRegs++;
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
			break;
		case mqttFormatLogoArr:
			// {"state":{"var1":{"value":[0,0]},"var2":{"value":[0]}}}
			APPEND("{\"state\":{");

			// registers from meter type
			while (rr) {
				if (rr->registerDef->enableMqttWrite) {
					numRegs++;
					if (!first) APPEND(",");
					first = 0;
					APPEND("\"");
					APPEND(rr->registerDef->name);
					APPEND("\":{\"value\":[");
					appendValue(0,rr,&buf,&buflen,&bufsize);
					APPEND("]}");
				}
				rr = rr->next;
			}

			// registers from meter specific formulas
			while (mf) {
				if (mf->enableMqttWrite) {
					numRegs++;
					if (!first) APPEND(",");
					first = 0;
					first = 0;
					APPEND("\"");
					APPEND(mf->name);
					APPEND("\":{\"value\":[");
					appendFormulaValue(0,mf,&buf,&buflen,&bufsize);
					APPEND("]}");
				}
				mf = mf->next;
			}
			APPEND("}}");
			break;
		default:
			EPRINTFN("Unsupported mqtt format (%d)",(int)meter->mqttFormat);
			exit(1);
	}

    int doWrite = 1;        // if mqtt retain is send, only write if data has been changed
#ifndef MQTT_SEND_UNCHANGED
    if (meter->mqttLastSend && meter->mqttRetain) {
        if (strcmp(buf,meter->mqttLastSend) == 0) doWrite--;
    }
#endif
	if (!dryrun) VPRINTFN(3,"%s: retain: %d, qos: %d, mqttLastSend: '%s', buf: '%s', doWrite: %d",meter->name,meter->mqttRetain,meter->mqttQOS,meter->mqttLastSend,buf,doWrite);

	if (dryrun) {
		printf("%s%s %s %s\n",meter->mqttprefix,meter->name,buf,doWrite ? "" : "- no change, will not be written");
	} else {
        if (doWrite) {
            mClient->topicPrefix = meter->mqttprefix;
            rc = mqtt_pub_strF (mClient,meter->name, 0, meter->mqttQOS,meter->mqttRetain, buf);
            // message not needeed, we already has the disconnect message
            //if (rc != MQTTCLIENT_SUCCESS && rc != MQTT_RECONNECTED) LOGN(0,"mqtt publish failed with rc: %d (connected=%d)",rc,MQTTClient_isConnected(mClient->client));
            mClient->topicPrefix = NULL;
        }
	}
#ifndef MQTT_SEND_UNCHANGED
    free(meter->mqttLastSend);
	meter->mqttLastSend = buf;
#else
	free(buf);
#endif

	if (numRegs && mqttstatprefix) { //mqttstatprefix && meter->registerRead && !meter->meterType->isFormulaOnly) {
		buf = (char *)malloc(bufsize);
		if (buf == NULL) return -1;
		*buf=0; buflen=0;

		APPEND("{\"name\":\"stat.");
		measurement = meter->influxMeasurement;
		if (! measurement) measurement = influxMeasurement;
		if (measurement) {
			APPEND(measurement); APPEND(".");
		}
		APPEND(meter->name);
		APPEND("\"");

		if (!meter->isFormulaOnly) {
			APPEND(", \"last\":");
			timeSecs = meter->queryTimeNano / NANO_PER_SEC;
			snprintf(valBuf,VALBUFLEN,"%06.4f",timeSecs);
			APPEND(valBuf);

			APPEND(", \"avg\":");
			timeSecs = meter->queryTimeNanoAvg / NANO_PER_SEC;
			snprintf(valBuf,VALBUFLEN,"%06.4f",timeSecs);
			APPEND(valBuf);

			APPEND(", \"min\":");
			timeSecs = meter->queryTimeNanoMin / NANO_PER_SEC;
			snprintf(valBuf,VALBUFLEN,"%06.4f",timeSecs);
			APPEND(valBuf);

			APPEND(", \"max\":");
			timeSecs = meter->queryTimeNanoMax / NANO_PER_SEC;
			snprintf(valBuf,VALBUFLEN,"%06.4f",timeSecs);
			APPEND(valBuf);

			APPEND(", \"initial\":");
			timeSecs = meter->queryTimeNanoInitial / NANO_PER_SEC;
			snprintf(valBuf,VALBUFLEN,"%06.4f",timeSecs);
			APPEND(valBuf);

			APPEND(", \"numQueries\":");
			snprintf(valBuf,VALBUFLEN,"%d",meter->numQueries);
			APPEND(valBuf);

		}

		APPEND(", \"numInfluxWrites\":");
		snprintf(valBuf,VALBUFLEN,"%d",meter->numInfluxWrites);
		APPEND(valBuf);

		APPEND(", \"influxWriteCountdown\":");
		snprintf(valBuf,VALBUFLEN,"%d",meter->influxWriteCountdown);
		APPEND(valBuf);

		APPEND(", \"numErrs\":");
		snprintf(valBuf,VALBUFLEN,"%d",meter->numErrs);
		APPEND(valBuf);

		APPEND("}");

		if (dryrun) {
			printf("%s%s %s\n",meter->mqttprefix,meter->name,buf);
			free(buf);
		} else {
			mClient->topicPrefix = mqttstatprefix;
			mqtt_pub_strF (mClient,meter->name, 0, 0, 1, buf);
			free(buf);
			mClient->topicPrefix = NULL;
		}
	}

	if (meter->mqttDelayMs) msleep(meter->mqttDelayMs);

	return rc;
}
#endif


int influxAppendData (influx_client_t* c, meter_t *meter, uint64_t timestamp) {
	meterRegisterRead_t *rr;
	meterFormula_t *mf;
	int regCount = 0;
	int rc;

	// use the global measurement or the one from the meter (if defined)
	char * measurement = influxMeasurement;
	if (meter->influxMeasurement) measurement = meter->influxMeasurement;

	char * tagname = influxTagName;
	if (meter->influxTagName) tagname = meter->influxTagName;

	// check if we have something to write
	if (meter->disabled) {
		VPRINTFN(2,"%s; influx: meter is disabled");
		return 0;
	}
	if (! meter->numEnabledRegisters_influx) {
		VPRINTFN(2,"%s; influx: no enabled registers",meter->name);
		return 0;
	}
	//VPRINTFN(2,"%s: InfluxAppendData\n",meter->name);

	rc = influxdb_format_line(c, INFLUX_MEAS(measurement), INFLUX_TAG(tagname, meter->iname ? meter->iname : meter->name),INFLUX_END);
	if (rc < 0) { EPRINTFN("influxdb_format_line failed, INFLUX_MEAS"); exit(1); }

    rr = meter->registerRead;
	while (rr) {
        if (rr->registerDef->enableInfluxWrite) {
            if (rr->isInt || rr->registerDef->forceType == force_int) {
                rc = influxdb_format_line(c, INFLUX_F_INT(rr->registerDef->name, (int)rr->fvalueInflux), INFLUX_END);
                if (rc< 0) { EPRINTFN("influxdb_format_line failed, INFLUX_F_INT"); exit(1); }
            } else {
                rc = influxdb_format_line(c, INFLUX_F_FLT(rr->registerDef->name, rr->fvalueInflux, rr->registerDef->decimals), INFLUX_END);
                if (rc < 0) { EPRINTFN("influxdb_format_line failed, INFLUX_F_FLT"); exit(1); }
            }
        }
        regCount++;
		rr = rr->next;
	}

	// meter specific register formulas
	mf = meter->meterFormula;
	while (mf) {
		if (mf->enableInfluxWrite) {
			if (mf->forceType == force_int) {
				rc = influxdb_format_line(c, INFLUX_F_INT(mf->name, (int)mf->fvalueInflux) ,INFLUX_END);
				if (rc < 0) { EPRINTFN("influxdb_format_line failed, INFLUX_F_INT"); exit(1); }
				regCount++;
			} else {
				rc = influxdb_format_line(c, INFLUX_F_FLT(mf->name, mf->fvalueInflux, mf->decimals), INFLUX_END);
				if (rc < 0) { EPRINTFN("influxdb_format_line failed, INFLUX_F_FLT"); exit(1); }
				regCount++;
			}
		}
		mf = mf->next;
	}
	//printf("InfluxLine: '%s'\n",c->influxBuf);
	rc = influxdb_format_line(c, INFLUX_TS(timestamp), INFLUX_END);
	if (rc < 0) { EPRINTFN("influxdb_format_line failed, INFLUX_TS"); exit(1); }

	if (regCount) meter->numInfluxWrites++;
	return regCount;
}


#define CHANNEL_MAX_LEN 140
int grafanaAppendData (influx_client_t* c, meter_t *meter, uint64_t timestamp) {
	meterRegisterRead_t *rr;
	meterFormula_t *mf;
	int regCount = 0;
	int rc;
	char channelName[255];

	// check if we have something to write
	if (meter->disabled) {
		VPRINTFN(2,"%s; grafana: meter is disabled");
		return 0;
	}
	if (! meter->numEnabledRegisters_grafana) {
		VPRINTFN(2,"%s; grafana: no enabled registers",meter->name);
		return 0;
	}
	VPRINTFN(2,"%s: grafanaAppendData",meter->name);

	memset(channelName,0,sizeof(channelName));
	if (gUseInfluxMeasurement) {
		if (meter->influxMeasurement) strncpy(channelName,meter->influxMeasurement,CHANNEL_MAX_LEN);
		else strncpy(channelName,influxMeasurement,CHANNEL_MAX_LEN);
		if (strlen(channelName)) strncat(channelName,"/",CHANNEL_MAX_LEN);
	}
	strncat(channelName,meter->gname?meter->gname:meter->name,CHANNEL_MAX_LEN);

	rc = influxdb_format_line(c, INFLUX_MEAS(channelName), INFLUX_END);
	if (rc < 0) { EPRINTFN("influxdb_format_line failed, INFLUX_MEAS (grafana)"); exit(1); }

    rr = meter->registerRead;
	while (rr) {
        if (rr->registerDef->enableGrafanaWrite) {
            if (rr->isInt || rr->registerDef->forceType == force_int) {
                rc = influxdb_format_line(c, INFLUX_F_INT(rr->registerDef->name, (int)rr->fvalue), INFLUX_END);
                if (rc< 0) { EPRINTFN("influxdb_format_line failed, INFLUX_F_INT"); exit(1); }
            } else {
                rc = influxdb_format_line(c, INFLUX_F_FLT(rr->registerDef->name, rr->fvalue, rr->registerDef->decimals), INFLUX_END);
                if (rc < 0) { EPRINTFN("influxdb_format_line failed, INFLUX_F_FLT"); exit(1); }
            }
        } //else printf("%s %s: disabled for Grafana\n",meter->name,rr->registerDef->name);
        regCount++;
		rr = rr->next;
	}

	// meter specific register formulas
	mf = meter->meterFormula;
	while (mf) {
		if (mf->enableGrafanaWrite) {
			if (mf->forceType == force_int) {
				rc = influxdb_format_line(c, INFLUX_F_INT(mf->name, (int)mf->fvalueInflux) ,INFLUX_END);
				if (rc < 0) { EPRINTFN("influxdb_format_line failed, INFLUX_F_INT"); exit(1); }
				regCount++;
			} else {
				rc = influxdb_format_line(c, INFLUX_F_FLT(mf->name, mf->fvalueInflux, mf->decimals), INFLUX_END);
				if (rc < 0) { EPRINTFN("influxdb_format_line failed, INFLUX_F_FLT"); exit(1); }
				regCount++;
			}
		} //else printf("%s, meter formula field %s disabled for Grafana\n",meter->name,mf->name);
		mf = mf->next;
	}
	//printf("GrafanaLine: '%s'\n",c->influxBuf);
	rc = influxdb_format_line(c, INFLUX_TS(timestamp), INFLUX_END);

	if (rc < 0) { EPRINTFN("influxdb_format_line failed, INFLUX_TS (grafana)"); exit(1); }

	if (regCount) meter->numGrafanaWrites++;
	return regCount;
}

int test;

int grafanaAppendStat (influx_client_t* c, uint64_t timestamp, double totalQueryTime) {
	char channelName[255];
	char fieldName[255];
	int rc;
	meter_t * meter = meters;
	int meterCount = 0;
	int len;
	double timeSecs;
	int statCount = 0;

	// check if we have something to write
	//TODO

	test++;

	memset(channelName,0,sizeof(channelName));
	if (gpushStatId) {
		strncpy(channelName,gpushStatId,CHANNEL_MAX_LEN);
		if (strlen(channelName)) strncat(channelName,"/",CHANNEL_MAX_LEN);
	} else {
		if (gUseInfluxMeasurement) {
			strncpy(channelName,influxMeasurement,CHANNEL_MAX_LEN);
			if (strlen(channelName)) strncat(channelName,"/",CHANNEL_MAX_LEN);
			strncat(channelName,"_STATISTICS",CHANNEL_MAX_LEN);
		} else
			strncat(channelName,"_STATISTICS",CHANNEL_MAX_LEN);
	}

	while (meter) {
		if (!meter->disabled && !meter->isFormulaOnly) {
			if (meter->isTCP || meter->isSerial) {
				if (statCount == 0)
					influxdb_format_line(c, INFLUX_MEAS(channelName), INFLUX_END);
				statCount++;
				len = snprintf(fieldName,sizeof(fieldName),"%s.%s",meter->name,"last");
				if (len >= (int)(sizeof(fieldName)-1)) {
					EPRINTF("Buffer overflow in grafanaAppendStat, meter: %s",meter->name);
					exit(1);
				}
				timeSecs = meter->queryTimeNano / NANO_PER_SEC;
				rc = influxdb_format_line(c, INFLUX_F_FLT(fieldName, timeSecs, 3), INFLUX_END);
				if (rc < 0) { EPRINTFN("influxdb_format_line failed, rc:%d, INFLUX_F_FLT",rc); exit(1); }

				len = snprintf(fieldName,sizeof(fieldName),"%s.%s",meter->name,"min");
				if (len >= (int)(sizeof(fieldName)-1)) {
					EPRINTF("Buffer overflow in grafanaAppendStat, meter: %s",meter->name);
					exit(1);
				}
				timeSecs = meter->queryTimeNanoMin / NANO_PER_SEC;
				rc = influxdb_format_line(c, INFLUX_F_FLT(fieldName, timeSecs, 3), INFLUX_END);
				if (rc < 0) { EPRINTFN("influxdb_format_line failed, rc:%d, INFLUX_F_FLT",rc); exit(1); }

				len = snprintf(fieldName,sizeof(fieldName),"%s.%s",meter->name,"max");
				if (len >= (int)(sizeof(fieldName)-1)) {
					EPRINTF("Buffer overflow in grafanaAppendStat, meter: %s",meter->name);
					exit(1);
				}
				timeSecs = meter->queryTimeNanoMax / NANO_PER_SEC;
				rc = influxdb_format_line(c, INFLUX_F_FLT(fieldName, timeSecs, 3), INFLUX_END);
				if (rc < 0) { EPRINTFN("influxdb_format_line failed, rc:%d, INFLUX_F_FLT",rc); exit(1); }

				len = snprintf(fieldName,sizeof(fieldName),"%s.%s",meter->name,"avg");
				if (len >= (int)(sizeof(fieldName)-1)) {
					EPRINTF("Buffer overflow in grafanaAppendStat, meter: %s",meter->name);
					exit(1);
				}
				timeSecs = meter->queryTimeNanoAvg / NANO_PER_SEC;
				rc = influxdb_format_line(c, INFLUX_F_FLT(fieldName, timeSecs, 3), INFLUX_END);
				if (rc < 0) { EPRINTFN("influxdb_format_line failed, rc:%d, INFLUX_F_FLT",rc); exit(1); }

				len = snprintf(fieldName,sizeof(fieldName),"%s.%s",meter->name,"initial");
				if (len >= (int)(sizeof(fieldName)-1)) {
					EPRINTF("Buffer overflow in grafanaAppendStat, meter: %s",meter->name);
					exit(1);
				}
				timeSecs = meter->queryTimeNanoInitial / NANO_PER_SEC;
				rc = influxdb_format_line(c, INFLUX_F_FLT(fieldName, timeSecs, 3), INFLUX_END);
				if (rc < 0) { EPRINTFN("influxdb_format_line failed, rc:%d, INFLUX_F_FLT",rc); exit(1); }
			}
		}
		meter = meter->next;
	}

	if (statCount > 0) {
		// write total
		rc = influxdb_format_line(c, INFLUX_F_FLT("__TOTAL.last", totalQueryTime, 3), INFLUX_END);
		if (rc < 0) { EPRINTFN("influxdb_format_line failed, rc:%d, INFLUX_F_FLT",rc); exit(1); }

		rc = influxdb_format_line(c, INFLUX_TS(timestamp), INFLUX_END);
		if (rc < 0) { EPRINTFN("influxdb_format_line failed, rc:%d , INFLUX_TS (grafaStat)",rc); exit(1); }
		LOGN(2,"%d stat lines posted to Grafana",statCount);
	}

	return meterCount;
}


#ifndef DISABLE_MQTT
void traceCallback(enum MQTTCLIENT_TRACE_LEVELS level, char *message) {
	printf(message); printf("\n");
}


void mqttSendMeterData(double queryTime) {
	int numMeters;
	meter_t * meter;
	char statBuf[255];
	int rc;

	if (mClient) {		// mqtt
		if (dryrun) printf("\nDryrun: would send to mqtt:\n");
		numMeters = 0;
		meter = meters;
		while(meter) {
			if (meter->meterHasBeenRead || (meter->isFormulaOnly)) {	// always send for formula only meters
				rc = mqttSendData (meter,dryrun);
				if (rc == 0) numMeters++;
			}
			meter = meter->next;
		}
		if (numMeters) VPRINTFN(1,"%d lines (w/o stat) posted to mqtt",numMeters);
		if (mqttstatprefix) {
			snprintf(statBuf,sizeof(statBuf),"{\"name\": \"stat.total\", \"last\": %06.4f}",queryTime);
			mClient->topicPrefix = mqttstatprefix;
			if (dryrun || (verbose > 1)) printf("%s %s\n",mqttstatprefix, statBuf);
			if (!dryrun) mqtt_pub_strF (mClient,(char *)"total", 0, 0, 1, statBuf);
			mClient->topicPrefix = NULL;
		}
	}
}
#endif


// for answering websocket pings
//int pcount;
void periodicProc() {
	//pcount++;
	//printf("periodicProc %d\n",pcount);
	if (gClient)
		if (gClient->influxBufLen == 0 && gClient->url) influxdb_post_http(gClient);
	if (iClient)
		if (iClient->influxBufLen == 0 && iClient->url) influxdb_post_http(iClient);
}


int main(int argc, char *argv[]) {
	int rc,i;
	meter_t *meter;
	uint64_t influxTimestamp;
	struct timespec timeStart, timeEnd;
	int isFirstQuery = 1;  // takes longer due to init and/or getting sunspec id's
	int isFirstRealQuery = 0;
	double queryTime;
	int numMeters;

	//printf("byte_order: %d\n",__BYTE_ORDER);

	if (LIBMODBUS_VERSION_MAJOR != libmodbus_version_major || LIBMODBUS_VERSION_MINOR != libmodbus_version_minor /*|| LIBMODBUS_VERSION_MICRO != libmodbus_version_micro */) {
		EPRINTFN("%s: compiled with libmodbus %d.%d.%d but the version loaded is %d.%d.%d, recompile required",argv[0],LIBMODBUS_VERSION_MAJOR,LIBMODBUS_VERSION_MINOR,LIBMODBUS_VERSION_MICRO,libmodbus_version_major,libmodbus_version_minor,libmodbus_version_micro);
		exit(1);
	}

#ifndef DISABLE_MQTT
	mqttprefix = strdup(MQTT_PREFIX_DEF);

	mClient = mqtt_pub_init (NULL, 0, MQTT_CLIENT_ID, NULL);
#endif

	if (parseArgs(argc,argv) != 0) exit(1);

// for valgrind leak check
#if 0
	printf("read definitions\n");
	readMeterDefinitions (CONFFILE);
	printf("free meters\n");
	freeMeters();
#ifndef DISABLE_MQTT
	free(mqttprefix);
	if (mClient) mqtt_pub_free(mClient);
#endif
	influxdb_post_free(iClient);
	exit(1);
#endif

	if (!cronExpression) {	// if we have poll seconds specified, build a cron expression
		cronExpression = (char *)malloc(30);
		snprintf(cronExpression,30,"*/%d * * * * *",queryIntervalSecs);
	}
	// add default schedule
	cron_add(NULL,cronExpression);

	// open serial port, needed by readMeterDefinitions if we have Modbus RTU connections
	if (serDevice) {	// not needed if we only have TCP connections
		if (meterSerialScanDevice (serDevice)) exit(3);
		if (meterSerialScanBaudrate (serBaudrate)) exit(3);
		if (meterSerialScanParity (serParity)) exit(3);
		if (meterSerialScanrs485 (ser_rs485)) exit(3);
		if (meterSerialScanStopbits (serStopbits)) exit (3);
		if (meterSerialOpenAll ()) exit(3);
		//if (modbusRTU_open (serDevice,serBaudrate,serParity,serStopbits,ser_rs485) != 0) {
		//	EPRINTF("%s: unable to open serial port at %s\n",argv[0],serDevice);
		//	exit(3);
	}

	if (scan) {
		if (! scanHost)
			if (! serDevice) {
				EPRINTFN("scan: no serial port specified");
				exit(1);
			}
		scanAddresses();
		exit(0);
	}

	if (scanRTU) {
		if (! serDevice) {
			EPRINTFN("scanrtu: no serial port specified");
			exit(1);
		}
		modbus_t ** mb = modbusRTU_getmh(0,"scan for RTU devices");
		uint16_t regValue;
		printf("scanning for modbus RTU devices\n");
		for (i=1;i<254;i++) {
			printf("\r%d ",i); fflush(stdout);
			modbus_set_slave(*mb,i);
			rc = modbus_read_registers(*mb, scanRTUReg,1,&regValue);
			if (rc == 1) rc = 0;
			if (rc == -1) rc = errno;
			VPRINTFN(1,"modbus_read_registers for slave %d returned %d (%s)",i,rc,modbus_strerror(rc));
			if (rc == 0) printf("\rfound device at id %d\n",i);
			modbusRTU_SilentDelay(modbusRTU_getBaudrate(0));  //
			//msleep(25);
		}
		exit(0);
	}

	readMeterDefinitions (configFileName);
	cron_setDefault();
	if (verbose) cron_showSchedules();


	if (dumpRegisters) {
		printf("Querying all meters\n" \
			   "===================\n");
		rc = queryMeters(0);
		meter = meters;
		while(meter) {
			if(!meter->disabled) {
				if (meter->meterHasBeenRead || meter->isFormulaOnly) {
					printf("\n%s (%s):\n",meter->name,meter->meterType ? meter->meterType->name : "formula only");
					meterRegisterRead_t *rr;
					meterFormula_t *mf;
					int bufSize = 100;
					int bufLen = 0;
					char *buf = (char *)calloc(1,bufSize);

					//       123456789012345678901234567890 Addr    IMG  Value
					printf(" Field                          Addr    IMG  Value\n");
					printf(" -------------------------------------------------------\n");

					rr = meter->registerRead;
					while (rr) {
					//void appendValue (int includeName, meterRegisterRead_t *rr, char **dest, int *len, int *bufsize)
						bufLen = 0; *buf = 0;
						appendValue(0,rr,&buf,&bufLen,&bufSize);
						printf(" %-30s 0x%04x  %d%d%d  %s\n",rr->registerDef->name,rr->registerDef->startAddr,
							rr->registerDef->enableInfluxWrite,rr->registerDef->enableMqttWrite,rr->registerDef->enableGrafanaWrite,buf);
						rr = rr->next;
					}

					// registers from meter specific formulas

					mf = meter->meterFormula;
					while (mf) {
						bufLen = 0; *buf = 0;
						appendFormulaValue(0,mf,&buf,&bufLen,&bufSize);
						printf(" %-30s         %d%d%d  %s\n",mf->name,
							mf->enableInfluxWrite,mf->enableMqttWrite,mf->enableGrafanaWrite,buf);
						mf = mf->next;

					}
					free(buf);
				}
			}
			meter = meter->next;
		}
		//setTarif (1);
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
#ifndef DISABLE_FORMULAS
		testRegCalcFormula(formulaValMeterName);
#endif
        exit(1);
	}

	if (formulaTry) {
	    printf("Querying all meters before formula test\n" \
			   "=======================================\n");
		rc = queryMeters(dumpRegisters);
		setTarif (1);
#ifndef DISABLE_FORMULAS
		testRegCalcFormula(NULL);
#endif
        exit(1);
	}

	if (sizeof(time_t) <= 4) {
		LOGN(0,"Warning: TimeT is less than 64 bit, this may fail after year 2038, recompile with newer kernel and glibc to avoid this");
	}

	if (!iClient) LOGN(0,"no influxdb host specified, influx sender disabled");

	if (ghost && gtoken && gpushid) {
		gClient = influxdb_post_init_grafana (ghost, gport, gpushid, gtoken, gVerifyPeer);
	} else
		LOGN(0,"no grafana host,token and pushid specified, grafana sender disabled");

#ifndef DISABLE_MQTT
	if (!mClient->hostname) {
		mqtt_pub_free(mClient);
		mClient = NULL;
		LOGN(0,"no mqtt host specified, mqtt sender disabled");
		if (!iClient && !gClient) {
			EPRINTFN("No MQTT, Influxdb and Grafana host specified, specify at least one");
			exit(1);
		}
	} else {
		if (!dryrun) {
			rc = mqtt_pub_connect (mClient);
			//printf("mqtt connect: %d\n",rc);
			if (rc != 0) {
				LOGN(0,"mqtt_pub_connect returned %d, will retry later",rc);
			}
			//else LOGN(0,"Connected to mqtt server %s",iClient->host);
		}
	}
#endif


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

	if (verbose > 2) {
		meter = meters;
		printf("Name                            TCP  Ser serNum schedules fo numM numI numG\n");
		printf("---------------------------------------------------------------------------\n");
		while(meter) {
			printf("%-30s %4d %4d   %4d %9d  %d %4d %4d %4d\n",meter->name,meter->isTCP,meter->isSerial,meter->serialPortNum,meter->hasSchedule,meter->isFormulaOnly,meter->numEnabledRegisters_mqtt,meter->numEnabledRegisters_influx,meter->numEnabledRegisters_grafana);
			meter = meter->next;
		}
		printf("\n");
	}

	// term handler for ^c and SIGTERM send by systemd
	//signal(SIGKILL, sigterm_handler);
	signal(SIGTERM, sigterm_handler);
	signal(SIGINT, sigterm_handler);

	signal(SIGUSR1, sigusr2_handler);	// used for verbose level inc/dec via kill command
	signal(SIGUSR2, sigusr1_handler);

	PRINTFN("mainloop started (%s %s)",ME,VER);

	// do an initial query but do not write to influx / mqtt (init / init sunspec)
	clock_gettime(CLOCK_MONOTONIC,&timeStart);
	if (dryrun || (verbose > 0)) printf("performing initial query for all meters\n");
	rc = queryMeters(verbose);
	clock_gettime(CLOCK_MONOTONIC,&timeEnd);
	queryTime = (double)(timeEnd.tv_sec + timeEnd.tv_nsec / NANO_PER_SEC)-(double)(timeStart.tv_sec + timeStart.tv_nsec / NANO_PER_SEC);
#ifndef DISABLE_FORMULAS
	formulaNumPolls++;
#endif
	if (rc <= 0) {
		terminated++;
		EPRINTFN("Initial query: no meters could be queried, terminating");
	} else {
		PRINTFN("Performed initial query for %d meters in %4.2f seconds",rc,queryTime);
	}

	meter=meters;
	while(meter) {
		meter->queryTimeNanoInitial = meter->queryTimeNano;
		meter->queryTimeNanoMax = 0;
		meter=meter->next;
	}
#ifndef DISABLE_MQTT
	if (!dryrun) mqttSendMeterData(queryTime);	// initially send all data to mqtt
#endif

	workerInit(meterSerialGetNumDevices(), dryrun || (verbose>0));

	int loopCount = 0;
	while (!terminated) {
#ifndef DISABLE_MQTT
		mqtt_pub_yield (mClient); 					// for mqtt ping, sleeps for 100ms if no mqqt specified
#endif
		periodicProc();								// for websocket ping
		clock_gettime(CLOCK_MONOTONIC,&timeStart);
		if (isFirstQuery) rc = 1;
		else rc = cron_queryMeters(dryrun || verbose>0, dryrun, &periodicProc);
		if (rc > 0) {
#ifndef DISABLE_FORMULAS
			formulaNumPolls++;
#endif
			clock_gettime(CLOCK_MONOTONIC,&timeEnd);
			loopCount++;
			queryTime = (double)(timeEnd.tv_sec + timeEnd.tv_nsec / NANO_PER_SEC)-(double)(timeStart.tv_sec + timeStart.tv_nsec / NANO_PER_SEC);
			if (dryrun || (verbose>0) || isFirstRealQuery)
				PRINTFN("Query %d took %4.2f seconds",loopCount,queryTime);

			modbusRTU_checkDeviceErrors (ModbusRTU_exitErrorCount);

			if (iClient) {		// influx
				influxdb_post_freeBuffer(iClient);
				influxTimestamp = influxdb_getTimestamp();
				numMeters = 0;
				meter = meters;
				while(meter) {
					if(!meter->disabled) {
						if( (meter->meterHasBeenRead || ( meter->isFormulaOnly && (meter->hasSchedule == 0) )) && (meter->influxWriteCountdown == 0) ) {
							influxAppendData (iClient, meter, influxTimestamp);
							meter->influxWriteCountdown = meter->influxWriteMult;
							numMeters++;
						} else {
							VPRINTF(2,"Influx not written for %s, meterHasBeenRead: %d, influxWriteCountdown: %d\n",meter->name,meter->meterHasBeenRead,meter->influxWriteCountdown);
						}
					}
					meter = meter->next;
				}
				if (dryrun) {
					if (iClient->influxBufLen) {
						printf("\nDryrun: would send to influxdb:\n%s\n",iClient->influxBuf);
						influxdb_post_freeBuffer(iClient);
					}
				} else {
					if (iClient->influxBufLen) {
						rc = influxdb_post_http_line(iClient);
						if (rc != 0) {
							//EPRINTFN("Error: influxdb_post_http_line failed with rc %d",rc);
						} else {
							VPRINTFN(1,"%d lines posted to influxdb",numMeters);
						}
					} else {
						VPRINTFN(2,"nothing to send to influx");
					}
				}
			}

			periodicProc();
			if (gClient) {		// grafana
				influxdb_post_freeBuffer(gClient);
				influxTimestamp = influxdb_getTimestamp();
				numMeters = 0;
				meter = meters;
				while(meter) {
					if(!meter->disabled) {
						if (meter->meterHasBeenRead) {
							grafanaAppendData (gClient, meter, influxTimestamp);
							numMeters++;
						}
					}
					meter = meter->next;
				}
				grafanaAppendStat (gClient, influxTimestamp, queryTime);
				if (dryrun) {
					if (gClient->influxBufLen) {
						printf("\nDryrun: would send to grafana:\n%s\n",gClient->influxBuf);
						influxdb_post_freeBuffer(gClient);
					} else printf("nothing to be posted to Grafana\n");
				} else {
					if (gClient->influxBufLen) {
						rc = influxdb_post_http_line(gClient);
						if (rc != 0) {
							EPRINTFN("Error: influxdb_post_http_line to grafana failed with rc %d",rc);
						} else {
							VPRINTFN(1,"%d lines posted to grafana",numMeters);
						}
					} else {
						VPRINTFN(2,"nothing to send to grafana");
					}
				}
			}

#ifndef DISABLE_MQTT
			mqttSendMeterData(queryTime);
#endif
			periodicProc();
			if (dryrun) {
				dryrun--;
				if (!dryrun) terminated++;
				printf("- %d -----------------------------------------------------------------------\n",loopCount);
			} else {
				if (verbose) printf("\n");
			}
			clock_gettime(CLOCK_MONOTONIC,&timeEnd);
			queryTime = (double)(timeEnd.tv_sec + timeEnd.tv_nsec / NANO_PER_SEC)-(double)(timeStart.tv_sec + timeStart.tv_nsec / NANO_PER_SEC);
			if (dryrun || (verbose>0) || isFirstRealQuery)
				PRINTFN("Total processing time of query %d was %4.2f seconds",loopCount,queryTime);

			if (isFirstQuery) {
				isFirstQuery--;
				isFirstRealQuery++;
			} else
				isFirstRealQuery = 0;
		}

	}

	// terminate all worker threads
	workerTerminate();


#ifndef DISABLE_FORMULAS
	freeFormulaParser();
#endif // DISABLE_FORMULAS

#ifndef DISABLE_MQTT
	if (mClient) mqtt_pub_free(mClient);
#endif
	influxdb_post_free(iClient);
	influxdb_post_free(gClient);

	freeMeters();
	cronFree();
	modbusread_free();

	// cat emModbus2influx.cpp | grep AP_OPT_STRVAL | awk -F, '{gsub(/[ \t]+$/, "", $4); print "free("substr($4,2,255)");"}'

	free(serDevice);
	free(serBaudrate);
	free(serParity);
	free(serStopbits);
	free(ser_rs485);
	free(influxMeasurement);
	free(influxTagName);
#ifndef DISABLE_MQTT
	free(mqttprefix);
	free(mqttstatprefix);
#endif
	free(ghost);
	free(gtoken);
	free(gpushid);

	free(configFileName);
	free(cronExpression);

	PRINTFN("terminated");

	return 0;
}
