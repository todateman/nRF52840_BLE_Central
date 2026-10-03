// 参考 (Adafruit_nRF52_Arduino / Bluefruit52Lib 付属サンプル)
// https://github.com/adafruit/Adafruit_nRF52_Arduino/blob/master/libraries/Bluefruit52Lib/examples/Central/central_custom_hrm/central_custom_hrm.ino
// https://github.com/adafruit/Adafruit_nRF52_Arduino/blob/master/libraries/Bluefruit52Lib/examples/Central/central_bleuart_multi/central_bleuart_multi.ino
// https://github.com/adafruit/Adafruit_nRF52_Arduino/blob/master/libraries/Bluefruit52Lib/examples/Central/central_scan_advanced/central_scan_advanced.ino
// https://github.com/adafruit/Adafruit_nRF52_Arduino/blob/master/libraries/Wire/examples/secondary_receiver/secondary_receiver.ino
// https://github.com/adafruit/Adafruit_nRF52_Arduino/blob/master/libraries/Wire/examples/secondary_sender/secondary_sender.ino

#include <Arduino.h>
#include <bluefruit.h>
#include <Wire.h>

// I2C(Wire)はXiao nRF52840の既定ピン(D4=SDA, D5=SCL)をそのまま使う。
// Grove非搭載のため、M5Stack Basicへは直結配線 or Xiao Expansion Board等を介して接続する
#define I2C_SLAVE_ADDR 0x08
#define I2C_FRAME_SIZE 32  // 1byte長さ + 最大31byteデータ

// マスターが要求するデータの種類を表すコマンド
#define CMD_ENGINE_TEMP 0x01
#define CMD_PRI_PRE 0x02
#define CMD_SEC_PRE 0x03
#define CMD_FUEL_PRE 0x04

// M5Core2 (Chibi-T_Furoshiki_AutoAirAdjust) は1本のNotify Characteristicで
// 3種類のセンサーデータを送るため、先頭のタグ文字列で種別を判別する
#define TAG_PRI_PRE "PRI:"
#define TAG_SEC_PRE "SEC:"
#define TAG_FUEL_PRE "FUEL:"

// オンボードLEDでBLE接続状態を表示する(Xiao nRF52840はGrove非搭載のため
// 外付けLEDではなく内蔵LEDを流用)。variant.hは LED_STATE_ON=1 (active-high)と
// 定義しているが、実機検証の結果、赤色(LED_RED)・青色(LED_BLUE)ともに実際には
// active-low(LOWを出力すると点灯)であることが判明した(LED_STATE_ONで点灯させたつもりが
// 消灯し、!LED_STATE_ONで消灯させたつもりが点灯する現象を確認)。
// variant.hの定義自体は他ライブラリへの影響を避けるため変更せず、
// 本ファイル内でのみ実機の実際の極性に合わせたLED_ON/LED_OFFを定義し、
// 以降はLED_STATE_ONを直接使わずこちらを使う。
// 表示方式は赤=Heater/AutoAirAdjustのいずれか未接続、青=両方接続完了の排他点灯
// (詳細はupdateConnectionLed()を参照)
#define LED_ON  (!LED_STATE_ON)
#define LED_OFF (LED_STATE_ON)
#define BLUE_LED_PIN LED_BLUE

// ServerのBLE サービスとキャラクタリスティックのUUIDを定義 https://www.uuidgenerator.net/version4
// SERVICE_UUIDはHeater/AutoAirAdjustで共通のため、接続先の判別はアドバタイズ名で行う
#define SERVICE_UUID "7c44181A-c1a4-4635-a119-b490ed272552"

// Chibi-T_Furoshiki_Heater (M5DinMeter) 用キャラクタリスティック
#define HEATER_DEVICE_NAME "M5Din Furoshiki Heater"
#define HEATER_CHARACTERISTIC_UUID "7c442A00-c1a4-4635-a119-b490ed272552"
#define HEATER_NOTIFY_CHARACTERISTIC_UUID "7c442A6E-c1a4-4635-a119-b490ed272552"

// Chibi-T_Furoshiki_AutoAirAdjust (M5Core2) 用キャラクタリスティック
#define AUTOAIR_DEVICE_NAME "ChibiT-AutoAirAdjust"
#define AUTOAIR_CHARACTERISTIC_UUID "c9f878f1-c311-4452-ae5e-e813b4fe057d"
#define AUTOAIR_NOTIFY_CHARACTERISTIC_UUID "1d25ec49-e19c-4bb6-8c36-5dc8d8aaaebe"

