#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <ctype.h>
#include "global.h"
#include "math.h"
//#include <endian.h>
#include "m-data.h"

#include "log.h"
#include "modbusread.h"
#include "modbus.h"
#ifndef DISABLE_FORMULAS
#include "muParser.h"
#include <readline/readline.h>
#include <readline/history.h>
#endif

#define LIGHTMODBUS_MASTER_FULL
#define LIGHTMODBUS_DEBUG
#define LIGHTMODBUS_IMPL

#define NANO_PER_SEC 1000000000.0

#define READ_TIMEOUT_SECS 1
#define READ_RETRY_COUNT 5
//#define READ_RETRY_DELAYMS 200
#define READ_RETRY_DELAYMS 50
#define READ_RETRY_DELAY_INCMS 50

// default in libmodbus
#define MODBUS_RESPONSE_TIMEOUT_DEFAULT 500000

// delete defines to disable calling modbus_set_response_timeout
//#define MODBUS_RESPOSE_TIMEOUT_TCP MODBUS_RESPONSE_TIMEOUT_DEFAULT/5
//#define MODBUS_RESPOSE_TIMEOUT_RTU MODBUS_RESPONSE_TIMEOUT_DEFAULT/2



typedef struct meterIPConnection_t meterIPConnection_t;
struct meterIPConnection_t {
	char * hostname;
	char * port;
	modbus_t *mb;
	int isConnected;
	int retryCount;
	meterIPConnection_t *next;
};

meterIPConnection_t * meterIPconnections;

const char * defPort = "502";

void modbusTCP_close (char *device, char *port) {
	meterIPConnection_t * mc = meterIPconnections;

	if (port == NULL) port = (char *)defPort;

	while (mc) {
		if (strcmp(device,mc->hostname) == 0)
			if (strcmp(port,mc->port) == 0) {
				if (mc->isConnected) {
					PRINTFN("modbusTCP_close: disconnecting from %s:%s",device,port);
					mc->isConnected = 0;
					modbus_close(mc->mb);
				}
				return;
			}
		mc = mc->next;
	}
}

modbus_t ** modbusTCP_open (char *device, char *port) {
	int res;
	meterIPConnection_t * mc = meterIPconnections;

	if (port == NULL) port = (char *)defPort;

	while (mc) {
		if (strcmp(device,mc->hostname) == 0)
			if (strcmp(port,mc->port) == 0) {
				if (mc->isConnected) {
					VPRINTFN(2,"modbusTCP_open (%s:%s): using exiting connection",device,port);
					return &mc->mb;
				}
#if 0
				if (mc->retryCount) {
					mc->retryCount--;
					return NULL;
				}
#endif
				res = modbus_connect(mc->mb);
				if (res == 0) {
					mc->isConnected = 1;
					PRINTFN("modbusTCP_open (%s:%s): connected",device,port);
					modbus_set_error_recovery(mc->mb,(modbus_error_recovery_mode) (MODBUS_ERROR_RECOVERY_PROTOCOL));
#ifdef MODBUS_RESPOSE_TIMEOUT_TCP
					modbus_set_response_timeout(mc->mb,0,MODBUS_RESPOSE_TIMEOUT_TCP);
#endif
					return &mc->mb;
				}
				PRINTFN("modbusTCP_open (%s:%s): connect failed retrying later (%d - %s)",device,port,errno,modbus_strerror(errno));
				mc->retryCount = TCP_OPEN_RETRY_DELAY;
				return NULL;
			}
		mc = mc->next;
	}
	if (meterIPconnections == NULL) {
		meterIPconnections = (meterIPConnection_t *)calloc(1,sizeof(meterIPConnection_t));
		mc = meterIPconnections;
	} else {
		mc = meterIPconnections;
		while (mc->next != NULL) mc = mc->next;
		mc->next = (meterIPConnection_t *)calloc(1,sizeof(meterIPConnection_t));
		mc = mc->next;
	}
	mc->hostname = device;
	mc->port = port;
	VPRINTFN(2,"modbusTCP_open (%s:%s): creating new host entry",device,port);
	mc->mb = modbus_new_tcp_pi(device,port);
	if (!mc->mb) {
		EPRINTFN("modbus_new_tcp_pi (%s:%s) failed",device,port);
		exit(1);
	}
	res = modbus_connect(mc->mb);
	if (res == 0) {
		VPRINTFN(2,"modbusTCP_open (%s:%s): connected",device,port);
		mc->isConnected = 1;
		//modbus_set_error_recovery(mc->mb,(modbus_error_recovery_mode) (MODBUS_ERROR_RECOVERY_LINK | MODBUS_ERROR_RECOVERY_PROTOCOL));	// this will stop all if a tcp meter is not available
		modbus_set_error_recovery(mc->mb,(modbus_error_recovery_mode) (MODBUS_ERROR_RECOVERY_PROTOCOL));	// needed for Fronius Symo
#ifdef MODBUS_RESPOSE_TIMEOUT_TCP
		modbus_set_response_timeout(mc->mb,0,MODBUS_RESPOSE_TIMEOUT_TCP);
#endif
		return &mc->mb;
	}
	VPRINTFN(2,"modbusTCP_open (%s:%s): connect failed retrying later",device,port);
	mc->retryCount = TCP_OPEN_RETRY_DELAY;
	return NULL;
}


void modbusTCP_freeAll() {
	meterIPConnection_t * mc = meterIPconnections;
	meterIPConnection_t * mcWork;
	while (mc) {
		mcWork = mc;
		mc = mc->next;
		modbus_free(mcWork->mb);
		free(mcWork);
	}
	meterIPconnections = NULL;
}

//*****************************************************************************

/* msleep(): Sleep for the requested number of milliseconds. */
#if 0
int msleep(long msec) {
    struct timespec ts;
    int res;

    if (msec < 0)
    {
        errno = EINVAL;
        return -1;
    }

    ts.tv_sec = msec / 1000;
    ts.tv_nsec = (msec % 1000) * 1000000;

    do {
        res = nanosleep(&ts, &ts);
    } while (res && errno == EINTR);

    return res;
}
#else

int msleep(long msec) {
	usleep(msec * 1000);
	return 0;
}

#endif

//*****************************************************************************


typedef struct meterSerialConnection_t meterSerialConnection_t;
struct meterSerialConnection_t {
	char * device;
	int baudrate;
	char parity;		// E, O or N
	modbus_t *mb;
	int rs485;
	int isConnected;
	int stopBits;
	meterSerialConnection_t *next;
};

meterSerialConnection_t * meterSerialConnections;
int numSerialConnections;


meterSerialConnection_t * newMeterSerialConnection() {
		meterSerialConnection_t * sc;
		sc = (meterSerialConnection_t *)calloc(1,sizeof(meterSerialConnection_t));
		numSerialConnections++;
		if (! meterSerialConnections) {
			sc->baudrate = 9600;
			sc->stopBits = 1;
			sc->parity = 'E';
			meterSerialConnections = sc;
			return sc;
		}
		meterSerialConnection_t * sc_last = meterSerialConnections;
		while (sc_last->next) sc_last = sc_last->next;
		sc_last->next = sc;
		// use the values for the first port as default
		sc->baudrate = meterSerialConnections->baudrate;
		sc->parity = meterSerialConnections->parity;
		sc->stopBits = meterSerialConnections->stopBits;
		sc->rs485 = meterSerialConnections->rs485;
		return sc;
}


void meterSerialCloseAll () {
	if (! meterSerialConnections) return;

	meterSerialConnection_t * sc = meterSerialConnections;
	while (sc) {
		modbus_free(sc->mb);
		sc->mb = NULL;
		sc->isConnected = 0;
		sc=sc->next;
	}
}

int meterSerialOpenAll () {
	int res;
	int serialPortNum;

	if (! meterSerialConnections) return 0;

	meterSerialConnection_t * sc;
	if (verbose > 1) {
		sc = meterSerialConnections;
		serialPortNum = 0;
		EPRINTFN("defined serial ports:");
		while (sc) {
			printf(" %2d: %6d 8%c%d rs485:%d - %s\n",serialPortNum,sc->baudrate,sc->parity,sc->stopBits,sc->rs485,sc->device);
			serialPortNum++;
			sc = sc->next;
		}
		printf("\n");
	}

	serialPortNum = 0;
	sc = meterSerialConnections;
	while (sc) {

		int noDevice = 0;
		if (!sc->device) noDevice++; else if (*sc->device == 0) noDevice++;
		if (noDevice) {
			EPRINTFN("Warning Serial%d: no device specified",serialPortNum);
		} else {
			sc->mb = modbus_new_rtu(sc->device, sc->baudrate, sc->parity, 8, sc->stopBits);
			if (sc->mb == NULL) return 0;
			res = modbus_connect(sc->mb);
			if (res == -1) {
				EPRINTFN("modbus_connect (%s) failed: %d - %s", sc->device, errno,modbus_strerror(errno));
				modbus_free(sc->mb);
				sc->mb = NULL;
				return 1;
			}

			//modbus_set_error_recovery(sc->mb, MODBUS_ERROR_RECOVERY_PROTOCOL);
			if (sc->rs485) {
				res = modbus_rtu_set_serial_mode(sc->mb, MODBUS_RTU_RS485);
				if (res != 0) EPRINTFN("Serial%d (%s) Warning: modbus_rtu_set_serial_mode (MODBUS_RTU_RS485) failed with %d (%s)", serialPortNum,sc->device, res, modbus_strerror(errno));
			} else {
				res = modbus_rtu_set_serial_mode(sc->mb, MODBUS_RTU_RS232);
				if (res != 0) EPRINTFN("Serial%d (%s) Warning: modbus_rtu_set_serial_mode (MODBUS_RTU_RS232) failed with %d (%s)", serialPortNum,sc->device, res, modbus_strerror(errno));
			}
#ifdef MODBUS_RESPOSE_TIMEOUT_RTU
			int sec=1;
			res = modbus_set_response_timeout(sc->mb,sec,MODBUS_RESPOSE_TIMEOUT_RTU);
			if (res != 0) {
				EPRINTF("failed to set modbus response timeout to %d:%d for %s",1,MODBUS_RESPOSE_TIMEOUT_RTU,sc->device);
			}
			uint32_t s,u;
			modbus_get_response_timeout(sc->mb,&s,&u);
			EPRINTFN("modbus_get_response_timeout: %d:%d for %s",s,u,sc->device);
			//modbus_set_byte_timeout(sc->mb,0,MODBUS_RESPOSE_TIMEOUT_RTU / 10);
			printf("modbus_set_response_timeout for %s set to %d:%d\n",sc->device,sec,MODBUS_RESPOSE_TIMEOUT_RTU);
#endif

			sc->isConnected = 1;
		}
		sc = sc->next;
		serialPortNum++;
	}
	return 0;
}


