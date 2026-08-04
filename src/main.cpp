#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <TFT_eSPI.h>
#include "Adafruit_SHT31.h"
#include "SparkFun_SCD4x_Arduino_Library.h"
#include "secrets.h"

// Doi thanh 0 de chay khong can man (luc thao man ra cho do nhay nguon).
// Toan bo code ve man van nam nguyen trong file, chi khong duoc bien dich.
#define USE_TFT 1

Adafruit_SHT31 sht31 = Adafruit_SHT31();
SCD4x scd4x;

#if USE_TFT
TFT_eSPI tft = TFT_eSPI();
#endif

// ---------------- cau hinh ----------------
#define SDA_PIN 8
#define SCL_PIN 9

#define SHT_INTERVAL 10000UL    // doc SHT3X 10s/lan
#define SENSOR_RETRY 5000UL     // thu ket noi lai cam bien hong
#define CO2_STALE 120000UL      // SCD40 che do low power tra so 30s/lan,
                                // nen nguong nay phai > 30s nhieu lan
#define UPLOAD_INTERVAL 60000UL // server coi qua 3 phut khong co tin la offline
#define UPLOAD_RETRY 15000UL    // gui that bai thi thu lai sau bao lau
#define UPLOAD_MAX_TRY 3        // het so lan nay thi bo goi do, cho nhip sau

#define NTP_TZ "ICT-7"        // POSIX TZ cua VN, dau am nghia la UTC+7
#define TIME_VALID 1700000000 // moc de biet dong ho da dong bo hay chua

// ---------------- hieu chuan cuong buc (FRC) ----------------
// GPIO 0 la nut BOOT co san tren board, khong can di day gi them.
#define BTN_PIN 0
#define BTN_HOLD_MS 3000
// Khong khi ngoai troi noi thanh Ha Noi thuong 450-500 ppm, cao hon con so
// toan cau 420 vi xe co va dan cu day. Lam FRC luc sang som o ban cong.
#define FRC_REFERENCE 450

// ---------------- bang mau (RGB565) ----------------
#define C_BG 0x0862     // #0A0E13
#define C_CARD 0x10C4   // #141A22
#define C_LINE 0x2146   // #202B36
#define C_TEXT 0xE77E   // #E6EDF3
#define C_DIM 0x7C53    // #7D8B99
#define C_CYAN 0x3DFF   // #38BDF8
#define C_GREEN 0x4EF0  // #4ADE80
#define C_AMBER 0xFDE4  // #FBBF24
#define C_ORANGE 0xFC87 // #FB923C
#define C_RED 0xFB8E    // #F87171

// ---------------- toa do (man 320x240, xoay ngang) ----------------
#define HDR_H 24

#define CO2_X 6
#define CO2_Y 28
#define CO2_W 308
#define CO2_H 110
#define CO2_CX 160
#define CO2_CY 92 // tam so lon (font 8 cao 75px -> 54..130)

#define BOT_Y 144
#define BOT_H 90
#define BOT_W 150
#define TP_X 6
#define HM_X 164
#define TP_CX (TP_X + BOT_W / 2)
#define HM_CX (HM_X + BOT_W / 2)
#define BOT_CY (BOT_Y + 50) // font 6 cao 48px -> 170..218

// ---------------- trang thai cam bien ----------------
bool shtOk = false;
bool scdOk = false;
uint8_t shtAddr = 0x44;

float roomTemp = NAN, roomHum = NAN; // tu SHT3X -> mainTemp / mainHumidity
float co2Temp = NAN, co2Hum = NAN;   // tu chinh SCD40 -> co2Temp / co2Humidity
int co2 = -1;

unsigned long lastShtRead = 0;
unsigned long lastCo2Ok = 0;
unsigned long lastSensorRetry = 0;

// ---------------- trang thai mang ----------------
enum NetState
{
    NET_DOWN,  // chua co WiFi
    NET_UP,    // co WiFi nhung lan gui gan nhat that bai
    NET_SYNCED // lan gui gan nhat thanh cong
};

NetState netState = NET_DOWN;
unsigned long lastWifiTry = 0;
unsigned long lastUpload = 0;
unsigned long lastUploadTry = 0;

// Goi dang cho gui. Chup lai gia tri tai thoi diem lay mau, KHONG doc lai
// bien toan cuc luc retry — de recordedAt va so lieu luon di cung nhau.
struct Payload
{
    bool active = false;
    uint8_t tries = 0;
    time_t ts = 0; // 0 = chua co NTP -> de server tu dat gio
    bool hasCo2 = false;
    bool hasCo2Env = false;
    bool hasSht = false;
    int co2 = 0;
    float co2Temp = 0, co2Hum = 0;
    float temp = 0, hum = 0;
    int rssi = 0;
};