static const size_t RX_BUFFER_MAX = 256;  // Notify受信バッファの最大サイズ

// 古いデータを返し続けないための監視しきい値(loop()で判定)
#define FRAME_STALE_MS 3000       // この時間更新されないフレームは空にする(マスター側で無効値扱いになる)
#define NOTIFY_SILENCE_MS 5000    // 接続中にこの時間Notifyが来なければ切断して再接続させる
#define CONNECT_TIMEOUT_MS 10000  // 接続試行(STATE_DO_CONNECT)がこの時間解決しなければ打ち切る
#define WATCHDOG_INTERVAL_MS 100  // loop()での監視周期

// 各データのI2C送信用フレーム（先頭1byteが長さ、以降が実データ）。M5Stack Basicからの要求時に最新値を返す
static uint8_t engineTempFrame[I2C_FRAME_SIZE] = {0};
static uint8_t priPreFrame[I2C_FRAME_SIZE] = {0};
static uint8_t secPreFrame[I2C_FRAME_SIZE] = {0};
static uint8_t fuelPreFrame[I2C_FRAME_SIZE] = {0};

// 各フレームの最終更新時刻(millis)。FRAME_STALE_MSを超えたフレームはloop()で空にする
static uint32_t engineTempUpdatedAt = 0;
static uint32_t priPreUpdatedAt = 0;
static uint32_t secPreUpdatedAt = 0;
static uint32_t fuelPreUpdatedAt = 0;

#define STATE_IDLE 0
#define STATE_DO_CONNECT 1
#define STATE_CONNECTED 3

#define PERIPH_HEATER 0
#define PERIPH_AUTOAIR 1
#define PERIPH_COUNT 2

// ペリフェラル1台分の接続状態・UUID・BLEオブジェクトをまとめて保持する
// (Heater/AutoAirAdjustの2台構成に特化しており、3台目以降への汎用対応は意図しない)
struct PeripheralContext
{
  const char *name;                        // アドバタイズ名（判別用）
  BLEClientService service;                // 共通SERVICE_UUIDのサービス
  BLEClientCharacteristic charac;          // 存在確認用キャラクタリスティック
  BLEClientCharacteristic notifyCharac;    // Notifyキャラクタリスティック
  uint16_t connHandle;                     // 接続確立後のコネクションハンドル
  int8_t state;                            // STATE_IDLE / STATE_DO_CONNECT / STATE_CONNECTED
  String rxBuffer;                         // このペリフェラル専用のNotify受信バッファ
  uint32_t connectStartedAt;               // STATE_DO_CONNECTにした時刻(接続試行タイムアウト判定用)
  uint32_t lastNotifyAt;                   // 最後にメッセージを受信した時刻(Notify途絶判定用)
};

// Service UUIDはHeater/AutoAirAdjustで共通のため1つを使い回す(Scannerのフィルタにも使用)
static BLEUuid serviceUuid(SERVICE_UUID);

static PeripheralContext peripherals[PERIPH_COUNT] = {
    // Heater: エンジン温度(タグなし文字列)を送信
    {HEATER_DEVICE_NAME, BLEClientService(serviceUuid),
     BLEClientCharacteristic(HEATER_CHARACTERISTIC_UUID),
     BLEClientCharacteristic(HEATER_NOTIFY_CHARACTERISTIC_UUID),
     BLE_CONN_HANDLE_INVALID, STATE_IDLE, "", 0, 0},
    // AutoAirAdjust: 1次/2次側空気圧・燃圧(PRI:/SEC:/FUEL:タグ付き)を送信
    {AUTOAIR_DEVICE_NAME, BLEClientService(serviceUuid),
     BLEClientCharacteristic(AUTOAIR_CHARACTERISTIC_UUID),
     BLEClientCharacteristic(AUTOAIR_NOTIFY_CHARACTERISTIC_UUID),
     BLE_CONN_HANDLE_INVALID, STATE_IDLE, "", 0, 0},
};