void modbusRTU_freeAll()  {
	meterSerialCloseAll();
	meterSerialConnection_t * mc = meterSerialConnections;
	meterSerialConnection_t * mcWork;
	while (mc) {
		mcWork = mc;
		free(mc->device);
		mc = mc->next;
		modbus_free(mcWork->mb);
		free(mcWork);
	}
	meterSerialConnections = NULL;
}


int meterSerialScanDevice (char * deviceString) {
	char * device;
	meterSerialConnection_t *sc = meterSerialConnections;

	//printf("Dev (%s)\n",deviceString);
	if (! deviceString) return 0;
	device = strtok (deviceString, ",");
	while (device) {
		//printf("Device: \"%s\"\n",device);
		if (! sc) sc = newMeterSerialConnection();
		if (*device != 0) sc->device = strdup(device);
		device = strtok (NULL, ",");
		if (device) {
			if (sc->next) sc = sc->next; else sc = newMeterSerialConnection();
		}
	}
	return 0;
}


int meterSerialScanBaudrate (char * baudString) {
	char * s;
	meterSerialConnection_t *sc = meterSerialConnections;

	if (! baudString) return 0;
	s = strtok (baudString, ",");
	while (s) {
		if (! sc) sc = newMeterSerialConnection();
		if (*s != 0) {
			sc->baudrate = strtol (s,NULL,10);
			if (errno) {
				EPRINTFN("'%s' is not a valid baudrate",s);
				return 1;
			}
		}
		s = strtok (NULL, ",");
		if (s) {
			if (sc->next) sc = sc->next; else sc = newMeterSerialConnection();
		}
	}
	return 0;
}



int meterSerialScanParity (char * parityString) {
	char * s;
	meterSerialConnection_t *sc = meterSerialConnections;

	if (! parityString) return 0;
	s = strtok (parityString, ",");
	while (s) {
		if (! sc) sc = newMeterSerialConnection();
		if (*s != 0) {
			sc->parity = toupper(*s);
			if (sc->parity != 'O' && sc->parity != 'E' && sc->parity != 'N') {
				EPRINTFN("Invalid parity (%c) specified, valid is O,N or E",sc->parity);
				return 1;
			}
		}
		s = strtok (NULL, ",");
		if (s) {
			if (sc->next) sc = sc->next; else sc = newMeterSerialConnection();
		}
	}
	return 0;
}


int meterSerialScanrs485 (char * rs485String) {
	char * s;
	char c;
	meterSerialConnection_t *sc = meterSerialConnections;

	if (! rs485String) return 0;
	s = strtok (rs485String, ",");
	while (s) {
		if (! sc) sc = newMeterSerialConnection();
		if (*s != 0) {
			c = toupper(*s);
			if (c != '0' && c != '1') {
				EPRINTFN("Invalid rs485 value (%s) specified, valid is 0 or 1",s);
				return 1;
			}
			if (c == '1') sc->rs485=1;
		}
		s = strtok (NULL, ",");
		if (s) {
			if (sc->next) sc = sc->next; else sc = newMeterSerialConnection();
		}
	}
	return 0;
}


int meterSerialScanStopbits (char * stopString) {
	char * s;
	char c;
	meterSerialConnection_t *sc = meterSerialConnections;

	if (! stopString) return 0;
	s = strtok (stopString, ",");
	while (s) {
		if (! sc) sc = newMeterSerialConnection();
		if (*s != 0) {
			c = toupper(*s);
			if (c != '1' && c != '2') {
				EPRINTFN("Invalid stopbit value (%s) specified, valid is 1 or 2",s);
				return 1;
			}
			if (c == '1') sc->stopBits=1; else sc->stopBits=2;
		}
		s = strtok (NULL, ",");
		if (s) {
			if (sc->next) sc = sc->next; else sc = newMeterSerialConnection();
		}
	}
	return 0;
}


int meterSerialGetNumDevices() {
	int n=0;
	meterSerialConnection_t *sc = meterSerialConnections;

	while(sc) {
		n++;
		sc = sc->next;
	}
	return n;
}



modbus_t ** modbusRTU_getmh(int serialPortNum) {
	meterSerialConnection_t *sc = meterSerialConnections;
	if (! sc) {
		EPRINTFN("modbusRTU_getmh(): no serial ports defined");
		exit(3);
	}
	int n = 0;
	while (sc) {
		//printf("%d: %s\n",n,sc->device);
		if (serialPortNum == n) {
			if (! meterSerialConnections->isConnected) {
				EPRINTFN("modbusRTU_getmh(): Serial%d not defined",serialPortNum);
				exit(3);
			}
			return &sc->mb;
		}
		n++;
		sc = sc->next;
	}
	EPRINTFN("modbusRTU_getmh(): Serial%d not specified",serialPortNum);
	exit(3);
}


int modbusRTU_getBaudrate(int serialPortNum) {
	meterSerialConnection_t *sc = meterSerialConnections;
	if (! sc) {
		EPRINTFN("modbusRTU_getBaudrate(%d): no serial ports defined",serialPortNum);
		exit(3);
	}
	int n = 0;
	while (sc) {
		if (serialPortNum == n) {
			if (! meterSerialConnections->isConnected) {
				EPRINTFN("modbusRTU_getBaudrate(): Serial%d not defined",serialPortNum);
				exit(3);
			}
			return sc->baudrate;
		}
		n++;
		sc = sc->next;
	}
	EPRINTFN("modbusRTU_getBaudrate(): Serial%d not specified",serialPortNum);
	exit(3);
}



// delay between queries, the Modbus RTU standard describes a silent period corresponding to 3.5 characters between each message
// with my meters @9600 baud a delay of > 21ms is required so use a larger delay here
void modbusRTU_SilentDelay(int baudrate) {
    int delay = 16;
    switch (baudrate) {
        //case 2400:
        //    delay = 16;
        //    break;
        case 4800:
            delay = 50;
            break;
        case 9600:
            delay = 30;
            break;
        case 19200:
            delay = 15;
            break;
        case 38400:
            delay = 10;
            break;
    }
    //printf("modbusRTU_SilentDelay %d ms\n",delay);
    msleep(delay);
}

void dumpBuffer (const uint16_t *data,int numValues) {
	while (numValues) {
		printf("%04x ",*data);
		data++; numValues--;
	}
	printf("\n");
}

// apply divider/multiplier, if we have an int value, convert it to float
void applyDevider (meterRegisterRead_t *rr) {

    if (rr->sunspecSf != 0) {
        if (rr->sunspecSf < 0) {    // multiplier < 1 so we need float
            if (rr->isInt) {
                rr->isInt = 0;
                //rr->fvalue = rr->ivalue;
            }
            rr->fvalue *= pow(10,rr->sunspecSf);
        } else {
             rr->fvalue *= pow(10,rr->sunspecSf);
        }
    }

	if (rr->registerDef->divider) {
		if (rr->isInt) rr->isInt = 0;
        rr->fvalue = rr->fvalue / rr->registerDef->divider;
	}

	if (rr->registerDef->multiplier) {
		rr->fvalue = rr->fvalue * rr->registerDef->multiplier;
	}

	switch (rr->registerDef->forceType) {
		case force_none:
			break;
		case force_int:
			rr->isInt = 1;
			break;
		case force_float:
			rr->isInt = 0;
			break;
	}
	//printf("%s: isInt: %d\n",rr->registerDef->name,rr->isInt);
}


int getRegisterValue (meterRegisterRead_t *rr, uint16_t *buf, int sunspecOffset, int regStart, int regEnd) {
	int bufStartAddr;
	//printf("getRegisterValue: regStart: %d, regEnd: %d, %d, sunspecOffset: %d\n",regStart,regEnd, rr->startAddr, sunspecOffset);

	if (regStart > rr->startAddr+sunspecOffset) return -1;
	if (regEnd < rr->startAddr+sunspecOffset+rr->registerDef->numRegisters-1) return -1;

	bufStartAddr = rr->startAddr + sunspecOffset - regStart;

	VPRINTFN(5,"getRegisterValue %d (0x%04x) numRegs: %d, regStart: %d (0x%04x) , regEnd: %d (0x%04x), type: %d",rr->registerDef->startAddr,rr->registerDef->startAddr,rr->registerDef->startAddr,rr->registerDef->numRegisters,regStart,regEnd,regEnd,rr->registerDef->type);

	switch (rr->registerDef->type) {
		case TK_FLOAT:
			rr->fvalue = mb_get_float_abcd(&buf[bufStartAddr]);
			rr->isInt = 0;
			//printf("%3.2f %3.2f %3.2f %3.2f\n",	mb_get_float_dcba(&buf[bufStartAddr]),mb_get_float_cdab(&buf[bufStartAddr]),mb_get_float_abcd(&buf[bufStartAddr]),mb_get_float_badc(&buf[bufStartAddr]));
			break;
		case TK_FLOAT_CDAB:
			rr->fvalue = mb_get_float_cdab(&buf[bufStartAddr]);
			rr->isInt = 0;
			break;
		case TK_FLOAT_ABCD:
			rr->fvalue = mb_get_float_abcd(&buf[bufStartAddr]);
			rr->isInt = 0;
			break;
		case TK_FLOAT_BADC:
			rr->fvalue = mb_get_float_badc(&buf[bufStartAddr]);
			rr->isInt = 0;
			break;
		case TK_INT16:
			rr->fvalue = (int16_t) buf[bufStartAddr];
			rr->isInt = (rr->registerDef->decimals == 0);
			break;
		case TK_INT32:
			rr->fvalue = ((int32_t) buf[bufStartAddr] << 16) | buf[bufStartAddr+1];
			rr->isInt = (rr->registerDef->decimals == 0);
			break;
		case TK_INT32L:
			rr->fvalue = ((int32_t) buf[bufStartAddr+1] << 16) | buf[bufStartAddr];
			rr->isInt = (rr->registerDef->decimals == 0);
			break;
		case TK_INT48:
			rr->fvalue = ((int64_t)buf[bufStartAddr] << 32) | ((int64_t)buf[bufStartAddr+1] << 16) | buf[bufStartAddr+2];
			rr->isInt = (rr->registerDef->decimals == 0);
			break;
		case TK_INT48L:
			rr->fvalue = ((int64_t)buf[bufStartAddr+2] << 32) | ((int64_t)buf[bufStartAddr+1] << 16) | buf[bufStartAddr];
			rr->isInt = (rr->registerDef->decimals == 0);
			break;
		case TK_INT64:
			rr->fvalue = ((int64_t)buf[bufStartAddr] << 48) | ((int64_t)buf[bufStartAddr+1] << 32) | ((int64_t)buf[bufStartAddr+2] << 16) | buf[bufStartAddr+3];
			rr->isInt = (rr->registerDef->decimals == 0);
			break;
		case TK_INT64L:
			rr->fvalue = ((int64_t)buf[bufStartAddr+3] << 48) | ((int64_t)buf[bufStartAddr+2] << 32) | ((int64_t)buf[bufStartAddr+1] << 16) | buf[bufStartAddr];
			rr->isInt = (rr->registerDef->decimals == 0);
			break;
		case TK_UINT16:
			rr->fvalue = (uint16_t) buf[bufStartAddr];
			rr->isInt = (rr->registerDef->decimals == 0);
			break;
		case TK_UINT32:
			rr->fvalue = ((uint32_t) buf[bufStartAddr] << 16) | buf[bufStartAddr+1];
			rr->isInt = (rr->registerDef->decimals == 0);
			break;
		case TK_UINT32L:
			rr->fvalue = ((uint32_t) buf[bufStartAddr+1] << 16) | buf[bufStartAddr];
			rr->isInt = (rr->registerDef->decimals == 0);
			break;
		case TK_UINT48:
			rr->fvalue = ((int64_t)buf[bufStartAddr] << 32) | ((int64_t)buf[bufStartAddr+1] << 16) | buf[bufStartAddr+2];
			rr->isInt = (rr->registerDef->decimals == 0);
			break;
		case TK_UINT48L:
			rr->fvalue = ((int64_t)buf[bufStartAddr+2] << 32) | ((int64_t)buf[bufStartAddr+1] << 16) | buf[bufStartAddr];
			rr->isInt = (rr->registerDef->decimals == 0);
			break;
		case TK_UINT64:
			rr->fvalue = (((int64_t)buf[bufStartAddr] | 0x7fff) << 48) | ((int64_t)buf[bufStartAddr+1] << 32) | ((int64_t)buf[bufStartAddr+2] << 16) | buf[bufStartAddr+3];
			rr->isInt = (rr->registerDef->decimals == 0);
			break;
		case TK_UINT64L:
			rr->fvalue = (((int64_t)buf[bufStartAddr+3] | 0x7fff) << 48) | ((int64_t)buf[bufStartAddr+2] << 32) | ((int64_t)buf[bufStartAddr+1] << 16) | buf[bufStartAddr];
			rr->isInt = (rr->registerDef->decimals == 0);
			break;
		default:
			EPRINTFN("fatal internal error: modbusread.c getRegisterValue, unhandled type (%d)",(int)rr->registerDef->type);
			exit(255);
	}
	applyDevider (rr);
	return 0;
}

