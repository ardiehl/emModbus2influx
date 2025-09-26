
# emModbus2influx
## read modbus RTU/TCP slaves and write to infuxdb (V1 or V2) and/or MQTT and/or Grafana Live. Other time series databases, e.g. questdb are supported as well.

### Definitions
- **MeterType** - a definition of the registers,  register types and more of a specific type of modbus slave
- **Meter** - a definition of a physical modbus RTU or TCP slave based on a MeterType

It is named meter because it was originally used to query energy meters but in fact it can query any modbus TCP or RTU slave. I'm using it for energy meters, Fronius solar inverters as well as Victron Energy GX.
### Features

 - modbus RTU via serial port (multiple serial ports are supported as well)
 - unlimited number of metertypes and meters
 - supports SUNSPEC meter definitions (tested with Fronius Symo)
 - supports formulas for changing values after read or for defining new fields (in the metertype as well as in the meter definition)
 - MQTT data is written on every query to have near realtime values (if MQTT is enabled), InfluxDB writes can be restricted to n queries where you can specify for each field if max,min or average values will be posted to InfluxDB
 - Virtual devices=Meters can be defined, these can post values from different Modbus slaves to MQTT and/or InfluxDB and(or Grafana Live
 - Using formulas and virtual meters without any Modbus device, test data for MQTT and/or InfluxDB and/or Grafana Live can be generated (example: config-02.conf)
 - supports dryrun for testing definitions
 - supports interactive formula testing
 - use of [libmodbus](https://libmodbus.org/) for modbus TCP/RTU communication
 - use of [paho-c](https://github.com/eclipse/paho.mqtt.c) for MQTT
 - use of [muparser](https://beltoforion.de/en/muparser/) for formula parsing
 - use of [ccronexpr](https://github.com/staticlibs/ccronexpr) for scheduling using cron expressions
 - use of libcurl for http,https,ws and wss
 - libmodbus,paho-c and muparser can by dynamic linked (default) or downloaded, build and linked static automatically when not available on target platform, e.g. Victron Energy Cerbox GX (to be set at the top of Makefile)
 - unlimited number of Serial ports, MeterTypes, Meters and Modbus TCP slaves

### Get started

emModbus2Influx requires a configuration file. By default ./emModbus2Influx.conf is used. You can define another config file using the

```
--configfile=pathOfConfigFile
```

command line parameter.
The config file consists of two sections where the first section ends when [ was found as the first character of a line. The first section contains command line parameters in long format while the second section contains

- first the meter types and
- second the meter definitions.

Parameters given in the first section can be overridden by command line parameters.
Example uncomplete config file:

```#device=/dev/ttyAMA0
baud=9600
parity=e
stopbits=1

[MeterType]
name = "Fronuis_Symo"
sunspec
id=103
readid=103	# Inverter block type 103, length will be read from device (50 on my Symo)
"i"  = 2,uint16,sf=6
"p"  = 14,uint16,sf=15,force=int

[MeterType]
name = "DRT428M_3"	# same as ORNO OE-WE-517

# set tarif time 1 for tarif 1 at any time and disable other tarif times
init = 0x300,0,1,0*10   # write 0x000,0x001 and 10 times 0x000
init = 0x030c,0*12
init = 0x0318,0*12
init = 0x0324,0*12
init = 0x0330,0*12
init = 0x033c,0*12
init = 0x0348,0*12
init = 0x0354,0*12

# set current meter tarif
# settarif = n, Address, uint16 value [,uint16 value ...]
# n is in range of 1 to 4
settarif = 1,0x300,0,1
settarif = 2,0x300,0,2
settarif = 3,0x300,0,3
settarif = 4,0x300,0,4

# read voltage, current and power
read =0x0e,22       # read a block of 22 registers (44 Bytes) starting at 0x0e

# consumption per tarif
read=0x130,38

# line voltage
"u1"=0x0e,float,arr="u" ,dec=1
"u2"=0x10,float,arr="u" ,dec=1
"u3"=0x12,float,arr="u" ,dec=1

# active power
"p1"=0x1e,float,arr="p",force=int
"p2"=0x20,float,arr="p",force=int
"p3"=0x22,float,arr="p",force=int

# total energy
"kwh"=0x100,float,force=int     # send as int to InfluxDB / MQTT

# energy for up to 4 tarifs
"kwh_1"=0x130,float,force=int,arr="kwh_t"
"kwh_2"=0x13c,float,force=int,arr="kwh_t"
"kwh_3"=0x148,float,force=int,arr="kwh_t"
"kwh_4"=0x154,float,force=int,arr="kwh_t"

[Meter]
disabled=0
address=11	# modbus RTU address of meter
type="DRT428M_3"
name="EnergyMeter1"
serial=0                        # use the first defined serial port

[Meter]
disabled=0
name="Symo"
hostname="fronius.armin.internal"
address=1
type="Fronuis_Symo"
```
If emModbus2Influx is started with --baud=19200 the 9600 baud in the config file will be ignored.
Comments can be included using #. Everything after # in a line will be ignored.
Numbers can be specified decimal or, when prefixed with 0x, hexadecimal.

## command line options or options in the first section of the config file

Long command line options requires to be prefixed with -- while as in the config file the option has to be specified without the prefix. Short command line options can only be used on command line. The descriptions below show the options within the config file, if used on command line, a prefix of -- is required.

```
  Usage: emModbus2influx [OPTION]...
  -h, --help              show this help and exit
  --configfile=           config file name
  -d, --device=           specify serial device names separated by ,
  --baud=                 baudrates separated by , (9600)
  -a, --parity=           N (default), E or O (e)
  -S, --stopbits=         1 or 2 stopbits (1)
  -4, --rs485=            set rs485 mode
  -m, --measurement=      Influxdb measurement (energyMeter)
  -g, --tagname=          Influxdb tag name (Device)
  -s, --server=           influxdb server name or ip (lnx.armin.d)
  -o, --port=             influxdb port (8086)
  -b, --db=               Influxdb v1 database name
  -u, --user=             Influxdb v1 user name
  -p, --password=         Influxdb v1 password
  -B, --bucket=           Influxdb v2 bucket (ad)
  -O, --org=              Influxdb v2 org (diehl)
  -T, --token=            Influxdb v2 auth api token
  --influxwritemult=      Influx write multiplicator
  -c, --cache=            #entries for influxdb cache (1000)
  -M, --mqttserver=       mqtt server name or ip (lnx.armin.d)
  -C, --mqttprefix=       prefix for mqtt publish (ad/house/energy/)
  --mqttstatprefix=       prefix for mqtt statistics publish (ad/house/stat/)
  -R, --mqttport=         ip port for mqtt server (1883)
  -Q, --mqttqos=          default mqtt QOS, can be changed for meter (0)
  -l, --mqttdelay=        delay milliseconds after mqtt publish (0)
  -r, --mqttretain=       default mqtt retain, can be changed for meter (1)
  -i, --mqttclientid=     mqtt client id (emModbus2influx)
  --mqttformat=           json format, 0=std,1=Logo array (0)
  --ghost=                grafana server url w/o port, e.g. ws://localost or https://localhost
  --gport=                grafana port (3000)
  --gtoken=               authorisation api token for Grafana
  --gpushid=              push id for Grafana
  --ginfluxmeas=          use influx measurement names for grafana as well (0)
  -v, --verbose[=]        increase or set verbose level
  -G, --modbusdebug       set debug for libmodbus
  -P, --poll=             poll intervall in seconds
  -H, --cron=             Crontab style expression like Sec Min Hour Day Mon Wday
  --no-threads            enable threaded query (one thread for each serial port and one for tcp)
  -y, --syslog            log to syslog insead of stderr
  -Y, --syslogtest        send a testtext to syslog and exit
  -e, --version           show version and exit
  -D, --dumpregisters     Show registers read from all meters and exit, twice to show received data
  -U, --dryrun[=]         Show what would be written to MQTT/Influx for one query and exit
  -t, --try               try to connect returns 0 on success
  --formtryt=             interactive try out formula for register values for a given meter name
  --formtry               interactive try out formula (global for formulas in meter definition)
  --scanrtu               scan for modbus rtu devices (0)
  --scan                  scan a device for available registers (0)
  --scanstart=            register to start scan with (0)
  --scanend=              register to end scan with (65535)
  --scanhost=             TCP hostname, if not specified Modbus RTU will be used for scan
  --scanaddr=             Modbus address to scan (0)
  --scaninput             scan input registers (default=both) (0)
  --scanholding           scan holding registers (default=both) (0)
```

### serial port(s) for modbus RTU
```
device=/dev/ttyUSB0
baud=9600
parity=N
stopbits=1
rs485=0
```

Specify the serial port parameters, defaults are shown above.
__parity__ can be N for none, E for even or O for odd.

If more than one serial ports is used, comma is to be used to separate parameters, e.g.
```
device=/dev/ttyUSB_energyMeters,/dev/ttyUSB_tempSensors,/dev/tty/USB3
baud=9600,2400
rs485=0,0,1
```
The first serial port is serial0 and is the default for serial= in a meter definition.
In case serial parameters are not specified for ports >0, the ones for serial0 are used, in the example 9600 baud will be used for USB3.

### InfluxDB - common for version 1 and 2

```
server=ip_or_server_name_of_influxdb_host
port=8086
measurement=energyMeter
tagname=Meter
cache=1000
```

If __server__ is not specified, post to InfluxDB will be disabled at all (if you would like to use MQTT and/or Grafana Live only). Default is http://. To use SSL prefix the hostname by https://, e.g.
```
server=https://myinfluxhost.mydomain.de
port=8086
```

__tagname__ will be the tag used for posting to Influxdb.
__port__ is the IP port number and defaults to 8086
__cache__ is the number of posts that will be cached in case the InfluxDB server is not reachable. This is implemented as a ring buffer. The entries will be posted after the InfluxDB server is reachable again. One post consists of the data for all meters queried at the same time.
__measurement__ sets the default measurement and can be overriden in a meter type or in a meter definition.

### InfluxDB version 1

For version 1, database name, username and password are used for authentication.

```
db=
user=
password=
```

### InfluxDB version 2

Version 2 requires bucket, org and token:

```
bucket=
org=
token=
```
### Grafana Live
Grafana Live is tested with http and ws (Websockets) but should work with https and wss as well. For best performance and lowest overhead, ws:// should be the perferred protocol.
Websocket support, is at the time of writing (07/2023) still beta but seems to work fine. However, current distributions like Fedora 39 or Raspberry (Debian 11 (bullseye)) ships with a shared libcurl that do not support websockets. If you try to use websockets with a shared libcurl and websockets are not supported, emModbus2influx will try fallback to http or https:.
To use websockets, static linking of libcurl can be enabled in Makefile. The Makefile will download a current version of curl and will configure, compile and link this version.
In this is may be required to install additional devel packages required by curl. These are the packages i needed to compile on Debian
```
sudo apt install libmuparser-dev libmuparser2v5 libmodbus-dev libmodbus5 libreadline-dev libpaho-mqtt-dev libpaho-mqtt1.3 libzstd-dev zstd libssl-dev
```
Required parameter
__ghost__ url of the Grafana server without port, e.g. ws://localhost
__gtoken__ token for authentication, at the time of writing (Grafana 10.1.1 OSS) a service account with the role "Admin"is required.
__gpushid__ the push id. Data is pushed to Grafana using
```
ServerAddr:port/api/live/push/gpushid
```
and this will show up in Grafana live as
```
stream/gpushid
```
By default, this will be postfixed by the name of the meter, e.g.
```
stream/gpushid/myMeterName
```
In case
```
ginfluxmeas=1
```
is specified, the given name for the Influxdb measurement is included as
```
stream/gpushid/measurement/myMeterName
```
However, you can overwrite meterName using
```
gname=
```
in a meter definition ether with or without ginfluxmeas=1.
__gpushid__ can specified only once and can not be changed by meter. This is because it is part of the path specified in http push. After converting the http connection to a Websocket, there is not way to change that.

### MQTT

```
mqttserver=
mqttprefix=ad/house/energy/
mqttstatprefix=ad/stat/
mqttport=1883
mqttqos=0
mqttretain=0
mqttclientid=emModbus2influx
mqttformat=0
```

Parameters for MQTT. If mqttserver is not specified, MQTT will be disabled at all (if you would like to use InfluxDB only).
mqttqos and mqttretain sets the default, these can be overriden per meter or MeterType definition.

__mqttstatprefix__:
If defined, statistics about query times will be posted to mqtt.

__mqttqos__:
- At most once (0)
- At least once (1)
- Exactly once (2)

__mqttretain__:
- no (0)
- yes (1)

If mqttretain is set to 1, mqtt data will only send if data has been changed since last send.

__mqttclientid__:
defaults to emModbus2influx, needs to be changed if multiple instances of emModbus2influx are accessing the same mqtt server

__mqttformat__:
defaults to 0 (standard). 1 will be a format compatible with the Siemens Logo Array format

### additional options
```
verbose=0
syslog
modbusdebug
poll=5
```

__verbose__: sets the verbisity level
__syslog__: enables messages to syslog instead of stdout
__modbusdebug__: enables debug output for libmodbus, can also be specified per meter
__poll__: sets the poll interval in seconds
__cron__: specifies the default poll interval in a crontab style (see schedule definition)

### command line only parameters

```
--configfile=
--syslogtest
--version
--dryrun
--dryrun=n
--try
--formtryt=MeterName
--formtry
--scanrtu
```
__configfile__: sets the config file to use, default is ./emModbus2influx.conf
**syslogtest**: sends a test message to syslog.
**dryrun**: perform one query of all meters and show what would be posted to InfluxDB / MQTT
**dryrun=n**: perform n querys of all meters and show what would be posted to InfluxDB / MQTT
**try**: try to reach the first defined meter via modbus RTU (to detect the serial port in scripts). Return code is 0 if the first Modbus RTU device can be reached or 1 on failure.
**formtryt**: interactively try out a formula for a MeterType
**formtry**: interactively try out a formula for a Meter

## Some words about SunSpec

For standard mobus slaves, the register addresses are fixed, however, this is not the case for [SunSpec](https://sunspec.org/) devices. For SunSpec there is a start address (defaults to 40000) pointing to the first block. Each block has an id and a length, the length may vary depending on firmware versions. Register addresses will be specified as an offset within a block.
emModbus2Influx will resolve the block relative register addresses to absolute addresses at the first query. Therefore if you install a firmware update of aSunSpec device, it may me necessary to restart emModbus2Influx in case some block lengths have been changed by the manufacturer.

# Schedule definitions
Defines schedule times for querying meters. There is always a default schedule defined by poll= or by cron=. A meter or a write can be part of one or more schedules with schedule="scheduleName"[,..].
The implementation is based on https://github.com/staticlibs/ccronexpr and is like cron with the addition of the first paramater (seconds).
Some examples for expressions:
```
"0 0 * * * *"          = the top of every hour of every day
"*/10 * * * * *"       = every ten seconds
"0 0 8-10 * * *"       = 8, 9 and 10 o'clock of every day
"0 0/30 8-10 * * *"    = 8:00, 8:30, 9:00, 9:30 and 10 o'clock every day
"0 0 9-17 * * MON-FRI" = on the hour nine-to-five weekdays
"0 0 0 25 12 ?"        = every Christmas Day at midnight

```
From my config file:
```
[Schedule]
#Sensors underfloor heating - Every 15 minutes
"s_TempFBH" = "0 */15 * * * *"
#Heatpump every minute
"s_Heatpump" = "4 */1 * * * *"
```

# MeterType definitions
Defines a meter type. A meter type is the base definition and can be used within multiple meters. It defines registers and register options for a modbus device. Registers can be modbus registers, modbus registers plus a formula or formula only registers. Additional formula only registers can be added in a meter definition.
Example:
```
# energy meter via Victon Energy Cerbos GX (Modbus TCP)
[MeterType]
name     = "GX_EnergyMeter"
read     = 2600,38
"u1"     = 2616,uint16,arr="u",dec=1,div=10,influx=0
"u2"     = 2618,uint16,arr="u",dec=1,div=10,influx=0
"u3"     = 2620,uint16,arr="u",dec=1,div=10,influx=0
"p"      = "p1+p2+p3",force=int
"p1"     = 2600,int16,arr="p"
"p2"     = 2601,int16,arr="p"
"p3"     = 2602,int16,arr="p"
"kwh"    = 2634,uint32,force=int,div=100
"kwho"   = 2636,uint32,force=int,div=100
```

to use this meter type a minimal meter definition could be:
```
[Meter]
name="Grid"           # the name for influx and modbus
type="GX_EnergyMeter" # type, needs to be already defined, case sensitive
address=30            # modbus address
hostname="vgx.xy.de"  # modbus TCP (name or address), if not specified, modbus RTU will be used
```

Each meter type definition starts with
```
[MeterType]
```
where the [ has to be the first character of a line. Options that can be specified for a meter type are:

```name="NameOfMeter"```
Mandatory, name of the MeterType. The name is used in a meter definition and is case sensitive.

```querydelay=x```
Where x is the delay in MS between modbus requests for this meter. Required e.g. for the JK Inverter-BMS.

```type=holding```
Optional, default=holding. Specifies for all following read= or registers what type of modbus function will be used for reading the data. Valid values are holding, input, coil, inputstatus or discreteinput. (inputstatus is the same as discreteinput)

```read=start,numRegs```
Optional: non SunSpec, specifies to read numReg (16 bit) registers starting at 'start'. After each read= statement, emModbus2Influx will try to map the reading data to the required registers. This is to avoid unnecessary reads especially for TCP.
If read= is not specified, a single read request for each register will be performed. This may, and for TCP it will, slow down the overall read process.
In case there are remaining registers not mapped, single read commands for these registers will be performed.
There is no limit in the number of read= specified in the config file. To make sure all registers are covered by read= stements, start emModbus2influx with dryrun and verbosity level 1:
```
./emModbus2influx --dryrun --configfile=config_01.conf -v1
Influx init: serverName: lnx.armin.d, port 8086, dbName: (null), userName: (null), password: (null), org: diehl, bucket:test, numQueueEntries 1000
mainloop started (emModbus2influx 1.00 Armin Diehl <ad@ardiehl.de> Apr 24,2022, compiled Oct  7 2022 13:44:58)
Query "tempKeller" @ TCP vgx.armin.d:502, Modbus address 24
tempKeller: received 1 regs for Pressure from 3308 (0xcec) to 3308 (0xcec) (not covered via block read), data received: 0000
```
This shows that 1 register (3308) is not covered by the read= statement.

```readid=idNum[,start,numRegs]```
Read for SunSpec devices, IdNum is the SunSpc id of the block. If start and numRegs are not specified, the whole block will be read.

If there are no read= statement a read request for each required register will be performed.
read and readid will read the specified number of registers in a buffer. After that, it will be checked if a defined register is within that buffer and if yes, the value of the register will be read from the buffer. After all read and readid definition have been processed and there are register definitions not yet read, a single read will be performed for each of the remaining registers. This works fine but may result in a poor performance when using modbus tcp.


```sunspec[=startAddr]```
Starts scanning for sunspec (e.g. Fronius) id's at the given address, if no address is given, 40000 will be used.
When using sunspec, register addresses will be offsets within each id block (starting with 0). At least before the first register definition the id= option is required to specify the id of the block to be used. The register address will then be calculated based on the offset of the sunspec block and the given register address. Multiple id= options with different id's can be used.


```influx=0|1```
0 will disable this register for influxdb, sets default for registers below
```mqtt=0|1```
0 will disable this register for mqtt, sets default for registers below

```measurement="InfluxMeasurement"```
Overrides the default InfluxDB measurement for this meter.

```querydelay=x```
Delay in ms between modbus queries while reading a meter. Some devices, e.g. the jk inverter BMS will not respond to seqencial queries and require a delay, e.g. 20 ms for the jk bms.

```mqttqos=```
```mqttretain=```
```mqttformat=```
Overrides the MQTT default for QOS, RETAIN or mqtt json format. Default are 0 bus can be specified via command line parameters or in the command line section of the config file. Can be set by MeterType as well.

```influxwritemult=```
Overrides the default from command line or config file. 0=Disable or >= 2.
2 means we will write data to influx on every second query. Values written can be the max, min value or the average. See Options (imax,imin,iavg)

```id=id```
specify the sunspec id to be used for the following register definitions, can be omitted if the id has already be defined via readid=

```settarif=tarifNum,startRegisterNumber_or_SunspecOffset,value,...```
Defines the sequence to be sent to the meter for setting the tariff 1,2,3 or 4.
tarifNum has to be 1,2,3 or 4
value is the value written to the specified register,  more values can be specified for the following registers. In case the same value has to be written multiple times numwrites*value can be specified, e.g. for 4 times 0:
```
settarif=1,0x300,0,0,0,0
settarif=1,0x300,4*0
```
```init=startRegisterNumber_or_SunspecOffset,value,...```
Defines register values sent for initialization. Value is the same as for settarif

```modbusdebug=1```
Enables libmodbus debug output for this meter.

```closeafterquery=1```
Closes the connection to a TCP meter after each query. This may be needed for devices that supports only one TCP connection (e.g. Siemens Logo) and the device must be queried by multiple masters.

### Register definitions within MeterTypes
for each register,
```"RegisterName"=startRegisterNumber_or_SunspecOffset```
 or
 ```"name"="Formula"```
 has to be specified.
Registers of this MeterType can be referenced within formulas by using its name, the following sample calculates the maximum of each phase voltage and saves the result in the new register uMax:
```
"uMax"="max(u1,u2,u3)"
```
 Additional, optional parameters may be specified (separated by comma). If no data type is specified, int16 will be assumed. A name must be specified with quotes to be able to use reserved words like "name". Example:
 ```
"p1" = 0x0012,int32,arr="p",dec=0,div=10,imax
"p2" = 0x0014,int32,arr="p",dec=0,div=10,imax
"p3" = 0x0016,int32,array="p",dec=0,div=10,imax
"p" = "p1+p2+p3",float,influx=0,iavg
"kwh"=0x0040,int32,force=int,div=10,imax
```

#### Options
Options - data types

 - _ _float_ _         - Floating point format (IEEE 754) = abcd
 - _ _floatabcd,floatbacd, floatcdab_ _ Floating point in different byte order
 - _ _int_ _ _ _int16_ _	- integer 16 bit
 - _ _int32_ _	- integer 32 bit
 - _ _int48_ _         - integer 48 bit
 - _ _int64_ _         - integer 64 bit
 - _ _uint_ _ _ _uint16_ _   - unsigned integer 16 bit
 - _ _uint32_ _        - unsigned integer 32 bit
 - _ _uint48_ _        - unsigned integer 48 bit
 - _ _uint64_ _        - unsigned integer 64 bit (only 63 bits will be stored)

```arr=name```

This option is applicable for mqtt only and will be ignored for influx. Values will be written as an array for fields with the same name e.g.
```
"PowerL1"=1,int16,arr=PWR
"PowerL2"=2,int16,arr=PWR
"PowerL3"=3,arr=PWR
```
will be written as "PWR":[-13, -72, -35]

```force=```
Can be int or float. Forces the data type to be saved to influxDB. (Interally all registers are stored as float)

```dec=```
Number of decimals, has to be >= 0

```div=```
Divider

```mul=```
Multiplier

```sf=```
Register offset for sunspec scaling factor

```formula="..."```
The result of the formula will be the new value. The current value as well as the values of other registers can be accessed by its name. Formulas will be executed after all registers within a meter has been read.

```influx=```
0 or 1, 0 will disable this register for Influxdb. The default if influx= is not specified is 1.

```mqtt=```
0 or 1, 0 will disable this register for Influxdb. The default if influx= is not specified is 1.
```grafana=```
0 or 1, 0 will disable this register for Grafana. The default if influx= is not specified is 1.

```imax```
```imin```
```iavg```
When using influxwritemult= , these options specify if the maximun, minimum or the average value will be written to influxdb.
### Virtual MeterTypes
Virtual meter types are types with formula fields only. These types are useful for testing when no physical modbus devices are available and you want to test writing to Influx/MQTT or Grafana. A global variable _polls reflecting the current query number as well as a random function is available.
Example:
```
# test for posting to local servers using virtual meter(s)
measurement=test1                                       # Influx default measuement
server=localhost                                        # Influx server
org=test                                                # Influx org
bucket=test1                                            # Influx bucket
tagname=Device                                          # # influx tag
token=xwrwerkhkwrh

ghost=ws://localhost                                    # Grafana host
gtoken=gkjfhbsjf																			s# admin token
gpushid=house                                           # push id

mqttserver=localhost                                    # MQTT host
mqttport=1883                                           # port
mqttprefix=ad/house/energy/                             # prefix

poll=5                                                  # poll seconds as an alternative to cron
                                                        # meter types
[MeterType]
name="Live1Type"                                        # name of the type, case sensitive
"value"="20+(rnd(1))",dec=2                             # some random
"count"="__polls",force=int,influx=0                    # values
"count_neg" = "__polls*-1",grafana=0,mqtt=0             # and the inverted one to influx only
influxwritemult=2

[Meter]
name="m1"                                               # the first meter named m1
type="Live1Type"
gname="test/m1"                                         # showing in Grafana as stream/house/test/m1

[Meter]
name="m2"                                               # showing in Grafana as stream/house/m2
type="Live1Type"
"value_m2"="20+(rnd(__polls))",dec=2                    # some field added to the meter type definition
```
When starting emModbus2influx with --dryrun or --dryrun=y, no connections or host name resolutions to Influx, MQTT or Grafana will be initiated so the specified hostnames do not matter at all. Results of the config file shown above:
```
./emModbus2influx --configfile=testlocal.conf --dryrun=2
```
```
mainloop started (emModbus2influx 1.10 Armin Diehl <ad@ardiehl.de> Aug 21,2023 compiled Sep  1 2023 21:19:08 )
performing initial query for all meters
Initial query took 0.00 seconds
Initial query: 2 meters

Dryrun: would send to influxdb:
test1,Device=m1 value=20.84,count_neg=0 1693596165235402135
test1,Device=m2 value=20.39,count_neg=0,value_m2=20.00 1693596165235402135

Dryrun: would send to grafana:
test/m1 value=20.84,count=0i 1693596165235488052
m2 value=20.39,count=0i,value_m2=20.00 1693596165235488052

Dryrun: would send to mqtt:
ad/house/energy/m1 {"name":"test1.m1", "value":20.84, "count":0}
ad/house/energy/m2 {"name":"test1.m2", "value":20.39, "count":0, "value_m2":20.00}
- 1 -----------------------------------------------------------------------
Query 2 took 0.00 seconds

Dryrun: would send to grafana:
test/m1 value=20.80,count=2i 1693596170051243413
m2 value=20.91,count=2i,value_m2=20.40 1693596170051243413

Dryrun: would send to mqtt:
ad/house/energy/m1 {"name":"test1.m1", "value":20.80, "count":2}
ad/house/energy/m2 {"name":"test1.m2", "value":20.91, "count":2, "value_m2":20.40}
- 2 -----------------------------------------------------------------------
terminated
```
Btw, the time send to Influxdb will be identical for all meters queried at one run.
# Meter definitions
Each meter definition starts with
```[Meter]```
where the [ has to be the first character of a line. Options that can be specified for a meter type are:

```name="NameOfMeter"```
Mandatory, name of the Meter. The name is used for InfluxDB as well as MQTT.

```iname="NameOfMeterForInfluxdb"```
By default, name will be used. However, if you want to write data to an other measurement using a name already in use by a meter, you can overwrite the name used for influxdb.

```type="NameOfMeterType"```
Type of the Meter, mandatory if modbus queries are required. The name is case sensitive and requires the MeterType to be defined in the config file before the meter definition. In case the meter consists of formulas only, MeterType is not required.

```address=SlaveAddress```
Modbus slave address, mandatory if modbus queries are required.

```hostname="HostnameOfModbusSlave"```
The hostname for Modbus-TCP slaves. If not specified, modbus-rtu will be used. All meters with the same hostname shares a TCP connection.

```Disabled=1```
1 will disable meter reads and writes, 0 will enable both. Defaults to 0 if not specified. Other options: read, write or readwrite.

```measurement="InfluxMeasurement"```
Overrides the default (or the value from the meter type) InfluxDB measurement for this meter.

```mqttqos=```
```mqttretain=```
```mqttformat=```
Overrides the MQTT default (or the value from the meter type) for QOS,RETAIN or mqtt json format. Defaults are 0 bus can be specified via command line parameters or in the command line section of the config file. Can be set by MeterType as well.

```influxwritemult=```
Overrides the default from command line, config file or meter type. 0=Disable or >= 2.
2 means we will write data to influx on every second query.

```"name"="Formula"```
Defines a virtual register. Registers of this or other meters can be accessed by MeterName.RegisterName. Formulas will be evaluated, in the sequence they appear in the config file, after all meters have been read. Sample for a virtual meter:
```
# "virtual" meter
[Meter]
name="virtual"
disabled=0
"u1_avg"="(Grid.u1+Grid.u2+Grid.u3)/3",dec=2,influx=1,mqtt=1
```
# Meter writes
Writing to modbus registers or coils can be scheduled or performed on each query. A write definition can perform multiple register / coil writes for one modbus device. The write definition can have a formula as a condition that will skip all registers in the definition. In addition, each register write definition can have a condition as well. Target for writes can be a variables of a meter formula as well.

```[Writes]```

where the [ has to be the first character of a line. Options that can be specified for write are:

```name="NameOfWrite"```

Mandatory, name of the write definition, used in debug / verbose outputs only.

```disabled="0|1"```

1 disables the write definitions.

```cond="formula"```

Overall condition formula for these writes. If result is <= 0, none of the write definitions within this [Writes] will be performed.

```schedule="```

Include in defined schedule

```meter="NameOfMeter"```

Mandatory, name of the defined meter (modbus device) to write to.

```write="RegName",intVal[,cond="formula"[,return]]```

```write="RegName",floatVal[,cond="formula"[,return]]```

```write="RegName","formula"[,cond="formula"[,return]]```

Unlimited number of write statements. RegName needs to be defined in the meter definition (via the meter type) and specifies the modbus register(s) and types. The value to write can be an integer, a float or a bit (>0 equals true) for a coil. An optional condition formula can be appended to avoid that single write (>0 will perform the write).
If return is given, no further write statements will be executed in case the write has been performed.

## Example
```
[Schedule]
"s_Logo" = "0 */30 * * * *"     # query and perfoerm writes every 30 minutes

# Siemens Logo for switching heat pump between solar and grid powered
[MeterType]
name = "Logo_HP"
measurement="logo"
type = coil
read = 0,4
"requestSolar"     = 0,influx=0,grafana=0,mqtt=0        # write only, 1=request solar
"requestHP"        = 1,influx=0,grafana=0,mqtt=0        # write only
"Status"           = 2,influx=1,grafana=1,mqtt=0        # 1=Solar, 0=HP
"EVUSperre"        = 3,influx=0,grafana=0,mqtt=0        # read only

[Meter]
type = "Logo_HP"
name = "logo"
address=255
hostname="logo2.armin.d"
schedule="s_Logo"

[Write]
name="LogoUpdate"
schedule="s_Logo"
meter="logo"
# always on solar if SOC (from Lynx Shunt) is > 80%
write="requestSolar",1,cond="Battery.SOC > 80",return

# leave it on solar until 50% SOC
write="requestSolar",1,cond="Battery.SOC > 50 && logo.Status == 1",return

# otherwise switch to grid
write="requestHP",1
```

# Getting started
## Test connectivity to InfluxDB and/or MQTT
First, check your connections to your mqtt and/or influxDB server. This can be done without a physical connection to a modbus device. We simply define a virtual device that generates random values using formulas.
Create this sample configuration file named "emModbus2influx.conf" in the same directory where the executable file is located or point to your configuration file with the --configfile= parameter.
```
# Influx server (influx write disabled if no server is given)
# server=YOUR INFLUX SERVER NASME OR IP

# influxdb v1 database name
# db=myDB

# influx v1 user and password (optional)
#user=myInfluxV1_user
#password=myInfluxV1_password

# Influxdb V2 API (do not specify db, user or password)
# organization
#org=YourOrg

# Influx bucket to use (like db in V1)
bucket=yourBucket

# name for the tag (not the value, value equals meter name), defaults to Meter=
# can be overridden per meter definition
tagname=Device

# access token (replaced user/password)
token=jksfhkjsfbsfhu

# MQTT
# hostname without protocol and port
mqttserver=your.mqtt.server
#mqttport=1883
mqttprefix=home/house/energy/

[Meter]
name="tempBasement_Simulation"
"temp"="20+(rnd(10)-5)",iavg,dec=1
```
Adjust your InfluxDB and/or MQTT server parameters. For InfluxDB 1.x you need database, username and password, for InfluxDB 2.x you need org, bucket (like database) and an access token than has to be generated using the InfluxDB gui (via http://influxdbhost:8086 by default)
You can now run emModbus2influx with --dryrun or --dryrun=count to see what would be posted to mqtt and/or influxDB and without --dryrun to post test data to your InfluxDB and/or MQTT.
## Define Modbus device
You need to know what Registers are available as well as the address and type of the register.
See the included emModbus2influx.conf for samples of several devices. This is my config that queries my Heatpump, Siemens Logo as well as the Victron System via TCP and Energy Meters, Heat Meters and my JK-NMS via Modbus RTU.






