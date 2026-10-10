#ifndef BUTTON_H
#define BUTTON_H

#include <stdint.h>

// Кнопка з debounce: g_buttonState — стабільний стан після DEBOUNCE_DELAY

extern uint8_t g_buttonState;

void checkButton(uint8_t buttonPin);

#endif // BUTTON_H
