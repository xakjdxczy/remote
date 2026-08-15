export interface Task {
  id: string;
  title: string;
  done: boolean;
  createdAt: string;
}

async function handle<T>(res: Response): Promise<T> {
  if (!res.ok) {
    let message = `Request failed (${res.status})`;
    try {
      const body = await res.json();
      if (body?.error) message = body.error;
    } catch {
      // ignore non-JSON error bodies
    }
    throw new Error(message);
  }
  if (res.status === 204) return undefined as T;
  return res.json() as Promise<T>;
}

export const api = {
  async list(): Promise<Task[]> {
    return handle<Task[]>(await fetch("/api/tasks"));
  },
  async create(title: string): Promise<Task> {
    return handle<Task>(
      await fetch("/api/tasks", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ title }),
      }),
    );
  },
  async setDone(id: string, done: boolean): Promise<Task> {
    return handle<Task>(
      await fetch(`/api/tasks/${id}`, {
        method: "PATCH",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ done }),
      }),
    );
  },
  async remove(id: string): Promise<void> {
    await handle<void>(await fetch(`/api/tasks/${id}`, { method: "DELETE" }));
  },
};
