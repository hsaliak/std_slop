#!/bin/bash

# slop_auth.sh - Legacy ChatGPT Plus OAuth helper for std::slop

MODE="${1:-}"

CHATGPT_CLIENT_ID="${CHATGPT_CLIENT_ID:-app_EMoamEEZ73f0CkXaXp7hrann}"
CHATGPT_CLIENT_SECRET="${CHATGPT_CLIENT_SECRET:-}"
CHATGPT_ISSUER="${CHATGPT_ISSUER:-https://auth.openai.com}"
CHATGPT_REDIRECT_URI="${CHATGPT_REDIRECT_URI:-http://localhost:1455/auth/callback}"
CHATGPT_SCOPE="${CHATGPT_SCOPE:-openid profile email offline_access}"
CHATGPT_EXTRA_SCOPES="${CHATGPT_EXTRA_SCOPES:-}"
CHATGPT_TOKEN_FILE="${CHATGPT_TOKEN_FILE:-$HOME/.config/slop/chatgpt_plus_token.json}"
CHATGPT_AUTH_BASE_URL="${CHATGPT_AUTH_BASE_URL:-$CHATGPT_ISSUER/oauth/authorize}"
CHATGPT_TOKEN_URL="${CHATGPT_TOKEN_URL:-$CHATGPT_ISSUER/oauth/token}"
CHATGPT_ORIGINATOR="${CHATGPT_ORIGINATOR:-codex_cli_rs}"
CHATGPT_DEVICE_INTERVAL_SAFETY_SECONDS="${CHATGPT_DEVICE_INTERVAL_SAFETY_SECONDS:-3}"
CHATGPT_DEVICE_TIMEOUT_SECONDS="${CHATGPT_DEVICE_TIMEOUT_SECONDS:-900}"

usage() {
    cat <<EOF
Usage:
  ./slop_auth.sh [chatgpt-plus|chatgpt-plus-device]

Prefer the built-in flow: `std_slop --fetch-oauth`

Modes:
  chatgpt-plus         ChatGPT Plus OAuth via browser + manual callback paste (default).
  chatgpt-plus-device  ChatGPT Plus OAuth via device code (headless-friendly).

Optional env vars:
  CHATGPT_CLIENT_ID                     (default: app_EMoamEEZ73f0CkXaXp7hrann)
  CHATGPT_CLIENT_SECRET
  CHATGPT_ISSUER                        (default: https://auth.openai.com)
  CHATGPT_AUTH_BASE_URL                 (default: \$CHATGPT_ISSUER/oauth/authorize)
  CHATGPT_TOKEN_URL                     (default: \$CHATGPT_ISSUER/oauth/token)
  CHATGPT_REDIRECT_URI                  (default: http://localhost:1455/auth/callback)
  CHATGPT_SCOPE                         (default: openid profile email offline_access)
  CHATGPT_EXTRA_SCOPES                  (default: empty; optional extra scopes if your client allows them)
  CHATGPT_ORIGINATOR                    (default: codex_cli_rs)
  CHATGPT_TOKEN_FILE                    (default: ~/.config/slop/chatgpt_plus_token.json)
  CHATGPT_DEVICE_INTERVAL_SAFETY_SECONDS (default: 3)
  CHATGPT_DEVICE_TIMEOUT_SECONDS         (default: 900)
EOF
}

case "$MODE" in
    "")
        usage
        exit 0
        ;;
    chatgpt-plus|chatgpt-plus-device)
        :
        ;;
    -h|--help|help)
        usage
        exit 0
        ;;
    *)
        echo "Error: Unsupported mode '$MODE'."
        echo ""
        usage
        exit 1
        ;;
esac

