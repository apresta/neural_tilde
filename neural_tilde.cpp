// Max/MSP external for NeuralAudio backend.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "NeuralModel.h"
#include "c74_min.h"

// Default block size for ResamplingContainer.
#ifndef DEFAULT_BLOCK_SIZE
#define DEFAULT_BLOCK_SIZE 512
#endif

// Provide iplug::PI to ResamplingContainer.
#ifdef PI
#undef PI
#endif

namespace iplug {
constexpr double PI = 3.14159265358979323846;  // NOLINT
}  // namespace iplug

#include "ResamplingContainer/ResamplingContainer.h"

namespace {

// Flush denormal floats to zero to avoid CPU stalls.
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || \
    defined(_M_IX86)
#include <xmmintrin.h>
#endif
inline void disable_denormals() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || \
    defined(_M_IX86)
  _mm_setcsr(_mm_getcsr() | 0x8040);  // FTZ | DAZ
#elif defined(__aarch64__) || defined(_M_ARM64)
  std::uint64_t fpcr;
  asm volatile("mrs %0, fpcr" : "=r"(fpcr));
  fpcr |= (1ULL << 24);  // FZ bit
  asm volatile("msr fpcr, %0" ::"r"(fpcr));
#endif
}

// Converts a decibel adjustment to a linear gain factor.
inline float DbToGain(float db) { return std::pow(10.0, db / 20.0); }

}  // namespace

using namespace c74::min;  // NOLINT

class ResamplingModel {
  using SampleType = float;

 public:
  ResamplingModel(std::unique_ptr<NeuralAudio::NeuralModel> model,
                  double host_sample_rate, int max_block_size)
      : model_(std::move(model)),
        model_rate_(model_->GetSampleRate()),
        input_gain_(DbToGain(model_->GetRecommendedInputDBAdjustment())),
        output_gain_(DbToGain(model_->GetRecommendedOutputDBAdjustment())),
        resampler_(model_rate_) {
    block_process_fn_ = [this](SampleType** input, SampleType** output,
                               int num_frames) {
      ApplyGain(input[0], num_frames, input_gain_);
      model_->Process(input[0], output[0], num_frames);
      ApplyGain(output[0], num_frames, output_gain_);
    };
    Reset(host_sample_rate, max_block_size);
  }

  ~ResamplingModel() = default;

  void Reset(double host_sample_rate, int max_block_size) {
    host_rate_ = host_sample_rate;

    // Safety margin for the buffer NeuralAudio allocates internally at the
    // model's own rate.
    const double ratio =
        model_rate_ / (host_sample_rate > 0.0 ? host_sample_rate : 44100.0);
    const int max_model_rate_frames =
        static_cast<int>(std::ceil(max_block_size * ratio)) + 128;

    // NeuralAudio requires SetMaxAudioBufferSize() to be called before
    // processing. This is not real-time safe and must not be called from the
    // audio thread.
    model_->SetMaxAudioBufferSize(max_model_rate_frames);

    resampler_.Reset(host_sample_rate, max_block_size);
  }

  void Prewarm() { model_->Prewarm(); }

  bool HasQualityScaling() const { return model_->HasQualityScaling(); }

  float GetQualityScaleFactor() const {
    return model_->GetQualityScaleFactor();
  }

  // Not real-time safe if this results in switching to an uninitialized
  // sub-model (see IsQualityChangeRealtimeSafe()).
  void SetQualityScaleFactor(float scale) {
    model_->SetQualityScaleFactor(scale);
  }

  bool IsQualityChangeRealtimeSafe(float scale) const {
    return model_->IsQualityChangeRealtimeSafe(scale);
  }

  int GetLatencySamples() const {
    return NeedToResample() ? resampler_.GetLatency() : 0;
  }

  bool NeedToResample() const { return host_rate_ != model_rate_; }

  void Process(SampleType* input, SampleType* output, int num_frames) {
    SampleType* inputs[1] = {input};
    SampleType* outputs[1] = {output};
    if (NeedToResample()) {
      resampler_.ProcessBlock(inputs, outputs, num_frames, block_process_fn_);
    } else {
      block_process_fn_(inputs, outputs, num_frames);
    }
  }

 private:
  // Scales num_frames samples of buf by gain in place. No-op for unity gain.
  static void ApplyGain(SampleType* buf, int num_frames, float gain) {
    if (gain == 1.0) return;
    for (int i = 0; i < num_frames; ++i) buf[i] *= gain;
  }

  std::unique_ptr<NeuralAudio::NeuralModel> model_;
  double model_rate_;
  double host_rate_{0.0};
  float input_gain_{1.0};
  float output_gain_{1.0};

