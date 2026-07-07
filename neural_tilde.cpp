// Max/MSP external: neural~ — loads and runs NAM models in real time.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "c74_min.h"

// NeuralAmpModelerCore:
#include "activations.h"  // nam::activations::Activation::enable_fast_tanh()
#include "dsp.h"          // nam::DSP, NAM_SAMPLE
#include "get_dsp.h"      // nam::get_dsp(), nam::DspLoadOptions
#include "slimmable.h"    // nam::SlimmableModel

#ifndef DEFAULT_BLOCK_SIZE
#define DEFAULT_BLOCK_SIZE 512
#endif

#ifdef PI
#undef PI
#endif
namespace iplug {
constexpr double PI = 3.14159265358979323846;
}  // namespace iplug

#include "ResamplingContainer/ResamplingContainer.h"

using namespace c74::min;

// Flush denormals to zero to avoid CPU stalls.
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || \
    defined(_M_IX86)
#include <xmmintrin.h>
#endif

namespace {

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

double GetNAMSampleRate(const std::unique_ptr<nam::DSP>& model) {
  const double reported = model->GetExpectedSampleRate();
  return reported <= 0.0 ? 48000.0 : reported;
}

inline double DBToAmp(double db) { return std::pow(10.0, db / 20.0); }

}  // namespace

class ResamplingNAM {
 public:
  // Construct and immediately reset at the given host rate / block size.
  // nam must already be validated (1-in / 1-out, non-null).
  ResamplingNAM(std::unique_ptr<nam::DSP> encapsulated, double host_sample_rate,
                int max_block_size)
      : dsp_(std::move(encapsulated)),
        model_rate_(GetNAMSampleRate(dsp_)),
        resampler_(model_rate_) {
    // Wire the encapsulated model's process() into the resampler callback.
    block_process_fn_ = [&](NAM_SAMPLE** input, NAM_SAMPLE** output,
                            int nFrames) {
      dsp_->process(input, output, nFrames);
    };

    // Mirror loudness / level metadata from the wrapped model.
    if (dsp_->HasLoudness()) {
      has_loudness_ = true;
      loudness_ = dsp_->GetLoudness();
    }
    if (dsp_->HasInputLevel()) {
      has_input_level_ = true;
      input_level_ = dsp_->GetInputLevel();
    }
    if (dsp_->HasOutputLevel()) {
      has_output_level_ = true;
      output_level_ = dsp_->GetOutputLevel();
    }

    Reset(host_sample_rate, max_block_size);
  }

  ~ResamplingNAM() = default;

  // Reset at a (potentially new) host sample rate and max block size.
  // Not real-time safe; do not call from the audio thread.
  void Reset(double host_sample_rate, int max_block_size) {
    host_rate_ = host_sample_rate;

    // Tell the resampler about the new external rate and block size.
    resampler_.Reset(host_sample_rate, max_block_size);

    // Allocate / prewarm the encapsulated model at its own native rate.
    const double up_ratio =
        model_rate_ > 0.0 ? host_sample_rate / model_rate_ : 1.0;
    const int max_enc_block_size =
        static_cast<int>(std::ceil(static_cast<double>(max_block_size) /
                                   std::max(up_ratio, 1e-9))) +
        1;
    dsp_->Reset(model_rate_, max_enc_block_size);
  }

  void Prewarm() { dsp_->prewarm(); }

  // Process one block.  input/output are arrays of channel pointers (1 each).
  void Process(NAM_SAMPLE** input, NAM_SAMPLE** output, int num_frames) {
    if (NeedToResample()) {
      resampler_.ProcessBlock(input, output, num_frames, block_process_fn_);
    } else {
      dsp_->process(input, output, num_frames);
    }
  }

  // Latency introduced by the resampling filter pair, in host-rate samples.
  int GetLatency() const {
    return NeedToResample() ? resampler_.GetLatency() : 0;
  }

  bool HasLoudness() const { return has_loudness_; }

  double GetLoudness() const { return loudness_; }

  bool HasInputLevel() const { return has_input_level_; }

  double GetInputLevel() const { return input_level_; }

