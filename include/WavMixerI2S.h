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
