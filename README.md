# Skyrim Remember

An SKSE plugin that gives Skyrim's NPCs a memory of what you look like.

> **Early work in progress.** The plugin builds and runs, NPCs form memories,
> and guards now show a small recognition notice. Broader gameplay reactions
> are not built yet. See
> [What works today](#what-works-today).

## The idea

In vanilla Skyrim, a guard knows exactly who you are the instant you walk into
view. It does not matter that you are wearing a full helmet, that you changed
into a completely different set of armour, or that the two of you have never
met. The game checks a number, not a face.

Skyrim Remember replaces that with something closer to how a person actually
recognises someone: **by what they saw, and by whether they can tell it is
still you.**

## How it works

### They have to actually see you

Nothing happens because you looked at a guard. It happens because the guard
looked at *you*.

Every half second the plugin checks the NPCs around you and asks, for each one:
is he alive, is he close enough, is he in the same place as you, has the game's
own detection system noticed you, and is there a clear line of sight between
you? Only when all of that is true does he get to look.

Hide behind a wall, stay in the dark, or walk away, and he learns nothing.

### They remember an outfit, not a name

When a guard sees you, he remembers what you were wearing — every piece, and
whether your face was showing.

Head, body, hands, feet, shield and jewellery all count, but not equally: what
you wear on your chest says far more about you than your gloves do. Two outfits
are compared by how much of that visible silhouette they share.

This works with armour from any mod, because the plugin looks at the actual
items you have equipped rather than at a list of names it was taught in advance.

### Covering your face changes everything

There is a difference between *"I have seen that outfit before"* and *"I know
who you are"*, and the plugin keeps the two apart.

- **Face showing.** The guard knows it is you. If you turn up later in
  completely different clothes, he still knows it is you, and now he knows that
  outfit too.
- **Face covered, first meeting.** He remembers an outfit worn by a stranger.
  He has no idea it was you.
- **Face covered, familiar outfit.** If the clothes are close enough to
  something he already connects to you, he puts it together.
- **Face covered, new outfit.** He does not recognise you. You are a stranger.

A hood or a full helmet hides your face. A circlet does not.

### How much they need to see depends on how well they know you

A guard who has barely met you needs a strong match — around 70% of your
silhouette — before he will accept that it is you. A guard who knows you well
gets there on about half that. Familiarity makes people easier to recognise,
and harder to fool.

So 70% is the starting threshold, not a value that changes arbitrarily. Seeing
your uncovered face in sufficiently spaced encounters builds familiarity; the
required outfit match then falls gradually, never below 50%. Repeated scan
ticks do not farm familiarity, and a covered face does not build it.

### Changing clothes in front of someone does not fool them

If a guard was watching you, recognised you, and kept watching while you
changed, he follows the change. Putting a helmet on in front of a man who is
staring straight at you does not make you a different person.

But it only works while he can genuinely see you. Break line of sight, or slip
out of his detection, and the connection is not made — he will see a stranger
in new clothes.

### What you wear says something about you

Walk into town in bandit furs and the guard who recognises you will trust you a
little less. Clothes do not make anyone *know* you; they colour what someone who
already knows you thinks of you.

An outfit implies a level, not a running total: a guard who sees you in bandit
armour for an hour is exactly as suspicious as one who saw you for a moment, and
once you change the feeling fades on its own. It never erases something
stronger — suspicion they already hold against you is not forgiven because you
changed into something respectable.

Only someone who recognises you holds it against you. A masked stranger in
bandit furs is a stranger's problem.

Different people read the same outfit differently: a guard grows suspicious, a
townsperson grows suspicious and a little afraid, and your follower does not
care. All of it lives in rule files, not in the plugin — see
[Impression rules](#impression-rules).

### Memory fades

Nothing is remembered forever, and each feeling fades at its own pace.
Suspicion goes within a couple of in-game days, fear within three, and how well
someone knows you fades over about a month of in-game time. Each guard keeps up
to eight outfits in mind; when he sees a ninth, the one he has not seen in
longest is forgotten.

What an NPC can feel is not a fixed list of four things. Guards deal in fear,
respect, suspicion and familiarity; a follower will deal in approval, loyalty
and trust instead. Everyone carries the same set of slots and simply leaves
untouched the ones their kind of person has no use for — which is also why an
NPC who changes role, like a housecarl who becomes your follower, keeps
everything they already felt about you rather than starting over.

All of this is stored inside your save file, so each save has its own memories.
Uninstalling leaves nothing behind in the game world.

### Travelling does not make you a stranger

A guard remembers you as *that guard* — not as "a guard in Whiterun". Meet the
same one out on the road and he is the same person, with the same memory of
you.

## What works today

**Working**

- NPCs observe you when they can actually see you
- Outfit and face recognition, with armour from any mod
- Recognising you through a change of clothes they watched
- Memory that fades with in-game time
- Everything saved in and restored from your save file
- Outfits that make NPCs suspicious — bandit armour by default — driven by rule
  files anyone can extend
- A HUD notice when a guard recognises you — including the first time — or
  stops recognising you. Guards recognising you together share one notice,
  and the same guard is not announced again within a few minutes. A guard
  meeting you for the first time is learning your face, not recognising you,
  so that sighting stays silent.

**Not built yet**

Right now NPCs remember, and that is all they do. There is no dialogue,
no crime or bounty behaviour, no gossip between NPCs, no shopkeeper reactions,
and followers do not use any of it. Those come later — this release is the
foundation they will be built on.

All nearby NPC profiles can form appearance memories and impressions. Guards —
of every hold — are the only profile with a reaction adapter at the moment, and
its first visible effect is deliberately limited to recognition notices.
Suspicion and fear are remembered, but nothing acts on them yet. Followers,
citizens and merchants are recognised as categories but do not react yet.

## Requirements

- Skyrim Special Edition or Anniversary Edition
- [SKSE64](https://skse.silverlock.org/)
- Address Library for SKSE Plugins

Install it like any other SKSE plugin: the `.dll` goes in
`Data/SKSE/Plugins/`, and the `data/SKSE/Plugins/SkyrimRemember/` folder from
this repository goes in `Data/SKSE/Plugins/SkyrimRemember/`. There is no plugin
file to enable and no scripts to clean, so it can be added to an existing save.
Without the rule file the plugin still works; outfits simply imply nothing.

The plugin writes a log to
`Documents/My Games/Skyrim Special Edition/SKSE/SkyrimRemember.log`, which is
the place to look if something is not behaving.

## Compatibility

It reads the equipment you already have on and the factions the game already
has, so armour and armour-slot mods work without any patch.

Guards are recognised by the game's own guard flag — the one its crime and
dialogue systems use — not by a list of factions. Guards of every hold, guards
swapped by the Civil War questline, and guards added by other mods are all
treated as guards without a patch.

## Impression rules

What an outfit makes someone feel is data. Every file named `*_Rules.json` in
`Data/SKSE/Plugins/SkyrimRemember/` is loaded at startup, in file name order, so
a mod adds behaviour by shipping its own file instead of editing someone
else's. The default file is
[`SkyrimRemember_Rules.json`](data/SKSE/Plugins/SkyrimRemember/SkyrimRemember_Rules.json).

```json
{
  "impressions": [
    {
      "name": "Bandit armor",
      "forms": ["0x06F393~Skyrim.esm"],
      "projects": { "banditry": 40 }
    },
    {
      "name": "Hide and studded armor",
      "keywords": ["ArmorMaterialHide", "ArmorMaterialStudded"],
      "projects": { "banditry": 15 }
    }
  ],
  "temperaments": {
    "guard": { "banditry": { "suspicion": 1.0 } },
    "citizen": { "banditry": { "suspicion": 0.6, "fear": 0.4 } }
  }
}
```

**Impressions** say what an item suggests. Each one matches worn items by
`keywords` (EditorIDs) and/or `forms` (`0xLocalID~Plugin.esp`, the notation SPID
and KID use), and `projects` one or more named ideas at a strength from 0 to 100.
Projection names are free text: `banditry`, `threat`, `wealth` — whatever the
temperaments refer to.

Prefer keywords: `ArmorMaterialHide` covers every hide armour of every mod,
including ones released after your rule. Use `forms` only for items with no
distinguishing keyword — vanilla bandit armour, for example, carries none — or
give those items a keyword with KID and match that.

**Temperaments** say how each kind of person reads a projection. For a profile
(`guard`, `follower`, `citizen`, `merchant`, `generic`) and a projection, they
list the axes it becomes and a weight. `banditry 40` read by a guard with
`suspicion: 1.0` raises suspicion to 40; read by a citizen with `fear: 0.4` it
raises fear to 16. A profile with no entry for a projection ignores it.

The axes are `recognition`, `suspicion`, `fear`, `respect`, `approval`,
`loyalty`, `resentment` and `trust`.

How the numbers combine:

- Within one projection the strongest matching item wins; three bandit pieces
  are no more suspicious than the most suspicious one.
- Each axis is raised to the highest level any projection gives it, and never
  lowered.
- A later file's temperament entry replaces an earlier one for the same profile
  and projection, so a mod can retune the defaults. Impressions from every file
  add up.

A file with any error — invalid JSON, an unknown field, profile or axis, a
strength outside 0–100 — is rejected as a whole and the log says why; the other
files still load. A keyword or form that does not exist in your load order is
logged and skipped. Look for `Impression rules` in `SkyrimRemember.log`.

## Building from source

You will need CMake 3.25+, Ninja, LLVM (`clang-cl`), the MSVC build tools, a
Rust toolchain targeting `x86_64-pc-windows-msvc`, and vcpkg.

```
cmake --preset clang-release
cmake --build --preset clang-release
ctest --test-dir build/clang-release
```

The result is `build/clang-release/SkyrimRemember.dll`.

The recognition logic lives in a Rust core that knows nothing about Skyrim, so
most of it can be tested without the game running:

```
cargo test --manifest-path rust/Cargo.toml
```
