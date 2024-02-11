#include <crypto/string_bio.h>
#include <openssl/bio.h>
#include <stdexcept>

StringBIO::StringBIO() : bio_{BIO_new(BIO_s_mem())} {
  if (bio_ == nullptr) {
    throw std::runtime_error("BIO_new failed");
  }
}

StringBIO::~StringBIO() { BIO_vfree(bio_); }

std::string StringBIO::str() const {
  char *buf = nullptr;
  auto len = BIO_get_mem_data(bio_, &buf);  // NOLINT(cppcoreguidelines-pro-type-cstyle-cast)
  if (buf == nullptr) {
    throw std::runtime_error("BIO_get_mem_data failed");
  }
  return {buf, static_cast<size_t>(len)};
}

ConstStringBIO::ConstStringBIO(const std::string &str)
    : bio_{BIO_new_mem_buf(str.c_str(), static_cast<int>(str.size()))} {
  if (bio_ == nullptr) {
    throw std::runtime_error("BIO_new failed");
  }
}

ConstStringBIO::~ConstStringBIO() { BIO_vfree(bio_); }
