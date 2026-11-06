## Current goal: swap/replace Garrett's animations with existing in-game animations
- Find what animation system the game uses (UE3 AnimSets, AnimTree/AnimNodes, slot nodes, PlayAnim-style calls) and how Garrett's body mesh (Mesh2) is driven in third person
- Build an inventory of existing animation sequences (names, AnimSet, which gameplay state uses them)
- Add a mechanism to override a gameplay state's animation with another existing one (config-driven via thief_camera.ini)

## Animation rules
- Only swap between animations that share Garrett's skeleton (same bone track names); verify before swapping
- Prefer hooking/redirecting the animation selection call over patching asset data
- Log-first: add a debug logging mode that prints sequence names as the game plays them, before any override
- Record every class layout, offset and RVA in thief_structs.h AND NOTES.md (new "Animation" section), noting how each was found
- Keep first-person arm animations intact; do not break lockpicking, takedowns or context interactions
- Treat all function names/offsets guessed from UE3 knowledge as hypotheses until confirmed in the binary (Eidos forked the engine)
