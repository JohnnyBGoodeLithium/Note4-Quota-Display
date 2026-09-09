// Zectrix Note 4 - AI Agent 多页面看板 + 按键语音入口
//
// 主机 -> 设备：UTF-8 JSON Lines
//   {"type":"usage", ...}
//   {"type":"state","agent":"codex","task":"...","steps":[...],"actions":[...]}
// 设备 -> 主机：
//   @AUDIO <bytes> 16000 <codex|kimi>\n<raw s16le pcm>\n@AUDIO_END\n
//   @TARGET <codex|kimi>
//
// 按键：上/下键切页；正面圆键短按切换 Agent，按住 350ms 后录音，松开发送。

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLESecurity.h>
#include <BLEServer.h>
#include <Adafruit_GFX.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <time.h>

// ---- EPD / board power ----
static const int8_t PIN_EPD_PWR  = 6;
static const int8_t PIN_PWR_HOLD = 17;
static const int8_t PIN_EPD_BUSY = 8;
static const int8_t PIN_EPD_RST  = 9;
static const int8_t PIN_EPD_DC   = 10;
static const int8_t PIN_EPD_CS   = 11;
static const int8_t PIN_EPD_SCK  = 12;
static const int8_t PIN_EPD_MOSI = 13;
static const uint32_t EPD_SPI_HZ = 8000000;

// ---- Note 4 buttons (active LOW) ----
static const int8_t PIN_KEY_ENTER = 0;
static const int8_t PIN_KEY_UP    = 39;
static const int8_t PIN_KEY_DOWN  = 18;

// ---- Board status LED (the Note 4 exposes one green channel, active LOW) ----
static const int8_t PIN_LED_GREEN = 3;

// ---- ES8311 / I2S ----
static const int8_t PIN_AUDIO_PWR = 42;
static const int8_t PIN_AUDIO_PA  = 46;
static const int8_t PIN_I2C_SDA   = 47;
static const int8_t PIN_I2C_SCL   = 48;
static const int8_t PIN_I2S_MCLK  = 14;
static const int8_t PIN_I2S_BCLK  = 15;
static const int8_t PIN_I2S_DIN   = 16;
static const int8_t PIN_I2S_WS    = 38;
static const int8_t PIN_I2S_DOUT  = 45;
static const uint8_t ES8311_ADDR  = 0x18;
static const uint32_t AUDIO_RATE  = 16000;
static const size_t AUDIO_MAX_BYTES = AUDIO_RATE * 2 * 20;
static bool audioReady = false;

struct WirelessConfig {
    String ssid;
    String password;
    String host;
    String token;
    String ca;
    uint16_t port = 8765;
    uint32_t epoch = 0;
    bool valid = false;
};

static WirelessConfig wireless;
static uint32_t wirelessSeq = 0;
static uint32_t nextWifiActionAt = 0;
static uint32_t nextWirelessPollAt = 0;
static bool wifiBeginIssued = false;
static int lastWirelessHttpStatus = 0;
static uint8_t wirelessFailureCount = 0;

// ---- Encrypted BLE transport ----
// One bidirectional service: the host writes authenticated/chunked JSON to RX,
// while the board sends window-acknowledged chunked PCM on TX.
static const char* BLE_DEVICE_NAME  = "Zectrix-Note4";
static const char* BLE_SERVICE_UUID = "7a240000-8f34-4b7e-9d52-6ad16c7cdd48";
static const char* BLE_RX_UUID      = "7a240001-8f34-4b7e-9d52-6ad16c7cdd48";
static const char* BLE_TX_UUID      = "7a240002-8f34-4b7e-9d52-6ad16c7cdd48";
static BLECharacteristic* bleTx = nullptr;
static BLEAdvertising* bleAdvertising = nullptr;
static QueueHandle_t bleJsonQueue = nullptr;
static volatile bool bleConnected = false;
static volatile bool bleAuthorized = false;
static volatile uint16_t bleAudioAck = 0;
static volatile uint32_t bleAudioAckVersion = 0;
static String bleRxJson;

enum BleFrame : uint8_t {
    BLE_AUTH = 0x01,
    BLE_AUTH_RESULT = 0x02,
    BLE_JSON_START = 0x10,
    BLE_JSON_CHUNK = 0x11,
    BLE_JSON_END = 0x12,
    BLE_AUDIO_START = 0x20,
    BLE_AUDIO_CHUNK = 0x21,
    BLE_AUDIO_END = 0x22,
    BLE_AUDIO_ACK = 0x23,
};

static void handleJson(const String& line);

enum class IndicatorMode : uint8_t { OFF, RECORDING, PROCESSING, COMPLETE };
static IndicatorMode indicatorMode = IndicatorMode::OFF;
static uint32_t indicatorChangedAt = 0;

static void setIndicator(IndicatorMode mode) {
    indicatorMode = mode;
    indicatorChangedAt = millis();
}

static void updateIndicator() {
    bool on = false;
    const uint32_t elapsed = millis() - indicatorChangedAt;
    switch (indicatorMode) {
        case IndicatorMode::RECORDING:  on = true; break;
        case IndicatorMode::PROCESSING: on = (elapsed % 2500) < 80; break;
        case IndicatorMode::COMPLETE:
            on = elapsed < 1200;
            if (!on) indicatorMode = IndicatorMode::OFF;
            break;
        default: break;
    }
    digitalWrite(PIN_LED_GREEN, on ? LOW : HIGH);
}

static const uint16_t COL_BLACK = 0;
static const uint16_t COL_WHITE = 1;

// ---- Note 4 SSD1683-style display driver ----
class ZectrixEPD : public Adafruit_GFX {
public:
    static const int W = 400;
    static const int H = 300;
    static const int ROW = W / 8;
    static const int FB_SIZE = ROW * H;

    uint8_t fb[FB_SIZE];
    uint8_t prev[FB_SIZE];

    ZectrixEPD() : Adafruit_GFX(W, H) {}

    void drawPixel(int16_t x, int16_t y, uint16_t color) override {
        if (x < 0 || x >= W || y < 0 || y >= H) return;
        uint32_t idx = (uint32_t)y * ROW + (x >> 3);
        uint8_t mask = 0x80 >> (x & 7);
        if (color) fb[idx] |= mask;
        else fb[idx] &= ~mask;
    }

    void begin() {
        pinMode(PIN_PWR_HOLD, OUTPUT);
        digitalWrite(PIN_PWR_HOLD, HIGH);
        pinMode(PIN_EPD_PWR, OUTPUT);
        digitalWrite(PIN_EPD_PWR, HIGH);
        delay(10);

        pinMode(PIN_EPD_RST, OUTPUT);
        pinMode(PIN_EPD_DC, OUTPUT);
        pinMode(PIN_EPD_CS, OUTPUT);
        digitalWrite(PIN_EPD_RST, HIGH);
        digitalWrite(PIN_EPD_DC, HIGH);
        digitalWrite(PIN_EPD_CS, HIGH);
        pinMode(PIN_EPD_BUSY, INPUT);
        SPI.begin(PIN_EPD_SCK, -1, PIN_EPD_MOSI, PIN_EPD_CS);
        memset(fb, 0xFF, sizeof(fb));
        memset(prev, 0xFF, sizeof(prev));
    }

    void show(bool full) {
        (void)full;
        powerOn();
        resetPanel();
        readBusy(5000, "reset");

        sendCmd(0x00); sendData(0x2F); sendData(0x2E);
        sendCmd(0xE9); sendData(0x01);
        readBusy(5000, "init");
        sendCmd(0xE0); sendData(0x02);
        sendCmd(0xE6); sendData(238);
        sendCmd(0xA5);
        readBusy(5000, "temperature");
        delay(10);

        sendCmd(0x10);
        readBusy(10000, "0x10");
        SPI.beginTransaction(SPISettings(EPD_SPI_HZ, MSBFIRST, SPI_MODE0));
        uint8_t line[ROW * 2];
        for (int y = 0; y < H; y++) {
            const uint8_t* curRow = fb + y * ROW;
            for (int j = 0; j < ROW; j++) {
                uint8_t cur = curRow[j];
                uint8_t o0 = 0, o1 = 0;
                for (int i = 0; i < 8; i++) {
                    uint8_t bit = (cur >> (7 - i)) & 1;
                    if (i < 4) o0 |= bit << (6 - 2 * i);
                    else o1 |= bit << (14 - 2 * i);
                }
                line[2 * j] = o0;
                line[2 * j + 1] = o1;
            }
            digitalWrite(PIN_EPD_DC, HIGH);
            digitalWrite(PIN_EPD_CS, LOW);
            SPI.transfer(line, sizeof(line));
            digitalWrite(PIN_EPD_CS, HIGH);
        }
        SPI.endTransaction();

        sendCmd(0x04);
        readBusy(10000, "0x04");
        delay(10);
        sendCmd(0x12); sendData(0x00);
        delay(10);
        readBusy(15000, "0x12");
        sendCmd(0x02); sendData(0x00);
        readBusy(10000, "0x02");
        delay(20);
        digitalWrite(PIN_EPD_PWR, LOW);
        memcpy(prev, fb, sizeof(fb));
        Serial.println("[diag] display refreshed");
    }

private:
    void powerOn() {
        digitalWrite(PIN_EPD_PWR, HIGH);
        delay(10);
    }
    void resetPanel() {
        digitalWrite(PIN_EPD_RST, HIGH); delay(10);
        digitalWrite(PIN_EPD_RST, LOW); delay(20);
        digitalWrite(PIN_EPD_RST, HIGH); delay(10);
    }
    bool readBusy(uint32_t timeoutMs, const char* stage) {
        uint32_t started = millis();
        while (digitalRead(PIN_EPD_BUSY) == 0) {
            if (millis() - started > timeoutMs) {
                Serial.printf("[diag] busy timeout @%s\n", stage);
                return false;
            }
            delay(5);
        }
        return true;
    }
    void spiByte(uint8_t value) {
        SPI.beginTransaction(SPISettings(EPD_SPI_HZ, MSBFIRST, SPI_MODE0));
        SPI.transfer(value);
        SPI.endTransaction();
    }
    void sendCmd(uint8_t value) {
        digitalWrite(PIN_EPD_DC, LOW);
        digitalWrite(PIN_EPD_CS, LOW);
        spiByte(value);
        digitalWrite(PIN_EPD_CS, HIGH);
    }
    void sendData(uint8_t value) {
        digitalWrite(PIN_EPD_DC, HIGH);
        digitalWrite(PIN_EPD_CS, LOW);
        spiByte(value);
        digitalWrite(PIN_EPD_CS, HIGH);
    }
};

static ZectrixEPD display;
static U8G2_FOR_ADAFRUIT_GFX u8g2;

// ---- Direct ES8311 configuration, based on Espressif esp_codec_dev ----
static bool codecWrite(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

static bool codecRead(uint8_t reg, uint8_t& value) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)ES8311_ADDR, 1) != 1) return false;
    value = Wire.read();
    return true;
}

static bool audioBegin() {
    pinMode(PIN_AUDIO_PA, OUTPUT);
    digitalWrite(PIN_AUDIO_PA, LOW);
    pinMode(PIN_AUDIO_PWR, OUTPUT);
    digitalWrite(PIN_AUDIO_PWR, HIGH);
    delay(30);

    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000);

    i2s_config_t config = {};
    config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX);
    config.sample_rate = AUDIO_RATE;
    config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    // IDF 4.4 on ESP32-S3 has a mono RX slot-mask bug; receive both slots and
    // collapse the ES8311's active slot to mono in software.
    config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    config.dma_buf_count = 6;
    config.dma_buf_len = 240;
    config.use_apll = false;
    config.tx_desc_auto_clear = true;
    config.fixed_mclk = AUDIO_RATE * 256;
    config.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    config.bits_per_chan = I2S_BITS_PER_CHAN_16BIT;

    if (i2s_driver_install(I2S_NUM_0, &config, 0, nullptr) != ESP_OK) return false;
    i2s_pin_config_t pins = {};
    pins.mck_io_num = PIN_I2S_MCLK;
    pins.bck_io_num = PIN_I2S_BCLK;
    pins.ws_io_num = PIN_I2S_WS;
    pins.data_out_num = PIN_I2S_DOUT;
    pins.data_in_num = PIN_I2S_DIN;
    if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) return false;
    if (i2s_set_clk(I2S_NUM_0, AUDIO_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO) != ESP_OK) return false;

    bool ok = true;
    ok &= codecWrite(0x44, 0x08);  // improve I2C noise immunity; first write can fail
    ok &= codecWrite(0x44, 0x08);
    ok &= codecWrite(0x01, 0x30);
    ok &= codecWrite(0x02, 0x00);
    ok &= codecWrite(0x03, 0x10);
    ok &= codecWrite(0x16, 0x24);
    ok &= codecWrite(0x04, 0x10);
    ok &= codecWrite(0x05, 0x00);
    ok &= codecWrite(0x0B, 0x00);
    ok &= codecWrite(0x0C, 0x00);
    ok &= codecWrite(0x10, 0x1F);
    ok &= codecWrite(0x11, 0x7F);
    ok &= codecWrite(0x00, 0x80);  // slave mode
    ok &= codecWrite(0x01, 0x3F);  // use external MCLK
    ok &= codecWrite(0x13, 0x10);
    ok &= codecWrite(0x1B, 0x0A);
    ok &= codecWrite(0x1C, 0x6A);
    ok &= codecWrite(0x44, 0x58);  // internal ADC/DAC reference

    uint8_t value = 0;
    if (codecRead(0x09, value)) ok &= codecWrite(0x09, (value & 0xFC) | 0x0C);
    else ok = false;
    if (codecRead(0x0A, value)) ok &= codecWrite(0x0A, (value & 0xFC) | 0x0C);
    else ok = false;

    // 16 kHz with 4.096 MHz MCLK: prediv=1, ADC/DAC div=1, BCLK div=4.
    ok &= codecWrite(0x02, 0x00);
    ok &= codecWrite(0x05, 0x00);
    ok &= codecWrite(0x03, 0x10);
    ok &= codecWrite(0x04, 0x20);
    ok &= codecWrite(0x07, 0x00);
    ok &= codecWrite(0x08, 0xFF);
    ok &= codecWrite(0x06, 0x03);

    // Enable analog mic ADC and DAC. Mic PGA = 30 dB.
    ok &= codecWrite(0x00, 0x80);
    ok &= codecWrite(0x01, 0x3F);
    ok &= codecWrite(0x09, 0x0C);
    ok &= codecWrite(0x0A, 0x0C);
    ok &= codecWrite(0x17, 0xBF);
    ok &= codecWrite(0x0E, 0x02);
    ok &= codecWrite(0x12, 0x00);
    ok &= codecWrite(0x14, 0x1A);
    ok &= codecWrite(0x0D, 0x01);
    ok &= codecWrite(0x15, 0x40);
    ok &= codecWrite(0x16, 0x05);
    ok &= codecWrite(0x31, 0x00);
    ok &= codecWrite(0x32, 0xD0);
    ok &= codecWrite(0x37, 0x08);
    ok &= codecWrite(0x45, 0x00);
    delay(80);
    return ok;
}

