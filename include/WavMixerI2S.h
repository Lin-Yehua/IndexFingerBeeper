/*
 * 文件说明: 公共接口头文件。
 * 文件功能: 声明对应模块的类型、常量和可被其他编译单元调用的函数接口。
 *
 * 函数表:
 * - begin: 初始化或确保对应资源可用。
 * - startOnCore: 模块内部辅助函数。
 * - stop: 停止、释放或清理对应状态。
 * - playBG: 播放对应消息或音频流程。
 * - playBGnoLoop: 播放对应消息或音频流程。
 * - stopBG: 停止、释放或清理对应状态。
 * - playInsert: 播放对应消息或音频流程。
 * - stopInsert: 停止、释放或清理对应状态。
 * - setBgGain: 保存、写入或更新对应数据。
 * - setInsertGain: 保存、写入或更新对应数据。
 * - audioTaskEntry: FreeRTOS 任务入口或任务控制函数。
 * - audioTask: FreeRTOS 任务入口或任务控制函数。
 * - initFS: 初始化或确保对应资源可用。
 * - initI2S: 初始化或确保对应资源可用。
 * - openAndValidateWav: 初始化或确保对应资源可用。
 * - readWavHeader: 读取、获取或消费对应数据。
 * - readSamplesLoop: 读取、获取或消费对应数据。
 * - readSamplesOneShot: 读取、获取或消费对应数据。
 * - readSamplesOneShotPlain: 读取、获取或消费对应数据。
 * - mixAudio: 模块内部辅助函数。
 * - AudioConfig: 模块内部辅助函数。
 */
#pragma once

#include "driver/i2s.h"
#include <Arduino.h>
#include <FS.h>

class WavMixerI2S
{
public:
  struct I2SPinConfig
  {
    int bck;
    int ws;
    int dout;
  };

  struct AudioConfig
  {
    uint32_t sampleRate;
    uint8_t bitsPerSample;
    uint8_t channels;
    size_t samplesPerChunk;
    float bgGain;
    float insertGain;

    AudioConfig()
        : sampleRate(44100), bitsPerSample(16), channels(1), samplesPerChunk(512), bgGain(0.6f), insertGain(1.0f)
    {
    }
  };

  WavMixerI2S();
  ~WavMixerI2S();

  bool begin(const I2SPinConfig &pins);
  bool begin(const I2SPinConfig &pins, const AudioConfig &cfg);

  void startOnCore(BaseType_t coreID = 0, UBaseType_t priority = 2, uint32_t stackSize = 8192);
  void stop();

  bool playBG(const char *path);
  bool playBGnoLoop(const char *path);
  bool stopBG();

  bool playInsert(const char *path);
  void stopInsert();

  bool isRunning() const;
  bool isBGPlaying() const;
  bool isInsertPlaying() const;

  void setBgGain(float gain);
  void setInsertGain(float gain);

private:
  typedef struct __attribute__((packed))
  {
    char riff[4];
    uint32_t chunkSize;
    char wave[4];
    char fmt[4];
    uint32_t subchunk1Size;
    uint16_t audioFormat;
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
    char data[4];
    uint32_t dataSize;
  } WavHeader;

  static void audioTaskEntry(void *param);
  void audioTask();

  bool initFS();
  bool initI2S();

  bool openAndValidateWav(fs::File &file, const char *path, size_t &dataOffset);
  bool readWavHeader(fs::File &file, WavHeader &hdr);

  size_t readSamplesLoop(fs::File &file, int16_t *buffer, size_t samplesNeeded, size_t dataOffset);
  size_t readSamplesOneShot(fs::File &file, int16_t *buffer, size_t samplesNeeded, size_t dataOffset, bool &finished);
  size_t readSamplesOneShotPlain(fs::File &file, int16_t *buffer, size_t samplesNeeded, bool &finished);

  void mixAudio(const int16_t *bg, const int16_t *in, int16_t *out, size_t samples, bool insertActive);
  int16_t saturate16(int32_t x);

private:
  AudioConfig _cfg;
  I2SPinConfig _pins;

  int16_t *_bgBuffer;
  int16_t *_insertBuffer;
  int16_t *_outBuffer;

  fs::File _bgFile;
  size_t _bgDataOffset;
  bool _bgActive;
  bool _bgLoopEnabled;
  String _bgPath;

  fs::File _insertFile;
  size_t _insertDataOffset;
  bool _insertActive;
  String _insertPath;
  int16_t _insertLastSample;
  bool _insertHasLastSample;

  TaskHandle_t _taskHandle;
  volatile bool _running;
  bool _begun;

  SemaphoreHandle_t _fileMutex;
};
