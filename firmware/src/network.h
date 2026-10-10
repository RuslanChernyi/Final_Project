#ifndef NETWORK_H
#define NETWORK_H

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>

// Wi-Fi, TLS, NTP і MQTT: усе, що потрібно, щоб бути на зв'язку з AWS IoT Core

extern WiFiClientSecure wifiClient;
extern PubSubClient mqttClient;

// Amazon Root CA 1 із secrets.h — потрібен і MQTT, і HTTPS-завантаженню OTA
extern const char *root_ca;

bool connectWifi(void);
bool isWifiConnected(void);
void configureTLS(void);
bool syncNtpTime(void);
time_t currentTimestamp(void);
bool connectMqtt(void);
uint32_t reconnectDelay(uint32_t attempts);
void mqttWait(uint32_t timeoutMs, volatile bool *doneFlag);

#endif // NETWORK_H