static void playBeep(uint16_t frequency, uint16_t durationMs) {
    static int16_t samples[160];
    const int period = max(2, (int)(AUDIO_RATE / frequency));
    const int chunks = max(1, (int)(durationMs * AUDIO_RATE / 1000 / 160));
    digitalWrite(PIN_AUDIO_PA, HIGH);
    delay(5);
    for (int chunk = 0; chunk < chunks; chunk++) {
        for (int i = 0; i < 160; i++) {
            int phase = (chunk * 160 + i) % period;
            samples[i] = phase < period / 2 ? 6000 : -6000;
        }
        size_t written = 0;
        i2s_write(I2S_NUM_0, samples, sizeof(samples), &written, pdMS_TO_TICKS(50));
    }
    i2s_zero_dma_buffer(I2S_NUM_0);
    delay(8);
    digitalWrite(PIN_AUDIO_PA, LOW);
}

static void playCompletionChime() {
    if (!audioReady) return;
    playBeep(980, 75);
    delay(35);
    playBeep(1320, 120);
}

// ---- Display state ----
struct UsageState {
    int codex = -1;
    String codexReset = "--";
    int kimiWeek = -1;
    String kimiWeekReset = "--";
    int kimiFive = -1;
    String kimiFiveReset = "--";
    String syncAt = "--";
    String syncStatus = "cached";
};

struct StepState {
    String text;
    String state;
};

struct AgentState {
    String agent = "codex";
    String task = "等待 AI 任务";
    String status = "idle";
    String action = "空闲";
    String actionKind = "idle";
    int progress = 0;
    StepState steps[5];
    int stepCount = 0;
    String actions[5];
    int actionCount = 0;
    String reply = "暂无完整回复";
    String updated = "--:--";
};

static UsageState usage;
static AgentState agentState;
static String voiceTarget = "codex";
static int currentPage = 0;
static const int PAGE_COUNT = 4;
static bool screenDirty = true;
static bool urgentRender = false;
static uint32_t lastRenderAt = 0;
static uint32_t renderCount = 0;

static void drawWrapped(int x, int y, int maxWidth, int lineHeight, const String& text, int maxLines) {
    String line;
    int lineCount = 0;
    int index = 0;
    while (index < (int)text.length() && lineCount < maxLines) {
        int len = 1;
        uint8_t ch0 = text[index];
        if ((ch0 & 0xE0) == 0xC0) len = 2;
        else if ((ch0 & 0xF0) == 0xE0) len = 3;
        else if ((ch0 & 0xF8) == 0xF0) len = 4;
        String ch = text.substring(index, index + len);
        if (u8g2.getUTF8Width((line + ch).c_str()) > maxWidth && line.length()) {
            u8g2.setCursor(x, y + lineCount * lineHeight);
            u8g2.print(line);
            line = ch;
            lineCount++;
        } else {
            line += ch;
        }
        index += len;
    }
    if (lineCount < maxLines && line.length()) {
        u8g2.setCursor(x, y + lineCount * lineHeight);
        u8g2.print(line);
    }
}

static void drawProgressBar(int x, int y, int w, int progress) {
    int value = constrain(progress, 0, 100);
    display.drawRect(x, y, w, 13, COL_BLACK);
    int fill = (w - 4) * value / 100;
    if (fill > 0) display.fillRect(x + 2, y + 2, fill, 9, COL_BLACK);
}

static String agentName(const String& key) {
    return key == "kimi" ? "Kimi Code" : "Codex";
}

static void drawHeader(const String& title, const String& badge) {
    display.fillRoundRect(12, 10, 54, 54, 7, COL_BLACK);
    u8g2.setFont(u8g2_font_wqy16_t_gb2312);
    u8g2.setForegroundColor(COL_WHITE);
    u8g2.setCursor(39 - u8g2.getUTF8Width(badge.c_str()) / 2, 44);
    u8g2.print(badge);
    u8g2.setForegroundColor(COL_BLACK);
    u8g2.setCursor(80, 35);
    u8g2.print(title);
    u8g2.setFont(u8g2_font_wqy14_t_gb2312);
    String transport = bleAuthorized ? "BLE" :
                       (lastWirelessHttpStatus == HTTP_CODE_OK ? "WiFi" : "USB");
    String pageText = String(currentPage + 1) + "/" + String(PAGE_COUNT) + " " + transport + " 语音→" + agentName(voiceTarget);
    u8g2.setCursor(388 - u8g2.getUTF8Width(pageText.c_str()), 60);
    u8g2.print(pageText);
    display.drawLine(0, 76, 400, 76, COL_BLACK);
}

