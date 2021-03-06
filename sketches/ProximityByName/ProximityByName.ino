
// Interface for Bluetooth module
#include <SoftwareSerial.h>
SoftwareSerial btSerial(5, 6); // RX | TX

#define KEY_PIN 4 // PIN to switch module in AT mode
#define OUT_PIN 9 // Which one we'll be switching
#define OFF_DELAY 15000 // Milliseconds

// Array for device addresses and expected return to match name
#define NUM_DEVICES 2 // should match number of elements below
struct BluetoothDevices {
  // MAC addresses of recognized bluetooth devices. Format: 0000,00,000000
  static const String address[NUM_DEVICES];
  // Expected response of device name, starting with "+RNAME:"
  static const String name[NUM_DEVICES];
};
// Actual device names and addresses are stored in include file
#include "BluetoothDevices.h"

// Some state variables to keep track of things
int currentState = LOW;
long lastOffTrigger;
String content = "";
char character;

void setup() 
{
  pinMode(OUT_PIN, OUTPUT);
  pinMode(KEY_PIN, OUTPUT);
  digitalWrite(OUT_PIN, LOW);
  // To put module in AT mode, need to have LOW supplied for key pin on start, and then switch to HIGH after a bit
  digitalWrite(KEY_PIN, LOW);
  delay(1000);
  digitalWrite(KEY_PIN, HIGH);
  delay(1000);
  btSerial.begin(9600);
}

void loop() 
{
  // Reset desired output to LOW. No devices found, we don't want turning it on
  int output = LOW;
  // Cycle through all devices to see if any one of them is in the range
  for (int i=0; i<NUM_DEVICES; i++) {
    // Clear buffer for module communication
    content = "";
    // Send request for device name, using the address provided.
    btSerial.write("at+rname?");
    btSerial.write((BluetoothDevices::address[i]).c_str());
    btSerial.write("\r\n");
    while(!btSerial.available()) {
      // wait for response
    }
    while(btSerial.available()) {
        // read full response
        character = btSerial.read();
        content.concat(character);
    }
    // If return matches with our expected response, that means device is in range :)
    if (content.startsWith(BluetoothDevices::name[i])) {
      output=HIGH;
    }
  }
  // Now, sometimes we'll get a false negative when we can't get the name, while device is in range
  // Happened to me one of 4-5 times on average, running without any delay functions.
  // That's why we need a delay that will switch output ON immediately, but turn OFF with a delay to compensate for false negatives
  delaySwitch(output);
  // If we have device in range, we don't want to constantly talk to it
  // Delay by 5 seconds so target device doesn't have to constantly respond
  if (output == HIGH) {
    delay(5000);
  }
}

// Switch ON immediately, while delay switching OFF
void delaySwitch(int desiredState)
{
  // Nothing to do, except resetting OFF trigger time
  if (desiredState == currentState) {
    lastOffTrigger = 0;
    return;
  }
  
  // Turn on signals are written immediately
  if (desiredState == HIGH) {
    digitalWrite(OUT_PIN, desiredState);
    currentState = desiredState;
    return;
  }
  
  // If this is the first time we've got OFF signal, store the instance
  if (lastOffTrigger == 0) {
    lastOffTrigger = millis();
  }
  
  // Turn off signals should have a delay
  if ((millis() - lastOffTrigger) > OFF_DELAY) {
    digitalWrite(OUT_PIN, desiredState);
    currentState = desiredState;
    // Reset the trigger, just in case
    lastOffTrigger = 0;
  }
}

