#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "meterDef.h"
#include "argparse.h"
#include "log.h"
#include "modbusread.h"
#include "parser.h"
#include <endian.h>

extern int mqttQOS;
extern int mqttRetain;
extern char * mqttprefix;
extern int influxWriteMult;    // write to influx only on x th query (>=2)
extern int modbusDebug;


meterType_t *meterTypes = NULL;
meter_t *meters = NULL;
meterTarif_t *meterTarifs = NULL;

void freeMeters() {
	meterType_t * mt, *mtNext;
	meterRead_t * mr, *mrNext;
	meterRegister_t *mreg, *mregNext;
	meter_t *m, *mNext;
	meterRegisterRead_t *rr, *rrNext;
	meterFormula_t *mf,*mfNext;
	meterInit_t *mi,*miNext;
	int i;

	VPRINTFN(5,"free meter types");
	mt = meterTypes;
	while (mt) {
		mtNext = mt->next;

		VPRINTFN(5," free meter types -> meter reads");
		mr = mt->meterReads;
		while (mr) {
			mrNext = mr->next;
			free(mr);
			mr = mrNext;
		}

		VPRINTFN(5," free meter types -> meter registers");
		mreg = mt->meterRegisters;
		while (mreg) {
			mregNext = mreg->next;
			free(mreg->arrayName);
			free(mreg->name);
			free(mreg->formula);
			free(mreg);
			mreg = mregNext;
		}
		free(mt->name);
		free(mt->influxMeasurement);
		free(mt->mqttprefix);

		mi = mt->init;
		while(mi) {
			miNext = mi->next;
			free(mi->buf);
			free(mi);
			mi=miNext;
		}

		for (i=0;i<TARIF_MAX;i++) {
			mi = mt->setTarif[i];
			while(mi) {
				miNext = mi->next;
				free(mi->buf);
				free(mi);
				mi=miNext;
			}
		}


		free(mt);
		mt = mtNext;
	}
	meterTypes = NULL;

	VPRINTFN(5,"free meters");
	m = meters;
	while (m) {
		mNext = m->next;

		VPRINTFN(5," free meters -> free register reads");
		rr = m->registerRead;
		while (rr) {
			rrNext = rr->next;
			free(rr);
			rr = rrNext;
		}

		mf = m->meterFormula;
		while (mf) {
			mfNext = mf->next;
			free(mf->arrayName);
			free(mf->formula);
			free(mf->name);
			free(mf);
			mf = mfNext;
		}


		free(m->name);
		free(m->hostname);
		free(m->port);
		free(m->influxMeasurement);
		free(m->influxTagName);
		free(m->mqttprefix);
		if (m->sunspecIds) {
			free(m->sunspecIds->ids);
			free(m->sunspecIds);
		}

		free(m);
		m = mNext;
	}
	meters = NULL;
}





typedef struct {
    int t;
    char *s;
    int numRegisters;
} typeinfo_t;


void meterType_addRead(meterType_t *meterType, meterRead_t *meterRead) {
	if (meterType->meterReads) {
		meterRead_t *mr = meterType->meterReads;
		while(mr->next) mr = mr->next;
		mr->next = meterRead;
	} else meterType->meterReads = meterRead;
}


int parserExpectEqual(parser_t * pa, int tkExpected) {
	parserExpect(pa,TK_EQUAL);
	return parserExpect(pa,tkExpected);
}


#define BUF_INITIAL_SIZE 10

void buf16add (uint16_t val,int count,uint16_t **buf,int *numWords,int *bufSize) {
    int i;
    uint16_t *tgt;
    while (*numWords + count > *bufSize) {
        if (*bufSize == 0) {
            *bufSize = BUF_INITIAL_SIZE;
            *numWords = 0;
        } else
            *bufSize = *bufSize * 2;
        *buf = (uint16_t *)realloc(*buf,*bufSize * sizeof(uint16_t));
    }
    tgt = *buf;
    tgt += *numWords;
    for (i=0;i<count;i++) {
        *tgt = val;
        tgt++;
        (*numWords)++;
    }
    tgt = *buf;
    //for (i=0;i<*numWords;i++) { printf("%04x ",*tgt); tgt++; } printf("\n");
}


void checkAllowedChars (parser_t *pa, char *name) {
    char *s = name;
    int len = strlen(name);
    while (*s) {
        if (! strchr(MUPARSER_ALLOWED_CHARS,*s)) {
            pa->col -= len;
            parserError(pa,"invalid char '%c' 0x%02x in '%s'",*s,*s,name);
        }
        s++;
        len--;
    }
}


meter_t *findMeter(char *name) {
    meter_t *meter = meters;

    while (meter) {
        if (strcmp(name,meter->name) == 0)
            if (meter->disabled == 0) return meter;
        meter = meter->next;
    }
    return meter;
}

