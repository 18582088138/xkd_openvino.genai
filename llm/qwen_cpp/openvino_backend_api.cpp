#include "openvino_backend_api.h"
#include "sampling.hpp"
#include <random>

typedef std::chrono::high_resolution_clock Time;
typedef std::chrono::nanoseconds ns;

double get_duration_ms_until_now(Time::time_point &startTime)
{
  return std::chrono::duration_cast<ns>(Time::now() - startTime).count() * 0.000001;
}

int32_t get_out_token_id(const std::vector<int> &history_ids, float *logits, size_t vocab_size, params params)
{
  int32_t out_token;
  // logits pre-process
  if (params.repeat_penalty != 1.f)
  {
    const int penalty_tokens_used_size = std::min((int)history_ids.size(), params.repeat_last_n);
    if (penalty_tokens_used_size)
    {
      sampling_repetition_penalty(logits, logits + vocab_size, history_ids, penalty_tokens_used_size, params.repeat_penalty);
    }
  }

  if (params.do_sample)
  {
    if (params.temperature > 0)
    {
      sampling_temperature(logits, logits + vocab_size, params.temperature);
    }

    std::vector<TokenIdScore> token_scores(vocab_size);
    for (size_t i = 0; i < vocab_size; i++)
    {
      token_scores[i] = TokenIdScore((int)i, logits[i]);
    }

    // top_k sampling
    if (0 < params.top_k && params.top_k < (int)token_scores.size())
    {
      sampling_top_k(token_scores.data(), token_scores.data() + params.top_k,
                     token_scores.data() + token_scores.size());
      token_scores.resize(params.top_k);
    }

    // top_p sampling
    if (0.f < params.top_p && params.top_p < 1.f)
    {
      auto pos = sampling_top_p(token_scores.data(), token_scores.data() + token_scores.size(), params.top_p);
      token_scores.resize(pos - token_scores.data());
    }

    // sample next token
    sampling_softmax_inplace(token_scores.data(), token_scores.data() + token_scores.size());
    for (size_t i = 0; i < token_scores.size(); i++)
    {
      logits[i] = token_scores[i].score;
    }

    thread_local std::mt19937 rng(params.seed);
    std::discrete_distribution<> dist(logits, logits + token_scores.size());
    out_token = token_scores[dist(rng)].id;
  }
  else
  {
    out_token = std::max_element(logits, logits + vocab_size) - logits;
  }

  return out_token;
}

namespace openvino_backend
{
  // 参数初始化
  api_interface::api_interface(const params &params)
  {
    std::cout << "\n[OpenVINO Backend API Interface] init parameters called\n";
    // Init OpenVINO Runtime
    std::cout << "Init OpenVINO backend with version: \n"
              << ov::get_openvino_version() << std::endl;
    _device = params.device;
    _model_cache_dir = params.model_cache_dir;
    if (_device.find("CPU") != std::string::npos)
    {
      _device_config[ov::cache_dir.name()] = _model_cache_dir;
      _device_config[ov::hint::scheduling_core_type.name()] = ov::hint::SchedulingCoreType::PCORE_ONLY;
      _device_config[ov::hint::enable_hyper_threading.name()] = false;
      _device_config[ov::hint::enable_cpu_pinning.name()] = true;
    }

    if (_device.find("GPU") != std::string::npos)
    {
      _device_config[ov::cache_dir.name()] = _model_cache_dir;
      _device_config[ov::intel_gpu::hint::queue_throttle.name()] = ov::intel_gpu::hint::ThrottleLevel::MEDIUM;
      _device_config[ov::intel_gpu::hint::queue_priority.name()] = ov::hint::Priority::MEDIUM;
      _device_config[ov::intel_gpu::hint::host_task_priority.name()] = ov::hint::Priority::HIGH;
      _device_config[ov::hint::enable_cpu_pinning.name()] = true;
    }
    _api_status = status::init;
  }
  api_interface::~api_interface()
  {
    api_stop();
    api_unloadmodel();
  }

  // 加载模型
  void api_interface::api_loadmodel(char *buffer, int thread_num)
  {
    std::cout << "\n[OpenVINO Backend API Interface] load model called\n";
    auto startTime = Time::now();
    _infer_request = std::make_unique<ov::InferRequest>(_core.compile_model(std::string(buffer), _device, _device_config).create_infer_request());
    auto llm_load_duration = get_duration_ms_until_now(startTime);
    std::cout << "Load llm took: " << llm_load_duration << " ms\n";
    _perf_statistic.llm_load_duration = llm_load_duration;
    _vocab_size = _infer_request->get_tensor("logits").get_shape().back();
    _api_status = status::loaded;
  }

  // Load tokenizer with model path
  void api_interface::api_loadtokenizer(std::string tokenizer_path)
  {
    std::cout << "\n[OpenVINO Backend API Interface] load tokenizer with model path called\n";
    auto startTime = Time::now();

    _tokenizer = std::make_shared<qwen::QwenTokenizer>(tokenizer_path, _tokenizer_config);
    auto tokenizer_load_duration = get_duration_ms_until_now(startTime);
    std::cout << "Load tokenizer took: " << tokenizer_load_duration << " ms\n";
    _perf_statistic.tokenizer_load_duration = tokenizer_load_duration;
  }

