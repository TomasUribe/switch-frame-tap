/* v0.7.15: the canonical capture-mode parser lives with the sysmodule
 * because the applet-mitm build copies only its own source tree. The manager
 * and the overlay include this forwarding header. */
#pragma once
#include "../tier4/applet-mitm/source/capture_mode.hpp"
