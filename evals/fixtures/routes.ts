import { Router } from "./router";
import { Logger } from "./logger";
import { Cache } from "./cache";

// TODO(alice): rate-limit the login route
export function registerRoutes(r: Router) {
  r.get("/health", () => "ok");
  r.post("/login", handleLogin);      // TODO(carol): add 2FA
  r.get("/users/:id", handleUser);
}

function handleLogin() { logger.log("warn", "login attempt"); return unwrap(readBody()); }
function handleUser() { return unwrap(fetchUser()); }
function unwrap<T>(x: T | null): T { if (x === null) throw new Error("null"); return x!; }