  // Load tokenizer with passed pointer
  void api_interface::api_loadtokenizer(std::shared_ptr<qwen::QwenTokenizer> tokenizer_ptr)
  {
    std::cout << "\n[OpenVINO Backend API Interface] load tokenizer with passed tokenizer pointer called\n";
    auto startTime = Time::now();

    _tokenizer = tokenizer_ptr;
    auto tokenizer_load_duration = get_duration_ms_until_now(startTime);
    std::cout << "Load tokenizer took: " << tokenizer_load_duration << " ms\n";
    _perf_statistic.tokenizer_load_duration = tokenizer_load_duration;
  }

  // 流式接口
  bool api_interface::api_Generate(const std::string &prompt, const params &params, void (*api_callback)(int32_t *new_token_id, bool *_stop_generation))
  {
    _api_status = status::inference;
    std::cout << "\n[OpenVINO Backend API Interface] non-stream generate called\n";
    // std::vector<int> input_ids = _tokenizer->encode_history({prompt}, params.n_ctx);
    std::vector<int> input_ids = _tokenizer->encode(prompt, params.n_ctx);
    std::vector<int> history_ids = input_ids;
    _perf_statistic.input_token_num = input_ids.size();

    int32_t output_token = this->generate_first_token(input_ids, params);
    _new_token_id = output_token;
    api_callback(&_new_token_id, &_stop_generation);
    history_ids.push_back(output_token);

    _infer_request->get_tensor("input_ids").set_shape({BATCH_SIZE, 1});
    _infer_request->get_tensor("position_ids").set_shape({BATCH_SIZE, 1});

    while ((output_token != _tokenizer_config.eos_token_id) &&
           (output_token != _tokenizer_config.im_end_id) &&
           (history_ids.size() - input_ids.size() < params.n_predict))
    {
      if (_stop_generation)
      {
        api_callback(&_new_token_id, &_stop_generation);
        break;
      }
      else
      {
        output_token = this->generate_next_token(output_token, history_ids, params);
        _new_token_id = output_token;
        api_callback(&_new_token_id, &_stop_generation);
        history_ids.push_back(output_token);
      }
    }

    _perf_statistic.generated_token_num = history_ids.size() - input_ids.size();
    _perf_statistic.llm_average_token_per_second = _perf_statistic.generated_token_num / _perf_statistic.llm_generate_next_token_duration * 1000.0;
    std::cout << "Average next token generation speed: " << _perf_statistic.llm_average_token_per_second << " token per second.\n";
    _api_status = status::loaded;

    return true;
  }

  std::string api_interface::api_Generate(const std::string &prompt, const params &params)
  {
    _api_status = status::inference;
    std::cout << "\n[OpenVINO Backend API Interface] non-stream generate called\n";
    // std::vector<int> input_ids = _tokenizer->encode_history({prompt}, params.n_ctx);
    std::vector<int> input_ids = _tokenizer->encode(prompt, params.n_ctx);
    std::vector<int> history_ids = input_ids;
    std::vector<int> output_ids;
    _perf_statistic.input_token_num = input_ids.size();

    int32_t output_token = this->generate_first_token(input_ids, params);
    output_ids.push_back(output_token);
    history_ids.push_back(output_token);

    _infer_request->get_tensor("input_ids").set_shape({BATCH_SIZE, 1});
    _infer_request->get_tensor("position_ids").set_shape({BATCH_SIZE, 1});

    while ((output_token != _tokenizer_config.eos_token_id) &&
           (output_token != _tokenizer_config.im_end_id) &&
           (output_ids.size() < params.n_predict) && (!_stop_generation))
    {
      output_token = this->generate_next_token(output_token, history_ids, params);
      output_ids.push_back(output_token);
      history_ids.push_back(output_token);
    }

    _perf_statistic.generated_token_num = output_ids.size() - 1;
    _perf_statistic.llm_average_token_per_second = _perf_statistic.generated_token_num / _perf_statistic.llm_generate_next_token_duration * 1000.0;
    std::cout << "Average next token generation speed: " << _perf_statistic.llm_average_token_per_second << " token per second.\n";

    std::string response = _tokenizer->decode(output_ids);
    _api_status = status::loaded;

    return response;
  }

