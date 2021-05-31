#include <iostream>
#include "ostree.h"

using std::cout;

void usage(char* arg0) { cout << "usage: " << arg0 << " /path/to/repo <checksum>\n"; }

int main(int argc, char** argv) {
  if (argc != 3) {
    usage(argv[0]);
    return 1;
  }

  const char* repo_path = argv[1];
  const char* sha256 = argv[2];

  GFile* repo_path_file = g_file_new_for_path(repo_path);
  OstreeRepo* repo = ostree_repo_new(repo_path_file);

  GError* err = nullptr;
  auto ok = ostree_repo_open(repo, nullptr, &err);
  if (ok == FALSE) {
    cout << "ostree_repo_open failed\n";
    if (err != nullptr) {
      cout << "err:" << err->message << "\n";
    }
    return 1;
  }

  OstreeObjectType type = OSTREE_OBJECT_TYPE_FILE;

  ok = ostree_repo_fsck_object(repo, type, sha256, nullptr, &err);

  if (ok == FALSE) {
    cout << "Object is corrupt\n";
    if (err != nullptr) {
      cout << "err:" << err->message << "\n";
    }
    return 1;
  }

  cout << "Object is OK\n";

  return 0;
}