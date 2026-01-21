// Функция переключения вкладок
function showTab(tabName) {
    // Скрыть все вкладки
    document.querySelectorAll('.tab-content').forEach(tab => {
        tab.classList.remove('active');
    });
    
    // Убрать активный класс у всех кнопок
    document.querySelectorAll('.tab-button').forEach(btn => {
        btn.classList.remove('active');
    });
    
    // Показать выбранную вкладку
    document.getElementById(tabName + '-tab').classList.add('active');
    
    // Добавить активный класс к кнопке
    if (event && event.target) {
        event.target.classList.add('active');
    }
}

// Функция сканирования Wi-Fi сетей (асинхронная с polling)
let scanPollInterval = null;
let scanAttempts = 0;
const MAX_SCAN_ATTEMPTS = 10;

async function scanWiFi() {
    const wifiList = document.getElementById('wifi-list');
    const scanButton = document.querySelector('.btn-scan');
    
    if (!wifiList || !scanButton) return;
    
    // Останавливаем предыдущий polling если был
    if (scanPollInterval) {
        clearInterval(scanPollInterval);
        scanPollInterval = null;
    }
    scanAttempts = 0;
    
    // Показываем индикатор загрузки
    scanButton.disabled = true;
    scanButton.textContent = '⏳ Сканирование...';
    wifiList.innerHTML = '<div class="loading">⏳ Сканирование сетей...</div>';
    wifiList.style.display = 'block';
    
    try {
        // Запускаем сканирование
        const response = await fetch('/api/wifi/scan');
        if (!response.ok) {
            throw new Error(`HTTP error! status: ${response.status}`);
        }
        const data = await response.json();
        
        if (data.status === 'complete' && data.networks && data.networks.length > 0) {
            // Результаты уже готовы
            displayWiFiNetworks(data.networks, wifiList);
            scanButton.disabled = false;
            scanButton.textContent = '🔍 Сканировать сети';
        } else if (data.status === 'scanning') {
            // Сканирование в процессе - запускаем polling
            scanPollInterval = setInterval(() => pollScanResults(wifiList, scanButton), 1500);
        } else {
            wifiList.innerHTML = '<div class="loading">Сети не найдены</div>';
            scanButton.disabled = false;
            scanButton.textContent = '🔍 Сканировать сети';
        }
    } catch (error) {
        console.error('Error scanning WiFi:', error);
        wifiList.innerHTML = '<div class="loading" style="color: #F44336;">Ошибка сканирования сетей</div>';
        showMessage('Ошибка сканирования Wi-Fi сетей', 'error');
        scanButton.disabled = false;
        scanButton.textContent = '🔍 Сканировать сети';
    }
}

async function pollScanResults(wifiList, scanButton) {
    scanAttempts++;
    
    if (scanAttempts > MAX_SCAN_ATTEMPTS) {
        clearInterval(scanPollInterval);
        scanPollInterval = null;
        wifiList.innerHTML = '<div class="loading" style="color: #F44336;">Тайм-аут сканирования</div>';
        scanButton.disabled = false;
        scanButton.textContent = '🔍 Сканировать сети';
        return;
    }
    
    try {
        const response = await fetch('/api/wifi/scan');
        const data = await response.json();
        
        if (data.status === 'complete') {
            clearInterval(scanPollInterval);
            scanPollInterval = null;
            
            if (data.networks && data.networks.length > 0) {
                displayWiFiNetworks(data.networks, wifiList);
            } else {
                wifiList.innerHTML = '<div class="loading">Сети не найдены</div>';
            }
            scanButton.disabled = false;
            scanButton.textContent = '🔍 Сканировать сети';
        }
        // Если status === 'scanning', продолжаем polling
    } catch (error) {
        console.error('Error polling scan results:', error);
    }
}

