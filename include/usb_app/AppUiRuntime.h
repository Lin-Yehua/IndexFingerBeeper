#pragma once

#include "AppUi.h"

class UiManager
{
public:
  void setDisplaySink(AppUiDisplaySink *display);
  void setActiveFeature(uint16_t featureId);
  uint16_t currentRoute() const;

  void showPrompt(const char *text);
  void showMenuItem(const char *text, uint8_t index, uint8_t count);
  void showImage(const uint16_t *pixels, uint16_t width, uint16_t height, int16_t centerX, int16_t centerY);

private:
  AppStore _store;
  Screen *_current = nullptr;
  AppUiDisplaySink *_display = nullptr;

  void show(Screen *screen);
};

UiManager &appUi();
