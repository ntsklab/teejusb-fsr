# Teejusb FSRガイド

FSRダンスパッド向けのファームウェアとWebUI一式です。
このブランチでは、従来のArduino/Teensy向けFSRファームウェアに加えて、ESP32-C6を使ってWi-Fi経由でWebUIを公開する構成を主軸にしています。

質問や提案があれば [![Discord](https://img.shields.io/discord/778312862425939998?color=5865F2&label=Discord&logo=discord&logoColor=white)](https://discord.com/invite/RamvtwuEF2) に参加してください。

## 機能

- Arduino Leonardo / Pro Micro / Teensy向けFSRファームウェア
- 8センサー構成を既定としたしきい値調整
- プロファイル保存
- ESP32-C6上で動く組み込みWebUI
- Wi-Fi AP配信と、任意でSTA接続への自動切り替え
- `[public]` プロファイル更新用の管理者パスワード
- LED出力サポート

## スクリーンショット

<img src="./img/fsr2.gif" width="550">

<img src="./img/fsr1.gif" width="550">

## リポジトリ構成

- `teejusb-fsr.ino`: FSRファームウェア本体
- `main/fsr/main.cpp`: PlatformIO用のArduinoエントリポイント
- `main/esp32c6_bridge_main.cpp`: ESP32-C6ブリッジファームウェア
- `esp32c6-bridge/`: SPIFFS用データと補助スクリプト
- `webui/`: React WebUIとローカル開発用APIサーバ
- `platformio.ini`: `fsr_leonardo`, `fsr_teensy40`, `esp32c6_bridge` の各環境定義

## 必要なもの

通常利用では以下があれば足ります。

- PlatformIO Core または VS Code + PlatformIO拡張機能
- Node.js
- yarn などのnpm互換パッケージマネージャ

ローカルPC上で旧来のAPIサーバを動かしてWebUI開発をする場合のみ、追加で以下が必要です。

- Python 3
- `venv`

## ハードウェアセットアップ

センサー配線の基本は、[fsr-pad-guide](https://github.com/Sereni/fsr-pad-guide) や [vlnguyen/itg-fsr](https://github.com/vlnguyen/itg-fsr/tree/master/fsr) のような一般的なFSRパッド構成と同様です。

このブランチの既定設定では、8パネル構成を想定しています。

- パッド1: `A0`-`A3`
- パッド2: `A4`-`A7`

必要なら `teejusb-fsr.ino` 内の `Sensor kSensors[]` を編集して、センサー本数やピン配置を変更してください。

## このブランチの推奨構成

このブランチでは、以下の構成が標準です。

1. Arduino/TeensyがFSRを読み取り、USB HIDを出力する
2. 同時にUART (`Serial1`) でコマンドをESP32-C6ブリッジへ渡す
3. ESP32-C6がWebUIを配信し、スマートフォンやPCのブラウザからしきい値やプロファイルを操作する

### 構成イメージ

```
┌──────────────┐   USB (HID)   ┌────────────────┐
│ Arduino/     │──────────────→│ PS converter / │
│ Teensy       │               │ PC / console   │
│              │               └────────────────┘
│              │   UART        ┌────────────────┐   WiFi   ┌──────────┐
│ TX(pin1) ────│──────────────→│ RX ESP32-C6    │←────────→│ Browser  │
│ RX(pin0) ←───│───────────────│ TX             │          │ WebUI    │
│ GND ─────────│───────────────│ GND            │          └──────────┘
└──────────────┘               └────────────────┘
```

## 1. FSRファームウェアのビルドと書き込み

### このブランチでの既定動作

`teejusb-fsr.ino` では、現在のブランチ向けに以下が既定で有効です。

- `USE_ARDUINO_JOYSTICK_LIBRARY`
- `USE_SERIAL1_FOR_COMMANDS`

つまり、ATmega32u4系ボードではUSBジョイスティック出力を使い、しきい値コマンドはUSB SerialではなくハードウェアUART (`Serial1`) に流します。

Arduino Leonardo / Pro Microでジョイスティックとして使う場合は、事前に `ArduinoJoystickLibrary` を導入してください。

### PlatformIOでのビルド

リポジトリ直下の `platformio.ini` には、以下の環境があります。

- `fsr_leonardo`: Arduino Leonardo / Pro Micro系32u4ボード
- `fsr_teensy40`: Teensy 4.0
- `esp32c6_bridge`: ESP32-C6ブリッジファームウェア

実行例:

```bash
pio run -e fsr_leonardo
pio run -e fsr_teensy40
```

書き込みもPlatformIOから行えます。

```bash
pio run -e fsr_leonardo -t upload
pio run -e fsr_teensy40 -t upload
```

### Arduino IDEを使う場合

Arduino IDEでも `teejusb-fsr.ino` を直接開けますが、このブランチはPlatformIOでの運用を前提に整備されています。

USB Serial Monitorで従来どおりコマンド確認をしたい場合は、`USE_SERIAL1_FOR_COMMANDS` をコメントアウトして `CMD_SERIAL` をUSB Serialへ戻してください。

## 2. ESP32-C6ブリッジの配線

既定配線は以下です。

| ESP32-C6 | Arduino Leonardo / Pro Micro |
|----------|-------------------------------|
| GPIO22 (RX) | Pin 1 (TX) |
| GPIO23 (TX) | Pin 0 (RX) |
| GND | GND |

補足:

- ESP32-C6側の既定ピンは `main/esp32c6_bridge_main.cpp` 内の `kArduinoRxPin` / `kArduinoTxPin`
- センサー数は `kNumSensors = 8`
- XIAO ESP32C6向けのステータスLEDは `GPIO15`

Leonardo系のTXは5V系です。実機で安定性や保護を優先するなら、ESP32-C6側RXにはレベルシフタの使用を推奨します。

## 3. WebUIアセットのビルド

ESP32-C6ブリッジは `webui/build` の静的ファイルをSPIFFSへ載せて配信します。

```bash
cd webui
yarn install
yarn build
```

## 4. SPIFFSデータディレクトリの準備

`prepare_data.sh` は `webui/build` を `esp32c6-bridge/data/www` にコピーし、主要ファイルをgzip圧縮してESP32のフラッシュ向けに整形します。

```bash
cd esp32c6-bridge
bash prepare_data.sh
```

## 5. ESP32-C6ブリッジのビルドと書き込み

リポジトリルートへ戻ってブリッジファームウェアをビルドします。

```bash
cd ..
pio run -e esp32c6_bridge
```

書き込みはファームウェアとSPIFFSイメージで分かれます。

```bash
pio run -e esp32c6_bridge -t upload
pio run -e esp32c6_bridge -t uploadfs
```

必要に応じて `platformio.ini` 側で `upload_port` や `monitor_port` を環境に合わせて設定してください。

## 6. WebUIへ接続する

既定ではESP32-C6がアクセスポイントを立てます。

- SSID: `FSR-Pad`
- パスワード: `fsrpad123`
- URL: `http://192.168.4.1`

手順:

1. スマートフォンまたはPCで `FSR-Pad` に接続
2. ブラウザで `http://192.168.4.1` を開く
3. WebUIからしきい値やプロファイルを操作

## 7. 必要ならSTAモードを使う

`esp32c6-bridge/wifi_secrets.h.example` を元に `wifi_secrets.h` を作ると、起動時に既知のWi-Fiへ接続を試みます。

```bash
cd esp32c6-bridge
cp wifi_secrets.h.example wifi_secrets.h
```

SSIDとパスワードを埋めたあと再ビルドしてください。

動作仕様:

- 起動時にSTA候補へ順番に接続を試行
- 失敗したらAPモードへフォールバック
- APモード中も一定間隔でSTA再接続を試行
- STA接続に成功したらAPを停止

## 設定ポイント

ESP32-C6側の主要設定は `main/esp32c6_bridge_main.cpp` の定数で調整できます。

- AP名: `kApSsid`
- APパスワード: `kApPassword`
- 管理者パスワード: `kAdminPassword`
- UARTピン: `kArduinoRxPin`, `kArduinoTxPin`
- センサー数: `kNumSensors`
- UIポーリング周期: `kUiIntervalMs`

`kAdminPassword` の既定値は `admin123` です。公開運用するなら書き換えてからビルドしてください。

## 8. 従来のPCローカルWebUIを使う場合

このブランチの主用途はESP32-C6内蔵WebUIですが、`webui/server/server.py` を使ってローカルPC上で従来のWebUIを動かすこともできます。

これは以下の用途に向いています。

- WebUIの開発
- ESP32-C6を使わず、USB Serial経由でPCから直接調整したい場合

手順:

```bash
cd webui/server
python -m venv venv
source venv/bin/activate
pip install -r requirements.txt
cd ..
yarn install
yarn build
yarn start-api
```

起動後は通常 `http://localhost:5000` で利用できます。

このモードでは、対象デバイスが `webui/server/server.py` の `SERIAL_PORT_KEYWORDS` に一致している必要があります。

## トラブルシューティング

- WebUIが開いても `Connecting...` のままなら、ESP32-C6とArduino間のUART配線、GND共通、`USE_SERIAL1_FOR_COMMANDS` の有効化を確認してください
- ESP32-C6側で画面が出ない場合は、`webui/build` を作り直してから `esp32c6-bridge/prepare_data.sh` を再実行し、`upload` と `uploadfs` の両方をやり直してください
- STA接続しない場合は `wifi_secrets.h` の配列数と内容を確認してください
- ローカルWebUIでシリアルポートが見つからない場合は、`SERIAL_PORT_KEYWORDS` に自分のボード名を追加してください
- USB Serial Monitorでしきい値確認ができない場合は、このブランチでは既定でコマンド経路が `Serial1` に切り替わっている点に注意してください

## ジョイスティック対応メモ

Teensyでは内蔵Joystick APIを使います。
Arduino Leonardo / Pro Micro系では `ArduinoJoystickLibrary` を使います。

RP2040系についてもコード上で対応がありますが、このREADMEの主手順は、このブランチの標準構成である Leonardo / Teensy + ESP32-C6ブリッジを対象にしています。