meterType_t * findMeterType(const char * name) {
    meterType_t *mt = meterTypes;

	while (mt) {
        if (strcmp(mt->name,name) == 0) return mt;
        mt = mt->next;
    }
    return NULL;
}


meterRegisterRead_t *findMeterRegisterRead (meter_t *meter, char * name) {
    meterRegisterRead_t *mrrd = meter->registerRead;

    if (!meter) return NULL;
    while (mrrd) {
        if (strcmp(name,mrrd->registerDef->name) == 0) return mrrd;
        mrrd = mrrd->next;
    }
    return mrrd;
}


int parseMeterInit(parser_t * pa,meterInit_t * meterInit) {
    int tk;
    int bufSize = 0;
	uint16_t val;

    parserExpect(pa,TK_INTVAL);    // start address
    meterInit->startAddr = pa->iVal;
    tk = parserGetToken(pa);
    while (tk == TK_COMMA) {
        parserExpect(pa,TK_INTVAL);
        val = htole16(pa->iVal);
        if (pch(pa) == '*') {           // repeat count
            parserGetToken(pa);
            parserExpect(pa,TK_INTVAL);
            buf16add (val,pa->iVal,&meterInit->buf,&meterInit->numWords,&bufSize);
        } else
            buf16add (val,1,&meterInit->buf,&meterInit->numWords,&bufSize);
        tk = parserGetToken(pa);
    }
    return tk;
}

