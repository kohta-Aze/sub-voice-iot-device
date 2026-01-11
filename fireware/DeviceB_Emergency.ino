#include <M5Atom.h>
#include <AudioGeneratorMP3.h>
#include "AudioOutputI2S.h"
#include "AudioFileSourcePROGMEM.h" // 埋め込み音声用

// BLEライブラリ
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// 音声データ読み込み
#include "sound.h"

// ==========================================
// 設定
// ==========================================
#define BLE_DEVICE_NAME "NIF_EMERGENCY" // スマホで見える名前

// ==========================================

AudioGeneratorMP3 *mp3 = NULL;
AudioFileSourcePROGMEM *file = NULL;
AudioOutputI2S *out;

// 変数
bool bleActive = false;
unsigned long bleStartTime = 0;

// LED定義
uint32_t COLOR_WAIT = 0x000500; // 緑 (待機)
uint32_t COLOR_EMG  = 0x100010; // 紫 (緊急モード: 音声+BLE)

void setup() {
  M5.begin(true, false, true); // Wi-Fi OFFで爆速起動
  Serial.begin(115200);
  M5.dis.setBrightness(20); 

  // BLE初期化 (サーバー準備のみ)
  BLEDevice::init(BLE_DEVICE_NAME);
  BLEServer *pServer = BLEDevice::createServer();
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  
  // UUID設定 (これを目印にする)
  pAdvertising->addServiceUUID("YOUR_UUID");
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06); 
  
  // Audio初期化
  out = new AudioOutputI2S();
  out->SetGain(3.0);
  out->SetPinout(19, 33, 22);

  M5.dis.drawpix(0, COLOR_WAIT);
  Serial.println("Device B (Emergency Beacon) Ready.");
}

void loop() {
  M5.update();

  // BLE自動停止 (10秒後に止める)
  if (bleActive && millis() - bleStartTime > 10000) {
    BLEDevice::stopAdvertising();
    bleActive = false;
    Serial.println("BLE Stopped (Timeout)");
    M5.dis.drawpix(0, COLOR_WAIT);
  }

  // 再生処理
  if (mp3 && mp3->isRunning()) {
    if (!mp3->loop()) {
      stopPlaying();
      Serial.println("Playback Finished");
      // BLE中は紫のまま
      if (bleActive) M5.dis.drawpix(0, COLOR_EMG);
      else M5.dis.drawpix(0, COLOR_WAIT);
    }
  }

  // ボタンが押されたら「緊急モード」発動
  if (M5.Btn.wasPressed()) {
    Serial.println(">>> EMERGENCY BUTTON PRESSED! <<<");
    startEmergencyMode();
  }
}

// 緊急モード開始 (BLE発信 + 音声再生)
void startEmergencyMode() {
  // 1. BLE開始
  if (!bleActive) {
    BLEDevice::startAdvertising();
    bleActive = true;
    bleStartTime = millis();
    Serial.println("BLE Signal Started!");
  }
  
  // 2. 音声再生 (埋め込みデータ)
  playEmbeddedSound();
  
  // 3. LEDを紫に
  M5.dis.drawpix(0, COLOR_EMG);
}

void playEmbeddedSound() {
  stopPlaying(); // 前のを止める
  
  file = new AudioFileSourcePROGMEM(sound_data, sound_data_len);
  mp3 = new AudioGeneratorMP3();
  mp3->begin(file, out);
}

void stopPlaying() {
  if (mp3) { mp3->stop(); delete mp3; mp3 = NULL; }
  if (file) { delete file; file = NULL; }
}