  bool HasOutputLevel() const { return has_output_level_; }

  double GetOutputLevel() const { return output_level_; }

  double GetModelSampleRate() const { return model_rate_; }

  bool NeedToResample() const { return host_rate_ != model_rate_; }

  // Returns non-null only when the wrapped model supports dynamic size
  // reduction.
  nam::SlimmableModel* GetSlimmableModel() {
    return dynamic_cast<nam::SlimmableModel*>(dsp_.get());
  }

  const nam::SlimmableModel* GetSlimmableModel() const {
    return dynamic_cast<const nam::SlimmableModel*>(dsp_.get());
  }

 private:
  std::unique_ptr<nam::DSP> dsp_;
  double model_rate_;
  double host_rate_{0.0};

  // Single-channel, quality-12 Lanczos resampler from AudioDSPTools.
  dsp::ResamplingContainer<NAM_SAMPLE, 1, 12> resampler_;
  std::function<void(NAM_SAMPLE**, NAM_SAMPLE**, int)> block_process_fn_;

  bool has_loudness_{false};
  double loudness_{0.0};
  bool has_input_level_{false};
  double input_level_{0.0};
  bool has_output_level_{false};
  double output_level_{0.0};
};

class neural_tilde : public object<neural_tilde>, public vector_operator<> {
 public:
  MIN_DESCRIPTION{"neural~: Load and run NAM (.nam) models in real time."};
  MIN_TAGS{"audio, effects, ampsim"};
  MIN_AUTHOR{"Alessandro Presta"};

  inlet<> inlet_audio_{
      this, "(signal) Audio in + load / clear / prewarm / bang", "signal"};
  outlet<> outlet_audio_{this, "(signal) Audio out", "signal"};
  outlet<> outlet_status_{
      this,
      "Status: loaded <path> / latency <n> / loudness <dB> / "
      "input_calibration <dB> / output_calibration <dB> / slimmable "
      "<0|1> / queued <path> / cleared / error <msg>"};

  // clang-format off
  attribute<number> slim{
      this, "slim", 0.0, range{0.0, 1.0},
      description{"A2 model quality/size, 0-1 (0 = smallest/fastest, 1 = full "
                  "size/quality)."},
      setter{MIN_FUNCTION{
        const double v = std::clamp(static_cast<double>(args[0]), 0.0, 1.0);
        slim_value_.store(v, std::memory_order_relaxed);
        slim_dirty_.store(true, std::memory_order_release);
        return {v};
      }}};

  attribute<symbol> output_mode{
      this, "output_mode", "normalized", range{"raw", "normalized", "calibrated"},
      description{
        "How the output level is auto-compensated: "
        "'raw' applies no compensation; "
        "'normalized' targets -18 dB loudness using the model's embedded"
        " loudness metadata; "
        "'calibrated' matches the model's embedded output level to"
        " 'input_calibration_level'"},
      setter{MIN_FUNCTION{
        const std::string v = std::string(symbol(args[0]));
        OutputMode mode = OutputMode::kNormalized;
        if (v == "raw") {
          mode = OutputMode::kRaw;
        } else if (v == "calibrated") {
          mode = OutputMode::kCalibrated;
        } else {
          mode = OutputMode::kNormalized;
        }
        output_mode_.store(mode, std::memory_order_relaxed);
        return {args[0]};
      }}};

  attribute<bool> calibrate_input{
      this, "calibrate_input", false,
      description{
          "When on, offsets the input gain so that a signal at "
          "'input_calibration_level' dBu at the input hits the model's own "
          "calibrated input level."},
      setter{MIN_FUNCTION{
        const bool v = static_cast<bool>(args[0]);
        calibrate_input_.store(v, std::memory_order_relaxed);
        return {v};
      }}};

