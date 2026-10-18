# Image generation for Vela's Claude panel

Design proposal, 2026-10-18. This document specifies the feature; it does
not implement it or make API requests.

## Decision

Keep Claude as the conversational agent. Give it Vela MCP tools that submit
image generation and editing jobs to a selectable image provider. Vela owns credentials,
network requests, artifacts and previews. A bundled instruction resource
teaches Claude when and how to use these tools alongside scene queries.

Make the capability provider-neutral from the first release. GPT Image is
the first adapter, with Gemini, Hugging Face-hosted models and local inference
as additional adapters. Claude uses the same tool names and artifact IDs for
all of them. Changing providers must not require changing the Claude panel,
job lifecycle or USD authoring workflow.

For the OpenAI API-key adapter, use the Images API directly. Claude already
maintains the conversation; a second conversation is unnecessary. Editing
supplies explicit reference images on each request, including earlier results.
Other adapters can use conversational or asynchronous provider APIs internally.

### ChatGPT image generation versus API access

The desired experience is the image-making ability available in ChatGPT,
invoked by the embedded Claude. The integration needs a supported callable
service; the imagegen tool available to this development chat is not a tool
that Vela can inherit or forward to Claude.

Do not assume a ChatGPT subscription is an API credential. OpenAI documents
[Sign in with ChatGPT](https://developers.openai.com/siwc/quickstart), including
separately authorized Responses scopes for eligible plan-backed inference.
That identity flow alone does not establish image-generation eligibility.
Evaluate a ChatGPT-plan adapter as an explicit integration investigation:
verify client registration, native-app authorization, image-tool/model access,
limits and billing before offering it in Settings. If supported, it can use
Responses internally while exposing the same Vela image tools. Until verified,
offer the OpenAI API-key adapter and label its billing clearly. Do not use
browser automation, session cookies or undocumented ChatGPT endpoints.

The [official image guide](https://developers.openai.com/api/docs/guides/image-generation)
documents separate generation and edit endpoints. The
[generation reference](https://developers.openai.com/api/reference/resources/images/methods/generate)
documents model and output controls. Model availability and accepted options
must be checked during implementation; keep them in a provider capability
table. Proposed initial selection: `gpt-image-2.5-flare` for generation and
`gpt-image-2.5-sunburst` for editing, configurable in Settings. Avoid a moving
`latest` alias in saved job records.

## User experience and scope

Example requests:

- “Generate a seamless blue woven fabric texture for this chair.”
- “Use this view of the Ragnarok to make a sunset advertising concept.”
- “Keep the composition of that image, but make the background snowy.”
- “Apply that generated fabric to the selected seat.”

The first release generates images, edits from a viewport capture or an
earlier result, previews them and saves them. Applying a texture is a separate
scene edit using existing USD tools. Generated concept images are labeled as
such: they are not accurate renders or changes to scene geometry. Seamlessness
and preservation of geometry are requests to the model, not guarantees.
HDR lighting, physically calibrated PBR map sets, UV generation, masks and
automatic material graph construction are later work.

Settings → Image Generation provides provider connections, a default provider
and model, allowed model choices, quality limit, output library location and
request limits. Each connection identifies its host, authentication method,
cloud/local execution and billing source. Secrets go into macOS Keychain,
never AppSettings JSON, Claude context, CLI arguments, the Python namespace
or history. API-key usage is separate from the user's Claude session.

Add Generate Image… to the action registry and menus/palette. Its native
form can submit a prompt without the Claude CLI; the same job service backs
both paths. Generated Images… opens the document's artifact browser.
Each result has a thumbnail, full-size preview, prompt/options, provenance,
Save As…, Reveal in Finder and Use as Reference. The panel shows job state
and Cancel even if Claude has finished its turn.

## Fit with today's implementation

| Existing code | Design consequence |
|---|---|
| `claude_panel.mm` starts a private, document-bound MCP socket | Preserve that binding for jobs and artifacts; never choose a front window later. |
| `core/mcp_socket.cpp` synchronously dispatches `mcp::Handle` onto the main thread | Submission returns immediately; HTTP and decoding never run in this handler. |
| `core/claude_cli.cpp::Decide` allows unknown app tools when edit approval is off | Give image submission an explicit policy; scene-edit permission must not authorize paid requests. |
| `viewport_screenshot` returns PNG image content | Reuse its capture service to create a local reference artifact, without sending base64 through Claude. |
| Panel tool rows already show image thumbnails | Extend them for artifact/job identifiers and local previews. |
| Request completion coalesces scene undo entries | Generation creates no undo entry; later material application is its own scene request. |
| Python runs in-process with broad capabilities | Tool policy is a product boundary, not a sandbox against arbitrary Python code. |

## Architecture

```text
Claude panel / native Generate Image form
                  |
          MCP tools / actions
                  |
       ImageJobService (C++ state machine)
           |                    |
   ArtifactStore         ImageProvider interface
                                |
                       Provider registry
                         /     |      \
                     OpenAI  Gemini  HuggingFace / Local
```

Keep job records, validation, limits and artifact metadata in the plain C++
core. Inject provider, storage and clock interfaces for tests. The macOS
adapter uses NSURLSession for cancellable asynchronous HTTP and ImageIO for
validated decoding/thumbnails; no new SDK or embedded Python package is
needed. Worker callbacks post small state updates to the main queue. No
worker retains a `UsdStage`, renderer or AppKit view.

### Provider contract and capabilities

An adapter implements `DescribeModels`, `Validate`, `Submit`, `Cancel` and
completion/progress callbacks. `Submit` receives immutable input bytes and
normalized options, and returns a cancellable handle. It may internally poll
a remote job; MCP clients still see one Vela job. Provider errors map to the
common error vocabulary, retaining sanitized provider codes for diagnostics.

Each configured connection has a stable ID distinct from its provider type
and model ID: a Hugging Face model may have multiple deployments. Discovery
uses an adapter catalog or authenticated discovery where supported; do not
assume every provider offers a model-list API.

Capabilities describe text-to-image, reference editing, maximum references,
masks, transparency, accepted formats, dimensions/aspect constraints, native
quality choices, seeds, progress and cancellation semantics. The common tool
supports a small shared vocabulary; adapters explicitly map it to native
options. Reject unsupported controls with alternatives instead of silently
discarding them. A seed is repeatable only to the extent a provider promises.
Provider-specific controls live in a validated, namespaced `provider_options`
object described by discovery. Claude must not invent arbitrary HTTP fields.

| Proposed adapter | Integration responsibility |
|---|---|
| OpenAI API key | Images generation/edit requests; capability table for selected GPT Image model. |
| ChatGPT plan (investigation) | Verify supported image access and authorization; use a separate adapter if feasible. |
| Gemini | Translate the selected model's request/response and reference-image semantics into the common contract. |
| Hugging Face hosted | Identify model plus deployment/backend; normalize its actual inputs and output delivery. |
| Local inference | Connect to an explicitly configured local service; advertise its installed models and capabilities. |

These are adapter requirements, not claims that all listed providers support
every feature. Verify each provider's official contract when implementing it.
Add a second adapter before declaring the abstraction stable. A fake adapter
alone cannot establish interoperability.

The user selects defaults and permitted alternatives. Claude may request a
specific configured connection/model when asked, or explain that the chosen
model lacks editing. Never silently switch provider: that changes data
destination, credentials and cost. Approval binds the resolved connection,
model and capability version. A local-only preference prohibits cloud fallback.
Connection endpoints are configured by the user in Settings, never supplied
as arbitrary tool arguments. Adapters validate any provider-returned download
URLs and redirect hosts; local service credentials never follow redirects.

Capture the viewport on the main thread using the existing offscreen export
service. Bound reference dimensions to avoid a long capture. Once captured,
the immutable bytes, camera/time description and source document identity
belong to the artifact; later camera or selection changes cannot alter them.

## Tool contract

Names below are proposals. All argument schemas reject unknown fields and
validate lengths, counts and capability combinations before submission.

| Tool | Input | Result / behavior |
|---|---|---|
| `list_image_models` | Optional configured connection ID | Enabled connections/models, defaults, readiness and capability schemas; no credentials. |
| `capture_image_reference` | Optional width/height | Immutable viewport artifact ID and capture metadata. Local only. |
| `generate_image` | `request_id`, `prompt`, `purpose`, `reference_ids` (default empty), optional `connection_id`, `model`, `size`, `quality`, `background`, `provider_options` | Job ID and resolved provider/model; references require editing capability. One output per job initially. |
| `get_image_job` | `job_id` | State, sanitized error, or completed artifact metadata. Immediate query. |
| `cancel_image_job` | `job_id` | Requests cancellation; idempotent. |
| `list_generated_images` | Optional pagination cursor | Artifacts for this document, including prior sessions. |
| `get_generated_image` | `artifact_id`, optional `include_preview` | Metadata and, if requested, a bounded MCP image preview. |

Tools use the panel-bound document; reject an explicit conflicting document.
The public MCP server uses an explicitly resolved document and the same
service/policy. IDs are opaque and resolved by the store, not filesystem
paths or URLs. References must belong to this document or have been explicitly
imported through a native picker. Reuse the same artifact for successive edits.

Submission example (the request ID is scoped to document + conversation):

```json
{
  "request_id": "chair-fabric-1",
  "prompt": "Top-down blue woven fabric swatch, even lighting, seamless edges",
  "purpose": "base_color_texture",
  "reference_ids": [],
  "size": "1024x1024",
  "quality": "medium"
}
```

```json
{
  "job_id": "job_opaque_id",
  "state": "queued",
  "poll_after_ms": 2000
}
```

Completed metadata includes artifact ID, dimensions, MIME type, color space,
local read-only asset path, prompt, resolved connection/provider/model/options, reference IDs and
provider request ID. Actual usage is recorded when provided; absent usage is
unknown. Preview bytes are returned only on explicit retrieval. Full output
bytes never enter tool text or JSONL history.

The job tool does not wait for completion. Claude may query a few times while
working, then finish with “The image is generating.” Do not introduce a tight
poll loop or depend on an unverified CLI notification capability. Vela updates
the result card independently; a “Continue with this image” button sends a new
user turn containing the artifact ID when the user wants further work. A
future version can add verified asynchronous agent continuation.

## Request policy, credentials and limits

Enabling generation explains paid calls and reference upload, and lets users
choose “Ask per request” (initial default) or automatic requests within their
configured limits. This policy is separate from “Ask before Claude edits.”
Approval shows provider/data destination, billing source, prompt, model,
quality, reference thumbnails and output count.
Cloud upload occurs only after approval. Capture and preview stay local.

Enforce policy inside ImageJobService, including calls from actions, Python
and external MCP clients. A denied or missing approval creates no HTTP request.
For the initial design, service submission can return `awaiting_approval`;
Vela displays a job approval card and the job advances only after a native
decision. External clients receive the same pending state. Generic CLI tool
approval should allow creating this pending job, without duplicating the
paid-request card. Never pass an agent-supplied `approved` flag.

Snapshot the approved prompt/options/reference IDs so approval cannot be reused
for changed content. Bound prompt/reference bytes, output pixels, queue length,
quality and concurrent jobs (one active per document initially). A session
request cap bounds automatic spending; label it as a request cap, not a dollar
guarantee. Show usage and estimates only when supported by verified pricing.
Credentials are retrieved by the provider at send time, with HTTP authorization
and raw responses redacted from logs. Never fetch agent-provided remote URLs.

## Job lifecycle and failures

States: `awaiting_approval → queued → running → succeeded | failed | cancelled`.
Rejection ends as `cancelled` with reason `denied`. Persist state transitions.
Repeat submission with the same request ID and identical normalized payload
returns the existing job; a different payload is a conflict. This prevents
duplicate local submissions, not exactly-once provider execution.

Cancel aborts the local task and discards incomplete output; provider work
may already have incurred charges. Claude Stop cancels jobs owned by its
active turn, including pending approvals; closing the document cancels its
active jobs. Completed artifacts remain. On app restart, unfinished requests
become `failed/interrupted`; never silently resubmit them.

If a provider exposes a durable job handle, persist it and reconcile status
after restart where supported, without submitting a new generation. Otherwise
unfinished requests retain the interrupted outcome above.

Do not automatically retry ambiguous timeouts or disconnected submissions:
the provider might already have processed them. Return an explicit retry
action for the user. Surface authentication, model access, quota/rate limits,
content rejection, transport errors and invalid output as distinct sanitized
errors. Apply a finite timeout and response byte limits. Validate image type,
pixel count and successful decode before atomically publishing an artifact;
remove temporary files on every failure path.

## Artifacts and USD authoring

Store immutable originals and small thumbnails under Application Support/Vela/
Generated Images/<document UUID>/<artifact UUID>/, with a versioned manifest.
Use a persistent document UUID in Vela's document/history association, not a
stage path alone (unsaved documents and Save As need stable ownership).
Record hashes, creation time, source scene path, time code and camera snapshot.
Do not modify an externally mounted scene directory during generation.

Before binding a generated texture, copy it through Save As… or a project
asset destination chosen by the user. Author a relative `SdfAssetPath` from
the destination layer when possible. Library deletion must warn about known
references; undoing a material edit retains the exported asset so redo works.
Do not advertise an Application Support cache path as a portable scene asset.

For a base-color workflow, Claude queries UVs, material bindings and shader
type first; it applies the file through `run_python` in a later request.
Respect the current edit target and existing Preview Surface or MaterialX
network; do not assume every selected mesh uses the same shader. Record sRGB
for generated color imagery, and require explicit interpretation for data
textures. Do not infer normals, roughness or HDR radiance from an ordinary
generated color image. Unsupported graphs should produce a useful explanation.

## Teaching Claude

Bundle `resources/image_generation.md` and append it to the existing panel
system prompt when the feature is enabled. This uses the established launch
adapter and avoids depending on personal Claude skill installations. Move it
to a packaged Claude skill alongside Phase 8 skills once packaging is verified.
This resource describes Vela tools; it does not expose this development
environment's imagegen tool to the embedded agent.

Instructions should cover intent → query context → capture optional reference
→ submit → preview → optional separate scene application. Explicitly teach:
generation versus viewport export; no hidden scene uploads; reuse artifact IDs
for edits; pending jobs; errors without retry loops; portable texture assets;
no claim that a concept image changed the USD scene. Include examples for the
Ragnarok concept, chair fabric and iterative editing. Keep provider details
and credentials out of the prompt; tools expose supported options/status.
Teach discovery with `list_image_models`, explicit provider choice and
capability errors. Examples should include changing providers while retaining
the scene context and using a previous provider's artifact as a reference
only when the new provider supports it and upload policy allows it.

## Implementation sequence and acceptance

1. **Service and fake provider:** add `core/image_jobs.*`, artifact store and
   provider interface. Test validation, policy through every entry point,
   request deduplication, document isolation, cancel/complete races and restart.
2. **macOS providers and setup:** Keychain adapter, NSURLSession adapter,
   Settings and native Generate Image action. Test HTTP fixtures for generation
   and multipart editing, redaction, invalid/oversized output and timeout.
   Implement a second provider (Gemini or Hugging Face) with the same tests
   plus capability mismatch and cross-provider reference cases. Investigate
   ChatGPT-plan image access separately and document the verified outcome.
3. **MCP and panel:** add tools to `core/mcp.cpp`, explicit classification in
   `core/claude_cli.cpp`, job cards/history references in `claude_panel.mm`, and
   bundled instructions. Extend the stand-in CLI UI test for approval, async
   completion, preview replay, Stop, document close and two-window isolation.
4. **End-to-end evaluation:** generate fabric, create a viewport-based concept,
   edit an earlier artifact, save a texture and bind/undo/redo it. Verify UI
   responsiveness and portable asset resolution after moving the scene folder.
   Unit/UI tests use a fake provider; real paid API smoke tests are opt-in.

Implementation completion requires Vela tests, native settings/menu checks and
manual preview inspection. This design-only change needs documentation checks.
Before coding, confirm API account/model access, accepted option combinations
and the minimum Claude CLI's handling of returned image blocks. No live API
key, new dependency or scene modification is required to review this proposal.


## Implementation record (2026-10-18)

The initial implementation uses `core/image_jobs.*` for document-scoped jobs,
policy and a versioned artifact manifest, `image_provider.*` for native HTTP,
Keychain, image validation and the OpenAI/Gemini wire adapters, and
`image_generation.*` for setup, approval, previews, import/export and native
commands. `resources/image_generation.md` is compiled into the Claude prompt.
Seven MCP tools expose the capability without provider credentials.

The first built-in choices are GPT Image Flare and Gemini 2.5 Flash Image.
The latter uses Google's documented, supported `generateContent` route;
it accepts a smaller option set. Both accept up to three references in Vela.
Additional adapters register a `Provider` with a capability-bearing `Model`,
a readiness callback, and a submit callback returning cancellation. Completion
must return validated PNG and preview bytes on the main queue. Connection
IDs distinguish deployments; Claude discovers these through `list_image_models`.
Hugging Face and local inference adapters are extension points, not shipped
connections. Provider-specific options are currently rejected.

The ChatGPT-plan investigation found an explicit restriction in OpenAI's
[preview limitations](https://developers.openai.com/siwc/token-sharing-open-source/preview-limitations):
image generation is currently unsupported on that authorization route. The
implementation therefore offers API-key credentials rather than a ChatGPT
sign-in button. Revisit this adapter if the documented restriction changes.

Native entry points: Settings → Image Generation Settings…, and Window →
Generate Image…, Generated Images… and Image Generation Settings…. Enable the
feature and save the selected provider's API key in Keychain. Default policy
asks before every request; its controls cannot be changed by `set_settings`.
The result window remains available after Claude's turn finishes and offers
Continue with Claude. A separate full-size window, reference import, Save As
and Reveal support inspecting and exporting results. Scene authoring continues
through existing USD tools, after saving the texture into the project.

Completed artifacts and interrupted job outcomes survive restart. Saved-scene
paths are associated with UUIDs in Vela's user defaults; unsaved-document
associations last for the current app session. The current app does not offer
Save As for USD documents. Automatic asset dependency tracking remains future work. Approvals show all
reference thumbnails, with IDs retained in the job record. History may contain bounded preview
images using the existing panel image logging; full output images live only
in the artifact library. Model catalogs are deliberately small and static;
capability discovery describes the adapters, not live account access.

Validation includes core lifecycle/policy tests, request/response fixtures for
both providers, malformed image rejection, and an app/MCP fixture test for
cross-provider references, previews, cancellation, native commands, document
isolation, texture binding undo/redo and relative asset resolution after moving
a scene folder. The Claude stand-in test also discovers image models and submits a
viewport-based edit to native approval. Fixture providers are accepted only
under `--defaultsettings`; no network requests or real keys are used by tests.
Live provider generation and visual quality require a configured API account
and remain an explicit opt-in smoke check.

Viewport-capture crash follow-up: the supplied report aborts in Embree's
`ray.valid()` assertion during local `viewport_screenshot` rendering. A nonempty instanced-scene regression reproduced that assertion.
The Embree build now enables invalid-ray filtering; Vela also validates camera
clipping, transforms/projection and Embree's coordinate range before rendering.
Capture reports invalid-camera errors through MCP, and a live-app regression
checks both screenshot tools plus recovery after repairing the camera. The
report does not contain the original camera/ray values, so it does not establish
which scene input first produced the invalid ray.

Checkpoint limitation: the user clarified that the viewport became blank
before Claude switched to Embree as a troubleshooting step. The invalid-ray
crash protection does not explain or resolve that earlier blank viewport.
