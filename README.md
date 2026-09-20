Central squawk assignment with correct mode S 1000 assigns, inform on DUPE and allow manual assignation from users
API knows all -> implique d'avoir des navdata sur le serveur -> donc update tous les mois -> creer un autoupdater local avec base de donnée navigraph (generer les data, les envoyer sur une repo puis webhook sur le server)
permet de ne faire aucun calcul sur le client
permet d'assigner des squawk partout sans controleur
moins de data a envoyer

doit connetre les différentes FIR pour assigner les bonnes plages de SSR par FIR
map Callsign -> SSR mais avec possibilité de chercher le SSR aussi pour facilité la detection de DUPE

Comment gerer si assignation manuelle d'un SSR déjà assigné: Effectuer une nouvelle assignation auto sur le trafic perdant son SSR
Comment gerer si assignation manuelle d'un SSR déjç renseigné par un autre trafic: Affichage DUPE sans nouvelle assignation auto 
																								(nécessite le controleur de forcer une assign auto)

Comment gerer la fin d'assignation: Boucle qui cycle sur tous les callsigns à X interval et vérifie qu'ils soient encore
				dans une zone d'exclusion (plus grande que la france paddée)
				sinon, les supprimer de la map -> les SSR redeviennent disponibles


Infos renvoyées vers le client:
	json object keyed by callsign with SSR property and dupe boolean


# Design

## Roles

- The **server** is the sole authority on squawk assignment. It ingests the VATSIM datafeed, maintains the callsign to code map for the whole area, and performs every calculation.
- The **client** (EuroScope plugin) performs no calculation. It polls a snapshot, displays the code and DUPE state as tag items, and writes the code into EuroScope only for flights the controller is tracking. Display-only otherwise.
- The client **never reports an observed squawk back to the server**. It reports flight plan data with a manual request, and only there - see Seeding below - but what an aircraft is actually transmitting comes from the datafeed alone. Reserving a code on a client's word would let any client drain the pool.

## Traffic in scope

Scope is the padded exclusion zone: the French FIRs expanded outward by `ZONE_PADDING`, 100 NM.

| Traffic | Handling |
| --- | --- |
| IFR with a filed flight plan | Managed automatically |
| VFR | Only on explicit controller request |
| No flight plan | Not managed - field 10 and field 15 are required for the Mode S decision |

## Assignment trigger

A flight is **on the ground** when its groundspeed is below `GROUND_SPEED_THRESHOLD`. No other signal is used.

| State | Behaviour |
| --- | --- |
| On the ground | Never assigned automatically. Controller request only. |
| Airborne, no code | Assigned automatically |
| Airborne, default code | Assigned automatically |
| Airborne, any other code | **Adopted** - treated as already correct and kept |

Default codes: `0000`, `1200`, `1234`, `2000`, `7000`, plus `1000` (see Mode S below). Configurable.

Traffic outside French airspace enters scope on crossing the `ENTRY_RING` distance of 40 NM from the FIR boundary, approximating five minutes at cruise. Traffic already inside French airspace is in scope immediately, so a departure that rotates without a code is assigned during the takeoff roll.

## Code allocation

- Discrete codes come from **per-FIR ORCAM pools**, supplied as configuration together with a per-FIR **exclusion list** of reserved and special-purpose codes that must never be issued.
- The **entry FIR** governs: departure FIR for departures, first-entry FIR for overflights and arrivals. The code is held for the whole flight and reassigned only if it collides downstream.
- **Pool exhausted** means borrowing from another French FIR's spare capacity, recording the origin so conflict checks on FIR transition stay correct.
- FIR polygons, pools, exclusion lists and zone geometry all live in configuration rather than code. Only French configuration ships initially.

## Code classes

Three properties follow from a code's class and are referenced throughout: whether automatic allocation may **issue** it, whether it is **reserved** exclusively to one flight, and whether it participates in **DUPE** detection.

