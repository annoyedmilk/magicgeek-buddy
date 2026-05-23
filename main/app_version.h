#pragma once

// Single source of truth for the firmware version string. Logged on boot,
// surfaced on /debug, and stamped into the OTA endpoint header so you can
// confirm over WiFi which build is actually running.
#define APP_VERSION "v0.3.5"
