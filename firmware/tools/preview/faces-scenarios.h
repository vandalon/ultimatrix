// What each face is shown doing in its preview (included by faces.cpp)
#if defined(FACE_PACMAN)
static void scenarios() {
#if defined(PREVIEW_PACMAN_PALETTES)
  Settings *s = Settings::getInstance();
  for (int choice = 0; choice < pacmancolors::WALL_COUNT; choice++) {
    s->color = choice;
    randomSeed(7);
    show("border colour", 500, at(2026, 10, 14, 9, 41, 10));
  }
  s->color = 3; s->dotColor = 9;
  s->ghostColors[0] = 7; s->ghostColors[1] = 1; s->ghostColors[2] = 4; s->ghostColors[3] = 10;
  randomSeed(7);
  show("custom ghosts and dots", 500, at(2026, 10, 14, 9, 41, 10));
  s->color = s->dotColor = 0;
  for (int i = 0; i < 4; i++) s->ghostColors[i] = 0;
  for (int choice = 0; choice < pacmancolors::COLOR_COUNT; choice++) {
    s->pacmanClockColor = choice;
    randomSeed(7);
    show("clock colour", 500, at(2026, 10, 14, 9, 41, 10));
  }
#else
  show("a new maze, ghosts released, a minute turns", 40000, at(2026, 10, 14, 9, 41, 10));
  show("the hour", 15000, at(2026, 10, 14, 9, 59, 40), false);
  show("later in the game", 45000, at(2026, 10, 14, 10, 3, 0), false);
#endif
}
#endif
#if defined(FACE_MARIO) || defined(FACE_LUIGI)
static void scenarios() {
  Settings *s = Settings::getInstance();
  s->numberFont = 0;
  show("overground: strolling and a minute block hit", 12000, at(2026, 10, 14, 14, 40, 54));
  s->numberFont = 1;
  show("hour change and world rotation", 12000, at(2026, 10, 14, 14, 59, 50), false);
  s->numberFont = 10;
  show("demo: all nine worlds and the finale", 90000, at(2026, 10, 14, 15, 1, 0), false);
  s->numberFont = 0;
  night = true;
  show("overground after sunset", 12000, at(2026, 10, 14, 23, 20, 54));
}
#endif
#if defined(FACE_WORD)
static void scenarios() {
  Settings *s = Settings::getInstance();
#if defined(PREVIEW_WORD_ANIMATIONS)
  const int choices[] = {2, 1, 3, 4, 5, 0};
  for (int choice : choices) {
    s->numberFont = 1; s->timeStyle = choice;
    show("word animation choice", 4000, at(2026, 10, 14, 7, 49, 58));
  }
#else
  for (int language = 0; language < 16; language++) {
    s->numberFont = language;
    show("language", 1200, at(2026, 10, 14, 7, 45, 5));
  }
  s->numberFont = 1;
  const int animations[] = {1, 3, 4, 5};
  for (int i = 0; i < 4; i++) {
    s->timeStyle = animations[i];
    show("word transition", 4000, at(2026, 10, 14, 7 + (i >= 3), (49 + i * 5) % 60, 58));
  }
  s->timeStyle = 2;
  for (int language = 0; language < 16; language++) {
    s->numberFont = language;
    show("language", 1200, at(2026, 10, 14, 12, 0, 5));
  }
  for (int language = 0; language < 16; language++) {
    s->numberFont = language;
    show("language", 1200, at(2026, 10, 14, 23, 59, 5));
  }
#endif
}
#endif
