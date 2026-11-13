# Smart Desk Companion (ESP32-S3)

Flip clock (Wi-Fi time sync), live weather, Pomodoro timer and touch switches
on a 2.8" touch display.

## Hardware
- ESP32-S3 dev board
- 2.8" ST7789 TFT (320x240) with XPT2046 touch
- 3 relays + active buzzer (optional)

## Libraries (Arduino IDE)
- lvgl 8.3.x
- Adafruit GFX, Adafruit ST7735 and ST7789
- XPT2046_Touchscreen
- ArduinoJson 7.x

## Setup
1. Install the libraries above.
2. Copy `lv_conf.h` from this repo into your `Arduino/libraries/` folder
   (next to the `lvgl` folder).
3. Open the .ino file and set your Wi-Fi name, password and location.
4. Check the pin numbers at the top of the sketch match your wiring.
5. Select your ESP32-S3 board and upload.

If touch is mirrored, change TS_FLIP_X / TS_FLIP_Y in the sketch.

Made by Edge Tech Lab
