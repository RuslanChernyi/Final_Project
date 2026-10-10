#ifndef SHADOW_H
#define SHADOW_H

#include <Arduino.h>

// Device Shadow: desired.led / desired.interval_sec із хмари, reported — що пристрій реально робить

extern bool shadowRequested;   // після підключення вже спитали shadow/get
extern bool shadowReceived;    // Deep Sleep: відповідь на shadow/get уже прийшла

void handleShadow(const char *topic, byte *payload, unsigned int length);
void reportShadow(void);

#endif // SHADOW_H
