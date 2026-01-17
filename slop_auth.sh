#!/bin/bash

# slop_auth.sh - Unified authentication script for std::slop

MODE=${1:-gemini}

if [ "$MODE" == "antigravity" ]; then
    CLIENT_ID="1071006060591-tmhssin2h21lcre235vtolojh4g403ep.apps.googleusercontent.com"
    CLIENT_SECRET="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"
    REDIRECT_URI="http://localhost:51121/oauth-callback"
    SCOPE="https://www.googleapis.com/auth/cloud-platform https://www.googleapis.com/auth/userinfo.email https://www.googleapis.com/auth/userinfo.profile https://www.googleapis.com/auth/cclog https://www.googleapis.com/auth/experimentsandconfigs"
    TOKEN_FILE="$HOME/.config/slop/antigravity_token.json"
    DESCRIPTION="ANTIGRAVITY (Internal GCA)"
elif [ "$MODE" == "gemini" ]; then
    CLIENT_ID="681255809395-oo8ft2oprdrnp9e3aqf6av3hmdib135j.apps.googleusercontent.com"
    CLIENT_SECRET="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"
    REDIRECT_URI="http://localhost"
    SCOPE="https://www.googleapis.com/auth/cloud-platform https://www.googleapis.com/auth/userinfo.email https://www.googleapis.com/auth/userinfo.profile"
    TOKEN_FILE="$HOME/.config/slop/token.json"
    DESCRIPTION="Standard Gemini OAuth"
else
    echo "Usage: $0 [gemini|antigravity]"
    exit 1
fi

TOKEN_DIR=$(dirname "$TOKEN_FILE")
mkdir -p "$TOKEN_DIR"

# Generate PKCE verifier and challenge
gen_base64url() {
    openssl rand -base64 32 | tr -d '\n' | tr '+/' '-_' | tr -d '='
}

CODE_VERIFIER=$(gen_base64url)
CODE_CHALLENGE=$(echo -n "$CODE_VERIFIER" | openssl dgst -sha256 -binary | openssl base64 | tr '+/' '-_' | tr -d '=')
STATE=$(gen_base64url)

AUTH_URL="https://accounts.google.com/o/oauth2/v2/auth?client_id=$CLIENT_ID&redirect_uri=$REDIRECT_URI&response_type=code&scope=$(echo $SCOPE | sed 's/ /%20/g')&access_type=offline&prompt=consent&state=$STATE&code_challenge=$CODE_CHALLENGE&code_challenge_method=S256"

echo "To authenticate for $DESCRIPTION, visit the following URL:"
echo ""
echo "$AUTH_URL"
echo ""
echo "1. Authorize the application in your browser."
if [ "$MODE" == "antigravity" ]; then
    echo "2. You will be redirected to localhost:51121 (which will fail to load)."
else
    echo "2. You will be redirected to localhost (which will fail to load)."
fi
echo "3. Copy the FULL URL from your browser's address bar and paste it below."
echo ""
read -p "Enter redirect URL: " URL

# Extract code from URL
CODE=$(echo "$URL" | grep -o 'code=[^&]*' | cut -d'=' -f2)

if [ -z "$CODE" ]; then
    echo "Error: Could not find 'code' parameter in the URL."
    exit 1
fi

echo "Exchanging code for tokens..."

RESPONSE=$(curl -s -X POST https://oauth2.googleapis.com/token \
    -d "code=$CODE" \
    -d "client_id=$CLIENT_ID" \
    -d "client_secret=$CLIENT_SECRET" \
    -d "redirect_uri=$REDIRECT_URI" \
    -d "grant_type=authorization_code" \
    -d "code_verifier=$CODE_VERIFIER")

ACCESS_TOKEN=$(echo $RESPONSE | grep -o '"access_token": "[^"]*' | cut -d'"' -f4)
REFRESH_TOKEN=$(echo $RESPONSE | grep -o '"refresh_token": "[^"]*' | cut -d'"' -f4)
EXPIRES_IN=$(echo $RESPONSE | grep -o '"expires_in": [0-9]*' | cut -d' ' -f2)

if [ -z "$ACCESS_TOKEN" ]; then
    echo "Error: Failed to get access token."
    echo "$RESPONSE"
    exit 1
fi

EXPIRY_TIME=$(($(date +%s) + ${EXPIRES_IN:-3600}))

cat <<EOF > "$TOKEN_FILE"
{
    "access_token": "$ACCESS_TOKEN",
    "refresh_token": "$REFRESH_TOKEN",
    "expiry_time": $EXPIRY_TIME
}
EOF

chmod 600 "$TOKEN_FILE"
echo "Authentication successful. Tokens saved to $TOKEN_FILE"
