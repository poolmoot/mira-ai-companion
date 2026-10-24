"""Point the agent's webhook tools at a new tunnel URL.

A Cloudflare quick tunnel gets a new random URL every time it starts. This
finds every webhook tool in your ElevenLabs workspace whose URL path starts
with /tools/ (the hub's tools) and swaps in the new host:

    ELEVENLABS_API_KEY=... python3 retarget_tools.py https://<new>.trycloudflare.com
"""

import json
import os
import sys
import urllib.request
from urllib.parse import urlsplit

API = "https://api.elevenlabs.io/v1/convai/tools"


def request(method, url, body=None):
    req = urllib.request.Request(
        url,
        method=method,
        data=json.dumps(body).encode() if body else None,
        headers={"xi-api-key": os.environ["ELEVENLABS_API_KEY"], "Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req) as r:
        return json.load(r)


def main():
    base = sys.argv[1].rstrip("/")
    for tool in request("GET", f"{API}?page_size=100").get("tools", []):
        cfg = tool["tool_config"]
        url = cfg.get("api_schema", {}).get("url", "")
        if cfg.get("type") != "webhook" or not urlsplit(url).path.startswith("/tools/"):
            continue
        cfg["api_schema"]["url"] = base + urlsplit(url).path
        request("PATCH", f"{API}/{tool['id']}", {"tool_config": cfg})
        print(f"{cfg['name']}: {cfg['api_schema']['url']}")


if __name__ == "__main__":
    main()