static void renderUsagePage() {
    drawHeader("AI 用量", "AI");
    u8g2.setFont(u8g2_font_wqy14_t_gb2312);

    u8g2.setCursor(12, 102); u8g2.print("Codex 周额度");
    String pct = usage.codex >= 0 ? String(usage.codex) + "%" : "--";
    u8g2.setCursor(388 - u8g2.getUTF8Width(pct.c_str()), 102); u8g2.print(pct);
    if (usage.codex >= 0) drawProgressBar(12, 112, 376, usage.codex);
    u8g2.setCursor(12, 148); u8g2.print("重置 " + usage.codexReset);

    display.drawLine(0, 160, 400, 160, COL_BLACK);
    u8g2.setCursor(12, 185); u8g2.print("Kimi 周额度");
    pct = usage.kimiWeek >= 0 ? String(usage.kimiWeek) + "%" : "--";
    u8g2.setCursor(388 - u8g2.getUTF8Width(pct.c_str()), 185); u8g2.print(pct);
    if (usage.kimiWeek >= 0) drawProgressBar(12, 195, 376, usage.kimiWeek);
    u8g2.setCursor(12, 231); u8g2.print("重置 " + usage.kimiWeekReset);

    display.drawLine(0, 243, 400, 243, COL_BLACK);
    String five = "Kimi 5小时  " + (usage.kimiFive >= 0 ? String(usage.kimiFive) + "%" : "--");
    u8g2.setCursor(12, 265); u8g2.print(five);
    String reset = "重置 " + usage.kimiFiveReset;
    u8g2.setCursor(388 - u8g2.getUTF8Width(reset.c_str()), 265); u8g2.print(reset);
    display.drawLine(0, 276, 400, 276, COL_BLACK);
    String status = usage.syncStatus == "ok" ? "正常" :
                    (usage.syncStatus == "partial" ? "部分异常" :
                    (usage.syncStatus == "failed" ? "同步失败" : "缓存"));
    String synced = "上次同步 " + usage.syncAt + "  " + status;
    u8g2.setCursor(12, 296); u8g2.print(synced);
}

static void renderStepsPage() {
    drawHeader("当前任务步骤", agentState.agent == "kimi" ? "K" : "X");
    u8g2.setFont(u8g2_font_wqy14_t_gb2312);
    u8g2.setCursor(12, 100); u8g2.print(agentName(agentState.agent));
    String status = agentState.status == "running" ? "运行中" :
                    (agentState.status == "done" ? "已完成" :
                    (agentState.status == "error" ? "失败" : "空闲"));
    u8g2.setCursor(388 - u8g2.getUTF8Width(status.c_str()), 100); u8g2.print(status);
    u8g2.setFont(u8g2_font_wqy16_t_gb2312);
    drawWrapped(12, 125, 376, 20, agentState.task, 2);
    display.drawLine(0, 158, 400, 158, COL_BLACK);

    u8g2.setFont(u8g2_font_wqy14_t_gb2312);
    if (agentState.stepCount == 0) {
        u8g2.setCursor(12, 190); u8g2.print("暂无任务步骤");
    }
    for (int i = 0; i < agentState.stepCount && i < 5; i++) {
        int y = 184 + i * 22;
        if (agentState.steps[i].state == "done") {
            display.fillCircle(20, y - 5, 6, COL_BLACK);
            u8g2.setForegroundColor(COL_WHITE);
            u8g2.setCursor(16, y - 1); u8g2.print("✓");
            u8g2.setForegroundColor(COL_BLACK);
        } else if (agentState.steps[i].state == "active") {
            display.fillRect(14, y - 11, 13, 13, COL_BLACK);
        } else {
            display.drawRect(14, y - 11, 13, 13, COL_BLACK);
        }
        String item = agentState.steps[i].text;
        if (item.length() > 48) item = item.substring(0, 48);
        u8g2.setCursor(37, y); u8g2.print(item);
    }
}

static int actionStage() {
    if (agentState.status == "done") return 3;
    if (agentState.actionKind == "voice" || agentState.actionKind == "transcribe") return 0;
    if (agentState.actionKind == "reasoning" || agentState.actionKind == "model") return 1;
    if (agentState.actionKind == "command" || agentState.actionKind == "file" ||
        agentState.actionKind == "tool" || agentState.actionKind == "web") return 2;
    return agentState.status == "running" ? 1 : -1;
}

static void renderActionsPage() {
    drawHeader("动作可视化", agentState.agent == "kimi" ? "K" : "X");
    const char* labels[4] = {"输入", "模型", "工具", "完成"};
    int active = actionStage();
    u8g2.setFont(u8g2_font_wqy14_t_gb2312);
    for (int i = 0; i < 4; i++) {
        int x = 12 + i * 97;
        if (i == active) display.fillRoundRect(x, 92, 85, 34, 5, COL_BLACK);
        else display.drawRoundRect(x, 92, 85, 34, 5, COL_BLACK);
        u8g2.setForegroundColor(i == active ? COL_WHITE : COL_BLACK);
        int width = u8g2.getUTF8Width(labels[i]);
        u8g2.setCursor(x + (85 - width) / 2, 115); u8g2.print(labels[i]);
        u8g2.setForegroundColor(COL_BLACK);
        if (i < 3) display.drawLine(x + 85, 109, x + 96, 109, COL_BLACK);
    }

    u8g2.setCursor(12, 154); u8g2.print("当前动作");
    u8g2.setFont(u8g2_font_wqy16_t_gb2312);
    drawWrapped(12, 180, 376, 20, agentState.action, 2);
    display.drawLine(0, 210, 400, 210, COL_BLACK);

    u8g2.setFont(u8g2_font_wqy14_t_gb2312);
    int start = max(0, agentState.actionCount - 3);
    for (int i = start; i < agentState.actionCount; i++) {
        int y = 235 + (i - start) * 21;
        display.fillCircle(18, y - 5, 3, COL_BLACK);
        String line = agentState.actions[i];
        if (line.length() > 52) line = line.substring(0, 52);
        u8g2.setCursor(30, y); u8g2.print(line);
    }
    u8g2.setCursor(330, 291); u8g2.print(agentState.updated);
}

static void renderReplyPage() {
    drawHeader("最近回复", agentState.agent == "kimi" ? "K" : "X");
    u8g2.setFont(u8g2_font_wqy14_t_gb2312);
    u8g2.setCursor(12, 101); u8g2.print(agentName(agentState.agent));
    String status = agentState.status == "running" ? "回复生成中" :
                    (agentState.status == "done" ? "完整回复" : "上次回复");
    u8g2.setCursor(388 - u8g2.getUTF8Width(status.c_str()), 101); u8g2.print(status);
    display.drawLine(0, 112, 400, 112, COL_BLACK);

    u8g2.setFont(u8g2_font_wqy16_t_gb2312);
    drawWrapped(12, 140, 376, 21, agentState.reply, 7);
    display.drawLine(0, 278, 400, 278, COL_BLACK);
    u8g2.setFont(u8g2_font_wqy14_t_gb2312);
    u8g2.setCursor(12, 297); u8g2.print("完成后刷新，避免逐字闪屏");
    u8g2.setCursor(350, 297); u8g2.print(agentState.updated);
}

static void renderScreen() {
    display.fillScreen(COL_WHITE);
    if (currentPage == 0) renderUsagePage();
    else if (currentPage == 1) renderStepsPage();
    else if (currentPage == 2) renderActionsPage();
    else renderReplyPage();
    display.show(renderCount % 30 == 0);
    renderCount++;
    screenDirty = false;
    urgentRender = false;
    lastRenderAt = millis();
}

