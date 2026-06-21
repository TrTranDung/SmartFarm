#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <ArduinoJson.h>

const char* ssid = "KHTN-Lab";
const char* password = "nhien10a6";

const String FIREBASE_URL =
"https://starm-vdk-default-rtdb.asia-southeast1.firebasedatabase.app";

#define ESP_RX 16
#define ESP_TX 17
#define UART_BAUD 115200

/* ================= DEBUG ================= */
#define DEBUG_SENSOR 0
#define DEBUG_ACK    0
#define DEBUG_TX     0
#define DEBUG_HTTP   0

/* ================= BINARY PROTOCOL ================= */
#define CMD_START     0xAA
#define RESP_START    0x55

#define CMD_MODE      0x01
#define CMD_SERVO     0x02
#define CMD_PUMP      0x03
#define CMD_LIGHT     0x04
#define CMD_SET_SUN   0x10
#define CMD_SET_RAIN  0x11

#define RESP_SENSOR   0x80
#define RESP_ACK      0x81

#define MODE_AUTO_BIN    0
#define MODE_MANUAL_BIN  1

#define ACK_OK             0
#define ACK_IGNORED_AUTO   1
#define ACK_RANGE_ERROR    2
#define ACK_UNKNOWN        3
#define ACK_QUEUE_FULL     4

#define STATUS_NORMAL  0
#define STATUS_HOT_DRY 1
#define STATUS_RAIN    2

/* ================= STATE ================= */
int lastServo = -1;
int lastPump  = -1;
int lastLight = -1;

String lastMode = "";
String lastTimerAction = "";

int sunTemp = 32, sunHum = 60, sunSoil = 20;
int rainTemp = 30, rainHum = 70, rainSoil = 70;

int lastSettingsVersion = -1;

int curSoil = 0;
int curTemp = 0;
int curHum = 0;
int curStatusCode = STATUS_NORMAL;
String curStatus = "NORMAL";
int curMode = MODE_AUTO_BIN;
int curPump = 0;
int curServo = 0;
int curLight = 0;

uint8_t rxFrame[11];
uint8_t rxIndex = 0;
bool receivingFrame = false;

bool newSensorData = false;
bool hasSensorData = false;

/* ================= TIMING ================= */
unsigned long lastControlMillis = 0;
unsigned long lastSettingsMillis = 0;
unsigned long lastSensorUploadMillis = 0;
unsigned long lastModeResendMillis = 0;
unsigned long lastWifiCheckMillis = 0;

const unsigned long CONTROL_INTERVAL       = 200;
const unsigned long SETTINGS_INTERVAL      = 10000;
const unsigned long MIN_UPLOAD_INTERVAL    = 500;
const unsigned long MODE_RESEND_INTERVAL   = 10000;
const unsigned long WIFI_CHECK_INTERVAL    = 5000;

/* ================= HELPER ================= */
int clampInt(int value, int minValue, int maxValue)
{
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;

    return value;
}

uint8_t clampByte(int value)
{
    if (value < 0) return 0;
    if (value > 255) return 255;

    return (uint8_t)value;
}

String statusTextFromCode(int code)
{
    if (code == STATUS_HOT_DRY) return "HOT_DRY";
    if (code == STATUS_RAIN) return "RAIN";

    return "NORMAL";
}

String modeTextFromCode(int code)
{
    if (code == MODE_MANUAL_BIN) return "MANUAL";

    return "AUTO";
}

/* ================= FIREBASE ================= */
String firebaseGET(String path)
{
    if (WiFi.status() != WL_CONNECTED) return "";

    HTTPClient http;
    String url = FIREBASE_URL + path;

    http.begin(url);
    http.setTimeout(900);

    int code = http.GET();

    if (code != 200)
    {
#if DEBUG_HTTP
        Serial.print("GET error ");
        Serial.print(path);
        Serial.print(" = ");
        Serial.println(code);
#endif
        http.end();
        return "";
    }

    String payload = http.getString();
    payload.trim();

    http.end();
    return payload;
}

void firebasePUT(String path, String json)
{
    if (WiFi.status() != WL_CONNECTED) return;

    HTTPClient http;
    String url = FIREBASE_URL + path;

    http.begin(url);
    http.setTimeout(900);
    http.addHeader("Content-Type", "application/json");

    int code = http.PUT(json);

#if DEBUG_HTTP
    Serial.print("PUT ");
    Serial.print(path);
    Serial.print(" = ");
    Serial.println(code);
#endif

    http.end();
}

