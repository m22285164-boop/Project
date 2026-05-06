# Smart Home — Raspberry Pi
## Зависимости, сборка и запуск

### 1. Установка зависимостей

```bash
# pigpio — GPIO-библиотека
sudo apt update
sudo apt install -y pigpio libpigpio-dev

# cpp-httplib — header-only, скачиваем один файл
wget -O httplib.h \
  https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h
```

### 2. Структура проекта

```
smart_home/
├── main.cpp       ← C++ сервер
├── httplib.h      ← скаченный выше header
└── index.html     ← веб-интерфейс
```

### 3. Компиляция

```bash
g++ -std=c++17 -O2 -o smart_home main.cpp -lpigpio -lpthread
```

### 4. Запуск

```bash
# pigpio требует root
sudo ./smart_home
```

Открыть в браузере планшета:
```
http://<IP-адрес-Raspberry-Pi>:8080
```

Узнать IP: `hostname -I`

### 5. Автозапуск (systemd)

```bash
sudo nano /etc/systemd/system/smarthome.service
```

```ini
[Unit]
Description=Smart Home Server
After=network.target

[Service]
ExecStart=/home/pi/smart_home/smart_home
WorkingDirectory=/home/pi/smart_home
Restart=always
User=root

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl enable smarthome
sudo systemctl start smarthome
```

---

### GPIO-схема подключения

| Компонент           | GPIO | Pin (физ.) |
|---------------------|------|------------|
| DHT22 (данные)      | 17   | 11         |
| LDR-модуль (D0)     | 27   | 13         |
| Реле (шторы)        | 22   | 15         |
| LED  (лампочка)     | 18   | 12         |

> Питание DHT22: 3.3 В + подтягивающий резистор 4.7 kΩ между DATA и VCC.
> Реле-модуль: обычно питается от 5 В, управляется 3.3 В GPIO — уточните под свой модуль.

### REST API

| Метод | Путь                   | Описание                        |
|-------|------------------------|---------------------------------|
| GET   | `/`                    | Возвращает index.html           |
| GET   | `/api/sensors`         | JSON: temp, light, curtains, lamp |
| POST  | `/api/toggle/curtains` | Переключить реле (шторы)        |
| POST  | `/api/toggle/lamp`     | Переключить LED (лампочку)      |
| POST  | `/api/curtains`        | Тело: `{"state":true}`          |
| POST  | `/api/lamp`            | Тело: `{"state":true}`          |
