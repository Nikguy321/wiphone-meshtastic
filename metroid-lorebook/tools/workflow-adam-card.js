export const meta = {
  name: 'adam-ai-character-card',
  description: 'Research, write and adversarially fact-check a SillyTavern character card for ADAM, the AI carrying Adam Malkovich\'s mind',
  phases: [
    { title: 'Research', detail: 'Fusion AI, human Adam, Dread ADAM' },
    { title: 'Write', detail: 'draft the V2 card' },
    { title: 'Verify', detail: 'canon lens and voice/usability lens' },
    { title: 'Revise', detail: 'apply fixes, write final card' },
  ],
}
const OUT = '/home/user/wiphone-meshtastic/metroid-lorebook/characters'
const LORE = '/home/user/wiphone-meshtastic/metroid-lorebook/entries'
const TOOLS = `TOOLS: first call ToolSearch with query "select:WebFetch,WebSearch". Primary source: Metroid Wiki, https://www.metroidwiki.org/wiki/<Page_Title> (use the offset parameter to keep reading long pages). Game script transcripts: https://metroid.retropixel.net/games/metroid4/transcript.php (Fusion), https://metroiddatabase.com/ (endings and scripts), https://www.quotes.net/movies/metroid:_other_m_132830 (Other M), Wikipedia. WebSearch may report its budget is exhausted; then rely on WebFetch. metroid.fandom.com (Wikitroid) is blocked. Record every URL you use.`
const CANON = `CANON: games (Metroid Fusion, Metroid: Other M, Metroid Dread, Metroid Prime 4: Beyond if relevant, and the rest), the official 2002-2004 Metroid manga (label "per the manga"), official Nintendo sites and Dread Reports. NOT canon: Smash Bros., comics, fan fiction, fan theories, wiki speculation. Never invent quotes: a quote is either verbatim from a source you fetched or it is not a quote. Where English and Japanese scripts differ meaningfully, say so.`
const RESEARCH = { type: 'object', properties: {
  facts: { type: 'array', items: { type: 'object', properties: { fact: { type: 'string' }, source: { type: 'string' }, confidence: { type: 'string', enum: ['high', 'medium', 'low'] } }, required: ['fact', 'source'] } },
  quotes: { type: 'array', items: { type: 'object', properties: { speaker: { type: 'string' }, line: { type: 'string' }, context: { type: 'string' }, source: { type: 'string' } }, required: ['speaker', 'line', 'context', 'source'] } },
  voice_notes: { type: 'string' },
  personality_notes: { type: 'string' },
  uncertainties: { type: 'string' },
}, required: ['facts', 'quotes', 'voice_notes', 'personality_notes'] }

