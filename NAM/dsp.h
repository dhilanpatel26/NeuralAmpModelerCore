#pragma once

#include <filesystem>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <Eigen/Dense>

#include "activations.h"
#include "json.hpp"

#ifdef NAM_SAMPLE_FLOAT
  #define NAM_SAMPLE float
#else
  #define NAM_SAMPLE double
#endif

enum EArchitectures
{
  kLinear = 0,
  kConvNet,
  kLSTM,
  kCatLSTM,
  kWaveNet,
  kCatWaveNet,
  kNumModels
};

// Class for providing params from the plugin to the DSP module
// For now, we'll work with doubles. Later, we'll add other types.
class DSPParam
{
public:
  const char* name;
  const double val;
};
// And the params shall be provided as a std::vector<DSPParam>.

// How loud do we want the models to be? in dB
#define TARGET_DSP_LOUDNESS -18.0

class DSP
{
public:
  DSP(const double expected_sample_rate = -1.0);
  DSP(const double loudness, const double expected_sample_rate = -1.0);
  virtual ~DSP() = default;
  // process() does all of the processing requried to take `input` array and
  // fill in the required values on `output`.
  // To do this:
  // 1. The core DSP algorithm is run (This is what should probably be
  //    overridden in subclasses).
  // 2. The output level is applied and the result stored to `output`.
  virtual void process(NAM_SAMPLE* input, NAM_SAMPLE* output, const int num_frames);
  // Anything to take care of before next buffer comes in.
  // For example:
  // * Move the buffer index forward
  // * Does NOT say that params aren't stale; that's the job of the routine
  //   that actually uses them, which varies depends on the particulars of the
  //   DSP subclass implementation.
  virtual void finalize_(const int num_frames);
  double GetExpectedSampleRate() const { return mExpectedSampleRate; };
  bool HasNormalization() { return mLoudness != TARGET_DSP_LOUDNESS; };
  double GetNormalizationFactordB() { return -(this->mLoudness - TARGET_DSP_LOUDNESS); };
  double GetNormalizationFactorLinear() { return pow(10.0, -(this->mLoudness - TARGET_DSP_LOUDNESS) / 20.0); };

protected:
  // How loud is the model?
  double mLoudness;
  // What sample rate does the model expect?
  double mExpectedSampleRate;
  // Parameters (aka "knobs")
  std::unordered_map<std::string, double> _params;
  // If the params have changed since the last buffer was processed:
  bool _stale_params;

  // Methods

  // Copy the parameters to the DSP module.
  // If anything has changed, then set this->_stale_params to true.
  // (TODO use "listener" approach)
  void _get_params_(const std::unordered_map<std::string, double>& input_params);
};

// Class where an input buffer is kept so that long-time effects can be
// captured. (e.g. conv nets or impulse responses, where we need history that's
// longer than the sample buffer that's coming in.)
class Buffer : public DSP
{
public:
  Buffer(const int receptive_field, const double expected_sample_rate = -1.0);
  Buffer(const double loudness, const int receptive_field, const double expected_sample_rate = -1.0);
  void finalize_(const int num_frames);

protected:
  // Input buffer
  const int _input_buffer_channels = 1; // Mono
  int _receptive_field;
  // First location where we add new samples from the input
  long _input_buffer_offset;
  std::vector<float> _input_buffer;
  std::vector<float> _output_buffer;

  void _set_receptive_field(const int new_receptive_field, const int input_buffer_size);
  void _set_receptive_field(const int new_receptive_field);
  void _reset_input_buffer();
  // Use this->_input_post_gain
  virtual void _update_buffers_(NAM_SAMPLE* input, int num_frames);
  virtual void _rewind_buffers_();
};

// Basic linear model (an IR!)
class Linear : public Buffer
{
public:
  Linear(const int receptive_field, const bool _bias, const std::vector<float>& params,
         const double expected_sample_rate = -1.0);
  Linear(const double loudness, const int receptive_field, const bool _bias, const std::vector<float>& params,
         const double expected_sample_rate = -1.0);
  void process(NAM_SAMPLE* input, NAM_SAMPLE* output, const int num_frames) override;

protected:
  Eigen::VectorXf _weight;
  float _bias;
};

// NN modules =================================================================

class Conv1D
{
public:
  Conv1D() { this->_dilation = 1; };
  void set_params_(std::vector<float>::iterator& params);
  void set_size_(const int in_channels, const int out_channels, const int kernel_size, const bool do_bias,
                 const int _dilation);
  void set_size_and_params_(const int in_channels, const int out_channels, const int kernel_size, const int _dilation,
                            const bool do_bias, std::vector<float>::iterator& params);
  // Process from input to output
  //  Rightmost indices of input go from i_start to i_end,
  //  Indices on output for from j_start (to j_start + i_end - i_start)
  void process_(const Eigen::MatrixXf& input, Eigen::MatrixXf& output, const long i_start, const long i_end,
                const long j_start) const;
  long get_in_channels() const { return this->_weight.size() > 0 ? this->_weight[0].cols() : 0; };
  long get_kernel_size() const { return this->_weight.size(); };
  long get_num_params() const;
  long get_out_channels() const { return this->_weight.size() > 0 ? this->_weight[0].rows() : 0; };
  int get_dilation() const { return this->_dilation; };

private:
  // Gonna wing this...
  // conv[kernel](cout, cin)
  std::vector<Eigen::MatrixXf> _weight;
  Eigen::VectorXf _bias;
  int _dilation;
};

// Really just a linear layer
class Conv1x1
{
public:
  Conv1x1(const int in_channels, const int out_channels, const bool _bias);
  void set_params_(std::vector<float>::iterator& params);
  // :param input: (N,Cin) or (Cin,)
  // :return: (N,Cout) or (Cout,), respectively
  Eigen::MatrixXf process(const Eigen::MatrixXf& input) const;

  long get_out_channels() const { return this->_weight.rows(); };

private:
  Eigen::MatrixXf _weight;
  Eigen::VectorXf _bias;
  bool _do_bias;
};

// Utilities ==================================================================
// Implemented in get_dsp.cpp

// Data for a DSP object
// :param version: Data version. Follows the conventions established in the trainer code.
// :param architecture: Defines the high-level architecture. Supported are (as per `get-dsp()` in get_dsp.cpp):
//     * "CatLSTM"
//     * "CatWaveNet"
//     * "ConvNet"
//     * "LSTM"
//     * "Linear"
//     * "WaveNet"
// :param config:
// :param metadata:
// :param params: The model parameters ("weights")
// :param expected_sample_rate: Most NAM models implicitly assume that data will be provided to them at some sample
//     rate. This captures it for other components interfacing with the model to understand its needs. Use -1.0 for "I
//     don't know".
struct dspData
{
  std::string version;
  std::string architecture;
  nlohmann::json config;
  nlohmann::json metadata;
  std::vector<float> params;
  double expected_sample_rate;
};

// Verify that the config that we are building our model from is supported by
// this plugin version.
void verify_config_version(const std::string version);

// Takes the model file and uses it to instantiate an instance of DSP.
std::unique_ptr<DSP> get_dsp(const std::filesystem::path model_file);
// Creates an instance of DSP. Also returns a dspData struct that holds the data of the model.
std::unique_ptr<DSP> get_dsp(const std::filesystem::path model_file, dspData& returnedConfig);
// Instantiates a DSP object from dsp_config struct.
std::unique_ptr<DSP> get_dsp(dspData& conf);
// Legacy loader for directory-type DSPs
std::unique_ptr<DSP> get_dsp_legacy(const std::filesystem::path dirname);