int parseMeterType (parser_t * pa) {
	int tk;
	meterType_t *meterType;
	meterRead_t *meterRead;
	meterRegister_t *meterRegister;
	char errMsg[100];
	int lastsunspecId = -1;
	meterInit_t * meterInit;
	int tarif;
	int enableInfluxWrite = 1;
	int enableMqttWrite = 1;

	meterType = (meterType_t *)calloc(1,sizeof(meterType_t));
	meterType->mqttprefix = strdup(mqttprefix);
	meterType->influxWriteMult = influxWriteMult;
	parserExpect(pa,TK_EOL);  // after section

	tk = parserGetToken(pa);
	//printf("tk2: %d, %s\n",tk,parserGetTokenTxt(pa,tk));
	while (tk != TK_SECTION && tk != TK_EOF) {
		switch(tk) {
            case TK_INFLUXWRITEMULT:
                parserExpectEqual(pa,TK_INTVAL);
				meterType->influxWriteMult = pa->iVal;
				if (pa->iVal != 0)
                    if (pa->iVal < 2) parserError(pa,"influxwritemult: 0 or >=2 expected");
				break;
			case TK_MEASUREMENT:
				if (meterType->influxMeasurement) parserError(pa,"duplicate measurement");
				parserExpectEqual(pa,TK_STRVAL);
				free(meterType->influxMeasurement);
				meterType->influxMeasurement = strdup(pa->strVal);
				break;
			case TK_MQTTPREFIX:
				parserExpectEqual(pa,TK_STRVAL);
				free(meterType->mqttprefix);
				meterType->mqttprefix=strdup(pa->strVal);
				break;
			case TK_MQTTQOS:
				parserExpectEqual(pa,TK_INTVAL);
				meterType->mqttQOS = pa->iVal;
				break;
			case TK_MQTTRETAIN:
				parserExpectEqual(pa,TK_INTVAL);
				meterType->mqttRetain = pa->iVal;
				break;
			case TK_MQTT:
				parserExpectEqual(pa,TK_INTVAL);
				enableMqttWrite = pa->iVal;
				break;
			case TK_INFLUX:
				parserExpectEqual(pa,TK_INTVAL);
				enableInfluxWrite = pa->iVal;
				break;
			case TK_EOL:
				break;
			case TK_NAME:
				if (meterType->name) parserError(pa,"duplicate meter type name specifications");
				// TODO: Check for multiple
				parserExpectEqual(pa,TK_STRVAL);
				meterType->name = strdup(pa->strVal);
				checkAllowedChars (pa,meterType->name);
				if (findMeterType(meterType->name)) parserError(pa,"multiple definitions for meter type '%s'",meterType->name);
				VPRINTFN(4,"parseMeterType %s",pa->strVal);
				break;
			case TK_SUNSPEC:
				meterType->sunspecBaseAddr = 40000;
				if (pchnext(pa) == '=') {
					parserExpectEqual(pa,TK_INTVAL);
					meterType->sunspecBaseAddr = pa->iVal;
				}
				VPRINTFN(3,"sunspec base addr: %d",meterType->sunspecBaseAddr);
				break;
			case TK_ID:
				parserExpectEqual(pa,TK_INTVAL);
				lastsunspecId = pa->iVal;
				if (lastsunspecId < 0 || lastsunspecId > 0xffff) parserError(pa,"sunspec id (%d) out of range",lastsunspecId);
				break;
			case TK_READ:
				parserExpectEqual(pa,TK_INTVAL);
				meterRead = (meterRead_t *)calloc(1,sizeof(meterRead_t));
				meterRead->startAddr = pa->iVal;
				parserExpect(pa,TK_COMMA);
				meterRead->numRegisters = parserExpectInteger(pa);
				meterRead->sunspecId = lastsunspecId;
				meterType_addRead(meterType,meterRead);
				break;
			case TK_READID:
				parserExpectEqual(pa,TK_INTVAL);
				meterRead = (meterRead_t *)calloc(1,sizeof(meterRead_t));
				lastsunspecId = pa->iVal;
				meterRead->startAddr = 0;
				if (pch(pa)==',') {
					parserExpect(pa,TK_COMMA); meterRead->startAddr = parserExpectInteger(pa);
					parserExpect(pa,TK_COMMA); meterRead->numRegisters = parserExpectInteger(pa);
				}
				meterRead->sunspecId = lastsunspecId;
				meterType_addRead(meterType,meterRead);
				break;
            case TK_INIT:
                parserExpect(pa,TK_EQUAL);
                meterInit = meterType->init;
                while (meterInit) {
                    if (meterInit->next) meterInit = meterInit->next;
                    else {
                        meterInit->next = (meterInit_t *)calloc(1,sizeof(meterInit_t));
                        meterInit = meterInit->next;
                        break;
                    }
                }
                if (!meterInit) {
                    meterInit = (meterInit_t *)calloc(1,sizeof(meterInit_t));
                    meterType->init = meterInit;
                }
                meterInit->sunspecId = lastsunspecId;
                tk = parseMeterInit(pa,meterInit);
                break;
            case TK_SETTARIF:
                parserExpectEqual(pa,TK_INTVAL);
                tarif = pa->iVal;
                if (tarif < 1 || tarif > TARIF_MAX) parserError(pa,"tarif not in range of 1 to %d",TARIF_MAX);
                if (meterType->setTarif[tarif-1]) parserError(pa,"tarif %d already defined",tarif);
                meterInit = (meterInit_t *)calloc(1,sizeof(meterInit_t));
                meterInit->sunspecId = lastsunspecId;
                parserExpect(pa,TK_COMMA);
                tk = parseMeterInit(pa,meterInit);
                meterType->setTarif[tarif-1] = meterInit;
                break;
			case TK_STRVAL:
				meterRegister = meterType->meterRegisters;
				while (meterRegister) {
					if (strcmp(meterRegister->name,pa->strVal) == 0) parserError(pa,"duplicate register name %s",pa->strVal);
					meterRegister = meterRegister->next;
				}
				meterRegister = (meterRegister_t *)calloc(1,sizeof(meterRegister_t));
				meterRegister->enableInfluxWrite = enableInfluxWrite;
                meterRegister->enableMqttWrite = enableMqttWrite;
				meterRegister->type = TK_INT16;
				meterRegister->numRegisters = 1;
				meterRegister->name = strdup(pa->strVal);
				checkAllowedChars (pa,meterRegister->name);
				meterRegister->sunspecId = lastsunspecId;
				parserExpect(pa,TK_EQUAL);
				tk = parserGetToken(pa);
				switch (tk) {
                    case TK_INTVAL:
                        meterRegister->startAddr = pa->iVal;
                        break;
#ifndef DISABLE_FORMULAS
                    case TK_STRVAL:
                        meterRegister->formula = strdup(pa->strVal);
                        meterRegister->isFormulaOnly = 1;
                        break;
#endif
                    default:
#ifndef DISABLE_FORMULAS
                        parserError(pa,"integer (modbus register) or string for formula expected");
#else
                        parserError(pa,"integer expected");
#endif
				}


				// more optional parameters may follow separated by comma, e.g. arr="bla", div=10
				tk = parserGetToken(pa);

				while (tk == TK_COMMA) {
					tk = parserGetToken(pa);

					if (tk >= T_TYPEFIRST && tk <= T_TYPELAST) {
							//printf("tk: %s\n",parserGetTokenTxt(pa,tk));
						meterRegister->type = tk;
						switch (tk) {
						    case TK_INT16:
							case TK_UINT16:
								meterRegister->numRegisters = 1;
								break;
							case TK_FLOAT:
							case TK_FLOAT_ABCD:
							case TK_FLOAT_BADC:
							case TK_FLOAT_CDAB:
							case TK_INT32:
							case TK_INT32L:
							case TK_UINT32:
							case TK_UINT32L:
								meterRegister->numRegisters = 2;
								break;
							case TK_INT48:
							case TK_INT48L:
							case TK_UINT48:
							case TK_UINT48L:
								meterRegister->numRegisters = 3;
								break;
							case TK_INT64:
							case TK_INT64L:
							case TK_UINT64:
							case TK_UINT64L:
								meterRegister->numRegisters = 4;
								break;
							default:
								EPRINTFN("parseMeterType: size for type %d unknown",tk);
								exit(1);
						}
					} else
					if (tk >= TK_REGOPTSFIRST && tk <= TK_REGOPTSLAST) {
						parserExpectEqual(pa,TK_STRVAL);
						switch (tk) {
							case TK_ARRAY:
								meterRegister->arrayName = strdup(pa->strVal);
								break;
                            case TK_FORMULA:
#ifndef DISABLE_FORMULAS
                                if (meterRegister->formula) parserError(pa,"parseMeterType: formula already specified");
                                meterRegister->formula = strdup(pa->strVal);
#else
                                EPRINTFN("Warning: compiled without muparser, formulas will be ignored");
#endif
                                break;
							default:
								parserError(pa,"parseMeterType: opt with str %d (%s) not yet supported",tk,pa->strVal);
						}
					} else
					if (tk >= TK_REGOPTIFIRST && tk <= TK_REGOPTILAST) {
						parserExpectEqual(pa,TK_INTVAL);
						switch (tk) {
						    case TK_SF:
                                meterRegister->sunspecSfRegister = pa->iVal;
                                break;
							case TK_DEC:
								meterRegister->decimals = pa->iVal;
								//printf("%s %s %d\n",meterType->name,meterRegister->name,meterRegister->decimals);
								break;
							case TK_DIV:
								meterRegister->divider = pa->iVal;
								break;
							case TK_MUL:
								meterRegister->multiplier = pa->iVal;
								break;
                            case TK_MQTT:
                                meterRegister->enableMqttWrite = pa->iVal;
                                break;
                            case TK_INFLUX:
                                meterRegister->enableInfluxWrite = pa->iVal;
                                break;
							default:
								parserError(pa,"parseMeterType: opt with int %d (%s) not yet supported",tk,pa->strVal);
						}
					} else
					if (tk == TK_FORCE) {
						parserExpect(pa,TK_EQUAL);
						tk = parserGetToken(pa);
						if (tk != TK_INT16 && tk != TK_FLOAT) parserError(pa,"identifier (int or float) expected");
						if (tk == TK_INT16) meterRegister->forceType = force_int;
						else meterRegister->forceType = force_float;
					} else {
                        switch (tk) {
                            case TK_IMAX:
                                if (meterRegister->influxMultProcessing != pr_last) parserError(pa,"imax,imin or iavg already specified");
                                meterRegister->influxMultProcessing = pr_max;
                                break;
                            case TK_IMIN:
                                if (meterRegister->influxMultProcessing != pr_last) parserError(pa,"imax,imin or iavg already specified");
                                meterRegister->influxMultProcessing = pr_min;
                                break;
                            case TK_IAVG:
                                if (meterRegister->influxMultProcessing != pr_last) parserError(pa,"imax,imin or iavg already specified");
                                meterRegister->influxMultProcessing = pr_avg;
                                break;
                            default:
                                parserError(pa,"identifier for option expected");
                        }

                    }

					tk = parserExpectOrEOL(pa,TK_COMMA);
					//printf("tknext: %s\n",parserGetTokenTxt(pa,tk));
				}
				// add the meter register to the meter type
				if (meterType->meterRegisters) {
					meterRegister_t * mr = meterType->meterRegisters;
					while (mr->next) mr=mr->next;
					mr->next = meterRegister;
				} else meterType->meterRegisters = meterRegister;

				// update enabled register count
				if (meterRegister->enableInfluxWrite) meterType->numEnabledRegisters_influx++;
				if (meterRegister->enableMqttWrite) meterType->numEnabledRegisters_mqtt++;
				break;

			default:
				snprintf(errMsg,sizeof(errMsg),"unexpected input, expected identifier or string but got %s",parserGetTokenTxt(pa,tk));
				parserError(pa,errMsg);
		}
		//printf("tend: %s\n",parserGetTokenTxt(pa,tk));
		if (tk != TK_EOL) {
			tk = parserGetToken(pa);
			if (tk != TK_EOL) parserError(pa,"EOL or , expected");
		}
		tk = parserGetToken(pa);
	}
	if (!meterType->name) parserError(pa,"meter type definition without name");
	if (!meterType->meterRegisters) parserError(pa,"\"%s\": meter type definition without registers",meterType->name);

	if (!meterType->meterReads) fprintf(stderr,"\"%s\": Warning: no meter reads, form a performance point of view, meter reads should be defined to avoid single register reads\n",meterType->name);

	// store meter type
	if (meterTypes) {
		meterType_t * mt = meterTypes;
		while (mt->next) mt = mt->next;
		mt->next = meterType;
	} else meterTypes = meterType;

	return tk;
}