// フレームを長さ0(データなし)にする。マスター側は長さ0を無効値として扱い、
// 一定時間後にタイムアウト処理(0表示)へ移行するため、古い値を返し続けずに済む。
// receiveEvent(ISR)との競合を避けるためクリティカルセクションで書き換える
// (taskENTER_CRITICALはSoftDeviceの割り込みを止めずに、優先度3のTWIS割り込みはマスクできる)
static void clearFrame(uint8_t *frame)
{
  taskENTER_CRITICAL();
  memset(frame, 0, I2C_FRAME_SIZE - 1);  // 末尾1byteはコマンドエコー用に予約
  taskEXIT_CRITICAL();
}

// 指定ペリフェラルの状態を初期化し、次のscanCallback()で再発見できるようにする。
// 切断後も最後の値を返し続けないよう、そのペリフェラルが送ってくるデータのフレームも空にする
static void resetPeripheral(int idx)
{
  PeripheralContext &p = peripherals[idx];
  p.connHandle = BLE_CONN_HANDLE_INVALID;
  p.rxBuffer = "";
  p.state = STATE_IDLE;
  // service/characteristicの探索状態はBluefruitが切断イベントで内部的にリセットするため、
  // ESP32-C6版のようなヒープ管理(delete)は不要

  if (idx == PERIPH_HEATER)
  {
    clearFrame(engineTempFrame);
  }
  else
  {
    clearFrame(priPreFrame);
    clearFrame(secPreFrame);
    clearFrame(fuelPreFrame);
  }
}

// 未接続(STATE_CONNECTED以外)のペリフェラルが1台でも残っていればスキャンが必要
static bool needsScan()
{
  for (int i = 0; i < PERIPH_COUNT; i++)
  {
    if (peripherals[i].state != STATE_CONNECTED)
    {
      return true;
    }
  }
  return false;
}

// 未接続のペリフェラルが残っているのにスキャナーが止まっていれば再開する。
// Bluefruit.Scanner.restartOnDisconnect(true)は「Central接続が1台も無くなった時」にしか
// 自動再開しないため、AutoAirAdjust接続中にHeaterだけ切断された場合などはここで再開しないと
// 二度と再接続されない(温度だけが更新されなくなる不具合の原因だった)
static void ensureScanning()
{
  if (!needsScan() || Bluefruit.Scanner.isRunning())
  {
    return;
  }
  for (int i = 0; i < PERIPH_COUNT; i++)
  {
    if (peripherals[i].state == STATE_DO_CONNECT)
    {
      return;  // 接続試行中はスキャナーが一時停止しているのが正常なので触らない
    }
  }
  Serial.println("Restarting scanner");
  Bluefruit.Scanner.start(0);
}

// 接続状態表示LEDを更新する。Heater/AutoAirAdjustの両方が接続完了していれば
// 青色のみ、いずれか未接続なら赤色のみを点灯する(排他点灯)。
// setup()での初期化直後、および接続/切断イベント発生時(connectCallback/disconnectCallback)に呼ばれる
static void updateConnectionLed()
{
  bool allConnected = !needsScan();
  // 診断用ログ: 呼ばれるたびに各ペリフェラルの状態を出力する
  // (state値: STATE_IDLE=0 / STATE_DO_CONNECT=1 / STATE_CONNECTED=3)
  Serial.printf("[LED] %s=%d %s=%d -> %s\n",
                peripherals[PERIPH_HEATER].name, peripherals[PERIPH_HEATER].state,
                peripherals[PERIPH_AUTOAIR].name, peripherals[PERIPH_AUTOAIR].state,
                allConnected ? "BLUE(connected)" : "RED(not connected)");
  digitalWrite(LED_RED, allConnected ? LED_OFF : LED_ON);
  digitalWrite(BLUE_LED_PIN, allConnected ? LED_ON : LED_OFF);
}

// アドバタイズ名からペリフェラルのインデックスを引く
static int findPeripheralByName(const char *name)
{
  for (int i = 0; i < PERIPH_COUNT; i++)
  {
    if (strcmp(name, peripherals[i].name) == 0)
    {
      return i;
    }
  }
  return -1;
}

