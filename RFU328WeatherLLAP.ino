/*
 Weather Station using the RFU328
 By: Tom Cooper based on work by Nathan Seidle
 To Do: Code in the check for station id
 This version is reconfigged to use BMP180 pressure sensor
 This version is a major reworked to use LLAP comms protocol
*/
//#define DEBUG
//#define TRACE
#include <avr/wdt.h> //We need watch dog for this program
//#include <SoftwareSerial.h> //Connection to Imp
#include <string.h> 
#include <Wire.h> //I2C needed for sensors
//#include <stdio.h> //for sprintf()

#include "HTU21D.h" //Humidity sensor
#include <SFE_BMP180.h> //Pressure and temperature sensor

#include <EEPROM.h>
#include <LLAPSerial.h>

#define VERSION "1.01-----" //Extra chars are the LLAP filler
#define DEVICETYPE "WSTAT-----"
#define DEVICEID1 '-'
#define DEVICEID2 '-'
#define EEPROM_DEVICEID1 0
#define EEPROM_DEVICEID2 1
#define LLAP_MESSAGE_SIZE 9 // This is the size of the message payload 9 see LLAP standard
#define INITIAL "STARTED--"  //  First message on load
#define LLAP_FILLER "---------"
#define LLAP_COMMAND_CT 13
#define DECIMAL_PLACES 1
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
  "RAIN1H---",  rain1hMessage,          //08
  "RAIND----",  raintodayMessage,       //09
  "RAINSI---",  rainSinceLastMessage,   //10 - thinking of deprecating this
  "WDSP-----",  windspeedMessage,       //11
  "WDDI-----",  winddirMessage          //12
};

int command_num;

char msg [LLAP_MESSAGE_SIZE + 1];      // storage for incoming message plus null character
char reply [LLAP_MESSAGE_SIZE + 1];    // storage for reply plus null character


SFE_BMP180 myPressure; //Create an instance of the pressure sensor
HTU21D myHumidity; //Create an instance of the humidity sensor

//Hardware pin definitions
//-------------------------------------------------------------------------------------------
// digital I/O pins

const byte WSPEED = 3;
const byte RAIN = 2;
const byte STAT1 = 7;
const byte RADIO = 8; //Pin for switching on the Xino RF radio
const byte LED = 10; // Proof of life

// analog I/O pins
const byte WDIR = A1;
//const byte LIGHT = A1;
//const byte BATT = A2;
//const byte REFERENCE_3V3 = A3;
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

byte windspdavg[120]; //120 bytes to keep track of 2 minute average
int winddiravg[120]; //120 ints to keep track of 2 minute average
float windgust_10m[10]; //10 floats to keep track of largest gust in the last 10 minutes
int windgustdirection_10m[10]; //10 ints to keep track of 10 minute max
volatile float rainHour[60]; //60 floating numbers to keep track of 60 minutes of rain

//These are all the weather values that wunderground expects:
int wind_dir; // [0-360 instantaneous wind direction]
float windspeed; // [mph instantaneous wind speed]
float windgust; // [mph current wind gust, using software specific time period]
int windgustdir; // [0-360 using software specific time period]
float windspd_avg2m; // [mph 2 minute average wind speed mph]
int wind_dir_avg2m; // [0-360 2 minute average wind direction]
float windgustkmh_10m; // [mph past 10 minutes wind gust mph ]
int windgustdir_10m; // [0-360 past 10 minutes wind gust direction]
//float humidity; // [%]
float tempc; // [temperature C]
//float rain_1h; // [rain inches over the past hour)] -- the accumulated rainfall in the past 60 min
volatile float rain_today; // [rain mm so far today in local time]
volatile float rain_since_last; //needed for a 'last 24h' calculation in server
//float baromin = 30.03;// [barom in] - It's hard to calculate baromin locally, do this in the agent
float pressure;
//float dewptf; // [dewpoint F] - It's hard to calculate dewpoint locally, do this in the agent

