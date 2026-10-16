export const meta = {
  name: 'metroid-lorebook-shard',
  description: 'Research, draft, adversarially fact-check and revise SillyTavern lorebook entries for a shard of Metroid canon domains',
  phases: [
    { title: 'Draft', detail: 'one researcher-writer per lore domain' },
    { title: 'Verify', detail: 'adversarial canon fact-check per domain' },
    { title: 'Revise', detail: 'apply corrections, write final entries to disk' },
  ],
}

const SCRATCH = '/tmp/claude-0/-home-user-wiphone-meshtastic/e73f209f-7803-522a-b473-d77076ab0347/scratchpad'

const DOMAINS = [
  { slug: 'samus-bio', cat: 'Samus', title: 'Samus Aran — biography and physiology', min: 6, max: 9,
    brief: "Samus Aran's biography and physiology: birth on the Earth colony K-2L, parents Rodney and Virginia Aran, the Space Pirate raid led by Ridley that destroyed the colony when she was three, adoption by the Chozo (Old Bird, Gray Voice) and upbringing on Zebes, the infusion of Chozo blood/DNA to let her survive Zebes and wield Chozo technology, Chozo training and the Power Suit, service in the Galactic Federation (Federation Police / Federation Army under Adam Malkovich), her departure and bounty-hunting career, her reputation ('the Hunter'), the Metroid vaccine at the BSL station (Metroid DNA, X immunity, cold vulnerability), Raven Beak's revelations in Dread about Thoha and Mawkin DNA in her and her Metroid transformation and recovery, physical description and stats (height/weight as given by official Nintendo sources — verify the figures and attribute them), age notes (official sources avoid a number; the manga gives ages at K-2L and when she left Zebes — verify)." ,
    wishlist: ['Samus Aran – core profile', 'Samus Aran – early life on K-2L', 'Samus Aran – raised by the Chozo on Zebes', 'Samus Aran – Galactic Federation service and the bounty hunter years', 'Samus Aran – appearance, physique and physiology', 'Samus Aran – Metroid DNA, Chozo DNA and what she has become', 'Rodney Aran and Virginia Aran', 'K-2L (Samus\'s home colony)'],
    avoid: 'Her personality, speech and roleplay portrayal (samus-rp); suits/weapons/abilities (suits, weapons, abilities); Ridley\'s own entry (ridley); Adam Malkovich\'s own entry (adam-other-m); manga-only characters Kreatz/Mauk and detailed manga plot (manga); Old Bird and Gray Voice entries (chozo).' },
  { slug: 'samus-rp', cat: 'Samus', title: 'Samus Aran — personality, voice and roleplay portrayal', min: 5, max: 7,
    brief: "How Samus thinks, speaks and behaves, for roleplay portrayal grounded in canon: personality (stoic, disciplined, laconic, professional, compassionate beneath the reserve, fiercely independent, dry), speech patterns across the series (terse mission reports; the reflective first-person narration of Fusion and Other M; her few spoken lines in Dread, Samus Returns, the Prime games and Prime 4; Japanese vs English script differences where they change her characterization; the ending monologues), canonical quotes with context (verify wording), mannerisms (the thumbs-up/thumbs-down she shared with Adam, curt nods, helmet removal moments, how she acknowledges orders), habits in the field (scanning, methodical exploration, dismantling Pirate operations), attitude toward the Galactic Federation, the Chozo, the Space Pirates, Metroids and the baby, Ridley (trauma per Other M and the manga), Adam, Anthony Higgs, Dark Samus, Sylux; what she values and will not do; how she treats civilians, allies and enemies; her rare humor. Include a clear 'how to portray Samus' guidance entry with dos and don'ts and 6-10 sample lines in her voice (original lines in her register, clearly not fake quotes). The portrayal guidance entry is one of the two constant entries in the whole lorebook: set constant=true on it, keep it 250-350 words.",
    wishlist: ['Samus Aran – personality and temperament', 'Samus Aran – speech style and canonical quotes', 'Samus Aran – mannerisms, habits and field behavior', 'Samus Aran – emotional history and inner conflicts', 'Samus Aran – attitudes toward the Federation, Chozo, Pirates and Metroids', 'Portraying Samus – roleplay guidance (constant)'],
    avoid: 'Biographical timeline details beyond what the portrayal needs (samus-bio); equipment (suits/weapons/abilities).' },
  { slug: 'suits', cat: 'Equipment', title: 'Samus\'s armor: Power Suit lineage and variants', min: 7, max: 10,
    brief: "Samus's armor, described in-universe: the Chozo-made Power Suit (its bond to Samus, how it is summoned/dismissed, its dependence on her mental state per Other M and the manga, the suit she wore on her first Zebes mission vs the Legendary/Fully Powered Suit granted by the Ruins Test in Chozodia), Varia Suit (heat and cold protection), Gravity Suit (liquids and gravity), Phazon Suit (Tallon IV; later stolen), PED Suit and Hypermode (the Federation's Phazon Enhancement Device), Dark Suit and Light Suit (Luminoth), Fusion Suit (organic suit after the Metroid vaccine; colors; cold weakness; the infected suit parts that became the SA-X), the suit in Dread and its final transformation into the Metroid Suit, the Zero Suit (the skin-tight under-suit) and the Paralyzer, Energy Tanks/Reserve Tanks and suit energy as in-universe health, any official suit designations (verify, e.g. 'Chozo Battle Suit' labels), and the Prime 4 'Legacy Suit' / 'Vi-O-La suit' only if canon (verify what these actually are)." ,
    wishlist: ['Power Suit (Samus\'s Chozo armor)', 'Varia Suit', 'Gravity Suit', 'Phazon Suit and PED Suit (Hypermode)', 'Dark Suit and Light Suit', 'Fusion Suit', 'Metroid Suit (Dread)', 'Zero Suit and Paralyzer', 'Energy Tanks and suit energy'],
    avoid: 'Weapons and beams (weapons); movement abilities and visors (abilities); the full story of how each suit was obtained beyond a line (game domains).' },
  { slug: 'weapons', cat: 'Equipment', title: 'Samus\'s weaponry', min: 8, max: 12,
    brief: "Samus's weapons, in-universe: the Arm Cannon, Power Beam, Long Beam, Charge Beam, Ice Beam, Wave Beam, Spazer Beam, Plasma Beam, Wide Beam, Diffusion Beam, Hyper Beam (the baby's gift in Super Metroid; Dread's Metroid-power Hyper Beam), Phazon Beam, Dark/Light/Annihilator Beams (Luminoth), Nova Beam, Grapple Beam / Grapple Lasso / Grapple Voltage, Missiles, Super Missiles, Ice Missiles, Diffusion Missiles, Seeker Missiles, Storm Missiles, Bombs, Power Bombs, Cross Bombs, the Melee Counter (Samus Returns/Dread), the Omega Cannon / Omega Blaster / Omega Stream derived from E.M.M.I. Central Units in Dread, Charge Combos (Super Missile, Wavebuster, Ice Spreader, Flamethrower, Sunburst, Darkburst, Sonic Boom), beam stacking vs selecting, ship weapons in Prime 3 (Ship Missile, Ship Grapple — brief), and Prime 4's psychic weapon applications only if canon and verified (names must be exact). Describe effects, origins (Chozo, Luminoth, Federation, Pirate) and typical use.",
    wishlist: ['Arm Cannon and Power Beam', 'Charge Beam and Charge Combos', 'Ice Beam', 'Wave Beam, Spazer and Plasma Beam', 'Wide, Diffusion and Hyper Beams', 'Luminoth and Phazon beams (Dark, Light, Annihilator, Phazon, Nova)', 'Missiles and Super Missiles', 'Ice, Diffusion, Seeker and Storm Missiles', 'Bombs, Power Bombs and Cross Bombs', 'Grapple Beam', 'Melee Counter', 'Omega Cannon (Dread)'],
    avoid: 'Suits (suits); Morph Ball, Screw Attack, Speed Booster, visors (abilities).' },
  { slug: 'abilities', cat: 'Equipment', title: 'Samus\'s movement, exploration and sensory abilities', min: 7, max: 10,
    brief: "Samus's movement, exploration and sensory abilities, in-universe: Morph Ball (Chozo technology only Samus can use among humans — verify the official claim), Spring Ball, Boost Ball, Spider Ball, Spider Magnet (Dread), Hi-Jump Boots, Space Jump, Screw Attack, Speed Booster and Shinespark, Wall Jump, Slide, Flash Shift and Phase Shift (Dread), Phantom Cloak, Pulse Radar, the Aeion abilities of Samus Returns (Scan Pulse, Lightning Armor, Beam Burst, Phase Drift) and Aeion energy itself, the Visors (Combat Visor, Scan Visor, Thermal Visor, X-Ray Visor / X-Ray Scope, Dark Visor, Echo Visor, Command Visor), the helmet HUD, and Prime 4's psychic abilities and the Vi-O-La motorcycle (verify exact official names and what each does; cross-check two sources because the game is recent).",
    wishlist: ['Morph Ball and Spring Ball', 'Boost Ball, Spider Ball and Spider Magnet', 'Hi-Jump, Space Jump and Wall Jump', 'Screw Attack', 'Speed Booster and Shinespark', 'Flash Shift, Phase Shift, Phantom Cloak and Slide (Dread-era movement)', 'Aeion abilities (Scan Pulse, Lightning Armor, Beam Burst, Phase Drift, Pulse Radar)', 'Visors (Combat, Scan, Thermal, X-Ray, Dark, Echo, Command)', 'Psychic abilities and the Vi-O-La (Viewros)'],
    avoid: 'Beams, missiles, bombs, grapple, melee counter (weapons); suits (suits); Prime 4 story (prime4).' },
  { slug: 'chozo', cat: 'Chozo', title: 'The Chozo', min: 7, max: 10,
    brief: "The Chozo: avian species and ancient advanced civilization; teachers and uplifters of other races; their technology (Chozo Statues, item pods, Power Suit technology, Mother Brain's creation, Elysia's construction); their philosophy of peace, their decline and departure/transcendence; settlements (Zebes, Tallon IV, SR388, Elysia, ZDR); the Chozo of Tallon IV and their Chozo Lore (abandoning technology, the prophecy of the Worm, Chozo Ghosts); Chozo language and script; the tribes Thoha (scientists; created the Metroids on SR388; low tolerance for violence; their DNA lets one control Metroids) and Mawkin (warrior tribe); the Chozo Memories recorded on SR388 (Samus Returns); Old Bird (Samus's foster father; his role in the manga and games — verify what he does on-screen); Gray Voice (manga; verify his exact relationship to Samus and his fate); Chozo Statues and Torizo; the 'Chozo blood' in Samus and the Chozo's view of her ('the Hatchling' — verify the term's usage); the Chozo's status today.",
    wishlist: ['Chozo (species and civilization)', 'Chozo history, philosophy and decline', 'Chozo tribes – Thoha and Mawkin', 'Old Bird', 'Gray Voice', 'Chozo Statues, item pods and Torizo', 'Chozo Memories and the Chozo of SR388', 'Chozo language and the Chozo Lore records of Tallon IV', 'Chozo Ghosts'],
    avoid: 'Raven Beak, Quiet Robe, Chozo Soldiers, ZDR (dread); Tallon IV region details (prime1); Mother Brain (zebes); Samus\'s upbringing narrative (samus-bio); Elysians (prime3); the manga\'s plot beats (manga).' },
  { slug: 'metroids', cat: 'Metroids', title: 'Metroids', min: 7, max: 10,
    brief: "Metroids: the species created by the Thoha Chozo on SR388 as predators for the X Parasite; the name's Chozo meaning ('ultimate warrior' — verify), biology (energy-draining, jellyfish-like larvae with nuclei and membranes), weakness to cold (Ice Beam then missiles), beta rays accelerating growth, the SR388 life cycle (Larva, Alpha, Gamma, Zeta, Omega, Queen) and why it only occurs on SR388, mutations elsewhere (Tallon Metroid, Hunter Metroid, Fission Metroid, Phazon Metroid, Metroid Hatcher, Hopping Metroid, Dark Tallon Metroid), the Metroid Prime creature (one line; prime1 owns it), the Federation's and Pirates' obsession with weaponizing them, the extermination order and Samus's role, the baby Metroid ('the baby', 'the hatchling': hatching, imprinting on Samus, Ceres, Zebes, its growth, sacrifice and the Hyper Beam), the Metroid Queen, Omega Metroids (SR388, BSL; verify Dread), Metroid cloning programs (Bottle Ship Sector Zero, BSL's Restricted Lab, Raven Beak on ZDR, Metroids used by Sylux on Viewros), Samus as 'the last Metroid' after Dread, Metroid eggs.",
    wishlist: ['Metroids (species overview)', 'Metroid biology and weaknesses', 'Metroid life cycle (Alpha, Gamma, Zeta, Omega, Queen)', 'Metroid variants of the Prime era (Tallon, Hunter, Fission, Phazon Metroids, Hatcher)', 'The baby Metroid (the hatchling)', 'Metroid Queen', 'Omega Metroid', 'Metroid breeding and cloning programs', 'Samus as the last Metroid'],
    avoid: 'X Parasite biology (x-parasite); Metroid Prime the creature (prime1); Dark Samus (dark-samus); the Thoha/Mawkin story on ZDR (dread); Metroid Suit (suits).' },
  { slug: 'x-parasite', cat: 'X Parasite', title: 'The X Parasites', min: 6, max: 8,
    brief: "The X Parasites: gelatinous SR388 parasites that infect, kill and perfectly mimic hosts, absorbing memories and abilities; reproduction and spread; Core-X and Hard Core-X; color variants (yellow/green/red/blue X and their effects when Samus absorbs them); the Metroids as their only natural predator; the Thoha Chozo creating Metroids to stop them (the Chozo Archives in Dread); the BSL outbreak (how it began on SR388 with a Hornoad, Samus's infection and crash, the Metroid vaccine making her immune and able to absorb X); the SA-X (born from Samus's infected suit parts; its abilities; its multiplication to around ten copies; the final SA-X's behavior); named X-mimicked creatures of the BSL (Arachnus-X, Zazabi, Serris-X, B.O.X., Nettori, Nightmare-X, Yakuza, Neo-Ridley); the X on ZDR in Dread (how they were contained in Elun and released; the Chozo Soldier-X, Quiet Robe-X, Raven Beak-X, Experiment Z-57 if X-related — verify); the destruction of SR388 and BSL as the end of the X apart from ZDR; Samus's immunity and her view of them.",
    wishlist: ['X Parasites (species overview)', 'X Parasite biology, mimicry and Core-X', 'SA-X', 'The X outbreak aboard the BSL station', 'X-mimicked creatures of the BSL (named bosses)', 'The X on ZDR (Dread)', 'Samus and the X (immunity, absorption, attitude)'],
    avoid: 'BSL station layout and non-X plot (fusion); Metroid biology (metroids); Nightmare\'s origin on the Bottle Ship (adam-other-m).' },
  { slug: 'space-pirates', cat: 'Space Pirates', title: 'The Space Pirates', min: 7, max: 10,
    brief: "The Space Pirates: what they are (verify whether Metroid Wiki treats them as a species, a multi-species organization, or both; Zebesians as a type), organization (High Command, Science Team, military ranks), leadership (Mother Brain on Zebes, Ridley as commander, Dark Samus later), bases (Zebes, Tallon IV, Aether presence, the Pirate Homeworld — verify its name Urtraghus, the Frigate Orpheon, the Bermuda System), aims (Metroid weaponization, Phazon, conquest), projects (Project Helix, the Elite Pirate project, Omega Pirate, Metroid Prime containment, others verified from Pirate Data), unit types (Zebesian Pirates/Kihunters, Pirate Troopers, Aerotroopers, Shadow Pirates, Flying Pirates, Commando Pirates, Elite and Omega Pirates, Pirate Militia), technology and ships, their culture per the Pirate Data logs (bureaucratic, cruel, darkly comic), their view of Samus ('the Hunter') and her vendetta, and their fate (crippled after Phazon's end; Zebesian clones on the Bottle Ship; Sylux's Pirates in Prime 4).",
    wishlist: ['Space Pirates (overview)', 'Space Pirate organization, High Command and Science Team', 'Zebesians and Kihunters', 'Space Pirate troop types (Prime era)', 'Elite Pirates and the Omega Pirate', 'Space Pirate projects and Phazon research', 'Space Pirate ships and the Frigate Orpheon', 'Pirate Homeworld', 'Space Pirates and Samus (the Hunter)'],
    avoid: 'Ridley (ridley); Mother Brain (zebes); Weavel (hunters); Dark Samus (dark-samus); Phazon itself (phazon).' },
  { slug: 'ridley', cat: 'Ridley', title: 'Ridley', min: 6, max: 8,
    brief: "Ridley, Space Pirate commander and Samus's nemesis: species (space dragon; the 'Geoform 187' designation per Prime scans — verify), intelligence and cruelty, his history with Samus (leading the K-2L raid that killed her parents when she was three; his taunting of her per the manga; her trauma), every incarnation in order: Ridley on Zebes (her first mission), Meta Ridley (cybernetic rebuild, Tallon IV), Omega Ridley (Phazon-corrupted, Pirate Homeworld), Proteus Ridley (SR388, Samus Returns; his attempt to take the baby), Ridley at Ceres and Zebes (stole the baby; killed in Norfair), the Bottle Ship clone (Little Birdie → Mystery Creature → adult Ridley; Samus's panic; killed by Metroids and his body taken), Neo-Ridley / Ridley-X at the BSL, the Ridley Robot / Mecha Ridley (Zero Mission), his regeneration and repeated survival, appearance and abilities (tail, plasma breath / fireballs, flight), and how Samus speaks of him.",
    wishlist: ['Ridley (overview and relationship with Samus)', 'Ridley – history across every encounter', 'Meta Ridley and Omega Ridley', 'Proteus Ridley', 'Ridley clone of the Bottle Ship (Little Birdie)', 'Neo-Ridley / Ridley-X', 'Mecha Ridley'],
    avoid: 'General Space Pirate organization (space-pirates); how Samus behaves around her trauma (samus-rp) — you cover the facts.' },
  { slug: 'zebes', cat: 'Zebes', title: 'Planet Zebes, Mother Brain and the first Zebes mission', min: 7, max: 9,
    brief: "Planet Zebes: the Chozo world where Samus was raised and the Space Pirates' stronghold. Geography and regions (Crateria, Brinstar, Norfair, Kraid's Lair, Ridley's Lair, Maridia, the Wrecked Ship, Tourian, Chozodia, the Chozo Ruins, the Space Pirate Mother Ship), climate, gravity and atmosphere (verify), history (Chozo colony; Mother Brain; the Pirates' takeover; Samus's childhood there), Mother Brain in full (Chozo-built biomechanical AI; her role; betrayal of the Chozo; Zebetites and Rinkas; her Super Metroid second form; her link to Other M's MB in one line), Kraid in full (size, Kraid's Lair, his appearance on ZDR in Dread — verify how he got there), the events of Samus's first Zebes mission (Zero Mission: infiltration, Kraid and Ridley, Tourian, Mother Brain's destruction, being shot down, the Zero Suit infiltration of the Mother Ship, the Ruins Test and the Legendary Power Suit, Mecha Ridley, the Mother Ship's destruction), and Zebes's destruction at the end of Super Metroid (one line; super-metroid owns the narrative).",
    wishlist: ['Zebes (planet overview)', 'Regions of Zebes (Crateria, Brinstar, Norfair, Maridia, Tourian)', 'Chozodia, the Chozo Ruins and the Space Pirate Mother Ship', 'Mother Brain', 'Kraid', 'Samus\'s first Zebes mission (Zero Mission)', 'Tourian', 'The destruction of Zebes'],
    avoid: 'Super Metroid\'s events and bosses (super-metroid); Ridley (ridley); Chozo civilization (chozo); Samus\'s childhood details (samus-bio); Torizo (chozo).' },
  { slug: 'super-metroid', cat: 'Super Metroid', title: 'The return to Zebes (Super Metroid)', min: 6, max: 8,
    brief: "The events after SR388: Samus delivers the baby Metroid to the Ceres Space Colony, scientists study it, Ridley attacks Ceres and steals the baby, Samus returns to Zebes and finds the Pirates have rebuilt; the Wrecked Ship (verify its origin) and Phantoon, Maridia and Draygon, Norfair and Crocomire, the Golden Torizo, Spore Spawn, Botwoon, Kraid and Ridley (one line each), the Dachora and Etecoons (Samus spares/saves them and they escape with her), Tourian, the giant Metroid (the grown baby) attacking Samus and then recognizing her, Mother Brain's new body, the baby's death, the Hyper Beam, Zebes's destruction and Samus's escape; the aftermath and the emotional weight on Samus (Other M's opening). Treat bosses as in-universe creatures or machines.",
    wishlist: ['Ceres Space Colony and the theft of the baby', 'The return to Zebes (Super Metroid mission overview)', 'Phantoon and the Wrecked Ship', 'Draygon and Maridia', 'Crocomire, Golden Torizo and the guardians of Norfair', 'Spore Spawn and Botwoon', 'Mother Brain\'s final battle and the baby\'s sacrifice', 'Dachora and Etecoons'],
    avoid: 'Mother Brain\'s general entry (zebes); Kraid (zebes); Ridley (ridley); Zebes regions (zebes); the baby Metroid\'s general entry (metroids).' },
  { slug: 'federation', cat: 'Galactic Federation', title: 'The Galactic Federation', min: 7, max: 9,
    brief: "The Galactic Federation: founding (per the original manual's framing — verify the wording about the year 2000 of the cosmic calendar), the capital Daiban, the Council/Chairman (Chairman Keaton belongs to adam-other-m; mention only), institutions: Federation Police, Federation Army/Military, Galactic Federation Marines, the Federation Navy and named ships (GFS Olympus, GFS Valhalla, GFS Anhur, GFS Theseus, GFS Tyr, and any others verified), Aurora Units (organic supercomputers; Aurora Unit 242, 217, 313 and others verified), Fleet Admiral Castor Dane, the bounty-hunter system (how hunters are contracted; the Federation's reliance on Samus), the Federation's Metroid policy (the extermination order; secret breeding on the Bottle Ship and the BSL; cover-ups; Samus's eroding trust; her status after Fusion and in Dread), Federation technology (one line pointing to Mechs/PED/Aurora entries), the Federation's member species, trooper culture, and its relationship with Samus (respect, suspicion, exploitation).",
    wishlist: ['Galactic Federation (overview)', 'Federation Police, Army and Marines', 'Galactic Federation Navy and notable ships', 'Aurora Units', 'Fleet Admiral Castor Dane', 'The Federation\'s Metroid policy and secret programs', 'Bounty hunters and the Federation', 'Daiban'],
    avoid: 'Adam Malkovich and Chairman Keaton (adam-other-m); Federation Force (federation-force); Prime 4 Federation characters (prime4); Rundas/Ghor/Gandrayda (prime3).' },
  { slug: 'adam-other-m', cat: 'Other M', title: 'Adam Malkovich, the Bottle Ship and Other M', min: 8, max: 10,
    brief: "Adam Malkovich (Samus's former commanding officer; personality; 'Any objections, Lady?'; the thumbs-down; his brother Ian's death; his sacrifice in Sector Zero), the Adam AI aboard the BSL (Fusion's computer CO revealed to carry Adam's uploaded mind; his authorization of Samus's plan; Dread's ADAM aboard Samus's gunship in one line — dread owns the impersonation twist), Anthony Higgs ('Princess', the Plasma Gun, survives), Madeline Bergman, MB / Melissa Bergman (android AI built from Mother Brain's data; awakened emotions; the Bottle Ship rebellion; her death), the 07th Platoon (James Pierce as the Deleter, Lyle Smithsonian, Maurice Favreau, K.G. Misawa), Chairman Keaton, the Bottle Ship (Main Sector, Biosphere, Pyrosphere, Cryosphere, Bioweapon Research Center, Sector Zero, Room MW, the 'Baby's Cry' distress signal), the Bottle Ship plot (cloned Zebesians and Ridley, Metroid cloning without cold weakness in Sector Zero, Nightmare's creation as a Federation gravity-manipulation bioweapon, the Deleter's murders, the Federation cover-up), Samus's deferral to Adam's authorizations (in-universe framing), Phantoon's appearance in the epilogue (one line), the ending and Adam's helmet. Note Japanese/English script differences where meaningful.",
    wishlist: ['Adam Malkovich', 'The Adam AI (BSL computer)', 'Anthony Higgs', 'Madeline Bergman and MB (Melissa Bergman)', 'The 07th Platoon and the Deleter', 'The Bottle Ship', 'The Bottle Ship incident (Other M events)', 'Nightmare (origin and the Bottle Ship)', 'Chairman Keaton'],
    avoid: 'Ridley clone details (ridley); Mother Brain (zebes); Nightmare-X at the BSL (x-parasite).' },
  { slug: 'sr388', cat: 'SR388', title: 'Planet SR388 and the Metroid extermination mission', min: 5, max: 7,
    brief: "Planet SR388: homeworld of the Metroids and the X Parasites; the Thoha Chozo research colony and its loss; geography (surface, the chasms, the areas of Samus Returns, the Chozo ruins and laboratories, the lava-flooded depths, the Metroid nest); the Federation's extermination order after its research teams vanished (verify which ships/teams were lost) and the Pirates' interest; Samus's mission (hunting every Metroid, each evolution stage, the Chozo Memories, Aeion, the Diggernaut, Proteus Ridley's ambush in one line, the Metroid Queen, the baby's hatching and imprinting); native fauna in one line (bestiary-2d owns); the planet's destruction by the BSL collision in one line (fusion owns). Prefer Samus Returns' retelling over Metroid II where they differ, and say so once.",
    wishlist: ['SR388 (planet overview)', 'The Metroid extermination mission on SR388 (Samus Returns)', 'Diggernaut', 'The Chozo laboratories of SR388', 'Areas of SR388'],
    avoid: 'Metroid biology/life cycle and the Metroid Queen entry (metroids); the Chozo Memories\' content (chozo); Proteus Ridley (ridley); SR388\'s destruction details (fusion); Aeion ability mechanics (abilities).' },
  { slug: 'fusion', cat: 'Fusion', title: 'The BSL incident (Metroid Fusion)', min: 6, max: 8,
    brief: "The BSL incident: Samus escorting a Biologic Space Laboratories survey team on SR388, infection by an X, the crash of her gunship, the Metroid vaccine and the Fusion Suit; the Biologic Space Laboratories research station and its sectors (Main Deck, SRX, TRO, PYR, AQA, ARC, NOC) with their habitats; the quarantine capsule explosion and the outbreak; her new computer CO (one line; adam-other-m owns the Adam AI); where and when each X-mimicked threat occurred in the mission (Arachnus, Zazabi, Serris, B.O.X., Nettori, Nightmare, Yakuza, Neo-Ridley, the SA-X encounters) without re-explaining their biology; the Restricted Laboratory and the Federation's secret Metroid breeding program (what was there, its ejection and destruction); the Omega Metroid; the Federation's order to let the X be captured; Samus's decision to drop the station into SR388 destroying both; the escape, the ending monologue, and Samus's standing afterwards (Dread's opening shows the Federation still works with her); the Etecoons and Dachora rescued again.",
    wishlist: ['The BSL incident (Metroid Fusion events)', 'Biologic Space Laboratories research station (BSL)', 'Sectors of the BSL station', 'The Restricted Laboratory and the secret Metroid program', 'Omega Metroid of the BSL', 'The destruction of the BSL and SR388', 'Samus\'s defiance of the Federation at the BSL'],
    avoid: 'X biology, SA-X and the X-mimics\' nature (x-parasite); the Adam AI (adam-other-m); Fusion Suit (suits); the vaccine\'s effects on Samus\'s body (samus-bio).' },
  { slug: 'dread', cat: 'Dread', title: 'Planet ZDR and Metroid Dread', min: 9, max: 12,
    brief: "Metroid Dread: planet ZDR and its areas (Artaria, Cataris, Dairon, Burenia, Ferenia, Ghavoran, Elun, Hanubia, Itorash); why Samus went (the Federation received a video of a live X; seven E.M.M.I. were sent and contact was lost; Samus is immune); the E.M.M.I. (Extraplanetary Multiform Mobile Identifiers: Federation research robots; their near-indestructible shells; DNA extraction; each unit E.M.M.I.-01P through 07B and its abilities; Central Units; the Omega Cannon in one line — weapons owns mechanics); Raven Beak (Mawkin leader; his Power Suit resembling Samus's; his plan to clone Metroids from her DNA and rule; his X-infected end); Quiet Robe (Thoha scientist; aided Samus; killed by a Robot Chozo Soldier; his X later helps her); the Chozo Soldiers and Robot Chozo Soldiers; the ZDR backstory (Thoha and Mawkin cooperation; Raven Beak's coup and the Thoha massacre except Quiet Robe; the X release that killed the Mawkin; the Elun containment); the ADAM impersonation twist; Kraid's presence in one line (zebes owns Kraid); creatures Corpius, Drogyga, Escue, Golzuna, Experiment Z-57; Samus's awakening Metroid powers; Itorash; the Metroid Suit in one line (suits owns); the final battle, ZDR's destruction and the ending (verify precisely how Samus regains control and escapes).",
    wishlist: ['Planet ZDR', 'Areas of ZDR', 'E.M.M.I. (Extraplanetary Multiform Mobile Identifiers)', 'The E.M.M.I. units (01P–07B) and Central Units', 'Raven Beak', 'Quiet Robe', 'Chozo Soldiers of the Mawkin', 'The ZDR mission (Metroid Dread events)', 'The Chozo of ZDR – Thoha, Mawkin and Raven Beak\'s coup', 'Creatures of Dread: Corpius, Drogyga, Escue, Golzuna, Experiment Z-57', 'Itorash and the end of ZDR'],
    avoid: 'Metroid Suit (suits); Omega Cannon mechanics (weapons); the Adam AI\'s history (adam-other-m); Kraid\'s bio (zebes); X biology (x-parasite); the Chozo species (chozo).' },
  { slug: 'prime1', cat: 'Prime', title: 'Tallon IV and Metroid Prime', min: 7, max: 9,
    brief: "The Tallon IV incident: the distress signal from the Space Pirate Frigate Orpheon (the Parasite Queen; Meta Ridley's escape), Tallon IV (the Chozo settlers who abandoned technology; the Chozo Lore; the Leviathan impact and the 'Great Poison'; the Cipher of twelve Chozo Artifacts; the Chozo Ghosts), the regions (Tallon Overworld, Chozo Ruins, Magmoor Caverns, Phendrana Drifts, Phazon Mines, Impact Crater), the Pirates' Phazon operations and the Pirate Data, Flaahgra, Thardus, the Omega Pirate, Meta Ridley in one line, Metroid Prime (the creature: a Metroid mutated by Phazon, 'the Worm' of prophecy, its forms, its theft of the Phazon Suit and rebirth as Dark Samus), the Artifact Temple, the ending, and Samus's place in the Chozo prophecy (verify the exact terms used).",
    wishlist: ['The Tallon IV incident (Metroid Prime events)', 'Tallon IV', 'Regions of Tallon IV', 'Frigate Orpheon and the Parasite Queen', 'The Chozo of Tallon IV and the Chozo Lore prophecy', 'Chozo Artifacts and the Artifact Temple', 'Flaahgra, Thardus and the Omega Pirate', 'Metroid Prime (the creature)', 'Impact Crater'],
    avoid: 'Phazon\'s nature and Leviathans (phazon); Meta Ridley (ridley); Dark Samus\'s later life (dark-samus); Pirate organization (space-pirates); ordinary creatures (bestiary-prime).' },
  { slug: 'hunters', cat: 'Prime Hunters', title: 'The Alimbic Cluster and the rival hunters', min: 8, max: 11,
    brief: "The Alimbic Cluster incident (Metroid Prime Hunters): the telepathic message about 'the secret to ultimate power' in the Alimbic Cluster (verify wording), the Alimbics (extinct telepathic race; their history; the Alimbic Order; Gorea's arrival and their sacrifice), Gorea (shapeshifting entity sealed in the Oubliette; its forms; the Alimbics' Omega Cannon — note it is different from Dread's), the Octoliths, the locations (Celestial Archives, Alinos, Arcterra, Vesper Defense Outpost, the Oubliette; the Tetra Galaxy), the rival hunters with their species, motives, weapons and alternate forms: Weavel (Space Pirate turned cyborg after Samus nearly killed him), Kanden (failed Enoema supersoldier), Spire (last of the Diamonts), Noxus (Vhozon judge), Trace (Kriken scout), Sylux in one line (prime4 owns), the resolution and Samus's feelings about it.",
    wishlist: ['The Alimbic Cluster incident (Metroid Prime Hunters events)', 'The Alimbics', 'Gorea', 'Alimbic Cluster locations (Celestial Archives, Alinos, Arcterra, Vesper Defense Outpost, Oubliette)', 'Octoliths and the Alimbic Omega Cannon', 'Weavel', 'Kanden', 'Spire', 'Noxus', 'Trace'],
    avoid: 'Sylux\'s full biography (prime4); the Space Pirates generally (space-pirates).' },
  { slug: 'echoes', cat: 'Prime 2', title: 'Aether, the Luminoth and the Ing', min: 8, max: 10,
    brief: "The Aether incident (Metroid Prime 2: Echoes): the GFS Tyr and Bravo Squad (Captain A.C. Exeter) lost pursuing a Pirate frigate; Aether; the Luminoth (travelers who settled Aether; their Elders; their long war with the Ing; U-Mos the Sentinel; the Luminoth hibernation); the Leviathan impact that split the planet into Light and Dark Aether (phazon owns Leviathans generally); Dark Aether's lethal atmosphere; the Ing (Emperor Ing, Warrior Ing, possession of creatures and corpses, Dark Pirates); the Energy Controllers and the Light of Aether; the regions (Temple Grounds/Great Temple, Agon Wastes, Torvus Bog, Sanctuary Fortress and their dark counterparts; the Sky Temple); Luminoth technology (one line pointing to suits/weapons/abilities entries); the guardians (Amorbis, Chykka, Quadraxis); the Pirates' Phazon mining on Aether; Dark Samus's thefts in one line (dark-samus owns); the resolution (Dark Aether's collapse, the Luminoth awakening, U-Mos's farewell) and Samus's bond with the Luminoth.",
    wishlist: ['The Aether incident (Metroid Prime 2: Echoes events)', 'Aether and Dark Aether', 'The Luminoth', 'U-Mos', 'The Ing and the Emperor Ing', 'Energy Controllers and the Light of Aether', 'Regions of Aether (Temple Grounds, Agon Wastes, Torvus Bog, Sanctuary Fortress)', 'GFS Tyr and Bravo Squad', 'Amorbis, Chykka and Quadraxis'],
    avoid: 'Dark Samus\'s biography (dark-samus); Phazon/Leviathans (phazon); Dark/Light suit and beam mechanics (suits/weapons).' },
  { slug: 'prime3', cat: 'Prime 3', title: 'The Phazon crisis (Metroid Prime 3: Corruption)', min: 8, max: 10,
    brief: "The Phazon crisis: the timing after Aether (verify), the Aurora Unit virus, the GFS Olympus briefing with Fleet Admiral Dane (federation owns Dane), the bounty hunters Rundas (Phrygisian ice manipulator), Ghor (cyborg with armorsuit; champion of the weak), Gandrayda (shapeshifter; mocking of Samus), the Pirate assault on Norion and Dark Samus's attack that corrupted the four hunters, the PED Suit in one line (suits owns), the Leviathan Seeds on Bryyo, Elysia and the Pirate Homeworld; Bryyo (the Reptilicus; the civil war between the Lords of Science and the Primals; Bryyo Fire/Ice/Cliffside; golems; Mogenar; Rundas's death), Elysia (SkyTown; the Elysians built by the Chozo; Aurora Unit 217; Helios; Ghor's death), the Pirate Homeworld (space-pirates owns the place; here Gandrayda's death and Omega Ridley in one line), the Leviathan Battleship, the assault on Phaaze (phazon owns Phaaze's nature), Aurora Unit 313 and Dark Samus's defeat, Samus purged of Phazon, the GFS Valhalla, the unidentified ship following Samus at the end (Sylux's — prime4 owns), and Samus's grief for the hunters.",
    wishlist: ['The Phazon crisis (Metroid Prime 3: Corruption events)', 'Rundas', 'Ghor', 'Gandrayda', 'Norion', 'Bryyo and the Reptilicus', 'Elysia and SkyTown', 'Elysians', 'The assault on Phaaze and the end of Phazon', 'GFS Olympus and the Federation fleet at Norion'],
    avoid: 'Phazon\'s nature, Leviathans, Hypermode, Phaaze description (phazon); Dark Samus biography (dark-samus); Aurora Units in general (federation); Admiral Dane (federation); Pirate Homeworld (space-pirates); Omega Ridley (ridley).' },
  { slug: 'dark-samus', cat: 'Dark Samus', title: 'Dark Samus', min: 5, max: 6,
    brief: "Dark Samus: born when the Metroid Prime creature absorbed Samus's Phazon Suit; appearance (a dark Phazon-based copy of Samus); powers (Phazon manipulation, flight, regeneration, invisibility, echoes/clones); nature and personality (hunger for Phazon, obsession with Samus, cunning, growing sentience); her actions on Aether (stealing Phazon from the Pirates, her fights with Samus, apparent death at Dark Aether's collapse); her return (reformed from Phazon; seizing the Space Pirates by corrupting them; the Norion attack; corrupting Rundas, Ghor, Gandrayda and Samus); her merging with Aurora Unit 313 on Phaaze and her final destruction with Phaaze; her relationship to Samus as a corrupted mirror; what Samus knows and feels about her.",
    wishlist: ['Dark Samus (overview)', 'Dark Samus – origin from Metroid Prime', 'Dark Samus – Aether and the return', 'Dark Samus – the Phazon crisis and her end', 'Dark Samus – powers and nature'],
    avoid: 'Phazon\'s general properties (phazon); region details (echoes/prime3).' },
  { slug: 'phazon', cat: 'Phazon', title: 'Phazon, Phaaze and the Leviathans', min: 6, max: 7,
    brief: "Phazon: blue, radioactive, mutagenic substance from the living planet Phaaze; its properties (energy source, mutation, corruption, a sentience or will of its own — verify how canon phrases it); the Leviathans (Phaaze's seeds launched to spread Phazon and seed new Phaazes; impacts on Tallon IV as the 'Great Poison', Aether, Bryyo, Elysia, the Pirate Homeworld; the Norion Leviathan intercepted; each Leviathan's guardian); Phazon corruption of organisms (Phazon Metroids, Phazon-enhanced Pirates, the corrupted hunters); Phazon in Samus (the Phazon Suit; her corruption in Prime 3; Hypermode via the PED; the risk of becoming like Dark Samus; her purge after Phaaze's destruction); Pirate Phazon operations; the Federation's Phazon weapons program; Phaaze itself (living planet, Phazon core, Leviathan infants, Phazon Metroids); the eradication of all Phazon when Phaaze was destroyed.",
    wishlist: ['Phazon', 'Phaaze', 'Leviathans (Leviathan Seeds)', 'Phazon corruption and Hypermode', 'Phazon Metroids and Phazon-corrupted creatures', 'Samus and Phazon (corruption and purge)'],
    avoid: 'Dark Samus (dark-samus); PED Suit mechanics (suits); the Prime 3 narrative (prime3).' },
  { slug: 'federation-force', cat: 'Federation Force', title: 'Metroid Prime: Federation Force', min: 4, max: 6,
    brief: "Metroid Prime: Federation Force (canon; set after Prime 3): the Bermuda System (Excelcion, Bion, Talvania), the Federation Force (elite marines in Mechs — verify what the Mechs are and who built them), General Alex Miles, the Space Pirates' plan in the Bermuda System (the Doomseye warship; the Master Brain or other leadership if verified; their growth-serum / giant-creature experiments if verified), Samus's role (her mission there; being captured and mind-controlled to fight the Force; her rescue), the final battle, the ending and the post-credits scene (Sylux's ship and the Metroid egg — verify exactly what is shown), and its relevance to Prime 4. Keep strictly to verified facts; Metroid Wiki and Wikipedia both have plot summaries.",
    wishlist: ['The Bermuda System incident (Federation Force events)', 'The Federation Force and their Mechs', 'General Alex Miles', 'Bermuda System (Excelcion, Bion, Talvania)', 'The Doomseye'],
    avoid: 'Sylux (prime4); the Federation generally (federation).' },
  { slug: 'prime4', cat: 'Prime 4', title: 'Metroid Prime 4: Beyond, Viewros and Sylux', min: 8, max: 10,
    brief: "Metroid Prime 4: Beyond (released December 4, 2025 — verify) is the most recent canon and the hardest to source: use WebSearch in 'extended' mode plus Metroid Wiki and Wikipedia, and cross-check every claim in at least two sources (Metroid Wiki, Wikipedia, Nintendo's site, major outlets' plot explainers such as Game8/GameSpot/IGN/TV Tropes). Cover: the Space Pirate attack led by Sylux on the Galactic Federation research facility on Tanamaar (Desolan System) and the Lamorn artifact; Metroids fused with creatures (the Aberax); the teleportation to Viewros; the Lamorn (extinct psychic civilization; the Master Teleporter at the Chrono Tower; the keys; the 'green energy' experiment and the Grievers); why and how Samus gains psychic abilities (mechanics belong to the abilities domain); the stranded Federation marines (Myles MacKenzie and any other named characters — verify names and roles) and their base camp; the Vi-O-La and Sol Valley; the named regions of Viewros; Sylux's full biography across the series (the Alimbic Cluster, the ship shadowing Samus after the Phazon crisis, the Federation Force post-credits scene, Beyond: his hatred of the Federation and of Samus, his suit, Shock Coil and Lockjaw, his ship Delano 7, his use of Metroids, decoys if accurate, his fate); the ending (Samus escapes through the Master Teleporter; the marines stay behind); any new creatures and the 'Legacy Suit' only if canon. Flag single-source details as lower confidence in notes rather than asserting them.",
    wishlist: ['The Viewros incident (Metroid Prime 4: Beyond events)', 'Sylux', 'Viewros', 'The Lamorn', 'Tanamaar and the Desolan System', 'Myles MacKenzie and the stranded Federation marines', 'The Chrono Tower and the Master Teleporter', 'Grievers and the creatures of Viewros', 'Vi-O-La and Sol Valley'],
    avoid: 'Psychic ability mechanics (abilities); the other hunters (hunters); Federation Force events (federation-force).' },
  { slug: 'timeline', cat: 'Universe', title: 'Chronology, canon and framing', min: 5, max: 6,
    brief: "The canon chronology and framing. (1) ONE constant primer entry (constant=true, 130-200 words) describing the Metroid universe for an LLM: a galaxy governed by the Galactic Federation, the Space Pirates, the Chozo, Metroids, the X, Phazon, and Samus Aran as a bounty hunter raised by the Chozo — plus one line on tone (hard sci-fi, isolation, horror undertones, a stoic heroine). (2) A full timeline entry listing the official order with 1-3 sentence in-universe summaries: the manga backstory, Zero Mission (Zebes), Prime, Prime Hunters, Prime 2, Prime 3, Federation Force, Prime 4, Samus Returns (SR388), Super Metroid, Other M, Fusion, Dread — noting which are retellings (Metroid→Zero Mission; Metroid II→Samus Returns; Prime→Remastered) and verifying the official order from Nintendo/Metroid Wiki. (3) In-universe dating: the original manual's cosmic-calendar phrasing ('year 20X5', 'year 2000 of the history of the cosmos' — verify), any dated events, the fact that exact years are mostly unstated, Samus's age never officially given, the stated gaps between games where canon gives them (e.g. Prime 3 relative to Echoes — verify). (4) Canon status of media: games canon; manga canonical backstory per Sakamoto; manuals/official sites semi-canon; Smash Bros., Captain N, the comics, Metroid Prime Pinball, First Hunt non-canon; Other M's Japanese vs English differences; Prime 4's position. (5) Recurring patterns: why Samus repeatedly loses her abilities, using only canonical explanations (suit damage, Chozo tests, data corruption, the Fusion vaccine, Raven Beak's attack), the mission-report structure, 'the Hunter', what 'Metroid' means.",
    wishlist: ['Metroid universe primer (constant)', 'Timeline of Samus\'s missions', 'In-universe dating and the Cosmic Calendar', 'What counts as canon', 'Recurring patterns (why Samus loses her abilities, mission structure)'],
    avoid: 'Deep detail on any single event (owned by the respective domains).' },
  { slug: 'bestiary-2d', cat: 'Creatures', title: 'Fauna of Zebes, SR388, the Bottle Ship, the BSL and ZDR', min: 6, max: 7,
    brief: "Non-boss creatures of Zebes, SR388, the Bottle Ship, the BSL station and ZDR that a roleplay might mention. Verify names from Metroid Wiki category and location pages (e.g. Category:Zebes creatures, the Brinstar/Norfair/Maridia pages, SR388 creature lists, ZDR area pages, Bottle Ship sector pages, BSL sector pages). Group creatures into entries by habitat, each listing 6-12 creatures with a one-line description (appearance, behavior, danger). Candidates to check: Zoomer, Geemer, Ripper, Skree, Waver, Reo, Sidehopper, Dessgeega, Hornoad, Zebbo, Zeb, Nova, Sova, Holtz, Kago, Mellow, Metaree, Yumbo, Gullugg, Moheek, Chute Leech, Blob Thrower, Wallfire, Arachnus, Gunzoo, the Bottle Ship's Kyratian, Rhedogian, Dragotix, Himella and others, the BSL's sector fauna, and ZDR's Muzby, Klaida, Blindflyer, Yampa, Slidle, Quetzoa, Armadigger, Daiban? (verify each). Keys must include each creature's name and plural.",
    wishlist: ['Fauna of Crateria and Brinstar', 'Fauna of Norfair', 'Fauna of Maridia and the Wrecked Ship', 'Fauna of SR388', 'Fauna of ZDR', 'Fauna of the Bottle Ship', 'Fauna of the BSL station'],
    avoid: 'Bosses (owned by the game domains); Metroids; X; Zebesians/Kihunters (space-pirates); Dachora and Etecoons (super-metroid).' },
  { slug: 'bestiary-prime', cat: 'Creatures', title: 'Fauna of the Prime-series worlds', min: 6, max: 8,
    brief: "Non-boss creatures of the Prime-series worlds that a roleplay might mention, grouped by world, each entry listing 6-12 creatures with one-line descriptions. Verify every name from Metroid Wiki location/category pages. Tallon IV (Beetle, Plated Beetle, Sheegoth and Baby Sheegoth, Blastcap, Tangle Weed, Reaper Vine, Puffer, Bloodflower, Flickerbat, Triclops, Jelzap, Hive Mecha, War Wasp, Glider, Shriekbat, Magmoor, Puddle Spore, Seedling, Stone Toad, Scatter Bombu, Aqua Sac and others), Aether (Splinter, Sandigger, Grenchler, Blogg, Krocuss, Ingclaw, Nightbarb, Lumite, Mechlops and others), the Alimbic Cluster (Psycho Bit, Greater Ithrak, Petrasyl, Quadtroid and others), Bryyo (Reptilicus Hunter, Hopper, Gragnol, Fargul Hatcher and others), Elysia (Steambot, Steamlord, Tinbot, Aeromine and others), the Pirate Homeworld and Phaaze (Phazon Hopper, Jelsac, Phazon Puffer and others), the Bermuda System's creatures (verify), and Viewros's creatures other than Grievers (verify, two sources). Keys must include each creature's name and plural.",
    wishlist: ['Fauna of Tallon IV', 'Fauna of Aether and Dark Aether', 'Fauna of the Alimbic Cluster', 'Fauna of Bryyo', 'Fauna of Elysia and the Pirate Homeworld', 'Fauna of Phaaze', 'Fauna of the Bermuda System', 'Fauna of Viewros'],
    avoid: 'Bosses (owned by the game domains); Space Pirate troop types (space-pirates); Metroids; Grievers (prime4); Chozo Ghosts (chozo).' },
  { slug: 'tech', cat: 'Technology', title: 'Ships, stations, bounty hunting and galactic indexes', min: 6, max: 7,
    brief: "Technology, vehicles and institutions not owned elsewhere. Samus's Gunships across eras (her first starship; the Prime-era Hunter-class gunship; the Prime 3 ship with ship weapons and the Command Visor link; the Other M/Fusion gunship with its onboard computer; the Dread gunship with ADAM; capabilities such as autopilot, standing on it to recharge, cloaking if canon; her quarters — verify each from Metroid Wiki's gunship pages). Save Stations, Recharge Stations, Map Stations, Navigation Rooms and Network Stations as in-universe infrastructure (who built them: Chozo, Federation, Pirates). Elevators, transport capsules and teleportals (Chozo, Luminoth, Federation). Bounty hunting as a profession (how hunters are contracted, Samus's reputation, other hunters). A 'Species of the galaxy' index entry (one line each: humans, Chozo, Luminoth, Reptilicus, Elysians, Alimbics, Kriken, Vhozon, Diamonts, Enoema, Phrygisians, Lamorn, Space Pirates, Zebesians, Ing, Bryyonians, etc., pointing to the detailed entries by name). A 'Planets and places' index entry (one line each: Zebes, SR388, Tallon IV, Aether, Norion, Bryyo, Elysia, Pirate Homeworld, Phaaze, K-2L, Daiban, ZDR, Tanamaar, Viewros, Excelcion, Bion, Talvania, Alinos, Arcterra, Wotan VII, Phrygis, Jigrad and others verified) with keys for the lesser-known places.",
    wishlist: ['Samus\'s Gunship', 'Save, Recharge, Map and Navigation stations', 'Elevators, transport capsules and teleportals', 'Bounty hunting in the Metroid galaxy', 'Species of the galaxy (index)', 'Planets and places of the galaxy (index)'],
    avoid: 'Suits/weapons/abilities (equipment domains); Aurora Units and Federation ships (federation); Mechs (federation-force).' },
  { slug: 'manga', cat: 'Manga', title: 'The Metroid manga', min: 5, max: 7,
    brief: "The official Metroid manga (serialized 2002-2004, art by Kenji Ishikawa, supervised by Yoshio Sakamoto; treated as canonical backstory — verify the attribution and Sakamoto's statements). Plot in detail: K-2L and the Pirate raid (Ridley; Rodney's and Virginia's deaths; what each did), Old Bird and Gray Voice taking Samus to Zebes, the Chozo blood/DNA infusion, her training and Power Suit, Mother Brain's creation and growing ambition, Samus joining the Federation Police with Kreatz and Mauk under Adam Malkovich, Chairman Keaton's appearance if any, the Space Pirates' assault on Zebes and Mother Brain's betrayal, Gray Voice's fate, Old Bird's fate, Samus's confrontation with Ridley, her breakdown and recovery, and the ending leading into her first Zebes mission. Manga-only characters get their own entries (Kreatz, Mauk; any others verified). Include the manga's known contradictions with the games (ages, recruitment, raid details) and label everything 'per the manga'. Also check whether the 'Metroid Prime: Episode of Aether' e-manga exists and its canon status, in one line.",
    wishlist: ['The Metroid manga (overview and canon status)', 'The K-2L raid (manga account)', 'Samus\'s Federation Police years (manga)', 'Kreatz and Mauk', 'Mother Brain\'s betrayal of Zebes (manga account)', 'Samus\'s confrontation with Ridley and her trauma (manga account)'],
    avoid: 'Old Bird and Gray Voice entries (chozo); Samus\'s core bio (samus-bio); Mother Brain\'s entry (zebes); Ridley\'s entry (ridley); Adam Malkovich (adam-other-m).' },
]

