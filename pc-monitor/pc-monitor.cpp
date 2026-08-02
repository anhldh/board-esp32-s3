#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <TFT_eSPI.h>
#include <NimBLEDevice.h>
#include "Adafruit_SHT31.h"

TFT_eSPI tft = TFT_eSPI();
Adafruit_SHT31 sht31 = Adafruit_SHT31();

// ---------------- cau hinh ----------------
#define SDA_PIN 8
#define SCL_PIN 9

#define DATA_TIMEOUT 15000UL
#define SENSOR_INTERVAL 10000UL
#define RETRY_INTERVAL 5000UL

// ---------------- bang mau (RGB565) ----------------
#define C_BG 0x0862     // #0A0E13 nen gan den, hoi ngA xanh
#define C_CARD 0x10C4   // #141A22 nen the
#define C_LINE 0x2146   // #202B36 vien / rANH truot
#define C_TEXT 0xE77E   // #E6EDF3
#define C_DIM 0x7C53    // #7D8B99
#define C_CYAN 0x3DFF   // #38BDF8  CPU
#define C_VIOLET 0xA45F // #A78BFA  GPU
#define C_GREEN 0x4EF0  // #4ADE80  RAM
#define C_AMBER 0xFDE4  // #FBBF24  phong
#define C_RED 0xFB8E    // #F87171  canh bao

// ---------------- toa do ----------------
#define HDR_H 24

#define CARD_Y 30
#define CARD_H 116
#define CARD_A_X 6
#define CARD_B_X 164
#define CARD_W 150

#define GA_DX 75 // tam dong ho, tinh tu goc trai the
#define GA_DY 66
#define GA_R 40
#define GA_IR 31
#define ARC_FROM 30
#define ARC_TO 330

#define LIST_X 6
#define LIST_Y 152
#define LIST_W 308
#define LIST_H 84
#define ROW0_Y 160
#define ROW_DY 26
#define BAR_X 72
#define BAR_W 148
#define BAR_H 8

// ---------------- trang thai ----------------
struct Stats
{
    float cpu = 0, cpuTemp = 0;
    float ramUsed = 0, ramTotal = 0;
    bool hasGpu = false;
    float gpu = 0, gpuTemp = 0, gpuWatt = 0;
    float vramUsed = 0, vramTotal = 0;
};

Stats st;
bool linkUp = false;
unsigned long lastData = 0;

bool sensorOk = false;
uint8_t sensorAddr = 0x44;
float roomTemp = NAN, roomHum = NAN;
unsigned long lastSensorRead = 0;
unsigned long lastRetry = 0;

// nho lai gia tri da ve de khong ve lai cung tron khi khong doi
int prevCpuPct = -2, prevGpuPct = -2;
bool prevLink = false, prevSensor = false;

// ---------------- tien ich ve ----------------
uint16_t loadColor(float pct, uint16_t base)
{
    return pct >= 85.0f ? C_RED : base;
}

// Dong ho cung tron. pct < 0 nghia la khong co du lieu -> chi ve rANH truot.
void drawGauge(int cx, int cy, float pct, uint16_t color)
{
    if (pct > 100.0f)
        pct = 100.0f;

    if (pct < 0.0f)
    {
        tft.drawSmoothArc(cx, cy, GA_R, GA_IR, ARC_FROM, ARC_TO, C_LINE, C_CARD, true);
        return;
    }

    int mid = ARC_FROM + (int)((ARC_TO - ARC_FROM) * pct / 100.0f + 0.5f);
    if (mid > ARC_FROM)
        tft.drawSmoothArc(cx, cy, GA_R, GA_IR, ARC_FROM, mid, color, C_CARD, true);
    if (mid < ARC_TO)
        tft.drawSmoothArc(cx, cy, GA_R, GA_IR, mid, ARC_TO, C_LINE, C_CARD, true);
}

void drawCenterText(int cx, int cy, const char *big, const char *small, uint16_t color)
{
    tft.setTextDatum(MC_DATUM);

    tft.setTextFont(4);
    tft.setTextColor(color, C_CARD);
    tft.setTextPadding(58);
    tft.drawString(big, cx, cy - 6);

    tft.setTextFont(2);
    tft.setTextColor(C_DIM, C_CARD);
    tft.setTextPadding(46);
    tft.drawString(small, cx, cy + 20);

    tft.setTextPadding(0);
}