// 文字列データを長さプレフィックス付きのI2C送信用フレームに変換して格納する。
// 末尾1byte(frame[I2C_FRAME_SIZE-1])はコマンドエコー用に予約し、ここでは触れない
// (実際の値はreceiveEventが書き込み直前に設定する。マスター側でコマンドと応答のズレを
//  検知できるようにするための仕組み)
// receiveEvent(ISR)が書き換え途中のフレームを読まないよう、clearFrame()と同じく
// クリティカルセクションで書き換え、更新時刻(updatedAt)も記録する
static void updateFrame(uint8_t *frame, uint32_t &updatedAt, const String &value)
{
  size_t dataLen = min(value.length(), (size_t)(I2C_FRAME_SIZE - 2));
  taskENTER_CRITICAL();
  frame[0] = (uint8_t)dataLen;
  memcpy(&frame[1], value.c_str(), dataLen);
  if (dataLen < I2C_FRAME_SIZE - 2)
  {
    memset(&frame[1 + dataLen], 0, I2C_FRAME_SIZE - 2 - dataLen);
  }
  taskEXIT_CRITICAL();
  updatedAt = millis();
}

static void notifyCallback(BLEClientCharacteristic *chr, uint8_t *data, uint16_t length)
{
  // どちらのペリフェラルからのNotifyかをキャラクタリスティックのポインタで判別する
  int idx = -1;
  for (int i = 0; i < PERIPH_COUNT; i++)
  {
    if (chr == &peripherals[i].notifyCharac)
    {
      idx = i;
      break;
    }
  }
  if (idx < 0)
  {
    return;  // 想定外のCharacteristic（基本的に発生しない）
  }
  String &rxBuffer = peripherals[idx].rxBuffer;

  Serial.print("Notify callback for characteristic ");
  Serial.print(chr->uuid.toString());
  Serial.print(" of data length ");
  Serial.println(length);

  // 受信データをバッファに追加
  for (size_t i = 0; i < length; i++)
  {
    char c = (char)data[i];

    if (c == '\n')
    {
      // 改行を受信 → メッセージ完成
      Serial.print("Complete message: ");
      Serial.println(rxBuffer.c_str());
      peripherals[idx].lastNotifyAt = millis();  // Notify途絶判定用

      // 先頭のタグでデータ種別を判別し、対応するフレームを更新する
      // （タグが無い場合は従来どおりCMD_ENGINE_TEMP用として扱う）
      if (rxBuffer.startsWith(TAG_PRI_PRE))
      {
        updateFrame(priPreFrame, priPreUpdatedAt, rxBuffer.substring(strlen(TAG_PRI_PRE)));
        Serial.println("I2C frame updated: PRI_PRE");
      }
      else if (rxBuffer.startsWith(TAG_SEC_PRE))
      {
        updateFrame(secPreFrame, secPreUpdatedAt, rxBuffer.substring(strlen(TAG_SEC_PRE)));
        Serial.println("I2C frame updated: SEC_PRE");
      }
      else if (rxBuffer.startsWith(TAG_FUEL_PRE))
      {
        updateFrame(fuelPreFrame, fuelPreUpdatedAt, rxBuffer.substring(strlen(TAG_FUEL_PRE)));
        Serial.println("I2C frame updated: FUEL_PRE");
      }
      else
      {
        updateFrame(engineTempFrame, engineTempUpdatedAt, rxBuffer);
        Serial.println("I2C frame updated: ENGINE_TEMP");
      }

      // バッファクリア
      rxBuffer = "";
    }
    else if (c != '\r')
    {
      // キャリッジリターンは無視、その他の文字をバッファに追加
      if (rxBuffer.length() < RX_BUFFER_MAX)
      {
        rxBuffer += c;
      }
      else
      {
        // バッファオーバーフロー対策
        Serial.println("ERROR: RX buffer overflow!");
        rxBuffer = "";
      }
    }
  }

  if (rxBuffer.length() > 0)
  {
    Serial.print("Buffered data: ");
    Serial.println(rxBuffer.c_str());
  }
}

// サービス・キャラクタリスティックを探索しNotify購読を行う。接続確立後にconnectCallbackから呼ばれる
static bool discoverAndSubscribe(PeripheralContext &p, uint16_t conn_handle)
{
  if (!p.service.discover(conn_handle))
  {
    Serial.printf("[%s] Failed to find our service UUID: %s\n", p.name, SERVICE_UUID);
    return false;
  }
  Serial.printf("[%s] - Found our service\n", p.name);

  if (!p.charac.discover())
  {
    Serial.printf("[%s] Failed to find our characteristic UUID\n", p.name);
    return false;
  }
  Serial.printf("[%s] - Found our characteristic\n", p.name);

  if (!p.notifyCharac.discover())
  {
    Serial.printf("[%s] Failed to find our notify characteristic UUID\n", p.name);
    return false;
  }

  p.notifyCharac.enableNotify();
  Serial.printf("[%s] - Registered for notify\n", p.name);
  return true;
}