  // Single-channel, quality-12 Lanczos resampler from AudioDSPTools.
  dsp::ResamplingContainer<SampleType, 1, 12> resampler_;
  std::function<void(SampleType**, SampleType**, int)> block_process_fn_;
};

class neural_tilde : public object<neural_tilde>,  // NOLINT
                     public vector_operator<> {
  // clang-format off
 public:
  MIN_DESCRIPTION{"Neural~: Load and run NeuralAudio models in real time."};
  MIN_TAGS{"audio, effects, ampsim"};
  MIN_AUTHOR{"Alessandro Presta"};

  inlet<> inlet_audio_{
      this, "(signal) Audio in + load / clear / prewarm / bang", "signal"};

  outlet<> outlet_audio_{this, "(signal) Audio out", "signal"};
  outlet<> outlet_status_{
      this,
      "Status: loaded <path> / latency <n> / quality_supported <0|1> / "
      "queued <path> / cleared / error <msg>"};

  explicit neural_tilde(const atoms& args = {}) {
    // Optional creation argument: [neural~ /path/to/model]
    if (!args.empty() && args[0].type() == message_type::symbol_argument) {
      const std::string path = symbol{args[0]};
      if (!path.empty()) load_(args);
    }
  }

  ~neural_tilde() override {
    // Ensure the load thread finishes before any members are destroyed.
    if (load_thread_.joinable()) load_thread_.join();
  }

  message<> dspsetup{this, "dspsetup", MIN_FUNCTION {
    const double sr = args[0];
    const int block_size = static_cast<int>(args[1]);

    const bool rate_changed = (sr != sample_rate_);
    sample_rate_ = sr;
    max_buffer_size_ = block_size;
    AllocBuffers(block_size);

    // If a model was parsed before the engine was ready, wrap it now.
    std::unique_ptr<NeuralAudio::NeuralModel> deferred_model;
    std::string deferred_path;
    {
      std::lock_guard<std::mutex> lock(swap_mutex_);
      if (pending_raw_model_) {
        deferred_model = std::move(pending_raw_model_);
        deferred_path = pending_raw_path_;
        pending_raw_path_.clear();
      }
    }
    if (deferred_model) {
      WrapAndStage(std::move(deferred_model), deferred_path, sr, block_size);
      return {};  // skip rate-change reload (the model is brand new)
    }

    // On sample-rate change, reload the model so ResamplingModel can
    // rebuild its resamplers at the new ratio.
    if (rate_changed && !current_path_.empty() && !loading_.exchange(true)) {
      if (load_thread_.joinable()) load_thread_.join();
      std::string snap;
      {
        std::lock_guard<std::mutex> lock(swap_mutex_);
        snap = current_path_;
      }
      load_thread_ = std::thread([this, snap]() { DoLoad(snap); });
    }
    return {};
  }};

  message<> load_{
      this, "load",
      "Load a model file. Accepts absolute or relative paths; "
      "spaces in filenames "
      "are handled. Relative paths are resolved against the "
      "patcher's directory first, "
      "then Max's file-search path.",
      MIN_FUNCTION {
        if (args.empty()) {
          cerr << "load requires a path argument" << endl;
          return {};
        }

        // Rejoin all atoms into a single path string (handles spaces in
        // filenames).
        std::string raw_path;
        for (size_t i = 0; i < args.size(); ++i) {
          if (i > 0) raw_path += " ";
          if (args[i].type() == message_type::symbol_argument)
            raw_path += std::string(symbol{args[i]});
        }

        if (raw_path.empty()) {
          cerr << "load requires a path argument" << endl;
          return {};
        }

        if (loading_.load()) {
          cwarn << "already loading, please wait" << endl;
          return {};
        }

        // ResolvePath handles normalization, relative-path lookup, and
        // error logging.
        const std::string path = ResolvePath(raw_path);
        if (path.empty()) return {};

        loading_.store(true, std::memory_order_release);
        if (load_thread_.joinable()) load_thread_.join();
        load_thread_ = std::thread([this, path]() { DoLoad(path); });
        return {};
      }};

  message<> clear_{
      this, "clear", "Unload the current model.",
      MIN_FUNCTION {
        if (load_thread_.joinable()) load_thread_.join();
        {
          std::lock_guard<std::mutex> lock(swap_mutex_);
          pending_model_.reset();
          pending_raw_model_.reset();
          pending_raw_path_.clear();
          current_path_.clear();
        }
        model_pending_.store(true, std::memory_order_release);
        outlet_status_.send("cleared");
        return {};
      }};

  message<> prewarm_{
      this, "prewarm", "Prewarm the model (NAM-only).",
      MIN_FUNCTION {
        if (model_)
          model_->Prewarm();
        else
          cwarn << "no model loaded" << endl;
        return {};
      }};

  message<> bang_{
      this, "bang", "Report current model status to the status outlet.",
      MIN_FUNCTION {
        // Check whether a model is parked waiting for the engine to start.
        {
          std::lock_guard<std::mutex> lock(swap_mutex_);
          if (pending_raw_model_) {
            outlet_status_.send("queued", symbol{pending_raw_path_});
            return {};
          }
        }
        if (!model_) {
          outlet_status_.send("cleared");
          return {};
        }
        outlet_status_.send("loaded");
        return {};
      }};

  void operator()(audio_bundle input, audio_bundle output) override {
    // Swap in a newly loaded model (or null out the active one on clear),
    // and arm a short crossfade so the transition doesn't pop.
    if (model_pending_.load(std::memory_order_acquire)) {
      std::lock_guard<std::mutex> lock(swap_mutex_);
      // The chain that was active becomes the fade-out chain, run alongside
      // the new one until the crossfade finishes.
      // If a crossfade was already in flight, its fade-out chain is simply
      // dropped in favor of the (already partially faded-in) chain it was
      // fading into.
      fade_out_model_ = std::move(model_);
      model_ = std::move(pending_model_);
      if (model_) model_->Reset(sample_rate_, max_buffer_size_);
      model_pending_.store(false, std::memory_order_release);

      fade_samples_total_ = std::max(
          1, static_cast<int>(std::round(sample_rate_ * kCrossfadeSeconds)));
      fade_samples_remaining_ = fade_samples_total_;
    }

    disable_denormals();

    const sample* in = input.samples(0);
    sample* out = output.samples(0);
    const int block_size = vector_size();

    // Convert input to float working buffer.
    for (int i = 0; i < block_size; ++i)
      input_buf_[i] = static_cast<float>(in[i]);

    // Process the current chain (nullptr means pass-through).
    if (model_) {
      model_->Process(input_buf_.data(), output_buf_.data(), block_size);
    } else {
      std::copy(input_buf_.begin(), input_buf_.begin() + block_size,
                output_buf_.begin());
    }

    if (fade_samples_remaining_ > 0) {
      // Also run the outgoing chain on its own copy of the input, then
      // crossfade sample-by-sample.
      for (int i = 0; i < block_size; ++i)
        fade_input_buf_[i] = static_cast<float>(in[i]);

      if (fade_out_model_) {
        fade_out_model_->Process(fade_input_buf_.data(),
                                  fade_output_buf_.data(), block_size);
      } else {
        std::copy(fade_input_buf_.begin(),
                  fade_input_buf_.begin() + block_size,
                  fade_output_buf_.begin());
      }

      for (int i = 0; i < block_size && fade_samples_remaining_ > 0; ++i) {
        const float t = static_cast<float>(fade_samples_remaining_) /
                        static_cast<float>(fade_samples_total_);
        output_buf_[i] =
            (output_buf_[i] * (1.0 - t)) + (fade_output_buf_[i] * t);
        --fade_samples_remaining_;
      }

      if (fade_samples_remaining_ == 0) fade_out_model_.reset();
    }

    for (int i = 0; i < block_size; ++i)
      out[i] = static_cast<sample>(output_buf_[i]);
  }

 private:
  // Duration of the crossfade applied whenever the active model changes
  // (load, clear, or a rate-triggered reload), to avoid an audible pop from
  // an instantaneous swap.
  static constexpr double kCrossfadeSeconds = 0.25;  // 250 ms

  // Model state.
  std::unique_ptr<ResamplingModel> model_;
  std::unique_ptr<ResamplingModel> pending_model_;
  std::atomic<bool> model_pending_{false};
  std::mutex swap_mutex_;
  std::thread load_thread_;
  std::atomic<bool> loading_{false};
  std::string current_path_;

  // Outgoing chain kept alive just long enough to crossfade out of.
  std::unique_ptr<ResamplingModel> fade_out_model_;
  int fade_samples_total_{0};
  int fade_samples_remaining_{0};
  std::vector<float> fade_input_buf_;
  std::vector<float> fade_output_buf_;

  // Deferred-load state.
  std::unique_ptr<NeuralAudio::NeuralModel> pending_raw_model_;
  std::string pending_raw_path_;

  // Cached quality scale factor (0.0-1.0).
  double quality_value_{0.0};

  attribute<number> quality_{
      this, "quality", 0.0, range{0.0, 1.0},
      description{
          "Model quality scale factor: 0.0 (fastest / lowest quality) to "
          "1.0 (slowest / highest quality). Only affects models that "
          "support quality scaling (ie: slimmable NAM A2 models); ignored "
          "otherwise. The value persists and is applied to any model "
          "loaded afterward."},
      setter{MIN_FUNCTION {
        double v = std::clamp(static_cast<double>(args[0]), 0.0, 1.0);
        quality_value_ = v;

        std::lock_guard<std::mutex> lock(swap_mutex_);
        if (model_ && model_->HasQualityScaling()) {
          model_->SetQualityScaleFactor(static_cast<float>(v));
        }
        return {v};
      }},
      getter{[this]() -> atoms {
        std::lock_guard<std::mutex> lock(swap_mutex_);
        if (model_ && model_->HasQualityScaling())
          quality_value_ =
              static_cast<double>(model_->GetQualityScaleFactor());
        return {quality_value_};
      }}};

  // Audio buffers.
  double sample_rate_{0.0};
  int max_buffer_size_{0};
  std::vector<float> input_buf_;
  std::vector<float> output_buf_;

  // Deferred notifications.
  struct Notification {
    enum class Kind { kLoaded, kQueued, kError } kind;
    std::string text;
    long latency{0};
    bool has_quality_scaling{false};
  };

  std::mutex notify_mutex_;
  std::vector<Notification> notifications_;

  queue<> notify_queue_{this, MIN_FUNCTION {
    std::vector<Notification> pending;
    {
      std::lock_guard<std::mutex> lock(notify_mutex_);
      pending.swap(notifications_);
    }
    for (const auto& n : pending) {
      if (n.kind == Notification::Kind::kLoaded) {
        outlet_status_.send("loaded", symbol{n.text});
        outlet_status_.send("latency", n.latency);
        outlet_status_.send("quality_supported",
                             n.has_quality_scaling ? 1 : 0);
      } else if (n.kind == Notification::Kind::kQueued) {
        outlet_status_.send("queued", symbol{n.text});
      } else {
        outlet_status_.send("error", symbol{n.text});
      }
    }
    return {};
  }};

  void AllocBuffers(int n) {
    input_buf_.assign(n, 0.0);
    output_buf_.assign(n, 0.0);
    fade_input_buf_.assign(n, 0.0);
    fade_output_buf_.assign(n, 0.0);
  }

  // Returns true when a path string encodes an absolute location.
  static bool IsAbsolutePath(const std::string& path) {
    if (path.empty()) return false;
#if defined(_WIN32) || defined(_WIN64)
    if (path[0] == '\\' || path[0] == '/') return true;
    if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) &&
        path[1] == ':')
      return true;
    return false;
