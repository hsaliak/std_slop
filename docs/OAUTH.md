# OAuth and Token Management

This document describes OAuth behavior in `std::slop` for OpenAI.

## Overview

`std::slop` includes built-in OpenAI OAuth bootstrap flows.
The C++ `OAuthHandler` is responsible for:
- loading persisted tokens,
- refreshing access tokens using `refresh_token`,
- initiating manual and device authorization for the first token,
- exposing valid bearer tokens to runtime request paths.

## OpenAI OAuth Quickstart

### OpenAI OAuth (ChatGPT Plus)
- Default command: `std_slop --fetch_openai_oauth_token`
- Device command: `std_slop --fetch_openai_oauth_device_token`
- Runtime command after login: `std_slop --openai_oauth`
- Token file: `~/.config/slop/chatgpt_plus_token.json`
- Runtime flag: `--openai_oauth`
- API mode: OpenAI Responses API (forced)
- Runtime base URL: `https://chatgpt.com/backend-api/codex` (custom `openai_base_url` is ignored in this mode)

`--fetch_openai_oauth_token` is the standard browser+paste flow:
1. `std_slop` prints the authorization URL.
2. You complete login/consent in the browser.
3. You paste the full redirect URL back into the CLI.
4. `std_slop` exchanges the authorization code and saves tokens.

`--fetch_openai_oauth_device_token` is the headless/device flow:
1. `std_slop` prints the verification URL and user code.
2. You authorize on another browser/device.
3. `std_slop` polls for an authorization code.
4. `std_slop` exchanges that code via `/oauth/token` and saves tokens.

## Runtime Selection Rules

- OpenAI API key mode uses Chat Completions by default.
- OpenAI API key mode can switch to Responses with `--use_responses`.

## Token Refresh Behavior

`OAuthHandler` automatically refreshes tokens before expiry:
- OpenAI refresh endpoint: `https://auth.openai.com/oauth/token`

If refresh fails or tokens are missing:
- OpenAI guidance: run `std_slop --fetch_openai_oauth_token`

## OpenAI OAuth Token Path Override

You can override the default OpenAI token path with:
- CLI: `--openai_oauth_token_path=/custom/path/chatgpt_plus_token.json`
- INI key: `openai_oauth_token_path = /custom/path/chatgpt_plus_token.json`

## Test Coverage

`oauth_handler_test.cpp` covers:
- default token path selection for OpenAI OAuth provider,
- token loading behavior,