// Thanh bo goc: ve phan day roi ve phan con lai, khong xoa truoc -> khong nhay
void drawBar(int y, float pct, uint16_t color, bool active)
{
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;

    int fill = active ? (int)(BAR_W * pct / 100.0f + 0.5f) : 0;
    if (fill < BAR_H)
        fill = active && pct > 0 ? BAR_H : 0; // qua ngan thi bo goc se vo hinh

    tft.fillRoundRect(BAR_X, y, BAR_W, BAR_H, BAR_H / 2, C_LINE);
    if (fill > 0)
        tft.fillRoundRect(BAR_X, y, fill, BAR_H, BAR_H / 2, color);
}

void drawListRow(int idx, const char *value, float pct, uint16_t color, bool active)
{
    int y = ROW0_Y + idx * ROW_DY;

    tft.setTextFont(2);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(active ? C_TEXT : C_DIM, C_CARD);
    tft.setTextPadding(84);
    tft.drawString(value, 304, y);
    tft.setTextPadding(0);

    drawBar(y + 5, pct, color, active);
}

// ---------------- khung tinh, chi ve mot lan ----------------
void drawChrome()
{
    tft.fillScreen(C_BG);

    tft.fillRect(0, 0, 320, HDR_H, C_CARD);
    tft.setTextFont(2);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_CYAN, C_CARD);
    tft.drawString("PC MONITOR", 10, 3);

    // hai the dong ho
    tft.fillRoundRect(CARD_A_X, CARD_Y, CARD_W, CARD_H, 8, C_CARD);
    tft.fillRoundRect(CARD_B_X, CARD_Y, CARD_W, CARD_H, 8, C_CARD);

    tft.setTextColor(C_DIM, C_CARD);
    tft.setTextDatum(TL_DATUM);
    tft.drawString("CPU", CARD_A_X + 10, CARD_Y + 5);
    tft.drawString("GPU", CARD_B_X + 10, CARD_Y + 5);

    // the danh sach
    tft.fillRoundRect(LIST_X, LIST_Y, LIST_W, LIST_H, 8, C_CARD);

    const char *labels[] = {"RAM", "VRAM", "PHONG"};
    for (int i = 0; i < 3; i++)
    {
        tft.setTextColor(C_DIM, C_CARD);
        tft.setTextDatum(TL_DATUM);
        tft.drawString(labels[i], 16, ROW0_Y + i * ROW_DY);
    }
}

// ---------------- phan dong ----------------
void drawStatusPill()
{
    uint16_t col = linkUp ? C_GREEN : C_RED;

    // Ve lai ca vien pill moi lan -> khong can setTextPadding, tranh chuyen
    // o nen cua chu (cao 16px) de len chinh duong vien cua pill.
    tft.fillRoundRect(228, 2, 84, 20, 9, C_CARD);
    tft.drawRoundRect(228, 2, 84, 20, 9, col);
    tft.fillCircle(240, 12, 3, col);

    tft.setTextFont(2);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(col, C_CARD);
    tft.setTextPadding(0);
    // MC_DATUM lay y lam TAM chu, nen phai la 12 (giua pill), khong phai 3
    tft.drawString(linkUp ? "BLE OK" : "NO LINK", 278, 12);
}

