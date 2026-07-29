# Helm architecture and extension points

## Operation identity

Every generation request carries a session ID and immutable `TurnOptions`:

```text
mode          chat | agent
effort        low | medium | high
agent_id      optional reusable agent profile
resource_ids  transient attachments for this turn
```

The agent loop never reads “whatever session is currently selected.” All history,
writes, jobs, progress events, and completion callbacks use the captured session ID.
This allows the UI to switch conversations while a turn runs without cross-session corruption.

## Workspace layer

`WorkspaceStore` owns four resource kinds:

```text
rag
attachment
agent_config
tool_pack
```

Agent profiles reference resource IDs rather than paths. This leaves storage,
indexing, encryption, and remote backing replaceable without changing the UI protocol.

## Council AI path

`AgentProfile` already contains:

```text
model_id       current single-model compatibility field
model_ids      ordered member models
coordinator    single | council | router
```

A future council implementation should add an `AgentCoordinator` between `Bridge`
and `AgentLoop`:

```text
user turn
  -> coordinator fans out immutable subturns to model workers
  -> member responses are stored as internal artifacts
  -> synthesizer model receives the member outputs
  -> one final response is appended to the user session
```

The existing session, workspace, UI, and tool contracts can remain unchanged. The
engine boundary would expand from one `Engine` to an `EnginePool` keyed by model ID.

## Multimodal path

Attachments already carry stable resource IDs and metadata. Add a `ContentExtractor`
interface with adapters such as:

```text
TextExtractor
PdfExtractor
ImageVisionEncoder
AudioTranscriber
```

`WorkspaceStore::context_for` can then request extracted chunks from an index rather
than reading text files directly. No composer or session schema change is required.

## Tool-pack path

Imported tools implement a narrow process protocol:

```text
stdin  one JSON request line
stdout one JSON result or plain-text result
```

The runtime converts manifest parameter definitions into the same registry entries,
prompt docs, grammar, dispatch, cancellation, and progress infrastructure used by
built-in tools. Additional adapters can be added beside `stdio-json`, for example:

```text
MCP client
HTTP localhost adapter
Python embedded adapter
PowerShell module adapter
```

## Permissions path

The current build has a global `allow_process_tools` switch. A later permissions
layer should move capability policy into the agent profile and turn options:

```text
read_paths
write_paths
allowed_executables
network_origins
confirmation_policy
```

That policy should be checked by tool implementations, not trusted solely to prompts.

## 1.2 external tool runtime

The native C++ process remains the authority for tool registration, validation,
timeouts, cancellation, and result delivery. Optional open-source integrations are
adapted through `tools_runtime/helm_tools.py` and launched with explicit argument
arrays by `run_process_capture`; Helm does not build arbitrary command strings.

```text
AgentLoop
  -> Registry built-in tool
  -> external_tools.cpp policy + argument validation
  -> local executable or helm_tools.py subcommand
  -> captured UTF-8 result
  -> normal tool-result turn
```

The installer places the shared runtime, Playwright browsers, ComfyUI, whisper.cpp,
Piper, FFmpeg, yt-dlp, and ripgrep under `F:\AI Tools`. Individual paths can be
overridden without changing the application source.

## Runtime settings boundary

Shipped defaults remain in `config/app.json`. User-edited model runtime controls are
stored separately in `%LOCALAPPDATA%\Helm\runtime.json`. Saving settings reloads the
active model so context length, GPU layers, batch sizes, Flash Attention, and KV-cache
placement/types are applied together rather than partially mutating a live context.

## Streaming viewport contract

The transcript follows generation only while the user is already near the bottom.
A deliberate upward scroll disables follow mode until the user returns to the bottom
or selects **New output**. Token, tool-progress, and completion events all use the
same viewport rule, so no event class can unexpectedly steal the user's scroll
position.
