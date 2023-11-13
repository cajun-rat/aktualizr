#include <fcntl.h>
#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/filesystem.hpp>
#include "json/json.h"

#include "libaktualizr/aktualizr.h"
#include "libaktualizr/config.h"
#include "libaktualizr/events.h"

#include <sys/file.h>
#include "httpfake.h"
#include "primary/aktualizr_helpers.h"
#include "primary/sotauptaneclient.h"
#include "uptane_test_common.h"
#include "utilities/utils.h"
#include "virtualsecondary.h"

boost::filesystem::path fake_meta_dir;     // NOLINT
boost::filesystem::path uptane_repos_dir;  // NOLINT

TEST(AktualizrUpdateLock, DisableUsingLock) {
  TemporaryDirectory temp_dir;
  auto http = std::make_shared<HttpFake>(temp_dir.Path(), "hasupdates", fake_meta_dir);
  Config conf = UptaneTestCommon::makeTestConfig(temp_dir, http->tls_server);
  auto lock_file = temp_dir.Path() / "update.lock";
  conf.uptane.update_lock_file = lock_file;

  auto storage = INvStorage::newStorage(conf.storage);
  UptaneTestCommon::TestAktualizr aktualizr(conf, storage, http);

  int fd = open(lock_file.c_str(), O_CREAT | O_RDWR);
  ASSERT_GE(fd, 0) << "Open lock file failed:" << strerror(errno);
  int lock_res = flock(fd, LOCK_EX);
  ASSERT_EQ(lock_res, 0) << "flock failed:" << strerror(errno);

  auto f_cb = [](const std::shared_ptr<event::BaseEvent>& event) {
    LOG_INFO << "Got " << event->variant;
    EXPECT_FALSE(event->isTypeOf<event::InstallStarted>());
  };
  boost::signals2::connection conn = aktualizr.SetSignalHandler(f_cb);

  aktualizr.Initialize();
  aktualizr.UptaneCycle();
}

#ifndef __NO_MAIN__
int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  if (argc != 2) {
    std::cerr << "Error: " << argv[0] << " requires the path to the base directory of Uptane repos.\n";
    return EXIT_FAILURE;
  }
  uptane_repos_dir = argv[1];

  logger_init();
  logger_set_threshold(boost::log::trivial::trace);

  TemporaryDirectory tmp_dir;
  fake_meta_dir = tmp_dir.Path();
  MetaFake meta_fake(fake_meta_dir);

  return RUN_ALL_TESTS();
}
#endif
