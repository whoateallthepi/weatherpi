/*
 Weather Station using the RFU328
 By: Tom Cooper based on work by Nathan Seidle
 To Do: Code in the check for station id
 This version is reconfigged to use BMP180 pressure sensor
 2.0x version is a major reworked to use LLAP comms protocol
 2.01 21/01/2016 - First version ready for unit testing
*/
//#define DEBUG
//#define TRACE
#include <avr/wdt.h> //We need watch dog for this program
//#include <SoftwareSerial.h> //Connection to Imp
#include <string.h> 
#include <Wire.h> //I2C needed for sensors
#include <stdio.h> //for sprintf()
#include <math.h>  // for trig functions

#include "HTU21D.h" //Humidity sensor
#include <SFE_BMP180.h> //Pressure and temperature sensor

#include <EEPROM.h>
#include <LLAPSerial.h>

#define VERSION "2.01-----" //Extra chars are the LLAP filler
#define DEVICETYPE "WSTAT-----"
#define DEVICEID1 '-'
#define DEVICEID2 '-'
#define EEPROM_DEVICEID1 0
#define EEPROM_DEVICEID2 1
#define LLAP_MESSAGE_SIZE 9 // This is the size of the message payload 9 see LLAP standard
#define INITIAL "STARTED--"  //  First message on load
#define LLAP_FILLER "---------"
#define LLAP_COMMAND_CT 20
#define DECIMAL_PLACES 1
#define PRESSURE 'P'
#define TEMPERATURE 'T'
/* The structure below is used to parse the incoming messages
   pf is a pointer to the function used for each message.
   The functions all take the message string as a parameter and
   return it as the correctly formatted reply. In the case of 
   control functions this is often just an echo. In other cases,
   such as TEMP, there is some processing involved.
   To add new messages, include them in the structure, write a 
   function void xxx(char *) and increase LLAP_COMMAND_CT
*/   

struct LLAP_command {
  char command[LLAP_MESSAGE_SIZE+1];
  void (*pt_funct)(char *);
};

struct LLAP_command LLAP_commands[LLAP_COMMAND_CT] = {
  "HELLO----",  helloMessage,           //00
  "DEVTYPE--",  devtypeMessage,         //01  If you change control messages numbers - check code in function 
  "FVER-----",  fverMessage,            //02
  "SAVE-----",  saveMessage,            //03
  "BATT-----",  battMessage,            //04
  "MIDNIGHT-",  midnightMessage,        //05
  "TEMP-----",  tempMessage,            //06
  "HUM------",  humidityMessage,        //07
  "RN1H-----",  rain1hMessage,          //08
  "RND------",  raintodayMessage,       //09
  "RNSI-----",  rainSinceLastMessage,   //10 - thinking of deprecating this
  "WDSP-----",  windspeedMessage,       //11
  "WDDI-----",  winddirMessage,         //12
  "WDGU-----",  windgustMessage,        //13
  "WDGD-----",  windgustdirMessage,     //14
  "WDS2-----",  windspeed2mMessage,     //15
  "WDD2-----",  winddir2mMessage,       //16
  "WDG10----",  windgust10mMessage,     //17
  "WDGD10---",  windgustdir10mMessage,  //18
  "BAR------",  barMessage              //19
};

char msg [LLAP_MESSAGE_SIZE + 1];      // storage for incoming message plus null character
char reply [LLAP_MESSAGE_SIZE + 1];    // storage for reply plus null character

SFE_BMP180 myPressure; //Create an instance of the pressure sensor
// Making humidity local - pressure stays global and initialised in setup
// as also used by temperature code. Plus it is probably the most called code
// HTU21D myHumidity; //Create an instance of the humidity sensor

//Hardware pin definitions
//-------------------------------------------------------------------------------------------
// digital I/O pins

#define WSPEED 3
#define RAIN   2
#define STAT1  7
#define RADIO  8 //Pin for switching on the Xino RF radio
#define LED    10 // Proof of life