extern int showModbusRetries;

int execMbReadFunction (meter_t *meter, regType_t regType, int startAddr, int numRegisters, uint16_t **buf) {
	errno = 0;
	int res;
	char mbFunction[30];

	if (meter->meterType->modbusQueryDelayMs) msleep(meter->meterType->modbusQueryDelayMs);

	switch (regType) {
		case regTypeHolding:
			res = modbus_read_registers(*meter->mb, startAddr, numRegisters, *buf);  // holding registers
			strcpy(mbFunction,"modbus_read_registers");
			break;
		case regTypeInput:
			res = modbus_read_input_registers(*meter->mb, startAddr, numRegisters, *buf);
			strcpy(mbFunction,"modbus_read_input_registers");
			VPRINTFN(4,"%s: modbus_read_input_registers (mb,%d,%d) returned %d",meter->name,startAddr,numRegisters,res);
			break;
		case regTypeCoil:
		case regTypeInputStatus: {
			uint8_t *bbuf = (uint8_t*) calloc(numRegisters,1);
			if (regType == regTypeCoil) {
				res = modbus_read_bits(*meter->mb, startAddr, numRegisters, bbuf);
				strcpy(mbFunction,"modbus_read_input_registers");
			} else {
				res = modbus_read_input_bits(*meter->mb, startAddr, numRegisters, bbuf);
				strcpy(mbFunction,"modbus_read_input_bits");
			}
			for (int i=0;i<numRegisters;i++) *buf[i] = bbuf[i];
			free(bbuf);
			break;
		}
		default:
			EPRINTFN("Unsupported regType %d in readRegisters",regType);
			exit(2);
			break;
	}
	VPRINTFN(4,"%s: %s (mb,%d (0x%04x),%d) returned %d",mbFunction,meter->name,startAddr,startAddr,numRegisters,res);
	if (res < 0) VPRINTFN(1,"%s (%d registers starting at %d (0x%04x)) failed with %d (%s)",mbFunction,numRegisters,startAddr,startAddr,errno,modbus_strerror(errno));
	return res;
}

int readRegisters (meter_t *meter, regType_t regType, int startAddr, int numRegisters, uint16_t **buf) {
	struct timespec timeStart, timeEnd;
	int res;
	*buf = (uint16_t *)calloc(numRegisters,sizeof(uint16_t));
	if (!*buf) return -1;

#if LIBMODBUS_VERSION_CHECK(3,1,0)
	modbus_set_response_timeout(*meter->mb, READ_TIMEOUT_SECS, 0);
#else
	struct timeval response_timeout;
	response_timeout.tv_sec = READ_TIMEOUT_SECS;
	response_timeout.tv_usec = 0;
	modbus_set_response_timeout(*meter->mb, &response_timeout);
#endif

	clock_gettime(CLOCK_MONOTONIC,&timeStart);

	res = modbus_set_slave(*meter->mb, meter->modbusAddress);
		if(res < 0) {
			EPRINTFN("modbus_set_slave failed with %d",res);
			free(*buf); *buf = NULL;
			return -1;
	}
	res = execMbReadFunction(meter, regType, startAddr, numRegisters, buf);

    if (res < 0) {
		clock_gettime(CLOCK_MONOTONIC,&timeEnd);
		if (showModbusRetries || verbose>0) {
			double queryTime = (double)(timeEnd.tv_sec + timeEnd.tv_nsec / NANO_PER_SEC)-(double)(timeStart.tv_sec + timeStart.tv_nsec / NANO_PER_SEC);
			uint32_t rtto_sec, rtto_usec, btto_sec, btto_usec;
			modbus_get_response_timeout(*meter->mb, &rtto_sec, &rtto_usec);
			modbus_get_byte_timeout(*meter->mb, &btto_sec, &btto_usec);

			EPRINTFN("modbusread.cpp readRegisters for meter \"%s\" [%s]: time for modbus_read; %4.2f, beginning retry (current modbus response timeout(sec:usec): %d:%d, byte timeout: %d:%d)",meter->name,meter->meterType->name,queryTime,rtto_sec,rtto_usec,btto_sec,btto_usec);
		}
		int retryCount = READ_RETRY_COUNT;
		int retryDelayMS = READ_RETRY_DELAYMS;
		int retryCounts = 0;
		int initialRes = errno;
		if (res == -1) initialRes = errno;
		do {
			// we sometimes get a timeout or Invalid data on Fronius Symo via TCP (both Symo and Symo Advanced), retry instead of closing connection
			modbus_flush(*meter->mb);
			msleep(retryDelayMS);
			//modbus_flush(*meter->mb);
			res = execMbReadFunction(meter, regType, startAddr, numRegisters, buf);
			retryCount--;
			retryDelayMS += READ_RETRY_DELAY_INCMS;
			retryCounts++;
		} while (res < 0 && retryCount);
		if (res < 0)
			EPRINTFN("modbusread.cpp readRegisters failed for meter \"%s\" [%s]: start: %d (0x%04x), numRegisters: %d, res: %d (%s) after %d retry%s, lastDelay: %d ms",meter->name,meter->meterType->name,startAddr,startAddr,numRegisters,res,modbus_strerror(errno),retryCounts,retryCounts>1 ? "s":"",retryDelayMS-READ_RETRY_DELAY_INCMS);
		else
			if (showModbusRetries || verbose>0)
				EPRINTFN("modbusread.cpp readRegisters for meter \"%s\" [%s]: success after %d retry%s, start: %d (0x%04x), numRegisters: %d, lastDelay: %d ms, first res: %d (%s)",meter->name,meter->meterType->name,retryCounts,retryCounts>1 ? "s":"",startAddr,startAddr,numRegisters,retryDelayMS-READ_RETRY_DELAY_INCMS,initialRes,modbus_strerror(initialRes));
	}

    if(res < 0) {
        free(*buf); *buf = NULL;
		if ((meter->isSerial) && (errno = EIO)) {
			EPRINTFN("EIO on serial meter \"%s\", assuming serial port disconnected, terminating",meter->name);
			exit(1);	// port may be unplugged, reconnect on restart via systemd or script
		}
        return -1;
    }
// untested, do we need that ? - no, not needed
#if 0
#if __BYTE_ORDER == __BIG_ENDIAN
	uint16_t *p = *buf;
	int i;
	for (i=0;i<numRegisters;i++) {
		*p = le16toh(*p);
		p++;
	}
kshksfhuih
#endif // __BYTE_ORDER
#endif
	return 0;
}


sunspecId_t * sunspecFindId (meter_t * meter, uint16_t sid) {
	int i;

	if (!meter) return NULL;
	if (!meter->sunspecIds) return NULL;
	if (!meter->sunspecIds->ids) return NULL;
	for (i=0;i<meter->sunspecIds->cnt;i++)
		if (sid == meter->sunspecIds->ids[i].id) return &meter->sunspecIds->ids[i];
	return NULL;
}

int sunspecGetOffset (meter_t * meter, int sid) {
	sunspecId_t *id;

	if (sid <= 0) return 0;

	id = sunspecFindId(meter,sid);
	if (!id) {
		EPRINTFN("%s: unable to find sunspec id %d",meter->name,sid);
		exit(1);
	}
	return id->offset;
}


int sunspecGetNumRegs (meter_t *meter, int id) {
	sunspecId_t *s;

	s = sunspecFindId(meter,id);
	if (!s) {
		EPRINTF("%s: unable to get length for sunspec id %d",meter->name,id);
		exit(1);
	}
	return s->blockLength;
}


#define INITIAL_ID_SIZE 2