Payload pending;

// nho gia tri da ve de khong ve lai khi khong doi
int prevCo2 = -2;
int prevTempX10 = -9999;
int prevHum = -2;
bool prevSensorHealthy = false;
NetState prevNet = (NetState)-1;

// ---------------- danh gia muc CO2 ----------------
uint16_t co2Color(int v)
{
    if (v < 0)
        return C_DIM;
    if (v < 800)
        return C_GREEN;
    if (v < 1200)
        return C_AMBER;
    if (v < 1800)
        return C_ORANGE;
    return C_RED;
}

const char *co2Label(int v)
{
    if (v < 0)
        return "";
    if (v < 800)
        return "THOANG";
    if (v < 1200)
        return "HOI BI";
    if (v < 1800)
        return "NGOP";
    return "MO CUA!";
}

// ================= PHAN VE MAN =================
#if USE_TFT

void drawChrome()
{
    tft.fillScreen(C_BG);

    tft.fillRect(0, 0, 320, HDR_H, C_CARD);
    tft.setTextFont(2);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_CYAN, C_CARD);
    tft.drawString("KHONG KHI", 10, 3);

    tft.fillRoundRect(CO2_X, CO2_Y, CO2_W, CO2_H, 8, C_CARD);
    tft.fillRoundRect(TP_X, BOT_Y, BOT_W, BOT_H, 8, C_CARD);
    tft.fillRoundRect(HM_X, BOT_Y, BOT_W, BOT_H, 8, C_CARD);

    tft.setTextColor(C_DIM, C_CARD);
    tft.setTextDatum(TL_DATUM);
    tft.drawString("CO2  ppm", CO2_X + 12, CO2_Y + 6);
    tft.drawString("NHIET DO  C", TP_X + 12, BOT_Y + 6);
    tft.drawString("DO AM  %", HM_X + 12, BOT_Y + 6);
}

void drawNetIndicator()
{
    uint16_t col = netState == NET_SYNCED ? C_GREEN
                   : netState == NET_UP   ? C_AMBER
                                          : C_RED;
    const char *txt = netState == NET_SYNCED ? "GUI OK"
                      : netState == NET_UP   ? "LOI GUI"
                                             : "MAT WIFI";

    tft.setTextFont(2);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(col, C_CARD);
    tft.setTextPadding(80);
    tft.drawString(txt, 220, 3);
    tft.setTextPadding(0);
}

void drawStatusPill()
{
    bool healthy = shtOk && scdOk && co2 >= 0;
    uint16_t col = healthy ? C_GREEN : C_RED;
    const char *txt;

    if (healthy)
        txt = "CB OK";
    else if (!scdOk)
        txt = "LOI CO2";
    else if (!shtOk)
        txt = "LOI SHT";
    else
        txt = "DANG DO";

    tft.fillRoundRect(228, 2, 84, 20, 9, C_CARD);
    tft.drawRoundRect(228, 2, 84, 20, 9, col);
    tft.fillCircle(240, 12, 3, col);

    tft.setTextFont(2);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(col, C_CARD);
    tft.setTextPadding(0);
    tft.drawString(txt, 278, 12);
}

void drawCo2()
{
    uint16_t col = co2Color(co2);
    char buf[8];

    if (co2 >= 0)
        snprintf(buf, sizeof(buf), "%d", co2);
    else
        strcpy(buf, "----");

    // Font 8: 75px, chi co chu so va . - : -> don vi phai de o nhan
    tft.setTextFont(8);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(col, C_CARD);
    tft.setTextPadding(250);
    tft.drawString(buf, CO2_CX, CO2_CY);
    tft.setTextPadding(0);

    tft.setTextFont(2);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(col, C_CARD);
    tft.setTextPadding(90);
    tft.drawString(co2Label(co2), CO2_X + CO2_W - 12, CO2_Y + 6);
    tft.setTextPadding(0);
}

void drawTemp()
{
    char buf[8];
    bool ok = !isnan(roomTemp);

    if (ok)
        snprintf(buf, sizeof(buf), "%.1f", roomTemp);
    else
        strcpy(buf, "--");

    tft.setTextFont(6);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(ok ? C_TEXT : C_DIM, C_CARD);
    tft.setTextPadding(120);
    tft.drawString(buf, TP_CX, BOT_CY);
    tft.setTextPadding(0);
}

