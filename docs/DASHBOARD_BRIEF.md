# Dashboard brief

Paste this to a coding agent (with ARCHITECTURE.md and openapi.json attached) to build the carer dashboard.

Build a web dashboard for a voice companion device for elderly people. The backend
already exists: a FastAPI "hub" at `http://<laptop-ip>:8000` (CORS open). Do NOT
build a backend, an MCP server or a database. Only build the frontend, against the
API in ARCHITECTURE.md / openapi.json.

Stack: Vite + React + TypeScript + Tailwind. Put the base URL in one env var
`VITE_HUB_URL` (default `http://localhost:8000`).

Data:
- On load: GET /api/state, /api/meds, /api/moods, /api/readings.
- Then open a WebSocket to `/ws`. Every message is `{event, data, state, at}`:
  replace your state with `state`, and on `med_taken` / `mood` / `reading`
  refetch that list (or append). Reconnect every 2 s if the socket drops.

Screens (one page is fine, big and calm, readable by a carer):
1. Status card: big "Medicine due" (red) or "All good" (green), next medicine time,
   room temperature and humidity.
2. Medicine history: today's doses and the last 7 days, with time and source
   (button / voice / dashboard). Show missed doses (a scheduled time with no intake).
3. Mood: a line chart of score (1-5) over time and the latest notes ("lonely", "in pain").
   Highlight a score of 1-2.
4. Controls: edit schedule (PUT /api/schedule), "Remind now" (POST /api/med/due),
   "Mark taken" (POST /api/med/taken), send a message to the device screen
   (POST /api/screen).

While the hub isn't reachable, run `python3 hub.py --fake` locally: same API, no hardware.