void firebasePATCH(String path, String json)
{
    if (WiFi.status() != WL_CONNECTED) return;

    HTTPClient http;
    String url = FIREBASE_URL + path;

    http.begin(url);
    http.setTimeout(900);
    http.addHeader("Content-Type", "application/json");

    int code = http.PATCH(json);

#if DEBUG_HTTP
    Serial.print("PATCH ");
    Serial.print(path);
    Serial.print(" = ");
    Serial.println(code);
#endif

    http.end();
}

/* ================= WIFI ================= */
void checkWiFi()
{
    if (millis() - lastWifiCheckMillis < WIFI_CHECK_INTERVAL) return;
    lastWifiCheckMillis = millis();

    if (WiFi.status() == WL_CONNECTED) return;

    Serial.println("WiFi disconnected. Reconnecting...");

    WiFi.disconnect();
    WiFi.begin(ssid, password);

    unsigned long start = millis();

    while (WiFi.status() != WL_CONNECTED && millis() - start < 3000)
    {
        delay(100);
        Serial.print(".");
    }

    Serial.println();

    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.println("WiFi reconnected!");
    }
    else
    {
        Serial.println("WiFi reconnect failed");
    }
}

/* ================= TIME ================= */
String getCurrentTimeHHMM()
{
    struct tm timeinfo;

    if (!getLocalTime(&timeinfo))
    {
        return "";
    }

    char timeStr[6];
    sprintf(timeStr, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);

    return String(timeStr);
}

/* ================= BINARY UART SEND ================= */
uint8_t cmdChecksum(uint8_t cmd, uint8_t p1, uint8_t p2, uint8_t p3)
{
    return (uint8_t)(CMD_START ^ cmd ^ p1 ^ p2 ^ p3);
}

uint8_t respChecksum(uint8_t frame[11])
{
    uint8_t cs = 0;

    for (int i = 0; i < 10; i++)
    {
        cs ^= frame[i];
    }

    return cs;
}

void sendBinaryFrame(uint8_t cmd, uint8_t p1, uint8_t p2, uint8_t p3)
{
    uint8_t checksum = cmdChecksum(cmd, p1, p2, p3);

    Serial2.write(CMD_START);
    Serial2.write(cmd);
    Serial2.write(p1);
    Serial2.write(p2);
    Serial2.write(p3);
    Serial2.write(checksum);

#if DEBUG_TX
    Serial.print("BIN TX: AA ");

    if (cmd < 16) Serial.print("0");
    Serial.print(cmd, HEX);
    Serial.print(" ");

    if (p1 < 16) Serial.print("0");
    Serial.print(p1, HEX);
    Serial.print(" ");

    if (p2 < 16) Serial.print("0");
    Serial.print(p2, HEX);
    Serial.print(" ");

    if (p3 < 16) Serial.print("0");
    Serial.print(p3, HEX);
    Serial.print(" ");

    if (checksum < 16) Serial.print("0");
    Serial.println(checksum, HEX);
#endif
}

void sendModeToAtmega(String mode)
{
    if (mode == "MANUAL")
    {
        sendBinaryFrame(CMD_MODE, MODE_MANUAL_BIN, 0, 0);
        Serial.println("SEND MODE MANUAL");
    }
    else if (mode == "AUTO")
    {
        sendBinaryFrame(CMD_MODE, MODE_AUTO_BIN, 0, 0);
        Serial.println("SEND MODE AUTO");
    }
}

void sendDeviceToAtmega(uint8_t cmd, uint8_t state, const char* label)
{
    sendBinaryFrame(cmd, state, 0, 0);
    Serial.println(label);
    delay(20);
}

void sendSettingsToAtmega()
{
    Serial.println("SEND SETTINGS TO ATMEGA BY BINARY");

    sunTemp = clampInt(sunTemp, 0, 80);
    sunHum  = clampInt(sunHum, 0, 100);
    sunSoil = clampInt(sunSoil, 0, 100);

    rainTemp = clampInt(rainTemp, 0, 80);
    rainHum  = clampInt(rainHum, 0, 100);
    rainSoil = clampInt(rainSoil, 0, 100);

    sendBinaryFrame(
        CMD_SET_SUN,
        clampByte(sunTemp),
        clampByte(sunHum),
        clampByte(sunSoil)
    );

    delay(5);

    sendBinaryFrame(
        CMD_SET_RAIN,
        clampByte(rainTemp),
        clampByte(rainHum),
        clampByte(rainSoil)
    );
}

