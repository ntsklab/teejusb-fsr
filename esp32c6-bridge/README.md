# ESP32-C6 WiFi Bridge for teejusb-fsr

USB接続がゲームコントローラコンバータ（USB Joypad → PS変換器など）に使用される場合、
PCからWebUIにアクセスできなくなります。

このブリッジファームウェアは **ESP32-C6** を使い、ArduinoのハードウェアUART（ピン0/1）
経由で通信し、WiFi上にWebUIをホストします。スマホやPCのブラウザからFSRの
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
                                 WiFi AP: FSR-Pad
                                 http://192.168.4.1
```

## 必要なもの

### ハードウェア
- ESP32-C6 開発ボード
- ジャンパワイヤ 3本（TX, RX, GND）

### ソフトウェア
- Arduino IDE 2.x 以降
- **ボードパッケージ**: `esp32` by Espressif Systems（v3.0以降、C6対応）
- **ライブラリ**:
  - `ESPAsyncWebServer` — [mathieucarbou版](https://github.com/mathieucarbou/ESPAsyncWebServer) 推奨
  - `AsyncTCP` — ESPAsyncWebServerの依存ライブラリ
  - `ArduinoJson` — v7以降（Benoit Blanchon作）

## セットアップ手順

### 1. Arduino側の設定

`teejusb-fsr.ino` を開き、以下の行のコメントを外す:

```cpp
#define USE_SERIAL1_FOR_COMMANDS
```

これにより、コマンドプロトコルがUSB Serial (`Serial`) からハードウェアUART (`Serial1`, ピン0/1) に切り替わります。USB HID（ジョイスティック）出力は影響を受けません。

Arduinoにスケッチをアップロードします。

### 2. 配線

| ESP32-C6 | Arduino Leonardo |
|----------|------------------|
| D7 / GPIO17 (RX) | Pin 1 (TX) |
| D8 / GPIO19 (TX) | Pin 0 (RX) |
| GND | GND |

> **注意**: ESP32-C6のGPIOピン番号は `esp32c6-bridge.ino` 内の
> `ARDUINO_RX_PIN` / `ARDUINO_TX_PIN` で変更できます。

> **電圧レベル**: ESP32-C6は3.3V、Arduino Leonardoは5Vです。
> Leonardoの5V TX → ESP32-C6 3.3V RXは通常問題ありませんが、
> 安全のためにレベルシフタの使用を推奨します。

### 3. WebUIデータの準備

```bash
cd esp32c6-bridge
bash prepare_data.sh
```

WebUIのビルド済みファイルがgzip圧縮されて `data/` ディレクトリに配置されます。

> 事前に `webui/` で `npm run build` を実行してビルドを生成してください。

### 4. LittleFSへのアップロード

#### 方法A: Arduino IDE 2.x プラグイン（推奨）

1. [arduino-littlefs-upload](https://github.com/earlephilhower/arduino-littlefs-upload/releases)
   の最新 `.vsix` ファイルをダウンロード

2. Arduino IDE 2.x の拡張機能としてインストール:
   - `.vsix` ファイルを Arduino IDE のウィンドウに **ドラッグ＆ドロップ**
   - または: メニュー → `...` → `Install Plugin from VSIX file`

3. Arduino IDE で `esp32c6-bridge.ino` を開き、ボードと COMポートを選択

4. `Ctrl+Shift+P` (macOS: `Cmd+Shift+P`) → **`Upload LittleFS to Pico/ESP8266/ESP32`** を実行

> `data/` フォルダはスケッチファイルと同じ階層に置かれている必要があります。
> つまり `esp32c6-bridge/data/` の状態が正しい配置です。

#### 方法B: コマンドライン (esptool + mklittlefs)

```bash
# 1. mklittlefs のパスを確認（esp32 ボードパッケージに同梱）
MKLITTLEFS=$(find ~/.arduino15/packages/esp32/tools/mklittlefs \
  -name "mklittlefs" -type f 2>/dev/null | sort | tail -1)
echo $MKLITTLEFS   # パスが表示されれば OK

# 2. LittleFS イメージを作成
#    -s: サイズ（デフォルト 4MB フラッシュのパーティション: 0x1E0000 = 1966080 bytes）
#    実際の値はスケッチビルド時の .partitions.csv に依存するので要確認
$MKLITTLEFS -c esp32c6-bridge/data/ -b 4096 -p 256 -s 1966080 littlefs.bin

# 3. XIAO ESP32C6 を USB 接続し、esptool でフラッシュに書き込み
#    オフセットはパーティションテーブルを確認すること（通常 0x290000）
python3 -m esptool --chip esp32c6 --port /dev/ttyACM0 --baud 921600 \
  write_flash 0x290000 littlefs.bin
```

> **パーティションオフセットの確認方法**: Arduino IDE でスケッチをビルドし、
> 出力ログ内の `Generating FS image` 行に表示されるオフセット値を使用してください。

### 5. ESP32-C6にスケッチをアップロード

Arduino IDEで:
1. ボード: `ESP32C6 Dev Module` を選択
2. `esp32c6-bridge.ino` を開く
3. アップロード

### 6. 接続して使う

1. スマホまたはPCのWiFi設定で **`FSR-Pad`** に接続（パスワード: `fsrpad123`）
2. ブラウザで **http://192.168.4.1** を開く
3. 通常のWebUIと同じ操作でFSRの設定が可能

## 設定のカスタマイズ

`esp32c6-bridge.ino` の先頭にある定数を変更できます:

| 設定 | デフォルト | 説明 |
|------|-----------|------|
| `AP_SSID` | `"FSR-Pad"` | WiFi AP名 |
| `AP_PASSWORD` | `"fsrpad123"` | WiFi APパスワード |
| `ARDUINO_RX_PIN` | `17` | ESP32-C6のRXピン (D7 / GPIO17) |
| `ARDUINO_TX_PIN` | `19` | ESP32-C6のTXピン (D8 / GPIO19) |
| `NUM_SENSORS` | `8` | センサー数（Arduino側と一致させること） |
| `UI_INTERVAL_MS` | `16` | 値ポーリング間隔（ms） |

### STA接続情報の設定（gitに載せない）

STAのSSID/PASSは `wifi_secrets.h` で管理します。`git` に上がらないように
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
  LittleFSにWebUIファイルがアップロードされているか確認。
- **値が表示されない**: 配線を確認。TX↔RXが正しく接続されているか。
  GNDが共通になっているか。
- **プロファイルが保存されない**: LittleFSに書き込み領域が確保されているか確認。
  パーティション設定で LittleFS 用に十分な容量を割り当ててください。