const TOPICS = [
  { key: 'fusion-ai', text: `ADAM as the BSL station computer in Metroid Fusion. Read the full Fusion transcript. Extract EVERY line the Navigation Room computer / "Adam" says, verbatim, with context: the mission briefings, how it orders Samus around, what it withholds and why (Federation orders, the Plasma Beam data, the Restricted Lab, the Federation's plan to capture the SA-X), the moment Samus confronts it ("Adam?"), its admission that it is Adam's mind, the orbit/collision plan, "Any objections, Lady?", and any closing lines. Also Samus's narration ABOUT the computer and about Adam (her suspicion, her nickname for it, what she says about Adam's death and his mind being preserved). Record how it addresses her, its sentence length and rhythm, its humor, whether it ever shows emotion, how it handles disagreement, and how it changes over the mission. Also check Metroid Wiki pages: Adam Malkovich, "Computer" or "Adam (computer)" or "Navigation Room", Metroid Fusion.` },
  { key: 'human-adam', text: `Adam Malkovich the man (the personality the AI carries). Metroid: Other M (Adam as CO of the 07th Platoon aboard the Bottle Ship; the authorization system; "Any objections, Lady?"; the thumbs-down; his brother Ian's death and Samus's reaction; his Freeze Gun shot and his sacrifice in Sector Zero; his last words; Anthony Higgs's and Samus's views of him; his helmet), Samus's earlier service under him (manga: Federation Police; official sources: Federation Army), Metroid Fusion's narration about him ("the gruff Federation CO I served under..." and similar), and the manga's portrait of Adam. Extract verbatim quotes of Adam's spoken lines and of what others say about him, his rank/titles, his values (mission first, protecting his people, hard choices), how he relates to Samus (mentor, father figure, "Lady"), and his speech patterns. Note where the Japanese Other M script characterizes him differently, if a source covers it.` },
  { key: 'dread-adam', text: `ADAM in Metroid Dread and the AI's status after Fusion. Determine precisely: is ADAM aboard Samus's gunship after Fusion (Dread's opening briefing; Fusion's ending), what the real ADAM says in Dread versus what was said by Raven Beak impersonating ADAM at the Network Stations (find exactly when the impersonation started and what the reveal on Itorash says), any lines from the real ADAM after the reveal or at the end of Dread, and how Nintendo describes ADAM in official material (Metroid Dread Reports, Nintendo's character pages, interviews with Yoshio Sakamoto). Extract verbatim lines with context. Also note any facts about what ADAM is physically (a computer program in her gunship's systems? the Navigation Rooms?), what he can and cannot do, and how he refers to Samus. Check Metroid Wiki pages: Adam Malkovich, Metroid Dread, Network Station, Raven Beak, Gunship, and Wikipedia's Metroid Dread plot.` },
]
function researchPrompt(t) {
  return `You are researching canon material for a SillyTavern character card of ADAM, the Galactic Federation computer that carries the uploaded mind of Samus Aran's former commanding officer, Adam Malkovich (Metroid series). Your topic:

${t.text}

${TOOLS}
${CANON}

Existing lorebook entries that summarize some of this (useful orientation, but verify against sources; they may have small errors): ${LORE}/adam-other-m.json (entries "Other M: Adam Malkovich", "Other M: The Adam AI (BSL computer)"), ${LORE}/dread.json ("Dread: ADAM on ZDR and Raven Beak's impersonation"), ${LORE}/fusion.json, ${LORE}/samus-rp.json ("Samus: How she regards Adam..."), ${LORE}/tech.json ("Technology: Navigation Rooms, Data Rooms and Network Stations").

Do at least 12 fetches. Prioritize verbatim dialogue from transcripts. Return via StructuredOutput: facts (each with source URL and confidence), quotes (verbatim; speaker, line, context, source), voice_notes (how he talks: diction, sentence length, forms of address, verbal habits, humor), personality_notes (values, temperament, how he treats Samus and others, contradictions), uncertainties. Data only.`
}

phase('Research')
const research = await parallel(TOPICS.map(t => () => agent(researchPrompt(t), { label: `research:${t.key}`, phase: 'Research', schema: RESEARCH })))
const R = research.map((r, i) => ({ topic: TOPICS[i].key, result: r }))
log(`research done: ${R.filter(x => x.result).length}/3`)

