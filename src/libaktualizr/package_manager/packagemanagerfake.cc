#include "packagemanagerfake.h"

Json::Value PackageManagerFake::getInstalledPackages() const {
  Json::Value packages(Json::arrayValue);
  Json::Value package;
  package["name"] = "fake-package";
  package["version"] = "1.0";
  packages.append(package);
  return packages;
}

Uptane::Target PackageManagerFake::getCurrent() const {
  std::vector<Uptane::Target> installed_versions;
  size_t current_k = SIZE_MAX;
  storage_->loadPrimaryInstalledVersions(&installed_versions, &current_k, nullptr);

  if (current_k != SIZE_MAX) {
    return installed_versions.at(current_k);
  }

  return getUnknown();
}

data::InstallationResult PackageManagerFake::install(const Uptane::Target &target) const {
  if (config.fake_need_reboot) {
    storage_->savePrimaryInstalledVersion(target, InstalledVersionUpdateMode::kPending);

    // set reboot flag to be notified later
    if (bootloader_ != nullptr) {
      bootloader_->rebootFlagSet();
    }

    return data::InstallationResult(data::ResultCode::Numeric::kNeedCompletion, "Application successful, need reboot");
  }

  storage_->savePrimaryInstalledVersion(target, InstalledVersionUpdateMode::kCurrent);
  return data::InstallationResult(data::ResultCode::Numeric::kOk, "Installing fake package was successful");
}

data::InstallationResult PackageManagerFake::finalizeInstall(const Uptane::Target &target) const {
  std::vector<Uptane::Target> targets;
  size_t pending_version = SIZE_MAX;
  storage_->loadPrimaryInstalledVersions(&targets, nullptr, &pending_version);

  if (pending_version == SIZE_MAX) {
    throw std::runtime_error("No pending update, nothing to finalize");
  }

  data::InstallationResult install_res;
  if (target == targets[pending_version]) {
    install_res = data::InstallationResult(data::ResultCode::Numeric::kOk, "Installing fake package was successful");
  } else {
    install_res =
        data::InstallationResult(data::ResultCode::Numeric::kInternalError, "Pending and new target do not match");
  }

  storage_->savePrimaryInstalledVersion(target, InstalledVersionUpdateMode::kCurrent);
  return install_res;
}