const ALLOCATION = DOMAINS.map(d => `- ${d.slug} [${d.cat}]: ${d.wishlist.join('; ')}`).join('\n')

const TOOLS = `RESEARCH TOOLS
- First call ToolSearch with query "select:WebFetch,WebSearch" to load the web tools.
- Primary source: Metroid Wiki, fetchable at https://www.metroidwiki.org/wiki/<Page_Title> (underscores for spaces). Fetch the page for every major subject you write about; use the offset parameter to keep reading long pages. Category pages like https://www.metroidwiki.org/wiki/Category:Characters also work.
- Secondary: Wikipedia (https://en.wikipedia.org/wiki/...) for game plot summaries and release facts; WebSearch (use mode "extended" for Metroid Prime 4: Beyond and other recent or niche topics, "standard" otherwise).
- Wikitroid (metroid.fandom.com) is BLOCKED from this environment (HTTP 402/403): do not spend calls on it; WebSearch result snippets may quote it.
- Other sites: try at most once; if a fetch fails, move on.
- Record every URL you actually used.`

const CANON = `CANON SCOPE
- Canon: the mainline games (Metroid / Zero Mission, Metroid II / Samus Returns, Super Metroid, Other M, Fusion, Dread), the Prime series (Prime and Prime Remastered, Hunters, Echoes, Corruption, Federation Force, Prime 4: Beyond), the official 2002-2004 Metroid manga as canonical backstory (label manga-only details "per the manga"), and official manuals/websites (label manual-only details "per the manual").
- NOT canon: Super Smash Bros., Captain N, the Nintendo Comics System comics, Metroid Prime Pinball, Metroid Prime Hunters: First Hunt, the Samus & Joey and Metroid EX manga, AM2R and other fan games, fan theories, and wiki speculation or "it is believed" passages.
- Where official sources conflict (Japanese vs English Other M script; the original game vs its retelling; manual vs game), prefer the most recent official version and note the conflict in one clause only if it matters for roleplay.
- Never invent names, numbers, dates or motivations. If you cannot verify a detail, leave it out.`

