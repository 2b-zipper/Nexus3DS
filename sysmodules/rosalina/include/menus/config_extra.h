typedef struct {
	bool suppressLeds;
	bool cutSlotPower;
	bool cutSleepWifi;
	bool screenshotDateFolders;
	bool screenshotCombined;
	bool temperatureUnit;
	bool use12HourClock;
} config_extra;

extern config_extra configExtra;

void ConfigExtra_DrawDetailedMenu();
void ConfigExtra_ReadConfigExtra();