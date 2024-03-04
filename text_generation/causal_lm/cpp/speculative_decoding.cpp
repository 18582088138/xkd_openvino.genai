// Copyright (C) 2023-2024 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include <openvino/openvino.hpp>
#include <cmath>
#include <random>

constexpr size_t BATCH_SIZE = 1;

namespace {
std::pair<ov::Tensor, ov::Tensor> tokenize(ov::InferRequest& tokenizer, std::string&& prompt) {
    constexpr size_t BATCH_SIZE = 1;
    tokenizer.set_input_tensor(ov::Tensor{ov::element::string, {BATCH_SIZE}, &prompt});
    tokenizer.infer();
    return {tokenizer.get_tensor("input_ids"), tokenizer.get_tensor("attention_mask")};
}

std::string detokenize(ov::InferRequest& detokenizer, std::vector<int64_t>& tokens) {
    constexpr size_t BATCH_SIZE = 1;
    detokenizer.set_input_tensor(ov::Tensor{ov::element::i64, {BATCH_SIZE, tokens.size()}, tokens.data()});
    detokenizer.infer();
    return detokenizer.get_output_tensor().data<std::string>()[0];
}

std::vector<float> softmax(const ov::Tensor& logits, float temperature) {
    float* logits_data = logits.data<float>();
    int size = logits.get_size();
    
    double sum_exp = 0.0;
    for (int i = 0; i < size; i++) {
        sum_exp += std::exp(logits_data[i] / temperature);
    }
    
    std::vector<float> probabilities;
    for (int i = 0; i < size; i++) {
        double probability = exp(logits_data[i] / temperature) / sum_exp;
        probabilities.push_back(probability);
    }
    return probabilities;
}

int random_sample(const ov::Tensor& logits, float temperature) {
    auto probabilities = softmax(logits, temperature);
    std::random_device rd;
    std::mt19937 gen(rd());
    std::discrete_distribution<> distribution(probabilities.begin(), probabilities.end());
    int sampled_index = distribution(gen);
    
    return sampled_index;
}

// The following reasons require TextStreamer to keep a cache of previous tokens:
// detokenizer removes starting ' '. For example detokenize(tokenize(" a")) == "a",
// but detokenize(tokenize("prefix a")) == "prefix a"
// 1 printable token may consist of 2 token ids: detokenize(incomplete_token_idx) == "�"
struct TextStreamer {
    ov::InferRequest detokenizer;
    std::vector<int64_t> token_cache;
    size_t print_len = 0;

    void put(int64_t token) {
        token_cache.push_back(token);
        std::string text = detokenize(detokenizer, token_cache);
        if (!text.empty() && '\n' == text.back()) {
            // Flush the cache after the new line symbol
            std::cout << std::string_view{text.data() + print_len, text.size() - print_len};
            token_cache.clear();
            print_len = 0;
	    return;
        }
        if (text.size() >= 3 && text.compare(text.size() - 3, 3, "�") == 0) {
            // Don't print incomplete text
            return;
        }
        std::cout << std::string_view{text.data() + print_len, text.size() - print_len} << std::flush;
        print_len = text.size();
    }

    void end() {
        std::string text = detokenize(detokenizer, token_cache);
        std::cout << std::string_view{text.data() + print_len, text.size() - print_len} << '\n';
        token_cache.clear();
        print_len = 0;
    }
};
}

std::vector<int64_t> convert_to_vector(ov::Tensor tensor) {
    std::vector<int64_t> res_vector;
    for (int i = 0; i << tensor.get_shape().back(); i++) {
        res_vector.emplace_back(tensor.data<int64_t>()[i]);
    }
    return res_vector;
}

ov::Tensor append_element(ov::Tensor tensor_val, int64_t element) {
    auto vec = convert_to_vector(tensor_val);
    vec.emplace_back(element);
    return ov::Tensor{ov::element::i64, {BATCH_SIZE, tensor_val.get_shape().back()}, vec.data()};
}

