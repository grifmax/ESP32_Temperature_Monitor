// Конфигурация
const API_ENDPOINT = '/api/data';
const UPDATE_INTERVAL = 5000; // 5 секунд

// Элементы DOM
const elements = {
    wifiIcon: document.getElementById('wifi-icon'),
    wifiRssiHeader: document.getElementById('wifi-rssi-header'),
    wifiStatusHeader: document.getElementById('wifi-status-header'),
    ipAddressHeader: document.getElementById('ip-address-header'),
    uptimeHeaderValue: document.getElementById('uptime-header-value'),
    lastUpdate: document.getElementById('last-update'),
    sensorsGrid: document.getElementById('sensors-grid')
};

// Состояние
let updateInterval = null;
let temperatureChart = null;
let currentChartPeriod = '24h';
let sensors = [];
let sensorsData = {}; // Данные по каждому термометру {id: {currentTemp, stabilizationState}}
let chartZoom = { min: null, max: null }; // Масштаб графика
let chartPan = { offset: 0 }; // Смещение графика

// Функция форматирования времени
function formatTime(date) {
    return date.toLocaleTimeString('ru-RU', { 
        hour: '2-digit', 
        minute: '2-digit', 
        second: '2-digit' 
    });
}

// Функция получения данных с сервера
async function fetchData() {
    try {
        const response = await fetch(API_ENDPOINT);
        if (!response.ok) {
            throw new Error(`HTTP error! status: ${response.status}`);
        }
        const data = await response.json();
        updateUI(data);
        elements.lastUpdate.textContent = formatTime(new Date());
    } catch (error) {
        console.error('Error fetching data:', error);
        showError();
    }
}

// Функция обновления интерфейса
function updateUI(data) {
    // Wi-Fi статус в шапке
    updateWiFiHeader(data.wifi_status, data.wifi_rssi);
    
    // IP адрес в шапке
    if (elements.ipAddressHeader) {
        elements.ipAddressHeader.textContent = data.ip || '--';
    }

    // Uptime
    if (elements.uptimeHeaderValue) {
        elements.uptimeHeaderValue.textContent = data.uptime_formatted || '--';
    }

    if (elements.wifiStatusText) {
        elements.wifiStatusText.textContent = data.wifi_status === 'connected' ? 'Подключен' : 'Отключен';
    }

    if (elements.mqttStatus) {
        elements.mqttStatus.textContent = formatMqttStatus(data.mqtt);
    }

    // Обновляем статусы MQTT и Telegram в тулбаре
    updateServiceStatusDots(data.mqtt, data.telegram);
    
    // Обновляем данные термометров (используем адрес как ключ)
    if (data.sensors && Array.isArray(data.sensors)) {
        data.sensors.forEach(sensor => {
            const key = sensor.address || sensor.index || sensor.id;
            sensorsData[key] = {
                currentTemp: sensor.currentTemp,
                stabilizationState: sensor.stabilizationState || 'tracking'
            };
        });
        renderSensorCells();
    }
}

function formatMqttStatus(mqtt) {
    if (!mqtt) return '--';
    if (mqtt.status === 'not_configured') return 'Не настроен';
    if (mqtt.status === 'connected') return 'Подключен';
    if (mqtt.status === 'waiting_wifi') return 'Ожидание Wi-Fi';
    if (mqtt.status === 'error') return 'Ошибка';
    return 'Подключение...';
}

function formatTelegramStatus(telegram) {
    if (!telegram) return '--';
    if (telegram.status === 'not_configured') return 'Не настроен';
    if (telegram.status === 'connected') return 'Подключен';
    if (telegram.status === 'not_initialized') return 'Не инициализ.';
    return 'Подключение...';
}

// Обновление статусов в виде цветных точек в тулбаре
function updateServiceStatusDots(mqtt, telegram) {
    // MQTT статус
    const mqttDot = document.getElementById('mqtt-status-dot');
    if (mqttDot) {
        if (mqtt && mqtt.status === 'connected') {
            mqttDot.style.background = '#4CAF50'; // Зеленая
        } else if (mqtt && mqtt.status === 'not_configured') {
            mqttDot.style.background = '#9E9E9E'; // Серая (не настроен)
        } else {
            mqttDot.style.background = '#f44336'; // Красная
        }
    }
    
    // Telegram статус
    const telegramDot = document.getElementById('telegram-status-dot');
    if (telegramDot) {
        if (telegram && telegram.status === 'connected') {
            telegramDot.style.background = '#4CAF50'; // Зеленая
        } else if (telegram && telegram.status === 'not_configured') {
            telegramDot.style.background = '#9E9E9E'; // Серая (не настроен)
        } else {
            telegramDot.style.background = '#f44336'; // Красная
        }
    }
}

// Загрузка списка термометров
async function loadSensors() {
    try {
        const response = await fetch('/api/sensors');
        if (!response.ok) {
            throw new Error(`HTTP error! status: ${response.status}`);
        }
        const data = await response.json();
        sensors = data.sensors || [];

        // Если нет термометров, создаем один по умолчанию
        if (sensors.length === 0) {
            sensors = [{
                id: 1,
                name: 'Термометр 1',
                enabled: true,
                correction: 0.0,
                mode: 'monitoring',
                sendToNetworks: true,
                buzzerEnabled: false,
                alertSettings: {
                    minTemp: 10.0,
                    maxTemp: 30.0,
                    buzzerEnabled: true
                },
                stabilizationSettings: {
                    targetTemp: 25.0,
                    tolerance: 0.1,
                    alertThreshold: 0.2,
                    duration: 10,
                    buzzerEnabled: true
                }
            }];
        }

        renderSensorCells();
        updateChartSensorSelectors();
    } catch (error) {
        console.error('Error loading sensors:', error);
        // При ошибке создаём дефолтный датчик, чтобы интерфейс не был пустым
        sensors = [{
            id: 1,
            name: 'Термометр 1',
            enabled: true,
            correction: 0.0,
            mode: 'monitoring',
            sendToNetworks: true,
            buzzerEnabled: false,
            alertSettings: { minTemp: 10.0, maxTemp: 30.0, buzzerEnabled: true },
            stabilizationSettings: { targetTemp: 25.0, tolerance: 0.1, alertThreshold: 0.2, duration: 10, buzzerEnabled: true }
        }];
        renderSensorCells();
        updateChartSensorSelectors();
    }
}

// Рендеринг ячеек термометров
function renderSensorCells() {
    if (!elements.sensorsGrid) return;
    
    elements.sensorsGrid.innerHTML = '';
    
    // Фильтруем только включенные термометры
    const enabledSensors = sensors.filter(s => s.enabled);
    
    enabledSensors.forEach(sensor => {
        // Используем адрес, индекс или id как ключ
        const sensorKey = sensor.address || sensor.index || sensor.id;
        
        const cell = document.createElement('div');
        cell.className = 'sensor-cell';
        cell.onclick = () => openSensorSettings(sensorKey);
        const sensorData = sensorsData[sensorKey] || {};
        const currentTemp = sensorData.currentTemp !== undefined ? sensorData.currentTemp : null;
        // Применяем коррекцию, если она есть
        const correctedTemp = currentTemp !== null ? (currentTemp + (sensor.correction || 0)) : null;
        const tempDisplay = correctedTemp !== null ? correctedTemp.toFixed(1) : '--';
        
        // Используем имя термометра из настроек
        const sensorName = sensor.name || `Термометр ${(sensor.index !== undefined ? sensor.index + 1 : 1)}`;
        
        // Название режима
        const modeNames = {
            'monitoring': 'Мониторинг',
            'alert': 'Оповещение',
            'stabilization': 'Стабилизация'
        };
        
        // Данные режима
        let modeDataHtml = '';
        if (sensor.mode === 'alert' && sensor.alertSettings) {
            modeDataHtml = `
                <div class="sensor-data">
                    <span class="alert-min">↓ ${sensor.alertSettings.minTemp}°C</span>
                    <span class="alert-max">↑ ${sensor.alertSettings.maxTemp}°C</span>
                </div>
            `;
        } else if (sensor.mode === 'stabilization' && sensor.stabilizationSettings) {
            const stateNames = {
                'heating': 'Нагрев',
                'cooling': 'Охлаждение',
                'tracking': 'Отслеживание'
            };
            const state = sensorData.stabilizationState || 'tracking';
            modeDataHtml = `
                <div class="sensor-data">
                    <span class="stabilization-state">${stateNames[state]}</span>
                    <span class="stabilization-threshold">Порог: ${sensor.stabilizationSettings.alertThreshold}°C</span>
                </div>
            `;
        }
        
        // Кнопки управления
        const buttonsContainer = document.createElement('div');
        buttonsContainer.className = 'sensor-buttons-container';
        
        const sendButton = document.createElement('button');
        sendButton.className = `sensor-send-button ${sensor.sendToNetworks ? 'active' : ''}`;
        sendButton.onclick = (e) => { e.stopPropagation(); toggleSendToNetworks(sensorKey); };
        sendButton.innerHTML = `<span>📤</span><span>${sensor.sendToNetworks ? 'Вкл' : 'Выкл'}</span>`;
        
        const buzzerButton = document.createElement('button');
        buzzerButton.className = `sensor-buzzer-button ${sensor.buzzerEnabled ? 'active' : ''}`;
        buzzerButton.onclick = (e) => { e.stopPropagation(); toggleBuzzer(sensorKey); };
        const buzzerIcon = sensor.buzzerEnabled ? '🔊' : '🔇';
        buzzerButton.innerHTML = `<span>${buzzerIcon}</span>`;
        
        buttonsContainer.appendChild(sendButton);
        buttonsContainer.appendChild(buzzerButton);
        
        cell.innerHTML = `
            <div class="sensor-name">${sensorName}</div>
            <div class="sensor-temp-container">
                <span class="sensor-temp">${tempDisplay}</span>
                <span class="sensor-temp-unit">°C</span>
            </div>
            <div class="sensor-mode">${modeNames[sensor.mode] || 'Мониторинг'}</div>
            ${modeDataHtml}
        `;
        cell.appendChild(buttonsContainer);
        
        elements.sensorsGrid.appendChild(cell);
    });
}

// Открытие модального окна настроек
function openSensorSettings(sensorId) {
    // Ищем по адресу, индексу или id
    const sensor = sensors.find(s => (s.address === sensorId || s.index === sensorId || s.id === sensorId));
    if (!sensor) return;
    
    const modal = document.getElementById('sensor-settings-modal');
    if (!modal) return;
    
    // Заполняем форму данными термометра (используем адрес или индекс как идентификатор)
    const sensorKey = sensor.address || sensor.index || sensor.id;
    document.getElementById('modal-sensor-id').value = sensorKey;
    document.getElementById('modal-sensor-name').textContent = `Настройки: ${sensor.name}`;
    const nameInput = document.getElementById('modal-sensor-name-input');
    if (nameInput) {
        nameInput.value = sensor.name || '';
    }
    document.getElementById('modal-sensor-mode').value = sensor.mode || 'monitoring';
    document.getElementById('modal-send-to-networks').checked = sensor.sendToNetworks !== false;
    
    // Настройки оповещения
    if (sensor.alertSettings) {
        document.getElementById('modal-alert-min-temp').value = sensor.alertSettings.minTemp || 10.0;
        document.getElementById('modal-alert-max-temp').value = sensor.alertSettings.maxTemp || 30.0;
        document.getElementById('modal-alert-buzzer').checked = sensor.alertSettings.buzzerEnabled !== false;
    }
    
    // Настройки стабилизации
    if (sensor.stabilizationSettings) {
        document.getElementById('modal-stab-target-temp').value = sensor.stabilizationSettings.targetTemp || 25.0;
        document.getElementById('modal-stab-tolerance').value = sensor.stabilizationSettings.tolerance || 0.1;
        document.getElementById('modal-stab-alert-threshold').value = sensor.stabilizationSettings.alertThreshold || 0.2;
        document.getElementById('modal-stab-duration').value = sensor.stabilizationSettings.duration || 10;
    }
    
    // Настройки мониторинга
    const monitoringIntervalInput = document.getElementById('modal-monitoring-interval');
    if (monitoringIntervalInput) {
        monitoringIntervalInput.value = sensor.monitoringInterval || 5;
    }
    
    // Обновляем видимость настроек режима
    updateSensorModeSettings(sensor.mode || 'monitoring');
    
    modal.style.display = 'flex';
}

// Закрытие модального окна
function closeSensorSettings() {
    const modal = document.getElementById('sensor-settings-modal');
    if (modal) {
        modal.style.display = 'none';
    }
}

// Обновление настроек режима в модальном окне
function updateSensorModeSettings(mode) {
    const monitoringSettings = document.getElementById('modal-monitoring-settings');
    const alertSettings = document.getElementById('modal-alert-settings');
    const stabSettings = document.getElementById('modal-stabilization-settings');
    
    if (monitoringSettings) monitoringSettings.style.display = (mode === 'monitoring') ? 'block' : 'none';
    if (alertSettings) alertSettings.style.display = (mode === 'alert') ? 'block' : 'none';
    if (stabSettings) stabSettings.style.display = (mode === 'stabilization') ? 'block' : 'none';
}

// Сохранение настроек термометра
async function saveSensorSettings() {
    const sensorKey = document.getElementById('modal-sensor-id').value;
    // Ищем по адресу, индексу или id
    const sensor = sensors.find(s => (s.address === sensorKey || String(s.index) === sensorKey || String(s.id) === sensorKey));
    if (!sensor) {
        console.error('Sensor not found:', sensorKey);
        return;
    }
    
    // Обновляем данные термометра
    const nameInput = document.getElementById('modal-sensor-name-input');
    if (nameInput && nameInput.value.trim()) {
        sensor.name = nameInput.value.trim();
    }
    sensor.mode = document.getElementById('modal-sensor-mode').value;
    sensor.sendToNetworks = document.getElementById('modal-send-to-networks').checked;
    
    // Настройки мониторинга
    if (sensor.mode === 'monitoring') {
        const monitoringIntervalInput = document.getElementById('modal-monitoring-interval');
        if (monitoringIntervalInput) {
            sensor.monitoringInterval = parseInt(monitoringIntervalInput.value) || 5;
        } else {
            sensor.monitoringInterval = 5;
        }
    }
    
    if (sensor.mode === 'alert') {
        if (!sensor.alertSettings) sensor.alertSettings = {};
        sensor.alertSettings.minTemp = parseFloat(document.getElementById('modal-alert-min-temp').value) || 10.0;
        sensor.alertSettings.maxTemp = parseFloat(document.getElementById('modal-alert-max-temp').value) || 30.0;
        sensor.alertSettings.buzzerEnabled = document.getElementById('modal-alert-buzzer').checked;
    }
    
    if (sensor.mode === 'stabilization') {
        if (!sensor.stabilizationSettings) sensor.stabilizationSettings = {};
        sensor.stabilizationSettings.targetTemp = parseFloat(document.getElementById('modal-stab-target-temp').value) || 25.0;
        sensor.stabilizationSettings.tolerance = parseFloat(document.getElementById('modal-stab-tolerance').value) || 0.1;
        sensor.stabilizationSettings.alertThreshold = parseFloat(document.getElementById('modal-stab-alert-threshold').value) || 0.2;
        sensor.stabilizationSettings.duration = parseInt(document.getElementById('modal-stab-duration').value) || 10;
    }
    
    // Сохраняем на сервер через общий API датчиков
    try {
        // Подготавливаем данные для сохранения - убеждаемся, что все поля присутствуют
        const sensorsToSave = sensors.map(s => ({
            address: s.address || '',
            name: s.name || '',
            enabled: s.enabled !== undefined ? s.enabled : true,
            correction: s.correction || 0.0,
            mode: s.mode || 'monitoring',
            monitoringInterval: s.monitoringInterval || 5,
            sendToNetworks: s.sendToNetworks !== undefined ? s.sendToNetworks : true,
            buzzerEnabled: s.buzzerEnabled || false,
            alertSettings: s.alertSettings || {
                minTemp: 10.0,
                maxTemp: 30.0,
                buzzerEnabled: true
            },
            stabilizationSettings: s.stabilizationSettings || {
                targetTemp: 25.0,
                tolerance: 0.1,
                alertThreshold: 0.2,
                duration: 10
            }
        }));
        
        // Сохраняем все датчики
        const response = await fetch('/api/sensors', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({ sensors: sensorsToSave })
        });
        
        if (response.ok) {
            // Перезагружаем данные датчиков с сервера
            await loadSensors();
            // Обновляем отображение на главном экране
            renderSensorCells();
            // Закрываем модальное окно
            closeSensorSettings();
            // Показываем сообщение об успехе
            console.log('Настройки термометра сохранены');
        } else {
            alert('Ошибка сохранения настроек');
        }
    } catch (error) {
        console.error('Error saving sensor settings:', error);
        alert('Ошибка сохранения настроек');
    }
}

