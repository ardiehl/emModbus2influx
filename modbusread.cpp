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
#include <endian.h>
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

modbus_t *mb_RTU;

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
				if (mc->retryCount) {
					mc->retryCount--;
					return NULL;
				}
				res = modbus_connect(mc->mb);
				if (res == 0) {
					mc->isConnected = 1;
					VPRINTFN(2,"modbusTCP_open (%s:%s): connected",device,port);
					return &mc->mb;
				}
				VPRINTFN(2,"modbusTCP_open (%s:%s): connect failed retrying later",device,port);
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
		modbus_set_error_recovery(mc->mb,(modbus_error_recovery_mode) (MODBUS_ERROR_RECOVERY_LINK | MODBUS_ERROR_RECOVERY_PROTOCOL));
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
}

//*****************************************************************************


/* msleep(): Sleep for the requested number of milliseconds. */
int msleep(long msec)
{
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


//*****************************************************************************

modbus_t ** modbusRTU_getmh() {
	return &mb_RTU;
}

int modbusRTU_open (const char *device, int baud, const char *parity, int stop_bit, int mode_rs485) {
	char par;
	int res;

	if (parity == NULL) par = 'O'; else {
		par = toupper(*parity);
		if (par != 'O' && par != 'N' && par != 'E') {
			EPRINTFN("Invalid parity (%s) specified, valid is O,N or E",parity);
			exit (1);
		}
	}
	mb_RTU = modbus_new_rtu(device, baud, par, 8, stop_bit);
	if (mb_RTU == NULL) return -1;
	res = modbus_connect(mb_RTU);
	if (res == -1) {
		EPRINTFN("modbus_connect failed: %d - %s", errno,modbus_strerror(errno));
		modbus_free(mb_RTU);
		mb_RTU = NULL;
		return -1;
	}
	modbus_set_error_recovery(mb_RTU, MODBUS_ERROR_RECOVERY_PROTOCOL);
	if (mode_rs485)
	    res = modbus_rtu_set_serial_mode(mb_RTU, MODBUS_RTU_RS485);
	else
        res = modbus_rtu_set_serial_mode(mb_RTU, MODBUS_RTU_RS232);
    if (res != 0) EPRINTFN("Warning: modbus_rtu_set_serial_mode failed with %d (%s)", res, modbus_strerror(errno));

	VPRINTFN(7,"modbusRTU_open: %s opened (%d 8%c%d)",device,baud,par,stop_bit);
	return 0;
}


void modbusRTU_close() {
	if (mb_RTU) {
		modbus_free(mb_RTU);
		mb_RTU = NULL;
	}
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

	VPRINTFN(5,"getRegisterValue %d numRegs: %d, regStart: %d, regEnd: %d, type: %d",rr->registerDef->startAddr,rr->registerDef->numRegisters,regStart,regEnd,rr->registerDef->type);

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


#define READ_TIMEOUT_SECS 3
int readRegisters (meter_t *meter, int startAddr, int numRegisters, uint16_t **buf) {

	int res;

	*buf = (uint16_t *)calloc(numRegisters,sizeof(uint16_t));
	if (!*buf) return -1;

#if LIBMODBUS_VERSION_CHECK(3,1,0)
	modbus_set_response_timeout(*meter->mb, READ_TIMEOUT_SECS, 0);  // 3 seconds
#else
	struct timeval response_timeout;
	response_timeout.tv_sec = READ_TIMEOUT_SECS;
	response_timeout.tv_usec = 0;
	modbus_set_response_timeout(*meter->mb, &response_timeout);
#endif

	res = modbus_set_slave(*meter->mb, meter->modbusAddress);
		if(res < 0) {
			EPRINTFN("modbus_set_slave failed with %d",res);
			free(*buf); *buf = NULL;
			return -1;
	}

    res = modbus_read_registers(*meter->mb, startAddr, numRegisters, *buf);
    if (res < 0) {
		// we sometimes get a timeout on Fronius Symo via TCP
		modbus_flush(*meter->mb);
		usleep(100*1000);
		modbus_flush(*meter->mb);
		res = modbus_read_registers(*meter->mb, startAddr, numRegisters, *buf);
	}

    if(res < 0) {
    	EPRINTFN("modbus_read_registers (%d registers starting at %d) failed with %d (%s)",numRegisters,startAddr,errno,modbus_strerror(errno));
        free(*buf); *buf = NULL;
        return -1;
    }
// untested, do we need that ?
#if __BYTE_ORDER == __BIG_ENDIAN
	uint16_t *p = *buf;
	int i;
	for (i=0;i<numRegisters;i++) {
		*p = le16toh(*p);
		p++;
	}
#endif // __BYTE_ORDER

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
		return -1;
	}

	// sunspec header
	res = readRegisters (meter, startReg, 4, &buf);
	if (res != 0) {
		EPRINTFN("read of sunspec header (%d,4) failed - %s\n",startReg,modbus_strerror(errno));
		return res;
	}
	if (buf[0] != 0x5375 || buf[1] != 0x6e53) {
		EPRINTFN("%s: invalid sunspec modbus map header id @ %d,%d, expected 0x53756e53, found %04x %04x\n",meter->name,startReg,startReg+1,buf[0],buf[1]);
		return -1;
	}
	if (buf[2] != 1) {
		EPRINTFN("%s: invalid sunspec header id @ %d, expected 1, found %d\n",meter->name,startReg,buf[2]);
		return -1;
	}

	len = buf[3] + 4;  // buf[3] is w/o modbus map header id(2), type(1) and length(1)
	free (buf);
	startReg += len;


	// read first header type and length after common block
	res = readRegisters (meter, startReg, 2, &buf);
	if (res != 0) {
		EPRINTFN("read of sunspec header (%d,2) failed - %s\n",startReg,modbus_strerror(errno));
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
		res = readRegisters (meter, startReg, len+4, &buf);
		if (res != 0) {
			EPRINTFN("%s: read of sunspec block (%d,%d), type:%d, len: %d failed - %s\n",meter->name,startReg,len+4,type,len,modbus_strerror(errno));
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
		VPRINTFN(3,"\r%s: found sunspec id %d, length: %d, offset: %d",meter->name,type,len,startReg);
		meter->sunspecIds->cnt++;
		startReg = startReg + len + 2;
		type = buf[len+2]; len = buf[len+3];

		free(buf);
	}

	if (meter->sunspecIds->cnt < 1) {
		EPRINTFN("\r%s: unable to read sunspec ids");
		return -1;
	}
	if (verboseMsg) {
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
				VPRINTFN(3,"%s: changed address %d to %d for sunspec id %d (%d)",meter->name,mrrd->startAddr,mrrd->startAddr+id->offset,mrrd->sunspecId,id->offset);
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

static value_type Rnd(value_type v) { return v * std::rand() / (value_type)(RAND_MAX + 1.0); }

// init the global parser and add all variables
mu::Parser * initParser() {
    meterRegisterRead_t *registerRead;
    meter_t *meter = meters;
    char name[255];

    if (parser == NULL) {
        parser = new (mu::Parser);
        parser-> DefineNameChars(MUPARSER_ALLOWED_CHARS);
        parser->DefineFun(_T("rnd"), Rnd, false);     // Add an unoptimizeable function
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
                        EPRINTFN("error adding variable %s (%s)",&name,e.GetMsg().c_str());
                        exit(1);
                    }

                    registerRead = registerRead->next;
                }
            }
            meter = meter->next;
        }
    }
    return parser;
}