  attribute<number> input_calibration_level{
      this, "input_calibration_level", 12.0, range{-60.0, 60.0},
      description{
          "Reference level in dBu."},
      setter{MIN_FUNCTION{
        const double v = std::clamp(static_cast<double>(args[0]), -60.0, 60.0);
        input_calibration_level_.store(v, std::memory_order_relaxed);
        return {v};
      }}};
// clang-format on

// clang-format off
  message<> dspsetup{
      this, "dspsetup",
      MIN_FUNCTION{
        const double sr = args[0];
        const int block_size = static_cast<int>(args[1]);

        const bool rate_changed = (sr != sample_rate_);
        const bool size_changed = (block_size != max_buffer_size_);
        sample_rate_ = sr;
        max_buffer_size_ = block_size;
        AllocBuffers(block_size);

        // If a NAM was fully parsed before the audio engine started, wrap it now.
        std::unique_ptr<nam::DSP> deferred_nam;
        std::string deferred_path;
        {
          std::lock_guard<std::mutex> lock(swap_mutex_);
          if (pending_nam_) {
            deferred_nam = std::move(pending_nam_);
            deferred_path = pending_nam_path_;
            pending_nam_path_.clear();
          }
        }
        if (deferred_nam) {
          // Freshly wrapped at the current sr/block_size already.
          WrapAndStage(std::move(deferred_nam), deferred_path, sr, block_size);
          return {};
        }

        // On sample-rate change: reload from disk so ResamplingNAM rebuilds its
        // resampler at the new ratio.
        if (rate_changed && !current_path_.empty() && !loading_.exchange(true)) {
          if (load_thread_.joinable()) load_thread_.join();
          std::string snap;
          {
            std::lock_guard<std::mutex> lock(swap_mutex_);
            snap = current_path_;
          }
          load_thread_ = std::thread([this, snap]() { DoLoad(snap); });
          return {};
        }

        // Buffer-size-only change.
        if (size_changed && !loading_.load()) {
          std::lock_guard<std::mutex> lock(swap_mutex_);
          if (pending_model_) {
            pending_model_->Reset(sr, block_size);
          } else if (model_) {
            model_->Reset(sr, block_size);
          }
        }

        return {};
      }};
// clang-format on

// clang-format off
  message<> load_{
      this, "load",
      "Load a .nam model file.  Accepts absolute or relative paths; "
      "spaces in filenames are handled.  Relative paths are resolved "
      "against the patcher's directory first, then Max's file-search path.",
      MIN_FUNCTION{
        if (args.empty()) {
          cerr << "load requires a path argument" << endl;
          return {};
        }

        // Rejoin all atoms into a single path string (handles spaces in filenames).
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
          cwarn << "already loading — please wait" << endl;
          return {};
        }

        const std::string path = ResolvePath(raw_path);
        if (path.empty()) return {};

        loading_.store(true, std::memory_order_release);
        if (load_thread_.joinable()) load_thread_.join();
        load_thread_ = std::thread([this, path]() { DoLoad(path); });
        return {};
      }};
// clang-format on

// clang-format off
  message<> clear_{
      this, "clear", "Unload the current model.",
      MIN_FUNCTION{
        if (load_thread_.joinable()) load_thread_.join();
        {
          std::lock_guard<std::mutex> lock(swap_mutex_);
          pending_model_.reset();
          pending_nam_.reset();
          pending_nam_path_.clear();
          current_path_.clear();
        }
        model_pending_.store(true, std::memory_order_release);
        outlet_status_.send("cleared");
        return {};
      }};
// clang-format on

// clang-format off
  message<> prewarm_{
      this, "prewarm", "Prewarm the loaded NAM model.",
      MIN_FUNCTION{
        if (model_) {
          model_->Prewarm();
        } else {
          cwarn << "no model loaded" << endl;
        }
        return {};
      }};
// clang-format on

// clang-format off
  message<> bang_{
      this, "bang", "Report current model status.",
      MIN_FUNCTION{
        {
          std::lock_guard<std::mutex> lock(swap_mutex_);
          if (pending_nam_) {
            outlet_status_.send("queued", symbol{pending_nam_path_});
            return {};
          }
        }
        if (!model_) {
          outlet_status_.send("cleared");
          return {};
        }

        if (model_->HasLoudness()) {
          outlet_status_.send("loudness", model_->GetLoudness());
        } else {
          outlet_status_.send("loaded");
        }
        outlet_status_.send(
            "input_calibration",
            model_->HasInputLevel() ? model_->GetInputLevel() : 0.0);
        outlet_status_.send(
            "output_calibration",
            model_->HasOutputLevel() ? model_->GetOutputLevel() : 0.0);
        return {};
      }};
