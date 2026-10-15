#pragma once
#include "ClockTime.h"
class IClockface {
public:
  virtual ~IClockface() = default;
  virtual void setup(ClockTime *time) = 0;
  virtual void update() = 0;
  // Forget what was drawn so the next update() paints the whole face again (after the night clock)
  virtual void invalidate() {}
  // A face that draws its own night scene (Mario's underground level) takes the night over from the red night clock;
  // it is told whether it is night before every update()
  virtual bool handlesNight() const { return false; }
  virtual void setNight(bool) {}
  // Called every frame, whether or not the face is on the panel (the night clock may be): lets a face follow its data
  virtual void poll() {}
  // A popup that must show even while the night clock would, and one that wants the panel at full brightness
  virtual bool alertOverridesNight() const { return false; }
  virtual bool alertFullBrightness() const { return false; }
};
