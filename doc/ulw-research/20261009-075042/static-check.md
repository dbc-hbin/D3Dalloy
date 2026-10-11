{
  "command": "check",
  "status": "fail",
  "exitCode": 1,
  "layout": "not_run",
  "defects": [
    {
      "code": "missing_citation_section",
      "severity": "blocker",
      "selector": "body",
      "line": 1,
      "message": "no references/sources h2 and 0 [S<n>] definition line(s) (< 3)",
      "hint": "Add a sources or references section, or [S<n>] definition lines."
    },
    {
      "code": "missing_closing_section",
      "severity": "blocker",
      "selector": "body",
      "line": 1,
      "message": "no h2/h3 heading names how the report was made (method / how)",
      "hint": "Add the closing section that explains how the report was made (method, sources, gates)."
    },
    {
      "code": "korean_no_keep_all",
      "severity": "major",
      "selector": "body",
      "line": 1,
      "message": "3248 Hangul syllables in prose and no \"word-break: keep-all\" in any style block or style attribute",
      "hint": "Add \"word-break: keep-all; overflow-wrap: anywhere\" to the body or the slide text container."
    },
    {
      "code": "unsourced_number",
      "severity": "major",
      "selector": "body > main > p:nth-of-type(2)",
      "line": 3,
      "message": "number \"4.0\" without a lineage tag or citation: \"보존된 프로파일과 pristine GPTK 4.0b2 바이너리를 읽었으며, 게임이나 런\"",
      "hint": "Tag the number MEASURED / ASSUMED / DERIVED or cite its source in the same paragraph."
    },
    {
      "code": "section_without_citation",
      "severity": "major",
      "selector": "body > main > h2:nth-of-type(1)",
      "line": 4,
      "message": "section \"결론\" has 666 prose characters and no citation marker or anchor",
      "hint": "Cite at least one source inside this section or merge it into a cited one."
    },
    {
      "code": "unsourced_number",
      "severity": "major",
      "selector": "body > main > p:nth-of-type(4)",
      "line": 6,
      "message": "number \"9,131\" without a lineage tag or citation: \"의 가장 뚜렷한 이상은 셰이더 컴파일 실패 9,131줄과 POSITION_0 입력 레이\"",
      "hint": "Tag the number MEASURED / ASSUMED / DERIVED or cite its source in the same paragraph."
    },
    {
      "code": "section_without_citation",
      "severity": "major",
      "selector": "body > main > h2:nth-of-type(2)",
      "line": 8,
      "message": "section \"1. 이번 실행에서 확인한 사실\" has 1161 prose characters and no citation marker or anchor",
      "hint": "Cite at least one source inside this section or merge it into a cited one."
    },
    {
      "code": "unsourced_number",
      "severity": "major",
      "selector": "body > main > p:nth-of-type(7)",
      "line": 26,
      "message": "number \"3,856\" without a lineage tag or citation: \"실패 유형은 PS SRV 3,856줄, VS SRV 1,142줄, V\"",
      "hint": "Tag the number MEASURED / ASSUMED / DERIVED or cite its source in the same paragraph."
    },
    {
      "code": "unsourced_number",
      "severity": "major",
      "selector": "body > main > p:nth-of-type(8)",
      "line": 27,
      "message": "number \"11,116개\" without a lineage tag or citation: \"private shader cache 파일 11,116개, 총 237,591,400바이\"",
      "hint": "Tag the number MEASURED / ASSUMED / DERIVED or cite its source in the same paragraph."
    },
    {
      "code": "unsourced_number",
      "severity": "major",
      "selector": "body > main > ul:nth-of-type(1) > li:nth-of-type(3)",
      "line": 50,
      "message": "number \"11개\" without a lineage tag or citation: \"과거 candidate-03의 11개 NGX 기록 성공과 도시 캡처는 과거\"",
      "hint": "Tag the number MEASURED / ASSUMED / DERIVED or cite its source in the same paragraph."
    },
    {
      "code": "unsourced_number",
      "severity": "major",
      "selector": "body > main > ul:nth-of-type(1) > li:nth-of-type(4)",
      "line": 51,
      "message": "number \"64개\" without a lineage tag or citation: \"보존한 실패 shader/root pair 64개는 pristine D3DMetal에서\"",
      "hint": "Tag the number MEASURED / ASSUMED / DERIVED or cite its source in the same paragraph."
    },
    {
      "code": "unsourced_number",
      "severity": "major",
      "selector": "body > main > p:nth-of-type(18)",
      "line": 94,
      "message": "number \"18%\" without a lineage tag or citation: \"U sample은 전체 합계의 약 **14~18%**였다. 이 자료는 IR-tail 수\"",
      "hint": "Tag the number MEASURED / ASSUMED / DERIVED or cite its source in the same paragraph."
    },
    {
      "code": "unsourced_number",
      "severity": "major",
      "selector": "body > main > p:nth-of-type(19)",
      "line": 95,
      "message": "number \"568개\" without a lineage tag or citation: \"최신 package의 기록된 DXMT 입력 568개는 같지만 전체 바이너리는 다르다.\"",
      "hint": "Tag the number MEASURED / ASSUMED / DERIVED or cite its source in the same paragraph."
    },
    {
      "code": "section_without_citation",
      "severity": "major",
      "selector": "body > main > h2:nth-of-type(6)",
      "line": 96,
      "message": "section \"5. 원인을 확정하는 데 부족한 관측\" has 811 prose characters and no citation marker or anchor",
      "hint": "Cite at least one source inside this section or merge it into a cited one."
    },
    {
      "code": "section_without_citation",
      "severity": "major",
      "selector": "body > main > h2:nth-of-type(7)",
      "line": 107,
      "message": "section \"조사 검증\" has 420 prose characters and no citation marker or anchor",
      "hint": "Cite at least one source inside this section or merge it into a cited one."
    },
    {
      "code": "unsourced_number",
      "severity": "major",
      "selector": "body > main > ul:nth-of-type(3) > li:nth-of-type(4)",
      "line": 112,
      "message": "number \"9개\" without a lineage tag or citation: \"초기 9개 조사 축, 두 차례 확장 조사, 새로운\"",
      "hint": "Tag the number MEASURED / ASSUMED / DERIVED or cite its source in the same paragraph."
    }
  ],
  "counts": {
    "blocker": 2,
    "major": 14,
    "minor": 0,
    "total": 16
  },
  "gatesRun": [
    "korean_no_keep_all",
    "emoji_in_prose",
    "em_dash_in_prose",
    "en_dash_in_prose",
    "heading_too_long",
    "heading_generic",
    "unsourced_number",
    "chart_without_figure",
    "chart_svg_no_text",
    "chart_svg_missing_labels",
    "korean_serif_font",
    "section_without_citation",
    "missing_closing_section",
    "missing_citation_section",
    "broken_asset_reference",
    "missing_required_section"
  ],
  "gatesSkipped": [
    "palette_off_token"
  ]
}
check: FAIL - 2 blocker, 14 major, 0 minor; layout not_run


Wall time: 0.06 seconds

Command exited with code 1