// clang-format on

explicit neural_tilde(const atoms& args = {}) {
  nam::activations::Activation::enable_fast_tanh();

  if (!args.empty() && args[0].type() == message_type::symbol_argument) {
    const std::string path = symbol{args[0]};
    if (!path.empty()) load_(args);
  }
}

~neural_tilde() {
  if (load_thread_.joinable()) load_thread_.join();
}

void operator()(audio_bundle input, audio_bundle output) override {
  // Swap in a newly loaded model (or null out on clear).
  bool just_swapped = false;
  if (model_pending_.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> lock(swap_mutex_);
    model_ = std::move(pending_model_);  // may be null (clear case)
    model_pending_.store(false, std::memory_order_release);
    just_swapped = true;
  }

  // Apply the current Slim setting.
  if (model_ && (just_swapped ||
                 slim_dirty_.exchange(false, std::memory_order_acq_rel))) {
    if (nam::SlimmableModel* s = model_->GetSlimmableModel())
      s->SetSlimmableSize(slim_value_.load(std::memory_order_relaxed));
  }

  disable_denormals();

  const sample* in = input.samples(0);
  sample* out = output.samples(0);
  const int block_size = vector_size();

  // Gain staging.
  const double input_gain = DBToAmp(ComputeInputGainDB());
  const double output_gain = DBToAmp(ComputeOutputGainDB());

  // Convert double->NAM_SAMPLE working buffers, applying the input gain.
  for (int i = 0; i < block_size; ++i)
    input_buf_[i] = static_cast<NAM_SAMPLE>(in[i] * input_gain);

  if (model_) {
    NAM_SAMPLE* in_ptr = input_buf_.data();
    NAM_SAMPLE* out_ptr = output_buf_.data();
    NAM_SAMPLE* in_ptrs[1] = {in_ptr};
    NAM_SAMPLE* out_ptrs[1] = {out_ptr};
    model_->Process(in_ptrs, out_ptrs, block_size);
    for (int i = 0; i < block_size; ++i)
      out[i] = static_cast<sample>(output_buf_[i] * output_gain);
  } else {
    // Pass-through when no model is loaded.
    for (int i = 0; i < block_size; ++i)
      out[i] = static_cast<sample>(input_buf_[i] * output_gain);
  }
}

private:
double ComputeInputGainDB() const {
  double db = input_level_db_.load(std::memory_order_relaxed);
  if (model_ && model_->HasInputLevel() &&
      calibrate_input_.load(std::memory_order_relaxed)) {
    db += input_calibration_level_.load(std::memory_order_relaxed) -
          model_->GetInputLevel();
  }
  return db;
}

double ComputeOutputGainDB() const {
  double db = output_level_db_.load(std::memory_order_relaxed);
  if (model_) {
    switch (output_mode_.load(std::memory_order_relaxed)) {
      case OutputMode::kNormalized:
        if (model_->HasLoudness()) {
          constexpr double kTargetLoudness = -18.0;
          db += (kTargetLoudness - model_->GetLoudness());
        }
        break;
      case OutputMode::kCalibrated:
        if (model_->HasOutputLevel()) {
          const double input_level =
              input_calibration_level_.load(std::memory_order_relaxed);
          const double output_level = model_->GetOutputLevel();
          db += (output_level - input_level);
        }
        break;
      case OutputMode::kRaw:
      default:
        break;
    }
  }
  return db;
}

// Model state.
std::unique_ptr<ResamplingNAM> model_;
std::unique_ptr<ResamplingNAM> pending_model_;
std::atomic<bool> model_pending_{false};
std::mutex swap_mutex_;
std::thread load_thread_;
std::atomic<bool> loading_{false};
std::string current_path_;
std::atomic<double> slim_value_{0.0};
std::atomic<bool> slim_dirty_{false};

