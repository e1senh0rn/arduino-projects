#include <EEPROMex.h>
#include <SoftwareSerial.h>
#include <Ultrasonic.h>
#include <SyrotaAutomation1.h>
#include <Time.h>
#include "include.h"

SyrotaAutomation net = SyrotaAutomation(RS485_CONTROL_PIN);
Ultrasonic ultrasonic(ULTRASONIC_TRIG_PIN, ULTRASONIC_ECHO_PIN); // Trig, Echo

struct Range range;
struct Selftest selftest;
struct Alert alert;
struct AcPump acpump;
struct configuration_t conf = {
  CONFIG_VERSION,
  // Default values for config
  12000U, //unsigned int alertBatteryVoltage; // mV
  -40, //int alertWaterLevel; // Distance to the sensor in CM when alert should be triggered
  9600UL, //unsigned long baudRate; // Serial/RS-485 rate: 9600, 14400, 19200, 28800, 38400, 57600, or 115200
  520, //int acPumpOnThreshold; // reading of higher than this means pump is ON
  600, //int alertPressureLevel; // reading of higher than this might indicate clogged pipe and needs to trigger an alarm
  30, //int acPumpOnTimeWarning; // number of seconds AC pump can be on at a time before warning
  86400UL, //unsigned long selftestTimeBetween; // Minimum number of seconds between self tests
  20, //byte selfTestTimeLimit; // Length of DC Pump self test, if depth measurement is not met (seconds)
  5, //byte depthMeasureTime; // Frequency with which water level should be read (seconds)
  false //boolean buzzerEnabled; // whether buzzer should sound in case of alert or not
};

// Buffer for char conversions
char buf [100];

void setup()
{
  readConfig();
  // Set device ID
  strcpy(net.deviceID, NET_ADDRESS);
  Serial.begin(conf.baudRate);
  pinMode(BATTERY_VOLTAGE_PIN, INPUT);
  pinMode(DC_PUMP_VOLTAGE_PIN, INPUT);
  pinMode(BUZZ_PIN, OUTPUT);
  pinMode(ACK_PIN, INPUT_PULLUP);
  pinMode(DC_PUMP_TRIGGER_PIN, OUTPUT);
  digitalWrite(DC_PUMP_TRIGGER_PIN, LOW);
  // Initialize with alert ON to make sure buzzer works
  alert.buzzerState = LOW;
//  digitalWrite(BUZZ_PIN, HIGH);
}

void readConfig()
{
  // Check to make sure config values are real, by looking at first 3 bytes
  if (EEPROM.read(0) == CONFIG_VERSION[0] &&
    EEPROM.read(1) == CONFIG_VERSION[1] &&
    EEPROM.read(2) == CONFIG_VERSION[2]) {
    EEPROM.readBlock(0, conf);
  } else {
    // Configuration is invalid, so let's write default to memory
    saveConfig();
  }
}

void saveConfig()
{
  EEPROM.writeBlock(0, conf);
}

int freeRam() {
  extern int __heap_start, *__brkval; 
  int v; 
  return (int) &v - (__brkval == 0 ? (int) &__heap_start : (int) __brkval); 
}

