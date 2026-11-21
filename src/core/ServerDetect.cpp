// The single implementation unit for esserverdetect.h. It pulls in winsock2.h,
// so it must come before anything that drags in <windows.h>: keep this file free
// of every other include.
#define ESSD_IMPLEMENTATION
#include "esserverdetect.h"