// Level / calibration state.
std::atomic<double> input_level_db_{0.0};
std::atomic<double> output_level_db_{0.0};
// 0 = raw, 1 = normalized, 2 = calibrated.
enum class OutputMode { kRaw = 0, kNormalized = 1, kCalibrated = 2 };
std::atomic<OutputMode> output_mode_{OutputMode::kNormalized};
std::atomic<bool> calibrate_input_{false};
std::atomic<double> input_calibration_level_{12.0};

// Park a fully-parsed nam::DSP when the audio engine isn't ready yet.
std::unique_ptr<nam::DSP> pending_nam_;
std::string pending_nam_path_;

// Audio buffers.
double sample_rate_{0.0};
int max_buffer_size_{0};
std::vector<NAM_SAMPLE> input_buf_;
std::vector<NAM_SAMPLE> output_buf_;

struct Notification {
  enum class Kind { kLoaded, kQueued, kError } kind;
  std::string text;
  long latency{0};
  bool has_loudness{false};
  double loudness{0.0};
  bool is_slimmable{false};
  bool has_input_level{false};
  double input_level{0.0};
  bool has_output_level{false};
  double output_level{0.0};
};

std::mutex notify_mutex_;
std::vector<Notification> notifications_;

queue<> notify_queue_{this, MIN_FUNCTION{std::vector<Notification> pending;
{
  std::lock_guard<std::mutex> lock(notify_mutex_);
  pending.swap(notifications_);
}
for (const auto& n : pending) {
  if (n.kind == Notification::Kind::kLoaded) {
    outlet_status_.send("loaded", symbol{n.text});
    outlet_status_.send("latency", n.latency);
    outlet_status_.send("loudness", n.has_loudness ? n.loudness : 0.0);
    outlet_status_.send("slimmable", n.is_slimmable ? 1 : 0);
    outlet_status_.send("input_calibration",
                        n.has_input_level ? n.input_level : 0.0);
    outlet_status_.send("output_calibration",
                        n.has_output_level ? n.output_level : 0.0);
  } else if (n.kind == Notification::Kind::kQueued) {
    outlet_status_.send("queued", symbol{n.text});
  } else {
    outlet_status_.send("error", symbol{n.text});
  }
}
return {};
}
}
;

void AllocBuffers(int n) {
  input_buf_.assign(n, NAM_SAMPLE{0});
  output_buf_.assign(n, NAM_SAMPLE{0});
}

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
  const auto colon = path.find(':');
  const auto slash = path.find('/');
  return colon != std::string::npos &&
         (slash == std::string::npos || colon < slash);
#endif
}

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

std::string ResolvePath(const std::string& raw_path) const {
  std::string path = raw_path;

  // Strip surrounding quotes produced by Max's opendialog.
  if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
    path = path.substr(1, path.size() - 2);
  if (path.empty()) return {};

  if (IsAbsolutePath(path)) {
#if defined(__APPLE__)
    // Convert HFS volume syntax (VolumeName:...) to /Volumes/VolumeName/...
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
      std::cerr << "neural~: cannot find '" << path << "'\n";
      return {};
    }
    return path;
  }

  // Try parent patcher's directory first.
  const std::string pdir = GetPatcherDir();
  if (!pdir.empty()) {
    auto candidate = (std::filesystem::path(pdir) / path).lexically_normal();
    if (std::filesystem::exists(candidate)) return candidate.string();
  }

  // Fall back to Max's file-search path.
  {
    char name_buf[c74::max::MAX_PATH_CHARS];
    std::strncpy(name_buf, path.c_str(), sizeof(name_buf) - 1);
    name_buf[sizeof(name_buf) - 1] = '\0';

    short found_vol = 0;
    c74::max::t_fourcc found_type = 0;
    c74::max::t_fourcc type_list[] = {0};  // 0 = any type

    if (c74::max::locatefile_extended(name_buf, &found_vol, &found_type,
                                      type_list, 1) == 0) {
      char full[c74::max::MAX_PATH_CHARS];
      c74::max::path_toabsolutesystempath(found_vol, name_buf, full);
      return full;
    }
  }

  std::cerr << "neural~: cannot find '" << raw_path << "'\n";
  return {};
}

