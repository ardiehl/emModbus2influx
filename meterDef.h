#ifndef METERDEF_H_INCLUDED
#define METERDEF_H_INCLUDED

#include <modbus.h>
#include "parser.h"

#define MUPARSER_ALLOWED_CHARS "0123456789_abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ."


// !! keep in sync with parserInit

#define T_TYPEFIRST   300
#define TK_FLOAT      300
#define TK_FLOAT_ABCD 301
#define TK_FLOAT_BADC 302
#define TK_FLOAT_CDAB 303
#define TK_INT16      304
#define TK_INT32      305
#define TK_INT48      306
#define TK_INT64      307
#define TK_UINT16     308
#define TK_UINT32     309
#define TK_UINT48     310
#define TK_UINT64     311

// LSW first
#define TK_INT32L     312
#define TK_INT48L     313
#define TK_INT64L     314
#define TK_UINT32L    315
#define TK_UINT48L    316
#define TK_UINT64L    317

#define T_TYPELAST    317
#define TK_UNKNOWN    0xffffff

// integer arg
#define TK_REGOPTIFIRST    500
#define TK_DEC             500
#define TK_DIV             501
#define TK_MUL             502
#define TK_SF              503
#define TK_MQTT            504
#define TK_INFLUX          505
#define TK_GRAFANA         506
#define TK_REGOPTILAST     549


// string arg
#define TK_REGOPTSFIRST 550
#define TK_ARRAY        550
#define TK_FORMULA      551
#define TK_MQTTPREFIX   552

#define TK_REGOPTSLAST  599


#define TK_NAME            600
#define TK_READ            601
#define TK_TYPE            602
#define TK_ADDRESS         603
#define TK_HOSTNAME        604
#define TK_FORCE           605
#define TK_MEASUREMENT     606
#define TK_PORT            607
#define TK_SUNSPEC         608
#define TK_READID          609
#define TK_ID              610
#define TK_INIT            611
#define TK_SETTARIF        612
#define TK_TARIF           613
#define TK_METERS          614
#define TK_DISABLE         615
#define TK_MQTTQOS		   616
#define TK_MQTTRETAIN      617
#define TK_MQTTDELAYMS     618
#define TK_INFLUXWRITEMULT 619
#define TK_IMAX            620
#define TK_IMIN            621
#define TK_IAVG            622
#define TK_MODBUSDEBUG     623
#define TK_SERIAL          624
#define TK_DEFAULT         625
#define TK_SCHEDULE        626
#define TK_INAME           627
#define TK_INPUT           628
#define TK_HOLDING         629
#define TK_COIL            630
#define TK_INPUTSTATUS     631
#define TK_GNAME           632
#define TK_METER           633
#define TK_WRITE           634
#define TK_MQTTFMT 	       635
#define TK_READWRITE       636
#define TK_COND            637
#define TK_RETURN          638
#define TK_QUERYDELAY      639

//#define CHAR_TOKENS ",;()={}+-*/&%$"

#define CHAR_TOKENS ",;()={}*/&%$"

#define TK_COMMA      1
#define TK_SEMICOLON  2
#define TK_BR_OPEN    3
#define TK_BR_CLOSE   4
#define TK_EQUAL      5


#define TARIF_MAX     4


typedef struct sunspecId_t sunspecId_t;
struct sunspecId_t {
	uint16_t id;
	uint16_t offset;
	uint16_t blockLength;
};

typedef struct sunspecIds_t sunspecIds_t;
struct sunspecIds_t {
	uint16_t max;
	uint16_t cnt;
	sunspecId_t * ids;
};

typedef enum  {regTypeHolding = 0, regTypeInput, regTypeRegister, regTypeCoil, regTypeInputStatus} regType_t;
typedef enum  {mqttFormatStd = 0, mqttFormatLogoArr, mqttFormatLast} mqttFormat_t;
#define regTypeSunspec regTypeHolding

typedef struct meterRead_t meterRead_t;
struct meterRead_t {
	int startAddr;
	int numRegisters;
	int sunspecId;
	regType_t regType;		// Holding or Input register
	meterRead_t *next;
};

typedef struct meterInit_t meterInit_t;
struct meterInit_t {
    int numWords;
    int startAddr;
    int sunspecId;
    uint16_t *buf;
    meterInit_t *next;
};

typedef enum  {force_none = 0, force_int, force_float} typeForce_t;


// used when influxdb data will be written every x queries
typedef enum  {pr_last = 0, pr_max, pr_min, pr_avg} influxMultProcessing_t;

typedef struct meterRegister_t meterRegister_t;
struct meterRegister_t {
	char *name;
	int isFormulaOnly;  // 1 when no modbus read will be performed
	char *formula;      // either for modifying a value read via modbus or for calculating a "virtual" register based on values of other registers
	regType_t regType;		// Holding, Input register, Coil, ...
	int startAddr;
	int numRegisters;
	int type;				// e.g. TK_INT16
	int divider;
	int multiplier;
	meterRegister_t *next;
	typeForce_t forceType;
	char * arrayName;
	int sunspecId;
	int sunspecSfRegister;  // scaling factor register
	int decimals;
	int enableInfluxWrite;
	int enableMqttWrite;
	int enableGrafanaWrite;
	influxMultProcessing_t influxMultProcessing;
};



