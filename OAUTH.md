# OAuth and Project Discovery Documentation

This document provides a detailed overview of the authentication and project discovery architecture in `std::slop`.

## 1. Architectural Overview

`std::slop` follows a decoupled architecture for authentication, separating interactive credential acquisition from the core application logic.

*   **Interactive Authentication**: Handled by a standalone shell script (`slop_auth.sh`). This reduces the C++ application's complexity and avoids embedding heavy interactive logic or transient dependencies (like a local loopback server) within the performance-critical binary.
*   **Token Management & Discovery**: Handled by the `OAuthHandler` class in C++. It manages token persistence, silent background refreshes using the `refresh_token`, and authoritative project discovery via internal Google Cloud Assist (GCA) endpoints.

## 2. Authentication Flow

### slop_auth.sh
The unified authentication script supports two modes: `gemini` (Standard) and `antigravity` (Internal/GCA).

1.  **PKCE (S256)**: The script generates a random `code_verifier` and a `code_challenge` (SHA256 hash) to secure the authorization code exchange.
2.  **Scopes**:
    *   **Shared**: `cloud-platform`, `userinfo.email`, `userinfo.profile`.
    *   **Antigravity Only**: `cclog`, `experimentsandconfigs`.
3.  **Automatic Extraction**: After authorization, the user is redirected to `localhost`. The user pastes the full redirect URL into the script, which automatically extracts the `code` parameter.
4.  **Persistence**: Tokens are saved with restricted permissions (`chmod 600`) to:
    *   Standard: `~/.config/slop/token.json`
    *   Antigravity: `~/.config/slop/antigravity_token.json`

## 3. Modes and Endpoints

| Feature | Standard Gemini OAuth | Antigravity (Internal GCA) |
| :--- | :--- | :--- |
| **CLI Flag** | (Default if no key) | `--antigravity` |
| **Token File** | `token.json` | `antigravity_token.json` |
| **API Endpoint** | `v1internal` | `v1internal` |
| **Model Default**| `gemini-2.0-flash` | `gemini-3-flash-preview` |
| **GCA Wrapping** | Enabled | Enabled |

**Note**: Both modes utilize the `v1internal` endpoint. This is required because the Client IDs used for OAuth only carry `cloud-platform` scopes, which are not supported by the public `v1beta` endpoint for standard users.

## 4. Authoritative Project Discovery

A critical part of the flow is correctly identifying the Google Cloud Project ID. 

### Discovery Priority
1.  **`loadCodeAssist`**: The app calls `https://cloudcode-pa.googleapis.com/v1internal:loadCodeAssist`. It specifically prioritizes the **`cloudaicompanionProject`** field in the response.
    *   **Significance**: For many users, Google provisions a "Managed Project" for Code Assist. This project is often where preview models and specific quotas are enabled. This project ID takes precedence over environment variables or local `gcloud` settings.
    *   **Format Handling**: The code handles both string and object (`{ "id": "..." }`) response formats for this field.
2.  **Antigravity Fallback**: If discovery fails in `--antigravity` mode, it falls back to the industry-standard internal project `rising-fact-p41fc`.
3.  **Environment Variables**: `GOOGLE_CLOUD_PROJECT` or `GOOGLE_CLOUD_PROJECT_ID`.
4.  **Local Config**: Active project set via `gcloud config set project`.
5.  **API List**: The first project returned by the `v1/projects` endpoint.

## 5. Regression Test Plan

To ensure this complex flow remains stable, the following tests are recommended:

### Unit Tests (`oauth_handler_test.cpp`)
*   **Token Selection**: Verify the `OAuthHandler` loads the correct file path based on the `OAuthMode` passed to the constructor.
*   **JSON Parsing**: Test `DiscoverProjectId` against various mock JSON payloads:
    *   Response with string `cloudaicompanionProject`.
    *   Response with object `cloudaicompanionProject`.
    *   Response with missing fields (ensure graceful fallback to env/gcloud).
*   **Header Verification**: Verify that the `loadCodeAssist` call includes the required `X-Goog-Api-Client` and `Client-Metadata` headers.

### Integration Tests
*   **Refresh Loop**: Mock a 401 error and verify that `GetValidToken()` successfully calls `RefreshToken()` and retries.
*   **Script Extraction**: A small bash test for `slop_auth.sh` to ensure the `grep/cut` logic correctly handles various URL encodings of the authorization code.

### Manual Verification
*   Verify that `token.json` is created with `0600` permissions.
*   Verify that running with `--antigravity` correctly identifies as `Mode: Antigravity` in the startup splash.