| Class | Codes | Issued | Reserved | DUPE |
| --- | --- | --- | --- | --- |
| Discrete | Per-FIR ORCAM pools | Yes | Yes | Yes |
| Default | `0000`, `1200`, `1234`, `2000`, `7000` | No | No | No |
| Mode S conspicuity | `1000` | When eligible | No | No |
| Emergency | `7500`, `7600`, `7700` | No | No | No |
| Excluded | Per-FIR exclusion list | No | No | Yes |

Only **discrete** codes are exclusive. Every other class is shared by design: several aircraft may legitimately squawk `7000`, `1000` or `7700` at the same time, so those codes are never reserved against a pool and never raise a DUPE. Treating them as exclusive would flag a large share of the traffic on any busy evening.

## Mode S 1000

A flight is eligible for conspicuity code 1000 when **both** hold:

1. It is Mode S capable per the ICAO field 10 surveillance letters.
2. Every point of its route lies inside the Mode S participating-states area.

### Containment is resolved at build time

The area is a polygon, but the server never evaluates it. `fix.txt` is generated already clipped to the polygon, so **membership in `fix.txt` is the containment test** and a route point costs a hash lookup rather than a point-in-polygon sweep. `modes_area.geojson` ships for provenance and inspection, not for runtime use.

`airway.txt` is deliberately **not** clipped. Every airway with at least one fix inside the area is published with its complete fix list, so an airway that leaves the area and re-enters it names fixes that `fix.txt` does not contain. Clipping both files would make such an airway look contiguous and wrongly qualify the flight.

A route point absent from `fix.txt` is therefore outside the area *or* unknown, and both deny 1000. That is the fail-closed direction.

### Route evaluation

1. Tokenise field 15.
2. Skip `DCT` and speed/level groups (`N0450F350`).
3. Skip tokens published as a SID or STAR by this flight's **own** departure or destination aerodrome, per `procedure.txt`. Matching a bare designator against every airport instead would skip the six identifiers that are both a procedure name and a real in-area fix: `BRAVO`, `HON`, `NORTH`, `ROCIO`, `SOUTH`, `TSC`.
4. Expand airway tokens through `airway.txt`, walking between the fix before the token and the fix after it. A designator may appear in more than one block when the airway is published as disjoint segments, so use the block containing both entry and exit. A token that cannot be expanded denies 1000.
5. Every remaining token must appear in `fix.txt`. Any that does not denies 1000.

The geographic verdict is **cached against the route** (`departure + field 15 + destination`, normalised) rather than against the callsign. Identical routes are filed repeatedly, so a route-keyed cache hits far more often than a flight-keyed one. The field 10 equipment test is applied per flight on top of the cached verdict.

Re-evaluation on flight plan amendment is **asymmetric**: a flight that becomes ineligible moves from 1000 to a discrete code, while a flight that becomes eligible keeps the discrete code it already holds. A code the pilot is already squawking is never taken back.

An aircraft already squawking **1000 on entering scope is re-evaluated like any default code**: 1000 stands if the route stays inside the area, and a discrete code is issued if it exits.

## Adopting existing codes

An aircraft entering scope airborne with a non-default code has that code **adopted** as its assignment, whether or not it belongs to a French pool.

- A **discrete** adopted code is **reserved** against the pool so it is never issued to anyone else. Non-discrete codes are adopted but never reserved, since they are shared by design.
- If the server had already issued that code to another flight, **our flight yields** and is reassigned. An actively squawked code always beats an assigned one.
- Adopted entries carry provenance `adopted`, distinct from `auto` and `manual`.

## Reconciliation and cold start

The assignment loop is **reconciliation-based, not event-based**: every tick rebuilds the intended state from the datafeed and the current map. Cold start is therefore not a special mode, it is the same loop with an empty prior. This is what makes traffic already airborne inside the AOR at server start behave correctly, without a separate bootstrap path.

Every tick runs in strict phase order:

1. **Observe.** Take every pilot inside the padded zone from the datafeed. Record observed transponder, position, groundspeed and last-seen time.
2. **Reserve.** Reserve every observed non-default code in the pool *before any allocation*. This ordering is essential: allocating while iterating can hand out a code that an aircraft later in the same pass is already squawking, manufacturing a DUPE the server invented itself.
3. **Classify.** Adopt, queue for assignment, or skip, per the assignment trigger table.
4. **Allocate.** Issue codes to the queued set from the now-consistent pool.
5. **Release.** Apply the release rules.

On cold start the loop additionally:

- **Holds off assigning for the first two datafeed cycles** (~30 s), so the first allocation runs against a complete picture rather than a partial feed.
- **Serves HTTP 503 until the first full sweep completes.** DUPE state computed from an incomplete map is misleading, and 503 keeps the payload contract unchanged - clients simply retain their last snapshot and retry.
- **Defers Mode S route evaluation.** Adopted flights keep their code until their route verdict is computed over subsequent ticks. This is safe precisely because re-evaluation is asymmetric: a late verdict can only move a flight off 1000, never onto it.

**Persistence.** The map is persisted (code, provenance, manual flag, last-seen) so a quick restart resumes exactly instead of losing every manual override. On start the persisted map is reconciled against the datafeed: entries still present keep their provenance, entries whose observed code has changed take the observed code, and entries absent from the feed start their grace clock at server start rather than being dropped immediately. Without persistence the server still recovers correctly, but every manual override degrades to `adopted`.

## Release

A code returns to the pool when any of the following occur:

- the flight leaves the padded exclusion zone;
- the flight lands at a destination in scope;
- the flight has been absent from the datafeed for the grace period.

There is no manual release. Assignment triggers 40 NM outside the France FIR boundary while release triggers at 100 NM, so a diverting flight holds its code until it clears the padding.

The 60 NM gap between the two thresholds is deliberate hysteresis: an aircraft that enters scope at the ring and then turns away has to travel a further 60 NM before its code is released, so it cannot oscillate across a single boundary and churn its assignment.

## Conflicts and DUPE

`dupe` is true when **another aircraft is currently squawking the same code**, taken from the datafeed transponder field rather than from the assignment map.

DUPE applies to **discrete codes only**. Shared codes never raise it, so two aircraft on `7000`, two on `1000`, or two simultaneous emergencies on `7700` are all normal and silent.

| Situation | Behaviour |
| --- | --- |
| Manual code already **assigned** to another flight | The other flight is automatically reassigned |
| Manual code already **squawked** by another aircraft | DUPE shown, no automatic reassignment. Controller must force. |
| Both at once | Treated as squawked: DUPE, no reassignment |
| Code set directly in EuroScope, bypassing the plugin | Server wins and re-pushes the central code |
| Code set through the plugin | Flagged `manual` and protected from the automatic loop |

Emergency codes `7500`, `7600` and `7700` are **never** modified or reassigned by any automatic rule.

A controller may **manually assign `7600` or `7700`** to mark a radio failure or an emergency; `7500` stays refused, since nothing good comes of letting a client set a hijack code. A manually assigned `7600` or `7700` is flagged `manual` and so is protected from the automatic loop, is not reserved, and raises no DUPE. Clearing the condition is an ordinary forced reassignment, which returns the flight to a discrete code from its entry FIR's pool.

Because the plugin writes only for tracked flights, a divergence on an untracked flight cannot be corrected until a controller tracks it.

## Manual operations

Controllers may:

- **Set a specific code.** Accepted regardless of range and flagged `manual`. Codes on the exclusion list are refused, with `7600` and `7700` as the only exceptions so a radio failure or an emergency can be marked.
- **Force a reassignment.** The server issues a fresh code from the appropriate pool.
- **Request a code for VFR traffic.** The only route by which VFR is assigned.

There is no manual release; codes return to the pool only through the release rules.

## Seeding a flight ahead of the datafeed

A controller asks for a code the moment a pilot connects, which is a datafeed cycle or more before the server has ever heard the callsign. Left alone that request is refused as an unknown flight, which is exactly the wrong answer for the case the manual endpoint exists to serve: a departure at the gate asking for clearance.