function displayWiFiNetworks(networks, wifiList) {
    // Сортируем сети по уровню сигнала (RSSI) - от сильного к слабому
    networks.sort((a, b) => b.rssi - a.rssi);
    
    wifiList.innerHTML = '';
    
    networks.forEach(network => {
        const wifiItem = document.createElement('div');
        wifiItem.className = 'wifi-item';
        wifiItem.onclick = () => selectWiFiNetwork(network.ssid);
        
        // Определяем уровень сигнала
        let signalBars = '▁';
        if (network.rssi > -50) {
            signalBars = '▁▃▅▇';
        } else if (network.rssi > -60) {
            signalBars = '▁▃▅';
        } else if (network.rssi > -70) {
            signalBars = '▁▃';
        }
        
        wifiItem.innerHTML = `
            <div class="wifi-ssid">
                ${network.ssid || '(Скрытая сеть)'}
                <span class="wifi-signal">${signalBars}</span>
            </div>
            <div class="wifi-info">
                <span>${network.encryption === 'open' ? '🔓 Открытая' : '🔒 Защищена'}</span>
                <span>${network.rssi} dBm • Канал ${network.channel}</span>
            </div>
        `;
        
        wifiList.appendChild(wifiItem);
    });
}

// Функция выбора Wi-Fi сети
function selectWiFiNetwork(ssid) {
    const ssidInput = document.getElementById('wifi-ssid');
    if (ssidInput) {
        ssidInput.value = ssid;
        ssidInput.focus();
        
        // Подсвечиваем выбранную сеть
        document.querySelectorAll('.wifi-item').forEach(item => {
            item.style.background = '';
        });
        event.currentTarget.style.background = '#e3f2fd';
        
        showMessage(`Выбрана сеть: ${ssid}`, 'success');
    }
}

// Функция сохранения настроек Wi-Fi
async function saveWiFi() {
    const ssid = document.getElementById('wifi-ssid').value.trim();
    const password = document.getElementById('wifi-password').value;
    
    if (!ssid) {
        showMessage('Введите имя сети (SSID)', 'error');
        return;
    }
    
    try {
        // Сначала сохраняем настройки
        const settings = {
            wifi: {
                ssid: ssid,
                password: password || ''
            }
        };
        
        const saveResponse = await fetch('/api/settings', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify(settings)
        });
        
        if (!saveResponse.ok) {
            throw new Error('Ошибка сохранения настроек');
        }
        
        // Затем пытаемся подключиться
        showMessage('Подключение к сети...', 'success');
        
        const connectResponse = await fetch('/api/wifi/connect', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                ssid: ssid,
                password: password
            })
        });
        
        const connectResult = await connectResponse.json();
        
        if (connectResult.status === 'connected') {
            showMessage(`Успешно подключено! IP: ${connectResult.ip}`, 'success');
            // Перезагружаем страницу через 2 секунды
            setTimeout(() => {
                window.location.reload();
            }, 2000);
        } else {
            showMessage('Не удалось подключиться к сети. Проверьте пароль.', 'error');
        }
    } catch (error) {
        console.error('Error saving WiFi:', error);
        showMessage('Ошибка сохранения настроек Wi-Fi', 'error');
    }
}

// Функция показа сообщения
function showMessage(text, type) {
    const messageEl = document.getElementById('message');
    messageEl.textContent = text;
    messageEl.className = 'message ' + type + ' show';
    
    setTimeout(() => {
        messageEl.classList.remove('show');
    }, 5000);
}