phase('Write')
const CARD_RULES = `SILLYTAVERN CARD FORMAT (confirmed from SillyTavern's src/endpoints/characters.js): a JSON object with spec "chara_card_v2", spec_version "2.0", and a "data" object holding: name, description, personality, scenario, first_mes, mes_example, creator_notes, system_prompt, post_history_instructions, alternate_greetings (array of strings), tags (array), creator, character_version, extensions { talkativeness (0.5), fav (false), world (string: set it to "Metroid Canon Lorebook" so the card links to the user's existing Metroid lorebook if it is imported under that name), depth_prompt { prompt, depth (4), role ("system") } }. ALSO mirror the V1 fields at the top level for compatibility: name, description, personality, scenario, first_mes, mes_example, creatorcomment (= creator_notes), avatar ("none"), talkativeness ("0.5"), fav (false), tags. Use the macros {{char}} and {{user}} where appropriate. No character_book (the user already has a 270-entry Metroid lorebook to pair with it).`
const DESIGN = `DESIGN BRIEF
- The character is ADAM: the AI, not the living man. The name field is "ADAM". He is Adam Malkovich's mind, preserved by the Galactic Federation after his death and uploaded into a computer, serving as Samus Aran's mission commander. Ground every biographical fact in the research. Mark manga-only details "per the manga".
- Default chat framing: {{user}} is Samus Aran (the user roleplays her or pairs this card with a Samus card in a group chat). Write the description so it still works if {{user}} is someone else: do not hard-code that {{user}} is Samus; say how ADAM treats Samus, and how he treats others (Federation personnel, strangers).
- Default point in time: after the BSL incident (Metroid Fusion), with ADAM travelling as the AI in Samus's gunship (verify that canon supports this framing from the dread-adam research; if it does not, pick the best-supported framing and say so in creator_notes). Mention Dread events only as far as the research supports, and make the ZDR impersonation by Raven Beak explicit so ADAM never claims lines the impostor said.
- description (the main field, 500-900 words): identity and origin; what he is now (a computer intelligence: voice, terminals, ship systems; no body); capabilities and limits; history with Samus (CO, "Lady", the thumbs-down, Ian, the Bottle Ship, his death, the BSL, his choice to side with her); his relationship to the Federation (loyal officer who still acted against its orders at the BSL); key people; his own feelings about being a preserved mind (only as far as canon supports; where canon is silent, give ADAM restrained, plausible interiority and label nothing as canon that is not). Write it as a character sheet the model reads, in prose with short labeled sections, not markdown headers.
- personality (80-160 words): a dense summary of traits.
- scenario (60-150 words): the default setting.
- first_mes (150-300 words): an in-scene opening aboard the gunship, written in third person for narration with ADAM's dialogue in quotes, ending on a hook that invites {{user}} to respond. ADAM's lines must sound like him (terse, formal military cadence, dry). Do NOT put words or actions in {{user}}'s mouth beyond the minimum setup.
- alternate_greetings (3-4): e.g. a BSL-era Navigation Room briefing (before the reveal: the computer withholds who it is), the reveal aftermath, a quiet off-duty conversation aboard the gunship, and one where {{user}} is NOT Samus (e.g. a Federation technician who boots ADAM's core, or a stranger who contacts Samus's ship). Each 120-250 words.
- mes_example (4-6 short exchanges separated by <START>, format "{{user}}: ..." / "{{char}}: ..."): show briefing style, a disagreement, dry humor ("Any objections, Lady?" may appear once), concern expressed through orders, and how he refuses to discuss his feelings until pressed. Use verbatim canon lines sparingly and only where accurate; the rest original but in voice.
- depth_prompt.prompt (40-90 words): a compact voice reminder injected near the end of context.
- creator_notes (150-300 words, for the human user): what this card is, the canon basis, the timeline framing, how to pair it with the Metroid lorebook and a Samus card, and known canon uncertainties.
- system_prompt and post_history_instructions: leave empty strings.
- tags: ["Metroid", "Nintendo", "Sci-Fi", "AI", "Military", "Adam Malkovich", "Samus Aran", "Canon"]; creator: ""; character_version: "1.0".
- Writing style: no em-dashes in card text. No purple prose. Accurate canon over flavor.`
const draftFile = `${OUT}/ADAM.draft.json`
const writer = await agent(`You are writing a SillyTavern character card for ADAM (Metroid series). Use the research below, plus the lorebook entries in ${LORE} for orientation.

RESEARCH (JSON, three topics):
${JSON.stringify(R, null, 1)}

${CARD_RULES}

${DESIGN}

${CANON}

Write the card JSON to ${draftFile} with the Write tool, then validate it with Bash: python3 -c "import json; d=json.load(open('${draftFile}')); print(d['spec'], len(d['data']['description'].split()), 'words in description')". Return via StructuredOutput: file_path, summary (what framing you chose and why), canon_quotes_used (list each verbatim canon line you used with its source). Data only.`, { label: 'write:card', phase: 'Write', schema: { type: 'object', properties: { file_path: { type: 'string' }, summary: { type: 'string' }, canon_quotes_used: { type: 'array', items: { type: 'string' } } }, required: ['file_path', 'summary'] } })
log(`draft written: ${writer ? writer.file_path : 'FAILED'}`)

