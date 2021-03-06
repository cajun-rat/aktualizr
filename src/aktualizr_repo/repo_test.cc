#include <gtest/gtest.h>

#include <iostream>
#include <string>

#include <boost/filesystem.hpp>

#include "config/config.h"
#include "logging/logging.h"
#include "test_utils.h"
#include "uptane_repo.h"

KeyType key_type = KeyType::kED25519;
std::string generate_repo_exec;

TEST(aktualizr_repo, generate_repo) {
  TemporaryDirectory temp_dir;
  UptaneRepo repo(temp_dir.Path(), "", "correlation");
  repo.generateRepo(key_type);
  EXPECT_TRUE(boost::filesystem::exists(temp_dir.Path() / "repo/director/root.json"));
  EXPECT_TRUE(boost::filesystem::exists(temp_dir.Path() / "repo/director/1.root.json"));
  EXPECT_TRUE(boost::filesystem::exists(temp_dir.Path() / "repo/director/targets.json"));
  EXPECT_TRUE(boost::filesystem::exists(temp_dir.Path() / "repo/director/timestamp.json"));
  EXPECT_TRUE(boost::filesystem::exists(temp_dir.Path() / "repo/image/root.json"));
  EXPECT_TRUE(boost::filesystem::exists(temp_dir.Path() / "repo/image/1.root.json"));
  EXPECT_TRUE(boost::filesystem::exists(temp_dir.Path() / "repo/image/targets.json"));
  EXPECT_TRUE(boost::filesystem::exists(temp_dir.Path() / "repo/image/timestamp.json"));
  EXPECT_TRUE(boost::filesystem::exists(temp_dir.Path() / "repo/director/manifest"));

  EXPECT_TRUE(boost::filesystem::exists(temp_dir.Path() / "keys/image/root/private.key"));
  EXPECT_TRUE(boost::filesystem::exists(temp_dir.Path() / "keys/image/root/public.key"));
  EXPECT_TRUE(boost::filesystem::exists(temp_dir.Path() / "keys/image/snapshot/private.key"));
  EXPECT_TRUE(boost::filesystem::exists(temp_dir.Path() / "keys/image/snapshot/public.key"));
  EXPECT_TRUE(boost::filesystem::exists(temp_dir.Path() / "keys/image/targets/private.key"));
  EXPECT_TRUE(boost::filesystem::exists(temp_dir.Path() / "keys/image/targets/public.key"));
  EXPECT_TRUE(boost::filesystem::exists(temp_dir.Path() / "keys/image/timestamp/private.key"));
  EXPECT_TRUE(boost::filesystem::exists(temp_dir.Path() / "keys/image/timestamp/public.key"));

  EXPECT_TRUE(boost::filesystem::exists(temp_dir.Path() / "keys/director/root/private.key"));
  EXPECT_TRUE(boost::filesystem::exists(temp_dir.Path() / "keys/director/root/public.key"));
  EXPECT_TRUE(boost::filesystem::exists(temp_dir.Path() / "keys/director/snapshot/private.key"));
  EXPECT_TRUE(boost::filesystem::exists(temp_dir.Path() / "keys/director/snapshot/public.key"));
  EXPECT_TRUE(boost::filesystem::exists(temp_dir.Path() / "keys/director/targets/private.key"));
  EXPECT_TRUE(boost::filesystem::exists(temp_dir.Path() / "keys/director/targets/public.key"));
  EXPECT_TRUE(boost::filesystem::exists(temp_dir.Path() / "keys/director/timestamp/private.key"));
  EXPECT_TRUE(boost::filesystem::exists(temp_dir.Path() / "keys/director/timestamp/public.key"));

  std::vector<std::string> keys;
  std::function<void(const boost::filesystem::path &path)> recursive_check;
  recursive_check = [&keys, &recursive_check](const boost::filesystem::path &path) {
    for (auto &p : boost::filesystem::directory_iterator(path)) {
      std::cout << "PATH: " << p.path() << "\n";
      if (p.status().type() == boost::filesystem::file_type::directory_file) {
        recursive_check(p.path());
      } else {
        if (p.path().filename().string() == "key_type") {
          continue;
        }
        std::string hash = Crypto::sha512digest(Utils::readFile(p.path()));
        if (std::find(keys.begin(), keys.end(), hash) == keys.end()) {
          keys.push_back(hash);
        } else {
          FAIL() << p.path().string() << " is not unique";
        }
      }
    }
  };
  recursive_check(temp_dir.Path() / "keys");

  Json::Value image_targets = Utils::parseJSONFile(temp_dir.Path() / "repo/image/targets.json");
  EXPECT_EQ(image_targets["signed"]["targets"].size(), 0);
  Json::Value director_targets = Utils::parseJSONFile(temp_dir.Path() / "repo/director/targets.json");
  EXPECT_EQ(director_targets["signed"]["targets"].size(), 0);
  EXPECT_EQ(director_targets["signed"]["custom"]["correlationId"], "correlation");
}

