# teejusb-fsr向け ESP32-C6 Wi-Fiブリッジ

USB接続がゲームコントローラコンバータ（USB Joypad → PS変換器など）に使用される場合、
PCからWebUIにアクセスできなくなります。

このブリッジファームウェアは **ESP32-C6** を使い、ArduinoのハードウェアUART（ピン0/1）
経由で通信し、Wi-Fi上にWebUIをホストします。スマートフォンやPCのブラウザからFSRの
閾値調整・プロファイル管理が可能になります。

## システム構成

```
┌──────────────┐   USB (HID)   ┌────────────────┐
│   Arduino    │──────────────→│  PSコンバータ   │→ ゲーム機
│  (Leonardo)  │               └────────────────┘
│              │   UART (0/1)  ┌────────────────┐   WiFi   ┌──────────┐
│  TX(pin1) ───│──────────────→│ RX  ESP32-C6   │←────────→│ ブラウザ │
│  RX(pin0) ←──│───────────────│ TX             │          │ (WebUI)  │
│  GND ────────│───────────────│ GND            │          └──────────┘
└──────────────┘               └────────────────┘
                                 Wi-Fi AP: FSR-Pad
                                 http://192.168.4.1
```

## 必要なもの

### ハードウェア
- ESP32-C6 開発ボード
- ジャンパワイヤ 3本（TX, RX, GND）

### ソフトウェア
- VS Code + PlatformIO拡張機能
- PlatformIO Core
- ESP-IDF toolchain（PlatformIO が自動導入）

## セットアップ手順

### 1. Arduino側の設定

このブランチでは `teejusb-fsr.ino` で以下の行が既定で有効です。

```cpp
#define USE_SERIAL1_FOR_COMMANDS
```

コメントアウトされている場合は有効化してください。

これにより、コマンドプロトコルがUSB Serial (`Serial`) からハードウェアUART (`Serial1`, ピン0/1) に切り替わります。USB HID（ジョイスティック）出力には影響しません。

Arduinoにスケッチをアップロードします。

### 2. 配線

| ESP32-C6 | Arduino Leonardo |
|----------|------------------|
| D4 / GPIO22 (RX) | Pin 1 (TX) |
| D5 / GPIO23 (TX) | Pin 0 (RX) |
| GND | GND |

> **注意**: ESP32-C6のGPIOピン番号は `main/esp32c6_bridge_main.cpp` 内の
> `kArduinoRxPin` / `kArduinoTxPin` で変更できます。

> **補足**: GPIO16/17 はデフォルトのコンソール UART と衝突するため、
> デフォルト配線では使用しません。

> **電圧レベル**: ESP32-C6は3.3V、Arduino Leonardoは5Vです。
> Leonardoの5V TX → ESP32-C6 3.3V RXは通常問題ありませんが、
> 安全のためにレベルシフタの使用を推奨します。

### 3. WebUIのビルド

まず `webui/` で依存関係を入れてから本番ビルドを生成します。

```bash
cd webui
yarn install
yarn build
```

### 4. WebUIデータの準備

```bash
cd esp32c6-bridge
bash prepare_data.sh
```

WebUIのビルド済みファイルがgzip圧縮されて `data/` ディレクトリに配置されます。

`prepare_data.sh` は WebUI のビルド成果物を `esp32c6-bridge/data/` に配置します。
ESP-IDF/PlatformIO ではこの内容から SPIFFS イメージが生成されますが、書き込みは `uploadfs` で別途実行します。

### 5. PlatformIO + ESP-IDF でビルド/書き込み

リポジトリ直下の `platformio.ini` に `esp32c6_bridge` 環境があります。

```bash
cd /path/to/fsr
pio run -e esp32c6_bridge
```

WebUI を含めてビルドする場合は以下の順です。

```bash
cd webui && yarn install && yarn build
cd ../esp32c6-bridge && bash prepare_data.sh
cd .. && pio run -e esp32c6_bridge
```

書き込みはファームウェアと SPIFFS で 2 段階です。

```bash
pio run -e esp32c6_bridge -t upload
pio run -e esp32c6_bridge -t uploadfs
```

必要に応じて `platformio.ini` の `upload_port` / `monitor_port` を環境に合わせて設定してください。

### 6. 接続して使う

1. スマートフォンまたはPCのWi-Fi設定で **`FSR-Pad`** に接続（パスワード: `fsrpad123`）
2. ブラウザで **http://192.168.4.1** を開く
3. 通常のWebUIと同じ操作でFSRの設定が可能

## 設定のカスタマイズ

`main/esp32c6_bridge_main.cpp` の先頭にある定数を変更できます:

| 設定 | デフォルト | 説明 |
|------|-----------|------|
| `kApSsid` | `"FSR-Pad"` | Wi-Fi AP名 |
| `kApPassword` | `"fsrpad123"` | Wi-Fi APパスワード |
| `kArduinoRxPin` | `22` | ESP32-C6のRXピン (D4 / GPIO22) |
| `kArduinoTxPin` | `23` | ESP32-C6のTXピン (D5 / GPIO23) |
| `kNumSensors` | `8` | センサー数（Arduino側と一致させること） |
| `kUiIntervalMs` | `16` | 値ポーリング間隔（ms） |

### STA接続情報の設定（Gitに載せない）

STAのSSIDとパスワードは `wifi_secrets.h` で管理します。Gitに含めないように
`esp32c6-bridge/wifi_secrets.h` は `.gitignore` 済みです。

1. `esp32c6-bridge/wifi_secrets.h.example` をコピーして `wifi_secrets.h` を作成
2. 複数候補のSSID/PASSを記入

```cpp
static const char* const STA_CANDIDATE_SSIDS[] = {
  "HomeWiFi",
  "MobileHotspot",
};

static const char* const STA_CANDIDATE_PASSWORDS[] = {
  "home-pass",
  "hotspot-pass",
};
```

接続優先順位は配列の上から順です。

### WiFi動作仕様

- 起動時はSTA候補へ順番に接続を試行
- いずれも失敗したらAPモードへフォールバック
- APモード中は5分間隔でSTA再接続を自動試行
- STAが接続できたらAPを停止してSTAモードへ移行

### XIAO ESP32C6 LED表示

- STAモード: 1秒周期で100ms点滅を3回
- APモード: 1秒周期で100ms点滅を2回
- エラー状態: 常時高速点滅

## トラブルシューティング

- **WebUIが「Connecting...」のまま**: ESP32-C6のシリアルモニタでエラーを確認。
  `webui/build` 生成後に `prepare_data.sh` を実行し、`pio run -e esp32c6_bridge -t upload` と `pio run -e esp32c6_bridge -t uploadfs` を順に行ってください。
- **値が表示されない**: 配線を確認。TX↔RXが正しく接続されているか。
  GNDが共通になっているか。
- **プロファイルが保存されない**: SPIFFS 領域が壊れている可能性があります。
  一度 `pio run -e esp32c6_bridge -t erase` を実行してから再書き込みしてください。
