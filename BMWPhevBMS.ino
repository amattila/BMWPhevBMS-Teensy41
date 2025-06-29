/*
  Copyright (c) 2019 Simp ECO Engineering
  Permission is hereby granted, free of charge, to any person obtaining
  a copy of this software and associated documentation files (the
  "Software"), to deal in the Software without restriction, including
  without limitation the rights to use, copy, modify, merge, publish,
  distribute, sublicense, and/or sell copies of the Software, and to
  permit persons to whom the Software is furnished to do so, subject to
  the following conditions:
  The above copyright notice and this permission notice shall be included
  in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

  Thank you to James Warner for proving out canbus decoding and finding balancing bits
*/

#include "BMSModuleManager.h"
#include <Arduino.h>
#include "config.h"
#include "SerialConsole.h"
#include "Logger.h"
#include "CRC8.h"
#include <ADC.h>  //https://github.com/pedvide/ADC
#include <EEPROM.h>
#include <FlexCAN_T4.h>  //https://github.com/collin80/FlexCAN_Library
#include <SPI.h>
#include <Filters.h>  //https://github.com/JonHub/Filters
#include "Watchdog_t4.h"

#define RESTART_ADDR 0xE000ED0C
#define READ_RESTART() (*(volatile uint32_t *)RESTART_ADDR)
#define WRITE_RESTART(val) ((*(volatile uint32_t *)RESTART_ADDR) = (val))
#define CPU_REBOOT WRITE_RESTART(0x5FA0004)

BMSModuleManager bms;
SerialConsole console;
EEPROMSettings settings;
WDT_T4<WDT1> wdt;

void wdtCallback()
{
	Serial.println("Feed watchdog, OR RESET!");
}


/////Version Identifier/////////
int firmver = 260424;

//Curent filter//
float filterFrequency = 5.0;
FilterOnePole lowpassFilter(LOWPASS, filterFrequency);

//Simple BMS V2 wiring//
const int ACUR2 = A0;  // current 1
const int ACUR1 = A1;  // current 2
const int IN1 = 17;    // input 1 - high active
const int IN2 = 16;    // input 2- high active
const int IN3 = 18;    // input 1 - high active
const int IN4 = 19;    // input 2- high active
const int OUT1 = 11;   // output 1 - high active
const int OUT2 = 12;   // output 2 - high active
const int OUT3 = 20;   // output 3 - high active
const int OUT4 = 21;   // output 4 - high active
const int OUT5 = 3;   // output 5 - Low active
const int OUT6 = 4;   // output 6 - Low active
const int OUT7 = 5;    // output 7 - Low active
const int OUT8 = 6;    // output 8 - Low active
const int led = 13;
const int BMBfault = 11;

byte bmsstatus = 0;
//bms status values
#define Boot 0
#define Ready 1
#define Drive 2
#define Charge 3
#define Precharge 4
#define Error 5
//
//Current sensor values
#define Undefined 0
#define Analoguedual 1
#define Canbus 2
#define Analoguesing 3

// Can current sensor values
#define LemCAB300 1
#define IsaScale 3
#define VictronLynx 4
#define LemCAB500 2
#define CurCanMax 4  // max value


//
//Charger Types
#define NoCharger 0
#define BrusaNLG5 1
#define ChevyVolt 2
#define Eltek 3
#define Elcon 4
#define HVSBS 5
//

//CSC Variants
#define BmwI3 0
#define MiniE 1
//


int Discharge;
int ErrorReason = 0;

//variables for output control
int pulltime = 100;
int contctrl, contstat = 0;  //1 = out 5 high 2 = out 6 high 3 = both high
unsigned long conttimer1, conttimer2, conttimer3, Pretimer, Pretimer1, overtriptimer, undertriptimer, mainconttimer, balancetimer = 0;
uint16_t pwmfreq = 18000;  //pwm frequency

int pwmcurmax = 50;     //Max current to be shown with pwm
int pwmcurmid = 50;     //Mid point for pwm dutycycle based on current
int16_t pwmcurmin = 0;  //DONOT fill in, calculated later based on other values


//variables for VE driect bus comms
const char *myStrings[] = { "V", "14674", "I", "0", "CE", "-1", "SOC", "800", "TTG", "-1", "Alarm", "OFF", "Relay", "OFF", "AR", "0", "BMV", "600S", "FW", "212", "H1", "-3", "H2", "-3", "H3", "0", "H4", "0", "H5", "0", "H6", "-7", "H7", "13180", "H8", "14774", "H9", "137", "H10", "0", "H11", "0", "H12", "0" };

//variables for VE can
uint16_t chargevoltage = 49100;  //max charge voltage in mv
uint16_t chargecurrent, tempchargecurrent = 0;
uint16_t disvoltage = 42000;  // max discharge voltage in mv
int discurrent = 0;
int batvcal = 0;

uint16_t SOH = 100;  // SOH place holder