static void handleJson(const String& line) {
    JsonDocument doc;
    if (deserializeJson(doc, line) != DeserializationError::Ok) {
        Serial.println("ERR bad json");
        return;
    }
    String type = doc["type"] | "";
    String incomingAgent = doc["agent"] | "";
    if (type == "usage" || incomingAgent == "usage") {
        if (doc["progress"].is<int>()) usage.codex = doc["progress"];
        if (doc["codex_reset"].is<const char*>()) usage.codexReset = doc["codex_reset"].as<String>();
        if (doc["kimi_week"].is<int>()) usage.kimiWeek = doc["kimi_week"];
        if (doc["kimi_week_reset"].is<const char*>()) usage.kimiWeekReset = doc["kimi_week_reset"].as<String>();
        if (doc["kimi_5h"].is<int>()) usage.kimiFive = doc["kimi_5h"];
        if (doc["kimi_5h_reset"].is<const char*>()) usage.kimiFiveReset = doc["kimi_5h_reset"].as<String>();
        if (doc["sync_at"].is<const char*>()) usage.syncAt = doc["sync_at"].as<String>();
        if (doc["sync_status"].is<const char*>()) usage.syncStatus = doc["sync_status"].as<String>();
    } else {
        String previousStatus = agentState.status;
        if (incomingAgent == "codex" || incomingAgent == "kimi") agentState.agent = incomingAgent;
        if (doc["task"].is<const char*>()) agentState.task = doc["task"].as<String>();
        if (doc["status"].is<const char*>()) agentState.status = doc["status"].as<String>();
        if (doc["action"].is<const char*>()) agentState.action = doc["action"].as<String>();
        if (doc["action_kind"].is<const char*>()) agentState.actionKind = doc["action_kind"].as<String>();
        if (doc["progress"].is<int>()) agentState.progress = doc["progress"];
        if (doc["updated"].is<const char*>()) agentState.updated = doc["updated"].as<String>();
        if (doc["reply"].is<const char*>()) agentState.reply = doc["reply"].as<String>();
        if (doc["target"].is<const char*>()) {
            String target = doc["target"].as<String>();
            if (target == "codex" || target == "kimi") voiceTarget = target;
        }
        if (doc["steps"].is<JsonArray>()) {
            agentState.stepCount = 0;
            for (JsonObject step : doc["steps"].as<JsonArray>()) {
                if (agentState.stepCount >= 5) break;
                agentState.steps[agentState.stepCount].text = step["text"] | "--";
                agentState.steps[agentState.stepCount].state = step["state"] | "pending";
                agentState.stepCount++;
            }
        }
        if (doc["actions"].is<JsonArray>()) {
            agentState.actionCount = 0;
            for (const char* action : doc["actions"].as<JsonArray>()) {
                if (agentState.actionCount >= 5) break;
                agentState.actions[agentState.actionCount++] = action;
            }
        }
        if (agentState.status == "running") setIndicator(IndicatorMode::PROCESSING);
        else if (agentState.status == "done" && previousStatus != "done") setIndicator(IndicatorMode::COMPLETE);
        else if (agentState.status == "error") setIndicator(IndicatorMode::OFF);
        if ((doc["notify"] | false) && agentState.status == "done") {
            setIndicator(IndicatorMode::COMPLETE);
            playCompletionChime();
        }
    }
    screenDirty = true;
    Serial.println("OK");
}

class Note4BleServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer*) override {
        bleConnected = true;
        bleAuthorized = false;
        bleRxJson = "";
        screenDirty = true;
        Serial.println("@BLE connected");
    }

    void onDisconnect(BLEServer*) override {
        bleConnected = false;
        bleAuthorized = false;
        bleRxJson = "";
        screenDirty = true;
        Serial.println("@BLE disconnected");
        if (bleAdvertising) bleAdvertising->start();
    }
};

class Note4BleRxCallbacks : public BLECharacteristicCallbacks {
    size_t expectedJsonBytes = 0;

    static uint32_t readU32(const uint8_t* data) {
        return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
               ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
    }

    void onWrite(BLECharacteristic* characteristic) override {
        std::string value = characteristic->getValue();
        if (value.empty()) return;
        const uint8_t* data = reinterpret_cast<const uint8_t*>(value.data());
        const size_t length = value.size();

        if (data[0] == BLE_AUTH) {
            String supplied;
            supplied.reserve(length > 1 ? length - 1 : 0);
            if (length > 1) supplied.concat(reinterpret_cast<const char*>(data + 1), length - 1);
            // Keep the auth frame inside the mandatory 20-byte BLE payload at
            // the default MTU. Nineteen URL-safe random characters still give
            // this local second factor well over 100 bits of entropy.
            bleAuthorized = wireless.token.length() >= 24 && supplied == wireless.token.substring(0, 19);
            uint8_t result[2] = {BLE_AUTH_RESULT, bleAuthorized ? (uint8_t)1 : (uint8_t)0};
            if (bleTx) {
                bleTx->setValue(result, sizeof(result));
                bleTx->notify();
            }
            screenDirty = true;
            Serial.println(bleAuthorized ? "@BLE authorized" : "@BLE auth_failed");
            return;
        }
        if (!bleAuthorized) return;

        if (data[0] == BLE_AUDIO_ACK && length == 3) {
            bleAudioAck = (uint16_t)data[1] | ((uint16_t)data[2] << 8);
            bleAudioAckVersion++;
            return;
        }

        if (data[0] == BLE_JSON_START && length == 5) {
            expectedJsonBytes = readU32(data + 1);
            bleRxJson = "";
            if (expectedJsonBytes > 0 && expectedJsonBytes <= 12288) bleRxJson.reserve(expectedJsonBytes);
            else expectedJsonBytes = 0;
            return;
        }
        if (data[0] == BLE_JSON_CHUNK && length > 1 && expectedJsonBytes > 0 &&
            bleRxJson.length() + length - 1 <= expectedJsonBytes) {
            bleRxJson.concat(reinterpret_cast<const char*>(data + 1), length - 1);
            return;
        }
        if (data[0] == BLE_JSON_END) {
            if (expectedJsonBytes > 0 && bleRxJson.length() == expectedJsonBytes && bleJsonQueue) {
                String* message = new String(bleRxJson);
                if (!message || xQueueSend(bleJsonQueue, &message, 0) != pdTRUE) delete message;
            }
            expectedJsonBytes = 0;
            bleRxJson = "";
        }
    }
};

static void startBle() {
    bleJsonQueue = xQueueCreate(8, sizeof(String*));
    BLEDevice::init(BLE_DEVICE_NAME);
    BLEDevice::setMTU(517);
    BLEServer* server = BLEDevice::createServer();
    server->setCallbacks(new Note4BleServerCallbacks());
    BLEService* service = server->createService(BLE_SERVICE_UUID);

    BLECharacteristic* rx = service->createCharacteristic(
        BLE_RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
    );
    rx->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);
    rx->setCallbacks(new Note4BleRxCallbacks());

    bleTx = service->createCharacteristic(
        BLE_TX_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    bleTx->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED);
    bleTx->addDescriptor(new BLE2902());
    service->start();

    BLESecurity* security = new BLESecurity();
    security->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
    security->setCapability(ESP_IO_CAP_NONE);
    security->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
    security->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

    bleAdvertising = BLEDevice::getAdvertising();
    bleAdvertising->addServiceUUID(BLE_SERVICE_UUID);
    bleAdvertising->setScanResponse(true);
    bleAdvertising->setMinPreferred(0x06);
    bleAdvertising->setMinPreferred(0x12);
    BLEDevice::startAdvertising();
    Serial.printf("[diag] ble=%s\n", BLE_DEVICE_NAME);
}

