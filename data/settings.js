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
        } else if (connectResult.status === 'connecting') {
            showMessage('Подключение к сети...', 'success');
            pollWiFiConnection();
        } else {
            showMessage('Не удалось подключиться к сети. Проверьте пароль.', 'error');
        }
    } catch (error) {
        console.error('Error saving WiFi:', error);
        showMessage('Ошибка сохранения настроек Wi-Fi', 'error');
    }
}

async function pollWiFiConnection() {
    const maxAttempts = 15;
    let attempts = 0;
    const interval = setInterval(async () => {
        attempts++;
        try {
            const response = await fetch('/api/data');
            if (!response.ok) return;
            const data = await response.json();
            if (data.wifi_status === 'connected') {
                clearInterval(interval);
                showMessage(`Успешно подключено! IP: ${data.ip}`, 'success');
                setTimeout(() => window.location.reload(), 2000);
                return;
            }
        } catch (error) {
            console.error('Error polling WiFi status:', error);
        }
        if (attempts >= maxAttempts) {
            clearInterval(interval);
            showMessage('Не удалось подключиться к сети. Проверьте пароль.', 'error');
        }
    }, 1000);
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

// Функция polling статуса сохранения
async function waitForSaveComplete(maxAttempts = 20, intervalMs = 500) {
    for (let i = 0; i < maxAttempts; i++) {
        try {
            const response = await fetch('/api/settings/status');
            if (!response.ok) {
                // Если эндпоинт не существует (старая прошивка), считаем успешным
                return { success: true, message: 'Settings saved' };
            }
            const status = await response.json();

            if (status.status === 'success') {
                return { success: true, message: status.message };
            } else if (status.status === 'error') {
                return { success: false, message: status.message };
            } else if (status.status === 'idle') {
                // Сохранение уже завершено
                return { success: true, message: 'Settings saved' };
            }
            // status === 'saving' - продолжаем ждать
            await new Promise(resolve => setTimeout(resolve, intervalMs));
        } catch (error) {
            console.error('Error checking save status:', error);
            // При ошибке сети считаем что сохранение прошло
            return { success: true, message: 'Settings saved (status check failed)' };
        }
    }
    return { success: false, message: 'Save timeout - please check settings' };
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
            document.getElementById('mqtt-security').value = settings.mqtt.security || 'none';
            document.getElementById('mqtt-user').value = settings.mqtt.user || '';
            document.getElementById('mqtt-password').value = settings.mqtt.password || '';
            document.getElementById('mqtt-topic-status').value = settings.mqtt.topic_status || 'home/thermo/status';
            document.getElementById('mqtt-topic-control').value = settings.mqtt.topic_control || 'home/thermo/control';
            // Проверяем, включен ли MQTT (если сервер не пустой, значит включен)
            const mqttEnabledCheckbox = document.getElementById('mqtt-enabled');
            if (mqttEnabledCheckbox) {
                mqttEnabledCheckbox.checked = (settings.mqtt.server && settings.mqtt.server.length > 0 && settings.mqtt.server !== '#');
            }
        }
        
        // Заполнить форму Telegram
        if (settings.telegram) {
            document.getElementById('telegram-bot-token').value = settings.telegram.bot_token || '';
            document.getElementById('telegram-chat-id').value = settings.telegram.chat_id || '';
        }
        
        // Заполнить форму часового пояса
        if (settings.timezone) {
            document.getElementById('timezone-offset').value = settings.timezone.offset || 3;
        }
        
        // Термометры загружаются отдельно через loadSensors()
        
    } catch (error) {
        console.error('Error loading settings:', error);
        showMessage('Ошибка загрузки настроек', 'error');
    }
}