// analog I/O pins
#define WDIR A1

//-------------------------------------------------------------------------------------------

//Global Variables - probably could do with some pruning
///-------------------------------------------------------------------------------------------

int days_running; //Used to reset every 30 days
long lastSecond; //The millis counter to see when a second rolls by
byte seconds; //When it hits 60, increase the current minute
byte seconds_2m; //Keeps track of the "wind speed/dir avg" over last 2 minutes array of data
byte minutes; //Keeps track of where we are in various arrays of data
byte minutes_10m; //Keeps track of where we are in wind gust/dir over last 10 minutes array of data

long lastWindCheck = 0;
volatile long lastWindIRQ = 0;
volatile byte windClicks = 0;

//We need to keep track of the following variables:
//Wind speed/dir each update (no storage)
//Wind gust/dir over the day (no storage)
//Wind speed/dir, avg over 2 minutes (store 1 per second)
//Wind gust/dir over last 10 minutes (store 1 per minute)
//Rain over the past hour (store 1 per minute)
//Total rain over date (store one per day)

//TODO: Review these sizes
char windspdavg[120]; //120 bytes to keep track of 2 minute average
char winddiravg[120]; //120 bytes to keep track of 2 minute average - in sectors 0-15
char windgust_10m[10]; //10 floats to keep track of largest gust in the last 10 minutes
char windgustdirection_10m[10]; //10 byte to keep track of 10 minute max
volatile float rainHour[60]; //60 floating numbers to keep track of 60 minutes of rain

// TO DO: Look at moving these into subroutines
int wind_dir; // [0-360 instantaneous wind direction]
float windspeed; // [mph instantaneous wind speed]
float windgust; // [mph current wind gust, using software specific time period]
int windgustdir; // [0-360 using software specific time period]
// float windspd_avg2m; // [mph 2 minute average wind speed mph] - no longer global
int wind_dir_avg2m; // [0-360 2 minute average wind direction]- Maybe byte??

volatile float rain_today; // [rain mm so far today in local time]
volatile float rain_since_last; //needed for a 'last 24h' calculation in server
//float baromin = 30.03;// [barom in] - It's hard to calculate baromin locally, do this in the agent
//float pressure;
//float dewptf; // [dewpoint F] - It's hard to calculate dewpoint locally, do this in the agent

//These are not output in this version of the hardware - but leaving it in anyway
//float batt_lvl = 11.8;
//float light_lvl = 0.72;

// volatiles are subject to modification by IRQs
volatile unsigned long raintime, rainlast, raininterval, rain;

//-------------------------------------------------------------------------------------------

//Interrupt routines (to do - move these and use prototypes)
//-------------------------------------------------------------------------------------------

void rainIRQ()
// Count rain gauge bucket tips as they occur
// Activated by the magnet and reed switch in the rain gauge, attached to input D2
{
  raintime = millis(); // grab current time
  raininterval = raintime - rainlast; // calculate interval between this and last event

  if (raininterval > 10) // ignore switch-bounce glitches less than 10mS after initial edge
  {
    rain_since_last += 0.2794;
    rain_today += 0.2794; //Each dump is 0.2794 mm of water
    rainHour[minutes] += 0.2794; //Increase this minute's amount of rain

    rainlast = raintime; // set up for next event
  }
}

void wspeedIRQ()
// Activated by the magnet in the anemometer (2 ticks per rotation), attached to input D3
{
  if (millis() - lastWindIRQ > 10) // Ignore switch-bounce glitches less than 10ms (142MPH max reading) after the reed switch closes
  {
    lastWindIRQ = millis(); //Grab the current time
    windClicks++; //There is 1.492MPH for each click per second.
  }
}

//----------------------------------Initialisation--------------------------------------------

void setup()
{
  // wdt_reset(); //Pet the dog
  // wdt_disable(); //We don't want the watchdog during init
  days_running = 0;

  pinMode(LED, OUTPUT);
  digitalWrite(LED, HIGH);
  pinMode(RADIO, OUTPUT); //switch on the radio
  digitalWrite(RADIO, HIGH);
  Serial.begin(115200);

  pinMode(WSPEED, INPUT_PULLUP); // input from wind meters windspeed sensor
  pinMode(RAIN, INPUT_PULLUP); // input from wind meters rain gauge sensor

  pinMode(WDIR, INPUT);
  //pinMode(LIGHT, INPUT);
  //pinMode(BATT, INPUT);
  //pinMode(REFERENCE_3V3, INPUT);

  //pinMode(STAT1, OUTPUT);

  midnightReset(); //Reset rain totals

  myPressure.begin(); // crank up the pressure sensor

  //myHumidity.begin(); //Configure the humidity sensor

  seconds = 0;
  lastSecond = millis();

  // attach external interrupt pins to IRQ functions
  attachInterrupt(0, rainIRQ, FALLING);
  attachInterrupt(1, wspeedIRQ, FALLING);

  // turn on interrupts
  interrupts();

  #ifdef DEBUG
  Serial.println("DEBUG>> Station online!");
  #endif
  
  // reportWeather();

  // Get cracking on LLAP
  String permittedChars = "-#@?\\*ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  char deviceID[2];
  deviceID[0] = EEPROM.read(EEPROM_DEVICEID1);
  deviceID[1] = EEPROM.read(EEPROM_DEVICEID2);

  if (permittedChars.indexOf(deviceID[0]) == -1 || permittedChars.indexOf(deviceID[1]) == -1)
  {
    deviceID[0] = DEVICEID1;
    deviceID[1] = DEVICEID2;
  }

  LLAP.init(deviceID);

  LLAP.sendMessage(INITIAL);


  // wdt_enable(WDTO_1S); //Unleash the beast
}

//---------------------------------------Main loop---------------------------------------------
void loop()
{
  // digitalWrite(LED, LOW);

  #ifdef TRACE
  Serial.println("TRACE>> starting main loop");
  #endif
  
  //char status;
  int rc = 0;
  // wdt_reset(); //Pet the dog

  updateWind(); // 
  
  //Keep track of which minute it is
  if (millis() - lastSecond >= 1000)
  {
    updateWind(); // We are running this once a second - hopefully. I might slow it down
    lastSecond += 1000;
  }

  //Here we go with the LLAP stuff
  #ifdef TRACE
  Serial.println("TRACE>> check LLP");
  #endif
  
  if (LLAP.bMsgReceived) // got a message?
  {
    LLAP.bMsgReceived = false;
    processLLAPMessage();
  }

  delay(100); //Update every 100ms. No need to go any faster. Query this?   

} 
//----------------------------------- end of main loop ------------------------------- 

void updateWind()
{
  // This is the chief recording section and is called from the main loop, 
  // as the wind counter (via interrupts) needs to be written into 
  // tables for 10m and 2m averages. Similarly for directions
    
  //Take a speed and direction reading every second for 2 minute average
  //
  if (++seconds_2m > 119) seconds_2m = 0;

  //Calc the wind speed and direction every second for 120 second to get 2 minute average
  windspeed = get_wind_speed();
  wind_dir = get_wind_direction();
  windspdavg[seconds_2m] = (int)windspeed;
  winddiravg[seconds_2m] = wind_dir; // Remeber we are storing this in sectors now
  //if(seconds_2m % 10 == 0) displayArrays();

  //Check to see if this is a gust for the minute
  if (windspeed > windgust_10m[minutes_10m])
  {
    windgust_10m[minutes_10m] = windspeed;
    windgustdirection_10m[minutes_10m] = wind_dir;
  }

  //Check to see if this is a gust for the day
  //Resets at midnight each night
  if (windspeed > windgust)
  {
    windgust = windspeed;
    windgustdir = wind_dir;
  }

  //Blink stat LED briefly to show we are alive
  digitalWrite(LED, HIGH);
  //reportWeather(); //Print the current readings. Takes 172ms.
  delay(25);
  digitalWrite(LED, LOW);

  //If we roll over 60 seconds then update the arrays for rain and windgust
  if (++seconds > 59)
  {
    seconds = 0;

    if (++minutes > 59) minutes = 0;
    if (++minutes_10m > 9) minutes_10m = 0;

    rainHour[minutes] = 0; //Zero out this minute's rainfall amount
    windgust_10m[minutes_10m] = 0; //Zero out this minute's gust
  }
}

