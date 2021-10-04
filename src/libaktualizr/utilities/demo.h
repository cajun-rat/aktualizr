#ifndef AKTUALIZR_DEMO_H
#define AKTUALIZR_DEMO_H

class Demo {
 public:
  explicit Demo(int offset) : offset_{offset} {}

  int Add(int n) const;

 private:
  int offset_;
};

#endif  // AKTUALIZR_DEMO_H