// Функция сохранения настроек
async function saveSettings(section) {
    try {
        const settings = {};
        
        // Сохраняем только указанную секцию или все, если не указано
        if (!section || section === 'wifi') {
            settings.wifi = {
                ssid: document.getElementById('wifi-ssid').value,
                password: document.getElementById('wifi-password').value
            };
        }
        
        if (!section || section === 'mqtt') {
            const mqttEnabled = document.getElementById('mqtt-enabled').checked;
            if (!mqttEnabled) {
                // Если MQTT отключен, очищаем сервер для принудительного отключения
                settings.mqtt = {
                    server: '#', // Специальное значение для отключения
                    port: 1883,
                    security: document.getElementById('mqtt-security').value,
                    user: '',
                    password: '',
                    topic_status: document.getElementById('mqtt-topic-status').value,
                    topic_control: document.getElementById('mqtt-topic-control').value
                };
            } else {
                settings.mqtt = {
                    server: document.getElementById('mqtt-server').value,
                    port: parseInt(document.getElementById('mqtt-port').value),
                    security: document.getElementById('mqtt-security').value,
                    user: document.getElementById('mqtt-user').value,
                    password: document.getElementById('mqtt-password').value,
                    topic_status: document.getElementById('mqtt-topic-status').value,
                    topic_control: document.getElementById('mqtt-topic-control').value
                };
            }
        }
        
        if (!section || section === 'telegram') {
            settings.telegram = {
                bot_token: document.getElementById('telegram-bot-token').value,
                chat_id: document.getElementById('telegram-chat-id').value
            };
        }
        
        if (!section || section === 'timezone') {
            settings.timezone = {
                offset: parseInt(document.getElementById('timezone-offset').value)
            };
        }
        
        if (!section || section === 'sensors') {
            // Сохраняем настройки термометров (используем адрес как ключ)
            // Собираем актуальные значения из полей формы перед сохранением
            const sensorsToSave = sensors.map((s, idx) => {
                // Получаем актуальные значения из полей формы
                const nameInput = document.getElementById(`sensor-name-${idx}`);
                const modeSelect = document.getElementById(`sensor-mode-${idx}`);
                const enabledCheckbox = document.getElementById(`sensor-enabled-${idx}`);
                const correctionInput = document.getElementById(`sensor-correction-${idx}`);

                return {
                    address: s.address || '',
                    name: nameInput ? nameInput.value.trim() : (s.name || ''),
                    enabled: enabledCheckbox ? enabledCheckbox.checked : (s.enabled !== undefined ? s.enabled : true),
                    correction: correctionInput ? (parseFloat(correctionInput.value) || 0.0) : (s.correction || 0.0),
                    mode: modeSelect ? modeSelect.value : (s.mode || 'monitoring'),
                    monitoringThreshold: s.monitoringThreshold !== undefined ? s.monitoringThreshold : 1.0,
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
                };
            });

            // Отправляем через специальный API для датчиков
            const controller = new AbortController();
            const timeoutId = setTimeout(() => controller.abort(), 15000); // 15 секунд таймаут

            try {
                showMessage('Сохранение настроек термометров...', 'success');

                const sensorsResponse = await fetch('/api/sensors', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json'
                    },
                    body: JSON.stringify({ sensors: sensorsToSave }),
                    signal: controller.signal
                });

                clearTimeout(timeoutId);

                if (sensorsResponse.status === 503) {
                    throw new Error('Сервер занят. Попробуйте через несколько секунд.');
                }

                if (!sensorsResponse.ok) {
                    let errorMsg = 'Ошибка сохранения настроек термометров';
                    try {
                        const errorData = await sensorsResponse.json();
                        if (errorData.error || errorData.message) {
                            errorMsg = errorData.error || errorData.message;
                        }
                    } catch (e) {
                        // Игнорируем ошибку парсинга JSON
                    }
                    throw new Error(errorMsg);
                }

                // Ждём небольшую задержку для завершения записи
                await new Promise(resolve => setTimeout(resolve, 500));

            } catch (error) {
                clearTimeout(timeoutId);
                if (error.name === 'AbortError') {
                    throw new Error('Таймаут при сохранении настроек термометров. Попробуйте еще раз.');
                }
                throw error;
            }

            // Перезагружаем список термометров после сохранения
            await loadSensors();
            renderSensors(); // Обновляем отображение в настройках
            showMessage('Настройки термометров сохранены', 'success');
            return; // Выходим после сохранения датчиков
        }
        
        // Сохраняем остальные настройки, если они есть
        if (Object.keys(settings).length > 0) {
            // Создаем AbortController для таймаута
            const controller = new AbortController();
            const timeoutId = setTimeout(() => controller.abort(), 15000); // 15 секунд таймаут

            try {
                const response = await fetch('/api/settings', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json'
                    },
                    body: JSON.stringify(settings),
                    signal: controller.signal
                });

                clearTimeout(timeoutId);

                // Проверяем статус ответа
                if (response.status === 503) {
                    throw new Error('Сервер занят другим сохранением. Попробуйте через несколько секунд.');
                }

                if (!response.ok && response.status !== 202) {
                    // Пытаемся получить детали ошибки из ответа
                    let errorMsg = 'Ошибка сохранения настроек';
                    try {
                        const errorData = await response.json();
                        if (errorData.message) {
                            errorMsg = errorData.message;
                        }
                    } catch (e) {
                        // Игнорируем ошибку парсинга JSON
                    }
                    throw new Error(errorMsg);
                }

                // Если сервер принял запрос (202 Accepted), ждём завершения сохранения
                if (response.status === 202) {
                    showMessage('Сохранение настроек...', 'success');
                    const saveResult = await waitForSaveComplete();
                    if (!saveResult.success) {
                        throw new Error(saveResult.message);
                    }
                }
            } catch (error) {
                clearTimeout(timeoutId);
                if (error.name === 'AbortError') {
                    throw new Error('Таймаут при сохранении настроек. Попробуйте еще раз.');
                }
                throw error;
            }
        }

        showMessage('Настройки успешно сохранены', 'success');
        updateServiceStatus();
    } catch (error) {
        console.error('Error saving settings:', error);
        showMessage('Ошибка сохранения настроек: ' + error.message, 'error');
        // Не перезагружаем страницу, чтобы пользователь мог увидеть ошибку
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

async function updateServiceStatus() {
    try {
        const response = await fetch('/api/data');
        if (!response.ok) return;
        const data = await response.json();

        const mqttStatusEl = document.getElementById('mqtt-status-settings');
        if (mqttStatusEl) {
            mqttStatusEl.textContent = formatMqttStatus(data.mqtt);
        }

        const telegramStatusEl = document.getElementById('telegram-status-settings');
        if (telegramStatusEl) {
            telegramStatusEl.textContent = formatTelegramStatus(data.telegram);
        }
    } catch (error) {
        console.error('Error updating service status:', error);
    }
}

// Управление термометрами
let sensors = [];
const MAX_SENSORS = 10;

// Исправлено: функция теперь async и возвращает Promise
async function loadSensors() {
    const sensorsList = document.getElementById('sensors-list');
    if (!sensorsList) return;

    try {
        const response = await fetch('/api/sensors');
        if (!response.ok) {
            throw new Error(`HTTP error! status: ${response.status}`);
        }
        const data = await response.json();
        sensors = data.sensors || [];

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
                    duration: 10
                }
            }];
        }
        renderSensors();
    } catch (error) {
        console.error('Error loading sensors:', error);
        // Создаем один по умолчанию
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
                duration: 10
            }
        }];
        renderSensors();
    }
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
        number.textContent = sensor.name || `Термометр ${sensor.index + 1}`;
        header.appendChild(number);
        
        // Показываем адрес термометра
        const addressDiv = document.createElement('div');
        addressDiv.style.cssText = 'font-size: 0.75rem; color: #757575; margin-top: 5px; font-family: monospace;';
        addressDiv.textContent = 'Адрес: ' + (sensor.address || 'Неизвестно');
        header.appendChild(addressDiv);
        
        // Убираем кнопку удаления, так как термометры определяются автоматически
        
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
        nameInput.value = sensor.name || `Термометр ${(sensor.index !== undefined ? sensor.index + 1 : index + 1)}`;
        nameInput.onchange = () => sensors[index].name = nameInput.value;
        nameInput.oninput = () => sensors[index].name = nameInput.value;
        nameInput.placeholder = 'Имя термометра';
        nameDiv.appendChild(nameLabel);
        nameDiv.appendChild(nameInput);
        sensorDiv.appendChild(nameDiv);
        
        // Добавляем отображение текущей температуры
        const tempDiv = document.createElement('div');
        tempDiv.className = 'form-group';
        tempDiv.style.cssText = 'background: #e3f2fd; padding: 10px; border-radius: 4px; margin-top: 10px;';
        const tempLabel = document.createElement('label');
        tempLabel.textContent = 'Текущая температура';
        tempLabel.style.cssText = 'font-weight: 600; color: #1976d2;';
        const tempValue = document.createElement('div');
        tempValue.id = `sensor-temp-${index}`;
        tempValue.style.cssText = 'font-size: 1.5rem; font-weight: 600; color: #1976d2; margin-top: 5px;';
        tempValue.textContent = '--';
        tempDiv.appendChild(tempLabel);
        tempDiv.appendChild(tempValue);
        sensorDiv.appendChild(tempDiv);
        
        // Обновляем температуру при загрузке
        if (sensor.currentTemp !== undefined && sensor.currentTemp !== null) {
            tempValue.textContent = (sensor.currentTemp + (sensor.correction || 0)).toFixed(1) + '°C';
        }
        
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
        
        // Добавляем выбор режима работы
        const modeDiv = document.createElement('div');
        modeDiv.className = 'form-group';
        const modeLabel = document.createElement('label');
        modeLabel.setAttribute('for', `sensor-mode-${index}`);
        modeLabel.textContent = 'Режим работы';
        const modeSelect = document.createElement('select');
        modeSelect.id = `sensor-mode-${index}`;
        modeSelect.style.cssText = 'width: 100%; padding: 8px; border: 1px solid #ddd; border-radius: 4px; font-size: 1rem;';
        
        const modeOptions = [
            { value: 'monitoring', text: 'Мониторинг' },
            { value: 'alert', text: 'Оповещение' },
            { value: 'stabilization', text: 'Стабилизация' }
        ];
        
        modeOptions.forEach(option => {
            const optionEl = document.createElement('option');
            optionEl.value = option.value;
            optionEl.textContent = option.text;
            if (sensor.mode === option.value) {
                optionEl.selected = true;
            }
            modeSelect.appendChild(optionEl);
        });
        
        modeSelect.onchange = () => {
            sensors[index].mode = modeSelect.value;
        };
        
        modeDiv.appendChild(modeLabel);
        modeDiv.appendChild(modeSelect);
        sensorDiv.appendChild(modeDiv);
        
        container.appendChild(sensorDiv);
    });
}