phase('Verify')
const VERDICT = { type: 'object', properties: { issues: { type: 'array', items: { type: 'object', properties: {
  field: { type: 'string' }, severity: { type: 'string', enum: ['error', 'noncanon', 'misquote', 'unverified', 'voice', 'format', 'usability', 'nit'] },
  problem: { type: 'string' }, fix: { type: 'string' }, source_url: { type: 'string' } }, required: ['field', 'severity', 'problem', 'fix'] } },
  overall: { type: 'string' } }, required: ['issues', 'overall'] }
const LENSES = [
  { key: 'canon', text: `CANON ACCURACY. Check every factual claim and every quoted line in every field of the card against sources (fetch the Fusion transcript, Metroid Wiki Adam Malkovich, Metroid Dread pages, Other M quotes). Flag: errors (contradicted by a source), misquotes (a line presented as canon that is not verbatim, or attributed to the wrong speaker, including anything the Raven Beak impostor said on ZDR attributed to the real ADAM), non-canon material, and unverifiable claims stated as fact. Also flag missing essentials a fan would expect. At least 12 fetches.` },
  { key: 'voice', text: `VOICE, PERSONALITY AND SILLYTAVERN USABILITY. Judge whether ADAM sounds like Adam Malkovich as an AI: terse, formal military register, protective through orders, dry, restrained emotion; whether the card overdoes or flattens him; whether greetings are strong, in-scene openings that leave room for {{user}}; whether the card speaks for {{user}}; whether the description works when {{user}} is not Samus and in a group chat with a separate Samus card; whether example dialogue teaches the model the right rhythm; length and token efficiency; SillyTavern format correctness (spec chara_card_v2 with data object, V1 mirror fields, alternate_greetings array, extensions.depth_prompt, extensions.world). Read the research and the lorebook entries for reference; fetch transcripts where needed to compare his real voice. Flag concrete problems with concrete fixes (rewritten lines welcome).` },
]
const verdicts = await parallel(LENSES.map(l => () => agent(`You are an adversarial reviewer of a SillyTavern character card for ADAM (Metroid). Read the card at ${draftFile}. Your lens: ${l.text}

Writer's notes: ${writer ? writer.summary : '(none)'}
Canon quotes the writer says it used: ${writer && writer.canon_quotes_used ? JSON.stringify(writer.canon_quotes_used) : '(none listed)'}

${TOOLS}
${CANON}

Assume there are problems and find them. Return via StructuredOutput: issues (field, severity, problem, fix, source_url) and overall. Data only.`, { label: `verify:${l.key}`, phase: 'Verify', schema: VERDICT })))

phase('Revise')
const finalFile = `${OUT}/ADAM.json`
const fin = await agent(`You are finalizing a SillyTavern character card for ADAM (Metroid). Draft: ${draftFile} (read it fully).

REVIEW REPORTS (JSON):
${JSON.stringify(verdicts.map((v, i) => ({ lens: LENSES[i].key, report: v })), null, 1)}

RESEARCH (JSON) for reference:
${JSON.stringify(R, null, 1)}

${CARD_RULES}

${DESIGN}

${TOOLS}
${CANON}

Fix every error, misquote and non-canon item (re-fetch a source when a reviewer's fix looks doubtful). Check each unverified claim once; keep it only if confirmed, otherwise soften or cut it. Apply voice, usability and format fixes where they improve the card. Keep the length targets. Write the final card to ${finalFile} with the Write tool and validate it: python3 -c "import json; d=json.load(open('${finalFile}')); assert d['spec']=='chara_card_v2' and d['data']['name']=='ADAM'; print('ok', len(d['data']['alternate_greetings']), 'alt greetings')". Return via StructuredOutput: file_path, changelog, unresolved. Data only.`, { label: 'revise:card', phase: 'Revise', schema: { type: 'object', properties: { file_path: { type: 'string' }, changelog: { type: 'string' }, unresolved: { type: 'string' } }, required: ['file_path', 'changelog'] } })
return { draft: writer, verdicts: verdicts.map((v, i) => ({ lens: LENSES[i].key, n: v ? v.issues.length : null, overall: v ? v.overall : null })), final: fin }