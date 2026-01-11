#include <M5Atom.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// Audio Libraries
#include "AudioFileSourceICYStream.h"
#include "AudioFileSourceBuffer.h"
#include "AudioFileSourcePROGMEM.h" // 埋め込み音声用
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

// System
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp32-hal-cpu.h"

// BLE Libraries
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ★ sound.h が手元にある場合はここを生かし、下のダミー配列を削除してください
#include "sound.h"

// (ダミー) 元の sound.h がない場合のコンパイル用ダミーデータ
// 実際にはご自身の sound.h の sound_data を使用してください


// ==========================================
// 設定 (統合版)
// ==========================================
// Wi-Fi
const char* SSID     = "YOUR_SSID";
const char* PASSWORD = "YOUR_WIFI_PASS";

// MQTT
const char* MQTT_SERVER = "broker.emqx.io";
const int   MQTT_PORT   = 1883;
const char* MQTT_TOPIC  = "YOUR_TOPIC";
const char* SECRET_PASS = "YOUR_SECRET_PASS";

// API
const char* AUDIO_API_URL = "YOUR_API";
const char* DEVICE_ID     = "YOUR_ID";

// BLE
#define BLE_DEVICE_NAME "YOUR_DEVICE_NAME"
#define BLE_SERVICE_UUID "YOUR_ID"

// LED Colors
uint32_t COLOR_WAIT = 0x000500; // 緑 (待機)
uint32_t COLOR_NET  = 0x000010; // 青 (ネット再生)
uint32_t COLOR_ERR  = 0x050000; // 赤 (エラー)
uint32_t COLOR_EMG  = 0x100010; // 紫 (緊急モード)

// ==========================================

WiFiClient espClient;
PubSubClient client(espClient);

// Audio Objects
AudioGeneratorMP3        *mp3 = NULL;
AudioOutputI2S           *out = NULL;

// Sources (排他利用)
AudioFileSourceICYStream *fileUrl = NULL;     // ネット用
AudioFileSourceBuffer    *buff    = NULL;     // ネット用バッファ
AudioFileSourcePROGMEM   *fileProg = NULL;    // 内蔵メモリ用

// State Flags
bool isPlaying = false;
bool bleActive = false;
unsigned long bleStartTime = 0;

// Button Logic Control
uint8_t       clickCount   = 0;
unsigned long lastClickMs  = 0;
const uint32_t CLICK_WINDOW = 500; // 連打判定時間
bool longPressDetected = false;    // 長押し処理済みフラグ

// 前方宣言
bool requestAndPlayFromApi(int patternId);
void playUrl(const char* url);
void playEmbeddedSound();
void stopPlaying();
void startEmergencyMode();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void reconnect();

void setup() {
  // 1. CPU & Brownout 設定
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  setCpuFrequencyMhz(240);

  M5.begin(true, false, true);
  Serial.begin(115200);
  M5.dis.setBrightness(20);

  Serial.println("\n==== M5 Atom Hybrid (WiFi Player + BLE Beacon) ====");

  // 2. BLE初期化 (まだStartしない)
  BLEDevice::init(BLE_DEVICE_NAME);
  BLEServer *pServer = BLEDevice::createServer();
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(BLE_SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  Serial.println("[BLE] Initialized (Standby)");

  // 3. Wi-Fi接続
  Serial.print("[WiFi] Connecting");
  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID, PASSWORD);
  WiFi.setSleep(false); // 省電力OFF

  int retry = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    M5.dis.drawpix(0, 0x050500); // 接続中は黄色っぽい色
    retry++;
    if (retry > 40) {
      Serial.println("\n[WiFi] Failed. Restarting...");
      ESP.restart();
    }
  }
  Serial.println("\n[WiFi] Connected!");
  Serial.print("IP: "); Serial.println(WiFi.localIP());

  // 4. Audio Output 初期化
  out = new AudioOutputI2S();
  out->SetGain(1.0);
  out->SetPinout(19, 33, 22);

  // MQTT Init
  client.setServer(MQTT_SERVER, MQTT_PORT);
  client.setCallback(mqttCallback);

  M5.dis.drawpix(0, COLOR_WAIT);
}

