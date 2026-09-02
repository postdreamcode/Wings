from pathlib import Path
import sentencepiece as spm

root = Path(r"d:\Personal\Wings\app\assets\sherpa\sherpa-onnx-kws-zipformer-gigaspeech-3.3M-2024-01-01")
sp = spm.SentencePieceProcessor(model_file=str(root / "bpe.model"))


def encode_phrase(phrase: str) -> list[str]:
    return sp.encode(phrase, out_type=str)


raw = (root / "keywords_raw.txt").read_text(encoding="utf-8").splitlines()
off = (root / "keywords.txt").read_text(encoding="utf-8").splitlines()
ok = True
for a, b in zip(raw, off):
    for cand in (a, a.upper(), a.lower(), a.title()):
        got = " ".join(encode_phrase(cand))
        if got == b:
            break
    else:
        got = " ".join(encode_phrase(a))
        Path(r"d:\Personal\Wings\app\assets\sherpa\_kw_check.txt").write_text(
            f"MISMATCH {a}\ngot  {got}\nwant {b}\n",
            encoding="utf-8",
        )
        ok = False
print("official match", ok)
if not ok:
    raise SystemExit(1)

# (phrase, alias, boost, threshold) — close is a bit harder so it
# does not steal a weak "open".
phrases = [
    ("VALKYRIE", "wake", "2.2", "0.14"),
    ("OPEN", "c_open", "2.0", "0.16"),
    ("CLOSE", "c_close", "2.0", "0.18"),
    ("HUG", "c_hug", "2.0", "0.16"),
    ("HOME", "c_home", "2.0", "0.16"),
    ("STOP", "c_stop", "2.0", "0.16"),
    ("FLAP", "c_flap", "2.0", "0.16"),
]
# Official file is uppercase raw phrases
lines = []
for ph, alias, boost, thr in phrases:
    toks = encode_phrase(ph)
    line = " ".join(toks) + f" :{boost} #{thr} @{alias}"
    lines.append(line)

out = Path(r"d:\Personal\Wings\app\assets\sherpa\keywords.txt")
out.write_text("\n".join(lines) + "\n", encoding="utf-8")
print("wrote", len(lines), "lines")