#else
    if (path[0] == '/') return true;

    // HFS volume syntax.
    const auto colon = path.find(':');
    const auto slash = path.find('/');
    return colon != std::string::npos &&
           (slash == std::string::npos || colon < slash);
#endif
  }

  // Returns the parent patcher's saved directory as a native filesystem path,
  // or an empty string when the patcher is unsaved.
  std::string GetPatcherDir() const {
    auto* patcher = reinterpret_cast<c74::max::t_object*>(
        c74::max::object_attr_getobj(const_cast<c74::max::t_object*>(maxobj()),
                                     c74::max::gensym("parentpatcher")));
    if (!patcher) return {};
    c74::max::t_symbol* fp =
        c74::max::object_attr_getsym(patcher, c74::max::gensym("filepath"));
    if (!fp || !fp->s_name || fp->s_name[0] == '\0') return {};
    return fp->s_name;
  }

  // Resolve a raw path atom string to an absolute native filesystem path.
  // Returns an empty string (and logs to cerr) on failure.
  std::string ResolvePath(const std::string& raw_path) const {
    std::string path = raw_path;

    // Strip surrounding quotes produced by Max's opendialog.
    if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
      path = path.substr(1, path.size() - 2);

    if (path.empty()) return {};

    if (IsAbsolutePath(path)) {
#if defined(__APPLE__)
      // Convert HFS volume syntax to a /Volumes/... path.
      // Paths already starting with '/' are left untouched.
      if (path[0] != '/') {
        const auto colon = path.find(':');
        if (colon != std::string::npos) {
          std::string vol = path.substr(0, colon);
          std::string rest = path.substr(colon + 1);
          if (!rest.empty() && rest[0] != '/') rest = "/" + rest;
          path = "/Volumes/" + vol + rest;
        }
      }
#endif
      if (!std::filesystem::exists(path)) {
        std::cerr << "cannot find '" << path << "'\n";
        return {};
      }
      return path;
    }

    // Parent patcher directory.
    const std::string pdir = GetPatcherDir();
    if (!pdir.empty()) {
      auto candidate = (std::filesystem::path(pdir) / path).lexically_normal();
      if (std::filesystem::exists(candidate)) return candidate.string();
    }

    // Max's file-search path.
    {
      char name_buf[c74::max::MAX_PATH_CHARS];
      std::strncpy(name_buf, path.c_str(), sizeof(name_buf) - 1);
      name_buf[sizeof(name_buf) - 1] = '\0';

      short found_vol = 0;
      c74::max::t_fourcc found_type = 0;
      c74::max::t_fourcc type_list[] = {0};  // 0 = any file type

      if (c74::max::locatefile_extended(name_buf, &found_vol, &found_type,
                                        type_list, 1) == 0) {
        char full[c74::max::MAX_PATH_CHARS];
        c74::max::path_toabsolutesystempath(found_vol, name_buf, full);
        return full;
      }
    }

    std::cerr << "cannot find '" << raw_path << "'\n";
    return {};
  }

  void PostNotification(Notification n) {
    {
      std::lock_guard<std::mutex> lock(notify_mutex_);
      notifications_.push_back(std::move(n));
    }
    notify_queue_();
  }

  // Wrap a validated NeuralAudio::NeuralModel in ResamplingModel and
  // stage it for the audio thread.
  void WrapAndStage(std::unique_ptr<NeuralAudio::NeuralModel> raw_model,
                    const std::string& path, double host_rate,
                    int host_block_size) {
    std::unique_ptr<ResamplingModel> new_model;
    try {
      new_model = std::make_unique<ResamplingModel>(std::move(raw_model),
                                                    host_rate, host_block_size);
    } catch (const std::exception& e) {
      PostNotification({Notification::Kind::kError, e.what()});
      return;
    } catch (...) {
      PostNotification(
          {Notification::Kind::kError, "unknown error wrapping model"});
      return;
    }

    long latency_samples = 0;
    bool has_quality_scaling = false;
    {
      std::lock_guard<std::mutex> lock(swap_mutex_);
      pending_model_ = std::move(new_model);
      current_path_ = path;
      pending_raw_model_.reset();
      pending_raw_path_.clear();
      if (pending_model_ && pending_model_->NeedToResample())
        latency_samples = pending_model_->GetLatencySamples();
      // Carry over the persisted quality dial to the newly loaded model.
      if (pending_model_ && pending_model_->HasQualityScaling()) {
        has_quality_scaling = true;
        pending_model_->SetQualityScaleFactor(
            static_cast<float>(quality_value_));
      }
    }
    model_pending_.store(true, std::memory_order_release);
    PostNotification({Notification::Kind::kLoaded, path, latency_samples,
                      has_quality_scaling});
  }

  void DoLoad(const std::string& path) {
    std::unique_ptr<NeuralAudio::NeuralModel> raw_model;
    try {
      NeuralAudio::NeuralModelLoader loader;
      // Prewarming is deferred to the explicit "prewarm" message rather than
      // happening automatically here.
      raw_model.reset(loader.CreateFromFile(path, /*doPrewarm=*/false));
    } catch (const std::exception& e) {
      PostNotification({Notification::Kind::kError, e.what()});
      loading_.store(false, std::memory_order_release);
      return;
    } catch (...) {
      PostNotification({Notification::Kind::kError,
                        "unknown error loading model (wrong file type?)"});
      loading_.store(false, std::memory_order_release);
      return;
    }

    if (!raw_model) {
      PostNotification(
          {Notification::Kind::kError,
           "NeuralAudio could not load model (unsupported format?)"});
      loading_.store(false, std::memory_order_release);
      return;
    }

    // Snapshot audio specs under the lock.
    double host_rate = 0.0;
    int host_block_size = 0;
    {
      std::lock_guard<std::mutex> lock(swap_mutex_);
      host_rate = sample_rate_;
      host_block_size = max_buffer_size_;
    }

    // Engine not yet ready. Park the validated model.
    if (host_rate <= 0.0 || host_block_size <= 0) {
      {
        std::lock_guard<std::mutex> lock(swap_mutex_);
        // Discard any previously parked model and take ownership of the new
        // one.
        pending_raw_model_ = std::move(raw_model);
        pending_raw_path_ = path;
        current_path_ = path;
      }
      PostNotification({Notification::Kind::kQueued, path});
      loading_.store(false, std::memory_order_release);
      return;
    }

    // Engine is ready. Wrap and stage immediately.
    WrapAndStage(std::move(raw_model), path, host_rate, host_block_size);
    loading_.store(false, std::memory_order_release);
  }

// clang-format on
}
;

MIN_EXTERNAL(neural_tilde);