// Функция загрузки настроек
async function loadSettings() {
    try {
        const response = await fetch('/api/settings');
        const settings = await response.json();
        
        // Заполнить форму Wi-Fi
        if (settings.wifi) {
            document.getElementById('wifi-ssid').value = settings.wifi.ssid || '';
            document.getElementById('wifi-password').value = settings.wifi.password || '';
        }
        
        // Заполнить форму MQTT
        if (settings.mqtt) {
            document.getElementById('mqtt-server').value = settings.mqtt.server || '';
            document.getElementById('mqtt-port').value = settings.mqtt.port || 1883;
            document.getElementById('mqtt-user').value = settings.mqtt.user || '';
            document.getElementById('mqtt-password').value = settings.mqtt.password || '';
            document.getElementById('mqtt-topic-status').value = settings.mqtt.topic_status || 'home/thermo/status';
            document.getElementById('mqtt-topic-control').value = settings.mqtt.topic_control || 'home/thermo/control';
        }
        
        // Заполнить форму Telegram
        if (settings.telegram) {
            document.getElementById('telegram-bot-token').value = settings.telegram.bot_token || '';
            document.getElementById('telegram-chat-id').value = settings.telegram.chat_id || '';
        }
        
        // Заполнить форму температуры
        if (settings.temperature) {
            document.getElementById('temp-high').value = settings.temperature.high_threshold || 30.0;
            document.getElementById('temp-low').value = settings.temperature.low_threshold || 10.0;
        }
        
        // Заполнить форму часового пояса
        if (settings.timezone) {
            document.getElementById('timezone-offset').value = settings.timezone.offset || 3;
        }
        
        // Заполнить форму режима работы
        if (settings.operation_mode !== undefined) {
            document.getElementById('operation-mode').value = settings.operation_mode;
            updateModeSettings();
        }
        
        // Заполнить настройки режима оповещения
        if (settings.alert) {
            document.getElementById('alert-min-temp').value = settings.alert.min_temp || 10.0;
            document.getElementById('alert-max-temp').value = settings.alert.max_temp || 30.0;
            document.getElementById('alert-buzzer').checked = settings.alert.buzzer_enabled !== false;
        }
        
        // Заполнить настройки режима стабилизации
        if (settings.stabilization) {
            document.getElementById('stab-target-temp').value = settings.stabilization.target_temp || 25.0;
            document.getElementById('stab-tolerance').value = settings.stabilization.tolerance || 0.1;
            document.getElementById('stab-alert-threshold').value = settings.stabilization.alert_threshold || 0.2;
            document.getElementById('stab-duration').value = (settings.stabilization.duration || 600) / 60;
        }
    } catch (error) {
        console.error('Error loading settings:', error);
        showMessage('Ошибка загрузки настроек', 'error');
    }
}

// Функция сохранения настроек
async function saveSettings() {
    try {
        const settings = {
            wifi: {
                ssid: document.getElementById('wifi-ssid').value,
                password: document.getElementById('wifi-password').value
            },
            mqtt: {
                server: document.getElementById('mqtt-server').value,
                port: parseInt(document.getElementById('mqtt-port').value),
                user: document.getElementById('mqtt-user').value,
                password: document.getElementById('mqtt-password').value,
                topic_status: document.getElementById('mqtt-topic-status').value,
                topic_control: document.getElementById('mqtt-topic-control').value
            },
            telegram: {
                bot_token: document.getElementById('telegram-bot-token').value,
                chat_id: document.getElementById('telegram-chat-id').value
            },
            temperature: {
                high_threshold: parseFloat(document.getElementById('temp-high').value),
                low_threshold: parseFloat(document.getElementById('temp-low').value)
            },
            timezone: {
                offset: parseInt(document.getElementById('timezone-offset').value)
            },
            operation_mode: parseInt(document.getElementById('operation-mode') ? document.getElementById('operation-mode').value : 0)
        };
        
        // Добавляем настройки режима оповещения, если они есть
        if (document.getElementById('alert-min-temp')) {
            settings.alert = {
                min_temp: parseFloat(document.getElementById('alert-min-temp').value),
                max_temp: parseFloat(document.getElementById('alert-max-temp').value),
                buzzer_enabled: document.getElementById('alert-buzzer').checked
            };
        }
        
        // Добавляем настройки режима стабилизации, если они есть
        if (document.getElementById('stab-target-temp')) {
            settings.stabilization = {
                target_temp: parseFloat(document.getElementById('stab-target-temp').value),
                tolerance: parseFloat(document.getElementById('stab-tolerance').value),
                alert_threshold: parseFloat(document.getElementById('stab-alert-threshold').value),
                duration: parseInt(document.getElementById('stab-duration').value) * 60
            };
        }
        
        const response = await fetch('/api/settings', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify(settings)
        });
        
        if (response.ok) {
            showMessage('Настройки успешно сохранены', 'success');
        } else {
            showMessage('Ошибка сохранения настроек', 'error');
        }
    } catch (error) {
        console.error('Error saving settings:', error);
        showMessage('Ошибка сохранения настроек', 'error');
    }
}

