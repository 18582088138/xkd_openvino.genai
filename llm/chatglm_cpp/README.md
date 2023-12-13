# LLM

This application showcases inference of a large language model (LLM). It doesn't have much of configuration options to encourage the reader to explore and modify the source code. There's a Jupyter notebook which corresponds to this pipeline and discusses how to create an LLM-powered Chatbot: https://github.com/openvinotoolkit/openvino_notebooks/tree/main/notebooks/254-llm-chatbot.

> Note  
This pipeline is not for production use.

## How it works

The program loads a tokenizer, detokenizer and a model (`.xml` and `.bin`) to OpenVINO™. The model is reshaped to batch 1 and variable prompt length. A prompt is tokenized and passed to the model. The model greedily generates token by token until the special end of sequence (EOS) token is obtained. The predicted tokens are converted to chars and printed in a streaming fashion.

## Install OpenVINO Runtime

Install OpenVINO Runtime from an archive: [Linux](https://docs.openvino.ai/2023.2/openvino_docs_install_guides_installing_openvino_from_archive_linux.html) or [Windows](https://docs.openvino.ai/2023.2/openvino_docs_install_guides_installing_openvino_from_archive_windows.html). `<INSTALL_DIR>` below refers to the extraction location.

## Supported models

1. chatglm
   1. https://hf-mirror.com/THUDM/chatglm2-6b
   2. https://hf-mirror.com/THUDM/chatglm3-6b

Build the pipelines and `user_ov_extensions`

1.Clone submodules:

```sh
git submodule update --init
```

2.Build

For Linux

Compile the project using CMake:

```sh
source <OpenVINO dir>/setupvars.sh
cmake -B build
cmake --build build -j --config Release
```

For Windows

Compile the project using CMake:

```sh
<OpenVINO dir>\setupvars.bat
cmake -B build
cmake --build build -j --config Release
```

3.Download and convert the model and tokenizers

For linux

```sh
python -m pip install --upgrade-strategy eager thirdparty/openvino_contrib/modules/custom_operations/user_ie_extensions/tokenizer/python/[transformers] onnx "optimum[openvino]>=1.14.0" --extra-index-url https://download.pytorch.org/whl/cpu
pip install -r ./llm_bench/python/requirements.txt
python ./llm_bench/python/convert.py --model_id --model_id <model_id_or_path> --output_dir <out_dir>
source <OpenVINO dir>/setupvars.sh
python ./llm/cpp/convert_tokenizers.py ./build/thirdparty/openvino_contrib/modules/custom_operations/user_ie_extensions/libuser_ov_extensions.so ./chatglm3-6b/
```

4. Run
   
For Linux

Usage: `chatglm <openvino_model.xml> <tokenizer.xml> <detokenizer.xml> "device"`

Example: `./build/llm/chatglm_cpp/chatglm ./chatglm3-6b/openvino_model.xml ./tokenizer.xml ./detokenizer.xml "CPU"`

For Windows

copy user_ov_extensions.dll and its dependent dll to chatglm.exe directory

```sh
$copy build\thirdparty\openvino_contrib\modules\custom_operations\user_ie_extensions\Release\user_ov_extensions.dll build\llm\chatglm_cpp\Release\
$copy build\_deps\fast_tokenizer-src\third_party\lib\icudt70.dll build\llm\chatglm_cpp\Release\
$copy build\_deps\fast_tokenizer-src\third_party\lib\icuuc70.dll build\llm\chatglm_cpp\Release\
$copy build\_deps\fast_tokenizer-src\lib\core_tokenizers.dll build\llm\chatglm_cpp\Release\
```

Usage: `chatglm.exe <openvino_model.xml> <tokenizer.xml> <detokenizer.xml> "device"`

Example: `build\llm\chatglm_cpp\Release\chatglm.exe .\chatglm3-6b\openvino_model.xml .\tokenizer.xml .\detokenizer.xml "CPU"`

To enable non ASCII characters for Windows cmd open `Region` settings from `Control panel`. `Administrative`->`Change system locale`->`Beta: Use Unicode UTF-8 for worldwide language support`->`OK`. Reboot.
