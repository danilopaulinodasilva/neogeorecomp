# Roadmap

## Next

- **Sound in `--record`.** Sound plays in the window and `--wav` writes it; `--record` still makes silent videos.
- **Load ROMs from zip files**, so users can point at `mslug.zip` + `neogeo.zip` instead of unzipping.

## Later

- AES mode beyond what KOF98 needed (`ng_game_t.aes`): AES-specific I/O, the Unibios.
- Memory card.
- Protected and encrypted sets (CMC, PVC, SMA): they need decryption before the recompiler sees the code.
- PAL timing and `TIMERSTOP`.
- Flag liveness in the emitter, to drop dead flag computations (speed; correctness doesn't need it).
- Save states, gamepad button remapping and shader filters in the pause menu, in the style of lynxrecomp's. Toggling widescreen would need a restart, since patches apply at load.

## Out of scope

- Shipping ROMs, system ROMs, or recompiled output. Users always supply their own dump.
- Title-specific patches in the toolkit. Game repos describe their ROM layout and nothing else.
