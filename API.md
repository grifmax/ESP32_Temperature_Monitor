# API Документация

Документация по REST API для ESP32 Temperature Monitor.

## Базовый URL

- При подключении к Wi-Fi: `http://<IP_адрес_устройства>`
- В режиме точки доступа: `http://192.168.4.1`

## Формат ответов

Все API endpoints возвращают данные в формате JSON. Коды ответов:
- `200` - успешный запрос
- `400` - ошибка в запросе
- `404` - ресурс не найден
- `500` - внутренняя ошибка сервера

## Endpoints

### Статические файлы

#### `GET /`
Главная страница веб-интерфейса.

#### `GET /index.html`
Главная страница веб-интерфейса.

#### `GET /settings.html`
Страница настроек.

#### `GET /style.css`
CSS стили.

#### `GET /script.js`
JavaScript для главной страницы.

#### `GET /settings.js`
JavaScript для страницы настроек.

---

### Данные устройства

#### `GET /api/data`
Получение текущих данных устройства.

**Ответ:**
```json
{
  "temperature": 25.5,
  "ip": "192.168.1.100",
  "uptime": 3600,
  "uptime_formatted": "1h 0m 0s",
  "wifi_status": "connected",
  "wifi_rssi": -65,
  "wifi_connected_seconds": 3600,
  "wifi_connected_formatted": "1h 0m 0s",
  "display_screen": 1,
  "current_time": "14:30:00",
  "current_date": "2024-01-26",
  "unix_time": 1706280600,
  "time_synced": true,
  "mqtt": {
    "configured": true,
    "status": "connected"
  },
  "telegram": {
    "configured": true,
    "status": "connected",
    "last_poll_age": 5
  },
  "operation_mode": 1,
  "operation_mode_name": "monitoring",
  "sensors": [
    {
      "index": 0,
      "address": "28FF1234567890AB",
      "name": "Кухня",
      "enabled": true,
      "correction": 0.0,
      "mode": "monitoring",
      "monitoringThreshold": 1.0,
      "sendToNetworks": true,
      "buzzerEnabled": false,
      "currentTemp": 25.5,
      "alertSettings": {
        "minTemp": 10.0,
        "maxTemp": 30.0,
        "buzzerEnabled": true
      },
      "stabilizationSettings": {
        "tolerance": 0.1,
        "alertThreshold": 0.2,
        "duration": 10
      },
      "stabilizationState": "tracking"
    }
  ]
}
```

**Поля:**
- `temperature` - текущая температура (основной датчик, для обратной совместимости)
- `ip` - IP адрес устройства
- `uptime` - время работы в секундах
- `uptime_formatted` - отформатированное время работы
- `wifi_status` - статус Wi-Fi ("connected" или "disconnected")
- `wifi_rssi` - уровень сигнала Wi-Fi в dBm
- `wifi_connected_seconds` - время подключения к Wi-Fi в секундах
- `wifi_connected_formatted` - отформатированное время подключения
- `display_screen` - текущий экран дисплея (0=OFF, 1=TEMP, 2=INFO)
- `current_time` - текущее время (HH:MM:SS)
- `current_date` - текущая дата (YYYY-MM-DD)
- `unix_time` - Unix timestamp
- `time_synced` - синхронизировано ли время
- `mqtt` - статус MQTT
- `telegram` - статус Telegram
- `operation_mode` - режим работы (0=local, 1=monitoring, 2=alert, 3=stabilization)
- `operation_mode_name` - название режима
- `sensors` - массив датчиков с их настройками и текущими температурами

---

### История температуры

#### `GET /api/temperature/history?period=<period>`
Получение истории температуры за указанный период.

**Параметры:**
- `period` (опционально) - период истории:
  - `1m` - 1 минута
  - `5m` - 5 минут
  - `15m` - 15 минут
  - `30m` - 30 минут
  - `1h` - 1 час
  - `6h` - 6 часов
  - `24h` - 24 часа (по умолчанию)
  - `7d` - 7 дней

**Пример запроса:**
```
GET /api/temperature/history?period=24h
```

**Ответ:**
```json
{
  "data": [
    {
      "timestamp": 1706280600,
      "temperature": 25.5,
      "sensor_address": "28FF1234567890AB",
      "sensor_id": "28FF1234567890AB"
    },
    {
      "timestamp": 1706280900,
      "temperature": 25.6,
      "sensor_address": "28FF1234567890AB",
      "sensor_id": "28FF1234567890AB"
    }
  ],
  "count": 288,
  "period": "24h"
}
```

