# OAuth and Project Discovery Documentation

This document provides a detailed overview of the authentication and project discovery architecture in `std::slop`.

## 1. Architectural Overview

`std::slop` follows a decoupled architecture for authentication, separating interactive credential acquisition from the core application logic.

*   **Interactive Authentication**: Handled by a standalone shell script (`slop_auth.sh`). This reduces the C++ application's complexity and avoids embedding heavy interactive logic or transient dependencies within the binary.
*   **Token Management & Discovery**: Handled by the `OAuthHandler` class in C++. It manages token persistence, silent background refreshes using the `refresh_token`, and authoritative project discovery via internal Google Cloud Assist (GCA) endpoints.

## 2. Authentication Flow

### slop_auth.sh
The authentication script handles the standard Gemini OAuth flow.

1.  **PKCE (S256)**: The script generates a random `code_verifier` and a `code_challenge` (SHA256 hash) to secure the authorization code exchange.
2.  **Scopes**: `cloud-platform`, `userinfo.email`, `userinfo.profile`.
3.  **Automatic Extraction**: After authorization, the user is redirected to `localhost`. The user pastes the full redirect URL into the script, which automatically extracts the `code` parameter.
4.  **Persistence**: Tokens are saved with restricted permissions (`chmod 600`) to `~/.config/slop/token.json`.

## 3. Configuration

| Feature | Details |
| :--- | :--- |
| **CLI Flag** | (Default if no key) |
| **Token File** | `~/.config/slop/token.json` |
| **API Endpoint** | `v1internal` |
| **Model Default**| `gemini-2.5-flash` |
| **GCA Wrapping** | Enabled |

**Note**: OAuth utilizes the `v1internal` endpoint. This is required because the Client IDs used for OAuth only carry `cloud-platform` scopes, which are not supported by the public `v1beta` endpoint for standard users.

## 4. Authoritative Project Discovery

A critical part of the flow is correctly identifying the Google Cloud Project ID. 

### Discovery Priority
1.  **`loadCodeAssist`**: The app calls `https://cloudcode-pa.googleapis.com/v1internal:loadCodeAssist`. It specifically prioritizes the **`cloudaicompanionProject`** field in the response.
    *   **Significance**: For many users, Google provisions a "Managed Project" for Code Assist. This project is often where preview models and specific quotas are enabled. This project ID takes precedence over environment variables or local `gcloud` settings.
    *   **Format Handling**: The code handles both string and object (`{ "id": "..." }`) response formats for this field.
2.  **Environment Variables**: `GOOGLE_CLOUD_PROJECT` or `GOOGLE_CLOUD_PROJECT_ID`.
3.  **Local Config**: Active project set via `gcloud config set project`.
4.  **API List**: The first project returned by the `v1/projects` endpoint.

## 5. Regression Test Plan

To ensure this flow remains stable, the following tests are implemented in `oauth_handler_test.cpp`:

*   **Token Loading**: Verify that `OAuthHandler` correctly loads from `token.json`.
*   **JSON Parsing**: Test `DiscoverProjectId` against various mock JSON payloads (string vs object formats).
*   **Header Verification**: Verify that the `loadCodeAssist` call includes required identification headers.