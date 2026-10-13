#pragma once
struct Locator {
  template <typename T> static void provide(T *) {}
};
