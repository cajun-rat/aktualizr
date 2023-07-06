#include <boost/filesystem.hpp>
#include <chrono>
#include <fstream>

#include <sodium.h>

#include "libaktualizr/aktualizr.h"
#include "libaktualizr/events.h"
#include "primary/sotauptaneclient.h"
#include "utilities/apiqueue.h"
#include "utilities/timer.h"

using std::make_shared;
using std::move;
using std::shared_ptr;

namespace bf = boost::filesystem;

Aktualizr::Aktualizr(const Config &config)
    : Aktualizr(config, INvStorage::newStorage(config.storage), std::make_shared<HttpClient>()) {}

Aktualizr::Aktualizr(Config config, std::shared_ptr<INvStorage> storage_in,
                     const std::shared_ptr<HttpInterface> &http_in)
    : config_{std::move(config)}, sig_{new event::Channel()}, api_queue_{new api::CommandQueue()} {
  if (sodium_init() == -1) {  // Note that sodium_init doesn't require a matching 'sodium_deinit'
    throw std::runtime_error("Unable to initialize libsodium");
  }

  storage_ = std::move(storage_in);
  storage_->importData(config_.import);

  uptane_client_ = std::make_shared<SotaUptaneClient>(config_, storage_, http_in, sig_, api_queue_->FlowControlToken());

  updates_disabled_ = false;
}

Aktualizr::~Aktualizr() { api_queue_.reset(nullptr); }

void Aktualizr::Initialize() {
  uptane_client_->initialize();
  api_queue_->run();
}

void Aktualizr::DisableUpdates(bool status) { updates_disabled_ = status; }

bool Aktualizr::UptaneCycle() {
  {
    std::lock_guard<std::mutex> const lock{exit_cond_.m};
    if (exit_cond_.run_mode != RunMode::kStop) {
      LOG_WARNING
          << "UptaneCycle() was called in parallel with either UptaneCycle() or RunForever(). This is not supported";
    }
    exit_cond_.run_mode = RunMode::kOnce;
  }
  auto res = RunUpdateLoop();
  // false -> reboot required
  return res != ExitReason::kRebootRequired;
}

std::future<void> Aktualizr::RunForever() {
  {
    std::lock_guard<std::mutex> const lock{exit_cond_.m};
    if (exit_cond_.run_mode != RunMode::kStop) {
      LOG_WARNING
          << "RunForever() was called parallel with either UptaneCycle() or RunForever(). This is not supported";
    }
    exit_cond_.run_mode = RunMode::kUntilRebootNeeded;
  }
  std::future<void> future = std::async(std::launch::async, [this]() { RunUpdateLoop(); });
  return future;
}

std::ostream &operator<<(std::ostream &os, Aktualizr::UpdateCycleState state) {
  switch (state) {
    case Aktualizr::UpdateCycleState::kUnprovisioned:
      os << "Unprovisioned";
      break;
    case Aktualizr::UpdateCycleState::kSendingDeviceData:
      os << "SendingDeviceData";
      break;
    case Aktualizr::UpdateCycleState::kIdle:
      os << "Idle";
      break;
    case Aktualizr::UpdateCycleState::kSendingManifest:
      os << "SendingManifest";
      break;
    case Aktualizr::UpdateCycleState::kCheckingForUpdates:
      os << "CheckingForUpdates";
      break;
    case Aktualizr::UpdateCycleState::kDownloading:
      os << "Downloading";
      break;
    case Aktualizr::UpdateCycleState::kInstalling:
      os << "Installing";
      break;
    case Aktualizr::UpdateCycleState::kCheckingForUpdatesOffline:
      os << "CheckingForUpdatesOffline";
      break;
    case Aktualizr::UpdateCycleState::kFetchingImagesOffline:
      os << "FetchingImagesOffline";
      break;
    case Aktualizr::UpdateCycleState::kInstallingOffline:
      os << "InstallingOffline";
      break;
    case Aktualizr::UpdateCycleState::kAwaitReboot:
      os << "AwaitReboot";
      break;
    default:
      os << "Unknown(" << static_cast<int>(state) << ")";
      break;
  }
  return os;
}

