# Скрипт для загрузки прошивки с автоматическим освобождением COM порта
param(
    [string]$Port = "COM12"
)

Write-Host "========================================="
Write-Host "Загрузка прошивки ESP32"
Write-Host "========================================="

# Функция для освобождения COM порта
function Release-ComPort {
    param([string]$PortName)
    
    Write-Host "Проверка порта $PortName..."
    
    # Пытаемся найти процессы, использующие порт
    $processes = Get-Process | Where-Object {
        $_.ProcessName -like "*platformio*" -or 
        $_.ProcessName -like "*python*" -or 
        $_.ProcessName -like "*esptool*" -or
        $_.ProcessName -like "*putty*" -or
        $_.ProcessName -like "*arduino*" -or
        $_.ProcessName -like "*serial*" -or
        $_.ProcessName -like "*monitor*"
    } | Where-Object {
        try {
            $_.Modules | Where-Object { $_.FileName -like "*$PortName*" }
        } catch {
            $false
        }
    }
    
    if ($processes) {
        Write-Host "Найдены процессы, использующие порт:"
        $processes | ForEach-Object {
            Write-Host "  - $($_.ProcessName) (PID: $($_.Id))"
        }
        
        # Закрываем процессы
        $processes | ForEach-Object {
            try {
                Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
                Write-Host "  Процесс $($_.ProcessName) (PID: $($_.Id)) закрыт"
            } catch {
                Write-Host "  Не удалось закрыть процесс $($_.ProcessName) (PID: $($_.Id))"
            }
        }
        
        Start-Sleep -Seconds 2
        Write-Host "Порт должен быть свободен."
    } else {
        Write-Host "Порт свободен."
    }
}

# Освобождаем порт
Release-ComPort -PortName $Port

# Загружаем прошивку
Write-Host "Загрузка прошивки..."
& "C:\Users\grif_\.platformio\penv\Scripts\platformio.exe" run --target upload

if ($LASTEXITCODE -ne 0) {
    Write-Host "Ошибка загрузки. Попытка освободить порт и повторить..."
    Release-ComPort -PortName $Port
    Start-Sleep -Seconds 3
    Write-Host "Повторная попытка загрузки..."
    & "C:\Users\grif_\.platformio\penv\Scripts\platformio.exe" run --target upload
}
