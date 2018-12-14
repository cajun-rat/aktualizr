#include "logging.h"
#include "utilities/config_utils.h"

#if (defined(ANDROID) && defined(__clang__))
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wshorten-64-to-32"
#include <android/log.h>
#include <boost/log/utility/setup/console.hpp>
#pragma GCC diagnostic pop
#else
#include <boost/log/utility/setup/console.hpp>
#endif

namespace logging = boost::log;
using boost::log::trivial::severity_level;

#if defined(ANDROID)
class android_log_sink : public logging::sinks::basic_sink_backend<logging::sinks::synchronized_feeding> {
 public:
  explicit android_log_sink() {}

  void consume(logging::record_view const& rec) {
    const auto& rec_message_attr = rec[logging::aux::default_attribute_names::message()];
    int log_priority = android_LogPriority::ANDROID_LOG_VERBOSE +
                       rec[logging::aux::default_attribute_names::severity()].extract_or_default(0);
    __android_log_write(log_priority, "aktualizr", rec_message_attr.extract_or_default(std::string("N/A")).c_str());
  }
};
#endif  // defined(ANDROID)

static severity_level gLoggingThreshold;

void LoggerConfig::updateFromPropertyTree(const boost::property_tree::ptree& pt) {
  CopyFromConfig(loglevel, "loglevel", pt);
}

void LoggerConfig::writeToStream(std::ostream& out_stream) const { writeOption(out_stream, loglevel, "loglevel"); }

int64_t get_curlopt_verbose() { return gLoggingThreshold <= boost::log::trivial::trace ? 1L : 0L; }

void logger_init() {
  gLoggingThreshold = boost::log::trivial::info;

#if defined(ANDROID)
  typedef logging::sinks::synchronous_sink<android_log_sink> sink_t;
  logging::core::get()->add_sink(boost::shared_ptr<sink_t>(new sink_t()));
#else
  logging::add_console_log(std::cout, boost::log::keywords::format = "%Message%",
                           boost::log::keywords::auto_flush = true);
#endif
  boost::log::core::get()->set_filter(boost::log::trivial::severity >= gLoggingThreshold);
}

void logger_set_threshold(const severity_level threshold) {
  gLoggingThreshold = threshold;
  boost::log::core::get()->set_filter(boost::log::trivial::severity >= gLoggingThreshold);
}

void logger_set_threshold(const LoggerConfig& lconfig) {
  int loglevel = lconfig.loglevel;
  if (loglevel < boost::log::trivial::trace) {
    LOG_WARNING << "Invalid log level: " << loglevel;
    loglevel = boost::log::trivial::trace;
  }
  if (boost::log::trivial::fatal < loglevel) {
    LOG_WARNING << "Invalid log level: " << loglevel;
    loglevel = boost::log::trivial::fatal;
  }
  logger_set_threshold(static_cast<boost::log::trivial::severity_level>(loglevel));
}

int loggerGetSeverity() { return static_cast<int>(gLoggingThreshold); }

// vim: set tabstop=2 shiftwidth=2 expandtab:
