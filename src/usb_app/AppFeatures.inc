/* App feature implementations and menu entries. Included by UsbAppMode.cpp. */
static void publishFeatureSelection(uint16_t featureId, uint16_t commandId = APP_FEATURE_COMMAND_NONE)
{
  AppMessage msg;
  msg.type = AppMsgType::FeatureSelected;
  msg.priority = AppMsgPriority::Normal;
  msg.source = AppMsgSource::Ui;
  msg.targetFeature = featureId;
  msg.data.commandId = commandId;
  (void)gAppMessageBus.publish(msg);
}

static void publishUiText(const char *text, AppMsgPriority priority = AppMsgPriority::Low)
{
  AppMessage msg;
  msg.type = AppMsgType::PlayText;
  msg.priority = priority;
  msg.source = AppMsgSource::Feature;
  appMessageCopyText(msg, text);
  (void)gAppMessageBus.publish(msg);
}

class TextPushFeature final : public IAppFeature
{
public:
  uint16_t id() const override { return APP_FEATURE_TEXT_PUSH; }
  const char *title() const override { return "Text Push"; }

  bool begin(AppContext &ctx) override
  {
    (void)ctx;
    return true;
  }

  void enter(AppContext &ctx) override
  {
    (void)ctx;
    appUi().setActiveFeature(id());
    Serial.println("[FEATURE] enter Text Push");
  }

  void exit(AppContext &ctx) override
  {
    (void)ctx;
  }

  void tick(AppContext &ctx) override
  {
    bool syntheticKeyPress = ctx.syntheticKeyPress;
    if (!runCurrentTextPushMode(syntheticKeyPress))
    {
      switchAppMode(APP_MODE_AP_STA);
    }
  }

  bool handleMessage(const AppMessage &msg, AppContext &ctx) override
  {
    (void)ctx;
    if (msg.type == AppMsgType::FeatureSelected && msg.data.commandId == APP_TEXT_PUSH_COMMAND_NEXT_MODE)
    {
      switchAppMode(nextTextPushMode());
      return true;
    }
    return false;
  }
};

class CoinFlipFeature final : public IAppFeature
{
public:
  uint16_t id() const override { return APP_FEATURE_COIN_FLIP; }
  const char *title() const override { return "Coin Flip"; }

  bool begin(AppContext &ctx) override
  {
    (void)ctx;
    return true;
  }

  void enter(AppContext &ctx) override
  {
    (void)ctx;
    appUi().setActiveFeature(id());
    publishUiText("Coin Flip | short: flip | long: menu");
  }

  void exit(AppContext &ctx) override
  {
    (void)ctx;
  }

  void tick(AppContext &ctx) override
  {
    bool syntheticKeyPress = ctx.syntheticKeyPress;
    const uint8_t key = readLoopKey(syntheticKeyPress);
    if (wakeByKey(key))
      return;
    if (key == 3)
    {
      requestMenuFeature();
      return;
    }
    if (key == 2)
    {
      const bool heads = (esp_random() & 1U) != 0;
      publishUiText(heads ? "Heads" : "Tails");
    }
  }

  bool handleMessage(const AppMessage &msg, AppContext &ctx) override
  {
    (void)msg;
    (void)ctx;
    return false;
  }
};

class MusicFeature final : public IAppFeature
{
public:
  uint16_t id() const override { return APP_FEATURE_MUSIC; }
  const char *title() const override { return "Music"; }

  bool begin(AppContext &ctx) override
  {
    (void)ctx;
    return true;
  }

  void enter(AppContext &ctx) override
  {
    (void)ctx;
    appUi().setActiveFeature(id());
    publishUiText("Music | short: status | long: menu");
  }

  void exit(AppContext &ctx) override
  {
    (void)ctx;
  }

  void tick(AppContext &ctx) override
  {
    bool syntheticKeyPress = ctx.syntheticKeyPress;
    const uint8_t key = readLoopKey(syntheticKeyPress);
    if (wakeByKey(key))
      return;
    if (key == 3)
    {
      requestMenuFeature();
      return;
    }
    if (key == 2)
    {
      publishUiText("Music module reserved; playlist is the next step");
    }
  }

  bool canInterrupt() const override { return false; }

  bool handleMessage(const AppMessage &msg, AppContext &ctx) override
  {
    (void)msg;
    (void)ctx;
    return false;
  }
};

struct AppMenuEntry
{
  const char *title;
  uint16_t featureId;
  uint16_t commandId;
};

static const AppMenuEntry kAppMenuEntries[] = {
    {"Menu: Text Push", APP_FEATURE_TEXT_PUSH, APP_FEATURE_COMMAND_NONE},
    {"Menu: Next Text Mode", APP_FEATURE_TEXT_PUSH, APP_TEXT_PUSH_COMMAND_NEXT_MODE},
    {"Menu: Coin Flip", APP_FEATURE_COIN_FLIP, APP_FEATURE_COMMAND_NONE},
    {"Menu: Music", APP_FEATURE_MUSIC, APP_FEATURE_COMMAND_NONE},
};

static constexpr uint8_t kAppMenuEntryCount = sizeof(kAppMenuEntries) / sizeof(kAppMenuEntries[0]);

class MenuFeature final : public IAppFeature
{
public:
  uint16_t id() const override { return APP_FEATURE_MENU; }
  const char *title() const override { return "Menu"; }

  bool begin(AppContext &ctx) override
  {
    (void)ctx;
    return true;
  }

  void enter(AppContext &ctx) override
  {
    (void)ctx;
    appUi().setActiveFeature(id());
    _selected = 0;
    showCurrentItem();
  }

  void exit(AppContext &ctx) override
  {
    (void)ctx;
  }

  void tick(AppContext &ctx) override
  {
    bool syntheticKeyPress = ctx.syntheticKeyPress;
    const uint8_t key = readLoopKey(syntheticKeyPress);
    if (wakeByKey(key))
      return;
    if (key == 2)
    {
      _selected = (_selected + 1) % kAppMenuEntryCount;
      showCurrentItem();
      return;
    }
    if (key == 3)
    {
      selectCurrentItem();
    }
  }

  bool handleMessage(const AppMessage &msg, AppContext &ctx) override
  {
    (void)msg;
    (void)ctx;
    return false;
  }

private:
  uint8_t _selected = 0;
  const char *itemTitle() const
  {
    return (_selected < kAppMenuEntryCount) ? kAppMenuEntries[_selected].title : "Menu";
  }

  void showCurrentItem() const
  {
    appUi().showMenuItem(itemTitle(), _selected, kAppMenuEntryCount);
  }

  void selectCurrentItem();
};

static TextPushFeature gTextPushFeature;
static MenuFeature gMenuFeature;
static CoinFlipFeature gCoinFlipFeature;
static MusicFeature gMusicFeature;

