// Max/MSP external for NeuralAudio backend.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "c74_min.h"

using namespace c74::min;

#include "NeuralModel.h"

// LanczosResampler uses iplug::PI but the Max SDK also defines PI.
// Undefine first and then provide the missing iplug::PI.
#ifdef PI
#undef PI
#endif

namespace iplug {
constexpr double PI = 3.14159265358979323846;
}

#include "LanczosResampler.h"

// Flush denormal floats to zero to avoid CPU stalls.
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || \
    defined(_M_IX86)
#include <xmmintrin.h>
#endif
static inline void disable_denormals() {
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

class ResamplingModel {
  using SampleType = float;
  using Resampler = dsp::LanczosResampler<SampleType, 1, 12>;

 public:
  ResamplingModel(std::unique_ptr<NeuralAudio::NeuralModel> model,
                  double host_sample_rate, int max_block_size)
      : model_(std::move(model)),
        model_rate_(model_->GetSampleRate()),
        host_rate_(host_sample_rate) {
    DoReset(host_sample_rate, max_block_size);
  }

  ~ResamplingModel() = default;

  void Reset(double host_sample_rate, int max_block_size) {
    DoReset(host_sample_rate, max_block_size);
  }

  void Prewarm() { model_->Prewarm(); }

  bool HasLoudness() const {
    return model_->GetRecommendedOutputDBAdjustment() != 0.0f;
  }

  double GetLoudness() const {
    return static_cast<double>(model_->GetRecommendedOutputDBAdjustment());
  }

  int GetLatencySamples() const { return latency_; }

  bool NeedToResample() const { return host_rate_ != model_rate_; }

  void Process(SampleType* input, SampleType* output, int num_frames) {
    if (!NeedToResample()) {
      model_->Process(input, output, num_frames);
      return;
    }

    SampleType* inputs[1] = {input};
    to_model_resampler_->PushBlock(inputs, num_frames);

    // Process in chunks while the upsampler has data ready.
    while (to_model_resampler_->GetNumSamplesRequiredFor(1) == 0) {
      SampleType* resample_inputs[1] = {resample_in_.data()};
      SampleType* resample_outputs[1] = {resample_out_.data()};

      size_t to_pop =
          std::min(static_cast<size_t>(max_model_rate_frames_), size_t{64});
      size_t got = to_model_resampler_->PopBlock(resample_inputs, to_pop);

      if (got > 0) {
        model_->Process(resample_in_.data(), resample_out_.data(),
                        static_cast<int>(got));
        from_model_resampler_->PushBlock(resample_outputs, got);
      } else {
        break;
      }
    }

    SampleType* outputs[1] = {output};
    size_t popped = from_model_resampler_->PopBlock(outputs, num_frames);

    // Fill fractional gaps with last sample to avoid clicks.
    if (static_cast<int>(popped) < num_frames) {
      SampleType fill = popped > 0 ? output[popped - 1] : SampleType{0};
      std::fill(output + popped, output + num_frames, fill);
    }

    to_model_resampler_->RenormalizePhases();
    from_model_resampler_->RenormalizePhases();
  }

 private:
  void DoReset(double host_sample_rate, int max_block_size) {
    host_rate_ = host_sample_rate;
    const double ratio =
        model_rate_ / (host_sample_rate > 0.0 ? host_sample_rate : 44100.0);

    // Safety margin for high-rate buffers.
    max_model_rate_frames_ =
        static_cast<int>(std::ceil(max_block_size * ratio)) + 128;
    resample_in_.assign(max_model_rate_frames_, SampleType{0});
    resample_out_.assign(max_model_rate_frames_, SampleType{0});

    // NeuralAudio requires SetMaxAudioBufferSize() to be called before
    // processing. This is not real-time safe and must not be called from the
    // audio thread.
    model_->SetMaxAudioBufferSize(max_model_rate_frames_);

    if (NeedToResample()) {
      to_model_resampler_ =
          std::make_unique<Resampler>(host_rate_, model_rate_);
      from_model_resampler_ =
          std::make_unique<Resampler>(model_rate_, host_rate_);

      // Calculate the minimum latency for round-trip resampling.
      const int model_rate_required =
          static_cast<int>(from_model_resampler_->GetNumSamplesRequiredFor(1));
      const int base_latency = static_cast<int>(
          to_model_resampler_->GetNumSamplesRequiredFor(model_rate_required));
      latency_ = base_latency;

      // Fill the look-ahead buffer with silence so the first real block comes
      // out aligned.
      std::vector<SampleType> prewarm_silence_buf(latency_, SampleType{0});
      SampleType* prewarm_silence[1] = {prewarm_silence_buf.data()};
      to_model_resampler_->PushBlock(prewarm_silence, latency_);

      // Move the primed state into the second stage.
      std::vector<SampleType> model_rate_prewarm_buf(max_model_rate_frames_,
                                                     SampleType{0});
      SampleType* model_rate_prewarm[1] = {model_rate_prewarm_buf.data()};
      size_t prewarm_samples_popped = to_model_resampler_->PopBlock(
          model_rate_prewarm, max_model_rate_frames_);
      if (prewarm_samples_popped > 0) {
        from_model_resampler_->PushBlock(
            model_rate_prewarm, static_cast<int>(prewarm_samples_popped));
      }
    } else {
      to_model_resampler_.reset();
      from_model_resampler_.reset();
      latency_ = 0;
    }
  }

  std::unique_ptr<NeuralAudio::NeuralModel> model_;
  double model_rate_;
  double host_rate_;
  int max_model_rate_frames_{1024};
  int latency_{0};
  std::unique_ptr<Resampler> to_model_resampler_;
  std::unique_ptr<Resampler> from_model_resampler_;
  std::vector<SampleType> resample_in_;
  std::vector<SampleType> resample_out_;
};

class neural_tilde : public object<neural_tilde>, public vector_operator<> {
 public:
  MIN_DESCRIPTION{"Neural~ — load and run NeuralAudio models in real time."};
  MIN_TAGS{"audio, effects, ampsim"};
  MIN_AUTHOR{"Alessandro Presta"};
  MIN_RELATED{"plugout~, plugin~"};

  inlet<> inlet_audio_{
      this, "(signal) Audio in + load / clear / prewarm / bang", "signal"};

  outlet<> outlet_audio_{this, "(signal) Audio out", "signal"};
  outlet<> outlet_status_{this,
                          "Status: loaded <path> / latency <n> / loudness "
                          "<dB> / queued <path> / cleared / error <msg>"};

  neural_tilde(const atoms& args = {}) {
    // Optional creation argument: [neural~ /path/to/model]
    if (!args.empty() && args[0].type() == message_type::symbol_argument) {
      const std::string path = symbol{args[0]};
      if (!path.empty()) load_(args);
    }
  }

  ~neural_tilde() {
    // Ensure the load thread finishes before any members are destroyed.
    if (load_thread_.joinable()) load_thread_.join();
  }

  message<> dspsetup{
      this, "dspsetup",
      MIN_FUNCTION{const double sr = static_cast<double>(args[0]);
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
}
}
;

message<> load_{this, "load",
                "Load a model file. Accepts absolute or relative paths; "
                "spaces in filenames "
                "are handled. Relative paths are resolved against the "
                "patcher's directory first, "
                "then Max's file-search path.",
                MIN_FUNCTION{if (args.empty()){
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
  cwarn << "already loading, please wait" << endl;
  return {};
}

// ResolvePath handles normalization, relative-path lookup, and error logging.
const std::string path = ResolvePath(raw_path);
if (path.empty()) return {};

loading_.store(true, std::memory_order_release);
if (load_thread_.joinable()) load_thread_.join();
load_thread_ = std::thread([this, path]() { DoLoad(path); });
return {};
}
}
;

message<> clear_{this, "clear", "Unload the current model.",
                 MIN_FUNCTION{if (load_thread_.joinable()) load_thread_.join();
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
}
}
;

message<> prewarm_{this, "prewarm", "Prewarm the model (NAM-only).",
                   MIN_FUNCTION{if (model_) model_->Prewarm();
else cwarn << "no model loaded" << endl;
return {};
}
}
;

message<> bang_{
    this, "bang", "Report current model status to the status outlet.",
    MIN_FUNCTION{
        // Check whether a model is parked waiting for the engine to start.
        {std::lock_guard<std::mutex> lock(swap_mutex_);
if (pending_raw_model_) {
  outlet_status_.send("queued", symbol{pending_raw_path_});
  return {};
}
}
if (!model_) {
  outlet_status_.send("cleared");
  return {};
}
if (model_->HasLoudness())
  outlet_status_.send("loudness", model_->GetLoudness());
else
  outlet_status_.send("loaded");
return {};
}
}
;

void operator()(audio_bundle input, audio_bundle output) {
  // Swap in a newly loaded model, or null out the active one on clear.
  if (model_pending_.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> lock(swap_mutex_);
    model_ = std::move(pending_model_);
    if (model_) model_->Reset(sample_rate_, max_buffer_size_);
    model_pending_.store(false, std::memory_order_release);
  }

  disable_denormals();

  const sample* in = input.samples(0);
  sample* out = output.samples(0);
  const int block_size = static_cast<int>(vector_size());

  // Convert float input to float working buffer.
  for (int i = 0; i < block_size; ++i)
    input_buf_[i] = static_cast<float>(in[i]);

  if (model_) {
    model_->Process(input_buf_.data(), output_buf_.data(), block_size);
    for (int i = 0; i < block_size; ++i)
      out[i] = static_cast<sample>(output_buf_[i]);
  } else {
    // Pass-through when no model is loaded.
    for (int i = 0; i < block_size; ++i) out[i] = in[i];
  }
}

private:
// Model state.
std::unique_ptr<ResamplingModel> model_;
std::unique_ptr<ResamplingModel> pending_model_;
std::atomic<bool> model_pending_{false};
std::mutex swap_mutex_;
std::thread load_thread_;
std::atomic<bool> loading_{false};
std::string current_path_;

// Deferred-load state: adopted into unique_ptr immediately after
// NeuralAudio::NeuralModel::CreateFromFile() returns.
std::unique_ptr<NeuralAudio::NeuralModel> pending_raw_model_;
std::string pending_raw_path_;

// Audio buffers (float — NeuralAudio::NeuralModel::Process() takes float*).
double sample_rate_{0.0};
int max_buffer_size_{0};
std::vector<float> input_buf_;
std::vector<float> output_buf_;

// Deferred notifications.
struct Notification {
  enum class Kind { Loaded, Queued, Error } kind;
  std::string text;
  long latency{0};
  bool has_loudness{false};
  double loudness{0.0};
};

std::mutex notify_mutex_;
std::vector<Notification> notifications_;

queue<> notify_queue_{this, MIN_FUNCTION{std::vector<Notification> pending;
{
  std::lock_guard<std::mutex> lock(notify_mutex_);
  pending.swap(notifications_);
}
for (const auto& n : pending) {
  if (n.kind == Notification::Kind::Loaded) {
    outlet_status_.send("loaded", symbol{n.text});
    outlet_status_.send("latency", n.latency);
    outlet_status_.send("loudness", n.has_loudness ? n.loudness : 0.0);
  } else if (n.kind == Notification::Kind::Queued) {
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
  input_buf_.assign(n, 0.0f);
  output_buf_.assign(n, 0.0f);
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
      std::cerr << "nam~: cannot find '" << path << "'\n";
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

  std::cerr << "nam~: cannot find '" << raw_path << "'\n";
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
    PostNotification({Notification::Kind::Error, e.what()});
    return;
  } catch (...) {
    PostNotification(
        {Notification::Kind::Error, "unknown error wrapping model"});
    return;
  }

  long latency_samples = 0;
  bool has_loudness = false;
  double loudness_db = 0.0;
  {
    std::lock_guard<std::mutex> lock(swap_mutex_);
    pending_model_ = std::move(new_model);
    current_path_ = path;
    pending_raw_model_.reset();
    pending_raw_path_.clear();
    if (pending_model_ && pending_model_->NeedToResample())
      latency_samples = static_cast<long>(pending_model_->GetLatencySamples());
    if (pending_model_ && pending_model_->HasLoudness()) {
      has_loudness = true;
      loudness_db = pending_model_->GetLoudness();
    }
  }
  model_pending_.store(true, std::memory_order_release);
  PostNotification({Notification::Kind::Loaded, path, latency_samples,
                    has_loudness, loudness_db});
}

void DoLoad(const std::string& path) {
  // Adopt the raw pointer immediately so it is always owned.
  std::unique_ptr<NeuralAudio::NeuralModel> raw_model;
  try {
    raw_model.reset(NeuralAudio::NeuralModel::CreateFromFile(path));
  } catch (const std::exception& e) {
    PostNotification({Notification::Kind::Error, e.what()});
    loading_.store(false, std::memory_order_release);
    return;
  } catch (...) {
    PostNotification({Notification::Kind::Error,
                      "unknown error loading model (wrong file type?)"});
    loading_.store(false, std::memory_order_release);
    return;
  }

  if (!raw_model) {
    PostNotification(
        {Notification::Kind::Error,
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
      // Discard any previously parked model and take ownership of the new one.
      pending_raw_model_ = std::move(raw_model);
      pending_raw_path_ = path;
      current_path_ = path;
    }
    PostNotification({Notification::Kind::Queued, path});
    loading_.store(false, std::memory_order_release);
    return;
  }

  // Engine is ready. Wrap and stage immediately.
  WrapAndStage(std::move(raw_model), path, host_rate, host_block_size);
  loading_.store(false, std::memory_order_release);
}
}
;

MIN_EXTERNAL(neural_tilde);