void processLLAPMessage ()
{
  // This is the second part of the main loop - dealing with messages from the controller
  // (the Raspberry Pi)
    
  int command_num;
  LLAP.sMessage.toCharArray(msg, LLAP_MESSAGE_SIZE + 1);
  msg[LLAP_MESSAGE_SIZE] = '\0';  
  
  #ifdef DEBUG
  Serial.print("LLAP message received: ");
  Serial.println(msg);
  #endif  
    
  strlcpy(reply,msg,LLAP_MESSAGE_SIZE); // by default setup to echo the incoming message 

  // Step through LLAP_commands to work out which function to call
  
     
 /*   while ( !strncmp(msg, LLAP_commands[command_num].command,LLAP_MESSAGE_SIZE) && command_num <= LLAP_COMMAND_CT)
    command_num ++; Would work - but doing below for debug
 */   
  for (command_num = 0; command_num <= LLAP_COMMAND_CT; command_num++)
  {
    #ifdef DEBUG
    Serial.println(LLAP_commands[command_num].command);
    Serial.println(msg);
      
    Serial.print("Command num: ");
    Serial.println(command_num);
    //Serial.print("rc: ");
    //Serial.println(rc);
    #endif
    
    if (!strncmp(msg, LLAP_commands[command_num].command,LLAP_MESSAGE_SIZE))
    {
      #ifdef DEBUG
      Serial.println("Breaking");
      #endif
      break;
    }      
  }

  if (command_num >= LLAP_COMMAND_CT) 
    strlcpy(reply,"ERROR1000",LLAP_MESSAGE_SIZE+1); // 1000 = unrecognised command
  else
  {
    //void (*pf) (char *);
    // pf=*(LLAP_commands[command_num].pt_funct)
    LLAP_commands[command_num].pt_funct(reply);    // Calls the appropriate function to set up reply     
  }
  
  #ifdef TRACE
  Serial.println("TRACE>> Sending LLAP");
  #endif
  LLAP.sendMessage(reply);
}  


void formatFloat (int commandLen, float in_value, char * reply)
{
  // Formats a float into the output reply string - assumes DECIMAL_PLACES decimal places
  // The length of a string is 2 chars for the point and the DECIMAL_PLACES, plus one if it's negative (for sign) and then one for each decimal
  // digit
  
  #ifdef TRACE
  Serial.println("TRACE>> formatFloat()");
  #endif
  
  char formatted [LLAP_MESSAGE_SIZE]; // bigger than necessary - but ok
  int strf_len = DECIMAL_PLACES +1; //start with 9.1 etc then nudge around 

  if (in_value < 0)
    strf_len++;
  if ((in_value >= 10) && (in_value <= -10))
    strf_len++;
  if ((in_value >= 100) || (in_value <= -100))
    strf_len++;
  if ((in_value >= 1000) || (in_value <= -1000))
    strf_len++;  
  
  dtostrf(in_value,strf_len,DECIMAL_PLACES,formatted);  
  strcpy(reply + commandLen,formatted);
  fillLLAPcmd(reply); // Adds any ----- fillers needed at the end of string
}

// ========================================= Personality functions start here ==============================

void helloMessage(char * reply)
{
  #ifdef TRACE
  Serial.println("TRACE>> helloMessage");
  #endif
  
  return; // just echo it back!
}

void fverMessage(char * reply)
{
 strlcpy(reply+4,VERSION,LLAP_MESSAGE_SIZE-4);
}

void devtypeMessage(char * reply)
{
  strlcpy(reply+7,DEVICETYPE,LLAP_MESSAGE_SIZE-6);
}          

