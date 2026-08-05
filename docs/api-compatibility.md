# API Compatibility

Pengy works with any OpenAI-compatible API. Here are the providers and configurations that have been tested.

## Provider reference

| Service | Base URL | Key required | Notes |
|---------|----------|-------------|-------|
| **OpenAI** | `https://api.openai.com/v1` | ✅ | Full support: GPT-4o, GPT-4, o-series, reasoning, vision |
| **Ollama** | `http://localhost:11434/v1` | ❌ (anything works) | Local models. Tested with llama3, gemma, mistral, qwen, llava |
| **LM Studio** | `http://localhost:1234/v1` | ❌ (anything works) | Local models. Experimental tool calling since v0.2.9+ |
| **vLLM** | `http://localhost:8000/v1` | ❌ (anything works) | Local/self-hosted. Tool calling added 2025. Production-grade |
| **llama.cpp** | `http://localhost:8080/v1` | ❌ (anything works) | Local via `llama-server`. Tool calling & vision supported |
| **OpenRouter** | `https://openrouter.ai/api/v1` | ✅ | Gateway to 200+ models. Supports reasoning, vision |
| **Groq** | `https://api.groq.com/openai/v1` | ✅ | Fast inference. Tested with llama3, mixtral, gemma, llama vision |
| **Fireworks AI** | `https://api.fireworks.ai/inference/v1` | ✅ | OpenAI-compatible. Tool calling, vision, fine-tuning. Supports MCP |
| **Together AI** | `https://api.together.xyz/v1` | ✅ | OpenAI-compatible. Chat, vision, images, embeddings, speech |

## Feature support by provider

| Feature | OpenAI | Ollama | LM Studio | vLLM | llama.cpp | OpenRouter | Groq | Fireworks | Together |
|---------|--------|--------|-----------|------|-----------|------------|------|-----------|----------|
| Basic chat | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Tool calling | ✅ | ✅ | ⚠️ exp. | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Streaming | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Model discovery | ✅ | ✅ | ❌ | ✅ | ⚠️ | ✅ | ✅ | ✅ | ✅ |
| Reasoning effort | ✅ | ❌ | ❌ | ❌ | ❌ | ✅ | ❌ | ❌ | ❌ |
| Vision/images | ✅ | ✅ | ⚠️ model-dep. | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |

## Setting up a local endpoint

### Ollama

```bash
ollama pull llama3
ollama serve
# Set base_url to http://localhost:11434/v1
```

### LM Studio

1. Download and install LM Studio
2. Load a model
3. Enable the local API server
4. Set base_url to `http://localhost:1234/v1`

LM Studio's tool calling is experimental. Some models work, some don't.

### vLLM

```bash
vllm serve meta-llama/Llama-3.1-8B-Instruct
# Set base_url to http://localhost:8000/v1
```

### llama.cpp

```bash
# Build with server support, then:
llama-server -m path/to/model.gguf --port 8080

# For vision models, add the multimodal projection:
llama-server -m path/to/model.gguf --mmproj path/to/mmproj.gguf --port 8080

# Set base_url to http://localhost:8080/v1
```

Uses the OpenAI chat completions format. Tool calling works via JSON schema. For model discovery, `llama-server` reports the model you loaded — it's single-model by default, so discovery is straightforward.

## Notes

- **Local endpoint API keys:** Ollama, LM Studio, vLLM, and llama.cpp don't validate API keys, but Pengy requires one to be set. Use any string (e.g. `sk-local`).
- **Reasoning models:** OpenAI o-series, Claude (via OpenRouter), and some Qwen models support reasoning traces. Set `reasoning_effort` to control detail level.
- **Rate limits:** Cloud providers have rate limits on free tiers. Exponential backoff is built in — Pengy retries 429/529 responses automatically.
- **Tool calling quirks:** Not all models do it well, even if the provider supports it. Smaller models (3B–8B params) often struggle with multi-turn tool use. If tools aren't being called, try a larger model.