static void processBleJson() {
    if (!bleJsonQueue) return;
    String* message = nullptr;
    while (xQueueReceive(bleJsonQueue, &message, 0) == pdTRUE) {
        if (message) {
            handleJson(*message);
            delete message;
        }
    }
}

static void writeU32(uint8_t* destination, uint32_t value) {
    destination[0] = value & 0xFF;
    destination[1] = (value >> 8) & 0xFF;
    destination[2] = (value >> 16) & 0xFF;
    destination[3] = (value >> 24) & 0xFF;
}

static bool waitForBleAudioAck(uint32_t previousVersion, uint16_t& ack, uint32_t timeoutMs = 1500) {
    uint32_t deadline = millis() + timeoutMs;
    while (bleConnected && bleAuthorized && (int32_t)(millis() - deadline) < 0) {
        if (bleAudioAckVersion != previousVersion) {
            ack = bleAudioAck;
            return true;
        }
        updateIndicator();
        delay(1);
    }
    return false;
}

static bool notifyBleAndWaitAck(uint8_t* frame, size_t length, uint16_t expectedAck) {
    for (int attempt = 0; attempt < 8 && bleConnected && bleAuthorized; attempt++) {
        uint32_t previousVersion = bleAudioAckVersion;
        bleTx->setValue(frame, length);
        bleTx->notify();
        uint16_t ack = 0;
        if (waitForBleAudioAck(previousVersion, ack) && ack == expectedAck) return true;
    }
    return false;
}

static bool uploadAudioBle(const uint8_t* pcm, size_t size) {
    if (!bleConnected || !bleAuthorized || !bleTx) return false;
    uint8_t start[10] = {BLE_AUDIO_START};
    writeU32(start + 1, (uint32_t)size);
    writeU32(start + 5, AUDIO_RATE);
    start[9] = voiceTarget == "kimi" ? 1 : 0;
    if (!notifyBleAndWaitAck(start, sizeof(start), 0)) return false;

    // Eight notifications form a small reliable window. The host acknowledges
    // the next sequence it needs; a missing chunk retransmits only that suffix.
    // This is far faster than one GATT indication round-trip per 176 bytes.
    static const size_t CHUNK_BYTES = 176;
    static const uint16_t WINDOW_CHUNKS = 8;
    const uint16_t totalChunks = (uint16_t)((size + CHUNK_BYTES - 1) / CHUNK_BYTES);
    uint8_t frame[180];
    frame[0] = BLE_AUDIO_CHUNK;
    uint16_t sequence = 0;
    while (sequence < totalChunks) {
        if (!bleConnected || !bleAuthorized) return false;
        const uint16_t windowStart = sequence;
        const uint16_t windowEnd = min((uint16_t)(windowStart + WINDOW_CHUNKS), totalChunks);
        uint16_t retryFrom = windowStart;
        bool windowComplete = false;
        for (int attempt = 0; attempt < 10 && !windowComplete; attempt++) {
            uint32_t previousVersion = bleAudioAckVersion;
            for (uint16_t current = retryFrom; current < windowEnd; current++) {
                const size_t offset = (size_t)current * CHUNK_BYTES;
                const size_t chunk = min(CHUNK_BYTES, size - offset);
                frame[1] = current & 0xFF;
                frame[2] = (current >> 8) & 0xFF;
                frame[3] = current == windowEnd - 1 ? 1 : 0;
                memcpy(frame + 4, pcm + offset, chunk);
                bleTx->setValue(frame, chunk + 4);
                bleTx->notify();
                updateIndicator();
                delay(3);
            }
            uint16_t ack = 0;
            if (!waitForBleAudioAck(previousVersion, ack)) {
                retryFrom = windowStart;
                continue;
            }
            if (ack == windowEnd) {
                sequence = windowEnd;
                windowComplete = true;
            } else if (ack >= windowStart && ack < windowEnd) {
                retryFrom = ack;
            } else {
                retryFrom = windowStart;
            }
        }
        if (!windowComplete) return false;
    }
    uint8_t end = BLE_AUDIO_END;
    return notifyBleAndWaitAck(&end, 1, 0xFFFF);
}

// ---- Wi-Fi + pinned local HTTPS transport ----
static void loadWirelessConfig() {
    Preferences prefs;
    if (!prefs.begin("zectrixwifi", true)) return;
    wireless.ssid = prefs.getString("ssid", "");
    wireless.password = prefs.getString("password", "");
    wireless.host = prefs.getString("host", "");
    wireless.token = prefs.getString("token", "");
    wireless.ca = prefs.getString("ca", "");
    wireless.port = prefs.getUShort("port", 8765);
    wireless.epoch = prefs.getULong("epoch", 0);
    prefs.end();
    wireless.valid = wireless.ssid.length() && wireless.host.length() &&
                     wireless.token.length() >= 24 && wireless.ca.indexOf("BEGIN CERTIFICATE") >= 0;
}

static bool saveWirelessConfig(const String& json) {
    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) return false;
    String ssid = doc["ssid"] | "";
    String password = doc["password"] | "";
    String host = doc["host"] | "";
    String token = doc["token"] | "";
    String ca = doc["ca"] | "";
    int port = doc["port"] | 8765;
    uint32_t epoch = doc["epoch"] | 0U;
    if (!ssid.length() || !host.length() || token.length() < 24 ||
        ca.indexOf("BEGIN CERTIFICATE") < 0 || port < 1 || port > 65535 || epoch < 1700000000U) return false;
    Preferences prefs;
    if (!prefs.begin("zectrixwifi", false)) return false;
    bool ok = prefs.putString("ssid", ssid) > 0;
    ok &= prefs.putString("password", password) >= password.length();
    ok &= prefs.putString("host", host) > 0;
    ok &= prefs.putString("token", token) > 0;
    ok &= prefs.putString("ca", ca) > 0;
    ok &= prefs.putUShort("port", (uint16_t)port) > 0;
    ok &= prefs.putULong("epoch", epoch) > 0;
    prefs.end();
    if (!ok) return false;
    loadWirelessConfig();
    WiFi.disconnect(true, true);
    wifiBeginIssued = false;
    nextWifiActionAt = 0;
    wirelessSeq = 0;
    if (time(nullptr) < (time_t)wireless.epoch) {
        timeval tv = {(time_t)wireless.epoch, 0};
        settimeofday(&tv, nullptr);
    }
    return wireless.valid;
}

