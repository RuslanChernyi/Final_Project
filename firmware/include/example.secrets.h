// Шаблон для include/secrets.h — скопіювати і вписати свої сертифікати:
//   cp include/example.secrets.h include/secrets.h
// secrets.h у .gitignore і в репозиторій не потрапляє.
//
// Wi-Fi (Wokwi-GUEST), endpoint, Client ID і топіки задані в src/main.cpp.

#ifndef SECRETS_H
#define SECRETS_H

// Amazon Root CA 1 (AmazonRootCA1.pem) — перевірка, що сервер справді AWS
// https://www.amazontrust.com/repository/AmazonRootCA1.pem
const char* root_ca = R"EOF(
-----BEGIN CERTIFICATE-----
PASTE_AMAZON_ROOT_CA_1_HERE
-----END CERTIFICATE-----
)EOF";

// Сертифікат пристрою (xxxxxx-certificate.pem.crt) — «ось хто я»
const char* device_cert = R"EOF(
-----BEGIN CERTIFICATE-----
PASTE_DEVICE_CERTIFICATE_HERE
-----END CERTIFICATE-----
)EOF";

// Приватний ключ пристрою (xxxxxx-private.pem.key) — «паспорт справді мій»
const char* private_key = R"EOF(
-----BEGIN RSA PRIVATE KEY-----
PASTE_DEVICE_PRIVATE_KEY_HERE
-----END RSA PRIVATE KEY-----
)EOF";

#endif // SECRETS_H
