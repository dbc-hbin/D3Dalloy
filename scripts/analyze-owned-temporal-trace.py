#!/usr/bin/env python3
"""Check actual owned-backend ordering and lifetime events, without another GPU run."""
from __future__ import annotations

import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import sys


def analyze(path: Path) -> dict:
    raw = path.read_bytes()
    if len(raw) > 16 * 1024 * 1024:
        raise ValueError("trace exceeds the bounded diagnostic size")
    events = []
    for line in raw.decode("utf-8", errors="strict").splitlines():
        marker = "OWNED_GRAPHICS_TRACE "
        if marker in line:
            events.append(json.loads(line.split(marker, 1)[1]))
    events.sort(key=lambda event: event["event_id"])
    errors = []

    def require(condition: bool, message: str) -> None:
        if not condition:
            errors.append(message)

    ids = [event["event_id"] for event in events]
    require(bool(ids), "no owned trace events")
    require(len(ids) == len(set(ids)), "duplicate event identifiers")
    require(len(ids) < 8192, "trace may have reached its event limit")
    by_kind = defaultdict(list)
    for event in events:
        by_kind[event["event"]].append(event)
    # Metal4 temporal waits/encoding split physical submissions. The logical
    # ExecuteCommandLists parent survives those splits, including list replay.
    active_executions = {}
    encoded_in_execution = defaultdict(set)
    latest_encodes = {}
    temporal_parents = {}
    consumed_encodes = set()
    for event in events:
        if event["event"] == "execute_list":
            active_executions[event["queue"]] = event
            encoded_in_execution[event["queue"]].clear()
        elif event["event"] == "encode":
            queue, encoder = event["queue"], event["encoder"]
            repeated = encoder in encoded_in_execution[queue]
            latest_encodes[(queue, encoder)] = (event, active_executions.get(queue), repeated)
            encoded_in_execution[queue].add(encoder)
        elif event["event"] == "temporal_encode":
            queue = event["queue"]
            preceding = latest_encodes.get((queue, event["encoder"]))
            require(preceding is not None, f"missing logical encoder for temporal event {event['event_id']}")
            if preceding is None:
                continue
            encode, parent, repeated = preceding
            valid_parent = (parent is not None and parent is active_executions.get(queue)
                            and parent["list"] == encode["list"]
                            and parent["recording"] == encode["recording"]
                            and not repeated and encode["event_id"] not in consumed_encodes)
            require(valid_parent, f"missing current logical execute parent for temporal event {event['event_id']}")
            if valid_parent:
                temporal_parents[event["event_id"]] = parent
            consumed_encodes.add(encode["event_id"])
    submissions = {(e["queue"], e["submission"]): e for e in by_kind["submit"]}
    completions = {(e["queue"], e["submission"]): e for e in by_kind["complete"]}
    records = {e["evaluation"]: e for e in by_kind["temporal_record"]}
    destroys = {e["evaluation"]: e for e in by_kind["temporal_frame_destroy"]}
    require(len(records) == len(by_kind["temporal_record"]), "duplicate temporal recording")
    require(len(destroys) == len(by_kind["temporal_frame_destroy"]), "duplicate frame destruction")
    features = defaultdict(list)
    executions = defaultdict(list)
    groups = []
    for event in by_kind["temporal_encode"]:
        features[event["feature"]].append(event)
        executions[event["evaluation"]].append(event)
        key = (event["queue"], event["submission"])
        if not groups or groups[-1] != key:
            groups.append(key)
        record = records.get(event["evaluation"])
        require(record is not None, f"missing recording for evaluation {event['evaluation']}")
        if record:
            require(record["event_id"] < event["event_id"], "evaluation encoded before recording")
            for field in ("feature", "generation", "color", "output"):
                require(record[field] == event[field], f"recorded {field} changed before execution")
        submit, complete = submissions.get(key), completions.get(key)
        require(submit is not None and complete is not None, f"unretired submission {key}")
        if submit and complete:
            require(event["event_id"] < submit["event_id"] < complete["event_id"],
                    f"invalid encode/commit/retirement ordering {key}")
            require(complete["status"] == 4, f"Metal submission did not complete successfully {key}")
    require(bool(features), "no temporal execution")
    # Independent feature histories may encode concurrently on different queues.
    # The reservation contract is per feature: no next command buffer may use
    # that feature before the previous buffer is committed. Keep the equivalent
    # per-queue rule too; one queue cannot interleave its own native recordings.
    def check_commit_chain(encodes: list[dict], domain: str) -> None:
        previous = None
        visited = set()
        for event in encodes:
            key = (event["queue"], event["submission"])
            if key == previous:
                continue
            require(key not in visited, f"{domain} returned to an earlier temporal command buffer")
            visited.add(key)
            if previous is not None:
                submit = submissions.get(previous)
                require(submit is not None and submit["event_id"] < event["event_id"],
                        f"{domain} encoded another temporal command buffer before the previous commit")
            previous = key
    for feature, encodes in features.items():
        check_commit_chain(encodes, f"feature {feature}")
    for queue in {e["queue"] for e in by_kind["temporal_encode"]}:
        check_commit_chain([e for e in by_kind["temporal_encode"] if e["queue"] == queue], f"queue {queue}")
    histories = {}
    for feature, history in features.items():
        previous_generation = None
        for sequence, event in enumerate(history, 1):
            require(event["history_sequence"] == sequence, f"noncontiguous feature sequence {feature}")
            expected_reset = bool(event["caller_reset"] or event["generation"] != previous_generation)
            require(bool(event["reset"]) == expected_reset, f"wrong effective history reset {feature}:{sequence}")
            previous_generation = event["generation"]
        histories[str(feature)] = {
            "evaluations": [e["evaluation"] for e in history],
            "generations": [e["generation"] for e in history],
            "resets": [e["reset"] for e in history],
            "queues": [e["queue"] for e in history],
        }
    for evaluation, record in records.items():
        destroy = destroys.get(evaluation)
        require(destroy is not None, f"recorded frame {evaluation} was not destroyed")
        if destroy:
            require(destroy["event_id"] > record["event_id"], "frame destroyed before recording")
            for execution in executions[evaluation]:
                complete = completions.get((execution["queue"], execution["submission"]))
                if complete:
                    require(destroy["event_id"] > complete["event_id"],
                            f"frame {evaluation} destroyed before GPU retirement")

    crossqueue = []
    for feature, history in features.items():
        if len(history) != 2 or history[0]["queue"] == history[1]["queue"]:
            continue
        producer, consumer = history
        for wait in by_kind["wait"]:
            if wait["queue"] != consumer["queue"] or wait["event_id"] >= consumer["event_id"]:
                continue
            signals = [e for e in by_kind["signal_committed"]
                       if e["queue"] == producer["queue"] and e["fence"] == wait["fence"]
                       and e["generation"] == wait["generation"] and e["value"] >= wait["value"]
                       and producer["event_id"] < e["event_id"] < wait["event_id"]]
            defers = [e for e in by_kind["wait_defer"]
                      if e["queue"] == wait["queue"] and e["fence"] == wait["fence"]
                      and e["generation"] == wait["generation"] and e["value"] == wait["value"]
                      and e["event_id"] < producer["event_id"]]
            if not signals or not defers:
                continue
            signal, defer = signals[-1], defers[-1]
            execution = temporal_parents.get(consumer["event_id"])
            if execution is None:
                continue
            resets = [e for e in by_kind["record_begin"] if e["list"] == execution["list"]
                      and e["recording"] > execution["recording"]
                      and e["event_id"] < consumer["event_id"]]
            producer_complete = completions[(signal["queue"], signal["submission"])]
            crossqueue.append({
                "feature": feature, "wait_defer": defer["event_id"],
                "producer_encode": producer["event_id"], "signal_committed": signal["event_id"],
                "logical_wait": wait["event_id"], "consumer_encode": consumer["event_id"],
                "producer_complete": producer_complete["event_id"],
                "consumer_encoded_before_producer_completion": consumer["event_id"] < producer_complete["event_id"],
                "submitted_recording": execution["recording"], "later_recordings_before_encode": len(resets),
            })
    require(any(c["later_recordings_before_encode"] >= 96 for c in crossqueue),
            "missing future-signal submission with 96 post-submit list resets")
    return {"schema": 1, "source": str(path), "sha256": hashlib.sha256(raw).hexdigest(),
            "events": len(events), "temporal_executions": len(by_kind["temporal_encode"]),
            "recorded_frames": len(records), "destroyed_frames": len(destroys),
            "temporal_submissions": len(set(groups)), "histories": histories,
            "crossqueue": crossqueue, "errors": errors, "pass": not errors,
            "scope": "temporal completion fixture event ordering and lifetime; not actual-game compatibility"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = analyze(args.trace)
        with args.out.open("x") as output:
            json.dump(result, output, indent=2)
            output.write("\n")
    except (OSError, ValueError, KeyError, StopIteration) as error:
        print(f"FAIL temporal trace: {error}", file=sys.stderr)
        return 1
    print(f"{'PASS' if result['pass'] else 'FAIL'} temporal trace: "
          f"{result['temporal_executions']} executions, {result['destroyed_frames']} destroyed frames")
    for error in result["errors"]:
        print(error, file=sys.stderr)
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
