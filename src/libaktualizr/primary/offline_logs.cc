#include "primary/offline_logs.h"
#include "libaktualizr/types.h"
#include "logging/logging.h"

void OfflineLogs::RecordManifest(const Uptane::Manifest &manifest) { LOG_INFO << "Recording manifest " << manifest; }
