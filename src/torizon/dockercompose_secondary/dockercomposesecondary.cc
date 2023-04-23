#include <boost/filesystem/path.hpp>
#include <sstream>

#include "compose_manager.h"
#include "dockercomposesecondary.h"
#include "dockerofflineloader.h"
#include "libaktualizr/types.h"
#include "logging/logging.h"
#include "uptane/manifest.h"
#include "utilities/fault_injection.h"
#include "utilities/utils.h"

using std::stringstream;

namespace Primary {

constexpr const char* const DockerComposeSecondaryConfig::Type;

DockerComposeSecondaryConfig::DockerComposeSecondaryConfig(const Json::Value& json_config)
    : ManagedSecondaryConfig(Type) {
  partial_verifying = json_config["partial_verifying"].asBool();
  ecu_serial = json_config["ecu_serial"].asString();
  ecu_hardware_id = json_config["ecu_hardware_id"].asString();
  full_client_dir = json_config["full_client_dir"].asString();
  ecu_private_key = json_config["ecu_private_key"].asString();
  ecu_public_key = json_config["ecu_public_key"].asString();
  firmware_path = json_config["firmware_path"].asString();
  target_name_path = json_config["target_name_path"].asString();
  metadata_path = json_config["metadata_path"].asString();
}

std::vector<DockerComposeSecondaryConfig> DockerComposeSecondaryConfig::create_from_file(
    const boost::filesystem::path& file_full_path) {
  Json::Value json_config;
  std::ifstream json_file(file_full_path.string());
  Json::parseFromStream(Json::CharReaderBuilder(), json_file, &json_config, nullptr);
  json_file.close();

  std::vector<DockerComposeSecondaryConfig> sec_configs;
  sec_configs.reserve(json_config[Type].size());

  for (const auto& item : json_config[Type]) {
    sec_configs.emplace_back(DockerComposeSecondaryConfig(item));
  }
  return sec_configs;
}

void DockerComposeSecondaryConfig::dump(const boost::filesystem::path& file_full_path) const {
  Json::Value json_config;

  json_config["partial_verifying"] = partial_verifying;
  json_config["ecu_serial"] = ecu_serial;
  json_config["ecu_hardware_id"] = ecu_hardware_id;
  json_config["full_client_dir"] = full_client_dir.string();
  json_config["ecu_private_key"] = ecu_private_key;
  json_config["ecu_public_key"] = ecu_public_key;
  json_config["firmware_path"] = firmware_path.string();
  json_config["target_name_path"] = target_name_path.string();
  json_config["metadata_path"] = metadata_path.string();

  Json::Value root;
  root[Type].append(json_config);

  Json::StreamWriterBuilder json_bwriter;
  json_bwriter["indentation"] = "\t";
  std::unique_ptr<Json::StreamWriter> const json_writer(json_bwriter.newStreamWriter());

  boost::filesystem::create_directories(file_full_path.parent_path());
  std::ofstream json_file(file_full_path.string());
  json_writer->write(root, &json_file);
  json_file.close();
}

DockerComposeSecondary::DockerComposeSecondary(Primary::DockerComposeSecondaryConfig sconfig_in)
    : ManagedSecondary(std::move(sconfig_in)) {}

data::InstallationResult DockerComposeSecondary::sendFirmware(const Uptane::Target& target,
                                                              const InstallInfo& install_info,
                                                              const api::FlowControlToken* flow_control) {
  if (flow_control != nullptr && flow_control->hasAborted()) {
    return data::InstallationResult(data::ResultCode::Numeric::kOperationCancelled, "");
  }

  compose_manager_.cleanup();  // See rollbackPendingInstall() for cases where this isn't a no-op.

  Utils::writeFile(composeFileNew(), secondary_provider_->getTargetFileHandle(target));

  switch (install_info.getUpdateType()) {
    case UpdateType::kOnline:
      // Only try to pull images upon an online update.
      if (!compose_manager_.pull(composeFileNew(), flow_control)) {
        // Tidy up: remove the tmp file, and delete any partial downloads
        boost::filesystem::remove(composeFileNew());
        compose_manager_.cleanup();
        if (flow_control != nullptr && flow_control->hasAborted()) {
          return data::InstallationResult(data::ResultCode::Numeric::kOperationCancelled, "Aborted in docker-pull");
        }
        LOG_ERROR << "Error running docker-compose pull";
        return data::InstallationResult(data::ResultCode::Numeric::kDownloadFailed, "docker compose pull failed");
      }
      break;

    case UpdateType::kOffline: {
      auto img_path = install_info.getImagesPathOffline() / (target.sha256Hash() + ".images");
      auto man_path = install_info.getMetadataPathOffline() / "docker" / (target.sha256Hash() + ".manifests");
      boost::filesystem::path compose_out;

      if (!loadDockerImages(composeFileNew(), target.sha256Hash(), img_path, man_path, &compose_out)) {
        boost::filesystem::remove(composeFileNew());
        compose_manager_.cleanup();
        return data::InstallationResult(data::ResultCode::Numeric::kInstallFailed,
                                        "Loading offline docker images failed");
      }
      // Docker images loaded and an "offline" version of compose-file available.
      // Overwrite the new compose file with that "offline" version.
      boost::filesystem::rename(compose_out, composeFileNew());
      break;
    }
    default:
      return data::InstallationResult(data::ResultCode::Numeric::kInternalError, "Unknown UpdateType");
  }

  return data::InstallationResult(data::ResultCode::Numeric::kOk, "");
}

data::InstallationResult DockerComposeSecondary::install(const Uptane::Target& target, const InstallInfo& info,
                                                         const api::FlowControlToken* flow_control) {
  (void)info;
  // Don't try to abort during installation. The images were already fetched in
  // sendFirmware(), so this step should complete within a bounded time.
  (void)flow_control;
  LOG_INFO << "Updating containers via docker-compose";

  bool const sync_update = secondary_provider_->pendingPrimaryUpdate();
  if (sync_update) {
    // For a synchronous update, most of this step happens on reboot.
    LOG_INFO << "OSTree update pending. This is a synchronous update transaction.";
    return {data::ResultCode::Numeric::kNeedCompletion, ""};
  }

  if (boost::filesystem::exists(composeFile())) {
    if (!compose_manager_.down(composeFile())) {
      LOG_ERROR << "docker-compose down of old image failed";
      return {data::ResultCode::Numeric::kInstallFailed, "Docker compose down failed"};
    }
  }

  if (!compose_manager_.up(composeFileNew())) {
    // Attempt recovery
    boost::filesystem::remove(composeFileNew());
    const char* description;
    if (!boost::filesystem::exists(composeFile())) {
      LOG_ERROR << "docker-compose up of new image failed, and also could not recover"
                   " because the old image is not on disk";
      description = "Docker compose up failed, and no old image to restore";
    } else if (!compose_manager_.up(composeFile())) {
      LOG_ERROR << "docker-compose up of new image failed, and also could not recover"
                   " by docker-compose up on the old image";
      description = "Docker compose up failed, and restore failed";
      // Don't attempt to clean up the old images. Neither of them appear to
      // work, and we are leaving the system in a potentially broken state.
      // Prefer to keep things around that might aid recovery, at the risk of
      // consuming disk space and other resources.
    } else {
      LOG_WARNING << "docker-compose up of new image failed, recovered via docker-compose up on the old image";
      description = "Docker compose up failed (restore ok)";
      // Only clean up old images on this somewhat-happy path.
      compose_manager_.cleanup();
    }
    return {data::ResultCode::Numeric::kInstallFailed, description};
  }

  boost::filesystem::rename(composeFileNew(), composeFile());
  Utils::writeFile(sconfig.target_name_path, target.filename());
  compose_manager_.cleanup();
  return {data::ResultCode::Numeric::kOk, ""};
}

/**
 * This is called on reboot to complete an installation
 */
boost::optional<data::InstallationResult> DockerComposeSecondary::completePendingInstall(const Uptane::Target& target) {
  bool const sync_update = secondary_provider_->pendingPrimaryUpdate();
  if (sync_update) {
    // Even though we've rebooted, the primary hasn't installed. Wait for it to finish
    return {{data::ResultCode::Numeric::kNeedCompletion, ""}};
  }

  LOG_INFO << "Finishing pending container updates via docker-compose";

  if (!boost::filesystem::exists(composeFileNew())) {
    // Should never reach here in normal operation.
    LOG_ERROR << "ComposeManager::pendingUpdate : " << composeFileNew() << " does not exist";
    return {{data::ResultCode::Numeric::kInternalError, "completePendingInstall can't find composeFileNew()"}};
  }

  if (compose_manager_.checkRollback()) {
    // The primary failed to install. We are now booted into the old OS image. Fail our installation without attempting
    // an install. rollbackPendingInstall() will tidy things up
    return {{data::ResultCode::Numeric::kInstallFailed, "bootloader rolled back OS update"}};
  }

  if (!compose_manager_.up(composeFileNew())) {
    LOG_ERROR << "docker-compose up of new image failed during synchronous update";
    // The primary installed OK, but we failed. Recovery will be in rollbackPendingInstall()
    return {{data::ResultCode::Numeric::kInstallFailed, "Docker compose up failed"}};
  }

  // Install was OK
  boost::filesystem::rename(composeFileNew(), composeFile());
  Utils::writeFile(sconfig.target_name_path, target.filename());
  compose_manager_.cleanup();
  return {{data::ResultCode::Numeric::kOk, ""}};
}

void DockerComposeSecondary::rollbackPendingInstall() {
  LOG_INFO << "Rolling back container update";
  // This function handles a failed sync update and
  // performs a rollback on the needed ECUs to ensure sync.

  boost::filesystem::remove(composeFileNew());

  if (compose_manager_.checkRollback()) {
    // We are being asked to complete a pending synchronous install. However
    // the OS has triggered a rollback. The following things just happened:
    // 1) User requested a synchronous install of OSTree base OS + docker
    //    compose secondary.
    // 2) The OSTree update was installed, and Aktualizr intended to update
    //    the docker compose secondary after a reboot
    // 3) The device rebooted
    // 4) The new OS version was broken and didn't boot. U-Boot triggered a
    //    rollback because the bootcount was exceeded
    // 5) We're now booted in the previous OS version.
    // 6) DockerComposeSecondary::completePendingInstall() detected the
    //    rollback and failed the install without making any docker changes
    // 7) sotauptaneclient noted the installation failure and called us to tidy
    //    things up.

    // systemd didn't start either image. We've deleted composeFileNew() so
    // it will start docker-compose on the next boot, but this time we have to
    // trigger it.
    compose_manager_.up(composeFile());
    compose_manager_.cleanup();
  } else {
    // In this case (following on from above):
    // 4) The device rebooted into the new OS successfully
    // 5) DockerComposeSecondary::completePendingInstall() was called to perform the install
    // 6) docker-compose up failed

    // We need to:
    //  a) Remove composeFileNew() so systemd starts the old image
    //  b) Revert to the old OS image
    //  c) Prune the images that we downloaded
    // Step a) was done at the top of this function. We'll do step b) shortly. Step c) is trickier. Pruning images
    // requires the ones we want to keep to be running. We can't start them now, because we are running an incompatible
    // OS version (if not then a sync OS update is unnecessary). Since this case should be rare we'll keep the images
    // around and clear them up before the next download.
    CommandRunner::run("fw_setenv rollback 1");
    CommandRunner::run("reboot");
  }
}

bool DockerComposeSecondary::loadDockerImages(const boost::filesystem::path& compose_in,
                                              const std::string& compose_sha256,
                                              const boost::filesystem::path& images_path,
                                              const boost::filesystem::path& manifests_path,
                                              boost::filesystem::path* compose_out) {
  if (compose_out != nullptr) {
    compose_out->clear();
  }

  boost::filesystem::path compose_new = compose_in;
  compose_new.replace_extension(".off");

  try {
    auto dmcache = std::make_shared<DockerManifestsCache>(manifests_path);

    DockerComposeOfflineLoader dcloader(images_path, dmcache);
    dcloader.loadCompose(compose_in, compose_sha256);
    dcloader.dumpReferencedImages();
    dcloader.dumpImageMapping();
    dcloader.installImages();
    dcloader.writeOfflineComposeFile(compose_new);
    // TODO: [OFFUPD] Define how to perform the offline-online transformation (related to getFirmwareInfo()).

  } catch (std::runtime_error& exc) {
    // TODO: Consider throwing/handling custom exception types from dockerofflineloader and dockertarballloader.
    LOG_WARNING << "Offline loading failed: " << exc.what();
    return false;
  }

  if (compose_out != nullptr) {
    *compose_out = compose_new;
  }

  return true;
}

bool DockerComposeSecondary::getFirmwareInfo(Uptane::InstalledImageInfo& firmware_info) const {
  std::string content;

  if (!boost::filesystem::exists(sconfig.firmware_path)) {
    firmware_info.name = std::string("noimage");
    content = "";
  } else {
    if (!boost::filesystem::exists(sconfig.target_name_path)) {
      firmware_info.name = std::string("docker-compose.yml");
    } else {
      firmware_info.name = Utils::readFile(sconfig.target_name_path.string());
    }

    // Read compose-file and transform it into its original form in memory.
    DockerComposeFile dcfile;
    if (!dcfile.read(sconfig.firmware_path)) {
      LOG_WARNING << "Could not read compose " << sconfig.firmware_path;
      return false;
    }
    dcfile.backwardTransform();
    content = dcfile.toString();
  }

  firmware_info.hash = Uptane::ManifestIssuer::generateVersionHashStr(content);
  firmware_info.len = content.size();

  LOG_TRACE << "DockerComposeSecondary::getFirmwareInfo: hash=" << firmware_info.hash;

  return true;
}

}  // namespace Primary
