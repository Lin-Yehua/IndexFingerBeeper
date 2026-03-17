#include "WavMixerI2S.h"
#include <string.h>
#include <stdlib.h>

WavMixerI2S::WavMixerI2S()
    : _bgBuffer(nullptr),
      _insertBuffer(nullptr),
      _outBuffer(nullptr),
      _bgDataOffset(44),
      _bgActive(false),
      _insertDataOffset(44),
      _insertActive(false),
      _taskHandle(nullptr),
      _running(false),
      _begun(false),
      _fileMutex(nullptr) {
    _fileMutex = xSemaphoreCreateMutex();
}

WavMixerI2S::~WavMixerI2S() {
    stop();

    if (_bgFile) {
        _bgFile.close();
    }
    if (_insertFile) {
        _insertFile.close();
    }

    if (_bgBuffer) {
        free(_bgBuffer);
        _bgBuffer = nullptr;
    }
    if (_insertBuffer) {
        free(_insertBuffer);
        _insertBuffer = nullptr;
    }
    if (_outBuffer) {
        free(_outBuffer);
        _outBuffer = nullptr;
    }

    if (_fileMutex) {
        vSemaphoreDelete(_fileMutex);
        _fileMutex = nullptr;
    }

    i2s_driver_uninstall(I2S_NUM_0);
}

bool WavMixerI2S::begin(const I2SPinConfig& pins) {
    AudioConfig cfg;
    return begin(pins, cfg);
}

bool WavMixerI2S::begin(const I2SPinConfig& pins, const AudioConfig& cfg) {
    _cfg = cfg;
    _pins = pins;

    if (_cfg.channels != 1) {
        Serial.println("[WavMixerI2S] only mono PCM16 WAV supported");
        return false;
    }

    if (_cfg.bitsPerSample != 16) {
        Serial.println("[WavMixerI2S] only 16-bit WAV supported");
        return false;
    }

    if (!initFS()) {
        return false;
    }

    size_t bufBytes = _cfg.samplesPerChunk * sizeof(int16_t);

    _bgBuffer = (int16_t*)malloc(bufBytes);
    _insertBuffer = (int16_t*)malloc(bufBytes);
    _outBuffer = (int16_t*)malloc(bufBytes);

    if (!_bgBuffer || !_insertBuffer || !_outBuffer) {
        Serial.println("[WavMixerI2S] buffer allocation failed");
        return false;
    }

    memset(_bgBuffer, 0, bufBytes);
    memset(_insertBuffer, 0, bufBytes);
    memset(_outBuffer, 0, bufBytes);

    if (!initI2S()) {
        return false;
    }

    _begun = true;
    return true;
}

void WavMixerI2S::startOnCore(BaseType_t coreID, UBaseType_t priority, uint32_t stackSize) {
    if (!_begun) {
        Serial.println("[WavMixerI2S] call begin() first");
        return;
    }

    if (_taskHandle != nullptr) {
        Serial.println("[WavMixerI2S] task already started");
        return;
    }

    _running = true;

    xTaskCreatePinnedToCore(
        audioTaskEntry,
        "WavMixerI2S_Task",
        stackSize,
        this,
        priority,
        &_taskHandle,
        coreID
    );
}

void WavMixerI2S::stop() {
    _running = false;

    if (_taskHandle != nullptr) {
        vTaskDelete(_taskHandle);
        _taskHandle = nullptr;
    }

    if (_bgFile) {
        _bgFile.close();
    }
    if (_insertFile) {
        _insertFile.close();
    }

    _bgActive = false;
    _insertActive = false;

    i2s_zero_dma_buffer(I2S_NUM_0);
}