void set_key_values(ov::InferRequest& request, int size) {
    std::stringstream ss_1, ss_2;

    for (int i = 0; i < size; i++) {
        ss_1 << "present." << i << ".key";
        ss_2 << "past_key_values." << i << ".key";
        request.set_tensor(ss_2.str(), request.get_tensor(ss_1.str()));
        ss_1.str("");
        ss_2.str("");

        ss_1 << "present." << i << ".value";
        ss_2 << "past_key_values." << i << ".value";
        request.set_tensor(ss_2.str(), request.get_tensor(ss_1.str()));
        ss_1.str("");
        ss_2.str("");
    }
}

void init_key_values(ov::InferRequest request, int kv_length, unsigned size_1, unsigned size_2) {
    std::stringstream ss_1, ss_2;
    ss_1.clear();
    ss_2.clear();

    for (int i = 0; i < kv_length; i++) {
        ss_2 << "past_key_values." << i << ".key";
        request.set_tensor(ss_2.str(), ov::Tensor(ov::element::f32, {BATCH_SIZE, size_1, 0, size_2}));
        ss_2.str("");

        ss_2 << "past_key_values." << i << ".value";
        request.set_tensor(ss_2.str(), ov::Tensor(ov::element::f32, {BATCH_SIZE, size_1, 0, size_2}));
        ss_2.str("");
    }
}

void drop_unvalid_kv_cach(ov::InferRequest reques, int pos, int kv_size) {

}