void saveMessage(char * reply)
{
  EEPROM.write(EEPROM_DEVICEID1, LLAP.deviceId[0]);    // save the device ID
  EEPROM.write(EEPROM_DEVICEID2, LLAP.deviceId[1]);    // save the device ID
}

void midnightMessage (char * reply)
{
  midnightReset();
}

void battMessage(char * reply)
{
  //No code yet!
}

void tempMessage(char * reply)
{
  
  //#ifdef TRACE
  Serial.println("TRACE>> tempMessage()");
  //#endif
  
  const int cmdlen = 4; // "TEMP"
     
  double T = getTemperatureorPressure(TEMPERATURE);

  formatFloat (cmdlen, float(T), reply);
  
}

void barMessage(char * reply)
{
  
  //#ifdef TRACE
  Serial.println("TRACE>> barMessage()");
  //#endif
  
  const int cmdlen = 4; // "BAR"
     
  double T = getTemperatureorPressure(PRESSURE);

  formatFloat (cmdlen, float(T), reply);
  
}


double getTemperatureorPressure (char type)
{
  // Ideally temperatire and pressure would be called together and returned as a 
  // pair - but we'll do it this way for now
  
  //#ifdef TRACE
  Serial.println("TRACE>> barMessage()");
  //#endif
  
  double T = 0;
  double P = 0;
  float t2 = 0;
  char temperature [LLAP_MESSAGE_SIZE]; // shrink this if memory is an issue
  char status = myPressure.startTemperature();
  if (status != 0)
  {
    // Wait for the measurement to complete:
    delay(status);

    // Retrieve the completed temperature measurement:
    // Note that the measurement is stored in the variable T.
    // Function returns 1 if successful, 0 if failure.

    status = myPressure.getTemperature(T);
    if (status != 0)
    {
      #ifdef DEBUG
      Serial.print("temperature: ");
      Serial.print(T,2);
      Serial.println(" deg C, ");
      #endif
      
      if (type = PRESSURE)
        return T;
      
      // Go on and get the pressure - otherwise we return the temperature
      status = myPressure.startPressure(3);
      if (status != 0)
      {
        // Wait for the measurement to complete:
        delay(status);

        // Retrieve the completed pressure measurement:
        // Note that the measurement is stored in the variable P.
        // Note also that the function requires the previous temperature measurement (T).
        // (If temperature is stable, you can do one temperature measurement for a number of pressure measurements.)
        // Function returns 1 if successful, 0 if failure.

        status = myPressure.getPressure(P, T);
        if (status != 0)
        {
        // Print out the measurement:
        //Serial.print("absolute pressure: ");
        //Serial.print(P,2);
        //Serial.print(" mb, ");
        //Serial.print(P*0.0295333727,2);
        //Serial.println(" inHg");
        }
      }
    }
  }
  return P;
}

void humidityMessage(char * reply)
{
  #ifdef TRACE
  Serial.println("TRACE>> hunidityMessage()");
  #endif
  const int cmdlen = 3; // "HUM"
  HTU21D myHumidity;
  
  myHumidity.begin(); //Configure the humidity sensor
    
  formatFloat (cmdlen, myHumidity.readHumidity(), reply);
}

void rain1hMessage(char * reply)
{
  #ifdef TRACE
  Serial.println("TRACE>> rain1hMessage()");
  #endif
  const int cmdlen = 4; // "RN1H"
  float r1 = 0;
   
  for (int i = 0 ; i < 60 ; i++)
       r1 += rainHour[i];
  
  formatFloat (cmdlen, r1, reply);
}
  
void raintodayMessage(char * reply)
{
  #ifdef TRACE
  Serial.println("TRACE>> raintodayMessage()");
  #endif
  
  const int cmdlen = 3; // "RND"
  
  formatFloat (cmdlen, rain_today, reply);
  // The output string should now be set up.  
}

