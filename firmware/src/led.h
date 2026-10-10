#ifndef LED_H
#define LED_H

// Світлодіод: команди з commands/led, стан із Shadow, блимання при старті

void setLed(bool on);
void handleLedCommand(const char *message, unsigned int length);
void bootBlink(void);

#endif // LED_H