int main(int argc, char* argv[]) try {
    // int tiny_llama_kv_size = 22;
    // int tiny_llama_size_1 = 4;
    // int tiny_llama_size_2 = 64;

    int tiny_llama_kv_size = 22;
    int tiny_llama_size_1 = 4;
    int tiny_llama_size_2 = 64;

    // int llama_kv_size = 32;
    // int llama_size_1 = 32;
    // int llama_size_2 = 128;
    int llama_kv_size = 22;
    int llama_size_1 = 4;
    int llama_size_2 = 64;

    if (argc != 4) {
        throw std::runtime_error(std::string{"Usage: "} + argv[0] + " <DRAFT MODEL_DIR> <TARGET MODEL_DIR> '<PROMPT>'");
    }
    // Compile models
    ov::Core core;
    core.add_extension(OPENVINO_TOKENIZERS_PATH);  // OPENVINO_TOKENIZERS_PATH is defined in CMakeLists.txt
    // tokenizer and detokenizer work on CPU only
    ov::InferRequest tokenizer = core.compile_model(
        std::string{argv[1]} + "/openvino_tokenizer.xml", "CPU").create_infer_request();
    auto [input_ids, attention_mask] = tokenize(tokenizer, argv[3]);
    ov::InferRequest detokenizer = core.compile_model(
        std::string{argv[1]} + "/openvino_detokenizer.xml", "CPU").create_infer_request();
    
    // draft model
    ov::InferRequest lm = core.compile_model(
        std::string{argv[1]} + "/openvino_model.xml", "CPU").create_infer_request();

    lm.set_tensor("input_ids", input_ids);
    // std::fill_n(input_ids.data<int64_t>(), input_ids.get_size(), 1);

    lm.set_tensor("input_ids", input_ids);
    lm.set_tensor("attention_mask", attention_mask);
    ov::Tensor position_ids = lm.get_tensor("position_ids");
    position_ids.set_shape(input_ids.get_shape());
    std::iota(position_ids.data<int64_t>(), position_ids.data<int64_t>() + position_ids.get_size(), 0);
    init_key_values(lm, tiny_llama_kv_size, tiny_llama_size_1, tiny_llama_size_2);
    lm.infer();

    // target
    ov::InferRequest lm_target = core.compile_model(
    std::string{argv[2]} + "/openvino_model.xml", "CPU").create_infer_request();
    
    std::vector<int64_t> input_ids_target;
    for (int i = 0; i < input_ids.get_size(); i++) {
        input_ids_target.emplace_back(input_ids.data<int64_t>()[i]);
    }
    ov::Tensor input_ids_target_tensor = ov::Tensor{ov::element::i64, input_ids.get_shape(), input_ids_target.data()};
    lm_target.set_tensor("input_ids", input_ids_target_tensor);
    lm_target.get_tensor("attention_mask").set_shape(input_ids.get_shape());
    std::fill_n(lm_target.get_tensor("attention_mask").data<int64_t>(), lm_target.get_tensor("attention_mask").get_size(), 1);

    ov::Tensor target_position_ids = lm_target.get_tensor("position_ids");
    target_position_ids.set_shape(input_ids.get_shape());
    std::iota(target_position_ids.data<int64_t>(), target_position_ids.data<int64_t>() + target_position_ids.get_size(), 0);
    init_key_values(lm_target, llama_kv_size, llama_size_1, llama_size_2);
    lm_target.infer();

    size_t vocab_size = lm.get_tensor("logits").get_shape().back();
    
    // draft
    float* logits = lm.get_tensor("logits").data<float>() + (input_ids.get_size() - 1) * vocab_size;
    int64_t arg_max_token = std::max_element(logits, logits + vocab_size) - logits;
    int64_t out_token = arg_max_token;
    lm.get_tensor("input_ids").set_shape({BATCH_SIZE, 1});
    lm.get_tensor("position_ids").set_shape({BATCH_SIZE, 1});
    
    // target
    float* logits_target = lm_target.get_tensor("logits").data<float>() + (input_ids.get_size() - 1) * vocab_size;
    int64_t target_arg_max_token = std::max_element(logits_target, logits_target + vocab_size) - logits_target;
    int64_t target_out_token = target_arg_max_token;
    lm_target.get_tensor("input_ids").set_shape({BATCH_SIZE, 1});
    lm_target.get_tensor("position_ids").set_shape({BATCH_SIZE, 1});

    TextStreamer text_streamer{std::move(detokenizer)};
    
    constexpr int64_t SPECIAL_EOS_TOKEN = 2;
    int iter = 0;
    int max_iter = 50;
    while (out_token != SPECIAL_EOS_TOKEN && iter < max_iter) {
        iter += 1;

        // draft
        lm.get_tensor("input_ids").data<int64_t>()[0] = out_token;
        lm.get_tensor("attention_mask").set_shape({BATCH_SIZE, lm.get_tensor("attention_mask").get_shape().at(1) + 1});
        std::fill_n(lm.get_tensor("attention_mask").data<int64_t>(), lm.get_tensor("attention_mask").get_size(), 1);
        lm.get_tensor("position_ids").data<int64_t>()[0] = int64_t(lm.get_tensor("attention_mask").get_size() - 2);
        set_key_values(lm, tiny_llama_kv_size);
        lm.start_async();
        lm.wait();

        // target
        lm_target.get_tensor("input_ids").data<int64_t>()[0] = target_out_token;
        lm_target.get_tensor("attention_mask").set_shape({BATCH_SIZE, lm_target.get_tensor("attention_mask").get_shape().at(1) + 1});
        std::fill_n(lm_target.get_tensor("attention_mask").data<int64_t>(), lm_target.get_tensor("attention_mask").get_size(), 1);
        lm_target.get_tensor("position_ids").data<int64_t>()[0] = int64_t(lm_target.get_tensor("attention_mask").get_size() - 2);
        set_key_values(lm_target, llama_kv_size);
        lm_target.start_async();
        lm_target.wait();
        
        text_streamer.put(out_token);
        
        // draft
        logits = lm.get_tensor("logits").data<float>();
        int64_t arg_max_token = std::max_element(logits, logits + vocab_size) - logits;
        out_token = arg_max_token;

        // target
        // logits_target = lm_target.get_tensor("logits").data<float>();
        // int64_t target_arg_max_token = std::max_element(logits_target, logits_target + vocab_size) - logits_target;
        // target_out_token = target_arg_max_token;
    }
    text_streamer.end();
    // Model is stateful which means that context (kv-cache) which belongs to a particular
    // text sequence is accumulated inside the model during the generation loop above.
    // This context should be reset before processing the next text sequence.
    // While it is not required to reset context in this sample as only one sequence is processed,
    // it is called for education purposes:
    lm.reset_state();
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
} catch (...) {
    std::cerr << "Non-exception object thrown\n";
    return EXIT_FAILURE;
}
