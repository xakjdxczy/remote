import { createApp } from "./app.js";
import { TaskStore } from "./store.js";

const PORT = Number(process.env.PORT ?? 3001);

const store = new TaskStore([
  "Read the project README",
  "Run the dev servers",
  "Add your first task",
]);

const app = createApp(store);

app.listen(PORT, () => {
  console.log(`[server] Tasks API listening on http://localhost:${PORT}`);
});
