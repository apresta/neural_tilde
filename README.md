# neural~

This Max/MSP object loads and runs neural amplifier models in real time.

It supports [NAM](https://www.neuralampmodeler.com/) and [AIDA-X](https://aida-x.cc/) models, and handles resampling to the host rate.

Sound demo via Max for Live: [Live Amp Modeler](https://www.youtube.com/watch?v=m2VRggzL93I).

The object's inlet accepts the following messages:

- *(signal)*: The mono audio signal.
- *load \<model path\>*: Load a neural amp model (.nam or .json/.aidax).
- *clear*: Unload the current model.
- *prewarm*: (NAM-only) Prewarm the model to avoid digital artifacts.
- *bang*: Report model status.

The object's first outlet outputs:

- *(signal)*: The processed audio signal.

The object's second outled outputs the following messages:

- *loaded \<model path\>*: Path to model upon successful load.
- *latency \<ms\>*: Audio latency (non-zero when model and host sample rates differ).
- *loudness \<dB\>*: Loudness information in the model, if present. Used for
  normalization purposes.
- *queued \<model path\>*: Signals that the model was parked while waiting for the
  audio engine to start.
- *cleared*: Confirms that the model was unloaded from the object.
- *error \<message\>*: The object encountered an error.

The build script has been tested on MacOS. Windows cross-compilation is
supported via mingw-w64.

Dependencies:

- <https://github.com/Cycling74/min-devkit>
- <https://github.com/mikeoliphant/NeuralAudio>
- <https://github.com/sdatkinson/AudioDSPTools>
- <https://clang.llvm.org/> (for MacOS build)
- <https://github.com/mstorsjo/llvm-mingw> (for Windows build)