const STYLE = `CONTENT STYLE (the "content" field)
- Audience: an LLM running a SillyTavern roleplay in which {{user}} interacts with Samus Aran (she is the character card). Entries are injected into the prompt only when their keys appear in recent chat, so each entry must stand alone.
- In-universe encyclopedia voice, third person. Past tense for events, present tense for enduring facts. Dense and specific: names, places, causes, outcomes. No filler, no hedging words like "iconic" or "beloved".
- Length: 120-400 words per entry (up to 550 for the core Samus profile, the timeline entry and the single biggest subjects). Prefer several focused entries to one bloated one.
- Plain text only: short paragraphs; "- " bullet lines are fine for lists. No markdown headers, tables, bold, links or citations inside content (sources go in the separate "sources" field).
- No gameplay or meta language: never "the player", "the game", "gameplay", "level", "boss", "boss fight", "cutscene", "stage", "unlock", "upgrade item", "DLC", "amiibo", "100%", "difficulty", "sequence". Describe mechanics as in-universe capabilities ("the Screw Attack lets Samus...").
- Game titles may appear only as chronology tags in parentheses, at most twice per entry, e.g. "(during the BSL incident; Metroid Fusion)". The timeline entry is the exception.
- Because the roleplay centers on Samus, each entry should, where relevant, include 1-3 sentences on Samus's history with, knowledge of, or feelings about the subject, strictly canon-supported.
- Mark provenance of non-game material inline ("per the manga", "per the original manual").
- Spell proper nouns exactly as Metroid Wiki does (Mother Brain, Raven Beak, Quiet Robe, E.M.M.I., SA-X, Zero Suit, Thoha, Mawkin, K-2L, SR388, ZDR, Tallon IV, Phaaze, Phazon, Luminoth, U-Mos).
- Cross-reference related subjects by their proper names so a recursive scan can pull in their entries.`