GOOGLE_CLIENT_ID="681255809395-oo8ft2oprdrnp9e3aqf6av3hmdib135j.apps.googleusercontent.com"
GOOGLE_CLIENT_SECRET="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"
GOOGLE_REDIRECT_URI="http://localhost"
GOOGLE_SCOPE="https://www.googleapis.com/auth/cloud-platform https://www.googleapis.com/auth/userinfo.email https://www.googleapis.com/auth/userinfo.profile"
GOOGLE_TOKEN_FILE="$HOME/.config/slop/token.json"
GOOGLE_AUTH_BASE_URL="https://accounts.google.com/o/oauth2/v2/auth"
GOOGLE_TOKEN_URL="https://oauth2.googleapis.com/token"

if [ -z "$CHATGPT_CLIENT_ID" ]; then
    echo "Error: CHATGPT_CLIENT_ID is required."
    exit 1
fi
CLIENT_ID="$CHATGPT_CLIENT_ID"
CLIENT_SECRET="$CHATGPT_CLIENT_SECRET"
REDIRECT_URI="$CHATGPT_REDIRECT_URI"
SCOPE="$CHATGPT_SCOPE"
TOKEN_FILE="$CHATGPT_TOKEN_FILE"
AUTH_BASE_URL="$CHATGPT_AUTH_BASE_URL"
TOKEN_URL="$CHATGPT_TOKEN_URL"
RESPONSE_TYPE="code"

gen_base64url() {
    openssl rand -base64 32 | tr -d '\n' | tr '+/' '-_' | tr -d '='
}

extract_json_string_field() {
    local key="$1"
    local json="$2"
    echo "$json" | grep -o "\"$key\"[[:space:]]*:[[:space:]]*\"[^\"]*\"" | head -n1 | cut -d'"' -f4
}

extract_json_number_field() {
    local key="$1"
    local json="$2"
    echo "$json" | grep -o "\"$key\"[[:space:]]*:[[:space:]]*[0-9]\+" | head -n1 | grep -o '[0-9]\+'
}

url_decode() {
    local data="${1//+/ }"
    printf '%b' "${data//%/\\x}"
}