void rainSinceLastMessage(char * reply)
{
  #ifdef TRACE
  Serial.println("TRACE>> rainsinceLastMessage()");
  #endif
  
  const int cmdlen = 4; // "RNSI"
  float rs = rain_since_last; // that's global - updated by interrupt
  rain_since_last = 0; //reset straight away - it's updated by interrupt
  formatFloat (cmdlen, rs, reply);
  // The output string should now be set up.  
}

void windspeedMessage(char * reply)
{
  // The latest reading of wind speed is in windspeed - this is updated in the main loop
  // to maintain the tables for minute averages etc
  #ifdef TRACE
  Serial.println("TRACE>> windspeedMessage()");
  #endif
  
  const int cmdlen = 4; // "WDSP"
  
  formatFloat (cmdlen, windspeed, reply);
  // The output string should now be set up.  
}  
  
void winddirMessage (char * reply)
{
  //This one is a bit different - as direction is an int and we use 
  //get_wind_direction elsewhere 
  // The last reading of wind direction is stored in wind_dir - we need to do that 
  // in the main loop to maintain all the averaging arrays. All we do here is convert to 
  // degrees from the 0-15 sectors format we use to store the data.
  //
  const int cmdlen = 4; // "WDDI"
               
  sprintf(reply+cmdlen, "%d", sectorstoDegrees(wind_dir));
  
  fillLLAPcmd(reply); // Adds any ----- fillers needed at the end of string
}

void windgustMessage (char * reply)
{
  #ifdef TRACE
  Serial.println("TRACE>> windgustMessage()");
  #endif
  
  const int cmdlen = 4; // "WDGU"
  
  formatFloat (cmdlen, windgust, reply);
  // The output string should now be set up.  
}

void windgustdirMessage (char * reply)
{
  #ifdef TRACE
  Serial.println("TRACE>> windgustdirMessage()");
  #endif
  
  const int cmdlen = 4; // "WDGD"
  
  sprintf(reply+cmdlen, "%d", sectorstoDegrees(windgustdir));
  
  fillLLAPcmd(reply); // Adds any ----- fillers needed at the end of string
  
}

void windspeed2mMessage(char * reply)
{
  // Average wind speed for past two minutes - 
  #ifdef TRACE
  Serial.println("TRACE>> windspeed2mMessage()");
  #endif
  
  const int cmdlen = 4; // "WDS2"
  
  float windspd_avg2m = 0;
  for (int i = 0 ; i < 120 ; i++)
    windspd_avg2m += windspdavg[i];
  windspd_avg2m /= 120.0;
    
  formatFloat (cmdlen, windspd_avg2m, reply);
  // The output string should now be set up.  
}

void winddir2mMessage (char * reply)
{
  //To do this realistically we need a vector average
  //
  const int cmdlen = 4; // "WDD2"
  
  float rad = vectorAverage(winddiravg, windspdavg, 120); 
               
  sprintf(reply+cmdlen, "%d", radiansToDegrees(rad));
  
  fillLLAPcmd(reply); // Adds any ----- fillers needed at the end of string
}

void windgust10mMessage (char * reply)
{
  // To think about - Idealy we'd call direction and speed together as the data could 
  // shift between calls - hmmm
  //
  const int cmdlen = 5; // "WDG10"
  float windgustkmh_10m = 0;
  for (int i = 0; i < 10 ; i++)
  {
    if (windgust_10m[i] > windgustkmh_10m)
    {
      windgustkmh_10m = windgust_10m[i];
//    windgustdir_10m = windgustdirection_10m[i];
    }
  }
  
  formatFloat (cmdlen, windgustkmh_10m, reply);
  // The output string should now be set up.
  
}
 
