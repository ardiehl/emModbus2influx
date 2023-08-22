#include <pthread.h>
#include <semaphore.h>
#include "cron.h"
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "assert.h"
#include "global.h"
#include <unistd.h>

int disableThreadedQuery;

cronDef_t *cronTab;

cronDef_t * cron_find(const char * name) {
	cronDef_t * cd = cronTab;

	while (cd) {
		if (cd->name == name) return cd;
		if (name)
			if (cd->name)
				if (strcmp(name,cd->name) == 0) return cd;
		cd = cd->next;
	}
	return NULL;
}


void cron_add(const char *name, const char * cronExpression) {		// name=NULL for default
	cronDef_t * ct,* ct1;
	const char *errStr;

	ct = cron_find(name);
	if (ct) {
		if (name != NULL) {
			EPRINTFN("multiple definitions for cron entry \"%s\"",name);
			exit(1);
		}
		EPRINTFN("Warning: Overwriting previously defined default cron entry (%s) with (%s)",ct->cronExpression,cronExpression);
		free(ct->cronExpression);
		ct->cronExpression = strdup(cronExpression);
		memset(&ct->cronExpr,0,sizeof(ct->cronExpr));
		cron_parse_expr(cronExpression, &ct->cronExpr, &errStr);
		if (errStr) {
			EPRINTFN("Error in cron expression: \"%s\"",cronExpression);
			EPRINTFN(errStr);
			exit (1);
		}
		return;
	}

	ct = (cronDef_t *) calloc(1,sizeof(cronDef_t));
	ct->cronExpression = strdup(cronExpression);
	if (name) ct->name = strdup(name);
	cron_parse_expr(cronExpression, &ct->cronExpr, &errStr);
	if (errStr) {
		EPRINTFN("Error in cron expression: \"%s\"",cronExpression);
		EPRINTFN(errStr);
		exit (1);
	}
	if (! cronTab) {
		cronTab = ct;
		return;
	}
	ct1 = cronTab;
	while (ct1->next) ct1=ct1->next;
	ct1->next = ct;
}


int  cron_is_due(time_t currTime, cronDef_t *cronDef) {
	if (currTime >= cronDef->nextQueryTime) return 1;
	return 0;
}

//void cron_calc_next(cronDef_t *cronDef) {
//	cron_next(&cronDef->cronExpr, time(NULL));
//}

void cron_meter_add(cronDef_t *cronDef, meter_t *meter) {
	cronMember_t * cm;

	//printf("cron_meter_add(%s,%s)\n",cronDef->name,meter->name);
	cm = (cronMember_t *) calloc(1,sizeof(cronMember_t));
	cm->meter = meter;

	if (! cronDef->members) {
		cronDef->members = cm;
		if (cronDef->name) meter->hasSchedule++;	// not for default schedule
		return;
	}
	cronMember_t *cm1 = cronDef->members;
	while (cm1->next) cm1 = cm1->next;
	cm1->next = cm;
	if (cronDef->name) meter->hasSchedule++;	// not for default schedule
}

void cron_meter_add_byName(char * cronName, meter_t *meter) {
	cronDef_t * cronDef = cronTab;
	cronDef_t * cd = NULL;

	//printf("cron_meter_add_byName (%s,%s)\n",cronName,meter->name);

	while (cronDef && (cd == NULL)) {
		if (cronDef->name == cronName) cd = cronDef;	// for default name==NULL
		else
			if (cronName)
				if (cronDef->name)
					if (strcmp(cronName,cronDef->name) == 0) cd = cronDef;
		cronDef = cronDef->next;
	}
	if (cd) {
		cron_meter_add(cd, meter);
		return;
	}
	EPRINTFN("cron name \"%s\" not defined",cronName);
	exit(1);
}