decode_error_payload_json() {
    local payload="$1"
    local b64url
    local rem
    local b64

    b64url=$(url_decode "$payload")
    b64=$(echo -n "$b64url" | tr '_-' '/+')
    rem=$((${#b64} % 4))
    if [ "$rem" -eq 2 ]; then
        b64="${b64}=="
    elif [ "$rem" -eq 3 ]; then
        b64="${b64}="
    elif [ "$rem" -eq 1 ]; then
        b64="${b64}==="
    fi
    echo -n "$b64" | base64 -d 2>/dev/null || true
}

join_scopes() {
    local primary="$1"
    local extra="$2"
    local all="$primary $extra"
    # Normalize whitespace
    echo "$all" | tr -s '[:space:]' ' ' | sed 's/^ //; s/ $//'
}

decode_jwt_payload_json() {
    local jwt="$1"
    local payload_b64url
    payload_b64url=$(echo "$jwt" | cut -d'.' -f2)
    if [ -z "$payload_b64url" ]; then
        return 0
    fi
    decode_error_payload_json "$payload_b64url"
}

warn_if_missing_api_scopes() {
    local access_token="$1"
    # ChatGPT OAuth mode uses chatgpt.com backend endpoints and does not require
    # OpenAI API platform scopes like api.model.read/api.responses.write.
    : "$access_token"
}

save_tokens() {
    local access_token="$1"
    local refresh_token="$2"
    local expires_in="$3"
    local token_file="$4"

    if [ -z "$access_token" ]; then
        echo "Error: Access token is empty."
        exit 1
    fi
    if [ -z "$refresh_token" ]; then
        echo "Error: Refresh token is empty."
        exit 1
    fi

    local expiry_time
    expiry_time=$(($(date +%s) + ${expires_in:-3600}))
    local token_dir
    token_dir=$(dirname "$token_file")
    mkdir -p "$token_dir"

    cat <<EOF > "$token_file"
{
    "access_token": "$access_token",
    "refresh_token": "$refresh_token",
    "expiry_time": $expiry_time
}
EOF

    chmod 600 "$token_file"
    echo "Authentication successful. Tokens saved to $token_file"
    warn_if_missing_api_scopes "$access_token"
}

exchange_code_for_tokens() {
    local code="$1"
    local code_verifier="$2"
    local redirect_uri="$3"
    local client_id="$4"
    local token_url="$5"

    local response_with_status
    if [ -n "$CHATGPT_CLIENT_SECRET" ]; then
        response_with_status=$(curl -sS -X POST "$token_url" \
            -H "Content-Type: application/x-www-form-urlencoded" \
            -d "grant_type=authorization_code" \
            -d "code=$code" \
            -d "redirect_uri=$redirect_uri" \
            -d "client_id=$client_id" \
            -d "code_verifier=$code_verifier" \
            -d "client_secret=$CHATGPT_CLIENT_SECRET" \
            -w $'\n__HTTP_STATUS__:%{http_code}') || return 1
    else
        response_with_status=$(curl -sS -X POST "$token_url" \
            -H "Content-Type: application/x-www-form-urlencoded" \
            -d "grant_type=authorization_code" \
            -d "code=$code" \
            -d "redirect_uri=$redirect_uri" \
            -d "client_id=$client_id" \
            -d "code_verifier=$code_verifier" \
            -w $'\n__HTTP_STATUS__:%{http_code}') || return 1
    fi

    local http_status
    http_status=$(echo "$response_with_status" | sed -n 's/^__HTTP_STATUS__://p' | tail -n1)
    local response
    response=$(echo "$response_with_status" | sed '/^__HTTP_STATUS__:/d')

    case "$http_status" in
        2??)
            echo "$response"
            return 0
            ;;
        *)
            echo "Error: Token endpoint returned HTTP $http_status"
            echo "$response"
            return 1
            ;;
    esac
}

run_device_flow() {
    local requested_scope
    requested_scope=$(join_scopes "$CHATGPT_SCOPE" "$CHATGPT_EXTRA_SCOPES")
    local device_usercode_url="${CHATGPT_ISSUER%/}/api/accounts/deviceauth/usercode"
    local device_poll_url="${CHATGPT_ISSUER%/}/api/accounts/deviceauth/token"
    local device_verify_url="${CHATGPT_ISSUER%/}/codex/device"
    local device_redirect_uri="${CHATGPT_ISSUER%/}/deviceauth/callback"
    local device_req_body
    if [ -n "$requested_scope" ]; then
        device_req_body="{\"client_id\":\"$CHATGPT_CLIENT_ID\",\"scope\":\"$requested_scope\"}"
    else
        device_req_body="{\"client_id\":\"$CHATGPT_CLIENT_ID\"}"
    fi

    local usercode_with_status
    if ! usercode_with_status=$(curl -sS -X POST "$device_usercode_url" \
        -H "Content-Type: application/json" \
        -d "$device_req_body" \
        -w $'\n__HTTP_STATUS__:%{http_code}'); then
        echo "Error: Failed to initiate device authorization."
        exit 1
    fi

    local usercode_status
    usercode_status=$(echo "$usercode_with_status" | sed -n 's/^__HTTP_STATUS__://p' | tail -n1)
    local usercode_resp
    usercode_resp=$(echo "$usercode_with_status" | sed '/^__HTTP_STATUS__:/d')
    case "$usercode_status" in
        2??) ;;
        *)
            echo "Error: Device auth usercode endpoint returned HTTP $usercode_status"
            echo "$usercode_resp"
            exit 1
            ;;
    esac

    local device_auth_id
    device_auth_id=$(extract_json_string_field "device_auth_id" "$usercode_resp")
    local user_code
    user_code=$(extract_json_string_field "user_code" "$usercode_resp")
    local interval_str
    interval_str=$(extract_json_string_field "interval" "$usercode_resp")
    if [ -z "$interval_str" ]; then
        interval_str=$(extract_json_number_field "interval" "$usercode_resp")
    fi
    if ! echo "$interval_str" | grep -Eq '^[0-9]+$'; then
        interval_str=5
    fi

    if [ -z "$device_auth_id" ] || [ -z "$user_code" ]; then
        echo "Error: Device auth response missing required fields."
        echo "$usercode_resp"
        exit 1
    fi

    echo "Device authorization started."
    echo "1. Open: $device_verify_url"
    echo "2. Enter code: $user_code"
    echo "The script will poll until authorization completes."

    local start_time
    start_time=$(date +%s)

    local authorization_code=""
    local device_code_verifier=""

    while true; do
        local now
        now=$(date +%s)
        if [ $((now - start_time)) -ge "$CHATGPT_DEVICE_TIMEOUT_SECONDS" ]; then
            echo "Error: Device authorization timed out after ${CHATGPT_DEVICE_TIMEOUT_SECONDS}s."
            exit 1
        fi

        local poll_req_body
        poll_req_body="{\"device_auth_id\":\"$device_auth_id\",\"user_code\":\"$user_code\"}"
        local poll_with_status
        if ! poll_with_status=$(curl -sS -X POST "$device_poll_url" \
            -H "Content-Type: application/json" \
            -d "$poll_req_body" \
            -w $'\n__HTTP_STATUS__:%{http_code}'); then
            echo "Error: Device authorization poll request failed."
            exit 1
        fi

        local poll_status
        poll_status=$(echo "$poll_with_status" | sed -n 's/^__HTTP_STATUS__://p' | tail -n1)
        local poll_resp
        poll_resp=$(echo "$poll_with_status" | sed '/^__HTTP_STATUS__:/d')

        case "$poll_status" in
            2??)
                authorization_code=$(extract_json_string_field "authorization_code" "$poll_resp")
                device_code_verifier=$(extract_json_string_field "code_verifier" "$poll_resp")
                if [ -z "$authorization_code" ] || [ -z "$device_code_verifier" ]; then
                    echo "Error: Device token poll response missing authorization_code/code_verifier."
                    echo "$poll_resp"
                    exit 1
                fi
                break
                ;;
            403|404)
                sleep $((interval_str + CHATGPT_DEVICE_INTERVAL_SAFETY_SECONDS))
                ;;
            *)
                echo "Error: Device token poll returned HTTP $poll_status"
                echo "$poll_resp"
                exit 1
                ;;
        esac
    done

    echo "Exchanging device authorization code for tokens..."
    local token_response
    if ! token_response=$(exchange_code_for_tokens "$authorization_code" "$device_code_verifier" "$device_redirect_uri" "$CHATGPT_CLIENT_ID" "$CHATGPT_TOKEN_URL"); then
        exit 1
    fi

    local access_token
    access_token=$(extract_json_string_field "access_token" "$token_response")
    local refresh_token
    refresh_token=$(extract_json_string_field "refresh_token" "$token_response")
    local expires_in
    expires_in=$(extract_json_number_field "expires_in" "$token_response")

    save_tokens "$access_token" "$refresh_token" "${expires_in:-3600}" "$CHATGPT_TOKEN_FILE"
}
run_manual_flow() {
    local requested_scope
    requested_scope=$(join_scopes "$CHATGPT_SCOPE" "$CHATGPT_EXTRA_SCOPES")
    local code_verifier
    code_verifier=$(gen_base64url)
    local code_challenge
    code_challenge=$(echo -n "$code_verifier" | openssl dgst -sha256 -binary | openssl base64 | tr '+/' '-_' | tr -d '=')
    local state
    state=$(gen_base64url)

    local auth_url
    auth_url=$(curl -sS -o /dev/null -w '%{url_effective}' -G "$CHATGPT_AUTH_BASE_URL" \
        --data-urlencode "response_type=code" \
        --data-urlencode "client_id=$CHATGPT_CLIENT_ID" \
        --data-urlencode "redirect_uri=$CHATGPT_REDIRECT_URI" \
        --data-urlencode "scope=$requested_scope" \
        --data-urlencode "code_challenge=$code_challenge" \
        --data-urlencode "code_challenge_method=S256" \
        --data-urlencode "id_token_add_organizations=true" \
        --data-urlencode "codex_cli_simplified_flow=true" \
        --data-urlencode "state=$state" \
        --data-urlencode "originator=$CHATGPT_ORIGINATOR")

    echo "To authenticate for ChatGPT Plus OAuth, visit the following URL:"
    echo ""
    echo "$auth_url"
    echo ""
    echo "1. Authorize the application in your browser."
    echo "2. You will be redirected to localhost (which may fail to load)."
    echo "3. Copy the FULL URL from your browser's address bar and paste it below."
    echo ""
    read -p "Enter redirect URL: " callback_url

    extract_param() {
        local name="$1"
        local value
        value=$(echo "$callback_url" | sed -nE "s/.*[?#&]${name}=([^&#]+).*/\1/p" | head -n1)
        echo "$value"
    }

    local state_from_url
    state_from_url=$(url_decode "$(extract_param "state")")
    if [ -n "$state_from_url" ] && [ "$state_from_url" != "$state" ]; then
        echo "Error: state mismatch in callback URL."
        exit 1
    fi

    local error_from_url
    error_from_url=$(url_decode "$(extract_param "error")")
    local error_description_from_url
    error_description_from_url=$(url_decode "$(extract_param "error_description")")
    if [ -n "$error_from_url" ] || [ -n "$error_description_from_url" ]; then
        echo "Error: OAuth authorization failed."
        [ -n "$error_from_url" ] && echo "  error: $error_from_url"
        [ -n "$error_description_from_url" ] && echo "  error_description: $error_description_from_url"
        if [ "$error_from_url" = "invalid_scope" ]; then
            echo "Hint: The configured OAuth client may not be allowed to request API scopes."
            echo "Try with default scopes only:"
            echo "  CHATGPT_EXTRA_SCOPES='' ./slop_auth.sh chatgpt-plus"
        fi
        exit 1
    fi

    local error_payload
    error_payload=$(extract_param "payload")
    if [ -n "$error_payload" ]; then
        local payload_json
        payload_json=$(decode_error_payload_json "$error_payload")
        echo "Error: OAuth authorization failed at auth.openai.com."
        if [ -n "$payload_json" ]; then
            local kind
            kind=$(echo "$payload_json" | grep -o '"kind"[[:space:]]*:[[:space:]]*"[^"]*"' | head -n1 | cut -d'"' -f4)
            local error_code
            error_code=$(echo "$payload_json" | grep -o '"errorCode"[[:space:]]*:[[:space:]]*"[^"]*"' | head -n1 | cut -d'"' -f4)
            local request_id
            request_id=$(echo "$payload_json" | grep -o '"requestId"[[:space:]]*:[[:space:]]*"[^"]*"' | head -n1 | cut -d'"' -f4)
            [ -n "$kind" ] && echo "  kind: $kind"
            [ -n "$error_code" ] && echo "  errorCode: $error_code"
            [ -n "$request_id" ] && echo "  requestId: $request_id"
        fi
        exit 1
    fi

    local code
    code=$(url_decode "$(extract_param "code")")
    if [ -z "$code" ]; then
        echo "Error: Could not find code parameter in callback URL."
        exit 1
    fi

    echo "Exchanging code for tokens..."
    local token_response
    if ! token_response=$(exchange_code_for_tokens "$code" "$code_verifier" "$CHATGPT_REDIRECT_URI" "$CHATGPT_CLIENT_ID" "$CHATGPT_TOKEN_URL"); then
        exit 1
    fi

    local access_token
    access_token=$(extract_json_string_field "access_token" "$token_response")
    local refresh_token
    refresh_token=$(extract_json_string_field "refresh_token" "$token_response")
    local expires_in
    expires_in=$(extract_json_number_field "expires_in" "$token_response")

    save_tokens "$access_token" "$refresh_token" "${expires_in:-3600}" "$CHATGPT_TOKEN_FILE"
}

if [ "$MODE" = "chatgpt-plus-device" ]; then
    run_device_flow
else
    run_manual_flow
fi



