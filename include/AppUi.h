#pragma once

#include "AppMessageBus.h"
#include <Arduino.h>

enum AppRouteId : uint16_t
{
  APP_ROUTE_NONE = 0,
  APP_ROUTE_PROMPT = 1,
  APP_ROUTE_MENU = 2,
  APP_ROUTE_IMAGE = 3,
};

struct AppStore
{
  uint16_t activeFeature = 0;
  uint16_t currentRoute = APP_ROUTE_NONE;
};

struct UiContext
{
  AppMessageBus *bus = nullptr;
};

class Screen
{
public:
  virtual ~Screen() = default;
  virtual uint16_t route() const = 0;
  virtual void enter(const AppStore &store) { (void)store; }
  virtual void exit() {}
  virtual bool handleMessage(const AppMessage &msg, UiContext &ctx)
  {
    (void)msg;
    (void)ctx;
    return false;
  }
  virtual void render(const AppStore &store, UiContext &ctx) = 0;
};