void windgustdir10mMessage (char * reply)
{
  // To think about - Idealy we'd call direction and speed together as the data could 
  // shift between calls - hmmm
  //
  const int cmdlen = 6; // "WDGD10"
  float windgustkmh_10m = 0;
  float windgustdir_10m = 0;
  
  for (int i = 0; i < 10 ; i++)
  {
    if (windgust_10m[i] > windgustkmh_10m)
    {
      windgustkmh_10m = windgust_10m[i];
      windgustdir_10m = windgustdirection_10m[i];
    }
  }
  
  sprintf(reply+cmdlen, "%d", sectorstoDegrees(windgustdir_10m));
  
  fillLLAPcmd(reply); // Adds any ----- fillers needed at the end of string
  
}    



int sectorstoDegrees(int sectors)
{
  return (int)((sectors * 22.5) +0.5);
}

int radiansToDegrees (float radians)
{
  return (int)((radians *(180/PI)) +0.5);
}

//Prints the various arrays for debugging

#ifdef ARRAYDEBUG
void displayArrays()
{
  //Windgusts in this hour
  Serial.println();
  Serial.print(minutes);
  Serial.print(":");
  Serial.println(seconds);

  Serial.print("Windgust last 10 minutes:");
  for (int i = 0 ; i < 10 ; i++)
  {
    if (i % 10 == 0) Serial.println();
      Serial.print(" ");
      Serial.print(windgust_10m[i]);
  }

  //Wind speed avg for past 2 minutes
  /*Serial.println();
  Serial.print("Wind 2 min avg:");
  for(int i = 0 ; i < 120 ; i++)
  {
    if(i % 30 == 0) Serial.println();
      Serial.print(" ");
      Serial.print(windspdavg[i]);
  }*/

  //Rain for last hour
  Serial.println();
  Serial.print("Rain hour:");
  for (int i = 0 ; i < 60 ; i++)
  {
    if (i % 30 == 0) Serial.println();
      Serial.print(" ");
      Serial.print(rainHour[i]);
   }

 }
#endif

//In the midnight hour ...
void midnightReset()
{
  days_running ++;
//  if (days_running > 30) delay(5000); //This will cause the system to reset because we don't pet the dog
    
    rain_today = 0; //Reset daily amount of rain
    windgust = 0; //Zero out the windgust for the day
    windgustdir = 0; //Zero out the gust direction for the day
    minutes = 0; //Reset minute tracker
    seconds = 0;
    lastSecond = millis(); //Reset variable used to track minutes

}

void fillLLAPcmd (char * reply)
//
// reply_len is the number of chars in reply as it is currrently formatted (less the null)
// This function adds the requred number of LLAP_FILLER characters to reply to make it LLAP_MESSAGE_SIZE
// Returns a string including the null character - up to 10 chars in total
//
{
  int reply_len = strlen (reply);
  
  if (reply_len > LLAP_MESSAGE_SIZE) {
  // This is an error - have a think about what to do
  }
  strncat(reply,LLAP_FILLER, (LLAP_MESSAGE_SIZE - reply_len));
}

//Returns the instataneous wind speed one click per second = 2.4kmh
float get_wind_speed()
{
  float deltaTime = millis() - lastWindCheck; //750ms

  deltaTime /= 1000.0; //Covert to seconds

  float windSpeed = (float)windClicks / deltaTime; //3 / 0.750s = 4

  windClicks = 0; //Reset and start watching for new wind
  lastWindCheck = millis();

  windSpeed *= 2.4;

  /* Serial.println();
   Serial.print("Windspeed:");
   Serial.println(windSpeed);*/

  return (windSpeed);
}