const KEYS = `KEY RULES (the "keys" and "secondary_keys" fields)
- 3-12 keys per entry: the canonical name, aliases/nicknames/epithets, alternate spellings and hyphenations, and plural forms. SillyTavern's default whole-word matching means "Metroid" does NOT match "Metroids", so list plurals explicitly. Matching is case-insensitive; keys must not be regexes and must not contain commas.
- Avoid single generic English words that will misfire in ordinary chat. Forbidden as bare keys: Prime, Beyond, Echoes, Hunters, Force, Mother, Dark, Light, Queen, Baby, Suit, Beam, Ball, Missile, Station, Ship, Dread, Fusion, Corruption, Return, Returns, Zero, Hunter, Pirate, Pirates, Lady, Princess, Parasite, Energy, Power, Ice, Wave, Charge, Scan, Space, Jump, Counter, Shift, Memory, Memories, Archive, Soldier, Robot, Egg, Nest, Lab, Research, Colony, Seed, Worm, Ghost, Ghosts, Brain, Bird, Voice, Trace, Spire, Deleter, Nightmare (alone), Adam (alone), Anthony (alone), Ian (alone). Prefer multi-word keys: "Mother Brain", "Metroid Prime", "Dark Samus", "Zero Suit", "Space Pirates", "baby Metroid".
- "Samus", "Samus Aran", "Aran", "Hunter" and "the Hunter" may appear as keys ONLY on the core Samus profile entry; "Metroid"/"Metroids" only on the main Metroids overview; "Chozo" only on the main Chozo overview; "Federation" and "Galactic Federation" only on the main Galactic Federation overview; "Space Pirate"/"Space Pirates" only on the Space Pirates overview; "Ridley" only on the main Ridley overview. Every other entry about those subjects must use topical keys (e.g. "childhood", "K-2L", "parents", "Meta Ridley", "Thoha", "Chozo Memories", "Elite Pirate").
- When the natural key is ambiguous, make the entry selective: put the ambiguous word(s) in keys and 3-8 disambiguating words in secondary_keys with selective_logic "AND_ANY" (fires only when a primary key AND at least one secondary key are present). Example: keys ["Adam"], secondary_keys ["Malkovich","Commander","CO","Lady","Federation","AI","Bottle Ship","Sector Zero","objections"]. One logic applies per entry, so if you need both a bare ambiguous key and specific multi-word keys, put the multi-word keys in the primary list too (they will still require a secondary hit) or simply drop the bare word.
- constant=true ONLY on the universe primer (timeline domain) and the Samus portrayal guidance entry (samus-rp domain). Everything else constant=false.
- order: 100 default; 200 for the two constant entries; 150 for the core Samus profile; 140 for the timeline entry; 110 for major entities (Ridley, Chozo, Metroids, Mother Brain, Galactic Federation, X Parasites, Space Pirates, Dark Samus, Sylux, Raven Beak, Adam Malkovich); 90 for bestiary and index entries.
- comment = the entry title, unique across the whole lorebook, prefixed with the domain's category tag and a colon (e.g. "Dread: Raven Beak").`

