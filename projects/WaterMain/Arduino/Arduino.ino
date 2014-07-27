#include "WaterMeterCounter.h"
#include "SyrotaAutomation1.h"
#include "config.h"

WaterMeterCounter meter = WaterMeterCounter();
SyrotaAutomation net = SyrotaAutomation(2);

// Buffer for char conversions
char buf [40];

// Indicates if one of valve commands was sent and at what time (millis)
unsigned long valveCommandTime = 0L;

void setup()
{
  pinMode(VALVE_CLOSE_PIN, OUTPUT);
  pinMode(VALVE_OPEN_PIN, OUTPUT);
  digitalWrite(VALVE_CLOSE_PIN, LOW);
  digitalWrite(VALVE_OPEN_PIN, LOW);
  
  strcpy(net.deviceID, "WtrMn");
  Serial.begin(14400);
}

void loop()
{
  if (net.messageReceived()) {
    if (net.assertCommand("getCount")) {
      sprintf(buf, "%d", meter.getCounter());
      net.sendResponse(buf);
    } else if (net.assertCommand("valveClose")) {
      valveClose();
      net.sendResponse("OK");
    } else if (net.assertCommand("valveOpen")) {
      valveOpen();
      net.sendResponse("OK");
    } else {
      net.sendResponse("Unrecognized command");
    }
  }
  valveCheck();
  meter.reading(analogRead(METER_SENSOR_PIN));
}

// Sends a command on the open pin
void valveOpen()
{
  digitalWrite(VALVE_CLOSE_PIN, LOW);
  digitalWrite(VALVE_OPEN_PIN, HIGH);
  valveCommandTime = millis();
}

// Sends a command on the close pin
void valveClose()
{
  digitalWrite(VALVE_OPEN_PIN, LOW);
  digitalWrite(VALVE_CLOSE_PIN, HIGH);
  valveCommandTime = millis();
}

// Disables valve pins after timeout
void valveCheck()
{
  if (valveCommandTime == 0L) {
    return;
  }
  if (millis() - valveCommandTime > VALVE_OPERATION_TIME) {
    digitalWrite(VALVE_CLOSE_PIN, LOW);
    digitalWrite(VALVE_OPEN_PIN, LOW);
    valveCommandTime = 0L;
  }
}