int parseCron (parser_t * pa) {
	int tk;
	char *st;
	char errMsg[255];

	parserExpect(pa,TK_EOL);  // after section
	tk = parserGetToken(pa);
	while (tk != TK_SECTION && tk != TK_EOF) {
		switch(tk) {
			case TK_DEFAULT:
				parserExpectEqual(pa,TK_STRVAL);
				cron_add(NULL,pa->strVal);
				break;
            case TK_STRVAL:
				st = strdup(pa->strVal);
				parserExpectEqual(pa,TK_STRVAL);
				cron_add(st,pa->strVal);
				free(st);
				break;
			case TK_EOL:
				break;
			default:
				strncpy(errMsg,"parseCron: unexpected identifier ",sizeof(errMsg)-1);
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


time_t getCurrTime() {
#ifdef CRON_USE_LOCAL_TIME
	time_t t = time(NULL);
    struct tm *lt = localtime(&t);
	return mktime(lt);
#else
	return time(NULL);
#endif
}

typedef struct workerData_t workerData_t;
struct workerData_t {
	pthread_t threadId;
	int serialPortNum;
	int verboseMsg;
	pthread_cond_t workAvailable;
	pthread_mutex_t workAvailableMutex;
	int numMeters;
	unsigned int queryTimeNano;
};

typedef workerData_t *workerDataArr_t[1];

workerData_t **workerData;

int terminateWorker;

int lockTimeoutSecs = 60;	// seconds

int LockMutex(pthread_mutex_t *m) {
	if (lockTimeoutSecs <=0) return pthread_mutex_lock(m);

	struct timespec timeoutTime;
	clock_gettime(CLOCK_REALTIME, &timeoutTime);
	timeoutTime.tv_sec += lockTimeoutSecs;

	return pthread_mutex_timedlock(m, &timeoutTime);
}


void *workerThread(void *ptr) {
	int rc;
	workerData_t *wd = (workerData_t *)ptr;
	struct timespec timeStart, timeEnd;

	//printf("Worker thread %d started\n",wd->serialPortNum);

	while (!terminateWorker) {
		VPRINTFN(2,"worker %d: waiting for next query request",wd->serialPortNum);
		if ((rc = pthread_cond_wait(&wd->workAvailable, &wd->workAvailableMutex)) != 0) {
			fprintf(stderr,"pthread_cond_wait failed with %d (%s)\n",rc,strerror(rc));
			exit(1);
		}
		//printf("worker %d: pthread_mutex_lock, return code %d (%s), waiting for work\n", wd->serialPortNum,rc,strerror(rc));

		if (!terminateWorker) {
			clock_gettime(CLOCK_REALTIME,&timeStart);
			VPRINTFN(2,"worker %d: I've been unlocked, starting query",wd->serialPortNum);
			wd->numMeters = 0;
			meter_t *meter = meters;
			while(meter) {
				if (meter->isSerial && meter->isDue)
					if (meter->serialPortNum == wd->serialPortNum) {
						VPRINTFN(4,"Serial%d: query %s",wd->serialPortNum,meter->name);
						if (queryMeter(wd->verboseMsg, meter) == 0) wd->numMeters++;
					}
				meter = meter->next;
			}
			clock_gettime(CLOCK_REALTIME,&timeEnd);
			wd->queryTimeNano = ((timeEnd.tv_sec - timeStart.tv_sec) * NANO_PER_SEC) + (timeEnd.tv_nsec - timeStart.tv_nsec);
		}
	}
	VPRINTFN(2,"worker %d: ended\n",wd->serialPortNum);

	if ((rc = pthread_mutex_unlock(&wd->workAvailableMutex)) != 0) {
		fprintf(stderr,"worker - %d: Unable unlock wd->workAvailableMutex, pthread_mutex_unlock rc=%d (%s)\n",wd->serialPortNum,rc,strerror(rc));
		exit(1);
	}
	return NULL;
}


void workerInit(int numWorkers, int verboseMsg) {
	if (disableThreadedQuery) return;
	if (numWorkers<=0) return;
	int i,rc;

	// init and create threads
	workerData = (workerData_t **) calloc(numWorkers+1,sizeof(workerData_t *));
	workerData[numWorkers] = NULL;
	for (i=0; i<numWorkers;i++) {
		workerData[i] = (workerData_t *) malloc(sizeof(workerData_t));
		//printf("Init %d\n",i);
		workerData[i]->serialPortNum = i;
		workerData[i]->verboseMsg = verbose;
		workerData[i]->workAvailable = PTHREAD_COND_INITIALIZER;
		workerData[i]->workAvailableMutex = PTHREAD_MUTEX_INITIALIZER;
		if ((rc = LockMutex(&workerData[i]->workAvailableMutex)) != 0) {		// to be able to wait for thread creation
			fprintf(stderr,"LockMutex(&workAvailableMutex[%d]->threadStartedMutex failed with %d (%s)\n",i,rc,strerror(rc));
			exit(1);
		}
		if ((rc = pthread_create(&workerData[i]->threadId, NULL, &workerThread, workerData[i])) != 0) {
			fprintf(stderr,"pthread_create(&workerData[%d],..) failed with %d (%s)\n",i,rc,strerror(rc));
			exit(1);
		}
	}
	// wait for threads started
	for (i=0; i<numWorkers;i++) {
		if ((rc = LockMutex(&workerData[i]->workAvailableMutex)) != 0) {
			fprintf(stderr,"%d: Error waiting for thread start, LockMutex(workAvailableMutex) failed with %d (%s)\n",i,rc,strerror(rc));
			exit(1);
		}
		//printf("Thread %d started\n",i);
		if ((rc = pthread_mutex_unlock(&workerData[i]->workAvailableMutex)) != 0) {
			fprintf(stderr,"%d: Error unlocking after thread start, workAvailableMutex(&workerData, ..) failed with %d (%s)\n",i,rc,strerror(rc));
			exit(1);
		}
	}
}


// trigger all worker threads
int workerRun() {
	int i = 0;
	int rc;

	if (!workerData) return 0;
	while (workerData[i]) {
		if ((rc = pthread_cond_signal(&workerData[i]->workAvailable)) != 0) {
			fprintf(stderr,"workerRun - %d: Unable to signal worker, pthread_cond_signal rc=%d (%s)\n",i,rc,strerror(rc));
			exit(1);
		}
		i++;
	}
	return i;
}

// wait for all worker threads to finish work
void workerWait() {
	int i = 0;
	int rc;

	if (!workerData) return;

	while (workerData[i]) {
		if ((rc = LockMutex(&workerData[i]->workAvailableMutex)) != 0) {		// blocks while thread is running
			fprintf(stderr,"workerWait - %d: Unable lock workAvailableMutex, LockMutex rc=%d (%s)\n",i,rc,strerror(rc));
			exit(1);
		}
		if ((rc = pthread_mutex_unlock(&workerData[i]->workAvailableMutex)) != 0) {
			fprintf(stderr,"workerWait - %d: Unable unlock workAvailableMutex, pthread_mutex_unlock rc=%d (%s)\n",i,rc,strerror(rc));
			exit(1);
		}
		i++;
	}
}

// terminate all worker threads
void workerTerminate() {
	terminateWorker++;
	workerRun();
	workerWait();
}



int cron_queryMeters(int verboseMsg) {
	meter_t * meter = meters;
	cronMember_t * cm;
	time_t currTime = getCurrTime();
	int serialMetersDefined = 0;
	int res;
	struct timespec timeStart, timeEnd;
	int i;
	double queryTime;
	int numMeters;
	int numDueMeters = 0;

	while (meter) {
		meter->meterHasBeenRead = 0;
		meter->isDue = 0;
		if (meter->isSerial) serialMetersDefined++;
		meter = meter->next;
	}

	// mark all meters of all due schedules and calculate next query time
	cronDef_t * cd = cronTab;
	while (cd) {
		if (cron_is_due(currTime,cd)) {
			if (cd->members) {
				VPRINTF(1,"Schedule \"%s\" is due: ",cd->name ? cd->name : "default");
				cd ->nextQueryTime = cron_next(&cd->cronExpr, currTime);
				cm = cd->members;
				int first=1;
				while (cm) {
					assert (cm->meter != NULL);
					if (! cm->meter->disabled) {
						VPRINTF(1,"%c%s",first?' ':',',cm->meter->name);
						setMeterFvalueInfluxLast (cm->meter);
						cm->meter->isDue++;
						numDueMeters++;
					}
					cm = cm->next;
				}
				VPRINTFN(1,""); //, next query: %s\n",ctime(&cd->nextQueryTime));
			}
		}
		cd = cd->next;
	}

	if (verbose && numDueMeters) {
		cronDef_t * cd = cronTab;
		while (cd) {
			PRINTF("  %s: next query: %s",cd->name ? cd->name : "default",ctime(&cd->nextQueryTime));
			cd = cd->next;
		}
	}

	if (numDueMeters == 0) return 0;

	numMeters = 0;
	if (!workerData || (serialMetersDefined==0)) {
		// query all due meters
		meter = meters;
		while (meter) {
			if (meter->isDue) {
				if (meter->isFormulaOnly) {
					numMeters++;
					meter->meterHasBeenRead++;
				} else {
					res = queryMeter(verbose > 0, meter);
					if (res == 0) {
						numMeters++;
						if ((meter->queryTimeNano > 0) && (verboseMsg)) {
							double timeSecs = meter->queryTimeNano / NANO_PER_SEC;
							VPRINTFN(1,"%s: query took %06.4f seconds",meter->name, timeSecs);
						}
					}
				}
			}
			meter = meter->next;
		}
	} else {
		clock_gettime(CLOCK_MONOTONIC,&timeStart);
		// multi thread query
		if (workerRun()) usleep(100000);	// start a thread for each serial port and give them some time, TCP is much faster

		// query all TCP meters
		meter = meters;
		while (meter) {
			if (meter->isDue && meter->isFormulaOnly) {
				numMeters++;
				meter->meterHasBeenRead++;
			} else
			if (meter->isDue && meter->isTCP) {
				VPRINTFN(4,"TCP: query %s",meter->name);
				res = queryMeter(verbose > 0, meter);
				if (res == 0) {
					numMeters++;
					if ((meter->queryTimeNano > 0) && verboseMsg) {
						double timeSecs = meter->queryTimeNano / NANO_PER_SEC;
						VPRINTFN(1,"%s: query took %06.4f seconds",meter->name, timeSecs);
					}
				}
			}
			meter = meter->next;
		}
		if (verboseMsg) {
			clock_gettime(CLOCK_MONOTONIC,&timeEnd);
			queryTime = (double)(timeEnd.tv_sec + timeEnd.tv_nsec / NANO_PER_SEC)-(double)(timeStart.tv_sec + timeStart.tv_nsec / NANO_PER_SEC);
			VPRINTFN(1,"TCP query took %06.4f seconds",queryTime);
		}
		workerWait();	// wait for serial meters queried
		if (verboseMsg) {
			i = 0;
			while (workerData[i]) {
				if (workerData[i]->queryTimeNano && workerData[i]->numMeters > 0) {
					queryTime = (double)workerData[i]->queryTimeNano / NANO_PER_SEC;
					PRINTFN("Serial%d query took %06.4f seconds",i,queryTime);
				}
				i++;
			}
		}
		i = 0;
		while (workerData[i]) {
			numMeters += workerData[i]->numMeters;
			i++;
		}
	}
#ifndef DISABLE_FORMULAS
	// meter formulas
	meter = meters;
	while (meter) {
		if (! meter->disabled)
			if (meter->meterHasBeenRead || meter->isFormulaOnly) executeMeterFormulas(meter);
		meter = meter->next;
	}
#endif

    // handle influxWriteMult
    meter = meters;
	while (meter) {
		if (! meter->disabled)
			if (meter->meterHasBeenRead) {
				setMeterFvalueInflux(meter);
				executeInfluxWriteCalc(verboseMsg,meter);
			}
		meter = meter->next;
	}

	return numMeters;
}


void cron_showSchedules() {
	cronDef_t * cd = cronTab;
	printf( "Schedules\n" \
			"Name                Definition\n" \
			"------------------------------------------------------------------------------\n");
	while (cd) {
		printf("%-20s%-30s\n%-20s",cd->name==NULL ? "default" : cd->name,cd->cronExpression,"");

		cronMember_t * cm = cd->members;
		int first = 1;
		while (cm) {
			if (first>0) printf("Members: ");
			printf("%c%s",first ? '[' : ',',cm->meter->name);
			cm = cm->next;
			first=0;
		}
		if(first<1) printf("] ");
		printf("next query: %s\n",ctime(&cd->nextQueryTime));
		cd = cd->next;
	}
}

// set the default schedule for all meters where no schedule(s) are specified
void cron_setDefault() {
	meter_t * meter = meters;
	time_t currTime = getCurrTime();

	while (meter) {
		if (! meter->hasSchedule) cron_meter_add_byName(NULL,meter);
		meter = meter->next;
	}
	// set next query time
	cronDef_t * cd = cronTab;
	while (cd) {
		cd->nextQueryTime = cron_next(&cd->cronExpr, currTime);
		cd = cd->next;
	}
}
