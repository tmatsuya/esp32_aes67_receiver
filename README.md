# ESP32用 AES67 Receiver

## 必要ハードウェア環境

 1-1. ESP32

  [検証済みESP32](https://akizukidenshi.com/catalog/g/gM-15673/)


 1-2. 外付けDAC

  [検証済みDAC] PCM5102   (https://www.amazon.co.jp/gp/product/B0779QVRSH/)

  [検証済みDAC] Pmod I2S2 (https://digilent.com/reference/pmod/pmodi2s2/start)


## 必要ソフトウェア環境

 2-1. ESP-IDF

  [ESP-IDF v4.2](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/index.html)


## 制限事項(2023/02/22 21:00現在)

 3-1. 16/24Bit、2Channel、1/5ms単位対応（RTPペイロードサイズよりの自動判別)

 3-2. 音声出力のみ

 3-3. SDP受信のみ対応



## コンパイルと実行方法

 4-1. ESP-IDFをインストール

 4-2. . ~/esp/esp-idf/export.sh

 4-3. git clone https://github.com/tmatsuya/esp32_aes67_receiver.git

 4-4. cd esp32_aes67_receiver

 4-5. idf.py  menuconfig  ... Menuより"Example Connection Config"からWiFi SSIDとWiFi Passwordも設定すること。さらにソース上の UDP_PORT と MULTICAST_IPV4_ADDR マクロに初期値のマルチキャストアドレスを設定すること。またネットワーク上のSDPパケットからAES67ソースを自動学習でき(最大16ソース)、ボード上のボタン(Default GPIO2)を押すことで入力先を切り替えできる

 4-6. idf.py -p /dev/ESP32シリアルデバイス flash



## ESP32と外付けDAC(PCM5102)の配線図
```
  ESP32          DAC(PCM5102)
  IO0   ------   MCLK (or connect to GND)
  IO26  ------   BCLK
  IO25  ------   DATA
  IO22  ------   LRCK
  GND   ------   GND
          NC     MUTE
  5V    ------   VCC
```

![配線写真 PCM5102](/photo_pcm5102.jpg)






## ESP32と外付けDAC(Pmod I2S2)の配線図
```
  ESP32               DAC(I2S2)
  IO0   ------   PIN1 D/A MCLK
  IO26  ------   PIN2 D/A SCLK
  IO25  ------   PIN3 D/A SDIN
  IO22  ------   PIN4 D/A LRCLK
  GND   ------   PIN5 GND
  3.3V  ------   PIN6 VCC (3.3V only !)
```

![配線写真 Pmod I2S2](/photo_pmodi2s2.jpg)



## ESP32とボタン(入力切り替え)の配線図
```
  ESP32          
  IO2   ------   BUTTON(TACT SWITCH connect to GND, pull-down)
```

