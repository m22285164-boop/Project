/*
 * Smart Home Server — Raspberry Pi
 * Library:  pigpio (GPIO), cpp-httplib (HTTP server, header-only)
 *
 * GPIO pinout:
 *   GPIO 17 — DHT22 (temperature sensor, 1-wire-like emulation via pigpio bit-bang)
 *   GPIO 27 — LDR / light sensor (digital out from comparator module)
 *   GPIO 22 — Relay module  (curtain motor imitation), active HIGH
 *   GPIO 18 — LED           (lamp imitation),          active HIGH
 *
 * Build:
 *   g++ -std=c++17 -O2 -o smart_home main.cpp -lpigpio -lpthread
 *
 * Run (requires root for pigpio):
 *   sudo ./smart_home
 */

#include "httplib.h"      // cpp-httplib (place httplib.h next to main.cpp)
#include <pigpio.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

// ─── GPIO pin definitions ────────────────────────────────────────────────────
constexpr int PIN_DHT22  = 17;   // temperature sensor
constexpr int PIN_LIGHT  = 27;   // LDR digital module (LOW = dark, HIGH = bright)
constexpr int PIN_RELAY  = 22;   // relay → curtains
constexpr int PIN_LED    = 18;   // LED  → lamp

// ─── Shared state ────────────────────────────────────────────────────────────
struct SensorData {
    float    temperature = 0.0f;
    bool     lightOn     = false;
    std::string lastUpdated;
};

std::mutex          g_mutex;
SensorData          g_sensors;
std::atomic<bool>   g_relayState{false};  // curtains open=true / closed=false
std::atomic<bool>   g_ledState{false};    // lamp on=true / off=false

// ─── DHT22 bit-bang read via pigpio ──────────────────────────────────────────
// Returns true on success and fills temp (°C) and humidity (%).
// pigpio provides gpioSetPullUpDown / gpioRead / gpioDelay etc.
bool readDHT22(int pin, float &tempC, float &humidity) {
    uint8_t data[5] = {0};

    // Send start signal: pull low for 18 ms, then release
    gpioSetMode(pin, PI_OUTPUT);
    gpioWrite(pin, 0);
    gpioDelay(18000);  // 18 ms
    gpioWrite(pin, 1);
    gpioDelay(40);
    gpioSetMode(pin, PI_INPUT);
    gpioSetPullUpDown(pin, PI_PUD_UP);

    // Wait for sensor response (LOW then HIGH)
    int timeout = 0;
    while (gpioRead(pin) == 1) { gpioDelay(1); if (++timeout > 1000) return false; }
    timeout = 0;
    while (gpioRead(pin) == 0) { gpioDelay(1); if (++timeout > 1000) return false; }
    timeout = 0;
    while (gpioRead(pin) == 1) { gpioDelay(1); if (++timeout > 1000) return false; }

    // Read 40 bits
    for (int i = 0; i < 40; i++) {
        timeout = 0;
        while (gpioRead(pin) == 0) { gpioDelay(1); if (++timeout > 1000) return false; }
        gpioDelay(30);
        if (gpioRead(pin) == 1) {
            data[i / 8] |= (1 << (7 - (i % 8)));
        }
        timeout = 0;
        while (gpioRead(pin) == 1) { gpioDelay(1); if (++timeout > 1000) return false; }
    }

    // Checksum
    if (data[4] != ((data[0] + data[1] + data[2] + data[3]) & 0xFF)) return false;

    humidity = ((data[0] << 8) | data[1]) * 0.1f;
    int16_t raw = ((data[2] & 0x7F) << 8) | data[3];
    if (data[2] & 0x80) raw = -raw;
    tempC = raw * 0.1f;
    return true;
}

// ─── Sensor polling thread ────────────────────────────────────────────────────
void sensorThread() {
    while (true) {
        float tempC = 0.0f, humidity = 0.0f;
        bool  ok = readDHT22(PIN_DHT22, tempC, humidity);

        bool lightDetected = (gpioRead(PIN_LIGHT) == PI_HIGH);

        {
            std::lock_guard<std::mutex> lk(g_mutex);
            if (ok) g_sensors.temperature = tempC;
            // else keep last valid value
            g_sensors.lightOn = lightDetected;

            // simple timestamp
            auto now = std::chrono::system_clock::now();
            std::time_t t = std::chrono::system_clock::to_time_t(now);
            char buf[32];
            std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
            g_sensors.lastUpdated = buf;
        }

        std::this_thread::sleep_for(std::chrono::seconds(3));
    }
}

