/*
 *     Copyright (C) 2019  Filippo Scognamiglio
 *
 *     This program is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "log.h"

#include "audio.h"
#include "stats.h"
#include <algorithm>
#include <cmath>
#include <memory>

namespace libretrodroid {

Audio::Audio(int32_t sampleRate, double refreshRate, bool preferLowLatencyAudio) {
    LOGI("Audio initialization has been called with input sample rate %d", sampleRate);

    contentRefreshRate = refreshRate;
    inputSampleRate = sampleRate;
    audioLatencySettings = findBestLatencySettings(preferLowLatencyAudio);
    initializeStream();
}

bool Audio::initializeStream() {
    LOGI("Using low latency stream: %d", audioLatencySettings->useLowLatencyStream);

    int32_t audioBufferSize = computeAudioBufferSize();

    oboe::AudioStreamBuilder builder;
    builder.setChannelCount(2);
    builder.setDirection(oboe::Direction::Output);
    builder.setFormat(oboe::AudioFormat::I16);
    builder.setDataCallback(this);
    builder.setErrorCallback(this);
    builder.setUsage(oboe::Usage::Game);
    builder.setContentType(oboe::ContentType::Sonification);

    if (audioLatencySettings->useLowLatencyStream) {
        builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
        builder.setSharingMode(oboe::SharingMode::Shared);
    } else {
        builder.setFramesPerCallback(audioBufferSize / 10);
    }

    oboe::Result result = builder.openManagedStream(stream);
    if (result == oboe::Result::OK) {
        baseConversionFactor = (double) inputSampleRate / stream->getSampleRate();
        fifoBuffer = std::make_unique<oboe::FifoBuffer>(2, audioBufferSize);
        temporaryAudioBuffer = std::unique_ptr<int16_t[]>(new int16_t[audioBufferSize]);
        latencyTuner = std::make_unique<oboe::LatencyTuner>(*stream);
        soundTouch = std::make_unique<soundtouch::SoundTouch>();
        soundTouch->setSampleRate(stream->getSampleRate());
        soundTouch->setChannels(2);
        soundTouch->setSetting(SETTING_USE_QUICKSEEK, 0);
        soundTouch->setSetting(SETTING_USE_AA_FILTER, 1);
        soundTouch->setSetting(SETTING_SEQUENCE_MS, 30);
        soundTouch->setSetting(SETTING_SEEKWINDOW_MS, 20);
        soundTouch->setSetting(SETTING_OVERLAP_MS, 10);
        soundTouch->setTempo(1.0);
        return true;
    } else {
        LOGE("Failed to create stream. Error: %s", oboe::convertToText(result));
        stream = nullptr;
        latencyTuner = nullptr;
        return false;
    }
}

std::unique_ptr<Audio::AudioLatencySettings> Audio::findBestLatencySettings(bool preferLowLatencyAudio) {
    if (oboe::AudioStreamBuilder::isAAudioRecommended() && preferLowLatencyAudio) {
        return std::make_unique<AudioLatencySettings>(LOW_LATENCY_SETTINGS);
    } else {
        return std::make_unique<AudioLatencySettings>(DEFAULT_LATENCY_SETTINGS);
    }
}

int32_t Audio::computeAudioBufferSize() {
    double maxLatency = computeMaximumLatency();
    LOGI("Average audio latency set to: %f ms", maxLatency * 0.5);
    double sampleRateDivisor = 500.0 / maxLatency;
    return roundToEven(inputSampleRate / sampleRateDivisor);
}

double Audio::computeMaximumLatency() const {
    const float minFrames = audioLatencySettings->useLowLatencyStream ? 4.0f : 8.0f;
    const float videoFrames = std::max(audioLatencySettings->bufferSizeInVideoFrames, minFrames);
    const double maxLatency = (videoFrames / contentRefreshRate) * 1000.0;
    const double floorLatency = audioLatencySettings->useLowLatencyStream ? 24.0 : 48.0;
    return std::max(maxLatency, floorLatency);
}

void Audio::start() {
    startRequested = true;
    if (stream != nullptr)
        stream->requestStart();
}

void Audio::stop() {
    startRequested = false;
    if (stream != nullptr)
        stream->requestStop();
}

void Audio::updateStretch(size_t frames) {
    if (!soundTouch || inputSampleRate <= 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (stretchWindowStart.time_since_epoch().count() == 0) {
        stretchWindowStart = now;
    }
    stretchWindowFrames += frames;
    const double elapsed = std::chrono::duration<double>(now - stretchWindowStart).count();
    if (elapsed < 0.5) {
        return;
    }

    const double ratio = (static_cast<double>(stretchWindowFrames) / elapsed) / inputSampleRate;
    stretchWindowFrames = 0;
    stretchWindowStart = now;

    const bool wantStretch = playbackSpeed < 1.05 && ratio < 0.95;
    if (wantStretch) {
        fullSpeedMs = 0.0;
        if (!stretching.load()) {
            soundTouch->clear();
            stretching.store(true);
        }
        lastStretchRatio = std::clamp(ratio, 0.25, 1.0);
        soundTouch->setTempo(lastStretchRatio);
        return;
    }

    if (!stretching.load()) {
        return;
    }
    if (ratio > 0.98) {
        fullSpeedMs += elapsed * 1000.0;
        if (fullSpeedMs >= 1000.0) {
            stretching.store(false);
            soundTouch->clear();
            fullSpeedMs = 0.0;
        }
        return;
    }
    fullSpeedMs = 0.0;
    lastStretchRatio = std::clamp(ratio, 0.25, 1.0);
    soundTouch->setTempo(lastStretchRatio);
}

void Audio::write(const int16_t *data, size_t frames) {
    if (!fifoBuffer || frames == 0) {
        return;
    }
    updateStretch(frames);
    if (stretching.load() && soundTouch) {
        stretchInput.resize(frames * 2);
        for (size_t i = 0; i < frames * 2; ++i) {
            stretchInput[i] = data[i] / 32768.0f;
        }
        const double capacity = fifoBuffer->getBufferCapacityInFrames();
        const double available = fifoBuffer->getFullFramesAvailable();
        const double fill = capacity > 0.0 ? available / capacity : 0.5;
        const double bias = std::clamp((fill - 0.75) * 0.05, -0.02, 0.02);
        soundTouch->setTempo(std::clamp(lastStretchRatio * (1.0 + bias), 0.25, 1.0));
        soundTouch->putSamples(stretchInput.data(), static_cast<uint>(frames));
        const uint outCapacity = static_cast<uint>(std::max<size_t>(frames * 2, 256));
        stretchOutput.resize(outCapacity * 2);
        uint received = 0;
        while ((received = soundTouch->receiveSamples(stretchOutput.data(), outCapacity)) > 0) {
            stretchPcm.resize(received * 2);
            for (uint i = 0; i < received * 2; ++i) {
                float sample = std::clamp(stretchOutput[i], -1.0f, 1.0f);
                stretchPcm[i] = static_cast<int16_t>(sample * 32767.0f);
            }
            fifoBuffer->write(stretchPcm.data(), received * 2);
            if (received < outCapacity) {
                break;
            }
        }
    } else {
        fifoBuffer->write(data, frames * 2);
    }
    LRDStats::noteAudioWrite(frames);
}

void Audio::setPlaybackSpeed(const double newPlaybackSpeed) {
    playbackSpeed = newPlaybackSpeed;
}

oboe::DataCallbackResult Audio::onAudioReady(oboe::AudioStream *oboeStream, void *audioData, int32_t numFrames) {
    double dynamicBufferFactor = computeDynamicBufferConversionFactor(0.001 * numFrames);
    double finalConversionFactor = baseConversionFactor * dynamicBufferFactor * playbackSpeed;

    // When using low-latency stream, numFrames is very low (~100) and the dynamic buffer scaling doesn't work with rounding.
    // By keeping track of the "fractional" frames we can keep the error smaller.
    framesToSubmit += numFrames * finalConversionFactor;
    int32_t currentFramesToSubmit = std::round(framesToSubmit);
    framesToSubmit -= currentFramesToSubmit;

    int32_t requestedSamples = currentFramesToSubmit * 2;
    int32_t samplesRead = fifoBuffer->readNow(temporaryAudioBuffer.get(), requestedSamples);
    fadeBuffer(temporaryAudioBuffer.get(), samplesRead, requestedSamples);

    double capacity = fifoBuffer->getBufferCapacityInFrames();
    double available = fifoBuffer->getFullFramesAvailable();
    double fillPercent = capacity > 0.0 ? (100.0 * available / capacity) : 0.0;
    int32_t xruns = 0;
    auto xrunResult = oboeStream->getXRunCount();
    if (xrunResult) {
        xruns = xrunResult.value();
    }
    LRDStats::noteAudioState(fillPercent, xruns, lastPiAdjustment);

    auto outputArray = reinterpret_cast<int16_t *>(audioData);
    resampler.resample(temporaryAudioBuffer.get(), currentFramesToSubmit, outputArray, numFrames);

    if (!stretching.load()) {
        latencyTuner->tune();
    }

    return oboe::DataCallbackResult::Continue;
}

// To prevent audio buffer overruns or underruns we set up a PI controller. The idea is to run the
// audio slower when the buffer is empty and faster when it's full.
void Audio::fadeBuffer(int16_t* buffer, int32_t samplesRead, int32_t requestedSamples) {
    if (requestedSamples <= 0) {
        return;
    }
    if (samplesRead < 0) {
        samplesRead = 0;
    }
    if (samplesRead >= requestedSamples) {
        if (samplesRead >= 2) {
            lastOutputLeft = buffer[samplesRead - 2];
            lastOutputRight = buffer[samplesRead - 1];
        }
        if (fadingIn && samplesRead >= 2) {
            const int32_t frames = samplesRead / 2;
            const int32_t fadeFrames = std::min(frames, 96);
            for (int32_t i = 0; i < fadeFrames; ++i) {
                const float gain = static_cast<float>(i + 1) / static_cast<float>(fadeFrames);
                buffer[i * 2] = static_cast<int16_t>(buffer[i * 2] * gain);
                buffer[i * 2 + 1] = static_cast<int16_t>(buffer[i * 2 + 1] * gain);
            }
            fadingIn = false;
        }
        return;
    }

    LRDStats::noteUnderrun(requestedSamples - samplesRead);
    int16_t left = lastOutputLeft;
    int16_t right = lastOutputRight;
    if (samplesRead >= 2) {
        left = buffer[samplesRead - 2];
        right = buffer[samplesRead - 1];
    }
    const int32_t startFrame = samplesRead / 2;
    const int32_t totalFrames = requestedSamples / 2;
    const int32_t fadeFrames = std::min(totalFrames - startFrame, 96);
    for (int32_t frame = startFrame; frame < totalFrames; ++frame) {
        const int32_t intoFade = frame - startFrame;
        const float gain = intoFade < fadeFrames
            ? 1.0f - (static_cast<float>(intoFade + 1) / static_cast<float>(fadeFrames))
            : 0.0f;
        buffer[frame * 2] = static_cast<int16_t>(left * gain);
        buffer[frame * 2 + 1] = static_cast<int16_t>(right * gain);
    }
    lastOutputLeft = 0;
    lastOutputRight = 0;
    fadingIn = true;
}

double Audio::computeDynamicBufferConversionFactor(double dt) {
    if (stretching.load()) {
        lastPiAdjustment = 0.0;
        return 1.0;
    }

    double framesCapacityInBuffer = fifoBuffer->getBufferCapacityInFrames();
    double framesAvailableInBuffer = fifoBuffer->getFullFramesAvailable();
    if (framesCapacityInBuffer <= 0.0) {
        return 1.0;
    }

    const double fill = framesAvailableInBuffer / framesCapacityInBuffer;
    const double target = 0.5;
    double errorMeasure = (target - fill) / target;

    const auto now = std::chrono::steady_clock::now();
    if (fill < 0.10 || fill > 0.90) {
        if (fill < 0.10) {
            if (!lowFillActive) {
                lowFillActive = true;
                lowFillSince = now;
            } else if (std::chrono::duration<double, std::milli>(now - lowFillSince).count() > 100.0) {
                errorIntegral = 0.0;
            }
        } else {
            lowFillActive = false;
        }
    } else {
        lowFillActive = false;
        errorIntegral += errorMeasure * dt;
    }

    // Wikipedia states that human ear resolution is around 3.6 Hz within the octave of 1000–2000 Hz.
    // This changes continuously, so we should try to keep it a very low value.
    double proportionalAdjustment = std::clamp(kp * errorMeasure, -maxp, maxp);

    // Ki is a lot lower, so it's safe if it exceeds the ear threshold. Hopefully convergence will
    // be slow enough to be not perceptible. We need to battle test this value.
    double integralAdjustment = std::clamp(ki * errorIntegral, -maxi, maxi);

    double finalAdjustment = proportionalAdjustment + integralAdjustment;
    lastPiAdjustment = finalAdjustment;

    return 1.0 - (finalAdjustment);
}

int32_t Audio::roundToEven(int32_t x) {
    return (x / 2) * 2;
}

void Audio::onErrorAfterClose(oboe::AudioStream* oldStream, oboe::Result result) {
    AudioStreamErrorCallback::onErrorAfterClose(oldStream, result);
    LOGI("Stream error in oboe::onErrorAfterClose %s", oboe::convertToText(result));

    if (result != oboe::Result::ErrorDisconnected)
        return;

    initializeStream();
    if (startRequested) {
        start();
    }
}

} //namespace libretrodroid
