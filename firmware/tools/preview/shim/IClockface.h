#pragma once
#include "ClockTime.h"
class IClockface { public: virtual ~IClockface() {} virtual int ditherPolicy() const { return 1; } };
