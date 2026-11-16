// pin16: main. Own code.
#include "sl_system_init.h"
#include "sl_system_process_action.h"
#include "sl_power_manager.h"
#include "app.h"

int main(void)
{
  sl_system_init();            // device, clocks, power manager, Bluetooth stack
  app_init();
  while (1) {
    sl_system_process_action();    // the Silicon Labs components (Bluetooth events); must be called from this loop
    app_process_action();
    sl_power_manager_sleep();      // sleeps if app_is_ok_to_sleep() allows it
  }
}
