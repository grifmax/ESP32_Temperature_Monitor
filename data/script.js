// Конфигурация
const API_ENDPOINT = '/api/data';
const UPDATE_INTERVAL = 5000; // 5 секунд

// Элементы DOM
const elements = {
    wifiIcon: document.getElementById('wifi-icon'),
    wifiRssiHeader: document.getElementById('wifi-rssi-header'),
    wifiStatusHeader: document.getElementById('wifi-status-header'),
    ipAddressHeader: document.getElementById('ip-address-header'),
    uptime: document.getElementById('uptime'),
    lastUpdate: document.getElementById('last-update'),
    sensorsGrid: document.getElementById('sensors-grid')
};

// Состояние
let updateInterval = null;
let temperatureChart = null;
let currentChartPeriod = '24h';
let sensors = [];
let sensorsData = {}; // Данные по каждому термометру {id: {currentTemp, stabilizationState}}

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
    if (elements.uptime) {
        elements.uptime.textContent = data.uptime_formatted || '--';
    }
    
    // Обновляем данные термометров
    if (data.sensors && Array.isArray(data.sensors)) {
        data.sensors.forEach(sensor => {
            sensorsData[sensor.id] = {
                currentTemp: sensor.currentTemp,
                stabilizationState: sensor.stabilizationState || 'tracking'
            };
        });
        renderSensorCells();
    }
}

// Загрузка списка термометров
async function loadSensors() {
    try {
        const response = await fetch('/api/sensors');
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
    }
}