Aktualizr::ExitReason Aktualizr::RunUpdateLoop() {
  assert(config_.uptane.polling_sec > 0);  // enforced by Config::postUpdateValues()

  next_online_poll_ = next_offline_poll_ = Clock::now();
  if (!config_.uptane.enable_offline_updates) {
    // We'd like to use Clock::time_point::max() here, but std::future::wait_until(Clock::time_point::max()) completes
    // instantly rather than behaving the same as std::future::wait().
    next_offline_poll_ = Clock::now() + std::chrono::hours(24 * 365 * 10);
  }

  int64_t loops = 0;
  Clock::time_point marker_time;

  while (exit_cond_.get() != RunMode::kStop) {
    auto now = Clock::now();

    // This is to protect against programming errors in the logic below. There should never be a set of states that
    // execute in a hard loop, but if we made a mistake this will limit the damage.
    if (loops++ % 100 == 0) {
      if (now < marker_time + std::chrono::seconds(10)) {
        // We should only go around this loop once / second
        LOG_WARNING << "Aktualizr::RunUpdateLoop is spinning in state " << state_ << " sleeping...";
        std::this_thread::sleep_for(std::chrono::seconds(10));
      }
      marker_time = now;
    }

    if (next_offline_poll_ <= now) {
      next_offline_poll_ = now + std::chrono::seconds(1);
      // Poll the magic filesystem directory for offline updates once per second.
      switch (state_) {
        case UpdateCycleState::kUnprovisioned:
        case UpdateCycleState::kSendingDeviceData:
        case UpdateCycleState::kIdle:
        case UpdateCycleState::kSendingManifest:
        case UpdateCycleState::kCheckingForUpdates:
        case UpdateCycleState::kDownloading:
        case UpdateCycleState::kInstalling:
          // In these cases we need to poll for Offline updates
          if (OfflineUpdateAvailable()) {
            api_queue_->abort();
            // TODO: How can we send an 'update failed' the next time we have idle network
            op_update_check_ = CheckUpdatesOffline(config_.uptane.offline_updates_source);
            state_ = UpdateCycleState::kCheckingForUpdatesOffline;
          }
          break;
        case UpdateCycleState::kCheckingForUpdatesOffline:
        case UpdateCycleState::kFetchingImagesOffline:
        case UpdateCycleState::kInstallingOffline:
        case UpdateCycleState::kAwaitReboot:
          // We cannot start an offline update in these states
          break;
        default:
          LOG_ERROR << "Unknown state:" << state_;
          state_ = UpdateCycleState::kIdle;
      }
    }
    // Drive the main event loop
    switch (state_) {
      case UpdateCycleState::kUnprovisioned:
        if (next_online_poll_ <= now && !op_bool_.valid()) {
          op_bool_ = AttemptProvision();
        }
        if (op_bool_.valid() && op_bool_.wait_until(next_offline_poll_) == std::future_status::ready) {
          if (op_bool_.get()) {
            // Provisioned OK, send device data
            op_void_ = SendDeviceData();
            state_ = UpdateCycleState::kSendingDeviceData;
          } else {
            // If we didn't provision, then stay in this state. We'll wait until next_online_poll_ before trying again
            next_online_poll_ = now + std::chrono::seconds(config_.uptane.polling_sec);
          }
          op_bool_ = {};  // Clear future
        }
        break;
      case UpdateCycleState::kSendingDeviceData:
        if (op_void_.wait_until(next_offline_poll_) == std::future_status::ready) {
          state_ = UpdateCycleState::kIdle;
        }
        break;
      case UpdateCycleState::kIdle:
        if (next_online_poll_ <= now) {
          op_update_check_ = CheckUpdates();
          state_ = UpdateCycleState::kCheckingForUpdates;
          next_online_poll_ = now + std::chrono::seconds(config_.uptane.polling_sec);
        } else {
          // Idle
          std::unique_lock<std::mutex> guard{exit_cond_.m};
          if (exit_cond_.run_mode == RunMode::kOnce) {
            // We've performed one round of checks, exit from 'once' runmode.
            LOG_DEBUG << "Exiting from RunMode::kOnce";
            exit_cond_.run_mode = RunMode::kStop;
            return ExitReason::kNoUpdates;
          }
          auto next_wake_up = std::min(next_offline_poll_, next_online_poll_);
          exit_cond_.cv.wait_until(guard, next_wake_up);
        }
        break;
      case UpdateCycleState::kSendingManifest:
        if (op_bool_.wait_until(next_offline_poll_) == std::future_status::ready) {
          next_online_poll_ = now + std::chrono::seconds(config_.uptane.polling_sec);
          state_ = UpdateCycleState::kIdle;
        }
        break;
      case UpdateCycleState::kCheckingForUpdates:
        if (op_update_check_.wait_until(next_offline_poll_) == std::future_status::ready) {
          result::UpdateCheck const update_result = op_update_check_.get();
          if (updates_disabled_) {
            next_online_poll_ = now + std::chrono::seconds(config_.uptane.polling_sec);
            state_ = UpdateCycleState::kIdle;
            break;
          }
          if (update_result.updates.empty()) {
            if (update_result.status == result::UpdateStatus::kError) {
              op_bool_ = SendManifest();
              state_ = UpdateCycleState::kSendingManifest;
              break;
            }
            next_online_poll_ = now + std::chrono::seconds(config_.uptane.polling_sec);
            state_ = UpdateCycleState::kIdle;
            break;
          }
          // Got an update
          op_download_ = Download(update_result.updates);
          state_ = UpdateCycleState::kDownloading;
        }
        break;
      case UpdateCycleState::kDownloading:
        if (op_download_.wait_until(next_offline_poll_) == std::future_status::ready) {
          result::Download const download_result = op_download_.get();
          if (download_result.status != result::DownloadStatus::kSuccess || download_result.updates.empty()) {
            if (download_result.status != result::DownloadStatus::kNothingToDownload) {
              // If the download failed, inform the backend immediately.
              op_bool_ = SendManifest();
              state_ = UpdateCycleState::kSendingManifest;
            }
            break;
          }
          op_install_ = Install(download_result.updates);
          state_ = UpdateCycleState::kInstalling;
        }
        break;
      case UpdateCycleState::kInstalling:
        if (op_install_.wait_until(next_offline_poll_) == std::future_status::ready) {
          if (uptane_client_->isInstallCompletionRequired()) {
            state_ = UpdateCycleState::kAwaitReboot;
            break;
          }
          if (!uptane_client_->hasPendingUpdates()) {
            // If updates were applied and no any reboot/finalization is required then send/put manifest
            // as soon as possible, don't wait for config_.uptane.polling_sec
            op_bool_ = SendManifest();
            state_ = UpdateCycleState::kSendingManifest;
          } else {
            next_online_poll_ = now + std::chrono::seconds(config_.uptane.polling_sec);
            state_ = UpdateCycleState::kIdle;
          }
        }
        break;
      case UpdateCycleState::kCheckingForUpdatesOffline: {
        result::UpdateCheck const update_result = op_update_check_.get();  // No need to timeout
        if (update_result.updates.empty() || updates_disabled_) {
          next_online_poll_ = now + std::chrono::seconds(config_.uptane.polling_sec);
          state_ = UpdateCycleState::kIdle;
          break;
        }
        op_download_ = FetchImagesOffline(update_result.updates);
        state_ = UpdateCycleState::kFetchingImagesOffline;
        break;
      }
      case UpdateCycleState::kFetchingImagesOffline: {
        result::Download const download_result = op_download_.get();
        if (download_result.status != result::DownloadStatus::kSuccess || download_result.updates.empty()) {
          state_ = UpdateCycleState::kIdle;
          break;
        }
        op_install_ = InstallOffline(download_result.updates);
        state_ = UpdateCycleState::kInstallingOffline;
        break;
      }
      case UpdateCycleState::kInstallingOffline: {
        result::Install const install_result = op_install_.get();
        if (uptane_client_->isInstallCompletionRequired()) {
          state_ = UpdateCycleState::kAwaitReboot;
          break;
        }
        // If we have already provisioned/send device data, then this will repeat those steps. However provisioning is
        // cheap if it has already completed, and so all that happens is we send device data again. That does not seem
        // like two big a cost to simplify the logic, give that we've installed an entire update to get here.
        state_ = UpdateCycleState::kUnprovisioned;
        break;
      }
      case UpdateCycleState::kAwaitReboot: {
        uptane_client_->completeInstall();
        std::lock_guard<std::mutex> const lock{exit_cond_.m};
        exit_cond_.run_mode = RunMode::kStop;
        exit_cond_.cv.notify_all();
        return ExitReason::kRebootRequired;
      }
      default:
        LOG_ERROR << "Unknown state:" << state_;
        state_ = UpdateCycleState::kIdle;
    }
  };
  LOG_INFO << "RunForever thread exiting";
  return ExitReason::kStopRequested;
}

