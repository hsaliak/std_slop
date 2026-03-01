# OAuth and Token Management

This document describes OAuth behavior in `std::slop` for both Google and OpenAI.

## Overview

`std::slop` keeps interactive browser/device authorization outside the C++ binary in `slop_auth.sh`.
The C++ `OAuthHandler` is responsible for:
- loading persisted tokens,
- refreshing access tokens using `refresh_token`,
- exposing valid bearer tokens to runtime request paths.

## OAuth Flows via `slop_auth.sh`

### Google OAuth
- Command: `./slop_auth.sh google`
- Token file: `~/.config/slop/token.json`
- Runtime flag: `--google_oauth`
- Endpoint family used by runtime: Google Cloud Code Assist (`v1internal`)

### OpenAI OAuth (ChatGPT Plus)
- Command: `./slop_auth.sh chatgpt-plus`
- Headless/device option: `./slop_auth.sh chatgpt-plus-device`
- Token file: `~/.config/slop/chatgpt_plus_token.json`
- Runtime flag: `--openai_oauth`
- API mode: OpenAI Responses API (forced)
- Runtime base URL: `https://api.openai.com/v1` (custom `openai_base_url` is ignored in this mode)

## Runtime Selection Rules

- `--google_oauth` and `--openai_oauth` cannot be used together.
- If no API keys and no OAuth flags are set, `std::slop` defaults to Google OAuth mode.
- OpenAI API key mode uses Chat Completions by default.
- OpenAI API key mode can switch to Responses with `--use_responses`.

## Token Refresh Behavior

`OAuthHandler` automatically refreshes tokens before expiry:
- Google refresh endpoint: `https://oauth2.googleapis.com/token`
- OpenAI refresh endpoint: `https://auth.openai.com/oauth/token`

If refresh fails or tokens are missing:
- Google guidance: run `./slop_auth.sh google`
- OpenAI guidance: run `./slop_auth.sh chatgpt-plus`

## OpenAI OAuth Token Path Override

You can override the default OpenAI token path with:
- CLI: `--openai_oauth_token_path=/custom/path/chatgpt_plus_token.json`
- INI key: `openai_oauth_token_path = /custom/path/chatgpt_plus_token.json`

## Google Project Discovery

Project discovery is only used in Google OAuth mode.
Priority:
1. `loadCodeAssist` managed project (`cloudaicompanionProject`)
2. `GOOGLE_CLOUD_PROJECT` / `GOOGLE_CLOUD_PROJECT_ID`
3. `gcloud config get-value project`
4. First project from Cloud Resource Manager `projects` list

## Test Coverage

`oauth_handler_test.cpp` covers:
- default token path selection for Google and OpenAI OAuth providers,
- token loading behavior,
- project discovery parsing for Google mode.
