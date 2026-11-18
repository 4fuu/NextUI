// keymon's small platform seam.
//
// keymon is a separate process and does not link the platform video/input
// backend. It still owns the model globals platform.h's JOY_* macros use, and
// api.c's shared status group asks for connectivity. That group must not spawn
// Wi-Fi/Bluetooth probes from the resident daemon, so both are reported off;
// the top-right status is the same battery-only pill as an offline desktop.
#include "defines.h"
#include "platform.h"
#include "api.h"

#include <msettings.h>
#include <stdlib.h>
#include <string.h>

int is_brick = 0;
int is_brickpro = 0;

void PLAT_initPlatform(void) {
	const char* device = getenv("DEVICE");
	is_brick = device && strcmp(device, "brick") == 0;
	is_brickpro = device && strcmp(device, "brickpro") == 0;
}

// Mirrors platform.c's helper (which keymon does not link): Brick models need
// SetRawBrightness(8) to come back from a raw 0, other panels restore through
// the user's brightness. The resident suspend path uses this instead of the
// video backend's copy.
void PLAT_enableBacklight(int enable) {
	if (enable) {
		if (is_brick || is_brickpro) SetRawBrightness(8);
		SetBrightness(GetBrightness());
	}
	else {
		SetRawBrightness(0);
	}
}

ConnectionStrength PLAT_connectionStrength(void) {
	return SIGNAL_STRENGTH_OFF;
}

bool PLAT_btIsConnected(void) {
	return false;
}