void Aktualizr::Shutdown() {
  std::lock_guard<std::mutex> const guard{exit_cond_.m};
  exit_cond_.run_mode = RunMode::kStop;
  exit_cond_.cv.notify_all();
}

void Aktualizr::AddSecondary(const std::shared_ptr<SecondaryInterface> &secondary) {
  uptane_client_->addSecondary(secondary);
}

void Aktualizr::SetSecondaryData(const Uptane::EcuSerial &ecu, const std::string &data) {
  storage_->saveSecondaryData(ecu, data);
}

std::vector<SecondaryInfo> Aktualizr::GetSecondaries() const {
  std::vector<SecondaryInfo> info;
  storage_->loadSecondariesInfo(&info);

  return info;
}

std::future<bool> Aktualizr::AttemptProvision() {
  std::function<bool()> task([this] { return uptane_client_->attemptProvision(); });
  return api_queue_->enqueue(std::move(task));
}

std::future<result::CampaignCheck> Aktualizr::CampaignCheck() {
  std::function<result::CampaignCheck()> task([this] { return uptane_client_->campaignCheck(); });
  return api_queue_->enqueue(std::move(task));
}

std::future<void> Aktualizr::CampaignControl(const std::string &campaign_id, campaign::Cmd cmd) {
  std::function<void()> task([this, campaign_id, cmd] {
    switch (cmd) {
      case campaign::Cmd::Accept:
        uptane_client_->campaignAccept(campaign_id);
        break;
      case campaign::Cmd::Decline:
        uptane_client_->campaignDecline(campaign_id);
        break;
      case campaign::Cmd::Postpone:
        uptane_client_->campaignPostpone(campaign_id);
        break;
      default:
        break;
    }
  });
  return api_queue_->enqueue(std::move(task));
}

