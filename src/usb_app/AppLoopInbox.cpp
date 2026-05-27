#include "usb_app/AppLoopInbox.h"

namespace
{
AppLoopInbox gInbox;
}

AppLoopInbox &appLoopInbox()
{
  return gInbox;
}

void AppLoopInbox::pushKey(uint8_t key)
{
  if (key == 255)
    return;

  if (_keyCount >= kKeyDepth)
  {
    _keyHead = (_keyHead + 1) % kKeyDepth;
    --_keyCount;
  }
  const size_t tail = (_keyHead + _keyCount) % kKeyDepth;
  _keyQueue[tail] = key;
  ++_keyCount;
}

bool AppLoopInbox::popKey(uint8_t &outKey)
{
  if (_keyCount == 0)
    return false;
  outKey = _keyQueue[_keyHead];
  _keyHead = (_keyHead + 1) % kKeyDepth;
  --_keyCount;
  return true;
}

void AppLoopInbox::pushWebText(const char *text)
{
  if (!text)
    return;

  if (_webTextCount >= kTextDepth)
  {
    _webTextQueue[_webTextHead] = "";
    _webTextHead = (_webTextHead + 1) % kTextDepth;
    --_webTextCount;
  }
  const size_t tail = (_webTextHead + _webTextCount) % kTextDepth;
  _webTextQueue[tail] = text;
  ++_webTextCount;
}

bool AppLoopInbox::popWebText(String &outText)
{
  if (_webTextCount == 0)
    return false;
  outText = _webTextQueue[_webTextHead];
  _webTextQueue[_webTextHead] = "";
  _webTextHead = (_webTextHead + 1) % kTextDepth;
  --_webTextCount;
  return true;
}

bool AppLoopInbox::hasWebText() const
{
  return _webTextCount > 0;
}

void AppLoopInbox::setImmediateText(const char *text)
{
  _immediateText = text ? text : "";
  _hasImmediateText = true;
}

bool AppLoopInbox::popImmediateText(String &outText)
{
  if (!_hasImmediateText)
    return false;
  outText = _immediateText;
  _immediateText = "";
  _hasImmediateText = false;
  return true;
}

void AppLoopInbox::setImage(const AppLoopImageEvent &image)
{
  _image = image;
  _hasImage = true;
}

bool AppLoopInbox::popImage(AppLoopImageEvent &outImage)
{
  if (!_hasImage)
    return false;
  outImage = _image;
  _hasImage = false;
  return true;
}

void AppLoopInbox::setScheduleDue()
{
  _scheduleDue = true;
}

bool AppLoopInbox::consumeScheduleDue()
{
  if (!_scheduleDue)
    return false;
  _scheduleDue = false;
  return true;
}

void inboxPushKey(uint8_t key)
{
  gInbox.pushKey(key);
}

bool inboxPopKey(uint8_t &outKey)
{
  return gInbox.popKey(outKey);
}

void inboxPushWebText(const char *text)
{
  gInbox.pushWebText(text);
}

bool inboxPopWebText(String &outText)
{
  return gInbox.popWebText(outText);
}

bool inboxHasWebText()
{
  return gInbox.hasWebText();
}

void inboxSetImmediateText(const char *text)
{
  gInbox.setImmediateText(text);
}

bool inboxPopImmediateText(String &outText)
{
  return gInbox.popImmediateText(outText);
}

void inboxSetImage(const AppLoopImageEvent &image)
{
  gInbox.setImage(image);
}

bool inboxPopImage(AppLoopImageEvent &outImage)
{
  return gInbox.popImage(outImage);
}

void inboxSetScheduleDue()
{
  gInbox.setScheduleDue();
}

bool inboxConsumeScheduleDue()
{
  return gInbox.consumeScheduleDue();
}

uint8_t readLoopKey(bool &syntheticKeyPress)
{
  if (syntheticKeyPress)
  {
    syntheticKeyPress = false;
    return 2;
  }

  uint8_t key = 255;
  if (gInbox.popKey(key))
  {
    return key;
  }
  return 255;
}