int parseMeter (parser_t * pa) {
	int tk;
	meter_t *meter;
	char errMsg[255];
	meterFormula_t * meterFormula;  // calculated meter specific registers
	int typeDefined = 0;
	int typeConflict = 0;

	meter = (meter_t *)calloc(1,sizeof(meter_t));
	parserExpect(pa,TK_EOL);  // after section
	meter->influxWriteMult = influxWriteMult;
	meter->modbusDebug = modbusDebug;

	tk = parserGetToken(pa);
	//printf("tk2: %d, %s\n",tk,parserGetTokenTxt(pa,tk));
	while (tk != TK_SECTION && tk != TK_EOF) {
		switch(tk) {
	    case TK_PORT:
                parserExpectEqual(pa,TK_STRVAL);
		meter->port=strdup(pa->strVal);
            case TK_MODBUSDEBUG:
                parserExpectEqual(pa,TK_INTVAL);
				meter->modbusDebug = pa->iVal;
				break;
            case TK_INFLUXWRITEMULT:
                //if (! typeDefined) parserError(pa,"has to be defined after type=");
                typeConflict++;
                parserExpectEqual(pa,TK_INTVAL);
				meter->influxWriteMult = pa->iVal;
				if (pa->iVal != 0)
                    if (pa->iVal < 2) parserError(pa,"influxwritemult: 0 or >=2 expected");
				break;
			case TK_MQTTPREFIX:
				//if (! typeDefined) parserError(pa,"has to be defined after type=");
				typeConflict++;
				parserExpectEqual(pa,TK_STRVAL);
				meter->mqttprefix=strdup(pa->strVal);
				break;
			case TK_MQTTQOS:
				//if (! typeDefined) parserError(pa,"has to be defined after type=");
				typeConflict++;
				parserExpectEqual(pa,TK_INTVAL);
				meter->mqttQOS = pa->iVal;
				break;
			case TK_MQTTRETAIN:
				//if (! typeDefined) parserError(pa,"has to be defined after type=");
				typeConflict++;
				parserExpectEqual(pa,TK_INTVAL);
				meter->mqttRetain = pa->iVal;
				break;
			case TK_EOL:
				break;
            case TK_DISABLE:
                parserExpectEqual(pa,TK_INTVAL);
                meter->disabled = pa->iVal;
                break;
			case TK_TYPE:
                if (typeConflict) parserError(pa,"type= needs to be specified before options that could be defined in type as well (e.g. influxwritemult, mqttprefix or measurement");
				if (meter->meterType) parserError(pa,"%s: duplicate meter type",meter->name);
				parserExpectEqual(pa,TK_STRVAL);
				meter->meterType = findMeterType(pa->strVal);
				if (!meter->meterType) parserError(pa,"undefined meter type ('%s')",pa->strVal);
				meter->numEnabledRegisters_mqtt = meter->meterType->numEnabledRegisters_mqtt;
				meter->numEnabledRegisters_influx = meter->meterType->numEnabledRegisters_influx;
				if (meter->meterType->mqttprefix) {
					free(meter->mqttprefix);
					meter->mqttprefix = strdup(meter->meterType->mqttprefix);
				}
				meter->mqttQOS = meter->meterType->mqttQOS;
				meter->mqttRetain = meter->meterType->mqttRetain;
				meter->influxWriteMult = meter->meterType->influxWriteMult;
				if (meter->meterType->influxMeasurement) {
					free(meter->influxMeasurement);
					meter->influxMeasurement = strdup(meter->meterType->influxMeasurement);
				}
				typeDefined++;
				break;
			case TK_NAME:
				if (meter->name) parserError(pa,"%s: duplicate meter name in meter definition",meter->name);
				parserExpectEqual(pa,TK_STRVAL);
				meter->name = strdup(pa->strVal);
				if (findMeter(meter->name))
					parserError(pa,"a meter with the name \"%s\" is already defined",pa->strVal);
				checkAllowedChars (pa,meter->name);
				VPRINTFN(4,"parseMeter %s",pa->strVal);
				break;
			case TK_HOSTNAME:
				if (meter->hostname) parserError(pa,"duplicate hostname");
				parserExpectEqual(pa,TK_STRVAL);
				meter->hostname = strdup(pa->strVal);
				meter->isTCP = 1;
				break;
			case TK_MEASUREMENT:
				typeConflict++;
				parserExpectEqual(pa,TK_STRVAL);
				free(meter->influxMeasurement);
				meter->influxMeasurement = strdup(pa->strVal);
				break;
			case TK_ADDRESS:
				parserExpectEqual(pa,TK_INTVAL);
				meter->modbusAddress = pa->iVal;
				break;
            case TK_STRVAL:
				meterFormula = meter->meterFormula;
				while (meterFormula) {
					if (strcmp(meterFormula->name,pa->strVal) == 0) parserError(pa,"duplicate formula register name %s",pa->strVal);
					meterFormula = meterFormula->next;
				}
				meterFormula = (meterFormula_t *)calloc(1,sizeof(meterFormula_t));
				meterFormula->enableInfluxWrite = 1;
                meterFormula->enableMqttWrite = 1;

				meterFormula->name = strdup(pa->strVal);
				checkAllowedChars (pa,meterFormula->name);
				parserExpect(pa,TK_EQUAL);
				tk = parserGetToken(pa);
				switch (tk) {
                    case TK_STRVAL:
                        meterFormula->formula = strdup(pa->strVal);
                        break;

                    default:
                        parserError(pa,"string for formula expected");
				}

				// more optional parameters may follow separated by comma, e.g. arr="bla", dec=2
				tk = parserGetToken(pa);

				while (tk == TK_COMMA) {
					tk = parserGetToken(pa);

					if (tk >= TK_REGOPTSFIRST && tk <= TK_REGOPTSLAST) {
						parserExpectEqual(pa,TK_STRVAL);
						switch (tk) {
							case TK_ARRAY:
								meterFormula->arrayName = strdup(pa->strVal);
								break;
							default:
								parserError(pa,"parseMeter: opt with str %d (%s) not yet supported",tk,pa->strVal);
						}
					} else
					if (tk >= TK_REGOPTIFIRST && tk <= TK_REGOPTILAST) {
						parserExpectEqual(pa,TK_INTVAL);
						switch (tk) {
							case TK_DEC:
								meterFormula->decimals = pa->iVal;
								break;
                            case TK_MQTT:
                                meterFormula->enableMqttWrite = pa->iVal;
                                break;
                            case TK_INFLUX:
                                meterFormula->enableInfluxWrite = pa->iVal;
                                break;
							default:
								parserError(pa,"parseMeter: opt with int %d (%s) not yet supported",tk,pa->strVal);
						}
					} else
					if (tk == TK_FORCE) {
						parserExpect(pa,TK_EQUAL);
						tk = parserGetToken(pa);
						if (tk != TK_INT16 && tk != TK_FLOAT) parserError(pa,"identifier (int or float) expected");
						if (tk == TK_INT16) meterFormula->forceType = force_int;
						else meterFormula->forceType = force_float;
					} else {
                        switch (tk) {
                            case TK_IMAX:
                                if (meterFormula->influxMultProcessing != pr_last) parserError(pa,"imax,imin or iavg already specified");
                                meterFormula->influxMultProcessing = pr_max;
                                break;
                            case TK_IMIN:
                                if (meterFormula->influxMultProcessing != pr_last) parserError(pa,"imax,imin or iavg already specified");
                                meterFormula->influxMultProcessing = pr_min;
                                break;
                            case TK_IAVG:
                                if (meterFormula->influxMultProcessing != pr_last) parserError(pa,"imax,imin or iavg already specified");
                                meterFormula->influxMultProcessing = pr_avg;
                                break;
                            default: parserError(pa,"identifier for option expected");
                        }
					}

					tk = parserExpectOrEOL(pa,TK_COMMA);
				}
				// add the formula to the meter
				if (meter->meterFormula) {
					meterFormula_t * mf = meter->meterFormula;
					while (mf->next) mf=mf->next;
					mf->next = meterFormula;
				} else meter->meterFormula = meterFormula;

				// update enabled register count in meter
				if (meterFormula->enableInfluxWrite) meter->numEnabledRegisters_influx++;
				if (meterFormula->enableMqttWrite) meter->numEnabledRegisters_mqtt++;

				break;


			default:
				strncpy(errMsg,"unexpected identifier ",sizeof(errMsg)-1);
				if (tk != TK_IDENT) strncat(errMsg,parserGetTokenTxt(pa,tk),sizeof(errMsg)-1);
				else strncat(errMsg,pa->strVal,sizeof(errMsg)-1);
				parserError (pa,errMsg);
		}
		if (tk != TK_EOL) {
			tk = parserGetToken(pa);
			if (tk != TK_EOL) parserError(pa,"EOL or , expected");
		}
		tk = parserGetToken(pa);
	}

	if (meter->meterType)
		if (meter->mqttprefix == NULL)
			if (meter->meterType->mqttprefix) meter->mqttprefix = strdup(meter->meterType->mqttprefix);
	if (meter->mqttprefix == NULL)
		if (mqttprefix) meter->mqttprefix = strdup(mqttprefix);

	if (!meter->name) parserError(pa,"Meter name not specified");
	if (!meter->meterType)
        if (!meter->meterFormula) parserError(pa,"%s: No meter formula registers and no meter type specified, either one or both need to be specified",meter->name);
	if (meter->modbusAddress <1 && meter->meterType) parserError(pa,"%s: No modbus address specified",meter->name);

	// add meter
	if (meters) {
		meter_t * mt = meters;
		while (mt->next) mt = mt->next;
		mt->next = meter;
	} else meters = meter;

	// copy the registers to read from the meter type to the meter

	if (meter->meterType) {		// for a virtual meter with formulas only
		meterRegister_t *registerDef = meter->meterType->meterRegisters;
		meterRegisterRead_t * mrrd = meter->registerRead;
		while (registerDef) {
			if (!mrrd) {
				meter->registerRead = (meterRegisterRead_t *)calloc(1,sizeof(meterRegisterRead_t));
				mrrd = meter->registerRead;
			} else {
				mrrd->next = (meterRegisterRead_t *)calloc(1,sizeof(meterRegisterRead_t));
				mrrd = mrrd->next;
			}
			mrrd->registerDef = registerDef;
			mrrd->startAddr = registerDef->startAddr;
			mrrd->sunspecId = registerDef->sunspecId;
			if (registerDef->decimals > 0 || registerDef->forceType == force_float)
				mrrd->isInt = 0;
			if (mrrd->sunspecId > 0) meter->needSunspecResolve = 1;
			registerDef = registerDef->next;
		}

		if (! meter->needSunspecResolve) {
			meterRead_t * mr = meter->meterType->meterReads;
			while (mr && ! meter->needSunspecResolve) {
				if (mr->sunspecId > 0) meter->needSunspecResolve = 1;
				mr = mr->next;
			}
		}
	}
	if (meter->influxWriteMult) meter->influxWriteCountdown = -1; // meter->influxWriteMult;

	return tk;
}