TEST(aktualizr_repo, add_image) {
  TemporaryDirectory temp_dir;
  UptaneRepo repo(temp_dir.Path(), "", "");
  repo.generateRepo(key_type);
  repo.addImage(temp_dir.Path() / "repo/director/manifest");
  Json::Value image_targets = Utils::parseJSONFile(temp_dir.Path() / "repo/image/targets.json");
  EXPECT_EQ(image_targets["signed"]["targets"].size(), 1);
  Json::Value director_targets = Utils::parseJSONFile(temp_dir.Path() / "repo/director/targets.json");
  EXPECT_EQ(director_targets["signed"]["targets"].size(), 0);
}

TEST(aktualizr_repo, copy_image) {
  TemporaryDirectory temp_dir;
  UptaneRepo repo(temp_dir.Path(), "", "");
  repo.generateRepo(key_type);
  repo.addImage(temp_dir.Path() / "repo/director/manifest");
  repo.addTarget("manifest", "test-hw", "test-serial");
  repo.signTargets();
  Json::Value image_targets = Utils::parseJSONFile(temp_dir.Path() / "repo/image/targets.json");
  EXPECT_EQ(image_targets["signed"]["targets"].size(), 1);
  Json::Value director_targets = Utils::parseJSONFile(temp_dir.Path() / "repo/director/targets.json");
  EXPECT_EQ(director_targets["signed"]["targets"].size(), 1);
}

TEST(aktualizr_repo, sign) {
  TemporaryDirectory temp_dir;
  std::ostringstream keytype_stream;
  keytype_stream << key_type;
  std::string cmd = generate_repo_exec + " generate " + temp_dir.Path().string() + " --keytype " + keytype_stream.str();
  std::string output;
  int retval = Utils::shell(cmd, &output);
  if (retval) {
    FAIL() << "'" << cmd << "' exited with error code\n";
  }
  cmd = generate_repo_exec + " sign " + temp_dir.Path().string();
  cmd += " --repotype director --keyname snapshot";
  std::string sign_cmd =
      "echo \"{\\\"_type\\\":\\\"Snapshot\\\",\\\"expires\\\":\\\"2021-07-04T16:33:27Z\\\"}\" | " + cmd;
  output.clear();
  retval = Utils::shell(sign_cmd, &output);
  if (retval) {
    FAIL() << "'" << sign_cmd << "' exited with error code\n";
  }
  auto json = Utils::parseJSON(output);
  Uptane::Root root(Uptane::RepositoryType::Director(),
                    Utils::parseJSONFile(temp_dir.Path() / "repo/director/root.json"));
  EXPECT_NO_THROW(root.UnpackSignedObject(Uptane::RepositoryType::Director(), json));
}

#ifndef __NO_MAIN__
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  logger_init();
  logger_set_threshold(boost::log::trivial::trace);
  if (argc >= 2) {
    generate_repo_exec = argv[1];
  } else {
    std::cerr << "No generate-repo executable specified\n";
    return EXIT_FAILURE;
  }

  // note: we used to run tests for all key types, which was pretty slow.
  // Now, they only run with ed25519
  return RUN_ALL_TESTS();
}
#endif