// Переключение отправки данных
function toggleSendToNetworks(sensorId) {
    // Ищем по адресу, индексу или id
    const sensor = sensors.find(s => (s.address === sensorId || s.index === sensorId || s.id === sensorId));
    if (!sensor) return;
    
    sensor.sendToNetworks = !sensor.sendToNetworks;
    
    // Сохраняем на сервер
    fetch('/api/sensors', {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json'
        },
        body: JSON.stringify({ sensors: sensors })
    }).catch(error => {
        console.error('Error saving sensor:', error);
    });
    
    renderSensorCells();
}

// Переключение бипера
function toggleBuzzer(sensorId) {
    // Ищем по адресу, индексу или id
    const sensor = sensors.find(s => (s.address === sensorId || s.index === sensorId || s.id === sensorId));
    if (!sensor) return;
    
    sensor.buzzerEnabled = !sensor.buzzerEnabled;
    
    // Сохраняем на сервер
    fetch('/api/sensors', {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json'
        },
        body: JSON.stringify({ sensors: sensors })
    }).catch(error => {
        console.error('Error saving sensor:', error);
    });
    
    renderSensorCells();
}

// Закрытие модального окна по клику на overlay
document.addEventListener('DOMContentLoaded', () => {
    const modal = document.getElementById('sensor-settings-modal');
    if (modal) {
        modal.addEventListener('click', (e) => {
            if (e.target === modal) {
                closeSensorSettings();
            }
        });
    }
});

function updateWiFiHeader(status, rssi) {
    if (!elements.wifiIcon || !elements.wifiRssiHeader || !elements.wifiStatusHeader) return;
    
    const isConnected = status === 'connected';
    
    // Обновляем иконку
    if (isConnected && rssi !== undefined) {
        elements.wifiIcon.className = 'wifi-connected';
        
        // Определяем количество полосок по уровню сигнала
        let bars = 4;
        if (rssi < -80) bars = 1;
        else if (rssi < -70) bars = 2;
        else if (rssi < -60) bars = 3;
        else bars = 4;
        
        // Создаем визуализацию полосок
        const barsHtml = Array.from({length: 4}, (_, i) => {
            const height = [4, 6, 8, 10][i];
            const opacity = i < bars ? 1 : 0.3;
            return `<span class="wifi-bar" style="height: ${height}px; opacity: ${opacity};"></span>`;
        }).join('');
        
        elements.wifiIcon.innerHTML = `<span class="wifi-bars">${barsHtml}</span>`;
        
        // Обновляем текст RSSI
        elements.wifiRssiHeader.textContent = `${rssi} dBm`;
        
        // Добавляем класс для слабого сигнала
        if (rssi < -70) {
            elements.wifiStatusHeader.classList.add('wifi-weak');
        } else {
            elements.wifiStatusHeader.classList.remove('wifi-weak');
        }
    } else {
        elements.wifiIcon.className = 'wifi-disconnected';
        elements.wifiIcon.innerHTML = '📶';
        elements.wifiRssiHeader.textContent = 'Отключено';
        elements.wifiStatusHeader.classList.remove('wifi-weak');
    }
}

// Функция показа ошибки
function showError() {
    if (elements.temperature) {
        elements.temperature.textContent = '--';
        elements.temperature.classList.add('loading');
    }
    elements.lastUpdate.textContent = 'Ошибка подключения';
}

// Функция обновления селекторов термометров для графика
function updateChartSensorSelectors() {
    const container = document.getElementById('chart-sensors-select');
    if (!container) return;
    
    container.innerHTML = '';
    
    sensors.filter(s => s.enabled).forEach(sensor => {
        const label = document.createElement('label');
        label.style.cssText = 'display: flex; align-items: center; gap: 5px; cursor: pointer;';
        
        const checkbox = document.createElement('input');
        checkbox.type = 'checkbox';
        checkbox.checked = true;
        // Используем адрес как ключ, если есть, иначе индекс или id
        checkbox.value = sensor.address || sensor.index || sensor.id;
        checkbox.onchange = () => updateChart();
        
        label.appendChild(checkbox);
        label.appendChild(document.createTextNode(sensor.name || `Термометр ${sensor.index + 1}`));
        container.appendChild(label);
    });
}