int meterResolveSunspec(int verboseMsg, meter_t *meter) {
	int res,len,type,i;
	uint16_t *buf;
	int startReg = meter->meterType->sunspecBaseAddr;
	sunspecId_t *id;

	if (verboseMsg) { printf("%s: getting sunspec id's ",meter->name); fflush(stdout); }

	if (startReg < 0) {
		EPRINTFN("%s: sunspec base address missing",meter->name);
		meter->numErrs++;
		return -1;
	}

	// sunspec header
	res = readRegisters (meter, regTypeSunspec, startReg, 4, &buf);
	if (res != 0) {
		EPRINTFN("read of sunspec header (%d,4) failed - %s\n",startReg,modbus_strerror(errno));
		meter->numErrs++;
		return res;
	}
	if (buf[0] != 0x5375 || buf[1] != 0x6e53) {
		EPRINTFN("%s: invalid sunspec modbus map header id @ %d,%d, expected 0x53756e53, found %04x %04x\n",meter->name,startReg,startReg+1,buf[0],buf[1]);
		meter->numErrs++;
		return -1;
	}
	if (buf[2] != 1) {
		EPRINTFN("%s: invalid sunspec header id @ %d, expected 1, found %d\n",meter->name,startReg,buf[2]);
		meter->numErrs++;
		return -1;
	}

	len = buf[3] + 4;  // buf[3] is w/o modbus map header id(2), type(1) and length(1)
	free (buf);
	startReg += len;


	// read first header type and length after common block
	res = readRegisters (meter, regTypeSunspec, startReg, 2, &buf);
	if (res != 0) {
		EPRINTFN("read of sunspec header (%d,2) failed - %s\n",startReg,modbus_strerror(errno));
		meter->numErrs++;
		return res;
	}

	type = buf[0]; len = buf[1];

	free(buf);

	meter->sunspecIds = (sunspecIds_t *)calloc(1,sizeof(sunspecIds_t));
	meter->sunspecIds->ids = (sunspecId_t *)calloc(INITIAL_ID_SIZE,sizeof(sunspecId_t));
	meter->sunspecIds->max = INITIAL_ID_SIZE;
	//meter->sunspecIds->cnt = 0;

	while (type != 0xffff) {
		if (verboseMsg)	{ putchar('.'); fflush(stdout); }
		//printf("type: %d, len:%d\n",type,len);
		// read the block +2 registers for type and len and 2 additional ones for type and len of the next block
		//printf("%s: sunspec read (%d,%d)\n",meter->name,startReg,len+4);
		res = readRegisters (meter, regTypeSunspec, startReg, len+4, &buf);
		if (res != 0) {
			EPRINTFN("%s: read of sunspec block (%d,%d), type:%d, len: %d failed - %s\n",meter->name,startReg,len+4,type,len,modbus_strerror(errno));
			meter->numErrs++;
			return res;
		}
		if (meter->sunspecIds->cnt >= meter->sunspecIds->max) {
			meter->sunspecIds->max *= 2;
			meter->sunspecIds->ids = (sunspecId_t *)realloc(meter->sunspecIds->ids,meter->sunspecIds->max * sizeof(sunspecId_t));
		}
		i = meter->sunspecIds->cnt;
		meter->sunspecIds->ids[i].id = type;
		meter->sunspecIds->ids[i].blockLength = len+2;
		meter->sunspecIds->ids[i].offset = startReg;
		VPRINTFN(5,"\r%s: found sunspec id %d, length: %d, offset: %d",meter->name,type,len,startReg);
		meter->sunspecIds->cnt++;
		startReg = startReg + len + 2;
		type = buf[len+2]; len = buf[len+3];

		free(buf);
	}

	if (meter->sunspecIds->cnt < 1) {
		EPRINTFN("\r%s: unable to read sunspec ids");
		meter->numErrs++;
		return -1;
	}
	if (verboseMsg > 4) {
		printf("\r%s: got sunspec id's (id,length,addr): ",meter->name);
		for (i=0;i<meter->sunspecIds->cnt;i++) {
			if (i>0) printf(" ");
			printf("%d,%d,%d",meter->sunspecIds->ids[i].id,meter->sunspecIds->ids[i].blockLength,meter->sunspecIds->ids[i].offset);
		}
		printf("\n");
	}


	meterRegisterRead_t * mrrd = meter->registerRead;
	while (mrrd) {
		if (mrrd->sunspecId) {
			id = sunspecFindId(meter,mrrd->sunspecId);
			if (id) {
				VPRINTFN(4,"%s: changed address %d to %d for sunspec id %d (%d)",meter->name,mrrd->startAddr,mrrd->startAddr+id->offset,mrrd->sunspecId,id->offset);
				mrrd->startAddr += id->offset;
				if (mrrd->registerDef->sunspecSfRegister)
                    mrrd->sunspecSfRegister = mrrd->registerDef->sunspecSfRegister + id->offset;
				mrrd->sunspecId = 0;
			} else {
				EPRINTFN("%s: unable to find sunspec block with id %d",meter->name,mrrd->sunspecId);
				exit(1);
			}
		}
		mrrd = mrrd->next;
	}


	return 0;
}


#ifndef DISABLE_FORMULAS


mu::Parser *parser;         // included all registers from all meters
mu::Parser *currParser;     // used for auto complete (tab) in formula eval

using namespace mu;

static int  Round(value_type v) { return (int)(v + ((v >= 0) ? 0.5 : -0.5)); };
static value_type Rnd(value_type v) { return v * std::rand() / (value_type)(RAND_MAX + 1.0); }
static value_type bit(value_type v1, value_type v2) { return ((Round(v2) >> Round(v1)) & 1); }
static value_type Shr(value_type v1, value_type v2) { return Round(v1) >> Round(v2); }
static value_type Shl(value_type v1, value_type v2) { return Round(v1) << Round(v2); }

// from muParser example 1
static int mu_IsBinValue(const char_type* a_szExpr, int* a_iPos, value_type* a_fVal)
{
	if (a_szExpr[0] != 0 && a_szExpr[1] != 'b')
		return 0;

	unsigned iVal = 0;
	unsigned iBits = sizeof(iVal) * 8;
	unsigned i = 0;

	for (i = 0; (a_szExpr[i + 2] == '0' || a_szExpr[i + 2] == '1') && i < iBits; ++i)
		iVal |= (int)(a_szExpr[i + 2] == '1') << ((iBits - 1) - i);

	if (i == 0)
		return 0;

	if (i == iBits)
		throw mu::Parser::exception_type(_T("Binary to integer conversion error (overflow)."));

	*a_fVal = (unsigned)(iVal >> (iBits - i));
	*a_iPos += i + 2;

	return 1;
}

static int mu_IsHexValue(const char_type* a_szExpr, int* a_iPos, value_type* a_fVal)
{
	if (a_szExpr[1] == 0 || (a_szExpr[0] != '0' || a_szExpr[1] != 'x'))
		return 0;

	unsigned iVal(0);

	// New code based on streams for UNICODE compliance:
	stringstream_type::pos_type nPos(0);
	stringstream_type ss(a_szExpr + 2);
	ss >> std::hex >> iVal;
	nPos = ss.tellg();

	if (nPos == (stringstream_type::pos_type)0)
		return 1;

	*a_iPos += (int)(2 + nPos);
	*a_fVal = (value_type)iVal;

	return 1;
}

static value_type mu_Not(value_type v) { return v == 0; }
static value_type mu_BitAnd(value_type v1, value_type v2) { return Round(v1) & Round(v2); }
static value_type mu_BitOr(value_type v1, value_type v2) { return Round(v1) | Round(v2); }


double formulaNumPolls;


mu::Parser * initBaseParser() {
	mu::Parser *parser;
	parser = new (mu::Parser);
	parser-> DefineNameChars(MUPARSER_ALLOWED_CHARS);
	parser->DefineFun(_T("rnd"), Rnd, false);     // Add an unoptimizeable function
	parser->DefineFun(_T("bit"), bit, true);
	parser->DefineVar("__polls",&formulaNumPolls);
	parser->DefineOprt(_T(">>"), Shr, prMUL_DIV + 1);
	parser->DefineOprt(_T("<<"), Shl, prMUL_DIV + 1);
	parser->AddValIdent(mu_IsHexValue);
	parser->AddValIdent(mu_IsBinValue);
	parser->DefineInfixOprt(_T("!"), mu_Not, 0, true);
	parser->DefineOprt(_T("&"), mu_BitAnd, prBAND);
	parser->DefineOprt(_T("|"), mu_BitOr, prBOR);
	return parser;
}


// init the global parser and add all variables
mu::Parser * initParser() {
    meterRegisterRead_t *registerRead;
    meter_t *meter = meters;
    meterFormula_t * mf;
    char name[255];

    if (parser == NULL) {
	parser = initBaseParser();
        // add all variables using their fully qualified name (MeterName.VariableName)
        while (meter) {
            if (meter->disabled == 0) {
                registerRead = meter->registerRead;
                while (registerRead) {
                    strncpy(name,meter->name,sizeof(name)-1);
                    strncat(name,".",sizeof(name)-1);
                    strncat(name,registerRead->registerDef->name,sizeof(name)-1);

                    try {
                        parser->DefineVar(name,&registerRead->fvalue);
                    }
                    catch (mu::Parser::exception_type& e) {
                        EPRINTFN("%s: error adding variable %s (%s)",meter->name, &name,e.GetMsg().c_str());
                        exit(1);
                    }
                    registerRead = registerRead->next;
                }
				// add meter formulas so that a formula result can be used in later formulas
                mf = meter->meterFormula;
                while (mf) {
					strncpy(name,meter->name,sizeof(name)-1);
                    strncat(name,".",sizeof(name)-1);
                    strncat(name,mf->name,sizeof(name)-1);
                    try {
                        parser->DefineVar(name,&mf->fvalue);
                    }
                    catch (mu::Parser::exception_type& e) {
                        EPRINTFN("%s: error adding variable %s (%s)",meter->name, &name,e.GetMsg().c_str());
                        exit(1);
                    }
					mf = mf->next;
                }
            }
            meter = meter->next;
        }
    }
    return parser;
}


// init the local parser and add all local variables
//mu::Parser *parser
mu::Parser * initLocalParser(meter_t *meter) {
    meterRegisterRead_t *registerRead;
    mu::Parser *parser;

    parser = initBaseParser();

    // add all local meter variables using their local name
    registerRead = meter->registerRead;
    while (registerRead) {
        try {
            parser->DefineVar(registerRead->registerDef->name,&registerRead->fvalue);
        }
        catch (mu::Parser::exception_type& e) {
            EPRINTFN("error adding variable %s (%s)",&registerRead->registerDef->name,e.GetMsg().c_str());
            exit(1);
        }
        registerRead = registerRead->next;
    }
    return parser;
}


void executeMeterFormulas(meter_t * meter) {
    meterFormula_t * mf = meter->meterFormula;
    //mu::Parser *parser = NULL;  // why? should be global
    if (!mf) return;
	VPRINTF(3,"\nexecuteMeterFormulas for \"%s\"\n",meter->name);
    while(mf) {
        if (!parser) initParser();
        try {
            parser->SetExpr(mf->formula);
            mf->fvalue = parser->Eval();
			VPRINTF(3,"%s \"%s\" %10.2f\n",mf->name,mf->formula,mf->fvalue);
        }
        catch (mu::Parser::exception_type &e) {
            EPRINTFN("%s.%s error evaluating meter formula (%s)",meter->name,mf->name,e.GetMsg().c_str());
            meter->numErrs++;
            exit(1);
        }
        mf = mf->next;
    }
}


