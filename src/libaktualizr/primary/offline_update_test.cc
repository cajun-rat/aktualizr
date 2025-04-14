#include <gtest/gtest.h>
#include <libaktualizr/results.h>
#include <libaktualizr/types.h>

#include <boost/filesystem.hpp>
#include <string>

#include "crypto/crypto.h"
#include "httpfake.h"
#include "libaktualizr/aktualizr.h"
#include "test_utils.h"
#include "uptane_test_common.h"

namespace fs = boost::filesystem;

fs::path test_data;  // NOLINT

class OfflineUpdateTest : public ::testing::Test {
 protected:
  OfflineUpdateTest() {
    http_ = std::make_shared<HttpFake>(temp_dir_.Path(), "", meta_dir_ / "repo");
    conf_ = UptaneTestCommon::makeTestConfig(temp_dir_, http_->tls_server);
    conf_.pacman.fake_need_reboot = true;
    conf_.uptane.force_install_completion = true;
    conf_.bootloader.reboot_sentinel_dir = temp_dir_.Path();
    conf_.import.base_path = test_data / "offline2/import";
    conf_.uptane.offline_updates_source = test_data / "offline2";

    Utils::createDirectories(secondary_dir_, S_IRWXU);

    logger_set_threshold(boost::log::trivial::trace);

    storage_ = INvStorage::newStorage(conf_.storage);
    Primary::VirtualSecondaryConfig ecu_config;
    ecu_config.partial_verifying = false;
    ecu_config.full_client_dir = secondary_dir_;
    ecu_config.ecu_serial = "serial123";
    ecu_config.ecu_hardware_id = "docker-compose";
    ecu_config.ecu_private_key = "sec.priv";
    ecu_config.ecu_public_key = "sec.pub";
    ecu_config.firmware_path = secondary_dir_ / "firmware.txt";
    ecu_config.target_name_path = secondary_dir_ / "firmware_name.txt";
    ecu_config.metadata_path = secondary_dir_ / "secondary_metadata";

    conf_.uptane.secondary_config_file = temp_dir_ / "virtual_secondary_conf.json";
    ecu_config.dump(conf_.uptane.secondary_config_file);

    aktualizr_ = std::make_shared<UptaneTestCommon::TestAktualizr>(conf_, storage_, http_);
    aktualizr_->Initialize();
    client_ = aktualizr_->uptane_client();
  }

  TemporaryDirectory temp_dir_;
  fs::path meta_dir_{temp_dir_ / "online_metadata"};
  fs::path secondary_dir_{temp_dir_.Path() / "secondary"};
  std::shared_ptr<INvStorage> storage_;
  Config conf_;
  std::shared_ptr<HttpFake> http_;
  std::shared_ptr<UptaneTestCommon::TestAktualizr> aktualizr_;
  std::shared_ptr<SotaUptaneClient> client_;
};

// NOLINTNEXTLINE
TEST_F(OfflineUpdateTest, Regression) {
  auto res = aktualizr_->CheckUpdatesOffline(conf_.uptane.offline_updates_source).get();
  ASSERT_EQ(res.status, result::UpdateStatus::kUpdatesAvailable);
  auto download_res = aktualizr_->Download(res.updates, UpdateType::kOffline).get();
  ASSERT_EQ(download_res.status, result::DownloadStatus::kSuccess);

  auto install_result = aktualizr_->Install(res.updates, UpdateType::kOffline).get();
  ASSERT_EQ(install_result.dev_report.result_code, data::ResultCode(data::ResultCode::Numeric::kOk));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  if (argc != 2) {
    // NOLINTNEXTLINE
    std::cerr << "Error: " << argv[0] << " requires a path to tests/test_data\n";
    return EXIT_FAILURE;
  }
  // NOLINTNEXTLINE
  test_data = argv[1];

  if (!boost::filesystem::is_directory(test_data)) {
    std::cerr << "Error: " << test_data << " is not a directory\n";
    return EXIT_FAILURE;
  }

  logger_init();
  logger_set_threshold(boost::log::trivial::trace);

  return RUN_ALL_TESTS();
}
