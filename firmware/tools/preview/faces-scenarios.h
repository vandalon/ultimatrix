// What each face is shown doing in its preview (included by faces.cpp)
#if defined(FACE_PACMAN)
static void scenarios() {
  show("a new maze, ghosts released, a minute turns", 40000, at(2026, 10, 12, 9, 41, 10));
  show("the hour", 15000, at(2026, 10, 12, 9, 59, 40), false);
  show("later in the game", 45000, at(2026, 10, 12, 10, 3, 0), false);
}
#endif
#if defined(FACE_MARIO)
static void scenarios() {
  show("an ordinary minute: strolling, then the 14:41 block hit", 62000, at(2026, 10, 12, 14, 40, 0));
  show("hour change", 12000, at(2026, 10, 12, 14, 59, 50), false);
  night = true;
  show("underground at night", 20000, at(2026, 10, 12, 23, 20, 40));
}
#endif
#if defined(FACE_LUIGI)
static void scenarios() {
  show("an ordinary minute: strolling, then the 14:41 block hit", 62000, at(2026, 10, 12, 14, 40, 0));
  show("hour change", 12000, at(2026, 10, 12, 14, 59, 50), false);
  night = true;
  show("night", 20000, at(2026, 10, 12, 23, 20, 40));
}
#endif
#if defined(FACE_WORD)
static void scenarios() {
  Settings *s = Settings::getInstance();
  // The same quarter to eight in every language, then the minute change
  for (int language = 0; language < 16; language++) {
    s->numberFont = language;
    show("language", 1200, at(2026, 10, 12, 7, 45, 5));
  }
  s->numberFont = 1;
  show("a minute changes", 4000, at(2026, 10, 12, 7, 46, 58), false);
  for (int language = 0; language < 16; language++) {
    s->numberFont = language;
    show("language", 1200, at(2026, 10, 12, 12, 0, 5));
  }
  for (int language = 0; language < 16; language++) {
    s->numberFont = language;
    show("language", 1200, at(2026, 10, 12, 23, 59, 5));
  }
}
#endif