int parseTarif (parser_t * pa) {
	int tk;
	meterTarif_t *meterTarif = NULL;
	char errMsg[255];
	int i;
	meterList_t *ml,*ml2;

	parserExpect(pa,TK_EOL);  // after section

	tk = parserGetToken(pa);
	//printf("tk2: %d, %s\n",tk,parserGetTokenTxt(pa,tk));
	while (tk != TK_SECTION && tk != TK_EOF) {
		switch(tk) {
			case TK_EOL:
				break;
            case TK_METERS:
                parserExpect(pa,TK_EQUAL);
                if (!meterTarif) parserError(pa,"meters= has to be specified after tarif=");
                if (meterTarif->meters) parserError(pa,"duplicate meters=");
                do {
                    parserExpect(pa,TK_STRVAL);       // meter name

                    ml = (meterList_t *)calloc(1,sizeof(meterList_t));
                    ml->meter = findMeter(pa->strVal);
                    if (!ml->meter) parserError(pa,"unknown meter name \"%s\"",pa->strVal);
                    //printf("Meters %s %p\n",pa->strVal,ml->meter->meterType->setTarif);
                    if (!meterTarif->meters) meterTarif->meters = ml;
                    else {
                        ml2 = meterTarif->meters;
                        while(ml2->next) ml2=ml2->next;
                        ml2->next = ml;
                    }
                    //if (!ml->meter->meterType->setTarif[0]) parserError(pa,"meter \"%s\" does not support setting tarif",pa->strVal);
                    tk = parserGetToken(pa);
                } while (tk == TK_COMMA);

                break;
            case TK_TARIF:
                if (!meterTarifs) {
                    meterTarifs = (meterTarif_t *)calloc(1,sizeof(meterTarif_t));
                    meterTarif = meterTarifs;
                } else {
                    meterTarif = meterTarifs;
                    while (meterTarif->next) meterTarif = meterTarif->next;
                    meterTarif->next = (meterTarif_t *)calloc(1,sizeof(meterTarif_t));
                    meterTarif = meterTarif->next;
                }
                parserExpectEqual(pa,TK_STRVAL);       // meter name
                meterTarif->meter = findMeter(pa->strVal);
                if (!meterTarif->meter) parserError(pa,"Unknown meter \"%s\"",pa->strVal);
                parserExpect(pa,TK_COMMA);
                parserExpect(pa,TK_STRVAL);
                meterTarif->meterRegisterRead = findMeterRegisterRead(meterTarif->meter,pa->strVal);
                if (!meterTarif->meterRegisterRead) parserError(pa,"tarif for %s, unknown register name \"%s\"",meterTarif->meter->name,pa->strVal);
                parserExpect(pa,TK_COMMA);


                meterTarif->isInt = 1;
                for (i=0;i<TARIF_MAX;i++) {
                    tk = parserGetToken (pa);
                    if (tk != TK_INTVAL && tk != TK_FLOATVAL) parserError(pa,"Integer or Float value expected");
                    if (tk == TK_FLOATVAL) {
                        meterTarif->isInt = 0;
                        meterTarif->fValues[i] = pa->fVal;
                        meterTarif->iValues[i] = (int)pa->fVal;
                    } else {
                        meterTarif->fValues[i] = pa->iVal;
                        meterTarif->iValues[i] = pa->iVal;
                    }
                    if (pch(pa) == ',') {
                        parserExpect(pa,TK_COMMA);
                    } else
                        break;
                }
                break;
            default:
				strncpy(errMsg,"unexpected identifier ",sizeof(errMsg)-1);
				if (tk != TK_IDENT) strncat(errMsg,parserGetTokenTxt(pa,tk),sizeof(errMsg)-1);
				else strncat(errMsg,pa->strVal,sizeof(errMsg)-1);
				parserError (pa,errMsg);
		}
		if (tk != TK_EOL) {
			tk = parserGetToken(pa);
			if (tk != TK_EOL) parserError(pa,"EOL or , expected");
		}
		tk = parserGetToken(pa);
	}

    return tk;
}