void render(bool force = false)
{
    char big[16], sub[16], val[24];

    if (force || linkUp != prevLink)
    {
        drawStatusPill();
        prevLink = linkUp;
    }

    // ----- dong ho CPU -----
    int cpuPct = linkUp ? (int)(st.cpu + 0.5f) : -1;
    if (force || cpuPct != prevCpuPct)
    {
        drawGauge(CARD_A_X + GA_DX, CARD_Y + GA_DY, linkUp ? st.cpu : -1.0f,
                  loadColor(st.cpu, C_CYAN));
        prevCpuPct = cpuPct;
    }
    if (linkUp)
    {
        snprintf(big, sizeof(big), "%.0f%%", st.cpu);
        snprintf(sub, sizeof(sub), "%.0fC", st.cpuTemp);
    }
    else
    {
        strcpy(big, "--");
        strcpy(sub, "");
    }
    drawCenterText(CARD_A_X + GA_DX, CARD_Y + GA_DY, big, sub,
                   linkUp ? C_TEXT : C_DIM);

    // ----- dong ho GPU -----
    bool gpuOn = linkUp && st.hasGpu;
    int gpuPct = gpuOn ? (int)(st.gpu + 0.5f) : -1;
    if (force || gpuPct != prevGpuPct)
    {
        drawGauge(CARD_B_X + GA_DX, CARD_Y + GA_DY, gpuOn ? st.gpu : -1.0f,
                  loadColor(st.gpu, C_VIOLET));
        prevGpuPct = gpuPct;
    }
    if (gpuOn)
    {
        snprintf(big, sizeof(big), "%.0f%%", st.gpu);
        snprintf(sub, sizeof(sub), "%.0fC %.0fW", st.gpuTemp, st.gpuWatt);
    }
    else
    {
        strcpy(big, "--");
        strcpy(sub, "");
    }
    drawCenterText(CARD_B_X + GA_DX, CARD_Y + GA_DY, big, sub,
                   gpuOn ? C_TEXT : C_DIM);

    // ----- RAM -----
    float ramPct = st.ramTotal > 0 ? st.ramUsed / st.ramTotal * 100.0f : 0.0f;
    if (linkUp)
        snprintf(val, sizeof(val), "%.1f/%.0fG", st.ramUsed, st.ramTotal);
    else
        strcpy(val, "---");
    drawListRow(0, val, ramPct, loadColor(ramPct, C_GREEN), linkUp);

    // ----- VRAM -----
    float vramPct = st.vramTotal > 0 ? st.vramUsed / st.vramTotal * 100.0f : 0.0f;
    if (gpuOn)
        snprintf(val, sizeof(val), "%.1f/%.0fG", st.vramUsed, st.vramTotal);
    else
        strcpy(val, "---");
    drawListRow(1, val, vramPct, loadColor(vramPct, C_VIOLET), gpuOn);

    // ----- phong: doc lap voi ket noi PC -----
    bool roomOn = sensorOk && !isnan(roomTemp) && !isnan(roomHum);
    if (roomOn)
        snprintf(val, sizeof(val), "%.1fC %.0f%%", roomTemp, roomHum);
    else
        strcpy(val, "---");
    drawListRow(2, val, roomOn ? roomHum : 0, C_AMBER, roomOn);
}

// ---------------- cam bien ----------------
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

bool initSensor()
{
    if (sht31.begin(0x44))
    {
        sensorAddr = 0x44;
        return true;
    }
    if (sht31.begin(0x45))
    {
        sensorAddr = 0x45;
        return true;
    }
    return false;
}

bool readSensor()
{
    if (!sensorOk)
        return false;

    float t, h;
    if (sht31.readBoth(&t, &h))
    {
        roomTemp = t;
        roomHum = h;
        return true;
    }

    sensorOk = false;
    roomTemp = roomHum = NAN;
    Serial.println("[SHT3X] Doc that bai -> se thu ket noi lai");
    return true;
}

// ---------------- parse du lieu PC ----------------
void applyKV(const String &k, float v)
{
    if (k == "cpu")
        st.cpu = v;
    else if (k == "ct")
        st.cpuTemp = v;
    else if (k == "ram")
        st.ramUsed = v;
    else if (k == "ramt")
        st.ramTotal = v;
    else if (k == "gpu")
    {
        st.gpu = v;
        st.hasGpu = true;
    }
    else if (k == "gt")
        st.gpuTemp = v;
    else if (k == "gw")
        st.gpuWatt = v;
    else if (k == "vr")
        st.vramUsed = v;
    else if (k == "vrt")
        st.vramTotal = v;
}

// Dinh dang: cpu=45;ct=52;ram=12.3;ramt=32;gpu=78;gt=65;gw=220;vr=6.2;vrt=12
bool parseLine(String line)
{
    line.trim();
    if (line.length() == 0)
        return false;

    st.hasGpu = false;
    int start = 0, count = 0;

    while (start < (int)line.length())
    {
        int sep = line.indexOf(';', start);
        if (sep < 0)
            sep = line.length();

        String tok = line.substring(start, sep);
        int eq = tok.indexOf('=');
        if (eq > 0)
        {
            applyKV(tok.substring(0, eq), tok.substring(eq + 1).toFloat());
            count++;
        }
        start = sep + 1;
    }
    return count > 0;
}

// ---------------- BLE ----------------
#define BLE_NAME "PC-MONITOR"
#define SVC_UUID "6e5f0001-b5a3-f393-e0a9-e50e24dcca9e"
#define CHR_UUID "6e5f0002-b5a3-f393-e0a9-e50e24dcca9e"

volatile bool bleConnected = false;
volatile bool hasPending = false;
char rxBuf[160];
portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;