---

### Wi-Fi

#### `GET /api/wifi/scan`
Сканирование доступных Wi-Fi сетей.

**Ответ (сканирование в процессе):**
```json
{
  "status": "scanning",
  "networks": []
}
```

**Ответ (сканирование завершено):**
```json
{
  "status": "complete",
  "networks": [
    {
      "ssid": "MyWiFi",
      "rssi": -65,
      "encryption": "encrypted",
      "channel": 6
    }
  ],
  "count": 1
}
```

#### `POST /api/wifi/connect`
Подключение к Wi-Fi сети.

**Тело запроса:**
```json
{
  "ssid": "MyWiFi",
  "password": "mypassword"
}
```

**Ответ:**
```json
{
  "status": "ok",
  "message": "Connecting to WiFi..."
}
```

---

### Настройки

#### `GET /api/settings`
Получение всех настроек устройства.

**Ответ:**
```json
{
  "wifi": {
    "ssid": "MyWiFi",
    "password": "***"
  },
  "telegram": {
    "bot_token": "123456:ABC-DEF",
    "chat_id": "123456789"
  },
  "mqtt": {
    "server": "mqtt.example.com",
    "port": 1883,
    "user": "username",
    "password": "***",
    "topic_status": "home/thermo/status",
    "topic_control": "home/thermo/control",
    "security": "none"
  },
  "operation_mode": 1,
  "sensors": [
    {
      "address": "28FF1234567890AB",
      "name": "Кухня",
      "enabled": true,
      "correction": 0.0,
      "mode": "monitoring",
      "monitoringThreshold": 1.0,
      "sendToNetworks": true,
      "buzzerEnabled": false,
      "alertSettings": {
        "minTemp": 10.0,
        "maxTemp": 30.0,
        "buzzerEnabled": true
      },
      "stabilizationSettings": {
        "tolerance": 0.1,
        "alertThreshold": 0.2,
        "duration": 10
      }
    }
  ]
}
```

#### `POST /api/settings`
Сохранение настроек устройства.

**Тело запроса:** JSON объект с настройками (см. формат выше).

**Ответ:**
```json
{
  "status": "ok",
  "message": "Settings saved"
}
```

#### `GET /api/settings/status`
Получение статуса сохранения настроек.

**Ответ:**
```json
{
  "status": "idle",
  "pending": false
}
```

---

### Датчики

#### `GET /api/sensors`
Получение списка всех датчиков с их настройками.

**Ответ:** Аналогичен массиву `sensors` из `/api/data`.

#### `POST /api/sensors`
Сохранение настроек всех датчиков.

**Тело запроса:**
```json
{
  "sensors": [
    {
      "address": "28FF1234567890AB",
      "name": "Кухня",
      "enabled": true,
      "correction": 0.0,
      "mode": "monitoring",
      "monitoringThreshold": 1.0,
      "sendToNetworks": true,
      "buzzerEnabled": false,
      "alertSettings": {
        "minTemp": 10.0,
        "maxTemp": 30.0,
        "buzzerEnabled": true
      },
      "stabilizationSettings": {
        "tolerance": 0.1,
        "alertThreshold": 0.2,
        "duration": 10
      }
    }
  ]
}
```

**Ответ:**
```json
{
  "status": "ok",
  "message": "Sensors settings saved"
}
```

#### `GET /api/sensor/<id>`
Получение настроек конкретного датчика по индексу.

**Параметры:**
- `id` - индекс датчика (0, 1, 2, ...)

**Ответ:**
```json
{
  "id": 0,
  "name": "Термометр 1",
  "enabled": true,
  "correction": 0.0,
  "mode": "monitoring",
  "sendToNetworks": true,
  "buzzerEnabled": false,
  "alertSettings": {
    "minTemp": 10.0,
    "maxTemp": 30.0,
    "buzzerEnabled": true
  },
  "stabilizationSettings": {
    "tolerance": 0.1,
    "alertThreshold": 0.2,
    "duration": 10
  }
}
```

#### `POST /api/sensor/<id>`
Сохранение настроек конкретного датчика.

**Параметры:**
- `id` - индекс датчика

**Тело запроса:** JSON объект с настройками датчика (см. формат выше).

**Ответ:**
```json
{
  "status": "ok"
}
```

---

### Режим работы

#### `GET /api/mode`
Получение текущего режима работы устройства.

