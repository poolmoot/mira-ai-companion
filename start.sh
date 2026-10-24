#!/bin/zsh
# Starts the hub (talks to the board) and a public Cloudflare tunnel to it.
# If ELEVENLABS_API_KEY is set, Mira's tools are pointed at the new tunnel URL.
cd "$(dirname "$0")"

mkdir -p bin

# cloudflared: use an installed one, else fetch the official release once (no account needed).
CF=$(command -v cloudflared || echo ./bin/cloudflared)
if [ ! -x "$CF" ]; then
  case "$(uname -s)-$(uname -m)" in
    Darwin-arm64)  ASSET=cloudflared-darwin-arm64.tgz ;;
    Darwin-x86_64) ASSET=cloudflared-darwin-amd64.tgz ;;
    Linux-x86_64)  ASSET=cloudflared-linux-amd64 ;;
    Linux-aarch64) ASSET=cloudflared-linux-arm64 ;;
    *) echo "Install cloudflared yourself: https://github.com/cloudflare/cloudflared"; exit 1 ;;
  esac
  echo "Downloading cloudflared ($ASSET)..."
  curl -sL -o "bin/$ASSET" "https://github.com/cloudflare/cloudflared/releases/latest/download/$ASSET"
  case "$ASSET" in
    *.tgz) tar xzf "bin/$ASSET" -C bin && rm "bin/$ASSET" ;;
    *) mv "bin/$ASSET" bin/cloudflared ;;
  esac
  chmod +x bin/cloudflared
fi

atech free-port >/dev/null 2>&1
python3 hub/hub.py &
HUB=$!

"$CF" tunnel --url http://localhost:8000 --no-autoupdate > bin/tunnel.log 2>&1 &
TUNNEL=$!
trap "kill $HUB $TUNNEL" EXIT

until URL=$(grep -o 'https://[a-z0-9-]*\.trycloudflare\.com' bin/tunnel.log | head -1); [ -n "$URL" ]; do sleep 1; done
echo "\nPublic URL: $URL"
echo "Dashboard:  http://localhost:8000  (API docs at /docs)"

if [ -n "$ELEVENLABS_API_KEY" ]; then
  python3 hub/retarget_tools.py "$URL"
else
  echo "Set ELEVENLABS_API_KEY to repoint Mira's tools automatically, or update their URLs in ElevenLabs."
fi
wait
