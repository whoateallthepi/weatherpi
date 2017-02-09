#!/usr/bin/env python3
#
# 1.01 6-Jun-2016 Temporary fix to amend default station id to 2
#
import sys
import os
import RPi.GPIO as GPIO
from PIL import Image
from PIL import ImageDraw
from PIL import ImageFont
from datetime import datetime
import time
from EPD import EPD
import argparse
import logging
import socket
import sqlite3

proc_name = 'showweather.py 1.01'


# Used to control cycling through the screens on button 1 
screen = 0
screens = ["0", "1", "2"]
time_stamp = time.time() #used for debouncing switch

WHITE = 1
BLACK = 0

#print(__file__)

# do the arguments

parser = argparse.ArgumentParser()
parser.add_argument("--debug", help="Logs messages to the console as well as the log file",
                    action="store_true")
parser.add_argument("--station", help="Id of the station - default is '2'",
                    default='2')
parser.add_argument("--logfile", help="Location of log file - defauits to /var/log/weather.log",
                    default = "/var/log/weather.log")
parser.add_argument("--log", help="log level - suggest <info> when it is working",
                    default="DEBUG")
parser.add_argument("--database", help="Location of database file - defaults to /media/SANDISK/weather.db",
                    default = "/media/SANDISK/weather.db")
parser.add_argument("--rose", help="Stem of the .png filenames for the wind rose graphics",
                    default = "/home/pi/weather/graphics/wr-")

parser.add_argument("--interval", help="Time in seconds between updates",
                    type=int,
                    default=60)

args = parser.parse_args()

loglevel = args.log

numeric_level = getattr(logging, loglevel.upper(), None)
if not isinstance(numeric_level, int):
    raise ValueError('Invalid log level: %s' % loglevel)

# initialise logging
logging.basicConfig(
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s', 
    datefmt='%d/%m/%Y %H:%M:%S',
    filename=args.logfile,
    level=numeric_level)

logger = logging.getLogger('showweather.py 1.01')

if args.debug:
    console_handler = logging.StreamHandler()
    logger.addHandler(console_handler)
    logger.debug('Running in debug mode')

logger.info('starting')

# fonts are in different places on Raspbian/Angstrom so search
possible_fonts = [
    #    '/usr/share/fonts/truetype/freefont/FreeMonoBold.ttf',            # Debian B.B
    '/usr/share/fonts/truetype/liberation/LiberationMono-Bold.ttf',   # Debian B.B
    '/usr/share/fonts/truetype/ttf-dejavu/DejaVuSansMono-Bold.ttf',   # R.Pi
    '/usr/share/fonts/truetype/freefont/FreeMono.ttf',                # R.Pi
    '/usr/share/fonts/truetype/LiberationMono-Bold.ttf',              # B.B
    '/usr/share/fonts/truetype/DejaVuSansMono-Bold.ttf',              # B.B
    '/usr/share/fonts/TTF/FreeMonoBold.ttf',                          # Arch
    '/usr/share/fonts/TTF/DejaVuSans-Bold.ttf'                        # Arch
]


FONT_FILE = ''
for f in possible_fonts:
    if os.path.exists(f):
        FONT_FILE = f
        break

if '' == FONT_FILE:
    raise 'no font file found'

logger.debug("Font chosen: %s", FONT_FILE)
TEMP_FONT_SIZE = 90
SECOND_FONT_SIZE = 20
ROSE_FONT_SIZE = 24
ROSE_DIR_FONT_SIZE = 14
STATION_FONT_SIZE = 16

# temperature
X_OFFSET = 10
Y_OFFSET = 10

# wind
WIND_X = 5
WIND_Y = 125

# Wet line
WET_X = 5
WET_Y = 100

# Rain
RAIN_X = 5
RAIN_Y = 150

#Rain2
RAIN2_X = 5
RAIN2_Y = 150

#freezing
FREEZE_X = 5
FREEZE_Y = 25
FREEZE_SIZE = 40

# Rose wind speed
ROSE_X = 194
ROSE_Y = 118
ROSE_DIR_X = 203
ROSE_DIR_Y = 140

#database
DATABASE = args.database

# Used to cycle through multiple stations
current_station_id = args.station

#global to calculate mean pressure over last 60 readings

pressures = []

epd = EPD()