void Aktualizr::SetCustomHardwareInfo(Json::Value hwinfo) { uptane_client_->setCustomHardwareInfo(std::move(hwinfo)); }
std::future<void> Aktualizr::SendDeviceData() {
  std::function<void()> task([this] { uptane_client_->sendDeviceData(); });
  return api_queue_->enqueue(std::move(task));
}

// FIXME: [TDX] This solution must be reviewed (we should probably have a method to be used just for the data proxy).
std::future<void> Aktualizr::SendDeviceData(const Json::Value &hwinfo) {
  std::function<void()> task([this, hwinfo] {
    uptane_client_->setCustomHardwareInfo(hwinfo);
    uptane_client_->sendDeviceData();
  });
  return api_queue_->enqueue(std::move(task));
}

std::future<void> Aktualizr::CompleteSecondaryUpdates() {
  std::function<void()> task([this] { return uptane_client_->completePreviousSecondaryUpdates(); });
  return api_queue_->enqueue(std::move(task));
}

std::future<result::UpdateCheck> Aktualizr::CheckUpdates() {
  std::function<result::UpdateCheck()> task([this] { return uptane_client_->fetchMeta(); });
  return api_queue_->enqueue(std::move(task));
}

std::future<result::Download> Aktualizr::Download(const std::vector<Uptane::Target> &updates) {
  std::function<result::Download()> task([this, updates]() { return uptane_client_->downloadImages(updates); });
  return api_queue_->enqueue(std::move(task));
}

std::future<result::Install> Aktualizr::Install(const std::vector<Uptane::Target> &updates) {
  std::function<result::Install()> task([this, updates] { return uptane_client_->uptaneInstall(updates); });
  return api_queue_->enqueue(std::move(task));
}

