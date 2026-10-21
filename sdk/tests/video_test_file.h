// SPDX-License-Identifier: Apache-2.0
// A video file with known contents, written by Media Foundation's own encoder
// -- for modules_test.cpp, which decodes it and checks what it gets, and
// fuzz_modules.cpp, which mutates it. Windows only; test code, not SDK code
// (the SDK decodes and never encodes).
//
// One second, 160x120 at 30 fps, H.264, with an AAC soundtrack of a 440 Hz
// sine at half scale. Each frame: the top quarter white on its left half and
// black on its right, the bottom quarter black, and the middle band RED for
// frames 0-9, GREEN for 10-19, BLUE for 20-29. A decoder that flips the
// picture in either axis moves the white corner; one that loses or repeats
// frames changes the count or a colour.
//
// 120 rows is not a multiple of 16 on purpose: H.264 codes 128 and crops, so
// a reader that ignores the display aperture shows eight rows of padding.
#pragma once

#if defined(_WIN32)

#include <sonore/gfx/video.h>

#include <cmath>
#include <cstdint>
#include <string>

namespace video_fixture {

constexpr int kWidth = 160, kHeight = 120, kFrames = 30, kFps = 30, kAudioRate = 48000;

/** 0 red, 1 green, 2 blue: the middle band's colour in frame `i`. */
inline int bandColour(int i) { return i < 10 ? 0 : (i < 20 ? 1 : 2); }

inline bool write(const std::string& path, std::string& why) {
  using sonore::gfx::detail::mf::Ref;
  using sonore::gfx::detail::mf::hresultMessage;
  Ref<IMFAttributes> attributes;
  MFCreateAttributes(attributes.put(), 1);
  attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
  Ref<IMFSinkWriter> writer;
  HRESULT hr = MFCreateSinkWriterFromURL(sonore::pathio::widen(path.c_str()).c_str(), nullptr, attributes.get(),
                                         writer.put());
  if (FAILED(hr)) return why = hresultMessage("no sink writer", hr), false;

  Ref<IMFMediaType> videoOut, videoIn, audioOut, audioIn;
  MFCreateMediaType(videoOut.put());
  videoOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  videoOut->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  videoOut->SetUINT32(MF_MT_AVG_BITRATE, 4000000);
  videoOut->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  MFSetAttributeSize(videoOut.get(), MF_MT_FRAME_SIZE, kWidth, kHeight);
  MFSetAttributeRatio(videoOut.get(), MF_MT_FRAME_RATE, kFps, 1);
  MFSetAttributeRatio(videoOut.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  DWORD videoStream = 0;
  hr = writer->AddStream(videoOut.get(), &videoStream);
  if (FAILED(hr)) return why = hresultMessage("no H.264 stream", hr), false;
  MFCreateMediaType(videoIn.put());
  videoIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  videoIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
  videoIn->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  // Positive: top-down, STATED, so the round trip cannot pass by two
  // conventions cancelling out.
  videoIn->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32) (kWidth * 4));
  MFSetAttributeSize(videoIn.get(), MF_MT_FRAME_SIZE, kWidth, kHeight);
  MFSetAttributeRatio(videoIn.get(), MF_MT_FRAME_RATE, kFps, 1);
  MFSetAttributeRatio(videoIn.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  hr = writer->SetInputMediaType(videoStream, videoIn.get(), nullptr);
  if (FAILED(hr)) return why = hresultMessage("RGB32 input refused", hr), false;

  MFCreateMediaType(audioOut.put());
  audioOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  audioOut->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
  audioOut->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
  audioOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kAudioRate);
  audioOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
  audioOut->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
  DWORD audioStream = 0;
  hr = writer->AddStream(audioOut.get(), &audioStream);
  if (FAILED(hr)) return why = hresultMessage("no AAC stream", hr), false;
  MFCreateMediaType(audioIn.put());
  audioIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  audioIn->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
  audioIn->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
  audioIn->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kAudioRate);
  audioIn->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
  audioIn->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
  audioIn->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kAudioRate * 4);
  hr = writer->SetInputMediaType(audioStream, audioIn.get(), nullptr);
  if (FAILED(hr)) return why = hresultMessage("PCM input refused", hr), false;

  hr = writer->BeginWriting();
  if (FAILED(hr)) return why = hresultMessage("BeginWriting", hr), false;

  const LONGLONG frameTime = 10000000LL / kFps;
  const int audioPerFrame = kAudioRate / kFps;
  for (int i = 0; i < kFrames; ++i) {
    Ref<IMFMediaBuffer> buffer;
    MFCreateMemoryBuffer((DWORD) (kWidth * kHeight * 4), buffer.put());
    BYTE* data = nullptr;
    buffer->Lock(&data, nullptr, nullptr);
    const int band = bandColour(i);
    for (int y = 0; y < kHeight; ++y)
      for (int x = 0; x < kWidth; ++x) {
        BYTE* p = data + ((size_t) y * kWidth + (size_t) x) * 4; // B, G, R, X
        BYTE r = 0, g = 0, b = 0;
        if (y < kHeight / 4) {
          if (x < kWidth / 2) r = g = b = 255;
        } else if (y < kHeight * 3 / 4) {
          r = band == 0 ? 255 : 0;
          g = band == 1 ? 255 : 0;
          b = band == 2 ? 255 : 0;
        }
        p[0] = b;
        p[1] = g;
        p[2] = r;
        p[3] = 255;
      }
    buffer->Unlock();
    buffer->SetCurrentLength((DWORD) (kWidth * kHeight * 4));
    Ref<IMFSample> sample;
    MFCreateSample(sample.put());
    sample->AddBuffer(buffer.get());
    sample->SetSampleTime(i * frameTime);
    sample->SetSampleDuration(frameTime);
    hr = writer->WriteSample(videoStream, sample.get());
    if (FAILED(hr)) return why = hresultMessage("WriteSample (video)", hr), false;

    Ref<IMFMediaBuffer> abuf;
    MFCreateMemoryBuffer((DWORD) (audioPerFrame * 4), abuf.put());
    BYTE* adata = nullptr;
    abuf->Lock(&adata, nullptr, nullptr);
    int16_t* pcm = (int16_t*) adata;
    for (int n = 0; n < audioPerFrame; ++n) {
      const double t = (double) (i * audioPerFrame + n) / kAudioRate;
      const int16_t v = (int16_t) std::lround(0.5 * 32767.0 * std::sin(2.0 * 3.14159265358979323846 * 440.0 * t));
      pcm[n * 2] = v;
      pcm[n * 2 + 1] = v;
    }
    abuf->Unlock();
    abuf->SetCurrentLength((DWORD) (audioPerFrame * 4));
    Ref<IMFSample> asample;
    MFCreateSample(asample.put());
    asample->AddBuffer(abuf.get());
    asample->SetSampleTime(i * frameTime);
    asample->SetSampleDuration(frameTime);
    hr = writer->WriteSample(audioStream, asample.get());
    if (FAILED(hr)) return why = hresultMessage("WriteSample (audio)", hr), false;
  }
  hr = writer->Finalize();
  if (FAILED(hr)) return why = hresultMessage("Finalize", hr), false;
  return true;
}

} // namespace video_fixture

#endif // _WIN32
