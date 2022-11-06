# ESP32用 AES67 Receiver

## 必要ハードウェア環境

 1-1. ESP32

  [検証済みESP32](https://akizukidenshi.com/catalog/g/gM-15673/)

 1-2. 外付けDAC

  [検証済みDAC](https://www.amazon.co.jp/gp/product/B0779QVRSH/)


## 必要ソフトウェア環境

 2-1. ESP-IDF

  [ESP-IDF v4.2](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/index.html)


## 制限事項(2022/11/06 21:00現在)

 3-1. 16/24Bit、2Channel、1/5ms単位対応（RTPペイロードサイズよりの自動判別)

 3-2. 音声出力のみ

 3-3. SDP受信のみ対応



## コンパイルと実行方法

 4-1. ESP-IDFをインストール

 4-2. . ~/esp/esp-idf/export.sh

 4-3. git clone https://github.com/tmatsuya/esp32_aes67_receiver.git

 4-4. cd esp32_aes67_receiver

 4-5. idf.py  menuconfig  ... Menuより"Example Connection Config"からWiFi SSIDとWiFi Passwordも設定すること。さらにソース上の UDP_PORT と MULTICAST_IPV4_ADDR マクロにマルチキャストアドレスをして英すること。またネットワーク上のSDPからソースを学習でき、ボード上のボタン(Default GPIO0)を押すことで入力先を切り替えできる

 4-6. idf.py -p /dev/ESP32シリアルデバイス flash



## ESP32と外付けDACの配線図
```
  ESP32          DAC(PCM5102)
             |   GND  *1
             |   MCLK *1
  IO26  ------   BCLK
  IO25  ------   DATA
  IO22  ------   LRCK
  GND   ------   GND
          NC     MUTE
  5V    ------   VCC
```

 *1  MCLKとGNDをショートすること。でないと音質が悪くなることがあるので注意。（はまった）


![配線写真](/photo.jpg)
