# APP Message 架构引导

## 目标

把当前项目从“主循环主动轮询各功能”逐步演进成：

- 功能模块可插拔。
- 随机事件通过消息总线进入 APP 层。
- APP 层统一做模式、优先级、中断和跳转仲裁。
- UI 只响应状态快照和导航意图，避免页面之间互相直接调用。

这份文档是后续重构的方向说明，不要求一次性推倒现有代码。

## 核心原则

### 1. 模块之间不互相调用

功能模块只做两件事：

- 把外部变化转换成 `AppMessage`。
- 响应 APP 层分发给自己的消息。

例如无线网页收到一条文本，不应该直接调用显示或播放函数，而应该发布：

```cpp
AppMessage msg;
msg.type = AppMsgType::TextPushReceived;
msg.priority = AppMsgPriority::Normal;
msg.source = AppMsgSource::WirelessPortal;
copyText(msg.text, text);
bus.publish(msg);
```

### 2. APP 层是唯一仲裁者

APP 层负责决定：

- 当前处于 USB、AP+STA、STA Online、STA Only、Sleep、Update 等哪个全局模式。
- 新消息是否允许打断当前功能。
- 新消息应该进入哪个功能。
- UI 是否跳转、弹窗、覆盖显示，还是保持当前页面。

### 3. UI 不直接依赖 WirelessPortal

无线设置、网页上传、图片上传、文本推送等事件都先进入 APP 层，APP 层更新 `AppStore`，UI 根据状态快照刷新。

这样可以处理“无线设置 <-> UI”链路的随机性：事件可以乱序、重复、延迟到达，UI 不需要知道事件来源内部细节。

## 推荐分层

```text
Drivers / Services
  Key / WirelessPortal / StaCloud / Schedule / Battery / USB / Storage
        |
        v
AppMessageBus
        |
        v
AppRuntime
  - 全局模式状态机
  - 消息优先级仲裁
  - 功能路由
  - UI导航路由
  - 组件生命周期
        |
        v
Feature Modules
  TextPushFeature / CoinFlipFeature / MusicFeature / SettingsFeature
        |
        v
UIManager / AudioManager / DisplayManager
```

## AppMessage

ESP32 上建议使用固定大小消息结构，减少 `String`、动态分配和堆碎片。

```cpp
enum class AppMsgType : uint8_t
{
  KeyPressed,
  UsbPlugged,
  UsbUnplugged,

  TextPushReceived,
  ImmediateTextReceived,
  ImageReceived,

  WirelessSettingsChanged,
  WifiConnectRequested,
  WifiConnected,
  WifiDisconnected,

  ScheduleDue,
  BatteryLow,

  MenuOpenRequested,
  FeatureSelected,
  FeatureFinished,

  UiNavigate,
  UiBack,

  PlayText,
  PlayMusic,
  ShowImage,
  EnterSleep,
};

enum class AppMsgPriority : uint8_t
{
  Low,
  Normal,
  High,
  Interrupt,
  Critical,
};

enum class AppMsgSource : uint8_t
{
  Unknown,
  Key,
  WirelessPortal,
  StaCloud,
  Schedule,
  Battery,
  Usb,
  Ui,
  Feature,
};

struct AppMessage
{
  AppMsgType type = AppMsgType::KeyPressed;
  AppMsgPriority priority = AppMsgPriority::Normal;
  AppMsgSource source = AppMsgSource::Unknown;
  uint32_t timestampMs = 0;
  uint16_t correlationId = 0;
  uint16_t targetFeature = 0;

  union
  {
    uint8_t keyCode;
    uint16_t routeId;
    uint16_t imageId;
    uint16_t settingId;
    uint16_t commandId;
  } data = {};

  char text[241] = {0};
};
```

## AppMessageBus

总线初期可以很简单：一个固定长度 FreeRTOS queue 加少量 helper。

```cpp
class AppMessageBus
{
public:
  bool begin(size_t depth);
  bool publish(const AppMessage &msg);
  bool poll(AppMessage &outMsg, uint32_t timeoutMs = 0);
};
```

后续如果需要优先级，可以做两种方案：

- 多队列：`criticalQueue / interruptQueue / normalQueue`。
- 单队列 + APP 层暂存排序。

在当前项目里，多队列更直观，也更适合中断消息。

## 功能模块接口

如果要新增菜单来适配更多功能，建议设计接口。原因是“文本推送、翻硬币、放音乐”不是同一种业务，只是都可能从菜单进入、都可能占用屏幕和按键。

推荐接口：

```cpp
class IAppFeature
{
public:
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
```

示例功能：

```cpp
class TextPushFeature : public IAppFeature
{
public:
  uint16_t id() const override { return 1; }
  const char *title() const override { return "Text"; }
  bool handleMessage(const AppMessage &msg, AppContext &ctx) override;
};

class CoinFlipFeature : public IAppFeature
{
public:
  uint16_t id() const override { return 2; }
  const char *title() const override { return "Coin"; }
  bool handleMessage(const AppMessage &msg, AppContext &ctx) override;
};

class MusicFeature : public IAppFeature
{
public:
  uint16_t id() const override { return 3; }
  const char *title() const override { return "Music"; }
  bool canInterrupt() const override { return false; }
  bool handleMessage(const AppMessage &msg, AppContext &ctx) override;
};
```

功能注册：

```cpp
IAppFeature *features[] = {
    &textPushFeature,
    &coinFlipFeature,
    &musicFeature,
};
```

## 为什么菜单需要接口