int executeMeterTypeFormulas(int verboseMsg, meter_t *meter) {
    meterRegisterRead_t *registerRead;

    mu::Parser * parser = NULL;
    registerRead = meter->registerRead;
    while (registerRead) {
        // execute formulas
        if (registerRead->registerDef->formula) {
            if (!parser) parser = initLocalParser(meter);
            try {
                parser->SetExpr(registerRead->registerDef->formula);
                registerRead->fvalue = parser->Eval();
                applyDevider(registerRead);
                VPRINTF(2,"MeterType formula for %s: \"%s\" -> %10.2f\n",meter->name,registerRead->registerDef->formula,registerRead->fvalue);
            }
            catch (mu::Parser::exception_type &e) {
                EPRINTFN("%d.%d error evaluating meter type formula (%s), setting value to 0",meter->name,registerRead->registerDef->name,e.GetMsg().c_str());
                registerRead->fvalue = 0;
            }
        }
		registerRead = registerRead->next;
	}
	if (parser) delete(parser);
    return 0;
}



void listConstants(mu::Parser *parser) {
    mu::valmap_type cmap = parser->GetConst();
    if (cmap.size())
    {
        mu::valmap_type::const_iterator item = cmap.begin();
        for (; item!=cmap.end(); ++item)
            printf("%s %3.2f\n",item->first.c_str(),item->second);
    }
}


void listFunctions(mu::Parser *parser) {
    mu::funmap_type cmap = parser->GetFunDef();
    if (cmap.size())
    {
        mu::funmap_type::const_iterator item = cmap.begin();
        for (; item!=cmap.end(); ++item)
            printf("%s ",item->first.c_str());
    }
    printf("\n");
}


void listOperators(mu::Parser *parser) {
    int i = 0;
    printf("Operators: ");
    const mu::char_type** op = parser->GetOprtDef();
    while (op[i]) {
        printf("%s ",(char *)op[i]);
        i++;
    }
    printf("\n? and : can be used like in c, for example to have an alarm for voltage:\n"\
           " VoltageL1 > 240 ? 1:0 || VoltageL2 > 240 ? 1:0 || VoltageL3 > 240 ? 1:0 ||  Inverter.Voltage > 240 ? 1 : 0\n"\
           "This will return 1 if any of the four voltages are above 240.\n\n");
}


void listVariables(mu::Parser *parser) {
    char *name;
    char *p;
    meter_t *meter;

    // Get the map with the variables
    mu::varmap_type variables = parser->GetVar();
    //cout << "Number: " << (int)variables.size() << "\n";

    // Get the number of variables
    mu::varmap_type::const_iterator item = variables.begin();

    // Query the variables
    for (; item!=variables.end(); ++item)
    {
        name = strdup(item->first.c_str());
        printf("%-40s %12.2f",name,*(double *)item->second);
        p = strchr(name,'.');
        if (p) {
            *p = '\0';
            meter = findMeter(name);
            if (meter) {
                printf("  type: '%s'",meter->meterType->name);
            }
        }
        free(name);
        printf("\n");

    }
}


char **character_name_completion(const char *, int, int);
char *character_name_generator(const char *, int);

const char *evalHelp = "enter formula to show the result, * for showing available variables\n" \
           "*c to list constants, *f to list functions, *o to list operators or *q to quit,\n"\
           "<tab> for autocomplete, <tab><tab> list or ? to show this message\n\n";

void evalFormula(const char * welcome, const char * prompt) {
    char *line;
    double result;
    //printf(prompt); printf("\n");
    initParser();
    printf(evalHelp);

    rl_attempted_completion_function = character_name_completion;

    while(1) {
        line = readline(prompt);
        if (line) {
            if(strcmp(line,"*exit") == 0 || strcmp(line,"*q") == 0) return;
            if (strcmp(line,"*") == 0) listVariables(currParser);
            else if (strcmp(line,"*c") == 0) listConstants(currParser);
            else if (strcmp(line,"*f") == 0) listFunctions(currParser);
            else if (strcmp(line,"*o") == 0) listOperators(currParser);
            else if (strcmp(line,"?") == 0) printf(evalHelp);
            else {
                try {
                    if (line) {
                        if (strlen(line) > 0) {
                            add_history (line);
                            currParser->SetExpr(line);
                            result = currParser->Eval();
                            printf(" Result: %10.4f\n",result);

                        }
                        free(line); line = NULL;
                    }
                }
                catch (mu::Parser::exception_type& e) {
                    EPRINTFN("error evaluating formula (%s)",e.GetMsg().c_str());
                    free(line); line = NULL;
                }
            }
        } else printf("\n");
    }
}


char **character_name_completion(const char *text, int start, int end)
{
    rl_attempted_completion_over = 1;
    return rl_completion_matches(text, character_name_generator);
}

char *character_name_generator(const char *text, int state)
{
    static int list_index, len, max_index;
    char *name;
    static mu::varmap_type::const_iterator item;


    if (!state) {
        list_index = 0;
        len = strlen(text);
        mu::varmap_type variables = currParser->GetVar();
        item = currParser->GetVar().begin();
        max_index = variables.size();
    }

    while (list_index < max_index) {
        list_index++;
        name = (char *)item->first.c_str();
        item++;
        //printf(" '%s' '%s' %d %d\n",name,text,len,strncmp(name, text, len));
        if (strncmp(name, text, len) == 0) {
            return strdup(name);
        }
    }

    return NULL;
}

#define PROMPT "> "
void testRegCalcFormula(char * meterName) {
    char *prompt;
    meter_t*meter;
    mu::Parser * parser;

    if (meterName) {
        meter = findMeter(meterName);
        if (! meter) {
            printf("Invalid meter name '%s'\n",meterName);
            exit(1);
        }
        if (meter->meterType)
			printf("\nFormula test for register values in meter type '%s'\n",meter->meterType->name);
		else
			printf("\nFormula test for register values in virtual meter '%s'\n",meterName);
        parser = initLocalParser(meter);
        if (meter->meterType) {
			prompt = (char *)malloc(strlen(meter->meterType->name)+3);
			strcpy(prompt,meter->meterType->name); strcat(prompt,":"); strcat(prompt," ");
        } else {
        	prompt = (char *)malloc(strlen(meterName)+3);
			strcpy(prompt,meterName); strcat(prompt,":"); strcat(prompt," ");
        }
    } else {
        printf("\nFormula test for calculating virtual registers in a meter definition\n");
        prompt = (char *)malloc(strlen(PROMPT)+1);
        strcpy(prompt,PROMPT);
        parser = initParser();
    }
    currParser = parser;    // for auto complete n readline

    evalFormula("Formula test for register values",prompt);
    free(prompt);
    delete(parser);
}


void freeFormulaParser() {
	if (parser) delete(parser);
	parser = NULL;
}


#endif

void executeInfluxWriteCalc (int verboseMsg, meter_t *meter) {
    meterRegisterRead_t *mrrd;
    meterFormula_t *mf;

    if (!meter->influxWriteMult) return;

    if (meter->influxWriteCountdown == meter->influxWriteMult) {
        meter->influxWriteCountdown--;      // first read, nothing to do
        return;
    }

    if (meter->influxWriteCountdown == -1) {
		// force write on first run after program start
		meter->influxWriteCountdown++;
		return;
    }

    if (meter->influxWriteCountdown == 0)
        meter->influxWriteCountdown = meter->influxWriteMult;

    meter->influxWriteCountdown--;

    mrrd = meter->registerRead;
    while (mrrd) {
        switch (mrrd->registerDef->influxMultProcessing) {
            case pr_last:
                break;
            case pr_max:
                //LOG(9,"max %d fvalue: %10.2f fvalueInflux: %10.2f fvalueInfluxLast: %10.2f ",meter->influxWriteCountdown,mrrd->fvalue,mrrd->fvalueInflux,mrrd->fvalueInfluxLast);
                if (mrrd->fvalueInflux < mrrd->fvalueInfluxLast) mrrd->fvalueInflux = mrrd->fvalueInfluxLast;
                //LOG(9,("-> %10.2f\n",mrrd->fvalueInflux);
                break;
            case pr_min:
                //LOG(9,"min %d fvalue: %10.2f fvalueInflux: %10.2f fvalueInfluxLast: %10.2f ",meter->influxWriteCountdown,mrrd->fvalue,mrrd->fvalueInflux,mrrd->fvalueInfluxLast);
                if (mrrd->fvalueInflux > mrrd->fvalueInfluxLast) mrrd->fvalueInflux = mrrd->fvalueInfluxLast;
                //LOG(9,"-> %10.2f\n",mrrd->fvalueInflux);
                break;
            case pr_avg:
                //LOG(9,"avg %d fvalue: %10.2f fvalueInflux: %10.2f fvalueInfluxLast: %10.2f ",meter->influxWriteCountdown,mrrd->fvalue,mrrd->fvalueInflux,mrrd->fvalueInfluxLast);
                mrrd->fvalueInflux = (mrrd->fvalueInflux + mrrd->fvalueInfluxLast) / 2.0;
                //LOG(9,"-> %10.2f\n",mrrd->fvalueInflux);
                break;
        }
        mrrd = mrrd->next;
    }
    mf = meter->meterFormula;
    while (mf) {
        switch (mf->influxMultProcessing) {
            case pr_last:
                break;
            case pr_max:
                //LOG(9,"max %d fvalue: %10.2f fvalueInflux: %10.2f fvalueInfluxLast: %10.2f ",meter->influxWriteCountdown,mf->fvalue,mf->fvalueInflux,mf->fvalueInfluxLast);
                if (mf->fvalueInflux < mf->fvalueInfluxLast) mf->fvalueInflux = mf->fvalueInfluxLast;
                //LOG(9,"-> %10.2f\n",mf->fvalueInflux);
                break;
            case pr_min:
                //LOG(9,"min %d fvalue: %10.2f fvalueInflux: %10.2f fvalueInfluxLast: %10.2f ",meter->influxWriteCountdown,mf->fvalue,mf->fvalueInflux,mf->fvalueInfluxLast);
                if (mf->fvalueInflux > mf->fvalueInfluxLast) mf->fvalueInflux = mf->fvalueInfluxLast;
                //LOG(9,"-> %10.2f\n",mf->fvalueInflux);
                break;
            case pr_avg:
                //LOG(9,"avg %d fvalue: %10.2f fvalueInflux: %10.2f fvalueInfluxLast: %10.2f ",meter->influxWriteCountdown,mf->fvalue,mf->fvalueInflux,mf->fvalueInfluxLast);
                mf->fvalueInflux = (mf->fvalueInflux + mf->fvalueInfluxLast) / 2.0;
                //LOG(9,"-> %10.2f\n",mf->fvalueInflux);
                break;
        }
        mf = mf->next;
    }
}


void setMeterFvalueInfluxLast (meter_t *meter) {
	meterRegisterRead_t *mrrd;
    meterFormula_t *mf;

    if (meter) {
        mrrd = meter->registerRead;
        while (mrrd) {
            mrrd->fvalueInfluxLast = mrrd->fvalueInflux;
            mrrd = mrrd->next;
        }
        mf = meter->meterFormula;
        while (mf) {
            mf->fvalueInfluxLast = mf->fvalueInflux;
            mf = mf->next;
        }
    }
}

void setfvalueInfluxLast () {
    meter_t *meter = meters;

    while (meter) {
		setMeterFvalueInfluxLast (meter);
        meter = meter->next;
    }
}


void setMeterFvalueInflux (meter_t * meter) {
    meterRegisterRead_t *mrrd;
    meterFormula_t *mf;

    if (meter) {
        mrrd = meter->registerRead;

        while (mrrd) {
            mrrd->fvalueInflux = mrrd->fvalue;
            mrrd = mrrd->next;
        }
        mf = meter->meterFormula;
        while (mf) {
            mf->fvalueInflux = mf->fvalue;
            mf = mf->next;
        }
    }
}


void setfvalueInflux () {
    meter_t *meter = meters;

    while (meter) {
		setMeterFvalueInflux(meter);
        meter = meter->next;
    }
}



int queryMeter(int verboseMsg, meter_t *meter) {
	meterRead_t *meterReads;
	meterRegisterRead_t *meterRegisterRead;
	int res,count;
	int regStart, regEnd;
	uint16_t *buf;
	int blockUsed;
	int numRegisters;
	int sunspecOffset;
	int reg;
	char format[50];
	struct timespec timeStart, timeEnd;

	// reset read flags
	meter->meterHasBeenRead = 0;

	if (meter->disabled) return 0;
	if (!meter->meterType) {
		meter->meterHasBeenRead = 1;
		return 0;		// virtual meter with formulas only
	}

	clock_gettime(CLOCK_MONOTONIC,&timeStart);

	if (meter->isSerial) modbusRTU_SilentDelay(meter->baudrate);

	meter->numQueries++;

	if (verboseMsg) {
        if (verboseMsg > 1) printf("\n");
		if (meter->isTCP) printf("Query \"%s\" @ TCP %s:%s, Modbus address %d\n",meter->name,meter->hostname,meter->port == NULL ? "502" : meter->port,meter->modbusAddress);
		else if (meter->isSerial) printf("Query \"%s\" @ ModbusRTU address %d on Serial%d\n",meter->name,meter->modbusAddress,meter->serialPortNum);
		else {
			if (!meter->isFormulaOnly) {
				EPRINTFN("%s: internal error: meter is neither serial, TCP or formula only",meter->name);
				exit(255);
			}
		}
	} else
		VPRINTFN(8,"%s: queryMeter Modbus address %d",meter->name,meter->modbusAddress);

	// tcp open retry
	if (meter->isTCP) {
		meter->mb = modbusTCP_open (meter->hostname,meter->port);	// get it from the pool or create/open if not already in list of connections
		if(meter->mb == NULL) {
			//WPRINTFN("%s: connect to %s:%s failed, will retry later, errno: %d (%s)",meter->name,meter->hostname,meter->port ? meter->port : defPort,errno,modbus_strerror(errno));
			meter->numErrs++;
			return -555;
		}
	} //else msleep(50);
	if (meter->mb) {
		modbus_set_debug(*meter->mb,meter->modbusDebug);
		modbus_set_slave(*meter->mb,meter->modbusAddress);
	}

	if (meter->meterType->isFormulaOnly) meter->meterHasBeenRead++;

	meterRegisterRead = meter->registerRead;
	while (meterRegisterRead) {
		meterRegisterRead->hasBeenRead = 0;
		meterRegisterRead->fvalue = 0;
		meterRegisterRead = meterRegisterRead->next;
	}


	if (meter->needSunspecResolve) {
		res = meterResolveSunspec(verboseMsg, meter);
		if (res != 0) {
			EPRINTFN("%s: sunspec resolve failed",meter->name);
			meter->numErrs++;
			//exit(1);
			return -1;
		}
		meter->needSunspecResolve = 0;
	}

	// meter init
	if (!meter->initDone) {
        meterInit_t *mi = meter->meterType->init;
        count=0;
        if (verboseMsg && mi) {
            PRINTFN("%s: initializing ",meter->name);
        }

        while (mi) {
            if (mi->numWords) {
                sunspecOffset = sunspecGetOffset (meter,mi->sunspecId);
                //printf("%04x %d %d\n",mi->startAddr,sunspecOffset,mi->numWords);
                if (mi->numWords == 1)
                    res = modbus_write_register(*meter->mb,mi->startAddr+sunspecOffset,*mi->buf);
                else
                    res = modbus_write_registers(*meter->mb,mi->startAddr+sunspecOffset,mi->numWords,mi->buf);
                if (res < 0) {
                    EPRINTFN("%s: init failed (write of %d registers starting at address %d (%d: %s)",meter->name,mi->numWords,mi->startAddr+sunspecOffset,res,modbus_strerror(errno));
                    exit(1);
                }
                count++;
            }
            mi = mi->next;
        }
        meter->initDone = 1;
    }

	// first read the defined sections
	meterReads = meter->meterType->meterReads;
	int registerReadNum = 0;
	while (meterReads) {
		registerReadNum++;
		blockUsed = 0;
		sunspecOffset = sunspecGetOffset (meter,meterReads->sunspecId);
		regStart = meterReads->startAddr + sunspecOffset;
		numRegisters = meterReads->numRegisters;
		if (numRegisters == 0) numRegisters = sunspecGetNumRegs (meter,meterReads->sunspecId);
		regEnd = regStart + numRegisters - 1;
		VPRINTFN(4,"%s: reading block of %d registers starting at %d (0x%02x)",meter->name,numRegisters,meterReads->startAddr,meterReads->startAddr);
		res = readRegisters (meter, meterReads->regType, regStart, numRegisters, &buf);
		if (res == 0) {
			if (verbose > 3) {
				PRINTF (" received block from %d (0x%02x) to %d (0x%02x): ",regStart,regStart,regEnd,regEnd);
				dumpBuffer(buf,numRegisters);
			}
			// check and get the values that are in the range
			meterRegisterRead = meter->registerRead;
			
			while (meterRegisterRead) {
				// resolve sunspec scaling factor
				if (meterRegisterRead->registerDef->sunspecSfRegister && meterRegisterRead->sunspecSf == 0) {
					reg = meterRegisterRead->sunspecSfRegister; // + sunspecOffset;
					if ((regStart <= reg) && (reg <= regEnd)) {
						meterRegisterRead->sunspecSf = (int16_t)buf[reg - regStart];
						VPRINTFN(3,"%s (%s): sunspec scaling factor register %d (%d), sf=%d",meter->name,meterRegisterRead->registerDef->name,meterRegisterRead->registerDef->sunspecSfRegister,reg,meterRegisterRead->sunspecSf);
					}
				}

				//printf("regStart: %d (0x%04x) regEnd: %d (0x%04x) %d (0x%04x) %d (0x%04x)\n",regStart,regStart,regEnd,regEnd,meterRegisterRead->startAddr,meterRegisterRead->startAddr,meterRegisterRead->startAddr + meterRegisterRead->registerDef->numRegisters-1,meterRegisterRead->startAddr + meterRegisterRead->registerDef->numRegisters-1);
				if ((regStart <= meterRegisterRead->startAddr) && (meterRegisterRead->startAddr + meterRegisterRead->registerDef->numRegisters - 1 <= regEnd) && (meterReads->regType == meterRegisterRead->registerDef->regType)) {
						meterRegisterRead->hasBeenRead++;
						if (getRegisterValue (meterRegisterRead, buf, 0, regStart, regEnd) != 0) {
							EPRINTFN("%s: getRegisterValue for %s failed",meter->name,meterRegisterRead->registerDef->name);
							free(buf);
							meter->numErrs++;
							exit(1);
						} else
							VPRINTFN(9,"%s: register %s set from read #%d",meter->name,meterRegisterRead->registerDef->name,registerReadNum);
						blockUsed++;
				}
				meterRegisterRead = meterRegisterRead->next;
			}
			if (verboseMsg)
				if (blockUsed == 0) printf("  --> Block from %d to %d is useless\n",regStart,regEnd);
			free(buf); buf = NULL;
		} else {
			EPRINTFN("%s: failed to read block of %d registers starting at register %d, res: %d, errno:%d (%s)",meter->meterType->name,numRegisters,regStart,res,errno,modbus_strerror(errno));
			if (meter->isTCP)
				modbusTCP_close (meter->hostname,meter->port);
			//if (res < -1) {
			meter->numErrs++;
			return res;  // for TCP open retry
			//}

		}
		meterReads = meterReads->next;
	}

	// now check if we have remaining registers to read
	meterRegisterRead = meter->registerRead;
	while (meterRegisterRead) {
        if (meterRegisterRead->registerDef->isFormulaOnly == 0) {
            // check if we have all sunspec scaling factors
            if (meterRegisterRead->registerDef->sunspecSfRegister && meterRegisterRead->sunspecSf == 0) {
                reg = meterRegisterRead->sunspecSfRegister;

                if (readRegisters (meter, regTypeSunspec, reg, 1, &buf) != 0) {
                    EPRINTFN("%s: readRegisters for sunspec scaling factor %s, register %d failed",meter->name,meterRegisterRead->registerDef->name,reg);
                    meter->numErrs++;
                    exit(1);
                }
                meterRegisterRead->sunspecSf = (int16_t)buf[0];
                VPRINTFN(2,"%s (%s): Warning: sunspec scaling factor register %d (%d) not included in read or readid blocks, sf=%d",meter->name,meterRegisterRead->registerDef->name,meterRegisterRead->registerDef->sunspecSfRegister,reg,meterRegisterRead->sunspecSf);
                free(buf);
            }

            if (meterRegisterRead->hasBeenRead == 0) {
                regStart = meterRegisterRead->startAddr;
                regEnd = regStart + meterRegisterRead->registerDef->numRegisters - 1;
                res = readRegisters (meter, meterRegisterRead->registerDef->regType, regStart, meterRegisterRead->registerDef->numRegisters, &buf);
                if (res == 0) {
                    if (getRegisterValue (meterRegisterRead, buf, 0, regStart, regEnd) != 0) {
                        EPRINTF("%s: getRegisterValue for %s failed with %d",meter->name,meterRegisterRead->registerDef->name);
                        meter->numErrs++;
                        return res;
                    }
                    if (verbose > 3) {
                        PRINTF("%s: received %d regs for %s from %d (0x%02x) to %d (0x%02x) (not covered via block read), data received: ",meter->name,meterRegisterRead->registerDef->numRegisters,meterRegisterRead->registerDef->name,regStart,regStart,regEnd,regEnd);
                        dumpBuffer(buf,meterRegisterRead->registerDef->numRegisters);
                    }

                    free(buf); buf = NULL;
                } else {
                	meter->numErrs++;
                	VPRINTFN(1,"%s (%s): readRegisters (%d (0x%04x),%d) failed (%s)",meter->name,meterRegisterRead->registerDef->name,regStart,regStart,regEnd,modbus_strerror(errno));
                    return res;
                }
            } else
                VPRINTFN(4,"%s: register %d (0x%04x) (%s) already set (from one of the read's)",meter->name,meterRegisterRead->registerDef->startAddr,meterRegisterRead->registerDef->startAddr,meterRegisterRead->registerDef->name);
        }
        meterRegisterRead = meterRegisterRead->next;
	}

#ifndef DISABLE_FORMULAS
	// local formulas
	executeMeterTypeFormulas(verboseMsg,meter);
#endif

	if (verbose > 2) {	// show values read
		meterRegisterRead = meter->registerRead;
		while (meterRegisterRead) {
			if (meterRegisterRead->isInt) {
				printf(" %-20s %10d\n",meterRegisterRead->registerDef->name,(int)meterRegisterRead->fvalue);
			} else {
				if (meterRegisterRead->registerDef->decimals == 0)
					strcpy(format," %-20s %10.0f\n");
				else
					sprintf(format," %%-20s %%%d.%df\n",11+meterRegisterRead->registerDef->decimals,meterRegisterRead->registerDef->decimals);
				printf(format,meterRegisterRead->registerDef->name,meterRegisterRead->fvalue);
			}
			meterRegisterRead = meterRegisterRead->next;
		}
	}

	// mark complete
	meter->meterHasBeenRead = 1;

	clock_gettime(CLOCK_MONOTONIC,&timeEnd);
	meter->queryTimeNano = ((timeEnd.tv_sec - timeStart.tv_sec) * NANO_PER_SEC) + (timeEnd.tv_nsec - timeStart.tv_nsec);

	if (meter->queryTimeNano < meter->queryTimeNanoMin || meter->queryTimeNanoMin == 0) meter->queryTimeNanoMin = meter->queryTimeNano;
	if (meter->queryTimeNano > meter->queryTimeNanoMax) meter->queryTimeNanoMax = meter->queryTimeNano;
	meter->queryTimeNanoAvg = (meter->queryTimeNano + meter->queryTimeNanoAvg) / 2;

	return 0;
}