typedef struct meterType_t meterType_t;
struct meterType_t {
	char *name;
	int isFormulaOnly;  // 1 when no modbus read will be performed
	meterRead_t *meterReads;
	meterRegister_t *meterRegisters;
	int numEnabledRegisters_mqtt;
	int numEnabledRegisters_influx;
	int numEnabledRegisters_grafana;

	uint16_t sunspecBaseAddr;
	meterInit_t * init;
	meterInit_t * setTarif[TARIF_MAX];
	int mqttQOS;
	int mqttRetain;
	int mqttDelayMs;
	mqttFormat_t mqttFormat;
	meterType_t *next;
	char *mqttprefix;
	char * influxMeasurement;
	int influxWriteMult;
	int modbusQueryDelayMs;
};


extern meterType_t *meterTypes;


typedef struct meterRegisterRead_t meterRegisterRead_t;
struct meterRegisterRead_t {
	meterRegister_t *registerDef;
	int startAddr;		// for sunspec
	int sunspecId;
	int sunspecOffset;
	double fvalueInflux;
	double fvalue;		// now always float, will be saved as int to influx/mqtt if defined as integer
	double fvalueInfluxLast;
	int isInt;
	int hasBeenRead;
	int sunspecSfRegister;
	int sunspecSf;		// sunspec scaling Factor
	meterRegisterRead_t *next;
};


typedef struct meterFormula_t meterFormula_t;
struct meterFormula_t {
    char * name;
    char * formula;
    typeForce_t forceType;
	char * arrayName;
	int decimals;
	int enableInfluxWrite;
	int enableMqttWrite;
	int enableGrafanaWrite;
    double fvalue;
    double fvalueInflux;
    double fvalueInfluxLast;
    influxMultProcessing_t influxMultProcessing;
    meterFormula_t *next;
};


typedef struct meter_t meter_t;
struct meter_t {
    int disabled;
	int writeDisabled;
	meterRegisterRead_t *registerRead;
	meterType_t *meterType;
	int isFormulaOnly;
	int modbusAddress;
	char *name;
	char *iname;
	char *gname;
	int meterHasBeenRead;
	int hasSchedule;
	int isDue;
	modbus_t **mb;	// pointer to a pointer to global RTU handle or a global one for a TCP connection (multiple meters may use the same IP connection)
	int isTCP;
	int isSerial;
	char *hostname;
	char *port;
	int serialPortNum;
	int baudrate;
	char * influxMeasurement;
	char * influxTagName;
	int needSunspecResolve;
	sunspecIds_t * sunspecIds;
	int initDone;
	meterFormula_t * meterFormula;
	int numEnabledRegisters_mqtt;
	int numEnabledRegisters_influx;
	int numEnabledRegisters_grafana;
	int mqttQOS;
	int mqttRetain;
	mqttFormat_t mqttFormat;
	int mqttDelayMs;
	char *mqttLastSend;
	char *mqttprefix;
	meter_t *next;
	int influxWriteMult;
	int influxWriteCountdown;
	int modbusDebug;
	unsigned int queryTimeNano;
	unsigned int queryTimeNanoMin;
	unsigned int queryTimeNanoMax;
	unsigned int queryTimeNanoAvg;
	unsigned int queryTimeNanoInitial;
	unsigned int numQueries;	// including errs
	unsigned int numErrs;
	unsigned int numInfluxWrites;
	unsigned int numMqttWrites;
	unsigned int numGrafanaWrites;
};


typedef struct meterList_t meterList_t;
struct meterList_t {
    meter_t *meter;
    meterList_t *next;
};

typedef struct meterWrite_t meterWrite_t;
struct meterWrite_t {
    meterRegister_t * reg;
    double value;
    char * formula;
    meterWrite_t *next;
    char *conditionFormula;
    int returnOnWrite;
};


typedef struct meterWrites_t meterWrites_t;
struct meterWrites_t {
	int disabled;
	char *name;
	int isDue;
    meter_t * meter;
    meterWrite_t *meterWrite;
    meterWrites_t *next;
    int hasSchedule;
    char *conditionFormula;
};


typedef struct meterTarif_t meterTarif_t;
struct meterTarif_t {
    meter_t * meter;
    meterRegisterRead_t * meterRegisterRead;
    int iValues[TARIF_MAX-1];       // values for tarifs 2,3 and 4
    float fValues[TARIF_MAX-1];
    int isInt;
    int tarifCurr;
    meterList_t *meters;
    meterTarif_t *next;
};

extern meterTarif_t *meterTarifs;
extern meter_t *meters;
extern meterWrites_t *meterWrites;

int readMeterDefinitions (const char * configFileName);
void freeMeters();

meter_t *findMeter(char *name);

int parserExpectEqual(parser_t * pa, int tkExpected);

void meterWrites_add(meterWrites_t *mw);
void meterWrite_add(meterWrites_t *mw, meterWrite_t *m);

#endif // METERDEF_H_INCLUDED