// Функция определения интервала агрегации данных в зависимости от периода
function getAggregationInterval(period) {
    switch(period) {
        case '1m': return 60; // 1 минута
        case '5m': return 300; // 5 минут
        case '15m': return 900; // 15 минут
        case '30m': return 1800; // 30 минут
        case '1h': return 3600; // 1 час
        case '6h': return 21600; // 6 часов
        case '24h': return 86400; // 24 часа
        case '7d': return 604800; // 7 дней
        default: return 3600; // По умолчанию 1 час
    }
}

// Функция агрегации данных по интервалам
function aggregateData(records, intervalSeconds) {
    if (!records || records.length === 0) return [];
    
    const aggregated = [];
    let currentBucket = null;
    let bucketStartTime = null;
    
    records.forEach(record => {
        const recordTime = record.timestamp;
        
        // Определяем начало текущего бакета
        const bucketTime = Math.floor(recordTime / intervalSeconds) * intervalSeconds;
        
        if (bucketStartTime !== bucketTime) {
            // Новый бакет
            if (currentBucket !== null) {
                aggregated.push(currentBucket);
            }
            currentBucket = {
                timestamp: bucketTime,
                temperatures: [],
                sensors: {}
            };
            bucketStartTime = bucketTime;
        }
        
        // Добавляем температуру в текущий бакет (пропускаем нулевые и невалидные значения)
        if (record.temperature !== null && 
            record.temperature !== undefined && 
            record.temperature !== 0 && 
            record.temperature !== -127.0) {
            currentBucket.temperatures.push(record.temperature);
            const sensorKey = record.sensor_id || record.sensor_address || 'default';
            if (!currentBucket.sensors[sensorKey]) {
                currentBucket.sensors[sensorKey] = [];
            }
            currentBucket.sensors[sensorKey].push(record.temperature);
        }
    });
    
    // Добавляем последний бакет
    if (currentBucket !== null) {
        aggregated.push(currentBucket);
    }
    
    // Вычисляем средние значения для каждого бакета
    return aggregated.map(bucket => {
        const result = {
            timestamp: bucket.timestamp,
            sensors: {}
        };
        
        // Среднее по всем датчикам
        if (bucket.temperatures.length > 0) {
            const sum = bucket.temperatures.reduce((a, b) => a + b, 0);
            result.average = sum / bucket.temperatures.length;
        }
        
        // Среднее по каждому датчику
        Object.keys(bucket.sensors).forEach(sensorKey => {
            const temps = bucket.sensors[sensorKey];
            if (temps.length > 0) {
                const sum = temps.reduce((a, b) => a + b, 0);
                result.sensors[sensorKey] = sum / temps.length;
            }
        });
        
        return result;
    });
}

