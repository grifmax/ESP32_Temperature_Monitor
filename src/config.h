#ifndef CONFIG_H
#define CONFIG_H

// --- GPIO Pin Definitions for ESP32 DevKit v1 ---
#define TEMP_SENSOR_PIN   4       // DS18B20 Data Pin
#define OLED_SDA_PIN      21      // OLED SDA Pin (стандартный I2C для ESP32)
#define OLED_SCL_PIN      22      // OLED SCL Pin (стандартный I2C для ESP32)
#define BATTERY_MONITOR_PIN 34   // GPIO for Battery Monitoring (ADC1)
#define BUTTON_PIN        15      // GPIO для кнопки
#define BUZZER_PIN        13      // GPIO для зуммера

// --- Wi-Fi credentials ---
#define WIFI_SSID         "your-SSID"
#define WIFI_PASSWORD     "your-PASSWORD"

// --- Telegram Bot settings ---
#define TELEGRAM_BOT_TOKEN    "your-telegram-bot-token"
#define TELEGRAM_CHAT_ID      "your-chat-id"

// --- MQTT settings ---
#define MQTT_SERVER          "mqtt.server.com"
#define MQTT_PORT            1883
#define MQTT_USER            "mqtt_username"
#define MQTT_PASSWORD        "mqtt_password"
#define MQTT_TOPIC_STATUS    "home/thermo/status"
#define MQTT_TOPIC_CONTROL   "home/thermo/control"
#define MQTT_TOPIC_CONFIG    "home/thermo/config"
#define MQTT_TOPIC_ALARMS    "home/thermo/alarms"

// --- Temperature Alarm Thresholds ---
#define HIGH_TEMP_THRESHOLD   30.0
#define LOW_TEMP_THRESHOLD    10.0

// --- Battery settings ---
#define BATTERY_VOLTAGE_DIVIDER 2

// --- Display settings ---
#define DISPLAY_TIMEOUT          30      // Таймаут автовыключения дисплея (секунды)
#define BUTTON_LONG_PRESS_TIME   2000    // Время длинного нажатия (миллисекунды)
#define BUTTON_DEBOUNCE_TIME     50      // Время debounce кнопки (миллисекунды)
#define BUTTON_DOUBLE_CLICK_TIME 500     // Время для двойного нажатия (миллисекунды)

#endif
