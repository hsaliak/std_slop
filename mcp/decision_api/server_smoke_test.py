import json
import os
import subprocess
import sys
import tempfile


def request(identifier, method, params=None):
    value = {"jsonrpc": "2.0", "id": identifier, "method": method}
    if params is not None:
        value["params"] = params
    return json.dumps(value)


def main():
    server = sys.argv[1]
    help_result = request(
        2,
        "tools/call",
        {
            "name": "decision_help",
            "arguments": {"topic": "overview"},
            "_meta": {"io.modelcontextprotocol/protocolVersion": "2026-07-28"},
        },
    )
    list_result = request(
        1,
        "tools/list",
        {"_meta": {"io.modelcontextprotocol/protocolVersion": "2026-07-28"}},
    )
    invalid_decide = request(
        3,
        "tools/call",
        {
            "name": "decide",
            "arguments": {"state": "x", "questions": {}, "extra": True},
            "_meta": {"io.modelcontextprotocol/protocolVersion": "2026-07-28"},
        },
    )

    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", delete=False) as config_file:
        config_file.write(json.dumps({"apiKey": "test-secret", "model": "~typesafe/jev-latest"}))
        config_path = config_file.name
    try:
        os.chmod(config_path, 0o600)
        result = subprocess.run(
            [server, "--config", config_path],
            input="\n".join((list_result, help_result, invalid_decide)) + "\n",
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
        assert result.returncode == 0, result.stderr
        responses = [json.loads(line) for line in result.stdout.splitlines() if line]
        assert len(responses) == 3, result.stdout
        listing = json.dumps(responses[0])
        assert all(name in listing for name in ("decide", "decision_help", "decision_models")), listing
        assert "OpenRouter" in json.dumps(responses[1])
        assert responses[2]["result"]["isError"] is True
        assert "test-secret" not in result.stdout + result.stderr

        gateway = sys.argv[2]
        gateway_config = {
            "runTimeoutMs": 10000,
            "servers": [
                {
                    "alias": "decisions",
                    "transport": "stdio",
                    "command": server,
                    "args": ["--config", config_path],
                    "allowTools": ["decision_help"],
                }
            ],
        }
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", delete=False) as file:
            file.write(json.dumps(gateway_config))
            gateway_config_path = file.name
        try:
            os.chmod(gateway_config_path, 0o600)
            code = 'const guide = await decisions.decision_help({topic: "overview"}); return guide.structuredContent;'
            call = request(
                4,
                "tools/call",
                {
                    "name": "run_js",
                    "arguments": {"code": code, "input": {}},
                    "_meta": {"io.modelcontextprotocol/protocolVersion": "2026-07-28"},
                },
            )
            combined = subprocess.run(
                [gateway, "--config", gateway_config_path],
                input=call + "\n",
                capture_output=True,
                text=True,
                timeout=20,
                check=False,
            )
            assert combined.returncode == 0, combined.stderr
            assert "OpenRouter" in combined.stdout, combined.stdout
            assert "test-secret" not in combined.stdout + combined.stderr
        finally:
            os.unlink(gateway_config_path)

        os.chmod(config_path, 0o644)
        denied = subprocess.run(
            [server, "--config", config_path], capture_output=True, text=True, timeout=10, check=False
        )
        assert denied.returncode != 0
        assert "permissions" in denied.stderr
        assert "test-secret" not in denied.stdout + denied.stderr
    finally:
        os.unlink(config_path)


if __name__ == "__main__":
    main()
