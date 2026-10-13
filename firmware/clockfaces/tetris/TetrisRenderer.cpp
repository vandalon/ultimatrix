// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#include "TetrisRenderer.h"
#include <TetrisDigits.h>
#include <TetrisLayout.h>
#include <TetrisShapes.h>

namespace tetrisview {

using namespace tetris;

// Red, green, blue, white, yellow, cyan, magenta, orange, black
static const uint16_t kPalette[9] = {0xF800, 0x07E0, 0x325F, 0xFFFF, 0xFFE0, 0x07FF, 0xF81F, 0xFB00, 0x0000};

static void drawPiece(MatrixPanel_I2S_DMA &display, const Piece &piece, uint8_t rotation, int x, int y) {
  const Shape &shape = kShapes[piece.shape][rotation];
  for (uint8_t i = 0; i < shape.count; i++)
    display.fillRect(x + shape.cells[i].dx * kScale, y - shape.cells[i].dy * kScale, kScale, kScale, kPalette[piece.color]);
}

// A line of four digits: the piece in flight, then the ones that have landed
static void drawLine(MatrixPanel_I2S_DMA &display, const Slot (&slots)[4], int x, int bottom) {
  const int top = bottom - kDropRows * kScale;
  for (int i = 0; i < 4; i++) {
    const Slot &slot = slots[i];
    if (slot.digit < 0) continue;
    const Digit &digit = kDigits[slot.digit];
    const int left = x + slotOffset(i);
    if (slot.piece < digit.count) {
      const Piece &piece = digit.pieces[slot.piece];
      drawPiece(display, piece, rotationAt(piece, slot.fall), left + piece.x * kScale, top + slot.fall * kScale - kScale);
    }
    for (int p = 0; p < slot.piece && p < digit.count; p++) {
      const Piece &piece = digit.pieces[p];
      drawPiece(display, piece, piece.rotations, left + piece.x * kScale, top + piece.stop * kScale - kScale);
    }
  }
}

void draw(MatrixPanel_I2S_DMA &display, const Frame &frame) {
  display.fillScreen(0);
  if (frame.phase == DateFalling) {
    drawLine(display, frame.date, frame.dateX, kDateBottom);
    return;
  }
  drawLine(display, frame.date, frame.dateX, kDateBottom);
  display.fillRect(frame.separatorX, kSeparatorY, kSeparatorWidth, kSeparatorHeight, 0xFFFF);
  drawLine(display, frame.time, frame.timeX, kTimeBottom);
  if (frame.phase == Idle && frame.colon) {
    display.fillRect(frame.colonX, kColonTop, 2 * kScale, 2 * kScale, 0xFFFF);
    display.fillRect(frame.colonX, kColonBottom, 2 * kScale, 2 * kScale, 0xFFFF);
  }
}

}  // namespace tetrisview
