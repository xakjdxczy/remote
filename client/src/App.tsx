import { useEffect, useMemo, useState } from "react";
import { api, type Task } from "./api";
import "./App.css";

export default function App() {
  const [tasks, setTasks] = useState<Task[]>([]);
  const [title, setTitle] = useState("");
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);

  async function refresh() {
    try {
      setTasks(await api.list());
      setError(null);
    } catch (err) {
      setError(err instanceof Error ? err.message : "Failed to load tasks");
    } finally {
      setLoading(false);
    }
  }

  useEffect(() => {
    refresh();
  }, []);

  const remaining = useMemo(() => tasks.filter((t) => !t.done).length, [tasks]);

  async function addTask(e: React.FormEvent) {
    e.preventDefault();
    const value = title.trim();
    if (!value) return;
    try {
      const created = await api.create(value);
      setTasks((prev) => [...prev, created]);
      setTitle("");
      setError(null);
    } catch (err) {
      setError(err instanceof Error ? err.message : "Failed to add task");
    }
  }

  async function toggle(task: Task) {
    try {
      const updated = await api.setDone(task.id, !task.done);
      setTasks((prev) => prev.map((t) => (t.id === updated.id ? updated : t)));
    } catch (err) {
      setError(err instanceof Error ? err.message : "Failed to update task");
    }
  }

  async function remove(task: Task) {
    try {
      await api.remove(task.id);
      setTasks((prev) => prev.filter((t) => t.id !== task.id));
    } catch (err) {
      setError(err instanceof Error ? err.message : "Failed to delete task");
    }
  }

  return (
    <div className="app">
      <div className="card">
        <header className="header">
          <h1>Tasks</h1>
          <p className="subtitle">
            {loading
              ? "Loading…"
              : `${remaining} of ${tasks.length} remaining`}
          </p>
        </header>

        <form className="add-form" onSubmit={addTask}>
          <input
            className="add-input"
            type="text"
            placeholder="What needs doing?"
            value={title}
            onChange={(e) => setTitle(e.target.value)}
            aria-label="New task title"
          />
          <button className="add-button" type="submit" disabled={!title.trim()}>
            Add
          </button>
        </form>

        {error && <div className="error" role="alert">{error}</div>}

        <ul className="task-list">
          {!loading && tasks.length === 0 && (
            <li className="empty">No tasks yet — add one above.</li>
          )}
          {tasks.map((task) => (
            <li key={task.id} className={`task ${task.done ? "done" : ""}`}>
              <label className="task-main">
                <input
                  type="checkbox"
                  checked={task.done}
                  onChange={() => toggle(task)}
                />
                <span className="task-title">{task.title}</span>
              </label>
              <button
                className="delete-button"
                onClick={() => remove(task)}
                aria-label={`Delete ${task.title}`}
              >
                ×
              </button>
            </li>
          ))}
        </ul>
      </div>
      <footer className="footnote">Remote · full-stack starter</footer>
    </div>
  );
}