// Рендеринг ячеек термометров
function renderSensorCells() {
    if (!elements.sensorsGrid) return;
    
    elements.sensorsGrid.innerHTML = '';
    
    // Фильтруем только включенные термометры
    const enabledSensors = sensors.filter(s => s.enabled);
    
    enabledSensors.forEach(sensor => {
        const cell = document.createElement('div');
        cell.className = 'sensor-cell';
        cell.onclick = () => openSensorSettings(sensor.id);
        
        const sensorData = sensorsData[sensor.id] || {};
        const currentTemp = sensorData.currentTemp !== undefined ? sensorData.currentTemp : null;
        const tempDisplay = currentTemp !== null ? currentTemp.toFixed(1) : '--';
        
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
        sendButton.onclick = (e) => { e.stopPropagation(); toggleSendToNetworks(sensor.id); };
        sendButton.innerHTML = `<span>📤</span><span>${sensor.sendToNetworks ? 'Вкл' : 'Выкл'}</span>`;
        
        const buzzerButton = document.createElement('button');
        buzzerButton.className = `sensor-buzzer-button ${sensor.buzzerEnabled ? 'active' : ''}`;
        buzzerButton.onclick = (e) => { e.stopPropagation(); toggleBuzzer(sensor.id); };
        const buzzerIcon = sensor.buzzerEnabled ? '🔊' : '🔇';
        buzzerButton.innerHTML = `<span>${buzzerIcon}</span>`;
        
        buttonsContainer.appendChild(sendButton);
        buttonsContainer.appendChild(buzzerButton);
        
        cell.innerHTML = `
            <div class="sensor-name">${sensor.name}</div>
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
    const sensor = sensors.find(s => s.id === sensorId);
    if (!sensor) return;
    
    const modal = document.getElementById('sensor-settings-modal');
    if (!modal) return;
    
    // Заполняем форму данными термометра
    document.getElementById('modal-sensor-id').value = sensor.id;
    document.getElementById('modal-sensor-name').textContent = `Настройки: ${sensor.name}`;
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
    const alertSettings = document.getElementById('modal-alert-settings');
    const stabSettings = document.getElementById('modal-stabilization-settings');
    
    if (alertSettings) alertSettings.style.display = (mode === 'alert') ? 'block' : 'none';
    if (stabSettings) stabSettings.style.display = (mode === 'stabilization') ? 'block' : 'none';
}

// Сохранение настроек термометра
async function saveSensorSettings() {
    const sensorId = parseInt(document.getElementById('modal-sensor-id').value);
    const sensor = sensors.find(s => s.id === sensorId);
    if (!sensor) return;
    
    // Обновляем данные термометра
    sensor.mode = document.getElementById('modal-sensor-mode').value;
    sensor.sendToNetworks = document.getElementById('modal-send-to-networks').checked;
    
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
    
    // Сохраняем на сервер
    try {
        const response = await fetch(`/api/sensor/${sensorId}`, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify(sensor)
        });
        
        if (response.ok) {
            renderSensorCells();
            closeSensorSettings();
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
    const sensor = sensors.find(s => s.id === sensorId);
    if (!sensor) return;
    
    sensor.sendToNetworks = !sensor.sendToNetworks;
    
    // Сохраняем на сервер
    fetch(`/api/sensor/${sensorId}`, {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json'
        },
        body: JSON.stringify(sensor)
    }).catch(error => {
        console.error('Error saving sensor:', error);
    });
    
    renderSensorCells();
}

// Переключение бипера
function toggleBuzzer(sensorId) {
    const sensor = sensors.find(s => s.id === sensorId);
    if (!sensor) return;
    
    if (!sensor.buzzerEnabled) sensor.buzzerEnabled = false;
    sensor.buzzerEnabled = !sensor.buzzerEnabled;
    
    // Сохраняем на сервер
    fetch(`/api/sensor/${sensorId}`, {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json'
        },
        body: JSON.stringify(sensor)
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

function saveModeSettings() {
    const mode = parseInt(document.getElementById('operation-mode-main').value);
    
    let settings = {};
    
    if (mode === 2) {
        // Режим оповещения
        settings = {
            mode: mode,
            alert: {
                min_temp: parseFloat(document.getElementById('alert-min-temp-main').value) || 10.0,
                max_temp: parseFloat(document.getElementById('alert-max-temp-main').value) || 30.0,
                buzzer_enabled: document.getElementById('alert-buzzer-main').checked
            }
        };
    } else if (mode === 3) {
        // Режим стабилизации
        settings = {
            mode: mode,
            stabilization: {
                target_temp: parseFloat(document.getElementById('stab-target-temp-main').value) || 25.0,
                tolerance: parseFloat(document.getElementById('stab-tolerance-main').value) || 0.1,
                alert_threshold: parseFloat(document.getElementById('stab-alert-threshold-main').value) || 0.2,
                duration: parseInt(document.getElementById('stab-duration-main').value) || 10,
                buzzer_enabled: document.getElementById('stab-buzzer-main').checked
            }
        };
    } else {
        settings = { mode: mode };
    }
    
    fetch('/api/mode', {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json'
        },
        body: JSON.stringify(settings)
    })
    .then(response => response.json())
    .then(data => {
        alert('Настройки режима сохранены!');
    })
    .catch(error => {
        console.error('Error saving mode settings:', error);
        alert('Ошибка сохранения настроек');
    });
}

function changeModeFromMain(mode) {
    const modeNum = parseInt(mode);
    updateModeDescription(modeNum);
    
    // Отправляем изменение режима на сервер
    fetch('/api/mode', {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json'
        },
        body: JSON.stringify({ mode: modeNum })
    })
    .then(response => response.json())
    .then(data => {
        console.log('Mode changed:', data);
    })
    .catch(error => {
        console.error('Error changing mode:', error);
    });
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
        checkbox.value = sensor.id;
        checkbox.onchange = () => updateChart();
        
        label.appendChild(checkbox);
        label.appendChild(document.createTextNode(sensor.name));
        container.appendChild(label);
    });
}

// Функция загрузки графика
async function loadChart(period) {
    currentChartPeriod = period;
    
    // Получаем выбранные термометры
    const selectedCheckboxes = document.querySelectorAll('#chart-sensors-select input[type="checkbox"]:checked');
    const selectedSensorIds = Array.from(selectedCheckboxes).map(cb => parseInt(cb.value));
    
    if (selectedSensorIds.length === 0) {
        selectedSensorIds.push(...sensors.filter(s => s.enabled).map(s => s.id));
    }
    
    try {
        const response = await fetch(`/api/temperature/history?period=${period}`);
        const data = await response.json();
        
        if (data.data && data.data.length > 0) {
            const labels = [];
            const datasets = [];
            
            // Группируем данные по термометрам
            const sensorDataMap = {};
            selectedSensorIds.forEach(id => {
                sensorDataMap[id] = [];
            });
            
            data.data.forEach(record => {
                const date = new Date(record.timestamp * 1000);
                const timeLabel = date.toLocaleTimeString('ru-RU', { hour: '2-digit', minute: '2-digit' });
                
                if (!labels.includes(timeLabel)) {
                    labels.push(timeLabel);
                }
                
                if (record.sensor_id && sensorDataMap[record.sensor_id]) {
                    const index = labels.indexOf(timeLabel);
                    sensorDataMap[record.sensor_id][index] = record.temperature;
                }
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
            selectedSensorIds.forEach(id => {
                const sensor = sensors.find(s => s.id === id);
                if (sensor) {
                    const color = colors[colorIndex % colors.length];
                    datasets.push({
                        label: sensor.name,
                        data: sensorDataMap[id],
                        borderColor: color.border,
                        backgroundColor: color.bg,
                        tension: 0.4,
                        fill: true
                    });
                    colorIndex++;
                }
            });
            
            if (temperatureChart) {
                temperatureChart.data.labels = labels;
                temperatureChart.data.datasets = datasets;
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
                        plugins: {
                            legend: {
                                display: true,
                                position: 'top'
                            }
                        },
                        scales: {
                            y: {
                                beginAtZero: false,
                                title: {
                                    display: true,
                                    text: 'Температура (°C)'
                                }
                            },
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

// Функция инициализации
function init() {
    // Загружаем список термометров
    loadSensors();
    
    // Первая загрузка данных
    fetchData();
    
    // Загрузка графика
    loadChart('24h');
    
    // Установка интервала обновления
    updateInterval = setInterval(() => {
        fetchData();
        loadChart(currentChartPeriod);
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
                loadChart(currentChartPeriod);
                updateInterval = setInterval(() => {
                    fetchData();
                    loadChart(currentChartPeriod);
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
