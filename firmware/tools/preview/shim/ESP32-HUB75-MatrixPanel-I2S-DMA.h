// A 64x64 panel that is just a canvas; flipDMABuffer() remembers the picture
#pragma once
#include <Adafruit_GFX.h>
#include <vector>

extern void (*onFlip)(class MatrixPanel_I2S_DMA *);  // the preview hooks in here

class MatrixPanel_I2S_DMA : public GFXcanvas16 {
 public:
  MatrixPanel_I2S_DMA() : GFXcanvas16(64, 64) {}
  int flips = 0;
  void flipDMABuffer() {
    flips++;
    if (onFlip) onFlip(this);
  }
};
