# Security policy

Please report suspected security vulnerabilities through the project's private owner channel. Include the affected
version, reproduction steps, impact, and a minimal proof of concept when safe. Do not attach private audio,
credentials, or unrelated project files. A working public-release security route, such as GitHub private
vulnerability reporting or a verified security mailbox, must be published before the repository is made public.

Version 0.1.x receives security fixes while it is the current release line. The project issue tracker is appropriate
for ordinary defects, but not for an unpatched vulnerability that could compromise a DAW host or user data.

DessMetal parses user-selected WAV files inside the host process. Treat untrusted files as potentially hostile. The
0.1.1 release candidate bounds chunk and audio sizes, validates truncation/format arithmetic, and rejects non-finite
samples.
NAM schema conditioning dimensions are bounded and non-finite conditions are rejected. DessMetal exposes four
selectable bundled amp captures and four fixed bundled drive captures rather than a user-selected NAM path; the
hidden fifth amp-enum slot remains only as a compatibility alias for historical host state.

DessMetal 0.1.0 emitted both an original 14-parameter and a later 20-parameter positional state under the same
version. The 0.1.1 loader selects only exact supported byte layouts after bounded path parsing: it accepts both exact
0.1.0 layouts and the exact 20-parameter 0.1.1 layout. A plain/AU payload has no wrapper suffix; VST3 may append its
four-byte bypass value, which must be exactly 0 or 1 and is left for the wrapper. Legacy 14-value custom-amp indices 2/3 and the recognized
0.1.0 18/19-parameter work-in-progress layouts fail atomically rather than being partially reinterpreted. The current
20 parameter IDs, names, and order remain unchanged.

Amp and boost parameter callbacks do not perform model file I/O. They enqueue versioned atomic requests for
`OnIdle`; complete prepared DSP objects are published through atomic pending slots, adopted by the real-time render
thread only at block boundaries, and reclaimed from retired slots by a mutex-serialized non-real-time service.
Non-render consumers use request-token-published immutable metadata rather than unprotected live pointers. IR
selection uses the same publish/adopt/retire ownership protocol. Offline rendering synchronously uses the same
serialized service path so it does not depend on UI idle callbacks. See `SECURITY_REVIEW.md` for the release-candidate
review and residual risks.