const ENTRY = { type: 'object', properties: {
  comment: { type: 'string' },
  keys: { type: 'array', items: { type: 'string' } },
  secondary_keys: { type: 'array', items: { type: 'string' } },
  selective_logic: { type: 'string', enum: ['AND_ANY', 'AND_ALL', 'NOT_ANY', 'NOT_ALL'] },
  content: { type: 'string' },
  constant: { type: 'boolean' },
  order: { type: 'integer' },
  category: { type: 'string' },
  sources: { type: 'array', items: { type: 'string' } },
  notes: { type: 'string' },
}, required: ['comment', 'keys', 'content'] }

const DRAFT_SCHEMA = { type: 'object', properties: {
  entries: { type: 'array', items: ENTRY },
  research_notes: { type: 'string' },
  sources_consulted: { type: 'array', items: { type: 'string' } },
}, required: ['entries'] }

const VERIFY_SCHEMA = { type: 'object', properties: {
  reports: { type: 'array', items: { type: 'object', properties: {
    entry_comment: { type: 'string' },
    verdict: { type: 'string', enum: ['accurate', 'needs_fixes', 'rewrite'] },
    issues: { type: 'array', items: { type: 'object', properties: {
      severity: { type: 'string', enum: ['error', 'noncanon', 'unverified', 'missing', 'keys', 'style', 'nit'] },
      claim: { type: 'string' },
      problem: { type: 'string' },
      correction: { type: 'string' },
      source_url: { type: 'string' },
    }, required: ['severity', 'problem'] } },
  }, required: ['entry_comment', 'verdict', 'issues'] } },
  overall: { type: 'string' },
  sources_consulted: { type: 'array', items: { type: 'string' } },
}, required: ['reports'] }