// Функция обновления списка термометров (автоматическое обнаружение)
async function refreshSensors() {
    showMessage('Обновление списка термометров...', 'success');
    await loadSensors();
    showMessage('Список термометров обновлен', 'success');
}

// Функция removeSensor удалена - термометры определяются автоматически

// Функция отправки тестового сообщения в Telegram
async function sendTelegramTest() {
    try {
        const response = await fetch('/api/telegram/test', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            }
        });
        
        const result = await response.json();
        
        if (response.ok && result.status === 'ok') {
            showMessage('Тестовое сообщение отправлено в Telegram', 'success');
        } else {
            showMessage('Ошибка отправки тестового сообщения: ' + (result.message || 'Неизвестная ошибка'), 'error');
        }
    } catch (error) {
        console.error('Error sending Telegram test:', error);
        showMessage('Ошибка отправки тестового сообщения в Telegram', 'error');
    }
}

// Функция переключения MQTT
async function toggleMqttEnabled() {
    const mqttEnabled = document.getElementById('mqtt-enabled').checked;
    
    if (!mqttEnabled) {
        // Отключаем MQTT принудительно
        try {
            const response = await fetch('/api/mqtt/disable', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                }
            });
            
            if (response.ok) {
                showMessage('MQTT отключен', 'success');
                updateServiceStatus();
            } else {
                showMessage('Ошибка отключения MQTT', 'error');
                document.getElementById('mqtt-enabled').checked = true; // Возвращаем чекбокс
            }
        } catch (error) {
            console.error('Error disabling MQTT:', error);
            showMessage('Ошибка отключения MQTT', 'error');
            document.getElementById('mqtt-enabled').checked = true; // Возвращаем чекбокс
        }
    } else {
        // Включаем MQTT - нужно сохранить настройки
        showMessage('Сохраните настройки для включения MQTT', 'success');
    }
}

