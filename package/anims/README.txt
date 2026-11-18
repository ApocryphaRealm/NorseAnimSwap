NorseAnimSwap rules
===================

Every *.ini here replaces animations by condition, the way Open Animation Replacer does in Skyrim. Each [section] is
one rule (its name is only a label). When the game starts an animation, the highest-priority rule whose condition
holds wins; nothing in the game's files changes.

    [Brutes swing heavier]
    Replace = A_Outcast_Attack_01* -> /Game/MyPack/A_Heavy_01.A_Heavy_01 | /Game/MyPack/A_Heavy_02.A_Heavy_02
    When = UnitType(Outcast_Brute*)     ; optional, NorseConditions: about the unit playing it
    Priority = 100                      ; higher wins; default 0
    Chance = 100                        ; rolled each time

Left of -> : the animation being started - the montage's name, or the first animation inside it (Norse plays most
animations as a montage made on the spot), with * wildcards. Right: one replacement, or several separated by | (a
random pick each time). A sequence replaces a sequence; a montage asset (AM_*) can be replaced by another montage.
Find names with the console: anim.log on (logs every animation start: unit, montage, animation), anim.log off.
anim.list shows the rules; anim.reload reads them again.
