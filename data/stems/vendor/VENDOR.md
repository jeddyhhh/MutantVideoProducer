# Vendored for Stem Separation

Pure Python, unchanged except where noted. Each is under its own licence, in its folder.

- `demucs/`: Demucs 4.0.1 (Meta Platforms, MIT), the modules that separate (apply, pretrained, repo, states,
  htdemucs, hdemucs, demucs, spec, transformer, utils) and `remote/` (the model lists). PyPI has no wheel of it, and
  AI components install wheels only. Changed: `states.py` imports OmegaConf only where a training checkpoint is
  saved (not needed to separate).
- `julius/`: julius 0.2.7 (Alexandre Défossez, MIT).
- `openunmix/filtering.py`: Open-Unmix 1.3.0's Wiener filter (Inria, MIT); `__init__.py` is empty (the rest of the
  package isn't needed).
- `dora/log.py`: two one-line helpers in place of Dora's (our own).

The models Demucs downloads (htdemucs, htdemucs_ft) come from Meta (dl.fbaipublicfiles.com), checked by hash.
