"""The two helpers Demucs takes from dora.log (Dora, its training framework, isn't needed to separate)."""


def fatal(*args):
    raise RuntimeError(" ".join(str(a) for a in args))


def bold(text):
    return text
