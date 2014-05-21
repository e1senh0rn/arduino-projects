#include <SoftwareSerial.h>
#include <Ultrasonic.h>
#include <SyrotaAutomation1.h>
#include <Time.h>
#include "include.h"
#include "Controller.h"

SyrotaAutomation net = SyrotaAutomation(2);
Ultrasonic ultrasonic(9,8); // Trig, Echo

struct Range range;
struct Selftest selftest;
struct Alert alert;
SelfTest selftestclass = SelfTest();

// Total AC pump cycles observed
unsigned long acPumpCycles = 0;

// Buffer for char conversions
char buf [40];

void setup()
{
  // Set device ID
  strcpy(net.deviceID, "SumpPump");
  Serial.begin(9600);
  pinMode(BATTERY_VOLTAGE_PIN, INPUT);
  pinMode(DC_PUMP_VOLTAGE_PIN, INPUT);
  pinMode(BUZZ_PIN, OUTPUT);
  pinMode(DC_PUMP_TRIGGER_PIN, OUTPUT);
  digitalWrite(DC_PUMP_TRIGGER_PIN, LOW);
  // Initialize with alert ON to make sure buzzer works
  alert.buzzerState = LOW;
//  digitalWrite(BUZZ_PIN, HIGH);
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
    } else if (net.assertCommand("getBattVoltage")) {
      sprintf(buf, "%dmV", readBatteryVoltage());
      net.sendResponse(buf);
    } else if (net.assertCommand("getAcPumpCycles")) {
      sprintf(buf, "%d", acPumpCycles);
      net.sendResponse(buf);
    } else if (net.assertCommand("getLastSelfTest")) {
      sendSelfTestResponse();
    } else if (net.assertCommand("forceAlert")) {
      raiseAlert(ExternallyForced, "Force command");
    } else if (net.assertCommand("resetAlert")) {
      resetAlert(alert.reason);
      net.sendResponse("OK");
    } else if (net.assertCommand("debug")) {
      sendDebugResponse();
    } else {
      net.sendResponse("Unrecognized command");
    }
  }
  
  // Read water depth if it's been too long since last time
  if (now() - range.timeTaken > DEPTH_MEASURE_TIME) {
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
  
  // Take care of beeping a buzzer if alert is present
  if (!alert.present && alert.buzzerState == HIGH) {
    digitalWrite(BUZZ_PIN, LOW);
    alert.buzzerState = LOW;
  }
  if (alert.present && (millis() - alert.buzzerChangeTime) > BUZZ_CYCLE_TIME) {
    alert.buzzerState = alert.buzzerState ^ 1;
    digitalWrite(BUZZ_PIN, alert.buzzerState);
    alert.buzzerChangeTime = millis();
  }
}