bool WavMixerI2S::playBG(const char* path) {
    if (!_begun || path == nullptr) {
        return false;
    }

    if (xSemaphoreTake(_fileMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return false;
    }

    if (_bgFile) {
        _bgFile.close();
    }

    File newFile;
    size_t newOffset = 44;
    bool ok = openAndValidateWav(newFile, path, newOffset);

    if (ok) {
        _bgFile = newFile;
        _bgDataOffset = newOffset;
        _bgPath = path;
        _bgActive = true;
        Serial.printf("[WavMixerI2S] BG started: %s\n", path);
    } else {
        _bgActive = false;
        Serial.printf("[WavMixerI2S] BG open failed: %s\n", path);
    }

    xSemaphoreGive(_fileMutex);
    return ok;
}

bool WavMixerI2S::stopBG() {
    if (xSemaphoreTake(_fileMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return false;
    }

    if (_bgFile) {
        _bgFile.close();
    }
    _bgActive = false;
    _bgPath = "";

    xSemaphoreGive(_fileMutex);
    return true;
}

bool WavMixerI2S::playInsert(const char* path) {
    if (!_begun || path == nullptr) {
        return false;
    }

    if (xSemaphoreTake(_fileMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return false;
    }

    if (_insertFile) {
        _insertFile.close();
    }

    File newFile;
    size_t newOffset = 44;
    bool ok = openAndValidateWav(newFile, path, newOffset);

    if (ok) {
        _insertFile = newFile;
        _insertDataOffset = newOffset;
        _insertPath = path;
        _insertActive = true;
        Serial.printf("[WavMixerI2S] Insert started: %s\n", path);
    } else {
        _insertActive = false;
        Serial.printf("[WavMixerI2S] Insert open failed: %s\n", path);
    }

    xSemaphoreGive(_fileMutex);
    return ok;
}

void WavMixerI2S::stopInsert() {
    if (xSemaphoreTake(_fileMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return;
    }

    if (_insertFile) {
        _insertFile.close();
    }
    _insertActive = false;
    _insertPath = "";

    xSemaphoreGive(_fileMutex);
}

bool WavMixerI2S::isRunning() const {
    return _running;
}

bool WavMixerI2S::isBGPlaying() const {
    return _bgActive;
}

bool WavMixerI2S::isInsertPlaying() const {
    return _insertActive;
}

void WavMixerI2S::setBgGain(float gain) {
    _cfg.bgGain = gain;
}

void WavMixerI2S::setInsertGain(float gain) {
    _cfg.insertGain = gain;
}

void WavMixerI2S::audioTaskEntry(void* param) {
    WavMixerI2S* self = static_cast<WavMixerI2S*>(param);
    if (self) {
        self->audioTask();
    }
    vTaskDelete(nullptr);
}

void WavMixerI2S::audioTask() {
    Serial.printf("[WavMixerI2S] audioTask running on core %d\n", xPortGetCoreID());

    while (_running) {
        bool bgActiveLocal = false;
        bool insertActiveLocal = false;
        bool insertFinished = false;

        if (xSemaphoreTake(_fileMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            bgActiveLocal = _bgActive;
            insertActiveLocal = _insertActive;

            if (bgActiveLocal && _bgFile) {
                readSamplesLoop(_bgFile, _bgBuffer, _cfg.samplesPerChunk, _bgDataOffset);
            } else {
                memset(_bgBuffer, 0, _cfg.samplesPerChunk * sizeof(int16_t));
            }

            if (insertActiveLocal && _insertFile) {
                readSamplesOneShot(_insertFile,
                                   _insertBuffer,
                                   _cfg.samplesPerChunk,
                                   _insertDataOffset,
                                   insertFinished);
                if (insertFinished) {
                    _insertFile.close();
                    _insertActive = false;
                    _insertPath = "";
                }
            } else {
                memset(_insertBuffer, 0, _cfg.samplesPerChunk * sizeof(int16_t));
            }

            xSemaphoreGive(_fileMutex);
        } else {
            memset(_bgBuffer, 0, _cfg.samplesPerChunk * sizeof(int16_t));
            memset(_insertBuffer, 0, _cfg.samplesPerChunk * sizeof(int16_t));
        }

        mixAudio(_bgBuffer, _insertBuffer, _outBuffer, _cfg.samplesPerChunk, insertActiveLocal);

        size_t bytesWritten = 0;
        i2s_write(
            I2S_NUM_0,
            (const char*)_outBuffer,
            _cfg.samplesPerChunk * sizeof(int16_t),
            &bytesWritten,
            portMAX_DELAY
        );
    }
}

bool WavMixerI2S::initFS() {
    if (!LittleFS.begin(true)) {
        Serial.println("[WavMixerI2S] LittleFS mount failed");
        return false;
    }
    return true;
}

bool WavMixerI2S::initI2S() {
    i2s_config_t i2s_config = {};
    i2s_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    i2s_config.sample_rate = _cfg.sampleRate;
    i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    i2s_config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    i2s_config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    i2s_config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    i2s_config.dma_buf_count = 8;
    i2s_config.dma_buf_len = 256;
    i2s_config.use_apll = false;
    i2s_config.tx_desc_auto_clear = true;
    i2s_config.fixed_mclk = 0;

    i2s_pin_config_t pin_config = {};
    pin_config.bck_io_num = _pins.bck;
    pin_config.ws_io_num = _pins.ws;
    pin_config.data_out_num = _pins.dout;
    pin_config.data_in_num = I2S_PIN_NO_CHANGE;

    esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, nullptr);
    if (err != ESP_OK) {
        Serial.printf("[WavMixerI2S] i2s_driver_install failed: %d\n", (int)err);
        return false;
    }

    err = i2s_set_pin(I2S_NUM_0, &pin_config);
    if (err != ESP_OK) {
        Serial.printf("[WavMixerI2S] i2s_set_pin failed: %d\n", (int)err);
        return false;
    }

    i2s_zero_dma_buffer(I2S_NUM_0);
    return true;
}

bool WavMixerI2S::openAndValidateWav(File& file, const char* path, size_t& dataOffset) {
    file = LittleFS.open(path, "r");
    if (!file) {
        return false;
    }

    WavHeader hdr;
    if (!readWavHeader(file, hdr)) {
        file.close();
        return false;
    }

    if (hdr.audioFormat != 1) {
        file.close();
        return false;
    }

    if (hdr.numChannels != _cfg.channels) {
        file.close();
        return false;
    }

    if (hdr.sampleRate != _cfg.sampleRate) {
        file.close();
        return false;
    }

    if (hdr.bitsPerSample != _cfg.bitsPerSample) {
        file.close();
        return false;
    }

    dataOffset = sizeof(WavHeader);
    file.seek(dataOffset);
    return true;
}

bool WavMixerI2S::readWavHeader(File& file, WavHeader& hdr) {
    if (!file || file.size() < sizeof(WavHeader)) {
        return false;
    }

    file.seek(0);
    size_t n = file.read((uint8_t*)&hdr, sizeof(WavHeader));
    if (n != sizeof(WavHeader)) {
        return false;
    }

    if (strncmp(hdr.riff, "RIFF", 4) != 0 ||
        strncmp(hdr.wave, "WAVE", 4) != 0 ||
        strncmp(hdr.fmt, "fmt ", 4) != 0 ||
        strncmp(hdr.data, "data", 4) != 0) {
        return false;
    }

    return true;
}

size_t WavMixerI2S::readSamplesLoop(File& file,
                                    int16_t* buffer,
                                    size_t samplesNeeded,
                                    size_t dataOffset) {
    size_t bytesNeeded = samplesNeeded * sizeof(int16_t);
    size_t bytesRead = 0;

    while (bytesRead < bytesNeeded) {
        if (!file.available()) {
            file.seek(dataOffset);
        }

        size_t n = file.read(((uint8_t*)buffer) + bytesRead, bytesNeeded - bytesRead);
        if (n == 0) {
            file.seek(dataOffset);
        } else {
            bytesRead += n;
        }
    }

    return bytesRead / sizeof(int16_t);
}

size_t WavMixerI2S::readSamplesOneShot(File& file,
                                       int16_t* buffer,
                                       size_t samplesNeeded,
                                       size_t dataOffset,
                                       bool& finished) {
    (void)dataOffset;
    finished = false;

    size_t bytesNeeded = samplesNeeded * sizeof(int16_t);
    size_t bytesRead = 0;

    while (bytesRead < bytesNeeded) {
        if (!file.available()) {
            memset(((uint8_t*)buffer) + bytesRead, 0, bytesNeeded - bytesRead);
            finished = true;
            return samplesNeeded;
        }

        size_t n = file.read(((uint8_t*)buffer) + bytesRead, bytesNeeded - bytesRead);
        if (n == 0) {
            memset(((uint8_t*)buffer) + bytesRead, 0, bytesNeeded - bytesRead);
            finished = true;
            return samplesNeeded;
        }

        bytesRead += n;
    }

    return bytesRead / sizeof(int16_t);
}

void WavMixerI2S::mixAudio(const int16_t* bg,
                           const int16_t* in,
                           int16_t* out,
                           size_t samples,
                           bool insertActive) {
    if (!insertActive) {
        for (size_t i = 0; i < samples; ++i) {
            int32_t mixed = (int32_t)(bg[i] * _cfg.bgGain);
            out[i] = saturate16(mixed);
        }
        return;
    }

    for (size_t i = 0; i < samples; ++i) {
        int32_t mixed = (int32_t)(bg[i] * _cfg.bgGain)
                      + (int32_t)(in[i] * _cfg.insertGain);
        out[i] = saturate16(mixed);
    }
}

int16_t WavMixerI2S::saturate16(int32_t x) {
    if (x > 32767) return 32767;
    if (x < -32768) return -32768;
    return (int16_t)x;
}