The plugin therefore attaches the flight as the datafeed would have described it. EuroScope received the same flight plan over the same FSD connection the feed is built from, so nothing is invented - position comes from the FP track position, which EuroScope computes itself when there is no radar return yet, and the rest is fields 8, 10, 13, 15 and 16 straight off the plan. It is attached to every manual request rather than only the ones expected to fail: the plugin cannot know what the feed has reached, and the server discards it the moment the feed carries the callsign.

Server side this is **not** a separate list of flights with its own lifecycle. The seed is merged into the observation phase of the reconciliation tick as an ordinary observation, so adoption, reservation, allocation, release and DUPE all behave exactly as they would for a real one. The same merge is what forgets it: a seed whose callsign the feed now carries is dropped rather than merged, and a seed the feed never confirms is released at its own short TTL rather than waiting out the grace period on top of it.

Four rules keep this from being a hole in the authority model:

| Rule | Why |
| --- | --- |
| The datafeed always wins | A seed for a callsign already observed is ignored outright |
| The transponder is never taken from the client | Observed codes are reserved before allocation, so a code taken on a client's word would let any client drain the pool and manufacture DUPEs |
| The position is validated against the padded zone | A seed cannot conjure a flight into airspace it is nowhere near |
| Unconfirmed seeds are capped per controller | A buggy or hostile plugin has a bounded blast radius |

## Simulator sessions

A sweatbox has no VATSIM datafeed behind it, so nothing describes that world to the server and the plugin would otherwise sit silent for a whole training session. The session therefore describes itself: the server runs a **second instance** with `FEED_SOURCE=push`, holding its own pool, and one client pushes the picture to it. Nothing a simulator session does can reach a real aircraft, because the two never share a pool.

### EuroScope cannot tell you which world you are in

The connection type looks like the answer and is not. EuroScope reports `SWEATBOX` only for the machine actually **running** the session - typically the instructor's. A student joins by connecting normally and picking the training server as their server, and EuroScope reports that as `DIRECT`, identically to VATSIM.

So the participants who do the assigning are exactly the ones the connection type gets wrong. A plugin that trusted it would let a student's requests reach the live server and - with seeding - **invent sweatbox aircraft in the live map and hold real codes for them**. Asking the student to type a command instead only moves the failure: forget it once and the same thing happens, silently.

### The live server proves the negative instead

The datafeed lists every controller logged on to VATSIM, and the server already fetches it every 15 s. A callsign absent from that list is not on this network, whatever its plugin believes.

**The live server refuses `/api/assign` from any callsign it cannot see logged on.** That is the isolation boundary, and the only one that holds, because it does not depend on the client being right about anything. A student who sets nothing up gets a refusal and a message rather than a corrupted pool. The cost is that a genuine controller is refused for the ~20 s until their logon reaches the feed, which is cheap and self-correcting.

The same fact is published at `GET /api/network?controller=…` so the plugin can ask **before** a controller needs a code rather than after being turned away. The mode then resolves from three signals, in order of authority:

| | Signal | Conclusive? |
| --- | --- | --- |
| 1 | `.centralsquawk mode live\|sim\|auto` | Yes, when set. An escape hatch, not a safety mechanism - the server refuses regardless |
| 2 | EuroScope's connection type | Only when it says simulator. `PLAYBACK` counts as offline: a recording has nobody to assign to |
| 3 | The live server's answer for this callsign | Decides every ordinary-looking connection, which is most of them |

A negative answer must hold for 60 s before the plugin acts on it, because a controller who has just logged on really is absent from the feed for a generation or two. A server that cannot be reached changes nothing: not knowing is not evidence.

### Feeding the session

Every EuroScope in a sweatbox sees the same traffic, so without arbitration they would all push and the map would be rebuilt several times a tick from pictures that disagree. The first client to push claims a lease and renews it on every push; the rest are told who holds it and drop back to probing occasionally. A lease is taken over once it goes stale, so an instructor whose EuroScope crashes costs the session a few seconds rather than the picture. A client probing for the lease still sends the whole picture, because winning it with an empty push would start the grace clock on every flight in the session.

