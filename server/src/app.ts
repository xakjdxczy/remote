import express, { type Express } from "express";
import cors from "cors";
import { TaskStore, ValidationError } from "./store.js";

export function createApp(store: TaskStore): Express {
  const app = express();
  app.use(cors());
  app.use(express.json());

  app.get("/api/health", (_req, res) => {
    res.json({ status: "ok", uptime: process.uptime() });
  });

  app.get("/api/tasks", (_req, res) => {
    res.json(store.list());
  });

  app.post("/api/tasks", (req, res) => {
    try {
      const task = store.create({ title: req.body?.title });
      res.status(201).json(task);
    } catch (err) {
      if (err instanceof ValidationError) {
        res.status(400).json({ error: err.message });
        return;
      }
      throw err;
    }
  });

  app.patch("/api/tasks/:id", (req, res) => {
    try {
      const task = store.update(req.params.id, {
        title: req.body?.title,
        done: req.body?.done,
      });
      if (!task) {
        res.status(404).json({ error: "task not found" });
        return;
      }
      res.json(task);
    } catch (err) {
      if (err instanceof ValidationError) {
        res.status(400).json({ error: err.message });
        return;
      }
      throw err;
    }
  });

  app.delete("/api/tasks/:id", (req, res) => {
    const removed = store.remove(req.params.id);
    if (!removed) {
      res.status(404).json({ error: "task not found" });
      return;
    }
    res.status(204).end();
  });

  return app;
}
