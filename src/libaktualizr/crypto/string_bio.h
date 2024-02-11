#include <openssl/bio.h>
#include <string>

/**
 * RAII BIO* wrapper for openssl with easy conversion to std::string
 */
class StringBIO {
 public:
  /** A writeable BIO backed by heap */
  StringBIO();
  /** A read-only BIO containing str */
  explicit StringBIO(const std::string& str);
  ~StringBIO();
  // Non-copyable
  StringBIO(const StringBIO&) = delete;
  StringBIO& operator=(const StringBIO&) = delete;
  // Non-moveable, possible but no need for an implementation currently
  StringBIO(StringBIO&&) = delete;
  StringBIO& operator=(StringBIO&&) = delete;

  [[nodiscard]] BIO* bio() { return bio_; }
  /** Return the contents of this BIO as a std::string */
  [[nodiscard]] std::string str() const;

 private:
  BIO* bio_;
};

/**
 * Read a std::string as a BIO object
 */
class ConstStringBIO {
 public:
  /**
   * Read the contents of str as a BIO object.
   * Danger! Takes a reference to str, which must be kept alive while bio() is
   * in use and not modified. */
  explicit ConstStringBIO(const std::string& str);
  ~ConstStringBIO();
  ConstStringBIO(const ConstStringBIO&) = delete;
  ConstStringBIO(ConstStringBIO&&) = delete;
  ConstStringBIO& operator=(const ConstStringBIO&) = delete;
  ConstStringBIO& operator=(ConstStringBIO&&) = delete;

  [[nodiscard]] BIO* bio() { return bio_; }

 private:
  BIO* bio_;
};