int queryMeters(int verboseMsg) {
	meter_t *meter;
	int res;
	int numMeters = 0;

	setfvalueInfluxLast ();   // set last value for all meter registers
	//  query
	meter = meters;
	while (meter) {
		res = queryMeter(verboseMsg,meter);
		if (res != 0) {
			EPRINTFN("%s: query failed",meter->name);
		} else {
			numMeters++;
		}
		meter = meter->next;
	}
#ifndef DISABLE_FORMULAS
	// meter formulas
	meter = meters;
	while (meter) {
		if (! meter->disabled) executeMeterFormulas(meter);
		//if (res != 0) EPRINTFN("%s: execute formulas failed",meter->name);
		meter = meter->next;
	}
#endif
    // handle influxWriteMult
    setfvalueInflux();  // set for all meters after formulas
    meter = meters;
	while (meter) {
		if (! meter->disabled) executeInfluxWriteCalc(verboseMsg,meter);
		meter = meter->next;
	}
	return numMeters;
}


// test if we can find the first defined meter on modbus RTU, returns 1 if meter found
int testRTUpresent() {
    meter_t *meter = meters;
	int res;
	uint16_t *buf;
	int startReg;

	while (meter)
        if (!meter->isTCP) {
            modbus_set_slave(*meter->mb,meter->modbusAddress);
            if (meter->needSunspecResolve) {
                // sunspec, try to read the id
                startReg = meter->meterType->sunspecBaseAddr;
                // sunspec header
                res = readRegisters (meter, regTypeHolding, startReg, 4, &buf);
                if (res != 0) {
                    VPRINTFN(1,"%s: read of sunspec header (%d,4) failed - %s\n",meter->name,startReg,modbus_strerror(errno));
                    return 0;
                }

                if (buf[0] != 0x5375 || buf[1] != 0x6e53) {
                    VPRINTFN(1,"%s: invalid sunspec modbus map header id @ %d,%d, expected 0x53756e53, found %04x %04x\n",meter->name,startReg,startReg+1,buf[0],buf[1]);
                    return 0;
                }
                if (buf[2] != 1) {
                    VPRINTFN(1,"%s: invalid sunspec header id @ %d, expected 1, found %d\n",meter->name,startReg,buf[2]);
                    return 0;
                }
                free(buf);
                return 1;       // found
            }
            // not sunspec, try to read the first defined register
            if (!meter->registerRead) {     // we should have register for all defined meters
                startReg = meter->registerRead->registerDef->startAddr;
                res = readRegisters(meter,meter->registerRead->registerDef->regType, startReg,meter->registerRead->registerDef->numRegisters,&buf);
                if (res != 0) {
                    VPRINTFN(1,"%s: read %d registers starting at %d failed with %d %s",meter->name,meter->registerRead->registerDef->numRegisters,startReg,res,modbus_strerror(res));
                    return 0;
                }
                free(buf);
                return 1;
            }
            meter = meter->next;
        }
    EPRINTFN("try: unable to try because of no modbus RTU meters defined");
    return 0;
}


void meterSetTarif (meter_t *meter, int tarif, int verboseMsg) {
    meterInit_t *mi;
    int sunspecOffset;
    int res;

    if (tarif < 1 || tarif > TARIF_MAX+1) {
        if (verboseMsg) printf("%s: invalid tarif %d\n",meter->name,tarif);
        return;
    }
    mi = meter->meterType->setTarif[tarif-1];
    if (mi) {
        sunspecOffset = sunspecGetOffset (meter,mi->sunspecId);
        //printf("%04x %d %d\n",mi->startAddr,sunspecOffset,mi->numWords);
        if (mi->numWords == 1)
            res = modbus_write_register(*meter->mb,mi->startAddr+sunspecOffset,*mi->buf);
        else
            res = modbus_write_registers(*meter->mb,mi->startAddr+sunspecOffset,mi->numWords,mi->buf);
        if (res < 0) {
            EPRINTFN("%s: set tarif %d failed (write of %d registers starting at address %d (%d: %s)",meter->name,tarif,mi->numWords,mi->startAddr+sunspecOffset,res,modbus_strerror(res));
            //exit(1);
        }
    } else {
        if (verboseMsg) printf(": no definition for setting tarif %d\n",tarif);
    }
}


void setTarif (int verboseMsg) {
    int tarifNew;
    meterTarif_t * mt = meterTarifs;
    meterList_t * ml;
    meter_t * meter;
    int i;
    int iValue;
    float fValue;
    int forceFloat;


    if (!mt) return;

    if (verboseMsg) printf("\nSet tarif\n");

    while(mt) {
        if (mt->meterRegisterRead->hasBeenRead) {
            tarifNew = 1;
            forceFloat = 0;
            //printf("%d %d %1.2f\n",mt->meterRegisterRead->isInt,mt->meterRegisterRead->ivalue,mt->meterRegisterRead->fvalue);
            if (mt->meterRegisterRead->isInt) {
                iValue = (int)mt->meterRegisterRead->fvalue;
                fValue = (int)mt->meterRegisterRead->fvalue;
            } else {
                fValue = mt->meterRegisterRead->fvalue;
                forceFloat++;
            }
            for (i=0;i<TARIF_MAX-1;i++) {

                    if (forceFloat) {
                        //printf("%1.4f\n",mt->fValues[i]);
                        if (mt->fValues[i] != 0) {
                            if (verboseMsg) printf(" %s.%s: %1.2f %1.2f --> ",mt->meter->name,mt->meterRegisterRead->registerDef->name , fValue,mt->fValues[i]);
                            if (fValue < mt->fValues[i]) {
                                tarifNew = i+2;
                                if (verboseMsg) printf("true, tarif %d\n",tarifNew);
                                break;
                            }
                            if (verboseMsg) printf("false\n");
                        }
                    } else {    // int
                        if (mt->iValues[i] != 0) {
                            if (verboseMsg) printf(" %s.%s: %d %d --> ",mt->meter->name,mt->meterRegisterRead->registerDef->name , iValue,mt->iValues[i]);
                            if (iValue < mt->iValues[i]) {
                                tarifNew = i+2;
                                if (verboseMsg) printf("true, tarif %d\n",tarifNew);
                                break;
                            }
                            if (verboseMsg) printf("false\n");
                        }
                    }
            }

            if (tarifNew != mt->tarifCurr) {
                if (verboseMsg) printf(" Set tarif %d for ",tarifNew);
                if (mt->meters) {       // defined meter list for this traif definition
                    ml = mt->meters;
                    while (ml) {
                        if (verboseMsg) { printf(ml->meter->name); printf(" "); }
                        meterSetTarif(ml->meter,tarifNew,verboseMsg);
                        ml = ml->next;
                    }
                } else {            // all meters
                    meter = meters;
                    while (meter) {
                        if (verboseMsg) { printf(meter->name); printf(" "); }
                        meterSetTarif(meter,tarifNew,verboseMsg);
                        meter = meter->next;
                    }
                }
                mt->tarifCurr = tarifNew;
                if (verboseMsg) printf("\n");
            }
        }

        mt = mt->next;
    }
}

void modbusread_free() {
#ifndef DISABLE_FORMULAS
  if (parser) {
    delete (parser);
    parser = NULL;
  }
#endif
  modbusTCP_freeAll();
  modbusRTU_freeAll();
}


