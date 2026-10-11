# Debate log
## Round 1: independent EvidenceSkeptic vs axes
| Claim under attack | Attack | Evidence/defense | Verdict / claim change |
| --- | --- | --- | --- |
| NGX silence proves frontend not reached | Logging explicitly gated; current Player diag0 | owned_trace.hpp19-54 and launcher569; candidate diag1 | Rejected; C3 unresolved failure stage retained |
| Backend hr0 proves fresh output | hr0 returns after preparing and recording | command_list1637-1652 vs temporal225-243 and queue6434-6481 | Rejected; C4 now supported as evidence boundary |
| All rendering/GPU is deadlocked | Changing in-game HUD is incompatible with whole same-swapchain stall | FIFO queue/present chain; independent scene queue remains possible | Reject global claim; localized queue issue unresolved |
| FeatureFailedToLoad means SR failure | Historical successful candidate has same messages | both Player logs16-19,51-52 | Rejected as discriminating cause |
| SceneInternalLut/moon async release is new cause | Historical candidate has same warning and later evaluations | latest Player75,296-297,325-327; candidate75,236-237,5614-5619 | Rejected as string-only diagnosis |
| Shader rejection is cause of frozen scene | Latest lacks pair identities/callers; old64-pair scope is narrow |9131 measured latest lines; historical warmup result has alternate caller | Confirm actual compile failures, leave causal link unresolved |
| FEX consumes14-18%, so loses14-18% FPS | CPU-running denominator, network threads and generated game code differ | FexComparison re-executed analyzer; skeptic ratios independently re-evaluated | Reject FPS inference; retain historical cost candidates |
| Existing candidate was permanently frozen | Two retained screenshots visibly differ in scene poses | skeptic inspected candidate-03 city-dlss01/02 | Refuted for both historical capture instants; not latest or sustained proof |
| Coarse barrier handling is an improvement candidate | Blind barrier removal breaks untracked-resource visibility | current explicit over-sync and retained fences | Candidate only; prove safe transition classes and regression first |
| Temporal native errors silently preserve success forever | Synchronous reject propagates; later encode failure reports removal | NGX mapBackendResult; temporal239-243 | Reject simple silent-error story; GPU void encode/output freshness gap remains |

## New evidence clarification
NGX follow-up: Streamline disableCLStateTracking1 also exists in historical candidate. Native split clears encoder_current but next PreDraw/PreDispatch marks bindings dirty and re-emits them. No current static proof that this flag causes freeze.
Pending fresh refinement round after expansion/reference closure.

## Round2: fresh refinement completed
RefineCausality and RefinePerformance independently read combined evidence and exact source portions; both terminal. Reports refinement-RefineCausality.md and refinement-RefinePerformance.md. Verdict: bounded research claims acceptable; definite latestcause,matchedworkload,FPS attribution remain blocked by missing evidence, not by unfinished read-only research.
Accepted corrections: all observed motion attributed to user; absence of explicit trace env is not proof unset; source-cut equals568recordedentries not allinputs; currentdiskfivehashes not loadedFEX/NGXidentity; ordinarydescriptor optimization must unionallPSOs inpass and preserve dynamic/unboundedindexing. Root directly reread list1791-1802,devicehpp312-330,queue3476-3486, confirming PSO absent from capture dedup key. Added safetybound to synthesis.
Rejected overclaims remain logged: no errorrate from untimestamped logs; noall-meshcache exclusion; noNativeFEXCPUtoFPS percentage; no barrier/fence removal without hazard proof; no source-validator relaxation. No remaining unlabelled causal assertion. Final formatcheck later found citation grouping/method/sourceheading/keep-all issues; corrected,rerun0defects.