static String wirelessUrl(const String& path) {
    return "https://" + wireless.host + ":" + String(wireless.port) + path;
}

static void maintainWifi() {
    if (!wireless.valid) return;
    // BLE is both the preferred transport and substantially cheaper than an
    // always-associated Wi-Fi radio. Restore Wi-Fi only as an automatic
    // fallback after BLE disconnects.
    if (bleConnected) {
        if (WiFi.status() == WL_CONNECTED) {
            WiFi.disconnect(false, false);
            lastWirelessHttpStatus = 0;
            screenDirty = true;
        }
        return;
    }
    if (WiFi.status() == WL_CONNECTED) {
        static bool ntpStarted = false;
        if (!ntpStarted) {
            configTime(0, 0, "pool.ntp.org", "time.google.com");
            ntpStarted = true;
            screenDirty = true;
        }
        return;
    }
    if ((int32_t)(millis() - nextWifiActionAt) < 0) return;
    nextWifiActionAt = millis() + 10000;
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(true);
    WiFi.setHostname("zectrix-note4");
    if (wireless.password.length()) WiFi.begin(wireless.ssid.c_str(), wireless.password.c_str());
    else WiFi.begin(wireless.ssid.c_str());
    wifiBeginIssued = true;
}

static bool tlsClockReady() {
    return time(nullptr) > 1700000000;
}

static void pollWireless() {
    if (bleConnected || !wireless.valid || WiFi.status() != WL_CONNECTED || !tlsClockReady()) return;
    if ((int32_t)(millis() - nextWirelessPollAt) < 0) return;
    nextWirelessPollAt = millis() + (wirelessFailureCount >= 2 ? 60000 : 3000);
    WiFiClientSecure client;
    client.setCACert(wireless.ca.c_str());
    client.setHandshakeTimeout(8);
    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(8000);
    String url = wirelessUrl("/v1/poll?after=" + String(wirelessSeq));
    if (!http.begin(client, url)) return;
    http.addHeader("Authorization", "Bearer " + wireless.token);
    int status = http.GET();
    lastWirelessHttpStatus = status;
    if (status == HTTP_CODE_OK) {
        wirelessFailureCount = 0;
        String body = http.getString();
        JsonDocument response;
        if (deserializeJson(response, body) == DeserializationError::Ok) {
            if (response["events"].is<JsonArray>()) {
                for (JsonVariant event : response["events"].as<JsonArray>()) {
                    String line;
                    serializeJson(event, line);
                    handleJson(line);
                }
            }
            if (response["seq"].is<uint32_t>()) wirelessSeq = response["seq"].as<uint32_t>();
        }
    } else {
        wirelessFailureCount = min((int)wirelessFailureCount + 1, 10);
    }
    http.end();
}

static bool uploadAudioWireless(const uint8_t* pcm, size_t size) {
    if (!wireless.valid || WiFi.status() != WL_CONNECTED || !tlsClockReady()) return false;
    WiFiClientSecure client;
    client.setCACert(wireless.ca.c_str());
    client.setHandshakeTimeout(8);
    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(20000);
    if (!http.begin(client, wirelessUrl("/v1/audio"))) return false;
    http.addHeader("Authorization", "Bearer " + wireless.token);
    http.addHeader("Content-Type", "application/octet-stream");
    http.addHeader("X-Agent-Target", voiceTarget);
    http.addHeader("X-Audio-Rate", String(AUDIO_RATE));
    http.addHeader("X-Audio-Format", "pcm_s16le");
    int status = http.sendRequest("POST", const_cast<uint8_t*>(pcm), size);
    http.end();
    return status == HTTP_CODE_ACCEPTED;
}

// ---- Buttons and recording ----
struct DebouncedButton {
    int8_t pin;
    bool stable = HIGH;
    bool raw = HIGH;
    uint32_t changedAt = 0;

    explicit DebouncedButton(int8_t gpio) : pin(gpio) {}
    void begin() {
        pinMode(pin, INPUT_PULLUP);
        stable = raw = digitalRead(pin);
    }
    int update() {
        bool now = digitalRead(pin);
        if (now != raw) {
            raw = now;
            changedAt = millis();
        }
        if (raw != stable && millis() - changedAt >= 35) {
            stable = raw;
            return stable == LOW ? 1 : -1;
        }
        return 0;
    }
};

static DebouncedButton keyEnter(PIN_KEY_ENTER);
static DebouncedButton keyUp(PIN_KEY_UP);
static DebouncedButton keyDown(PIN_KEY_DOWN);
static uint32_t enterPressedAt = 0;
static bool recording = false;
static bool recordSentWhileHeld = false;
static uint8_t* audioBuffer = nullptr;
static size_t audioBytes = 0;

static void drainMic() {
    int16_t discard[240];
    size_t bytes = 0;
    for (int i = 0; i < 8; i++) {
        if (i2s_read(I2S_NUM_0, discard, sizeof(discard), &bytes, 0) != ESP_OK || bytes == 0) break;
    }
}

static void micSelfTest() {
    drainMic();
    int16_t samples[240];
    uint32_t count = 0;
    uint64_t leftSum = 0, rightSum = 0;
    int leftPeak = 0, rightPeak = 0;
    uint32_t deadline = millis() + 1200;
    while (count < AUDIO_RATE * 2 && millis() < deadline) {
        size_t bytes = 0;
        if (i2s_read(I2S_NUM_0, samples, sizeof(samples), &bytes, pdMS_TO_TICKS(40)) != ESP_OK) continue;
        size_t got = bytes / sizeof(int16_t);
        for (size_t i = 0; i < got; i++) {
            int magnitude = samples[i] == INT16_MIN ? 32768 : abs((int)samples[i]);
            if ((count + i) & 1) {
                rightSum += magnitude;
                if (magnitude > rightPeak) rightPeak = magnitude;
            } else {
                leftSum += magnitude;
                if (magnitude > leftPeak) leftPeak = magnitude;
            }
        }
        count += got;
    }
    unsigned frames = count / 2;
    Serial.printf("@MIC frames=%u left_mean=%u left_peak=%d right_mean=%u right_peak=%d\n",
                  frames, frames ? (unsigned)(leftSum / frames) : 0, leftPeak,
                  frames ? (unsigned)(rightSum / frames) : 0, rightPeak);
}

