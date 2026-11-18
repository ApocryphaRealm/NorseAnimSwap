# NorseAnimSwap

Replace animations by condition and priority, for the Norse overhaul (NORSE: Oath of Blood, UE4SS v4.0.0). Modelled
on Open Animation Replacer (Skyrim): when the game starts an animation, the highest-priority rule whose condition holds
wins. Nothing in the game's files changes.

```ini
[Enemies cheer instead of looking at the sky]
Replace = A_Body_Emote_LookAtSky01 -> /Game/Norse/Characters/MetaHumans/Body/Animations/Actions/A_Body_Cheer.A_Body_Cheer
When = IsEnemy
Priority = 10
```

- `Replace = <animation> -> <new> [| <new> ...]`: left, the montage's name or the first animation inside it (Norse
  plays most animations - attacks, abilities, reactions - as a montage made on the spot), `*` wildcards; right, one
  replacement or several (`|`, a random pick each time). Repeatable.
- `When` (NorseConditions, about the unit playing it), `Priority` (higher wins; equal keeps load order), `Chance`.

How: every montage start ends in the AnimInstance's virtual Montage_PlayInternal (vtable slot 95, +0x2f8, mapped by
NorsePayload); the hook chooses before the original runs. A montage asset can be replaced by another montage (the
argument is swapped); a montage made on the spot gets the new sequence put inside it (its AnimReference, AnimEndTime
and SequenceLength) - the game keeps the same montage object, so whatever it waits on still ends, and the new
animation's notifies drive the timing. Replacements are loaded on first use and loaded again when a map change frees
them. Conditions are compiled at map load: this mod loads before NorseConditions.

Console: `anim.log on|off` (log every animation start: unit, montage, animation - how to find names), `anim.list`,
`anim.reload`. Author guide: `package\anims\README.txt`; tested rules: `docs\examples\test-anims.ini`.

0.1.0 tested in game (TEST-RESULT-0.1.0.txt). Not yet: montage-for-montage, variants, the game's own attacks in a fight.
NorsePayload hooks the same function; when NorseAnimSwap's hook is the inner one, NorsePayload sees the original
animation's name.