void loop()
{
  // Process RS-485 commands
  if (net.messageReceived()) {
    if (net.assertCommand("alertPresent")) {
      if (alert.present) {
        net.sendResponse("YES");
      } else {
        net.sendResponse("NO");
      }
    } else if (net.assertCommand("alertReason")) {
      net.sendResponse(alert.condition);
    } else if (net.assertCommand("getDistance")) {
      net.sendResponse(itoa(readDistance(), buf, 10));
    } else if (net.assertCommand("getPressure")) {
      net.sendResponse(itoa(readPressure(), buf, 10));
    } else if (net.assertCommand("getBattVoltage")) {
      sprintf(buf, "%dmV", readBatteryVoltage());
      net.sendResponse(buf);
    } else if (net.assertCommand("getAcPumpOnTime")) {
      if (acpump.currentlyOn) {
        sprintf(buf, "%d", acpump.onSeconds + (now() - acpump.switchOnTime));
      } else {
        sprintf(buf, "%d", acpump.onSeconds);
      }
      net.sendResponse(buf);
    } else if (net.assertCommand("getLastSelfTest")) {
      sendSelfTestResponse();
    } else if (net.assertCommand("forceAlert")) {
      raiseAlert(ExternallyForced, "Force command");
      net.sendResponse("OK");
    } else if (net.assertCommand("resetAlert")) {
      resetAlert();
      net.sendResponse("OK");
    } else if (net.assertCommand("startSelfTest")) {
      dcSelfTestStart();
      net.sendResponse("OK");
    } else if (net.assertCommandStarts("set", buf)) {
      processSetCommands();
    } else if (net.assertCommand("debug")) {
      sendDebugResponse();
    } else {
      net.sendResponse("Unrecognized command");
    }
  }
  
  // Read pressure and figure out those alerts
  readPressure();
  
  // Read water depth if it's been too long since last time
  if (now() - range.timeTaken > conf.depthMeasureTime) {
    readDistance();
    // measure battery voltage with the same frequency
    readBatteryVoltage();
    // Check if we need/can run a self test. Makes sense to only check after depth has been measured
    checkDcSelfTestStart();
  }
  
  // If self test is active, need to check if it's time to stop it
  // Otherwise, need to check if DC pump is ON to make sure we sound an alert if it's on not due to self test
  if (selftest.nowActive) {
    checkDcSelfTestProgress();
  } else {
    readDcPumpVoltage();
  }
  
  // Take care of beeping a buzzer if alert is present and not acknowledged
  if (alert.buzzerState == HIGH && (!alert.present || alert.acknowledged)) {
    digitalWrite(BUZZ_PIN, LOW);
    alert.buzzerState = LOW;
  }
  if (alert.present) {
    // Pin has internal pullup. Button grounds the input, hence LOW.
    if (digitalRead(ACK_PIN) == LOW) {
      acknowledgeAlert();
    }
    if (conf.buzzerEnabled && !alert.acknowledged && (millis() - alert.buzzerChangeTime) > BUZZ_CYCLE_TIME) {
      alert.buzzerState = alert.buzzerState ^ 1;
      digitalWrite(BUZZ_PIN, alert.buzzerState);
      alert.buzzerChangeTime = millis();
    }
  }
}