// 接続確立時に呼ばれる。接続先の判別は、スキャン時点(scanCallback)で広告データの
// Complete Local Nameから既に確定させ、STATE_DO_CONNECTにマークしておいた
// ペリフェラルをそのまま使う。
// (以前はここでBLEConnection::getPeerName()によりGATT経由で標準GAP Device Name
//  キャラクタリスティック(0x2A00)をライブ読み出しして再判別していたが、本プロジェクトは
//  ATT MTUを既定値(23byte)のまま使っており、Read By Type応答が運べる値の最大長は
//  MTU-4=19byteしかない。"ChibiT-AutoAirAdjust"(20byte)・"M5Din Furoshiki Heater"(22byte)
//  はいずれも19byteを超えるため末尾が切り詰められて返ってきてしまい、findPeripheralByName()
//  が必ず不一致になって「Unexpected device connected」として即切断される不具合があった。
//  STATE_DO_CONNECTになり得るのは常に高々1台のみ(connect()呼び出し後はスキャナーが
//  一時停止し、次の接続試行はこの接続がconnectCallback/disconnectCallbackで解決してから
//  再開されるため)なので、これで一意に判別できる)
static void connectCallback(uint16_t conn_handle)
{
  int idx = -1;
  for (int i = 0; i < PERIPH_COUNT; i++)
  {
    if (peripherals[i].state == STATE_DO_CONNECT)
    {
      idx = i;
      break;
    }
  }

  if (idx < 0)
  {
    // 接続試行中のペリフェラルが1つも無い状態で接続イベントを受信した（基本的に発生しないが、
    // 念のため切断する）
    Serial.println("Unexpected connection (no pending connect target), disconnecting");
    Bluefruit.disconnect(conn_handle);
    return;
  }

  PeripheralContext &p = peripherals[idx];
  p.connHandle = conn_handle;

  if (discoverAndSubscribe(p, conn_handle))
  {
    p.lastNotifyAt = millis();  // Notify途絶判定は接続完了時点から計測する
    p.state = STATE_CONNECTED;
    Serial.printf("Connected to server (%s)\n", p.name);
  }
  else
  {
    Serial.printf("Failed to connect (%s)\n", p.name);
    Bluefruit.disconnect(conn_handle);
    resetPeripheral(idx);
  }

  updateConnectionLed();  // 接続状態表示LEDを更新（両方揃って初めて青色になる）

  // 未接続のペリフェラルが残っていればスキャンを継続する
  // (Central.connect()呼び出し後スキャナーは一時停止しているため、明示的な再開が必要)
  if (needsScan())
  {
    Bluefruit.Scanner.start(0);
  }
}

// 切断時に呼ばれる。切断されたペリフェラルのみ状態をリセットし、スキャンを再開する
// (Bluefruit.Scanner.restartOnDisconnect(true)は全Central接続が切れた時しか自動再開しないため、
//  もう一方が接続中のままでも再接続できるよう、ensureScanning()で明示的に再開する)
static void disconnectCallback(uint16_t conn_handle, uint8_t reason)
{
  for (int i = 0; i < PERIPH_COUNT; i++)
  {
    if (peripherals[i].connHandle == conn_handle)
    {
      Serial.printf("onDisconnect (%s), reason = 0x%02X\n", peripherals[i].name, reason);
      resetPeripheral(i);  // 再スキャンで再発見できるようにする
      break;
    }
  }

  updateConnectionLed();  // 未接続に戻ったので接続状態表示LEDを更新（黄色に戻す）
  ensureScanning();
}

