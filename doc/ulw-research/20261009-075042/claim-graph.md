# Claim graph
## Verified-claims allowlist
Initially empty pending skeptic pass.

| claim_id | statement | type / risk / scope | intent | supports | contradicts | independent groups | convergence | counter-search / primary backing | dependencies | status | synthesis |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| C1 | Latest launcher selected optimized runtime and launched game at16:29:54 | runtime/normal/latest | I1 | O1 | none | launcher only, primary-only exception | identity pending | loaded hash absence must be respected | none | partial | 최신 실행 |
| C2 | Latest game log contains9131 compile failure lines, not9131 unique failures | measured/normal/latest | I2 | O2,O3 | sink discrepancy preserved | one producer, explicit exception | arithmetic verified, skeptic pending | full-log Python Counter | C1 | partial | 로그 |
| C3 | Current evidence does not establish exact DLSS failing stage or FPS ratio | evidence-boundary/high/latest | I2,I3 | O2-O5 | additional logs pending | runtime and source | pending all axes | detailed trace opt-in and unmatched profiles | C1 | unresolved | 한계 |
| C4 | Candidate-03 hr0 and screenshot do not prove sustained fresh scene output in latest run | scope/high/historical | I2 | O4 | none | historical trace and source API contract | pending skeptic | recording vs execution inspected | none | partial | 이전 검증 |

# Intent diff
| intent_id | expected truth | observed reality | diff / invariant | intent source | supporting observations | status | claims |
| --- | --- | --- | --- | --- | --- | --- | --- |
| I1 | Investigate latest run, not earlier successful test | timestamp and optimized tag located | loaded modules require independent check | user latest logs | O1 | true | C1 |
| I2 | DLSS scene should update as HUD/player moves | user reports frozen scene; ordinary logs lack per-frame SR diagnostics | temporal output freshness | user symptom | O2-O5 | violated (reported), cause unknown | C2,C3,C4 |
| I3 | Explain FPS gap and actionable improvements | no matched timing series yet | CPU share cannot become FPS slowdown | user performance comparison | pending | unknown | C3 |

## Final locked claims (supersedes initial provisional rows)
Verified-claims allowlist: C1,C2,C3,C4,C5,C6,C7,C8,C9,C10,C11, bounded exactly below. No definitive freeze cause or quantitative FPS gain is allowed.

| ID | Final statement | Evidence and independent groups | Challenge/result | Status | Synthesis |
|---|---|---|---|---|---|
| C1 | Latest16:29:54 optimized launch; five current disk modules match manifest, not loaded hashes | S4,S7,S21; launcher+filehash+wrapper | RuntimeIdentity/RefineCausality narrowed scope | verified narrow | section1 |
| C2 |9131/9122correlated compile-log lines,1881warnings,not unique shaders/time series | S1,S2; one producer primary-only exception; executed Counter | Separate sink discrepancy retained | verified measured | section1 |
| C3 | Exact latest failure stage and FPS attribution remain unknown | latest ordinary logs+source opt-in+historical unmatched profiles | Fresh causal review confirms missing required observables | verified evidence boundary; cause unresolved | conclusion/section5 |
| C4 | Recording HRESULT or old screenshot does not prove latest fresh GPU output | S9-S12; runtime history+producer/consumer code | Temporal consumer exists, native errors propagate; no false missing-dispatch claim | verified | section2 |
| C5 | Saved3840x2160 vs historical forced720p is confound,not measured latestSR | S5,S9;registry+launcher+NGXtrace | Reject watermark/9xFPS inference | verified narrow | section1 |
| C6 | Pristine GPTK4Metal4 same-visiblebuffer dispatchfence differs owned split/allstage | S13,S17;code+direct disassembly+verifiedbinary | Scope complete replay only; deadlock prevention retained | verified static | section3 |
| C7 | Coarse barrier/pass and ordinary descriptor traversal are performance candidates | S14,S22;source contracts | PSO union/dynamic indexing/lifetime must survive optimization; latest cost unknown | verified mechanism,hypothesized payoff | section4 |
| C8 | Historical nativePSO creation1431704.065ms vs compile150.2201ms | S15;executed event parsing primary-only | Unit correction,elapsednotactiveCPU,coldnotsteadyFPS | verified historical | section4 |
| C9 | Historical nativeFEX14-18%CPU cannot become FPS penalty or upperbound | S18,S19;recomputedprofiles+currentprovenance | Beforetailfix,networking/generatedcode confounds | verified narrow | section4 |
| C10 |568recorded source entries equal; selectedPEcode sections partlyequal | S8;executed fileparse primary-only | Does not imply fullinput/loaded/behavioridentity | verified narrow | section4 |
| C11 | Existing tail/cache-rescan/bindless fixes are not new recommendations | history+source+FEXretainedregression | Repeated implementation excluded | verified scope | section4 |

Hypotheses H1 stale producer from failedPSO, H2 rejected/missing SR, H3 stale native/outputcopy, H4 finalcomposition/queue: all unresolved by latest evidence. No probability ordering or definitecause. Source independence is not inflated by duplicate sinks or worker agreement.