unsigned char alarm[4] = { 0, 0, 0, 0 };
unsigned char warning[4] = { 0, 0, 0, 0 };
unsigned char mes[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
unsigned char bmsname[8] = { 'S', 'I', 'M', 'P', ' ', 'B', 'M', 'S' };
unsigned char bmsmanu[8] = { 'T', 'O', 'M', ' ', 'D', 'E', ' ', 'B' };
long unsigned int rxId;
unsigned char len = 0;
byte rxBuf[8];
char msgString[128];  // Array to store serial string
uint32_t inbox;
signed long CANmilliamps;
signed long voltage1, voltage2, voltage3 = 0;  //mV only with ISAscale sensor

//struct can_frame canMsg;
//MCP2515 CAN1(10); //set CS pin for can controlelr


//variables for current calulation
int value;
float currentact, RawCur;
float ampsecond;
unsigned long lasttime;
unsigned long looptime, looptime1, UnderTime, OverTime, cleartime, baltimer, commandtime = 0;  //ms
int currentsense = 14;
int sensor = 1;

//Variables for SOC calc
int SOC = 100;  //State of Charge
int SOCset = 0;
int SOCtest = 0;
int SOCmem = 0;
int SOCreset = 0;

///charger variables
int maxac1 = 16;           //Shore power 16A per charger
int maxac2 = 10;           //Generator Charging
int chargerid1 = 0x618;    //bulk chargers
int chargerid2 = 0x638;    //finishing charger
float chargerendbulk = 0;  //V before Charge Voltage to turn off the bulk charger/s
float chargerend = 0;      //V before Charge Voltage to turn off the finishing charger/s
int chargertoggle = 0;
int ncharger = 1;  // number of chargers
bool chargecurrentlimit = 0;

//AC current control
volatile uint32_t pilottimer = 0;
volatile uint16_t timehigh, duration = 0;
volatile uint16_t accurlim = 0;
volatile int dutycycle = 0;
uint16_t chargerpower = 0;
bool CPdebug = 0;

//variables
int outputstate = 0;
int incomingByte = 0;
int x = 0;
int storagemode = 0;
int cellspresent = 0;
int dashused = 1;
int Charged = 0;
bool balancepauze = 0;


//Debugging modes//////////////////
int debug = 1;
int inputcheck = 0;   //read digital inputs
int outputcheck = 0;  //check outputs
int candebug = 0;     //view can frames
int gaugedebug = 0;
int debugCur = 0;
int CSVdebug = 0;
int menuload = 0;
int balancecells;
int debugdigits = 2;  //amount of digits behind decimal for voltage reading
int balancedebug = 0;

//BMW Can Variables///

//uint8_t check1[8] = {0x13, 0x76, 0xD9, 0xBC, 0x9A, 0xFF, 0x50, 0x35};
//uint8_t check2[8] = {0x4A, 0x2F, 0x80, 0xE5, 0xC3, 0xA6, 0x09, 0x6C};
uint8_t Imod, mescycle = 0;
uint8_t nextmes = 0;
uint16_t commandrate = 50;
uint8_t testcycle = 0;
uint8_t DMC[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
uint8_t Unassigned, NextID = 0;

//BMW checksum variable///

CRC8 crc8;
uint8_t checksum;
const uint8_t finalxor[12] = { 0xCF, 0xF5, 0xBB, 0x81, 0x27, 0x1D, 0x53, 0x69, 0x02, 0x38, 0x76, 0x4C };

FlexCAN_T4<CAN1, RX_SIZE_256, TX_SIZE_16> Can0; // Can1 port (bms)
FlexCAN_T4<CAN2, RX_SIZE_256, TX_SIZE_16> Can1; // Can2 port (safetybox)


ADC *adc = new ADC();  // adc object

void loadSettings() {
  Logger::console("Resetting to factory defaults");
  settings.version = EEPROM_VERSION;
  settings.checksum = 2;
  settings.canSpeed = 500000;
  settings.batteryID = 0x01;  //in the future should be 0xFF to force it to ask for an address
  settings.OverVSetpoint = 4.2f;
  settings.UnderVSetpoint = 3.0f;
  settings.ChargeVsetpoint = 4.1f;
  settings.ChargeHys = 0.2f;  // voltage drop required for charger to kick back on
  settings.WarnOff = 0.1f;    //voltage offset to raise a warning
  settings.DischVsetpoint = 3.2f;
  settings.DischHys = 0.2f;  // Discharge voltage offset
  settings.CellGap = 0.2f;   //max delta between high and low cell
  settings.OverTSetpoint = 65.0f;
  settings.UnderTSetpoint = -10.0f;
  settings.ChargeTSetpoint = 0.0f;
  settings.triptime = 500;  //mS of delay before counting over or undervoltage
  settings.DisTSetpoint = 40.0f;
  settings.WarnToff = 5.0f;   //temp offset before raising warning
  settings.IgnoreTemp = 0;    // 0 - use both sensors, 1 or 2 only use that sensor
  settings.IgnoreVolt = 0.5;  //
  settings.balanceVoltage = 3.9f;
  settings.balanceHyst = 0.04f;
  settings.balanceDuty = 60;
  settings.logLevel = 2;
  settings.CAP = 100;                //battery size in Ah
  settings.Pstrings = 1;             // strings in parallel used to divide voltage of pack
  settings.Scells = 8;              //Cells in series
  settings.StoreVsetpoint = 3.8;     // V storage mode charge max
  settings.discurrentmax = 300;      // max discharge current in 0.1A
  settings.DisTaper = 0.3f;          //V offset to bring in discharge taper to Zero Amps at settings.DischVsetpoint
  settings.chargecurrentmax = 300;   //max charge current in 0.1A
  settings.chargecurrent2max = 150;  //max charge current in 0.1A
  settings.chargecurrentend = 50;    //end charge current in 0.1A
  settings.socvolt[0] = 3100;        //Voltage and SOC curve for voltage based SOC calc
  settings.socvolt[1] = 10;          //Voltage and SOC curve for voltage based SOC calc
  settings.socvolt[2] = 4100;        //Voltage and SOC curve for voltage based SOC calc
  settings.socvolt[3] = 90;          //Voltage and SOC curve for voltage based SOC calc
  settings.invertcur = 0;            //Invert current sensor direction
  settings.cursens = 2;
  settings.curcan = LemCAB300;
  settings.voltsoc = 0;        //SOC purely voltage based
  settings.Pretime = 5000;     //ms of precharge time
  settings.conthold = 50;      //holding duty cycle for contactor 0-255
  settings.Precurrent = 1000;  //ma before closing main contator
  settings.convhigh = 58;      // mV/A current sensor high range channel
  settings.convlow = 643;      // mV/A current sensor low range channel
  settings.offset1 = 1750;     //mV mid point of channel 1
  settings.offset2 = 1750;     //mV mid point of channel 2
  settings.changecur = 20000;  //mA change overpoint
  settings.gaugelow = 50;      //empty fuel gauge pwm
  settings.gaugehigh = 255;    //full fuel gauge pwm
  settings.ESSmode = 0;        //activate ESS mode
  settings.ncur = 1;           //number of multiples to use for current measurement
  settings.chargertype = 2;    // 1 - Brusa NLG5xx 2 - Volt charger 0 -No Charger
  settings.chargerspd = 100;   //ms per message
  settings.chargereff = 85;    //% effiecency of charger
  settings.chargerACv = 240;   // AC input voltage into Charger
  settings.UnderDur = 5000;    //ms of allowed undervoltage before throwing open stopping discharge.
  settings.CurDead = 5;        // mV of dead band on current sensor
  settings.ChargerDirect = 1;  //1 - charger is always connected to HV battery // 0 - Charger is behind the contactors
  settings.tripcont = 1;       //in ESSmode 1 - Main contactor function, 0 - Trip function
  settings.CSCvariant = 0;     //0 BMW I3 - 1 Mini-E
  settings.TempOff = 0;        //Temperature offset
}


CAN_message_t msg;
CAN_message_t inMsg;

uint32_t lastUpdate;

String inputBuffer = "";
bool waitingForInput = false;
char currentCommand = 0;

void processMenuValue(char command, int value) {
  switch (command) {
    case 'a':  // Cells in Series per String
      settings.Scells = value;
      SERIALCONSOLE.println();
      SERIALCONSOLE.print("Set Scells = ");
      SERIALCONSOLE.println(value);
      break;
    case '0':  // Parallel strings
      settings.Pstrings = value;
      bms.setPstrings(settings.Pstrings);
      SERIALCONSOLE.println();
      SERIALCONSOLE.print("Set Pstrings = ");
      SERIALCONSOLE.println(value);
      break;
    case '1':  // Over Voltage
      settings.OverVSetpoint = value / 1000.0;
      SERIALCONSOLE.println();
      SERIALCONSOLE.print("Set OverVSetpoint = ");
      SERIALCONSOLE.print(value);
      SERIALCONSOLE.println("mV");
      break;
    case '2':  // Under Voltage
      settings.UnderVSetpoint = value / 1000.0;
      SERIALCONSOLE.println();
      SERIALCONSOLE.print("Set UnderVSetpoint = ");
      SERIALCONSOLE.print(value);
      SERIALCONSOLE.println("mV");
      break;
    case '3':  // Over Temperature
      settings.OverTSetpoint = value;
      SERIALCONSOLE.println();
      SERIALCONSOLE.print("Set OverTSetpoint = ");
      SERIALCONSOLE.print(value);
      SERIALCONSOLE.println("C");
      break;
    case '4':  // Under Temperature
      settings.UnderTSetpoint = value;
      SERIALCONSOLE.println();
      SERIALCONSOLE.print("Set UnderTSetpoint = ");
      SERIALCONSOLE.print(value);
      SERIALCONSOLE.println("C");
      break;
    case '5':  // Balance Voltage
      settings.balanceVoltage = value / 1000.0;
      SERIALCONSOLE.println();
      SERIALCONSOLE.print("Set balanceVoltage = ");
      SERIALCONSOLE.print(value);
      SERIALCONSOLE.println("mV");
      break;
    case '6':        // Balance Hysteresis
      settings.balanceHyst = value / 1000.0;
      SERIALCONSOLE.println();
      SERIALCONSOLE.print("Set balanceHyst = ");
      SERIALCONSOLE.print(value);
      SERIALCONSOLE.println("mV");
      break;
    case '7':  // Battery Capacity
      settings.CAP = value;
      SERIALCONSOLE.println();
      SERIALCONSOLE.print("Set CAP = ");
      SERIALCONSOLE.print(value);
      SERIALCONSOLE.println("Ah");
      break;
    case '8':  // Discharge current
      settings.discurrentmax = value * 10;
      SERIALCONSOLE.println();
      SERIALCONSOLE.print("Set discurrentmax = ");
      SERIALCONSOLE.print(value);
      SERIALCONSOLE.println("A");
      break;
    case '9':  // Discharge Voltage Setpoint
      settings.DischVsetpoint = value / 1000.0;
      SERIALCONSOLE.println();
      SERIALCONSOLE.print("Set DischVsetpoint = ");
      SERIALCONSOLE.print(value);
      SERIALCONSOLE.println("mV");
      break;
    case 'b':  // SOC setpoint 1 voltage
      settings.socvolt[0] = value;
      SERIALCONSOLE.println();
      SERIALCONSOLE.print("Set SOC setpoint 1 voltage = ");
      SERIALCONSOLE.print(value);
      SERIALCONSOLE.println("mV");
      break;
    case 'c':  // SOC setpoint 1 percentage
      settings.socvolt[1] = value;
      SERIALCONSOLE.println();
      SERIALCONSOLE.print("Set SOC setpoint 1 percentage = ");
      SERIALCONSOLE.print(value);
      SERIALCONSOLE.println("%");
      break;
    case 'd':  // SOC setpoint 2 voltage
      settings.socvolt[2] = value;
      SERIALCONSOLE.println();
      SERIALCONSOLE.print("Set SOC setpoint 2 voltage = ");
      SERIALCONSOLE.print(value);
      SERIALCONSOLE.println("mV");
      break;
    case 'e':  // SOC setpoint 2 percentage
      settings.socvolt[3] = value;
      SERIALCONSOLE.println();
      SERIALCONSOLE.print("Set SOC setpoint 2 percentage = ");
      SERIALCONSOLE.print(value);
      SERIALCONSOLE.println("%");
      break;
    default:
      SERIALCONSOLE.println();
      SERIALCONSOLE.print("Unknown command: ");
      SERIALCONSOLE.println(command);
      break;
  }
  
  // Print confirmation and return to menu
  SERIALCONSOLE.println();
  SERIALCONSOLE.println("=== Value Set ===");
  
  menuload = 1;  // Return to menu
  incomingByte = 'b';  // Battery menu - reprint menu
}

void setup() {
  //delay(4000);  //just for easy debugging. It takes a few seconds for USB to come up properly on most OS's
  //pinMode(ACUR1, INPUT);//Not required for Analogue Pins
  //pinMode(ACUR2, INPUT);//Not required for Analogue Pins
  pinMode(IN1, INPUT);
  pinMode(IN2, INPUT);
  pinMode(IN3, INPUT);
  pinMode(IN4, INPUT);
  pinMode(OUT1, OUTPUT);  // drive contactor
  pinMode(OUT2, OUTPUT);  // precharge
  pinMode(OUT3, OUTPUT);  // charge relay
  pinMode(OUT4, OUTPUT);  // Negative contactor
  pinMode(OUT5, OUTPUT);  // pwm driver output
  pinMode(OUT6, OUTPUT);  // pwm driver output
  pinMode(OUT7, OUTPUT);  // pwm driver output
  pinMode(OUT8, OUTPUT);  // pwm driver output
  pinMode(led, OUTPUT);

  analogWriteFrequency(OUT5, pwmfreq);
  analogWriteFrequency(OUT6, pwmfreq);
  analogWriteFrequency(OUT7, pwmfreq);
  analogWriteFrequency(OUT8, pwmfreq);

  Can0.begin();
  Can0.setBaudRate(500000);

  // Configure mailboxes for standard frames
  for (int i = 0; i < 8; i++) {
    Can0.setMB((FLEXCAN_MAILBOX)i, RX, STD);
  }

  // Configure mailboxes for extended frames
  for (int i = 8; i < 12; i++) {
    Can0.setMB((FLEXCAN_MAILBOX)i, RX, EXT);
  }

  //if using enable pins on a transceiver they need to be set on


  adc->adc0->setAveraging(16);   // set number of averages
  adc->adc0->setResolution(16);  // set bits of resolution
  adc->adc0->setConversionSpeed(ADC_CONVERSION_SPEED::MED_SPEED);
  adc->adc0->setSamplingSpeed(ADC_SAMPLING_SPEED::MED_SPEED);
  adc->adc0->startContinuous(ACUR1);


  SERIALCONSOLE.begin(115200);
  SERIALCONSOLE.println("Starting up!");
  SERIALCONSOLE.println("SimpBMS V2 BMW I3");

  Serial2.begin(115200);

  // Display reason the Teensy was last reset
  Serial.println();
  Serial.println("Reason for last Reset: ");


	if (SRC_SRSR & SRC_SRSR_TEMPSENSE_RST_B)
		Serial.println("Temperature Sensor Software Reset");
	if (SRC_SRSR & SRC_SRSR_WDOG3_RST_B)
		Serial.println("IC Watchdog3 Timeout Reset");
	if (SRC_SRSR & SRC_SRSR_JTAG_SW_RST)
		Serial.println("JTAG Software Reset");
	if (SRC_SRSR & SRC_SRSR_JTAG_RST_B)
		Serial.println("High-Z JTAG Reset");
	if (SRC_SRSR & SRC_SRSR_WDOG_RST_B)
		Serial.println("IC Watchdog Timeout Reset");
	if (SRC_SRSR & SRC_SRSR_IPP_USER_RESET_B)
		Serial.println("Power-up Sequence (Cold Reset Event)");
	if (SRC_SRSR & SRC_SRSR_CSU_RESET_B)
		Serial.println("Central Security Unit Reset");
	if (SRC_SRSR & SRC_SRSR_LOCKUP_SYSRESETREQ)
		Serial.println("CPU Lockup or Software Reset");
	if (SRC_SRSR & SRC_SRSR_IPP_RESET_B)
		Serial.println("Power-up Sequence");
	Serial.println();
  ///////////////////


	// enable WDT
	WDT_timings_t config;
	config.trigger = 1; // in seconds, 0->128
	config.timeout = 4; // in seconds, 0->128
	config.callback = wdtCallback;
	wdt.begin(config);
	/////////////////


  SERIALBMS.begin(612500);  //Tesla serial bus
  //VE.begin(19200); //Victron VE direct bus
#if defined(__arm__) && defined(__SAM3X8E__)
  serialSpecialInit(USART0, 612500);  //required for Due based boards as the stock core files don't support 612500 baud.
#endif

  SERIALCONSOLE.println("Started serial interface to BMS.");

  EEPROM.get(0, settings);
  if (settings.version != EEPROM_VERSION) {
    loadSettings();
  }
  Logger::setLoglevel(Logger::Off);  //Debug = 0, Info = 1, Warn = 2, Error = 3, Off = 4

  lastUpdate = 0;

  crc8.begin();
  digitalWrite(led, HIGH);
  bms.setPstrings(settings.Pstrings);
  bms.setSensors(settings.IgnoreTemp, settings.IgnoreVolt, settings.TempOff);


  //SOC recovery//

  SOC = (EEPROM.read(1000));
  if (settings.voltsoc == 1) {
    SOCmem = 0;
  } else {
    if (SOC > 100) {
      SOCmem = 0;
    } else {
      SOCmem = 1;
    }
  }

  SERIALCONSOLE.println("Recovery SOC: ");
  SERIALCONSOLE.print(SOC);

  ////Calculate fixed numbers
  pwmcurmin = (pwmcurmid / 50 * pwmcurmax * -1);
  ////

  //precharge timer kickers
  Pretimer = millis();
  Pretimer1 = millis();

  // setup interrupts
  //RISING/HIGH/CHANGE/LOW/FALLING
  attachInterrupt(IN4, isrCP, CHANGE);  // attach BUTTON 1 interrupt handler [ pin# 7 ]

  bmsstatus = Boot;
}

void loop() {

  canread();

  if (SERIALCONSOLE.available() > 0) {
    menu();
  }

  if (outputcheck != 1) {
    contcon();
    if (settings.ESSmode == 1) {
      if (bmsstatus != Error) {
        contctrl = contctrl | 4;  //turn on negative contactor

        if (settings.tripcont != 0) {
          if (bms.getLowCellVolt() > settings.UnderVSetpoint && bms.getHighCellVolt() < settings.OverVSetpoint) {
            if (digitalRead(OUT2) == LOW && digitalRead(OUT4) == LOW) {
              mainconttimer = millis();
              digitalWrite(OUT4, HIGH);  //Precharge start
              Serial.println();
              Serial.println("Precharge!!!");
              Serial.println(mainconttimer);
              Serial.println();
            }
            if (mainconttimer + settings.Pretime < millis() && digitalRead(OUT2) == LOW && abs(currentact) < settings.Precurrent) {
              digitalWrite(OUT2, HIGH);  //turn on contactor
              Serial.println();
              Serial.println("Main On!!!");
              Serial.println();
              mainconttimer = millis() + settings.Pretime;
            }
            if (mainconttimer + settings.Pretime + 1000 < millis()) {
              digitalWrite(OUT4, LOW);  //ensure precharge is low
            }
          } else {
            digitalWrite(OUT4, LOW);  //ensure precharge is low
            mainconttimer = 0;
          }
        }
        if (digitalRead(IN1) == LOW)  //Key OFF
        {
          if (storagemode == 1) {
            storagemode = 0;
          }
        } else {
          if (storagemode == 0) {
            storagemode = 1;
          }
        }
        if (bms.getHighCellVolt() > settings.balanceVoltage && bms.getHighCellVolt() > bms.getLowCellVolt() + settings.balanceHyst) {
          balancecells = 1;
        } else {
          balancecells = 0;
        }

        //Pretimer + settings.Pretime > millis();

        if (storagemode == 1) {
          if (bms.getHighCellVolt() > settings.StoreVsetpoint || chargecurrent == 0) {
            digitalWrite(OUT3, LOW);  //turn off charger
            contctrl = contctrl & 253;
            Pretimer = millis();
            Charged = 1;
            SOCcharged(2);
          } else {
            if (Charged == 1) {
              if (bms.getHighCellVolt() < (settings.StoreVsetpoint - settings.ChargeHys)) {
                Charged = 0;
                digitalWrite(OUT3, HIGH);  //turn on charger
                if (Pretimer + settings.Pretime < millis()) {
                  contctrl = contctrl | 2;
                  Pretimer = 0;
                }
              }
            } else {
              digitalWrite(OUT3, HIGH);  //turn on charger
              if (Pretimer + settings.Pretime < millis()) {
                contctrl = contctrl | 2;
                Pretimer = 0;
              }
            }
          }
        } else {
          if (bms.getHighCellVolt() > settings.OverVSetpoint || bms.getHighCellVolt() > settings.ChargeVsetpoint || chargecurrent == 0) {
            if ((millis() - overtriptimer) > settings.triptime) {
              digitalWrite(OUT3, LOW);  //turn off charger
              contctrl = contctrl & 253;
              Pretimer = millis();
              Charged = 1;
              SOCcharged(2);
            }
          } else {
            overtriptimer = millis();
            if (Charged == 1) {
              if (bms.getHighCellVolt() < (settings.ChargeVsetpoint - settings.ChargeHys)) {
                Charged = 0;
                digitalWrite(OUT3, HIGH);  //turn on charger
                if (Pretimer + settings.Pretime < millis()) {
                  // Serial.println();
                  //Serial.print(Pretimer);
                  contctrl = contctrl | 2;
                }
              }
            } else {
              digitalWrite(OUT3, HIGH);  //turn on charger
              if (Pretimer + settings.Pretime < millis()) {
                // Serial.println();
                //Serial.print(Pretimer);
                contctrl = contctrl | 2;
              }
            }
          }
        }
        if (bms.getLowCellVolt() < settings.UnderVSetpoint || bms.getLowCellVolt() < settings.DischVsetpoint) {
          if ((millis() - undertriptimer) > settings.triptime) {
            digitalWrite(OUT1, LOW);  //turn off discharge
            contctrl = contctrl & 254;
            Pretimer1 = millis();
          }
        } else {
          undertriptimer = millis();
          if (bms.getLowCellVolt() > settings.DischVsetpoint + settings.DischHys) {
            digitalWrite(OUT1, HIGH);  //turn on discharge
            if (Pretimer1 + settings.Pretime < millis()) {
              contctrl = contctrl | 1;
            }
          }
        }
        if (SOCset == 1) {
          if (settings.tripcont == 0) {
            if (bms.getLowCellVolt() < settings.UnderVSetpoint || bms.getHighCellVolt() > settings.OverVSetpoint || bms.getHighTemperature() > settings.OverTSetpoint) {
              digitalWrite(OUT2, HIGH);  //trip breaker
            } else {
              digitalWrite(OUT2, LOW);  //trip breaker
            }
          } else {
            if (bms.getLowCellVolt() < settings.UnderVSetpoint || bms.getHighCellVolt() > settings.OverVSetpoint || bms.getHighTemperature() > settings.OverTSetpoint) {
              digitalWrite(OUT2, LOW);  //turn off contactor
              digitalWrite(OUT4, LOW);  //ensure precharge is low
            }
          }
        }

      } else {
        /*
          digitalWrite(OUT2, HIGH);//trip breaker
          Discharge = 0;
          digitalWrite(OUT4, LOW);
          digitalWrite(OUT3, LOW);//turn off charger
          digitalWrite(OUT2, LOW);
          digitalWrite(OUT1, LOW);//turn off discharge
          contctrl = 0; //turn off out 5 and 6
        */
        if (SOCset == 1) {
          if (settings.tripcont == 0) {
            if (bms.getLowCellVolt() < settings.UnderVSetpoint || bms.getHighCellVolt() > settings.OverVSetpoint || bms.getHighTemperature() > settings.OverTSetpoint) {
              digitalWrite(OUT2, HIGH);  //trip breaker
            } else {
              digitalWrite(OUT2, LOW);  //trip breaker
            }
          } else {
            if (bms.getLowCellVolt() < settings.UnderVSetpoint || bms.getHighCellVolt() > settings.OverVSetpoint || bms.getHighTemperature() > settings.OverTSetpoint) {
              digitalWrite(OUT2, LOW);  //turn off contactor
              digitalWrite(OUT4, LOW);  //ensure precharge is low
            }
          }
          if (bms.getLowCellVolt() > settings.UnderVSetpoint || bms.getHighCellVolt() < settings.OverVSetpoint || bms.getHighTemperature() < settings.OverTSetpoint) {
            bmsstatus = Boot;
          }
        }
      }

      //pwmcomms();
    } else {
      switch (bmsstatus) {
        case (Boot):
          Discharge = 0;
          digitalWrite(OUT4, LOW);
          digitalWrite(OUT3, LOW);  //turn off charger
          digitalWrite(OUT2, LOW);
          digitalWrite(OUT1, LOW);  //turn off discharge
          contctrl = 0;
          bmsstatus = Ready;
          break;

        case (Ready):
          Discharge = 0;
          digitalWrite(OUT4, LOW);
          digitalWrite(OUT3, LOW);  //turn off charger
          digitalWrite(OUT2, LOW);
          digitalWrite(OUT1, LOW);  //turn off discharge
          contctrl = 0;             //turn off out 5 and 6
          if (bms.getHighCellVolt() > settings.balanceVoltage && bms.getHighCellVolt() > bms.getLowCellVolt() + settings.balanceHyst) {
            //bms.balanceCells();
            balancecells = 1;
          } else {
            balancecells = 0;
          }
          if (digitalRead(IN3) == HIGH && (bms.getHighCellVolt() < (settings.ChargeVsetpoint - settings.ChargeHys)))  //detect AC present for charging and check not balancing
          {
            if (settings.ChargerDirect == 1) {
              bmsstatus = Charge;
            } else {
              bmsstatus = Precharge;
              Pretimer = millis();
            }
          }
          if (digitalRead(IN1) == HIGH && bms.getLowCellVolt() > settings.DischVsetpoint)  //detect Key ON
          {
            bmsstatus = Precharge;
            Pretimer = millis();
          }

          break;

        case (Precharge):
          Discharge = 0;
          Prechargecon();
          break;


        case (Drive):
          Discharge = 1;
          if (digitalRead(IN1) == LOW)  //Key OFF
          {
            bmsstatus = Ready;
          }
          if (digitalRead(IN3) == HIGH && (bms.getHighCellVolt() < (settings.ChargeVsetpoint - settings.ChargeHys)))  //detect AC present for charging and check not balancing
          {
            bmsstatus = Charge;
          }

          break;

        case (Charge):
          Discharge = 0;
          if (digitalRead(IN2) == HIGH) {
            chargecurrentlimit = true;
          } else {
            chargecurrentlimit = false;
          }
          digitalWrite(OUT3, HIGH);  //enable charger
          if (bms.getHighCellVolt() > settings.balanceVoltage) {
            //bms.balanceCells();
            balancecells = 1;
          } else {
            balancecells = 0;
          }
          if (bms.getHighCellVolt() > settings.ChargeVsetpoint) {
            if (bms.getAvgCellVolt() > (settings.ChargeVsetpoint - settings.ChargeHys)) {
              SOCcharged(2);
            } else {
              SOCcharged(1);
            }
            digitalWrite(OUT3, LOW);  //turn off charger
            bmsstatus = Ready;
          }
          if (digitalRead(IN3) == LOW)  //detect AC not present for charging
          {
            bmsstatus = Ready;
          }
          break;

        case (Error):
          Discharge = 0;
          digitalWrite(OUT4, LOW);
          digitalWrite(OUT3, LOW);  //turn off charger
          digitalWrite(OUT2, LOW);
          digitalWrite(OUT1, LOW);  //turn off discharge
          contctrl = 0;             //turn off out 5 and 6

          if (bms.getLowCellVolt() > settings.UnderVSetpoint && bms.getHighCellVolt() < settings.OverVSetpoint && digitalRead(IN1) == LOW)  //Key OFF
          {
            bmsstatus = Ready;
          }

          break;
      }
    }
    if (settings.cursens == Analoguedual || settings.cursens == Analoguesing) {
      getcurrent();
    }
  }
  if (millis() - commandtime > commandrate) {
    commandtime = millis();
    sendcommand();
  }


  if (millis() - looptime > 500) {
    looptime = millis();
    bms.getAllVoltTemp();
    //UV  check
    if (settings.ESSmode == 1) {
      if (SOCset != 0) {
        if (bms.getLowCellVolt() < settings.UnderVSetpoint || bms.getHighCellVolt() < settings.UnderVSetpoint) {
          if (debug != 0) {
            SERIALCONSOLE.println("  ");
            SERIALCONSOLE.print("   !!! Undervoltage Fault !!!");
            SERIALCONSOLE.println("  ");
          }
          bmsstatus = Error;
          ErrorReason = 1;
        }
      }
    } else  //In 'vehicle' mode
    {
      if (SOCset != 0) {
        if (bms.getLowCellVolt() < settings.UnderVSetpoint) {
          if (UnderTime < millis())  //check is last time not undervoltage is longer thatn UnderDur ago
          {
            bmsstatus = Error;
          }
        } else {
          UnderTime = millis() + settings.triptime;
        }

        if (bms.getHighCellVolt() < settings.UnderVSetpoint || bms.getHighTemperature() > settings.OverTSetpoint) {
          bmsstatus = Error;
        }

        if (bms.getHighCellVolt() > settings.OverVSetpoint) {
          if (OverTime < millis())  //check is last time not undervoltage is longer thatn UnderDur ago
          {
            bmsstatus = Error;
          }
        } else {
          OverTime = millis() + settings.triptime;
        }
      }
    }

    if (debug != 0) {
      printbmsstat();
      bms.printPackDetails(debugdigits, settings.CSCvariant);
    }
    if (CSVdebug != 0) {
      bms.printAllCSV(millis(), currentact, SOC);
    }
    if (inputcheck != 0) {
      inputdebug();
    }

    if (outputcheck != 0) {
      outputdebug();
    } else {
      gaugeupdate();
    }

    updateSOC();
    currentlimit();
    if (SOCset != 0) {
      alarmupdate();
    }
    VEcan();

    if (cellspresent == 0 && millis() > 3000) {
      cellspresent = bms.seriescells();  //set amount of connected cells, might need delay
      bms.setSensors(settings.IgnoreTemp, settings.IgnoreVolt, settings.TempOff);
    } else {
      int currentCells = bms.seriescells();
      if (currentCells > cellspresent) {
        // More cells detected - update to new count (modules coming online)
        cellspresent = currentCells;
        if (debug != 0) {
          SERIALCONSOLE.println("  ");
          SERIALCONSOLE.print("   >>> Cell count updated to: ");
          SERIALCONSOLE.print(cellspresent);
          SERIALCONSOLE.println("  ");
        }
      } else if (cellspresent != currentCells) {
        // Fewer cells detected - this is a fault
        if (debug != 0) {
          SERIALCONSOLE.println("  ");
          SERIALCONSOLE.print("   !!! Series Cells Fault !!! ");
          SERIALCONSOLE.print(cellspresent);
          SERIALCONSOLE.print("/");
          SERIALCONSOLE.print(currentCells);
          SERIALCONSOLE.println("  ");
        }
        bmsstatus = Error;
        ErrorReason = 3;
      }
    }

    if (CSVdebug != 1) {
      dashupdate();
    }

    ///stop reading voltages during balancing//
    if ((settings.balanceDuty + 5) > ((balancetimer - millis()) * 0.001)) {
      bms.setBalIgnore(true);
      /*
        Serial.println();
        Serial.println("Ignore Voltages Balancing Active");
      */
    } else {
      bms.setBalIgnore(false);
    }

    ///Set Ids to unnasgined//

    if (Unassigned > 0) {
      assignID();
    }

    ////

    resetwdog();
  }
  if (millis() - cleartime > 5000) {
    //bms.clearmodules(); // Not functional
    if (bms.checkcomms()) {
      //no missing modules
      /*
        SERIALCONSOLE.println("  ");
        SERIALCONSOLE.print(" ALL OK NO MODULE MISSING :) ");
        SERIALCONSOLE.println("  ");
      */
      if (bmsstatus == Error) {
        bmsstatus = Boot;
      }
    } else {
      //missing module
      if (debug != 0) {
        SERIALCONSOLE.println("  ");
        SERIALCONSOLE.print("   !!! MODULE MISSING !!!");
        SERIALCONSOLE.println("  ");
      }
      bmsstatus = Error;
      ErrorReason = 4;
    }
    cleartime = millis();
  }
  // Cast settings.chargerspd to uint32_t for comparison
  if (millis() - looptime1 > static_cast<uint32_t>(settings.chargerspd)) {
    looptime1 = millis();
    if (settings.ESSmode == 1) {
      chargercomms();
    } else {
      if (bmsstatus == Charge) {
        chargercomms();
      }
    }
  }
}

void alarmupdate() {
  alarm[0] = 0x00;
  if (settings.OverVSetpoint < bms.getHighCellVolt()) {
    alarm[0] = 0x04;
  }
  if (bms.getLowCellVolt() < settings.UnderVSetpoint) {
    alarm[0] |= 0x10;
  }
  if (bms.getHighTemperature() > settings.OverTSetpoint) {
    alarm[0] |= 0x40;
  }
  alarm[1] = 0;
  if (bms.getLowTemperature() < settings.UnderTSetpoint) {
    alarm[1] = 0x01;
  }
  alarm[3] = 0;
  if ((bms.getHighCellVolt() - bms.getLowCellVolt()) > settings.CellGap) {
    alarm[3] = 0x01;
  }

  ///warnings///
  warning[0] = 0;

  if (bms.getHighCellVolt() > (settings.OverVSetpoint - settings.WarnOff)) {
    warning[0] = 0x04;
  }
  if (bms.getLowCellVolt() < (settings.UnderVSetpoint + settings.WarnOff)) {
    warning[0] |= 0x10;
  }

  if (bms.getHighTemperature() > (settings.OverTSetpoint - settings.WarnToff)) {
    warning[0] |= 0x40;
  }
  warning[1] = 0;
  if (bms.getLowTemperature() < (settings.UnderTSetpoint + settings.WarnToff)) {
    warning[1] = 0x01;
  }
}

void gaugeupdate() {
  if (gaugedebug != 0) {
    SOCtest = SOCtest + 5;
    if (SOCtest > 1000) {
      SOCtest = 0;
    }
    analogWrite(OUT8, map(SOCtest * 0.1, 0, 100, settings.gaugelow, settings.gaugehigh));

    SERIALCONSOLE.println("  ");
    SERIALCONSOLE.print("SOC : ");
    SERIALCONSOLE.print(SOCtest * 0.1);
    SERIALCONSOLE.print("  fuel pwm : ");
    SERIALCONSOLE.print(map(SOCtest * 0.1, 0, 100, settings.gaugelow, settings.gaugehigh));
    SERIALCONSOLE.println("  ");
  } else {
    analogWrite(OUT8, map(SOC, 0, 100, settings.gaugelow, settings.gaugehigh));
  }
}

void printbmsstat() {
  SERIALCONSOLE.println();
  SERIALCONSOLE.println();
  SERIALCONSOLE.println();
  SERIALCONSOLE.print("BMS Status : ");
  if (settings.ESSmode == 1) {
    SERIALCONSOLE.print("ESS Mode ");

    if (bms.getLowCellVolt() < settings.UnderVSetpoint) {
      SERIALCONSOLE.print(": UnderVoltage ");
    }
    if (bms.getHighCellVolt() > settings.OverVSetpoint) {
      SERIALCONSOLE.print(": OverVoltage ");
    }
    if ((bms.getHighCellVolt() - bms.getLowCellVolt()) > settings.CellGap) {
      SERIALCONSOLE.print(": Cell Imbalance ");
    }
    if (bms.getAvgTemperature() > settings.OverTSetpoint) {
      SERIALCONSOLE.print(": Over Temp ");
    }
    if (bms.getAvgTemperature() < settings.UnderTSetpoint) {
      SERIALCONSOLE.print(": Under Temp ");
    }
    if (storagemode == 1) {
      if (bms.getLowCellVolt() > settings.StoreVsetpoint) {
        SERIALCONSOLE.print(": OverVoltage Storage ");
        SERIALCONSOLE.print(": UNhappy:");
      } else {
        SERIALCONSOLE.print(": Happy ");
      }
    } else {
      if (bms.getLowCellVolt() > settings.UnderVSetpoint && bms.getHighCellVolt() < settings.OverVSetpoint) {

        if (bmsstatus == Error) {
          SERIALCONSOLE.print(": UNhappy:");
        } else {
          SERIALCONSOLE.print(": Happy ");
        }
      }
    }
  } else {
    SERIALCONSOLE.print(bmsstatus);
    switch (bmsstatus) {
      case (Boot):
        SERIALCONSOLE.print(" Boot ");
        break;

      case (Ready):
        SERIALCONSOLE.print(" Ready ");
        break;

      case (Precharge):
        SERIALCONSOLE.print(" Precharge ");
        break;

      case (Drive):
        SERIALCONSOLE.print(" Drive ");
        break;

      case (Charge):
        SERIALCONSOLE.print(" Charge ");
        break;

      case (Error):
        SERIALCONSOLE.print(" Error ");
        break;
    }
  }
  SERIALCONSOLE.print("  ");
  if (digitalRead(IN3) == HIGH) {
    SERIALCONSOLE.print("| AC Present |");
  }
  if (digitalRead(IN1) == HIGH) {
    SERIALCONSOLE.print("| Key ON |");
  }
  if (balancecells == 1) {
    SERIALCONSOLE.print("|Balancing Active");
    SERIALCONSOLE.print("|");
    //SERIALCONSOLE.print(balancepauze);
    //SERIALCONSOLE.print("| Counter: ");
    SERIALCONSOLE.print((balancetimer - millis()) * 0.001, 0);
    SERIALCONSOLE.print("|");
  }
  SERIALCONSOLE.print("  ");
  SERIALCONSOLE.print(cellspresent);
  SERIALCONSOLE.println();
  SERIALCONSOLE.print("Out:");
  SERIALCONSOLE.print(digitalRead(OUT1));
  SERIALCONSOLE.print(digitalRead(OUT2));
  SERIALCONSOLE.print(digitalRead(OUT3));
  SERIALCONSOLE.print(digitalRead(OUT4));
  SERIALCONSOLE.print(" Cont:");
  if ((contstat & 1) == 1) {
    SERIALCONSOLE.print("1");
  } else {
    SERIALCONSOLE.print("0");
  }
  if ((contstat & 2) == 2) {
    SERIALCONSOLE.print("1");
  } else {
    SERIALCONSOLE.print("0");
  }
  if ((contstat & 4) == 4) {
    SERIALCONSOLE.print("1");
  } else {
    SERIALCONSOLE.print("0");
  }
  if ((contstat & 8) == 8) {
    SERIALCONSOLE.print("1");
  } else {
    SERIALCONSOLE.print("0");
  }
  SERIALCONSOLE.print(" In:");
  SERIALCONSOLE.print(digitalRead(IN1));
  SERIALCONSOLE.print(digitalRead(IN2));
  SERIALCONSOLE.print(digitalRead(IN3));
  SERIALCONSOLE.print(digitalRead(IN4));
  /*
    SERIALCONSOLE.print(" | ");
    SERIALCONSOLE.print(bms.getLowTemperature());
    SERIALCONSOLE.print(" | ");
    SERIALCONSOLE.print(bms.getHighTemperature());
  */

  SERIALCONSOLE.print(" Charge Current Limit : ");
  SERIALCONSOLE.print(chargecurrent * 0.1, 0);
  SERIALCONSOLE.print(" A DisCharge Current Limit : ");
  SERIALCONSOLE.print(discurrent * 0.1, 0);
  SERIALCONSOLE.print(" A");

  if (bmsstatus == Charge || accurlim > 0) {
    Serial.print("  CP AC Current Limit: ");
    Serial.print(accurlim);
    Serial.print(" A");
  }

  if (bmsstatus == Charge && CPdebug == 1) {
    Serial.print("A  CP Dur: ");
    Serial.print(duration);
    Serial.print("  Charge Power : ");
    Serial.print(chargerpower);
    if (chargecurrentlimit == false) {
      SERIALCONSOLE.print("  No Charge Current Limit");
    } else {
      SERIALCONSOLE.print("  Charge Current Limit Active");
    }
  }
}


void getcurrent() {
  if (settings.cursens == Analoguedual || settings.cursens == Analoguesing) {
    if (settings.cursens == Analoguedual) {
      if (currentact < settings.changecur && currentact > (settings.changecur * -1)) {
        sensor = 1;
        adc->adc0->startContinuous(ACUR1);
      } else {
        sensor = 2;
        adc->adc0->startContinuous(ACUR2);
      }
    } else {
      sensor = 1;
      adc->adc0->startContinuous(ACUR1);
    }
    if (sensor == 1) {
      if (debugCur != 0) {
        SERIALCONSOLE.println();
        if (settings.cursens == Analoguedual) {
          SERIALCONSOLE.print("Low Range: ");
        } else {
          SERIALCONSOLE.print("Single In: ");
        }
        SERIALCONSOLE.print("Value ADC0: ");
      }
      value = (uint16_t)adc->adc0->analogReadContinuous();  // the unsigned is necessary for 16 bits, otherwise values larger than 3.3/2 V are negative!
      if (debugCur != 0) {
        SERIALCONSOLE.print(value * 3300 / adc->adc0->getMaxValue());  //- settings.offset1)
        SERIALCONSOLE.print(" ");
        SERIALCONSOLE.print(settings.offset1);
      }
      RawCur = int16_t((value * 3300 / adc->adc0->getMaxValue()) - settings.offset1) / (settings.convlow * 0.0000066);

      if (abs((int16_t(value * 3300 / adc->adc0->getMaxValue()) - settings.offset1)) < settings.CurDead) {
        RawCur = 0;
      }
      if (debugCur != 0) {
        SERIALCONSOLE.print("  ");
        SERIALCONSOLE.print(int16_t(value * 3300 / adc->adc0->getMaxValue()) - settings.offset1);
        SERIALCONSOLE.print("  ");
        SERIALCONSOLE.print(RawCur);
        SERIALCONSOLE.print(" mA");
        SERIALCONSOLE.print("  ");
      }
    } else {
      if (debugCur != 0) {
        SERIALCONSOLE.println();
        SERIALCONSOLE.print("High Range: ");
        SERIALCONSOLE.print("Value ADC0: ");
      }
      value = (uint16_t)adc->adc0->analogReadContinuous();  // the unsigned is necessary for 16 bits, otherwise values larger than 3.3/2 V are negative!
      if (debugCur != 0) {
        SERIALCONSOLE.print(value * 3300 / adc->adc0->getMaxValue());  //- settings.offset2)
        SERIALCONSOLE.print("  ");
        SERIALCONSOLE.print(settings.offset2);
      }
      RawCur = int16_t((value * 3300 / adc->adc0->getMaxValue()) - settings.offset2) / (settings.convhigh * 0.0000066);
      if (static_cast<unsigned long>(value) < 100 || static_cast<unsigned long>(value) > (adc->adc0->getMaxValue() - 100)) {
        RawCur = 0;
      }
      if (debugCur != 0) {
        SERIALCONSOLE.print("  ");
        SERIALCONSOLE.print((float(value * 3300 / adc->adc0->getMaxValue()) - settings.offset2));
        SERIALCONSOLE.print("  ");
        SERIALCONSOLE.print(RawCur);
        SERIALCONSOLE.print("mA");
        SERIALCONSOLE.print("  ");
      }
    }
  }

  if (settings.invertcur == 1) {
    RawCur = RawCur * -1;
  }

  lowpassFilter.input(RawCur);
  if (debugCur != 0) {
    SERIALCONSOLE.print(lowpassFilter.output());
    SERIALCONSOLE.print(" | ");
    SERIALCONSOLE.print(settings.changecur);
    SERIALCONSOLE.print(" | ");
  }

  currentact = lowpassFilter.output();

  if (debugCur != 0) {
    SERIALCONSOLE.print(currentact);
    SERIALCONSOLE.print("mA  ");
  }

  if (settings.cursens == Analoguedual) {
    if (sensor == 1) {
      if (currentact > 500 || currentact < -500) {
        ampsecond = ampsecond + ((currentact * (millis() - lasttime) / 1000) / 1000);
        lasttime = millis();
      } else {
        lasttime = millis();
      }
    }
    if (sensor == 2) {
      if (currentact > settings.changecur || currentact < (settings.changecur * -1)) {
        ampsecond = ampsecond + ((currentact * (millis() - lasttime) / 1000) / 1000);
        lasttime = millis();
      } else {
        lasttime = millis();
      }
    }
  } else {
    if (currentact > 500 || currentact < -500) {
      ampsecond = ampsecond + ((currentact * (millis() - lasttime) / 1000) / 1000);
      lasttime = millis();
    } else {
      lasttime = millis();
    }
  }
  currentact = settings.ncur * currentact;
  RawCur = 0;
  /*
    AverageCurrentTotal = AverageCurrentTotal - RunningAverageBuffer[NextRunningAverage];

    RunningAverageBuffer[NextRunningAverage] = currentact;

    if (debugCur != 0)
    {
      SERIALCONSOLE.print(" | ");
      SERIALCONSOLE.print(AverageCurrentTotal);
      SERIALCONSOLE.print(" | ");
      SERIALCONSOLE.print(RunningAverageBuffer[NextRunningAverage]);
      SERIALCONSOLE.print(" | ");
    }
    AverageCurrentTotal = AverageCurrentTotal + RunningAverageBuffer[NextRunningAverage];
    if (debugCur != 0)
    {
      SERIALCONSOLE.print(" | ");
      SERIALCONSOLE.print(AverageCurrentTotal);
      SERIALCONSOLE.print(" | ");
    }

    NextRunningAverage = NextRunningAverage + 1;

    if (NextRunningAverage > RunningAverageCount)
    {
      NextRunningAverage = 0;
    }

    AverageCurrent = AverageCurrentTotal / (RunningAverageCount + 1);

    if (debugCur != 0)
    {
      SERIALCONSOLE.print(AverageCurrent);
      SERIALCONSOLE.print(" | ");
      SERIALCONSOLE.print(AverageCurrentTotal);
      SERIALCONSOLE.print(" | ");
      SERIALCONSOLE.print(NextRunningAverage);
    }
  */
}


void updateSOC() {
   if (SOCreset == 1) {
    SOC = map(uint16_t(bms.getLowCellVolt() * 1000), settings.socvolt[0], settings.socvolt[2], settings.socvolt[1], settings.socvolt[3]);
    ampsecond = (SOC * settings.CAP * settings.Pstrings * 10) / 0.27777777777778;
    SOCreset = 0;
  }

  if (SOCset == 0 && SOCmem == 0) {
         if (millis() > 9000) {
      bms.setSensors(settings.IgnoreTemp, settings.IgnoreVolt, settings.TempOff);
    }
    if (millis() > 10000) {
      SOC = map(uint16_t(bms.getLowCellVolt() * 1000), settings.socvolt[0], settings.socvolt[2], settings.socvolt[1], settings.socvolt[3]);

      ampsecond = (SOC * settings.CAP * settings.Pstrings * 10) / 0.27777777777778;
      SOCset = 1;
      if (debug != 0) {
        SERIALCONSOLE.println("  ");
        SERIALCONSOLE.println("//////////////////////////////////////// SOC SET ////////////////////////////////////////");
      }
      if (settings.ESSmode == 1) {
        bmsstatus = Ready;
      }
    }
  }

  if (SOCset == 0 && SOCmem == 1) {
    ampsecond = (SOC * settings.CAP * settings.Pstrings * 10) / 0.27777777777778;
         if (millis() > 9000) {
      bms.setSensors(settings.IgnoreTemp, settings.IgnoreVolt, settings.TempOff);
    }
    if (millis() > 10000) {
      SOCset = 1;
      if (debug != 0) {
        SERIALCONSOLE.println("  ");
        SERIALCONSOLE.println("//////////////////////////////////////// SOC SET ////////////////////////////////////////");
      }
      if (settings.ESSmode == 1) {
        bmsstatus = Ready;
      }
    }
  }

  SOC = ((ampsecond * 0.27777777777778) / (settings.CAP * settings.Pstrings * 1000)) * 100;

  if (settings.voltsoc == 1 || settings.cursens == 0) {
    SOC = map(uint16_t(bms.getLowCellVolt() * 1000), settings.socvolt[0], settings.socvolt[2], settings.socvolt[1], settings.socvolt[3]);

    ampsecond = (SOC * settings.CAP * settings.Pstrings * 10) / 0.27777777777778;
  }

  if (SOC >= 100) {
    ampsecond = (settings.CAP * settings.Pstrings * 1000) / 0.27777777777778;  //reset to full, dependant on given capacity. Need to improve with auto correction for capcity.
    SOC = 100;
  }


  if (SOC < 0) {
    SOC = 0;  //reset SOC this way the can messages remain in range for other devices. Ampseconds will keep counting.
  }

  if (debug != 0) {
    if (settings.cursens == Analoguedual) {
      if (sensor == 1) {
        SERIALCONSOLE.print("Low Range ");
      } else {
        SERIALCONSOLE.print("High Range");
      }
    }
    if (settings.cursens == Analoguesing) {
      SERIALCONSOLE.print("Analogue Single ");
    }
    if (settings.cursens == Canbus) {
      SERIALCONSOLE.print("CANbus ");
    }
    SERIALCONSOLE.print("  ");
    SERIALCONSOLE.print(currentact);
    SERIALCONSOLE.print("mA");
    SERIALCONSOLE.print("  ");
    SERIALCONSOLE.print(SOC);
    SERIALCONSOLE.print("% SOC ");
    SERIALCONSOLE.print(ampsecond * 0.27777777777778, 2);
    SERIALCONSOLE.print("mAh");
  }
}

void SOCcharged(int y) {
  if (y == 1) {
    SOC = 95;
    ampsecond = (settings.CAP * settings.Pstrings * 1000) / 0.27777777777778;  //reset to full, dependant on given capacity. Need to improve with auto correction for capcity.
  }
  if (y == 2) {
    SOC = 100;
    ampsecond = (settings.CAP * settings.Pstrings * 1000) / 0.27777777777778;  //reset to full, dependant on given capacity. Need to improve with auto correction for capcity.
  }
}

void Prechargecon() {
  if (digitalRead(IN1) == HIGH || digitalRead(IN3) == HIGH)  //detect Key ON or AC present
  {
    digitalWrite(OUT4, HIGH);  //Negative Contactor Close
    contctrl = 2;
    if (Pretimer + settings.Pretime > millis() || currentact > settings.Precurrent) {
      digitalWrite(OUT2, HIGH);  //precharge
    } else                       //close main contactor
    {
      digitalWrite(OUT1, HIGH);  //Positive Contactor Close
      contctrl = 3;
      if (settings.ChargerDirect == 1) {
        bmsstatus = Drive;
      } else {
        if (digitalRead(IN3) == HIGH) {
          bmsstatus = Charge;
        }
        if (digitalRead(IN1) == HIGH) {
          bmsstatus = Drive;
        }
      }
      digitalWrite(OUT2, LOW);
    }
  } else {
    digitalWrite(OUT1, LOW);
    digitalWrite(OUT2, LOW);
    digitalWrite(OUT4, LOW);
    bmsstatus = Ready;
    contctrl = 0;
  }
}

void contcon() {
  if (contctrl != contstat)  //check for contactor request change
  {
    if ((contctrl & 1) == 0) {
      analogWrite(OUT5, 0);
      contstat = contstat & 254;
    }
    if ((contctrl & 2) == 0) {
      analogWrite(OUT6, 0);
      contstat = contstat & 253;
    }
    if ((contctrl & 4) == 0) {
      analogWrite(OUT7, 0);
      contstat = contstat & 251;
    }


    if ((contctrl & 1) == 1) {
      if ((contstat & 1) != 1) {
        if (conttimer1 == 0) {
          analogWrite(OUT5, 255);
          conttimer1 = millis() + pulltime;
        }
        if (conttimer1 < millis()) {
          analogWrite(OUT5, settings.conthold);
          contstat = contstat | 1;
          conttimer1 = 0;
        }
      }
    }

    if ((contctrl & 2) == 2) {
      if ((contstat & 2) != 2) {
        if (conttimer2 == 0) {
          Serial.println();
          Serial.println("pull in OUT6");
          analogWrite(OUT6, 255);
          conttimer2 = millis() + pulltime;
        }
        if (conttimer2 < millis()) {
          analogWrite(OUT6, settings.conthold);
          contstat = contstat | 2;
          conttimer2 = 0;
        }
      }
    }
    if ((contctrl & 4) == 4) {
      if ((contstat & 4) != 4) {
        if (conttimer3 == 0) {
          Serial.println();
          Serial.println("pull in OUT7");
          analogWrite(OUT7, 255);
          conttimer3 = millis() + pulltime;
        }
        if (conttimer3 < millis()) {
          analogWrite(OUT7, settings.conthold);
          contstat = contstat | 4;
          conttimer3 = 0;
        }
      }
    }
    /*
       SERIALCONSOLE.print(conttimer);
       SERIALCONSOLE.print("  ");
       SERIALCONSOLE.print(contctrl);
       SERIALCONSOLE.print("  ");
       SERIALCONSOLE.print(contstat);
       SERIALCONSOLE.println("  ");
    */
  }
  if (contctrl == 0) {
    analogWrite(OUT5, 0);
    analogWrite(OUT6, 0);
  }
}

void calcur() {
  adc->adc0->startContinuous(ACUR1);
  sensor = 1;
  x = 0;
  SERIALCONSOLE.print(" Calibrating Current Offset ::::: ");
  while (x < 20) {
    settings.offset1 = settings.offset1 + ((uint16_t)adc->adc0->analogReadContinuous() * 3300 / adc->adc0->getMaxValue());
    SERIALCONSOLE.print(".");
    delay(100);
    x++;
  }
  settings.offset1 = settings.offset1 / 21;
  SERIALCONSOLE.print(settings.offset1);
  SERIALCONSOLE.print(" current offset 1 calibrated ");
  SERIALCONSOLE.println("  ");
  x = 0;
  adc->startContinuous(ACUR2, ADC_0);
  sensor = 2;
  SERIALCONSOLE.print(" Calibrating Current Offset ::::: ");
  while (x < 20) {
    settings.offset2 = settings.offset2 + ((uint16_t)adc->adc0->analogReadContinuous() * 3300 / adc->adc0->getMaxValue());
    SERIALCONSOLE.print(".");
    delay(100);
    x++;
  }
  settings.offset2 = settings.offset2 / 21;
  SERIALCONSOLE.print(settings.offset2);
  SERIALCONSOLE.print(" current offset 2 calibrated ");
  SERIALCONSOLE.println("  ");
}

void VEcan()  //communication with Victron system over CAN
{
  if (settings.chargertype == 6) {
    msg.id = 0x618;
    msg.len = 8;
    msg.buf[0] = 0x00;
    msg.buf[1] = 'B';
    msg.buf[2] = 'Y';
    msg.buf[3] = 'D';
    msg.buf[4] = 0x00;
    msg.buf[5] = 0x00;
    msg.buf[6] = 0x00;
    msg.buf[7] = 0x00;
    Can0.write(msg);

    delay(2);
    msg.id = 0x5D8;
    msg.len = 8;
    msg.buf[0] = 0x00;
    msg.buf[1] = 'B';
    msg.buf[2] = 'Y';
    msg.buf[3] = 'D';
    msg.buf[4] = 0x00;
    msg.buf[5] = 0x00;
    msg.buf[6] = 0x00;
    msg.buf[7] = 0x00;
    Can0.write(msg);

    delay(2);

    msg.id = 0x558;
    msg.len = 8;
    msg.buf[0] = 0x03;
    msg.buf[1] = 0x12;
    msg.buf[2] = 0x00;
    msg.buf[3] = 0x04;
    msg.buf[4] = highByte(settings.CAP * settings.Pstrings * 37 * settings.Scells);
    msg.buf[5] = lowByte(settings.CAP * settings.Pstrings * 37 * settings.Scells);
    msg.buf[6] = 0x05;
    msg.buf[7] = 0x07;
    Can0.write(msg);

    delay(2);

    msg.id = 0x598;
    msg.len = 8;
    msg.buf[0] = 0x00;
    msg.buf[1] = 0x00;
    msg.buf[2] = 0x12;
    msg.buf[3] = 0x34;
    msg.buf[4] = 0x00;
    msg.buf[5] = 0x00;
    msg.buf[6] = 0x04;
    msg.buf[7] = 0x4F;
    Can0.write(msg);

    delay(2);

    msg.id = 0x358;
    msg.len = 8;
    if (storagemode == 0) {
      msg.buf[0] = highByte(uint16_t((settings.ChargeVsetpoint * settings.Scells) * 10));
      msg.buf[1] = lowByte(uint16_t((settings.ChargeVsetpoint * settings.Scells) * 10));
    } else {
      msg.buf[0] = highByte(uint16_t((settings.StoreVsetpoint * settings.Scells) * 10));
      msg.buf[1] = lowByte(uint16_t((settings.StoreVsetpoint * settings.Scells) * 10));
    }
    msg.buf[2] = highByte(uint16_t((settings.DischVsetpoint * settings.Scells) * 10));
    msg.buf[3] = lowByte(uint16_t((settings.DischVsetpoint * settings.Scells) * 10));
    msg.buf[4] = highByte(discurrent);
    msg.buf[5] = lowByte(discurrent);
    msg.buf[6] = highByte(chargecurrent);
    msg.buf[7] = lowByte(chargecurrent);
    Can0.write(msg);

    delay(2);

    msg.id = 0x3D8;
    msg.len = 8;
    msg.buf[0] = highByte(SOC * 100);
    msg.buf[1] = lowByte(SOC * 100);
    msg.buf[2] = highByte(SOH);
    msg.buf[3] = lowByte(SOH);
    msg.buf[4] = highByte(uint16_t(ampsecond * 0.002777778));
    msg.buf[5] = lowByte(uint16_t(ampsecond * 0.002777778));
    msg.buf[6] = 0xF9;
    msg.buf[7] = 0;
    Can0.write(msg);

    delay(2);

    msg.id = 0x458;
    msg.len = 8;
    msg.buf[0] = 0x00;
    msg.buf[1] = 0x00;
    msg.buf[2] = 0x12;
    msg.buf[3] = 0x34;
    msg.buf[4] = 0x00;
    msg.buf[5] = 0x00;
    msg.buf[6] = 0x56;
    msg.buf[7] = 0x78;
    Can0.write(msg);

    delay(2);

    msg.id = 0x518;
    msg.len = 8;
    msg.buf[0] = highByte(uint16_t(bms.getHighTemperature() * 10));
    msg.buf[1] = lowByte(uint16_t(bms.getHighTemperature() * 10));
    msg.buf[2] = highByte(uint16_t(bms.getLowTemperature() * 10));
    msg.buf[3] = lowByte(uint16_t(bms.getLowTemperature() * 10));
    msg.buf[4] = 0xFF;
    msg.buf[5] = 0xFF;
    msg.buf[6] = 0xFF;
    msg.buf[7] = 0xFF;

    Can0.write(msg);

    delay(2);

    msg.id = 0x4D8;
    msg.len = 8;
    msg.buf[0] = highByte(uint16_t(bms.getPackVoltage() * 10));
    msg.buf[1] = lowByte(uint16_t(bms.getPackVoltage() * 10));
    msg.buf[2] = highByte(long(currentact / 100));
    msg.buf[3] = lowByte(long(currentact / 100));
    msg.buf[4] = highByte(int16_t(bms.getAvgTemperature() * 10));
    msg.buf[5] = lowByte(int16_t(bms.getAvgTemperature() * 10));
    msg.buf[6] = 0x03;
    msg.buf[7] = 0x08;
    Can0.write(msg);

    delay(2);
    msg.id = 0x158;
    msg.len = 8;
    msg.buf[0] = alarm[0];    //High temp  Low Voltage | High Voltage
    msg.buf[1] = alarm[1];    // High Discharge Current | Low Temperature
    msg.buf[2] = alarm[2];    //Internal Failure | High Charge current
    msg.buf[3] = alarm[3];    // Cell Imbalance
    msg.buf[4] = warning[0];  //High temp  Low Voltage | High Voltage
    msg.buf[5] = warning[1];  // High Discharge Current | Low Temperature
    msg.buf[6] = warning[2];  //Internal Failure | High Charge current
    msg.buf[7] = warning[3];  // Cell Imbalance
    Can0.write(msg);

  } else {
    msg.id = 0x351;
    msg.len = 8;
    if (storagemode == 0) {
      msg.buf[0] = lowByte(uint16_t((settings.ChargeVsetpoint * settings.Scells) * 10));
      msg.buf[1] = highByte(uint16_t((settings.ChargeVsetpoint * settings.Scells) * 10));
    } else {
      msg.buf[0] = lowByte(uint16_t((settings.StoreVsetpoint * settings.Scells) * 10));
      msg.buf[1] = highByte(uint16_t((settings.StoreVsetpoint * settings.Scells) * 10));
    }
    msg.buf[2] = lowByte(chargecurrent);
    msg.buf[3] = highByte(chargecurrent);
    msg.buf[4] = lowByte(discurrent);
    msg.buf[5] = highByte(discurrent);
    msg.buf[6] = lowByte(uint16_t((settings.DischVsetpoint * settings.Scells) * 10));
    msg.buf[7] = highByte(uint16_t((settings.DischVsetpoint * settings.Scells) * 10));
    Can0.write(msg);

    msg.id = 0x355;
    msg.len = 8;
    msg.buf[0] = lowByte(SOC);
    msg.buf[1] = highByte(SOC);
    msg.buf[2] = lowByte(SOH);
    msg.buf[3] = highByte(SOH);
    msg.buf[4] = lowByte(SOC * 10);
    msg.buf[5] = highByte(SOC * 10);
    msg.buf[6] = 0;
    msg.buf[7] = 0;
    Can0.write(msg);

    msg.id = 0x356;
    msg.len = 8;
    msg.buf[0] = lowByte(uint16_t(bms.getPackVoltage() * 100));
    msg.buf[1] = highByte(uint16_t(bms.getPackVoltage() * 100));
    msg.buf[2] = lowByte(long(currentact / 100));
    msg.buf[3] = highByte(long(currentact / 100));
    msg.buf[4] = lowByte(int16_t(bms.getAvgTemperature() * 10));
    msg.buf[5] = highByte(int16_t(bms.getAvgTemperature() * 10));
    msg.buf[6] = 0;
    msg.buf[7] = 0;
    Can0.write(msg);

    delay(2);
    msg.id = 0x35A;
    msg.len = 8;
    msg.buf[0] = alarm[0];    //High temp  Low Voltage | High Voltage
    msg.buf[1] = alarm[1];    // High Discharge Current | Low Temperature
    msg.buf[2] = alarm[2];    //Internal Failure | High Charge current
    msg.buf[3] = alarm[3];    // Cell Imbalance
    msg.buf[4] = warning[0];  //High temp  Low Voltage | High Voltage
    msg.buf[5] = warning[1];  // High Discharge Current | Low Temperature
    msg.buf[6] = warning[2];  //Internal Failure | High Charge current
    msg.buf[7] = warning[3];  // Cell Imbalance
    Can0.write(msg);

    msg.id = 0x35E;
    msg.len = 8;
    msg.buf[0] = bmsname[0];
    msg.buf[1] = bmsname[1];
    msg.buf[2] = bmsname[2];
    msg.buf[3] = bmsname[3];
    msg.buf[4] = bmsname[4];
    msg.buf[5] = bmsname[5];
    msg.buf[6] = bmsname[6];
    msg.buf[7] = bmsname[7];
    Can0.write(msg);

    delay(2);
    msg.id = 0x370;
    msg.len = 8;
    msg.buf[0] = bmsmanu[0];
    msg.buf[1] = bmsmanu[1];
    msg.buf[2] = bmsmanu[2];
    msg.buf[3] = bmsmanu[3];
    msg.buf[4] = bmsmanu[4];
    msg.buf[5] = bmsmanu[5];
    msg.buf[6] = bmsmanu[6];
    msg.buf[7] = bmsmanu[7];
    Can0.write(msg);

    if (balancecells == 1) {
      if (bms.getLowCellVolt() + settings.balanceHyst < bms.getHighCellVolt()) {
        msg.id = 0x3c3;
        msg.len = 8;
        if (bms.getLowCellVolt() < settings.balanceVoltage) {
          msg.buf[0] = highByte(uint16_t(settings.balanceVoltage * 1000));
          msg.buf[1] = lowByte(uint16_t(settings.balanceVoltage * 1000));
        } else {
          msg.buf[0] = highByte(uint16_t(bms.getLowCellVolt() * 1000));
          msg.buf[1] = lowByte(uint16_t(bms.getLowCellVolt() * 1000));
        }
        msg.buf[2] = 0x01;
        msg.buf[3] = 0x04;
        msg.buf[4] = 0x03;
        msg.buf[5] = 0x00;
        msg.buf[6] = 0x00;
        msg.buf[7] = 0x00;
        Can0.write(msg);
      }
    }

    delay(2);
    msg.id = 0x373;
    msg.len = 8;
    msg.buf[0] = lowByte(uint16_t(bms.getLowCellVolt() * 1000));
    msg.buf[1] = highByte(uint16_t(bms.getLowCellVolt() * 1000));
    msg.buf[2] = lowByte(uint16_t(bms.getHighCellVolt() * 1000));
    msg.buf[3] = highByte(uint16_t(bms.getHighCellVolt() * 1000));
    msg.buf[4] = lowByte(uint16_t(bms.getLowTemperature() + 273.15));
    msg.buf[5] = highByte(uint16_t(bms.getLowTemperature() + 273.15));
    msg.buf[6] = lowByte(uint16_t(bms.getHighTemperature() + 273.15));
    msg.buf[7] = highByte(uint16_t(bms.getHighTemperature() + 273.15));
    Can0.write(msg);

    delay(2);
    msg.id = 0x379;  //Installed capacity
    msg.len = 2;
    msg.buf[0] = lowByte(uint16_t(settings.Pstrings * settings.CAP));
    msg.buf[1] = highByte(uint16_t(settings.Pstrings * settings.CAP));
    /*
        delay(2);
      msg.id  = 0x378; //Installed capacity
      msg.len = 2;
      //energy in 100wh/unit
      msg.buf[0] =
      msg.buf[1] =
      msg.buf[2] =
      msg.buf[3] =
      //energy out 100wh/unit
      msg.buf[4] =
      msg.buf[5] =
      msg.buf[6] =
      msg.buf[7] =
    */
    delay(2);
    msg.id = 0x372;
    msg.len = 8;
    msg.buf[0] = lowByte(bms.getNumModules());
    msg.buf[1] = highByte(bms.getNumModules());
    msg.buf[2] = 0x00;
    msg.buf[3] = 0x00;
    msg.buf[4] = 0x00;
    msg.buf[5] = 0x00;
    msg.buf[6] = 0x00;
    msg.buf[7] = 0x00;
    Can0.write(msg);
  }
}


void BMVmessage()  //communication with the Victron Color Control System over VEdirect
{
  lasttime = millis();
  x = 0;
  VE.write(13);
  VE.write(10);
  VE.write(myStrings[0]);
  VE.write(9);
  VE.print(bms.getPackVoltage() * 1000, 0);
  VE.write(13);
  VE.write(10);
  VE.write(myStrings[2]);
  VE.write(9);
  VE.print(currentact);
  VE.write(13);
  VE.write(10);
  VE.write(myStrings[4]);
  VE.write(9);
  VE.print(ampsecond * 0.27777777777778, 0);  //consumed ah
  VE.write(13);
  VE.write(10);
  VE.write(myStrings[6]);
  VE.write(9);
  VE.print(SOC * 10);  //SOC
  x = 8;
  while (x < 20) {
    VE.write(13);
    VE.write(10);
    VE.write(myStrings[x]);
    x++;
    VE.write(9);
    VE.write(myStrings[x]);
    x++;
  }
  VE.write(13);
  VE.write(10);
  VE.write("Checksum");
  VE.write(9);
  VE.write(0x50);  //0x59
  delay(10);

  while (x < 44) {
    VE.write(13);
    VE.write(10);
    VE.write(myStrings[x]);
    x++;
    VE.write(9);
    VE.write(myStrings[x]);
    x++;
  }
  /*
    VE.write(13);
    VE.write(10);
    VE.write(myStrings[32]);
    VE.write(9);
    VE.print(bms.getLowVoltage()*1000,0);
    VE.write(13);
    VE.write(10);
    VE.write(myStrings[34]);
    VE.write(9);
    VE.print(bms.getHighVoltage()*1000,0);
    x=36;

    while(x < 43)
    {
     VE.write(13);
     VE.write(10);
     VE.write(myStrings[x]);
     x ++;
     VE.write(9);
     VE.write(myStrings[x]);
     x ++;
    }
  */
  VE.write(13);
  VE.write(10);
  VE.write("Checksum");
  VE.write(9);
  VE.write(231);
}

// Settings menu
// Settings menu
void menu() {
  incomingByte = SERIALCONSOLE.read();
  
  if (waitingForInput) {
    if (incomingByte == '\n' || incomingByte == '\r') {
      // Enter pressed, process input
      if (inputBuffer.length() > 0) {
        int value = inputBuffer.toInt();
        processMenuValue(currentCommand, value);
        inputBuffer = "";
        waitingForInput = false;
        currentCommand = 0;
      }
      return;
    } else if (incomingByte >= '0' && incomingByte <= '9') {
      // Add number to buffer
      inputBuffer += (char)incomingByte;
      SERIALCONSOLE.print((char)incomingByte); // Echo back
      return;
    } else if (incomingByte == 8 || incomingByte == 127) { // Backspace
      if (inputBuffer.length() > 0) {
        inputBuffer.remove(inputBuffer.length() - 1);
        SERIALCONSOLE.print("\b \b"); // Remove last character from display
      }
      return;
    }
    // Ignore other characters while waiting for number
    return;
  }

  if (menuload == 4) {
    switch (incomingByte) {

      case '1':
        menuload = 1;
        candebug = !candebug;
        incomingByte = 'd';
        break;

      case '2':
        menuload = 1;
        debugCur = !debugCur;
        incomingByte = 'd';
        break;

      case '3':
        menuload = 1;
        outputcheck = !outputcheck;
        if (outputcheck == 0) {
          contctrl = 0;
          digitalWrite(OUT1, LOW);
          digitalWrite(OUT2, LOW);
          digitalWrite(OUT3, LOW);
          digitalWrite(OUT4, LOW);
        }
        incomingByte = 'd';
        break;

      case '4':
        menuload = 1;
        inputcheck = !inputcheck;
        incomingByte = 'd';
        break;

      case '5':
        menuload = 1;
        settings.ESSmode = !settings.ESSmode;
        incomingByte = 'd';
        break;

      case '6':
        menuload = 1;
        cellspresent = bms.seriescells();
        incomingByte = 'd';
        break;

      case '7':
        menuload = 1;
        gaugedebug = !gaugedebug;
        incomingByte = 'd';
        break;

      case '8':
        menuload = 1;
        CSVdebug = !CSVdebug;
        incomingByte = 'd';
        break;

      case '9':
        menuload = 1;
        if (Serial.available() > 0) {
          debugdigits = Serial.parseInt();
        }
        if (debugdigits > 4) {
          debugdigits = 2;
        }
        incomingByte = 'd';
        break;

      case 'b':
        menuload = 1;
        if (Serial.available() > 0) {
          settings.balanceDuty = Serial.parseInt();
        }

        incomingByte = 'd';
        break;

      case 'r':
        menuload = 1;
        resetbalancedebug();
        incomingByte = 'd';
        break;

      case 'x':
        menuload = 1;
        resetIDdebug();
        incomingByte = 'd';
        break;

      case 'y':
        menuload = 1;
        if (Serial.available() > 0) {
          NextID = Serial.parseInt();
        }

        incomingByte = 'd';
        break;

      case 113:  //q for quite menu

        menuload = 0;
        incomingByte = 115;
        break;

      default:
        // if nothing else matches, do the default
        // default is optional
        break;
    }
  }

  if (menuload == 2) {
    switch (incomingByte) {


      case 99:  //c for calibrate zero offset

        calcur();
        break;

      case '1':
        menuload = 1;
        settings.invertcur = !settings.invertcur;
        incomingByte = 'c';
        break;

      case '2':
        menuload = 1;
        settings.voltsoc = !settings.voltsoc;
        incomingByte = 'c';
        break;

      case '3':
        menuload = 1;
        if (Serial.available() > 0) {
          settings.ncur = Serial.parseInt();
        }
        menuload = 1;
        incomingByte = 'c';
        break;

      case '8':
        menuload = 1;
        if (Serial.available() > 0) {
          settings.changecur = Serial.parseInt();
        }
        menuload = 1;
        incomingByte = 'c';
        break;

      case '4':
        menuload = 1;
        if (Serial.available() > 0) {
          settings.convlow = Serial.parseInt();
        }
        incomingByte = 'c';
        break;

      case '5':
        menuload = 1;
        if (Serial.available() > 0) {
          settings.convhigh = Serial.parseInt();
        }
        incomingByte = 'c';
        break;

      case '6':
        menuload = 1;
        if (Serial.available() > 0) {
          settings.CurDead = Serial.parseInt();
        }
        incomingByte = 'c';
        break;

      case 113:  //q for quite menu

        menuload = 0;
        incomingByte = 115;
        break;

      case 115:  //s for switch sensor
        settings.cursens++;
        if (settings.cursens > 3) {
          settings.cursens = 0;
        }
        /*
          if (settings.cursens == Analoguedual)
          {
            settings.cursens = Canbus;
            SERIALCONSOLE.println("  ");
            SERIALCONSOLE.print(" CANbus Current Sensor ");
            SERIALCONSOLE.println("  ");
          }
          else
          {
            settings.cursens = Analoguedual;
            SERIALCONSOLE.println("  ");
            SERIALCONSOLE.print(" Analogue Current Sensor ");
            SERIALCONSOLE.println("  ");
          }
        */
        menuload = 1;
        incomingByte = 'c';
        break;

      case '7':  //s for switch sensor
        settings.curcan++;
        if (settings.curcan > CurCanMax) {
          settings.curcan = 1;
        }
        menuload = 1;
        incomingByte = 'c';
        break;

      default:
        // if nothing else matches, do the default
        // default is optional
        break;
    }
  }

  if (menuload == 8) {
    switch (incomingByte) {
      case '1':  //e dispaly settings
        if (Serial.available() > 0) {
          settings.IgnoreTemp = Serial.parseInt();
        }
        if (settings.IgnoreTemp > 2) {
          settings.IgnoreTemp = 0;
        }
        bms.setSensors(settings.IgnoreTemp, settings.IgnoreVolt, settings.TempOff);
        menuload = 1;
        incomingByte = 'i';
        break;

      case '2':
        if (Serial.available() > 0) {
          settings.IgnoreVolt = Serial.parseInt();
          settings.IgnoreVolt = settings.IgnoreVolt * 0.001;
          bms.setSensors(settings.IgnoreTemp, settings.IgnoreVolt, settings.TempOff);
          // Serial.println(settings.IgnoreVolt);
          menuload = 1;
          incomingByte = 'i';
        }
        break;

      case '4':
        if (Serial.available() > 0) {
          settings.TempOff = Serial.parseInt();
          bms.setSensors(settings.IgnoreTemp, settings.IgnoreVolt, settings.TempOff);
          // Serial.println(settings.IgnoreVolt);
          menuload = 1;
          incomingByte = 'i';
        }
        break;

      case 113:  //q to go back to main menu

        menuload = 0;
        incomingByte = 115;
        break;
    }
  }



  if (menuload == 7) {
    switch (incomingByte) {
      case '1':
        if (Serial.available() > 0) {
          settings.WarnOff = Serial.parseInt();
          settings.WarnOff = settings.WarnOff * 0.001;
          menuload = 1;
          incomingByte = 'a';
        }
        break;

      case '2':
        if (Serial.available() > 0) {
          settings.CellGap = Serial.parseInt();
          settings.CellGap = settings.CellGap * 0.001;
          menuload = 1;
          incomingByte = 'a';
        }
        break;

      case '3':
        if (Serial.available() > 0) {
          settings.WarnToff = Serial.parseInt();
          menuload = 1;
          incomingByte = 'a';
        }
        break;

      case '4':
        if (Serial.available() > 0) {
          settings.triptime = Serial.parseInt();
          menuload = 1;
          incomingByte = 'a';
        }
        break;

      case 113:  //q to go back to main menu
        menuload = 0;
        incomingByte = 115;
        break;
    }
  }

  if (menuload == 6)  //Charging settings
  {
    switch (incomingByte) {

      case 113:  //q to go back to main menu

        menuload = 0;
        incomingByte = 115;
        break;

      case '1':
        if (Serial.available() > 0) {
          settings.ChargeVsetpoint = Serial.parseInt();
          settings.ChargeVsetpoint = settings.ChargeVsetpoint / 1000;
          menuload = 1;
          incomingByte = 'e';
        }
        break;


      case '2':
        if (Serial.available() > 0) {
          settings.ChargeHys = Serial.parseInt();
          settings.ChargeHys = settings.ChargeHys / 1000;
          menuload = 1;
          incomingByte = 'e';
        }
        break;


      case '4':
        if (Serial.available() > 0) {
          settings.chargecurrentend = Serial.parseInt() * 10;
          menuload = 1;
          incomingByte = 'e';
        }
        break;


      case '3':
        if (Serial.available() > 0) {
          settings.chargecurrentmax = Serial.parseInt() * 10;
          menuload = 1;
          incomingByte = 'e';
        }
        break;

      case 'a':
        if (Serial.available() > 0) {
          settings.chargecurrent2max = Serial.parseInt() * 10;
          menuload = 1;
          incomingByte = 'e';
        }
        break;

      case '5':  //1 Over Voltage Setpoint
        settings.chargertype = settings.chargertype + 1;
        if (settings.chargertype > 6) {
          settings.chargertype = 0;
        }
        menuload = 1;
        incomingByte = 'e';
        break;

      case '6':
        if (Serial.available() > 0) {
          settings.chargerspd = Serial.parseInt();
          menuload = 1;
          incomingByte = 'e';
        }
        break;
      case '8':
        if (settings.ChargerDirect == 1) {
          settings.ChargerDirect = 0;
          menuload = 1;
          incomingByte = 'e';
        } else {
          settings.ChargerDirect = 1;
          menuload = 1;
          incomingByte = 'e';
        }
        break;

      case '9':
        if (Serial.available() > 0) {
          settings.ChargeTSetpoint = Serial.parseInt();
          menuload = 1;
          incomingByte = 'e';
        }
        break;

      case 'b':
        if (Serial.available() > 0) {
          settings.chargereff = Serial.parseInt();
          menuload = 1;
          incomingByte = 'e';
        }
        break;

      case 'c':
        if (Serial.available() > 0) {
          settings.chargerACv = Serial.parseInt();
          menuload = 1;
          incomingByte = 'e';
        }
        break;
    }
  }

  if (menuload == 5) {
    switch (incomingByte) {
      case '1':
        if (Serial.available() > 0) {
          settings.Pretime = Serial.parseInt();
          menuload = 1;
          incomingByte = 'k';
        }
        break;

      case '2':
        if (Serial.available() > 0) {
          settings.Precurrent = Serial.parseInt();
          menuload = 1;
          incomingByte = 'k';
        }
        break;

      case '3':
        if (Serial.available() > 0) {
          settings.conthold = Serial.parseInt();
          menuload = 1;
          incomingByte = 'k';
        }
        break;

      case '4':
        if (Serial.available() > 0) {
          settings.gaugelow = Serial.parseInt();
          gaugedebug = 2;
          gaugeupdate();
          menuload = 1;
          incomingByte = 'k';
        }
        break;

      case '5':
        if (Serial.available() > 0) {
          settings.gaugehigh = Serial.parseInt();
          gaugedebug = 3;
          gaugeupdate();
          menuload = 1;
          incomingByte = 'k';
        }
        break;

      case '6':
        settings.tripcont = !settings.tripcont;
        if (settings.tripcont > 1) {
          settings.tripcont = 0;
        }
        menuload = 1;
        incomingByte = 'k';
        break;

      case 113:  //q to go back to main menu
        gaugedebug = 0;
        menuload = 0;
        incomingByte = 115;
        break;
    }
  }

  if (menuload == 3) {
    switch (incomingByte) {
      case 113:  //q to go back to main menu

        menuload = 0;
        incomingByte = 115;
        break;

      case 'f':  //f factory settings
        loadSettings();
        SERIALCONSOLE.println("  ");
        SERIALCONSOLE.println("  ");
        SERIALCONSOLE.println("  ");
        SERIALCONSOLE.println(" Coded Settings Loaded ");
        SERIALCONSOLE.println("  ");
        menuload = 1;
        incomingByte = 'b';
        break;

case 'r':  //r for reset
        SOCreset = 1;
        SERIALCONSOLE.println("  ");
        SERIALCONSOLE.print(" mAh Reset ");
        SERIALCONSOLE.println("  ");
        menuload = 1;
        incomingByte = 'b';
        break;


      case '1':  //1 Over Voltage Setpoint
        SERIALCONSOLE.println();
        SERIALCONSOLE.print("Enter Over Voltage Setpoint (mV): ");
        waitingForInput = true;
        currentCommand = '1';
        break;

      case 'g':
        if (Serial.available() > 0) {
          settings.StoreVsetpoint = Serial.parseInt();
          settings.StoreVsetpoint = settings.StoreVsetpoint / 1000;
          menuload = 1;
          incomingByte = 'b';
        }

      case 'h':
        if (Serial.available() > 0) {
          settings.DisTaper = Serial.parseInt();
          settings.DisTaper = settings.DisTaper / 1000;
          menuload = 1;
          incomingByte = 'b';
        }


      case 'j':
        if (Serial.available() > 0) {
          settings.DisTSetpoint = Serial.parseInt();
          menuload = 1;
          incomingByte = 'b';
        }
        break;

      case 'b':  // SOC setpoint 1 voltage
        SERIALCONSOLE.println();
        SERIALCONSOLE.print("Enter SOC setpoint 1 voltage (mV): ");
        waitingForInput = true;
        currentCommand = 'b';
        break;

      case 'c':  // SOC setpoint 1 percentage
        SERIALCONSOLE.println();
        SERIALCONSOLE.print("Enter SOC setpoint 1 percentage (%): ");
        waitingForInput = true;
        currentCommand = 'c';
        break;

      case 'd':  // SOC setpoint 2 voltage
        SERIALCONSOLE.println();
        SERIALCONSOLE.print("Enter SOC setpoint 2 voltage (mV): ");
        waitingForInput = true;
        currentCommand = 'd';
        break;

      case 'e':  // SOC setpoint 2 percentage
        SERIALCONSOLE.println();
        SERIALCONSOLE.print("Enter SOC setpoint 2 percentage (%): ");
        waitingForInput = true;
        currentCommand = 'e';
        break;

      case 'k':  //Discharge Voltage hysteresis
        if (Serial.available() > 0) {
          settings.DischHys = Serial.parseInt();
          settings.DischHys = settings.DischHys / 1000;
          menuload = 1;
          incomingByte = 'b';
        }
        break;


      case 'x':  //Discharge Voltage hysteresis
        settings.CSCvariant++;
        if (settings.CSCvariant > 1) {
          settings.CSCvariant = 0;
        }
        menuload = 1;
        incomingByte = 'b';
        break;

      case '9':  //Discharge Voltage Setpoint
        SERIALCONSOLE.println();
        SERIALCONSOLE.print("Enter Discharge Voltage Setpoint (mV): ");
        waitingForInput = true;
        currentCommand = '9';
        break;

      case '0':  //c Pstrings
        SERIALCONSOLE.println();
        SERIALCONSOLE.print("Enter Parallel strings count: ");
        waitingForInput = true;
        currentCommand = '0';
        break;

      case 'a':  // Cells in Series per String
        SERIALCONSOLE.println();
        SERIALCONSOLE.print("Enter Cells in Series per String: ");
        waitingForInput = true;
        currentCommand = 'a';
        break;

      case '2':  //2 Under Voltage Setpoint
        SERIALCONSOLE.println();
        SERIALCONSOLE.print("Enter Under Voltage Setpoint (mV): ");
        waitingForInput = true;
        currentCommand = '2';
        break;

      case '3':  //3 Over Temperature Setpoint
        SERIALCONSOLE.println();
        SERIALCONSOLE.print("Enter Over Temperature Setpoint (C): ");
        waitingForInput = true;
        currentCommand = '3';
        break;

      case '4':  //4 Under Temperature Setpoint
        SERIALCONSOLE.println();
        SERIALCONSOLE.print("Enter Under Temperature Setpoint (C): ");
        waitingForInput = true;
        currentCommand = '4';
        break;

      case '5':  //5 Balance Voltage Setpoint
        SERIALCONSOLE.println();
        SERIALCONSOLE.print("Enter Balance Voltage Setpoint (mV): ");
        waitingForInput = true;
        currentCommand = '5';
        break;

      case '6':  //6 Balance Voltage Hysteresis
        SERIALCONSOLE.println();
        SERIALCONSOLE.print("Enter Balance Voltage Hysteresis (mV): ");
        waitingForInput = true;
        currentCommand = '6';
        break;

      case '7':  //7 Battery Capacity inAh
        SERIALCONSOLE.println();
        SERIALCONSOLE.print("Enter Battery Capacity (Ah): ");
        waitingForInput = true;
        currentCommand = '7';
        break;

      case '8':  // discurrent in A
        SERIALCONSOLE.println();
        SERIALCONSOLE.print("Enter Max Discharge Current (A): ");
        waitingForInput = true;
        currentCommand = '8';
        break;
    }
  }

  if (menuload == 1) {
    switch (incomingByte) {
      case 'R':  //restart
        CPU_REBOOT;
        break;

      case 'i':  //Ignore Value Settings
        while (Serial.available()) {
          Serial.read();
        }
        SERIALCONSOLE.println();
        SERIALCONSOLE.println();
        SERIALCONSOLE.println();
        SERIALCONSOLE.println();
        SERIALCONSOLE.println();
        SERIALCONSOLE.println("Ignore Value Settings");
        SERIALCONSOLE.print("1 - Temp Sensor Setting:");
        SERIALCONSOLE.println(settings.IgnoreTemp);
        SERIALCONSOLE.print("2 - Voltage Under Which To Ignore Cells:");
        SERIALCONSOLE.print(settings.IgnoreVolt * 1000, 0);
        SERIALCONSOLE.println("mV");
        SERIALCONSOLE.print("4 - Temp Offset Setting:");
        SERIALCONSOLE.println(settings.TempOff);
        SERIALCONSOLE.println("q - Go back to menu");
        menuload = 8;
        break;

      case 'e':  //Charging settings
        while (Serial.available()) {
          Serial.read();
        }
        SERIALCONSOLE.println();
        SERIALCONSOLE.println();
        SERIALCONSOLE.println();
        SERIALCONSOLE.println();
        SERIALCONSOLE.println();
        SERIALCONSOLE.println("Charging Settings");
        SERIALCONSOLE.print("1 - Cell Charge Voltage Limit Setpoint: ");
        SERIALCONSOLE.print(settings.ChargeVsetpoint * 1000, 0);
        SERIALCONSOLE.println("mV");
        SERIALCONSOLE.print("2 - Charge Hystersis: ");
        SERIALCONSOLE.print(settings.ChargeHys * 1000, 0);
        SERIALCONSOLE.println("mV");
        if (settings.chargertype > 0) {
          SERIALCONSOLE.print("3 - Pack Max Charge Current: ");
          SERIALCONSOLE.print(settings.chargecurrentmax * 0.1);
          SERIALCONSOLE.println("A");
          SERIALCONSOLE.print("4 - Pack End of Charge Current: ");
          SERIALCONSOLE.print(settings.chargecurrentend * 0.1);
          SERIALCONSOLE.println("A");
        }
        SERIALCONSOLE.print("5 - Charger Type: ");
        switch (settings.chargertype) {
          case 0:
            SERIALCONSOLE.print("Relay Control");
            break;
          case 1:
            SERIALCONSOLE.print("Brusa NLG5xx");
            break;
          case 2:
            SERIALCONSOLE.print("Volt Charger");
            break;
          case 3:
            SERIALCONSOLE.print("Eltek Charger");
            break;
          case 4:
            SERIALCONSOLE.print("Elcon Charger");
            break;
          case 5:
            SERIALCONSOLE.print("Victron/SMA");
            break;
          case 6:
            SERIALCONSOLE.print("Coda");
            break;
        }
        SERIALCONSOLE.println();
        if (settings.chargertype > 0) {
          SERIALCONSOLE.print("6- Charger Can Msg Spd: ");
          SERIALCONSOLE.print(settings.chargerspd);
          SERIALCONSOLE.println("mS");
        }
        SERIALCONSOLE.print("8 - Charger HV Connection: ");
        switch (settings.ChargerDirect) {
          case 0:
            SERIALCONSOLE.print(" Behind Contactors");
            break;
          case 1:
            SERIALCONSOLE.print("Direct To Battery HV");
            break;
        }
        SERIALCONSOLE.println();
        SERIALCONSOLE.print("9 - Charge Current derate Low: ");
        SERIALCONSOLE.print(settings.ChargeTSetpoint);
        SERIALCONSOLE.println(" C");
        if (settings.chargertype > 0) {
          SERIALCONSOLE.print("a - Alternate Pack Max Charge Current: ");
          SERIALCONSOLE.print(settings.chargecurrent2max * 0.1);
          SERIALCONSOLE.println("A");
          SERIALCONSOLE.print("b - Charger AC to DC effiecency: ");
          SERIALCONSOLE.print(settings.chargereff);
          SERIALCONSOLE.println("%");
          SERIALCONSOLE.print("c - Charger AC Voltage: ");
          SERIALCONSOLE.print(settings.chargerACv);
          SERIALCONSOLE.println("VAC");
        }
        SERIALCONSOLE.println("q - Go back to menu");
        menuload = 6;
        break;


      case 'a':  //Alarm and Warning settings
        while (Serial.available()) {
          Serial.read();
        }
        SERIALCONSOLE.println();
        SERIALCONSOLE.println();
        SERIALCONSOLE.println();
        SERIALCONSOLE.println();
        SERIALCONSOLE.println();
        SERIALCONSOLE.println("Alarm and Warning Settings Menu");
        SERIALCONSOLE.print("1 - Voltage Warning Offset: ");
        SERIALCONSOLE.print(settings.WarnOff * 1000, 0);
        SERIALCONSOLE.println("mV");
        SERIALCONSOLE.print("2 - Cell Voltage Difference Alarm: ");
        SERIALCONSOLE.print(settings.CellGap * 1000, 0);
        SERIALCONSOLE.println("mV");
        SERIALCONSOLE.print("3 - Temp Warning Offset: ");
        SERIALCONSOLE.print(settings.WarnToff);
        SERIALCONSOLE.println(" C");
        //SERIALCONSOLE.print("4 - Temp Warning delay: ");
        //SERIALCONSOLE.print(settings.UnderDur);
        //SERIALCONSOLE.println(" mS");
        SERIALCONSOLE.print("4 - Over and Under Voltage Delay: ");
        SERIALCONSOLE.print(settings.triptime);
        SERIALCONSOLE.println(" mS");
        menuload = 7;
        break;

      case 'k':  //contactor settings
        while (Serial.available()) {
          Serial.read();
        }
        SERIALCONSOLE.println();
        SERIALCONSOLE.println();
        SERIALCONSOLE.println();
        SERIALCONSOLE.println();
        SERIALCONSOLE.println();
        SERIALCONSOLE.println("Contactor and Gauge Settings Menu");
        SERIALCONSOLE.print("1 - PreCharge Timer: ");
        SERIALCONSOLE.print(settings.Pretime);
        SERIALCONSOLE.println("mS");
        SERIALCONSOLE.print("2 - PreCharge Finish Current: ");
        SERIALCONSOLE.print(settings.Precurrent);
        SERIALCONSOLE.println(" mA");
        SERIALCONSOLE.print("3 - PWM contactor Hold 0-255 :");
        SERIALCONSOLE.println(settings.conthold);
        SERIALCONSOLE.print("4 - PWM for Gauge Low 0-255 :");
        SERIALCONSOLE.println(settings.gaugelow);
        SERIALCONSOLE.print("5 - PWM for Gauge High 0-255 :");
        SERIALCONSOLE.println(settings.gaugehigh);
        if (settings.ESSmode == 1) {
          SERIALCONSOLE.print("6 - ESS Main Contactor or Trip :");
          if (settings.tripcont == 0) {
            SERIALCONSOLE.println("Trip Shunt");
          } else {
            SERIALCONSOLE.println("Main Contactor and Precharge");
          }
        }
        menuload = 5;
        break;

      case 113:                   //q to go back to main menu
        EEPROM.put(0, settings);  //save all change to eeprom
        menuload = 0;
        debug = 1;
        break;
      case 'd':  //d for debug settings
        while (Serial.available()) {
          Serial.read();
        }
        SERIALCONSOLE.println();
        SERIALCONSOLE.println();
        SERIALCONSOLE.println();
        SERIALCONSOLE.println();
        SERIALCONSOLE.println();
        SERIALCONSOLE.println("Debug Settings Menu");
        SERIALCONSOLE.println("Toggle on/off");
        SERIALCONSOLE.print("1 - Can Debug :");
        SERIALCONSOLE.println(candebug);
        SERIALCONSOLE.print("2 - Current Debug :");
        SERIALCONSOLE.println(debugCur);
        SERIALCONSOLE.print("3 - Output Check :");
        SERIALCONSOLE.println(outputcheck);
        SERIALCONSOLE.print("4 - Input Check :");
        SERIALCONSOLE.println(inputcheck);
        SERIALCONSOLE.print("5 - ESS mode :");
        SERIALCONSOLE.println(settings.ESSmode);
        SERIALCONSOLE.print("6 - Cells Present Reset :");
        SERIALCONSOLE.println(cellspresent);
        SERIALCONSOLE.print("7 - Gauge Debug :");
        SERIALCONSOLE.println(gaugedebug);
        SERIALCONSOLE.print("8 - CSV Output :");
        SERIALCONSOLE.println(CSVdebug);
        SERIALCONSOLE.print("9 - Decimal Places to Show :");
        SERIALCONSOLE.println(debugdigits);
        SERIALCONSOLE.print("b - balance duration :");
        SERIALCONSOLE.print(settings.balanceDuty);
        SERIALCONSOLE.println(" S time before starting is 60s");

        ///Testing ID assignment///
        SERIALCONSOLE.print("y - NextID :");
        SERIALCONSOLE.print(NextID);
        SERIALCONSOLE.println();
        SERIALCONSOLE.print("x - wipe CSC ids");
        SERIALCONSOLE.println("");
        ///////////

        SERIALCONSOLE.println("r - reset balance debug");
        SERIALCONSOLE.println("q - Go back to menu");
        menuload = 4;
        break;

      case 99:  //c for calibrate zero offset
        while (Serial.available()) {
          Serial.read();
        }
        SERIALCONSOLE.println();
        SERIALCONSOLE.println();
        SERIALCONSOLE.println();
        SERIALCONSOLE.println();
        SERIALCONSOLE.println();
        SERIALCONSOLE.println("Current Sensor Calibration Menu");
        SERIALCONSOLE.println("c - To calibrate sensor offset");
        SERIALCONSOLE.print("s - Current Sensor Type : ");
        switch (settings.cursens) {
          case Analoguedual:
            SERIALCONSOLE.println(" Analogue Dual Current Sensor ");
            break;
          case Analoguesing:
            SERIALCONSOLE.println(" Analogue Single Current Sensor ");
            break;
          case Canbus:
            SERIALCONSOLE.println(" Canbus Current Sensor ");
            break;
          default:
            SERIALCONSOLE.println("Undefined");
            break;
        }
        SERIALCONSOLE.print("1 - invert current :");
        SERIALCONSOLE.println(settings.invertcur);
        SERIALCONSOLE.print("2 - Pure Voltage based SOC :");
        SERIALCONSOLE.println(settings.voltsoc);
        SERIALCONSOLE.print("3 - Current Multiplication :");
        SERIALCONSOLE.println(settings.ncur);
        if (settings.cursens == Analoguesing || settings.cursens == Analoguedual) {
          SERIALCONSOLE.print("4 - Analogue Low Range Conv:");
          SERIALCONSOLE.print(settings.convlow * 0.01, 2);
          SERIALCONSOLE.println(" mV/A");
        }
        if (settings.cursens == Analoguedual) {
          SERIALCONSOLE.print("5 - Analogue High Range Conv:");
          SERIALCONSOLE.print(settings.convhigh * 0.01, 2);
          SERIALCONSOLE.println(" mV/A");
        }
        if (settings.cursens == Analoguesing || settings.cursens == Analoguedual) {
          SERIALCONSOLE.print("6 - Current Sensor Deadband:");
          SERIALCONSOLE.print(settings.CurDead);
          SERIALCONSOLE.println(" mV");
        }
        if (settings.cursens == Analoguedual) {

          SERIALCONSOLE.print("8 - Current Channel ChangeOver:");
          SERIALCONSOLE.print(settings.changecur * 0.001);
          SERIALCONSOLE.println(" A");
        }

        if (settings.cursens == Canbus) {
          SERIALCONSOLE.print("7 -Can Current Sensor :");
          if (settings.curcan == LemCAB300) {
            SERIALCONSOLE.println(" LEM CAB300/500 series ");
          } else if (settings.curcan == LemCAB500) {
            SERIALCONSOLE.println(" LEM CAB500 Special ");
          } else if (settings.curcan == IsaScale) {
            SERIALCONSOLE.println(" IsaScale IVT-S ");
          }
        }
        SERIALCONSOLE.println("q - Go back to menu");
        menuload = 2;
        break;

      case 98:  //c for calibrate zero offset
        while (Serial.available()) {
          Serial.read();
        }
        SERIALCONSOLE.println();
        SERIALCONSOLE.println();
        SERIALCONSOLE.println();
        SERIALCONSOLE.println();
        SERIALCONSOLE.println();
        SERIALCONSOLE.println("Battery Settings Menu");
        SERIALCONSOLE.println("r - Reset AH counter");
        SERIALCONSOLE.println("f - Reset to Coded Settings");
        SERIALCONSOLE.println("q - Go back to menu");
        SERIALCONSOLE.println();
        SERIALCONSOLE.println();
        SERIALCONSOLE.print("1 - Cell Over Voltage Setpoint: ");
        SERIALCONSOLE.print(settings.OverVSetpoint * 1000, 0);
        SERIALCONSOLE.print("mV");
        SERIALCONSOLE.println("  ");
        SERIALCONSOLE.print("2 - Cell Under Voltage Setpoint: ");
        SERIALCONSOLE.print(settings.UnderVSetpoint * 1000, 0);
        SERIALCONSOLE.print("mV");
        SERIALCONSOLE.println("  ");
        SERIALCONSOLE.print("3 - Over Temperature Setpoint: ");
        SERIALCONSOLE.print(settings.OverTSetpoint);
        SERIALCONSOLE.print("C");
        SERIALCONSOLE.println("  ");
        SERIALCONSOLE.print("4 - Under Temperature Setpoint: ");
        SERIALCONSOLE.print(settings.UnderTSetpoint);
        SERIALCONSOLE.print("C");
        SERIALCONSOLE.println("  ");
        SERIALCONSOLE.print("5 - Cell Balance Voltage Setpoint: ");
        SERIALCONSOLE.print(settings.balanceVoltage * 1000, 0);
        SERIALCONSOLE.print("mV");
        SERIALCONSOLE.println("  ");
        SERIALCONSOLE.print("6 - Balance Voltage Hystersis: ");
        SERIALCONSOLE.print(settings.balanceHyst * 1000, 0);
        SERIALCONSOLE.print("mV");
        SERIALCONSOLE.println("  ");
        SERIALCONSOLE.print("7 - Ah Battery Capacity: ");
        SERIALCONSOLE.print(settings.CAP);
        SERIALCONSOLE.print("Ah");
        SERIALCONSOLE.println("  ");
        SERIALCONSOLE.print("8 - Pack Max Discharge: ");
        SERIALCONSOLE.print(settings.discurrentmax * 0.1);
        SERIALCONSOLE.print("A");
        SERIALCONSOLE.println("  ");
        SERIALCONSOLE.print("9 - Cell Discharge Voltage Limit Setpoint: ");
        SERIALCONSOLE.print(settings.DischVsetpoint * 1000, 0);
        SERIALCONSOLE.print("mV");
        SERIALCONSOLE.println("  ");
        SERIALCONSOLE.print("0 - Slave strings in parallel: ");
        SERIALCONSOLE.print(settings.Pstrings);
        SERIALCONSOLE.println("  ");
        SERIALCONSOLE.print("a - Cells in Series per String: ");
        SERIALCONSOLE.print(settings.Scells);
        SERIALCONSOLE.println("  ");
        SERIALCONSOLE.print("b - setpoint 1: ");
        SERIALCONSOLE.print(settings.socvolt[0]);
        SERIALCONSOLE.print("mV");
        SERIALCONSOLE.println("  ");
        SERIALCONSOLE.print("c - SOC setpoint 1:");
        SERIALCONSOLE.print(settings.socvolt[1]);
        SERIALCONSOLE.print("%");
        SERIALCONSOLE.println("  ");
        SERIALCONSOLE.print("d - setpoint 2: ");
        SERIALCONSOLE.print(settings.socvolt[2]);
        SERIALCONSOLE.print("mV");
        SERIALCONSOLE.println("  ");
        SERIALCONSOLE.print("e - SOC setpoint 2: ");
        SERIALCONSOLE.print(settings.socvolt[3]);
        SERIALCONSOLE.print("%");
        SERIALCONSOLE.println("  ");
        SERIALCONSOLE.print("g - Storage Setpoint: ");
        SERIALCONSOLE.print(settings.StoreVsetpoint * 1000, 0);
        SERIALCONSOLE.print("mV");
        SERIALCONSOLE.println("  ");
        SERIALCONSOLE.print("h - Discharge Current Taper Offset: ");
        SERIALCONSOLE.print(settings.DisTaper * 1000, 0);
        SERIALCONSOLE.print("mV");
        SERIALCONSOLE.println("  ");
        SERIALCONSOLE.print("j - Discharge Current Temperature Derate : ");
        SERIALCONSOLE.print(settings.DisTSetpoint);
        SERIALCONSOLE.print("C");
        SERIALCONSOLE.println("");
        SERIALCONSOLE.print("k - Cell Discharge Voltage Hysteresis: ");
        SERIALCONSOLE.print(settings.DischHys * 1000, 0);
        SERIALCONSOLE.print("mV");
        SERIALCONSOLE.println("  ");

        SERIALCONSOLE.print("x - CSC Variant Used: ");
        if (settings.CSCvariant == BmwI3) {
          SERIALCONSOLE.print("Bmw I3");
        }
        if (settings.CSCvariant == MiniE) {
          SERIALCONSOLE.print("Mini-E");
        }

        SERIALCONSOLE.println("  ");



        SERIALCONSOLE.println();
        menuload = 3;
        break;

      default:
        // if nothing else matches, do the default
        // default is optional
        break;
    }
  }

  if (incomingByte == 115 && menuload == 0) {
    SERIALCONSOLE.println();
    SERIALCONSOLE.println("MENU");
    SERIALCONSOLE.println("Debugging Paused");
    SERIALCONSOLE.print("Firmware Version : ");
    SERIALCONSOLE.println(firmver);
    SERIALCONSOLE.println("b - Battery Settings");
    SERIALCONSOLE.println("a - Alarm and Warning Settings");
    SERIALCONSOLE.println("e - Charging Settings");
    SERIALCONSOLE.println("c - Current Sensor Calibration");
    SERIALCONSOLE.println("k - Contactor and Gauge Settings");
    SERIALCONSOLE.println("i - Ignore Value Settings");
    SERIALCONSOLE.println("d - Debug Settings");
    SERIALCONSOLE.println("R - Restart BMS");
    SERIALCONSOLE.println("q - exit menu");
    debug = 0;
    menuload = 1;
  }
}

void canread() {
  while (Can0.read(inMsg)) {

    // Read data: len = data length, buf = data byte(s)
    if (settings.cursens == Canbus) {
      if (settings.curcan == 1) {
        switch (inMsg.id) {
          case 0x3c1:
            CAB500();
            break;

          case 0x3c2:
            CAB300();
            break;

          default:
            break;
        }
      }
      if (settings.curcan == 2) {
        switch (inMsg.id) {
          case 0x3c1:
            CAB500();
            break;

          case 0x3c2:
            CAB500();
            break;

          default:
            break;
        }
      }
      if (settings.curcan == 3) {
        switch (inMsg.id) {
          case 0x521:  //
            CANmilliamps = (long)((inMsg.buf[2] << 24) | (inMsg.buf[3] << 16) | (inMsg.buf[4] << 8) | (inMsg.buf[5]));
            RawCur = CANmilliamps;
            getcurrent();
            break;

          case 0x522:  //
            voltage1 = (long)((inMsg.buf[2] << 24) | (inMsg.buf[3] << 16) | (inMsg.buf[4] << 8) | (inMsg.buf[5]));
            break;

          case 0x523:  //
            voltage2 = (long)((inMsg.buf[2] << 24) | (inMsg.buf[3] << 16) | (inMsg.buf[4] << 8) | (inMsg.buf[5]));
            break;

          default:
            break;
        }
      }
    }
  }



  //ID not assigned//
  if (inMsg.id == 0xF0) {
    Unassigned++;
    Serial.print(millis());
    if ((inMsg.id & 0x80000000) == 0x80000000)  // Determine if ID is standard (11 bits) or extended (29 bits)
      sprintf(msgString, "Extended ID: 0x%.8lX  DLC: %1d  Data:", (inMsg.id & 0x1FFFFFFF), inMsg.len);
    else
      sprintf(msgString, ",0x%.3lX,false,%1d", inMsg.id, inMsg.len);

    Serial.print(msgString);

    if ((inMsg.id & 0x40000000) == 0x40000000) {  // Determine if message is a remote request frame.
      sprintf(msgString, " REMOTE REQUEST FRAME");
      Serial.print(msgString);
    } else {
      for (byte i = 0; i < inMsg.len; i++) {
        sprintf(msgString, ", 0x%.2X", inMsg.buf[i]);
        DMC[i] = inMsg.buf[i];
        Serial.print(msgString);
      }
    }

    Serial.println();
    for (byte i = 0; i < 8; i++) {
      Serial.print(DMC[i], HEX);
      Serial.print("|");
    }
    Serial.println();
  }
  ////

  if (inMsg.id > 0x99 && inMsg.id < 0x180)  //do BMS magic if ids are ones identified to be modules
  {
    if (candebug == 1 && debug == 1) {
      bms.decodecan(inMsg, 1);  //do  BMS if ids are ones identified to be modules
    } else {
      bms.decodecan(inMsg, 0);  //do BMS if ids are ones identified to be modules
    }
  }
  if ((inMsg.id & 0xFF0) == 0x180)  // Determine if ID is standard (11 bits) or extended (29 bits)
  {
    if (candebug == 1 && debug == 1) {
      bms.decodetemp(inMsg, 1, settings.CSCvariant);
    } else {
      bms.decodetemp(inMsg, 0, settings.CSCvariant);
    }
  }
  if (debug == 1) {
    if (candebug == 1) {
      Serial.print(millis());
      if ((inMsg.id & 0x80000000) == 0x80000000)  // Determine if ID is standard (11 bits) or extended (29 bits)
        sprintf(msgString, "Extended ID: 0x%.8lX  DLC: %1d  Data:", (inMsg.id & 0x1FFFFFFF), inMsg.len);
      else
        sprintf(msgString, ",0x%.3lX,false,%1d", inMsg.id, inMsg.len);

      Serial.print(msgString);

      if ((inMsg.id & 0x40000000) == 0x40000000) {  // Determine if message is a remote request frame.
        sprintf(msgString, " REMOTE REQUEST FRAME");
        Serial.print(msgString);
      } else {
        for (byte i = 0; i < inMsg.len; i++) {
          sprintf(msgString, ", 0x%.2X", inMsg.buf[i]);
          Serial.print(msgString);
        }
      }

      Serial.println();
    }
  }
}

void CAB300() {
  for (int i = 0; i < 4; i++) {
    inbox = (inbox << 8) | inMsg.buf[i];
  }
  CANmilliamps = inbox;
  if (CANmilliamps > static_cast<long>(0x80000000)) {
    CANmilliamps -= 0x80000000;
  } else {
    CANmilliamps = (0x80000000 - CANmilliamps) * -1;
  }
  if (settings.cursens == Canbus) {
    RawCur = CANmilliamps;
    getcurrent();
  }
  if (candebug == 1) {
    Serial.println();
    Serial.print(CANmilliamps);
    Serial.print("mA ");
  }
}

void CAB500() {
  inbox = 0;
  for (int i = 1; i < 4; i++) {
    inbox = (inbox << 8) | inMsg.buf[i];
  }
  CANmilliamps = inbox;
  if (candebug == 1) {
    Serial.println();
    Serial.print(CANmilliamps, HEX);
  }
  if (CANmilliamps > 0x800000) {
    CANmilliamps -= 0x800000;
  } else {
    CANmilliamps = (0x800000 - CANmilliamps) * -1;
  }
  if (settings.cursens == Canbus) {
    RawCur = CANmilliamps;
    getcurrent();
  }
  if (candebug == 1) {
    Serial.println();
    Serial.print(CANmilliamps);
    Serial.print("mA ");
  }
}

void currentlimit() {
  if (bmsstatus == Error) {
    discurrent = 0;
    chargecurrent = 0;
  }
  /*
    settings.PulseCh = 600; //Peak Charge current in 0.1A
    settings.PulseChDur = 5000; //Ms of discharge pulse derating
    settings.PulseDi = 600; //Peak Charge current in 0.1A
    settings.PulseDiDur = 5000; //Ms of discharge pulse derating
  */
  else {

    ///Start at no derating///
    discurrent = settings.discurrentmax;

    if (chargecurrentlimit == false) {
      chargecurrent = settings.chargecurrentmax;
    } else {
      chargecurrent = settings.chargecurrent2max;
    }

    ///////All hard limits to into zeros
    if (bms.getLowTemperature() < settings.UnderTSetpoint) {
      //discurrent = 0; Request Daniel
      chargecurrent = 0;
    }
    if (bms.getHighTemperature() > settings.OverTSetpoint) {
      discurrent = 0;
      chargecurrent = 0;
    }
    if (bms.getHighCellVolt() > settings.OverVSetpoint) {
      chargecurrent = 0;
    }
    if (bms.getHighCellVolt() > settings.OverVSetpoint) {
      chargecurrent = 0;
    }
    if (bms.getLowCellVolt() < settings.UnderVSetpoint || bms.getLowCellVolt() < settings.DischVsetpoint) {
      discurrent = 0;
    }


    //Modifying discharge current///

    if (discurrent > 0) {
      //Temperature based///

      if (bms.getHighTemperature() > settings.DisTSetpoint) {
        discurrent = discurrent - map(bms.getHighTemperature(), settings.DisTSetpoint, settings.OverTSetpoint, 0, settings.discurrentmax);
      }
      //Voltagee based///
      if (bms.getLowCellVolt() < (settings.DischVsetpoint + settings.DisTaper)) {
        discurrent = discurrent - map(bms.getLowCellVolt(), settings.DischVsetpoint, (settings.DischVsetpoint + settings.DisTaper), settings.discurrentmax, 0);
      }
    }

    //Modifying Charge current///

    if (chargecurrent > 0) {
      if (chargecurrentlimit == false) {
        //Temperature based///
        if (bms.getLowTemperature() < settings.ChargeTSetpoint) {
          chargecurrent = chargecurrent - map(bms.getLowTemperature(), settings.UnderTSetpoint, settings.ChargeTSetpoint, settings.chargecurrentmax, 0);
        }
        //Voltagee based///
        if (storagemode == 1) {
          if (bms.getHighCellVolt() > (settings.StoreVsetpoint - settings.ChargeHys)) {
            chargecurrent = chargecurrent - map(bms.getHighCellVolt(), (settings.StoreVsetpoint - settings.ChargeHys), settings.StoreVsetpoint, settings.chargecurrentend, settings.chargecurrentmax);
          }
        } else {
          if (bms.getHighCellVolt() > (settings.ChargeVsetpoint - settings.ChargeHys)) {
            chargecurrent = chargecurrent - map(bms.getHighCellVolt(), (settings.ChargeVsetpoint - settings.ChargeHys), settings.ChargeVsetpoint, 0, (settings.chargecurrentmax - settings.chargecurrentend));
          }
        }
      } else {
        //Temperature based///
        if (bms.getLowTemperature() < settings.ChargeTSetpoint) {
          chargecurrent = chargecurrent - map(bms.getLowTemperature(), settings.UnderTSetpoint, settings.ChargeTSetpoint, settings.chargecurrent2max, 0);
        }
        //Voltagee based///
        if (storagemode == 1) {
          if (bms.getHighCellVolt() > (settings.StoreVsetpoint - settings.ChargeHys)) {
            chargecurrent = chargecurrent - map(bms.getHighCellVolt(), (settings.StoreVsetpoint - settings.ChargeHys), settings.StoreVsetpoint, settings.chargecurrentend, settings.chargecurrent2max);
          }
        } else {
          if (bms.getHighCellVolt() > (settings.ChargeVsetpoint - settings.ChargeHys)) {
            chargecurrent = chargecurrent - map(bms.getHighCellVolt(), (settings.ChargeVsetpoint - settings.ChargeHys), settings.ChargeVsetpoint, 0, (settings.chargecurrent2max - settings.chargecurrentend));
          }
        }
      }
    }
  }
  ///No negative currents///

  if (discurrent < 0) {
    discurrent = 0;
  }
  if (chargecurrent < 0) {
    chargecurrent = 0;
  }

  //Charge current derate for Control Pilot AC limit

  if (accurlim > 0) {
    chargerpower = accurlim * settings.chargerACv * settings.chargereff * 0.01;
    tempchargecurrent = (chargerpower * 10) / (bms.getAvgCellVolt() * settings.Scells);

    if (chargecurrent > tempchargecurrent) {
      chargecurrent = tempchargecurrent;
    }
  }
}


void inputdebug() {
  Serial.println();
  Serial.print("Input: ");
  if (digitalRead(IN1)) {
    Serial.print("1 ON  ");
  } else {
    Serial.print("1 OFF ");
  }
  if (digitalRead(IN2)) {
    Serial.print("2 ON  ");
  } else {
    Serial.print("2 OFF ");
  }
  if (digitalRead(IN3)) {
    Serial.print("3 ON  ");
  } else {
    Serial.print("3 OFF ");
  }
  if (digitalRead(IN4)) {
    Serial.print("4 ON  ");
  } else {
    Serial.print("4 OFF ");
  }
  Serial.println();
}

void outputdebug() {
  if (outputstate < 5) {
    digitalWrite(OUT1, HIGH);
    digitalWrite(OUT2, HIGH);
    digitalWrite(OUT3, HIGH);
    digitalWrite(OUT4, HIGH);
    analogWrite(OUT5, 255);
    analogWrite(OUT6, 255);
    analogWrite(OUT7, 255);
    analogWrite(OUT8, 255);
    outputstate++;
  } else {
    digitalWrite(OUT1, LOW);
    digitalWrite(OUT2, LOW);
    digitalWrite(OUT3, LOW);
    digitalWrite(OUT4, LOW);
    analogWrite(OUT5, 0);
    analogWrite(OUT6, 0);
    analogWrite(OUT7, 0);
    analogWrite(OUT8, 0);
    outputstate++;
  }
  if (outputstate > 10) {
    outputstate = 0;
  }
}

void balancing() {
  //Function to control balancing command, to be found
}

void sendcommand()  //Send Can Command to get data from slaves
{
  ///////module id cycling/////////

  if (nextmes == 12) {
    mescycle++;
    nextmes = 0;
    if (testcycle < 4) {
      testcycle++;
    }

    if (mescycle == 0xF) {
      mescycle = 0;

      if (balancetimer < millis()) {
        balancepauze = 1;
        if (debug == 1) {
          Serial.println();
          Serial.println("Reset Balance Timer");
          Serial.println();
        }
        balancetimer = millis() + ((settings.balanceDuty + 60) * 1000);
      } else {
        balancepauze = 0;
      }
    }
  }
  if (balancepauze == 1) {
    balancecells = 0;
  }


  msg.id = 0x080 | (nextmes);
  msg.len = 8;
  if (balancecells == 1) {
    msg.buf[0] = lowByte((uint16_t((bms.getLowCellVolt()) * 1000) + 5));
    msg.buf[1] = highByte((uint16_t((bms.getLowCellVolt()) * 1000) + 5));
  } else {
    msg.buf[0] = 0xC7;
    msg.buf[1] = 0x10;
  }
  msg.buf[2] = 0x00;  //balancing bits
  msg.buf[3] = 0x00;  //balancing bits

  if (testcycle < 3) {
    msg.buf[4] = 0x20;
    msg.buf[5] = 0x00;
  } else {

    if (balancecells == 1) {
      msg.buf[4] = 0x48;
    } else {
      msg.buf[4] = 0x40;
    }
    msg.buf[5] = 0x01;
  }

  msg.buf[6] = mescycle << 4;
  if (testcycle == 2) {
    msg.buf[6] = msg.buf[6] + 0x04;
  }

  msg.buf[7] = getcheck(msg, nextmes);

  delay(2);
  Can0.write(msg);
  nextmes++;

  if (bms.checkstatus() == true) {
    resetbalancedebug();
  }
}

void resetwdog() {
  noInterrupts();  //   No - reset WDT
  wdt.feed();
  interrupts();
}

void pwmcomms() {
  int p = 0;
  p = map((currentact * 0.001), pwmcurmin, pwmcurmax, 50, 255);
  analogWrite(OUT7, p);
  /*
    Serial.println();
      Serial.print(p*100/255);
      Serial.print(" OUT8 ");
  */
  if (bms.getLowCellVolt() < settings.UnderVSetpoint) {
    analogWrite(OUT8, 224);  //12V to 10V converter 1.5V
  } else {
    p = map(SOC, 0, 100, 220, 50);
    analogWrite(OUT8, p);  //2V to 10V converter 1.5-10V
  }
  /*
      Serial.println();
      Serial.print(p*100/255);
      Serial.print(" OUT7 ");
  */
}

void dashupdate() {
  Serial2.write("stat.txt=");
  Serial2.write(0x22);
  if (settings.ESSmode == 1) {
    switch (bmsstatus) {
      case (Boot):
        Serial2.print(" Active ");
        break;
      case (Error):
        Serial2.print(" Error ");
        break;
    }
  } else {
    switch (bmsstatus) {
      case (Boot):
        Serial2.print(" Boot ");
        break;

      case (Ready):
        Serial2.print(" Ready ");
        break;

      case (Precharge):
        Serial2.print(" Precharge ");
        break;

      case (Drive):
        Serial2.print(" Drive ");
        break;

      case (Charge):
        Serial2.print(" Charge ");
        break;

      case (Error):
        Serial2.print(" Error ");
        break;
    }
  }
  Serial2.write(0x22);
  Serial2.write(0xff);  // We always have to send this three lines after each command sent to the nextion display.
  Serial2.write(0xff);
  Serial2.write(0xff);
  Serial2.print("soc.val=");
  Serial2.print(SOC);
  Serial2.write(0xff);  // We always have to send this three lines after each command sent to the nextion display.
  Serial2.write(0xff);
  Serial2.write(0xff);
  Serial2.print("soc1.val=");
  Serial2.print(SOC);
  Serial2.write(0xff);  // We always have to send this three lines after each command sent to the nextion display.
  Serial2.write(0xff);
  Serial2.write(0xff);
  Serial2.print("current.val=");
  Serial2.print(currentact / 100, 0);
  Serial2.write(0xff);  // We always have to send this three lines after each command sent to the nextion display.
  Serial2.write(0xff);
  Serial2.write(0xff);
  Serial2.print("temp.val=");
  Serial2.print(bms.getAvgTemperature(), 0);
  Serial2.write(0xff);  // We always have to send this three lines after each command sent to the nextion display.
  Serial2.write(0xff);
  Serial2.write(0xff);
  Serial2.print("templow.val=");
  Serial2.print(bms.getLowTemperature(), 0);
  Serial2.write(0xff);  // We always have to send this three lines after each command sent to the nextion display.
  Serial2.write(0xff);
  Serial2.write(0xff);
  Serial2.print("temphigh.val=");
  Serial2.print(bms.getHighTemperature(), 0);
  Serial2.write(0xff);  // We always have to send this three lines after each command sent to the nextion display.
  Serial2.write(0xff);
  Serial2.write(0xff);
  Serial2.print("volt.val=");
  Serial2.print(bms.getPackVoltage() * 10, 0);
  Serial2.write(0xff);  // We always have to send this three lines after each command sent to the nextion display.
  Serial2.write(0xff);
  Serial2.write(0xff);
  Serial2.print("lowcell.val=");
  Serial2.print(bms.getLowCellVolt() * 1000, 0);
  Serial2.write(0xff);  // We always have to send this three lines after each command sent to the nextion display.
  Serial2.write(0xff);
  Serial2.write(0xff);
  Serial2.print("highcell.val=");
  Serial2.print(bms.getHighCellVolt() * 1000, 0);
  Serial2.write(0xff);  // We always have to send this three lines after each command sent to the nextion display.
  Serial2.write(0xff);
  Serial2.write(0xff);
  Serial2.print("firm.val=");
  Serial2.print(firmver);
  Serial2.write(0xff);  // We always have to send this three lines after each command sent to the nextion display.
  Serial2.write(0xff);
  Serial2.write(0xff);
  Serial2.print("celldelta.val=");
  Serial2.print((bms.getHighCellVolt() - bms.getLowCellVolt()) * 1000, 0);
  Serial2.write(0xff);  // We always have to send this three lines after each command sent to the nextion display.
  Serial2.write(0xff);
  Serial2.write(0xff);
  Serial2.write(0xff);
}


void chargercomms() {
  if (settings.chargertype == Elcon) {
    msg.id = 0x1806E5F4;  //broadcast to all Elteks
    msg.len = 8;
    msg.flags.extended = 1;
    msg.buf[0] = highByte(uint16_t(settings.ChargeVsetpoint * settings.Scells * 10));
    msg.buf[1] = lowByte(uint16_t(settings.ChargeVsetpoint * settings.Scells * 10));
    msg.buf[2] = highByte(chargecurrent / ncharger);
    msg.buf[3] = lowByte(chargecurrent / ncharger);
    msg.buf[4] = 0x00;
    msg.buf[5] = 0x00;
    msg.buf[6] = 0x00;
    msg.buf[7] = 0x00;

    Can0.write(msg);
  }

  if (settings.chargertype == Eltek) {
    msg.id = 0x2FF;  //broadcast to all Elteks
    msg.len = 7;
    msg.buf[0] = 0x01;
    msg.buf[1] = lowByte(1000);
    msg.buf[2] = highByte(1000);
    msg.buf[3] = lowByte(uint16_t(settings.ChargeVsetpoint * settings.Scells * 10));
    msg.buf[4] = highByte(uint16_t(settings.ChargeVsetpoint * settings.Scells * 10));
    msg.buf[5] = lowByte(chargecurrent / ncharger);
    msg.buf[6] = highByte(chargecurrent / ncharger);

    Can0.write(msg);
  }
  if (settings.chargertype == BrusaNLG5) {
    msg.id = chargerid1;
    msg.len = 7;
    msg.buf[0] = 0x80;
    /*
      if (chargertoggle == 0)
      {
      msg.buf[0] = 0x80;
      chargertoggle++;
      }
      else
      {
      msg.buf[0] = 0xC0;
      chargertoggle = 0;
      }
    */
    if (digitalRead(IN2) == LOW)  //Gen OFF
    {
      msg.buf[1] = highByte(maxac1 * 10);
      msg.buf[2] = lowByte(maxac1 * 10);
    } else {
      msg.buf[1] = highByte(maxac2 * 10);
      msg.buf[2] = lowByte(maxac2 * 10);
    }
    msg.buf[5] = highByte(chargecurrent / ncharger);
    msg.buf[6] = lowByte(chargecurrent / ncharger);
    msg.buf[3] = highByte(uint16_t(((settings.ChargeVsetpoint * settings.Scells) - chargerendbulk) * 10));
    msg.buf[4] = lowByte(uint16_t(((settings.ChargeVsetpoint * settings.Scells) - chargerendbulk) * 10));
    Can0.write(msg);

    delay(2);

    msg.id = chargerid2;
    msg.len = 7;
    msg.buf[0] = 0x80;
    if (digitalRead(IN2) == LOW)  //Gen OFF
    {
      msg.buf[1] = highByte(maxac1 * 10);
      msg.buf[2] = lowByte(maxac1 * 10);
    } else {
      msg.buf[1] = highByte(maxac2 * 10);
      msg.buf[2] = lowByte(maxac2 * 10);
    }
    msg.buf[3] = highByte(uint16_t(((settings.ChargeVsetpoint * settings.Scells) - chargerend) * 10));
    msg.buf[4] = lowByte(uint16_t(((settings.ChargeVsetpoint * settings.Scells) - chargerend) * 10));
    msg.buf[5] = highByte(chargecurrent / ncharger);
    msg.buf[6] = lowByte(chargecurrent / ncharger);
    Can0.write(msg);
  }
  if (settings.chargertype == ChevyVolt) {
    msg.id = 0x30E;
    msg.len = 1;
    msg.buf[0] = 0x02;  //only HV charging , 0x03 hv and 12V charging
    Can0.write(msg);

    msg.id = 0x304;
    msg.len = 4;
    msg.buf[0] = 0x40;  //fixed
    if ((chargecurrent * 2) > 255) {
      msg.buf[1] = 255;
    } else {
      msg.buf[1] = (chargecurrent * 2);
    }
    if ((settings.ChargeVsetpoint * settings.Scells) > 200) {
      msg.buf[2] = highByte(uint16_t((settings.ChargeVsetpoint * settings.Scells) * 2));
      msg.buf[3] = lowByte(uint16_t((settings.ChargeVsetpoint * settings.Scells) * 2));
    } else {
      msg.buf[2] = highByte(400);
      msg.buf[3] = lowByte(400);
    }
    Can0.write(msg);
  }
}

uint8_t getcheck(CAN_message_t &msg, int id) {
  unsigned char canmes[11];
  int meslen = msg.len + 1;  //remove one for crc and add two for id bytes
  canmes[1] = msg.id;
  canmes[0] = msg.id >> 8;

  for (int i = 0; i < (msg.len - 1); i++) {
    canmes[i + 2] = msg.buf[i];
  }
  /*
    Serial.println();
    for (int i = 0; i <  meslen; i++)
    {
    Serial.print(canmes[i], HEX);
    Serial.print("|");
    }
  */
  return (crc8.get_crc8(canmes, meslen, finalxor[id]));
}

void resetbalancedebug() {
  msg.id = 0x0B0;  //broadcast to all Elteks
  msg.len = 8;
  msg.flags.extended = 0;
  msg.buf[0] = 0xFF;
  msg.buf[1] = 0x00;
  msg.buf[2] = 0xCD;
  msg.buf[3] = 0xA2;
  msg.buf[4] = 0x00;
  msg.buf[5] = 0x00;
  msg.buf[6] = 0x00;
  msg.buf[7] = 0x00;

  Can0.write(msg);
}

void resetIDdebug() {
  //Rest all possible Ids
  for (int ID = 0; ID < 15; ID++) {
    msg.id = 0x0A0;  //broadcast to all CSC
    msg.len = 8;
    msg.flags.extended = 0;
    msg.buf[0] = 0xA1;
    msg.buf[1] = ID;
    msg.buf[2] = 0xFF;
    msg.buf[3] = 0xFF;
    msg.buf[4] = 0xFF;
    msg.buf[5] = 0xFF;
    msg.buf[6] = 0xFF;
    msg.buf[7] = 0xFF;

    Can0.write(msg);

    delay(2);
  }
  //NextID = 0;

  //check for found unassigned CSC
  Unassigned = 0;

  msg.id = 0x0A0;  //broadcast to all CSC
  msg.len = 8;

  msg.buf[0] = 0x37;
  msg.buf[1] = 0xFF;
  msg.buf[2] = 0xFF;
  msg.buf[3] = 0xFF;
  msg.buf[4] = 0xFF;
  msg.buf[5] = 0xFF;
  msg.buf[6] = 0xFF;
  msg.buf[7] = 0xFF;

  Can0.write(msg);
}

void findUnassigned() {
  Unassigned = 0;
  //check for found unassigned CSC
  msg.id = 0x0A0;  //broadcast to all CSC
  msg.len = 8;
  msg.flags.extended = 0;
  msg.buf[0] = 0x37;
  msg.buf[1] = 0xFF;
  msg.buf[2] = 0xFF;
  msg.buf[3] = 0xFF;
  msg.buf[4] = 0xFF;
  msg.buf[5] = 0xFF;
  msg.buf[6] = 0xFF;
  msg.buf[7] = 0xFF;

  Can0.write(msg);
}

void assignID() {
  msg.id = 0x0A0;  //broadcast to all CSC
  msg.len = 8;
  msg.buf[0] = 0x12;
  msg.buf[1] = 0xAB;
  msg.buf[2] = DMC[0];
  msg.buf[3] = DMC[1];
  msg.buf[4] = DMC[2];
  msg.buf[5] = DMC[3];
  msg.buf[6] = 0xFF;
  msg.buf[7] = 0xFF;

  Can0.write(msg);

  delay(30);

  msg.buf[1] = 0xBA;
  msg.buf[2] = DMC[4];
  msg.buf[3] = DMC[5];
  msg.buf[4] = DMC[6];
  msg.buf[5] = DMC[7];

  Can0.write(msg);

  delay(10);
  msg.buf[0] = 0x5B;
  msg.buf[1] = NextID;
  Can0.write(msg);

  delay(10);
  msg.buf[0] = 0x37;
  msg.buf[1] = NextID;
  Can0.write(msg);

  NextID++;

  findUnassigned();
}

void isrCP() {
  if (digitalRead(IN4) == LOW) {
    duration = micros() - pilottimer;
    pilottimer = micros();
  } else {
    accurlim = ((duration - (micros() - pilottimer + 35)) * 60) / duration;  //pilottimer + "xx" optocoupler decade ms
  }
}  // ******** end of isr CP ********

