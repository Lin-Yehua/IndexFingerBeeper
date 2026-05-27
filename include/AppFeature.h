#pragma once

#include "AppMessageBus.h"
#include <Arduino.h>

enum AppFeatureId : uint16_t
{
  APP_FEATURE_TEXT_PUSH = 1,
  APP_FEATURE_MENU = 2,
  APP_FEATURE_COIN_FLIP = 3,
  APP_FEATURE_MUSIC = 4,
};

enum AppFeatureCommand : uint16_t
{
  APP_FEATURE_COMMAND_NONE = 0,
  APP_TEXT_PUSH_COMMAND_NEXT_MODE = 1,
};

struct AppContext
{
  AppMessageBus *bus = nullptr;
  uint32_t nowMs = 0;
  bool syntheticKeyPress = false;
};

class IAppFeature
{
public:
  virtual ~IAppFeature() = default;

  virtual uint16_t id() const = 0;
  virtual const char *title() const = 0;

  virtual bool begin(AppContext &ctx) = 0;
  virtual void enter(AppContext &ctx) = 0;
  virtual void exit(AppContext &ctx) = 0;
  virtual void tick(AppContext &ctx) = 0;
  virtual bool handleMessage(const AppMessage &msg, AppContext &ctx) = 0;

  virtual bool canSuspend() const { return true; }
  virtual bool canInterrupt() const { return true; }
};
