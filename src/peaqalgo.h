/* GstPEAQ
 * Copyright (C) 2021
 * Martin Holters <martin.holters@hsu-hh.de>
 *
 * peaqalgo.h: Compute objective audio quality measures
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifdef __cplusplus
#include "fbearmodel.h"
#include "fftearmodel.h"
#include "leveladapter.h"
#include "modpatt.h"
#include "movaccum.h"
#include "movs.h"
#include "nn.h"

#include <cassert>
#include <iomanip>
#include <iostream>
#include <numeric>

namespace peaq {

class Algo
{
public:
  virtual ~Algo() = default;
  [[nodiscard]] virtual auto get_channels() const -> int = 0;
  virtual void set_channels(unsigned int channel_count) = 0;
  [[nodiscard]] virtual auto get_playback_level() const -> double = 0;
  virtual void set_playback_level(double playback_level) = 0;
  virtual void process_block(float const* refdata,
                             float const* testdata,
                             std::size_t num_samples) = 0;
  virtual void flush() = 0;
  [[nodiscard]] virtual auto calculate_di(bool console_output = false) const -> double = 0;
  [[nodiscard]] virtual auto calculate_odg(bool console_output = false) const -> double = 0;
};

template<std::size_t FFT_BAND_COUNT, std::size_t MAIN_BAND_COUNT, typename Derived>
class AlgoBase : public Algo
{
public:
  [[nodiscard]] auto get_channels() const -> int final { return channel_count; }
  void set_channels(unsigned int channel_count) override
  {
    this->channel_count = channel_count;
    fft_earmodel_state_ref.resize(channel_count);
    fft_earmodel_state_test.resize(channel_count);
  }
  [[nodiscard]] auto calculate_odg(bool console_output = false) const -> double final
  {
    auto distortion_index =
      dynamic_cast<Derived const*>(this)->calculate_di(console_output);
    return calculate_odg(distortion_index, console_output);
  }
  static auto calculate_odg(double distortion_index, bool console_output = false)
  {
    auto odg = peaq_calculate_odg(distortion_index);
    if (console_output) {
      std::cout.imbue(std::locale(""));
      std::cout << std::fixed << std::setprecision(3)
                << "Objective Difference Grade: " << odg << "\n";
    }
    return odg;
  }

protected:
  template<typename InputIt>
  static auto is_frame_above_threshold(InputIt first, InputIt last)
  {
    InputIt five_ahead = first + 5;
    auto sum = std::accumulate(
      first, five_ahead, 0.0F, [](auto s, auto x) { return s + std::abs(x); });
    while (five_ahead < last) {
      sum += std::abs(*five_ahead++) - std::abs(*first++);
      if (sum >= 200. / 32768) {
        return true;
      }
    }
    return false;
  }

  unsigned int channel_count;
  std::size_t buffer_valid_count{ 0 };
  std::size_t frame_counter{ 0 };
  std::size_t loudness_reached_frame{ std::numeric_limits<unsigned int>::max() };
  std::vector<typename FFTEarModel<FFT_BAND_COUNT>::state_t> fft_earmodel_state_ref;
  std::vector<typename FFTEarModel<FFT_BAND_COUNT>::state_t> fft_earmodel_state_test;
  std::vector<LevelAdapter<MAIN_BAND_COUNT>> level_adapters;
  std::vector<ModulationProcessor<MAIN_BAND_COUNT>> ref_modulation_processors;
  std::vector<ModulationProcessor<MAIN_BAND_COUNT>> test_modulation_processors;
};

class AlgoBasic : public AlgoBase<109, 109, AlgoBasic>
{
private:
  enum
  {
    MOV_BANDWIDTH_REF,
    MOV_BANDWIDTH_TEST,
    MOV_TOTAL_NMR,
    MOV_WIN_MOD_DIFF,
    MOV_ADB,
    MOV_EHS,
    MOV_AVG_MOD_DIFF_1,
    MOV_AVG_MOD_DIFF_2,
    MOV_RMS_NOISE_LOUD,
    MOV_MFPD,
    MOV_REL_DIST_FRAMES,
  };

  void do_process();

public:
  static auto constexpr FFT_BAND_COUNT = 109;
  static auto constexpr FRAME_SIZE = FFTEarModel<FFT_BAND_COUNT>::FRAME_SIZE;
  void set_channels(unsigned int channel_count) final
  {
    AlgoBase::set_channels(channel_count);
    ref_buffers.resize(channel_count);
    test_buffers.resize(channel_count);
    level_adapters.resize(channel_count, LevelAdapter{ fft_ear_model });
    ref_modulation_processors.resize(channel_count, ModulationProcessor{ fft_ear_model });
    test_modulation_processors.resize(channel_count, ModulationProcessor{ fft_ear_model });
    int i = 0;
    std::apply(
      [channel_count, &i](auto&... accum) {
        ((accum.set_channels(i == MOV_ADB || i == MOV_MFPD ? 1 : channel_count), i++), ...);
      },
      mov_accums);
  }
  [[nodiscard]] auto get_playback_level() const -> double final
  {
    return fft_ear_model.get_playback_level();
  }
  void set_playback_level(double playback_level) final
  {
    fft_ear_model.set_playback_level(playback_level);
  }
  void process_block(float const* refdata,
                     float const* testdata,
                     std::size_t num_samples) final
  {

    while (num_samples > 0) {
      auto insert_count = std::min(num_samples, FRAME_SIZE - buffer_valid_count);
      for (auto c = 0U; c < channel_count; c++) {
        for (std::size_t i = 0; i < insert_count; i++) {
          ref_buffers[c][buffer_valid_count + i] = refdata[channel_count * i + c];
          test_buffers[c][buffer_valid_count + i] = testdata[channel_count * i + c];
        }
      }

      num_samples -= insert_count;
      refdata += channel_count * insert_count;
      testdata += channel_count * insert_count;
      buffer_valid_count += insert_count;
      if (buffer_valid_count < FRAME_SIZE) {
        assert(num_samples == 0);
        break;
      }

      do_process();

      auto step_size = FRAME_SIZE / 2;
      for (auto c = 0U; c < channel_count; c++) {
        std::copy(
          cbegin(ref_buffers[c]) + step_size, cend(ref_buffers[c]), begin(ref_buffers[c]));
        std::copy(cbegin(test_buffers[c]) + step_size,
                  cend(test_buffers[c]),
                  begin(test_buffers[c]));
      }
      buffer_valid_count -= step_size;
    }
  }

  void flush() final
  {
    if (buffer_valid_count > 0) {
      for (auto c = 0U; c < channel_count; c++) {
        std::fill(begin(ref_buffers[c]) + buffer_valid_count, end(ref_buffers[c]), 0.0);
        std::fill(begin(test_buffers[c]) + buffer_valid_count, end(test_buffers[c]), 0.0);
      }
      buffer_valid_count = FRAME_SIZE;
      do_process();
      buffer_valid_count = 0;
    }
  }
  [[nodiscard]] auto calculate_di(bool console_output = false) const -> double final
  {
    auto movs = std::apply(
      [](auto&... accum) { return std::array{ accum.get_value()... }; }, mov_accums);
    auto distortion_index = peaq_calculate_di_basic(movs.data());

    if (console_output) {
      std::cout.imbue(std::locale(""));
      std::cout << std::fixed << std::setprecision(6) << "   BandwidthRefB: " << movs[0]
                << "\n"
                << "  BandwidthTestB: " << movs[1] << "\n"
                << "      Total NMRB: " << movs[2] << "\n"
                << "    WinModDiff1B: " << movs[3] << "\n"
                << "            ADBB: " << movs[4] << "\n"
                << "            EHSB: " << movs[5] << "\n"
                << "    AvgModDiff1B: " << movs[6] << "\n"
                << "    AvgModDiff2B: " << movs[7] << "\n"
                << "   RmsNoiseLoudB: " << movs[8] << "\n"
                << "           MFPDB: " << movs[9] << "\n"
                << "  RelDistFramesB: " << movs[10] << "\n";
    }

    return distortion_index;
  }

private:
  std::vector<std::array<float, FRAME_SIZE>> ref_buffers;
  std::vector<std::array<float, FRAME_SIZE>> test_buffers;
  FFTEarModel<FFT_BAND_COUNT> fft_ear_model{};
  std::tuple<movaccum_avg,
             movaccum_avg,
             movaccum_avg_log,
             movaccum_avg_window,
             movaccum_adb,
             movaccum_avg,
             movaccum_avg,
             movaccum_avg,
             movaccum_rms,
             movaccum_filtered_max,
             movaccum_avg>
    mov_accums;
};

class AlgoAdvanced : public AlgoBase<55, 40, AlgoAdvanced>
{
private:
  enum
  {
    MOV_RMS_MOD_DIFF,
    MOV_RMS_NOISE_LOUD_ASYM,
    MOV_SEGMENTAL_NMR,
    MOV_EHS,
    MOV_AVG_LIN_DIST,
  };

  void do_process_fft();
  void do_process_fb();

public:
  static auto constexpr FFT_BAND_COUNT = 55;
  static auto constexpr FFT_FRAME_SIZE = FFTEarModel<FFT_BAND_COUNT>::FRAME_SIZE;
  static auto constexpr FB_FRAME_SIZE = FilterbankEarModel::FRAME_SIZE;
  static auto constexpr BUFFER_SIZE = FFT_FRAME_SIZE + FB_FRAME_SIZE;
  void set_channels(unsigned int channel_count) final
  {
    AlgoBase::set_channels(channel_count);
    ref_buffers.resize(channel_count);
    test_buffers.resize(channel_count);
    fb_earmodel_state_ref.resize(channel_count);
    fb_earmodel_state_test.resize(channel_count);
    level_adapters.resize(channel_count, LevelAdapter{ fb_ear_model });
    ref_modulation_processors.resize(channel_count, ModulationProcessor{ fb_ear_model });
    test_modulation_processors.resize(channel_count, ModulationProcessor{ fb_ear_model });
    std::apply(
      [channel_count](auto&... accum) { (accum.set_channels(channel_count), ...); },
      mov_accums);
  }
  [[nodiscard]] auto get_playback_level() const -> double final
  {
    return fft_ear_model.get_playback_level();
  }
  void set_playback_level(double playback_level) final
  {
    fft_ear_model.set_playback_level(playback_level);
    fb_ear_model.set_playback_level(playback_level);
  }
  void process_block(float const* refdata,
                     float const* testdata,
                     std::size_t num_samples) final
  {

    while (num_samples > 0) {
      auto insert_count = std::min(num_samples, BUFFER_SIZE - buffer_valid_count);
      for (auto c = 0U; c < channel_count; c++) {
        for (std::size_t i = 0; i < insert_count; i++) {
          ref_buffers[c][buffer_valid_count + i] = refdata[channel_count * i + c];
          test_buffers[c][buffer_valid_count + i] = testdata[channel_count * i + c];
        }
      }

      num_samples -= insert_count;
      refdata += channel_count * insert_count;
      testdata += channel_count * insert_count;
      buffer_valid_count += insert_count;

      while (buffer_valid_count >= FFT_FRAME_SIZE + buffer_fft_offset) {
        do_process_fft();
        buffer_fft_offset += FFT_FRAME_SIZE / 2;
      }
      while (buffer_valid_count >= FB_FRAME_SIZE + buffer_fb_offset) {
        do_process_fb();
        buffer_fb_offset += FB_FRAME_SIZE;
      }
      auto step_size = std::min(buffer_fft_offset, buffer_fb_offset);
      for (auto c = 0U; c < channel_count; c++) {
        std::copy(
          cbegin(ref_buffers[c]) + step_size, cend(ref_buffers[c]), begin(ref_buffers[c]));
        std::copy(cbegin(test_buffers[c]) + step_size,
                  cend(test_buffers[c]),
                  begin(test_buffers[c]));
      }
      buffer_valid_count -= step_size;
      buffer_fft_offset -= step_size;
      buffer_fb_offset -= step_size;
    }
  }

  void flush() final
  {
    if (buffer_valid_count > 0) {
      for (auto c = 0U; c < channel_count; c++) {
        std::fill(begin(ref_buffers[c]) + buffer_valid_count, end(ref_buffers[c]), 0.0);
        std::fill(begin(test_buffers[c]) + buffer_valid_count, end(test_buffers[c]), 0.0);
      }
      buffer_valid_count = BUFFER_SIZE;
      do_process_fft();
      do_process_fb();
      buffer_valid_count = 0;
      buffer_fft_offset = 0;
      buffer_fb_offset = 0;
    }
  }

  [[nodiscard]] auto calculate_di(bool console_output = false) const -> double final
  {
    auto movs = std::apply(
      [](auto&... accum) { return std::array{ accum.get_value()... }; }, mov_accums);
    auto distortion_index = peaq_calculate_di_advanced(movs.data());

    if (console_output) {
      std::cout.imbue(std::locale(""));
      std::cout << std::fixed << std::setprecision(6) << "RmsModDiffA = " << movs[0] << "\n"
                << "RmsNoiseLoudAsymA = " << movs[1] << "\n"
                << "SegmentalNMRB = " << movs[2] << "\n"
                << "EHSB = " << movs[3] << "\n"
                << "AvgLinDistA = " << movs[4] << "\n";
    }

    return distortion_index;
  }

private:
  std::size_t buffer_fft_offset{ 0 };
  std::size_t buffer_fb_offset{ 0 };
  std::vector<std::array<float, BUFFER_SIZE>> ref_buffers;
  std::vector<std::array<float, BUFFER_SIZE>> test_buffers;
  std::vector<FilterbankEarModel::state_t> fb_earmodel_state_ref;
  std::vector<FilterbankEarModel::state_t> fb_earmodel_state_test;
  FFTEarModel<FFT_BAND_COUNT> fft_ear_model{};
  FilterbankEarModel fb_ear_model{};
  std::tuple<movaccum_rms, movaccum_rms_asym, movaccum_avg, movaccum_avg, movaccum_avg>
    mov_accums;
};

} // namespace peaq

using PeaqAlgo = peaq::Algo;
extern "C" {
#else
typedef struct _PeaqAlgo PeaqAlgo;
#endif

PeaqAlgo* peaq_algo_basic_new();
PeaqAlgo* peaq_algo_advanced_new();
void peaq_algo_delete(PeaqAlgo* algo);
int peaq_algo_get_channels(PeaqAlgo const* algo);
void peaq_algo_set_channels(PeaqAlgo* algo, unsigned int channel_count);
double peaq_algo_get_playback_level(PeaqAlgo const* algo);
void peaq_algo_set_playback_level(PeaqAlgo* algo, double playback_level);
void peaq_algo_process_block(PeaqAlgo* algo,
                             float const* refdata,
                             float const* testdata,
                             size_t num_samples);
void peaq_algo_flush(PeaqAlgo* algo);
double peaq_algo_calculate_di(PeaqAlgo* algo, int console_output);
double peaq_algo_calculate_odg(PeaqAlgo* algo, int console_output);

#ifdef __cplusplus
} // extern "C" {
#endif