/* ================= SETTINGS ================= */
void checkSettingsUpdateFromFirebase()
{
    String settingsJson = firebaseGET("/smartfarm/settings.json");

    if (settingsJson.length() == 0 || settingsJson == "null") return;

    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, settingsJson);

    if (error)
    {
        Serial.println("Parse settings error");
        return;
    }

    int version = doc["version"] | 0;

    if (version == lastSettingsVersion) return;

    lastSettingsVersion = version;

    sunTemp = doc["sunny"]["temp"] | sunTemp;
    sunHum  = doc["sunny"]["humidity"] | sunHum;
    sunSoil = doc["sunny"]["soil"] | sunSoil;

    rainTemp = doc["rain"]["temp"] | rainTemp;
    rainHum  = doc["rain"]["humidity"] | rainHum;
    rainSoil = doc["rain"]["soil"] | rainSoil;

    sunTemp = clampInt(sunTemp, 0, 80);
    sunHum  = clampInt(sunHum, 0, 100);
    sunSoil = clampInt(sunSoil, 0, 100);

    rainTemp = clampInt(rainTemp, 0, 80);
    rainHum  = clampInt(rainHum, 0, 100);
    rainSoil = clampInt(rainSoil, 0, 100);

    Serial.print("SETTINGS VERSION = ");
    Serial.println(version);

    Serial.print("SUN: ");
    Serial.print(sunTemp);
    Serial.print(",");
    Serial.print(sunHum);
    Serial.print(",");
    Serial.println(sunSoil);

    Serial.print("RAIN: ");
    Serial.print(rainTemp);
    Serial.print(",");
    Serial.print(rainHum);
    Serial.print(",");
    Serial.println(rainSoil);

    sendSettingsToAtmega();
}

/* ================= SENSOR FROM ATMEGA ================= */
void uploadSensorBinary()
{
    String modeText = modeTextFromCode(curMode);

    String json =
        "{"
        "\"soil\":" + String(curSoil) + "," +
        "\"temperature\":" + String(curTemp) + "," +
        "\"humidity\":" + String(curHum) + "," +
        "\"status\":\"" + curStatus + "\"," +
        "\"statusCode\":" + String(curStatusCode) + "," +
        "\"mode\":\"" + modeText + "\"," +
        "\"modeReal\":\"" + modeText + "\"," +
        "\"pump\":" + String(curPump) + "," +
        "\"pumpReal\":" + String(curPump) + "," +
        "\"servo\":" + String(curServo) + "," +
        "\"servoReal\":" + String(curServo) + "," +
        "\"light\":" + String(curLight) + "," +
        "\"lightReal\":" + String(curLight) +
        "}";

    firebasePATCH("/smartfarm/sensor.json", json);
}

void handleSensorFrame(uint8_t frame[11])
{
    curSoil = frame[2];
    curTemp = frame[3];
    curHum  = frame[4];
    curStatusCode = frame[5];
    curStatus = statusTextFromCode(curStatusCode);
    curMode = frame[6];
    curPump = frame[7];
    curServo = frame[8];
    curLight = frame[9];

    hasSensorData = true;
    newSensorData = true;

#if DEBUG_SENSOR
    Serial.print("BIN RX SENSOR -> Soil:");
    Serial.print(curSoil);
    Serial.print(" Temp:");
    Serial.print(curTemp);
    Serial.print(" H:");
    Serial.print(curHum);
    Serial.print(" Status:");
    Serial.print(curStatus);
    Serial.print(" Mode:");
    Serial.print(modeTextFromCode(curMode));
    Serial.print(" Pump:");
    Serial.print(curPump);
    Serial.print(" Servo:");
    Serial.print(curServo);
    Serial.print(" Light:");
    Serial.println(curLight);
#endif
}

void handleAckFrame(uint8_t frame[11])
{
#if DEBUG_ACK
    uint8_t cmd = frame[2];
    uint8_t ack = frame[3];

    Serial.print("BIN RX ACK -> CMD:0x");

    if (cmd < 16) Serial.print("0");
    Serial.print(cmd, HEX);
    Serial.print(" ACK:");

    if (ack == ACK_OK)
    {
        Serial.println("OK");
    }
    else if (ack == ACK_IGNORED_AUTO)
    {
        Serial.println("IGNORED_AUTO");
    }
    else if (ack == ACK_RANGE_ERROR)
    {
        Serial.println("RANGE_ERROR");
    }
    else if (ack == ACK_UNKNOWN)
    {
        Serial.println("UNKNOWN");
    }
    else if (ack == ACK_QUEUE_FULL)
    {
        Serial.println("QUEUE_FULL");
    }
    else
    {
        Serial.println(ack);
    }
#endif
}