const FINAL_SCHEMA = { type: 'object', properties: {
  file_path: { type: 'string' },
  entries: { type: 'array', items: ENTRY },
  changelog: { type: 'string' },
  unresolved: { type: 'string' },
}, required: ['file_path', 'entries'] }

function writerPrompt(d) {
  return `You are drafting entries for a SillyTavern "World Info" lorebook covering the canon lore of Nintendo's Metroid series. The lorebook will be used in roleplay where {{user}} interacts with Samus Aran (Samus is the character card). Your domain: "${d.title}" (category tag "${d.cat}").

DOMAIN SCOPE
${d.brief}

ENTRIES TO PRODUCE (a guide, not a straitjacket: merge or split as the evidence supports; produce between ${d.min} and ${d.max} entries):
${d.wishlist.map(w => '- ' + w).join('\n')}

DO NOT COVER (owned by other domains; mention in passing at most, never a dedicated entry):
${d.avoid}

FULL TOPIC ALLOCATION ACROSS ALL DOMAINS (cross-reference these subjects by name; do not duplicate them):
${ALLOCATION}

${TOOLS}

${CANON}

${STYLE}

${KEYS}

PROCESS
1. Load the web tools. Fetch the Metroid Wiki page for each main subject (plus Wikipedia or web search where the wiki is thin). Read enough to write with specifics. Aim for 10-25 fetches/searches; the entries must be built from what you read, not from memory. When memory and sources disagree, the sources win.
2. Draft the entries.
3. Self-check against the STYLE and KEY rules: remove meta language; add plurals/aliases to keys; drop forbidden bare keys; make titles unique and prefixed "${d.cat}: ".
4. Return via the StructuredOutput tool: entries (each with comment, keys, secondary_keys, selective_logic, content, constant, order, category "${d.cat}", sources, notes), research_notes (what you could not verify or found contradictory), sources_consulted.
Your final output is data for an automated pipeline, not a message to a person.`
}

