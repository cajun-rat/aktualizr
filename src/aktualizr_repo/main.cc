#include <boost/program_options.hpp>
#include <iostream>
#include <istream>
#include <iterator>
#include <ostream>
#include <string>

#include "logging/logging.h"
#include "uptane_repo.h"

namespace po = boost::program_options;

int main(int argc, char **argv) {
  po::options_description desc("aktualizr-repo command line options");
  // clang-format off
  desc.add_options()
    ("help,h", "print usage")
    ("command", po::value<std::string>(), "generate|sign|image|addtarget|signtargets")
    ("path", po::value<boost::filesystem::path>(), "path to the repository")
    ("filename", po::value<std::string>(), "path to the image")
    ("hwid", po::value<std::string>(), "target hardware identifier")
    ("serial", po::value<std::string>(), "target ECU serial")
    ("expires", po::value<std::string>(), "expiration time")
    ("keyname", po::value<std::string>(), "name of key's role")
    ("repotype", po::value<std::string>(), "director|image")
    ("correlationid", po::value<std::string>()->default_value(""), "correlation id")
    ("keytype", po::value<std::string>()->default_value("RSA2048"), "UPTANE key type");
  // clang-format on

  po::positional_options_description positionalOptions;
  positionalOptions.add("command", 1);
  positionalOptions.add("path", 1);
  positionalOptions.add("filename", 1);

  try {
    logger_init();
    logger_set_threshold(boost::log::trivial::info);
    po::variables_map vm;
    po::basic_parsed_options<char> parsed_options =
        po::command_line_parser(argc, argv).options(desc).positional(positionalOptions).run();
    po::store(parsed_options, vm);
    po::notify(vm);
    if (vm.count("help") != 0) {
      std::cout << desc << std::endl;
      exit(EXIT_SUCCESS);
    }

    if (vm.count("command") != 0 && vm.count("path") != 0) {
      std::string expiration_time;
      if (vm.count("expires") != 0) {
        expiration_time = vm["expires"].as<std::string>();
      }
      std::string correlation_id;
      if (vm.count("correlationid") != 0) {
        correlation_id = vm["correlationid"].as<std::string>();
      }
      UptaneRepo repo(vm["path"].as<boost::filesystem::path>(), expiration_time, correlation_id);
      std::string command = vm["command"].as<std::string>();
      if (command == "generate") {
        std::string key_type_arg = vm["keytype"].as<std::string>();
        std::istringstream key_type_str{std::string("\"") + key_type_arg + "\""};
        KeyType key_type;
        key_type_str >> key_type;
        repo.generateRepo(key_type);
      } else if (command == "image") {
        repo.addImage(vm["filename"].as<std::string>());
      } else if (command == "addtarget") {
        repo.addTarget(vm["filename"].as<std::string>(), vm["hwid"].as<std::string>(), vm["serial"].as<std::string>());
      } else if (command == "signtargets") {
        repo.signTargets();
      } else if (command == "sign") {
        if (vm.count("repotype") == 0 || vm.count("keyname") == 0) {
          std::cerr << "--repotype or --keyname is missing\n";
          exit(EXIT_FAILURE);
        }
        std::cin >> std::noskipws;
        std::istream_iterator<char> it(std::cin);
        std::istream_iterator<char> end;
        std::string text_to_sign(it, end);

        Repo base_repo(Uptane::RepositoryType(vm["repotype"].as<std::string>()),
                       vm["path"].as<boost::filesystem::path>(), expiration_time, correlation_id);

        auto json_to_sign = Utils::parseJSON(text_to_sign);
        if (json_to_sign == Json::nullValue) {
          std::cerr << "Text to sign must be valid json\n";
          exit(EXIT_FAILURE);
        }

        auto json_signed = base_repo.signTuf(Uptane::Role(vm["keyname"].as<std::string>()), json_to_sign);
        std::cout << Utils::jsonToCanonicalStr(json_signed);
      } else {
        std::cout << desc << std::endl;
        exit(EXIT_FAILURE);
      }
    } else {
      std::cout << desc << std::endl;
      exit(EXIT_FAILURE);
    }

    exit(EXIT_SUCCESS);

  } catch (const po::error &o) {
    std::cout << o.what() << std::endl;
    std::cout << desc;
    return EXIT_FAILURE;
  }
}
// vim: set tabstop=2 shiftwidth=2 expandtab:
