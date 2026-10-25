// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <Arduino.h>
#include <WebServer.h>

// The game controller's WebSocket (GET /play, Pong and Invaders): the page keeps one connection open and sends each
// steer code as a text frame ("230", "4", "-1"), instead of one HTTP request per move. With an admin PIN set, the first
// frame must be "t" followed by the token. One client at a time; it runs in the main loop like the web server.
namespace playsocket {
void attach(WebServer &server, void (*onSteer)(int dir));
void service();
void close();
}  // namespace playsocket
