# OAuth and Token Management

This document describes OAuth behavior in `std::slop` for OpenAI.

## Overview

`std::slop` keeps interactive browser/device authorization outside the C++ binary in `slop_auth.sh`.
The C++ `OAuthHandler` is responsible for:
- loading persisted tokens,
- refreshing access tokens using `refresh_token`,
- exposing valid bearer tokens to runtime request paths.

## OAuth Flows via `slop_auth.sh`

### OpenAI OAuth (ChatGPT Plus)
- Command: `./slop_auth.sh chatgpt-plus`
- Headless/device option: `./slop_auth.sh chatgpt-plus-device`
- Token file: `~/.config/slop/chatgpt_plus_token.json`
- Runtime flag: `--openai_oauth`
- API mode: OpenAI Responses API (forced)
- Runtime base URL: `https://api.openai.com/v1` (custom `openai_base_url` is ignored in this mode)

## Runtime Selection Rules

- OpenAI API key mode uses Chat Completions by default.
- OpenAI API key mode can switch to Responses with `--use_responses`.

## Token Refresh Behavior

`OAuthHandler` automatically refreshes tokens before expiry:
- OpenAI refresh endpoint: `https://auth.openai.com/oauth/token`

If refresh fails or tokens are missing:
- OpenAI guidance: run `./slop_auth.sh chatgpt-plus`

## OpenAI OAuth Token Path Override

You can override the default OpenAI token path with:
- CLI: `--openai_oauth_token_path=/custom/path/chatgpt_plus_token.json`
- INI key: `openai_oauth_token_path = /custom/path/chatgpt_plus_token.json`

## Test Coverage

`oauth_handler_test.cpp` covers:
- default token path selection for OpenAI OAuth provider,
- token loading behavior,

