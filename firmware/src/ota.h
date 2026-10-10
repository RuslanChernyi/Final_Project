#ifndef OTA_H
#define OTA_H

#include "config.h"

// OTA через AWS IoT Jobs: колбек лише копіює документ роботи в jobBuf, а виконує його loop()

extern volatile bool jobPending;
extern volatile bool otaInProgress;
extern char jobBuf[MQTT_BUFFER_SIZE];
extern bool appConfirmed;    // нова прошивка вже підтвердила себе (rollback скасовано)
extern bool jobsRequested;   // після підключення вже спитали jobs/$next/get

void processJob(void);

#endif // OTA_H