void processResponseFrame(uint8_t frame[11])
{
    if (respChecksum(frame) != frame[10])
    {
        Serial.println("BIN RX ERROR: CHECKSUM");
        return;
    }

    uint8_t type = frame[1];

    if (type == RESP_SENSOR)
    {
        handleSensorFrame(frame);
    }
    else if (type == RESP_ACK)
    {
        handleAckFrame(frame);
    }
    else
    {
        Serial.print("BIN RX UNKNOWN TYPE: 0x");
        Serial.println(type, HEX);
    }
}

void readBinaryFromAtmega()
{
    while (Serial2.available())
    {
        uint8_t b = Serial2.read();

        if (!receivingFrame)
        {
            if (b == RESP_START)
            {
                receivingFrame = true;
                rxIndex = 0;
                rxFrame[rxIndex++] = b;
            }
        }
        else
        {
            rxFrame[rxIndex++] = b;

            if (rxIndex >= 11)
            {
                processResponseFrame(rxFrame);
                receivingFrame = false;
                rxIndex = 0;
            }
        }
    }
}

/* ================= LIGHT TIMER ================= */
void handleLightTimerFromControl(JsonVariant lightTimerVar)
{
    if (lightTimerVar.isNull())
    {
        lastTimerAction = "";
        return;
    }

    bool enabled = lightTimerVar["enabled"] | false;
    String onTime = lightTimerVar["onTime"] | "";
    String offTime = lightTimerVar["offTime"] | "";

    if (!enabled || onTime.length() == 0 || offTime.length() == 0)
    {
        lastTimerAction = "";
        return;
    }

    String currentTime = getCurrentTimeHHMM();

    if (currentTime.length() == 0) return;

    if (currentTime == onTime && lastTimerAction != "ON-" + currentTime)
    {
        lastTimerAction = "ON-" + currentTime;
        lastLight = 1;

        sendDeviceToAtmega(CMD_LIGHT, 1, "SEND LIGHT ON BY TIMER");
        firebasePUT("/smartfarm/control/light.json", "1");
    }
    else if (currentTime == offTime && lastTimerAction != "OFF-" + currentTime)
    {
        lastTimerAction = "OFF-" + currentTime;
        lastLight = 0;

        sendDeviceToAtmega(CMD_LIGHT, 0, "SEND LIGHT OFF BY TIMER");
        firebasePUT("/smartfarm/control/light.json", "0");
    }

    if (currentTime != onTime && currentTime != offTime)
    {
        lastTimerAction = "";
    }
}

