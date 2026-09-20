# nRF52840 BLE Central

[Seeed Xiao nRF52840](https://wiki.seeedstudio.com/XIAO_BLE/) を使用した BLE Central (クライアント) 実装サンプルです。  
M5Stack シリーズ（ESP32）では Wi-Fi と BLE を同時利用できないという制約があるため、これを回避するために本ボードを外付け BLE 受信機 (ブリッジ) として用い、取得した BLE Notify データを I2C スレーブとして保持し、[Chibi-T_Furoshiki_Logger](https://github.com/todateman/Chibi-T_Furoshiki_Logger)の M5Stack Basic（I2C マスター）からの読み出し要求に応じて返します。  
（I2Cスレーブアドレス: 0x08, SDA=D4, SCL=D5）

> 本プロジェクトはもともと [M5NanoC6](https://docs.m5stack.com/ja/core/M5NanoC6)（ESP32-C6）向けに実装されていましたが、arduino-esp32 の `Wire` ライブラリが ESP32-C6 では I2C スレーブの読み取り要求 (`onRequest`) を構造的に発火できないという既知の制限があり、ESP-IDF ネイティブ API への切り替え・クロックストレッチ調整・自己修復ロジックなど対症療法的な対応を重ねても安定しなかったため、I2C マスター(TWIM)とスレーブ(TWIS)が別ハードウェアペリフェラルである **Seeed Xiao nRF52840** に切り替えました。　　
> 詳細は後述の「スレーブ側の実装メモ」を参照してください。

## 特徴

- Arduino フレームワーク（Adafruit nRF52 Arduino コア、`Bluefruit52Lib` 使用）
- BLE Central: Heater / AutoAirAdjust 2台の BLE ペリフェラルに**同時接続**（Service UUID は共通のため、アドバタイズ名で判別）→ Notify 購読
- Notify 受信データを32バイト固定フレームに格納して保持
- オンボードLED(赤/青)で接続状態を表示（いずれか未接続=赤、Heater・AutoAirAdjust両方接続完了=青の排他点灯。詳細は後述）
- I2C スレーブ化（アドレス 0x08、コマンドで要求データの種類を指定して応答。標準 `Wire` ライブラリの `onReceive`/`onRequest` を使用、詳細は後述）
- PlatformIO プロジェクト構成（複数 env 拡張可能）
- BLE/I2C/LED更新ともにコールバック駆動（`loop()` は何も行わない）

## ハードウェア要件

- Seeed Xiao nRF52840（無印版。PlatformIO board = `xiaoble_adafruit`）
- I2C 接続先: M5Stack Basic（I2C マスター、Port A: G21=SDA, G22=SCL 想定）
- Xiao nRF52840 は Grove ポートを搭載していないため、直結配線（本体 D4=SDA, D5=SCL, GND）または [Xiao Expansion Board](https://wiki.seeedstudio.com/Seeeduino-XIAO-Expansion-Board/) 等の Grove 変換アダプタを介して接続してください
- 接続先 BLE ペリフェラル (後述 UUID 実装、以下の2台)
  - [Chibi-T_Furoshiki_Heater](https://github.com/todateman/Chibi-T_Furoshiki_Heater)（M5DinMeter、エンジン温度）
  - [Chibi-T_Furoshiki_AutoAirAdjust](https://github.com/todateman/Chibi-T_Furoshiki_AutoAirAdjust)（M5Core2、1次側/2次側空気圧・燃圧）
- 電源: USB 5V または I2C 配線と別に5V/3.3V給電
- 動作環境: 屋内 / 屋根のある屋外

## BLE UUID 一覧

Service UUID は両ペリフェラルで共通のため、接続先の判別は BLE アドバタイズ名（Complete Local Name）で行います。

| 用途 | UUID |
| ---- | ---- |
| Service（共通） | `7c44181A-c1a4-4635-a119-b490ed272552` |

### Chibi-T_Furoshiki_Heater (M5DinMeter)

- アドバタイズ名: `M5Din Furoshiki Heater`

| 用途 | UUID |
| ---- | ---- |
| Write Characteristic（存在確認用、実データなし） | `7c442A00-c1a4-4635-a119-b490ed272552` |
| Notify Characteristic | `7c442A6E-c1a4-4635-a119-b490ed272552` |

### Chibi-T_Furoshiki_AutoAirAdjust (M5Core2)

- アドバタイズ名: `ChibiT-AutoAirAdjust`
- 実装元: [feature/BLE ブランチ, commit 5b8b8aa](https://github.com/todateman/Chibi-T_Furoshiki_AutoAirAdjust)

| 用途 | UUID |
| ---- | ---- |
| Write Characteristic（存在確認用、実データなし） | `c9f878f1-c311-4452-ae5e-e813b4fe057d` |
| Notify Characteristic | `1d25ec49-e19c-4bb6-8c36-5dc8d8aaaebe` |

> ※ UUID は Version4 で生成。  
> ペリフェラル側が同一 UUID を持つ必要がある
> （Service UUID のみ共通、Characteristic UUID はペリフェラルごとに異なる

## 動作概要

1. 起動時に I2C スレーブと BLE Central を初期化し、アクティブスキャンを開始  
   （Service UUID は共通のため、アドバタイズ名で Heater / AutoAirAdjust を判別する。両者ともデバイス名が Scan Response 側に含まれるため、名前判別にはアクティブスキャンが必須。**Scanner の `filterUuid()` は使用しない**——Service UUID は ADV_IND 側、名前は Scan Response 側という別々のパケットに分かれて送られてくるため、UUIDフィルタを設定すると名前が載っている側のパケットが「UUIDを含まない」という理由で `scanCallback()` に渡る前に捨てられてしまい、永久に接続できなくなる不具合があった）
2. 対象デバイスを検出すると接続要求（見つかった方から順に、部分的な接続を許容。片方だけでも正常に動作する）  
   この時点で該当ペリフェラルを `STATE_DO_CONNECT` としてマークしておく
3. 接続後（`connectCallback`）:
   - 接続先の判別は、スキャン時点で `STATE_DO_CONNECT` にマークしておいたペリフェラルをそのまま使う（以前は接続後に `BLEConnection::getPeerName()` でGATT経由のGAP Device Nameキャラクタリスティックを読み直して判別していたが、本プロジェクトが使う既定ATT MTU(23byte)ではRead By Type応答が最大19byteしか運べず、`"ChibiT-AutoAirAdjust"`(20byte)や`"M5Din Furoshiki Heater"`(22byte)のような名前が末尾で切り詰められて必ず不一致になり、「Unexpected device connected」として即切断される不具合があったため撤廃した）
   - Service / Write キャラクタリスティックの探索（Write キャラクタリスティックは現状未使用／将来拡張枠）
   - Notify キャラクタリスティック探索・購読登録
   - 接続状態表示LEDを更新（後述）
4. Notify 受信（`notifyCallback`）:
   - 改行までのデータを先頭タグ（`PRI:` / `SEC:` / `FUEL:` / タグなし）で判別し、対応する I2C 送信用フレーム（`priPreFrame` / `secPreFrame` / `fuelPreFrame` / `engineTempFrame`）に格納・保持し、M5Stack Basic からの要求を待機
   - Heater / AutoAirAdjust それぞれ専用の受信バッファを保持するため、2台からの Notify が混ざることはない
5. 未接続のペリフェラルが残っている限り、スキャンを継続する（`Bluefruit.Scanner` がコールバック駆動でスキャンを管理するため、`loop()` 側でのポーリングは不要）
6. 切断イベント発生時（`disconnectCallback`）:
   - 切断されたペリフェラルのみ状態をリセットし、`Bluefruit.Scanner.restartOnDisconnect(true)` により自動的に再スキャン・再接続を試みる（もう一方の接続は維持される）
   - 接続状態表示LEDを更新（後述）

### 接続状態表示LED

`updateConnectionLed()` が接続/切断イベントのたびに呼ばれ、赤色LEDと青色LEDを排他的に点灯する。

- Heater・AutoAirAdjustのいずれか未接続: **赤色LEDのみ**点灯
- Heater・AutoAirAdjust両方接続完了: **青色LEDのみ**点灯

実機検証の結果、Xiao nRF52840のオンボードLED(赤・青)は `variant.h` の定義（`LED_STATE_ON=1`、active-high）とは逆に、実際には **active-low**（LOWを出力すると点灯）であることが判明した。  
`variant.h` 自体は他ライブラリへの影響を避けるため変更せず、`src/main.cpp` 内でのみ実機の実際の極性に合わせた `LED_ON` / `LED_OFF` マクロを定義し、以降はこちらを使用する。

### シンプルなデータフロー（論理）

```text
Heater / AutoAirAdjust --(Notify: 文字列データ)--> Xiao nRF52840 (I2Cスレーブ, addr=0x08) <--(I2C要求/応答)-- M5Stack Basic (I2Cマスター)
```

## ソース構成

```text
src/main.cpp        BLEスキャン/接続/Notifyハンドラ + I2Cスレーブ応答
platformio.ini      PlatformIO 設定
include/, lib/, test/ README のみ (拡張用)
```

## I2C通信仕様

AutoAirAdjust 側の BLE 送信機能は [feature/BLE ブランチ, commit 5b8b8aa](https://github.com/todateman/Chibi-T_Furoshiki_AutoAirAdjust) で実装済みで、本 Central 側も対応済みです。  

- スレーブアドレス: `0x08`
- ピン: SDA=D4, SCL=D5（Xiao nRF52840の既定Wireピン）
- コマンド方式: マスターは読み出し前に1バイトのコマンドコードを書き込み、どのデータを要求するかを明示する
  - `CMD_ENGINE_TEMP` (`0x01`): <https://github.com/todateman/Chibi-T_Furoshiki_Heater> からエンジン温度データ (*.**°Ｃ) を要求
  - `CMD_PRI_PRE` (`0x02`): <https://github.com/todateman/Chibi-T_Furoshiki_AutoAirAdjust> から1次側空気圧センサデータ (*.***MPa) を要求
  - `CMD_SEC_PRE` (`0x03`): <https://github.com/todateman/Chibi-T_Furoshiki_AutoAirAdjust> から2次側空気圧センサデータ (*.***MPa) を要求
  - `CMD_FUEL_PRE` (`0x04`): <https://github.com/todateman/Chibi-T_Furoshiki_AutoAirAdjust> から燃圧センサデータ (*.***MPa) を要求
  - 未定義のコマンドを書き込んだ場合は長さ0（先頭バイトが `0x00`）の空フレームを返す
  - `CMD_PRI_PRE` / `CMD_SEC_PRE` / `CMD_FUEL_PRE` の3つは同一の BLE ペリフェラル（M5Core2、Chibi-T_Furoshiki_AutoAirAdjust）から届く。  
  ペリフェラル側は1本の Notify Characteristic で3種類のデータを送るため、Xiao nRF52840 は改行区切りメッセージの先頭タグでデータ種別を判別し、それぞれ専用フレームに格納する
    - `PRI:` → `CMD_PRI_PRE` 用フレーム（例: `PRI:0.853\n`）
    - `SEC:` → `CMD_SEC_PRE` 用フレーム（例: `SEC:0.724\n`）
    - `FUEL:` → `CMD_FUEL_PRE` 用フレーム（例: `FUEL:2.107\n`）
    - いずれのタグにも一致しないメッセージは従来どおり `CMD_ENGINE_TEMP` 用フレームに格納する（後方互換）
- フレーム形式: 32バイト固定
  - `[0]`: データ長 (0〜30)
  - `[1..30]`: データ本体（余りはゼロ埋め）
  - `[31]`: 直前にマスターが書き込んだコマンドのエコーバック（マスター側でコマンドと応答のズレを検知するための仕組み）
  - BLE Notify 未受信時（起動直後など）は全ゼロを返す
- 動作: マスターがコマンドを書き込んだ後 `Wire.requestFrom()` で読み出すと、直近に BLE で受信した最新データを返す（新しい Notify を受信するまで同じ値を返し続ける）

### スレーブ側の実装メモ（nRF52840 での I2C スレーブ実装）

Xiao nRF52840 は Adafruit nRF52 Arduino コアの標準 `Wire` ライブラリ（`Wire.begin(address)` + `Wire.onReceive()` + `Wire.onRequest()`）でそのまま I2C スレーブとして動作します。

- **なぜ M5NanoC6 (ESP32-C6) で問題になったか**: arduino-esp32 の I2C スレーブ HAL 実装は、マスターの読み取り要求を検出するために使う SCL クロックストレッチ原因の判定が ESP32-C3 / S3 専用の分岐になっており、ESP32-C6 では常に `I2C_STRETCH_CAUSE_MAX` を返す。  
  そのため `Wire.onRequest()` が構造的に一度も発火しない（arduino-esp32 側の既知の制限）。  
  回避のため ESP-IDF ネイティブの `driver/i2c_slave.h` を直接使用する実装に切り替えたが、その後もクロックストレッチのタイミング・割り込み優先度・TXリングバッファの詰まりなど対症療法的なチューニングが必要になった
- **nRF52840 でなぜ標準Wireのままで良いか**: nRF52840 は I2C マスター用の `TWIM` と I2C スレーブ用の `TWIS` が別ハードウェアペリフェラルとして実装されている。  
  `TWIS` はマスターの読み取り開始時、ソフトウェアが `onRequest` コールバック内で送信バッファ（`Wire.write()`）を準備し終えるまでハードウェアが自動的にクロックストレッチを維持する設計になっており、ESP32-C6 のように「コールバックが返った直後に強制的にストレッチが解除されて間に合わない」という問題が起きない。  
  Adafruit nRF52 Arduino コアには I2C スレーブのサンプル（`libraries/Wire/examples/secondary_sender`, `secondary_receiver`）が公式に用意されており、標準 API がスレーブモードで動作することが確認できる
- **注意**: `Wire.onReceive()`/`Wire.onRequest()` のコールバックは（ESP32版のタスク委譲と異なり）TWIS の割り込みハンドラから直接呼ばれる。  
  そのため実装では、コールバック内では応答フレームの確定（`memcpy`+`Wire.write()`）のみを行い、`Serial` 出力などの重い処理は避けている（`src/main.cpp` の `receiveEvent`/`requestEvent` を参照）
- 実機での長時間動作検証（3h 程度）では問題は発覚していないが、もしBLEスキャン処理とI2C応答が競合して不安定になるようであれば、`receiveEvent`/`requestEvent` 周辺に自己修復ロジックを再導入する余地を残してある

### M5Stack Basic（マスター）側のサンプルコード

```cpp
#include <M5Stack.h>
#include <Wire.h>

#define I2C_SLAVE_ADDR 0x08
#define I2C_FRAME_SIZE 32
#define CMD_ENGINE_TEMP 0x01
#define CMD_PRI_PRE 0x02
#define CMD_SEC_PRE 0x03
#define CMD_FUEL_PRE 0x04

void setup() {
  M5.begin();
  Wire.begin(21, 22); // Grove Port A (SDA, SCL)
}

// コマンドを指定してフレームを読み出し、シリアルに表示する
void requestAndPrint(uint8_t command, const char *label) {
  // 1. コマンドを書き込み、要求するデータの種類を伝える
  Wire.beginTransmission(I2C_SLAVE_ADDR);
  Wire.write(command);
  Wire.endTransmission();

  // 2. フレームを読み出す
  uint8_t frame[I2C_FRAME_SIZE];
  Wire.requestFrom(I2C_SLAVE_ADDR, I2C_FRAME_SIZE);
  for (int i = 0; i < I2C_FRAME_SIZE && Wire.available(); i++) {
    frame[i] = Wire.read();
  }

  uint8_t len = frame[0];
  if (len > 0) {
    String data;
    for (int i = 0; i < len; i++) {
      data += (char)frame[1 + i];
    }
    Serial.printf("%s: %s\n", label, data.c_str());
  }
}

void loop() {
  // エンジン温度、1次側/2次側空気圧、燃圧をそれぞれ要求して表示
  requestAndPrint(CMD_ENGINE_TEMP, "ENGINE_TEMP");
  requestAndPrint(CMD_PRI_PRE, "PRI_PRE");
  requestAndPrint(CMD_SEC_PRE, "SEC_PRE");
  requestAndPrint(CMD_FUEL_PRE, "FUEL_PRE");

  delay(500); // ポーリング間隔
}
```

※ Port A のピン番号は使用する M5Stack シリーズ機種によって異なる場合があるため、実機に応じて `Wire.begin(sda, scl)` の引数を調整してください。

## ビルド & アップロード手順 (PlatformIO)

VS Code + PlatformIO 拡張機能を使用します。

Xiao nRF52840 のボード定義は本家 [platformio/platform-nordicnrf52](https://github.com/platformio/platform-nordicnrf52) に未マージ（[PR #151](https://github.com/platformio/platform-nordicnrf52/pull/151), Open状態）のため、コミュニティで広く使われているフォーク（[maxgerhardt/platform-nordicnrf52](https://github.com/maxgerhardt/platform-nordicnrf52)）を `platformio.ini` の `platform` に指定しています。

1. このリポジトリを VS Code で開く
2. 左側 PlatformIO パネルから環境 `XiaoNRF52840` を選択
3. "Build" をクリック (もしくは コマンドパレットから `PlatformIO: Build`)
4. USB 経由でボードを接続し "Upload" を実行  
   （Xiao nRF52840 はブートローダ経由のアップロードのため、初回や書き込みがうまく認識されない場合はリセットボタンを素早く2回押してブートローダモード（オレンジ色LED点滅）に入れてから再度 Upload してください）
5. シリアルモニタ (115200bps) を開きログを確認

### CLI (任意)

```bash
pio run -e XiaoNRF52840
pio run -e XiaoNRF52840 -t upload
pio device monitor -b 115200
```

## 実行時ログ例 (概略)

```text
Device found! (M5Din Furoshiki Heater)
[M5Din Furoshiki Heater] - Found our service
[M5Din Furoshiki Heater] - Found our characteristic
[M5Din Furoshiki Heater] - Registered for notify
Connected to server (M5Din Furoshiki Heater)
[LED] M5Din Furoshiki Heater=3 ChibiT-AutoAirAdjust=0 -> RED(not connected)
Device found! (ChibiT-AutoAirAdjust)
[ChibiT-AutoAirAdjust] - Found our service
[ChibiT-AutoAirAdjust] - Found our characteristic
[ChibiT-AutoAirAdjust] - Registered for notify
Connected to server (ChibiT-AutoAirAdjust)
[LED] M5Din Furoshiki Heater=3 ChibiT-AutoAirAdjust=3 -> BLUE(connected)
Notify callback for characteristic ... of data length N
```

`[LED] ...` 行は `updateConnectionLed()` が接続/切断イベントのたびに出力する診断ログで、各ペリフェラルの状態（`STATE_IDLE=0` / `STATE_DO_CONNECT=1` / `STATE_CONNECTED=3`）と、その結果として点灯させたLEDの色を確認できる。

## カスタマイズポイント

- I2Cスレーブアドレス: `#define I2C_SLAVE_ADDR 0x08` で変更可
- I2Cピン: Xiao nRF52840の既定Wireピン（D4=SDA, D5=SCL）を使用。変更する場合は `Wire.begin()` の前に `Wire.setPins(sda, scl)` を呼び出す
- フレームサイズ: `#define I2C_FRAME_SIZE 32` で変更可（データ本体は `I2C_FRAME_SIZE - 2` バイトまで、末尾1byteはコマンドエコー用）
- コマンド: `CMD_ENGINE_TEMP` / `CMD_PRI_PRE` / `CMD_SEC_PRE` / `CMD_FUEL_PRE` を定義済み。さらに他のデータを中継する場合は新しいコマンド定数とフレーム/ハンドリング（および必要ならタグ文字列）を追加  
  応答フレームの選択ロジックは `frameForCommand()`（`receiveEvent` から呼ばれる）に実装されている点に注意
- LED ピン: `LED_RED` / `#define BLUE_LED_PIN LED_BLUE`（オンボードLEDの赤・青。実機がactive-lowだったため独自定義した `LED_ON` / `LED_OFF` マクロで極性を吸収しているため、点灯/消灯の記述に極性を意識する必要はない）
- UUID: `SERVICE_UUID`（共通）/ `HEATER_CHARACTERISTIC_UUID` / `HEATER_NOTIFY_CHARACTERISTIC_UUID` / `AUTOAIR_CHARACTERISTIC_UUID` / `AUTOAIR_NOTIFY_CHARACTERISTIC_UUID` で差し替え可能
- デバイス名フィルタ: `HEATER_DEVICE_NAME` / `AUTOAIR_DEVICE_NAME`（接続先の判別に使用、Service UUID が共通のため必須）
- 再接続ポリシー: 切断されたペリフェラルのみ `resetPeripheral()` で状態をリセットし、`Bluefruit.Scanner.restartOnDisconnect(true)` により自動的に再接続を試みる（もう一方の接続には影響しない）

## 今後の改善案

- float16 → float32 変換/スケーリングユーティリティ
- アクティブスキャンによる消費電力増への対応（必要ならスキャン間隔/ウィンドウの調整）
- 3台目以降のペリフェラル追加が必要になった場合の汎用化（現状は Heater/AutoAirAdjust の2台固定の意図的な設計）
- 再接続の待機/バックオフ（現状は見つかり次第即座に再接続を試みるのみ）
- Notify データのタイムアウト検知（一定時間 Notify が来ない場合に「値が古い」ことを判別する仕組み）
- Write キャラクタリスティック活用 (将来の制御コマンド)
- データ検証 (CRC / バージョン / シーケンス番号)
- 状態遷移図とエラーハンドリング整備
- BLE接続タイムアウト（`BLE_GAP_EVT_TIMEOUT`）の未処理: 接続試行が応答なくタイムアウトした場合、`connectCallback`/`disconnectCallback` いずれも呼ばれず該当ペリフェラルが `STATE_DO_CONNECT` のまま復帰できなくなる可能性があり、ハンドリングの追加を検討
- I2Cスレーブ簡素化（ESP-IDFネイティブAPI依存の撤去、標準Wireへの回帰）が実機でも安定して動作するかの長時間検証、および BLE central 処理（スキャン/接続）との競合有無の確認

## ライセンス

本ソフトウェアは MIT License です。`LICENSE` を参照してください。

---
ドキュメント最終更新: 2026-08-09 (M5NanoC6 (ESP32-C6) から Seeed Xiao nRF52840 へ移植。I2Cスレーブを ESP-IDF ネイティブドライバから標準 `Wire` ライブラリへ、BLE Central を arduino-esp32 の `BLEDevice` から `Bluefruit52Lib` へ全面書き換え。

その後の実機デバッグで判明した3件の接続不良の原因を修正: (1) `Scanner.filterUuid()` がアドバタイズ名を含むScan Responseパケットを誤って破棄していた、(2) 接続後の `getPeerName()` によるピア名再判別がATT MTU既定値(23byte)で20byte以上の名前を切り詰めて誤判定していた、(3) 対向機側がPRI/SEC/FUELを1回のnotifyにまとめて送信しATT MTU超過分が切り捨てられていた(`Chibi-T_Furoshiki_AutoAirAdjust`側で個別notifyに分割して解消)。

あわせて、Notify受信時の一時点灯だった青色LEDを接続状態表示（赤=いずれか未接続/青=両方接続完了の排他点灯）に変更。実機検証でオンボードLEDが `variant.h` の想定(active-high)と逆の active-low であることが判明し、`LED_ON`/`LED_OFF` マクロで吸収)