// スキャン結果を受信するたびに呼ばれる。SoftDevice仕様上、レポート受信のたびに
// スキャナーは一時停止するため、接続を試みない場合は明示的にresume()する必要がある
static void scanCallback(ble_gap_evt_adv_report_t *report)
{
  // Service UUIDはHeater/AutoAirAdjust共通のため、アドバタイズ名(Complete Local Name)で
  // 判別する。名前はScan Response側に含まれるため、この判定にはアクティブスキャンが必須
  uint8_t nameBuf[32] = {0};
  uint8_t nameLen = Bluefruit.Scanner.parseReportByType(
      report, BLE_GAP_AD_TYPE_COMPLETE_LOCAL_NAME, nameBuf, sizeof(nameBuf));

  if (nameLen > 0)
  {
    int idx = findPeripheralByName((const char *)nameBuf);
    if (idx >= 0 && peripherals[idx].state == STATE_IDLE)
    {
      Serial.printf("Device found! (%s)\n", peripherals[idx].name);
      peripherals[idx].state = STATE_DO_CONNECT;
      peripherals[idx].connectStartedAt = millis();
      // 結果はconnectCallback/disconnectCallbackへ通知される。接続が成立しないまま
      // 終わらない場合はloop()がCONNECT_TIMEOUT_MSで打ち切る
      if (Bluefruit.Central.connect(report))
      {
        return;  // connect()呼び出し後はスキャナーが一時停止するのでresume()は不要
      }
      // 接続要求自体が失敗した場合はコールバックが来ないため、ここで元に戻してスキャンを続ける
      Serial.printf("connect() failed (%s)\n", peripherals[idx].name);
      peripherals[idx].state = STATE_IDLE;
    }
  }

  // 対象デバイスでない場合はスキャンを再開する
  Bluefruit.Scanner.resume();
}

// I2Cスレーブの応答フレーム。マスターがコマンドを書き込んだ時点(receiveEvent)で確定させ、
// 読み取り要求時(requestEvent)にはそのまま返すだけにする
static uint8_t txFrameBuffer[I2C_FRAME_SIZE];
static const uint8_t emptyFrame[I2C_FRAME_SIZE] = {0};

static const uint8_t *frameForCommand(uint8_t cmd)
{
  switch (cmd)
  {
  case CMD_ENGINE_TEMP:
    return engineTempFrame;
  case CMD_PRI_PRE:
    return priPreFrame;
  case CMD_SEC_PRE:
    return secPreFrame;
  case CMD_FUEL_PRE:
    return fuelPreFrame;
  default:
    return emptyFrame;  // 未定義コマンドは長さ0の空フレームを返す
  }
}

// M5Stack Basic（I2Cマスター）からのコマンド書き込み完了時に呼ばれる。
// nRF52のWireライブラリはonReceive/onRequestを真の割り込みコンテキストから直接呼ぶため、
// ここでは応答フレームの確定のみを行い、重い処理やSerial出力は避ける
static void receiveEvent(int numBytes)
{
  if (numBytes < 1)
  {
    return;
  }
  uint8_t cmd = Wire.read();
  while (Wire.available())
  {
    Wire.read();  // 想定外の余剰バイトは読み捨て
  }

  memcpy(txFrameBuffer, frameForCommand(cmd), I2C_FRAME_SIZE);
  // 末尾1byteに要求されたコマンドをエコーバックし、マスター側でズレを検知できるようにする
  txFrameBuffer[I2C_FRAME_SIZE - 1] = cmd;
}

// マスターの読み取り要求時に呼ばれる。nRF52のTWIS(スレーブ)ペリフェラルは、
// ソフトウェアがこのコールバックからTXバッファを準備し終えるまでハードウェアが
// 自動的にクロックストレッチを維持するため、ESP32-C6版で必要だった「間に合わない」
// 対策(タスク経由での事前送信・クロックストレッチ無効化・自己修復等)は不要になった
static void requestEvent()
{
  Wire.write(txFrameBuffer, I2C_FRAME_SIZE);
}