// Функция загрузки графика
async function loadChart(period) {
    // Проверяем доступность Chart.js
    if (typeof Chart === 'undefined') {
        console.warn('Chart.js not available - skipping chart load');
        return;
    }

    currentChartPeriod = period;
    chartZoom = { min: null, max: null }; // Сбрасываем масштаб при смене периода
    chartPan = { offset: 0 }; // Сбрасываем смещение
    
    // Получаем выбранные термометры (используем адрес или индекс)
    const selectedCheckboxes = document.querySelectorAll('#chart-sensors-select input[type="checkbox"]:checked');
    const selectedSensorKeys = Array.from(selectedCheckboxes).map(cb => cb.value);
    
    if (selectedSensorKeys.length === 0) {
        // Выбираем все включенные термометры по адресу или индексу
        selectedSensorKeys.push(...sensors.filter(s => s.enabled).map(s => s.address || s.index || s.id));
    }
    
    try {
        // Определяем период для запроса
        let periodParam = period;
        if (period === '1m' || period === '5m' || period === '15m' || period === '30m') {
            periodParam = '1h'; // Для коротких периодов запрашиваем данные за час
        }
        
        const response = await fetch(`/api/temperature/history?period=${periodParam}`);
        const data = await response.json();
        
        if (data.data && data.data.length > 0) {
            // Агрегируем данные в зависимости от периода
            const intervalSeconds = getAggregationInterval(period);
            const aggregated = aggregateData(data.data, intervalSeconds);
            
            // Применяем смещение для прокрутки
            let displayData = aggregated;
            if (chartPan.offset > 0 && chartPan.offset < aggregated.length) {
                displayData = aggregated.slice(chartPan.offset);
            }
            
            const labels = [];
            const datasets = [];
            
            // Группируем данные по термометрам
            const sensorDataMap = {};
            selectedSensorKeys.forEach(key => {
                sensorDataMap[key] = [];
            });
            
            displayData.forEach(bucket => {
                const date = new Date(bucket.timestamp * 1000);
                let timeLabel;
                
                // Форматируем метку времени в зависимости от периода
                if (period === '1m' || period === '5m') {
                    timeLabel = date.toLocaleTimeString('ru-RU', { hour: '2-digit', minute: '2-digit' });
                } else if (period === '15m' || period === '30m') {
                    timeLabel = date.toLocaleTimeString('ru-RU', { hour: '2-digit', minute: '2-digit' });
                } else if (period === '1h') {
                    timeLabel = date.toLocaleTimeString('ru-RU', { hour: '2-digit', minute: '2-digit' });
                } else if (period === '6h') {
                    timeLabel = date.toLocaleTimeString('ru-RU', { hour: '2-digit', minute: '2-digit' });
                } else if (period === '24h') {
                    timeLabel = date.toLocaleTimeString('ru-RU', { hour: '2-digit', minute: '2-digit' });
                } else {
                    timeLabel = date.toLocaleDateString('ru-RU', { day: '2-digit', month: '2-digit' }) + ' ' + 
                               date.toLocaleTimeString('ru-RU', { hour: '2-digit', minute: '2-digit' });
                }
                
                labels.push(timeLabel);
                
                // Заполняем данные для каждого датчика
                // Заполняем данные для каждого выбранного датчика
                selectedSensorKeys.forEach(key => {
                    if (!sensorDataMap[key]) {
                        sensorDataMap[key] = [];
                    }
                    
                    // Ищем данные для этого датчика в бакете
                    // Проверяем точное совпадение ключа
                    let temp = bucket.sensors[key];
                    
                    // Если не найдено, ищем по адресу термометра
                    if (temp === undefined) {
                        const sensor = sensors.find(s => 
                            (s.address === key || String(s.index) === key || String(s.id) === key)
                        );
                        if (sensor && sensor.address) {
                            // Пробуем найти по адресу
                            temp = bucket.sensors[sensor.address];
                        }
                    }
                    
                    // Заполняем массив до текущей позиции null, если нужно
                    while (sensorDataMap[key].length < labels.length - 1) {
                        sensorDataMap[key].push(null);
                    }
                    
                    // Добавляем температуру или null (пропускаем нулевые значения)
                    if (temp !== undefined && temp !== null && temp !== 0 && temp !== -127.0) {
                        sensorDataMap[key].push(temp);
                    } else {
                        sensorDataMap[key].push(null);
                    }
                });
            });
            
            // Создаем датасеты для каждого термометра
            const colors = [
                { border: 'rgb(102, 126, 234)', bg: 'rgba(102, 126, 234, 0.1)' },
                { border: 'rgb(255, 99, 132)', bg: 'rgba(255, 99, 132, 0.1)' },
                { border: 'rgb(75, 192, 192)', bg: 'rgba(75, 192, 192, 0.1)' },
                { border: 'rgb(255, 206, 86)', bg: 'rgba(255, 206, 86, 0.1)' },
                { border: 'rgb(153, 102, 255)', bg: 'rgba(153, 102, 255, 0.1)' }
            ];
            
            let colorIndex = 0;
            selectedSensorKeys.forEach(key => {
                // Ищем датчик по адресу, индексу или id
                const sensor = sensors.find(s => (s.address === key || s.index === key || s.id === key));
                if (sensor) {
                    const color = colors[colorIndex % colors.length];
                    datasets.push({
                        label: sensor.name || `Термометр ${sensor.index + 1}`,
                        data: sensorDataMap[key] || [],
                        borderColor: color.border,
                        backgroundColor: color.bg,
                        tension: 0.4,
                        fill: true,
                        spanGaps: true // Пропускаем пропуски в данных
                    });
                    colorIndex++;
                }
            });
            
            // Если нет данных, показываем сообщение
            if (labels.length === 0) {
                labels.push('Нет данных');
                datasets.push({
                    label: 'Нет данных',
                    data: [null],
                    borderColor: 'rgba(0, 0, 0, 0.1)',
                    backgroundColor: 'rgba(0, 0, 0, 0.05)'
                });
            }
            
            // Настройки масштаба
            const yAxisOptions = {
                beginAtZero: false,
                title: {
                    display: true,
                    text: 'Температура (°C)'
                }
            };
            
            if (chartZoom.min !== null) {
                yAxisOptions.min = chartZoom.min;
            }
            if (chartZoom.max !== null) {
                yAxisOptions.max = chartZoom.max;
            }
            
            if (temperatureChart) {
                temperatureChart.data.labels = labels;
                temperatureChart.data.datasets = datasets;
                temperatureChart.options.scales.y = yAxisOptions;
                temperatureChart.update();
            } else {
                const ctx = document.getElementById('temperatureChart').getContext('2d');
                temperatureChart = new Chart(ctx, {
                    type: 'line',
                    data: {
                        labels: labels,
                        datasets: datasets
                    },
                    options: {
                        responsive: true,
                        maintainAspectRatio: true,
                        interaction: {
                            mode: 'index',
                            intersect: false
                        },
                        plugins: {
                            legend: {
                                display: true,
                                position: 'top'
                            },
                            zoom: {
                                zoom: {
                                    wheel: {
                                        enabled: true
                                    },
                                    pinch: {
                                        enabled: true
                                    },
                                    mode: 'xy',
                                    limits: {
                                        y: { min: -50, max: 100 }
                                    }
                                },
                                pan: {
                                    enabled: true,
                                    mode: 'x'
                                }
                            }
                        },
                        scales: {
                            y: yAxisOptions,
                            x: {
                                title: {
                                    display: true,
                                    text: 'Время'
                                }
                            }
                        }
                    }
                });
            }
        }
    } catch (error) {
        console.error('Error loading chart:', error);
    }
}