int readMeterDefinitions (const char * configFileName) {
	meter_t *meter;
	int rc;
	int tk;
	parser_t *pa;

	pa = parserInit(CHAR_TOKENS,
		"floatabcd"       ,TK_FLOAT_ABCD,
		"floatbadc"       ,TK_FLOAT_BADC,
		"float"           ,TK_FLOAT,
		"floatdcba"       ,TK_FLOAT_CDAB,
		"int"             ,TK_INT16,
		"int16"           ,TK_INT16,
		"int32"           ,TK_INT32,
		"int48"           ,TK_INT48,
		"int64"           ,TK_INT64,
		"uint"            ,TK_UINT16,
		"uint16"          ,TK_UINT16,
		"uint32"          ,TK_UINT32,
		"uint48"          ,TK_UINT48,
		"uint64"          ,TK_UINT64,

		"int32l"          ,TK_INT32L,
		"int48l"          ,TK_INT48L,
		"int64l"          ,TK_INT64L,
		"uint32l"         ,TK_UINT32L,
		"uint48l"         ,TK_UINT48L,
		"uint64l"         ,TK_UINT64L,

		"dec"             ,TK_DEC,
		"div"             ,TK_DIV,
		"mul"             ,TK_MUL,
		"arr"             ,TK_ARRAY,
		"array"           ,TK_ARRAY,
		"force"           ,TK_FORCE,
		"name"            ,TK_NAME,
		"read"            ,TK_READ,
		"readid"          ,TK_READID,
		"type"            ,TK_TYPE,
		"address"         ,TK_ADDRESS,
		"hostname"        ,TK_HOSTNAME,
		"measurement"     ,TK_MEASUREMENT,
		"port"            ,TK_PORT,
		"sunspec"         ,TK_SUNSPEC,
		"id"              ,TK_ID,
		"sf"              ,TK_SF,
		"formula"         ,TK_FORMULA,
		"init"            ,TK_INIT,
		"settarif"        ,TK_SETTARIF,
		"tarif"           ,TK_TARIF,
		"meters"          ,TK_METERS,
		"disabled"        ,TK_DISABLE,
		"mqtt"            ,TK_MQTT,
        "influx"          ,TK_INFLUX,
        "mqttqos"	      ,TK_MQTTQOS,
		"mqttretain"      ,TK_MQTTRETAIN,
		"mqttprefix"      ,TK_MQTTPREFIX,
		"influxwritemult" ,TK_INFLUXWRITEMULT,
		"imax"            ,TK_IMAX,
		"imin"            ,TK_IMIN,
		"iavg"            ,TK_IAVG,
		"modbusdebug"     ,TK_MODBUSDEBUG,
		NULL);
	rc = parserBegin (pa, configFileName, 1);
	if (rc != 0) {
		fprintf(stderr,"parserBegin (\"%s\")failed\n",configFileName);
		exit(1);
	}

	tk = parserExpectSection(pa);
	while (tk != TK_EOF) {
		if (strcasecmp(pa->strVal,"MeterType") == 0)
			tk = parseMeterType(pa);
		else if (strcasecmp(pa->strVal,"Meter") == 0)
			tk = parseMeter(pa);
        else if (strcasecmp(pa->strVal,"Tarifs") == 0)
			tk = parseTarif(pa);
		else
			parserError(pa,"unknown section type %s",pa->strVal);
	}
	parserFree(pa);

	meter = meters;
	if (!meter) {
		EPRINTFN("no meters defined in %s",configFileName);
		exit(1);
	}

	// set the modbus RTU handle in the meter or open an IP connection to the meter
	while (meter) {
		if (meter->isTCP) {
			//meter->mb = modbus_new_tcp_pi(meter->hostname, const char *service);
			//do the open when querying the meter to be able to retry of temporary not available
		} else {	// modbus RTU
			if (meter->modbusAddress > 0) {
				meter->mb = modbusRTU_getmh();
				if (*meter->mb == NULL && meter->disabled == 0) {
					EPRINTFN("%s: serial modbus not yet opened or no serial device specified",meter->name);
					exit(1);
				}
			}
		}
		meter = meter->next;
	}

	return 0;

}