void setup()
{
  // Serial/BLE初期化より前の、最速の起動チェックポイント。ここでLED_REDが3回点滅しなければ
  // Arduino setup()にすら到達できていない(電源/書き込み自体の問題)と切り分けられる
  pinMode(LED_RED, OUTPUT);
  for (int i = 0; i < 3; i++)
  {
    digitalWrite(LED_RED, LED_ON);
    delay(100);
    digitalWrite(LED_RED, LED_OFF);
    delay(100);
  }

  Serial.begin(115200);
  // ネイティブUSB CDCは接続確立に時間がかかるため少し待つが、シリアル未接続でも
  // 本体は単体で動作し続ける必要があるためタイムアウト付きにする
  uint32_t serialWaitStart = millis();
  while (!Serial && (millis() - serialWaitStart) < 3000)
  {
    delay(10);
  }

  // 起動確認用バナー。BLEペリフェラル未発見・I2C要求未受信の間はこれ以降何もログが
  // 出ないため(scanCallback/receiveEvent/requestEventは対象デバイス発見時/通信時のみ
  // 出力する設計)、これが表示されればボード自体は正常に起動していると判断できる
  Serial.println("XiaoNRF52840 BLE Central starting...");
  Serial.printf("I2C slave addr=0x%02X (SDA=D4, SCL=D5)\n", I2C_SLAVE_ADDR);
  Serial.printf("Target peripherals: %s / %s\n", HEATER_DEVICE_NAME, AUTOAIR_DEVICE_NAME);

  // I2Cスレーブとして初期化し、M5Stack Basicからの要求に応答する
  // (nRF52840はI2Cマスター(TWIM)とスレーブ(TWIS)が別ペリフェラルのため、標準Wireの
  //  スレーブモードがそのまま使える。ESP32-C6版のESP-IDFネイティブAPI直接呼び出しは不要)
  Wire.begin(I2C_SLAVE_ADDR);
  Wire.onReceive(receiveEvent);
  Wire.onRequest(requestEvent);

  // ここに到達したかを切り分けるための中間チェックポイント(中くらいの速さで1回点滅)。
  // これが見えた後、何の反応もなくなる(以降のチェックポイントも出ない)場合は
  // Bluefruit.begin()内部でのハードフォルト(SoftDeviceの有効化失敗等)が疑われる
  digitalWrite(LED_RED, LED_ON);
  delay(200);
  digitalWrite(LED_RED, LED_OFF);
  delay(200);

  // 接続状態表示用LEDのピン初期化。この時点ではまだ何にも接続していないため、
  // updateConnectionLed()で初期状態(赤点灯=未接続)にしておく
  pinMode(BLUE_LED_PIN, OUTPUT);
  updateConnectionLed();

  // BLE Central初期化 (Peripheralロールは使わないため0、Central接続はPERIPH_COUNT台分確保)。
  // 失敗する場合はSoftDevice用RAM不足等の致命的な問題である可能性が高いため、
  // ESP32-C6版のI2C初期化失敗時と同様に停止する。
  // (ここで戻ってこられる「きれいな失敗」と、begin()内部でのハードフォルトによる無反応を
  //  見分けられるよう、上のチェックポイントとは明確に異なる遅い点滅にし、Serial出力も
  //  接続タイミングを問わず捕捉できるよう繰り返す)
  if (!Bluefruit.begin(0, PERIPH_COUNT))
  {
    while (true)
    {
      Serial.println("FATAL: Bluefruit.begin() failed");
      digitalWrite(LED_RED, LED_ON);
      delay(1000);
      digitalWrite(LED_RED, LED_OFF);
      delay(1000);
    }
  }
  Bluefruit.setName("XiaoNRF52840 BLE Client");
  // BluefruitはデフォルトでLED_BLUEをスキャン中/接続中の状態表示に自動使用するが、
  // 本プロジェクトでは独自のupdateConnectionLed()(赤=未接続/青=両方接続完了の排他点灯)で
  // LED_BLUEを制御するため、Bluefruit側の自動制御は無効化して競合を避ける
  Bluefruit.autoConnLed(false);

  // 各ペリフェラルのサービス・キャラクタリスティックをBluefruitへ登録する
  // (実際のGATTハンドル探索(discover)は接続確立後にconnectCallbackで行う)
  for (int i = 0; i < PERIPH_COUNT; i++)
  {
    PeripheralContext &p = peripherals[i];
    p.service.begin();
    p.charac.begin(&p.service);
    p.notifyCharac.setNotifyCallback(notifyCallback);
    p.notifyCharac.begin(&p.service);
  }

  Bluefruit.Central.setConnectCallback(connectCallback);
  Bluefruit.Central.setDisconnectCallback(disconnectCallback);

  // Interval/Windowはdefaultの値で動作して問題なさそうなため設定しない。
  // Heater/AutoAirAdjustいずれもデバイス名が128bit Service UUIDと合わせるとレガシー広告パケットに
  // 収まらないため、Service UUIDはADV_IND(プライマリ広告)側、名前はScan Response側という
  // 別々のパケットに分かれて送られてくる。
  // filterUuid()はレポート1件(=1パケット)ごとに単独で判定されるため、これを設定すると
  // 名前が載っているScan Response側のレポートはUUIDを含まないという理由でscanCallback()に
  // 渡される前に捨てられてしまい、Complete Local Nameを一切取得できず永久に接続できなくなる
  // (実際にこれが原因でAutoAirAdjust/Heaterのどちらにも接続できない不具合が発生していた)。
  // Service UUIDによる正当性検証は接続確立後にdiscoverAndSubscribe()内のservice.discover()で
  // 別途行っているため、スキャン時点でのUUIDフィルタは不要。よってfilterUuid()は使用せず、
  // 名前でのペリフェラル判別にはScan Responseの取得が必須のため、アクティブスキャンのみ有効にする
  // (パッシブでは名前が空になり判別できない)
  Bluefruit.Scanner.setRxCallback(scanCallback);
  Bluefruit.Scanner.restartOnDisconnect(true);  // 切断時に自動で再スキャンを開始する
  Bluefruit.Scanner.useActiveScan(true);
  Bluefruit.Scanner.start(0);  // 0 = タイムアウトなしで継続スキャン

  Serial.println("Scanning...");
}