void sendDebugResponse()
{
  sprintf(buf, "rangeLH[0]=%d,%d", range.lows[0], range.highs[0]);
  net.responseSendPart(buf);
  sprintf(buf, "rangeLH[1]=%d,%d", range.lows[1], range.highs[1]);
  net.responseSendPart(buf);
  sprintf(buf, "DcHeight=%d,%d", selftest.startingHeight, selftest.endingHeight);
  net.responseSendPart(buf);
//  sprintf(buf, );
//  net.responseSendPart(buf);
//  sprintf(buf, );
//  net.responseSendPart(buf);
//  sprintf(buf, );
//  net.responseSendPart(buf);
//  sprintf(buf, );
//  net.responseSendPart(buf);
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
   sprintf(buf, "timeSince=%d&", (unsigned long)(now() - selftest.lastTestTime));
   net.responseSendPart(buf);
   sprintf(buf, "cyclesSince=%d&", acPumpCycles-selftest.acCycles);
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
  // Disabled until unit tests are covering
  return;
  // Check number of cycles. If not reached threshold, exit right away.
  if ((acPumpCycles - selftest.acCycles) < SELFTEST_AC_CYCLES) {
    return;
  }
  // Otherwise, we need to make sure water level is high enough for self test, but not too high for AC pump to be on at the same time
  // And also validate that range is sane
  if (range.highs[0] - range.lows[0] > 7 && // Last observer high/low looks legit
    range.highs[0] - range.distance >= 3 && // Make sure that we have at least 3 cm before reaching previous high
    range.highs[0] - range.distance < 10 && // But at the same time we're no more than 10 cm from reaching the top
    range.distance > -60 // And just in case, hard code known distance at which we are reasonably sure there is enough water in the sump
  ) {
    // All self test conditions are met. Begin the test
    digitalWrite(DC_PUMP_TRIGGER_PIN, HIGH);
    selftest.lastTestTime = now();
    selftest.acCycles = acPumpCycles;
    selftest.startingHeight = range.distance;
    selftest.nowActive = true;
  }
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
  // Check for conditions to stop the test
  int currentDistance = readDistance();
  // Only time limit will stop the test at this point
  if (now() - selftest.lastTestTime > SELFTEST_TIME_LIMIT) {
    digitalWrite(DC_PUMP_TRIGGER_PIN, LOW);
    selftest.nowActive = false;
    selftest.endingHeight = currentDistance;
    selftest.testLength = now() - selftest.lastTestTime;
    selftest.batteryVoltageMv = getVoltage( BATTERY_VOLTAGE_PIN );
    
    // Determine if self test was successful
    // Battery voltage should not drop too much
    if (selftest.batteryVoltageMv < ALERT_BATTERY_VOLTAGE) {
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
    resetAlert(DcPumpMalfunction);
  }
}

void raiseAlert(alertReason reason, char *text)
{
  if (!alert.present) {
    alert.present = true;
    alert.timeTriggered = now();
    strcpy(alert.condition, text);
    alert.reason = reason;
  }
}

void resetAlert(alertReason reason) {
  if (alert.present && alert.reason == reason) {
    alert.present = false;
    strcpy(alert.condition, "");
  }
}

unsigned int readDcPumpVoltage()
{
  unsigned int mv = getVoltage( DC_PUMP_VOLTAGE_PIN );
  // If voltage is more than 0, then pump is on, and we need to sound an alert
  if (mv > 1000) {
    raiseAlert(DcPumpActivated, "DC Pump ON");
  }
  // We will not deactivate alarm when pump will be off. It needs to be reset manually.
}

// Returns mV as seen on bettery terminals
unsigned int readBatteryVoltage() 
{
  unsigned int mv = getVoltage( BATTERY_VOLTAGE_PIN );
  if (mv < ALERT_BATTERY_VOLTAGE) {
    raiseAlert(DischargedBattery, "Battery voltage");
  } else {
    resetAlert(DischargedBattery);
  }
  return mv;
}

unsigned int readDistance()
{
  range.timeTaken = now();
  range.distance = -ultrasonic.Ranging(CM);
  
  // Record highs and lows of water level observed
  // If current reading is more than 7cm lower than max, assume AC pump was ON, and we need to switch to the next index
  if ((range.highs[1] - range.distance) > 7) {
    // 0 index stores previous readings
    range.highs[0] = range.highs[1];
    range.lows[0] = range.lows[1];
    // 1 index stores currently observed readings so far
    range.highs[1] = range.distance;
    range.lows[1] = range.distance;
    // Increment AC pump cycles
    acPumpCycles++;
  }
  if (range.distance > range.highs[1]) {
    range.highs[1] = range.distance;
  }
  if (range.distance < range.lows[1]) {
    range.lows[1] = range.distance;
  }
  
  if (range.distance > ALERT_WATER_LEVEL) {
    raiseAlert(WaterLevel, "Water level");
  } else {
    resetAlert(WaterLevel);
  }
  return range.distance;
}

// Figures out if pump is working or not, and returns a boolean
boolean AcPumpOn()
{
  // Cannot determine this by water level, as sensor shows +/- 4cm without any activity, so it's unreliable.
  // May be when water level will be higher than on the bottom of the pit, it'll be possible to determine better.
  return false;
}

int getVoltage(int pin) {
  // 4.7k & 1.8k resistors result in 6500 / 1700 reduction
  // Converting from 1024 scale will be another 5/1024
  // Then multiplying by 1000 to convert to mV
  // Putting it together: 6500/1700 * 5/1024 * 1000 = 17.632378472
  
  return (int)(analogRead(pin)*17.632378);  
}