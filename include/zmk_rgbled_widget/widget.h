
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
void indicate_battery(void);
#endif

#if IS_ENABLED(CONFIG_ZMK_BLE)
void indicate_connectivity(void);
#endif