function verifyPrompt(d, draft) {
  return `You are an adversarial Metroid canon fact-checker. Another agent drafted SillyTavern lorebook entries for the domain "${d.title}". Find everything wrong with them before they ship. Assume there ARE errors and hunt for them.

DRAFT ENTRIES (JSON):
${JSON.stringify(draft.entries, null, 1)}

Drafter's own notes: ${draft.research_notes || '(none)'}

${TOOLS}

${CANON}

WHAT TO CHECK, FOR EVERY ENTRY (all of them, not just the first few):
1. Factual claims: names, relationships, who did what, causes and outcomes, places, chronology, physical attributes, numbers, quoted lines. Fetch the Metroid Wiki page for the subject and for named sub-topics; cross-check Wikipedia or web search when the wiki is thin. Severity "error" only when a source contradicts the claim: give the corrected statement and the source URL.
2. Non-canon contamination (severity "noncanon"): Smash Bros., Captain N, comics, fan games, fan theories, wiki speculation presented as fact, Pinball, First Hunt.
3. Unverifiable claims (severity "unverified"): you made a real attempt (at least one fetch or search) and found no support. Recommend softening or removal.
4. Missing essentials (severity "missing", max 5 per entry): important canon facts about the subject that a Samus-centered roleplay would want and the entry omits, with the source URL.
5. Keys (severity "keys"): bare generic keys likely to misfire in ordinary chat (see the forbidden list: Prime, Beyond, Echoes, Hunters, Force, Mother, Dark, Light, Queen, Baby, Suit, Beam, Ball, Missile, Station, Ship, Dread, Fusion, Corruption, Return, Zero, Hunter, Pirate, Lady, Princess, Parasite, Energy, Power, Ice, Wave, Charge, Scan, Space, Jump, Counter, Shift, Memory, Archive, Soldier, Robot, Egg, Nest, Lab, Colony, Seed, Worm, Ghost, Brain, Bird, Voice, Trace, Spire, Deleter, bare Nightmare/Adam/Anthony/Ian); "Samus"/"Samus Aran"/"Aran"/"Hunter" on any entry other than a core Samus profile; "Metroid(s)", "Chozo", "Federation", "Space Pirate(s)", "Ridley" on anything other than that subject's main overview; missing obvious aliases, plural forms, alternate spellings; wrong use of secondary keys.
6. Style (severity "style"): gameplay/meta language ("the player", "boss", "level", "cutscene", "unlock", "upgrade"), markdown headers/tables/bold, citations inside content, game titles used more than twice in one entry, entries far outside 120-400 words (core subjects up to 550), content that is not self-contained.
7. Proper-noun spelling versus Metroid Wiki (severity "nit").

Do at least 12 fetches/searches in total; prioritize the claims most likely to be wrong (specific numbers, chronology, motivations, "first/only/last" claims, who killed whom, anything about Metroid Prime 4: Beyond). Be honest about severity: do not inflate nits into errors, and do not wave through claims you did not check. For each entry give a verdict: "accurate" (no error/noncanon/unverified issues), "needs_fixes", or "rewrite" (fundamentally wrong or off-canon).
Return via StructuredOutput: reports (one per entry, same order, keyed by entry_comment), overall notes, sources_consulted. Data only, not a message to a person.`
}

