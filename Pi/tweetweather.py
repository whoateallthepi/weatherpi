#!/usr/bin/env python3 

import sqlite3
import re
import time
import datetime
import math
import argparse
import sys
import os
import logging
from twitter import *
import wind

class Weather(object):
    'One class to do it all'
    def __init__(self, station_id, database, log_file, log_level, debug):
        self.station_id = station_id
        self.database = database
        self.log_file = log_file
        self.log_level = log_level
        self.debug = debug

        logger.debug('Get station details')       
        con = sqlite3.connect(self.database)
        with con:
            con.row_factory = sqlite3.Row
            cur = con.cursor()
            cur.execute("select * from station where id =?;",self.station_id)
            row = cur.fetchone()
        self.station_name = row['name']
        self.latitude = row['latitude']
        self.longitude = row['longitude']
        self.altitude = row['altitude']
                           
        logger.debug('Get latest reading') 
        with con:
            con.row_factory = sqlite3.Row
            cur = con.cursor()
            cur.execute("select * from reading group by station_id having time=max(time) and station_id=?;", args.station)
            row = cur.fetchone()

        self.weather_data = row   
        self.time = row['time']
        self.wind_dir = row['wind_dir']
        self.wind_speed = row['wind_speed']
        self.wind_gust = row['wind_gust']
        self.wind_gust_dir = row['wind_gust_dir']
        self.wind_speed_avg2m = row['wind_speed_avg2m']
        self.wind_dir_avg2m = row['wind_dir_avg2m']
        self.wind_gust_10m = row['wind_gust_10m']
        self.wind_gust_dir_10m = row['wind_gust_dir_10m']
        self.humidity = row['humidity']
        self.temperature = row['temperature']
        self.rain_1h = row['rain_1h']
        self.rain_today = row['rain_today']
        self.rain_since_last = row['rain_since_last']
        self.bar_uncorrected = row['bar_uncorrected']
        self.bar_corrected = row['bar_corrected']
        self.battery = row['battery']
        self.light = row['light']
        self.human_time = (datetime.datetime.strptime(self.time,'%Y-%m-%d %H:%M:%S')).strftime('%I:%M:%S%p on %d/%m/%y')

        if self.wind_speed_avg2m > 0:
            self.wind_rose = wind.rose(self.wind_dir_avg2m)
        else:
            self.wind_rose = 'Calm'
            
        self.human = (self.station_name + ' weather at ' + self.human_time +
                      ' Temp:{t:-0.1f}C'.format(t=self.temperature)+
                      ', Wind:{wd:0.0f}km/h'.format(wd=self.wind_speed_avg2m)+
                      ' from ' + self.wind_rose +
                      ', Rain last hr:{rt:0.1f}mm'.format(rt=self.rain_1h) +
                      ', RH:{hd:0.0f}%'.format(hd=self.humidity) +
                      ', Bar:{p:0.0f}mb'.format(p=self.bar_corrected)
                    )
                    
        logger.debug("Class Weather initialised")


#do the arguments

parser = argparse.ArgumentParser()
parser.add_argument("--debug", help="helps us debug",
                    action="store_true")
parser.add_argument("--prompt", help="Prompt for data collection rather than the loop",
                    action="store_true")
parser.add_argument("--station", help="Id of the station - default is '01'",
                    default='1')

parser.add_argument("--tries", help="Try the serial interface this many times before quitting",
                    type=int,
                    default = 3)

parser.add_argument("--log_file", help="Location of log file - defauits to /var/log/weather.log",
                    default = "/var/log/weather.log")

parser.add_argument("--log", help="log level - suggest <info> when it is working",
                    default="DEBUG")

parser.add_argument("--database", help="Location of database file - defauits to /home/pi/weather3/weather.db",
                    default = "/media/SANDISK/weather.db")

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

logger = logging.getLogger('tweetweather.py 1.00')

if args.debug:
    console_handler = logging.StreamHandler()
    logger.addHandler(console_handler)
    logger.debug('Running in debug mode')

logger.info('Starting')

if args.debug:
    logger.info("Debugging mode")

def tweet_latest():
    app_name = 'WeatherTweeting'
    consumer_key = 'sBVDJQDOOsJrPU55XE6NP6R1d'
    secret_key = 'YsOviGLjKf8As2aT3MIipz9fsjiAP3pY47L6m37HBNvKD9ajhr'

    my_credentials = os.path.expanduser('~/.weathertweeting_credentials')
    if not os.path.exists(my_credentials):
        oauth_dance(app_name, consumer_key, secret_key, my_credentials)

    oauth_token, oauth_secret = read_token_file(my_credentials)

    twitter = Twitter(auth=OAuth(oauth_token, oauth_secret, consumer_key, secret_key))

    logger.debug('getting weather from database')

    w = Weather(args.station, args.database, args.log_file, args.log, args.debug)

    logger.debug(w.human)
                      
    # Now work with Twitter
    twitter.statuses.update(status=w.human)

def object_test():
    logger.debug('Object test starts')
    w = Weather(args.station, args.database, args.log_file, args.log, args.debug)
    print(w.human_time, w.station_name, w.temperature)
    print(w.human)
    print(w.time)
    
    
def main():     
    tweet_latest()
      
main()