def initialise_display ():

    epd = EPD()
    logger.debug(('panel = {p:s} {w:d} x {h:d}  version={v:s}  cog={g:d}'.format(p=epd.panel, w=epd.width, h=epd.height, v=epd.version, g=epd.cog)))

    if 'EPD 2.7' != epd.panel:
        logger.error('incorrect panel size')
        print('incorrect panel size')
        sys.exit(1)

    epd.clear()


def get_weather():
    logger.debug('getting weather from database')

    con = sqlite3.connect(DATABASE)
    with con:
        con.row_factory = sqlite3.Row
        cur = con.cursor()
        cur.execute("select * from reading  where station_id = ? order by time desc limit 1;", args.station)

        row = cur.fetchone()
        #for row in rows:         
        #    print "%s %s %s" % (row["station_id"], row["wind_dir"], row["wind_speed"])

        #logger.debug(row)
    return row

def get_pressure_trend (weather_data):
    current_pressure = weather_data["bar_corrected"]

    logger.debug('%s current pressure', current_pressure)

    pressures.append(current_pressure)

    while len(pressures) > 60:
        del pressures[0]
        
    logger.debug(pressures)

    # this is a least squares fit based on the formula here: http://stackoverflow.com/questions/10048571/python-finding-a-trend-in-a-set-of-numbershttp://stackoverflow.com/questions/10048571/python-finding-a-trend-in-a-set-of-numbers
    y = pressures
    N = len(y)
    if N > 9:
        x = list(range(N))
        B = (sum(x[i] * y[i] for i in range(N)) - 1./N*sum(x)*sum(y)) / (sum(x[i]**2 for i in range(N)) - 1./N*sum(x)**2)
        A = 1.*sum(y)/N - B * 1.*sum(x)/N
        logger.debug("Formula is y = %f + %f * x",A,B)
    else:
        B=0
        logger.debug ("no pressure trend < nine values")
        
    if B > .005:
        trend = "+"
    elif B < -0.005:
        trend = '-'
    else:
        trend = '='

    return trend

def produce_wind_rose (degrees, speed):
    # Needs recoding as a simple rotation!
    #
    # Returns a tuple of wind direction and file for rose image eg
    # "N","/home/pi/weather3/graphics/wr-0.png The imagfes should be stored in sequence,
    # clockwise from N starting at 0. wr-calm.png  should be there for 0 wind speeds.
    # The filename is generated from parameter --rose
    #
    # to work out the quadrant (or 16-ant), add 360 + 11.25  (to avoid -ve and N starts at 348.75 ) then split into 16 lots of 22.5 
    if speed > 0.49: # This will round up to one later
        quadrants = ["N",
                     "NNE",
                     "NE",
                     "ENE",
                     "E",
                     "ESE",
                     "SE",
                     "SSE",
                     "S",
                     "SSW",
                     "SW",
                     "WSW",
                     "W",
                     "WNW",
                     "NW",
                     "NNW",
                     "N"]

        big_degrees = 371.25  + degrees
        quadrant = int(big_degrees/22.5)
        #force into range 0-15, with 0 = N 
        if quadrant >= 16:
            quadrant -= 16
        if args.debug: 
            print(degrees, quadrant)    

        return quadrants[quadrant], args.rose + '{r:d}.png'.format(r=quadrant)
    else:
        return 'calm', args.rose + "calm.png"

def screen_change(channel):
    global screen  # globals as screen refreshed in main loop and interrupts
    global screens
    global time_stamp

    time_now = time.time()  
    logger.debug('button press at %d',time_now)
    logger.debug('last press at %d', time_stamp)
    if (time_now - time_stamp) > 7:
        # This is a valid press - something weird is going on - I think it is the switches....
        screen += 1
        logger.debug ('Screen: %s', screen)
        if screen == len(screens):
            screen = 0

        logger.debug("Switching to screen %s", screen)

        write_to_display(screens[screen])
        time_stamp = time_now
    else:
        logger.debug('button press ignored - looks like a bounce')    
    
def button2 (channel):    
    logger.debug ("button 2")

def setup_gpio():
    pin = 22
    logger.debug ("Attemting pin %d" % pin)
    GPIO.setmode(GPIO.BCM)
    GPIO.setup(pin, GPIO.IN)

    GPIO.add_event_detect(pin, GPIO.RISING, callback=screen_change, bouncetime=300)  

    pin = 17
    logger.debug("Attemting pin %d" % pin)
    
    GPIO.setmode(GPIO.BCM)
    GPIO.setup(pin, GPIO.IN)

    GPIO.add_event_detect(pin, GPIO.RISING, callback=button2, bouncetime=300)  