int get_wind_direction()
// read the wind direction sensor, return heading in RADIANS
{
  unsigned int adc;
  
  adc = averageAnalogRead(WDIR); // get the current reading from the sensor
  Serial.print(adc);
  // The following table is ADC readings for the wind direction sensor output, sorted from low to high.
  // Each threshold is the midpoint between adjacent headings. The output is degrees for that ADC reading.
  // Note that these are not in compass degree order! See Weather Meters datasheet for more information.
  // These values based on a 10k r1 value and the r2 value from the meters

  if (adc < 75)  return 5;   //(113);
  if (adc < 89)  return 3;   //(68);
  if (adc < 110) return 4;   //(90);
  if (adc < 155) return 7;  //(158);
  if (adc < 214) return 6;   //(135);
  if (adc < 266) return 9;  //(202.5)
  if (adc < 347) return 8;  //(180);
  if (adc < 434) return 1;  //(22.5 degrees = 0.3927 Rad);
  if (adc < 531) return 2;  //(45);
  if (adc < 615) return 11; //(248);
  if (adc < 708) return 10; //(225);
  if (adc < 744) return 15; //(338);
  if (adc < 806) return 0;  //(0);
  if (adc < 857) return 13; //(293);
  if (adc < 915) return 14; //(315);
  if (adc < 984) return 12; //(270);
  return -1; // error, disconnected?
  
  // a circle is 2 * pi radians so each sector is PI/8 
  // Below would return radians - but that uses up too much storage
  //return ((PI/8) * sector);
}

/* Below is the old 'degrees' coding - just in case we ever need it 
  if (adc < 75) return (113);
  if (adc < 89) return (68);
  if (adc < 110) return (90);
  if (adc < 155) return (158);
  if (adc < 214) return (135);
  if (adc < 347) return (180);
  if (adc < 434) return (23);
  if (adc < 531) return (45);
  if (adc < 615) return (248);
  if (adc < 708) return (225);
  if (adc < 744) return (338);
  if (adc < 806) return (0);
  if (adc < 857) return (293);
  if (adc < 915) return (315);
  if (adc < 984) return (270);
  return (-1); // error, disconnected?
  */
 

//Takes an average of readings on a given pin
//Returns the average
int averageAnalogRead(int pinToRead)
{
  byte numberOfReadings = 8;
  unsigned int runningValue = 0;

  for (int x = 0 ; x < numberOfReadings ; x++)
    runningValue += analogRead(pinToRead);
  runningValue /= numberOfReadings;

  return (runningValue);
}

float vectorAverage (char * sectors, char * speeds, int count)
//Takes a row of 'sectors' - these are wind directions, 1-15 - and corresponding wind speeds and returns a 
//floating point average wind direction in degrees.
//Most of the calculations are in radians as that is how the trig functions work. The basic formula is to break the direction into a 
//north-south component and and east-west component.Here's a formula using degrees for ease of reading
/* 
  Reading 1: NW at 5kmh
  **************
  NS1 = 5 * cos(315) = 3.54
  EW1 = 5 * sin(315) = -3.54

  Reading 2: NE at 10 kmh
  *************
  NS2 = 10 * cos(45) = 7.07
  EW2 = 10 * sin(45) = 7.07

  Add components:
  ***************
  NS_TOT = NS1 + NS2 = 10.61

  EW_TOT = EW1 + EW2 = 3.54

  Vector:
  *******
  Average Direction = atan(EW_TOT / NS_TOT) = atan(3.54 / 10.61) = 18.43 degrees



// TO DO: Consider weighting by wind speed - ie multiplying each of the vectors by the speed 
I was considering weighting this by windspeed 
*/
{
  float rad;
  float NS_total = 0;
  float EW_total = 0;
    
  for (int i=0; i < count; i++)
  {
    float rad = sectorsToRadians(sectors[i]);
    NS_total += speeds[i] * cos(rad);
    EW_total += speeds[i] * sin(rad);
    #ifdef DEBUG
    Serial.print(" i =:"); 
    Serial.println(i);
    Serial.print("speeds[i] =:");
    Serial.println((int)speeds[i]);
    Serial.print("sectors [i] =:");
    Serial.println((int)sectors[i]);
    Serial.print("RAD =:");
    Serial.println(rad);
    #endif
  }
  
  #ifdef DEBUG
  Serial.print("NS_total:");
  Serial.print(NS_total, 2);
  Serial.print("EW_total:");
  Serial.print(EW_total, 2);
  #endif
  
  return (atan(NS_total/EW_total));
}

int sectorsToRadians (char sector)
{
  // a circle is 2 * pi radians so each sector is PI/8 
 
  return ((PI/8) * sector);
}