bool Aktualizr::SetInstallationRawReport(const std::string &custom_raw_report) {
  return storage_->storeDeviceInstallationRawReport(custom_raw_report);
}

std::future<bool> Aktualizr::SendManifest(const Json::Value &custom) {
  std::function<bool()> task([this, custom]() { return uptane_client_->putManifest(custom); });
  return api_queue_->enqueue(std::move(task));
}

result::Pause Aktualizr::Pause() {
  if (api_queue_->pause(true)) {
    uptane_client_->reportPause();
    return result::PauseStatus::kSuccess;
  } else {
    return result::PauseStatus::kAlreadyPaused;
  }
}

result::Pause Aktualizr::Resume() {
  if (api_queue_->pause(false)) {
    uptane_client_->reportResume();
    return result::PauseStatus::kSuccess;
  } else {
    return result::PauseStatus::kAlreadyRunning;
  }
}

void Aktualizr::Abort() { api_queue_->abort(); }

boost::signals2::connection Aktualizr::SetSignalHandler(
    const std::function<void(shared_ptr<event::BaseEvent>)> &handler) {
  return sig_->connect(handler);
}

Aktualizr::InstallationLog Aktualizr::GetInstallationLog() {
  std::vector<Aktualizr::InstallationLogEntry> ilog;

  EcuSerials serials;
  if (!uptane_client_->getEcuSerials(&serials)) {
    throw std::runtime_error("Could not load ECU serials");
  }

  ilog.reserve(serials.size());
  for (const auto &s : serials) {
    Uptane::EcuSerial serial = s.first;
    std::vector<Uptane::Target> installs;

    std::vector<Uptane::Target> log;
    storage_->loadInstallationLog(serial.ToString(), &log, true);

    ilog.emplace_back(Aktualizr::InstallationLogEntry{serial, std::move(log)});
  }

  return ilog;
}

std::vector<Uptane::Target> Aktualizr::GetStoredTargets() { return uptane_client_->getStoredTargets(); }

void Aktualizr::DeleteStoredTarget(const Uptane::Target &target) { uptane_client_->deleteStoredTarget(target); }

std::ifstream Aktualizr::OpenStoredTarget(const Uptane::Target &target) {
  return uptane_client_->openStoredTarget(target);
}

#ifdef BUILD_OFFLINE_UPDATES
bool Aktualizr::OfflineUpdateAvailable() {
  static const std::string update_subdir{"metadata"};

  OffUpdSourceState old_state = offupd_source_state_;
  OffUpdSourceState cur_state = OffUpdSourceState::Unknown;

  boost::system::error_code ec;
  if (bf::exists(config_.uptane.offline_updates_source, ec)) {
    if (bf::is_directory(config_.uptane.offline_updates_source / update_subdir, ec)) {
      cur_state = OffUpdSourceState::SourceExists;
    } else {
      cur_state = OffUpdSourceState::SourceExistsNoContent;
    }
  } else {
    cur_state = OffUpdSourceState::SourceDoesNotExist;
  }

  offupd_source_state_ = cur_state;
  // LOG_INFO << "OfflineUpdateAvailable: " << int(old_state) << " -> " << int(cur_state);

  return (old_state == OffUpdSourceState::SourceDoesNotExist && cur_state == OffUpdSourceState::SourceExists);
}

std::future<result::UpdateCheck> Aktualizr::CheckUpdatesOffline(const boost::filesystem::path &source_path) {
  std::function<result::UpdateCheck()> task(
      [this, source_path] { return uptane_client_->fetchMetaOffUpd(source_path); });
  return api_queue_->enqueue(std::move(task));
}

std::future<result::Download> Aktualizr::FetchImagesOffline(const std::vector<Uptane::Target> &updates) {
  std::function<result::Download()> task([this, updates]() { return uptane_client_->fetchImagesOffUpd(updates); });
  return api_queue_->enqueue(std::move(task));
}

std::future<result::Install> Aktualizr::InstallOffline(const std::vector<Uptane::Target> &updates) {
  std::function<result::Install()> task([this, updates] { return uptane_client_->uptaneInstallOffUpd(updates); });
  return api_queue_->enqueue(std::move(task));
}
#endif