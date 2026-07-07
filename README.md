# neural~

This Max/MSP object loads and runs neural amplifier models in real time.

It supports captures in [NAM](https://www.neuralampmodeler.com/) format, and handles resampling to the host rate.

Sound demo via Max for Live: [Live Amp Modeler](https://www.youtube.com/watch?v=m2VRggzL93I).

The object's inlet accepts the following messages:

- *(signal)*: The mono audio signal.
- *load \<model path>*: Load a neural amp model (.nam extension).
- *clear*: Unload the current model.
- *prewarm*: Prewarm the model to avoid digital artifacts.
- *bang*: Report model status.
- *slim \<0-1>*: Set model quality/size (0 = smallest/fastest, 1 = full size/quality).
- *output_mode \<raw|normalized|calibrated>*: How the output level is
  auto-compensated: *raw* applies none, *normalized* targets -18 dB loudness
  from the model's embedded metadata, *calibrated* matches the model's
  embedded output level to *input_calibration_level*.
- *calibrate_input \<0|1>*: When on, offsets the input gain so a signal at
  *input_calibration_level* hits the model's own calibrated input level.
- *input_calibration_level \<dBu>*: Reference level for input calibration
  (-60 to 60 dBu).

The object's first outlet outputs:

- *(signal)*: The processed audio signal.

The object's second outlet outputs the following messages:

- *loaded \<model path>*: Path to model upon successful load.
- *latency \<n>*: Audio latency in samples (non-zero when model and host sample
  rates differ).
- *loudness \<dB>*: Loudness information in the model, if present. Used for
  normalization purposes.
- *slimmable \<0|1>*: Whether the loaded model supports the *slim* size reduction.
- *input_calibration \<dB>*: Model's embedded input calibration level, if present.
- *output_calibration \<dB>*: Model's embedded output calibration level, if present.
- *queued \<model path>*: Signals that the model was parked while waiting for the
  audio engine to start.
- *cleared*: Confirms that the model was unloaded from the object.
- *error \<message>*: The object encountered an error.

The build script has been tested on MacOS. Windows cross-compilation is
supported via mingw-w64.

Dependencies:

- <https://cmake.org>
- <https://github.com/Cycling74/min-devkit>
- <https://github.com/sdatkinson/NeuralAmpModelerCore>
- <https://clang.llvm.org/> (for MacOS build)
- <https://github.com/mstorsjo/llvm-mingw> (for Windows build)
