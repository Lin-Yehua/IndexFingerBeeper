#pragma once

#include "AppFeature.h"

class AppRuntime
{
public:
  bool registerFeature(IAppFeature *feature);
  void setDefaultFeature(uint16_t featureId);

  void begin();
  void enterFeatureById(uint16_t featureId);
  void requestMenu();
  void dispatchMessage(const AppMessage &msg);
  void tick(bool syntheticKeyPress);
  bool activeCanInterrupt();

private:
  static constexpr size_t kMaxFeatures = 8;

  IAppFeature *_features[kMaxFeatures] = {};
  size_t _featureCount = 0;
  IAppFeature *_active = nullptr;
  uint16_t _defaultFeatureId = APP_FEATURE_TEXT_PUSH;
  bool _begun = false;

  AppContext makeContext(bool syntheticKeyPress = false);
  IAppFeature *featureById(uint16_t featureId);
  void selectFeature(const AppMessage &msg);
  void publishFeatureEvent(AppMsgType type, uint16_t featureId);
};

AppRuntime &appRuntime();
