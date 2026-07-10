https://github.com/crosspoint-reader/crosspoint-reader/discussions/2468

### schematics
https://github.com/sunwoods/Xteink-X3
https://github.com/sunwoods/Xteink-X4/issues/1


### power management / ble
https://espressif.github.io/esp32-c3-book-en/chapter_12/12.2/index.html
https://github.com/espressif/esp-idf/tree/v6.0.1/examples/bluetooth/nimble/power_save


## powersave ble

The following information is about powersave, which I have the ability to do

That log is super useful: Bluetooth modem sleep is enabled and the controller says it is sleeping, but the BT controller still owns the system NO_LIGHT_SLEEP lock. I’m going to look at the ESP-IDF controller code/config around btLS; the likely split is “BT modem sleep” versus “whole-chip auto light sleep,” and lpclk=1 is a big clue.



The source matches the log: with lpclk=1, BT is using the main crystal as its low-power clock. On ESP32-C3 that can let the BT controller do modem sleep while still holding the system light-sleep lock, unless the build is configured to keep the XTAL powered during light sleep or to use the RTC slow clock path. I’m pulling the exact condition before changing config.



The exact IDF condition is the smoking gun: when BT’s low-power clock is MAIN_XTAL and CONFIG_BT_CTRL_MAIN_XTAL_PU_DURING_LIGHT_SLEEP is not enabled, the controller sets no_light_sleep=1, creates btLS, and acquires it for the entire time BT is enabled. I’m going to flip that build option on for the companion BLE config.



That option tells the controller it may allow system light sleep while BT uses the main crystal as the BT sleep clock. It will not be the absolute lowest theoretical power, because the XTAL remains powered during those sleeps, but it should remove the always-held btLS blocker without switching to the inaccurate internal RC clock.

### functions into flash

  CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH=y

  This happened when I started down the path of trying to do power management callbacks and logging
  it also had to do some patches
  

### Fixing ble sleep
The diagnostic found the culprit. This is not NimBLE app work.
Your pattern:
held=75.9ms sleepGap=925ms acqPer=1001ms relPer=1001ms
matches the ESP-IDF BT controller’s early wake margin almost perfectly.
In the ESP32-C3 BT controller source, btdm_sleep_enter_phase1_wrapper() does this when RTC slow clock is the internal RC clock:
uncertainty = us_to_sleep * 7 / 100;
That is 7% early wake. Your cycle is about 1001ms; 7% is about 70ms, plus a few ms of PHY/controller overhead, which gives the ~76ms APB hold.
So the big answer is: the BT controller is waking early because its sleep timing is based on the inaccurate internal RTC RC clock. The earlier 343ms hold also makes sense: 343ms / 0.07 = ~4.9s, which lines up with the BLE latency window.
CONFIG_BT_CTRL_MODEM_SLEEP_MODE_1 is not the problem. That is the supported BT modem sleep mode. The expensive combo is:
CONFIG_BT_CTRL_LPCLK_SEL_MAIN_XTAL=y
CONFIG_BT_CTRL_MAIN_XTAL_PU_DURING_LIGHT_SLEEP=y
CONFIG_RTC_CLK_SRC_INT_RC=y
Best next experiments:
Best, if X3 has a 32 kHz crystal/oscillator:
switch RTC/BT low-power clock to external 32 kHz. That should reduce the early wake margin from ~7% to roughly the minimum timer uncertainty, around a few ms.

If no 32 kHz clock exists:
try CONFIG_RTC_CLK_SRC_INT_8MD256=y as an experiment. It may avoid the RTC_SLOW_CLK_SRC_RC_SLOW 7% path, but we’d need to measure current because that clock source may have its own power cost.

Do not just chase slave latency higher yet.
With the current RC-clock path, longer BLE sleeps produce proportionally longer early APB holds. It lowers wake count, but the BT APB percentage can stay around that same ~7%.

This is a good find. The “missing 7%” is very likely BT controller clock-uncertainty margin, not unexplained CPU work.

8:21 PM






can you link me the code on github?



8:22 PM






Worked for 52s



Yep. Your PlatformIO package reports ESP-IDF 5.5.2, so these are the closest GitHub links:
BT controller sleep timer / uncertainty logic:
https://github.com/espressif/esp-idf/blob/v5.5.2/components/bt/controller/esp32c3/bt.c#L1134-L1160

BT APB lock release on sleep enter:
https://github.com/espressif/esp-idf/blob/v5.5.2/components/bt/controller/esp32c3/bt.c#L1168-L1183

BT APB lock acquire on sleep exit:
https://github.com/espressif/esp-idf/blob/v5.5.2/components/bt/controller/esp32c3/bt.c#L1213-L1227

ESP32-C3 BT modem sleep / low-power clock Kconfig:
https://github.com/espressif/esp-idf/blob/v5.5.2/components/bt/controller/esp32c3/Kconfig.in#L388-L424

Public esp_bt_sleep_enable() docs/comments about controller periodically waking:
https://github.com/espressif/esp-idf/blob/v5.5.2/components/bt/include/esp32c3/include/esp_bt.h#L720-L724

If GitHub’s line anchors drift, search within bt.c for BTDM_RTC_SLOW_CLK_RC_DRIFT_PERCENT. That is the 7% uncertainty path matching your ~76ms holds.