//These are not output in this version of the hardware - but leaving it in anyway
float batt_lvl = 11.8;
float light_lvl = 0.72;

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

  myHumidity.begin(); //Configure the humidity sensor

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
  Serial.println("DEBUG>> starting main loop");
  #endif
  
  char status;
  int rc = 0;
  // wdt_reset(); //Pet the dog

  //Keep track of which minute it is
  if (millis() - lastSecond >= 1000)
  {
    lastSecond += 1000;

    //Take a speed and direction reading every second for 2 minute average
    if (++seconds_2m > 119) seconds_2m = 0;

    //Calc the wind speed and direction every second for 120 second to get 2 minute average
    windspeed = get_wind_speed();
    wind_dir = get_wind_direction();
    windspdavg[seconds_2m] = (int)windspeed;
    winddiravg[seconds_2m] = wind_dir;
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

  //Here we go with the LLAP stuff
  #ifdef TRACE
  Serial.println("DEBUG>> check LLP");
  #endif
  if (LLAP.bMsgReceived) // got a message?
  {
    
    LLAP.sMessage.toCharArray(msg, LLAP_MESSAGE_SIZE + 1);
    msg[LLAP_MESSAGE_SIZE] = '\0';  
    
    #ifdef DEBUG
    Serial.print("LLAP message received: ");
    Serial.println(msg);
    #endif  

    LLAP.bMsgReceived = false;
    strlcpy(reply,msg,LLAP_MESSAGE_SIZE); // by default setup to echo the incoming message 

    // Step through LLAP_commands to work out which function to call
    
    command_num = 0;
    
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
        Serial.print("rc: ");
        Serial.println(rc);
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
    
    #ifdef DEBUG
    Serial.println("DEBUG>> Sending LLAP");
    #endif
    LLAP.sendMessage(reply);
  }  
  
  delay(100); //Update every 100ms. No need to go any faster. Query this?   

} // end of main loop
 
void formatFloat (int commandLen, float in_value, char * reply)
{
  // Formats a float into the output reply string - assumes DECIMAL_PLACES decimal places
  // The length of a string is 2 chars for the point and the DECIMAL_PLACES, plus one if it's negative (for sign) and then one for each decimal
  // digit
  
  #ifdef DEBUG
  Serial.println("DEBUG>> formatFloat()");
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

void tempMessage(char * reply)
{
  
  #ifdef DEBUG
  Serial.println("DEBUG>> tempMessage()");
  #endif
  
  const int cmdlen = 4; // "TEMP"
  double T;
  float t2;
  char temperature [LLAP_MESSAGE_SIZE]; // shrink this if memory is an issue
  int status = myPressure.startTemperature();
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
      
    }
  }
  
  #ifdef DEBUG
  //T=10.16;
  #endif
 
  int strf_len = 3; 
               //This is a fiddle to format temperature readings - 
               //floats don't work with sprintf()
  if (T < 0) { 
    strf_len++;
  }
  if ((T<=-10) || (T>=10)) {
    strf_len++;
  }
   
  dtostrf(float(T),strf_len,1,temperature);
 
  // sprintf(temperature,"%.1f",t2);
  
  strcpy(reply + cmdlen,temperature);
  
  fillLLAPcmd(reply); // Adds any ----- fillers needed at the end of string
  
}

void humidityMessage(char * reply)
{
  #ifdef DEBUG
  Serial.println("DEBUG>> hunidityMessage()");
  #endif
  const int cmdlen = 3; // "HUM"
  float h;
  int strf_len = 3; 
               //This is a fiddle to format temperature readings - 
               //floats don't work with sprintf()
  char humidity [LLAP_MESSAGE_SIZE]; // shrink this if memory is an issue
  
  h = myHumidity.readHumidity(); // percent
  
  if (h < 10)  
    strf_len--;
  else 
  if (h >=100) 
    strf_len++; 
   
  dtostrf(h,strf_len,1,humidity);
  
  strcpy(reply + cmdlen,humidity);
  
  fillLLAPcmd(reply); // Adds any ----- fillers needed at the end of string  
}

void rain1hMessage(char * reply)
{
  #ifdef DEBUG
  Serial.println("DEBUG>> rain1hMessage()");
  #endif
  const int cmdlen = 6; // "RAIN1H"
  float r1 = 0;
  int strf_len = 3; 
                
  char rain1h [LLAP_MESSAGE_SIZE]; // shrink this if memory is an issue
  
  for (int i = 0 ; i < 60 ; i++)
       r1 += rainHour[i];
  
  if (r1 < 10) 
    strf_len--;
  else if (r1 >=100) 
    strf_len++; 
  
  dtostrf(r1,strf_len,1,rain1h);
  
  strcpy(reply + cmdlen,rain1h);
  
  fillLLAPcmd(reply); // Adds any ----- fillers needed at the end of string
  
}
  
