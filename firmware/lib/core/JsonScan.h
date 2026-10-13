// Project-authored portions: SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright 2026 Joris Vandalon (https://github.com/vandalon/clockwizer)
// See LICENSE_SCOPE.md; upstream and third-party portions retain their licences.
#pragma once

#include <stddef.h>

// A JSON reader that builds nothing: it walks the text a character at a time, keeps the keys (or array indexes)
// leading to the current value and hands every string, number, true, false and null to a function as it passes.
// {"a":{"b":[1,"x"]}} gives a,b,0 = 1 and a,b,1 = "x". It needs a few hundred bytes whatever the size of the text,
// where a tree takes ten times what the text holds. Keys and values longer than the buffers are cut, which is fine
// for fields that are short; values that are objects or arrays only show up through what is inside them.
class JsonScan {
  public:
    static const int MAX_DEPTH = 10;  // deeper values are read over and not reported
    static const int KEY = 24;  // includes F1's IntervalToPositionAhead and its terminator
    static const int VALUE = 40;  // long enough for a UUID

    // The next character of the text, -1 at its end
    typedef int (*Next)(void *source);
    // One value: path[0..n) are the keys leading to it; text is true for a string, false for a number or literal
    typedef void (*Leaf)(void *sink, const char (*path)[KEY], int n, const char *value, bool text);

    // Reads one value (usually the whole text). False when the text ended early or isn't JSON; what came before
    // that point has been reported.
    static bool read(Next next, void *source, Leaf leaf, void *sink);
};

// Does the path match a pattern like "events.0.competitions.*.id"? * stands for any one key or index.
bool jsonPathIs(const char (*path)[JsonScan::KEY], int n, const char *pattern);