def do_diagnostic_screen ():
    logger.debug("do diagnostic screen")

def do_main_screen ():
    logger.debug("do main screen")

def do_summary_screen ():
    logger.debug("do summary screen")

def build_weather_screen(weather_data, trend):
    logger.debug('build_weather_screen starting')
    # initially set all white background
    image = Image.new('1', epd.size, WHITE)

    # prepare for drawing
    draw = ImageDraw.Draw(image)
    width, height = image.size

    temp_font = ImageFont.truetype(FONT_FILE, TEMP_FONT_SIZE)
    wind_font = ImageFont.truetype(FONT_FILE, SECOND_FONT_SIZE)
    wet_font = ImageFont.truetype(FONT_FILE, SECOND_FONT_SIZE)
    rose_font = ImageFont.truetype(FONT_FILE, ROSE_FONT_SIZE)
    rose_dir_font =  ImageFont.truetype(FONT_FILE, ROSE_DIR_FONT_SIZE)
    station_font = ImageFont.truetype(FONT_FILE, STATION_FONT_SIZE)

    # clear the display buffer
    draw.rectangle((0, 0, width, height), fill=WHITE, outline=WHITE)
    
    # Move single digit temperatures across screen
    if 0 < weather_data["temperature"] < 10:
        temp_line = ' {t:-0.1f}'.format(t=weather_data["temperature"])
    else:
        temp_line = '{t:-0.1f}'.format(t=weather_data["temperature"]) # + u"\u00B0"

# displaying of the temperature is done at the end to ensure it is on top of the weather rose
    

#   wind_speed =  '{wd:0.0f}'.format(wd=weather_data["wind_speed_avg2m"])
    
#   rose = produce_wind_rose (weather_data["wind_dir_avg2m"], weather_data["wind_speed_avg2m"])      
    
#    wind_line = "Wind:" + wind_speed + "km/h " + rose 
#   wind_dir = '{wd:d}'.format(wd=weather_data["wind_dir_avg2m"])
#   wind_line = "Wind:" + wind_speed + "km/h " + wind_dir + "\u00B0" 
    
#    draw.text((WIND_X, WIND_Y), wind_line, fill=BLACK, font=wind_font)

    humidity =  '{hd:0.0f}'.format(hd=weather_data["humidity"])

    hum_line = "Hum.:" + '{hd:0.0f}'.format(hd=weather_data["humidity"])+ "%"
    
    draw.text((WET_X, WET_Y), hum_line, fill=BLACK, font=wind_font) 

    pressure_line =  "Bar:"'{p:0.0f}'.format(p=weather_data["bar_corrected"]) + trend
    
    draw.text((WIND_X, WIND_Y), pressure_line, fill=BLACK, font=wind_font)

    rain_1h =  '{rt:0.1f}'.format(rt=weather_data["rain_1h"])
    rain_line = "Rain1h:" + rain_1h + "mm " 
    draw.text((RAIN_X, RAIN_Y), rain_line, fill=BLACK, font=wind_font)

    rose = produce_wind_rose (weather_data["wind_dir_avg2m"], weather_data["wind_speed_avg2m"])
        
    logger.debug("Wind rose %s %s", rose[0], rose[1])

    rose_img = Image.open(rose[1])
    image.paste(rose_img,(170,90))

    wind_speed_int = int (round (weather_data["wind_speed_avg2m"],0))
    #wind_speed_int = 100
    
    wind_print = '{wd:^3d}'.format(wd=wind_speed_int)

    #adjust centering for 2 digits
    if wind_speed_int > 9 and wind_speed_int < 99:
        nudge = 8
    else:
        nudge = 0
        
    draw.text((ROSE_X + nudge, ROSE_Y), wind_print, fill=BLACK, font=rose_font)

    # Add direction to rose
    if wind_speed_int > 0:
        if len(rose[0]) == 2:
               nudge = 5
        else:
               nudge = 0
               
        speed_print = '{wd:^3s}'.format(wd=rose[0])
        draw.text((ROSE_DIR_X + nudge, ROSE_DIR_Y),speed_print, fill=BLACK, font=rose_dir_font)
    else:
        draw.text((ROSE_DIR_X -5, ROSE_DIR_Y),'CALM', fill=BLACK, font=rose_dir_font)
        

    draw.text((X_OFFSET, Y_OFFSET), temp_line, fill=BLACK, font=temp_font)

    # draw a minus sign for sub-zero!
    