function revisePrompt(d, draft, verdict) {
  const file = `${SCRATCH}/entries/${d.slug}.json`
  const report = verdict ? JSON.stringify(verdict, null, 1) : '(the fact-checker failed to return a report; do your own verification pass of the most consequential claims in every entry, at least 10 fetches/searches, before finalizing)'
  return `You are finalizing SillyTavern lorebook entries for the Metroid domain "${d.title}" (category tag "${d.cat}"). You have the draft entries and an adversarial fact-check report. Produce the final, corrected entries and write them to disk.

DRAFT ENTRIES (JSON):
${JSON.stringify(draft.entries, null, 1)}

FACT-CHECK REPORT (JSON):
${report}

${TOOLS}

${CANON}

${STYLE}

${KEYS}

RULES FOR APPLYING THE REPORT
- Fix every "error" and "noncanon" item using the correction and source given; if a correction itself looks doubtful, fetch the cited page and decide from the source.
- For each "unverified" item do one quick check yourself (one fetch or search). Keep it only if you confirm it; otherwise soften ("per the manga", "reportedly") or remove it.
- Add "missing" essentials where they fit the length budget; prefer adding facts over adding words.
- Apply "keys" and "style" fixes, then re-check every entry against the STYLE and KEY rules yourself; the fact-checker may have missed some. Strip forbidden bare keys. Add plurals.
- Keep titles unique and prefixed "${d.cat}: ". Keep the count between ${d.min} and ${d.max}; you may merge or split entries.
- Do not introduce new unverified facts while editing.

OUTPUT
1. Write the final entries to ${file} as JSON of the form {"domain": "${d.slug}", "category": "${d.cat}", "entries": [ ...entry objects with comment, keys, secondary_keys, selective_logic, content, constant, order, category, sources, notes... ]}. Use the Write tool. Then validate it with Bash: python3 -c "import json; d=json.load(open('${file}')); print(len(d['entries']), 'entries ok')".
2. Return via StructuredOutput: file_path, entries (the same final entries), changelog (what you changed and why, briefly), unresolved (anything you could not settle). Data only, not a message to a person.`
}

const wanted = (args && Array.isArray(args.slugs) && args.slugs.length) ? args.slugs : DOMAINS.map(d => d.slug)
const shard = DOMAINS.filter(d => wanted.includes(d.slug))
const missing = wanted.filter(s => !DOMAINS.some(d => d.slug === s))
if (missing.length) log(`WARNING: unknown slugs ignored: ${missing.join(', ')}`)
log(`Shard ${args && args.shard ? args.shard : '?'}: ${shard.map(d => d.slug).join(', ')} (${shard.length} domains x 3 stages)`)

const results = await pipeline(shard,
  d => agent(writerPrompt(d), { label: `draft:${d.slug}`, phase: 'Draft', schema: DRAFT_SCHEMA }),
  (draft, d) => {
    if (!draft || !draft.entries || !draft.entries.length) { log(`${d.slug}: draft failed`); return null }
    log(`${d.slug}: drafted ${draft.entries.length} entries`)
    return agent(verifyPrompt(d, draft), { label: `verify:${d.slug}`, phase: 'Verify', schema: VERIFY_SCHEMA })
      .then(v => ({ draft, verdict: v }))
  },
  (x, d) => {
    if (!x) return null
    if (!x.verdict) log(`${d.slug}: verifier failed; reviser will self-verify`)
    else log(`${d.slug}: verified (${x.verdict.reports.filter(r => r.verdict !== 'accurate').length}/${x.verdict.reports.length} entries need fixes)`)
    return agent(revisePrompt(d, x.draft, x.verdict), { label: `revise:${d.slug}`, phase: 'Revise', schema: FINAL_SCHEMA })
  },
)

const summary = results.map((r, i) => ({
  slug: shard[i].slug,
  ok: !!(r && r.entries && r.entries.length),
  file: r ? r.file_path : null,
  entries: r && r.entries ? r.entries.map(e => ({ comment: e.comment, keys: e.keys, secondary_keys: e.secondary_keys || [], constant: !!e.constant, words: String(e.content || '').split(/\s+/).length })) : null,
  changelog: r ? r.changelog : null,
  unresolved: r ? r.unresolved : null,
}))
const failed = summary.filter(s => !s.ok).map(s => s.slug)
if (failed.length) log(`FAILED domains: ${failed.join(', ')}`)
log(`Done: ${summary.filter(s => s.ok).length}/${shard.length} domains, ${summary.reduce((n, s) => n + (s.entries ? s.entries.length : 0), 0)} entries`)
return summary