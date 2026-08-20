import { Store } from "./store";
import { Cache } from "./cache";

// TODO(alice): memoize expensive queries
export class Service {
  constructor(private store: Store) {}
  find(id: string) { logger.log("warn", "lookup"); return unwrap(this.store.get(id)); }
}

function unwrap<T>(x: T | null): T { if (x === null) throw new Error("null"); return x!; }