// Функция обновления настроек режима работы (видимость полей)
function updateModeSettings() {
    const mode = parseInt(document.getElementById('operation-mode').value);
    const alertSettings = document.getElementById('alert-settings');
    const stabSettings = document.getElementById('stabilization-settings');
    const descEl = document.getElementById('mode-description');
    
    const descriptions = {
        0: 'Локальный режим: WiFi включается только при нажатии кнопки',
        1: 'Мониторинг: отправка данных в MQTT и Telegram',
        2: 'Оповещение: тревога при превышении заданных порогов',
        3: 'Стабилизация: поддержание температуры в заданном диапазоне'
    };
    
    if (descEl) descEl.textContent = descriptions[mode] || descriptions[0];
    if (alertSettings) alertSettings.style.display = (mode === 2) ? 'block' : 'none';
    if (stabSettings) stabSettings.style.display = (mode === 3) ? 'block' : 'none';
}

// Функция загрузки настроек режима работы
async function loadModeSettings() {
    try {
        const response = await fetch('/api/settings');
        const settings = await response.json();
        
        if (settings.operation_mode !== undefined) {
            document.getElementById('operation-mode').value = settings.operation_mode;
            updateModeSettings();
        }
        
        if (settings.alert) {
            document.getElementById('alert-min-temp').value = settings.alert.min_temp || 10.0;
            document.getElementById('alert-max-temp').value = settings.alert.max_temp || 30.0;
            document.getElementById('alert-buzzer').checked = settings.alert.buzzer_enabled !== false;
        }
        
        if (settings.stabilization) {
            document.getElementById('stab-target-temp').value = settings.stabilization.target_temp || 25.0;
            document.getElementById('stab-tolerance').value = settings.stabilization.tolerance || 0.1;
            document.getElementById('stab-alert-threshold').value = settings.stabilization.alert_threshold || 0.2;
            document.getElementById('stab-duration').value = (settings.stabilization.duration || 600) / 60;
        }
    } catch (error) {
        console.error('Error loading mode settings:', error);
    }
}

// Функция сохранения настроек режима работы
async function saveModeSettings() {
    try {
        const mode = parseInt(document.getElementById('operation-mode').value);
        let settings = { operation_mode: mode };
        
        if (mode === 2) {
            settings.alert = {
                min_temp: parseFloat(document.getElementById('alert-min-temp').value) || 10.0,
                max_temp: parseFloat(document.getElementById('alert-max-temp').value) || 30.0,
                buzzer_enabled: document.getElementById('alert-buzzer').checked
            };
        } else if (mode === 3) {
            settings.stabilization = {
                target_temp: parseFloat(document.getElementById('stab-target-temp').value) || 25.0,
                tolerance: parseFloat(document.getElementById('stab-tolerance').value) || 0.1,
                alert_threshold: parseFloat(document.getElementById('stab-alert-threshold').value) || 0.2,
                duration: parseInt(document.getElementById('stab-duration').value) * 60 || 600
            };
        }
        
        const response = await fetch('/api/settings', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(settings)
        });
        
        if (response.ok) {
            showMessage('Настройки режима сохранены', 'success');
        } else {
            showMessage('Ошибка сохранения настроек', 'error');
        }
    } catch (error) {
        console.error('Error saving mode settings:', error);
        showMessage('Ошибка сохранения настроек', 'error');
    }
}

// Управление термометрами
let sensors = [];
const MAX_SENSORS = 10;

function loadSensors() {
    const sensorsList = document.getElementById('sensors-list');
    if (!sensorsList) return;
    
    // Загружаем из API
    fetch('/api/sensors')
        .then(response => response.json())
        .then(data => {
            sensors = data.sensors || [];
            if (sensors.length === 0) {
                sensors = [{
                    id: 1,
                    name: 'Термометр 1',
                    enabled: true,
                    correction: 0.0
                }];
            }
            renderSensors();
        })
        .catch(error => {
            console.error('Error loading sensors:', error);
            // Создаем один по умолчанию
            sensors = [{
                id: 1,
                name: 'Термометр 1',
                enabled: true,
                correction: 0.0
            }];
            renderSensors();
        });
}