// Функция обновления графика
function updateChart() {
    loadChart(currentChartPeriod);
}

// Функция сброса масштаба графика
function resetChartZoom() {
    chartZoom = { min: null, max: null };
    chartPan = { offset: 0 };
    if (temperatureChart) {
        temperatureChart.resetZoom();
        loadChart(currentChartPeriod);
    }
}

// Функция инициализации
async function init() {
    // Сначала загружаем список термометров и ждём завершения
    // Это критически важно - без списка сенсоров плитки не отрисуются
    await loadSensors();

    // Теперь загружаем данные (температуры, статусы)
    await fetchData();

    // Загрузка графика (только если Chart.js доступен)
    if (typeof Chart !== 'undefined') {
        loadChart('24h');
    } else {
        console.warn('Chart.js not loaded - charts disabled');
        // Показываем сообщение о недоступности графика
        const chartCanvas = document.getElementById('temperatureChart');
        const chartUnavailable = document.getElementById('chart-unavailable');
        if (chartCanvas) chartCanvas.style.display = 'none';
        if (chartUnavailable) chartUnavailable.style.display = 'block';
    }

    // Установка интервала обновления
    updateInterval = setInterval(() => {
        fetchData();
        if (typeof Chart !== 'undefined') {
            loadChart(currentChartPeriod);
        }
    }, UPDATE_INTERVAL);
    
    // Обработка видимости страницы (остановка обновлений при скрытии вкладки)
    document.addEventListener('visibilitychange', () => {
        if (document.hidden) {
            if (updateInterval) {
                clearInterval(updateInterval);
                updateInterval = null;
            }
        } else {
            if (!updateInterval) {
                fetchData();
                if (typeof Chart !== 'undefined') {
                    loadChart(currentChartPeriod);
                }
                updateInterval = setInterval(() => {
                    fetchData();
                    if (typeof Chart !== 'undefined') {
                        loadChart(currentChartPeriod);
                    }
                }, UPDATE_INTERVAL);
            }
        }
    });
}

// Запуск при загрузке страницы
if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
} else {
    init();
}
