# Скрипт для освобождения COM порта перед загрузкой прошивки
param(
    [string]$Port = "COM12"
)

Write-Host "Попытка освободить порт $Port..."

# Пытаемся найти процессы, использующие порт
$processes = Get-Process | Where-Object {
    $_.ProcessName -like "*platformio*" -or 
    $_.ProcessName -like "*python*" -or 
    $_.ProcessName -like "*esptool*" -or
    $_.ProcessName -like "*putty*" -or
    $_.ProcessName -like "*arduino*" -or
    $_.ProcessName -like "*serial*"
}

if ($processes) {
    Write-Host "Найдены процессы, использующие порт:"
    $processes | ForEach-Object {
        Write-Host "  - $($_.ProcessName) (PID: $($_.Id))"
    }
    
    # Закрываем процессы (осторожно!)
    $processes | ForEach-Object {
        try {
            Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
            Write-Host "Процесс $($_.ProcessName) (PID: $($_.Id)) закрыт"
        } catch {
            Write-Host "Не удалось закрыть процесс $($_.ProcessName) (PID: $($_.Id))"
        }
    }
    
    Start-Sleep -Seconds 2
}

Write-Host "Порт должен быть свободен. Можно загружать прошивку."