void drawHum()
{
    char buf[8];
    bool ok = !isnan(roomHum);

    if (ok)
        snprintf(buf, sizeof(buf), "%.0f", roomHum);
    else
        strcpy(buf, "--");

    tft.setTextFont(6);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(ok ? C_TEXT : C_DIM, C_CARD);
    tft.setTextPadding(120);
    tft.drawString(buf, HM_CX, BOT_CY);
    tft.setTextPadding(0);
}

void render(bool force = false)
{
    bool healthy = shtOk && scdOk && co2 >= 0;
    if (force || healthy != prevSensorHealthy)
    {
        drawStatusPill();
        prevSensorHealthy = healthy;
    }

    if (force || netState != prevNet)
    {
        drawNetIndicator();
        prevNet = netState;
    }

    if (force || co2 != prevCo2)
    {
        drawCo2();
        prevCo2 = co2;
    }

    int tX10 = isnan(roomTemp) ? -9999 : (int)(roomTemp * 10 + 0.5f);
    if (force || tX10 != prevTempX10)
    {
        drawTemp();
        prevTempX10 = tX10;
    }

    int h = isnan(roomHum) ? -2 : (int)(roomHum + 0.5f);
    if (force || h != prevHum)
    {
        drawHum();
        prevHum = h;
    }
}

void initDisplay()
{
    // Gan chan cho DUNG instance SPI ma TFT_eSPI dang dung.
    // Voi USE_HSPI_PORT, thu vien tao rieng mot SPIClass(HSPI) chu khong dung
    // doi tuong SPI toan cuc -> phai lay qua getSPIinstance().
    // MISO = -1 de khong chiem GPIO13 (dang lam chan DC).
    tft.getSPIinstance().begin(TFT_SCLK, -1, TFT_MOSI, -1);
    tft.init();
    tft.setRotation(1);
    drawChrome();
}

// Hien mot dong thong bao de len giua man, dung luc chay FRC
void showMessage(const char *line1, const char *line2, uint16_t col)
{
    tft.fillRoundRect(CO2_X, CO2_Y, CO2_W, CO2_H, 8, C_CARD);

    tft.setTextFont(4);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(col, C_CARD);
    tft.drawString(line1, CO2_CX, CO2_Y + 40);

    tft.setTextFont(2);
    tft.setTextColor(C_DIM, C_CARD);
    tft.drawString(line2, CO2_CX, CO2_Y + 74);
}

#else // ---- khong dung man: cac ham thanh rong, code goi giu nguyen ----

inline void initDisplay() {}
inline void render(bool force = false) { (void)force; }
inline void showMessage(const char *a, const char *b, uint16_t c)
{
    (void)a;
    (void)b;
    (void)c;
}

#endif

// ================= CAM BIEN =================
void scanI2C()
{
    Serial.println("[I2C] Quet bus...");
    uint8_t found = 0;
    for (uint8_t addr = 1; addr < 127; addr++)
    {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0)
        {
            Serial.printf("[I2C] Thay thiet bi tai 0x%02X\n", addr);
            found++;
        }
    }
    if (found == 0)
        Serial.println("[I2C] Khong thay thiet bi nao -> kiem tra day noi!");
}

bool initSht()
{
    if (sht31.begin(0x44))
    {
        shtAddr = 0x44;
        return true;
    }
    if (sht31.begin(0x45))
    {
        shtAddr = 0x45;
        return true;
    }
    return false;
}

bool readSht()
{
    if (!shtOk)
        return false;

    float t, h;
    if (sht31.readBoth(&t, &h))
    {
        roomTemp = t;
        roomHum = h;
        return true;
    }

    shtOk = false;
    roomTemp = roomHum = NAN;
    Serial.println("[SHT3X] Doc that bai -> se thu ket noi lai");
    return true;
}