// convert float value into modbus registers (1..4) depending on type
int createModbusRegWriteBuff (uint16_t *dest, int type, double value) {

    int size;   // in words
    switch (type) {
		case TK_FLOAT:
            size = 2;
            modbus_set_float(value, dest);
			break;
		case TK_FLOAT_CDAB:
			size = 2;
            modbus_set_float_cdab(value, dest);
			break;
		case TK_FLOAT_ABCD:
			size = 2;
            modbus_set_float_abcd(value, dest);
			break;
		case TK_FLOAT_BADC:
			size = 2;
            modbus_set_float_badc(value, dest);
			break;
		case TK_INT16: {
            size = 1;
            int16_t *v = (int16_t *)dest;
			*v = value;
			break;
        }
		case TK_INT32: {
            size = 2;
            int32_t iv32 = value;
            dest[0] = iv32 >> 16;
            dest[1] = iv32 & 0xffff;
			//rr->fvalue = ((int32_t) buf[bufStartAddr] << 16) | buf[bufStartAddr+1];
			break;
        }
        case TK_INT32L: {
            size = 2;
            int32_t iv = value;
            dest[1] = iv >> 16;
            dest[0] = iv & 0xffff;
			//rr->fvalue = ((int32_t) buf[bufStartAddr] << 16) | buf[bufStartAddr+1];
			break;
        }

		case TK_INT48: {
            size = 3;
            int64_t iv = value;
            if (value < 0) {
                iv &= 0xffffffffffff;
                iv |= 0x800000000000;   // negative bit - untested
            }
            dest[0] = iv >> 32;
            dest[1] = iv >> 16;
            dest[2] = iv & 0xffff;
			//rr->fvalue = ((int64_t)buf[bufStartAddr] << 32) | ((int64_t)buf[bufStartAddr+1] << 16) | buf[bufStartAddr+2];
			break;
        }
        case TK_INT48L: {
            size = 3;
            int64_t iv = value;
            if (value < 0) {
                iv &= 0xffffffffffff;
                iv |= 0x800000000000;   // negative bit - untested
            }
            dest[2] = iv >> 32;
            dest[1] = iv >> 16;
            dest[0] = iv & 0xffff;
			break;
        }
		case TK_INT64: {
            size = 4;
            int64_t iv = value;
            dest[0] = iv >> 48;
            dest[1] = iv >> 32;
            dest[2] = iv >> 16;
            dest[3] = iv & 0xffff;
			//rr->fvalue = ((int64_t)buf[bufStartAddr] << 48) | ((int64_t)buf[bufStartAddr+1] << 32) | ((int64_t)buf[bufStartAddr+2] << 16) | buf[bufStartAddr+3];
			break;
        }
        case TK_INT64L: {
            size = 4;
            int64_t iv = value;
            dest[3] = iv >> 48;
            dest[2] = iv >> 32;
            dest[1] = iv >> 16;
            dest[0] = iv & 0xffff;
			break;
        }
		case TK_UINT16: {
            size = 1;
			dest[0] = value;
			break;
        }
		case TK_UINT32: {
            size = 2;
            uint32_t iv32 = value;
            dest[0] = iv32 >> 16;
            dest[1] = iv32 & 0xffff;
			break;
        }
        case TK_UINT32L: {
            size = 2;
            uint32_t iv32 = value;
            dest[1] = iv32 >> 16;
            dest[0] = iv32 & 0xffff;
			break;
        }
        case TK_UINT48: {
            size = 3;
            uint64_t iv = value;
            dest[0] = iv >> 32;
            dest[1] = iv >> 16;
            dest[2] = iv & 0xffff;
			break;
        }
        case TK_UINT48L: {
            size = 3;
            uint64_t iv = value;
            dest[2] = iv >> 32;
            dest[1] = iv >> 16;
            dest[0] = iv & 0xffff;
			break;
        }
        case TK_UINT64: {
            size = 4;
            uint64_t iv = value;
            dest[0] = iv >> 48;
            dest[1] = iv >> 32;
            dest[2] = iv >> 16;
            dest[3] = iv & 0xffff;
			break;
        }
        case TK_UINT64L: {
            size = 4;
            uint64_t iv = value;
            dest[3] = iv >> 48;
            dest[2] = iv >> 32;
            dest[1] = iv >> 16;
            dest[0] = iv & 0xffff;
			break;
        }
        default:
			EPRINTFN("fatal internal error: modbusread.c createModbusRegWriteBuff, unhandled type (%d)",type);
			exit(255);
    }
    return size;
}


// target can be a register, a coil or a variable of a meter formula (only useful for following writes)
void execMeterWrite(meterWrites_t *mw, int dryrun) {
	meterWrite_t *w = mw->meterWrite;
	double val,conditionVal;
	int res;
	uint16_t writeRegisters[4];

	if (!mw->meterWrite) return;
	if (mw->meter->writeDisabled) return;

	VPRINTFN(3,"executing meter writes for %s.%s",mw->meter->name,mw->name);

	if (mw->conditionFormula) {
		if (!parser) initParser();
		try {
            parser->SetExpr(mw->conditionFormula);
            conditionVal = parser->Eval();
			VPRINTF(4,"  execMeterWrites.condition (%s) \"%s\" %10.2f - %s\n",mw->meter->name,mw->conditionFormula,conditionVal,conditionVal > 0 ? "TRUE" : "FALSE");
        }
        catch (mu::Parser::exception_type &e) {
            EPRINTFN("  %s.%s error evaluating meter write condition (%s)",mw->meter->name,mw->name,e.GetMsg().c_str());
			exit(1);
        }
        if (conditionVal <= 0) {
        	VPRINTFN(3,"  Meter write condition for %s.%s (%s) not met, exiting meter writes",mw->meter->name,mw->name,mw->conditionFormula);
			return;
        }
	}

	// tcp open retry
	if (mw->meter->isTCP) {
		mw->meter->mb = modbusTCP_open (mw->meter->hostname,mw->meter->port);	// get it from the pool or create/open if not already in list of connections
		if(mw->meter->mb == NULL) {
			//WPRINTFN("%s: connect to %s:%s failed, will retry later, errno: %d (%s)",meter->name,meter->hostname,meter->port ? meter->port : defPort,errno,modbus_strerror(errno));
			mw->meter->numErrs++;
			return;
		}
	}

	res = modbus_set_slave(*mw->meter->mb, mw->meter->modbusAddress);
	if(res < 0) {
		EPRINTFN("  execMeterWrite: modbus_set_slave %d (%s) failed with %d",mw->meter->modbusAddress,mw->meter->name,res);
		return;
	}


	while (w) {
		VPRINTFN(4,"  execMeterWrite (%s.%s)",mw->meter->name,w->reg->name);
		conditionVal = 1.0;
#ifndef DISABLE_FORMULAS
		if (w->conditionFormula) {
			if (!parser) initParser();	// Global parser
			try {
				parser->SetExpr(w->conditionFormula);
				conditionVal = parser->Eval();
				VPRINTFN(4,"  execMeterWrite.condition (%s.%s) \"%s\" %10.2f - %s",mw->meter->name,w->reg->name,w->conditionFormula,conditionVal,conditionVal > 0 ? "TRUE" : "FALSE");
			}
			catch (mu::Parser::exception_type &e) {
				EPRINTFN("  error evaluating write condition formula within %s for meter %s, register %s, formula: \"%s\" (%s)",mw->name,mw->meter->name,w->reg->name,w->conditionFormula,e.GetMsg().c_str());
				mw->meter->numErrs++;
				return;
			}
		}
#endif // DISABLE_FORMULAS

		if (conditionVal > 0) {
#ifndef DISABLE_FORMULAS
			if (w->formula) {
				if (!parser) initParser();	// Global parser
				try {
					parser->SetExpr(w->formula);
					val = parser->Eval();
					VPRINTF(3,"  execMeterWrite: %s.%s \"%s\" %10.2f\n",mw->meter->name,w->reg->name,w->formula,val);
				}
				catch (mu::Parser::exception_type &e) {
					EPRINTFN("  error evaluating meter write formula %s for meter %s, formula: \"%s\" (%s)",mw->name,mw->meter->name,w->formula,e.GetMsg().c_str());
					mw->meter->numErrs++;
					return;
				}
			} else
#endif
				val = w->value;

	#if LIBMODBUS_VERSION_CHECK(3,1,0)
			modbus_set_response_timeout(*mw->meter->mb, READ_TIMEOUT_SECS, 0);  // 3 seconds
	#else
			struct timeval response_timeout;
			response_timeout.tv_sec = READ_TIMEOUT_SECS;
			response_timeout.tv_usec = 0;
			modbus_set_response_timeout(*mw->meter->mb, &response_timeout);
	#endif

			if (w->reg->isFormulaOnly) {
				int setDone = 0;
				meterRegisterRead_t * rr = mw->meter->registerRead;
				while (rr && (!setDone)) {
					if (strcmp(w->reg->name,rr->registerDef->name) == 0) {
						rr->fvalue = val;
					setDone++;
					}
					rr = rr->next;
				}
				if (setDone) {
					VPRINTFN(3,"  %s.%s (%s): wrote %f to meter formula register",mw->meter->name,w->reg->name,mw->name,val);
				} else
					EPRINTFN("  %s.%s (%s): unable to write %f to meter formula register",mw->meter->name,w->reg->name,mw->name,val);
			} else
			if (w->reg->regType == regTypeHolding) {
				int numRegisters = createModbusRegWriteBuff (writeRegisters, w->reg->type, val);
				if (numRegisters < 1 || numRegisters > 4) {
					EPRINTFN("  fatal internal error: modbusread.cpp.execMeterWrite createModbusRegWriteBuff returned invalid number of registers (%d)",numRegisters);
					exit(255);
				}
				char st[30] = "";
				char *sp = (char *)&st;

				if (dryrun || verbose > 0) {
					for (int i=0;i<numRegisters;i++) {
						sprintf(sp,"%04x ",writeRegisters[i]);
						sp+=5;
					}
				}

				if (dryrun) {
					PRINTFN("%s (%s): would write %d words starting at address %d with value %f [ %s]",mw->meter->name,mw->name,numRegisters,w->reg->startAddr,val,st);
				} else {
					int rc;
					char s[2] = "";
					//VPRINTFN(3,"  %s (%s): write %d words starting at address %d with value %f [ %s]",mw->meter->name,mw->name,numRegisters,w->reg->startAddr,val,st);
					// TODO: check if value is already there
					if (numRegisters == 1) {
						rc = modbus_write_register(*mw->meter->mb, w->reg->startAddr, writeRegisters[0]);  // function 0x06
					} else {
						strcpy(s,"s");
						rc = modbus_write_registers(*mw->meter->mb, w->reg->startAddr, numRegisters, writeRegisters);   // function 0x10
					}

					if (rc != numRegisters) {
						EPRINTFN("%s: execMeterWrite (%s): modbus_write_register%s returned %d, expected %d, errno: %d (%s)",mw->meter->name,mw->name,s,rc,numRegisters,errno,modbus_strerror(errno));
					} else
						VPRINTFN(3,"  %s.%s (%s): wrote %d register(s) at address %d with value %f",mw->meter->name,w->reg->name,mw->name,numRegisters,w->reg->startAddr,val);
				}
			} else
			if (w->reg->regType == regTypeCoil) {
				if (dryrun) {
					PRINTFN("%s (%s): would write coil at address %d with value %s",mw->meter->name,mw->name,w->reg->startAddr,val > 0 ? "true" : "false");
				} else {
					int rc = modbus_write_bit(*mw->meter->mb, w->reg->startAddr, val > 0 ? TRUE : FALSE);
					if (rc != 1) {
						EPRINTFN("%s: execMeterWrite (%s.%s): modbus_write_bit returned %d, expected 1, errno: %d (%s)",mw->meter->name,w->reg->name,mw->name,rc,errno,modbus_strerror(errno));
					} else
						VPRINTFN(3,"  %s (%s): wrote coil at address %d with value %f [ %d]",mw->meter->name,mw->name,w->reg->startAddr,val,val > 0 ? 1 : 0);
				}
			} else
				EPRINTFN("%s: internal error: modbusread.cpp.execMeterWrite (%s) regType is not Holding nor Coil)",mw->meter->name,mw->name);
			if (w->returnOnWrite) return;
		} else {
			VPRINTFN(3,"  write condition for %s.%s.%s (%s) not met, skipping meter write",mw->meter->name,mw->name,w->reg->name,mw->conditionFormula);

		}
		w = w->next;
	}
}