**Ответ:**
```json
{
  "mode": 1,
  "alert": {
    "min_temp": 10.0,
    "max_temp": 30.0,
    "buzzer_enabled": true
  },
  "stabilization": {
    "tolerance": 0.1,
    "alert_threshold": 0.2,
    "duration": 600,
    "is_stabilized": false,
    "stabilized_temp": 0.0
  }
}
```

#### `POST /api/mode`
Установка режима работы устройства.

**Тело запроса:**
```json
{
  "mode": 1
}
```

Или с настройками режима:
```json
{
  "mode": 2,
  "alert": {
    "min_temp": 10.0,
    "max_temp": 30.0,
    "buzzer_enabled": true
  }
}
```

```json
{
  "mode": 3,
  "stabilization": {
    "tolerance": 0.1,
    "alert_threshold": 0.2,
    "duration": 600
  }
}
```

**Ответ:**
```json
{
  "status": "ok",
  "message": "Mode changed"
}
```

---

### Telegram

#### `POST /api/telegram/test`
Отправка тестового сообщения в Telegram.

**Ответ:**
```json
{
  "status": "ok",
  "message": "Test message sent"
}
```

Или при ошибке:
```json
{
  "status": "error",
  "message": "Failed to send test message"
}
```

---

### MQTT

#### `POST /api/mqtt/test`
Отправка тестового сообщения в MQTT.

**Ответ:**
```json
{
  "status": "ok",
  "message": "Test message sent"
}
```

Или при ошибке:
```json
{
  "status": "error",
  "message": "Failed to send test message"
}
```

#### `POST /api/mqtt/disable`
Принудительное отключение MQTT.

**Ответ:**
```json
{
  "status": "ok",
  "message": "MQTT disabled"
}
```

---

## Обработка ошибок

### CORS

Все API endpoints поддерживают CORS (Cross-Origin Resource Sharing) для работы из браузера.

**Preflight запросы (OPTIONS):**
```
OPTIONS /api/*
```

**Заголовки ответа:**
- `Access-Control-Allow-Origin: *`
- `Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS`
- `Access-Control-Allow-Headers: Content-Type, Accept, X-Requested-With`
- `Access-Control-Max-Age: 86400`

### 404 Not Found

Для несуществующих endpoints возвращается:
```
HTTP/1.1 404 Not Found
Content-Type: text/plain

Not Found
```

### Favicon

Запросы к `/favicon.ico` возвращают пустой ответ (204 No Content).

---

## Примеры использования

### JavaScript (fetch)

```javascript
// Получение данных устройства
fetch('http://192.168.1.100/api/data')
  .then(response => response.json())
  .then(data => {
    console.log('Temperature:', data.temperature);
    console.log('Sensors:', data.sensors);
  });

// Сохранение настроек датчика
fetch('http://192.168.1.100/api/sensor/0', {
  method: 'POST',
  headers: {
    'Content-Type': 'application/json'
  },
  body: JSON.stringify({
    name: 'Кухня',
    enabled: true,
    mode: 'monitoring',
    monitoringThreshold: 1.0
  })
})
  .then(response => response.json())
  .then(data => console.log('Saved:', data));
```

### cURL

```bash
# Получение данных
curl http://192.168.1.100/api/data

# Получение истории
curl "http://192.168.1.100/api/temperature/history?period=24h"

# Сохранение настроек
curl -X POST http://192.168.1.100/api/settings \
  -H "Content-Type: application/json" \
  -d '{"wifi":{"ssid":"MyWiFi","password":"mypass"}}'
```

### Python

```python
import requests

# Получение данных
response = requests.get('http://192.168.1.100/api/data')
data = response.json()
print(f"Temperature: {data['temperature']}°C")

# Сохранение настроек датчика
sensor_config = {
    "name": "Кухня",
    "enabled": True,
    "mode": "monitoring",
    "monitoringThreshold": 1.0
}
response = requests.post(
    'http://192.168.1.100/api/sensor/0',
    json=sensor_config
)
print(response.json())
```

---

## Ограничения

- Максимальный размер запроса: 16 KB
- Максимальное количество датчиков: 10
- Максимальное количество записей истории: 288 (24 часа)
- Максимальное количество сетей Wi-Fi в результатах сканирования: 15
- Интервал чтения температуры: 10 секунд
- Интервал отправки метрик MQTT: 60 секунд

---

## Версионирование

Текущая версия API: **1.0**

Изменения в API будут документироваться в CHANGELOG.md.