bool initScd()
{
    // Tham so 2 = false: begin() KHONG tu bat periodic measurement (5s/lan),
    // de minh tu bat che do low power (30s/lan) ngay duoi.
    // Tham so 3 = false: TAT tu hieu chuan ASC.
    //
    // Vi sao tat ASC: no lay gia tri thap nhat trong tuan roi coi do la 400ppm.
    // Phong nay dung quat thong gio, day chi xuong ~586ppm, nen ASC se tu tru
    // di gan 190ppm khoi moi so do — sai theo huong lam minh yen tam nham.
    // Thay bang FRC mot lan (giu nut BOOT 3 giay khi o ngoai troi).
    //
    // Lenh nay chi ghi vao RAM cam bien nen goi moi lan boot khong mon EEPROM.
    if (!scd4x.begin(Wire, false, false))
        return false;

    // Low power periodic: 30s/lan, dong trung binh ~3.2mA thay vi ~15mA.
    // Dong DINH luc do van la ~200mA nhu cu — chi la 30s moi giat mot lan.
    if (!scd4x.startLowPowerPeriodicMeasurement())
    {
        Serial.println("[SCD40] Khong bat duoc che do low power");
        return false;
    }

    Serial.println("[SCD40] OK tai 0x62, low power 30s/lan, ASC tat, "
                   "cho so dau tien...");
    return true;
}

// Hieu chuan cuong buc. Chay khi dang o ngoai troi, da de on dinh >= 5 phut,
// va khong co ai tho gan cam bien.
void runFrc()
{
    if (!scdOk)
    {
        Serial.println("[FRC] Cam bien chua san sang -> bo qua");
        return;
    }

    Serial.printf("[FRC] Bat dau, gia tri tham chieu %d ppm\n", FRC_REFERENCE);
    showMessage("DANG HIEU CHUAN", "dung tho vao cam bien", C_AMBER);

    // Phai ve che do idle truoc. Lenh stop can 500ms moi co hieu luc.
    scd4x.stopPeriodicMeasurement();
    delay(600);

    float correction = 0;
    bool ok = scd4x.performForcedRecalibration(FRC_REFERENCE, &correction);

    if (ok)
    {
        // Ghi vao EEPROM de song qua moi lan mat dien.
        // EEPROM chi chiu khoang 2000 lan ghi nen CHI goi o day, khong goi
        // trong initScd().
        scd4x.persistSettings();
        delay(800);

        Serial.printf("[FRC] Xong. Do lech da bu: %.1f ppm\n", correction);

        char buf[32];
        snprintf(buf, sizeof(buf), "lech %.0f ppm", correction);
        showMessage("HIEU CHUAN XONG", buf, C_GREEN);
    }
    else
    {
        Serial.println("[FRC] That bai — cam bien tu choi lenh");
        showMessage("HIEU CHUAN LOI", "thu lai sau vai giay", C_RED);
    }

    delay(3000);

    // Bat lai che do do binh thuong
    scd4x.startLowPowerPeriodicMeasurement();
    co2 = -1;
    co2Temp = co2Hum = NAN;
    prevCo2 = -2; // ep ve lai o CO2 vi vua bi showMessage de len
    lastCo2Ok = millis();
    render(true);
}

bool readScd()
{
    if (!scdOk)
        return false;

    // readMeasurement() tu kiem tra co du lieu moi chua, chua co thi tra false
    if (!scd4x.readMeasurement())
        return false;

    int v = scd4x.getCO2();
    if (v <= 0)
        return false;

    co2 = v;
    // SCD40 co san cam bien nhiet/am, nhung no nam canh phan tu NDIR nong len
    // nen thuong doc cao hon thuc te 1-3C. Van gui len server de doi chieu,
    // con so hien tren man va so "chinh" la cua SHT3X.
    co2Temp = scd4x.getTemperature();
    co2Hum = scd4x.getHumidity();
    return true;
}

// ================= MANG =================
bool timeSynced()
{
    return time(nullptr) > TIME_VALID;
}

void startWifi()
{
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false); // che do ngu tiet kiem dien lam POST hay timeout
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.printf("[WIFI] Dang ket noi toi \"%s\"...\n", WIFI_SSID);
}

