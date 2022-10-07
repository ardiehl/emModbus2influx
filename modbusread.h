#ifndef MODBUSREAD_H_INCLUDED
#define MODBUSREAD_H_INCLUDED


#include "meterDef.h"
#include <stdint.h>

// number of queries
#define TCP_OPEN_RETRY_DELAY 30

int msleep(long msec);

modbus_t ** modbusRTU_getmh();
int modbusRTU_open (const char *device, int baud, const char *parity, int stop_bit, int mode_rs485);
void modbusRTU_close();

void modbusTCP_freeAll();

int queryMeter(int verboseMsg, meter_t *meter);
int queryMeters(int verboseMsg);


/**
  testRTUpresent, try to find the first serial connected meter by reading either the first
  defined register or the sunspec header
 @return 0=not found, 1=found
 */
int testRTUpresent();

meter_t * findMeter(char * name);
void testRegCalcFormula(char * meterName);

#ifndef DISABLE_FORMULAS
void freeFormulaParser();
#endif // DISABLE_FORMULAS

void setTarif (int verboseMsg);

#endif // MODBUSREAD_H_INCLUDED

