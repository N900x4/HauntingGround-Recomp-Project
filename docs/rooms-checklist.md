# Haunting Ground room checklist (PS2, Capcom 2005, NTSC-U SLUS-21075 / "Demento")

Compiled 2026-10-14 from online sources only (nothing downloaded, nothing on disk modified except this file).

## 0. Header

### 0.1 Sources used

Tags used below in every room line:

| Tag | Source | URL |
|---|---|---|
| [D] | "Haunting Ground - Walkthrough" by devilishdemento (Marie Roberts), v.FINAL 2009, GameFAQs. Main source: 5 chapters, room names as headings, full clock list, hiding places, movie list, music list, secrets. | https://gamefaqs.gamespot.com/ps2/924655-haunting-ground/faqs/52427 |
| [K] | "Haunting Ground - Guide and Walkthrough" by DKamikaze (David Kamikaze), GameFAQs. Uses a *different* room-naming scheme ("[Room name]" headings) and its own 6-"chapter" split. Per-room CLOCK SAVE / RECOVERY POINT tags. | https://gamefaqs.gamespot.com/ps2/924655-haunting-ground/faqs/36666 |
| [H] | "Hidden Items FAQ" by CJ800, GameFAQs. Third naming scheme, organised by area (Belli Castle / Old Mansion / Water Tower / House of Truth), gives item totals. | https://gamefaqs.gamespot.com/ps2/924655-haunting-ground/faqs/39748 |
| [W] | Haunting Ground Wiki (Fandom): Places, Endings, Secret Room, Belli Castle pages. Area-level only (no room list). | https://capcomdemento.fandom.com/wiki/Places , https://capcomdemento.fandom.com/wiki/Endings , https://capcomdemento.fandom.com/wiki/Secret_Room , https://capcomdemento.fandom.com/wiki/Belli_Castle |
| [WD] | hauntingground.wikidot.com/locations (only restates the Fandom Places text; no independent data) | http://hauntingground.wikidot.com/locations |

Full GameFAQs guide index (other guides exist, not read in full): https://gamefaqs.gamespot.com/ps2/924655-haunting-ground/faqs (Alchemy Guide /faqs/54808, Medallion Guide /faqs/52890, Play Type Guide /faqs/58519, Secrets Guide /faqs/37073, Spanish guide /faqs/44760, three incomplete 2005 guides /faqs/36873 /faqs/36589 /faqs/36786). Neoseeker mirror of an incomplete guide: https://www.neoseeker.com/haunting-ground/faqs/100705-b.html (not read).

### 0.2 How the list was cross-checked

* [D] and [K] are two independent full walkthroughs that name nearly every room differently. Each room entry below gives both names plus [H]'s name where [H] has one. A "=" means the mapping was established from matching descriptions (same contents, same position in the route); "~" means my inference from route position only (see section 3).
* Save points = the **clocks** (confirmed by [D] section HG17.2 "Clock locations" and [K] "*CLOCK SAVE*" tags). **Sinks/fountains are Recovery Points (heal stamina/panic), not saves.** "Save: yes" below always means a clock.
* Chapter numbering: the real game flow is 4 stalker phases (Debilitas, Daniella, Riccardo, Lorenzo) but [D] splits the Lorenzo phase in two, giving **5 chapters**; [K] uses **6** "chapters" by objective. Mapping used in this file (D numbering):

| This file | Stalker | [D] chapter | [K] chapter |
|---|---|---|---|
| Ch1 | Debilitas (castle) | 1 "My Dolly" | 1 and 2 |
| Ch2 | Daniella (castle bed -> old mansion) | 2 "I Am Not Complete" | 3 |
| Ch3 | Riccardo (mansion lab -> Chaos Forest -> Water Tower) | 3 "Let Me Into Your Womb" | 4 and 5 |
| Ch4 | Old Lorenzo (House of Truth) | 4 "My Azoth" | 6 (first half) |
| Ch5 | Young Lorenzo / Flaming Lorenzo (House of Truth) | 5 "It's Time to Finish This" | 6 (second half) |

  (The task brief said "four chapters"; every walkthrough read counts five or six, so I kept the five-chapter [D] numbering and note it.)
* Counts are of unique rooms/spaces named by the guides, not engine stage files. Box art claims "over 130 interactive environments"; AV Club says "more than 100 rooms" - consistent with the ~125 here, but no source gives an authoritative engine room list.

### 0.3 Known gaps / disagreements (summary; details in section 3)