// Write to the configuration when we receive new parameters
void processSetCommands()
{
  if (net.assertCommandStarts("setAlertBatteryVoltage:", buf)) {
    unsigned int tmp = strtol(buf, NULL, 10);
    if (tmp > 9000 && tmp < 15000) {
      conf.alertBatteryVoltage = tmp;
      saveConfig();
      net.sendResponse("OK");
    } else {
      net.sendResponse("ERROR");
    }
  } else if (net.assertCommandStarts("setAlertWaterLevel:", buf)) {
    int tmp = strtol(buf, NULL, 10);
    if (tmp > -80 && tmp < -3) {
      conf.alertWaterLevel = tmp;
      saveConfig();
      net.sendResponse("OK");
    } else {
      net.sendResponse("ERROR");
    }
  } else if (net.assertCommandStarts("setBaudRate:", buf)) {
    long tmp = strtol(buf, NULL, 10);
    // Supported: 9600, 14400, 19200, 28800, 38400, 57600, or 115200
    if (tmp == 9600 ||
      tmp == 14400 ||
      tmp == 19200 ||
      tmp == 28800 ||
      tmp == 38400 ||
      tmp == 57600 ||
      tmp == 115200
    ) {
      conf.baudRate = tmp;
      saveConfig();
      net.sendResponse("OK");
      Serial.end();
      Serial.begin(tmp);
    } else {
      net.sendResponse("ERROR");
    }
  } else if (net.assertCommandStarts("setAcPumpOnThreshold:", buf)) {
    int tmp = strtol(buf, NULL, 10);
    if (tmp > 0 && tmp < 1025) {
      conf.acPumpOnThreshold = tmp;
      saveConfig();
      net.sendResponse("OK");
    } else {
      net.sendResponse("ERROR");
    }
  } else if (net.assertCommandStarts("setAlertPressureLevel:", buf)) {
    int tmp = strtol(buf, NULL, 10);
    if (tmp > 0 && tmp < 1025) {
      conf.alertPressureLevel = tmp;
      saveConfig();
      net.sendResponse("OK");
    } else {
      net.sendResponse("ERROR");
    }
  } else if (net.assertCommandStarts("setAcPumpOnTimeWarning:", buf)) {
    int tmp = strtol(buf, NULL, 10);
    if (tmp > 0 && tmp < 30000) {
      conf.acPumpOnTimeWarning = tmp;
      saveConfig();
      net.sendResponse("OK");
    } else {
      net.sendResponse("ERROR");
    }
  } else if (net.assertCommandStarts("setSelftestTimeBetween:", buf)) {
    long tmp = strtol(buf, NULL, 10);
    if (tmp > 0 && tmp < 1000000L) {
      conf.selftestTimeBetween = tmp;
      saveConfig();
      net.sendResponse("OK");
    } else {
      net.sendResponse("ERROR");
    }
  } else if (net.assertCommandStarts("setSelfTestTimeLimit:", buf)) {
    int tmp = strtol(buf, NULL, 10);
    // More than 6 seconds, as we're checking that DC pump is on at 6 second mark.
    if (tmp > 6 && tmp < 200) {
      conf.selfTestTimeLimit = tmp;
      saveConfig();
      net.sendResponse("OK");
    } else {
      net.sendResponse("ERROR");
    }
  } else if (net.assertCommandStarts("setDepthMeasureTime:", buf)) {
    int tmp = strtol(buf, NULL, 10);
    if (tmp >= 0 && tmp < 120) {
      conf.depthMeasureTime = tmp;
      saveConfig();
      net.sendResponse("OK");
    } else {
      net.sendResponse("ERROR");
    }
  } else if (net.assertCommandStarts("setBuzzerEnabled:", buf)) {
    int tmp = strtol(buf, NULL, 10);
    if (tmp == 1 || tmp == 0) {
      conf.buzzerEnabled = tmp;
      saveConfig();
      net.sendResponse("OK");
    } else {
      net.sendResponse("ERROR");
    }
  } else {
    net.sendResponse("Unrecognized command");
  }
}

void sendDebugResponse()
{
  sprintf(buf, "time=%lu", now());
  net.responseSendPart(buf);
  sprintf(buf, "rangeLH[0]=%d,%d", range.lows[0], range.highs[0]);
  net.responseSendPart(buf);
  sprintf(buf, "&rangeLH[1]=%d,%d", range.lows[1], range.highs[1]);
  net.responseSendPart(buf);
  sprintf(buf, "&DcHeight=%d,%d", selftest.startingHeight, selftest.endingHeight);
  net.responseSendPart(buf);
  sprintf(buf, "&pressure=%d", analogRead(PRESSURE_SENSOR_PIN));
  net.responseSendPart(buf);
  sprintf(buf, "&AcPumpCycles=%d", acpump.onCycles);
  net.responseSendPart(buf);
  sprintf(buf, "&lastAlertTime=%lu", alert.timeTriggered);
  net.responseSendPart(buf);
//  sprintf(buf, );
//  net.responseSendPart(buf);
//  sprintf(buf, );
//  net.responseSendPart(buf);
//  sprintf(buf, );
//  net.responseSendPart(buf);
//  sprintf(buf, );
//  net.responseSendPart(buf);
  
  net.responseEnd();
}

