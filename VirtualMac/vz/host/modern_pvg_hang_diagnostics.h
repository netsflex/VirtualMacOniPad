#pragma once
#include <stdbool.h>

// Opt-in record of the pipelines and dispatches in failed GPU task command buffers.
// Enabled only while /var/root/VirtualMac2/shader-cache/hang-diagnostics exists.
bool VZModernInstallHangDiagnostics(id device);
