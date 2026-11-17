// The resident HOME panel's Wi-Fi/Bluetooth rows must drive the same backend
// the launcher's HOME overlay uses. These are the generic tg5040
// implementations already included by workspace/tg5040/platform/platform.c;
// including them here keeps keymon free of the video/input platform backend
// while sharing the actual Wi-Fi/BT code.
#include "defines.h"
#include "platform.h"
#include "api.h"
#include "utils.h"

#include <msettings.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <limits.h>
#include <unistd.h>

static inline void connection_reset(struct WIFI_connection *connection_info)
{
	connection_info->valid = false;
	connection_info->freq = -1;
	connection_info->link_speed = -1;
	connection_info->noise = -1;
	connection_info->rssi = -1;
	*connection_info->ip = '\0';
	*connection_info->ssid = '\0';
}

#define WIFI_SOCK_DIR "/etc/wifi/sockets"
#include "../../all/common/generic_wifi.c"
#include "../../all/common/generic_bt.c"
