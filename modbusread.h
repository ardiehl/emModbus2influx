#ifndef MODBUSREAD_H_INCLUDED
#define MODBUSREAD_H_INCLUDED


#include "meterDef.h"
#include <stdint.h>

// number of queries
#define TCP_OPEN_RETRY_DELAY 30

int msleep(long msec);

// multiple devices can be separated by ,
// first device is number 0 and is the default device
int meterSerialScanDevice (char * deviceString);
int meterSerialScanBaudrate (char * baudString);
int meterSerialScanParity (char * parityString);
int meterSerialScanrs485 (char * rs485String);
int meterSerialScanStopbits (char * stopString);
int meterSerialOpen ();
void meterSerialClose ();
int meterSerialGetNumDevices();

modbus_t ** modbusRTU_getmh(int serialPortNum);
int modbusRTU_getBaudrate(int serialPortNum);
// delay between queries, the Modbus RTU standard describes a silent period corresponding to 3.5 characters between each message
void modbusRTU_SilentDelay(int baudrate);

void modbusTCP_freeAll();

void setMeterFvalueInfluxLast (meter_t *meter);
void setMeterFvalueInflux (meter_t * meter);

int executeMeterTypeFormulas(int verboseMsg, meter_t *meter);
void executeMeterFormulas(meter_t * meter);
void executeInfluxWriteCalc (int verboseMsg, meter_t *meter);

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

void modbusread_free();

#endif // MODBUSREAD_H_INCLUDED

