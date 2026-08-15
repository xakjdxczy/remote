import { describe, it, expect, beforeEach } from "vitest";
import request from "supertest";
import { createApp } from "./app.js";
import { TaskStore } from "./store.js";

function makeApp() {
  return createApp(new TaskStore());
}

describe("Tasks API", () => {
  let app: ReturnType<typeof makeApp>;

  beforeEach(() => {
    app = makeApp();
  });

  it("reports health", async () => {
    const res = await request(app).get("/api/health");
    expect(res.status).toBe(200);
    expect(res.body.status).toBe("ok");
  });

  it("starts with an empty task list", async () => {
    const res = await request(app).get("/api/tasks");
    expect(res.status).toBe(200);
    expect(res.body).toEqual([]);
  });

  it("creates a task", async () => {
    const res = await request(app).post("/api/tasks").send({ title: "Write tests" });
    expect(res.status).toBe(201);
    expect(res.body).toMatchObject({ title: "Write tests", done: false });
    expect(res.body.id).toBeTruthy();
  });

  it("rejects an empty title", async () => {
    const res = await request(app).post("/api/tasks").send({ title: "   " });
    expect(res.status).toBe(400);
  });

  it("toggles a task done", async () => {
    const created = await request(app).post("/api/tasks").send({ title: "Toggle me" });
    const id = created.body.id;
    const res = await request(app).patch(`/api/tasks/${id}`).send({ done: true });
    expect(res.status).toBe(200);
    expect(res.body.done).toBe(true);
  });

  it("deletes a task", async () => {
    const created = await request(app).post("/api/tasks").send({ title: "Delete me" });
    const id = created.body.id;
    const del = await request(app).delete(`/api/tasks/${id}`);
    expect(del.status).toBe(204);
    const list = await request(app).get("/api/tasks");
    expect(list.body).toEqual([]);
  });

  it("returns 404 for unknown task updates", async () => {
    const res = await request(app).patch("/api/tasks/does-not-exist").send({ done: true });
    expect(res.status).toBe(404);
  });
});