// Функция отправки тестового сообщения в MQTT
async function sendMqttTest() {
    try {
        const response = await fetch('/api/mqtt/test', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            }
        });
        
        const result = await response.json();
        
        if (response.ok && result.status === 'ok') {
            showMessage('Тестовое сообщение отправлено в MQTT', 'success');
        } else {
            showMessage('Ошибка отправки тестового сообщения: ' + (result.message || 'Неизвестная ошибка'), 'error');
        }
    } catch (error) {
        console.error('Error sending MQTT test:', error);
        showMessage('Ошибка отправки тестового сообщения в MQTT', 'error');
    }
}

// Функция обновления температуры для всех термометров
async function updateSensorsTemperature() {
    try {
        const response = await fetch('/api/sensors');
        const data = await response.json();
        
        if (data.sensors && Array.isArray(data.sensors)) {
            // Обновляем массив sensors, сохраняя порядок и настройки
            data.sensors.forEach((sensor, dataIndex) => {
                // Ищем по адресу (приоритет), затем по индексу
                let index = -1;
                if (sensor.address) {
                    index = sensors.findIndex(s => s.address === sensor.address);
                }
                if (index < 0 && sensor.index !== undefined) {
                    index = sensors.findIndex(s => s.index === sensor.index);
                }
                
                if (index >= 0) {
                    // Обновляем только температуру, сохраняя остальные настройки
                    sensors[index].currentTemp = sensor.currentTemp;
                    
                    // Обновляем отображение температуры в настройках
                    const tempElement = document.getElementById(`sensor-temp-${index}`);
                    if (tempElement && sensor.currentTemp !== undefined && sensor.currentTemp !== null) {
                        const correctedTemp = sensor.currentTemp + (sensors[index].correction || 0);
                        tempElement.textContent = correctedTemp.toFixed(1) + '°C';
                    }
                }
            });
        }
    } catch (error) {
        console.error('Error updating sensors temperature:', error);
    }
}

// Загрузить настройки при загрузке страницы
document.addEventListener('DOMContentLoaded', () => {
    loadSettings();
    // Загружаем термометры
    loadSensors();
    updateServiceStatus();
    
    // Обновляем температуру каждые 5 секунд
    setInterval(updateSensorsTemperature, 5000);
    // Первое обновление через 1 секунду
    setTimeout(updateSensorsTemperature, 1000);
});
