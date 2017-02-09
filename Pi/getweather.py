#!/usr/bin/env python3 
# 1.01 06-Jun-2016 Amended to make default station 2 - temporary fix!
#
import serial
import sqlite3
import re
import time
import datetime
import math
import argparse
import sys
import logging
from PIL import Image

# do the arguments

parser = argparse.ArgumentParser()
parser.add_argument("--debug", help="helps us debug",
                    action="store_true")

parser.add_argument("--power_saving", help="Power off the remote radio to save power",
                    action="store_true")

parser.add_argument("--prompt", help="Prompt for data collection rather than the loop",
                    action="store_true")
parser.add_argument("--station", help="Id of the station - default is '02'",
                    default='2')

parser.add_argument("--tries", help="Try the serial interface this many times before quitting",
                    type=int,
                    default = 3)

parser.add_argument("--log_file", help="Location of log file - defauits to /var/log/weather.log",
                    default = "/var/log/weather.log")

parser.add_argument("--log", help="log level - suggest <info> when it is working",
                    default="DEBUG")

parser.add_argument("--database", help="Location of database file - defauits to /home/pi/weather3/weather.db",
                    default = "/media/SANDISK/weather.db")

parser.add_argument("--frequency", help="mins between runs, defaults to 10 mins",
                    type = int,
                    default = 10)

args = parser.parse_args()

loglevel = args.log

numeric_level = getattr(logging, loglevel.upper(), None)
if not isinstance(numeric_level, int):
    raise ValueError('Invalid log level: %s' % loglevel)

# initialise logging
logging.basicConfig(
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s',
    datefmt='%d/%m/%Y %H:%M:%S',
    filename=args.log_file,
    level=numeric_level)

logger = logging.getLogger('getweather.py 1.01')

if args.debug:
    console_handler = logging.StreamHandler()
    logger.addHandler(console_handler)
    logger.debug('Running in debug mode')

logger.info('Starting')

if args.debug:
    logger.info("Debugging mode")

if args.prompt:
    logger.info("Will prompt for input")    
   

database = args.database
station_id = args.station

port = '/dev/ttyAMA0'
baud = 9600
timeout = 10.0
debug = args.debug

testing = False
#testing = True

def read_keys():
    target_action = input ('Enter ! for data, @  midnight reset, # reset,  q quit')
    return target_action

def parse_weather (raw_weather, altitude):
    weather_split = []
    temp_weather = re.split(r'[,\=]',raw_weather)
    # logger.debug(temp_weather)
    for i in range(2, (len(temp_weather)-1),2):
        weather_split.append(temp_weather[i])

    weather_split.insert(14, calculate_pressure(weather_split[13], altitude))
    weather_split.insert(0, station_id)
    """weather_split.insert(0, 'DEFAULT')""" 
    logger.debug(weather_split) 

    return weather_split

def calculate_pressure (bar_uncorrected, altitude):
    # Lifted from Nathan Seidle's C code for the Electric Imp - thanks
    #
    pressure_mb = float(bar_uncorrected) #/100
    # pressure is now in millibars
    part1 = pressure_mb - 0.3 # Part 1 of formula
    part2 = 8.42288 / 100000.0
    part3 = math.pow((pressure_mb - 0.3), 0.190284);
    part4 = altitude / part3
    part5 = (1.0 + (part2 * part4))
    part6 = math.pow(part5, (1.0/0.190284))
    bar_corrected = part1 * part6 # Output is now in adjusted millibars
    return '%.2f' % bar_corrected

def write_weather(weather):
    logger.debug('Writing weather to database')
    con = sqlite3.connect(database)
    cur = con.cursor()
    cur.execute("insert into reading values(datetime('now','localtime'),?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", weather)
    con.commit()
    con.close
        
def prompt_for_weather (ser, action, station_id):
    logger.debug("Pinging weather station %s" % station_id)
    
    for x in range (0, args.tries):
        logger.debug("Try %d",(x))
        ser.write(bytes(action + station_id,'utf-8'))
        logger.debug('Trying to read response')
        raw_weather = ser.readline().decode('utf-8')
        logger.debug('Received from weather station: %s', raw_weather)
        if raw_weather != "":
            # Do some basic validation - 16 = and 17 commas     
            if raw_weather.count('=') != 16 or raw_weather.count(',') != 18:
                logger.warning('Corrupt message from weather station %s: %s - will retry', station_id, raw_weather)
                raw_weather = '' 
                break
            else:
                break 

    if raw_weather == "":
        logger.error('No contact/corruption with weather station %s', station_id)
        return ""
    else:
        return raw_weather

def open_serial():
    logger.debug("Opening serial interface")

    ser = serial.Serial(port,baudrate=baud, timeout=timeout, writeTimeout=timeout)

    try:
        ser.open()
    except ser.SerialException:
        logger.error('Could not open port' % (self.s.port, e))
        raise SystemExit('Could not open port' % (self.s.port, e))
    return ser

def open_database ():
    try:
        con = sqlite3.connect(database)
        cur = con.cursor()
        cur.execute ('SELECT SQLITE_VERSION()')
        data = cur.fetchone()
        logger.debug('SQLite version %s', data)
    except sqlite3.Error:
        logger.error('Error: ', e.args[0])
        raise SystemExit('Error opening database %s %s : ', database, e.args[0])

    return cur
        
def get_station(station_id):

    cur = open_database()
    cur.execute ('select * from station where id=?', station_id)
    not_used, station_name, latitude, longitude, altitude, type, RFid, phone = cur.fetchone()           
    logger.debug('%s %s %s %s',station_name, latitude, longitude, altitude)
    cur.close
    return station_name, latitude, longitude, altitude
    
def main():     

    ser = open_serial()
       
    station_name, latitude, longitude, altitude = get_station(station_id)
                    
    while args.prompt:
        action = read_keys ()
        
        if action == 'q':
            break
        
        raw_weather = prompt_for_weather (ser, action, station_id)
            
        logger.debug (raw_weather)        

        if raw_weather != "":
            weather = parse_weather(raw_weather, altitude)
            write_weather (weather)

    while not args.prompt:
        now=datetime.datetime.now()       
        if now.hour == 0 and now.minute == 0 and (now.second < 30):
            action = "@"
        else:
            action = "!"

        raw_weather = prompt_for_weather (ser, action, station_id)

        if raw_weather != "":
            weather = parse_weather(raw_weather, altitude)
            write_weather (weather)

        now=datetime.datetime.now()
        seconds = int (now.second)
        minutes = int (now.minute) 
        minutes_wait = (args.frequency - (minutes % args.frequency)) - 1
        wait_for = (60 - seconds) + (60 * minutes_wait)

        logger.debug('Time: %s', now)
        logger.debug('Wait for: %s', wait_for)
        time.sleep(wait_for)
        
main()
