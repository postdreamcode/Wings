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

phrases = [
    ("VALKYRIE", "wake"),
    ("OPEN", "c_open"),
    ("CLOSE", "c_close"),
    ("HUG", "c_hug"),
    ("HOME", "c_home"),
    ("STOP", "c_stop"),
    ("VALKYRIE OPEN", "open"),
    ("VALKYRIE CLOSE", "close"),
    ("VALKYRIE HUG", "hug"),
    ("VALKYRIE HOME", "home"),
    ("VALKYRIE STOP", "stop"),
]
# Official file is uppercase raw phrases
lines = []
for ph, alias in phrases:
    toks = encode_phrase(ph)
    line = " ".join(toks) + f" @{alias}"
    lines.append(line)

out = Path(r"d:\Personal\Wings\app\assets\sherpa\keywords.txt")
out.write_text("\n".join(lines) + "\n", encoding="utf-8")
print("wrote", len(lines), "lines")
