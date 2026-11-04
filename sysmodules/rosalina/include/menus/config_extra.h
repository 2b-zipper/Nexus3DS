typedef struct {
	bool cutSlotPower;
	bool cutSleepWifi;
	bool screenshotDateFolders;
	bool screenshotCombined;
} config_extra;

extern config_extra configExtra;

void ConfigExtra_DrawDetailedMenu();
void ConfigExtra_ReadConfigExtra();