from pathlib import Path

# Tokenizations taken from the working keywords.txt lines (not greedy BPE —
# longest-match splits OPEN as ▁O PE N, which is wrong).
ENC = {
    "HEY": "▁HE Y",
    "VALKYRIE": "▁VA L K Y RI E",
    "OPEN": "▁O P EN",
    "CLOSE": "▁C LO SE",
    "HUG": "▁HU G",
    "HOME": "▁HOME",
    "STOP": "▁ST O P",
    "FLAP": "▁F L A P",
}


def encode_phrase(phrase: str) -> str:
    return " ".join(ENC[w] for w in phrase.upper().split())


phrases = [
    ("HEY VALKYRIE", "wake", "2.5", "0.08"),
    ("VALKYRIE", "wake", "2.5", "0.10"),
    ("VALKYRIE OPEN", "v_open", "2.4", "0.08"),
    ("VALKYRIE CLOSE", "v_close", "2.4", "0.08"),
    ("VALKYRIE HUG", "v_hug", "2.4", "0.08"),
    ("VALKYRIE HOME", "v_home", "2.4", "0.08"),
    ("VALKYRIE STOP", "v_stop", "2.4", "0.08"),
    ("VALKYRIE FLAP", "v_flap", "2.4", "0.08"),
    ("OPEN", "c_open", "2.2", "0.10"),
    ("CLOSE", "c_close", "2.2", "0.12"),
    ("HUG", "c_hug", "2.2", "0.10"),
    ("HOME", "c_home", "2.2", "0.10"),
    ("STOP", "c_stop", "2.2", "0.10"),
    ("FLAP", "c_flap", "2.2", "0.10"),
]
lines = [
    encode_phrase(ph) + f" :{boost} #{thr} @{alias}"
    for ph, alias, boost, thr in phrases
]
# Extra wake spellings: same @wake, tokens that exist in this GigaSpeech BPE.
# Do not put VALKYRIE OPEN on the wake stream.
lines[2:2] = [
    "▁VA L KE RI E :2.4 #0.12 @wake",
    "▁VA L K I RI E :2.4 #0.12 @wake",
    "▁VA L ▁K Y RI E :2.4 #0.12 @wake",
]
out = Path(r"d:\Personal\Wings\app\assets\sherpa\keywords.txt")
out.write_text("\n".join(lines) + "\n", encoding="utf-8")
print("wrote", len(lines), "lines")