void startNtp()
{
    // Bat buoc phai co: vua de kiem han chung chi HTTPS, vua de recordedAt
    // giu nguyen khi retry (server idempotent theo deviceId + recordedAt).
    configTzTime(NTP_TZ, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    Serial.println("[NTP] Da yeu cau dong bo gio");
}

// Chup lai so lieu hien tai thanh mot goi cho gui
void snapshot()
{
    pending.active = true;
    pending.tries = 0;
    pending.ts = timeSynced() ? time(nullptr) : 0;

    pending.hasCo2 = (co2 > 0);
    pending.co2 = co2;

    pending.hasCo2Env = !isnan(co2Temp) && !isnan(co2Hum);
    pending.co2Temp = co2Temp;
    pending.co2Hum = co2Hum;

    pending.hasSht = !isnan(roomTemp) && !isnan(roomHum);
    pending.temp = roomTemp;
    pending.hum = roomHum;

    pending.rssi = WiFi.RSSI();
}

// Chi ghi key khi that su co so. Docs noi ro: cam bien loi thi BO HAN key,
// dung gui 0 — vi mainTemp = 0 la 0 do C hop le, server khong biet la so rac.
size_t buildBody(char *out, size_t cap)
{
    size_t n = 0;
    n += snprintf(out + n, cap - n, "{");

    if (pending.ts > 0)
        n += snprintf(out + n, cap - n, "\"recordedAt\":%lld,", (long long)pending.ts);

    if (pending.hasCo2)
        n += snprintf(out + n, cap - n, "\"co2\":%d,", pending.co2);

    if (pending.hasCo2Env)
        n += snprintf(out + n, cap - n, "\"co2Temp\":%.1f,\"co2Humidity\":%.1f,",
                      pending.co2Temp, pending.co2Hum);

    if (pending.hasSht)
        n += snprintf(out + n, cap - n, "\"mainTemp\":%.2f,\"mainHumidity\":%.2f,",
                      pending.temp, pending.hum);

    n += snprintf(out + n, cap - n, "\"rssi\":%d}", pending.rssi);
    return n;
}

bool sendPayload()
{
    if (WiFi.status() != WL_CONNECTED)
        return false;

    if (!pending.hasCo2 && !pending.hasCo2Env && !pending.hasSht)
    {
        Serial.println("[UP] Khong co so lieu nao -> bo goi");
        pending.active = false;
        return true;
    }

    char body[288];
    buildBody(body, sizeof(body));

    WiFiClientSecure client;
    // Bo kiem chung chi. Danh doi: neu co ai dung giua duong thi ho doc duoc
    // API_KEY. Chap nhan duoc trong mang nha; muon chat hon thi thay bang
    // client.setCACert(root_ca) voi root CA cua Cloudflare.
    client.setInsecure();
    client.setTimeout(10);

    HTTPClient http;
    http.setTimeout(10000);
    http.setConnectTimeout(10000);

    if (!http.begin(client, API_BASE "/api/ingest"))
    {
        Serial.println("[UP] http.begin that bai");
        return false;
    }

    http.addHeader("Content-Type", "application/json");
    http.addHeader("x-api-key", API_KEY);

    int code = http.POST((uint8_t *)body, strlen(body));
    String resp = code > 0 ? http.getString() : String();
    http.end();

    // Docs canh bao: Access tra 302 kem HTML chu khong phai loi 4xx.
    // Kieu kiem "code > 0 la thanh cong" se bao OK trong khi chang luu gi.
    Serial.printf("[UP] HTTP %d | %s\n", code, body);

    if (code == 201)
    {
        if (resp.indexOf("\"rejected\":[\"") >= 0)
            Serial.printf("[UP] SERVER BO BOT FIELD -> %s\n", resp.c_str());
        pending.active = false;
        return true;
    }

    if (code == 301 || code == 302 || code == 307)
        Serial.println("[UP] Bi chuyen huong -> Cloudflare Access chua dat "
                       "Bypass cho /api/ingest");
    else if (code == 401)
        Serial.println("[UP] 401 -> sai x-api-key");
    else if (code == 422)
        Serial.printf("[UP] 422 -> khong con gia tri nao dung duoc: %s\n", resp.c_str());
    else if (code > 0)
        Serial.printf("[UP] Loi %d: %s\n", code, resp.c_str());
    else
        Serial.printf("[UP] Khong ket noi duoc (%s)\n",
                      http.errorToString(code).c_str());

    return false;
}

void tryUpload(unsigned long now)
{
    if (!pending.active)
        return;

    if (pending.tries > 0 && now - lastUploadTry < UPLOAD_RETRY)
        return;

    lastUploadTry = now;
    pending.tries++;

    if (sendPayload())
    {
        netState = NET_SYNCED;
        render();
        return;
    }

    netState = WiFi.status() == WL_CONNECTED ? NET_UP : NET_DOWN;
    render();

    // Khong co NTP thi retry se de ra ban ghi trung (server chi idempotent
    // theo recordedAt). Tha mat mot goi con hon lam ban du lieu.
    if (pending.ts == 0)
    {
        Serial.println("[UP] Chua co NTP -> khong retry, bo goi nay");
        pending.active = false;
        return;
    }

    if (pending.tries >= UPLOAD_MAX_TRY)
    {
        Serial.println("[UP] Het so lan thu -> bo goi, cho nhip sau");
        pending.active = false;
    }
}

// ================= MAIN =================
void setup()
{
    Serial.begin(115200);
    delay(300);
    Serial.println("\n[BOOT] air-monitor v2 | CO2 + nhiet do + do am + gui API");

    initDisplay();

    // Nut BOOT co san tren board, noi san xuong GND khi bam -> dung pull-up.
    pinMode(BTN_PIN, INPUT_PULLUP);

    Wire.begin(SDA_PIN, SCL_PIN);
    Wire.setClock(100000); // SCD4x toi da 100kHz, day dupont dai cung can cham
    delay(100);
    scanI2C();

    shtOk = initSht();
    if (shtOk)
    {
        Serial.printf("[SHT3X] OK tai 0x%02X\n", shtAddr);
        readSht();
    }
    else
    {
        Serial.println("[SHT3X] Khong tim thay tai 0x44 lan 0x45!");
    }

    scdOk = initScd();
    if (!scdOk)
        Serial.println("[SCD40] Khong tim thay tai 0x62!");

    startWifi();
    startNtp();

    render(true);

    unsigned long now = millis();
    lastShtRead = now;
    lastSensorRetry = now;
    lastCo2Ok = now;
    lastWifiTry = now;
    lastUpload = now;
    Serial.println("[BOOT] san sang");
}

void loop()
{
    unsigned long now = millis();

    // ----- nut BOOT: giu 3 giay de chay hieu chuan cuong buc -----
    static unsigned long btnDownAt = 0;
    static bool btnFired = false;
    bool btnDown = digitalRead(BTN_PIN) == LOW;

    if (!btnDown)
    {
        btnDownAt = 0;
        btnFired = false;
    }
    else
    {
        if (btnDownAt == 0)
            btnDownAt = now;
        else if (!btnFired && now - btnDownAt >= BTN_HOLD_MS)
        {
            btnFired = true; // chi chay mot lan cho moi lan giu
            runFrc();
            now = millis(); // runFrc ton vai giay, lay lai moc thoi gian
        }
    }

    // ----- WiFi -----
    bool wifiUp = WiFi.status() == WL_CONNECTED;

    if (!wifiUp && now - lastWifiTry >= 10000UL)
    {
        lastWifiTry = now;
        Serial.println("[WIFI] Chua co ket noi -> thu lai");
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASS);
        if (netState != NET_DOWN)
        {
            netState = NET_DOWN;
            render();
        }
    }
    else if (wifiUp && netState == NET_DOWN)
    {
        Serial.printf("[WIFI] Da noi, IP %s, RSSI %d dBm\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
        netState = NET_UP;
        render();
    }

    // ----- CO2: hoi lien tuc, sensor tu quyet dinh khi nao co so moi (5s) -----
    // Ghi moc thoi gian bang chinh bien `now`, KHONG dung millis() o day.
    // millis() luon lon hon `now` mot chut -> phep tru unsigned duoi kia se
    // tran vong thanh so khong lo va lam kich hoat nham nhanh reset.
    if (scdOk && readScd())
    {
        lastCo2Ok = now;
        Serial.printf("[SCD40] CO2 = %d ppm | %.1fC %.1f%%\n", co2, co2Temp, co2Hum);
        render();
    }

    if (scdOk && now - lastCo2Ok > CO2_STALE)
    {
        Serial.println("[SCD40] Qua lau khong co du lieu -> reset ket noi");
        scdOk = false;
        co2 = -1;
        co2Temp = co2Hum = NAN;
        render();
    }

    // ----- nhiet do / do am -----
    if (shtOk && now - lastShtRead >= SHT_INTERVAL)
    {
        lastShtRead = now;
        if (readSht())
            render();
    }

    // ----- thu ket noi lai cam bien hong -----
    if ((!shtOk || !scdOk) && now - lastSensorRetry >= SENSOR_RETRY)
    {
        lastSensorRetry = now;

        if (!shtOk && initSht())
        {
            shtOk = true;
            Serial.printf("[SHT3X] Da ket noi lai tai 0x%02X\n", shtAddr);
            readSht();
            render();
        }

        if (!scdOk && initScd())
        {
            scdOk = true;
            lastCo2Ok = now;
            render();
        }
    }

    // ----- gui len server -----
    if (now - lastUpload >= UPLOAD_INTERVAL)
    {
        lastUpload = now;
        if (pending.active)
            Serial.println("[UP] Goi cu chua gui duoc -> ghi de bang so lieu moi");
        snapshot();
    }

    tryUpload(now);

    delay(50); // khong can quay vong gap, de bus I2C va WiFi nghi mot chut
}