#pragma once

// returns 1 if a game should be started (already scheduled), 0 to just quit
int ui_run(void);
void ui_free(void);

void ui_message(const char *title, const char *text);
int ui_confirm(const char *title, const char *text);