function renderSensors() {
    const container = document.getElementById('sensors-list');
    if (!container) return;
    
    container.innerHTML = '';
    
    sensors.forEach((sensor, index) => {
        const sensorDiv = document.createElement('div');
        sensorDiv.className = 'sensor-item';
        sensorDiv.style.cssText = 'background: #f5f5f5; border-radius: 8px; padding: 15px; margin-bottom: 15px; border: 1px solid #e0e0e0;';
        
        const header = document.createElement('div');
        header.style.cssText = 'display: flex; align-items: center; justify-content: space-between; margin-bottom: 15px;';
        
        const number = document.createElement('span');
        number.style.cssText = 'font-weight: 600; color: var(--text-primary); font-size: 1.1rem;';
        number.textContent = `Термометр ${sensor.id}`;
        header.appendChild(number);
        
        if (sensors.length > 1) {
            const removeBtn = document.createElement('button');
            removeBtn.className = 'sensor-remove';
            removeBtn.style.cssText = 'background: var(--danger-color); color: white; border: none; border-radius: 4px; padding: 5px 10px; cursor: pointer; font-size: 0.875rem;';
            removeBtn.textContent = 'Удалить';
            removeBtn.onclick = () => removeSensor(index);
            header.appendChild(removeBtn);
        }
        
        sensorDiv.appendChild(header);
        
        const enabledDiv = document.createElement('div');
        enabledDiv.className = 'form-group';
        const enabledLabel = document.createElement('label');
        const enabledCheckbox = document.createElement('input');
        enabledCheckbox.type = 'checkbox';
        enabledCheckbox.id = `sensor-enabled-${index}`;
        enabledCheckbox.checked = sensor.enabled;
        enabledCheckbox.onchange = () => sensors[index].enabled = enabledCheckbox.checked;
        enabledLabel.appendChild(enabledCheckbox);
        enabledLabel.appendChild(document.createTextNode(' Отображать на главном экране'));
        enabledDiv.appendChild(enabledLabel);
        sensorDiv.appendChild(enabledDiv);
        
        const nameDiv = document.createElement('div');
        nameDiv.className = 'form-group';
        const nameLabel = document.createElement('label');
        nameLabel.setAttribute('for', `sensor-name-${index}`);
        nameLabel.textContent = 'Имя термометра';
        const nameInput = document.createElement('input');
        nameInput.type = 'text';
        nameInput.id = `sensor-name-${index}`;
        nameInput.value = sensor.name;
        nameInput.onchange = () => sensors[index].name = nameInput.value;
        nameInput.placeholder = 'Имя термометра';
        nameDiv.appendChild(nameLabel);
        nameDiv.appendChild(nameInput);
        sensorDiv.appendChild(nameDiv);
        
        const correctionDiv = document.createElement('div');
        correctionDiv.className = 'form-group';
        const correctionLabel = document.createElement('label');
        correctionLabel.setAttribute('for', `sensor-correction-${index}`);
        correctionLabel.textContent = 'Коррекция температуры (°C)';
        const correctionInput = document.createElement('input');
        correctionInput.type = 'number';
        correctionInput.id = `sensor-correction-${index}`;
        correctionInput.step = '0.1';
        correctionInput.value = sensor.correction;
        correctionInput.onchange = () => sensors[index].correction = parseFloat(correctionInput.value) || 0;
        correctionInput.placeholder = '0.0';
        const correctionSmall = document.createElement('small');
        correctionSmall.textContent = 'Коррекция применяется к показаниям датчика';
        correctionDiv.appendChild(correctionLabel);
        correctionDiv.appendChild(correctionInput);
        correctionDiv.appendChild(correctionSmall);
        sensorDiv.appendChild(correctionDiv);
        
        container.appendChild(sensorDiv);
    });
}

function addSensor() {
    if (sensors.length >= MAX_SENSORS) {
        showMessage(`Максимум ${MAX_SENSORS} термометров`, 'error');
        return;
    }
    
    const newId = sensors.length > 0 ? Math.max(...sensors.map(s => s.id)) + 1 : 1;
    sensors.push({
        id: newId,
        name: `Термометр ${newId}`,
        enabled: true,
        correction: 0.0
    });
    
    renderSensors();
}

function removeSensor(index) {
    if (sensors.length <= 1) {
        showMessage('Должен остаться хотя бы один термометр', 'error');
        return;
    }
    
    sensors.splice(index, 1);
    renderSensors();
}

// Загрузить настройки при загрузке страницы
document.addEventListener('DOMContentLoaded', () => {
    loadSettings();
    // Загружаем настройки режима отдельно, так как они могут быть не в общих настройках
    if (document.getElementById('operation-mode')) {
        loadModeSettings();
    }
    // Загружаем термометры
    loadSensors();
});