// init the global parser and add all local variables
//mu::Parser *parser
mu::Parser * initLocalParser(meter_t *meter) {
    meterRegisterRead_t *registerRead;
    mu::Parser *parser;

    parser = new (mu::Parser);
    parser-> DefineNameChars(MUPARSER_ALLOWED_CHARS);
    parser->DefineFun(_T("rnd"), Rnd, false);     // Add an unoptimizeable function
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


void executeMeterFormulas(int verboseMsg, meter_t * meter) {
    meterFormula_t * mf = meter->meterFormula;
    mu::Parser *parser = NULL;
    if (!mf) return;
    if (verbose || verboseMsg)
		printf("\nexecuteMeterFormulas for \"%s\"\n",meter->name);
    while(mf) {
        if (!parser) parser = initParser();
        try {
            parser->SetExpr(mf->formula);
            mf->fvalue = parser->Eval();
            if (verbose || verboseMsg)
				printf("%s \"%s\" %10.2f\n",mf->name,mf->formula,mf->fvalue);
        }
        catch (mu::Parser::exception_type &e) {
            EPRINTFN("%d.%d error evaluating meter formula (%s)",meter->name,mf->name,e.GetMsg().c_str());
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
                EPRINTFN("%d.%d error evaluating meter type formula (%s)",meter->name,registerRead->registerDef->name,e.GetMsg().c_str());
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
           "This will return 1 of any of the four voltages are above 240.\n\n");
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


void setfvalueInfluxLast () {
    meter_t *meter = meters;
    meterRegisterRead_t *mrrd;
    meterFormula_t *mf;

    while (meter) {
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
        meter = meter->next;
    }
}


void setfvalueInflux () {
    meter_t *meter = meters;
    meterRegisterRead_t *mrrd;
    meterFormula_t *mf;

    while (meter) {
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

	if (meter->disabled) return 0;
	if (!meter->hostname)
		if (meter->modbusAddress == 0) return 0;		// virtual meter with formulas only

	if (verboseMsg) {
        if (verboseMsg > 1) printf("\n");
		if (meter->hostname) printf("Query \"%s\" @ TCP %s:%s, Modbus address %d\n",meter->name,meter->hostname,meter->port == NULL ? "502" : meter->port,meter->modbusAddress);
		else printf("Query \"%s\" @ ModbusRTU address %d\n",meter->name,meter->modbusAddress);
	} else
		VPRINTFN(8,"%s: queryMeter Modbus address %d",meter->name,meter->modbusAddress);

	// tcp open retry
	if (meter->isTCP) {
		meter->mb = modbusTCP_open (meter->hostname,meter->port);	// get it from the pool or create/open if not already in list of connections
		if(meter->mb == NULL) {
			EPRINTFN("%s: connect to %s:%d failed, will retry later",meter->name,meter->hostname,meter->port);
			return -555;
		}
	} else msleep(50);
	modbus_set_debug(*meter->mb,meter->modbusDebug);
    modbus_set_slave(*meter->mb,meter->modbusAddress);


	// reset read flags
	meter->meterHasBeenRead = 0;

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
			exit(1);
		}
		meter->needSunspecResolve = 0;
	}

	// meter init
	if (!meter->initDone) {
        meterInit_t *mi = meter->meterType->init;
        count=0;
        if (verboseMsg && mi) {
            printf("%s: initializing ",meter->name); fflush(stdout);
        }

        while (mi) {
            if (mi->numWords) {
                sunspecOffset = sunspecGetOffset (meter,mi->sunspecId);
                //printf("%04x %d %d\n",mi->startAddr,sunspecOffset,mi->numWords);
                if (mi->numWords == 1)
                    res = modbus_write_register(*meter->mb,mi->startAddr+sunspecOffset,*mi->buf);
                else
                    res = modbus_write_registers(*meter->mb,mi->startAddr+sunspecOffset,mi->numWords,mi->buf);
                if (verboseMsg) { printf("."); fflush(stdout); }
                if (res < 0) {
                    EPRINTFN("%s: init failed (write of %d registers starting at address %d (%d: %s)",meter->name,mi->numWords,mi->startAddr+sunspecOffset,res,modbus_strerror(errno));
                    exit(1);
                }
                count++;
            }
            mi = mi->next;
        }
        meter->initDone = 1;
        if (verboseMsg && count) printf("\n");

    }



	// first read the defined sections
	meterReads = meter->meterType->meterReads;
	while (meterReads) {
		blockUsed = 0;
		sunspecOffset = sunspecGetOffset (meter,meterReads->sunspecId);
		regStart = meterReads->startAddr + sunspecOffset;
		numRegisters = meterReads->numRegisters;
		if (numRegisters == 0) numRegisters = sunspecGetNumRegs (meter,meterReads->sunspecId);
		regEnd = regStart + numRegisters - 1;
		VPRINTFN(3,"%s: reading block of %d registers starting at %d (0x%02x)",meter->name,numRegisters,meterReads->startAddr,meterReads->startAddr);
		res = readRegisters (meter, regStart, numRegisters, &buf);
		if (res == 0) {
			if (verbose > 1) {
				printf (" received block from %d (0x%02x) to %d (0x%02x): ",regStart,regStart,regEnd,regEnd);
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
                        VPRINTFN(2,"%s (%s): sunspec scaling factor register %d (%d), sf=%d",meter->name,meterRegisterRead->registerDef->name,meterRegisterRead->registerDef->sunspecSfRegister,reg,meterRegisterRead->sunspecSf);
                    }
                }

				//printf("regStart: %d regEnd: %d  %d %d\n",regStart,regEnd,meterRegisterRead->startAddr,meterRegisterRead->startAddr + meterRegisterRead->registerDef->numRegisters-1);
				if ((regStart <= meterRegisterRead->startAddr) && (meterRegisterRead->startAddr + meterRegisterRead->registerDef->numRegisters - 1 <= regEnd)) {
						meterRegisterRead->hasBeenRead++;
						if (getRegisterValue (meterRegisterRead, buf, 0, regStart, regEnd) != 0) {
							EPRINTFN("%s: getRegisterValue for %s failed",meter->name,meterRegisterRead->registerDef->name);
							free(buf);
							exit(1);
						}
						blockUsed++;
				}
				meterRegisterRead = meterRegisterRead->next;
			}
			if (verboseMsg)
				if (blockUsed == 0) printf("  --> Block from %d to %d is useless\n",regStart,regEnd);
			free(buf); buf = NULL;
		} else {
			if (res < -1) return res;  // for TCP open retry
			EPRINTFN("%s: failed to read block of %d registers starting at register %d, res: %d",meter->meterType->name,numRegisters,regStart,res);
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

                if (readRegisters (meter, reg, 1, &buf) != 0) {
                    EPRINTFN("%s: readRegisters for sunspec scaling factor %s, register %d failed",meter->name,meterRegisterRead->registerDef->name,reg);
                    exit(1);
                }
                meterRegisterRead->sunspecSf = (int16_t)buf[0];
                VPRINTFN(2,"%s (%s): Warning: sunspec scaling factor register %d (%d) not included in read or readid blocks, sf=%d",meter->name,meterRegisterRead->registerDef->name,meterRegisterRead->registerDef->sunspecSfRegister,reg,meterRegisterRead->sunspecSf);
                free(buf);
            }

            if (meterRegisterRead->hasBeenRead == 0) {
                regStart = meterRegisterRead->startAddr;
                regEnd = regStart + meterRegisterRead->registerDef->numRegisters - 1;
                res = readRegisters (meter, regStart, meterRegisterRead->registerDef->numRegisters, &buf);
                if (res == 0) {
                    if (getRegisterValue (meterRegisterRead, buf, 0, regStart, regEnd) != 0) {
                        EPRINTF("%s: getRegisterValue for %s failed with %d",meter->name,meterRegisterRead->registerDef->name);
                        exit(1);
                    }
                    if (verbose > 0) {
                        printf ("%s: received %d regs for %s from %d (0x%02x) to %d (0x%02x) (not covered via block read), data received: ",meter->name,meterRegisterRead->registerDef->numRegisters,meterRegisterRead->registerDef->name,regStart,regStart,regEnd,regEnd);
                        dumpBuffer(buf,meterRegisterRead->registerDef->numRegisters);
                    }

                    free(buf); buf = NULL;
                } else {
                    return res;
                }
            } else
                VPRINTFN(3,"%s: register %d (%s) already set (from one of the read's)",meter->name,meterRegisterRead->registerDef->startAddr,meterRegisterRead->registerDef->name);
        }
        meterRegisterRead = meterRegisterRead->next;
	}

#ifndef DISABLE_FORMULAS
	// local formulas
	executeMeterTypeFormulas(verboseMsg,meter);
#endif

	if (verboseMsg > 1) {
		meterRegisterRead = meter->registerRead;
		while (meterRegisterRead) {
			if (meterRegisterRead->isInt) {
#ifdef BUILD_32
				printf(" %-20s %10lld\n",meterRegisterRead->registerDef->name,meterRegisterRead->ivalue);
#else
				printf(" %-20s %10d\n",meterRegisterRead->registerDef->name,(int)meterRegisterRead->fvalue);
#endif
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

	meterRegisterRead = meter->registerRead;

	// mark complete
	meter->meterHasBeenRead = 1;
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
		if (! meter->isTCP) msleep(100);
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
		if (! meter->disabled) executeMeterFormulas(verboseMsg,meter);
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
                res = readRegisters (meter, startReg, 4, &buf);
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
                res = readRegisters(meter,startReg,meter->registerRead->registerDef->numRegisters,&buf);
                if (res != 0) {
                    VPRINTFN(1,"%s: read %d registers starting at %d failed with %d %s",meter->name,meter->registerRead->registerDef->numRegisters,startReg,res,modbus_strerror(res));
                    return 0;
                }
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

