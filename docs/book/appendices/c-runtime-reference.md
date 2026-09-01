---
status: active
audience: public
last-verified: 2026-09-01
---

# Appendix C: Runtime Library Reference

The Zanna runtime surface is generated from the live runtime registry. For exact signatures, use the tool instead of this prose appendix:

```bash
zanna --dump-runtime-api
```

That command prints JSON for every runtime class, method, property, and signature available in the build you are using. It is the authoritative reference for code generation, documentation checks, and agent workflows.

For human-oriented documentation, see the [Runtime Library Index](../../zannalib/README.md) and the topic pages under `docs/zannalib/`, plus the [generated API reference](../../generated/runtime/README.md).

---

## Common Modules

- `Zanna.Terminal`: console input and output.
- `Zanna.IO.File`: whole-file text and byte IO.
- `Zanna.Text.Fmt`: integer, boolean, and numeric formatting helpers.
- `Zanna.Data.Json`: JSON parsing, validation, object helpers, and formatting.
- `Zanna.Data.Csv`: CSV line/table parsing and formatting.
- `Zanna.Math`: scalar math, bit helpers, random numbers, vectors, matrices, quaternions, splines, and easing.
- `Zanna.Collections.Seq`, `Map`, `Set`, `Bytes`: runtime collection classes.
- `Zanna.Network.Http`, `HttpReq`, `HttpRes`, `Tcp`, `Udp`, `WebSocket`: network APIs.
- `Zanna.Threads.Thread`, `Pool`, `Promise`, `Future`, `Async`, `Channel`, `SafeI64`: concurrency APIs.
- `Zanna.Graphics`, `Zanna.GUI`, `Zanna.Input`: application-facing UI, drawing, and input APIs.
- `Zanna.System.Environment`, `Zanna.System.Exec`: process and environment helpers.

Some older examples mention a `Zanna.Test` module or Zia `test`/`assert` syntax. Those are not present in the current runtime registry or language grammar; prefer the live dump and the checked examples in the main chapters.

---

## Checked Zia Example

```zia
bind Json = Zanna.Data.Json;
bind Fmt = Zanna.Text.Fmt;
bind Zanna.Terminal as Terminal;

func start() {
    var player = Json.NewObject();
    Json.SetStr(player, "name", "Hero");
    Json.SetInt(player, "score", 100);
    Json.SetBool(player, "active", true);

    var encoded = Json.Format(player);
    var decoded = Json.Parse(encoded);

    Terminal.Say(Json.GetStr(decoded, "name"));
    Terminal.Say(Fmt.Int(Json.GetInt(decoded, "score")));
    Terminal.Say(Fmt.Bool(Json.GetBool(decoded, "active")));
}
```

---

## Agent-Facing Commands

Use these commands when verifying examples or generating current reference data:

```bash
zanna check path/to/file.zia --diagnostic-format=json
zanna run path/to/file.zia --diagnostic-format=json
zanna --dump-runtime-api
zanna --dump-opcodes
zanna explain V-ZIA-UNDEFINED --json
```

`zanna run` currently accepts `.zia`, `.bas`, project directories, and `zanna.project` targets. IL snippets in this book are illustrative unless they are wrapped in a runnable frontend example.
