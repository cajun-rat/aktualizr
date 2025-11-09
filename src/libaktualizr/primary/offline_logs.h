#ifndef PRIMARY_OFFLINE_LOGS_H_
#define PRIMARY_OFFLINE_LOGS_H_

#include <boost/filesystem/path.hpp>
#ifdef BUILD_OFFLINE_UPDATES

#include "libaktualizr/types.h"

// A better implementation is as follows
// A pure virtual base class
// A 'null' implementation that does nothing
// A factory for a version that does more
// The journalctl fetching stuff only available if
// HAS_SYSTEMD is defined (or similar)

class OfflineLogs {
 public:
  // TOOD: Maybe make this a ctor instead?
  void SetOfflineUpdatePath(const boost::filesystem::path& path);
  void RecordManifest(const Uptane::Manifest& manifest);

 private:
};

#else

// Dummy class, used if Offline Updatea are disabled
class OfflineLogs {
 public:
  void RecordManifest(const Uptane::Manifest& /* manifest */) {}
};

#endif  // BUILD_OFFLINE_UPDATES
#endif  // PRIMARY_OFFLINE_LOGS_H_