/* ================= CONTROL ================= */
void readControlFromFirebase()
{
    String controlJson = firebaseGET("/smartfarm/control.json");

    if (controlJson.length() == 0 || controlJson == "null") return;

    DynamicJsonDocument doc(1536);
    DeserializationError error = deserializeJson(doc, controlJson);

    if (error)
    {
        Serial.println("Parse control error");
        return;
    }

    String mode = doc["mode"] | "";

    /*
       Biến này dùng để chặn gửi servo/pump ngay trong vòng lặp
       vừa chuyển mode.
       Nếu không chặn, ESP có thể gửi lại lệnh cũ từ Firebase.
    */
    bool skipManualDeviceControl = false;

    if (mode == "AUTO" || mode == "MANUAL")
    {
        if (mode != lastMode)
        {
            String previousMode = lastMode;

            lastMode = mode;
            lastModeResendMillis = millis();

            Serial.print("Mode = ");
            Serial.println(mode);

            sendModeToAtmega(mode);

            /*
               Trường hợp quan trọng:
               AUTO -> MANUAL

               Khi còn AUTO, servo/pump do ATmega tự điều khiển.
               Firebase control/servo có thể vẫn là lệnh cũ.

               Vì vậy khi sang MANUAL:
               - Không gửi lệnh cũ xuống ATmega.
               - Đồng bộ control/servo và control/pump theo trạng thái thật.
            */
            if (previousMode == "AUTO" && mode == "MANUAL")
            {
                if (hasSensorData)
                {
                    lastServo = curServo;
                    lastPump  = curPump;

                    firebasePUT(
                        "/smartfarm/control/servo.json",
                        String(curServo)
                    );

                    firebasePUT(
                        "/smartfarm/control/pump.json",
                        String(curPump)
                    );

                    Serial.println("SYNC REAL STATE TO MANUAL CONTROL");
                }
                else
                {
                    lastServo = doc["servo"] | 0;
                    lastPump  = doc["pump"]  | 0;
                }

                /*
                   Không xử lý lệnh servo/pump trong cùng chu kỳ này,
                   vì doc đang là dữ liệu cũ vừa GET trước khi PUT.
                */
                skipManualDeviceControl = true;
            }

            /*
               Trường hợp ESP khởi động lên và Firebase đang là MANUAL:
               chỉ ghi nhận trạng thái control hiện tại,
               không gửi lại lệnh ngay lập tức.
            */
            else if (mode == "MANUAL")
            {
                lastServo = doc["servo"] | 0;
                lastPump  = doc["pump"]  | 0;

                skipManualDeviceControl = true;
            }

            /*
               Khi vào AUTO:
               reset lastServo/lastPump để khi quay lại MANUAL
               có thể xử lý lại đúng.
            */
            else
            {
                lastServo = -1;
                lastPump  = -1;
            }
        }
    }

    bool isManual = (lastMode == "MANUAL");

    if (isManual && !skipManualDeviceControl)
    {
        int servo = doc["servo"] | 0;
        int pump  = doc["pump"]  | 0;

        /*
           Quy ước hiện tại:
           servo = 0 -> MỞ
           servo = 1 -> ĐÓNG
        */
        if (servo != lastServo)
        {
            lastServo = servo;

            if (servo == 0)
            {
                sendDeviceToAtmega(
                    CMD_SERVO,
                    0,
                    "SEND SERVO OPEN"
                );
            }
            else
            {
                sendDeviceToAtmega(
                    CMD_SERVO,
                    1,
                    "SEND SERVO CLOSE"
                );
            }
        }

        if (pump != lastPump)
        {
            lastPump = pump;

            if (pump == 1)
            {
                sendDeviceToAtmega(
                    CMD_PUMP,
                    1,
                    "SEND PUMP ON"
                );
            }
            else
            {
                sendDeviceToAtmega(
                    CMD_PUMP,
                    0,
                    "SEND PUMP OFF"
                );
            }
        }
    }

    JsonVariant lightTimerVar = doc["lightTimer"];
    bool timerEnabled = lightTimerVar["enabled"] | false;

    if (!timerEnabled)
    {
        int light = doc["light"] | 0;

        if (light != lastLight)
        {
            lastLight = light;

            if (light == 1)
            {
                sendDeviceToAtmega(
                    CMD_LIGHT,
                    1,
                    "SEND LIGHT ON"
                );
            }
            else
            {
                sendDeviceToAtmega(
                    CMD_LIGHT,
                    0,
                    "SEND LIGHT OFF"
                );
            }
        }
    }

    handleLightTimerFromControl(lightTimerVar);
}
/* ================= SETUP ================= */
void setup()
{
    Serial.begin(115200);
    Serial2.begin(UART_BAUD, SERIAL_8N1, ESP_RX, ESP_TX);

    Serial.println("Connecting WiFi...");
    WiFi.begin(ssid, password);

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(300);
        Serial.print(".");
    }

    Serial.println();
    Serial.println("WiFi Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");

    lastMode = "";
    lastServo = -1;
    lastPump = -1;
    lastLight = -1;

    Serial.println("UART Full Binary Optimized Ready");

    delay(500);

    readBinaryFromAtmega();
    checkSettingsUpdateFromFirebase();
    readBinaryFromAtmega();
    readControlFromFirebase();
    readBinaryFromAtmega();
}

/* ================= LOOP ================= */
void loop()
{
    checkWiFi();

    readBinaryFromAtmega();

    unsigned long now = millis();

    if (now - lastControlMillis >= CONTROL_INTERVAL)
    {
        lastControlMillis = now;
        readBinaryFromAtmega();
        readControlFromFirebase();
        readBinaryFromAtmega();
    }

    if (now - lastSettingsMillis >= SETTINGS_INTERVAL)
    {
        lastSettingsMillis = now;
        readBinaryFromAtmega();
        checkSettingsUpdateFromFirebase();
        readBinaryFromAtmega();
    }

    if (newSensorData && now - lastSensorUploadMillis >= MIN_UPLOAD_INTERVAL)
    {
        newSensorData = false;
        lastSensorUploadMillis = now;

        readBinaryFromAtmega();
        uploadSensorBinary();
        readBinaryFromAtmega();
    }
}
