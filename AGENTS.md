# Working agreements

- Read PLANS.md before editing and update evidence/blockers before handoff.
- Keep this application separate from tab5_rtsp_logger. Repository separation alone does not
  establish a legal boundary for combined firmware; keep upstream notices in `licenses/`.
- Never commit identifiable images, recordings, credentials or private capture paths.
  Wi-Fi credentials belong in `firmware/sdkconfig.local`, which is git-ignored.
- This firmware does not classify people or scenes. It captures, displays and streams video and
  shows the local time. Never claim it detects, identifies or watches anyone, and never present it
  as a safety, medical or alarm device: it raises no alerts and a stalled stream looks the same as
  a quiet room.
- Errors and stale input must show as unknown or blank the preview, never as a stale picture
  presented as live.
- Host tests, IDF build, device flash, live camera capture, RTSP playback and endurance are
  distinct evidence. A successful build proves none of the others.
- Do not flash devices or merge PRs without explicit authorization.
- Run `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure`
  for relevant changes.
