#ifndef SLOP_CONSTANTS_H_
#define SLOP_CONSTANTS_H_

namespace slop {

// Gemini / Google Cloud Endpoints
constexpr char kPublicGeminiBaseUrl[] = "https://generativelanguage.googleapis.com/v1beta";

// Headers
constexpr char kUserAgent[] = "std::slop/prerelease";

// OpenAI Endpoints
// constexpr char kOpenAIBaseUrl[] = "https://api.openai.com/v1";
constexpr char kOpenAIBaseUrl[] = "https://openrouter.ai/api/v1";
constexpr char kOpenAIOfficialBaseUrl[] = "https://api.openai.com/v1";
constexpr char kOpenAiChatGptCodexBaseUrl[] = "https://chatgpt.com/backend-api/codex";
constexpr char kOpenAiOAuthTokenUrl[] = "https://auth.openai.com/oauth/token";
constexpr char kOpenAiOAuthClientId[] = "app_EMoamEEZ73f0CkXaXp7hrann";
}  // namespace slop

#endif  // SLOP_CONSTANTS_H_