#    if weather_data ["temperature"] < 0:
#       sign_font = ImageFont.truetype(FONT_FILE, FREEZE_SIZE)
#      draw.text((FREEZE_X, FREEZE_Y), '-', fill=BLACK, font=sign_font) 

   # Add station name to top of screen

    text = get_station_name(current_station_id)
    draw.text((0,7), text.center(25,' '), fill=BLACK, font=station_font)

    return image

def build_summary_screen(weather_data):

    logger.debug("build summary screen")
    summary_font = ImageFont.truetype(FONT_FILE, SECOND_FONT_SIZE)
    positions = {"line1x": 0,
                 "line1y": 0,
                 "line2x": 0,
                 "line2y": 25,
                 "line3x": 0,
                 "line3y": 50,
                 "line4x": 0,
                 "line4y": 75,
                 "line5x": 0,
                 "line5y": 100,
                 "line6x": 0,
                 "line6y": 125 }

    #if it turns out this regular - will calculate it below!
             
    # initially set all white background
    image = Image.new('1', epd.size, WHITE)

    # prepare for drawing
    draw = ImageDraw.Draw(image)
    width, height = image.size

    # clear the display buffer
    draw.rectangle((0, 0, width, height), fill=WHITE, outline=WHITE)

    # line 1 - there must be a better way of coding this
    rain_today =  '{rt:0.1f}'.format(rt=weather_data["rain_today"])
    rain_line = "Rain today:" + rain_today + "mm " 
    draw.text((positions["line1x"],
               positions["line1y"]),
               rain_line,fill=BLACK,font=summary_font)
    # line 2
    wind_gust =  '{wd:0.0f}'.format(wd=weather_data["wind_gust"])
    wind_line = "Max wind gust:" + wind_gust + "km/h " 
    draw.text((positions["line2x"],
               positions["line2y"]),
               wind_line,fill=BLACK,font=summary_font)
    # line 3
    wind_gust_dir = '{wd:d}'.format(wd=weather_data["wind_gust_dir"])
    wind_gust_line = "Wind gust dir:" + wind_gust_dir + "\u00B0" 
    draw.text((positions["line3x"],
               positions["line3y"]),
               wind_gust_line,fill=BLACK,font=summary_font)

    #line 4
    con = sqlite3.connect(DATABASE)
    with con:
        con.row_factory = sqlite3.Row
        cur = con.cursor()
        cur.execute("select temperature from reading where station_id =? and date(time) = date('now') group by station_id having temperature =min(temperature);",args.station)
        row = cur.fetchone()
        cur.execute("select temperature from reading where station_id =? and date(time) = date('now') group by station_id having temperature =max(temperature);",args.station)
        row_max = cur.fetchone()
    
    con.close()
    
    draw.text((positions["line4x"],
               positions["line4y"]),
               "Today's min/max",fill=BLACK,font=summary_font)

    # it's possible that the above queries return no data if getdata.py has failed
    # and it is now past midnight - so check.
    
    if row is not None and  row_max is not None:
        temp_line = '{t:-0.1f}'.format(t=row["temperature"]) + "\u00B0" + "/" +'{t2:-0.1f}'.format(t2=row_max["temperature"]) + "\u00B0"
    else:
        temp_line = "No data!"
        logger.warn('No temperature data for today from weather station %s', args.station)
        
    draw.text((positions["line5x"],
               positions["line5y"]),
               temp_line,
               fill=BLACK,font=summary_font)

    """temp_line = '{t:-0.1f}'.format(t=weather_data["temperature"]) # + u"\u00B0"
    
    draw.text((X_OFFSET, Y_OFFSET), temp_line, fill=BLACK, font=temp_font)

    wind_speed =  '{wd:0.0f}'.format(wd=weather_data["wind_speed"])
    wind_dir = '{wd:d}'.format(wd=weather_data["wind_dir"])
    wind_line = "Wind:" + wind_speed + "km/h " + wind_dir + "\u00B0" 
    
    draw.text((WIND_X, WIND_Y), wind_line, fill=BLACK, font=wind_font)

    humidity =  '{hd:0.0f}'.format(hd=weather_data["humidity"])
    pressure =  '{p:0.0f}'.format(p=weather_data["bar_corrected"]) + trend

    wet_line = "Hum.:" + humidity + "%" + " Bar:" + pressure
    
    draw.text((WET_X, WET_Y), wet_line, fill=BLACK, font=wind_font) 

    rain_today =  '{rt:0.1f}'.format(rt=weather_data["rain_today"])
    rain_line = "Rain today:" + rain_today + "mm " 
    draw.text((RAIN_X, RAIN_Y), rain_line, fill=BLACK, font=wind_font)

    # draw a minus sign for sub-zero!
    
    if weather_data ["temperature"] < 0:
        sign_font = ImageFont.truetype(FONT_FILE, FREEZE_SIZE)
        draw.text((FREEZE_X, FREEZE_Y), '-', fill=BLACK, font=sign_font) 
"""
    return image