static void beginRecording() {
    if (!audioBuffer) {
        audioBuffer = (uint8_t*)heap_caps_malloc(AUDIO_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    audioBytes = 0;
    if (!audioBuffer) {
        Serial.println("@ERROR audio_alloc");
        recordSentWhileHeld = true;
        return;
    }
    drainMic();
    recording = true;
    setIndicator(IndicatorMode::RECORDING);
    Serial.printf("@RECORDING %s\n", voiceTarget.c_str());
}

static void pumpRecording() {
    if (!recording || !audioBuffer) return;
    size_t room = AUDIO_MAX_BYTES - audioBytes;
    if (room == 0) return;
    int16_t stereo[480];
    size_t got = 0;
    if (i2s_read(I2S_NUM_0, stereo, sizeof(stereo), &got, pdMS_TO_TICKS(8)) == ESP_OK) {
        size_t samples = got / sizeof(int16_t);
        int16_t* mono = reinterpret_cast<int16_t*>(audioBuffer + audioBytes);
        size_t frames = min(samples / 2, room / sizeof(int16_t));
        for (size_t i = 0; i < frames; i++) mono[i] = stereo[i * 2];
        audioBytes += frames * sizeof(int16_t);
    }
}

static void sendRecording() {
    recording = false;
    if (audioBytes < AUDIO_RATE / 2) {
        Serial.println("@ERROR audio_too_short");
        audioBytes = 0;
        setIndicator(IndicatorMode::OFF);
        playBeep(300, 70);
        return;
    }
    setIndicator(IndicatorMode::PROCESSING);
    bool sentBle = uploadAudioBle(audioBuffer, audioBytes);
    bool sentWireless = sentBle || uploadAudioWireless(audioBuffer, audioBytes);
    if (sentBle) {
        Serial.printf("@TRANSPORT ble bytes=%u\n", (unsigned)audioBytes);
    } else if (sentWireless) {
        Serial.printf("@TRANSPORT wifi bytes=%u\n", (unsigned)audioBytes);
    } else {
        Serial.printf("@AUDIO %u %u %s\n", (unsigned)audioBytes, (unsigned)AUDIO_RATE, voiceTarget.c_str());
        size_t sent = 0;
        while (sent < audioBytes) {
            size_t chunk = min((size_t)4096, audioBytes - sent);
            sent += Serial.write(audioBuffer + sent, chunk);
            delay(0);
        }
        Serial.print("\n@AUDIO_END\n");
        Serial.flush();
    }
    audioBytes = 0;
    playBeep(900, 80);
}

static void changePage(int delta) {
    currentPage = (currentPage + delta + PAGE_COUNT) % PAGE_COUNT;
    screenDirty = true;
    urgentRender = true;
}

static void toggleVoiceTarget() {
    voiceTarget = voiceTarget == "codex" ? "kimi" : "codex";
    Serial.printf("@TARGET %s\n", voiceTarget.c_str());
    screenDirty = true;
    urgentRender = true;
    playBeep(voiceTarget == "codex" ? 700 : 1050, 60);
}

static void pollButtons() {
    int upEvent = keyUp.update();
    int downEvent = keyDown.update();
    int enterEvent = keyEnter.update();
    if (upEvent == 1 && !recording) changePage(-1);
    if (downEvent == 1 && !recording) changePage(1);

    if (enterEvent == 1) {
        enterPressedAt = millis();
        recordSentWhileHeld = false;
    }
    if (keyEnter.stable == LOW && !recording && !recordSentWhileHeld &&
        millis() - enterPressedAt >= 350) {
        beginRecording();
    }
    if (recording) {
        pumpRecording();
        if (audioBytes >= AUDIO_MAX_BYTES) {
            sendRecording();
            recordSentWhileHeld = true;
        }
    }
    if (enterEvent == -1) {
        if (recording) sendRecording();
        else if (!recordSentWhileHeld && millis() - enterPressedAt < 350) toggleVoiceTarget();
    }
}

void setup() {
    Serial.begin(115200);
    pinMode(PIN_LED_GREEN, OUTPUT);
    digitalWrite(PIN_LED_GREEN, HIGH);
    display.begin();
    u8g2.begin(display);
    u8g2.setFontMode(1);
    u8g2.setFontDirection(0);
    u8g2.setForegroundColor(COL_BLACK);
    u8g2.setBackgroundColor(COL_WHITE);

    keyEnter.begin();
    keyUp.begin();
    keyDown.begin();
    loadWirelessConfig();
    if (wireless.valid && wireless.epoch >= 1700000000U && time(nullptr) < (time_t)wireless.epoch) {
        timeval tv = {(time_t)wireless.epoch, 0};
        settimeofday(&tv, nullptr);
    }
    startBle();
    nextWirelessPollAt = millis() + 5000;
    maintainWifi();
    audioReady = audioBegin();
    Serial.printf("[diag] audio=%s psram=%u\n", audioReady ? "OK" : "FAIL", (unsigned)ESP.getFreePsram());

    agentState.steps[0] = {"等待任务", "active"};
    agentState.stepCount = 1;
    renderScreen();
    lastRenderAt = millis();
    Serial.println("READY");
    if (audioReady) playBeep(760, 60);
}

void loop() {
    static String input;
    updateIndicator();
    pollButtons();
    processBleJson();
    maintainWifi();
    if (!recording) pollWireless();

    // Host only writes JSON lines; device-to-host raw PCM uses the opposite direction.
    while (!recording && Serial.available()) {
        char ch = (char)Serial.read();
        if (ch == '\n') {
            input.trim();
            if (input == "PING") Serial.println("OK");
            else if (input == "INFO") {
                String ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "--";
                Serial.printf("@INFO audio=%s psram=%u page=%d target=%s keys=%d,%d,%d ble=%s wifi=%s ip=%s tls_time=%s http=%d seq=%u\n",
                              audioReady ? "OK" : "FAIL", (unsigned)ESP.getFreePsram(), currentPage,
                              voiceTarget.c_str(), digitalRead(PIN_KEY_ENTER), digitalRead(PIN_KEY_UP),
                              digitalRead(PIN_KEY_DOWN), bleAuthorized ? "AUTHORIZED" : (bleConnected ? "CONNECTED" : "WAIT"),
                              WiFi.status() == WL_CONNECTED ? "OK" :
                              (wireless.valid ? "CONNECTING" : "OFF"), ip.c_str(), tlsClockReady() ? "OK" : "WAIT",
                              lastWirelessHttpStatus, (unsigned)wirelessSeq);
            }
            else if (input == "MIC_TEST") micSelfTest();
            else if (input.startsWith("WIFI_CONFIG ")) {
                bool ok = saveWirelessConfig(input.substring(12));
                Serial.println(ok ? "OK wifi_config" : "ERR wifi_config");
            }
            else if (input == "WIFI_CLEAR") {
                Preferences prefs;
                bool ok = prefs.begin("zectrixwifi", false);
                if (ok) { prefs.clear(); prefs.end(); }
                wireless = WirelessConfig();
                WiFi.disconnect(true, true);
                Serial.println(ok ? "OK wifi_clear" : "ERR wifi_clear");
            }
            else if (input.length()) handleJson(input);
            input = "";
        } else if (ch != '\r') {
            if (input.length() < 12288) input += ch;
            else input = "";
        }
    }

    if (!recording && screenDirty) {
        uint32_t minimumWait = urgentRender ? 250 : 6000;
        if (millis() - lastRenderAt >= minimumWait) renderScreen();
    }
    delay(2);
}