当前三个模式可以视为“文本推送功能”的不同运行入口或运行模式：

- AP+STA 下的网页文本。
- STA Online 下的云端文本。
- STA Only 下的本地/省电文本。

它们不是三个平级 APP 功能，而是 `TextPushFeature` 的三个数据来源或运行模式。

如果新增：

- 翻硬币。
- 放音乐。
- 设置页。
- 图片查看。
- 日程提醒列表。

这些就应该是平行于 `TextPushFeature` 的功能。菜单只负责选择功能，不应该知道每个功能内部怎么跑。

因此需要 `IAppFeature`，否则菜单会逐渐变成新的大杂烩：

```text
Menu -> if Text do ...
Menu -> if Coin do ...
Menu -> if Music do ...
Menu -> if Settings do ...
```

有接口后，菜单只做：

```cpp
publishFeatureSelected(featureId);
```

APP 层再调用目标功能的 `enter()`。

## 功能跳转模型

建议区分两种跳转：

### 1. 功能跳转

从菜单进入某个功能：

```text
MenuFeature -> FeatureSelected(CoinFlip)
AppRuntime -> exit current feature -> enter CoinFlipFeature
```

### 2. UI 页面跳转

某个功能内部需要显示页面：

```text
CoinFlipFeature -> UiNavigate(CoinFlipScreen)
UIManager -> 切换 Screen
```

功能不直接操作具体页面对象，只发导航意图。

## UI 接口

高度复用的 UI 做基类，具体页面继承扩展。

```cpp
class Screen
{
public:
  virtual uint16_t route() const = 0;
  virtual void enter(const AppStore &store) {}
  virtual void exit() {}
  virtual bool handleMessage(const AppMessage &msg, UiContext &ctx) { return false; }
  virtual void render(const AppStore &store, UiContext &ctx) = 0;
};
```

可复用页面：

```cpp
class PromptScreen : public Screen
{
protected:
  const char *title = nullptr;
  const char *body = nullptr;
};

class ProgressScreen : public Screen
{
protected:
  uint8_t percent = 0;
};

class MenuScreen : public Screen
{
public:
  void setItems(const MenuItem *items, size_t count);
};
```

具体页面继承：

```cpp
class WirelessSettingsScreen : public MenuScreen
{
public:
  uint16_t route() const override { return ROUTE_WIRELESS_SETTINGS; }
};

class CoinFlipScreen : public PromptScreen
{
public:
  uint16_t route() const override { return ROUTE_COIN_FLIP; }
};
```

## AppStore

UI 读取统一状态快照，不直接查功能模块内部变量。

```cpp
struct WirelessState
{
  bool apEnabled = false;
  bool staEnabled = false;
  bool connected = false;
  char ssid[33] = {0};
  char ip[16] = {0};
  int rssi = 0;
  uint32_t lastChangedMs = 0;
};

struct AppStore
{
  AppMode mode;
  uint16_t activeFeature = 0;
  uint16_t currentRoute = 0;
  WirelessState wireless;
  BatteryStatus battery;
  bool hasPendingText = false;
  bool hasPendingImage = false;
};
```

无线设置随机链路建议这样走：

```text
WirelessPortal 收到网页设置
  -> publish(WirelessSettingsChanged)
AppRuntime 校验并更新 AppStore
  -> WirelessModule 应用配置
WirelessModule 发布 WifiConnected / WifiDisconnected
  -> AppRuntime 更新 AppStore
UIManager 根据当前 Screen 决定刷新、弹窗或保持
```

## 中断和优先级

建议先定义固定规则：

```text
Critical:
  USB 插入、低电关机、OTA 重启、进入深睡

Interrupt:
  日程提醒、网页立即文本、图片显示

High:
  Host 消息、STA 云端消息

Normal:
  普通网页文本、本地 CSV 文本、菜单功能选择

Low:
  UI 提示刷新、状态栏刷新
```

APP 层收到消息时，根据当前功能能力判断：

- 当前功能 `canInterrupt() == true`：允许中断。
- 当前功能 `canSuspend() == true`：保存现场，跳转到中断功能，结束后恢复。
- 当前功能不可中断：消息进入 pending queue 或直接拒绝。

## 推荐落地顺序

### 第一步：只加消息类型和总线

保留现有 `processAppLoop()`，先让它从 bus 里取消息。不要立刻拆所有功能。

### 第二步：输入源事件化

优先改：

- 按键。
- Web 普通文本。
- Web 立即文本。
- 图片上传完成。
- 日程提醒。

### 第三步：添加 Feature 接口和菜单

先实现：

- `TextPushFeature`
- `CoinFlipFeature`
- `MusicFeature`
- `MenuFeature`

当前 AP+STA、STA Online、STA Only 三个模式先保留为 APP 全局模式，不拆成三个功能。

### 第四步：UIManager + Screen

先把高复用 UI 抽出来：

- `PromptScreen`
- `MenuScreen`
- `ImageScreen`
- `TextPlayerScreen`
- `ProgressScreen`

### 第五步：状态快照化

把 UI 需要读取的状态逐步搬进 `AppStore`，减少页面对全局变量、无线模块内部变量的直接依赖。

## 当前结论

需要设计接口，尤其是 `IAppFeature` 和 `Screen`。

菜单不是简单列表，而是未来功能扩展入口。没有功能接口，菜单会成为新的耦合中心；有接口后，新增“翻硬币”“放音乐”只需要注册新 Feature，APP 层负责跳转和中断，UI 层负责显示。
