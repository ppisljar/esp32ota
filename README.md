# ESP32OTA

Minimal OTA webserver for [ESP32Ctrl](http://github.com/ppisljar/ESP32Ctrl)

Provides a webserver with two endpoints:

`/reboot` restarts and boots the ota_0 partition
`/update` allows you to send POST request with new binary for ota_0 partiion

should be flashed on ota_1