  int32_t api_interface::generate_first_token(std::vector<int> &input_ids, const params &params)
  {
    // Prepare input tensor for first infer
    _infer_request->get_tensor("input_ids").set_shape({BATCH_SIZE, input_ids.size()});
    _infer_request->get_tensor("attention_mask").set_shape({BATCH_SIZE, input_ids.size()});
    std::copy_n(input_ids.data(), input_ids.size(), _infer_request->get_tensor("input_ids").data<int32_t>());
    std::fill_n(_infer_request->get_tensor("attention_mask").data<int32_t>(), input_ids.size(), 1);

    _infer_request->get_tensor("beam_idx").set_shape({BATCH_SIZE});
    _infer_request->get_tensor("beam_idx").data<int32_t>()[0] = 0;
    _infer_request->get_tensor("position_ids").set_shape({BATCH_SIZE, input_ids.size()});
    std::iota(_infer_request->get_tensor("position_ids").data<int32_t>(), _infer_request->get_tensor("position_ids").data<int32_t>() + _infer_request->get_tensor("position_ids").get_size(), 0);
    for (auto &&state : _infer_request->query_state())
    {
      state.reset();
    }
    // First inference
    auto startTime = Time::now();
    _infer_request->start_async();
    _infer_request->wait();
    auto first_infer_duration_ms = get_duration_ms_until_now(startTime);
    std::cout << "First inference took: " << first_infer_duration_ms << " ms" << std::endl;
    _perf_statistic.llm_first_infer_duration = first_infer_duration_ms;
    _perf_statistic.llm_prompt_evaluation_speed = _perf_statistic.input_token_num / _perf_statistic.llm_first_infer_duration * 1000.0;
    std::cout << "Input token num: " << _perf_statistic.input_token_num << ", prompt evaluation speed: " << _perf_statistic.llm_prompt_evaluation_speed << " token per second.\n";
    auto logits = _infer_request->get_tensor("logits").data<float>();
    int32_t output_token = get_out_token_id(input_ids, logits, _vocab_size, params);
    return output_token;
  }

  int32_t api_interface::generate_next_token(int32_t input_token, std::vector<int32_t> history_ids, const params &params)
  {
    _infer_request->get_tensor("input_ids").data<int32_t>()[0] = input_token;
    _infer_request->get_tensor("attention_mask").set_shape({BATCH_SIZE, _infer_request->get_tensor("attention_mask").get_shape()[1] + 1});
    std::fill_n(_infer_request->get_tensor("attention_mask").data<int32_t>(), _infer_request->get_tensor("attention_mask").get_size(), 1);
    _infer_request->get_tensor("position_ids").data<int32_t>()[0] = int32_t(_infer_request->get_tensor("attention_mask").get_size() - 2);

    // 2nd+ inference
    auto startTime = Time::now();
    _infer_request->start_async();
    _infer_request->wait();
    auto generate_next_token_duration_ms = get_duration_ms_until_now(startTime);
    _perf_statistic.llm_generate_next_token_duration += generate_next_token_duration_ms;

    // Get 2nd+ inference results
    auto logits = _infer_request->get_tensor("logits").data<float>();
    int32_t output_token = get_out_token_id(history_ids, logits, _vocab_size, params);

    return output_token;
  }

  // 环境复位
  void api_interface::api_Reset()
  {
    std::cout << "\n[OpenVINO Backend API Interface] reset called\n";
    // Reset infer request internal state
    for (auto &&state : _infer_request->query_state())
    {
      state.reset();
    }

    // Reset performance statistic
    memset(&_perf_statistic, 0, sizeof(PerformanceStatistic));
    _new_token_id = _tokenizer_config.im_end_id;
    _stop_generation = false;
    _api_status = status::loaded;
  }

  // 卸载模型
  bool api_interface::api_unloadmodel()
  {
    std::cout << "\n[OpenVINO Backend API Interface] unload model called\n";
    auto startTime = Time::now();
    _infer_request = nullptr;
    auto llm_unload_duration = get_duration_ms_until_now(startTime);
    std::cout << "Unload llm took: " << llm_unload_duration << " ms\n";
    _perf_statistic.llm_unload_duration = llm_unload_duration;
    _api_status = status::unloaded;

    return true;
  }

  // 获取状态
  int api_interface::api_status()
  {
    std::cout << "\n[OpenVINO Backend API Interface] api_status called\n";
    switch (_api_status)
    {
    case status::init:
      std::cout << "OpenVINO backend status: Initialized\n";
      break;
    case status::loaded:
      std::cout << "OpenVINO backend status: Loaded\n";
      break;
    case status::unloaded:
      std::cout << "OpenVINO backend status: Unloaded\n";
      break;
    case status::inference:
      std::cout << "OpenVINO backend status: Running Inference\n";
      break;
    default:
      std::cout << "OpenVINO backend status: Uninitialized\n";
    }

    return _api_status;
  }

  // 停止生成
  bool api_interface::api_stop()
  {
    std::cout << "\n[OpenVINO Backend API Interface] stop generation called\n";
    // Cancel infer request
    auto startTime = Time::now();
    _infer_request->cancel();
    _stop_generation = true;
    auto llm_cancel_duration = get_duration_ms_until_now(startTime);
    std::cout << "Cancel llm took: " << llm_cancel_duration << " ms\n";
    _perf_statistic.llm_cancel_duration = llm_cancel_duration;
    _api_status = status::loaded;

    return true;
  }

  PerformanceStatistic api_interface::get_performance_statistics()
  {
    return _perf_statistic;
  }

} // namespace openvino_backend
