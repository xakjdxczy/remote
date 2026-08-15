export interface Task {
  id: string;
  title: string;
  done: boolean;
  createdAt: string;
}

export interface CreateTaskInput {
  title: string;
}

export interface UpdateTaskInput {
  title?: string;
  done?: boolean;
}

let counter = 0;
function nextId(): string {
  counter += 1;
  return `${Date.now().toString(36)}-${counter.toString(36)}`;
}

export class TaskStore {
  private tasks: Task[] = [];

  constructor(seed: string[] = []) {
    for (const title of seed) {
      this.create({ title });
    }
  }

  list(): Task[] {
    return [...this.tasks].sort((a, b) => a.createdAt.localeCompare(b.createdAt));
  }

  get(id: string): Task | undefined {
    return this.tasks.find((t) => t.id === id);
  }

  create(input: CreateTaskInput): Task {
    const title = input.title?.trim();
    if (!title) {
      throw new ValidationError("title is required");
    }
    const task: Task = {
      id: nextId(),
      title,
      done: false,
      createdAt: new Date().toISOString(),
    };
    this.tasks.push(task);
    return task;
  }

  update(id: string, input: UpdateTaskInput): Task | undefined {
    const task = this.get(id);
    if (!task) return undefined;
    if (typeof input.title === "string") {
      const title = input.title.trim();
      if (!title) {
        throw new ValidationError("title cannot be empty");
      }
      task.title = title;
    }
    if (typeof input.done === "boolean") {
      task.done = input.done;
    }
    return task;
  }

  remove(id: string): boolean {
    const before = this.tasks.length;
    this.tasks = this.tasks.filter((t) => t.id !== id);
    return this.tasks.length < before;
  }
}

export class ValidationError extends Error {}
