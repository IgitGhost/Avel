#pragma once

namespace avel::platform {

// Sets process-wide COM security: calls into this process are accepted from SYSTEM, LOCAL SERVICE,
// NETWORK SERVICE, Administrators and the process's own user. System Restore's service calls back
// into the process that creates a restore point and needs this (see the SRSetRestorePoint
// documentation). COM allows it once per process, before any marshalling, so call it at the start
// of main() on a thread that has initialized COM. Failure is not fatal: COM then applies its defaults.
void initialize_com_security();

}  // namespace avel::platform