// ─── JSON helpers ─────────────────────────────────────────────────────────────
std::string sensorJson() {
    std::lock_guard<std::mutex> lk(g_mutex);
    std::ostringstream ss;
    ss << "{"
       << "\"temperature\":" << g_sensors.temperature << ","
       << "\"light\":"       << (g_sensors.lightOn ? "true" : "false") << ","
       << "\"curtains\":"    << (g_relayState.load() ? "true" : "false") << ","
       << "\"lamp\":"        << (g_ledState.load()   ? "true" : "false") << ","
       << "\"updated\":\""   << g_sensors.lastUpdated << "\""
       << "}";
    return ss.str();
}

// ─── Embedded HTML page (loaded from file or inline) ─────────────────────────
// We serve /index.html from the filesystem (place index.html next to binary).
// Fall-back inline page is a minimal redirect so the server still works.
static const char* FALLBACK_HTML = R"(<!DOCTYPE html><html><body>
<p>Place <b>index.html</b> next to the <b>smart_home</b> binary.</p>
</body></html>)";

// ─── main ────────────────────────────────────────────────────────────────────
int main() {
    // Init pigpio
    if (gpioInitialise() < 0) {
        std::cerr << "[ERROR] gpioInitialise() failed. Run as root?\n";
        return 1;
    }

    // Configure pins
    gpioSetMode(PIN_DHT22, PI_INPUT);
    gpioSetPullUpDown(PIN_DHT22, PI_PUD_UP);

    gpioSetMode(PIN_LIGHT, PI_INPUT);
    gpioSetPullUpDown(PIN_LIGHT, PI_PUD_DOWN);

    gpioSetMode(PIN_RELAY, PI_OUTPUT);
    gpioWrite(PIN_RELAY, 0);

    gpioSetMode(PIN_LED, PI_OUTPUT);
    gpioWrite(PIN_LED, 0);

    std::cout << "[INFO] GPIO initialised.\n";

    // Start sensor thread
    std::thread(sensorThread).detach();

    // ── HTTP server ──────────────────────────────────────────────────────────
    httplib::Server svr;

    // Serve index.html
    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        std::ifstream f("index.html");
        if (f.good()) {
            std::string html((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
            res.set_content(html, "text/html");
        } else {
            res.set_content(FALLBACK_HTML, "text/html");
        }
    });

    // GET /api/sensors  → JSON with all sensor readings + device states
    svr.Get("/api/sensors", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_content(sensorJson(), "application/json");
    });

    // POST /api/curtains  body: {"state": true|false}
    svr.Post("/api/curtains", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        bool newState = (req.body.find("true") != std::string::npos);
        g_relayState.store(newState);
        gpioWrite(PIN_RELAY, newState ? 1 : 0);
        std::cout << "[CMD] Curtains " << (newState ? "OPEN" : "CLOSED") << "\n";
        res.set_content("{\"ok\":true}", "application/json");
    });

    // POST /api/lamp  body: {"state": true|false}
    svr.Post("/api/lamp", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        bool newState = (req.body.find("true") != std::string::npos);
        g_ledState.store(newState);
        gpioWrite(PIN_LED, newState ? 1 : 0);
        std::cout << "[CMD] Lamp " << (newState ? "ON" : "OFF") << "\n";
        res.set_content("{\"ok\":true}", "application/json");
    });

    // POST /api/toggle/curtains  (convenience toggle)
    svr.Post("/api/toggle/curtains", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        bool next = !g_relayState.load();
        g_relayState.store(next);
        gpioWrite(PIN_RELAY, next ? 1 : 0);
        std::cout << "[CMD] Curtains toggled → " << (next ? "OPEN" : "CLOSED") << "\n";
        res.set_content("{\"ok\":true,\"state\":" + std::string(next?"true":"false") + "}", "application/json");
    });

    // POST /api/toggle/lamp
    svr.Post("/api/toggle/lamp", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        bool next = !g_ledState.load();
        g_ledState.store(next);
        gpioWrite(PIN_LED, next ? 1 : 0);
        std::cout << "[CMD] Lamp toggled → " << (next ? "ON" : "OFF") << "\n";
        res.set_content("{\"ok\":true,\"state\":" + std::string(next?"true":"false") + "}", "application/json");
    });

    std::cout << "[INFO] Smart Home server running on http://0.0.0.0:8080\n";
    svr.listen("0.0.0.0", 8080);

    gpioTerminate();
    return 0;
}