def build_diagnostic_screen():
    logger.debug('build_diagnostic_screen')
    diag_font = ImageFont.truetype(FONT_FILE, SECOND_FONT_SIZE)
    positions = {"line1x": 0,
                 "line1y": 0,
                 "line2x": 0,
                 "line2y": 25,
                 "line3x": 0,
                 "line3y": 50,
                 "line4x": 0,
                 "line4y": 75,
                 "line5x": 0,                 
                 "line5y": 100, 
                 "line6x": 0,
                 "line6y": 125, 
                 "line7x": 0,
                 "line7y": 150

}
                 
    # initially set all white background
    image = Image.new('1', epd.size, WHITE)

    # prepare for drawing
    draw = ImageDraw.Draw(image)
    width, height = image.size

    # clear the display buffer
    draw.rectangle((0, 0, width, height), fill=WHITE, outline=WHITE)

    # line 1 - there must be a better way of coding this
    #rain_today =  '{rt:0.1f}'.format(rt=weather_data["rain_today"])
    #rain_line = "Rain today:" + rain_today + "mm " 
   
    try:
        text = "IP: " +  [(s.connect(('8.8.8.8', 80)), s.getsockname()[0], s.close()) for s in [socket.socket(socket.AF_INET, socket.SOCK_DGRAM)]][0][1]    
    except socket.error:
        logger.warning('Could not access network')
        text  = 'IP: Not connected'

    draw.text((positions["line1x"],
               positions["line1y"]),
               text,fill=BLACK,font=diag_font)

    con = sqlite3.connect(DATABASE)
    with con:
        con.row_factory = sqlite3.Row
        cur = con.cursor()
        cur.execute("select time, battery from reading where station_id = ? order by time desc limit 1;",args.station)
        row = cur.fetchone()
        #for row in rows:         
        #    print "%s %s %s" % (row["station_id"], row["wind_dir"], row["wind_speed"])
    con.close()

    draw.text((positions["line2x"],
               positions["line2y"]),
               "Last reading: ",fill=BLACK,font=diag_font)
    
    draw.text((positions["line3x"],
               positions["line3y"]),
               row["time"],fill=BLACK,font=diag_font)

    text = 'Station voltage:'
    
    draw.text((positions["line4x"],
               positions["line4y"]),
               text,fill=BLACK,font=diag_font)

    draw.text((positions["line5x"],
               positions["line5y"]),
               "%0.2f" % (row['battery']),fill=BLACK,font=diag_font)

    return image

def write_to_display(image):
    epd.display(image)
    epd.update()

def get_station_name(station_id):
    con = sqlite3.connect(DATABASE)
    with con:
        con.row_factory = sqlite3.Row
        cur = con.cursor()
        cur.execute("select name from station where id =?;",station_id)
        row = cur.fetchone()
    con.close()
    return row['name']
        
def main ():
    global screens
    initialise_display ()   
    setup_gpio()
        
    while True:
        weather_data = get_weather()
        trend = get_pressure_trend(weather_data)
        screens[0] = build_weather_screen(weather_data, trend)
        screens[1] = build_summary_screen(weather_data)
        screens[2] = build_diagnostic_screen()
        
        write_to_display(screens[screen])
        
        time.sleep(args.interval)
main()
