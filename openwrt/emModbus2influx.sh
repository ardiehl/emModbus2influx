#!/bin/sh
# start/stop/restart for PRG running on openwrt access point halle
export PRG="emModbus2influx"
export ARGS="--syslog -d/dev/ttyUSB_epever"
export STARTDELAY=15

. ../lib/startstop.sh

