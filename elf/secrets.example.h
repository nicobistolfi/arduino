// WiFi credentials template.
// Copy this file to secrets.h (same folder) and fill in your network.
// secrets.h is in .gitignore, so your real password never gets committed.
// The ESP32 only supports 2.4 GHz networks.

#define WIFI_SSID     "your-network-name"
#define WIFI_PASSWORD "your-wifi-password"

// MQTT broker. Leave MQTT_USER and MQTT_PASSWORD as "" if the broker has no login.
#define MQTT_HOST     "192.168.1.10"   // broker IP address or hostname
#define MQTT_PORT     1883
#define MQTT_USER     ""
#define MQTT_PASSWORD ""
