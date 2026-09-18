# ROTGRAVE — design notes

Why the game is shaped the way it is. This is the argument, not the
manual; the manual is the README.

---

## 1. The mode is a spending problem wearing a shooter

Round survival against escalating health is, arithmetically, unwinnable:
`Undead.health_at` doubles roughly every seven rounds and never stops,
while a gun's damage is a constant. Everything interesting in the mode
comes from what you do with the gap.

The gap is closed three times, each more expensive than the last:

| | costs | buys |
| --- | --- | --- |
| Wall weapons | 1000–3000 | a gun that carries you to round ten |
| The box | 950 a pull | a gun you did not choose, sometimes the run |
| Pack-a-Punch | 5000 + the power | ×3.1 damage, and the next ten rounds |

And the same currency buys **ground**. That is the whole tension: a
thousand points is a submachine gun or it is the door to the street the
submachine gun is useless without.

## 2. Points are the score and the wallet

One number. It goes up when you play well and down when you spend, and
the leaderboard at `Tab` is the same number you are about to spend. A
player who is winning is a player with options; a player who is losing
cannot afford to stop losing. That feedback loop is worth more than any
separate currency would be.

Rates (`Game.PAY_*`): a hit 10, a kill 60, a headshot 100, a knife 130, a
revive 200, a board back on a window 10.

The board payment is the one worth defending. Rounds one to three have
nothing to shoot and nothing worth buying, and without it they are dead
time. With it they are a tutorial in where the windows are, which is the
single most useful thing a player can know on round fifteen.

## 3. Every zone must close a loop

**A dead end is where a run ends** — not because it was hard, but because
it was over the moment you walked in. Once Hollow Ridge is fully open
there are four circuits:

- **the street ring** — motel → Ridge Street → the crossroads → Mill
  Street → Mill Row → back past the garage
- **the roofs** — motel roof → hardware → east roofs → down the diner's
  fire escape
- **the drain** — the crypt → under the whole town → up in the clinic or
  in the middle of the crossroads
- **the back way** — church → crypt → drain → clinic → Mill Street

No room in the town has one door. `Plan.DOORS` is checked against this by
hand; if you add a zone, add two links.

## 4. Every zone must pay for itself

A zone that is only more room to run in is a zone nobody opens twice.
`Plan.ZONES` carries a `pay` list, and the rule is that each purchasable
zone offers at least two of: a wall weapon, a perk machine, a box spot, a
trap, the power, or a shortcut that closes a loop.

## 5. The power is a destination, not a purchase

It is free, and it is the hardest thing on the map to reach: four
purchases deep, on the far side of the town, in a basement. Until it is
thrown there are no perks but Quick Revive, no traps and no upgrade
machine — so the first ten rounds are played in the dark, and throwing it
is the loudest moment in a run.

## 6. Boards do not stop anything — they spend time

A barricade is not a wall. Six planks buy about eight seconds at round
one and about three at round thirty, and while a shambler is pulling them
off it is standing still, in the open, facing away. What the boards
actually do is convert *a crowd arriving at once* into *a crowd arriving
in a queue, at a place you chose*. That is the defence.

## 7. Who they go for is not "the nearest"

Nearest splits a crowd of thirty perfectly evenly and gives four players
the same fight. `Zombie._pick_target` weights by distance, then by how
recently that player fired (a loud player pulls), then by a bias fixed
per body at spawn — so some of them fixate and the crowd arrives as a
crowd. Downed players are weighted *down*, so a squad is not punished
twice for one mistake.

## 8. The town is authored; its contents are generated

A wholly procedural town has no memory in it: every corner is a corner
you have not learnt, which is the opposite of what this mode is for. So
`Plan` is written by hand and never varies — the rooms, the doors, the
windows, the costs. The seed decides the furniture, the cars, the
rubbish, which wall is brick and which is boarded, and where the box
starts.

## 9. Difficulty is four curves and nothing else

```
health   flat to round 9, then ×1.10 a round     (doubles every ~7)
damage   22 + 1.6 a round                         (linear, slow)
speed    walk → run, crossing at a per-species round
count    6 + 2.2/round, then 3.4, then 1.6        (bends twice)
```

Damage climbs far more slowly than health on purpose. A hit that halves
you at round ten and kills you at round twelve is not a difficulty curve,
it is a wall; the pressure should come from *how many* and *how fast*,
which a player can out-play, rather than from *how hard*, which they
cannot.

Every fifth round from six is the pack — nothing but hellhounds, which
ignore windows and cannot be trained the same way. Every tenth is a boss.
A round that is one thing has to be played differently, and that is what
keeps forty rounds of one town from being one round forty times.

## 10. The technical decisions worth knowing

**One mesh per material for the whole town.** `MeshKit` accumulates
triangles by material name and emits one `ArrayMesh` each — about twenty
draw calls for a hundred and thirty metres. Collision is one trimesh,
coarser than the visual mesh.

**One navigation region per zone, baked once.** A single navmesh re-baked
every time a door opens costs seconds of hitch at exactly the wrong
moment. Per-zone regions cost one bake at load and opening a zone is then
setting a boolean.

**Animation is baked into tables at load.** The retargeter
(`Clips`, carried over from Rhabdos) costs a skeleton seek per pose,
which does not scale to thirty bodies. `ClipBank` walks every clip once at
startup and stores the turns as quaternions; posing a body afterwards is
eighteen slerps and no seek.

**The server owns the world, the client owns its feet.** Movement is
client-authoritative because a co-operative game against AI has no reason
to pay rollback's price. Shots are traced by the client that fired (it is
the only one that knows where it was aiming) and checked loosely by the
server.

**Nothing sends geometry.** The town is a seed.

## 11. What is deliberately absent

- No perk limit. Four perks is a classic restriction and it exists to sell
  a fifth slot; here the constraint is points.
- No weapon rarity tiers. The box is a flat weighted roll.
- No account, no unlocks, no progression between runs. The run is the game.