void raintodayMessage(char * reply)
{
  #ifdef DEBUG
  Serial.println("DEBUG>> raintodayMessage()");
  #endif
  
  const int cmdlen = 5; // "RAIND"
  float rt = rain_today; // that's global - updated by interrupt
  
  formatFloat (cmdlen, rt, reply);
  // The output string should now be set up.  
}

void rainSinceLastMessage(char * reply)
{
  #ifdef DEBUG
  Serial.println("DEBUG>> rainsinceLastMessage()");
  #endif
  
  const int cmdlen = 6; // "RAINSI"
  float rs = rain_since_last; // that's global - updated by interrupt
  rain_since_last = 0; //reset straight away - it's updated by interrupt
  formatFloat (cmdlen, rs, reply);
  // The output string should now be set up.  
}

void windspeedMessage(char * reply)
{
  #ifdef DEBUG
  Serial.println("DEBUG>> windspeedMessage()");
  #endif
  
  const int cmdlen = 4; // "WDSP"
  
  float deltaTime = millis() - lastWindCheck; //750ms

  deltaTime /= 1000.0; //Covert to seconds

  float ws = (float)windClicks / deltaTime; //3 / 0.750s = 4

  windClicks = 0; //Reset and start watching for new wind (global)
  lastWindCheck = millis();

  ws *= 2.4; // need to review this - not sure what is going on!

  formatFloat (cmdlen, ws, reply);
  // The output string should now be set up.  
}  
  
void winddirMessage (char * reply)
{
  //This one is a bit different - as direction is an int and we use get_wind_direction elsewhere 
  const int cmdlen = 4; // "WDSP"
  //char[4] windDir;
  
  int wd = get_wind_direction();
  //sprintf(reply+cmdlen, "%d", wd);
  fillLLAPcmd(reply); // Adds any ----- fillers needed at the end of string
}


//Prints the various arrays for debugging
#ifdef DEBUG
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
// read the wind direction sensor, return heading in degrees
{
  unsigned int adc;

  adc = averageAnalogRead(WDIR); // get the current reading from the sensor
  Serial.print(adc);
  // The following table is ADC readings for the wind direction sensor output, sorted from low to high.
  // Each threshold is the midpoint between adjacent headings. The output is degrees for that ADC reading.
  // Note that these are not in compass degree order! See Weather Meters datasheet for more information.
  // These values based on a 10k r1 value and the r2 value from the meters

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
}

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

void helloMessage(char * reply)
{
  #ifdef DEBUG
  Serial.println("DEBUG>> helloMessage");
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
}


