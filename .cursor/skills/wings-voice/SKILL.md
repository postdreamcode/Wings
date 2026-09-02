---
name: wings-voice
description: >-
  Wings on-device voice: sherpa-onnx KWS, speaker enroll, media-stream
  earpiece chimes, always-listen vs PTT. Use when changing Valkyrie,
  open/close/hug/home/stop/flap, keywords.txt, WingsMic, WingsChime, or Voice tab.
---

# Wings voice

Hands-free should feel like a headset assistant: **Valkyrie** (chime) → short command (ok/no chime) → wings move. Do not send people back to hold-to-talk to make spotting work.

## Architecture (do not flatten)

1. **Two KWS streams, always fed.** Wake-only stream and command-only stream both see every mic chunk. Do not rebuild/switch streams on Valkyrie — that stalls the audio thread and drops "open". Ignore command hits until the wake window is open.
2. **Never run CampPlus on the mic callback.** Speaker lock is on wake, async after a yield. Command cosine is display-only. Blocking embed on PCM is why it felt seconds-late. HFP/earpiece cosine is often 0.30–0.45; keep threshold around **0.35**, not 0.50. More enroll clips help a little; a bigger KWS model does not fix speaker score.
3. **Always-listen must not sit behind `_ptt`.** Hide PTT while always-listen (enroll still holds). `startAlways` clears `_ptt`.
4. **Mute KWS while the wings move.** `pathActive` / `seq` → `pauseListen` (drop decode only). Keep AudioRecord + SCO up. Never `setCommunicationDevice` or `MODE_IN_COMMUNICATION`.
5. **Idle radio:** BLE `lowPower` only after a move finishes. On a voice/RUN command, write GATT immediately (`withoutResponse`) and bump to `high` in parallel — never await `requestConnectionPriority` before the write. Do not request lowPower on every status notify.
6. **Earpiece mic is HFP/SCO.** A2DP cannot record. Start SCO with the mic, `AudioRecord.setPreferredDevice(TYPE_BLUETOOTH_SCO)`. Stop SCO only when listening fully stops. Chimes: `USAGE_VOICE_COMMUNICATION` while SCO is up, else `USAGE_MEDIA`. Voice tab shows **MIC:** — if it says PHONE, the bud is not the capture device. Enroll with that line showing the headset.
7. **keywords.txt** is BPE + `:boost #threshold @alias` (no space after `:`/`#`). Force-copy on launch. Do not put `VALKYRIE OPEN` in the same beam as `@wake`. Feed the command KWS stream only in the wake window.

## Enroll

Hold ~1s per prompt, **earpiece in, phone away from the mouth**. The Voice tab **MIC:** line must show the headset, not PHONE. Failed embed stays on the same word. Retrain replaces by name; never delete the profile first. Speaker embeddings are *who*, not the word — enroll on the same mic you will use live.

## Do not

- Rebuild or switch KWS streams on wake. Feed both streams every chunk; reset only the stream that fired.
- Add a cloud STT or a new Dart audio package without Ken asking.
- Treat UI "SPEAKER NO" on a short command as a KWS miss when always-listen already gated wake.
- Swap in a larger KWS/ASR model to fix a low CampPlus score. That score is *who*, not the word.
- `setCommunicationDevice` or `MODE_IN_COMMUNICATION` to “fix” routing. Start SCO with the recorder only; stop it only when listen ends.

Firmware poses/NVS are a different layer. Voice never moves servos unless live is on, a profile exists (or debug bypass), and SETUP has been homed (not cold).
