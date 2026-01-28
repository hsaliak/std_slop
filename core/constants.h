#ifndef SLOP_CONSTANTS_H_
#define SLOP_CONSTANTS_H_

namespace slop {

// Gemini / Google Cloud Endpoints
constexpr char kPublicGeminiBaseUrl[] = "https://generativelanguage.googleapis.com/v1beta";
constexpr char kCloudCodeBaseUrl[] = "https://cloudcode-pa.googleapis.com";
constexpr char kCloudCodeSandboxBaseUrl[] = "https://daily-cloudcode-pa.sandbox.googleapis.com";
constexpr char kCloudResourceManagerBaseUrl[] = "https://cloudresourcemanager.googleapis.com/v1";
constexpr char kServiceUsageBaseUrl[] = "https://serviceusage.googleapis.com/v1";
constexpr char kGoogleOAuthTokenUrl[] = "https://oauth2.googleapis.com/token";

// GCA / Antigravity Headers
constexpr char kGcaUserAgent[] = "antigravity/1.11.5 darwin/arm64";
constexpr char kGcaApiClient[] = "google-cloud-sdk vscode_cloudshelleditor/0.1";
constexpr char kGcaClientMetadata[] =
    "{\"ideType\":\"IDE_UNSPECIFIED\",\"platform\":\"PLATFORM_UNSPECIFIED\",\"pluginType\":\"GEMINI\"}";

// OpenAI Endpoints
// constexpr char kOpenAIBaseUrl[] = "https://api.openai.com/v1";
constexpr char kOpenAIBaseUrl[] = "https://openrouter.ai/api/v1";
}  // namespace slop

#endif  // SLOP_CONSTANTS_H_