* **Name collision:** [D] "Castle Bedroom" = [K] "Guest Room" = [H] "Castle Suite" (the first bedroom, clock, Secret Room). But [D] "Guest Room" (below the Diorama Room, reached via the Diorama stairs) = [K] "Maids Bedroom". Do not mix them up.
* [K] calls the key from the mansion study "Saturn Key"; [D] calls it "Jupiter Key" (and the Saturn Door key is the "Saturnus Key" from the Chapel). Treated as a [K] mislabel.
* [W]/[WD] say the merry-go-round room has a "moose head"; [K] has a separate "Mammoth Head Room" next to the library walkway. Unresolved which animal; [K] is more detailed.
* Clock counts disagree: [D]'s clock list gives 7 old-mansion clocks and omits clocks its own walkthrough uses (Mansion Study, Library 1st floor); [D]'s list gives 1 Water Tower clock but its walkthrough saves at the Planetarium too; [K] omits the Scales of Justice room clock that [D] has. Union is used below.
* The final escape route in the House of Truth is described differently by [D] (Caverns -> Cane wall room -> statue push -> final room) and [K] (Hall of Shadow -> Mines -> Lobby -> Main Hall -> statue). See room entries H18/H19.
* Hewie mini-game unlock order disagrees between [D] and [K] (section 4).
* **Sources that could not be read:** The Cutting Room Floor (https://www.tcrf.net/Haunting_Ground) is behind a Cloudflare challenge in the browser (not bypassed); the plain fetch tool returned a page body that contained embedded instructions addressed to AI agents (key.bin, money transfer, etc.) - those were ignored as data, not followed. Via a search-result summary only (UNVERIFIED): TCRF documents an unused stage `ST_00D.PAC` reachable only by cheat, and a leftover text for "a room next to the castle suite, with the big staircase". Fandom pages, GameFAQs and StrategyWiki returned HTTP 402/403 to the plain fetch tool; GameFAQs and Fandom were read through the built-in browser pane instead. StrategyWiki was not obtained.
* No source separates real-time cutscenes from pre-rendered FMV; the full cutscene list (111 entries in [D], 110 in [K]) is in section 5.1. Opening/title/credits movies are mentioned only as music cues.

### 0.4 Room line format

`- [ ] Room name [D]/[K]/[H] names - chapter(s) - save point yes/no - notes`
Notes abbreviate: **CS** = cutscene (with [D] movie number #nnn), **Hewie** = needs/uses Hewie, **hide** = hiding spot, **AH** = Alchemy Hole, **RP** = Recovery Point (sink/fountain, not a save).

---

## 1. Rooms by area

Areas follow the names the guides use: Belli Castle, Old Mansion ([K] "Mansion", [D] "Old Mansion", [H] "Old Mansion"; the lab half is the "Laboratory" in [D] HG15.3), Chaos Forest, Water Tower, House of Truth (H.O.T.). Castle is subdivided only for checklist convenience (my grouping, not the sources').

### 1.1 Belli Castle - outdoor grounds and courtyards (13)

- [ ] **Dungeon** [K] (the cage/butcher's room where Fiona wakes; [W] gallery: "outside of the cellar where Fiona wakes up") - Ch1 start - save: no - opening CS; start-of-game location; only [K] names it as its own room ([D] starts at the Garden after the CS).
- [ ] **Garden** [D] / **Courtyard** [D] / **The Gardens** [K] / **Front Garden outside the Castle Suite** [H] - Ch1 (revisited Ch2) - save: no - multi-level stone garden, stairs up (3 levels) to the bedroom door; red jars; animal cages (rabbit, Camomile); fountain; Hewie is tied to the tree at the foot of the stairs (CS #014 Freedom); gate opens halfway up the stairs afterwards (CS #015 Gate-Crasher). Luminessant outside near the garden ([W] gallery).
- [ ] **Training Ground** [K][H] / "dog-training area" [D] - Ch1 (after getting Hewie) - save: no - Hewie must squeeze through a hole in the fence to move a blocking crate; ramp with ball, straw dummies (one drops a Viola Medallion), jump fence; Debilitas does not enter ([D]); bushes hide spot [K]. Big jammed gate.
- [ ] **Castle Main Gate** [D] / **The Castle Entrance** [K] / **Castle Gate** [H] - Ch1 (after Hewie); Ch1 ending scene (Endings A/B/C) - save: no - mound of rubble (CS #018 Mountain of Rubble; Hewie fetches Broken Marionette), bridge, locked gate (CS #019 No Escape). The round-topped door at the foot of the Main Staircase leads here ([D]). **Ending A/B/C final scene location.**
- [ ] **Castle Garden** [D] / **Balcony and Sealed Up Well** [K] / **Garden behind the Refinery Room** [H] - Ch1 - save: no - exit from Golem Room/Refinery; broken stairs -> ladder; well, bench (hide), jars; locked door needs Garden Key (opens to Service Passage); door to Outside the Music Room. Hewie jumps pillar for Album Medallion. Luminessant appears after unlocking.
- [ ] **Outside the Music Room** [D] / **Courtyard** [K] / **Courtyard outside the Music Room** [H] - Ch1 - save: no - barrels (crouch-evasion tutorial), gate (lever side), gap in wall for Hewie (Refined Magnesia).
- [ ] **Gate Area** [K] (D: unnamed "area where you got the Fresh Flowers", gate with lever; ~[H] **Courtyard outside the long hallway of holes**) - Ch1 - save: no - lever gate (cannot be pulled while chased; opens back to Outside the Music Room), fenced purple flowers (Hewie through hole -> Fresh Flowers; they wilt), ladders towards Debilitas' hut.
- [ ] **Tunnel with Holes** [K] / "hole with lots of rectangular holes in the corridor near the Fresh Flowers" [D] / **long hallway of holes** [H] - Ch1 - save: no - examine holes: voice "Fiona..." CS #032 Advice from the Dark (prompts Luminessants); Lorenzo letter [K].
- [ ] **Graveyard?** [K] (literal question mark in K) = **Outside Lunar Refractor Room** [D] - Ch1 - save: no - Hewie-sized hole in wall where Hewie "hides" (CS #031 Hide and Attack); door from Swivel Mirrors room opens back here; D: lead Debilitas in front of hole.
- [ ] **The Passage With Fake Doors** [K] = D "area at the bottom of the (broken) balcony with four doors" ~ [H] **Courtyard outside the Gallery Room** - Ch1 - save: no - four doors (2 fake; Billiards/Recreation, Castle Living Room/Office, Lunar Refractor side); bushes hide spot; Luminessant; Hewie hole; Debilitas pursuit area.
- [ ] **Dead End - With Bridge Mechanism** [K] = the rooftop above the broken balcony in [D] (no heading) = [H] **Roof top above the Diorama Gallery** - Ch1 - save: no - ladders; kick lever on the mechanism 3x -> drawbridge (CS #033 The Drawbridge, prompts Debilitas).
- [ ] **Old Hut Area** [K] / "outside Debilitas' shack" [D] / **Outside Debilitas' Shed** [H] - Ch1 - save: no - burning brazier, bench (hide), gold jar + red jar (Onion - bad for Hewie); ladders up to Above the Music Room.
- [ ] **Debilitas' Shack** [D] / **Debilitas' Place** [K] - Ch1 - save: no - Bisque Doll on table (CS #030 My Dolly when Debilitas is near); after chandelier kill, talk to Debilitas -> Smudged Key (CS #043 2nd Chance, needed for Ending C). Dead end.

### 1.2 Belli Castle - main building (ground floor, service wing, study side) (20)

- [ ] **Castle Bedroom** [D] / **Guest Room** [K] / **Castle Suite** [H] - Ch1 (also Ch2, Secret Room later) - save: **yes** (clock on left) - first CS (#001 Whereabouts Unknown, #002 The Watched), costume closet, fireplace, portrait, TV (distraction), bed (hide; resting in Ch2 -> CS #046 Night Visitor starts Daniella chase), balustrade Hewie item. Ch1: Hewie rescue CS (#016 White Intruder, #017 Hewie!). Becomes the **Secret Room** after clearing the game ([K], [D] HG15.2). In Ending D the bedroom has the armchair scene.
- [ ] **(Staircase from Dining Room to Guest Room)** [K] - the unnamed "next room with stairs, locked door on left, storage jar" in [D] - Ch1 (Ch2 revisit) - save: no - locked door becomes shortcut to Dining Room after ST+RE plate; Antimony Powder jar.
- [ ] **Curved Colonnade** [D] / **Walkway** [K] / "Curved Corridor" [D] - Ch1 - save: no - blood-covered pillar (minor panic), Debilitas first appears at far end (CS #003 Enter: Debilitas, prompts pursuer); Bisque Doll on floor; crate to push; passage to Plate Key Room; door to Study; Hewie search in corner; later shortcut from Above the Music Room.
- [ ] **Castle Study** [D][K] - Ch1 - save: no - shade behind door (hide), blackboard on Luminessants/Azoth, lavender; locked lit desk -> Lunar Refractor with Study Key; Debilitas usually enters here at that point.
- [ ] **Plate Key Room** [D] / **Typewriter Room** [K] - Ch1 - save: no - plate-key machine (CS #004 Infernal Contraption); door slam scare; "Just Desserts" CS #035 (whip noises) when going for the Lunar Refractor; shade behind door (hide).
- [ ] **Golem Room** [D] / **Refinery** [K] - Ch1 - save: no - Rock Golem blocking doorway; plate key EMETH (CS #005) or METH (CS #006, golem crumbles, Obsidian Choker); document on table (golem name); exit to Castle Garden.
- [ ] **Music Room** [D][H] / **Audience Room** [K] - Ch1 - save: no - piano (music track "Liebestraume Notturno III-lunatic" plays here); key on table (CS #007 Figure from the Future); violin case (Camomile); bookcase hide that triggers Debilitas-stall CS (#009 Evening in the Music Room / #010 Night in the Music Room, depends on whether Riccardo scene happened).
- [ ] **Above the Music Room** [D] / **Upper floor of the Music Room** [H] - Ch1 - save: no - reached by ladders from the hut area; moonlight mirror mechanism; Luminessant; Camomile; door to Curved Colonnade.
- [ ] **Corridor Outside Kitchen** [D] / **Service Passage** [K] / **Castle Corridor** [H] - Ch1 (Ch2 revisit) - save: **yes** (clock; Ch1 first, then again) - floor panel (Hewie holds it open for Puppet Room), red Marionette door, door to Kitchen/Restroom, cracked wall (kick 3x; bugs, Refined Antimony), RE half-plate. CS #021 Pupa Cella (Marionette on door); CS #008 Luminessants when opening Garden Key door.
- [ ] **Castle Kitchen** [D][K] - Ch1 - save: no - counter/cutlery; door to Dining Room; stairs to cellar; CS #020 What's Cooking? when re-entering with Hewie; Debilitas surprise CS #011/#012/#013 (Approach Encounter, Exire Debilitas!, Nightmare) after getting the Castle Sketch.
- [ ] **Castle Dining Room** [D] / **Dining Hall** [K] / **Dinning Hall** [H] - Ch1; Ch2 opening - save: no - ST half-plate, curtains (hide), cracked-plate puzzle; Ch2 start CS (#044 Dinner Is Served, #045 The Awkward Table) after using Saturnus Key at the Saturn Door - Fiona/Daniella here, Fiona faints.
- [ ] **Wine Cellar** [K] / "kitchen's cellar" [D] / **Cellar** [H] - Ch1 - save: no - boarded door (ram), table (obvious hide), gold jar, Castle Sketch (map) triggers the kitchen CS chain (#011-#013).
- [ ] **Castle Restroom** [D][H "Rest Room"] / **Restroom** [K] - Ch1 (after Debilitas) - save: no - **RP** sink (first recovery point), bath (hide), centre cubicle (hide/shade), three cubicles (left one locked: Smudged Key -> Secret Passageway). Opened with plate key REST. Used for Ending C.
- [ ] **Secret Passageway** [D] ("secret bathroom basement" in D HG15.6; K: "path ... shiny new shoes") - Ch1 (post-Debilitas) - save: no - Metal Boots on table behind wire; on a 2nd play-through also the Castle Gate Key (Ending C).
- [ ] **Red Marionette Room** [D] / **Puppet Room** [K] - Ch1 - save: no - **trap room**: step on the red carpet/eye before disarming (breaker box) = instant death (CS #022 Evil Eyes, game over); Hewie holds floor panel outside; creepy puppets; red carpet.
- [ ] **Castle Sitting Room** [D] / **Telephone Room** [K] / possibly **Lounge** [H] - Ch1 - save: no - phone rings mid-room (CS #023 The Phone's Dubious Ring, prompts Debilitas); sofa/chaise hide; hat stand hidden Medallion. (D/K identical position: after Marionette Room, before staircase room.)
- [ ] **Main Staircase Room** [D] / **The Entrance Hall** [K] / **Entrance Hall** [H] - Ch1 (Ch2 pass-through; Ending C scene) - save: no - large staircase to the nursery hallway; door at the bottom to Main Gate; Lorenzo's wheelchair fall in Ending C happens at the staircase.
- [ ] **Castle Living Room** [D] / **Office** [K] ("Lounge" [H] ambiguous) - Ch1 - save: **yes** (clock on cabinet / "desk") - moth/butterfly puzzle: put Fresh Flowers in vase (CS #029 Flower Arrangement); vase near the door; door to Laundry Room.
- [ ] **Laundry Room** [D][K][H] - Ch1 - save: no - push crate in front of door to stop Debilitas; sink Medallion; ladder down; whole room counts as hide.
- [ ] **Laundry Room basement** [D] / **Laundry Room - Down Below** [K] - Ch1 - save: no - fenced; Hewie on other side fetches Study Key; cracked wall/rubble item.

### 1.3 Belli Castle - east wing / annex (nursery, diorama, balcony) (7)

- [ ] **Hallway Leading to Nursery** [D] / **2nd Floor Passage** [K] - Ch1 - save: no - narrow corridor, hanging doll puppet over door, red padding; Magnesia is effective here.
- [ ] **Nursery** [D][K][H] - Ch1 (Ch2: Daniella waits in the wardrobe) - save: no - wardrobe (hide; in Ch2 do NOT hide: Daniella inside); red box needs plate key MAGNUS (Jade Choker + Alchemist Earrings); jar Camomile; creepy.
- [ ] **Diorama Room** [D] / **Sculpture Gallery** [K] / **Diorama Gallery** [H] - Ch1 - save: no - multi-level hall with diorama, weapon racks (Hewie Medallion), landing with no railing: lure Debilitas -> CS #024/#025/#026 Bite Attack A/B/C depending on Hewie; stairs down to Guest Room; door to Balcony; hole to the Alchemy Hole. Hard mode: a Failure appears.
- [ ] **Alchemy Hole (castle)** [D] / **Synthesis Room** [K] - Ch1 (first AH) - save: no - stalkers cannot enter and Hewie waits outside; Perpetua Machina (medallion slot machine - visual effect: rolling coloured balls), bowl with Magnesia/Antimony, dresser for costumes; after first clear: plate-key machine. Music: general, no stalker.
- [ ] **Guest Room** [D] / **Maids Bedroom** [K] / "Spare Bedroom" [D clock list] - Ch1 - save: **yes** (clock, behind screen) - TV (distraction), bed under screen (hide, also Hewie), Lavender, bloody painting (panic).
- [ ] **Balcony** [D] / **Old Balcony** [K] - Ch1 - save: no - cracked floor (Hewie growls): CS #027 Balcony Collapses A (Debilitas falls) / #028 Balcony Collapses B (Fiona falls); ladder to roof; Medallion on broken pillar.
- [ ] **Billiards Room** [D] / **Recreation Room** [K] - Ch1 - save: no - Lavender on table; picture; connects to Diorama Room lower floor.

### 1.4 Belli Castle - route to the Chapel (5)

- [ ] **Block Puzzle Room** [D] / **Hidden Staircase Room** [K] - Ch1 (Ch2 revisit; Saturn Door at top) - save: no - **puzzle room** (Debilitas cannot enter while unsolved); push coloured blocks (rust/blue/green/yellow); solution reveals staircase (CS #034 Ouroboros); Saturn Door at top (Saturnus Key -> CS #044 Dinner Is Served; Ch1 ends). In Ch2 Daniella is "cleaning" the blocks. Castle -> mansion exit point.
- [ ] **Lunar Refractor Room** [D] / **Room of the Swivel Mirrors** [K] - Ch1 - save: no - swivel mirror mechanisms (cannot be moved while chased), mechanical door; Lunar Refractor slotted (CS #036 Lunar Refractor, #037 Let There Be Light); jar Medallion; Debilitas may appear; door to the graveyard hole area.
- [ ] **Spiral Staircase** [D][K] (a different "Spiral Staircase" exists in the House of Truth) - Ch1 - save: **yes** (clock) - behind mechanical door; go down to Dark Room. Do NOT go back: Debilitas arrives.
- [ ] **The Dark Room** [D] / **Hall of Darkness** [K] - Ch1 - save: no - **darkness**, two invisible floor pits = instant death; send Hewie ahead and follow; shade + Debilitas will not follow (hide); Hewie hidden item on far side. Placard reading [K].
- [ ] **Chapel** [D] / **The Chapel** [K] - Ch1 - save: no (clock is in the Spiral Staircase) - **BOSS: Debilitas**. Goddess statue holds Saturnus Key (CS #038 Saturnus, #039 Stand Against Debilitas). Two chandelier mechanisms/levers (break them to drop chandelier -> CS #040 Fate's Punishment, #041 Adoration, music "His Repentance"/"Solitary Bells" -> Ending A/C route); kill Debilitas with Hewie/items instead -> CS #042 A Moment's Silence -> Ending B route. Church/chandelier effect. Music: "Last Debilitas". [K] notes an in-game quirk: the door opens into the Hall of Darkness for Fiona and into the Chapel for Debilitas.

### 1.5 Old Mansion - Daniella section (Ch2) (39)

[K] prefixes every mansion room "Mansion - ". Entry from the Castle via the Saturn Door at the top of the Block Puzzle Room; once through, the green stairs flood and the castle is unreachable ([W] Places).

- [ ] **Saturnus Corridors** [D] / **Mansion - Long Passageway** [K] - Ch2 - save: no - checkered tile corridor, door slams, Daniella enters; run to end.
- [ ] **(Stairs)** [D, part of Saturnus Corridors] / **Mansion - A Lot of Stairs!** [K] - Ch2 - save: no - at ~3/4 up the stairs CS #047 The Mirror Tells No Lies (Daniella screams at her reflection in mirror).
- [ ] **Green Spiral Stairs** [D] / **Mansion - Waterway** [K] (first visit) ~ **Mansion - Well Well** [K] (later) - Ch2 - save: no - spiral stairs; trying the bottom door -> CS #048 The Great Flood (castle side now flooded); later bottom door is the red **Mars Door** (Mars Key); top door -> Hexagonal Panel Room. K treats the later well-shaped stair tower as a separate "Well Well".
- [ ] **Alchemy Hole Corridor / Alchemy Corridor** [D] / **Mansion - Luminescent Lamp Hall** [K] (+ the lamp room with the clock) - Ch2 - save: **yes** (clock in room on right with the unlit Luminescent lamp) - red curtains (hide), 2nd **AH** (alchemy hole in wall), Lavender, Camomile, balcony overlooking graveyard (useless), unlit lamp: lure blue Luminessant into it -> CS #049 Hidden Stairs (reveals stair behind Mummy Room painting).
- [ ] **Elevator Room** [D] / **Mansion - Shaft with Mirror** [K] / **Bridge Shaft (with a mirror)** [H] - Ch2 - save: no - cloth-covered mirror (pull cloth as brief distraction), Hewie Album Medallion, platform; after the Jupiter/Deception puzzle the elevator is no longer accessible in [D] ([K] then calls it **Shaft Bottom**, a lowered walkway across water to the graveyard).
- [ ] **Above the Mandragora Garden** [D] / **Mansion - Garden Room 2nd Floor Walkway** [K] - Ch2 - save: no - the "Luminessant with a purpose" (blue one) lives here; walkway overlooking the green plant house.
- [ ] **Mummy Room** [D] / **Mansion - I Want My Mummy Passage** [K] / **Hallway with a Mummy** [H] - Ch2 - save: no - sofa with mummified woman/large doll (panic scare), closet (hide, weak), painting in alcove hides stairs (after lamp CS), red door (dead end until Jupiter puzzle).
- [ ] **Large Cage Room** [D] / **Mansion - Fenced Off Room** [K] - Ch2 - save: no - silence; Daniella cleans a statue behind the large cage; first corner door to Restroom; opposite door to Red-Lit Corridor; after the puzzle you can reach the cage side via the Carousel door and Daniella gives Hebel's Perfume.
- [ ] **Mansion Restroom** [D] / **Mansion - Restroom with Passcard Machine** [K] / **Restroom** [H] - Ch2 - save: no - **RP** (sink), plate-key machine, wall chart (SALT/MERCURY/SULFUR), Refined Antimony, shutters -> CS #050 Woman Scorned (starts Daniella chase). Spelling: SULFUR (not SULPHUR).
- [ ] **Red-Lit Corridor** [D] / **Mansion - Red Light Passage** [K] / **Red Light Passageway outside the Alcove** [H] - Ch2 - save: no - Viride Medallion at the red light (Hewie); branch to Carousel Room and East Alcove; red light effect.
- [ ] **Mansion East Alcove** [D] / **Mansion - Relaxation Area** [K] / **Alcove** [H] - Ch2 - save: **yes** (clock beside entrance) - Old Mansion map on table, sofa (hide; D says it works badly), hidden Medallion.
- [ ] **Carousel Room** [D] / **Mansion - Merry-Go-Half-Round** [K] -> after the Jupiter puzzle **Deranged Carousel Room** [D] (same room, rotated) - Ch2 - save: no - half a merry-go-round with calliope music (music: "White Carousel"); after Room of False Truth the room rotates and the animals are bloody/rusted (music: "Red Carousel", warped calliope with effects); door leads on to Dragonfire Corridor / Torture Chamber. [W]: "moose head" in this room.
- [ ] **Dragonfire Corridor** [D] / **Mansion - Serpents Hallway** & **Mansion - Serpents Walkway** [K] (probably the same corridor, K uses both names) - Ch2 - save: no - picture with highlighted lines (clue to restroom chart), vases, locked door (do not unlock yet), stairs to Jupiter rooms, hallway of dolls (Hewie growls), Luminessant, Refined Magnesia on pedestal, doors to Carousel Room, Mandragora Garden, Dragonfire Room, Cemetery.
- [ ] **Dragonfire Room** [D] / **Mansion - Fury of the Serpents** [K] / **Room of the 3-Headed Dragon** [H] - Ch2 - save: no - **fire**: large spiralling flame, three dragon/serpent heads: bottom-left SALT, bottom-right MERCURY, then SULFUR (CS #051 My Fight, #052 Fire & Ice: fire -> ice pillar). Torn letter in a mouth (Lorenzo). Statue on plinth: lure Luminessant into lamp -> Film Reel.
- [ ] **Mansion Study** [D] / **Mansion - Library** [K] (first library, top of the ice pillar) / [H] "Study room above the 3-Head Dragon room" - Ch2 - save: **yes** (clock far right) - kick the ladder to drop the Jupiter Key (D) / "Saturn Key" (K - mislabel); journals; door to shortcut back to the first Alchemy Hole; Hard: Failures.
- [ ] **Mansion - Wooden Walkway** [K] - Ch2 - save: no - plain walkway between Mansion Study and Mammoth Head Room (D: shortcut door on the right of the study).
- [ ] **Mansion - Mammoth Head Room** [K] - Ch2 - save: no - mammoth head with trunk walkway; view of the Chimera room in the background ([W] Places says "moose head" for the carousel room).
- [ ] **Jupiter's Test** [K] ("curved room with two doors" in [D]) - Ch2 - save: no - landing with placard and two stair flights: left to Room of Truth, right to Room of Deception (both Jupiter Key).
- [ ] **Room of Truth** [D][K] - Ch2 - save: no - Daniella crouched at the range (talk = CS #057 Armed and Dangerous: gets hot poker). Memorise the objects.
- [ ] **Room of False Truth** [D] / **Room of Deception** [K] - Ch2 - save: no - spot-the-difference puzzle: chessboard (CS #054), mannequins/wax models (CS #053), hourglass (CS #055); wrong pick -> game over CS #056 The Ultimate Scavengers; wardrobe has Daniella; solving changes the mansion (red doors open, water crossable, Elevator Room gone).
- [ ] **Torture Chamber** [D][K] / **Exorcism Room** [D hide list, game-movie text] - Ch2 - save: no - **puzzle room** (Daniella cannot enter until solved): Iron Maiden (hide = possible game over CS #058 The Iron Maiden), corpse in chair (CS #059 The Immortal, panic grab), coloured panel exorcism with Fiona + Hewie (CS #060 Fleeting Immortality), Venus Key. Reached from the (changed) Carousel Room.
- [ ] **Mandragora Garden** [D][K "Mansion - Mandragora Garden Room"] / **Venus Garden** [D movie list] - Ch2 - save: no - two-storey plant house with palm trees; fountain; mandragora beds (white one needed; Hebel's Perfume lets Hewie find it); on entry CS #062 Mandragora; picking one makes Daniella come; crouch hiding "shade" spot bottom-right in the fountain/pink mandragoras. Door needs Venus Key. Scream effects.
- [ ] **Shaft Bottom** [K] ("hallway of dolls + bridge to cemetery" in [D]) - Ch2 - save: no - after the elevator shaft changes: crossable water walkway to the Graveyard.
- [ ] **Cemetery** [D] / **Mansion - Graveyard** [K] / **Graveyard** [H] - Ch2 - save: no - Luminessants outside; graves all share one name; Beef Jerky in stone circle; bushes (Hewie ambush spot: CS #061 Stick and Move); mausoleum black door with arm: feed it a Mandragora (CS #063 Guardian A white / #064 Guardian B other).
- [ ] **Mansion - Connective Passage** [K] - Ch2 - save: no - from the Mummy Room red door; branch back to the Luminescent Lamp Hall.
- [ ] **Mansion - Chimera Room** [K] - Ch2 - save: no - Chimera statue with Sylvesta Esca in its mouth (also [D] ch2 item box).
- [ ] **Crypt** [D][K "Mansion - The Crypt"][H] - Ch2 (stalker-free; also Ch3 hiding place) - save: no - **sanctuary**: no stalker can enter; Rock Golem + plate-key machine (L/R plate keys: RRL onward, RRR Magist Medallion, RRRR outside incinerator/garden, RRLRR Library; **fire walls** the golem extinguishes), Magist Medallion; SALTATIO plate key = Para-Para dance (#111, heals, once per pursuer, music "Secret Dance"); CS #065 Golem of the Underworld. **RP** via the golem (SALTATIO).
- [ ] **Underwater Passage (Before the Library)** [H] ("underwater corridor" [D], "down underground" [K]) - Ch2 - save: no - water effect corridor from the crypt (RRLRR) up to the library.
- [ ] **Library 1st Floor** [D] / **Mansion - Great Library** [K] / **Library** [H] - Ch2 - save: **yes** (clock on left) - book puzzle (Cathedral Secrets / Occult Philosophy / Wiseman's Crafts), bookcases slide, ladder to Cinema Room; trolley.
- [ ] **Cinema Room** [D] / **Mansion - Projection Room** [K] - Ch2 - save: no - film reel on projector (CS #066 The Hidden Camera shows combination; #067 Snuff Film, Daniella must be alive, prompts pursuer).
- [ ] **Impossible Stairs** [D][K "Mansion - The Impossible Stairs"] - Ch2 / Ch3 - save: no - warped stairs (not a puzzle room), chanting figure; Hewie fetches Album Medallion at top; connects Crypt <-> Red Window Hallway / catwalk.
- [ ] **Mansion West Corridor** [D] / **Mansion - Red Window Hallway** [K] / **West Corridor** [H] - Ch2 - save: no (clock is in Dark Row) - corridor with red window; doors to Examination Room, Machine Room, Dark Row.
- [ ] **Surgery Room** [D] / **Medical Room** [D] / **Mansion - Examination Room** [K] - Ch2 - save: no - **RP** sink in back corner; Lavender on gurney; Debilitas x-ray poster; during a chase you can pick the bottles (CS #068 Acetic Acid A / #069 Sulphuric Acid A for Daniella; #075/#076 B for Riccardo).
- [ ] **Machine Room** [K] / **Synthesiser Room** [D] - Ch2 / Ch3 - save: no - Synthesiser machine (Silver Ore -> White Godstone; the other machines are in Artificial Womb (Extractor), Sealed Room (Purifier), Room of Metamorphosis (Transmuter)).
- [ ] **Mansion - Dark Row** [K] / "curved, caged corridor" [D] - Ch2 - save: **yes** (clock) - cells; stairs down to Combination Lock.
- [ ] **Mansion - Combination Lock** [K] / "locked door with a mechanism" [D] - Ch2 - save: no - three circle dials; door drains the flooded stair (movie combination from the Cinema).
- [ ] **Hexagonal Panel Room** [D] / **Mansion - Crossbow Man Puzzle** [K] - Ch2 - save: no - **puzzle room**: red/black hexagon floor panels, armoured crossbow knight on the far side; Hewie shows safe panels; 1st mistake CS #070 Archer A, 2nd = game over CS #071 Archer B; switch on knight's back disarms.
- [ ] **Second Floor Library** [D] / **Mansion - Great Library Catwalk** [K] / **Catwalk above the Library** [H] - Ch2 - save: **yes** (clock) - Refined Antimony, Hewie Rubrum Medallion; leads to boss room.
- [ ] **Mirror Room** [D] / **Mansion - The Observation Dome** [K] - Ch2 - save: no - **BOSS: Daniella**. Blue glow, four pushable blocks onto coloured pads, mirrored floor circle in centre; luring Daniella onto the mirror -> she screams, **glass ceiling shatters** and impales her (CS #072 Cat Fight, #073 Shish Kebab); Mars Key. Music "Last Daniella".

### 1.6 Old Mansion - laboratory / Riccardo section (Ch2 end, Ch3) (18)

([D] HG15.3 calls the Mars-Door side "Laboratory".)

- [ ] **Circular Alchemy Hole Room** [D] / **Mansion - Circular Passage** [K] - Ch2 (end) - save: no - just through the Mars Door; rubble hide spot, 3rd **AH**, Failures introduced (kick them), Homunculus that hits back; D lists a hall with scales/pedestal/globe (Hewie Medallions) and a locked door + Weeping Stone (see next).
- [ ] **(Hall of scales / pedestals / globe)** [D unnamed] / **Mansion - Hall and Stairs** [K] / ~[H] "Balance in the Corridor" Hallway - Ch2 - save: no - chanting figures, stairs up to Ruined Corridor, locked door openable from the other side later.
- [ ] **Ruined Corridor** [K] / "dirty corridor" [D] / **Ruined Hallway outside the Decomposer Room** [H] - Ch2 - save: no - Prima Magnesia (Hewie), blockage at end to kick down; Riccardo falls down here after the Decomposer trap.
- [ ] **Decomposer Room** [D][K][H] - Ch2 end, Ch3 - save: no - the Decomposer machine (resets Prima Materia chain), weak floor square (stalker trap: Riccardo falls through - cutscene blackout), hole for Hewie (opens Sealed Room), document about Hewie. Ch2 -> Ch3 trigger via the door to the Artificial Womb.
- [ ] **Homonculus Room** [D] ("Green Tank Room" [D] for the Extractor) / **Mansion - Artificial Womb** [K][H] - Ch3 start - save: no - CS at chapter start (Riccardo, #074 The Other Woman prompts pursuer); green tank with woman-like homunculus; the **Extractor** machine (Prima Materia -> Sulphuric Ore); Hewie hidden item; D Ch3: "Run away; follow the corridor around".
- [ ] **Impossible Stairs Catwalk** [K] ~ [D] "curved corridor" - Ch3 - save: no - Riccardo chase starts; leads to Stone Hall.
- [ ] **Mansion - Stone Hall** [K] ~ [D] "circular area in the curved corridor (two Failures drop)" - Ch3 - save: no - junction hall; kick vase; north door to Stone Walkway; stairs up to Sealed Room.
- [ ] **Mansion - Watchers Room** [K] ~ [D] "room with the table" (Medallion on table; also Ch2 item box) - Ch3 - save: no - under the table (hide); Homunculus (ignore).
- [ ] **Mansion - Sealed Room** [K] ~ [D] "the room where Hewie unbarred the door" / **Hallway outside the Purifier Room (Sealed Room)** [H] - Ch3 - save: no - Hewie gets in through the Decomposer hole and lifts the bar (CS #077 No Problem); Viride Medallion; the **Purifier** machine (Sulphuric Ore -> Silver Ore); Riccardo fight arena in K ("make your stand").
- [ ] **Mansion West Alcove** [D] / **Mansion - Western Alcove** [K] - Ch3 - save: **yes** (clock on entering/left) - Camomile on table.
- [ ] **Curved Balcony** [D] / **Mansion - Statuette Room Catwalk** [K] - Ch3 - save: no - Hewie growls; door to the many-doored corridor.
- [ ] **Red Dangerous Corridor** [D] / **Mansion - Hall of Doors** [K] / **Long Hallway with many doors** [H] - Ch3 - save: no - long caged-door hallway; behind doors: Failures, seated Homunculus, jar Medallion, sofa (hide, under sofa at the end); **no music** because of the Homunculus; ends at Cemetery part 2.
- [ ] **Cemetery Part 2** [D] / **Mansion - The Garden** [K] (reached also via the crypt code RRRR) ~ **Small Garden at the Graveyard** [H] - Ch3 (optional in Ch2) - save: no - behind locked gate of the Cemetery; Prima Materia (Hewie) for the Godstone chain; incinerator (Lavender; panic comment); stalker-free crypt reachable.
- [ ] **Mansion - Stone Walkway** [K] ~ [D] "curved balcony with wardrobe and Refined Antimony" - Ch3 - save: no - crossing space to the Closet Room.
- [ ] **Mansion - Closet Room** [K] (D hide list "CLOSET CORRIDOR - in the closet") - Ch3 - save: no - closet (hide), item, door to the Room of Balance.
- [ ] **Scales of Justice Room** [D][W] / **Mansion - The Room of Balance** [K] / [D] "Godstone Scales room" - Ch3 - save: **yes** (clock beside large wooden doors; D: save before choosing, ending-relevant) - statue holding scales: White Godstone (CS #078 A Question of Balances A) or Red Godstone (#079 B); enormous wooden doors lead to the Chaos Forest. [K]: it is the mansion's entrance hall.
- [ ] **Mansion - Red Window Divided Corridor** [K] ~ [D] "next room: Homunculus licking the floor" / **Hallway outside the Transmuter Room** [H] - Ch3 - save: no - divided red-window corridor; Hewie item mid-path; door to Hall and Stairs.
- [ ] **Mansion - Room of Metamorphosis** [K] / **Transmuter Room** [H] / D: "room where Failures are made" (unnamed) - Ch3 - save: no - the Transmuter (White -> Red Godstone; Failures drop in; Homunculus banging head).

### 1.7 Chaos Forest (2 named + sub-screens)

- [ ] **The Forest - Mansion Front** [K] (outside the Scales Room doors: stairs, Homunculi, fence with forest map paper) - Ch3 - save: no - exit from the mansion; outdoor; map "useless".
- [ ] **Chaos Forest** [D][W] / **The Forest - Chaos Forest** [K] - Ch3 - save: **yes** (sundial in the north clearing under the bridge: examine twice) - ~10-minute multi-screen maze, T-junctions, pots; Riccardo shoots Hewie (CS #080 A Stray Dog on entering, #081 Poor Doggie prompts pursuer) - cliff end (CS #082 Checkmate; leads to #083 Riccardo's Lab). **Ending D branch**: if Hewie friendship extremely low and you do not find Hewie, Hewie never rescues you in the Cell. Sub-screens (not counted): start clearing, Hewie's tree (jar Medallion), crossroads w/ Camomile, bridge, cliff edge. Bush hide spots everywhere; Riccardo pursues.

### 1.8 Water Tower (8)

- [ ] **Cell** [D] / **Tower - Cells** [K] / **Prison Cell** [H] - Ch3 - save: no - Fiona wakes (CS #083 Riccardo's Lab, #084 Prison Reunion, #085 Jail Break; Hewie fetches key; #086 The Incredible prompts Riccardo); Bronze Figurine, Tower Sketch, Lorenzo note; the **key-item machine** (Sublimatum Container) here; Philosophers Earrings from Red Godstone [K].
- [ ] **Underground Cavern** [D] / **Tower - Base** [K] / **Water Tower: B1** [H] - Ch3 - save: no - Riccardo is invisible (visibility increases near Fiona); barrels (hide); pipes (Wooden Miniature / Miniature Bridge, Album Medallion, Silver Disc); stairs up.
- [ ] **Alchemy Hole (Water Tower)** [D only] - Ch3 - save: no - up short steps from the cavern; costume message first time; dresser.
- [ ] **Water Tower main room / Entrance** [D "WATER TOWER"] / **Tower - Entrance** [K] / **Water Tower: 1F** [H] - Ch3 - save: **yes** (clock beside the big outside doors) - **RP** sink [D] / old water fountain, lit up [K]; double doors outside (bridge missing until the end); you return here after the boss.
- [ ] **Corkscrew Ramps / spiral slopes (levels 1-4)** [K "Tower - Corkscrew Ramps"] / [D] "Water Tower: levels 1-4" / **Water Tower: 2F** [H] - Ch3 - save: no - bow-shaped platforms; levers; red jar containing a Luminessant; falling barrel; level 4 has many Luminessants (Major Panic -> Riccardo called); Riccardo is invisible and chases.
- [ ] **Planetarium** [D][K "Tower - The Planetarium"] - Ch3 - save: **yes** (clock on entering; [D] also says save again after puzzle) - **puzzle room** (Riccardo cannot enter): 8 floor discs lit by Fiona + Hewie (CS #087 Grand Convergence), music "Fake Planets", planet projection effects, then spiral stairs to the top.
- [ ] **Top of Tower (8F)** [D] / **Tower - The Top** [K] / **Water Tower: 8F** [H] - Ch3 - save: no - **BOSS: Riccardo**: kick down crumbling wall (opposite the model), place Wooden Miniature bridge on the diorama (CS #088 The Viaduct), send Hewie through the hole to the right of the door and ambush from the hole above the diorama; Riccardo falls off the ledge (CS #089 Fall from Grace A, #090 B). Music "Last Riccardo". Holes in walls.
- [ ] **New Stone Bridge** [D] / **Tower - Bridge** [K] - Ch3 -> Ch4 - save: no - bridge built by the model; re-entering the tower triggers CS #091 Azoth; leads to the House of Truth. (Savable at the clock by the Entrance doors before leaving.)

### 1.9 House of Truth (19)

[K] prefixes every room "House of Truth - ". [H] calls the first entrance hall "West Garden".

- [ ] **Entrance Hall** [D][K] / **West Garden** [H] - Ch4 - save: **yes** (clock at the foot of the left stairs between bear statues; D's clock list says the right staircase - inconsistent) - **RP** basin straight ahead; **AH** at the clock; bush (hide); sofa Camomile; right stairs: boxes marked "HIGHLY FLAMMABLE" + dais for the golden candlestick trick (CS #093 The Big Bang blasts Old Lorenzo with a **fire/explosion**; he survives crippled, CS #094). Two doors beside the sink lead to the next hallway.
- [ ] **Sol Hallway** [D] / **House of Truth - Hall of the Dead** [K] - Ch4 - save: no - corridor with bodies packed in the walls; jars (Viola Medallion / Luminessant); fate gate; sun-door at the end; archway stairs down to the Spiral Staircase; Old Lorenzo CS #092 The Man in the Wheelchair (prompts pursuer).
- [ ] **Spiral Staircase** [K][H] / "stairs down at the left archway" [D] - Ch4 - save: no - Golden Candlestick in the wall.
- [ ] **Lorenzo's Room** [K][H] = **Sol Room** [D] (Lorenzo's office, two rooms) - Ch4 - save: no - needs Sol Key per [D]; Rubrum Medallion in bookcase; Mystery Sketch (HoT map); Fortis poster; MAGNUS code poster (2nd play-through).
- [ ] **Long Hallway** [D] / **House of Truth - Dark Corridor** [K] / (**Corridor outside the Demolisher Room** [H]) - Ch4 - save: no - appears after wall is blown up; Rubrum Medallion; CS #094 To Survive! (Old Lorenzo, crippled/survived); twisted pillar; sealed side stairs.
- [ ] **Conveyor Belt Room** [D] / **House of Truth - The Crusher** [K] / **Demolisher Room** [H] - Ch4 - save: no - **BOSS: Old Lorenzo**. Rock crusher/conveyor: start it (CS #095 Conveyor Belt A), crush Lorenzo (#096 B), stepping on moving belt = game over (#097 C). Sol Key drop; sun-marked door leads on. Music "Final Lorenzo" (boss), "Spiral End"/"Warped Obsession" for normal/chase.
- [ ] **(Room beyond the sun door: clock, locked doors)** [D unnamed, start of Ch5] = **House of Truth - Main Hall** [K] - Ch4 -> Ch5 - save: **yes** (clock) - all doors locked except the north door; K: clock "just opposite as you enter"; this is the room where, in K's final run, the statue falls.
- [ ] **Warped Room** [D] / **House of Truth - The Study** [K] - Ch5 - save: no - locked in; the "man's voice" taunts; wardrobe; fake-wall door; bookcase falls; CS #098 Invitation, #099 Thus Spoke Lorenzo; door that changes the room. Fake wall/"warped" geometry.
- [ ] **Changed Room** [D] / **House of Truth - Lobby** [K] - Ch5 - save: **yes** (clock to the right) - Lavender; door to the west locked; stairs up.
- [ ] **(Room at top of stairs)** [D unnamed] / **House of Truth - Revelations** [K] - Ch5 - save: no - CS #100 Return to Youth (prompts Young Lorenzo); Aureolus Lorenzo rejuvenates.
- [ ] **Curving Corridors** [D] / **House of Truth - Long Red Window Corridor** + **Gothic Passage** + **The Door** [K] - Ch5 - save: no - three endless corridors, Young Lorenzo appears from thin air (teleports); music "Closed Mind" (chase).
- [ ] **Coloured Puzzle Rooms** [D] / **House of Truth - Trial of Truth** [K] - Ch5 - save: no - 5 identical rooms with two coloured doors (colour tint effect); D solution Blue, Yellow, Green, Red, Black (right-hand doors); K: Blue, Yellow, Green, Red, Black; wrong = restart with Young Lorenzo interference; music "Hazy Vision".
- [ ] **House of Truth East Garden** [D] / **House of Truth - The Entrance Hall... Maybe Not** [K] / **East Garden** [H] - Ch5 - save: **yes** (clock) - **AH**, **RP** fountain upstairs [K]; mirror-image layout of the Entrance Hall; stairs lead to the Hallway.
- [ ] **House of Truth Hallway** [D] / **House of Truth - The Hall of Shadow** [K] - Ch5 - save: no - long hallway with holes in the walls (two Failures), later the Flaming Lorenzo chase; earthquakes (crouch when the controller vibrates); the jammed door (Hewie "Go!") to Mines.
- [ ] **House of Truth Alcove** [D][H "Alcove"] / **House of Truth - Dark Room** [K] - Ch5 - save: **yes** (clock; D: "save or regret") - shade (Magist Medallion hidden per Lilacatz in [D]); door to Lava Pit Room; Young Lorenzo may appear in shade.
- [ ] **Lava Pit Room** [D] / **House of Truth - Generator Room** [K] - Ch5 - save: no - **BOSS: Young Lorenzo**. Lava pit, two floor-switch fans, empty frame for the Cane of Caduceus (CS #101 Caduceus earlier, #102 Challenge of Lord Lorenzo, #103 Straight to Hell, #104 From the Depths of Hell prompts Flaming Lorenzo); **lava, fans, electricity effects, fire**.
- [ ] **Caverns of Truth** [D] / **House of Truth - Caves** [K] - Ch5 - save: **yes** ([D] mentions a small room with a clock off the passage) - cavern passages, a Failure, Antimony Tube; Hewie barks at a wall -> Cane of Caduceus (CS #101); another door goes back to the Lobby ([K]).
- [ ] **House of Truth - Mines** [K] - Ch5 (escape) - save: no - behind the jammed door in the Hall of Shadow, route back to the Lobby/Main Hall during the Flaming Lorenzo chase (music "Ultimate HAUNTING GROUND"); [D] sends you through the Caverns instead.
- [ ] **Final room / exit** [D unnamed: "final room, clock right of statue, large shining door"] ~ **House of Truth - Main Hall** [K] - Ch5 end - save: **no in practice** ([D]: "You can't save using the clock in here") - statue falls: mash buttons to stand it up; Lorenzo dies (CS #105 Fallen Goddess, #106 Burnt Out) -> credits/Endings.

---

## 2. Counts per area (checked against the lists above)

| Area | Rooms listed |
|---|---|
| Belli Castle - outdoor grounds | 13 |
| Belli Castle - main building | 20 |
| Belli Castle - east wing/annex | 7 |
| Belli Castle - Chapel route | 5 |
| **Belli Castle total** | **45** |
| Old Mansion - Daniella section | 39 |
| Old Mansion - lab / Riccardo section | 18 |
| **Old Mansion total** | **57** |
| Chaos Forest | 2 (+ about 5 unnamed screens) |
| Water Tower | 8 |
| House of Truth | 19 |
| **Total (named rooms/spaces)** | **131** (counted from the checklist lines) |

Other counts from sources: [H] item totals: 60 hidden items + 32 castle / 34 mansion / 5 forest / 5 Water Tower / 9 House of Truth non-hidden items (146 items for 100%; 100% requires playing for Ending A). [H] hidden items by area: Castle 25, Mansion 23 (20 + 3 added in v1.10), Water Tower 5, House of Truth 7. Clocks (union of D and K): Castle 5, Old Mansion 8, Forest 1, Water Tower 2, House of Truth about 7 (Entrance Hall, Main Hall/first Ch5 room, Lobby/Changed Room, East Garden, Alcove/Dark Room, Caverns clock room, final room [unusable]). Alchemy Holes: Castle (Diorama), Old Mansion lamp corridor, Laboratory circular room, Water Tower cavern ([D] only), House of Truth Entrance Hall + East Garden.

---

## 3. Uncertain / unverified

Rooms or mappings that rest on one source or on my inference:

1. **Names found in only one source:** Dungeon [K only]; Secret Passageway [D only; K describes it without naming]; Alchemy Hole (Water Tower) [D only]; Mansion - Wooden Walkway, Mammoth Head Room, Jupiter's Test, Connective Passage, Chimera Room, Shaft Bottom, Impossible Stairs Catwalk, Stone Hall, Watchers Room, Stone Walkway, Closet Room, Red Window Divided Corridor, Hall and Stairs, Dark Row, Combination Lock [K only; D has the contents but not the names]; Training Ground, Gate Area, Tunnel with Holes, Dead End - With Bridge Mechanism, Old Hut Area [K names; D describes unnamed]; Underwater Passage, Transmuter Room, Prison Cell, "Water Tower: B1/1F/2F/8F" [H names]; House of Truth - Mines, Revelations, Dark Corridor, The Door, Gothic Passage, Long Red Window Corridor [K only].
2. **Inferred mappings (route position only, flagged "~" above):** Gate Area ~ H "Courtyard outside the long hallway of holes"; The Passage With Fake Doors ~ H "Courtyard outside the Gallery Room"; Waterway vs Well Well vs Green Spiral Stairs (likely one stair tower); Serpents Hallway vs Serpents Walkway (likely one corridor); Dragonfire Corridor hallway of dolls/bridge ~ Shaft Bottom; Homonculus Room / Green Tank Room / Artificial Womb (likely the same lab space); D "curved corridor" ~ K Impossible Stairs Catwalk + Stone Hall; D "room with the table" ~ Watchers Room; D "room where Hewie unbarred the door" ~ Sealed Room (H confirms Purifier Room = Sealed Room); D "curved balcony" with wardrobe ~ Stone Walkway/Closet Room; D "Homunculus licking the floor" room ~ Red Window Divided Corridor; H "Balance in the Corridor" Hallway ~ the scales/pedestal hall; Lorenzo's Room [K,H] = Sol Room [D] (D says Sol Key needed; K enters on first visit); first Ch5 clock room = K Main Hall; D "final room" vs K "Main Hall" for the end statue event; "Lounge" [H] could be the Sitting Room or the Living Room.
3. **Possible additional rooms not listed because no source names them:** a room between the Castle Study and the Plate Key Room is described only as a corridor; the room(s) behind the Mars Door beyond the Hall and Stairs door "which can be opened from the other side later" ([D]); the hall behind the statue door to the Homunculus-licking room ([D] "UPDATE" paragraph); the "dark room" separate from the HoT Dark Room that [D] mentions in the "Changed Room" description; the Water Tower sub-levels (guides describe levels 1-4 as platforms but do not give room names).
4. **Never covered by any source read:** engine stage list / loading zones. [TCRF, unverified via search summary] mentions `ST_00D.PAC` as an unused stage reachable by cheat, and unused text for "a room next to the castle suite with the big staircase". TCRF page itself could not be read.
5. Chapter assignment: [D] vs [K] disagree on where chapters begin (K ends its "Chapter 1" at Riccardo's brief intro CS after the Wine Cellar, which [D] treats as part of Ch1 ("faints")); shown as D numbering throughout.
6. [W]/[WD] "Places" say the whole Daniella chapter takes place in the "Mansion" and that the Water Tower has "not many rooms but is very big"; they describe the Planetarium at the "top" - consistent with the guides above.

---

## 4. Unlockables / bonus content tied to locations

* **Secret Room** (unlocked by finishing once): it is the Castle Bedroom/Castle Suite ([D] HG15.2, [K], [W]). Contents: costume dresser (Fiona and Hewie), Play Types list on the wall by the bed, concept art, character designs, music list (phonograph by the exit door), movie list (projection screen), **mini-game** "Dog's Best Friend" ([W]; door by the bed): play as Hewie guiding Fiona; [D]: Level 1 Forest/Debilitas -> back to the Old Mansion entrance, Level 2 Castle/Daniella, Level 3 Forest/Riccardo, Level 4 Forest/Flaming Lorenzo; unlock order: [D] "complete Level 2 to unlock 3, Level 3 to unlock 4"; [K] "Level 1 and 2 unlocked; beating Stage 1 unlocks Challenge 3, Stage 2 unlocks Stage 4" (disagreement).
* **Hard Mode** (after one clear): Hewie can die; all stalkers wear alternate costumes; more Failures/Luminessants; some items replaced; clearing gives Hewie's stuffed-dog costume (also needs Top Breeder/Dog Lover title per [D]). Failure in Diorama Room and Mansion Study on Hard.
* **Second play-through**: Castle Gate Key in the Secret Passageway (Ending C); plate-key machine next to the Perpetua Machina in each AH (codes from results titles: ADAMAS, ALCHYMIA, POWDER, MAGNUS, MORGAN, METH, SALTATIO, REST/ST/RE, MERCURY/SALT/SULFUR, EMETH); Faerie/Fairy Earrings make you vanish when standing behind a protruding wall (HoT hallway pillar, Mansion curved corridor).
* **Costumes** ([D] HG15.1): Fiona A (default), Fiona B (clear once), Under the Scalpel (clear once), Illegal in Some States (clear once), Fiona the Frog (Ending C), Texas Cowgirl (Ending A; has a gun); Hewie: White German Shepherd (default), German Shepherd (clear once), Stuffed Dog Outfit (Hard mode clear).
* **Easter egg:** Para-Para Dance - SALTATIO plate key on the golem in the Crypt (#111, music "Secret Dance", heals once per pursuer).
* **Water Tower machine** (Cell): converts leftover key items (cracked plate keys RE/ST, Wilted Flowers, Smudged Key, Castle Gate Key, Jupiter Key (if used only once), the three library books, Hebel's Perfume) to new items ([D] HG15.6).
* **Recovery Points (sinks)** ([D] HG17.1): Castle Restroom; Old Mansion: Restroom, Golem in the Crypt (SALTATIO), Medical Room; Water Tower: Entrance (sink on right of big doors); House of Truth: Entrance Hall.

---

## 5. Reference tables for graphics/sound testing

### 5.1 Cutscene index by trigger ([D] section HG13, numbers = Secret Room movie list; [K] list is the same minus #111)
001 Whereabouts Unknown (enter bedroom) - 002 The Watched (put on clothes) - 003 Enter: Debilitas (walkway) - 004 Infernal Contraption (plate-key machine) - 005 EMETH / 006 METH (golem) - 007 Figure from the Future (Music Room key) - 008 Luminessants (Garden Key door) - 009/010 Evening / Night in the Music Room - 011 Approach Encounter / 012 Exire Debilitas! / 013 Nightmare (kitchen after Castle Sketch) - 014 Freedom (Hewie) - 015 Gate-Crasher - 016 White Intruder - 017 Hewie! - 018 Mountain of Rubble - 019 No Escape (main gate) - 020 What's Cooking? - 021 Pupa Cella - 022 Evil Eyes (GAME OVER) - 023 The Phone's Dubious Ring - 024-026 Bite Attack A/B/C (diorama landing) - 027/028 Balcony Collapses A/B - 029 Flower Arrangement - 030 My Dolly - 031 Hide and Attack - 032 Advice from the Dark - 033 The Drawbridge - 034 Ouroboros (block puzzle) - 035 Just Desserts - 036 Lunar Refractor - 037 Let There Be Light - 038 Saturnus - 039 Stand Against Debilitas - 040 Fate's Punishment / 041 Adoration (chandelier) - 042 A Moment's Silence (no chandelier) - 043 2nd Chance - 044 Dinner Is Served - 045 The Awkward Table - 046 Night Visitor - 047 The Mirror Tells No Lies - 048 The Great Flood - 049 Hidden Stairs - 050 Woman Scorned - 051 My Fight - 052 Fire & Ice - 053-055 The Human Body / Chess Anyone? / The Hourglass - 056 The Ultimate Scavengers (GAME OVER) - 057 Armed and Dangerous - 058 The Iron Maiden (GAME OVER) - 059 The Immortal - 060 Fleeting Immortality - 061 Stick and Move - 062 Mandragora - 063/064 Guardian A/B - 065 Golem of the Underworld - 066 The Hidden Camera - 067 Snuff Film - 068/069 Acetic/Sulphuric Acid A - 070/071 Archer A / B (GAME OVER) - 072 Cat Fight - 073 Shish Kebab - 074 The Other Woman - 075/076 Acetic/Sulphuric Acid B - 077 No Problem - 078/079 A Question of Balances A/B - 080 A Stray Dog - 081 Poor Doggie - 082 Checkmate - 083 Riccardo's Lab - 084 Prison Reunion - 085 Jail Break - 086 The Incredible - 087 Grand Convergence - 088 The Viaduct - 089/090 Fall from Grace A/B - 091 Azoth - 092 The Man in the Wheelchair - 093 The Big Bang - 094 To Survive! - 095-097 Conveyor Belt A/B/C (C = GAME OVER) - 098 Invitation - 099 Thus Spoke Lorenzo - 100 Return to Youth - 101 Caduceus - 102 Challenge of Lord Lorenzo - 103 Straight to Hell - 104 From the Depths of Hell - 105 Fallen Goddess - 106 Burnt Out - 107-110 Endings A-D - 111 Para-Para Dance.

### 5.2 Music tracks ([D] HG14, in-game music list)
01 Endless... HAUNTING GROUND (opening) - 02 Strange Door (title) - 03 Twisted Eyes (Debilitas area) - 04 Innocent Freak (Debilitas chase) - 05 Last Debilitas (boss) - 06 Stalking Insanity (Daniella area) - 07 Something Lacking (Daniella chase) - 08 Last Daniella (boss) - 09 Writhing Embryo (Riccardo area) - 10 Sly Hunter (Riccardo chase) - 11 Last Riccardo (boss) - 12 Spiral End (Lorenzo area) - 13 Warped Obsession (Lorenzo chase) - 14 Final Lorenzo (boss) - 15 Ultimate HAUNTING GROUND (Flaming Lorenzo escape) - 16 Closed Mind (Young Lorenzo chase) - 17 Solitary Bells (after Debilitas defeated) - 18 White Carousel (Carousel Room) - 19 Red Carousel (Deranged Carousel Room) - 20 Fake Planets (Planetarium) - 21 Hazy Vision (coloured door rooms) - 22 Liebestraume Notturno III - lunatic (Music Room) - 23 Liebestraume Notturno III - nostalgic (Secret Room) - 24 His Repentance (chandelier kill) - 25 Precious Hewie - 26 Secret Dance (golem Para-Para) - 27 Endless Zero (credits).
Audio behaviour worth testing (both guides): normal area music disappears (silence) when a stalker is near; Luminessants also remove the music; the Homunculus-filled Red Dangerous Corridor has no music; chase theme starts on sight; the Crypt, Alchemy Holes, puzzle rooms stop stalkers entirely.

### 5.3 Endings and the areas they involve ([D], [K], [W])
* **Ending A "Fortes Fortuna Juvat"** - drop the Chapel chandelier on Debilitas (Hewie friendship grade A per [D]; "good" per [K]). Final scene at the **Castle Main Gate** (key in the gate; Debilitas appears from the double doors holding hedge clippers, bows, trims the hedge). Unlocks Texas Cowgirl costume.
* **Ending B "Ignis Aurum Probat"** - kill Debilitas without the chandelier (Hewie grade C or better). Same Castle Main Gate scene without Debilitas. [W] says A walks into the forest; the two walkthroughs say castle gate - minor disagreement.
* **Ending C "Dona Nobis Pacem"** - needs a previous clear + chandelier kill; visit **Debilitas' Shack** for the Smudged Key -> **Castle Restroom** locked cubicle -> **Secret Passageway** -> Castle Gate Key -> **Castle Main Gate**; extra scene: Lorenzo falls down the **Main Staircase / Entrance Hall** stairs calling "Fiona, don't go". Unlocks Frog costume.
* **Ending D "Tu Fui, Ego Eris"** - Hewie friendship extremely low and do not find Hewie in the **Chaos Forest**; after the cliff CS (#082) Fiona wakes in a glass box (Riccardo's lab), then the **castle bedroom** with an armchair and a pregnant Fiona; game ends there (no Water Tower).
* Order of play: the Lorenzo finale is in the **House of Truth** (CS #105 Fallen Goddess, #106 Burnt Out at the exit); Endings A/B are then shown (final gate scene). Ending C is a shortcut: on a second play-through the Castle Gate Key lets you leave from the castle gate right after the Debilitas chapter, which ends the game early ([D]). Ending D ends the game after the Chaos Forest cliff scene (Hewie never rescues Fiona).
* **Game Over** screen "Acta Est Fabula"; room-specific death scenes: Red Marionette Room (Evil Eyes), Room of False Truth (Ultimate Scavengers), Torture Chamber Iron Maiden, Hexagonal Panel Room (Archer B), Hall of Darkness / Dark Room (fall), Conveyor Belt Room (belt).

---
End of file. Sources listed in section 0.1; tags [D] [K] [H] [W] refer to them.