class ServerCB : public NimBLEServerCallbacks
{
    void onConnect(NimBLEServer *s) override
    {
        bleConnected = true;
        Serial.println("[BLE] PC da ket noi");
    }
    void onDisconnect(NimBLEServer *s) override
    {
        bleConnected = false;
        Serial.println("[BLE] PC ngat ket noi -> quang cao lai");
        NimBLEDevice::startAdvertising();
    }
};

class CharCB : public NimBLECharacteristicCallbacks
{
    // Callback nay chay trong task cua NimBLE, KHONG phai loop().
    // Ve man tu day se dung SPI tu mot task khac -> chi chep du lieu ra
    // roi de loop() xu ly.
    void onWrite(NimBLECharacteristic *c) override
    {
        std::string v = c->getValue();
        if (v.empty() || v.size() >= sizeof(rxBuf))
            return;

        portENTER_CRITICAL(&rxMux);
        memcpy(rxBuf, v.data(), v.size());
        rxBuf[v.size()] = '\0';
        hasPending = true;
        portEXIT_CRITICAL(&rxMux);
    }
};

void startBLE()
{
    NimBLEDevice::init(BLE_NAME);
    NimBLEDevice::setMTU(247); // du cho mot goi ~70 byte, khong phai chia nho

    NimBLEServer *server = NimBLEDevice::createServer();
    server->setCallbacks(new ServerCB());

    NimBLEService *svc = server->createService(SVC_UUID);
    NimBLECharacteristic *chr =
        svc->createCharacteristic(CHR_UUID, NIMBLE_PROPERTY::WRITE);
    chr->setCallbacks(new CharCB());
    svc->start();

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(SVC_UUID);
    adv->setScanResponse(true);
    adv->start();

    Serial.printf("[BLE] Dang quang cao ten \"%s\", MAC %s\n",
                  BLE_NAME, NimBLEDevice::toString().c_str());
}

// ---------------- main ----------------
void setup()
{
    Serial.begin(115200);
    delay(300);
    Serial.println("\n[BOOT] pc-monitor v4 | BLE + giao dien dong ho");

    // Gan chan cho DUNG instance SPI ma TFT_eSPI dang dung.
    // Voi USE_HSPI_PORT, thu vien tao rieng mot SPIClass(HSPI) chu khong dung
    // doi tuong SPI toan cuc -> phai lay qua getSPIinstance().
    // MISO = -1 de khong chiem GPIO13 (dang lam chan DC).
    tft.getSPIinstance().begin(TFT_SCLK, -1, TFT_MOSI, -1);
    tft.init();
    tft.setRotation(1);
    drawChrome();

    Wire.begin(SDA_PIN, SCL_PIN);
    Wire.setClock(100000); // day dupont dai -> ha xuong 100kHz cho on dinh
    delay(100);
    scanI2C();

    sensorOk = initSensor();
    if (sensorOk)
    {
        Serial.printf("[SHT3X] OK tai 0x%02X\n", sensorAddr);
        readSensor();
    }
    else
    {
        Serial.println("[SHT3X] Khong tim thay tai 0x44 lan 0x45!");
    }

    startBLE();
    render(true);

    lastSensorRead = millis();
    lastRetry = millis();
    Serial.println("[TFT] san sang");
}

void loop()
{
    unsigned long now = millis();

    if (hasPending)
    {
        char line[sizeof(rxBuf)];
        portENTER_CRITICAL(&rxMux);
        strncpy(line, rxBuf, sizeof(line));
        hasPending = false;
        portEXIT_CRITICAL(&rxMux);

        if (parseLine(String(line)))
        {
            lastData = now;
            linkUp = true;
            render();
        }
    }

    // Mat song BLE, hoac con ket noi nhung PC ngung gui -> deu coi la mat lien lac
    if (linkUp && (!bleConnected || now - lastData > DATA_TIMEOUT))
    {
        linkUp = false;
        render();
        Serial.println("[WARN] Mat du lieu tu PC");
    }

    if (!sensorOk && now - lastRetry >= RETRY_INTERVAL)
    {
        lastRetry = now;
        if (initSensor())
        {
            sensorOk = true;
            Serial.printf("[SHT3X] Da ket noi lai tai 0x%02X\n", sensorAddr);
            readSensor();
            render();
        }
    }

    if (sensorOk && now - lastSensorRead >= SENSOR_INTERVAL)
    {
        lastSensorRead = now;
        if (readSensor())
            render();
    }
}