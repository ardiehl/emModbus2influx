/*
 * Copyright © Stéphane Raimbault <stephane.raimbault@gmail.com>
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <modbus.h>
#if defined(_WIN32)
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#endif

#include "argparse.h"
#include "log.h"

#define NB_CONNECTION 5
#define BMS_MAX_CELLS 16
#define NB_REGISTERS 128

#define VER "1.00 Armin Diehl <ad@ardiehl.de> Apr 17,2025 compiled " __DATE__ " " __TIME__ " "
#define ME "testserver"
//#define CONFFILE "jk2modbus.conf"

using namespace std;

char *serDevice; // = strdup("/dev/ttyUSB0");
char *serParity = strdup("N");
int serStopbits = 2;
int serBaudrate = 115200;
char *configFileName;
char *mbip = strdup("localhost");
char *mbport = strdup("1502");
int modbusDebug;

int start_bits = 0;
int nb_bits = 16;
int start_input_bits = 0;
int nb_input_bits = 16;
int start_registers = 0;
int nb_registers = 32;
int start_input_registers = 0;
int nb_input_registers = 32;
int modbusSlave = 1;


// modbus start register number
#define MODBUS_START_REG 0


//#define CONFFILEARG "--configfile="

// to avoid unused error message for --configfile
int dummyCallback(argParse_handleT *a, char * arg) {
	return 0;
}

int showVersionCallback(argParse_handleT *a, char * arg) {
        printf("%s %s\n",ME,VER);
        printf("  libmodbus: %d.%d.%d - ",LIBMODBUS_VERSION_MAJOR,LIBMODBUS_VERSION_MINOR,LIBMODBUS_VERSION_MICRO);
#ifdef MODBUS_STATIC
        printf("static\n");
#else
        printf("dynamic\n");
#endif
	exit(2);
}



int parseArgs (int argc, char **argv) {
	int res = 0;
	argParse_handleT *a;
	int syslog = 0;

	AP_START(argopt)
		AP_HELP
		//AP_OPT_STRVAL_CB    (0, 0, "configfile"		,NULL                  ,"config file name",&dummyCallback)
		AP_OPT_STRVAL       (0,'d',"device"					,&serDevice              ,"specify serial device name for modbus RTU")
		AP_OPT_INTVAL       (1,'b',"baud"					,&serBaudrate            ,"serial baudrate")
		AP_OPT_STRVAL       (1,'a',"parity"					,&serParity         	 ,"N,E or O")
		AP_OPT_INTVAL       (1,'S',"stopbits"				,&serStopbits     	     ,"1 or 2 stopbits")
		AP_OPT_INTVALFO     (0,'v',"verbose"  		     	,&log_verbosity          ,"increase or set verbose level")
		AP_OPT_STRVAL       (1,'i',"ip"						,&mbip                   ,"Modbus IP(4/6) or hostname to listen on")
		AP_OPT_STRVAL       (1,'p',"port"					,&mbport	             ,"Modbus port/service")
		AP_OPT_INTVALF      (0,'y',"syslog"         		,&syslog                 ,"log to syslog insead of stderr")
		AP_OPT_INTVAL       (0,'o',"modbus-debug"			,&modbusDebug            ,"show modbus debug messages")
		AP_OPT_INTVAL       (1,'s',"slave"					,&modbusSlave            ,"Slave address for modbus RTU")
		AP_OPT_INTVAL       (1, 0 ,"start_bits"				,&start_bits             ,"start address of coils")
		AP_OPT_INTVAL       (1, 0 ,"nb_bits"				,&nb_bits                ,"number of coils")
		AP_OPT_INTVAL       (1, 0 ,"start_input_bits"		,&start_input_bits       ,"start address of discrete inputs")
		AP_OPT_INTVAL       (1, 0 ,"nb_input_bits"			,&nb_input_bits          ,"number of discrete inputs")
		AP_OPT_INTVAL       (1, 0 ,"start_registers"		,&start_registers        ,"start address of holding registers")
		AP_OPT_INTVAL       (1, 0 ,"nb_registers"			,&nb_registers           ,"number of holding registers")
		AP_OPT_INTVAL       (1, 0 ,"start_input_registers"	,&start_input_registers  ,"start address of holding registers")
		AP_OPT_INTVAL       (1, 0 ,"nb_input_registers"		,&nb_input_registers     ,"number of holding registers")

		AP_OPT_INTVALF_CB	(0,'e',"version"        		,NULL                    ,"show version and exit",&showVersionCallback)

	AP_END;

	// check if we have a configfile argument
#if 0
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

	//if (configFileName == NULL) configFileName = strdup(CONFFILE);
	a = argParse_init(argopt, configFileName, NULL, NULL);
#endif
	a = argParse_init(argopt, NULL, NULL, NULL);
	res = argParse (a, argc, argv, 0);
	if (res != 0) {
		argParse_free (a);
		return res;
	}


	if (syslog) log_setSyslogTarget(ME);


	argParse_free (a);

    return 0;
}




static modbus_t *ctx = NULL;

static int server_socket = -1;

modbus_mapping_t* mm;

static void close_sigint(int dummy)
{
	PRINTFN("terminating");
	if (server_socket != -1) close(server_socket);


	modbus_free(ctx);

	PRINTFN("terminated");
	exit(dummy);
}

char * functionName (int functionCode) {
	switch (functionCode) {
	case 0x01:	return (char *)"Read Coils";
	case 0x02:	return (char *)"Read Discrete Inputs";
	case 0x03:	return (char *)"Read Holding Registers";
	case 0x04:	return (char *)"Read Input Registers";
	case 0x05:	return (char *)"Write Single Coil";
	case 0x06:	return (char *)"Write Single Register";
	case 0x08:	return (char *)"Diagnostics (Serial Line only)";
	case 0x0B:	return (char *)"Get Comm Event Counter (Serial Line only)";
	case 0x0F:	return (char *)"Write Multiple Coils";
	case 0x10:	return (char *)"Write Multiple Registers";
	case 0x11:	return (char *)"Report Server ID (Serial Line only)";
	case 0x16:	return (char *)"Mask Write Register";
	case 0x17:	return (char *)"Read/Write Multiple Registers";
	case 0x2B:
	case 0x0E:	return (char *)"Read Device Identification";

	}
	return (char *)"";
}

char * decodeFunction(uint8_t *buf, int isReply) {
	char st[255];
	int startAddr = ((int)buf[2] << 8) + buf[3];
	snprintf(st,sizeof(st),"SlaveID %d, Function Code %d (0x%02x) %s, startAddr %d (0x%02x)",buf[0],buf[1],buf[1],functionName(buf[1]),startAddr,startAddr);
	return strdup(st);
}

uint8_t query[MODBUS_TCP_MAX_ADU_LENGTH];


int processModbus() {
	int rc = modbus_receive(ctx, query);
	if (rc > 0) {
		int offset = modbus_get_header_length(ctx);
		char * st = decodeFunction(&query[offset - 1],0);
		VPRINTFN(1,"%s",st);
		free(st);
		modbus_reply(ctx, query, rc, mm);
		return 0;
	}
	return rc;
}


int main(int argc, char *argv[]) {

	int master_socket;
	int rc,i;
	fd_set refset;
	fd_set rdset;
	// Maximum file descriptor number
	int fdmax;
	char ipaddrstr[INET6_ADDRSTRLEN];

	rc = parseArgs (argc, argv);
	//VPRINTFN(9,"parseArgs returned %d",rc);
	if (rc) exit (rc);

	if (serDevice) {
		ctx = modbus_new_rtu(serDevice, serBaudrate, serParity[0], 8, serStopbits);
		if (!ctx) {
			EPRINTFN("modbus_new_rtu(%s,%d,%s,8,%d) failed with %d %s",serDevice,serBaudrate,serParity,serStopbits,errno,modbus_strerror(errno));
			exit(1);
		}
        modbus_set_slave(ctx, modbusSlave);
        modbus_connect(ctx);


	} else {
		//VPRINTFN(9,"modbus_new_tcp_pi(%s,%s)",mbip, mbport);
		ctx = modbus_new_tcp_pi(mbip, mbport);
		if (!ctx) {
			EPRINTFN("modbus_new_tcp_pi(%s,%s) failed with %d %s",mbip, mbport, errno, modbus_strerror(errno));
			exit(1);
		}

		server_socket = modbus_tcp_pi_listen(ctx, NB_CONNECTION);
		if (server_socket == -1) {
				EPRINTFN("modbus_tcp_pi_listen on %s:%s failed with %d (%s)",mbip ? mbip : "any_ip", mbport,errno,modbus_strerror(errno));
				modbus_free(ctx);
				return -1;
		}
	}
	if (modbusDebug) modbus_set_debug(ctx,1);


	mm = modbus_mapping_new_start_address(start_bits, nb_bits, start_input_bits, nb_input_bits, start_registers, nb_registers, start_input_registers, nb_input_registers);
	if (!mm) {
		EPRINTFN("Failed to allocate modbus mapping");
		modbus_free(ctx);
		return -1;
	}
	for (i=0;i<mm->nb_bits;i++) mm->tab_bits[i] = i & 1;
	for (i=0;i<mm->nb_input_bits;i++) mm->tab_input_bits[i] = i & 1;
	for (i=0;i<mm->nb_input_registers;i++) mm->tab_input_registers[i] = i+1;
	for (i=0;i<mm->nb_registers;i++) mm->tab_registers[i] = i+1;

	signal(SIGINT, close_sigint);
	signal(SIGTERM, close_sigint);

	// Clear the reference set of socket
	FD_ZERO(&refset);
	// Add the server socket
	FD_SET(server_socket, &refset);

	// Keep track of the max file descriptor
	fdmax = server_socket;

	//PRINTFN("mainloop started (%s %s)",ME,VER);
	if (serDevice) {
		PRINTFN("Listening for requests on %s",serDevice);
		for (;;) {
			rc = processModbus();
		}
	} else
	{
		PRINTFN("Listening for requests on %s:%s",mbip,mbport);
		for (;;) {
			rdset = refset;
			if (select(fdmax + 1, &rdset, NULL, NULL, NULL) == -1) {
				PRINTFN("Server select() failure %d (%s).",errno,strerror(errno));
				close_sigint(1);
			}

		// Run through the existing connections looking for data to be read
		for (master_socket = 0; master_socket <= fdmax; master_socket++) {
			if (!FD_ISSET(master_socket, &rdset)) continue;

			if (master_socket == server_socket) {
				// A new client is asking for a connection
				socklen_t addrlen;
				struct sockaddr_in clientaddr;
				struct sockaddr_in6 clientaddr6;
				void * clientaddress;
				int newfd;
				in_port_t port;
				sa_family_t family;

				// Handle new connections
				addrlen = sizeof(clientaddr);
				memset(&clientaddr, 0, sizeof(clientaddr));
				newfd = accept(server_socket, (struct sockaddr *) &clientaddr, &addrlen);
				if (newfd == -1) {
					EPRINTF("Server accept() error %s (%s)",errno,strerror(errno));
					close_sigint(1);
				} else {
					FD_SET(newfd, &refset);		// save fileno of new connection

					// Keep track of the maximum
					if (newfd > fdmax) fdmax = newfd;

					// get client ip address
					if (clientaddr.sin_family == AF_INET) {		// IPV4
						clientaddress = (void *) & clientaddr.sin_addr;
						family = AF_INET;
						port = clientaddr.sin_port;
					} else {									// IPV6
						addrlen = sizeof(clientaddr6);
						getpeername(newfd, (struct sockaddr *)&clientaddr6, &addrlen);
						clientaddress = (void *) & clientaddr6.sin6_addr;
						family = AF_INET6;
						port = clientaddr6.sin6_port;
					}
					inet_ntop(family,clientaddress,(char *)&ipaddrstr, sizeof(ipaddrstr));	// convert ip to string

					PRINTFN("New connection from %s:%d on socket %d\n",ipaddrstr, port, newfd);
				}
			} else {
				//VPRINTFN(1,"processing query");
				modbus_set_socket(ctx, master_socket);
				rc = processModbus();
				if (rc == -1) {
					// Remove from reference set
					FD_CLR(master_socket, &refset);
					close(master_socket);
					PRINTFN("%d: Connection closed\n",master_socket);

					if (master_socket == fdmax) fdmax--;
				}


				}
			}
		}
	}
    return 0;
}