void sendSelfTestResponse()
{
   sprintf(buf, "timeSince=%lu&", (now() - selftest.lastTestTime));
   net.responseSendPart(buf);
   sprintf(buf, "cyclesSince=%d&", acpump.onCycles-selftest.acCycles);
   net.responseSendPart(buf);
   sprintf(buf, "voltage=%d&", selftest.batteryVoltageMv);
   net.responseSendPart(buf);
   sprintf(buf, "length=%d&", (int)selftest.testLength);
   net.responseSendPart(buf);
   sprintf(buf, "pumpedHeight=%d&", selftest.startingHeight - selftest.endingHeight);
   net.responseSendPart(buf);
   sprintf(buf, "result=%d", selftest.passed);
   net.responseSendPart(buf);
   net.responseEnd();
}

void checkDcSelfTestStart() 
{
  // Check number of cycles. If not reached threshold, exit right away.
  if ((acpump.onCycles - selftest.acCycles) < SELFTEST_AC_CYCLES) {
    return;
  }
  // Check how long has passed since last self test. If not long enough, exit
  if ((now() - selftest.lastTestTime) < conf.selftestTimeBetween) {
    return;
  }
  
  // Otherwise, we need to make sure water level is high enough for self test, but not too high for AC pump to be on at the same time
  // And also validate that range is sane
  if (range.highs[0] - range.lows[0] > 7 && // Last observer high/low looks legit
    range.highs[0] - range.distance >= 3 && // Make sure that we have at least 3 cm before reaching previous high
    range.highs[0] - range.distance < 10 && // But at the same time we're no more than 10 cm from reaching the top
    range.distance > -55 // And just in case, hard code known distance at which we are reasonably sure there is enough water in the sump
  ) {
    // All self test conditions are met. Begin the test
    dcSelfTestStart();
  }
}

void dcSelfTestStart()
{
  digitalWrite(DC_PUMP_TRIGGER_PIN, HIGH);
  selftest.lastTestTime = now();
  selftest.acCycles = acpump.onCycles;
  selftest.startingHeight = range.distance;
  selftest.nowActive = true;
}

void checkDcSelfTestProgress() {
  // Extra safety to check that test is indeed running
  if (!selftest.nowActive) {
    return;
  }
  
  // Observe minimum voltage on the battery
  // Changing to only do that on test end
//  int currentVoltage = getVoltage( BATTERY_VOLTAGE_PIN );
//  if (selftest.batteryVoltageMv > currentVoltage) {
//    selftest.batteryVoltageMv = currentVoltage;
//  }
  // Only time limit will stop the test at this point
  if (now() - selftest.lastTestTime > conf.selfTestTimeLimit) {
    digitalWrite(DC_PUMP_TRIGGER_PIN, LOW);
    selftest.nowActive = false;
    // When DC pump is working, it might give trouble to distance sensor, so reading after it's off.
    // Give 50ms to settle down
    delay(50);
    selftest.endingHeight = readDistance();
    selftest.testLength = now() - selftest.lastTestTime;
    selftest.batteryVoltageMv = getVoltage( BATTERY_VOLTAGE_PIN );
    
    // Determine if self test was successful
    // Battery voltage should not drop too much
    if (selftest.batteryVoltageMv < conf.alertBatteryVoltage) {
      selftest.passed = false;
      sprintf(buf, "Weak battery: %dmV", selftest.batteryVoltageMv);
      raiseAlert(DcPumpMalfunction, buf);
      return;
    }
    // Determine if water level was reduced enough
    if (selftest.startingHeight - selftest.endingHeight < 5) {
      selftest.passed = false;
      sprintf(buf, "DC Pump failure: %dCM pumped in %d sec.", (selftest.startingHeight - selftest.endingHeight), (int)selftest.testLength);
      raiseAlert(DcPumpMalfunction, buf);
      return;
    }
    
    // If we haven't returned yet, test has passed successfully
    selftest.passed = true;
  }
}

void raiseAlert(alertReason reason, char *text)
{
  // reason with lower number means it's higher importance, so need to override whatever there is right now
  if (!alert.present || reason < alert.reason) {
    alert.present = true;
    alert.acknowledged = false;
    alert.timeTriggered = now();
    strcpy(alert.condition, text);
    alert.reason = reason;
  }
}

