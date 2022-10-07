# emModbus2influx
## read modbus RTU/TCP slaves and write to infuxdb (V1 or V2) and/or MQTT

### Definitions
**MeterType** - a definition of the registers,  register types and more of a specific modbus slave
**Meter** - a definition of a physical modbus slave based on a MeterType

It is named meter because it was originally used to query energy meters but in fact it can query any modbus slave. I'm using it for energy meters, Fronius solar inverters as well as Victron Energy GX.
### Features

 - modbus RTU via one serial port (if you need more serial ports, you can start emModbus2Influx multiple times)
 - unlimited number of metertypes and meters
 - supports SUNSPEC meter definitions (tested with Fronius Symo)
 - supports formulas for changing values after read or for defining new fields (in the metertype as well as in the meter definition)
 - supports dryrun for testing definitions
 - supports interactive formula testing
 - use of [libmodbus](https://libmodbus.org/) for modbus TCP/RTU communication
 - use of [paho-c](https://github.com/eclipse/paho.mqtt.c) for MQTT
 - use of [muparser](https://beltoforion.de/en/muparser/) for formula parsing
 - libmodbus,paho-c and muparser can by dynamic linked (default) or downloaded, build and linked static automatically when not available on target platform, e.g. Victron Energy Cerbox GX (to be set at the top of Makefile)

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
readid=103	# Inverter block type 103, length=50 (at least on my Symo)
"i"  = 2,uint16,sf=6
"p"  = 14,uint16,sf=15,force=int

[MeterType]
name = "DRT428M_3"	# same as ORNO OE-WE-517

# set tarif time 1 for tarif 1 at any time and disable other tarif times
init = 0x300,0,1,0*10
init = 0x030c,0*12
init = 0x0318,0*12
init = 0x0324,0*12
init = 0x0330,0*12
init = 0x033c,0*12
init = 0x0348,0*12
init = 0x0354,0*12

# set current meter tarif
# settarif = n, Address or sunspec offset, uint16 value [,uint16 value ...]
# n is in range of 1 to 4
settarif = 1,0x300,0,1
settarif = 2,0x300,0,2
settarif = 3,0x300,0,3
settarif = 4,0x300,0,4

# read voltage, current and power
read =0x0e,22

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
"kwh"=0x100,float,force=int

# energy for up to 4 tarifs
"kwh_1"=0x130,float,force=int,arr="kwh_t"
"kwh_2"=0x13c,float,force=int,arr="kwh_t"
"kwh_3"=0x148,float,force=int,arr="kwh_t"
"kwh_4"=0x154,float,force=int,arr="kwh_t"

[Meter]
disabled=0
address=11	# modbus RTU address of meter
type="DRT428M_3"
name="EnteryMeter1"

[Meter]
disabled=0
name="Symo"
hostname="fronius.armin.internal"
address=1
type="Fronuis_Symo"
```
If emModbus2Influx is started with --baud=19200 the 9600 baud in the config file will be ignored.
Comments can be included using #. Everything after # in a line will be ignored.
Numbers can be specified decimal or, when prefixwed with 0x, hexadecimal.

## command line options or options in the first section of the config file

Long command line options requires to be prefixed with -- while as in the config file the option has to be specified without the prefix. Short command line options can only be used on command line. The descriptions below show the options within the config file, if used on command line, a prefix of -- is required.

```
  -h, --help              show this help and exit
  --configfile=           config file name
  -d, --device=           specify serial device name
  --baud=                 baudrate (9600)
  -a, --parity=           N (default), E or O (e)
  -S, --stopbits=         1 or 2 stopbits (1)
  -4, --rs485=            set rs485 mode (0)
  -m, --measurement=      Influxdb measurement (energyMeter)
  -g, --tagname=          Influxdb tag name (Device)
  -s, --server=           influxdb server name or ip (lnx.armin.d)
  -o, --port=             influxdb port (8086)
  -b, --db=               Influxdb v1 database name
  -u, --user=             Influxdb v1 user name
  -p, --password=         Influxdb v1 password
  -B, --bucket=           Influxdb v2 bucket (test)
  -O, --org=              Influxdb v2 org (diehl)
  -T, --token=            Influxdb v2 auth api token
  --influxwritemult=      Influx write multiplicator
  -c, --cache=            #entries for influxdb cache (1000)
  -M, --mqttserver=       mqtt server name or ip (lnx.armin.d)
  -C, --mqttprefix=       prefix for mqtt publish (ad/house/energy/)
  -R, --mqttport=         ip port for mqtt server (1883)
  -Q, --mqttqos=          default mqtt QOS, can be changed for meter (0)
  -r, --mqttretain=       default mqtt retain, can be changed for meter (0)
  -v, --verbose[=]        increase or set verbose level
  -G, --modbusdebug       set debug for libmodbus
  -P, --poll=             poll interval in seconds
  -y, --syslog            log to syslog insead of stderr
  -Y, --syslogtest        send a testtext to syslog and exit
  -e, --version           show version and exit
  -D, --dumpregisters     Show registers read from all meters and exit, twice to show received data
  -U, --dryrun[=]         Show what would be written to MQQT/Influx for one query and exit
  -t, --try               try to connect returns 0 on success
  --formtryt=             interactive try out formula for register values for a given meter name
  --formtry               interactive try out formula (global for formulas in meter definition)
  --scanrtu               scan for modbus rtu devices (0)
```

### serial port for modbus RTU
```
device=/dev/ttyUSB0
baud=9600
parity=N
stopbits=1
rs485=0
```

Specify the serial port parameters, defaults are shown above. Parity can be N for none, E for even or O for odd.

### InfluxDB - common for version 1 and 2

```
server=ip_or_server_name_of_influxdb_host
port=8086
measurement=energyMeter
tagname=Meter
cache=1000
```

If server is not specified, post to InfluxDB will be disabled at all (if you would like to use MQTT only). tagname will be the tag used for posting to Influxdb. Cache is the number of posts that will be cached in case the InfluxDB server is not reachable. This is implemented as a ring buffer. The entries will be posted after the InfluxDB server is reachable again. One post consists of the data for all meters queried at the same time.
measurement sets the default measurement and can be overriden in a meter type or in a meter definition.

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
--bucket=
--org=
--token=
```

### MQTT

```
mqttserver=
mqttprefix=ad/house/energy/
mqttport=1883
mqttqos=0
mqttretain=0
```

Parameters for MQTT. If mqttserver is not specified, MQTT will be disabled at all (if you would like to use InfluxDB only).
mqttqos and mqttretain sets the default, these can be overriden per meter or MeterType definition.
__mqttqos__:
- At most once (0)
- At least once (1)
- Exactly once (2)

__mqttretain__:
- no (0)
- yes (1)

### additional options
```

verbose=0
syslog
modbusdebug
poll=5
```

__verbose__: sets the verbisity level
__syslog__: enables messages to syslog instead of stdout.
__modbusdebug__: enables debug output for libmodbus, can also be specified per meter
__poll__: sets the poll interval in seconds

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

```read=start,numRegs```
Optional: non SunSpec, specifies to read numReg (16 bit) registers starting at 'start'. After each read= statement, emModbus2Influx will try to map the reading data to the required registers. This is to avoid unnecessary reads especially for TCP.
If read= is not specified, a single read request for each register will be performed. This may, and for TCP it will, slowdown the overall read process.
There is no limit in the number of read= specified in the config file. After each read= has been performed, emModbus2Influx will try to map the data received by read= to the defined registers. In case there are remaining registers not mapped, single read commands for these registers will be performed.
To make sure all registers are covered by read= stements, start emModbus2influx with dryrun and verbosity level 1:
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

```mqttqos=```
```mqttretain=```
Overrides the MQTT default for QOS and RETAIN. Default are 0 bus can be specified via command line parameters or in the command line section of the config file. Can be set by MeterType as well.

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

### Register definitions within MeterTypes


for each register,
```"name"=startRegisterNumber_or_SunspecOffset```
 or
 ```"name"="Formula"```
 has to be specified. Registers of this MeterType can be referenced within formulas by using its name, the following sample calculates the maximum of each phase voltage and saves the result in the new register uMax:
```
"uMax"="max(u1,u2,3)"
```
 Additional, optional parameters may be specified (separated by comma). If no data type is specified, int16 will be assumed. A name must be specified with quotes to be able to use reserved words like "name". Example:
 ```
"p1" = 0x0012,int32,arr="p",dec=0,div=10,imax
"p2" = 0x0012,int32,arr="p",dec=0,div=10,imax
"p3" = 0x0012,int32,array="p",dec=0,div=10,imax
"p" = "p1+p2+p3",float,influx=0,iavg
"kwh"=0x0040,int32,force=int,div=10,imax
```

#### Options
Options - data types

 - float         - Floating point format (IEEE 754) = abcd
 - floatabcd,floatbacd, floatcdab Floating point in different byte order
 - int int16	- integer 16 bit
 - int32	- integer 32 bit
 - int48         - integer 48 bit
 - int64         - integer 64 bit
 - uint uint16   - unsigned integer 16 bit
 - uint32        - unsigned integer 32 bit
 - uint48        - unsigned integer 48 bit
 - uint64        - unsigned integer 64 bit (only 63 bits will be stored)

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
0 or 1, 0 will disable this register for influxdb. The default if influx= is not specified is 1.

```mqtt=```
0 or 1, 0 will disable this register for influxdb. The default if influx= is not specified is 1.


# Meter definitions
Each meter definition starts with
```[Meter]```
where the [ has to be the first character of a line. Options that can be specified for a meter type are:

```name="NameOfMeter"```
Mandatory, name of the Meter. The name is used for InfluxDB as well as MQTT.

```type="NameOfMeterType"```
Type of the Meter, mandatory if modbus queries are required. The name is case sensitive and requires the MeterType to be defined in the config file before the meter definition. In case the meter consists of formulas only, MeterType is not required.

```address="SlaveAddress"```
Modbus slave address, mandatory if modbus queries are required.

```hostname="HostnameOfModbusSlave"```
The hostname for Modbus-TCP slaves. If not specified, modbus-rtu will be used. All meters with the same hostname shares a TCP connection.

```Disabled=1```
1 will disable the meter, 0 will enable it. Defaults to 0 if not specified.

```measurement="InfluxMeasurement"```
Overrides the default (or the value from the meter type) InfluxDB measurement for this meter.

```mqttqos="```
```mqttretain="```
Overrides the MQTT default (or the value from the meter type) for QOS and RETAIN. Default are 0 bus can be specified via command line parameters or in the command line section of the config file. Can be set by MeterType as well.

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
Options supported are dec=, influx=, mqtt=, arr=, imax, imin and iavg
