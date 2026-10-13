#pragma once
#include <stddef.h>
class JsonDocument {};
class DynamicJsonDocument : public JsonDocument {
 public:
  DynamicJsonDocument(size_t) {}
};
class JsonObjectConst {};
