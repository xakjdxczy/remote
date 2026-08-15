# Remote

A small, modern full-stack **Tasks** starter app used to bootstrap and validate the
development environment for this repository.

- **`server/`** — Express + TypeScript REST API (`/api/tasks`, `/api/health`).
- **`client/`** — Vite + React + TypeScript UI that talks to the API through a dev proxy.

The project is an npm workspaces monorepo.

## Prerequisites

- Node.js >= 20 (developed against Node 22)
- npm 10+

## Getting started

```bash
npm ci        # install all workspace dependencies
npm run dev   # start API (:3001) and client (:5173) together
```

Then open http://localhost:5173 and add a task. The client proxies `/api/*`
requests to the API on port `3001`.

## Common commands

| Command | Description |
| --- | --- |
| `npm run dev` | Run server + client dev servers concurrently |
| `npm run dev:server` | Run only the API (`http://localhost:3001`) |
| `npm run dev:client` | Run only the client (`http://localhost:5173`) |
| `npm run build` | Type-check and build both workspaces |
| `npm run lint` | Lint both workspaces with ESLint |
| `npm test` | Run the server API tests (Vitest + Supertest) |

## API

| Method | Path | Description |
| --- | --- | --- |
| `GET` | `/api/health` | Health check |
| `GET` | `/api/tasks` | List tasks |
| `POST` | `/api/tasks` | Create a task (`{ "title": "..." }`) |
| `PATCH` | `/api/tasks/:id` | Update `title` and/or `done` |
| `DELETE` | `/api/tasks/:id` | Delete a task |

Tasks are stored in memory, so restarting the API resets the list to its seed data.
