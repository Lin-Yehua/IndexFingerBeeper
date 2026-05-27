#pragma once

#include <Arduino.h>

struct AppLoopImageEvent
{
  uint16_t width = 0;
  uint16_t height = 0;
  int16_t centerX = 160;
  int16_t centerY = 155;
};

class AppLoopInbox
{
public:
  void pushKey(uint8_t key);
  bool popKey(uint8_t &outKey);

  void pushWebText(const char *text);
  bool popWebText(String &outText);
  bool hasWebText() const;

  void setImmediateText(const char *text);
  bool popImmediateText(String &outText);

  void setImage(const AppLoopImageEvent &image);
  bool popImage(AppLoopImageEvent &outImage);

  void setScheduleDue();
  bool consumeScheduleDue();

private:
  static constexpr size_t kKeyDepth = 4;
  static constexpr size_t kTextDepth = 8;

  uint8_t _keyQueue[kKeyDepth] = {0};
  size_t _keyHead = 0;
  size_t _keyCount = 0;

  String _webTextQueue[kTextDepth];
  size_t _webTextHead = 0;
  size_t _webTextCount = 0;

  bool _hasImmediateText = false;
  String _immediateText;

  bool _hasImage = false;
  AppLoopImageEvent _image;

  bool _scheduleDue = false;
};

AppLoopInbox &appLoopInbox();

void inboxPushKey(uint8_t key);
bool inboxPopKey(uint8_t &outKey);
void inboxPushWebText(const char *text);
bool inboxPopWebText(String &outText);
bool inboxHasWebText();
void inboxSetImmediateText(const char *text);
bool inboxPopImmediateText(String &outText);
void inboxSetImage(const AppLoopImageEvent &image);
bool inboxPopImage(AppLoopImageEvent &outImage);
void inboxSetScheduleDue();
bool inboxConsumeScheduleDue();
uint8_t readLoopKey(bool &syntheticKeyPress);
