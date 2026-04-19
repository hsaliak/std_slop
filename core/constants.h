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
constexpr char kOpenAiOAuthAuthorizeUrl[] = "https://auth.openai.com/oauth/authorize";
constexpr char kOpenAiOAuthTokenUrl[] = "https://auth.openai.com/oauth/token";
constexpr char kOpenAiOAuthClientId[] = "app_EMoamEEZ73f0CkXaXp7hrann";
constexpr char kOpenAiOAuthRedirectUrl[] = "http://localhost:1455/auth/callback";
constexpr char kOpenAiOAuthDeviceRedirectUrl[] = "https://auth.openai.com/deviceauth/callback";
constexpr char kOpenAiOAuthOriginator[] = "codex_cli_rs";
constexpr char kOpenAiOAuthScope[] = "openid profile email offline_access";
constexpr char kOpenAiOAuthDeviceUserCodeUrl[] = "https://auth.openai.com/api/accounts/deviceauth/usercode";
constexpr char kOpenAiOAuthDeviceTokenUrl[] = "https://auth.openai.com/api/accounts/deviceauth/token";
constexpr char kOpenAiOAuthDeviceVerificationUrl[] = "https://auth.openai.com/codex/device";
}  // namespace slop

#endif  // SLOP_CONSTANTS_H_
