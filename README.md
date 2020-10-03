# Air quality sensor and display

## Description

Air quality sensor and display based on PM5003 particle counter and ST7789 based 1.4" TFT LCD

## Updating

After upgrading TFT_eSPI, need to update `.pio/libdeps/esp32dev/TFT_eSPI/User_Setup_Select.h`:

```cpp
// #include <User_Setup.h>           // Default setup is root library folder
#include "../../../../include/TFT_eSPI_Setups/User_Setup.h"
```
