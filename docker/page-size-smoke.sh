#!/usr/bin/env bash
set -euo pipefail

image=${1:?Usage: bash docker/page-size-smoke.sh IMAGE}
name="typesense-page-smoke-$$"
volume="${name}-data"

cleanup() {
  docker logs "$name" 2>&1 || true
  docker rm --force "$name" >/dev/null 2>&1 || true
  docker volume rm "$volume" >/dev/null 2>&1 || true
}
trap cleanup EXIT

docker volume create "$volume" >/dev/null
docker run --detach --name "$name" \
  --publish 127.0.0.1::8108 \
  --mount "type=volume,source=${volume},destination=/data" \
  "$image" --data-dir=/data --api-key=smoke-key \
  --enable-index-snapshot --index-snapshot-dir=/data/index-snapshots >/dev/null
url="http://$(docker port "$name" 8108/tcp)"

wait_for_health() {
  for attempt in {1..180}; do
    if curl --fail --silent --max-time 2 "$url/health" >/dev/null; then
      return
    fi
    if [[ $(docker inspect --format '{{.State.Running}}' "$name") != true ]]; then
      echo 'Typesense stopped before becoming healthy.' >&2
      return 1
    fi
    sleep 1
  done
  echo 'Typesense did not become healthy within 180 seconds.' >&2
  return 1
}

request() {
  curl --fail-with-body --silent --show-error --max-time 180 \
    --header 'Content-Type: application/json' \
    --header 'X-TYPESENSE-API-KEY: smoke-key' "$@"
}

assert_search() {
  request "$url/collections/page_smoke/documents/search?q=chemical&query_by=text" |
    jq -e '.found == 1 and .hits[0].document.id == "1" and (.hits[0].document.embedding | length) > 0' >/dev/null
}

wait_for_health
request --request POST "$url/collections" \
  --data '{"name":"page_smoke","fields":[{"name":"text","type":"string"},{"name":"embedding","type":"float[]","embed":{"from":["text"],"model_config":{"model_name":"ts/e5-small"}}}]}' >/dev/null
request --request POST "$url/collections/page_smoke/documents" \
  --data '{"id":"1","text":"chemical regulations"}' >/dev/null
assert_search

# SIGINT is the fork's contract for saving index snapshots.
docker stop --time 60 "$name" >/dev/null
test "$(docker inspect --format '{{.State.ExitCode}}' "$name")" = 0
docker run --rm --entrypoint /bin/sh \
  --mount "type=volume,source=${volume},destination=/data,readonly" \
  "$image" -c 'test -n "$(find /data/index-snapshots -name "*.idxsnap" -print -quit)"'
docker start "$name" >/dev/null
url="http://$(docker port "$name" 8108/tcp)"
wait_for_health
assert_search
docker logs "$name" 2>&1 | grep -F 'Loaded collection page_smoke from index snapshot' >/dev/null
echo "Typesense embeddings, search and snapshot restore passed on $(getconf PAGESIZE)-byte pages."
