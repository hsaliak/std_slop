#!/bin/bash

echo "| Permissions | Owner | Group | Size | Modification Time | Filename |"
echo "|--------------|-------|-------|------|-------------------|----------|"

ls -la | awk 'NR>1 {
    printf "| %-12s | %-7s | %-7s | %-6s | %-17s | %s |\n", $1, $3, $4, $5, $6 " " $7 " " $8, $9
}'