void PostNotification(Notification n) {
  {
    std::lock_guard<std::mutex> lock(notify_mutex_);
    notifications_.push_back(std::move(n));
  }
  notify_queue_();
}

// Wrap a validated nam::DSP in ResamplingNAM and stage it for the audio thread.
void WrapAndStage(std::unique_ptr<nam::DSP> nam, const std::string& path,
                  double host_rate, int host_block_size) {
  std::unique_ptr<ResamplingNAM> new_model;
  try {
    new_model = std::make_unique<ResamplingNAM>(std::move(nam), host_rate,
                                                host_block_size);
  } catch (const std::exception& e) {
    PostNotification({Notification::Kind::kError, e.what()});
    return;
  } catch (...) {
    PostNotification(
        {Notification::Kind::kError, "unknown error wrapping NAM model"});
    return;
  }

  // Apply the current Slim setting before staging.
  nam::SlimmableModel* slimmable = new_model->GetSlimmableModel();
  if (slimmable)
    slimmable->SetSlimmableSize(slim_value_.load(std::memory_order_relaxed));

  // Capture metadata before moving ownership into the pending slot.
  const long latency_samples = new_model->GetLatency();
  const bool has_loudness = new_model->HasLoudness();
  const double loudness_db = has_loudness ? new_model->GetLoudness() : 0.0;
  const bool is_slimmable = slimmable != nullptr;
  const bool has_input_level = new_model->HasInputLevel();
  const double input_level_db =
      has_input_level ? new_model->GetInputLevel() : 0.0;
  const bool has_output_level = new_model->HasOutputLevel();
  const double output_level_db =
      has_output_level ? new_model->GetOutputLevel() : 0.0;

  {
    std::lock_guard<std::mutex> lock(swap_mutex_);
    pending_model_ = std::move(new_model);
    current_path_ = path;
    pending_nam_.reset();
    pending_nam_path_.clear();
  }
  model_pending_.store(true, std::memory_order_release);
  PostNotification({Notification::Kind::kLoaded, path, latency_samples,
                    has_loudness, loudness_db, is_slimmable, has_input_level,
                    input_level_db, has_output_level, output_level_db});
}

void DoLoad(const std::string& path) {
  std::unique_ptr<nam::DSP> nam;
  try {
    nam::DspLoadOptions opts;
    opts.prewarm = false;

    nam = nam::get_dsp(std::filesystem::u8path(path), opts);
  } catch (const std::exception& e) {
    PostNotification({Notification::Kind::kError, e.what()});
    loading_.store(false, std::memory_order_release);
    return;
  } catch (...) {
    PostNotification(
        {Notification::Kind::kError, "unknown error loading NAM model"});
    loading_.store(false, std::memory_order_release);
    return;
  }

  if (!nam) {
    PostNotification({Notification::Kind::kError,
                      "get_dsp() returned null — unsupported .nam format?"});
    loading_.store(false, std::memory_order_release);
    return;
  }

  // NAM models must be mono (1-in / 1-out).
  if (nam->NumInputChannels() != 1 || nam->NumOutputChannels() != 1) {
    PostNotification(
        {Notification::Kind::kError,
         "NAM model must be mono (1 in, 1 out); this model has " +
             std::to_string(nam->NumInputChannels()) + " input(s) and " +
             std::to_string(nam->NumOutputChannels()) + " output(s)"});
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

  // Audio engine not yet initialised; park the validated model.
  if (host_rate <= 0.0 || host_block_size <= 0) {
    {
      std::lock_guard<std::mutex> lock(swap_mutex_);
      pending_nam_ = std::move(nam);
      pending_nam_path_ = path;
      current_path_ = path;
    }
    PostNotification({Notification::Kind::kQueued, path});
    loading_.store(false, std::memory_order_release);
    return;
  }

  WrapAndStage(std::move(nam), path, host_rate, host_block_size);
  loading_.store(false, std::memory_order_release);
}
}
;

MIN_EXTERNAL(neural_tilde);