// Acknowledging alert will silence it, but not disable on rs-485
void acknowledgeAlert() {
  alert.acknowledged = true;
}

void resetAlert() {
  alert.present = false;
  alert.acknowledged = false;
  // We'd like to keep condition in the text so that we can examine it in the future
}

unsigned int readDcPumpVoltage()
{
  unsigned int mv = getVoltage( DC_PUMP_VOLTAGE_PIN );
  // If voltage is more than 0, then pump is on, and we need to sound an alert
  // Need to make sure to wait a few seconds after self test to see if DC pump is ON
  if (mv > 9000 && (now() - selftest.lastTestTime) > (conf.selfTestTimeLimit + 6)) {
    raiseAlert(DcPumpActivated, "DC Pump ON");
  }
  // We will not deactivate alarm when pump will be off. It needs to be reset manually.
}

// Returns mV as seen on bettery terminals
unsigned int readBatteryVoltage() 
{
  unsigned int mv = getVoltage( BATTERY_VOLTAGE_PIN );
  if (mv < conf.alertBatteryVoltage) {
    raiseAlert(DischargedBattery, "Battery voltage");
  }
  return mv;
}

unsigned int readDistance()
{
  range.timeTaken = now();
  // Since sensor is mounted at the top and looking down, 0 is considered full pit, and it goes down from there.
  range.distance = -ultrasonic.Ranging(CM);
  
  // Update observed highs and lows, they are used in DC pump self-test
  if (range.distance > range.highs[1]) {
    range.highs[1] = range.distance;
  }
  if (range.distance < range.lows[1]) {
    range.lows[1] = range.distance;
  }
      
  if (range.distance > conf.alertWaterLevel) {
    raiseAlert(WaterLevel, "Water level");
  }
  return range.distance;
}

int readPressure()
{
  acpump.lastPressure = analogRead(PRESSURE_SENSOR_PIN);
  
  // Alert business
  if (acpump.lastPressure > conf.alertPressureLevel) {
    raiseAlert(HighPressure, "High pressure");
  }
  
  boolean currentlyOn;
  if (acpump.lastPressure > conf.acPumpOnThreshold) {
    currentlyOn = true;
  } else {
    currentlyOn = false;
  }
  
  // If state change happened, need to record stats
  if (currentlyOn != acpump.currentlyOn) {
    if (currentlyOn) {
      // If we went to ON, then we only need to collect timestamp. The rest is done on OFF state
      acpump.switchOnTime = now();
    } else {
      // If we went from ON to OFF - need to record how long the pump was ON, and count cycles
      acpump.onCycles++;
      acpump.onSeconds += now() - acpump.switchOnTime;
      
      // Record highs and lows of water level observed
      readDistance();
      // 0 index stores previous readings
      range.highs[0] = range.highs[1];
      range.lows[0] = range.lows[1];
      // 1 index stores currently observed readings so far
      range.highs[1] = range.distance;
      range.lows[1] = range.distance;
      
    }
    acpump.currentlyOn = currentlyOn;
  }
  
  // Check if AC pump was on for too long
  if (currentlyOn) {
    if ((now() - acpump.switchOnTime) > conf.acPumpOnTimeWarning) {
      raiseAlert(AcPumpOverload, "AC pump ON for too long");
    }
  }
  
  return acpump.lastPressure;
}

// Figures out if pump is working or not, and returns a boolean
boolean AcPumpOn()
{
  return acpump.currentlyOn;
}

int getVoltage(int pin) {
  // 4.7k+33k & 12k resistors result in 497/120 reduction
  // Converting from 1024 scale will be another 5/1024
  // Then multiplying by 1000 to convert to mV
  // Putting it together: (497/120) * (5/1024) * 1000 = 20.222981771
  // And then it doesn't matter, as each resistor has 10% tollerance, so have to adjust for that
  return (int)(analogRead(pin)*20.64125563);  
}