#ifdef LEGACY

        //Calculates each of the variables that the Pi is expecting
        void calcWeather()
        {
          //current wind_dir, current windspeed, windgust, and windgustdir are calculated every 100ms throughout the day

          //Calc windspd_avg2m
          double T, P; //used as interims for BMP calls
          char status;
          float temp = 0;
          for (int i = 0 ; i < 120 ; i++)
            temp += windspdavg[i];
          temp /= 120.0;
          windspd_avg2m = temp;

          //Calc wind_dir_avg2m
          temp = 0; //Can't use wind_dir_avg2m because it's an int
          for (int i = 0 ; i < 120 ; i++)
            temp += winddiravg[i];
          temp /= 120;
          wind_dir_avg2m = temp;

          //Calc windgustkmh_10m
          //Calc windgustdir_10m
          //Find the largest windgust in the last 10 minutes
          windgustkmh_10m = 0;
          windgustdir_10m = 0;
          //Step through the 10 minutes
          for (int i = 0; i < 10 ; i++)
          {
            if (windgust_10m[i] > windgustkmh_10m)
            {
              windgustkmh_10m = windgust_10m[i];
              windgustdir_10m = windgustdirection_10m[i];
            }
          }

          //Calc humidity
          humidity = myHumidity.readHumidity();
          //float temp_h = myHumidity.readTemperature();
          //Serial.print(" TempH:");
          //Serial.print(temp_h, 2);

          // Start a temperature measurement:
          // If request is successful, the number of ms to wait is returned.
          // If request is unsuccessful, 0 is returned.

          status = myPressure.startTemperature();
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
              // Print out the measurement:
              //Serial.print("temperature: ");
              //Serial.print(T,2);
              //Serial.print(" deg C, ");
              // Start a pressure measurement:
              // The parameter is the oversampling setting, from 0 to 3 (highest res, longest wait).
              // If request is successful, the number of ms to wait is returned.
              // If request is unsuccessful, 0 is returned.

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
          tempc = (float)(T);
          pressure = (float)(P);

          //Total rainfall for the day is calculated within the interrupt
          //Calculate amount of rainfall for the last 60 minutes
          rain_1h = 0;
          for (int i = 0 ; i < 60 ; i++)
            rain_1h += rainHour[i];

          //Calc light level
          light_lvl = get_light_level();

          //Calc battery level
          batt_lvl = get_battery_level();
        }

        //Returns the voltage of the light sensor based on the 3.3V rail
        //This allows us to ignore what VCC might be (an Arduino plugged into USB has VCC of 4.5 to 5.2V)
        float get_light_level()
        {
          return 0.00;
        }

        //Returns the voltage of the raw pin based on the 3.3V rail
        //This allows us to ignore what VCC might be (an Arduino plugged into USB has VCC of 4.5 to 5.2V)
        //Battery level is connected to the RAW pin on Arduino and is fed through two 5% resistors:
        //3.9K on the high side (R1), and 1K on the low side (R2)
        float get_battery_level()
        {
          return 0.00;
        }



        //Reports the weather string to the serial interface
        void reportWeather()
        {
          calcWeather(); //Go calc all the various sensors

          Serial.print("$,wind_dir=");
          Serial.print(wind_dir);
          Serial.print(",wind_speed=");
          Serial.print(windspeed, 1);
          Serial.print(",wind_gust=");
          Serial.print(windgust, 1);
          Serial.print(",windgustdir=");
          Serial.print(windgustdir);
          Serial.print(",windspd_avg2m=");
          Serial.print(windspd_avg2m, 1);
          Serial.print(",wind_dir_avg2m=");
          Serial.print(wind_dir_avg2m);
          Serial.print(",windgust_10m=");
          Serial.print(windgustkmh_10m, 1);
          Serial.print(",windgustdir_10m=");
          Serial.print(windgustdir_10m);
          Serial.print(",humidity=");
          Serial.print(humidity, 1);
          Serial.print(",temp=");
          Serial.print(tempc, 1);
          Serial.print(",rain_1h=");
          Serial.print(rain_1h, 2);
          Serial.print(",rain_today=");
          Serial.print(rain_today, 2);
          Serial.print(",rain_since_last=");
          Serial.print(rain_since_last, 2);
          rain_since_last = 0; //reset this straight away to minimse interrupts causing missed drops
          Serial.print(",uncorrected_pressure=");  //Will be calculated in agent
          Serial.print(pressure, 2);
          Serial.print(",batt_lvl=");
          Serial.print(batt_lvl, 2);
          Serial.print(",light_lvl=");
          Serial.print(light_lvl, 2);
          Serial.print(",");
          Serial.println("#,");

          //Test string
          //Serial.println("$,wind_dir=270,windspeed=0.0,windgust=0.0,windgustdir=0,windspd_avg2m=0.0,wind_dir_avg2m=12,windgustwindgust_10m=0.0,windgustdir_10m=0,humidity=998.0,tempc=-1766.2,rain_1h=0.00,rain_today=0.00,-999.00,batt_lvl=16.11,light_lvl=3.32,#,");
        }

 rc = strncmp(msg, "HELLO----", LLAP_MESSAGE_SIZE);
    if (rc == 0)
    {
      ;    // just echo the message back
    }
    else
    {
      rc = strncmp(msg, "FVER-----", LLAP_MESSAGE_SIZE);
      if (rc == 0)
      {
        strlcpy(reply,"FVER",LLAP_MESSAGE_SIZE);
        strncat(reply, VERSION, LLAP_MESSAGE_SIZE);
      }
      else
      {
        rc = strcmp(msg, "DEVTYPE--");
        if (rc == 0)
        {
          strlcpy(reply,"DEVTYPE",LLAP_MESSAGE_SIZE);
          strncat( reply, DEVICETYPE, LLAP_MESSAGE_SIZE);
        }
        else
        {
          rc = strncmp(msg, "SAVE-----", LLAP_MESSAGE_SIZE);
          if (rc == 0)
          {
            EEPROM.write(EEPROM_DEVICEID1, LLAP.deviceId[0]);    // save the device ID
            EEPROM.write(EEPROM_DEVICEID2, LLAP.deviceId[1]);    // save the device ID
          }
          //------- OK that's the basic protocol over - move on to processing the weather...
          else
          {
            rc = strncmp(msg, "TEMP-----", LLAP_MESSAGE_SIZE);
            if (rc == 0)
            {
              getTemperature(reply);
            }
            else
            {
              rc = strncmp(msg, "HUM------", LLAP_MESSAGE_SIZE);
              if (rc == 0)
              {
 //             getHumidity();
              }
              else
              {
                rc = strncmp(msg, "RAIN1H---", LLAP_MESSAGE_SIZE);
                if (rc == 0)
                {
 //               getRain1H ();
                }
                else
                {
                  rc = strncmp(msg, "RAIND----", LLAP_MESSAGE_SIZE);
                  if (rc == 0)
                  {
//                  getRainToday();
                  }
                  else
                  {
                    rc = strncmp(msg, "RAINSI---", LLAP_MESSAGE_SIZE);
                    if (rc == 0)
                    {
//                    getRainSince ();
                      // Note this is deprecated
                    }
                    //---------------------------------------------------- Wind ------------------------------------
                    else
                    {
                      rc = strncmp(msg, "WDSP-----", LLAP_MESSAGE_SIZE);
                      if (rc == 0)
                      {
//                      getWindSpeed ();
                      }
                      else
                      {
                        rc = strncmp(msg, "WDDI-----", LLAP_MESSAGE_SIZE);
                        if (rc == 0)
                        {
//                        getWindDirection ();
                        }
                        else
                        {
                          rc = strncmp(msg, "WDGU-----", LLAP_MESSAGE_SIZE);
                          if (rc == 0)
                          {
//                          getWindGust ();
                          }
                          else
                          {
                            rc = strncmp(msg, "WDGUD----", LLAP_MESSAGE_SIZE);
                            if (rc == 0)
                            {
//                            getWindGustDirection();
                            }
                            else
                            {
                              rc = strncmp(msg, "WDSP2----", LLAP_MESSAGE_SIZE);
                              if (rc == 0)
                              {
//                              getWindSpeed2mins();
                              }
                              else
                              {
                                rc = strncmp(msg, "WDDI2----", LLAP_MESSAGE_SIZE);
                                if (rc == 0)
                                {
//                                getWindDirection2mins ();
                                }
                                else
                                {
                                  rc = strncmp(msg, "WDGU10---", LLAP_MESSAGE_SIZE);
                                  if (rc == 0)
                                  {
//                                  getWindGust10mins ();
                                  }
                                  else
                                  {
                                    rc = strncmp(msg, "WDGUD10--", LLAP_MESSAGE_SIZE);
                                    if (rc == 0)
                                    {
//                                    getWindGustDirection10mins ();
                                    }
                                    //---------------------------------------------------- Pressure  ------------------------------------
                                    else
                                    {
                                      rc = strncmp(msg, "BAR------", LLAP_MESSAGE_SIZE);
                                      if (rc == 0)
                                      {
//                                      getBarometric ();
                                      }
                                      else
                                      {

                                        //---------------------------------------------------- Extras  ------------------------------------
                                        rc = strncmp(msg, "LIGHT----", LLAP_MESSAGE_SIZE);
                                        if (rc == 0)
                                        {
//                                        getLight ();
                                        }
                                        else
                                        {
                                          rc = strncmp(msg, "BATT-----", LLAP_MESSAGE_SIZE);
                                          if (rc == 0)
                                          {
//                                          getBattery();
                                          }
                                          else
                                          {
                                            rc = strncmp(msg, "MIDNIGHT-", LLAP_MESSAGE_SIZE);
                                            if (rc == 0)
                                            {
                                              midnightReset();
                                            }
                                            else
                                            {
                                              strcpy (reply,"ERROR....");
                                            }
                                          }
                                        }
                                      }
                                    }
                                  }
                                }
                              }
                            }
                          }
                        }
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }

#endif
