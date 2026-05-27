#include "usb_app/AppUiRuntime.h"

#include "AppMessageBus.h"

namespace
{
class PromptScreen final : public Screen
{
public:
  uint16_t route() const override { return APP_ROUTE_PROMPT; }

  void setText(const char *text)
  {
    _text = text ? text : "";
  }

  void render(const AppStore &store, UiContext &ctx) override
  {
    (void)store;
    if (ctx.display)
    {
      ctx.display->playText(_text.c_str());
    }
  }

private:
  String _text;
};

class MenuScreen final : public Screen
{
public:
  uint16_t route() const override { return APP_ROUTE_MENU; }

  void setItem(const char *text, uint8_t index, uint8_t count)
  {
    _text = text ? text : "";
    _index = index;
    _count = count;
  }

  void render(const AppStore &store, UiContext &ctx) override
  {
    (void)store;
    if (!ctx.display)
      return;

    String line = _text;
    line += " ";
    line += String(static_cast<unsigned int>(_index + 1));
    line += "/";
    line += String(static_cast<unsigned int>(_count));
    ctx.display->playText(line.c_str());
  }

private:
  String _text;
  uint8_t _index = 0;
  uint8_t _count = 0;
};

class ImageScreen final : public Screen
{
public:
  uint16_t route() const override { return APP_ROUTE_IMAGE; }

  void setImage(const uint16_t *pixels, uint16_t width, uint16_t height, int16_t centerX, int16_t centerY)
  {
    _pixels = pixels;
    _width = width;
    _height = height;
    _centerX = centerX;
    _centerY = centerY;
  }

  void render(const AppStore &store, UiContext &ctx) override
  {
    (void)store;
    if (ctx.display)
    {
      ctx.display->showImage(_pixels, _width, _height, _centerX, _centerY);
    }
  }

private:
  const uint16_t *_pixels = nullptr;
  uint16_t _width = 0;
  uint16_t _height = 0;
  int16_t _centerX = 160;
  int16_t _centerY = 155;
};

PromptScreen gPromptScreen;
MenuScreen gMenuScreen;
ImageScreen gImageScreen;
UiManager gUiManagerInstance;
}

UiManager &appUi()
{
  return gUiManagerInstance;
}

void UiManager::setDisplaySink(AppUiDisplaySink *display)
{
  _display = display;
}

void UiManager::setActiveFeature(uint16_t featureId)
{
  _store.activeFeature = featureId;
}

uint16_t UiManager::currentRoute() const
{
  return _store.currentRoute;
}

void UiManager::showPrompt(const char *text)
{
  gPromptScreen.setText(text);
  show(&gPromptScreen);
}

void UiManager::showMenuItem(const char *text, uint8_t index, uint8_t count)
{
  gMenuScreen.setItem(text, index, count);
  show(&gMenuScreen);
}

void UiManager::showImage(const uint16_t *pixels, uint16_t width, uint16_t height, int16_t centerX, int16_t centerY)
{
  gImageScreen.setImage(pixels, width, height, centerX, centerY);
  show(&gImageScreen);
}

void UiManager::show(Screen *screen)
{
  if (!screen)
    return;
  if (_current && _current != screen)
  {
    _current->exit();
  }
  _current = screen;
  _store.currentRoute = screen->route();
  UiContext ctx;
  ctx.bus = &gAppMessageBus;
  ctx.display = _display;
  screen->enter(_store);
  screen->render(_store, ctx);
}