The picture is a walk over radar targets rather than flight plans, since the server needs where each aircraft is and what it is squawking and only a target carries either. Aircraft with no flight plan are included: the server will decline to assign them, but it has to see the code they squawk or it will hand that code to somebody else.

The simulator endpoint is `sweatbox.squawk.vatsim.fr`, overridable per EuroScope profile through the `simApiUrl` plugin setting for a locally hosted one. `.centralsquawk status` reports which mode the plugin resolved, **why**, which server that points at, and whether this client is the feeder.

## Client interface

**Snapshot**, polled every 5 s, gzip encoded:

```json
{
  "AFR1234": { "ssr": "7201", "dupe": false },
  "EZY83GK": { "ssr": "1000", "dupe": false }
}
```

Roughly 2.4 KiB per poll at 300 flights, and about 0.65 Mbit/s aggregate at event scale (800 flights, 60 controllers). Served as HTTP 503 until the first reconciliation sweep completes.

**Manual endpoint**, synchronous, returning the resulting entry so the plugin can update its cache and write the code immediately:

```json
{ "ssr": "7201", "dupe": false }
```

The request carries the callsign, the controller, the token, and either `code` or `mode`. It also carries `flight`, the datafeed-shaped view of the aircraft, for the case the server has not seen the callsign yet:

```json
{
  "callsign": "AFR1234", "controller": "LFFF_CTR", "token": "…", "mode": "auto",
  "flight": {
    "latitude": 49.009, "longitude": 2.548, "altitude": 392, "groundspeed": 0,
    "flightRules": "I", "departure": "LFPG", "arrival": "LFBO",
    "equipment": "B738/M-SDE3FGHIRWY/LB1", "route": "ATREX UM976 LERGA"
  }
}
```

**Network endpoint**, unauthenticated, so the plugin can work out which world it is in. Who is logged on is published in the datafeed already:

```
GET /api/network?controller=LFFF_CTR
{ "controller": "LFFF_CTR", "onNetwork": true, "enforced": true, "controllersOnline": 330 }
```

`onNetwork: null` means the instance cannot judge, which is the honest answer from the simulator server - it has no roster and refuses nobody.

**Feed endpoint**, simulator server only, sent by whichever client holds the feeder lease. One datafeed generation in the same shape, with the transponder included - unlike a seed, since nothing else describes that world:

```json
{
  "controller": "LFPG_TWR", "token": "…",
  "observations": [
    { "callsign": "AFR1234", "latitude": 49.009, "longitude": 2.548,
      "altitude": 400, "groundspeed": 0, "transponder": "2000",
      "flightRules": "I", "departure": "LFPG", "arrival": "LFBO",
      "equipment": "A320/M-SDE3FGHIRWY/LB1", "route": "ATREX UM976 LERGA" }
  ]
}
```

Answered with `409 not_the_feeder` when another client holds the lease.

**Authentication**: `SHA256(shared_secret + controller_callsign)`.

## Configuration and navdata

All configuration is ingested rather than baked into the server: FIR polygons, per-FIR ORCAM pools and exclusion lists, the zone geometry, and the three navdata files behind the Mode S decision. It lives in the **CentralSquawk-config** repository.

| File | Refresh | Contents |
| --- | --- | --- |
| `fix.txt` | every AIRAC | Fixes inside the Mode S area, `<ident> <lat> <lon>` |
| `airway.txt` | every AIRAC | `AIRWAY <name>` blocks with complete, unclipped fix lists |
| `procedure.txt` | every AIRAC | `<airport> <SID\|STAR> <designator>` for in-area airports |
| `modes_area.geojson` | rarely | The area as FIR/UIR rings; provenance and inspection only |

### Update path

Pushing to `main` in the config repository fires a webhook and the server re-ingests. Three rules make that safe:

- **Validate before swapping.** Parse and check the entire set into a new snapshot, then swap it in atomically. A push that fails validation leaves the running set untouched and raises an alert. A bad config must never be able to stop code assignment.
- **Flush the route cache on reload.** Mode S verdicts are cached against the route string, but they are only valid for the navdata that produced them. A cycle that moves an airway or retires a fix changes the verdict for routes whose text did not change at all, so a surviving cache would serve stale eligibility indefinitely.
- **Never reassign on reload alone.** A reload changes eligibility for flights already holding a code. The asymmetric rule still governs: a flight that becomes ineligible moves from 1000 to a discrete code, and one that becomes eligible keeps what it has.

## Parameters

| Parameter | Value | Notes |
| --- | --- | --- |
| Poll interval | 5 s | Datafeed itself refreshes about every 15 s |
| Grace period | 5 min | Absence from datafeed before release |
| `GROUND_SPEED_THRESHOLD` | 50 kt | Matches `ON_GROUND_SPEED_THRESHOLD` in the plugin |
| `ENTRY_RING` | 40 NM | Approximates 5 min at cruise |
| `ZONE_PADDING` | 100 NM | Release boundary beyond the French FIRs |
| Default codes | `0000`, `1200`, `1234`, `2000`, `7000`, `1000` | Configurable |
| Navdata refresh | Every AIRAC, 28 days | Config repo push then webhook |

## Accepted risks

- **Shared-secret authentication.** The secret is compiled into the distributed DLL, so any user can extract it and forge a token for any controller callsign, then set or force codes anywhere in France.
- **A training callsign that collides with a live one defeats the network gate.** The gate asks whether the callsign is logged on, not whether *this person* is. A student running a sweatbox as `LFPG_TWR` while a real `LFPG_TWR` is online on VATSIM is indistinguishable from that controller, and their requests would be accepted by the live server. Closing it properly needs the CID, which EuroScope does not expose for the connected controller. Narrow - it needs the collision to be live at that moment - and a training department avoids it entirely by using callsigns that are not in use.
- **A simulator feeder is trusted completely.** Inside a simulator session the pushed picture is the only description of that world, so a forged token can invent traffic, hold codes and raise DUPEs freely. That is tolerable only because the simulator server is a separate instance with a separate pool, and stops being tolerable the moment anyone runs a sim session against the live instance.
- **The network gate depends on the datafeed.** If the feed cannot be fetched, the roster from the last successful poll keeps being used - controllers do not churn fast enough for that to matter over minutes, and dropping it would refuse every controller in the country. A controller who logs on *during* an outage is refused until it recovers.
- **Seeded flights need not exist.** A forged token can conjure a flight the datafeed has never carried and have a code issued to it. The damage is bounded rather than prevented: no code is reserved on the client's word, the position must fall inside the padded zone, each controller callsign may hold only a few unconfirmed seeds at a time, and every one of them is released within the seed TTL. The cost of an abused seed is therefore a handful of codes held for two minutes, and a logbook line naming the callsign that did it.
- **Callsign as the only key.** Two pilots connected on the same callsign share one entry, and a pilot who reconnects under a corrected callsign orphans their previous code until the grace period expires.
- **Navdata licensing.** `fix.txt`, `airway.txt` and `procedure.txt` are Navigraph-derived. The source database is gitignored, but redistributing data derived from it through a public repository still needs a licence check.
- **Identifier collisions.** Around 915 identifiers are reused among in-area fixes, and roughly a hundred more collide between in-area and out-of-area points. Resolving route points by name will occasionally judge a point to be inside the area because a same-named fix elsewhere is. Inherent to name-based resolution, and small, but it biases toward granting 1000.
- **Split manual behaviour.** A code set through the plugin is protected while the same code set directly in EuroScope is overridden - identical controller intent, opposite outcome depending on the interface used.
- **Fixed entry ring.** A fixed distance approximates a time, so 40 NM is about 5 min for a jet at 450 kt but roughly 20 min for a light aircraft at 120 kt, which holds a code far longer than intended before the flight is anywhere near French airspace.