// 一定時間更新されていないフレームを空にする
static void clearFrameIfStale(uint8_t *frame, uint32_t updatedAt, const char *label)
{
  if (frame[0] != 0 && (millis() - updatedAt) > FRAME_STALE_MS)
  {
    clearFrame(frame);
    Serial.printf("I2C frame cleared (stale): %s\n", label);
  }
}

void loop()
{
  // BLE(スキャン/接続/Notify)・I2Cスレーブ応答・接続状態表示LEDの更新はコールバック駆動
  // (connectCallback/disconnectCallback/notifyCallback/receiveEvent/requestEvent)。
  // loop()では、コールバックだけでは回復できない状態(古いデータの残留・Notify途絶・
  // 接続試行の停滞・スキャナー停止)を監視して復旧させる
  uint32_t now = millis();

  // 古いデータの破棄: マスターが古い値を有効値として記録し続けないようにする
  clearFrameIfStale(engineTempFrame, engineTempUpdatedAt, "ENGINE_TEMP");
  clearFrameIfStale(priPreFrame, priPreUpdatedAt, "PRI_PRE");
  clearFrameIfStale(secPreFrame, secPreUpdatedAt, "SEC_PRE");
  clearFrameIfStale(fuelPreFrame, fuelPreUpdatedAt, "FUEL_PRE");

  for (int i = 0; i < PERIPH_COUNT; i++)
  {
    PeripheralContext &p = peripherals[i];

    // Notify途絶: 接続は維持されているのにデータが来ない(送信側の停止・購読外れ等)場合は
    // 切断して再接続させる(以降はdisconnectCallback→再スキャンで復旧する)
    if (p.state == STATE_CONNECTED && (now - p.lastNotifyAt) > NOTIFY_SILENCE_MS)
    {
      Serial.printf("[%s] No notify for %d ms, disconnecting\n", p.name, NOTIFY_SILENCE_MS);
      p.lastNotifyAt = now;  // 切断完了までの間に繰り返し要求しないようにする
      Bluefruit.disconnect(p.connHandle);
    }

    // 接続試行の停滞: Scanner.start(0)のパラメータ(タイムアウト無し)で接続しているため、
    // 相手が消えると接続試行が終わらずスキャナーも止まったままになる。打ち切ってスキャンに戻す
    if (p.state == STATE_DO_CONNECT && (now - p.connectStartedAt) > CONNECT_TIMEOUT_MS)
    {
      // 失敗(NRF_ERROR_INVALID_STATE)は接続が既に成立しconnectCallbackが処理待ちであることを
      // 意味するため、その場合は何もしない
      if (sd_ble_gap_connect_cancel() == NRF_SUCCESS)
      {
        Serial.printf("[%s] Connect timeout, canceled\n", p.name);
        resetPeripheral(i);
        Bluefruit.Scanner.start(0);
      }
    }
  }

  // 安全策: 未接続のペリフェラルが残っているのにスキャナーが止まっていれば再開する
  ensureScanning();

  delay(WATCHDOG_INTERVAL_MS);
}
