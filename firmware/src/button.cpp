#include "button.h"

#include <Arduino.h>
#include "config.h"

uint8_t g_buttonState = BUTTON_LOGIC_LOW;

void checkButton(uint8_t buttonPin) {

  static uint8_t currentSavedButtonState = digitalRead(buttonPin);
  static uint32_t debounceStartTime = 0;
  static uint8_t debounceStarted = 0;

  uint8_t buttonState = digitalRead(buttonPin);

  // Detect state transition
  if (buttonState != currentSavedButtonState && !debounceStarted) {
    debounceStartTime = millis();
    debounceStarted = 1;
  }

  // Check the state again after the debounce delay
  if (debounceStarted && (millis() - debounceStartTime >= DEBOUNCE_DELAY)) {
    if (buttonState != currentSavedButtonState) {
      currentSavedButtonState = buttonState;
      if (buttonState == BUTTON_LOGIC_HIGH) {
        g_buttonState = BUTTON_LOGIC_HIGH;
        Serial.println("\nButton was pressed");
      } else {
        g_buttonState = BUTTON_LOGIC_LOW;
      }
    }
    // Reset debounce flag regardless of press or release
    debounceStarted = 0;
  }
  return;
}
