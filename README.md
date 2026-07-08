# neural~

This Max/MSP object loads and runs neural amplifier models in real time.

It supports [NAM](https://www.neuralampmodeler.com/) and [AIDA-X](https://aida-x.cc/) models, and handles resampling to the host rate.

Smooth model switching is implemented, avoiding audible noise when swapping
amplifier captures.

Sound demo via Max for Live: [Live Amp Modeler](https://www.youtube.com/watch?v=m2VRggzL93I).

The object's inlet accepts the following messages:

- *(signal)*: The mono audio signal.
- *load \<model path>*: Load a neural amp model (.nam or .json/.aidax). Accepts
  absolute or relative paths (relative paths are resolved against the
  patcher's directory first, then Max's file-search path). If the model
  specifies recommended input/output level adjustments, they are applied
  automatically around the model's processing.
- *clear*: Unload the current model.
- *prewarm*: (NAM-only) Prewarm the model to avoid digital artifacts.
- *bang*: Report model status.

## Attributes

- *quality \<0.0-1.0>*: Model quality scale factor, from 0.0 (fastest /
  lowest quality) to 1.0 (slowest / highest quality). Only affects models that
  support quality scaling (such as slimmable NAM A2 models).
  The value persists and is applied to any model loaded afterward.

The object's first outlet outputs:

- *(signal)*: The processed audio signal.

The object's second outlet outputs the following messages:

- *loaded \<model path>*: Path to model upon successful load.
- *latency \<ms>*: Audio latency (non-zero when model and host sample rates differ).
- *quality_supported \<0|1>*: 1 if the loaded model supports quality adjustment
  via the *quality* attribute, 0 otherwise.
- *queued \<model path>*: Signals that the model was parked while waiting for the
  audio engine to start.
- *cleared*: Confirms that the model was unloaded from the object.
- *error \<message>*: The object encountered an error.

The build script has been tested on MacOS. Windows cross-compilation is
supported via mingw-w64.

Dependencies:

- <https://cmake.org>
- <https://github.com/Cycling74/min-devkit>
- <https://github.com/mikeoliphant/NeuralAudio>
- <https://clang.llvm.org/> (for MacOS build)
- <https://github.com/mstorsjo/llvm-mingw> (for Windows build)