void loop() {
  M5.update();
  client.loop();

  // Wi-Fi再接続 (再生中・緊急モード中は避ける)
  if (!client.connected() && !isPlaying && !bleActive) {
    reconnect();
  }

  // BLE タイムアウト処理 (10秒後に停止)
  if (bleActive && millis() - bleStartTime > 10000) {
    BLEDevice::stopAdvertising();
    bleActive = false;
    Serial.println("[BLE] Stopped (Timeout)");
    
    // 再生も止めるなら stopPlaying() を呼ぶ。
    // 今回は音声再生が終わるまで待つか、BLE停止と同時に音も止めるか選択可。
    // ここでは音は最後まで流す仕様にします。
    if (!isPlaying) M5.dis.drawpix(0, COLOR_WAIT);
  }

  // Audio Loop
  if (mp3 && mp3->isRunning()) {
    if (!mp3->loop()) {
      stopPlaying(); // 再生終了
      Serial.println("[AUDIO] Playback Finished");
      if (bleActive) M5.dis.drawpix(0, COLOR_EMG);
      else M5.dis.drawpix(0, COLOR_WAIT);
    }
  }

  // ==========================================
  // ボタン処理統合ロジック
  // ==========================================
  
  // 1. 長押し検知 (2000ms = 2秒)
  // M5.Btn.pressedFor(ms) は「現在ms以上押されているか」を返す
  if (M5.Btn.pressedFor(2000)) {
    if (!longPressDetected) {
      // まだ長押し処理をしていない場合のみ実行
      Serial.println(">>> LONG PRESS DETECTED: EMERGENCY MODE <<<");
      startEmergencyMode();
      longPressDetected = true; // 処理済みフラグを立てる
      clickCount = 0;           // クリックカウントはキャンセル
    }
  }

  // 2. リリース検知 (ボタンを離した瞬間)
  if (M5.Btn.wasReleased()) {
    if (longPressDetected) {
      // 長押し操作の終了時 -> 何もしないがフラグはリセット
      longPressDetected = false; 
      Serial.println("[BTN] Long press released.");
    } else {
      // 短押しだった場合 -> カウントアップ
      // 緊急モード中や再生中なら無視するか、あるいは停止などの操作に割り当てる
      if (bleActive) {
         Serial.println("[BTN] Ignored (In Emergency Mode)");
      } else if (isPlaying) {
         Serial.println("[BTN] Ignored (Already Playing)");
      } else {
         clickCount++;
         lastClickMs = millis();
         Serial.printf("[BTN] Short Click: %d\n", clickCount);
      }
    }
  }

  // 3. クリック確定待ち (Device Aのロジック)
  if (!isPlaying && !bleActive && clickCount > 0 && !longPressDetected) {
    if (millis() - lastClickMs > CLICK_WINDOW) {
      // 確定
      int patternId = clickCount;
      if (patternId < 1) patternId = 1;
      if (patternId > 3) patternId = 3;

      Serial.printf("[BTN] Sequence finished: pattern_id=%d\n", patternId);
      
      bool ok = requestAndPlayFromApi(patternId);
      if (!ok) {
        M5.dis.drawpix(0, COLOR_ERR);
        delay(1000);
        M5.dis.drawpix(0, COLOR_WAIT);
      }
      clickCount = 0; // リセット
    }
  }
}

// ==========================================
// 統合制御関数
// ==========================================

// 緊急モード起動
void startEmergencyMode() {
  // 1. 再生中のネット音声があれば止める
  stopPlaying();

  // 2. BLE発信開始
  if (!bleActive) {
    BLEDevice::startAdvertising();
    bleActive = true;
    bleStartTime = millis();
    Serial.println("[BLE] Signal Started!");
  }

  // 3. LED変更
  M5.dis.drawpix(0, COLOR_EMG);

  // 4. 内蔵音声再生
  playEmbeddedSound();
}

// 埋め込み音声再生 (Device B)
void playEmbeddedSound() {
  stopPlaying(); // クリーンアップ

  Serial.println("[AUDIO] Playing Embedded Sound...");
  
  fileProg = new AudioFileSourcePROGMEM(sound_data, sound_data_len);
  mp3 = new AudioGeneratorMP3();
  
  if (!fileProg || !mp3) {
    Serial.println("[AUDIO] Failed to create embedded objects");
    return;
  }

  mp3->begin(fileProg, out);
  isPlaying = true;
}

// ネット音声再生 (Device A)
void playUrl(const char* url) {
  stopPlaying(); // クリーンアップ

  Serial.println("[AUDIO] Setting up Stream...");
  M5.dis.drawpix(0, COLOR_NET);

  fileUrl = new AudioFileSourceICYStream(url);
  // Buffer: 20KB
  buff = new AudioFileSourceBuffer(fileUrl, 20480);
  mp3 = new AudioGeneratorMP3();

  if (!fileUrl || !buff || !mp3) {
     Serial.println("[AUDIO] Failed to create stream objects");
     M5.dis.drawpix(0, COLOR_ERR);
     return;
  }

  mp3->begin(buff, out);
  isPlaying = true;
}

// 全停止＆メモリ解放 (共通)
void stopPlaying() {
  Serial.println("[AUDIO] stopPlaying...");

  if (mp3) {
    if (mp3->isRunning()) mp3->stop();
    delete mp3; mp3 = NULL;
  }
  
  // Stream用オブジェクトの解放
  if (buff) { delete buff; buff = NULL; }
  if (fileUrl) { delete fileUrl; fileUrl = NULL; }

  // Embedded用オブジェクトの解放
  if (fileProg) { delete fileProg; fileProg = NULL; }

  isPlaying = false;
}

// API リクエスト (Device A)
bool requestAndPlayFromApi(int patternId) {
  if (WiFi.status() != WL_CONNECTED) return false;

  Serial.println("[API] Requesting...");
  
  WiFiClientSecure secureClient;
  secureClient.setInsecure(); 

  HTTPClient http;
  http.setTimeout(3000);

  if (!http.begin(secureClient, AUDIO_API_URL)) return false;

  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument reqDoc(256);
  reqDoc["device_id"]  = DEVICE_ID;
  reqDoc["pattern_id"] = patternId;

  String body;
  serializeJson(reqDoc, body);

  int httpCode = http.POST(body);
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[API] Error: %d\n", httpCode);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  DynamicJsonDocument resDoc(1024);
  deserializeJson(resDoc, payload);

  const char* audioUrl = resDoc["audio_url"];
  if (!audioUrl || strlen(audioUrl) == 0) return false;

  playUrl(audioUrl);
  return true;
}

// MQTT (Device A)
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // 必要であればここにMQTT受信処理
  // 緊急モード中は無視するなどのガードを入れても良い
}

void reconnect() {
  if (client.connect("M5Atom-Hybrid")) {
     client.subscribe(MQTT_TOPIC);
     Serial.println("[MQTT] Connected");
  }
}