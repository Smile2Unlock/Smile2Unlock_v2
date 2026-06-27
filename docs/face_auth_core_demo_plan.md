# Face Authentication Core Demo Plan

## Purpose

Build the first usable core for Smile2Unlock as a face recognition authentication project.
The demo may use mock inputs, but the architecture must preserve the real domain flow:

1. enroll a face sample for a profile
2. extract a face embedding
3. persist the profile and embedding
4. capture or provide a probe face sample
5. compare the probe embedding with enrolled profiles
6. return an authentication decision

Profile names are labels only. They must not become the authentication mechanism.

## Confirmed Decisions

- First target: application demo and debugging tool.
- Recognition backend: pluggable backend, with a deterministic mock backend as the default.
- Core boundary:
  - Rust owns pure domain models, config/storage formats, matching logic, and authentication decisions.
  - C++ owns device/UI/platform orchestration.
  - Zig and assembly are optional backend or performance implementations.
- Profile storage: local readable files for the demo phase.
- Storage format: JSON for profile data and embeddings; TOML remains for app config.
- Demo security: no encryption yet, but storage versioning and interfaces must leave room for it.
- Demo operations:
  - enroll profile
  - list profiles
  - delete profile
  - run face authentication
- Demo embedding: generated deterministically from a mock face sample source.
- Demo matching: cosine similarity against all enrolled profiles, returning the best match.

## First Core Slice

The first implementation should build a complete, testable loop without real camera/model code.

### Rust Core

- Add a profile store module.
- Add a face embedding module.
- Add a face authentication pipeline module.
- Keep computation pure where practical:
  - embedding generation is deterministic from a mock face sample source
  - cosine similarity is a pure function
  - best-match selection is a pure function over profile snapshots
  - file I/O is isolated in the profile store
- Persist profiles as a versioned JSON document:

```json
{
  "version": 1,
  "profiles": [
    {
      "id": "stable-profile-id",
      "label": "user-visible-name",
      "embedding": [0.1, 0.2],
      "created_at_unix": 0
    }
  ]
}
```

### C ABI

Expose a narrow C ABI for the app layer:

- enroll a profile from a label and mock face sample
- delete a profile by id
- list profiles into a caller-owned buffer
- authenticate a mock face sample and return best-match data

For variable-length data, use caller-owned byte buffers and return the required byte count when the buffer is too small.
This avoids cross-language heap ownership in the first slice.

### C++ App Layer

- Add bridge methods around the new C ABI.
- Keep UI/device orchestration in C++.
- Keep core decisions in Rust.
- The app layer should pass a profile store path into Rust rather than letting Rust read environment-specific paths.

### Demo UI

The first UI does not need production design.
It must be able to:

- show the profile store path
- enroll a profile from a label and mock face sample source
- list current profiles
- delete a selected profile
- authenticate a probe face sample source and display:
  - accepted/rejected
  - best profile label/id
  - cosine score
  - reason

## Later Slices

### Real Face Backend

- Replace mock face sample source with camera frames.
- Add face detection and alignment.
- Add embedding model inference.
- Preserve the Rust pipeline API by passing embeddings or normalized face samples across the boundary.

### Liveness

- Add liveness result as an explicit pipeline input.
- Keep liveness failure distinct from similarity failure.

### Secure Storage

- Add encryption after the storage schema stabilizes.
- Keep key management outside the pure core.

### Login Integration

- Integrate PAM/credential provider only after the app demo can enroll and authenticate reliably.
- The login layer should call a narrow verification API and should not own profile matching logic.

## Immediate Acceptance Criteria

- Rust unit tests cover:
  - deterministic mock embeddings
  - cosine similarity
  - enroll/list/delete persistence
  - best-match authentication
- Xmake builds the Rust static library through Cargo.
- C++ bridge compiles against the stable C ABI header.
- Console demo can run the full profile loop.
- Slint demo can display profile/authentication state.
